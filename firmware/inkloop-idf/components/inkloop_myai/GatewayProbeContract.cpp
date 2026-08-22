#include "GatewayProbeContract.h"

#include <climits>
#include <set>

namespace inkloop {
namespace myai {

Status GatewayProbeContract::validateCandidates(
    const std::vector<GatewayCandidate>& candidates) {
  if (candidates.empty() || candidates.size() > kMaximumCandidates)
    return Status(ErrorCode::Protocol, 0,
                  "invalid MyAI gateway candidate count");
  std::set<std::string> candidate_ids;
  for (const GatewayCandidate& candidate : candidates) {
    if (candidate.id.empty() || !candidate_ids.insert(candidate.id).second)
      return Status(ErrorCode::Protocol, 0,
                    "duplicate or empty MyAI gateway candidate id");
  }
  return Status::success();
}

Status GatewayProbeContract::selectFastest(
    const std::vector<GatewayCandidate>& candidates,
    const std::vector<GatewayProbe>& results, GatewayCandidate& selected) {
  selected = GatewayCandidate();
  Status valid = validateCandidates(candidates);
  if (!valid.ok()) return valid;
  if (results.size() != candidates.size())
    return Status(ErrorCode::Protocol, 0,
                  "incomplete MyAI gateway probe result set");

  std::set<std::string> candidate_ids;
  for (const GatewayCandidate& candidate : candidates)
    candidate_ids.insert(candidate.id);
  std::set<std::string> result_ids;
  for (const GatewayProbe& result : results) {
    if (candidate_ids.count(result.gatewayId) != 1 ||
        !result_ids.insert(result.gatewayId).second)
      return Status(ErrorCode::Protocol, 0,
                    "unknown or duplicate MyAI gateway probe result");
  }
  if (result_ids != candidate_ids)
    return Status(ErrorCode::Protocol, 0,
                  "mismatched MyAI gateway probe result set");

  const GatewayCandidate* fastest = nullptr;
  uint32_t fastest_latency = UINT_MAX;
  for (const GatewayCandidate& candidate : candidates) {
    for (const GatewayProbe& result : results) {
      if (result.gatewayId != candidate.id) continue;
      if (result.ok && (!fastest || result.latencyMs < fastest_latency)) {
        fastest = &candidate;
        fastest_latency = result.latencyMs;
      }
      break;
    }
  }
  if (!fastest)
    return Status(ErrorCode::NoGateway, 503,
                  "no reachable public MyAI gateway");
  selected = *fastest;
  return Status::success();
}

}  // namespace myai
}  // namespace inkloop
