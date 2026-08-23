#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "inkloop/album_navigation_core.hpp"
#include "inkloop/board.hpp"
#include "inkloop/diagnostics/serial_diagnostic_events.hpp"
#include "inkloop/runtime_supervisor.hpp"
#include "inkloop/storage/posix_atomic_album_store.hpp"

namespace inkloop {

struct NativeDisplayDiagnostics {
  uint32_t decode_failures = 0;
  uint32_t render_failures = 0;
  uint32_t panel_failures = 0;
  uint32_t persistence_failures = 0;
  uint32_t unchanged_skips = 0;
  uint32_t onboarding_failures = 0;
  uint32_t onboarding_unchanged_skips = 0;
  uint32_t completed_album_refreshes = 0;
  uint32_t panel_writes = 0;
  uint32_t last_load_decode_ms = 0;
  uint32_t maximum_load_decode_ms = 0;
  uint32_t last_conversion_ms = 0;
  uint32_t maximum_conversion_ms = 0;
  uint32_t last_panel_refresh_ms = 0;
  uint32_t maximum_panel_refresh_ms = 0;
  uint32_t last_album_total_ms = 0;
  uint32_t maximum_album_total_ms = 0;
};

struct NativeProvisioningPageRequest {
  std::string_view ssid;
  std::string_view access_value;
  std::string_view local_host;
  std::string_view local_ip;
};

struct NativeMyAiPairingPageRequest {
  std::string_view six_digit_code;
  std::string_view binding_url;
};

enum class NativeDisplayPageRequestResult : uint8_t {
  Accepted,
  AlreadyPending,
  Unchanged,
  NotReady,
  Busy,
  InvalidInput,
};

// Sole product display owner. It normalizes a PNG into the selected board's
// logical RGB geometry, asks that board's renderer for one complete native
// 4-bpp frame, then invokes the panel exactly once. Decode/conversion never
// emits an intermediate e-paper frame.
class NativeDisplayService final {
 public:
  NativeDisplayService(IBoardAdapter& board, RuntimeSupervisor& supervisor,
                       storage::PosixAtomicAlbumStore* album_store,
                       diagnostics::ISerialDiagnosticEventSink*
                           serial_diagnostics = nullptr)
      : board_(board), supervisor_(supervisor), album_store_(album_store),
        serial_diagnostics_(serial_diagnostics) {}

  esp_err_t configure();
  // Supervisor must be stopped first. This removes every pending writer and
  // maintenance admission without refreshing or otherwise mutating storage.
  void shutdown();
  // Portal-owned destructive storage work must acquire this gate before it
  // unmounts or formats removable storage. Admission is atomic with respect
  // to every Display-owned catalog/render state; a successful begin promises
  // that no new Display album I/O will start until finishStorageMaintenance().
  bool beginStorageMaintenance();
  // reload_catalog must be true after any attempted destructive mutation. The
  // maintenance gate remains closed while the new catalog is read, and catalog
  // synchronization plus gate release are one atomic state transition. A
  // failed reload invalidates navigation and keeps the gate closed for retry.
  // false is only for an operation aborted before storage could change.
  bool finishStorageMaintenance(bool reload_catalog);
  // Re-reads only the small atomic album index. It never refreshes the panel
  // and is used after Portal-owned downloads, uploads, strategy edits or
  // server deletion propagation.
  bool reloadCatalog();
  // These APIs only validate and copy into one bounded mailbox. The caller's
  // lane never allocates a frame or writes the panel; service() consumes the
  // request on the Display lane and either commits one complete native frame
  // or leaves the existing e-paper contents untouched.
  NativeDisplayPageRequestResult requestProvisioningPage(
      const NativeProvisioningPageRequest& request);
  NativeDisplayPageRequestResult requestMyAiPairingPage(
      const NativeMyAiPairingPageRequest& request);
  // Drops a not-yet-rendered onboarding mailbox or schedules the Display lane
  // to replace a visible onboarding page with the persisted current album
  // page (falling back to ordinal zero). The request carries no page secrets.
  NativeDisplayPageRequestResult requestAlbumRestore();
  AlbumStepResult selectRelative(int direction, size_t& ordinal);
  bool busy() const;
  bool refreshing() const;
  NativeDisplayDiagnostics diagnostics() const;

 private:
  static WorkDisposition handler(const WorkEnvelope& envelope, void* context);
  static void tick(void* context);
  WorkDisposition handle(const WorkEnvelope& envelope);
  void service();
  bool synchronizeCatalog();
  bool renderOrdinal(size_t ordinal);
  bool renderOrdinalAdmitted(size_t ordinal);
  bool renderOnboardingPage();
  bool writePanelFrame(const uint8_t* frame, size_t frame_bytes);
  AdmissionResult postRefreshStarting(size_t ordinal);
  AdmissionResult postImageLed(uint8_t mode);
  void emitSerialAigcPhase(
      diagnostics::SerialDiagnosticAigcPhase phase) const;
  void emitSerialAigcError() const;
  uint64_t nextRequestId();
  static uint32_t nowMs();

  enum class OnboardingPageKind : uint8_t {
    None,
    Provisioning,
    MyAiPairing,
  };

  struct PageFingerprint {
    uint64_t first = 0;
    uint64_t second = 0;

    bool operator==(const PageFingerprint& other) const {
      return first == other.first && second == other.second;
    }
  };

  struct OnboardingMailbox {
    OnboardingPageKind kind = OnboardingPageKind::None;
    PageFingerprint fingerprint{};
    std::array<char, 33> ssid{};
    std::array<char, 64> access_value{};
    std::array<char, 65> local_host{};
    std::array<char, 65> local_ip{};
    std::array<char, 7> six_digit_code{};
    std::array<char, 257> binding_url{};
  };

  static PageFingerprint fingerprint(
      OnboardingPageKind kind, std::string_view first,
      std::string_view second, std::string_view third = {},
      std::string_view fourth = {});
  static void clearMailbox(OnboardingMailbox& mailbox);

  IBoardAdapter& board_;
  RuntimeSupervisor& supervisor_;
  storage::PosixAtomicAlbumStore* album_store_;
  diagnostics::ISerialDiagnosticEventSink* serial_diagnostics_ = nullptr;
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  NativeDisplayDiagnostics diagnostics_{};
  AlbumNavigationCore navigation_{};
  OnboardingMailbox onboarding_mailbox_{};
  PageFingerprint visible_onboarding_fingerprint_{};
  uint64_t sequence_ = 0;
  bool configured_ = false;
  bool onboarding_pending_ = false;
  bool onboarding_rendering_ = false;
  bool onboarding_visible_ = false;
  bool onboarding_replacement_pending_ = false;
  bool album_restore_pending_ = false;
  bool catalog_known_empty_ = false;
  bool catalog_refreshing_ = false;
  bool album_rendering_ = false;
  bool storage_maintenance_ = false;
};

}  // namespace inkloop
