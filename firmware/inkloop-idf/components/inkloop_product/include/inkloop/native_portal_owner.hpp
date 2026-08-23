#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "inkloop/board.hpp"
#include "inkloop/esp_wifi_station.hpp"
#include "inkloop/native_display_service.hpp"
#include "inkloop/native_voice_service.hpp"
#include "inkloop/portal/esp_portal_server.hpp"
#include "inkloop/runtime_supervisor.hpp"
#include "inkloop/status_led_owner.hpp"
#include "inkloop/storage/posix_atomic_album_store.hpp"

namespace inkloop {

// The product settings component is the sole persistence owner. Portal keeps
// only a bounded copy; it never invents a second settings journal.
class IPortalSettingsOwner {
 public:
  virtual ~IPortalSettingsOwner() = default;
  virtual portal::PortalResult readPortalSettings(
      portal::PortalSettingsSnapshot& output) const = 0;
  virtual portal::PortalResult applyPortalSettings(
      const portal::PortalSettingsPatch& patch,
      portal::PortalSettingsSnapshot& output) = 0;
};

class IPortalAlbumMutationOwner {
 public:
  virtual ~IPortalAlbumMutationOwner() = default;
  virtual portal::PortalResult deletePortalAlbumItem(
      const std::string& exact_asset_id) = 0;
};

// Atomic, credential-free bridge to the root-owned OTA coordinator. Reading
// and requesting must never perform network or flash work in the Portal task.
class IPortalFirmwareUpdateOwner {
 public:
  virtual ~IPortalFirmwareUpdateOwner() = default;
  virtual portal::PortalResult readPortalFirmwareUpdate(
      portal::PortalFirmwareUpdateSnapshot& output) const = 0;
  virtual portal::PortalResult requestPortalFirmwareUpdate(
      uint64_t request_id) = 0;
};

struct NativePortalAlbumDiagnosticSnapshot {
  size_t total_items = 0U;
  size_t current_one_based = 0U;
  bool ready = false;
};

// Product owner for the low-priority local WebUI. HTTP callbacks only copy
// bounded snapshots/commands/chunks. Album and settings mutations are drained
// by the existing Portal lane through tick().
class NativePortalOwner final : public portal::IPortalReadCache,
                                public portal::IPortalCommandQueue,
                                public portal::IPortalUploadQueue,
                                public portal::IPortalPreviewSource,
                                public INativeLocalChatSnapshotConsumer {
 public:
  NativePortalOwner(IBoardAdapter& board, RuntimeSupervisor& supervisor,
                    EspWifiStationOwner& wifi, EspStatusLedOwner& leds,
                    NativeDisplayService& display, NativeVoiceService& voice,
                    const char* storage_root,
                    storage::PosixAtomicAlbumStore* album_store);
  ~NativePortalOwner();

  NativePortalOwner(const NativePortalOwner&) = delete;
  NativePortalOwner& operator=(const NativePortalOwner&) = delete;

  esp_err_t initialize();
  // Supervisor must be stopped first. Joins HTTP/preview workers, removes
  // mDNS, closes preview files, aborts an interrupted upload transaction and
  // releases all queue/mutex/heap resources. Safe to call repeatedly.
  esp_err_t shutdown();
  esp_err_t attachSettingsOwner(IPortalSettingsOwner& owner);
  esp_err_t attachAlbumMutationOwner(IPortalAlbumMutationOwner& owner);
  esp_err_t attachFirmwareUpdateOwner(IPortalFirmwareUpdateOwner& owner);
  // Called only by the future sole Storage-lane chat owner after it has made
  // a bounded local snapshot. Portal never scans the live JSONL concurrently.
  portal::PortalResult publishLocalChatSnapshot(
      const portal::ChatPage& snapshot);
  void tick(bool wifi_online, bool storage_mutation_allowed);
  bool mutationBusy() const;
  // Cross-lane, allocation-free cache invalidation. Display completion may
  // follow an AIGC or Inkloop asset commit performed by another Portal-lane
  // owner; the next admitted Portal tick refreshes the browser-facing album
  // snapshot without doing filesystem I/O on the caller's lane.
  void requestAlbumRefresh();
  // Coordinator-only TF maintenance gate. begin atomically rejects new
  // storage-facing HTTP work and succeeds only after the Portal server and
  // every queued/active upload, preview and command are drained. Failure rolls
  // the gate back. finish invalidates caches after a changed filesystem;
  // end permits a refreshed server restart on the next Portal tick.
  bool beginStorageMaintenance();
  bool finishStorageMaintenance(bool storage_changed,
                                bool storage_available);
  void endStorageMaintenance();
  bool storageMaintenanceActive() const;
  uint32_t lastAccessMs() const;
  bool running() const;
  // Serial diagnostics consume the same owner-maintained caches without
  // pretending that a browser accessed the WebUI. These snapshots contain no
  // session, CSRF, access-code, device-code or MyAI credential material.
  portal::PortalResult readSerialDiagnosticState(
      portal::PortalStateSnapshot& output) const;
  NativePortalAlbumDiagnosticSnapshot serialDiagnosticAlbum() const;

  portal::PortalResult readState(
      portal::PortalStateSnapshot& output) const override;
  portal::PortalResult readAlbumPage(
      const portal::AlbumPageQuery& query,
      portal::AlbumPage& output) const override;
  portal::PortalResult readLocalChatPage(
      const portal::ChatPageQuery& query,
      portal::ChatPage& output) const override;
  portal::PortalResult tryEnqueue(
      const portal::PortalCommand& command) override;

  portal::PortalResult tryBegin(
      const portal::PortalStreamRequest& request) override;
  portal::PortalResult tryWrite(uint64_t request_id, const uint8_t* bytes,
                                size_t length) override;
  portal::PortalResult tryFinish(uint64_t request_id) override;
  void tryAbort(uint64_t request_id) override;

  portal::PortalResult open(const std::string& asset_id,
                            portal::PortalPreviewInfo& output) override;
  portal::PortalResult read(uint64_t handle, uint8_t* output, size_t capacity,
                            size_t& bytes_read) override;
  void close(uint64_t handle) override;
  bool accept(const NativeLocalChatSnapshot& snapshot) override;

 private:
  static constexpr size_t kCommandSlots = 8U;
  static constexpr size_t kUploadSlots = 32U;
  static constexpr size_t kUploadChunkBytes = 2048U;
  static constexpr size_t kUploadEventDepth = kUploadSlots + 4U;
  static constexpr size_t kPreviewHandles = 3U;

  enum class UploadEventKind : uint8_t { Begin, Chunk, Finish, Abort };

  struct UploadEvent {
    UploadEventKind kind = UploadEventKind::Abort;
    uint8_t slot = 0;
    uint16_t length = 0;
    uint64_t request_id = 0;
    size_t content_length = 0;
    std::array<char, portal::kMaximumAlbumTitleBytes + 1U> title{};
  };

  struct ActiveUpload {
    uint64_t request_id = 0;
    size_t expected = 0;
    size_t received = 0;
    std::array<uint8_t, 24> png_header{};
    size_t header_bytes = 0;
    std::string title;
    bool failed = false;
  };

  struct PreviewHandle {
    uint64_t handle = 0;
    std::FILE* file = nullptr;
    size_t remaining = 0;
  };

  class StorageHttpGuard {
   public:
    StorageHttpGuard(const NativePortalOwner& owner,
                     bool allow_during_maintenance = false);
    ~StorageHttpGuard();
    bool active() const { return active_; }

   private:
    const NativePortalOwner& owner_;
    bool active_ = false;
  };

  static uint32_t nowMs();
  static std::string cursorFor(size_t ordinal);
  static bool parseCursor(const std::string& cursor, size_t& ordinal);
  static portal::MyAiPortalState portalMyAiState(
      myai::ActivationState state, bool authorization_verified);
  static std::string bootToken(size_t bytes);
  static bool copyTitle(const std::string& title,
                        std::array<char,
                                   portal::kMaximumAlbumTitleBytes + 1U>& out);

  void noteAccess() const;
  bool takeCommand(portal::PortalCommand& command);
  portal::PortalResult enqueueUploadEvent(const UploadEvent& event);
  bool takeUploadEvent(UploadEvent& event);
  void releaseUploadSlot(uint8_t slot);
  void serviceCommand(const portal::PortalCommand& command);
  void serviceUpload(const UploadEvent& event);
  void failActiveUpload();
  void finishActiveUpload();
  void refreshState();
  void refreshAlbum();
  bool tryRefreshAlbum();
  void refreshChat();
  esp_err_t startServer();
  esp_err_t stopServer();
  bool loadSettings();
  void applySettings(const portal::PortalSettingsPatch& patch);
  AdmissionResult requestDisplay(size_t ordinal);
  uint64_t nextRequestId();
  bool beginStorageHttpOperation(bool allow_during_maintenance) const;
  void endStorageHttpOperation() const;

  IBoardAdapter& board_;
  RuntimeSupervisor& supervisor_;
  EspWifiStationOwner& wifi_;
  EspStatusLedOwner& leds_;
  NativeDisplayService& display_;
  NativeVoiceService& voice_;
  const char* storage_root_;
  storage::PosixAtomicAlbumStore* album_store_;
  IPortalSettingsOwner* settings_owner_ = nullptr;
  IPortalAlbumMutationOwner* album_mutation_owner_ = nullptr;
  IPortalFirmwareUpdateOwner* firmware_update_owner_ = nullptr;

  mutable StaticSemaphore_t cache_mutex_storage_{};
  mutable SemaphoreHandle_t cache_mutex_ = nullptr;
  StaticSemaphore_t command_mutex_storage_{};
  SemaphoreHandle_t command_mutex_ = nullptr;
  StaticSemaphore_t upload_mutex_storage_{};
  SemaphoreHandle_t upload_mutex_ = nullptr;
  StaticQueue_t command_queue_storage_{};
  alignas(uint8_t)
  std::array<uint8_t, kCommandSlots> command_queue_bytes_{};
  QueueHandle_t command_queue_ = nullptr;
  StaticQueue_t upload_queue_storage_{};
  alignas(UploadEvent)
  std::array<uint8_t, sizeof(UploadEvent) * kUploadEventDepth>
      upload_queue_bytes_{};
  QueueHandle_t upload_queue_ = nullptr;

  std::array<portal::PortalCommand, kCommandSlots> commands_{};
  std::array<bool, kCommandSlots> command_used_{};
  uint8_t* upload_pool_ = nullptr;
  std::array<bool, kUploadSlots> upload_slot_used_{};
  uint64_t accepted_upload_request_ = 0;
  uint64_t abort_requested_id_ = 0;
  ActiveUpload active_upload_{};
  std::array<PreviewHandle, kPreviewHandles> preview_handles_{};

  portal::PortalStateSnapshot state_cache_{};
  storage::AlbumIndex album_cache_{};
  portal::ChatPage chat_cache_{};
  bool state_cache_ready_ = false;
  bool album_cache_ready_ = false;
  bool chat_cache_ready_ = false;
  uint64_t album_revision_ = 0;
  uint64_t preview_sequence_ = 0;
  uint64_t request_sequence_ = 0;
  mutable portMUX_TYPE activity_mux_ = portMUX_INITIALIZER_UNLOCKED;
  mutable uint32_t last_access_ms_ = 0;
  mutable uint32_t storage_http_operations_ = 0;
  bool storage_maintenance_active_ = false;
  bool storage_available_ = true;
  bool restart_refresh_required_ = false;
  std::atomic<uint32_t> album_refresh_generation_{0U};
  uint32_t album_refresh_applied_generation_ = 0U;
  uint32_t next_state_refresh_ms_ = 0;
  uint32_t next_album_refresh_ms_ = 0;
  uint32_t next_chat_refresh_ms_ = 0;
  uint32_t next_runtime_summary_ms_ = 0;
  bool settings_ready_ = false;
  bool initialized_ = false;
  bool mdns_started_ = false;
  // DNS-SD instance text must remain valid for the lifetime of the service.
  // 63 visible bytes is the mDNS label limit; the final byte is the NUL.
  std::array<char, 64> mdns_instance_name_{};
  std::unique_ptr<portal::PortalCore> core_;
  std::unique_ptr<portal::EspPortalServer> server_;
};

}  // namespace inkloop
