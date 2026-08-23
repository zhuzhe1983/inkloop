#include "inkloop/native_inkloop_service.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <new>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "inkloop/product_opcodes.hpp"
#include "inkloop/slow_io_arbitration.hpp"

namespace inkloop {
namespace {

constexpr char kTag[] = "ink-native-cloud";
constexpr uint32_t kSyncIntervalMs = 30000U;
constexpr uint32_t kTaskRetryMs = 30000U;
constexpr uint32_t kMinimumRetryMs = 1000U;
constexpr uint32_t kDisplayResultTimeoutMs = 120000U;
constexpr std::time_t kMinimumValidEpoch = 1700000000;
constexpr uint64_t kRequestNamespace = 0x494e4b0000000000ULL;

uint32_t boundedRetry(uint32_t requested) {
  return std::max<uint32_t>(
      kMinimumRetryMs,
      std::min<uint32_t>(requested == 0 ? kSyncIntervalMs : requested,
                         kSyncIntervalMs));
}

}  // namespace

NativeInkloopService::NativeInkloopService(
    RuntimeSupervisor& supervisor, const char* task_root,
    storage::PosixAtomicAlbumStore* album_store,
    NativeDisplayService& display)
    : supervisor_(supervisor), task_root_(task_root), album_store_(album_store),
      display_(display) {}

uint32_t NativeInkloopService::nowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

bool NativeInkloopService::due(uint32_t now_ms, uint32_t deadline_ms) {
  return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

bool NativeInkloopService::sixDigits(const char* value) {
  if (!value) return false;
  for (size_t at = 0; at < 6U; ++at) {
    if (value[at] < '0' || value[at] > '9') return false;
  }
  return value[6] == '\0';
}

uint64_t NativeInkloopService::nextRequestId() {
  portENTER_CRITICAL(&mux_);
  do {
    sequence_ = (sequence_ + 1U) & 0x000000ffffffffffULL;
  } while (sequence_ == 0);
  const uint64_t value = kRequestNamespace | sequence_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

esp_err_t NativeInkloopService::initialize() {
  if (initialized_ || !task_root_ || task_root_[0] != '/' || !album_store_ ||
      !album_store_->pathsValid()) {
    return ESP_ERR_INVALID_STATE;
  }
  task_store_.reset(
      new (std::nothrow) storage::PosixTaskStore(std::string(task_root_)));
  if (!task_store_ || !task_store_->pathsValid()) return ESP_ERR_NO_MEM;
  client_.reset(new (std::nothrow) cloud::InkloopCloudClient(
      cloud::InkloopCloudConfig{}, http_, identity_store_, *task_store_));
  if (!client_) return ESP_ERR_NO_MEM;
  const cloud::InkloopCloudStatus status = client_->initialize();
  if (!status.ok()) {
    ESP_LOGE(kTag, "initialize failed code=%s detail=%s",
             cloud::InkloopCloudClient::codeName(status.code),
             status.detail.c_str());
    return ESP_FAIL;
  }
  registration_required_ = client_->identity().device_id.empty();
  // A valid local journal remains authoritative while temporarily offline.
  // The first online sync still runs immediately before any online download.
  tasks_synchronized_ = !registration_required_;
  next_sync_ms_ = nowMs();
  next_task_attempt_ms_ = next_sync_ms_;
  initialized_ = true;
  return ESP_OK;
}

void NativeInkloopService::shutdown() {
  if (album_store_) album_store_->abort();
  client_.reset();
  task_store_.reset();
  portENTER_CRITICAL(&mux_);
  display_mailbox_ = DisplayMailbox{};
  sequence_ = 0U;
  next_sync_ms_ = 0U;
  next_task_attempt_ms_ = 0U;
  next_ack_attempt_ms_ = 0U;
  registration_required_ = false;
  tasks_synchronized_ = false;
  storage_maintenance_ = false;
  portal_operation_active_ = false;
  selected_album_available_ = true;
  initialized_ = false;
  portEXIT_CRITICAL(&mux_);
}

bool NativeInkloopService::beginStorageMaintenance() {
  portENTER_CRITICAL(&mux_);
  const bool admitted = initialized_ && !storage_maintenance_ &&
      !portal_operation_active_ &&
      display_mailbox_.phase == DisplayMailboxPhase::Idle;
  if (admitted) storage_maintenance_ = true;
  portEXIT_CRITICAL(&mux_);
  return admitted;
}

void NativeInkloopService::endStorageMaintenance(
    bool selected_album_available) {
  const uint32_t now = nowMs();
  portENTER_CRITICAL(&mux_);
  if (storage_maintenance_) {
    selected_album_available_ = selected_album_available;
    // A successfully formatted selected album is empty. Make the next due
    // task eligible immediately so its frame can be fetched again; the task
    // journal itself lives on internal storage and remains authoritative.
    next_task_attempt_ms_ = now;
    storage_maintenance_ = false;
  }
  portEXIT_CRITICAL(&mux_);
}

bool NativeInkloopService::requestImmediateSync() {
  const uint32_t now = nowMs();
  portENTER_CRITICAL(&mux_);
  const bool admitted = initialized_;
  if (admitted) next_sync_ms_ = now;
  portEXIT_CRITICAL(&mux_);
  return admitted;
}

void NativeInkloopService::scheduleSync(uint32_t now_ms,
                                        uint32_t requested_delay_ms) {
  next_sync_ms_ = now_ms + boundedRetry(requested_delay_ms);
}

void NativeInkloopService::scheduleTaskRetry(uint32_t now_ms) {
  portENTER_CRITICAL(&mux_);
  next_task_attempt_ms_ = now_ms + kTaskRetryMs;
  portEXIT_CRITICAL(&mux_);
}

void NativeInkloopService::synchronize(
    const NativeMyAiOnboardingSnapshot& onboarding, uint32_t now_ms) {
  if (!client_ || !task_store_) return;
  if (registration_required_) {
    if (!sixDigits(onboarding.device_code.data())) {
      scheduleSync(now_ms);
      return;
    }
    cloud::InkloopRegistrationResult registration;
    const cloud::InkloopCloudStatus status = client_->registerDevice(
        std::string(onboarding.device_code.data()), registration);
    portENTER_CRITICAL(&mux_);
    if (status.ok()) {
      ++diagnostics_.registrations;
      diagnostics_.paired = registration.paired;
    } else {
      ++diagnostics_.registration_failures;
    }
    portEXIT_CRITICAL(&mux_);
    if (!status.ok()) {
      ESP_LOGW(kTag, "registration failed code=%s http=%d detail=%s",
               cloud::InkloopCloudClient::codeName(status.code),
               status.http_status, status.detail.c_str());
      scheduleSync(now_ms, status.retry_after_ms);
      return;
    }
    registration_required_ = !registration.paired;
    tasks_synchronized_ = false;
    if (registration.paired)
      next_sync_ms_ = now_ms;
    else
      scheduleSync(now_ms);
    return;
  }

  cloud::InkloopSyncResult result;
  const cloud::InkloopCloudStatus status = client_->syncTasks(result);
  portENTER_CRITICAL(&mux_);
  if (status.ok()) {
    ++diagnostics_.syncs;
    diagnostics_.paired = result.paired;
  } else {
    ++diagnostics_.sync_failures;
  }
  portEXIT_CRITICAL(&mux_);
  if (!status.ok()) {
    ESP_LOGW(kTag, "sync failed code=%s http=%d detail=%s",
             cloud::InkloopCloudClient::codeName(status.code),
             status.http_status, status.detail.c_str());
    // A changed response can replace the task journal before persisting its
    // applied revision. Keep the execution gate closed until a later online
    // sync has reconciled the album; pure transport failures keep the already
    // reconciled offline cache usable.
    if (result.changed) {
      tasks_synchronized_ = false;
    }
    if (status.code == cloud::InkloopCloudCode::Unauthorized ||
        status.code == cloud::InkloopCloudCode::PairingRequired) {
      registration_required_ = true;
      tasks_synchronized_ = false;
    }
    scheduleSync(now_ms, result.changed ? kMinimumRetryMs
                                        : status.retry_after_ms);
    return;
  }
  if (!result.paired || result.requires_registration) {
    registration_required_ = true;
    tasks_synchronized_ = false;
    scheduleSync(now_ms);
    return;
  }
  if (!reconcileTaskAssets()) {
    tasks_synchronized_ = false;
    scheduleTaskRetry(now_ms);
    scheduleSync(now_ms, kMinimumRetryMs);
    return;
  }
  tasks_synchronized_ = true;
  scheduleSync(now_ms);
}

bool NativeInkloopService::reconcileTaskAssets() {
  if (!task_store_ || !album_store_) return false;
  std::vector<storage::InkloopTaskRecord> tasks;
  if (task_store_->load(tasks) != storage::TaskStoreCode::Ok) return false;
  std::vector<storage::AlbumTaskBinding> retained;
  retained.reserve(tasks.size());
  for (const storage::InkloopTaskRecord& task : tasks) {
    retained.push_back(
        storage::AlbumTaskBinding{task.id, task.frame_hash,
                                  task.render_strategy});
  }
  size_t removed = 0;
  const myai::Status pruned = album_store_->pruneTaskAssets(retained, removed);
  if (!pruned.ok()) return false;
  if (removed > 0U) {
    portENTER_CRITICAL(&mux_);
    diagnostics_.pruned_assets += static_cast<uint32_t>(removed);
    portEXIT_CRITICAL(&mux_);
  }
  // A strategy-only change or deletion must be visible to the next physical
  // button press even when no scheduled refresh is due.
  return display_.reloadCatalog();
}

bool NativeInkloopService::queueDisplay(
    const storage::InkloopTaskRecord& task, size_t ordinal, uint32_t now_ms,
    std::time_t now, const std::tm& local) {
  if (ordinal > 255U || task.id.size() >= DisplayMailbox{}.task_id.size() ||
      now < 0 || static_cast<uint64_t>(now) > 0xffffffffULL) {
    return false;
  }
  DisplayMailbox pending;
  std::memcpy(pending.task_id.data(), task.id.data(), task.id.size());
  pending.task_id[task.id.size()] = '\0';
  pending.request_id = nextRequestId();
  pending.revision = task.revision;
  pending.run_at = static_cast<uint32_t>(now);
  pending.local_day = storage::PosixTaskStore::localDayStamp(local);
  pending.result_deadline_ms = now_ms + kDisplayResultTimeoutMs;
  pending.phase = DisplayMailboxPhase::AwaitingResult;

  portENTER_CRITICAL(&mux_);
  if (display_mailbox_.phase != DisplayMailboxPhase::Idle) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  display_mailbox_ = pending;
  diagnostics_.display_pending = true;
  portEXIT_CRITICAL(&mux_);

  WorkEnvelope command{};
  command.generation = 1;
  command.request_id = pending.request_id;
  command.opcode = productOpcode(ProductOpcode::DisplayAlbumOrdinal);
  command.work_class = WorkClass::Display;
  command.kind = EnvelopeKind::Command;
  command.disposition = WorkDisposition::Accepted;
  command.flags = static_cast<uint8_t>(ordinal);
  command.deadline_ms = pending.result_deadline_ms;
  if (supervisor_.post(command) == AdmissionResult::Admitted) return true;

  portENTER_CRITICAL(&mux_);
  if (display_mailbox_.request_id == pending.request_id) {
    display_mailbox_ = DisplayMailbox{};
    diagnostics_.display_pending = false;
  }
  ++diagnostics_.display_admission_failures;
  portEXIT_CRITICAL(&mux_);
  scheduleTaskRetry(now_ms);
  return false;
}

void NativeInkloopService::runDueTask(uint32_t now_ms,
                                      bool download_allowed) {
  if (!task_store_ || !client_ || !album_store_) return;
  portENTER_CRITICAL(&mux_);
  const bool may_attempt = display_mailbox_.phase == DisplayMailboxPhase::Idle &&
      due(now_ms, next_task_attempt_ms_);
  portEXIT_CRITICAL(&mux_);
  if (!may_attempt) return;

  const std::time_t now = std::time(nullptr);
  if (now < kMinimumValidEpoch || now > static_cast<std::time_t>(0xffffffffULL))
    return;
  std::tm local{};
  if (!localtime_r(&now, &local)) return;
  storage::InkloopTaskRecord task;
  const storage::TaskStoreCode due_task = task_store_->firstDue(now, local, task);
  if (due_task != storage::TaskStoreCode::Ok) {
    scheduleTaskRetry(now_ms);
    return;
  }
  if (task.id.empty()) {
    scheduleTaskRetry(now_ms);
    return;
  }

  storage::AlbumIndex catalog;
  if (!album_store_->readCatalog(catalog).ok()) {
    scheduleTaskRetry(now_ms);
    return;
  }
  size_t ordinal = storage::kMaximumAlbumEntries;
  for (size_t at = 0; at < catalog.assets.size(); ++at) {
    const storage::AlbumIndexAsset& asset = catalog.assets[at];
    if (asset.content_sha256 == task.frame_hash && asset.task_id == task.id) {
      ordinal = at;
      break;
    }
  }

  if (ordinal == storage::kMaximumAlbumEntries) {
    if (!download_allowed) {
      scheduleTaskRetry(now_ms);
      return;
    }
    storage::AlbumCommitResult committed;
    const cloud::InkloopCloudStatus downloaded = downloader_.download(
        client_->identity(), task, *album_store_, committed);
    portENTER_CRITICAL(&mux_);
    if (downloaded.ok())
      ++diagnostics_.task_downloads;
    else
      ++diagnostics_.task_download_failures;
    portEXIT_CRITICAL(&mux_);
    if (!downloaded.ok()) {
      ESP_LOGW(kTag, "frame failed task=%s code=%s http=%d detail=%s",
               task.id.c_str(),
               cloud::InkloopCloudClient::codeName(downloaded.code),
               downloaded.http_status, downloaded.detail.c_str());
      scheduleTaskRetry(now_ms);
      return;
    }
    ordinal = committed.ordinal;
    if (!display_.reloadCatalog()) {
      scheduleTaskRetry(now_ms);
      return;
    }
  }
  queueDisplay(task, ordinal, now_ms, now, local);
}

bool NativeInkloopService::handleControlResult(
    const WorkEnvelope& envelope) {
  if (envelope.kind != EnvelopeKind::Result ||
      envelope.work_class != WorkClass::Display ||
      envelope.opcode != productOpcode(ProductOpcode::DisplayAlbumOrdinal)) {
    return false;
  }
  bool matched = false;
  portENTER_CRITICAL(&mux_);
  if (display_mailbox_.phase == DisplayMailboxPhase::AwaitingResult &&
      display_mailbox_.request_id == envelope.request_id) {
    matched = true;
    if (envelope.disposition == WorkDisposition::Complete) {
      display_mailbox_.phase = DisplayMailboxPhase::AcknowledgementReady;
      next_ack_attempt_ms_ = nowMs();
    } else {
      display_mailbox_ = DisplayMailbox{};
      diagnostics_.display_pending = false;
      ++diagnostics_.display_failures;
      next_task_attempt_ms_ = nowMs() + kTaskRetryMs;
    }
  }
  portEXIT_CRITICAL(&mux_);
  return matched;
}

void NativeInkloopService::drainAcknowledgement(uint32_t now_ms) {
  DisplayMailbox acknowledgement;
  portENTER_CRITICAL(&mux_);
  const bool ready = display_mailbox_.phase ==
          DisplayMailboxPhase::AcknowledgementReady &&
      due(now_ms, next_ack_attempt_ms_);
  if (ready) acknowledgement = display_mailbox_;
  portEXIT_CRITICAL(&mux_);
  if (!ready || !task_store_) return;

  const storage::TaskStoreCode marked = task_store_->markRun(
      acknowledgement.task_id.data(), acknowledgement.revision,
      acknowledgement.run_at, acknowledgement.local_day);
  portENTER_CRITICAL(&mux_);
  if (display_mailbox_.phase == DisplayMailboxPhase::AcknowledgementReady &&
      display_mailbox_.request_id == acknowledgement.request_id) {
    if (marked == storage::TaskStoreCode::Ok ||
        marked == storage::TaskStoreCode::InvalidRecord) {
      display_mailbox_ = DisplayMailbox{};
      diagnostics_.display_pending = false;
      if (marked == storage::TaskStoreCode::Ok)
        ++diagnostics_.acknowledgements;
      else
        ++diagnostics_.acknowledgement_failures;
      next_task_attempt_ms_ = now_ms;
    } else {
      ++diagnostics_.acknowledgement_failures;
      next_ack_attempt_ms_ = now_ms + kTaskRetryMs;
    }
  }
  portEXIT_CRITICAL(&mux_);
}

void NativeInkloopService::expireDisplayResult(uint32_t now_ms) {
  portENTER_CRITICAL(&mux_);
  if (display_mailbox_.phase == DisplayMailboxPhase::AwaitingResult &&
      due(now_ms, display_mailbox_.result_deadline_ms)) {
    display_mailbox_ = DisplayMailbox{};
    diagnostics_.display_pending = false;
    ++diagnostics_.display_failures;
    ++diagnostics_.display_result_timeouts;
    next_task_attempt_ms_ = now_ms + kTaskRetryMs;
  }
  portEXIT_CRITICAL(&mux_);
}

void NativeInkloopService::portalTick(
    bool wifi_online, bool slow_io_allowed, bool scheduled_display_allowed,
    const NativeMyAiOnboardingSnapshot& onboarding) {
  portENTER_CRITICAL(&mux_);
  if (!initialized_ || storage_maintenance_ || portal_operation_active_ ||
      !selected_album_available_) {
    portEXIT_CRITICAL(&mux_);
    return;
  }
  portal_operation_active_ = true;
  portEXIT_CRITICAL(&mux_);
  portalTickAdmitted(wifi_online, slow_io_allowed, scheduled_display_allowed,
                     onboarding);
  portENTER_CRITICAL(&mux_);
  portal_operation_active_ = false;
  portEXIT_CRITICAL(&mux_);
}

void NativeInkloopService::portalTickAdmitted(
    bool wifi_online, bool slow_io_allowed, bool scheduled_display_allowed,
    const NativeMyAiOnboardingSnapshot& onboarding) {
  const uint32_t now = nowMs();
  expireDisplayResult(now);
  drainAcknowledgement(now);
  // portalTick() has already acquired this owner's Portal-lane lease, so the
  // public busy() view is necessarily true here.  Only competing storage or
  // Display work can block the admitted owner itself.
  if (!slow_io_allowed || admittedSlowIoBusy()) return;
  if (wifi_online && due(now, next_sync_ms_)) synchronize(onboarding, now);
  if (scheduled_display_allowed && !registration_required_ &&
      tasks_synchronized_)
    runDueTask(now, wifi_online);
}

bool NativeInkloopService::admittedSlowIoBusy() const {
  portENTER_CRITICAL(&mux_);
  const bool value = SlowIoArbitration::inkloopOwnerAdmittedBusy(
      storage_maintenance_,
      display_mailbox_.phase != DisplayMailboxPhase::Idle);
  portEXIT_CRITICAL(&mux_);
  return value;
}

bool NativeInkloopService::busy() const {
  portENTER_CRITICAL(&mux_);
  const bool value = SlowIoArbitration::inkloopOwnerBusy(
      storage_maintenance_, portal_operation_active_,
      display_mailbox_.phase != DisplayMailboxPhase::Idle);
  portEXIT_CRITICAL(&mux_);
  return value;
}

storage::TaskStoreCode NativeInkloopService::nextTaskEpoch(
    std::time_t now, uint64_t& epoch_seconds) const {
  epoch_seconds = 0U;
  if (!initialized_ || !task_store_)
    return storage::TaskStoreCode::IoError;
  return task_store_->nextDueEpoch(now, epoch_seconds);
}

NativeInkloopDiagnostics NativeInkloopService::diagnostics() const {
  portENTER_CRITICAL(&mux_);
  const NativeInkloopDiagnostics value = diagnostics_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

}  // namespace inkloop
