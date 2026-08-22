#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "inkloop/inkloop_cloud_client.hpp"

namespace inkloop {
namespace cloud {

// Exact Arduino Preferences surface used by the released firmware. The
// backend contract is transactional: a failed commit leaves the previous
// readable values untouched.
struct ArduinoInkloopIdentityRecord {
  bool device_id_present = false;
  bool secret_present = false;
  bool revision_present = false;
  std::string device_id;
  std::string secret;
  uint32_t revision = 0;
};

class IArduinoInkloopIdentityNvs {
 public:
  virtual ~IArduinoInkloopIdentityNvs() = default;
  virtual InkloopCloudStatus read(ArduinoInkloopIdentityRecord& record) = 0;
  virtual InkloopCloudStatus commitFreshIdentity(
      const std::string& secret) = 0;
  virtual InkloopCloudStatus commitDeviceId(
      const std::string& device_id) = 0;
  virtual InkloopCloudStatus commitRevision(uint32_t revision) = 0;
};

// ESP.getEfuseMac() writes the six default eFuse MAC bytes into a uint64_t.
// Keeping that numeric value preserves already registered M5PC-* hardware IDs.
class IArduinoInkloopIdentityPlatform {
 public:
  virtual ~IArduinoInkloopIdentityPlatform() = default;
  virtual bool readLegacyEfuseMac(uint64_t& mac) = 0;
  virtual bool fillRandom(uint8_t* bytes, size_t length) = 0;
};

class ArduinoCompatibleInkloopIdentityStore final
    : public IInkloopIdentityStore {
 public:
  ArduinoCompatibleInkloopIdentityStore(
      IArduinoInkloopIdentityNvs& nvs,
      IArduinoInkloopIdentityPlatform& platform);

  InkloopCloudStatus loadOrCreate(
      InkloopIdentitySnapshot& snapshot) override;
  InkloopCloudStatus saveDeviceId(
      const std::string& device_id) override;
  InkloopCloudStatus saveAppliedRevision(uint32_t revision) override;

 private:
  IArduinoInkloopIdentityNvs& nvs_;
  IArduinoInkloopIdentityPlatform& platform_;
};

}  // namespace cloud
}  // namespace inkloop
