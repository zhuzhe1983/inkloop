#include "inkloop/local_tool_display_correlation.hpp"

#include <algorithm>
#include <limits>

namespace inkloop {

bool LocalToolDisplayCorrelation::due(uint32_t now_ms,
                                      uint32_t deadline_ms) {
  // A valid wrapped deadline may be exactly zero.  The phase, rather than a
  // sentinel deadline value, determines whether a timeout is armed.
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool LocalToolDisplayCorrelation::validAssetId(std::string_view value) {
  if (value.empty() ||
      value.size() > local_tools::kMaximumImageIdBytes ||
      value.front() == '.' || value.back() == '.' ||
      value.find("..") != std::string_view::npos ||
      value.find('/') != std::string_view::npos ||
      value.find('\\') != std::string_view::npos ||
      (value.size() == 2U &&
       ((value.front() >= 'a' && value.front() <= 'z') ||
        (value.front() >= 'A' && value.front() <= 'Z')) &&
       value.back() == ':')) {
    return false;
  }
  for (const unsigned char character : value) {
    const bool valid =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' ||
        character == '_' || character == ':' || character == '.';
    if (!valid) return false;
  }
  return true;
}

LocalToolDisplayArmResult LocalToolDisplayCorrelation::arm(
    uint64_t request_id, uint32_t ordinal, uint32_t total,
    std::string_view expected_asset_id, uint32_t now_ms,
    uint32_t timeout_ms) {
  if (phase_ != LocalToolDisplayPhase::Idle)
    return LocalToolDisplayArmResult::Busy;
  if (request_id == 0U || ordinal == 0U ||
      ordinal > local_tools::kMaximumImageOrdinal || total < ordinal ||
      total > local_tools::kMaximumImageOrdinal || timeout_ms == 0U ||
      timeout_ms > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
      !validAssetId(expected_asset_id)) {
    return LocalToolDisplayArmResult::Invalid;
  }

  terminal_ = LocalToolDisplayTerminal{};
  std::copy(expected_asset_id.begin(), expected_asset_id.end(),
            terminal_.expected_asset_id.begin());
  terminal_.request_id = request_id;
  terminal_.ordinal = ordinal;
  terminal_.total = total;
  terminal_.disposition = WorkDisposition::Accepted;
  deadline_ms_ = now_ms + timeout_ms;
  phase_ = LocalToolDisplayPhase::AwaitingResult;
  return LocalToolDisplayArmResult::Armed;
}

bool LocalToolDisplayCorrelation::resolve(
    uint64_t request_id, WorkDisposition disposition) {
  if (phase_ != LocalToolDisplayPhase::AwaitingResult || request_id == 0U ||
      terminal_.request_id != request_id ||
      disposition == WorkDisposition::Accepted) {
    return false;
  }
  terminal_.disposition = disposition;
  deadline_ms_ = 0U;
  phase_ = LocalToolDisplayPhase::Terminal;
  return true;
}

bool LocalToolDisplayCorrelation::expire(uint32_t now_ms) {
  if (phase_ != LocalToolDisplayPhase::AwaitingResult ||
      !due(now_ms, deadline_ms_)) {
    return false;
  }
  terminal_.disposition = WorkDisposition::TimedOut;
  deadline_ms_ = 0U;
  phase_ = LocalToolDisplayPhase::Terminal;
  return true;
}

bool LocalToolDisplayCorrelation::takeTerminal(
    LocalToolDisplayTerminal& output) {
  if (phase_ != LocalToolDisplayPhase::Terminal) return false;
  output = terminal_;
  reset();
  return true;
}

bool LocalToolDisplayCorrelation::owns(uint64_t request_id) const {
  return request_id != 0U && phase_ != LocalToolDisplayPhase::Idle &&
      terminal_.request_id == request_id;
}

bool LocalToolDisplayCorrelation::matches(
    uint64_t request_id, uint32_t zero_based_ordinal) const {
  return owns(request_id) && terminal_.ordinal != 0U &&
      terminal_.ordinal - 1U == zero_based_ordinal;
}

void LocalToolDisplayCorrelation::reset() {
  terminal_ = LocalToolDisplayTerminal{};
  deadline_ms_ = 0U;
  phase_ = LocalToolDisplayPhase::Idle;
}

}  // namespace inkloop
