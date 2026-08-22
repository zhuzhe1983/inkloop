#include "inkloop/button_input.hpp"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "inkloop/product_opcodes.hpp"

namespace inkloop {
namespace {

constexpr std::array kButtons{
    BoardButton::Previous, BoardButton::Next, BoardButton::Voice};
static_assert(kButtons.size() == ButtonDebounceCore::kButtonCount);
constexpr uint32_t kRawEventDeadlineMs = 100;

uint32_t nowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

}  // namespace

EspButtonInputOwner::EspButtonInputOwner(IBoardAdapter& board,
                                         RuntimeSupervisor& supervisor)
    : board_(board), supervisor_(supervisor) {
  button_pins_.fill(GPIO_NUM_NC);
  for (size_t index = 0; index < contexts_.size(); ++index) {
    contexts_[index].owner = this;
    contexts_[index].button = kButtons[index];
  }
}

EspButtonInputOwner::~EspButtonInputOwner() { disarm(); }

esp_err_t EspButtonInputOwner::configure() {
  if (configured_) return ESP_ERR_INVALID_STATE;
  esp_err_t status = supervisor_.registerHandler(
      TaskLane::Input, &EspButtonInputOwner::handle, this);
  if (status != ESP_OK) return status;

  const BoardDescriptor& capabilities = board_.descriptor();
  bool any_supported = false;
  for (const IsrContext& context : contexts_) {
    if (capabilities.supportsButton(context.button)) any_supported = true;
  }
  if (any_supported) {
    status = gpio_install_isr_service(0);
    if (status == ESP_OK) {
      isr_service_owned_ = true;
    } else if (status != ESP_ERR_INVALID_STATE) {
      return status;
    }
  }
  for (size_t index = 0; index < contexts_.size(); ++index) {
    if (!capabilities.supportsButton(contexts_[index].button)) continue;
    const gpio_num_t pin = board_.buttonGpio(contexts_[index].button);
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
  if (isr_service_owned_) {
    gpio_uninstall_isr_service();
    isr_service_owned_ = false;
  }
}

void EspButtonInputOwner::gpioIsr(void* opaque) {
  auto* context = static_cast<IsrContext*>(opaque);
  if (context && context->owner) context->owner->postFromIsr(context->button);
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

void EspButtonInputOwner::postFromIsr(BoardButton button) {
  WorkEnvelope event{};
  event.generation = 1;
  portENTER_CRITICAL_ISR(&sequence_mux_);
  event.request_id = ++sequence_;
  portEXIT_CRITICAL_ISR(&sequence_mux_);
  event.work_class = WorkClass::Button;
  event.kind = EnvelopeKind::Command;
  event.disposition = WorkDisposition::Accepted;
  event.opcode = productOpcode(opcodeFor(button));
  const uint32_t captured = nowMs();
  event.deadline_ms = captured + kRawEventDeadlineMs;
  BaseType_t task_woken = pdFALSE;
  supervisor_.postButtonFromIsr(event, &task_woken);
  if (task_woken == pdTRUE) portYIELD_FROM_ISR();
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
