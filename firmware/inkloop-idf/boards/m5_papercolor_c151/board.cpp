#include "inkloop/board.hpp"

#include <new>

#include "M5PM1.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inkloop/papercolor_audio_codec.hpp"
#include "inkloop/papercolor_ed2208.hpp"
#include "inkloop/papercolor_ed2208_protocol.hpp"
#include "inkloop/papercolor_renderer.hpp"
#include "led_strip.h"

namespace inkloop {
namespace {

constexpr char kTag[] = "ink-board";
constexpr gpio_num_t kInternalI2cSda = GPIO_NUM_3;
constexpr gpio_num_t kInternalI2cScl = GPIO_NUM_2;
constexpr gpio_num_t kSpiMosi = GPIO_NUM_13;
constexpr gpio_num_t kSpiMiso = GPIO_NUM_14;
constexpr gpio_num_t kSpiClock = GPIO_NUM_15;
constexpr gpio_num_t kPreviousButton = GPIO_NUM_10;
constexpr gpio_num_t kNextButton = GPIO_NUM_9;
constexpr gpio_num_t kVoiceButton = GPIO_NUM_1;
constexpr gpio_num_t kRgbData = GPIO_NUM_21;
constexpr size_t kRgbCount = 2;
constexpr uint8_t kExpectedPm1DeviceId = 0x50;
constexpr uint8_t kExpectedPm1DeviceModel = 0x20;

esp_err_t pm1Status(m5pm1_err_t status) {
  if (status == M5PM1_OK) return ESP_OK;
  switch (status) {
    case M5PM1_ERR_INVALID_ARG:
      return ESP_ERR_INVALID_ARG;
    case M5PM1_ERR_TIMEOUT:
      return ESP_ERR_TIMEOUT;
    case M5PM1_ERR_NOT_SUPPORTED:
      return ESP_ERR_NOT_SUPPORTED;
    case M5PM1_ERR_NOT_INIT:
      return ESP_ERR_INVALID_STATE;
    case M5PM1_ERR_I2C_CONFIG:
    case M5PM1_ERR_I2C_COMM:
      return ESP_ERR_INVALID_RESPONSE;
    default:
      return ESP_FAIL;
  }
}

class PaperColorBoardAdapter final : public IBoardAdapter {
 public:
  ~PaperColorBoardAdapter() override = default;

  const BoardDescriptor& descriptor() const override { return descriptor_; }

  esp_err_t initialize() override {
    if (initialized_) return ESP_OK;
    if (initializing_ || failed_) return ESP_ERR_INVALID_STATE;
    initializing_ = true;

    esp_err_t status = initializeI2cAndPm1();
    if (status == ESP_OK) status = initializeButtons();
    if (status == ESP_OK) status = initializeRgb();
    if (status == ESP_OK) status = audio_codec_.initialize(i2c_bus_);
    if (status == ESP_OK) status = initializeSharedSpi();
    if (status == ESP_OK) status = display_.initialize(SPI2_HOST);

    if (status != ESP_OK) {
      ESP_LOGE(kTag, "hardware initialization failed: %s",
               esp_err_to_name(status));
      shutdown();
      failed_ = true;
      initializing_ = false;
      return status;
    }
    initialized_ = true;
    initializing_ = false;
    ESP_LOGI(kTag,
             "HARDWARE_READY:READY board=m5-papercolor-c151 display=400x600 rgb=2");
    return ESP_OK;
  }

  void shutdown() override {
    display_.shutdown();
    if (spi_bus_ready_) {
      // Product shutdown must unmount an attached SD card first.
      spi_bus_free(SPI2_HOST);
      spi_bus_ready_ = false;
    }
    audio_codec_.shutdown();
    if (rgb_strip_) {
      led_strip_clear(rgb_strip_);
      led_strip_del(rgb_strip_);
      rgb_strip_ = nullptr;
    }
    // M5PM1 borrows the bus but owns the PM1 device handle. Destroy and
    // reconstruct it before deleting the caller-owned bus so both successful
    // startup and partial-begin rollback release ownership in the right order.
    if (pm1_ready_ || i2c_bus_) {
      pm1_.~M5PM1();
      new (&pm1_) M5PM1();
      pm1_ready_ = false;
    }
    if (i2c_bus_) {
      const esp_err_t released = i2c_del_master_bus(i2c_bus_);
      if (released == ESP_OK) {
        i2c_bus_ = nullptr;
      } else {
        ESP_LOGE(kTag, "I2C shutdown failed: %s",
                 esp_err_to_name(released));
      }
    }
    initialized_ = false;
    sd_power_ready_ = false;
    deep_sleep_prepared_ = false;
  }

  IBoardDisplay* display() override {
    return initialized_ ? &display_ : nullptr;
  }

  IBoardRenderer* renderer() override {
    return initialized_ ? &renderer_ : nullptr;
  }

  IAudioCodecControl* audioCodec() override {
    return initialized_ ? &audio_codec_ : nullptr;
  }

  EspI2sAudioConfig audioConfig() const override {
    return papercolor_audio_config();
  }

  i2c_master_bus_handle_t internalI2cBus() const override {
    return initialized_ ? i2c_bus_ : nullptr;
  }

  spi_host_device_t sharedStorageSpiHost() const override {
    return SPI2_HOST;
  }

  gpio_num_t buttonGpio(BoardButton button) const override {
    switch (button) {
      case BoardButton::Previous:
        return kPreviousButton;
      case BoardButton::Next:
        return kNextButton;
      case BoardButton::Voice:
        return kVoiceButton;
    }
    return GPIO_NUM_NC;
  }

  bool buttonPressed(BoardButton button) const override {
    const gpio_num_t pin = buttonGpio(button);
    return initialized_ && pin != GPIO_NUM_NC && gpio_get_level(pin) == 0;
  }

  esp_err_t setRgb(const BoardRgbPixel* pixels, size_t count) override {
    if (!initialized_ || !rgb_strip_) return ESP_ERR_INVALID_STATE;
    if (!pixels || count != kRgbCount) return ESP_ERR_INVALID_ARG;
    for (size_t index = 0; index < kRgbCount; ++index) {
      const esp_err_t status = led_strip_set_pixel(
          rgb_strip_, index, pixels[index].red, pixels[index].green,
          pixels[index].blue);
      if (status != ESP_OK) return status;
    }
    return led_strip_refresh(rgb_strip_);
  }

  esp_err_t prepareForDeepSleep() override {
    if (!initialized_ || !pm1_ready_ || deep_sleep_prepared_) {
      return ESP_ERR_INVALID_STATE;
    }
    if (display_.busy()) return ESP_ERR_INVALID_STATE;

    // Mark the transaction before its first hardware mutation. The product
    // sleep owner will call restore even if a later step fails.
    deep_sleep_prepared_ = true;
    esp_err_t status = display_.sleep();
    // Panel sleep is the mandatory first boundary. A failed controller
    // command must not be followed by rail changes.
    if (status != ESP_OK) return status;
    BoardRgbPixel dark[kRgbCount]{};
    status = setRgb(dark, kRgbCount);
    if (status != ESP_OK) return status;
    status = audio_codec_.prepareForDeepSleep();
    if (status != ESP_OK) return status;

    // Official C151 wiring: PM1 GPIO0 gates EPD power, GPIO3 powers TF,
    // GPIO4 enables TF detection, and the LDO supplies the two RGB LEDs.
    // DCDC 3V3 and ESP GPIO 1/9/10 are deliberately untouched so RTC/EXT1
    // wake remains available. TF also stays powered: until Storage owns a
    // fail-safe VFS unmount/remount transaction, dropping GPIO3/4 while FAT
    // is mounted risks corrupting the album.
    status = pm1Status(pm1_.gpioSet(
        M5PM1_GPIO_NUM_0, M5PM1_GPIO_MODE_OUTPUT, 0,
        M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL));
    if (status != ESP_OK) return status;
    return pm1Status(pm1_.setLdoEnable(false));
  }

  esp_err_t restoreAfterFailedDeepSleep() override {
    if (!initialized_) return ESP_ERR_INVALID_STATE;
    if (!deep_sleep_prepared_) return ESP_OK;

    esp_err_t status = pm1Status(pm1_.setLdoEnable(true));
    if (status != ESP_OK) return status;
    status = pm1Status(pm1_.gpioSet(
        M5PM1_GPIO_NUM_0, M5PM1_GPIO_MODE_OUTPUT, 1,
        M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL));
    if (status != ESP_OK) return status;
    // GPIO0 has just restored EPD power. Wake only reinitializes the ED2208;
    // it does not visibly refresh the retained e-paper image.
    vTaskDelay(pdMS_TO_TICKS(20));
    status = display_.wake();
    if (status != ESP_OK) return status;
    deep_sleep_prepared_ = false;
    return ESP_OK;
  }

  esp_err_t prepareSdCard() override {
    if (!initialized_ || !pm1_ready_ || !spi_bus_ready_) {
      return ESP_ERR_INVALID_STATE;
    }
    esp_err_t status = pm1Status(pm1_.gpioSet(
        M5PM1_GPIO_NUM_3, M5PM1_GPIO_MODE_OUTPUT, 1,
        M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL));
    if (status == ESP_OK) {
      status = pm1Status(pm1_.gpioSet(
          M5PM1_GPIO_NUM_4, M5PM1_GPIO_MODE_OUTPUT, 1,
          M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL));
    }
    if (status == ESP_OK) {
      status = pm1Status(pm1_.gpioSet(
          M5PM1_GPIO_NUM_1, M5PM1_GPIO_MODE_INPUT, 0,
          M5PM1_GPIO_PULL_UP, M5PM1_GPIO_DRIVE_PUSHPULL));
    }
    if (status != ESP_OK) return status;
    vTaskDelay(pdMS_TO_TICKS(20));
    sd_power_ready_ = true;
    return ESP_OK;
  }

  bool sdCardInserted() const override {
    if (!initialized_ || !sd_power_ready_) return false;
    uint8_t level = 1;
    return pm1_.gpioGetInput(M5PM1_GPIO_NUM_1, &level) == M5PM1_OK &&
           level == 0;
  }

 private:
  esp_err_t initializeI2cAndPm1() {
    if (!esp_psram_is_initialized()) {
      ESP_LOGE(kTag, "OPI PSRAM is required for PaperColor");
      return ESP_ERR_NOT_SUPPORTED;
    }

    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = kInternalI2cSda;
    bus.scl_io_num = kInternalI2cScl;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    // M5PM1 performs blocking register transactions and keeps transfer
    // buffers on its call stack. A non-zero trans_queue_depth switches the
    // IDF master bus into asynchronous mode, which returned before those
    // buffers were consumed and produced random identity bytes/corruption.
    bus.trans_queue_depth = 0;
    bus.flags.enable_internal_pullup = true;
    esp_err_t status = i2c_new_master_bus(&bus, &i2c_bus_);
    if (status != ESP_OK) return status;

    status = pm1Status(
        pm1_.begin(i2c_bus_, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K));
    if (status != ESP_OK) return status;
    pm1_ready_ = true;

    uint8_t device_id = 0;
    uint8_t device_model = 0;
    status = pm1Status(pm1_.getDeviceId(&device_id));
    if (status == ESP_OK) {
      status = pm1Status(pm1_.getDeviceModel(&device_model));
    }
    if (status != ESP_OK || device_id != kExpectedPm1DeviceId ||
        device_model != kExpectedPm1DeviceModel) {
      ESP_LOGE(kTag, "unexpected PM1 identity id=%02x model=%02x",
               device_id, device_model);
      return status == ESP_OK ? ESP_ERR_INVALID_RESPONSE : status;
    }

    const m5pm1_err_t steps[] = {
        pm1_.setI2cConfig(0, M5PM1_I2C_SPEED_100K),
        pm1_.wdtSet(0),
        pm1_.setChargeEnable(true),
        pm1_.setDcdcEnable(true),
        pm1_.setLdoEnable(true),
        pm1_.setLedEnLevel(true),
        pm1_.gpioSet(M5PM1_GPIO_NUM_0, M5PM1_GPIO_MODE_OUTPUT, 1,
                     M5PM1_GPIO_PULL_NONE, M5PM1_GPIO_DRIVE_PUSHPULL),
    };
    for (const m5pm1_err_t step : steps) {
      status = pm1Status(step);
      if (status != ESP_OK) return status;
    }
    return ESP_OK;
  }

  esp_err_t initializeButtons() {
    gpio_config_t input{};
    input.pin_bit_mask = (1ULL << kPreviousButton) | (1ULL << kNextButton) |
                         (1ULL << kVoiceButton);
    input.mode = GPIO_MODE_INPUT;
    input.pull_up_en = GPIO_PULLUP_ENABLE;
    input.pull_down_en = GPIO_PULLDOWN_DISABLE;
    input.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&input);
  }

  esp_err_t initializeRgb() {
    led_strip_config_t strip{};
    strip.strip_gpio_num = kRgbData;
    strip.max_leds = kRgbCount;
    strip.led_model = LED_MODEL_WS2812;
    strip.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt{};
    rmt.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt.resolution_hz = 10000000;
    esp_err_t status = led_strip_new_rmt_device(&strip, &rmt, &rgb_strip_);
    if (status != ESP_OK) return status;
    status = led_strip_clear(rgb_strip_);
    if (status != ESP_OK) return status;
    return led_strip_refresh(rgb_strip_);
  }

  esp_err_t initializeSharedSpi() {
    spi_bus_config_t bus{};
    bus.mosi_io_num = kSpiMosi;
    bus.miso_io_num = kSpiMiso;
    bus.sclk_io_num = kSpiClock;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    const esp_err_t status =
        spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (status == ESP_OK) spi_bus_ready_ = true;
    return status;
  }

  static constexpr BoardDescriptor descriptor_{
      "m5-papercolor-c151",
      400,
      600,
      6,
      kPaperColorEd2208NativePaletteMask,
      true,
      true,
      true,
      true,
      2,
      static_cast<uint8_t>(boardButtonMask(BoardButton::Previous) |
                           boardButtonMask(BoardButton::Next) |
                           boardButtonMask(BoardButton::Voice))};
  static_assert(descriptor_.valid(),
                "PaperColor board descriptor must remain valid");
  mutable M5PM1 pm1_{};
  PaperColorAudioCodec audio_codec_{};
  PaperColorEd2208Display display_{};
  PaperColorRenderer renderer_{};
  i2c_master_bus_handle_t i2c_bus_ = nullptr;
  led_strip_handle_t rgb_strip_ = nullptr;
  bool initializing_ = false;
  bool initialized_ = false;
  bool failed_ = false;
  bool pm1_ready_ = false;
  bool spi_bus_ready_ = false;
  bool sd_power_ready_ = false;
  bool deep_sleep_prepared_ = false;
};

constexpr BoardDescriptor PaperColorBoardAdapter::descriptor_;

}  // namespace

IBoardAdapter& board_adapter() {
  static PaperColorBoardAdapter adapter;
  return adapter;
}

}  // namespace inkloop
