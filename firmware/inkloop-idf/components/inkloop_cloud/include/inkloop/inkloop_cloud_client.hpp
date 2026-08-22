#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "inkloop/myai/MyAiAdapters.h"
#include "inkloop/storage/aigc_album_sink.hpp"
#include "inkloop/storage/posix_task_store.hpp"

namespace inkloop {
namespace cloud {

enum class InkloopCloudCode : uint8_t {
  Ok,
  InvalidArgument,
  NotInitialized,
  Storage,
  Security,
  Transport,
  Unauthorized,
  Conflict,
  ServerUnavailable,
  TooLarge,
  Protocol,
  PairingRequired,
};

struct InkloopCloudStatus {
  InkloopCloudCode code = InkloopCloudCode::Ok;
  int http_status = 0;
  uint32_t retry_after_ms = 0;
  std::string detail;

  bool ok() const { return code == InkloopCloudCode::Ok; }
  static InkloopCloudStatus success() { return {}; }
};

struct InkloopIdentitySnapshot {
  std::string hardware_id;
  std::string device_id;
  std::string secret;
  uint32_t applied_revision = 0;
};

// Implementations own credential generation and durable commits. A failed
// commit must leave the previous readable snapshot intact.
class IInkloopIdentityStore {
 public:
  virtual ~IInkloopIdentityStore() = default;
  virtual InkloopCloudStatus loadOrCreate(
      InkloopIdentitySnapshot& snapshot) = 0;
  virtual InkloopCloudStatus saveDeviceId(
      const std::string& device_id) = 0;
  virtual InkloopCloudStatus saveAppliedRevision(uint32_t revision) = 0;
};

// Streaming frame transport. The implementation authenticates with the
// supplied identity and commits through the album staging transaction; it must
// never materialize the complete PNG in a network buffer.
class IInkloopFrameDownloader {
 public:
  virtual ~IInkloopFrameDownloader() = default;
  virtual InkloopCloudStatus download(
      const InkloopIdentitySnapshot& identity,
      const storage::InkloopTaskRecord& task,
      storage::IAlbumStagingStore& album,
      storage::AlbumCommitResult& result) = 0;
};

struct InkloopCloudConfig {
  std::string api_url = "https://inkloop.mess.host/api/devices";
  std::string sku_id = "m5-papercolor-c151";
  std::string firmware_version = "0.3.0-idf";
  uint32_t request_timeout_ms = 20000;
  size_t maximum_response_bytes = 256U * 1024U;
};

struct InkloopRegistrationResult {
  bool paired = false;
  bool requested_pairing_code_accepted = false;
  std::string pairing_code;
  std::string pairing_expires_at;
  uint32_t poll_seconds = 15;
};

struct InkloopSyncResult {
  bool paired = false;
  bool changed = false;
  bool became_paired = false;
  bool requires_registration = false;
  uint32_t revision = 0;
  uint32_t poll_seconds = 15;
  size_t task_count = 0;
};

// Portable protocol owner. It never downloads frames or touches display
// hardware; those operations are delegated to the board/runtime integration.
class InkloopCloudClient final {
 public:
  InkloopCloudClient(InkloopCloudConfig config, myai::IHttpTransport& http,
                     IInkloopIdentityStore& identity,
                     storage::PosixTaskStore& tasks);

  InkloopCloudStatus initialize();
  InkloopCloudStatus registerDevice(
      const std::string& authoritative_pairing_code,
      InkloopRegistrationResult& result);
  InkloopCloudStatus syncTasks(InkloopSyncResult& result);

  const InkloopIdentitySnapshot& identity() const { return identity_snapshot_; }
  bool paired() const { return paired_; }
  bool initialized() const { return initialized_; }
  static const char* codeName(InkloopCloudCode code);

 private:
  InkloopCloudStatus perform(const std::string& body, bool authenticated,
                             myai::HttpResponse& response);
  InkloopCloudStatus classifyHttp(const myai::HttpResponse& response) const;
  InkloopCloudStatus parseRegistration(
      const std::string& body, const std::string& requested_code,
      std::string& device_id, InkloopRegistrationResult& result) const;
  InkloopCloudStatus parseSync(const std::string& body,
                              InkloopSyncResult& result,
                              std::vector<storage::InkloopTaskRecord>& tasks) const;
  std::string registrationBody(const std::string& pairing_code) const;
  std::string syncBody() const;
  static bool validConfig(const InkloopCloudConfig& config);
  static bool validIdentity(const InkloopIdentitySnapshot& identity);

  InkloopCloudConfig config_;
  myai::IHttpTransport& http_;
  IInkloopIdentityStore& identity_store_;
  storage::PosixTaskStore& tasks_;
  InkloopIdentitySnapshot identity_snapshot_;
  bool paired_ = false;
  bool initialized_ = false;
};

}  // namespace cloud
}  // namespace inkloop
