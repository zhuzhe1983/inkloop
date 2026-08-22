#include "inkloop/bounded_text_pool.hpp"

#include <algorithm>
#include <cstring>

namespace inkloop {

uint64_t BoundedTextPool::put(ProductTextKind kind,
                              const std::string& text) {
  if (text.empty() || text.size() > kMaximumTextBytes) return 0;
  portENTER_CRITICAL(&mux_);
  Slot* available = nullptr;
  for (Slot& slot : slots_) {
    if (!slot.occupied) {
      available = &slot;
      break;
    }
  }
  if (!available) {
    portEXIT_CRITICAL(&mux_);
    return 0;
  }
  do {
    ++next_ticket_;
  } while (next_ticket_ == 0);
  std::memcpy(available->bytes.data(), text.data(), text.size());
  available->bytes[text.size()] = '\0';
  available->ticket = next_ticket_;
  available->length = text.size();
  available->kind = kind;
  available->occupied = true;
  const uint64_t ticket = available->ticket;
  portEXIT_CRITICAL(&mux_);
  return ticket;
}

bool BoundedTextPool::take(uint64_t ticket, ProductTextKind& kind,
                           std::string& text) {
  if (ticket == 0) return false;
  portENTER_CRITICAL(&mux_);
  Slot* found = nullptr;
  for (Slot& slot : slots_) {
    if (slot.occupied && slot.ticket == ticket) {
      found = &slot;
      break;
    }
  }
  if (!found) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  kind = found->kind;
  text.assign(found->bytes.data(), found->length);
  std::fill(found->bytes.begin(), found->bytes.end(), '\0');
  found->ticket = 0;
  found->length = 0;
  found->occupied = false;
  portEXIT_CRITICAL(&mux_);
  return true;
}

bool BoundedTextPool::release(uint64_t ticket) {
  if (ticket == 0) return false;
  portENTER_CRITICAL(&mux_);
  for (Slot& slot : slots_) {
    if (slot.occupied && slot.ticket == ticket) {
      std::fill(slot.bytes.begin(), slot.bytes.end(), '\0');
      slot.ticket = 0;
      slot.length = 0;
      slot.occupied = false;
      portEXIT_CRITICAL(&mux_);
      return true;
    }
  }
  portEXIT_CRITICAL(&mux_);
  return false;
}

void BoundedTextPool::clear() {
  portENTER_CRITICAL(&mux_);
  for (Slot& slot : slots_) {
    std::fill(slot.bytes.begin(), slot.bytes.end(), '\0');
    slot.ticket = 0U;
    slot.length = 0U;
    slot.kind = ProductTextKind::ToolState;
    slot.occupied = false;
  }
  next_ticket_ = 0U;
  portEXIT_CRITICAL(&mux_);
}

size_t BoundedTextPool::used() const {
  portENTER_CRITICAL(&mux_);
  size_t count = 0;
  for (const Slot& slot : slots_)
    if (slot.occupied) ++count;
  portEXIT_CRITICAL(&mux_);
  return count;
}

}  // namespace inkloop
