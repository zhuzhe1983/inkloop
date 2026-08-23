import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const core = join(repo, "firmware/inkloop-idf/components/inkloop_portal");
const idf = join(repo, "firmware/inkloop-idf/components/inkloop_portal_idf");
const portalSource = readFileSync(join(core, "portal_core.cpp"), "utf8");
const portalHeader = readFileSync(
  join(core, "include/inkloop/portal/portal_core.hpp"),
  "utf8",
);

const flowHarness = String.raw`
#include <cassert>
#include <cstddef>
#include <cstdint>
#include "inkloop/portal/bounded_queue_flow.hpp"

using namespace inkloop::portal;

int main() {
  constexpr size_t bytes = 1536U * 1024U;
  constexpr size_t chunk = 2048U;
  constexpr size_t capacity = 32U;
  size_t pending = 0;
  size_t maximum_pending = 0;
  size_t accepted = 0;
  int64_t now_us = 0;
  const int64_t deadline_us = 30000000LL;
  auto clock = [&] { return now_us; };
  auto yield = [&] {
    now_us += 1000;
    // Portal lane runs every 100 ms and drains at most twelve chunks.
    if ((now_us % 100000LL) == 0) {
      pending = pending > 12U ? pending - 12U : 0U;
    }
  };
  for (size_t offset = 0; offset < bytes; offset += chunk) {
    const PortalResult result = retryBusyUntil(
        deadline_us,
        [&] {
          if (pending == capacity) return PortalResult::Busy;
          ++pending;
          ++accepted;
          if (pending > maximum_pending) maximum_pending = pending;
          return PortalResult::Ok;
        },
        clock, yield);
    assert(result == PortalResult::Ok);
  }
  const PortalResult finish = retryBusyUntil(
      deadline_us,
      [&] { return pending == capacity ? PortalResult::Busy
                                       : PortalResult::Ok; },
      clock, yield);
  assert(finish == PortalResult::Ok);
  assert(accepted == bytes / chunk);
  assert(maximum_pending == capacity);
  assert(now_us > 5000000LL && now_us < deadline_us);
  return 0;
}
`;

function buildAndRunFlowHarness() {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-portal-flow-"));
  try {
    const source = join(scratch, "flow.cpp");
    const binary = join(scratch, "flow");
    writeFileSync(source, flowHarness);
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(core, "include"), "-I", join(idf, "include"),
      source, "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [], { stdio: "pipe" });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

const harness = String.raw`
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

#include "inkloop/portal/portal_core.hpp"

using namespace inkloop::portal;

struct Cache final : IPortalReadCache {
  mutable PortalResult state_result = PortalResult::Ok;
  mutable PortalResult album_result = PortalResult::Ok;
  mutable PortalResult chat_result = PortalResult::Ok;
  mutable bool bad_album_total = false;
  mutable bool bad_chat_total = false;
  mutable bool bad_runtime_count = false;
  mutable bool bad_runtime_queue = false;
  mutable bool bad_runtime_core = false;
  mutable bool bad_display_dimensions = false;
  mutable bool bad_myai_error = false;
  mutable bool minimal_capabilities = false;
  mutable PortalFirmwareUpdateSnapshot firmware_update{};

  PortalResult readState(PortalStateSnapshot& value) const override {
    if (state_result != PortalResult::Ok) return state_result;
    value.firmware_version = "idf-0.3";
    value.firmware_update = firmware_update;
    value.device_name = "Inkloop PaperColor";
    value.display_width = 400;
    value.display_height = 600;
    if (bad_display_dimensions) value.display_width = 0;
    value.wifi_online = true;
    value.storage_ready = true;
    value.display_busy = false;
    value.display_completed_refreshes = 3;
    value.display_load_decode_ms = 420;
    value.display_conversion_ms = 870;
    value.display_panel_refresh_ms = 18400;
    value.display_total_ms = 19710;
    value.capabilities.has_microphone = !minimal_capabilities;
    value.capabilities.has_speaker = !minimal_capabilities;
    value.capabilities.rgb_pixels = minimal_capabilities ? 0 : 2;
    value.capabilities.has_removable_storage = !minimal_capabilities;
    value.capabilities.render_strategy_count = minimal_capabilities ? 3 : 4;
    value.capabilities.render_strategies[0] =
        {"official-quality", "官方高质量"};
    value.capabilities.render_strategies[1] =
        {"classic-six-color", "经典六色抖动"};
    if (!minimal_capabilities) {
      value.capabilities.render_strategies[2] =
          {"reflectance-photo", "照片优化"};
      value.capabilities.render_strategies[3] =
          {"solid-clean", "纯色 / 文字"};
    } else {
      value.capabilities.render_strategies[2] =
          {"solid-clean", "纯色 / 文字"};
    }
    value.myai_state = MyAiPortalState::Pairing;
    value.myai_error.available = true;
    value.myai_error.source = MyAiPortalErrorSource::Authorization;
    value.myai_error.code = MyAiPortalErrorCode::Unauthorized;
    value.myai_error.http_status = bad_myai_error ? 42 : 401;
    value.myai_error.retry_after_ms = 5000;
    value.myai_error.sequence = 7;
    value.myai_error.observed_at_ms = 12345;
    value.tutorial.step = TutorialPortalStep::GalleryPaging;
    value.tutorial.persistence_pending = true;
    value.pairing_code = "950940";
    value.binding_url = "https://myai.vibapp.ai/device/950940";
    value.runtime.available = true;
    value.runtime.sequence = 47;
    value.runtime.last_managed_update_ms = 123456;
    value.runtime.internal_heap_sampled = true;
    value.runtime.internal_heap_min_free_bytes = 81234;
    value.runtime.psram_available = true;
    value.runtime.psram_min_free_bytes = 3456789;
    value.runtime.resource_sample_count = 19;
    value.runtime.lane_count = kPortalRuntimeLaneCount;
    const uint32_t capacities[kPortalRuntimeLaneCount] =
        {32, 32, 32, 16, 8, 4, 8, 4};
    const int8_t cores[kPortalRuntimeLaneCount] = {1, 1, 1, 1, 0, 0, 0, 0};
    const uint8_t priorities[kPortalRuntimeLaneCount] =
        {22, 20, 18, 8, 7, 6, 9, 3};
    for (size_t index = 0; index < kPortalRuntimeLaneCount; ++index) {
      PortalRuntimeLaneTelemetry& lane = value.runtime.lanes[index];
      lane.queue_capacity = capacities[index];
      lane.queue_depth = index % 3;
      lane.queue_high_water = 3;
      lane.stack_sampled = true;
      lane.stack_low_water_bytes = 2048 - static_cast<uint32_t>(index * 32);
      lane.handler_count = 20 + index;
      lane.handler_max_us = 150 + static_cast<uint32_t>(index);
      lane.tick_count = 40 + index;
      lane.tick_max_us = 90 + static_cast<uint32_t>(index);
      lane.tick_late_count = index == 6 ? 2 : 0;
      lane.tick_missed = index == 6 ? 1 : 0;
      lane.tick_late_max_us = index == 6 ? 3000 : 0;
      lane.last_progress_ms = 123450 + static_cast<uint32_t>(index);
      lane.configured_core = cores[index];
      lane.observed_core = cores[index];
      lane.configured_priority = priorities[index];
      lane.observed_priority = priorities[index];
      lane.task_running = true;
    }
    if (bad_runtime_count) value.runtime.lane_count = 7;
    if (bad_runtime_queue) value.runtime.lanes[0].queue_depth = 33;
    if (bad_runtime_core) value.runtime.lanes[1].observed_core = 2;
    value.settings.volume = 60;
    value.settings.led_maximum_brightness_percent = 55;
    value.settings.voice_assistance_enabled = !minimal_capabilities;
    value.settings.assistant_prompt = "简洁回答";
    value.settings.image_prompt_template = "六色海报：{prompt}";
    value.settings.negative_prompt = "文字，水印";
    value.settings.asset_storage_preference =
        minimal_capabilities ? "automatic" : "removable";
    value.settings.default_render_strategy = "solid-clean";
    value.settings.local_management_password_overridden = true;
    return PortalResult::Ok;
  }

  PortalResult readAlbumPage(const AlbumPageQuery& query,
                             AlbumPage& value) const override {
    if (album_result != PortalResult::Ok) return album_result;
    assert(query.limit <= kMaximumAlbumPageItems);
    value.total_items = bad_album_total ? kMaximumAlbumTotalItems + 1 : 2;
    value.revision = 7;
    AlbumItem first;
    first.id = "asset-1";
    first.title = "日落";
    first.origin = "upload";
    first.bytes = 1234;
    first.current = true;
    first.render_strategy = "solid-clean";
    value.items.push_back(first);
    if (query.cursor.empty()) value.next_cursor = "page-2";
    return PortalResult::Ok;
  }

  PortalResult readLocalChatPage(const ChatPageQuery& query,
                                 ChatPage& value) const override {
    if (chat_result != PortalResult::Ok) return chat_result;
    assert(query.limit <= kMaximumChatPageItems);
    value.total_items = bad_chat_total ? kMaximumChatTotalItems + 1 : 3;
    value.next_after_sequence = 3;
    value.has_more = false;
    ChatItem user;
    user.sequence = 1;
    user.role = ChatRole::User;
    user.text = "请生成高对比海报";
    value.items.push_back(user);
    ChatItem artifact;
    artifact.sequence = 2;
    artifact.role = ChatRole::User;
    artifact.text = " [blank_audio] ";
    value.items.push_back(artifact);
    ChatItem assistant;
    assistant.sequence = 3;
    assistant.role = ChatRole::Assistant;
    assistant.text = "已经排队";
    value.items.push_back(assistant);
    return PortalResult::Ok;
  }
};

struct Commands final : IPortalCommandQueue {
  PortalResult result = PortalResult::Ok;
  std::vector<PortalCommand> received;
  PortalResult tryEnqueue(const PortalCommand& command) override {
    if (result == PortalResult::Ok) received.push_back(command);
    return result;
  }
};

PortalAccessConfig access() {
  PortalAccessConfig value;
  value.access_code = "secret99";
  value.session_id = "session_abcdefghijklmnopqrstuvwxyz";
  value.csrf_token = "csrf_abcdefghijklmnopqrstuvwxyz0123";
  value.allowed_hosts = {"inkloop.local", "192.168.4.1"};
  value.allowed_origins = {"http://inkloop.local", "http://192.168.4.1"};
  value.session_lifetime_seconds = 900;
  return value;
}

PortalRequest request(std::string method, std::string path, uint64_t now = 100) {
  PortalRequest value;
  value.method = std::move(method);
  value.path = std::move(path);
  value.host = "inkloop.local";
  value.origin = "http://inkloop.local";
  value.peer_is_local = true;
  value.now_seconds = now;
  return value;
}

std::string login(PortalCore& portal) {
  PortalRequest login = request("POST", "/api/session");
  login.content_type = "application/x-www-form-urlencoded";
  login.body = "nonce=secret99";
  login.content_length = login.body.size();
  PortalResponse response = portal.handle(login);
  assert(response.status == 200);
  assert(response.body.find("csrfToken") != std::string::npos);
  const size_t end = response.set_cookie.find(';');
  assert(end != std::string::npos);
  return response.set_cookie.substr(0, end);
}

PortalRequest authenticated(std::string method, std::string path,
                            const std::string& cookie) {
  PortalRequest value = request(std::move(method), std::move(path), 101);
  value.cookie = cookie;
  if (value.method == "POST") {
    value.csrf_token = access().csrf_token;
    value.content_type = "application/x-www-form-urlencoded";
  }
  return value;
}

int main() {
  Cache cache;
  Commands commands;

  PortalAccessConfig invalid = access();
  invalid.csrf_token = "short";
  PortalCore invalid_portal(invalid, cache, commands);
  assert(!invalid_portal.ready());

  PortalCore portal(access(), cache, commands);
  assert(portal.ready());
  PortalRequest dashboard = request("GET", "/");
  PortalResponse rendered = portal.handle(dashboard);
  assert(rendered.status == 200);
  assert(rendered.body.find("data-tab=\"device\"") != std::string::npos);
  assert(rendered.body.find("data-tab=\"album\"") != std::string::npos);
  assert(rendered.body.find("data-tab=\"myai\"") != std::string::npos);
  assert(rendered.body.find("data-tab=\"settings\"") != std::string::npos);
  const size_t myai_panel = rendered.body.find("data-panel=\"myai\"");
  const size_t myai_settings = rendered.body.find("id=\"myai-settings\"");
  const size_t prompt_field = rendered.body.find("name=\"assistant_prompt\"");
  const size_t general_settings = rendered.body.find("data-panel=\"settings\"");
  assert(myai_panel < myai_settings && myai_settings < prompt_field &&
         prompt_field < general_settings);
  assert(rendered.body.find("保存 MyAI 设置") != std::string::npos);
  assert(rendered.body.find("MyAI 设置已排队保存") != std::string::npos);
  assert(rendered.body.find("/api/album/upload") != std::string::npos);
  assert(rendered.body.find("/api/album/preview") != std::string::npos);
  assert(rendered.body.find("/api/chat?limit=") != std::string::npos);
  assert(rendered.body.find("/api/tutorial/restart") != std::string::npos);
  assert(rendered.body.find("id=\"tutorial-restart\"") != std::string::npos);
  assert(rendered.body.find("name=\"storage_preference\"") != std::string::npos);
  assert(rendered.body.find("name=\"default_render_strategy\"") != std::string::npos);
  assert(rendered.body.find("name=\"local_password\"") != std::string::npos);
  assert(rendered.body.find("画面、保存与刷新") != std::string::npos);
  assert(rendered.body.find("声音与状态灯") != std::string::npos);
  assert(rendered.body.find("__inkloopPortalRefresh") != std::string::npos);
  assert(rendered.body.find("visibilitychange") != std::string::npos);
  assert(rendered.body.find("portal.hidden") != std::string::npos);
  assert(rendered.body.find("cadence=5000") != std::string::npos);
  assert(rendered.body.find("displayTimingSignature") != std::string::npos);
  assert(rendered.body.find("storageCapacitySignature") != std::string::npos);
  assert(rendered.body.find("storageFreeBytes") != std::string::npos);
  assert(rendered.body.find("storageTotalBytes") != std::string::npos);
  assert(rendered.body.find("剩余空间") != std::string::npos);
  assert(rendered.body.find("总空间") != std::string::npos);
  assert(rendered.body.find("最近物理刷新") != std::string::npos);
  assert(rendered.body.find("运行诊断") != std::string::npos);
  assert(rendered.body.find("runtime-diagnostics") != std::string::npos);
  assert(rendered.body.find("仅随当前可见") != std::string::npos);
  assert(rendered.body.find("系统与固件") != std::string::npos);
  assert(rendered.body.find("id=\"firmware-update\"") != std::string::npos);
  assert(rendered.body.find("/api/system/update") != std::string::npos);
  assert(rendered.body.find("install-signed-firmware") != std::string::npos);
  assert(std::strlen(PortalCore::dashboardHtml()) < kMaximumPortalResponseBytes);

  PortalRequest remote = dashboard;
  remote.peer_is_local = false;
  assert(portal.handle(remote).status == 403);
  PortalRequest bad_host = dashboard;
  bad_host.host = "inkloop.local.attacker.test";
  assert(portal.handle(bad_host).status == 400);
  assert(portal.handle(request("GET", "/api/state")).status == 401);

  PortalRequest wrong = request("POST", "/api/session");
  wrong.content_type = "application/x-www-form-urlencoded";
  wrong.body = "nonce=wrong999";
  assert(portal.handle(wrong).status == 401);
  wrong.origin = "http://attacker.test";
  assert(portal.handle(wrong).status == 403);

  const std::string cookie = login(portal);
  PortalRequest state = authenticated("GET", "/api/state", cookie);
  PortalResponse state_response = portal.handle(state);
  assert(state_response.status == 200);
  assert(state_response.body.find("\"tabs\":[\"device\",\"album\",\"myai\",\"settings\"]") != std::string::npos);
  assert(state_response.body.find("\"firmwareUpdate\":{\"configured\":false,\"acceptedOffline\":false,\"currentVersion\":\"idf-0.3\",\"status\":\"unavailable\",\"code\":\"none\"}") != std::string::npos);
  assert(state_response.body.find("950940") != std::string::npos);
  assert(state_response.body.find("\"state\":\"pairing\"") != std::string::npos);
  assert(state_response.body.find("\"lastError\":{\"available\":true,\"source\":\"authorization\",\"code\":\"unauthorized\",\"httpStatus\":401,\"retryAfterMs\":5000,\"sequence\":7,\"observedAtMs\":12345}") != std::string::npos);
  assert(state_response.body.find("\"tutorial\":{\"step\":\"gallery_paging\",\"inFlight\":false,\"persistencePending\":true,\"persistenceError\":false}") != std::string::npos);
  assert(state_response.body.find("device_token") == std::string::npos);
  assert(state_response.body.find("pairing_token") == std::string::npos);
  assert(state_response.body.find("image_url") == std::string::npos);
  assert(state_response.body.find("manifest_url") == std::string::npos);
  assert(state_response.body.find("detached_signature") == std::string::npos);
  assert(state_response.body.find("public_key") == std::string::npos);
  assert(state_response.body.find("http_status") == std::string::npos);
  assert(state_response.body.find("\"assetStoragePreference\":\"removable\"") != std::string::npos);
  assert(state_response.body.find("\"displayTiming\":{\"completedRefreshes\":3") != std::string::npos);
  assert(state_response.body.find("\"panelRefreshMs\":18400") != std::string::npos);
  assert(state_response.body.find("\"runtimeTelemetry\":{\"available\":true") != std::string::npos);
  assert(state_response.body.find("\"laneCount\":8") != std::string::npos);
  assert(state_response.body.find("\"queueHighWater\":3") != std::string::npos);
  assert(state_response.body.find("\"stackLowWaterBytes\":2048") != std::string::npos);
  assert(state_response.body.find("\"handlerMaxUs\":150") != std::string::npos);
  assert(state_response.body.find("\"tickMissed\":1") != std::string::npos);
  assert(state_response.body.find("\"internalHeapMinFreeBytes\":81234") != std::string::npos);
  assert(state_response.body.find("ink-input") == std::string::npos);
  assert(state_response.body.find("0x") == std::string::npos);
  assert(state_response.body.find("\"defaultRenderStrategy\":\"solid-clean\"") != std::string::npos);
  assert(state_response.body.find("\"microphone\":true") != std::string::npos);
  assert(state_response.body.find("\"speaker\":true") != std::string::npos);
  assert(state_response.body.find("\"duplexAudio\":true") != std::string::npos);
  assert(state_response.body.find("\"rgbPixels\":2") != std::string::npos);
  assert(state_response.body.find("\"removableStorage\":true") != std::string::npos);
  assert(state_response.body.find("\"reflectance-photo\"") != std::string::npos);
  assert(state_response.body.find("\"displayName\":\"照片优化\"") != std::string::npos);
  assert(state_response.body.find("localManagementPasswordOverridden\":true") != std::string::npos);
  assert(state_response.body.find("portal pass 42") == std::string::npos);
  cache.bad_runtime_count = true;
  assert(portal.handle(state).status == 422);
  cache.bad_runtime_count = false;
  cache.bad_runtime_queue = true;
  assert(portal.handle(state).status == 422);
  cache.bad_runtime_queue = false;
  cache.bad_runtime_core = true;
  assert(portal.handle(state).status == 422);
  cache.bad_runtime_core = false;
  cache.bad_display_dimensions = true;
  assert(portal.handle(state).status == 422);
  cache.bad_display_dimensions = false;
  cache.bad_myai_error = true;
  assert(portal.handle(state).status == 422);
  cache.bad_myai_error = false;

  PortalRequest expired = state;
  expired.now_seconds = 1000;
  assert(portal.handle(expired).status == 401);
  login(portal);

  PortalRequest no_csrf = authenticated("POST", "/api/settings", cookie);
  no_csrf.csrf_token.clear();
  no_csrf.body = "volume=10";
  assert(portal.handle(no_csrf).status == 403);

  cache.firmware_update.configured = true;
  cache.firmware_update.phase = PortalFirmwareUpdatePhase::Ready;
  const size_t before_update_rejections = commands.received.size();
  PortalRequest update_without_session = request("POST", "/api/system/update");
  update_without_session.content_type = "application/x-www-form-urlencoded";
  update_without_session.body = "confirm=install-signed-firmware";
  assert(portal.handle(update_without_session).status == 401);
  PortalRequest update_no_csrf = authenticated(
      "POST", "/api/system/update", cookie);
  update_no_csrf.csrf_token.clear();
  update_no_csrf.body = "confirm=install-signed-firmware";
  assert(portal.handle(update_no_csrf).status == 403);
  PortalRequest update_bad_origin = authenticated(
      "POST", "/api/system/update", cookie);
  update_bad_origin.origin = "http://attacker.test";
  update_bad_origin.body = "confirm=install-signed-firmware";
  assert(portal.handle(update_bad_origin).status == 403);
  PortalRequest update_remote = authenticated(
      "POST", "/api/system/update", cookie);
  update_remote.peer_is_local = false;
  update_remote.body = "confirm=install-signed-firmware";
  assert(portal.handle(update_remote).status == 403);
  assert(portal.handle(authenticated(
      "GET", "/api/system/update", cookie)).status == 404);
  PortalRequest update = authenticated("POST", "/api/system/update", cookie);
  update.body = "confirm=install-signed-firmware";
  PortalRequest update_wrong_type = update;
  update_wrong_type.content_type = "application/json";
  assert(portal.handle(update_wrong_type).status == 415);
  PortalRequest update_empty = update;
  update_empty.body.clear();
  assert(portal.handle(update_empty).status == 400);
  PortalRequest update_wrong_confirmation = update;
  update_wrong_confirmation.body = "confirm=yes";
  assert(portal.handle(update_wrong_confirmation).status == 422);
  PortalRequest update_duplicate = update;
  update_duplicate.body =
      "confirm=install-signed-firmware&confirm=install-signed-firmware";
  assert(portal.handle(update_duplicate).status == 400);
  PortalRequest update_extra = update;
  update_extra.body = "confirm=install-signed-firmware&force=1";
  assert(portal.handle(update_extra).status == 400);
  PortalRequest update_query = update;
  update_query.path = "/api/system/update?force=1";
  assert(portal.handle(update_query).status == 400);
  assert(commands.received.size() == before_update_rejections);

  PortalResponse update_queued = portal.handle(update);
  assert(update_queued.status == 202);
  assert(update_queued.body.find("REQUEST_FIRMWARE_UPDATE") !=
         std::string::npos);
  assert(update_queued.body.find("\"state\":\"accepted\"") !=
         std::string::npos);
  assert(update_queued.body.find("\"offlineAfterAcceptance\":true") !=
         std::string::npos);
  assert(update_queued.body.find(
      "\"resultAfterReboot\":\"when_recorded\"") != std::string::npos);
  assert(commands.received.back().type ==
         PortalCommandType::RequestFirmwareUpdate);
  assert(commands.received.back().request_id != 0U);

  cache.firmware_update.accepted_offline = true;
  cache.firmware_update.phase = PortalFirmwareUpdatePhase::AcceptedOffline;
  PortalResponse accepted = portal.handle(state);
  assert(accepted.status == 200);
  assert(accepted.body.find("\"status\":\"accepted_offline\"") !=
         std::string::npos);
  PortalResponse update_busy = portal.handle(update);
  assert(update_busy.status == 409 && update_busy.retry_after_seconds == 5U);
  cache.firmware_update.accepted_offline = false;
  cache.firmware_update.phase = PortalFirmwareUpdatePhase::Ready;
  cache.firmware_update.code = PortalFirmwareUpdateCode::NetworkUnavailable;
  assert(portal.handle(state).body.find(
      "\"code\":\"network_unavailable\"") != std::string::npos);
  const std::pair<PortalFirmwareUpdateCode, const char*> public_codes[] = {
      {PortalFirmwareUpdateCode::None, "none"},
      {PortalFirmwareUpdateCode::UpToDate, "up_to_date"},
      {PortalFirmwareUpdateCode::NetworkUnavailable, "network_unavailable"},
      {PortalFirmwareUpdateCode::TimedOut, "timed_out"},
      {PortalFirmwareUpdateCode::ManifestRejected, "manifest_rejected"},
      {PortalFirmwareUpdateCode::ImageRejected, "image_rejected"},
      {PortalFirmwareUpdateCode::VerificationFailed, "verification_failed"},
      {PortalFirmwareUpdateCode::StagingFailed, "staging_failed"},
      {PortalFirmwareUpdateCode::InternalError, "internal_error"},
      {PortalFirmwareUpdateCode::UpdateConfirmed, "update_confirmed"},
      {PortalFirmwareUpdateCode::UpdateRolledBack, "update_rolled_back"},
  };
  for (const auto& expected : public_codes) {
    cache.firmware_update.code = expected.first;
    const PortalResponse coded = portal.handle(state);
    assert(coded.status == 200);
    assert(coded.body.find(std::string("\"code\":\"") + expected.second +
                           "\"") != std::string::npos);
  }
  cache.firmware_update.code = PortalFirmwareUpdateCode::ConfigurationInvalid;
  assert(portal.handle(state).status == 422);
  cache.firmware_update.code = PortalFirmwareUpdateCode::NetworkUnavailable;
  cache.firmware_update.accepted_offline = true;
  cache.firmware_update.phase = PortalFirmwareUpdatePhase::AcceptedOffline;
  assert(portal.handle(state).status == 422);
  assert(portal.handle(update).status == 422);
  cache.firmware_update.accepted_offline = false;
  cache.firmware_update.phase = PortalFirmwareUpdatePhase::Ready;
  cache.firmware_update.code = PortalFirmwareUpdateCode::None;
  cache.firmware_update.phase =
      static_cast<PortalFirmwareUpdatePhase>(255U);
  assert(portal.handle(state).status == 422);
  cache.firmware_update.phase = PortalFirmwareUpdatePhase::Ready;
  cache.state_result = PortalResult::Unavailable;
  assert(portal.handle(update).status == 503);
  cache.state_result = PortalResult::Ok;
  cache.firmware_update.configured = false;
  cache.firmware_update.phase = PortalFirmwareUpdatePhase::Unavailable;
  assert(portal.handle(update).status == 503);
  cache.firmware_update.code = PortalFirmwareUpdateCode::ConfigurationInvalid;
  assert(portal.handle(state).status == 200);
  assert(portal.handle(state).body.find(
      "\"code\":\"configuration_invalid\"") != std::string::npos);
  cache.firmware_update.code = PortalFirmwareUpdateCode::None;
  cache.firmware_update.configured = true;
  cache.firmware_update.phase = PortalFirmwareUpdatePhase::Ready;

  PortalRequest settings = authenticated("POST", "/api/settings", cookie);
  settings.body = "volume=72&led_brightness=44&voice_assistance=1&assistant_prompt=%E7%AE%80%E6%B4%81&image_prompt_template=%7Bprompt%7D&negative_prompt=watermark&storage_preference=internal&default_render_strategy=solid-clean&local_password=portal%20pass%2042&local_password_confirm=portal%20pass%2042";
  PortalResponse queued = portal.handle(settings);
  assert(queued.status == 202);
  assert(commands.received.back().type == PortalCommandType::UpdateSettings);
  assert(commands.received.back().settings.has_volume);
  assert(commands.received.back().settings.volume == 72);
  assert(commands.received.back().settings.has_led_maximum_brightness);
  assert(commands.received.back().settings.led_maximum_brightness_percent == 44);
  assert(commands.received.back().settings.assistant_prompt == "简洁");
  assert(commands.received.back().settings.asset_storage_preference == "internal");
  assert(commands.received.back().settings.default_render_strategy == "solid-clean");
  assert(commands.received.back().settings.local_management_password_override == "portal pass 42");

  PortalRequest myai_settings_request =
      authenticated("POST", "/api/settings", cookie);
  myai_settings_request.body =
      "assistant_prompt=Inkloop+helper&image_prompt_template=Six-color+%7Bprompt%7D&negative_prompt=watermark";
  assert(portal.handle(myai_settings_request).status == 202);
  const PortalSettingsPatch& myai_patch = commands.received.back().settings;
  assert(myai_patch.has_assistant_prompt &&
         myai_patch.assistant_prompt == "Inkloop helper");
  assert(myai_patch.has_image_prompt_template &&
         myai_patch.image_prompt_template == "Six-color {prompt}");
  assert(myai_patch.has_negative_prompt &&
         myai_patch.negative_prompt == "watermark");
  assert(!myai_patch.has_volume && !myai_patch.has_led_maximum_brightness &&
         !myai_patch.has_asset_storage_preference &&
         !myai_patch.has_local_management_password_override);

  PortalRequest invalid_utf8 = authenticated("POST", "/api/settings", cookie);
  invalid_utf8.body = "assistant_prompt=%FF";
  assert(portal.handle(invalid_utf8).status == 422);
  PortalRequest long_prompt = authenticated("POST", "/api/settings", cookie);
  long_prompt.body = "assistant_prompt=" + std::string(513, 'x');
  assert(portal.handle(long_prompt).status == 422);
  PortalRequest bad_led = authenticated("POST", "/api/settings", cookie);
  bad_led.body = "led_brightness=101";
  assert(portal.handle(bad_led).status == 422);
  PortalRequest reset_password = authenticated("POST", "/api/settings", cookie);
  reset_password.body = "reset_local_password=1";
  assert(portal.handle(reset_password).status == 202);
  assert(commands.received.back().settings.has_local_management_password_override);
  assert(commands.received.back().settings.local_management_password_override.empty());
  PortalRequest bad_password = authenticated("POST", "/api/settings", cookie);
  bad_password.body = "local_password=abcdefgh&local_password_confirm=abcdefgi";
  assert(portal.handle(bad_password).status == 422);

  PortalRequest preview_volume = authenticated("POST", "/api/audio/preview", cookie);
  preview_volume.body = "volume=18";
  assert(portal.handle(preview_volume).status == 202);
  assert(commands.received.back().type == PortalCommandType::PreviewVolume);
  assert(commands.received.back().volume == 18);

  const std::pair<const char*, PortalCommandType> simple[] = {
      {"/api/onboarding/myai/start", PortalCommandType::StartMyAiPairing},
      {"/api/onboarding/myai/rebind", PortalCommandType::RebindMyAi},
      {"/api/tutorial/restart", PortalCommandType::RestartMyAiTutorial},
      {"/api/chat/clear", PortalCommandType::ClearLocalChat},
  };
  for (const auto& route : simple) {
    PortalRequest action = authenticated("POST", route.first, cookie);
    assert(portal.handle(action).status == 202);
    assert(commands.received.back().type == route.second);
  }

  PortalRequest display = authenticated("POST", "/api/album/display", cookie);
  display.body = "asset_id=asset-1";
  assert(portal.handle(display).status == 202);
  assert(commands.received.back().type == PortalCommandType::DisplayAlbumItem);
  assert(commands.received.back().asset_id == "asset-1");
  PortalRequest traversal = display;
  traversal.body = "asset_id=..%2Fsecret";
  assert(portal.handle(traversal).status == 422);

  PortalRequest render = authenticated("POST", "/api/album/render", cookie);
  render.body = "asset_id=asset-1&render_strategy=solid-clean";
  assert(portal.handle(render).status == 202);
  assert(commands.received.back().type == PortalCommandType::SetAlbumRenderStrategy);

  PortalRequest generate = authenticated("POST", "/api/aigc/generate", cookie);
  generate.body = "prompt=%E9%AB%98%E5%AF%B9%E6%AF%94%E6%B5%B7%E6%8A%A5";
  assert(portal.handle(generate).status == 202);
  assert(commands.received.back().type == PortalCommandType::GenerateImage);
  assert(commands.received.back().prompt == "高对比海报");

  PortalResponse album = portal.handle(authenticated(
      "GET", "/api/album?cursor=&limit=8", cookie));
  assert(album.status == 200);
  assert(album.body.find("asset-1") != std::string::npos);
  assert(album.body.find("\"maximumItems\":96") != std::string::npos);
  cache.bad_album_total = true;
  assert(portal.handle(authenticated("GET", "/api/album", cookie)).status == 422);
  cache.bad_album_total = false;

  PortalResponse preview = portal.handle(authenticated(
      "GET", "/api/album/preview?asset_id=asset-1", cookie));
  assert(preview.status == 200);
  assert(preview.disposition == ResponseDisposition::StreamAlbumPreview);
  assert(preview.stream.asset_id == "asset-1");

  PortalRequest upload = authenticated(
      "POST", "/api/album/upload?title=%E6%96%B0%E5%9B%BE", cookie);
  upload.content_type = "image/png";
  upload.content_length = 1024;
  PortalResponse upload_response = portal.handle(upload);
  assert(upload_response.status == 202);
  assert(upload_response.disposition == ResponseDisposition::StreamAlbumUpload);
  assert(upload_response.stream.upload_title == "新图");
  assert(upload_response.stream.content_length == 1024);
  upload.content_length = kMaximumAlbumUploadBytes + 1;
  assert(portal.handle(upload).status == 413);
  upload.content_length = 10;
  upload.content_type = "multipart/form-data";
  assert(portal.handle(upload).status == 413);

  PortalResponse chat = portal.handle(authenticated(
      "GET", "/api/chat?after=0&limit=24", cookie));
  assert(chat.status == 200);
  assert(chat.body.find("请生成高对比海报") != std::string::npos);
  assert(chat.body.find("已经排队") != std::string::npos);
  assert(chat.body.find("blank_audio") == std::string::npos);
  assert(chat.body.find("audio_base64") == std::string::npos);
  assert(chat.body.find("\"retention\":\"local\"") != std::string::npos);
  assert(chat.body.find("\"maximumItems\":4096") != std::string::npos);
  cache.bad_chat_total = true;
  assert(portal.handle(authenticated("GET", "/api/chat", cookie)).status == 422);
  cache.bad_chat_total = false;

  Cache minimal_cache;
  minimal_cache.minimal_capabilities = true;
  Commands minimal_commands;
  PortalCore minimal_portal(access(), minimal_cache, minimal_commands);
  assert(minimal_portal.ready());
  const std::string minimal_cookie = login(minimal_portal);
  PortalResponse minimal_state = minimal_portal.handle(
      authenticated("GET", "/api/state", minimal_cookie));
  assert(minimal_state.status == 200);
  assert(minimal_state.body.find("\"microphone\":false") != std::string::npos);
  assert(minimal_state.body.find("\"speaker\":false") != std::string::npos);
  assert(minimal_state.body.find("\"duplexAudio\":false") != std::string::npos);
  assert(minimal_state.body.find("\"rgbPixels\":0") != std::string::npos);
  assert(minimal_state.body.find("\"removableStorage\":false") != std::string::npos);
  assert(minimal_state.body.find("\"official-quality\"") != std::string::npos);
  assert(minimal_state.body.find("\"classic-six-color\"") != std::string::npos);
  assert(minimal_state.body.find("\"solid-clean\"") != std::string::npos);
  assert(minimal_state.body.find("reflectance-photo") == std::string::npos);

  const size_t before_rejections = minimal_commands.received.size();
  PortalRequest minimal_preview = authenticated(
      "POST", "/api/audio/preview", minimal_cookie);
  minimal_preview.body = "volume=18";
  assert(minimal_portal.handle(minimal_preview).status == 422);
  PortalRequest minimal_volume = authenticated(
      "POST", "/api/settings", minimal_cookie);
  minimal_volume.body = "volume=18";
  assert(minimal_portal.handle(minimal_volume).status == 422);
  PortalRequest minimal_voice = authenticated(
      "POST", "/api/settings", minimal_cookie);
  minimal_voice.body = "voice_assistance=1";
  assert(minimal_portal.handle(minimal_voice).status == 422);
  PortalRequest minimal_led = authenticated(
      "POST", "/api/settings", minimal_cookie);
  minimal_led.body = "led_brightness=18";
  assert(minimal_portal.handle(minimal_led).status == 422);
  PortalRequest minimal_tutorial = authenticated(
      "POST", "/api/tutorial/restart", minimal_cookie);
  assert(minimal_portal.handle(minimal_tutorial).status == 422);
  PortalRequest minimal_storage = authenticated(
      "POST", "/api/settings", minimal_cookie);
  minimal_storage.body = "storage_preference=removable";
  assert(minimal_portal.handle(minimal_storage).status == 422);
  PortalRequest minimal_default = authenticated(
      "POST", "/api/settings", minimal_cookie);
  minimal_default.body = "default_render_strategy=reflectance-photo";
  assert(minimal_portal.handle(minimal_default).status == 422);
  PortalRequest minimal_render = authenticated(
      "POST", "/api/album/render", minimal_cookie);
  minimal_render.body =
      "asset_id=asset-1&render_strategy=reflectance-photo";
  assert(minimal_portal.handle(minimal_render).status == 422);
  assert(minimal_commands.received.size() == before_rejections);

  minimal_default.body = "default_render_strategy=solid-clean";
  assert(minimal_portal.handle(minimal_default).status == 202);
  assert(minimal_commands.received.back().type ==
         PortalCommandType::UpdateSettings);
  minimal_render.body = "asset_id=asset-1&render_strategy=solid-clean";
  assert(minimal_portal.handle(minimal_render).status == 202);
  assert(minimal_commands.received.back().type ==
         PortalCommandType::SetAlbumRenderStrategy);

  commands.result = PortalResult::Busy;
  const size_t before_busy_update = commands.received.size();
  PortalResponse busy_update_response = portal.handle(update);
  assert(busy_update_response.status == 409 &&
         busy_update_response.retry_after_seconds == 1);
  assert(commands.received.size() == before_busy_update);
  PortalRequest busy = authenticated("POST", "/api/audio/preview", cookie);
  busy.body = "volume=22";
  PortalResponse busy_response = portal.handle(busy);
  assert(busy_response.status == 409 && busy_response.retry_after_seconds == 1);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-portal-"));
  try {
    const source = join(scratch, "portal.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I",
      join(core, "include"),
      source,
      join(core, "portal_core.cpp"),
      "-o",
      binary,
    ];
    if (sanitized) {
      args.splice(
        1,
        0,
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
      );
    }
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("native Portal core preserves bounded Arduino WebUI behavior", () => {
  buildAndRun(false);
});

test("native Portal core survives adversarial requests under ASan/UBSan", () => {
  buildAndRun(true);
});

test("ESP-IDF adapter keeps slow work outside HTTP handlers", () => {
  const header = readFileSync(
    join(idf, "include/inkloop/portal/esp_portal_server.hpp"),
    "utf8",
  );
  const source = readFileSync(join(idf, "esp_portal_server.cpp"), "utf8");
  const cmake = readFileSync(join(idf, "CMakeLists.txt"), "utf8");
  const combined = `${header}\n${source}`;
  assert.match(source, /httpd_req_async_handler_begin/);
  assert.match(source, /xQueueSend\(preview_queue/);
  assert.match(source, /previewLoop\(\)/);
  assert.match(source, /httpd_resp_send_chunk/);
  assert.match(source, /std::array<uint8_t, kReceiveChunkBytes>/);
  assert.match(source, /retryBusyUntil/);
  assert.match(header, /http_task_stack_bytes = 8192U/);
  assert.match(source, /native\.stack_size = config\.http_task_stack_bytes/);
  assert.match(source, /kQueueBackpressureDeadlineUs = 30000000LL/);
  assert.match(header, /tryBegin/);
  assert.match(header, /tryWrite/);
  assert.match(header, /tryFinish/);
  assert.match(cmake, /esp_http_server/);
  assert.match(cmake, /inkloop_portal/);
  assert.doesNotMatch(combined, /esp_http_client|MyAiClient|DisplayController|LocalChatLog|AlbumStore/);
  assert.doesNotMatch(source, /std::vector\s*<\s*uint8_t|malloc\s*\(|realloc\s*\(/);
});

test("Portal state snapshots stay off the constrained HTTP task stack", () => {
  const stateRender = portalSource.slice(
    portalSource.indexOf("PortalResponse PortalCore::renderState"),
    portalSource.indexOf("PortalResponse PortalCore::renderAlbum", portalSource.indexOf("PortalResponse PortalCore::renderState")),
  );
  assert.match(stateRender, /new \(std::nothrow\) PortalStateSnapshot/);
  assert.doesNotMatch(stateRender, /\n\s*PortalStateSnapshot state;/);
});

test("1.5 MiB fast upload tolerates the bounded slow Portal consumer", () => {
  buildAndRunFlowHarness();
});

test("embedded Portal browser scripts remain syntactically valid", () => {
  const html = portalSource.split('R"INKLOOP(')[1]?.split(')INKLOOP"')[0];
  assert.ok(html);
  const scripts = html.split("<script>").slice(1).map(
    (part) => part.split("</script>")[0],
  );
  assert.equal(scripts.length, 7);
  assert.match(html, /state\.displayWidth/);
  assert.match(html, /state\.displayHeight/);
  assert.match(html, /canvas\.style\.aspectRatio/);
  assert.match(html, /runtimeTelemetry/);
  assert.match(html, /storageCapacitySignature/);
  assert.match(html, /root\.querySelector\('\[data-storage-capacity\]'\)/);
  assert.match(html, /new MutationObserver\(render\)\.observe\(root/);
  assert.match(html, /free<=total/);
  assert.match(html, /else\{stop\(\);timer=setTimeout\(poll,cadence\)\}/);
  assert.match(html, /runtime-diagnostics/);
  assert.match(html, /运行诊断暂不可用/);
  assert.match(html, /applyCapabilities\(state\.capabilities,f\)/);
  assert.match(html, /window\.__inkloopCapabilities/);
  assert.match(html, /c\.duplexAudio/);
  assert.match(html, /c\.rgbPixels>0/);
  assert.match(html, /if\(c\.removableStorage\)storage\.push/);
  assert.match(html, /list\.map\(x=>/);
  assert.match(html, /firmware-update-state/);
  assert.match(html, /firmwareUpdate/);
  assert.match(html, /\/api\/system\/update/);
  assert.match(html, /confirm\('请求签名固件更新/);
  assert.match(html, /confirm:'install-signed-firmware'/);
  assert.match(html, /!snapshot\.configured\|\|snapshot\.acceptedOffline/);
  assert.match(html, /offlineAfterAcceptance!==true/);
  assert.match(html, /resultAfterReboot!==['"]when_recorded['"]/);
  assert.match(html, /本地 Portal 会离线/);
  assert.match(html, /终态成功记录时/);
  assert.equal((html.match(/\/api\/system\/update/g) ?? []).length, 1);
  const firmwareScript = scripts.at(-1);
  const clickAt = firmwareScript.indexOf("button.onclick=");
  const confirmAt = firmwareScript.indexOf("confirm('", clickAt);
  const postAt = firmwareScript.indexOf("'/api/system/update'", clickAt);
  assert.ok(clickAt >= 0 && confirmAt > clickAt && postAt > confirmAt);
  assert.doesNotMatch(firmwareScript.slice(0, clickAt), /\/api\/system\/update/);
  assert.doesNotMatch(
    firmwareScript,
    /\bchecking\b|\bdownloading\b|\bstaging\b|\breboot_pending\b|正在检查|正在下载|正在验证并准备/,
  );
  assert.doesNotMatch(html, /\[\['official-quality','官方高质量'\]/);
  assert.doesNotMatch(html, /<option value="reflectance-photo">/);
  assert.doesNotMatch(html, /setInterval\s*\(/);
  for (const script of scripts) assert.doesNotThrow(() => new Function(script));
});

test("firmware update Portal contract stays bounded and HTTP-task safe", () => {
  const snapshotStart = portalHeader.indexOf(
    "struct PortalFirmwareUpdateSnapshot",
  );
  const snapshotEnd = portalHeader.indexOf(
    "struct PortalSettingsSnapshot",
    snapshotStart,
  );
  assert.ok(snapshotStart >= 0 && snapshotEnd > snapshotStart);
  const snapshot = portalHeader.slice(snapshotStart, snapshotEnd);
  assert.match(snapshot, /bool configured = false/);
  assert.match(snapshot, /bool accepted_offline = false/);
  assert.match(snapshot, /PortalFirmwareUpdatePhase phase/);
  assert.match(snapshot, /PortalFirmwareUpdateCode code/);
  assert.doesNotMatch(snapshot, /std::string|std::array|std::vector|url|key|token|signature|manifest/i);

  const handlerStart = portalSource.indexOf(
    "PortalResponse PortalCore::requestFirmwareUpdate",
  );
  const handlerEnd = portalSource.indexOf(
    "PortalResponse PortalCore::enqueueSimple",
    handlerStart,
  );
  assert.ok(handlerStart >= 0 && handlerEnd > handlerStart);
  const handler = portalSource.slice(handlerStart, handlerEnd);
  assert.match(handler, /cache_\.readState\(state\)/);
  assert.match(handler, /PortalCommandType::RequestFirmwareUpdate/);
  assert.match(handler, /enqueueCommand\(std::move\(command\)\)/);
  assert.doesNotMatch(
    handler,
    /esp_http_client|esp_ota|esp_partition|https?:\/\/|manifest|signature|esp_restart|vTaskDelay|sleep/i,
  );

  const enqueueStart = portalSource.indexOf(
    "PortalResponse PortalCore::enqueueCommand",
  );
  const enqueueEnd = portalSource.indexOf(
    "uint64_t PortalCore::nextRequestId",
    enqueueStart,
  );
  const enqueue = portalSource.slice(enqueueStart, enqueueEnd);
  assert.match(enqueue, /state\\\":\\\"accepted/);
  assert.match(enqueue, /offlineAfterAcceptance\\\":true/);
  assert.match(enqueue, /resultAfterReboot\\\":\\\"when_recorded/);
});
