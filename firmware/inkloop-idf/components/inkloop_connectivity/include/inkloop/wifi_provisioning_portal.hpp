#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace inkloop {

inline constexpr size_t kMaximumWifiSsidBytes = 32U;
inline constexpr size_t kMaximumWifiPasswordBytes = 63U;
inline constexpr size_t kMaximumWifiProvisioningBodyBytes = 320U;

enum class WifiProvisioningSubmitResult : uint8_t {
  Accepted,
  Busy,
  Invalid,
  Unavailable,
};

class IWifiProvisioningSink {
 public:
  virtual ~IWifiProvisioningSink() = default;
  // Called from the HTTP task. It may only copy into one bounded pending slot.
  virtual WifiProvisioningSubmitResult trySubmitCredentials(
      const std::string& ssid, const std::string& password) = 0;
};

struct WifiProvisioningRequest {
  std::string method;
  std::string path;
  std::string content_type;
  std::string body;
};

struct WifiProvisioningResponse {
  int status = 500;
  std::string content_type = "application/json; charset=utf-8";
  std::string body;
};

class WifiProvisioningPortal final {
 public:
  explicit WifiProvisioningPortal(IWifiProvisioningSink& sink)
      : sink_(sink) {}

  WifiProvisioningResponse handle(const WifiProvisioningRequest& request);
  static const char* pageHtml();
  static bool validSsid(const std::string& value);
  static bool validPassword(const std::string& value);

 private:
  IWifiProvisioningSink& sink_;
};

const char* wifiProvisioningSubmitResultName(
    WifiProvisioningSubmitResult result);

}  // namespace inkloop
