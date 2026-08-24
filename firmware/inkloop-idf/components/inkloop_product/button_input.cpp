#include "inkloop/button_input.hpp"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "inkloop/button_latency_telemetry.hpp"
#include "inkloop/product_opcodes.hpp"

namespace inkloop {
namespace {

constexpr std::array kButtons{
    BoardButton::Previous, BoardButton::Next, BoardButton::Voice};
static_assert(kButtons.size() == ButtonDebounceCore::kButtonCount);
constexpr uint32_t kRawEventDeadlineMs = 100;
constexpr uint32_t kInputDrainIntervalMs = 1U;
static_assert(kInputDrainIntervalMs == 1U,
              "button mailbox drain must preserve the 20 ms p99 budget");

uint32_t nowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

ButtonLatencyButton latencyButton(BoardButton button) {
  switch (button) {
    case BoardButton::Previous:
      return ButtonLatencyButton::Previous;
    case BoardButton::Next:
      return ButtonLatencyButton::Next;
    case BoardButton::Voice:
      return ButtonLatencyButton::Top;
  }
  return ButtonLatencyButton::Previous;
}

}  // namespace

EspButtonInputOwner::EspButtonInputOwner(IBoardAdapter& board,
                                         RuntimeSupervisor& supervisor,
                                         ButtonLatencyTelemetry* latency)
    : board_(board), supervisor_(supervisor), latency_(latency) {
  button_pins_.fill(GPIO_NUM_NC);
  for (size_t index = 0; index < contexts_.size(); ++index) {
    contexts_[index].owner = this;
    contexts_[index].button_index = static_cast<uint8_t>(index);
  }
}

EspButtonInputOwner::~EspButtonInputOwner() { disarm(); }

esp_err_t EspButtonInputOwner::configure() {
  if (configured_) return ESP_ERR_INVALID_STATE;
  esp_err_t status = supervisor_.registerHandler(
      TaskLane::Input, &EspButtonInputOwner::handle, this);
  if (status != ESP_OK) return status;
  status = supervisor_.registerTickHandler(
      TaskLane::Input, &EspButtonInputOwner::inputTick, this,
      kInputDrainIntervalMs);
  if (status != ESP_OK) return status;

  clearRawEdges();

  const BoardDescriptor& capabilities = board_.descriptor();
  bool any_supported = false;
  for (size_t index = 0; index < contexts_.size(); ++index) {
    if (capabilities.supportsButton(kButtons[index])) any_supported = true;
  }
  if (any_supported) {
    status = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (status == ESP_OK) {
      isr_service_owned_ = true;
    } else {
      // ESP-IDF cannot report the flags used by an already-installed global
      // dispatcher. Borrowing it would make cache-off safety unverifiable, so
      // this first product owner fails closed instead.
      return status;
    }
  }
  for (size_t index = 0; index < contexts_.size(); ++index) {
    if (!capabilities.supportsButton(kButtons[index])) continue;
    const gpio_num_t pin = board_.buttonGpio(kButtons[index]);
    if (pin == GPIO_NUM_NC) {
      disarm();
      return ESP_ERR_NOT_SUPPORTED;
    }
    button_pins_[index] = pin;
    gpio_intr_disable(pin);
    status = gpio_set_intr_type(pin, GPIO_INTR_NEGEDGE);
    if (status == ESP_OK) {
      status = gpio_isr_handler_add(pin, &EspButtonInputOwner::gpioIsr,
                                    &contexts_[index]);
      if (status == ESP_OK) handler_installed_[index] = true;
    }
    if (status != ESP_OK) {
      disarm();
      return status;
    }
  }
  configured_ = true;
  return ESP_OK;
}

esp_err_t EspButtonInputOwner::arm() {
  if (!configured_ || armed_ || !supervisor_.started()) {
    return ESP_ERR_INVALID_STATE;
  }
  portENTER_CRITICAL(&raw_edge_mux_);
  mailbox_accepting_ = true;
  portEXIT_CRITICAL(&raw_edge_mux_);
  for (size_t index = 0; index < contexts_.size(); ++index) {
    if (!handler_installed_[index]) continue;
    const esp_err_t status = gpio_intr_enable(button_pins_[index]);
    if (status != ESP_OK) {
      disarm();
      return status;
    }
  }
  armed_ = true;
  return ESP_OK;
}

void EspButtonInputOwner::disarm() {
  for (size_t index = 0; index < contexts_.size(); ++index) {
    const gpio_num_t pin = button_pins_[index];
    if (pin == GPIO_NUM_NC) continue;
    gpio_intr_disable(pin);
    if (handler_installed_[index]) {
      gpio_isr_handler_remove(pin);
      handler_installed_[index] = false;
    }
    button_pins_[index] = GPIO_NUM_NC;
  }
  armed_ = false;
  configured_ = false;
  clearRawEdges();
  if (isr_service_owned_) {
    gpio_uninstall_isr_service();
    isr_service_owned_ = false;
  }
}

void IRAM_ATTR EspButtonInputOwner::gpioIsr(void* opaque) {
  auto* context = static_cast<IsrContext*>(opaque);
  if (context && context->owner) {
    context->owner->captureRawEdgeFromIsr(context->button_index);
  }
}

ProductOpcode EspButtonInputOwner::opcodeFor(BoardButton button) {
  switch (button) {
    case BoardButton::Previous:
      return ProductOpcode::RawButtonPrevious;
    case BoardButton::Next:
      return ProductOpcode::RawButtonNext;
    case BoardButton::Voice:
      return ProductOpcode::RawButtonVoice;
  }
  return ProductOpcode::None;
}

size_t EspButtonInputOwner::indexFor(BoardButton button) {
  switch (button) {
    case BoardButton::Previous:
      return 0;
    case BoardButton::Next:
      return 1;
    case BoardButton::Voice:
      return 2;
  }
  return ButtonDebounceCore::kButtonCount;
}

bool EspButtonInputOwner::buttonForOpcode(uint16_t opcode,
                                          BoardButton& button) {
  if (opcode == productOpcode(ProductOpcode::RawButtonPrevious)) {
    button = BoardButton::Previous;
  } else if (opcode == productOpcode(ProductOpcode::RawButtonNext)) {
    button = BoardButton::Next;
  } else if (opcode == productOpcode(ProductOpcode::RawButtonVoice)) {
    button = BoardButton::Voice;
  } else {
    return false;
  }
  return true;
}

void IRAM_ATTR EspButtonInputOwner::captureRawEdgeFromIsr(
    uint8_t button_index) {
  if (button_index >= ButtonDebounceCore::kButtonCount) return;
  const uint64_t captured = static_cast<uint64_t>(esp_timer_get_time());
  constexpr uint64_t kEventMarker = 1ULL << 63U;
  portENTER_CRITICAL_ISR(&raw_edge_mux_);
  if (!mailbox_accepting_) {
    portEXIT_CRITICAL_ISR(&raw_edge_mux_);
    return;
  }
  sequence_ = (sequence_ + 1U) & ~kEventMarker;
  if (sequence_ == 0U) sequence_ = 1U;
  if (raw_edge_count_ >= kRawEdgeCapacity) {
    if (raw_edge_overflow_ != UINT32_MAX) ++raw_edge_overflow_;
    portEXIT_CRITICAL_ISR(&raw_edge_mux_);
    return;
  }
  const size_t tail =
      (raw_edge_head_ + raw_edge_count_) % kRawEdgeCapacity;
  raw_edges_[tail].request_id = kEventMarker | sequence_;
  raw_edges_[tail].captured_us = static_cast<uint32_t>(captured);
  raw_edges_[tail].captured_ms = static_cast<uint32_t>(captured / 1000ULL);
  raw_edges_[tail].button_index = button_index;
  ++raw_edge_count_;
  portEXIT_CRITICAL_ISR(&raw_edge_mux_);
}

void EspButtonInputOwner::inputTick(void* context) {
  if (context) static_cast<EspButtonInputOwner*>(context)->serviceRawEdges();
}

bool EspButtonInputOwner::popRawEdge(RawEdge& edge) {
  portENTER_CRITICAL(&raw_edge_mux_);
  if (raw_edge_count_ == 0U) {
    portEXIT_CRITICAL(&raw_edge_mux_);
    return false;
  }
  edge = raw_edges_[raw_edge_head_];
  raw_edges_[raw_edge_head_] = RawEdge{};
  raw_edge_head_ = (raw_edge_head_ + 1U) % kRawEdgeCapacity;
  --raw_edge_count_;
  portEXIT_CRITICAL(&raw_edge_mux_);
  return true;
}

void EspButtonInputOwner::clearRawEdges() {
  portENTER_CRITICAL(&raw_edge_mux_);
  mailbox_accepting_ = false;
  for (RawEdge& edge : raw_edges_) edge = RawEdge{};
  raw_edge_head_ = 0U;
  raw_edge_count_ = 0U;
  portEXIT_CRITICAL(&raw_edge_mux_);
}

bool EspButtonInputOwner::hasPendingRawEdge() const {
  portENTER_CRITICAL(&raw_edge_mux_);
  const bool pending = raw_edge_count_ != 0U;
  portEXIT_CRITICAL(&raw_edge_mux_);
  return pending;
}

uint32_t EspButtonInputOwner::rawEdgeOverflowCount() const {
  portENTER_CRITICAL(&raw_edge_mux_);
  const uint32_t overflows = raw_edge_overflow_;
  portEXIT_CRITICAL(&raw_edge_mux_);
  return overflows;
}

void EspButtonInputOwner::serviceRawEdges() {
  for (size_t count = 0U; count < kRawEdgeCapacity; ++count) {
    RawEdge edge;
    if (!popRawEdge(edge)) return;
    const BoardButton button = kButtons[edge.button_index];
    WorkEnvelope event{};
    event.generation = 1;
    event.request_id = edge.request_id;
    event.work_class = WorkClass::Button;
    event.kind = EnvelopeKind::Command;
    event.disposition = WorkDisposition::Accepted;
    event.opcode = productOpcode(opcodeFor(button));
    event.deadline_ms = edge.captured_ms + kRawEventDeadlineMs;
    if (latency_) {
      latency_->recordCapture(
          event.request_id, latencyButton(button), edge.captured_us);
    }
    if (supervisor_.post(event) != AdmissionResult::Admitted && latency_) {
      latency_->recordTerminal(
          event.request_id, ButtonLatencyOutcome::NotReady,
          static_cast<uint32_t>(esp_timer_get_time()));
    }
  }
}

WorkDisposition EspButtonInputOwner::handle(const WorkEnvelope& envelope,
                                             void* context) {
  return context
             ? static_cast<EspButtonInputOwner*>(context)->handleInput(envelope)
             : WorkDisposition::Failed;
}

WorkDisposition EspButtonInputOwner::handleInput(
    const WorkEnvelope& envelope) {
  BoardButton button = BoardButton::Previous;
  if (envelope.kind != EnvelopeKind::Command ||
      envelope.work_class != WorkClass::Button ||
      !buttonForOpcode(envelope.opcode, button)) {
    return WorkDisposition::Failed;
  }
  return debounce_.accept(indexFor(button), nowMs())
             ? WorkDisposition::Complete
             : WorkDisposition::Cancelled;
}

}  // namespace inkloop
