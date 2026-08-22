#include "inkloop/native_display_service.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "inkloop/onboarding/pairing_frame.hpp"
#include "inkloop/product_opcodes.hpp"
#include "inkloop/status_led_core.hpp"
#include "pngle.h"

namespace inkloop {
namespace {

constexpr char kTag[] = "ink-display";
constexpr size_t kPngFeedBytes = 4096U;
constexpr uint32_t kLedDeadlineMs = 250;
constexpr uint32_t kDisplayTickMs = 50;

bool printable(std::string_view value, size_t minimum, size_t maximum) {
  if (value.size() < minimum || value.size() > maximum) return false;
  for (const unsigned char character : value) {
    if (character < 0x20U || character > 0x7eU) return false;
  }
  return true;
}

template <size_t Size>
bool copyBounded(std::string_view value, std::array<char, Size>& output) {
  if (value.size() >= Size) return false;
  output.fill('\0');
  std::copy(value.begin(), value.end(), output.begin());
  return true;
}

class HeapBytes final {
 public:
  HeapBytes(size_t size, bool prefer_psram)
      : bytes_(static_cast<uint8_t*>(heap_caps_malloc(
            size, MALLOC_CAP_8BIT |
                      (prefer_psram ? MALLOC_CAP_SPIRAM
                                    : MALLOC_CAP_INTERNAL)))),
        size_(size) {}
  ~HeapBytes() { heap_caps_free(bytes_); }
  uint8_t* get() const { return bytes_; }
  size_t size() const { return size_; }
 private:
  uint8_t* bytes_ = nullptr;
  size_t size_ = 0;
};

struct DecodeContext {
  uint8_t* rgb = nullptr;
  size_t rgb_bytes = 0;
  uint32_t target_width = 0;
  uint32_t target_height = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool initialized = false;
  bool done = false;
  bool failed = false;
};

void pngInit(pngle_t* png, uint32_t width, uint32_t height) {
  auto* context = static_cast<DecodeContext*>(pngle_get_user_data(png));
  const bool canonical = context && width == context->target_width &&
      height == context->target_height;
  const bool rotated = context &&
      context->target_width != context->target_height &&
      width == context->target_height && height == context->target_width;
  if (!context || !context->rgb || (!canonical && !rotated)) {
    if (context) context->failed = true;
    return;
  }
  context->width = width;
  context->height = height;
  context->initialized = true;
}

void pngDraw(pngle_t* png, uint32_t x, uint32_t y, uint32_t width,
             uint32_t height, const uint8_t rgba[4]) {
  auto* context = static_cast<DecodeContext*>(pngle_get_user_data(png));
  if (!context || !context->initialized || context->failed || !rgba ||
      x >= context->width || y >= context->height || width == 0 ||
      height == 0 || width > context->width - x ||
      height > context->height - y) {
    if (context) context->failed = true;
    return;
  }
  const uint32_t alpha = rgba[3];
  const uint8_t red = static_cast<uint8_t>(
      (static_cast<uint32_t>(rgba[0]) * alpha + 255U * (255U - alpha) +
       127U) / 255U);
  const uint8_t green = static_cast<uint8_t>(
      (static_cast<uint32_t>(rgba[1]) * alpha + 255U * (255U - alpha) +
       127U) / 255U);
  const uint8_t blue = static_cast<uint8_t>(
      (static_cast<uint32_t>(rgba[2]) * alpha + 255U * (255U - alpha) +
       127U) / 255U);
  for (uint32_t source_y = y; source_y < y + height; ++source_y) {
    for (uint32_t source_x = x; source_x < x + width; ++source_x) {
      uint32_t target_x = source_x;
      uint32_t target_y = source_y;
      if (context->width == context->target_height &&
          context->height == context->target_width) {
        target_x = context->target_width - 1U - source_y;
        target_y = source_x;
      }
      const size_t at =
          (static_cast<size_t>(target_y) * context->target_width +
           target_x) * 3U;
      if (at > context->rgb_bytes || context->rgb_bytes - at < 3U) {
        context->failed = true;
        return;
      }
      context->rgb[at] = red;
      context->rgb[at + 1U] = green;
      context->rgb[at + 2U] = blue;
    }
  }
}

void pngDone(pngle_t* png) {
  auto* context = static_cast<DecodeContext*>(pngle_get_user_data(png));
  if (context) context->done = true;
}

bool decodePngFile(const std::string& path, size_t expected, uint8_t* rgb,
                   size_t rgb_bytes, uint16_t target_width,
                   uint16_t target_height) {
  if (path.empty() || !rgb || target_width == 0U || target_height == 0U ||
      expected < 45U || expected > storage::kMaximumAlbumAssetBytes) {
    return false;
  }
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) return false;
  struct stat status {};
  bool valid = ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
      status.st_size >= 0 && static_cast<uint64_t>(status.st_size) == expected;
  std::memset(rgb, 0xff, rgb_bytes);
  pngle_t* png = pngle_new();
  if (!png) {
    ::close(descriptor);
    return false;
  }
  DecodeContext context;
  context.rgb = rgb;
  context.rgb_bytes = rgb_bytes;
  context.target_width = target_width;
  context.target_height = target_height;
  pngle_set_user_data(png, &context);
  pngle_set_init_callback(png, &pngInit);
  pngle_set_draw_callback(png, &pngDraw);
  pngle_set_done_callback(png, &pngDone);
  HeapBytes feed(kPngFeedBytes, false);
  size_t buffered = 0U;
  size_t read_total = 0U;
  bool eof = false;
  valid = valid && feed.get();
  while (valid && (!eof || buffered != 0U)) {
    if (!eof && buffered < feed.size()) {
      const ssize_t count = ::read(
          descriptor, feed.get() + buffered, feed.size() - buffered);
      if (count < 0) {
        valid = false;
        break;
      }
      if (count == 0) {
        eof = true;
      } else {
        buffered += static_cast<size_t>(count);
        read_total += static_cast<size_t>(count);
        if (read_total > expected) valid = false;
      }
    }
    if (!valid || buffered == 0U) continue;
    const int consumed = pngle_feed(png, feed.get(), buffered);
    if (consumed < 0 || static_cast<size_t>(consumed) > buffered) {
      valid = false;
      break;
    }
    if (consumed == 0) {
      if (eof || buffered == feed.size()) valid = false;
      continue;
    }
    const size_t used = static_cast<size_t>(consumed);
    buffered -= used;
    if (buffered != 0U)
      std::memmove(feed.get(), feed.get() + used, buffered);
  }
  if (::close(descriptor) != 0) valid = false;
  valid = valid && read_total == expected &&
      context.initialized && context.done && !context.failed &&
      pngle_get_width(png) == context.width &&
      pngle_get_height(png) == context.height;
  pngle_destroy(png);
  return valid;
}

}  // namespace

uint32_t NativeDisplayService::nowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

NativeDisplayService::PageFingerprint NativeDisplayService::fingerprint(
    OnboardingPageKind kind, std::string_view first, std::string_view second,
    std::string_view third, std::string_view fourth) {
  PageFingerprint result{1469598103934665603ULL, 1099511628211ULL};
  const auto mix = [&result](uint8_t value) {
    result.first = (result.first ^ value) * 1099511628211ULL;
    result.second = (result.second ^ value) * 14029467366897019727ULL;
    result.second ^= result.second >> 29U;
  };
  mix(static_cast<uint8_t>(kind));
  const std::array<std::string_view, 4> fields{first, second, third, fourth};
  for (const std::string_view field : fields) {
    uint64_t length = field.size();
    for (size_t at = 0; at < sizeof(length); ++at) {
      mix(static_cast<uint8_t>(length & 0xffU));
      length >>= 8U;
    }
    for (const unsigned char character : field) mix(character);
  }
  return result;
}

void NativeDisplayService::clearMailbox(OnboardingMailbox& mailbox) {
  // The Settings access value and MyAI URL are short-lived display inputs.
  // Erase the complete trivially-copyable mailbox after the Display lane has
  // consumed it; diagnostics and deduplication retain only a fingerprint.
  volatile uint8_t* bytes = reinterpret_cast<volatile uint8_t*>(&mailbox);
  for (size_t at = 0; at < sizeof(mailbox); ++at) bytes[at] = 0;
  mailbox = OnboardingMailbox{};
}

uint64_t NativeDisplayService::nextRequestId() {
  portENTER_CRITICAL(&mux_);
  const uint64_t value = ++sequence_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

esp_err_t NativeDisplayService::configure() {
  const BoardDescriptor& descriptor = board_.descriptor();
  const size_t pixels =
      static_cast<size_t>(descriptor.width) * descriptor.height;
  if (configured_ || !album_store_ || !board_.display() ||
      !board_.renderer() || descriptor.width == 0U ||
      descriptor.height == 0U || descriptor.palette_colors == 0U ||
      descriptor.palette_colors > 16U || pixels == 0U ||
      pixels > storage::kMaximumAlbumAssetBytes) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!synchronizeCatalog()) return ESP_FAIL;
  esp_err_t status = supervisor_.registerHandler(
      TaskLane::Display, &NativeDisplayService::handler, this);
  if (status == ESP_OK) {
    status = supervisor_.registerTickHandler(
        TaskLane::Display, &NativeDisplayService::tick, this, kDisplayTickMs);
  }
  if (status == ESP_OK) configured_ = true;
  return status;
}

void NativeDisplayService::shutdown() {
  portENTER_CRITICAL(&mux_);
  clearMailbox(onboarding_mailbox_);
  navigation_ = AlbumNavigationCore();
  visible_onboarding_fingerprint_ = PageFingerprint{};
  configured_ = false;
  onboarding_pending_ = false;
  onboarding_rendering_ = false;
  onboarding_visible_ = false;
  onboarding_replacement_pending_ = false;
  album_restore_pending_ = false;
  catalog_known_empty_ = false;
  catalog_refreshing_ = false;
  album_rendering_ = false;
  storage_maintenance_ = false;
  portEXIT_CRITICAL(&mux_);
}

bool NativeDisplayService::beginStorageMaintenance() {
  portENTER_CRITICAL(&mux_);
  const bool quiescent = configured_ && !storage_maintenance_ &&
      !catalog_refreshing_ && !album_rendering_ && !navigation_.pending() &&
      !navigation_.refreshing() && !onboarding_pending_ &&
      !onboarding_rendering_ && !onboarding_replacement_pending_ &&
      !album_restore_pending_ && album_store_;
  if (quiescent) storage_maintenance_ = true;
  portEXIT_CRITICAL(&mux_);
  return quiescent;
}

bool NativeDisplayService::finishStorageMaintenance(bool reload_catalog) {
  portENTER_CRITICAL(&mux_);
  if (!storage_maintenance_ || catalog_refreshing_ || album_rendering_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  if (!reload_catalog) {
    storage_maintenance_ = false;
    portEXIT_CRITICAL(&mux_);
    return true;
  }
  catalog_refreshing_ = true;
  portEXIT_CRITICAL(&mux_);

  storage::AlbumIndex index;
  const bool loaded = album_store_ && !album_store_->active() &&
      album_store_->readCatalog(index).ok();
  size_t current = AlbumNavigationCore::kNoOrdinal;
  if (loaded) {
    for (size_t ordinal = 0; ordinal < index.assets.size(); ++ordinal) {
      if (index.assets[ordinal].id == index.current) {
        current = ordinal;
        break;
      }
    }
  }

  portENTER_CRITICAL(&mux_);
  const bool synchronized = loaded &&
      navigation_.synchronize(index.assets.size(), current);
  if (synchronized) {
    catalog_known_empty_ = index.assets.empty();
    storage_maintenance_ = false;
  } else {
    navigation_.invalidate();
    catalog_known_empty_ = false;
  }
  catalog_refreshing_ = false;
  portEXIT_CRITICAL(&mux_);
  return synchronized;
}

bool NativeDisplayService::synchronizeCatalog() {
  portENTER_CRITICAL(&mux_);
  if (storage_maintenance_ || catalog_refreshing_ || album_rendering_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  catalog_refreshing_ = true;
  portEXIT_CRITICAL(&mux_);

  storage::AlbumIndex index;
  bool synchronized = false;
  if (album_store_ && !album_store_->active() &&
      album_store_->readCatalog(index).ok()) {
    size_t current = AlbumNavigationCore::kNoOrdinal;
    for (size_t ordinal = 0; ordinal < index.assets.size(); ++ordinal) {
      if (index.assets[ordinal].id == index.current) {
        current = ordinal;
        break;
      }
    }
    portENTER_CRITICAL(&mux_);
    synchronized = navigation_.synchronize(index.assets.size(), current);
    if (synchronized) catalog_known_empty_ = index.assets.empty();
    portEXIT_CRITICAL(&mux_);
  }
  portENTER_CRITICAL(&mux_);
  catalog_refreshing_ = false;
  portEXIT_CRITICAL(&mux_);
  return synchronized;
}

bool NativeDisplayService::reloadCatalog() {
  return configured_ && synchronizeCatalog();
}

NativeDisplayPageRequestResult NativeDisplayService::requestProvisioningPage(
    const NativeProvisioningPageRequest& request) {
  IBoardRenderer* renderer = board_.renderer();
  if (!renderer || !renderer->supportsOnboardingFrames())
    return NativeDisplayPageRequestResult::NotReady;
  if (!printable(request.ssid, 1U, 32U) ||
      !printable(request.access_value, 8U, 63U) ||
      !printable(request.local_host, 1U, 64U) ||
      !printable(request.local_ip, 1U, 64U)) {
    return NativeDisplayPageRequestResult::InvalidInput;
  }
  const PageFingerprint content = fingerprint(
      OnboardingPageKind::Provisioning, request.ssid, request.access_value,
      request.local_host, request.local_ip);
  portENTER_CRITICAL(&mux_);
  NativeDisplayPageRequestResult result = NativeDisplayPageRequestResult::Busy;
  if (!configured_) {
    result = NativeDisplayPageRequestResult::NotReady;
  } else if (storage_maintenance_) {
    result = NativeDisplayPageRequestResult::Busy;
  } else {
    album_restore_pending_ = false;
    if (onboarding_pending_ || onboarding_rendering_) {
      result = onboarding_mailbox_.fingerprint == content
                   ? NativeDisplayPageRequestResult::AlreadyPending
                   : NativeDisplayPageRequestResult::Busy;
    } else if (onboarding_replacement_pending_ || album_rendering_) {
      result = NativeDisplayPageRequestResult::Busy;
    } else if (onboarding_visible_ &&
               visible_onboarding_fingerprint_ == content) {
      ++diagnostics_.onboarding_unchanged_skips;
      result = NativeDisplayPageRequestResult::Unchanged;
    } else {
      clearMailbox(onboarding_mailbox_);
      onboarding_mailbox_.kind = OnboardingPageKind::Provisioning;
      onboarding_mailbox_.fingerprint = content;
      const bool copied = copyBounded(request.ssid, onboarding_mailbox_.ssid) &&
          copyBounded(request.access_value,
                      onboarding_mailbox_.access_value) &&
          copyBounded(request.local_host, onboarding_mailbox_.local_host) &&
          copyBounded(request.local_ip, onboarding_mailbox_.local_ip);
      if (copied) {
        onboarding_pending_ = true;
        result = NativeDisplayPageRequestResult::Accepted;
      } else {
        clearMailbox(onboarding_mailbox_);
        result = NativeDisplayPageRequestResult::InvalidInput;
      }
    }
  }
  portEXIT_CRITICAL(&mux_);
  return result;
}

NativeDisplayPageRequestResult NativeDisplayService::requestMyAiPairingPage(
    const NativeMyAiPairingPageRequest& request) {
  IBoardRenderer* renderer = board_.renderer();
  if (!renderer || !renderer->supportsOnboardingFrames())
    return NativeDisplayPageRequestResult::NotReady;
  if (!onboarding::validMyAiPairingInputs(
          request.six_digit_code, request.binding_url)) {
    return NativeDisplayPageRequestResult::InvalidInput;
  }
  const PageFingerprint content = fingerprint(
      OnboardingPageKind::MyAiPairing, request.six_digit_code,
      request.binding_url);
  portENTER_CRITICAL(&mux_);
  NativeDisplayPageRequestResult result = NativeDisplayPageRequestResult::Busy;
  if (!configured_) {
    result = NativeDisplayPageRequestResult::NotReady;
  } else if (storage_maintenance_) {
    result = NativeDisplayPageRequestResult::Busy;
  } else {
    album_restore_pending_ = false;
    if (onboarding_pending_ || onboarding_rendering_) {
      result = onboarding_mailbox_.fingerprint == content
                   ? NativeDisplayPageRequestResult::AlreadyPending
                   : NativeDisplayPageRequestResult::Busy;
    } else if (onboarding_replacement_pending_ || album_rendering_) {
      result = NativeDisplayPageRequestResult::Busy;
    } else if (onboarding_visible_ &&
               visible_onboarding_fingerprint_ == content) {
      ++diagnostics_.onboarding_unchanged_skips;
      result = NativeDisplayPageRequestResult::Unchanged;
    } else {
      clearMailbox(onboarding_mailbox_);
      onboarding_mailbox_.kind = OnboardingPageKind::MyAiPairing;
      onboarding_mailbox_.fingerprint = content;
      const bool copied = copyBounded(
          request.six_digit_code, onboarding_mailbox_.six_digit_code) &&
          copyBounded(request.binding_url, onboarding_mailbox_.binding_url);
      if (copied) {
        onboarding_pending_ = true;
        result = NativeDisplayPageRequestResult::Accepted;
      } else {
        clearMailbox(onboarding_mailbox_);
        result = NativeDisplayPageRequestResult::InvalidInput;
      }
    }
  }
  portEXIT_CRITICAL(&mux_);
  return result;
}

NativeDisplayPageRequestResult NativeDisplayService::requestAlbumRestore() {
  portENTER_CRITICAL(&mux_);
  NativeDisplayPageRequestResult result = NativeDisplayPageRequestResult::Busy;
  if (!configured_) {
    result = NativeDisplayPageRequestResult::NotReady;
  } else if (storage_maintenance_) {
    result = NativeDisplayPageRequestResult::Busy;
  } else {
    // Authority disappeared before Display consumed the secret-bearing
    // mailbox. Cancel it in place so a stale page can never be rendered later.
    if (onboarding_pending_ && !onboarding_rendering_) {
      onboarding_pending_ = false;
      clearMailbox(onboarding_mailbox_);
    }
    if (album_restore_pending_) {
      result = NativeDisplayPageRequestResult::AlreadyPending;
    } else if (onboarding_rendering_) {
      album_restore_pending_ = true;
      result = NativeDisplayPageRequestResult::Accepted;
    } else if (!onboarding_visible_ || catalog_known_empty_) {
      result = NativeDisplayPageRequestResult::Unchanged;
    } else {
      album_restore_pending_ = true;
      result = NativeDisplayPageRequestResult::Accepted;
    }
  }
  portEXIT_CRITICAL(&mux_);
  return result;
}

AlbumStepResult NativeDisplayService::selectRelative(int direction,
                                                      size_t& ordinal) {
  portENTER_CRITICAL(&mux_);
  const AlbumStepResult result = storage_maintenance_
      ? AlbumStepResult::Busy
      : navigation_.step(direction, nowMs(), ordinal);
  portEXIT_CRITICAL(&mux_);
  return result;
}

bool NativeDisplayService::refreshing() const {
  portENTER_CRITICAL(&mux_);
  const bool value = navigation_.refreshing();
  portEXIT_CRITICAL(&mux_);
  return value;
}

bool NativeDisplayService::busy() const {
  portENTER_CRITICAL(&mux_);
  const bool navigation_busy = navigation_.pending() || navigation_.refreshing() ||
      onboarding_pending_ || onboarding_rendering_ ||
      onboarding_replacement_pending_ || album_restore_pending_ ||
      catalog_refreshing_ || album_rendering_ || storage_maintenance_;
  portEXIT_CRITICAL(&mux_);
  return navigation_busy || (album_store_ && album_store_->active());
}

WorkDisposition NativeDisplayService::handler(
    const WorkEnvelope& envelope, void* context) {
  return context
             ? static_cast<NativeDisplayService*>(context)->handle(envelope)
             : WorkDisposition::Failed;
}

void NativeDisplayService::tick(void* context) {
  if (context) static_cast<NativeDisplayService*>(context)->service();
}

void NativeDisplayService::service() {
  bool render_onboarding = false;
  bool restore_album = false;
  portENTER_CRITICAL(&mux_);
  if (storage_maintenance_) {
    portEXIT_CRITICAL(&mux_);
    return;
  }
  if (onboarding_pending_ && !onboarding_rendering_) {
    onboarding_pending_ = false;
    onboarding_rendering_ = true;
    render_onboarding = true;
  } else if (album_restore_pending_ && !onboarding_rendering_) {
    if (!onboarding_visible_ || catalog_known_empty_) {
      album_restore_pending_ = false;
    } else if (!catalog_refreshing_ && !album_rendering_ &&
               !navigation_.pending() && !navigation_.refreshing()) {
      restore_album = true;
      onboarding_replacement_pending_ = true;
    }
  }
  portEXIT_CRITICAL(&mux_);
  if (render_onboarding) {
    const bool rendered = renderOnboardingPage();
    portENTER_CRITICAL(&mux_);
    if (rendered) {
      visible_onboarding_fingerprint_ = onboarding_mailbox_.fingerprint;
      onboarding_visible_ = true;
    } else {
      ++diagnostics_.onboarding_failures;
    }
    clearMailbox(onboarding_mailbox_);
    onboarding_rendering_ = false;
    portEXIT_CRITICAL(&mux_);
    return;
  }
  if (restore_album) {
    // kNoOrdinal is an internal restore token: renderOrdinalAdmitted() reads
    // the catalog and resolves persisted-current-or-zero on the Display lane.
    if (!album_store_ || album_store_->active()) {
      portENTER_CRITICAL(&mux_);
      onboarding_replacement_pending_ = false;
      portEXIT_CRITICAL(&mux_);
      return;
    }
    const bool restored = renderOrdinal(AlbumNavigationCore::kNoOrdinal);
    portENTER_CRITICAL(&mux_);
    if (!restored) onboarding_replacement_pending_ = false;
    if (restored || !onboarding_visible_) album_restore_pending_ = false;
    portEXIT_CRITICAL(&mux_);
    return;
  }
  // A Portal-owned AIGC/upload/download transaction can span many chunks.
  // Keep the user's pending selection intact until that transaction commits;
  // never turn a temporary storage-busy condition into a failed refresh.
  if (!album_store_ || album_store_->active()) return;
  size_t ordinal = AlbumNavigationCore::kNoOrdinal;
  bool changed = false;
  portENTER_CRITICAL(&mux_);
  const bool settled = !storage_maintenance_ && !catalog_refreshing_ &&
      !album_rendering_ &&
      navigation_.takeSettled(nowMs(), ordinal, changed);
  portEXIT_CRITICAL(&mux_);
  if (!settled || !changed) return;
  postRefreshStarting(ordinal);
  renderOrdinal(ordinal);
}

bool NativeDisplayService::writePanelFrame(const uint8_t* frame,
                                           size_t frame_bytes) {
  const BoardDescriptor& descriptor = board_.descriptor();
  if (!frame || frame_bytes != descriptor.packed4BppFrameBytes()) return false;
  const BoardFrameView view{
      frame, frame_bytes, descriptor.width, descriptor.height,
      BoardFrameFormat::Palette4Bpp};
  IBoardDisplay* display = board_.display();
  const uint32_t started = nowMs();
  const bool written = display && display->writeFullFrame(view) == ESP_OK;
  const uint32_t elapsed = nowMs() - started;
  portENTER_CRITICAL(&mux_);
  diagnostics_.last_panel_refresh_ms = elapsed;
  diagnostics_.maximum_panel_refresh_ms = std::max(
      diagnostics_.maximum_panel_refresh_ms, elapsed);
  if (written)
    ++diagnostics_.panel_writes;
  else
    ++diagnostics_.panel_failures;
  portEXIT_CRITICAL(&mux_);
  return written;
}

bool NativeDisplayService::renderOnboardingPage() {
  const BoardDescriptor& descriptor = board_.descriptor();
  IBoardRenderer* renderer = board_.renderer();
  if (!renderer || !renderer->supportsOnboardingFrames()) return false;
  HeapBytes frame(descriptor.packed4BppFrameBytes(), descriptor.has_psram);
  if (!frame.get()) return false;
  esp_err_t rendered = ESP_FAIL;
  if (onboarding_mailbox_.kind == OnboardingPageKind::Provisioning) {
    rendered = renderer->renderProvisioningFrame(
        onboarding_mailbox_.ssid.data(),
        onboarding_mailbox_.access_value.data(),
        onboarding_mailbox_.local_host.data(),
        onboarding_mailbox_.local_ip.data(), frame.get(), frame.size());
  } else if (onboarding_mailbox_.kind == OnboardingPageKind::MyAiPairing) {
    rendered = renderer->renderMyAiPairingFrame(
        onboarding_mailbox_.six_digit_code.data(),
        onboarding_mailbox_.binding_url.data(), frame.get(), frame.size());
  }
  return rendered == ESP_OK && writePanelFrame(frame.get(), frame.size());
}

AdmissionResult NativeDisplayService::postImageLed(uint8_t mode) {
  WorkEnvelope command{};
  command.generation = 1;
  command.request_id = nextRequestId();
  command.deadline_ms = nowMs() + kLedDeadlineMs;
  command.opcode = productOpcode(ProductOpcode::SetImageLed);
  command.work_class = WorkClass::LedStatus;
  command.kind = EnvelopeKind::Command;
  command.disposition = WorkDisposition::Accepted;
  command.flags = mode;
  return supervisor_.post(command);
}

AdmissionResult NativeDisplayService::postRefreshStarting(size_t ordinal) {
  if (ordinal > 255U) return AdmissionResult::InvalidEnvelope;
  WorkEnvelope command{};
  command.generation = 1;
  command.request_id = nextRequestId();
  command.deadline_ms = nowMs() + kLedDeadlineMs;
  command.opcode = productOpcode(ProductOpcode::AlbumRefreshStarting);
  command.work_class = WorkClass::Control;
  command.kind = EnvelopeKind::Command;
  command.disposition = WorkDisposition::Accepted;
  command.flags = static_cast<uint8_t>(ordinal);
  return supervisor_.post(command);
}

WorkDisposition NativeDisplayService::handle(
    const WorkEnvelope& envelope) {
  if (envelope.kind != EnvelopeKind::Command ||
      envelope.work_class != WorkClass::Display ||
      envelope.opcode != productOpcode(ProductOpcode::DisplayAlbumOrdinal)) {
    return WorkDisposition::Failed;
  }
  portENTER_CRITICAL(&mux_);
  const bool blocked = storage_maintenance_ || catalog_refreshing_ ||
      album_rendering_;
  portEXIT_CRITICAL(&mux_);
  if (blocked) return WorkDisposition::Busy;
  return renderOrdinal(envelope.flags) ? WorkDisposition::Complete
                                       : WorkDisposition::Failed;
}

bool NativeDisplayService::renderOrdinal(size_t ordinal) {
  portENTER_CRITICAL(&mux_);
  if (storage_maintenance_ || catalog_refreshing_ || album_rendering_) {
    portEXIT_CRITICAL(&mux_);
    return false;
  }
  album_rendering_ = true;
  portEXIT_CRITICAL(&mux_);

  const bool rendered = renderOrdinalAdmitted(ordinal);

  portENTER_CRITICAL(&mux_);
  album_rendering_ = false;
  portEXIT_CRITICAL(&mux_);
  return rendered;
}

bool NativeDisplayService::renderOrdinalAdmitted(size_t ordinal) {
  storage::AlbumIndex index;
  const bool restore_onboarding = ordinal == AlbumNavigationCore::kNoOrdinal;
  if (!album_store_->readCatalog(index).ok() ||
      (!restore_onboarding && ordinal >= index.assets.size())) {
    portENTER_CRITICAL(&mux_);
    ++diagnostics_.decode_failures;
    navigation_.invalidate();
    portEXIT_CRITICAL(&mux_);
    postImageLed(static_cast<uint8_t>(ImageLedMode::Error));
    return false;
  }
  size_t current = AlbumNavigationCore::kNoOrdinal;
  for (size_t at = 0; at < index.assets.size(); ++at) {
    if (index.assets[at].id == index.current) {
      current = at;
      break;
    }
  }
  portENTER_CRITICAL(&mux_);
  catalog_known_empty_ = index.assets.empty();
  if (restore_onboarding && index.assets.empty()) {
    navigation_.finish(0, AlbumNavigationCore::kNoOrdinal);
    onboarding_replacement_pending_ = false;
    portEXIT_CRITICAL(&mux_);
    return true;
  }
  portEXIT_CRITICAL(&mux_);
  if (restore_onboarding) {
    ordinal = current == AlbumNavigationCore::kNoOrdinal ? 0U : current;
  }
  bool announce = false;
  bool onboarding_visible = false;
  portENTER_CRITICAL(&mux_);
  if (!navigation_.refreshing()) {
    if (!navigation_.synchronize(index.assets.size(), current) ||
        !navigation_.beginImmediate(ordinal)) {
      navigation_.finish(index.assets.size(), current);
      portEXIT_CRITICAL(&mux_);
      postImageLed(static_cast<uint8_t>(ImageLedMode::Error));
      return false;
    }
    announce = navigation_.refreshing();
  }
  onboarding_visible = onboarding_visible_;
  if (onboarding_visible) onboarding_replacement_pending_ = true;
  portEXIT_CRITICAL(&mux_);
  if (announce) postRefreshStarting(ordinal);
  const storage::AlbumIndexAsset asset = index.assets[ordinal];
  if (!onboarding_visible && index.current == asset.id &&
      index.current_render_strategy == asset.render_strategy) {
    portENTER_CRITICAL(&mux_);
    ++diagnostics_.unchanged_skips;
    navigation_.finish(index.assets.size(), ordinal);
    portEXIT_CRITICAL(&mux_);
    postImageLed(static_cast<uint8_t>(ImageLedMode::Complete));
    return true;
  }
  const uint32_t total_started = nowMs();
  const uint32_t decode_started = total_started;
  const BoardDescriptor& descriptor = board_.descriptor();
  IBoardRenderer* renderer = board_.renderer();
  if (!renderer || !renderer->supportsRenderStrategy(asset.render_strategy)) {
    portENTER_CRITICAL(&mux_);
    ++diagnostics_.render_failures;
    onboarding_replacement_pending_ = false;
    navigation_.finish(index.assets.size(), current);
    portEXIT_CRITICAL(&mux_);
    postImageLed(static_cast<uint8_t>(ImageLedMode::Error));
    return false;
  }
  const size_t pixel_count =
      static_cast<size_t>(descriptor.width) * descriptor.height;
  const size_t rgb_bytes = pixel_count * 3U;
  const size_t frame_bytes = descriptor.packed4BppFrameBytes();
  std::string path;
  HeapBytes rgb(rgb_bytes, descriptor.has_psram);
  HeapBytes frame(frame_bytes, descriptor.has_psram);
  postImageLed(static_cast<uint8_t>(ImageLedMode::Converting));
  if (!rgb.get() || !frame.get() ||
      !album_store_->absoluteAssetPath(asset, path) ||
      !decodePngFile(path, asset.bytes, rgb.get(), rgb.size(),
                     descriptor.width, descriptor.height)) {
    const uint32_t elapsed = nowMs() - decode_started;
    portENTER_CRITICAL(&mux_);
    ++diagnostics_.decode_failures;
    onboarding_replacement_pending_ = false;
    diagnostics_.last_load_decode_ms = elapsed;
    diagnostics_.maximum_load_decode_ms = std::max(
        diagnostics_.maximum_load_decode_ms, elapsed);
    navigation_.finish(index.assets.size(), current);
    portEXIT_CRITICAL(&mux_);
    postImageLed(static_cast<uint8_t>(ImageLedMode::Error));
    return false;
  }
  const uint32_t decoded_ms = nowMs() - decode_started;
  portENTER_CRITICAL(&mux_);
  diagnostics_.last_load_decode_ms = decoded_ms;
  diagnostics_.maximum_load_decode_ms = std::max(
      diagnostics_.maximum_load_decode_ms, decoded_ms);
  portEXIT_CRITICAL(&mux_);

  const uint32_t conversion_started = nowMs();
  const BoardRgbFrameView rgb_view{
      rgb.get(), rgb.size(), descriptor.width, descriptor.height,
      static_cast<size_t>(descriptor.width) * 3U};
  if (renderer->renderRgbFullFrame(
          rgb_view, asset.render_strategy, frame.get(), frame.size()) !=
      ESP_OK) {
    const uint32_t elapsed = nowMs() - conversion_started;
    portENTER_CRITICAL(&mux_);
    ++diagnostics_.render_failures;
    onboarding_replacement_pending_ = false;
    diagnostics_.last_conversion_ms = elapsed;
    diagnostics_.maximum_conversion_ms = std::max(
        diagnostics_.maximum_conversion_ms, elapsed);
    navigation_.finish(index.assets.size(), current);
    portEXIT_CRITICAL(&mux_);
    postImageLed(static_cast<uint8_t>(ImageLedMode::Error));
    return false;
  }
  const uint32_t converted_ms = nowMs() - conversion_started;
  portENTER_CRITICAL(&mux_);
  diagnostics_.last_conversion_ms = converted_ms;
  diagnostics_.maximum_conversion_ms = std::max(
      diagnostics_.maximum_conversion_ms, converted_ms);
  portEXIT_CRITICAL(&mux_);

  postImageLed(static_cast<uint8_t>(ImageLedMode::Writing));
  if (!writePanelFrame(frame.get(), frame.size())) {
    portENTER_CRITICAL(&mux_);
    onboarding_replacement_pending_ = false;
    navigation_.finish(index.assets.size(), current);
    portEXIT_CRITICAL(&mux_);
    postImageLed(static_cast<uint8_t>(ImageLedMode::Error));
    return false;
  }
  portENTER_CRITICAL(&mux_);
  onboarding_visible_ = false;
  onboarding_replacement_pending_ = false;
  portEXIT_CRITICAL(&mux_);
  if (!album_store_->markCurrent(asset.id).ok()) {
    portENTER_CRITICAL(&mux_);
    ++diagnostics_.persistence_failures;
    navigation_.finish(index.assets.size(), current);
    portEXIT_CRITICAL(&mux_);
    postImageLed(static_cast<uint8_t>(ImageLedMode::Error));
    return false;
  }
  const uint32_t total_ms = nowMs() - total_started;
  uint32_t panel_ms = 0;
  portENTER_CRITICAL(&mux_);
  navigation_.finish(index.assets.size(), ordinal);
  ++diagnostics_.completed_album_refreshes;
  diagnostics_.last_album_total_ms = total_ms;
  diagnostics_.maximum_album_total_ms = std::max(
      diagnostics_.maximum_album_total_ms, total_ms);
  panel_ms = diagnostics_.last_panel_refresh_ms;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGI(kTag,
           "album refresh timing load_decode_ms=%lu conversion_ms=%lu "
           "panel_ms=%lu total_ms=%lu",
           static_cast<unsigned long>(decoded_ms),
           static_cast<unsigned long>(converted_ms),
           static_cast<unsigned long>(panel_ms),
           static_cast<unsigned long>(total_ms));
  postImageLed(static_cast<uint8_t>(ImageLedMode::Complete));
  return true;
}

NativeDisplayDiagnostics NativeDisplayService::diagnostics() const {
  portENTER_CRITICAL(&mux_);
  const NativeDisplayDiagnostics value = diagnostics_;
  portEXIT_CRITICAL(&mux_);
  return value;
}

}  // namespace inkloop
