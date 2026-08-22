#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace inkloop {

// Blocking DNS/TLS/HTTP work is executed on one bounded worker. The caller
// remains the sole owner of product state and pumps only responsive UI work
// while waiting, so network latency cannot starve buttons, the Portal or audio.
enum class ResponsiveWorkKind : uint8_t {
  InkloopNetwork,
  MyAiNetwork,
  MyAiImageStream,
  WebSocketHandshake,
  StorageHardware,
  PortalTransfer,
  DisplayHardware,
};

using ResponsiveWorkFunction = void (*)(void* context);
using ResponsivePumpFunction = void (*)(
    void* context, ResponsiveWorkKind kind);

class ResponsiveWorkExecutor {
 public:
  ResponsiveWorkExecutor() = default;
  ~ResponsiveWorkExecutor() = default;

  bool begin();
  void setPump(ResponsivePumpFunction pump, void* context);
  bool execute(
      ResponsiveWorkKind kind,
      ResponsiveWorkFunction work,
      void* context);

  bool active() const { return active_.load(std::memory_order_acquire); }
  ResponsiveWorkKind activeKind() const {
    return activeKind_.load(std::memory_order_acquire);
  }
  uint32_t lastElapsedMilliseconds() const {
    return lastElapsedMilliseconds_.load(std::memory_order_acquire);
  }

 private:
  struct WorkItem {
    ResponsiveWorkFunction function = nullptr;
    void* context = nullptr;
  };

  static void taskEntry(void* context);
  void run();

  QueueHandle_t queue_ = nullptr;
  SemaphoreHandle_t completion_ = nullptr;
  SemaphoreHandle_t dispatch_ = nullptr;
  TaskHandle_t task_ = nullptr;
  ResponsivePumpFunction pump_ = nullptr;
  void* pumpContext_ = nullptr;
  std::atomic<bool> active_{false};
  std::atomic<ResponsiveWorkKind> activeKind_{
      ResponsiveWorkKind::InkloopNetwork};
  std::atomic<uint32_t> lastElapsedMilliseconds_{0};
};

ResponsiveWorkExecutor& responsiveWorkExecutor();

}  // namespace inkloop
