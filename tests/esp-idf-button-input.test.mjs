import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const product = join(
  repo, "firmware/inkloop-idf/components/inkloop_product");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include "inkloop/button_debounce.hpp"

using inkloop::ButtonDebounceCore;

int main() {
  ButtonDebounceCore debounce(35);
  assert(debounce.accept(0, 100));
  assert(!debounce.accept(0, 100));
  assert(!debounce.accept(0, 134));
  assert(debounce.accept(0, 135));

  // Each physical button has an independent debounce window.
  assert(debounce.accept(1, 101));
  assert(debounce.accept(2, 102));
  assert(!debounce.accept(3, 103));

  // Unsigned subtraction keeps the decision correct across millis wrap.
  ButtonDebounceCore wrapping(35);
  assert(wrapping.accept(0, UINT32_MAX - 9U));
  assert(!wrapping.accept(0, 20U));
  assert(wrapping.accept(0, 25U));
  return 0;
}
`;

const ownerHarness = String.raw`
#include <cassert>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "inkloop/button_input.hpp"
#include "inkloop/button_latency_telemetry.hpp"

using namespace inkloop;

class FakeBoard final : public IBoardAdapter {
 public:
  explicit FakeBoard(uint8_t button_mask) : descriptor_{button_mask} {}

  const BoardDescriptor& descriptor() const override { return descriptor_; }

  gpio_num_t buttonGpio(BoardButton button) const override {
    assert(descriptor_.supportsButton(button));
    ++reads_[static_cast<unsigned>(button)];
    switch (button) {
      case BoardButton::Previous:
        return previous_pin;
      case BoardButton::Next:
        return next_pin;
      case BoardButton::Voice:
        return voice_pin;
    }
    return GPIO_NUM_NC;
  }

  int reads(BoardButton button) const {
    return reads_[static_cast<unsigned>(button)];
  }

  gpio_num_t previous_pin = 10;
  gpio_num_t next_pin = 9;
  gpio_num_t voice_pin = 1;

 private:
  BoardDescriptor descriptor_;
  mutable int reads_[3]{};
};

constexpr uint64_t pinMask(gpio_num_t pin) {
  return 1ULL << static_cast<unsigned>(pin);
}

int main() {
  RuntimeSupervisor supervisor;

  test_gpio::reset();
  FakeBoard next_only(boardButtonMask(BoardButton::Next));
  {
    EspButtonInputOwner input(next_only, supervisor);
    assert(input.configure() == ESP_OK);
    assert(test_gpio::install_calls == 1);
    assert(test_gpio::install_flags == ESP_INTR_FLAG_IRAM);
    assert(test_gpio::added == pinMask(9));
    assert(next_only.reads(BoardButton::Previous) == 0);
    assert(next_only.reads(BoardButton::Next) == 1);
    assert(next_only.reads(BoardButton::Voice) == 0);
    assert(input.arm() == ESP_OK);
    assert(test_gpio::enabled == pinMask(9));
    input.disarm();
    assert(test_gpio::removed == pinMask(9));
    assert(test_gpio::uninstall_calls == 1);
    assert(next_only.reads(BoardButton::Previous) == 0);
    assert(next_only.reads(BoardButton::Next) == 1);
    assert(next_only.reads(BoardButton::Voice) == 0);
  }

  test_gpio::reset();
  FakeBoard no_buttons(0U);
  {
    EspButtonInputOwner input(no_buttons, supervisor);
    assert(input.configure() == ESP_OK);
    assert(test_gpio::install_calls == 0);
    assert(input.arm() == ESP_OK);
    input.disarm();
    assert(test_gpio::added == 0U && test_gpio::enabled == 0U &&
           test_gpio::removed == 0U);
    assert(test_gpio::uninstall_calls == 0);
    assert(no_buttons.reads(BoardButton::Previous) == 0);
    assert(no_buttons.reads(BoardButton::Next) == 0);
    assert(no_buttons.reads(BoardButton::Voice) == 0);
  }

  test_gpio::reset();
  FakeBoard c151(static_cast<uint8_t>(
      boardButtonMask(BoardButton::Previous) |
      boardButtonMask(BoardButton::Next) |
      boardButtonMask(BoardButton::Voice)));
  {
    ButtonLatencyTelemetry latency;
    EspButtonInputOwner input(c151, supervisor, &latency);
    assert(input.configure() == ESP_OK);
    assert(supervisor.tick_interval_ms == 1U);
    const uint64_t expected = pinMask(10) | pinMask(9) | pinMask(1);
    assert(test_gpio::added == expected);
    assert(input.arm() == ESP_OK);
    assert(test_gpio::enabled == expected);

    // ISR capture is invisible to task-context owners until the Input tick.
    test_timer::now_us = 12'345'678;
    test_gpio::fire(9);
    assert(input.hasPendingRawEdge());
    assert(supervisor.post_count == 0);
    assert(latency.capture_count == 0);
    supervisor.runTick();
    assert(!input.hasPendingRawEdge());
    assert(supervisor.post_count == 1);
    assert(latency.capture_count == 1);
    assert(latency.last_captured_us == 12'345'678U);
    assert(supervisor.last.deadline_ms == 12'345U + 100U);
    assert(supervisor.last.opcode ==
           productOpcode(ProductOpcode::RawButtonNext));

    // Task-context admission rejection closes latency immediately.
    latency.reset();
    supervisor.post_result = AdmissionResult::QueueFull;
    test_timer::now_us = 12'346'000;
    test_gpio::fire(1);
    supervisor.runTick();
    assert(latency.capture_count == 1);
    assert(latency.terminal_count == 1);
    assert(latency.last_terminal == ButtonLatencyOutcome::NotReady);

    // A GPIO storm remains bounded and cannot overwrite accepted records.
    latency.reset();
    supervisor.post_result = AdmissionResult::Admitted;
    const int before_posts = supervisor.post_count;
    for (size_t count = 0; count < EspButtonInputOwner::kRawEdgeCapacity + 3U;
         ++count) {
      test_timer::now_us += 10;
      test_gpio::fire(10);
    }
    assert(input.hasPendingRawEdge());
    assert(input.rawEdgeOverflowCount() == 3U);
    assert(supervisor.post_count == before_posts);
    supervisor.runTick();
    assert(!input.hasPendingRawEdge());
    assert(supervisor.post_count == before_posts +
        static_cast<int>(EspButtonInputOwner::kRawEdgeCapacity));

    test_gpio::fire(10);
    assert(input.hasPendingRawEdge());
    input.disarm();
    assert(!input.hasPendingRawEdge());
    assert(test_gpio::removed == expected);
    assert(test_gpio::uninstall_calls == 1);
  }

  test_gpio::reset();
  FakeBoard inconsistent(boardButtonMask(BoardButton::Next));
  inconsistent.next_pin = GPIO_NUM_NC;
  EspButtonInputOwner invalid(inconsistent, supervisor);
  assert(invalid.configure() == ESP_ERR_NOT_SUPPORTED);
  assert(inconsistent.reads(BoardButton::Previous) == 0);
  assert(inconsistent.reads(BoardButton::Next) == 1);
  assert(inconsistent.reads(BoardButton::Voice) == 0);
  assert(test_gpio::uninstall_calls == 1);

  // ESP-IDF cannot prove that an existing process-global dispatcher was
  // installed with IRAM flags. Product therefore fails closed before adding
  // handlers instead of borrowing a cache-unsafe service.
  test_gpio::reset();
  test_gpio::install_result = ESP_ERR_INVALID_STATE;
  FakeBoard borrowed_service(boardButtonMask(BoardButton::Next));
  {
    EspButtonInputOwner input(borrowed_service, supervisor);
    assert(input.configure() == ESP_ERR_INVALID_STATE);
    input.disarm();
    assert(test_gpio::added == 0U);
    assert(test_gpio::removed == 0U);
    assert(test_gpio::uninstall_calls == 0);
  }
  return 0;
}
`;

function writeStub(root, relative, source) {
  const target = join(root, relative);
  mkdirSync(dirname(target), { recursive: true });
  writeFileSync(target, source);
}

function buildAndRunOwner(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-button-owner-"));
  try {
    writeStub(scratch, "inkloop/board.hpp", String.raw`
#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_NOT_SUPPORTED = 0x106;
using gpio_num_t = int;
constexpr gpio_num_t GPIO_NUM_NC = -1;
namespace inkloop {
enum class BoardButton : uint8_t { Previous, Next, Voice };
constexpr uint8_t boardButtonMask(BoardButton button) {
  return static_cast<uint8_t>(1U << static_cast<uint8_t>(button));
}
struct BoardDescriptor {
  uint8_t button_mask;
  constexpr bool supportsButton(BoardButton button) const {
    return (button_mask & boardButtonMask(button)) != 0U;
  }
};
class IBoardAdapter {
 public:
  virtual ~IBoardAdapter() = default;
  virtual const BoardDescriptor& descriptor() const = 0;
  virtual gpio_num_t buttonGpio(BoardButton button) const = 0;
};
}  // namespace inkloop
`);
    writeStub(scratch, "driver/gpio.h", String.raw`
#pragma once
#include <cassert>
#include <cstdint>
#include "inkloop/board.hpp"
constexpr int GPIO_INTR_NEGEDGE = 1;
namespace test_gpio {
inline int install_calls = 0;
inline int uninstall_calls = 0;
inline int install_flags = 0;
inline esp_err_t install_result = ESP_OK;
inline uint64_t disabled = 0;
inline uint64_t configured = 0;
inline uint64_t added = 0;
inline uint64_t enabled = 0;
inline uint64_t removed = 0;
inline void (*handlers[64])(void*){};
inline void* contexts[64]{};
inline uint64_t bit(gpio_num_t pin) {
  return pin >= 0 && pin < 64 ? 1ULL << static_cast<unsigned>(pin) : 0U;
}
inline void reset() {
  install_calls = 0;
  uninstall_calls = 0;
  install_flags = 0;
  install_result = ESP_OK;
  disabled = configured = added = enabled = removed = 0;
  for (unsigned index = 0; index < 64; ++index) {
    handlers[index] = nullptr;
    contexts[index] = nullptr;
  }
}
inline void fire(gpio_num_t pin) {
  assert(pin >= 0 && pin < 64 && handlers[pin]);
  handlers[pin](contexts[pin]);
}
}  // namespace test_gpio
inline esp_err_t gpio_install_isr_service(int flags) {
  ++test_gpio::install_calls;
  test_gpio::install_flags = flags;
  return test_gpio::install_result;
}
inline void gpio_uninstall_isr_service() {
  ++test_gpio::uninstall_calls;
}
inline esp_err_t gpio_intr_disable(gpio_num_t pin) {
  test_gpio::disabled |= test_gpio::bit(pin);
  return ESP_OK;
}
inline esp_err_t gpio_set_intr_type(gpio_num_t pin, int) {
  test_gpio::configured |= test_gpio::bit(pin);
  return ESP_OK;
}
inline esp_err_t gpio_isr_handler_add(
    gpio_num_t pin, void (*handler)(void*), void* context) {
  test_gpio::added |= test_gpio::bit(pin);
  test_gpio::handlers[pin] = handler;
  test_gpio::contexts[pin] = context;
  return ESP_OK;
}
inline esp_err_t gpio_intr_enable(gpio_num_t pin) {
  test_gpio::enabled |= test_gpio::bit(pin);
  return ESP_OK;
}
inline esp_err_t gpio_isr_handler_remove(gpio_num_t pin) {
  test_gpio::removed |= test_gpio::bit(pin);
  test_gpio::handlers[pin] = nullptr;
  test_gpio::contexts[pin] = nullptr;
  return ESP_OK;
}
`);
    writeStub(scratch, "esp_attr.h", String.raw`
#pragma once
#define IRAM_ATTR
`);
    writeStub(scratch, "esp_intr_alloc.h", String.raw`
#pragma once
constexpr int ESP_INTR_FLAG_IRAM = 0x400;
`);
    writeStub(scratch, "inkloop/product_opcodes.hpp", String.raw`
#pragma once
#include <cstdint>
namespace inkloop {
enum class ProductOpcode : uint16_t {
  None,
  RawButtonPrevious,
  RawButtonNext,
  RawButtonVoice,
};
constexpr uint16_t productOpcode(ProductOpcode opcode) {
  return static_cast<uint16_t>(opcode);
}
}  // namespace inkloop
`);
    writeStub(scratch, "inkloop/button_latency_telemetry.hpp", String.raw`
#pragma once
#include <cstdint>
namespace inkloop {
enum class ButtonLatencyButton : uint8_t { Previous, Next, Top };
enum class ButtonLatencyOutcome : uint8_t {
  Led, Navigation, Debounced, NotReady
};
class ButtonLatencyTelemetry {
 public:
  void recordCapture(uint64_t event_id, ButtonLatencyButton button,
                     uint32_t captured_us) {
    ++capture_count;
    last_event_id = event_id;
    last_button = button;
    last_captured_us = captured_us;
  }
  void recordTerminal(uint64_t event_id, ButtonLatencyOutcome outcome,
                      uint32_t terminal_us) {
    ++terminal_count;
    last_event_id = event_id;
    last_terminal = outcome;
    last_terminal_us = terminal_us;
  }
  void reset() {
    capture_count = 0;
    terminal_count = 0;
    last_event_id = 0;
    last_captured_us = 0;
    last_terminal_us = 0;
  }
  int capture_count = 0;
  int terminal_count = 0;
  uint64_t last_event_id = 0;
  uint32_t last_captured_us = 0;
  uint32_t last_terminal_us = 0;
  ButtonLatencyButton last_button = ButtonLatencyButton::Previous;
  ButtonLatencyOutcome last_terminal = ButtonLatencyOutcome::NotReady;
};
}  // namespace inkloop
`);
    writeStub(scratch, "inkloop/runtime_supervisor.hpp", String.raw`
#pragma once
#include <cassert>
#include <cstdint>
#include "inkloop/board.hpp"
using BaseType_t = int;
constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdTRUE = 1;
using portMUX_TYPE = int;
constexpr portMUX_TYPE portMUX_INITIALIZER_UNLOCKED = 0;
#define portENTER_CRITICAL_ISR(mux) ((void)(mux))
#define portEXIT_CRITICAL_ISR(mux) ((void)(mux))
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define portYIELD_FROM_ISR() ((void)0)
namespace inkloop {
enum class TaskLane : uint8_t { Input };
enum class WorkClass : uint8_t { Button };
enum class EnvelopeKind : uint8_t { Command };
enum class WorkDisposition : uint8_t {
  Accepted, Complete, Cancelled, Failed, TimedOut
};
enum class AdmissionResult : uint8_t {
  Admitted, NotReady, InvalidEnvelope, WrongLane, StaleGeneration, Expired,
  QueueFull, Underflow
};
struct WorkEnvelope {
  uint32_t generation = 0;
  uint64_t request_id = 0;
  WorkClass work_class = WorkClass::Button;
  EnvelopeKind kind = EnvelopeKind::Command;
  WorkDisposition disposition = WorkDisposition::Accepted;
  uint16_t opcode = 0;
  uint32_t deadline_ms = 0;
};
class RuntimeSupervisor {
 public:
  using Handler = WorkDisposition (*)(const WorkEnvelope&, void*);
  using TickHandler = void (*)(void*);
  esp_err_t registerHandler(TaskLane, Handler, void*) { return ESP_OK; }
  esp_err_t registerTickHandler(TaskLane, TickHandler handler, void* context,
                                uint32_t interval_ms) {
    tick = handler;
    tick_context = context;
    tick_interval_ms = interval_ms;
    return ESP_OK;
  }
  bool started() const { return true; }
  AdmissionResult post(const WorkEnvelope& envelope) {
    ++post_count;
    last = envelope;
    return post_result;
  }
  void runTick() { assert(tick); tick(tick_context); }
  TickHandler tick = nullptr;
  void* tick_context = nullptr;
  uint32_t tick_interval_ms = 0;
  int post_count = 0;
  WorkEnvelope last{};
  AdmissionResult post_result = AdmissionResult::Admitted;
};
}  // namespace inkloop
`);
    writeStub(scratch, "esp_timer.h", String.raw`
#pragma once
#include <cstdint>
namespace test_timer { inline int64_t now_us = 1000; }
inline int64_t esp_timer_get_time() { return test_timer::now_us; }
`);

    const source = join(scratch, "button_owner.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, ownerHarness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", scratch, "-I", join(product, "include"), source,
      join(product, "button_input.cpp"),
      join(product, "button_debounce.cpp"), "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    );
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-buttons-"));
  try {
    const source = join(scratch, "button_debounce_harness.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(product, "include"), source,
      join(product, "button_debounce.cpp"), "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    );
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("button debounce is independent, bounded and wrap-safe", () => {
  buildAndRun(false);
  buildAndRun(true);
});

test("button owner skips unsupported capabilities under strict C++17 and sanitizers", () => {
  buildAndRunOwner(false);
  buildAndRunOwner(true);
});

test("button GPIO owner uses a cache-safe bounded mailbox and fail-closed cleanup", () => {
  const header = readFileSync(join(
    product, "include/inkloop/button_input.hpp"), "utf8");
  const source = readFileSync(join(product, "button_input.cpp"), "utf8");
  const combined = `${header}\n${source}`;
  assert.match(source, /GPIO_INTR_NEGEDGE/);
  assert.match(source, /gpio_install_isr_service\(ESP_INTR_FLAG_IRAM\)/);
  assert.match(source, /void IRAM_ATTR EspButtonInputOwner::gpioIsr/);
  assert.match(
    source,
    /void IRAM_ATTR EspButtonInputOwner::captureRawEdgeFromIsr/,
  );
  assert.match(source, /portENTER_CRITICAL_ISR/);
  assert.match(
    source,
    /event\.deadline_ms\s*=\s*edge\.captured_ms \+ kRawEventDeadlineMs/,
  );
  assert.match(
    source,
    /registerTickHandler\([\s\S]{0,150}TaskLane::Input[\s\S]{0,150}kInputDrainIntervalMs/,
  );
  assert.match(source, /kInputDrainIntervalMs\s*=\s*1U/);
  assert.match(source, /static_assert\(kInputDrainIntervalMs == 1U/);
  assert.match(
    source,
    /latency_->recordCapture\([\s\S]{0,120}edge\.captured_us/,
  );
  assert.match(
    source,
    /supervisor_\.post\(event\) != AdmissionResult::Admitted[\s\S]{0,180}ButtonLatencyOutcome::NotReady/,
  );
  assert.doesNotMatch(source, /postButtonFromIsr/);
  assert.match(source, /gpio_intr_disable\(pin\)/);
  assert.match(
    source,
    /capabilities\.supportsButton\(kButtons\[index\]\)[\s\S]*continue/,
  );
  assert.match(source, /if \(!handler_installed_\[index\]\) continue;[\s\S]*gpio_intr_enable\(button_pins_\[index\]\)/);
  assert.match(header, /handler_installed_/);
  assert.match(header, /button_pins_/);
  assert.match(header, /RawEdge raw_edges_\[kRawEdgeCapacity\]/);
  assert.match(header, /bool hasPendingRawEdge\(\) const/);
  assert.match(source, /if \(handler_installed_\[index\]\)[\s\S]*gpio_isr_handler_remove/);
  assert.match(source, /if \(isr_service_owned_\)[\s\S]*gpio_uninstall_isr_service/);
  assert.equal((source.match(/board_\.buttonGpio/g) ?? []).length, 1);
  assert.match(source, /WorkDisposition::Cancelled/);
  const isrStart = source.indexOf(
    "void IRAM_ATTR EspButtonInputOwner::gpioIsr",
  );
  const isrEnd = source.indexOf(
    "ProductOpcode EspButtonInputOwner::opcodeFor", isrStart,
  );
  const isrPath = source.slice(isrStart, isrEnd);
  assert.match(isrPath, /captureRawEdgeFromIsr/);
  assert.doesNotMatch(
    isrPath,
    /supervisor_|latency_|recordCapture|recordTerminal|xQueue|post\(|ESP_LOG|malloc|new\s/,
  );
  const captureStart = source.indexOf(
    "void IRAM_ATTR EspButtonInputOwner::captureRawEdgeFromIsr",
  );
  const captureEnd = source.indexOf(
    "void EspButtonInputOwner::inputTick", captureStart,
  );
  const capturePath = source.slice(captureStart, captureEnd);
  assert.match(capturePath, /esp_timer_get_time\(\)/);
  assert.match(capturePath, /raw_edges_\[tail\]/);
  assert.doesNotMatch(
    capturePath,
    /supervisor_|latency_|recordCapture|recordTerminal|xQueue|post\(|ESP_LOG|malloc|new\s|std::/,
  );
  assert.doesNotMatch(combined, /delay\(|vTaskDelay|Arduino\.h|M5Unified/);
  const sdkconfig = readFileSync(join(
    repo, "firmware/inkloop-idf/sdkconfig"), "utf8");
  assert.doesNotMatch(
    sdkconfig,
    /^CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y$/m,
  );
});
