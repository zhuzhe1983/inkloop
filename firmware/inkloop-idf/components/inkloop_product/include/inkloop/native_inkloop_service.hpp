#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <memory>

#include "inkloop/cloud/esp_frame_downloader.hpp"
#include "inkloop/cloud/esp_nvs_identity_store.hpp"
#include "inkloop/board.hpp"
#include "inkloop/inkloop_cloud_client.hpp"
#include "inkloop/myai/esp_http_adapters.hpp"
#include "inkloop/native_display_service.hpp"
#include "inkloop/native_voice_service.hpp"
#include "inkloop/runtime_supervisor.hpp"
#include "inkloop/storage/posix_atomic_album_store.hpp"
#include "inkloop/storage/posix_task_store.hpp"

namespace inkloop {

struct NativeInkloopDiagnostics {
  uint32_t registrations = 0;
  uint32_t registration_failures = 0;
  uint32_t syncs = 0;
  uint32_t sync_failures = 0;
  uint32_t task_downloads = 0;
  uint32_t task_download_failures = 0;
  uint32_t display_admission_failures = 0;
  uint32_t display_failures = 0;
  uint32_t display_result_timeouts = 0;
  uint32_t acknowledgements = 0;
  uint32_t acknowledgement_failures = 0;
  uint32_t pruned_assets = 0;
  bool paired = false;
  bool display_pending = false;
};

// Native scheduled-content owner. All cloud, task-store and album mutations
// run only from the low-priority Portal lane. The high-priority Control lane
// copies display completion into a bounded POD mailbox and never performs
// network or filesystem I/O.
class NativeInkloopService final {
 public:
  NativeInkloopService(RuntimeSupervisor& supervisor,
                       const BoardDescriptor& board, const char* task_root,
                       storage::PosixAtomicAlbumStore* album_store,
                       NativeDisplayService& display);

  esp_err_t initialize();
  // Supervisor must be stopped first. Aborts any interrupted album
  // transaction and releases the sole cloud/task-journal writer objects.
  void shutdown();
  // The composition root acquires this gate before removable-storage
  // maintenance. Admission is atomic with respect to Portal-owned cloud,
  // task-journal and album work; success also proves that no scheduled
  // display acknowledgement is still waiting to touch the task journal.
  bool beginStorageMaintenance();
  // `selected_album_available` is false only when this boot selected the TF
  // album and formatting left that mount unusable. Cloud/task execution then
  // stays fail-closed until reboot while the rest of the product may recover.
  void endStorageMaintenance(bool selected_album_available);
  // Coalesces a wake/configuration hint into the existing Portal-owned sync
  // lane. It never performs network, filesystem or display work on the
  // caller's lane; the next online Portal tick consumes the request.
  bool requestImmediateSync();
  void portalTick(bool wifi_online, bool slow_io_allowed,
                  bool scheduled_display_allowed,
                  const NativeMyAiOnboardingSnapshot& onboarding);
  bool handleControlResult(const WorkEnvelope& envelope);
  // Portal-lane-only read through the sole task-store owner. Power policy uses
  // this instead of opening a second journal instance against the same files.
  storage::TaskStoreCode nextTaskEpoch(std::time_t now,
                                       uint64_t& epoch_seconds) const;
  bool busy() const;
  NativeInkloopDiagnostics diagnostics() const;

 private:
  enum class DisplayMailboxPhase : uint8_t {
    Idle,
    AwaitingResult,
    AcknowledgementReady,
  };

  struct DisplayMailbox {
    std::array<char, 101> task_id{};
    uint64_t request_id = 0;
    uint32_t revision = 0;
    uint32_t run_at = 0;
    uint32_t local_day = 0;
    uint32_t result_deadline_ms = 0;
    DisplayMailboxPhase phase = DisplayMailboxPhase::Idle;
  };

  void synchronize(const NativeMyAiOnboardingSnapshot& onboarding,
                   uint32_t now_ms);
  void portalTickAdmitted(bool wifi_online, bool slow_io_allowed,
                          bool scheduled_display_allowed,
                          const NativeMyAiOnboardingSnapshot& onboarding);
  bool admittedSlowIoBusy() const;
  bool reconcileTaskAssets();
  void runDueTask(uint32_t now_ms, bool download_allowed);
  bool queueDisplay(const storage::InkloopTaskRecord& task, size_t ordinal,
                    uint32_t now_ms, std::time_t now,
                    const std::tm& local);
  void drainAcknowledgement(uint32_t now_ms);
  void expireDisplayResult(uint32_t now_ms);
  void scheduleSync(uint32_t now_ms, uint32_t requested_delay_ms = 0);
  void scheduleTaskRetry(uint32_t now_ms);
  uint64_t nextRequestId();
  static uint32_t nowMs();
  static bool due(uint32_t now_ms, uint32_t deadline_ms);
  static bool sixDigits(const char* value);

  RuntimeSupervisor& supervisor_;
  const BoardDescriptor& board_;
  const char* task_root_;
  storage::PosixAtomicAlbumStore* album_store_;
  NativeDisplayService& display_;
  myai::EspEndpointSecurity endpoint_security_{};
  myai::EspHttpTransport http_{endpoint_security_};
  cloud::EspNvsInkloopIdentityStore identity_store_{};
  cloud::EspInkloopFrameDownloader downloader_{endpoint_security_};
  std::unique_ptr<storage::PosixTaskStore> task_store_;
  std::unique_ptr<cloud::InkloopCloudClient> client_;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  NativeInkloopDiagnostics diagnostics_{};
  DisplayMailbox display_mailbox_{};
  uint64_t sequence_ = 0;
  uint32_t next_sync_ms_ = 0;
  uint32_t next_task_attempt_ms_ = 0;
  uint32_t next_ack_attempt_ms_ = 0;
  bool registration_required_ = false;
  bool tasks_synchronized_ = false;
  bool storage_maintenance_ = false;
  bool portal_operation_active_ = false;
  bool selected_album_available_ = true;
  bool initialized_ = false;
};

}  // namespace inkloop
