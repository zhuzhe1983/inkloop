#include "inkloop/papercolor_ed2208.hpp"

#include <array>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inkloop/papercolor_ed2208_protocol.hpp"

namespace inkloop {
namespace {

constexpr char kTag[] = "ink-ed2208";
constexpr gpio_num_t kReset = GPIO_NUM_12;
constexpr gpio_num_t kBusy = GPIO_NUM_11;
constexpr gpio_num_t kDataCommand = GPIO_NUM_43;
constexpr gpio_num_t kChipSelect = GPIO_NUM_44;
constexpr uint32_t kSpiClockHz = 4000000;
constexpr size_t kRowBytes = kPaperColorEd2208Width / 2U;

void delayMs(uint32_t milliseconds) {
  vTaskDelay(pdMS_TO_TICKS(milliseconds));
}

}  // namespace

PaperColorEd2208Display::~PaperColorEd2208Display() { shutdown(); }

esp_err_t PaperColorEd2208Display::initialize(spi_host_device_t host) {
  if (initialized_ || device_) return ESP_ERR_INVALID_STATE;
  if (host != SPI2_HOST) return ESP_ERR_INVALID_ARG;
  host_ = host;

  gpio_config_t outputs{};
  outputs.pin_bit_mask = (1ULL << kReset) | (1ULL << kDataCommand) |
                         (1ULL << kChipSelect);
  outputs.mode = GPIO_MODE_OUTPUT;
  outputs.pull_up_en = GPIO_PULLUP_DISABLE;
  outputs.pull_down_en = GPIO_PULLDOWN_DISABLE;
  outputs.intr_type = GPIO_INTR_DISABLE;
  esp_err_t status = gpio_config(&outputs);
  if (status != ESP_OK) return status;
  if ((status = gpio_set_level(kChipSelect, 1)) != ESP_OK ||
      (status = gpio_set_level(kDataCommand, 1)) != ESP_OK ||
      (status = gpio_set_level(kReset, 1)) != ESP_OK) {
    return status;
  }

  gpio_config_t busy_input{};
  busy_input.pin_bit_mask = 1ULL << kBusy;
  busy_input.mode = GPIO_MODE_INPUT;
  busy_input.pull_up_en = GPIO_PULLUP_ENABLE;
  busy_input.pull_down_en = GPIO_PULLDOWN_DISABLE;
  busy_input.intr_type = GPIO_INTR_DISABLE;
  status = gpio_config(&busy_input);
  if (status != ESP_OK) return status;

  spi_device_interface_config_t device{};
  device.clock_speed_hz = kSpiClockHz;
  device.mode = 0;
  device.spics_io_num = -1;  // Manual CS keeps complete ED2208 phases atomic.
  device.queue_size = 1;
  status = spi_bus_add_device(host_, &device, &device_);
  if (status != ESP_OK) return status;

  status = hardwareReset();
  if (status == ESP_OK) status = initializeController();
  if (status != ESP_OK) {
    shutdown();
    return status;
  }
  initialized_ = true;
  sleeping_ = false;
  ESP_LOGI(kTag, "controller ready without visible refresh");
  return ESP_OK;
}

void PaperColorEd2208Display::shutdown() {
  if (transaction_active_) endTransaction();
  if (device_) {
    spi_bus_remove_device(device_);
    device_ = nullptr;
  }
  initialized_ = false;
  sleeping_ = false;
}

esp_err_t PaperColorEd2208Display::hardwareReset() {
  esp_err_t status = gpio_set_level(kReset, 1);
  if (status != ESP_OK) return status;
  delayMs(1);
  status = gpio_set_level(kReset, 0);
  if (status != ESP_OK) return status;
  delayMs(2);
  status = gpio_set_level(kReset, 1);
  if (status != ESP_OK) return status;
  delayMs(10);
  return ESP_OK;
}

esp_err_t PaperColorEd2208Display::beginTransaction() {
  if (!device_ || transaction_active_) return ESP_ERR_INVALID_STATE;
  esp_err_t status = spi_device_acquire_bus(device_, portMAX_DELAY);
  if (status != ESP_OK) return status;
  status = gpio_set_level(kChipSelect, 0);
  if (status != ESP_OK) {
    spi_device_release_bus(device_);
    return status;
  }
  transaction_active_ = true;
  return ESP_OK;
}

void PaperColorEd2208Display::endTransaction() {
  if (!transaction_active_) return;
  gpio_set_level(kChipSelect, 1);
  transaction_active_ = false;
  spi_device_release_bus(device_);
}

esp_err_t PaperColorEd2208Display::transmit(bool data_mode,
                                            const uint8_t* bytes,
                                            size_t length) {
  if (!transaction_active_ || !bytes || length == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t status = gpio_set_level(kDataCommand, data_mode ? 1 : 0);
  if (status != ESP_OK) return status;
  spi_transaction_t transaction{};
  transaction.length = length * 8U;
  transaction.tx_buffer = bytes;
  return spi_device_polling_transmit(device_, &transaction);
}

esp_err_t PaperColorEd2208Display::transmitByte(bool data_mode,
                                                uint8_t value) {
  return transmit(data_mode, &value, 1);
}

esp_err_t PaperColorEd2208Display::waitBusy(uint32_t timeout_ms) const {
  if (gpio_get_level(kBusy) != 0) return ESP_OK;
  TickType_t remaining = pdMS_TO_TICKS(timeout_ms);
  const TickType_t step = pdMS_TO_TICKS(10);
  while (gpio_get_level(kBusy) == 0) {
    if (remaining < step) {
      ESP_LOGE(kTag, "busy timeout after %lu ms",
               static_cast<unsigned long>(timeout_ms));
      return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(step);
    remaining -= step;
  }
  delayMs(200);
  return ESP_OK;
}

esp_err_t PaperColorEd2208Display::initializeController() {
  esp_err_t status = beginTransaction();
  if (status != ESP_OK) return status;
  for (size_t index = 0; index < ed2208InitCommandCount(); ++index) {
    status = waitBusy();
    if (status != ESP_OK) break;
    const Ed2208CommandView step = ed2208InitCommand(index);
    status = transmitByte(false, step.command);
    if (status == ESP_OK) status = transmit(true, step.data, step.length);
    if (status != ESP_OK) break;
  }
  if (status == ESP_OK) status = waitBusy();
  if (status == ESP_OK) status = transmitByte(false, 0x61);
  const std::array<uint8_t, 4> resolution{{
      static_cast<uint8_t>(kPaperColorEd2208Width >> 8U),
      static_cast<uint8_t>(kPaperColorEd2208Width & 0xFFU),
      static_cast<uint8_t>(kPaperColorEd2208Height >> 8U),
      static_cast<uint8_t>(kPaperColorEd2208Height & 0xFFU),
  }};
  if (status == ESP_OK) {
    status = transmit(true, resolution.data(), resolution.size());
  }
  endTransaction();
  return status;
}

esp_err_t PaperColorEd2208Display::wakeIfNeeded() {
  if (!sleeping_) return ESP_OK;
  esp_err_t status = hardwareReset();
  if (status == ESP_OK) status = initializeController();
  if (status == ESP_OK) sleeping_ = false;
  return status;
}

esp_err_t PaperColorEd2208Display::writeFullFrame(
    const BoardFrameView& frame) {
  if (!initialized_) return ESP_ERR_INVALID_STATE;
  if (frame.width != kPaperColorEd2208Width ||
      frame.height != kPaperColorEd2208Height ||
      frame.format != BoardFrameFormat::Palette4Bpp ||
      !ed2208FrameValid(frame.bytes, frame.length)) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t status = wakeIfNeeded();
  if (status != ESP_OK) return status;
  status = beginTransaction();
  if (status != ESP_OK) return status;
  status = transmitByte(false, 0x10);
  for (size_t offset = 0; status == ESP_OK && offset < frame.length;
       offset += kRowBytes) {
    status = transmit(true, frame.bytes + offset, kRowBytes);
  }
  endTransaction();
  if (status != ESP_OK) return status;
  return refreshPanel();
}

esp_err_t PaperColorEd2208Display::refreshPanel() {
  esp_err_t status = beginTransaction();
  if (status != ESP_OK) return status;
  status = transmitByte(false, 0x04);
  if (status == ESP_OK) status = waitBusy();
  if (status == ESP_OK) delayMs(200);
  const std::array<uint8_t, 4> booster{{0x6F, 0x1F, 0x17, 0x27}};
  if (status == ESP_OK) status = transmitByte(false, 0x06);
  if (status == ESP_OK) {
    status = transmit(true, booster.data(), booster.size());
  }
  if (status == ESP_OK) delayMs(200);
  if (status == ESP_OK) status = transmitByte(false, 0x12);
  if (status == ESP_OK) status = transmitByte(true, 0x00);
  if (status == ESP_OK) status = waitBusy();
  if (status == ESP_OK) status = transmitByte(false, 0x02);
  if (status == ESP_OK) status = transmitByte(true, 0x00);
  if (status == ESP_OK) status = waitBusy();
  if (status == ESP_OK) delayMs(200);
  endTransaction();
  return status;
}

esp_err_t PaperColorEd2208Display::sleep() {
  if (!initialized_) return ESP_ERR_INVALID_STATE;
  if (sleeping_) return ESP_OK;
  esp_err_t status = beginTransaction();
  if (status != ESP_OK) return status;
  status = transmitByte(false, 0x07);
  if (status == ESP_OK) status = transmitByte(true, 0xA5);
  endTransaction();
  if (status == ESP_OK) sleeping_ = true;
  return status;
}

bool PaperColorEd2208Display::busy() const {
  return initialized_ && gpio_get_level(kBusy) == 0;
}

}  // namespace inkloop
