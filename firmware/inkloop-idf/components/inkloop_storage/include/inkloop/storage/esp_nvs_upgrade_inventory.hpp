#pragma once

#include <array>

#include "inkloop/storage/upgrade_audit.hpp"

namespace inkloop {
namespace storage {

// Native read-only probe for every namespace in kProtectedNvsNamespaces.
// Call only after nvs_flash_init(). It never creates a namespace, writes a
// marker, rotates a slot, or returns secret material to callers.
class EspNvsUpgradeInventory final {
 public:
  std::array<RecordProbe, kProtectedNvsNamespaces.size()> inspect() const;
};

}  // namespace storage
}  // namespace inkloop
