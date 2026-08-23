#include "inkloop/papercolor_audio_codec.hpp"

#include <array>
#include <utility>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace inkloop {
namespace {

constexpr gpio_num_t kCodecEnable = GPIO_NUM_45;
constexpr gpio_num_t kSpeakerEnable = GPIO_NUM_46;
constexpr uint16_t kEs7210Address = 0x40;
constexpr uint16_t kEs8311Address = 0x18;

constexpr std::array<std::pair<uint8_t, uint8_t>, 28> kCaptureRegisters{{
    {0x00, 0x41}, {0x01, 0x1f}, {0x06, 0x00}, {0x07, 0x20},
    {0x08, 0x10}, {0x09, 0x30}, {0x0A, 0x30}, {0x20, 0x0a},
    {0x21, 0x2a}, {0x22, 0x0a}, {0x23, 0x2a}, {0x02, 0xC1},
    {0x04, 0x01}, {0x05, 0x00}, {0x11, 0x60}, {0x40, 0x42},
    {0x41, 0x70}, {0x42, 0x70}, {0x43, 0x1B}, {0x44, 0x00},
    {0x45, 0x00}, {0x46, 0x00}, {0x47, 0x00}, {0x48, 0x00},
    {0x49, 0x00}, {0x4A, 0x00}, {0x4B, 0x00}, {0x4C, 0xFF},
}};

constexpr std::array<std::pair<uint8_t, uint8_t>, 8> kPlaybackRegisters{{
    {0x00, 0x80}, {0x01, 0xB5}, {0x02, 0x18}, {0x0D, 0x01},
    {0x12, 0x00}, {0x13, 0x10}, {0x32, 0xCF}, {0x37, 0x08},
}};

i2c_device_config_t codecDeviceConfig(uint16_t address,
                                      uint32_t clock_hz) {
  i2c_device_config_t config{};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = clock_hz;
  return config;
}

}  // namespace

PaperColorAudioCodec::~PaperColorAudioCodec() { shutdown(); }

esp_err_t PaperColorAudioCodec::initialize(
    i2c_master_bus_handle_t internal_bus) {
  if (!internal_bus) return ESP_ERR_INVALID_ARG;
  if (initialized_ || capture_codec_ || playback_codec_)
    return ESP_ERR_INVALID_STATE;

  gpio_config_t gpio{};
  gpio.pin_bit_mask = (1ULL << kCodecEnable) | (1ULL << kSpeakerEnable);
  gpio.mode = GPIO_MODE_OUTPUT;
  gpio.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio.intr_type = GPIO_INTR_DISABLE;
  esp_err_t status = gpio_config(&gpio);
  if (status != ESP_OK) return status;
  status = setPower(false, false);
  if (status != ESP_OK) return status;

  const i2c_device_config_t capture =
      codecDeviceConfig(kEs7210Address, 400000);
  status = i2c_master_bus_add_device(internal_bus, &capture, &capture_codec_);
  if (status != ESP_OK) return status;
  const i2c_device_config_t playback =
      codecDeviceConfig(kEs8311Address, 100000);
  status = i2c_master_bus_add_device(internal_bus, &playback, &playback_codec_);
  if (status != ESP_OK) {
    i2c_master_bus_rm_device(capture_codec_);
    capture_codec_ = nullptr;
    return status;
  }
  initialized_ = true;
  return ESP_OK;
}

void PaperColorAudioCodec::shutdown() {
  if (initialized_) {
    if (capture_active_) deactivateCapture();
    if (playback_active_) deactivatePlayback();
    setPower(false, false);
  }
  if (capture_codec_) i2c_master_bus_rm_device(capture_codec_);
  if (playback_codec_) i2c_master_bus_rm_device(playback_codec_);
  capture_codec_ = nullptr;
  playback_codec_ = nullptr;
  initialized_ = false;
  capture_active_ = false;
  playback_active_ = false;
}

esp_err_t PaperColorAudioCodec::prepareForDeepSleep() {
  if (!initialized_) return ESP_ERR_INVALID_STATE;
  if (capture_active_ || playback_active_) return ESP_ERR_INVALID_STATE;
  return setPower(false, false);
}

esp_err_t PaperColorAudioCodec::writeRegister(
    i2c_master_dev_handle_t device, uint8_t reg, uint8_t value) {
  if (!device) return ESP_ERR_INVALID_STATE;
  const uint8_t command[] = {reg, value};
  esp_err_t status = ESP_FAIL;
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    status = i2c_master_transmit(device, command, sizeof(command), 100);
    if (status == ESP_OK) break;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return status;
}

esp_err_t PaperColorAudioCodec::setPower(bool codec_enabled,
                                         bool speaker_enabled) {
  esp_err_t status = gpio_set_level(kSpeakerEnable, speaker_enabled ? 1 : 0);
  if (status != ESP_OK) return status;
  return gpio_set_level(kCodecEnable, codec_enabled ? 1 : 0);
}

esp_err_t PaperColorAudioCodec::activateCapture() {
  if (!initialized_) return ESP_ERR_INVALID_STATE;
  if (capture_active_) return ESP_OK;
  if (playback_active_) return ESP_ERR_INVALID_STATE;
  esp_err_t status = setPower(true, false);
  if (status != ESP_OK) return status;
  vTaskDelay(pdMS_TO_TICKS(50));
  status = writeRegister(capture_codec_, 0x00, 0xFF);
  for (const auto& reg : kCaptureRegisters) {
    if (status != ESP_OK) break;
    status = writeRegister(capture_codec_, reg.first, reg.second);
  }
  if (status == ESP_OK) status = writeRegister(capture_codec_, 0x01, 0x14);
  if (status != ESP_OK) {
    setPower(false, false);
    return status;
  }
  capture_active_ = true;
  return ESP_OK;
}

esp_err_t PaperColorAudioCodec::deactivateCapture() {
  if (!initialized_ || !capture_active_) return ESP_ERR_INVALID_STATE;
  const esp_err_t status = writeRegister(capture_codec_, 0x00, 0xFF);
  const esp_err_t power = setPower(false, false);
  capture_active_ = false;
  return status != ESP_OK ? status : power;
}

esp_err_t PaperColorAudioCodec::activatePlayback() {
  if (!initialized_) return ESP_ERR_INVALID_STATE;
  if (playback_active_) return ESP_OK;
  if (capture_active_) return ESP_ERR_INVALID_STATE;
  esp_err_t status = setPower(true, true);
  if (status != ESP_OK) return status;
  vTaskDelay(pdMS_TO_TICKS(5));
  for (const auto& reg : kPlaybackRegisters) {
    status = writeRegister(playback_codec_, reg.first, reg.second);
    if (status != ESP_OK) break;
  }
  if (status != ESP_OK) {
    setPower(false, false);
    return status;
  }
  playback_active_ = true;
  return ESP_OK;
}

esp_err_t PaperColorAudioCodec::deactivatePlayback() {
  if (!initialized_ || !playback_active_) return ESP_ERR_INVALID_STATE;
  const esp_err_t status = setPower(false, false);
  playback_active_ = false;
  return status;
}

EspI2sAudioConfig papercolor_audio_config() {
  EspI2sAudioConfig config;
  config.capture_port = I2S_NUM_1;
  config.playback_port = I2S_NUM_0;
  config.mclk = GPIO_NUM_42;
  config.bclk = GPIO_NUM_40;
  config.word_select = GPIO_NUM_41;
  config.capture_data = GPIO_NUM_39;
  config.playback_data = GPIO_NUM_38;
  config.capture_sample_rate_hz = 16000;
  config.dma_frame_count = 320;
  config.dma_descriptor_count = 6;
  return config;
}

}  // namespace inkloop
