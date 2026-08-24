#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "inkloop/local_tools/local_tools.hpp"
#include "inkloop/work_contracts.hpp"

namespace inkloop {

// DisplayInteractiveAlbumOrdinal uses payload_bytes only as a typed marker
// that an expected-asset mailbox must exist for this request_id. No pointer or
// byte buffer crosses a task queue.
inline constexpr uint32_t kLocalToolDisplaySelectionPayloadMarker = 1U;
inline constexpr uint8_t kLocalToolDisplaySelectionFlag = 0x80U;
inline constexpr uint8_t kLocalToolDisplayOrdinalMask = 0x7fU;
inline constexpr uint32_t kLocalToolDisplayResultTimeoutMs = 120000U;

enum class LocalToolDisplayArmResult : uint8_t {
  Armed,
  Busy,
  Invalid,
};

enum class LocalToolDisplayPhase : uint8_t {
  Idle,
  AwaitingResult,
  Terminal,
};

// Fixed-capacity correlation state copied only while the caller holds its
// cross-core critical section. It deliberately owns no string, pointer,
// semaphore or heap allocation.
struct LocalToolDisplayTerminal {
  std::array<char, local_tools::kMaximumImageIdBytes + 1U>
      expected_asset_id{};
  uint64_t request_id = 0U;
  uint32_t ordinal = 0U;
  uint32_t total = 0U;
  WorkDisposition disposition = WorkDisposition::Failed;
};

static_assert(std::is_trivially_copyable<LocalToolDisplayTerminal>::value,
              "local-tool display correlation must remain POD");

// Portable one-flight state machine. NativeVoiceService supplies the critical
// section because arm/expiry run on Portal while resolve runs on Control.
class LocalToolDisplayCorrelation final {
 public:
  LocalToolDisplayArmResult arm(uint64_t request_id, uint32_t ordinal,
                                uint32_t total,
                                std::string_view expected_asset_id,
                                uint32_t now_ms, uint32_t timeout_ms);
  bool resolve(uint64_t request_id, WorkDisposition disposition);
  bool expire(uint32_t now_ms);
  bool takeTerminal(LocalToolDisplayTerminal& output);
  bool owns(uint64_t request_id) const;
  bool matches(uint64_t request_id, uint32_t zero_based_ordinal) const;
  bool active() const { return phase_ != LocalToolDisplayPhase::Idle; }
  uint64_t requestId() const { return terminal_.request_id; }
  LocalToolDisplayPhase phase() const { return phase_; }
  void reset();

 private:
  static bool due(uint32_t now_ms, uint32_t deadline_ms);
  static bool validAssetId(std::string_view value);

  LocalToolDisplayTerminal terminal_{};
  uint32_t deadline_ms_ = 0U;
  LocalToolDisplayPhase phase_ = LocalToolDisplayPhase::Idle;
};

}  // namespace inkloop
