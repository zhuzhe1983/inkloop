#pragma once

#include <cstdint>

namespace inkloop {

enum class StorageMaintenanceCode : uint8_t {
  Ok,
  Busy,
  NotReady,
  IoError,
};

struct StorageMaintenanceResult {
  StorageMaintenanceCode code = StorageMaintenanceCode::Ok;

  bool ok() const { return code == StorageMaintenanceCode::Ok; }
};

// Composition boundary for the only destructive filesystem action exposed by
// local tools. DeviceState owns settings and confirmation semantics, while the
// product runtime owns cross-task quiescence and the exact removable target.
class IStorageMaintenanceCoordinator {
 public:
  virtual ~IStorageMaintenanceCoordinator() = default;
  virtual StorageMaintenanceResult formatTfCardConfirmed() = 0;
};

}  // namespace inkloop
