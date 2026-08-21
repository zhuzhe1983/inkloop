#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include "TaskStore.h"

namespace inkloop {

struct RegistrationResult {
  bool ok = false;
  int httpStatus = 0;
  bool paired = false;
  // True only when a requested authoritative code was accepted. An already
  // paired response is an idempotent success and intentionally returns no
  // secret code.
  bool requestedPairingCodeAccepted = false;
  String pairingCode;
  String pairingExpiresAt;
};

struct SyncResult {
  bool ok = false;
  bool becamePaired = false;
  bool requiresRegistration = false;
};

struct DownloadedFrame {
  uint8_t* bytes = nullptr;
  size_t length = 0;
  bool landscape = false;

  void release();
};

class InkloopClient {
 public:
  explicit InkloopClient(TaskStore& tasks) : tasks_(tasks) {}

  bool beginIdentity();
  RegistrationResult registerDevice();
  RegistrationResult registerDevice(const String& requestedPairingCode);
  SyncResult syncTasks();
  bool downloadFrame(const String& frameUrl, DownloadedFrame& frame);

  const String& hardwareId() const { return hardwareId_; }
  const String& deviceId() const { return deviceId_; }
  const String& pairingCode() const { return pairingCode_; }
  bool paired() const { return paired_; }
  uint32_t appliedRevision() const { return appliedRevision_; }
  String apiUrl() const;

 private:
  RegistrationResult registerDeviceImpl(const String* requestedPairingCode);
  template <typename Client>
  int postJsonWithClient(Client& client, const String& url, const String& body, String& response, bool authenticate);
  int postJson(const String& body, String& response, bool authenticate);
  template <typename Client>
  bool downloadFrameWithClient(Client& client, const String& frameUrl, DownloadedFrame& frame);
  static bool allowedPrivateHttp(const String& url);
  static String hexBytes(const uint8_t* bytes, size_t length);

  TaskStore& tasks_;
  Preferences preferences_;
  String hardwareId_;
  String deviceId_;
  String deviceSecret_;
  String pairingCode_;
  uint32_t appliedRevision_ = 0;
  bool paired_ = false;
};

}  // namespace inkloop
