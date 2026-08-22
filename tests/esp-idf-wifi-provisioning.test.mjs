import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import test from "node:test";

const root = resolve(import.meta.dirname, "..");
const component = join(
  root,
  "firmware/inkloop-idf/components/inkloop_connectivity",
);

const harness = String.raw`
#include <cassert>
#include <string>
#include "inkloop/wifi_provisioning_portal.hpp"

using namespace inkloop;

struct Sink final : IWifiProvisioningSink {
  WifiProvisioningSubmitResult result = WifiProvisioningSubmitResult::Accepted;
  std::string ssid;
  std::string password;
  int calls = 0;
  WifiProvisioningSubmitResult trySubmitCredentials(
      const std::string& next_ssid, const std::string& next_password) override {
    ++calls;
    ssid = next_ssid;
    password = next_password;
    return result;
  }
};

int main() {
  Sink sink;
  WifiProvisioningPortal portal(sink);
  WifiProvisioningRequest request;
  request.method = "GET";
  request.path = "/";
  auto page = portal.handle(request);
  assert(page.status == 200);
  assert(page.body.find("/configure") != std::string::npos);
  assert(page.body.find("maxlength=\"32\"") != std::string::npos);

  request.method = "POST";
  request.path = "/configure";
  request.content_type = "application/x-www-form-urlencoded";
  request.body = "ssid=z-home&password=super%2Bsecret";
  auto accepted = portal.handle(request);
  assert(accepted.status == 202);
  assert(sink.calls == 1 && sink.ssid == "z-home");
  assert(sink.password == "super+secret");
  assert(accepted.body.find("super") == std::string::npos);

  request.body = "ssid=%E5%AE%B6%E9%87%8C%E7%9A%84WiFi&password=12345678";
  assert(portal.handle(request).status == 202);
  assert(sink.ssid == "家里的WiFi");

  sink.result = WifiProvisioningSubmitResult::Busy;
  assert(portal.handle(request).status == 409);
  request.body = "ssid=z-home&password=short";
  assert(portal.handle(request).status == 422);
  request.body = "ssid=z-home&password=&password=duplicate";
  assert(portal.handle(request).status == 422);
  request.body = "ssid=z-home&password=12345678&extra=x";
  assert(portal.handle(request).status == 422);
  request.body = std::string(321, 'x');
  assert(portal.handle(request).status == 413);
  request.content_type = "application/json";
  request.body = "{}";
  assert(portal.handle(request).status == 415);
  request.method = "GET";
  request.path = "/configure";
  assert(portal.handle(request).status == 404);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-wifi-provision-"));
  try {
    const source = join(scratch, "test.cpp");
    const binary = join(scratch, sanitized ? "san" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I",
      join(component, "include"),
      source,
      join(component, "wifi_provisioning_portal.cpp"),
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

test("Wi-Fi provisioning portal validates and never echoes credentials", () => {
  buildAndRun(false);
  buildAndRun(true);
});

test("sole station owner owns APSTA, credential commit and reconnect", () => {
  const header = readFileSync(
    join(component, "include/inkloop/esp_wifi_station.hpp"),
    "utf8",
  );
  const source = readFileSync(join(component, "esp_wifi_station.cpp"), "utf8");
  const server = readFileSync(
    join(component, "esp_wifi_provisioning_server.cpp"),
    "utf8",
  );
  assert.match(header, /public IWifiProvisioningSink/);
  assert.match(header, /setLocalAccessCodeOverride/);
  assert.match(source, /WifiStationAction::RequireProvisioning/);
  assert.match(source, /esp_wifi_set_mode\(WIFI_MODE_APSTA\)/);
  assert.match(source, /esp_wifi_set_storage\(WIFI_STORAGE_FLASH\)/);
  assert.match(source, /esp_wifi_set_config\(WIFI_IF_STA/);
  assert.match(source, /core_\.begin\(true, now_ms\)/);
  assert.match(server, /kMaximumWifiProvisioningBodyBytes/);
  assert.doesNotMatch(server, /esp_wifi_set_config|esp_wifi_set_mode/);
  assert.doesNotMatch(source, /submitted_password_.*ESP_LOG/);
});
