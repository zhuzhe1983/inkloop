#pragma once

#include "MyAiTypes.h"

#include <cstddef>
#include <vector>

namespace inkloop {
namespace myai {

class GatewayProbeContract {
 public:
  static constexpr size_t kMaximumCandidates = 8;
  static constexpr uint32_t kTotalDeadlineMs = 8000;

  static Status validateCandidates(
      const std::vector<GatewayCandidate>& candidates);

  // Results must contain exactly one record for every candidate ID, without
  // duplicates or unknown IDs. The fastest successful candidate is selected;
  // ties preserve Center's candidate order for deterministic behavior.
  static Status selectFastest(const std::vector<GatewayCandidate>& candidates,
                              const std::vector<GatewayProbe>& results,
                              GatewayCandidate& selected);
};

}  // namespace myai
}  // namespace inkloop
