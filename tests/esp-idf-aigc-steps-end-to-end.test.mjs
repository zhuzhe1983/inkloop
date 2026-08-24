import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const components = join(repo, "firmware/inkloop-idf/components");
const portal = join(components, "inkloop_portal");
const diagnostics = join(components, "inkloop_diagnostics");
const settings = join(components, "inkloop_settings");
const product = join(components, "inkloop_product");
const myai = join(components, "inkloop_myai");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "inkloop/myai/CanonicalJsonCodec.h"
#include "inkloop/native_device_aigc_settings.hpp"
#include "inkloop/portal/portal_core.hpp"
#include "inkloop/settings/device_settings.hpp"
#include "inkloop/settings/settings_extension_journal.hpp"
#include "inkloop/settings/settings_journal.hpp"

using namespace inkloop;

struct Cache final : portal::IPortalReadCache {
  portal::PortalResult readState(
      portal::PortalStateSnapshot& output) const override {
    output.capabilities.render_strategy_count = 1U;
    output.capabilities.render_strategies[0].id = "official-quality";
    output.capabilities.render_strategies[0].display_name =
        "Official quality";
    return portal::PortalResult::Ok;
  }
  portal::PortalResult readAlbumPage(
      const portal::AlbumPageQuery&, portal::AlbumPage&) const override {
    return portal::PortalResult::Unavailable;
  }
  portal::PortalResult readLocalChatPage(
      const portal::ChatPageQuery&, portal::ChatPage&) const override {
    return portal::PortalResult::Unavailable;
  }
};

struct Commands final : portal::IPortalCommandQueue {
  std::vector<portal::PortalCommand> received;

  portal::PortalResult tryEnqueue(
      const portal::PortalCommand& command) override {
    received.push_back(command);
    return portal::PortalResult::Ok;
  }
};

struct Journal final : settings::ISettingsJournalStore {
  settings::SettingsJournalState state;

  Journal() { state.namespace_available = true; }

  settings::SettingsStatus inspect(
      settings::SettingsJournalState& output) override {
    output = state;
    return settings::SettingsStatus::success();
  }

  settings::SettingsStatus writeSlotAndCommit(
      std::uint8_t slot,
      const std::vector<std::uint8_t>& encoded) override {
    state.slot_present[slot] = true;
    state.slot[slot] = encoded;
    return settings::SettingsStatus::success();
  }

  settings::SettingsStatus writeHeadAndMarkerAndCommit(
      std::uint32_t generation) override {
    state.head_present = true;
    state.head_generation = generation;
    state.marker_present = true;
    state.marker_valid = true;
    return settings::SettingsStatus::success();
  }
};

struct ExtensionJournal final : settings::ISettingsExtensionJournalStore {
  settings::SettingsExtensionJournalState state;

  ExtensionJournal() { state.namespace_available = true; }

  settings::SettingsStatus inspect(
      settings::SettingsExtensionJournalState& output) override {
    output = state;
    return settings::SettingsStatus::success();
  }

  settings::SettingsStatus writeSlot(
      std::uint8_t slot,
      const std::vector<std::uint8_t>& encoded) override {
    assert(slot < 2U);
    state.slot_present[slot] = true;
    state.slot[slot] = encoded;
    return settings::SettingsStatus::success();
  }

  settings::SettingsStatus writeHead(
      std::uint32_t sequence) override {
    assert(sequence != 0U);
    state.head_present = true;
    state.head_sequence = sequence;
    return settings::SettingsStatus::success();
  }
};

portal::PortalAccessConfig access() {
  portal::PortalAccessConfig value;
  value.access_code = "secret99";
  value.session_id = "session_abcdefghijklmnopqrstuvwxyz";
  value.csrf_token = "csrf_abcdefghijklmnopqrstuvwxyz0123";
  value.allowed_hosts = {"inkloop.local"};
  value.allowed_origins = {"http://inkloop.local"};
  return value;
}

portal::PortalRequest request(std::string method, std::string path,
                              std::uint64_t now_seconds) {
  portal::PortalRequest value;
  value.method = std::move(method);
  value.path = std::move(path);
  value.host = "inkloop.local";
  value.origin = "http://inkloop.local";
  value.peer_is_local = true;
  value.now_seconds = now_seconds;
  return value;
}

std::string login(portal::PortalCore& core) {
  portal::PortalRequest login_request =
      request("POST", "/api/session", 100U);
  login_request.content_type = "application/x-www-form-urlencoded";
  login_request.body = "nonce=secret99";
  login_request.content_length = login_request.body.size();
  const portal::PortalResponse response = core.handle(login_request);
  assert(response.status == 200);
  const std::size_t separator = response.set_cookie.find(';');
  assert(separator != std::string::npos);
  return response.set_cookie.substr(0U, separator);
}

int main() {
  Cache cache;
  Commands commands;
  portal::PortalCore core(access(), cache, commands);
  assert(core.ready());
  const std::string cookie = login(core);

  portal::PortalRequest update =
      request("POST", "/api/settings", 101U);
  update.cookie = cookie;
  update.csrf_token = access().csrf_token;
  update.content_type = "application/x-www-form-urlencoded";
  update.body = "image_steps=37";
  update.content_length = update.body.size();
  assert(core.handle(update).status == 202);
  assert(commands.received.size() == 1U);
  const portal::PortalSettingsPatch& patch =
      commands.received.front().settings;
  assert(patch.has_image_generation_steps);
  assert(patch.image_generation_steps == 37U);

  Journal journal;
  ExtensionJournal extension_journal;
  const settings::DeviceSettings defaults =
      settings::makeGenericDeviceDefaults();
  settings::SettingsStoreCore owner_store(journal, defaults);
  settings::SettingsExtensionStoreCore owner_extension_store(
      extension_journal);
  settings::SettingsSnapshot owner_snapshot;
  assert(settings::loadRollbackCompatibleSettings(
      owner_store, owner_extension_store, owner_snapshot).ok());
  settings::DeviceSettings next = owner_snapshot.values;
  applyNativeDeviceAigcSettingsPatch(patch, next);
  settings::SettingsSnapshot committed;
  assert(settings::saveRollbackCompatibleSettings(
      owner_store, owner_extension_store, next,
      owner_snapshot.generation, committed).ok());
  // Steps are extension-only. The rollback-compatible schema-2 main journal
  // remains untouched while ext-head commits the new durable value.
  assert(committed.generation == 0U);
  assert(extension_journal.state.head_present);
  assert(extension_journal.state.head_sequence == 1U);
  assert(nativeDeviceAigcSteps(committed.values) == 37U);

  // Exercise a reboot-style reload so the request cannot accidentally read
  // the transient Portal command instead of the journal winner.
  settings::SettingsStoreCore rebooted_owner_store(journal, defaults);
  settings::SettingsExtensionStoreCore rebooted_extension_store(
      extension_journal);
  settings::SettingsSnapshot reloaded;
  assert(settings::loadRollbackCompatibleSettings(
      rebooted_owner_store, rebooted_extension_store, reloaded).ok());
  assert(reloaded.generation == 0U);
  const std::uint8_t configured_steps =
      nativeDeviceAigcSteps(reloaded.values);
  assert(configured_steps == 37U);

  myai::ImageRequest image_request;
  image_request.prompt = "six-color test image";
  image_request.steps = configured_steps;
  const myai::CanonicalJsonCodec codec("m5-papercolor-c151");
  const std::string json = codec.aigcGenerateBody(
      "669191", "0C:DA:43:85:84:28", image_request);
  const std::string expected = "\"steps\":37";
  assert(json.find(expected) != std::string::npos);
  assert(json.find(expected) == json.rfind(expected));
  assert(json.find("\"steps\":20") == std::string::npos);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-aigc-steps-e2e-"));
  try {
    const source = join(scratch, "aigc_steps_e2e.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I", join(portal, "include"),
      "-I", join(diagnostics, "include"),
      "-I", join(settings, "include"),
      "-I", join(product, "include"),
      "-I", join(myai, "include"),
      "-I", join(myai, "include/inkloop/myai"),
      source,
      join(portal, "portal_core.cpp"),
      join(diagnostics, "diagnostic_detail.cpp"),
      join(settings, "device_settings.cpp"),
      join(settings, "settings_journal.cpp"),
      join(settings, "settings_extension_journal.cpp"),
      join(myai, "CanonicalJsonCodec.cpp"),
      "-o", binary,
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

test("Portal steps persist through the native device owner path into MyAI JSON", () => {
  buildAndRun(false);
});

test("AIGC steps end-to-end path is clean under ASan and UBSan", () => {
  buildAndRun(true);
});
