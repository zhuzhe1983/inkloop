#pragma once

#include "inkloop/cloud/arduino_compatible_identity_store.hpp"

namespace inkloop {
namespace cloud {

class EspArduinoInkloopIdentityNvs final
    : public IArduinoInkloopIdentityNvs {
 public:
  InkloopCloudStatus read(ArduinoInkloopIdentityRecord& record) override;
  InkloopCloudStatus commitFreshIdentity(
      const std::string& secret) override;
  InkloopCloudStatus commitDeviceId(
      const std::string& device_id) override;
  InkloopCloudStatus commitRevision(uint32_t revision) override;
};

class EspArduinoInkloopIdentityPlatform final
    : public IArduinoInkloopIdentityPlatform {
 public:
  bool readLegacyEfuseMac(uint64_t& mac) override;
  bool fillRandom(uint8_t* bytes, size_t length) override;
};

// Drop-in identity owner for InkloopCloudClient. It assumes nvs_flash has
// already been initialized by the boot/storage owner and never initializes,
// erases, or repairs NVS on its own.
class EspNvsInkloopIdentityStore final : public IInkloopIdentityStore {
 public:
  EspNvsInkloopIdentityStore();

  InkloopCloudStatus loadOrCreate(
      InkloopIdentitySnapshot& snapshot) override;
  InkloopCloudStatus saveDeviceId(
      const std::string& device_id) override;
  InkloopCloudStatus saveAppliedRevision(uint32_t revision) override;

 private:
  EspArduinoInkloopIdentityNvs nvs_;
  EspArduinoInkloopIdentityPlatform platform_;
  ArduinoCompatibleInkloopIdentityStore store_;
};

}  // namespace cloud
}  // namespace inkloop
