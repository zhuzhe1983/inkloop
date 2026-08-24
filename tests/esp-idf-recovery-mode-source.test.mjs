import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const recovery = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_recovery",
);
const connectivity = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_connectivity",
);
const ownerHeader = readFileSync(
  join(recovery, "include/inkloop/recovery/recovery_network_owner.hpp"),
  "utf8",
);
const owner = readFileSync(join(recovery, "recovery_network_owner.cpp"), "utf8");
const config = readFileSync(join(recovery, "recovery_network_config.cpp"), "utf8");
const serverHeader = readFileSync(
  join(recovery, "include/inkloop/recovery/esp_recovery_server.hpp"),
  "utf8",
);
const portalHeader = readFileSync(
  join(recovery, "include/inkloop/recovery/recovery_portal.hpp"),
  "utf8",
);
const portal = readFileSync(join(recovery, "recovery_portal.cpp"), "utf8");
const wifiHeader = readFileSync(
  join(connectivity, "include/inkloop/esp_wifi_station.hpp"),
  "utf8",
);
const wifi = readFileSync(join(connectivity, "esp_wifi_station.cpp"), "utf8");
const cmake = readFileSync(join(recovery, "CMakeLists.txt"), "utf8");
const main = readFileSync(
  join(repo, "firmware/inkloop-idf/main/app_main.cpp"),
  "utf8",
);

test("recovery owner composes only fixed diagnostics and sole Wi-Fi owner", () => {
  assert.match(ownerHeader, /const IRecoveryDiagnosticCache& cache_/);
  assert.match(ownerHeader, /std::unique_ptr<EspWifiStationOwner> wifi_/);
  assert.match(owner, /wifi_->initialize\(now_ms\)/);
  assert.match(owner, /wifi_->tick\(now_ms\)/);
  assert.match(owner, /WifiStationPhase::Online/);
  assert.match(owner, /wifi\.provisioning_ap/);
  assert.match(owner, /wifi\.provisioning_ipv4/);
  assert.doesNotMatch(
    `${ownerHeader}\n${owner}`,
    /ProductRuntime|NativePortalOwner|NativeVoice|NativeDisplay|MyAi|AlbumStore|TaskStore|LocalChatLog|inkloop_storage|inkloop_product|inkloop_ota/,
  );
});

test("port 8080 coexists with port-80 provisioning and exact actual IP", () => {
  assert.match(serverHeader, /port = kRecoveryHttpPort/);
  assert.match(owner, /config\.port = kRecoveryHttpPort/);
  assert.match(owner, /mdns_service_add[\s\S]*kRecoveryHttpPort/);
  assert.match(config, /allowed_host_count = 2U/);
  assert.match(config, /allowed_origin_count = 2U/);
  assert.match(config, /std::string\(kRecoveryMdnsHost\) \+ ":" \+ port/);
  assert.match(wifiHeader, /provisioning_ipv4/);
  assert.match(wifi, /esp_netif_get_ip_info\(ap_netif_, &ap_ip\)/);
  assert.match(wifi, /provisioning_ipv4_ = actual_ipv4/);
  assert.doesNotMatch(`${owner}\n${config}`, /192\.168\.4\.1|0\.0\.0\.0|"\*"/);
  assert.match(portal, /http:\/\/inkloop\.local:8080\//);
  assert.match(portal, /location\.origin\+'\/'/);
  assert.match(portal, /http:\/\/inkloop\.local\//);
});

test("credentials derive from Wi-Fi and boot CSPRNG then scrub on stop", () => {
  assert.match(owner, /wifi_->localAccessCode\(\)/);
  assert.match(owner, /esp_fill_random\(entropy\.data\(\), entropy\.size\(\)\)/);
  assert.match(owner, /session_token = bootToken\(\)/);
  assert.match(owner, /csrf_token = bootToken\(\)/);
  assert.match(owner, /secureZero\(local_access\)/);
  assert.match(owner, /scrubAccess\(access\)/);
  assert.match(owner, /secureZero\(guidance_\.mdns_url\)/);
  assert.match(portalHeader, /~RecoveryPortalCore\(\)/);
  assert.match(portal, /RecoveryPortalCore::~RecoveryPortalCore\(\)/);
  assert.match(portal, /scrub\(access_\.access_code\)/);
  assert.match(portal, /scrub\(access_\.session_id\)/);
  assert.match(portal, /scrub\(access_\.csrf_token\)/);
  assert.match(owner, /esp_err_t RecoveryNetworkModeOwner::stop\(\)/);
  assert.match(owner, /stopRecoveryNetworkOwners\(/);
  assert.match(owner, /if \(status != ESP_OK\) return status/);
  assert.match(owner, /stop_requested_ = true/);
  assert.match(owner, /stop_requested_ = false/);
  assert.match(owner, /!initialized_ \|\| !wifi_ \|\| stop_requested_/);
  assert.doesNotMatch(
    owner.match(/esp_err_t RecoveryNetworkModeOwner::stop\(\)[\s\S]*?\n\}/)?.[0] ?? "",
    /wifi_\.reset\(\)/,
  );
  const wifiShutdown = wifi.slice(
    wifi.indexOf("esp_err_t EspWifiStationOwner::shutdown()"),
    wifi.indexOf("bool EspWifiStationOwner::credentialFailure"),
  );
  for (const boundary of [
    /provisioning_server_->stop\(\)/,
    /esp_event_handler_instance_unregister\([\s\S]*NETIF_SNTP_EVENT/,
    /esp_event_handler_instance_unregister\([\s\S]*WIFI_EVENT/,
    /esp_event_handler_instance_unregister\([\s\S]*IP_EVENT/,
    /esp_wifi_stop\(\)/,
    /esp_wifi_deinit\(\)/,
  ]) assert.match(wifiShutdown, boundary);
  assert.doesNotMatch(owner, /access_code\s*=\s*"|session_id\s*=\s*"|csrf_token\s*=\s*"/);
  for (const line of owner.split("\n").filter((value) => value.includes("ESP_LOG"))) {
    assert.doesNotMatch(line, /access|session|csrf|code/i);
  }
});

test("owner is bounded caller-driven and does no mutation or outbound work", () => {
  assert.match(ownerHeader, /void tick\(uint32_t now_ms\)/);
  assert.doesNotMatch(
    owner,
    /xTaskCreate|xTimerCreate|esp_timer_create|while\s*\(\s*true\s*\)|for\s*\(\s*;;\s*\)|vTaskDelay|esp_http_client|esp_websocket_client/,
  );
  assert.doesNotMatch(
    `${ownerHeader}\n${owner}\n${config}`,
    /nvs_(?:set|erase|commit)|fopen|fwrite|unlink|mkfs|format|migrate\s*\(/,
  );
  assert.match(owner, /kStartRetryIntervalMs = 1000U/);
  assert.match(owner, /next_start_attempt_ms_ = now_ms \+ kStartRetryIntervalMs/);
  assert.match(cmake, /inkloop_connectivity/);
  assert.match(cmake, /mdns/);
  assert.doesNotMatch(cmake, /inkloop_product|inkloop_storage|inkloop_ota|inkloop_portal|inkloop_voice|inkloop_display/);
});

test("all Recovery Wi-Fi is RAM-only and cannot mutate NVS", () => {
  assert.match(ownerHeader,
    /RecoveryWifiStoragePolicy::VolatileRam/);
  assert.doesNotMatch(ownerHeader,
    /RecoveryWifiStoragePolicy[\s\S]*PersistentFlash/);
  assert.match(owner,
    /WifiCredentialStorage::VolatileRam/);
  assert.doesNotMatch(owner,
    /WifiCredentialStorage::PersistentFlash/);
  assert.match(wifiHeader,
    /enum class WifiCredentialStorage[\s\S]*PersistentFlash[\s\S]*VolatileRam/);
  assert.match(wifi,
    /config\.nvs_enable = persistent/);
  assert.match(wifi,
    /persistent \? esp_wifi_set_storage\(WIFI_STORAGE_FLASH\)[\s\S]*esp_wifi_set_storage\(WIFI_STORAGE_RAM\)/);
  assert.match(wifi,
    /kept in RAM for Recovery/);
  assert.match(main, /prepareRecoveryReadOnlyOrDeinit\(\)/);
  assert.match(main, /recoveryReadOnlyReady\(\)/);
  const earlyStart = main.indexOf(
    "[[noreturn]] void runEarlyOtaRecoveryNetwork(",
  );
  const earlyEnd = main.indexOf(
    "[[noreturn]] void runRecoveryAfterProductFailure(",
    earlyStart,
  );
  assert.notEqual(earlyStart, -1);
  assert.notEqual(earlyEnd, -1);
  const early = main.slice(earlyStart, earlyEnd);
  assert.doesNotMatch(early,
    /nvs_flash_init|nvs_flash_erase|nvs_set_|nvs_commit|PersistentFlash/);
  assert.match(early, /diagnostic, ESP_OK, nullptr/);
  assert.match(early, /RecoveryWifiStoragePolicy::VolatileRam/);
});
