#pragma once

#include <stdint.h>

namespace inkloop {

enum class AlbumMutationResult : uint8_t {
  Complete,
  RevisionUnavailable,
  RevisionPersistenceFailed,
  MutationFailed,
};

// The album catalog and its NVS revision live in different stores, so they
// cannot share one atomic commit.  Persist the next revision first and publish
// that freshness barrier before touching the catalog.  A failed catalog
// mutation may consume a revision, but a failed revision write can never be
// followed by a catalog mutation.
template <typename PersistRevision, typename PublishRevision,
          typename PerformMutation>
AlbumMutationResult runRevisionGatedAlbumMutation(
    uint64_t currentRevision, PersistRevision persistRevision,
    PublishRevision publishRevision, PerformMutation performMutation,
    uint64_t* promotedRevision = nullptr) {
  if (promotedRevision) *promotedRevision = currentRevision;
  if (currentRevision == 0 || currentRevision == UINT64_MAX) {
    publishRevision(0, false);
    return AlbumMutationResult::RevisionUnavailable;
  }
  const uint64_t nextRevision = currentRevision + 1;
  if (!persistRevision(nextRevision)) {
    // Revision zero is an invalid confirmation binding.  Publishing it clears
    // any authority held by a concurrent confirmation flow and prevents new
    // confirmations until persistent revision health is restored.
    publishRevision(0, false);
    return AlbumMutationResult::RevisionPersistenceFailed;
  }
  if (promotedRevision) *promotedRevision = nextRevision;
  publishRevision(nextRevision, true);
  return performMutation() ? AlbumMutationResult::Complete
                           : AlbumMutationResult::MutationFailed;
}

}  // namespace inkloop
