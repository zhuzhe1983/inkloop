#pragma once

#include "driver/spi_master.h"
#include "inkloop/board.hpp"

namespace inkloop {

// Native ED2208 owner. It does not allocate an RGB framebuffer and never
// refreshes during initialize/wake; a stable 4-bpp frame is the only operation
// that can start a visible panel refresh.
class PaperColorEd2208Display final : public IBoardDisplay {
 public:
  PaperColorEd2208Display() = default;
  ~PaperColorEd2208Display() override;

  PaperColorEd2208Display(const PaperColorEd2208Display&) = delete;
  PaperColorEd2208Display& operator=(const PaperColorEd2208Display&) = delete;

  esp_err_t initialize(spi_host_device_t host);
  void shutdown();

  esp_err_t writeFullFrame(const BoardFrameView& frame) override;
  esp_err_t sleep() override;
  bool busy() const override;

 private:
  esp_err_t hardwareReset();
  esp_err_t initializeController();
  esp_err_t wakeIfNeeded();
  esp_err_t waitBusy(uint32_t timeout_ms = 20000) const;
  esp_err_t beginTransaction();
  void endTransaction();
  esp_err_t transmit(bool data_mode, const uint8_t* bytes, size_t length);
  esp_err_t transmitByte(bool data_mode, uint8_t value);
  esp_err_t refreshPanel();

  spi_host_device_t host_ = SPI2_HOST;
  spi_device_handle_t device_ = nullptr;
  bool initialized_ = false;
  bool sleeping_ = false;
  bool transaction_active_ = false;
};

}  // namespace inkloop
