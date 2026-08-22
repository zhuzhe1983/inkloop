import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo, "firmware/inkloop-idf/components/inkloop_connectivity");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <string>
#include "inkloop/wifi_station_core.hpp"

using namespace inkloop;

int main() {
  WifiStationCore empty;
  assert(empty.begin(false, 10) == WifiStationAction::RequireProvisioning);
  assert(empty.phase() == WifiStationPhase::NoCredentials);
  assert(empty.tick(10) == WifiStationAction::RequireProvisioning);
  assert(empty.phase() == WifiStationPhase::ProvisioningRequired);

  WifiStationCore saved;
  assert(saved.begin(true, 100) == WifiStationAction::Connect);
  saved.connectStarted(100, true);
  assert(saved.phase() == WifiStationPhase::Connecting);
  saved.disconnected(201, false, 200);
  assert(saved.phase() == WifiStationPhase::RetryWaiting);
  assert(saved.tick(2199) == WifiStationAction::None);
  assert(saved.tick(2200) == WifiStationAction::Connect);
  saved.connectStarted(2200, true);
  saved.connected();
  assert(saved.online());
  assert(saved.retryCount() == 0);
  saved.disconnected(203, false, 1000000);
  assert(saved.phase() == WifiStationPhase::RetryWaiting);
  assert(saved.tick(1002000) == WifiStationAction::Connect);
  saved.connectStarted(1002000, true);
  saved.connected();
  assert(saved.online());

  WifiStationCore rejected;
  assert(rejected.begin(true, 0) == WifiStationAction::Connect);
  rejected.connectStarted(0, true);
  rejected.disconnected(202, true, 100);
  assert(rejected.phase() == WifiStationPhase::RetryWaiting);
  rejected.disconnected(202, true, 200);
  assert(rejected.phase() == WifiStationPhase::ProvisioningRequired);

  WifiStationCore timeout;
  assert(timeout.begin(true, UINT32_MAX - 1000U) ==
         WifiStationAction::Connect);
  timeout.connectStarted(UINT32_MAX - 1000U, true);
  assert(timeout.tick(23998U) == WifiStationAction::None);
  assert(timeout.tick(23999U) == WifiStationAction::None);
  assert(timeout.phase() == WifiStationPhase::Connecting);

  WifiStationPolicy custom;
  custom.saved_connect_timeout_ms = 100;
  WifiStationCore failed(custom);
  assert(failed.begin(true, 0) == WifiStationAction::Connect);
  failed.connectStarted(0, false);
  assert(failed.phase() == WifiStationPhase::Failed);
  assert(std::string(wifiStationPhaseName(failed.phase())) == "FAILED");
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-wifi-"));
  try {
    const source = join(scratch, "wifi_station_harness.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(component, "include"), source,
      join(component, "wifi_station_core.cpp"), "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    );
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

test("saved Wi-Fi policy is silent, bounded and wrap-safe", () => {
  buildAndRun(false);
  buildAndRun(true);
});

test("native station owner owns credential provisioning without broad reset", () => {
  const source = readFileSync(join(component, "esp_wifi_station.cpp"), "utf8");
  const header = readFileSync(join(
    component, "include/inkloop/esp_wifi_station.hpp"), "utf8");
  const combined = `${header}\n${source}`;
  assert.match(source, /esp_wifi_get_config\(WIFI_IF_STA/);
  assert.match(source, /esp_wifi_set_storage\(WIFI_STORAGE_FLASH\)/);
  assert.match(source, /esp_wifi_connect\(\)/);
  assert.match(source, /WIFI_EVENT_STA_DISCONNECTED/);
  assert.match(source, /IP_EVENT_STA_GOT_IP/);
  assert.equal((source.match(/esp_wifi_set_config\(WIFI_IF_STA/g) ?? []).length, 1);
  assert.match(source, /submitted Wi-Fi credentials persisted; connecting/);
  assert.doesNotMatch(source, /esp_wifi_restore|nvs_erase|nvs_flash_erase|format/);
  assert.doesNotMatch(combined, /passphrase|Arduino\.h|WiFiManager/);
  assert.doesNotMatch(source, /ESP_LOG\w*\([^\n]*(submitted_password_|sta\.password)/);
});

test("native station starts non-blocking Shanghai clock synchronization", () => {
  const source = readFileSync(join(component, "esp_wifi_station.cpp"), "utf8");
  assert.match(source, /setenv\("TZ", "CST-8"/);
  assert.match(source, /ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE\([\s\S]*ntp\.aliyun\.com[\s\S]*pool\.ntp\.org[\s\S]*time\.cloudflare\.com/);
  assert.match(source, /esp_netif_sntp_init/);
  assert.match(source, /NETIF_SNTP_TIME_SYNC/);
  assert.doesNotMatch(source, /esp_netif_sntp_sync_wait|vTaskDelay|sleep\(/);
});
