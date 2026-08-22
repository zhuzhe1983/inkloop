#pragma once

#include "inkloop/myai/CredentialPersistence.h"

namespace inkloop {
namespace myai {

class JsonSha256CredentialCodec final : public ICredentialRecordCodec {
 public:
  Status encode(const CredentialSnapshot& snapshot,
                std::string& output) const override;
  Status decode(const std::string& input,
                CredentialSnapshot& snapshot) const override;
};

class EspNvsCredentialJournalStore final : public ICredentialJournalStore {
 public:
  Status inspect(CredentialJournalState& state) override;
  Status writeSlotAndCommit(uint8_t slot,
                            const std::string& encoded) override;
  Status writeHeadAndMarkerAndCommit(uint32_t generation) override;
};

}  // namespace myai
}  // namespace inkloop
