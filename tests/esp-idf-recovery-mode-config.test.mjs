import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_recovery",
);

const harness = String.raw`
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

#include "inkloop/recovery/recovery_network_config.hpp"
#include "inkloop/recovery/recovery_network_stop.hpp"

using namespace inkloop::recovery;

struct FakeStation {
  explicit FakeStation(int failure) : failure_(failure) { ++live; }
  ~FakeStation() {
    assert(shutdown_complete);
    --live;
  }

  esp_err_t shutdown() {
    ++shutdown_calls;
    if (failure_ != ESP_OK) {
      const esp_err_t result = failure_;
      failure_ = ESP_OK;
      return result;
    }
    shutdown_complete = true;
    return ESP_OK;
  }

  static int live;
  int failure_ = ESP_OK;
  int shutdown_calls = 0;
  bool shutdown_complete = false;
};

int FakeStation::live = 0;

void stopFaultMatrix() {
  constexpr esp_err_t kHttpStopFailed = -101;
  constexpr esp_err_t kWifiFailures[] = {
      -201,  // provisioning HTTP stop
      -202,  // SNTP event unregister
      -203,  // Wi-Fi event unregister
      -204,  // IP event unregister
      -205,  // driver stop
      -206,  // driver deinitialize
  };

  {
    auto station = std::make_unique<FakeStation>(ESP_OK);
    FakeStation* const identity = station.get();
    int service_calls = 0;
    esp_err_t service_result = kHttpStopFailed;
    auto stop_services = [&] {
      ++service_calls;
      const esp_err_t result = service_result;
      service_result = ESP_OK;
      return result;
    };
    assert(detail::stopRecoveryNetworkOwners(station, stop_services) ==
           kHttpStopFailed);
    assert(station.get() == identity);
    assert(identity->shutdown_calls == 0);
    assert(FakeStation::live == 1);
    assert(detail::stopRecoveryNetworkOwners(station, stop_services) == ESP_OK);
    assert(!station);
    assert(service_calls == 2);
    assert(FakeStation::live == 0);
  }

  for (const esp_err_t failure : kWifiFailures) {
    auto station = std::make_unique<FakeStation>(failure);
    FakeStation* const identity = station.get();
    int service_calls = 0;
    auto stop_services = [&] {
      ++service_calls;
      return ESP_OK;
    };
    assert(detail::stopRecoveryNetworkOwners(station, stop_services) ==
           failure);
    assert(station.get() == identity);
    assert(identity->shutdown_calls == 1);
    assert(!identity->shutdown_complete);
    assert(FakeStation::live == 1);
    assert(detail::stopRecoveryNetworkOwners(station, stop_services) == ESP_OK);
    assert(!station);
    assert(service_calls == 2);
    assert(FakeStation::live == 0);
  }

  std::unique_ptr<FakeStation> absent;
  int idempotent_calls = 0;
  assert(detail::stopRecoveryNetworkOwners(absent, [&] {
    ++idempotent_calls;
    return ESP_OK;
  }) == ESP_OK);
  assert(idempotent_calls == 1);
  assert(FakeStation::live == 0);
}

std::array<char, 64> access(const std::string& text) {
  assert(text.size() < 64U);
  std::array<char, 64> output{};
  std::copy(text.begin(), text.end(), output.begin());
  return output;
}

bool build(const std::array<char, 64>& code, const std::string& ip,
           const std::string& session = "session_0123456789abcdef",
           const std::string& csrf = "csrf_0123456789abcdef") {
  RecoveryAccessConfig config;
  RecoveryEndpointGuidance guidance;
  return buildRecoveryAccessConfig(code, ip, session, csrf, config, guidance);
}

int main() {
  stopFaultMatrix();
  const auto code = access("saved-home-password");
  RecoveryAccessConfig config;
  RecoveryEndpointGuidance guidance;
  assert(buildRecoveryAccessConfig(
      code, "192.168.199.156", "session_0123456789abcdef",
      "csrf_0123456789abcdef", config, guidance));
  assert(config.access_code == "saved-home-password");
  assert(config.allowed_host_count == 2U);
  assert(config.allowed_hosts[0] == "inkloop.local:8080");
  assert(config.allowed_hosts[1] == "192.168.199.156:8080");
  assert(config.allowed_origins[0] == "http://inkloop.local:8080");
  assert(config.allowed_origins[1] == "http://192.168.199.156:8080");
  assert(config.allowed_hosts[2].empty() && config.allowed_hosts[3].empty());
  assert(config.allowed_origins[2].empty() && config.allowed_origins[3].empty());
  assert(std::string(guidance.mdns_url.data()) ==
         "http://inkloop.local:8080/");
  assert(std::string(guidance.local_ip_url.data()) ==
         "http://192.168.199.156:8080/");
  assert(config.session_lifetime_seconds == 900U);

  assert(build(code, "10.0.0.8"));
  assert(build(code, "172.16.0.1"));
  assert(build(code, "172.31.255.254"));
  assert(build(code, "192.168.4.1"));
  assert(build(code, "169.254.2.3"));
  assert(!build(code, "172.15.255.254"));
  assert(!build(code, "172.32.0.1"));
  assert(!build(code, "8.8.8.8"));
  assert(!build(code, "127.0.0.1"));
  assert(!build(code, "0.0.0.0"));
  assert(!build(code, "192.168.001.1"));
  assert(!build(code, "192.168.1"));
  assert(!build(code, "192.168.1.1:8080"));
  assert(!build(code, "*"));
  assert(!build(access("abc"), "192.168.4.1"));
  std::array<char, 64> unterminated{};
  unterminated.fill('A');
  assert(!build(unterminated, "192.168.4.1"));
  assert(!build(code, "192.168.4.1", "same_token_1234567890",
                "same_token_1234567890"));
  assert(!build(code, "192.168.4.1", "short", "csrf_0123456789abcdef"));
  assert(!build(code, "192.168.4.1", "session_0123456789abcdef",
                "csrf!0123456789abcdef"));

  RecoveryAccessConfig untouched;
  untouched.access_code = "sentinel";
  RecoveryEndpointGuidance untouched_guidance;
  untouched_guidance.mdns_url[0] = 'X';
  assert(!buildRecoveryAccessConfig(
      code, "example.com", "session_0123456789abcdef",
      "csrf_0123456789abcdef", untouched, untouched_guidance));
  assert(untouched.access_code == "sentinel");
  assert(untouched_guidance.mdns_url[0] == 'X');
  return 0;
}
`;

function buildAndRun(sanitize) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-recovery-mode-"));
  try {
    const harnessPath = join(scratch, "config.cpp");
    const espErrorPath = join(scratch, "esp_err.h");
    const binary = join(scratch, sanitize ? "sanitized" : "strict");
    writeFileSync(harnessPath, harness);
    writeFileSync(
      espErrorPath,
      "#pragma once\nusing esp_err_t = int;\ninline constexpr esp_err_t ESP_OK = 0;\n",
    );
    const flags = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I",
      scratch,
      "-I",
      join(component, "include"),
      harnessPath,
      join(component, "recovery_network_config.cpp"),
      "-o",
      binary,
    ];
    if (sanitize) {
      flags.splice(
        5,
        0,
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
      );
    }
    execFileSync("c++", flags, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitize
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("recovery access and shutdown fault matrix are strict and exact", () => {
  buildAndRun(false);
});

test("recovery access and shutdown matrix pass ASan and UBSan", () => {
  buildAndRun(true);
});

test("verified recovery snapshots have a bounded 16 KiB HTTP task", () => {
  const server = readFileSync(join(component, "esp_recovery_server.cpp"), "utf8");
  assert.match(server, /kRecoveryHttpTaskStackBytes = 16U \* 1024U/);
  assert.match(server, /native\.stack_size = kRecoveryHttpTaskStackBytes/);
});
