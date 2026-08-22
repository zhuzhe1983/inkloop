import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const main = join(repo, "firmware/inkloop-idf/main");
const ota = join(repo, "firmware/inkloop-idf/components/inkloop_ota");

const harness = String.raw`
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "ota_update_owner.hpp"

using namespace inkloop;

static OtaTextView text(const std::string& value) {
  return {value.data(), value.size()};
}

struct ConfigurationFixture {
  std::string url =
      "https://inkloop.mess.host/firmware/c151/manifest.json";
  std::string public_key =
      "d75a980182b10ab7d54bfed3c964073a"
      "0ee172f3daa62325af021a68f707511a";
  std::uint32_t deadline = 120000U;

  OtaUpdateRawConfiguration view() const {
    return {text(url), text(public_key), deadline};
  }
};

static void assertSnapshot(const OtaUpdateOwner& owner,
                           OtaUpdateState state, OtaUpdateCode code,
                           std::uint64_t request_id) {
  const OtaUpdateSnapshot observed = owner.snapshot();
  assert(observed.state == state);
  assert(observed.code == code);
  assert(observed.request_id == request_id);
}

static void configurationMatrix() {
  static_assert(std::is_trivially_copyable<OtaUpdateSnapshot>::value);
  ConfigurationFixture fixture;
  OtaUpdateOwner valid(fixture.view());
  assertSnapshot(valid, OtaUpdateState::Idle, OtaUpdateCode::Ready, 0U);

  OtaUpdateOwner missing_all({{}, {}, 0U});
  assertSnapshot(missing_all, OtaUpdateState::Disabled,
                 OtaUpdateCode::ConfigurationMissing, 0U);
  OtaUpdateRawConfiguration input = fixture.view();
  input.manifest_url = {};
  OtaUpdateOwner missing_url(input);
  assertSnapshot(missing_url, OtaUpdateState::Disabled,
                 OtaUpdateCode::ConfigurationMissing, 0U);
  input = fixture.view();
  input.public_key_hex = {};
  OtaUpdateOwner missing_key(input);
  assertSnapshot(missing_key, OtaUpdateState::Disabled,
                 OtaUpdateCode::ConfigurationMissing, 0U);

  for (const std::string& bad_url : {
           std::string("http://inkloop.mess.host/manifest.json"),
           std::string("https://user:secret@inkloop.mess.host/manifest.json"),
           std::string("https://inkloop.mess.host/manifest.json?token=x"),
           std::string("https://127.0.0.1/manifest.json")}) {
    ConfigurationFixture bad = fixture;
    bad.url = bad_url;
    OtaUpdateOwner rejected(bad.view());
    assertSnapshot(rejected, OtaUpdateState::Disabled,
                   OtaUpdateCode::ManifestUrlRejected, 0U);
  }
  for (const std::string& placeholder : {
           std::string("https://updates.example.com/manifest.json"),
           std::string("https://placeholder.mess.host/manifest.json"),
           std::string("https://firmware.invalid/manifest.json"),
           std::string("https://firmware.test/manifest.json")}) {
    ConfigurationFixture bad = fixture;
    bad.url = placeholder;
    OtaUpdateOwner rejected(bad.view());
    assertSnapshot(rejected, OtaUpdateState::Disabled,
                   OtaUpdateCode::PlaceholderEndpointRejected, 0U);
  }

  for (const std::string& bad_key : {
           std::string(63U, 'a'), std::string(65U, 'a'),
           std::string(64U, '0'), std::string(64U, 'A'),
           std::string(64U, 'g')}) {
    ConfigurationFixture bad = fixture;
    bad.public_key = bad_key;
    OtaUpdateOwner rejected(bad.view());
    assertSnapshot(rejected, OtaUpdateState::Disabled,
                   OtaUpdateCode::PublicKeyRejected, 0U);
  }
  for (const std::uint32_t bad_deadline : {
           0U, kMaximumOtaAcquisitionDeadlineMs + 1U}) {
    ConfigurationFixture bad = fixture;
    bad.deadline = bad_deadline;
    OtaUpdateOwner rejected(bad.view());
    assertSnapshot(rejected, OtaUpdateState::Disabled,
                   OtaUpdateCode::DeadlineRejected, 0U);
  }

  assert(missing_all.request(1U) == OtaUpdateCode::Disabled);
  assert(missing_all.request(0U) == OtaUpdateCode::InvalidRequestId);
  OtaUpdateRequest request;
  assert(missing_all.take(request) == OtaUpdateCode::Disabled);
  assert(request.request_id == 0U);
}

static void twoStepAndFailureMatrix() {
  ConfigurationFixture fixture;
  OtaUpdateOwner owner(fixture.view());
  constexpr std::uint64_t kRequestId = 0xFEDCBA9876543210ULL;
  assert(owner.request(0U) == OtaUpdateCode::InvalidRequestId);
  assertSnapshot(owner, OtaUpdateState::Idle, OtaUpdateCode::Ready, 0U);
  assert(owner.request(kRequestId) == OtaUpdateCode::Ok);
  assertSnapshot(owner, OtaUpdateState::Requested, OtaUpdateCode::Ok,
                 kRequestId);
  assert(owner.request(kRequestId) == OtaUpdateCode::DuplicateRequest);
  assert(owner.request(kRequestId - 1U) == OtaUpdateCode::Busy);

  OtaUpdateRequest request;
  assert(owner.take(request) == OtaUpdateCode::Ok);
  assert(request.request_id == kRequestId);
  assertSnapshot(owner, OtaUpdateState::Running, OtaUpdateCode::Ok,
                 kRequestId);
  OtaUpdateRequest absent;
  assert(owner.take(absent) == OtaUpdateCode::NoRequest);
  assert(absent.request_id == 0U);
  assert(owner.fail({kRequestId - 1U}, OtaUpdateCode::QuiesceFailed) ==
         OtaUpdateCode::RequestMismatch);
  assert(owner.fail(request, OtaUpdateCode::Ready) ==
         OtaUpdateCode::InvalidTerminalCode);
  assertSnapshot(owner, OtaUpdateState::Running, OtaUpdateCode::Ok,
                 kRequestId);
  assert(owner.fail(request, OtaUpdateCode::QuiesceFailed) ==
         OtaUpdateCode::QuiesceFailed);
  assertSnapshot(owner, OtaUpdateState::Failed,
                 OtaUpdateCode::QuiesceFailed, kRequestId);
  assert(owner.fail(request, OtaUpdateCode::QuiesceFailed) ==
         OtaUpdateCode::NoRequest);
  assert(owner.request(kRequestId) == OtaUpdateCode::DuplicateRequest);
  assert(owner.request(kRequestId + 1U) == OtaUpdateCode::Busy);

  const std::array<OtaUpdateCode, 12U> accepted_failures{
      OtaUpdateCode::PlatformUnavailable,
      OtaUpdateCode::VerifierUnavailable,
      OtaUpdateCode::AcquisitionInvalidState,
      OtaUpdateCode::AcquisitionConfigurationRejected,
      OtaUpdateCode::DeadlineExceeded,
      OtaUpdateCode::ManifestFetchFailed,
      OtaUpdateCode::ManifestRejected,
      OtaUpdateCode::ImageOriginMismatch,
      OtaUpdateCode::StagingBeginFailed,
      OtaUpdateCode::ImageFetchFailed,
      OtaUpdateCode::StagingFinishFailed,
      OtaUpdateCode::QuiesceFailed};
  std::uint64_t id = 100U;
  for (const OtaUpdateCode code : accepted_failures) {
    OtaUpdateOwner failed(fixture.view());
    assert(failed.request(id) == OtaUpdateCode::Ok);
    OtaUpdateRequest claimed;
    assert(failed.take(claimed) == OtaUpdateCode::Ok);
    assert(failed.fail(claimed, code) == code);
    assertSnapshot(failed, OtaUpdateState::Failed, code, id);
    ++id;
  }
}

static void acquisitionClaimMatrix() {
  ConfigurationFixture fixture;
  const std::string board = "m5-papercolor-c151";
  const std::string version = "0.4.0-beta.1";

  OtaUpdateOwner idle(fixture.view());
  assert(idle.acquire({1U}, text(board), text(version)) ==
         OtaUpdateCode::RequestMismatch);
  assertSnapshot(idle, OtaUpdateState::Idle, OtaUpdateCode::Ready, 0U);

  OtaUpdateOwner requested(fixture.view());
  assert(requested.request(8U) == OtaUpdateCode::Ok);
  assert(requested.acquire({8U}, text(board), text(version)) ==
         OtaUpdateCode::NoRequest);
  assertSnapshot(requested, OtaUpdateState::Requested, OtaUpdateCode::Ok, 8U);

  OtaUpdateOwner wrong(fixture.view());
  assert(wrong.request(9U) == OtaUpdateCode::Ok);
  OtaUpdateRequest claimed;
  assert(wrong.take(claimed) == OtaUpdateCode::Ok);
  assert(wrong.acquire({10U}, text(board), text(version)) ==
         OtaUpdateCode::RequestMismatch);
  assertSnapshot(wrong, OtaUpdateState::Running, OtaUpdateCode::Ok, 9U);

  OtaUpdateOwner invalid(fixture.view());
  assert(invalid.request(11U) == OtaUpdateCode::Ok);
  assert(invalid.take(claimed) == OtaUpdateCode::Ok);
  assert(invalid.acquire(claimed, {}, text(version)) ==
         OtaUpdateCode::AcquisitionConfigurationRejected);
  assertSnapshot(invalid, OtaUpdateState::Failed,
                 OtaUpdateCode::AcquisitionConfigurationRejected, 11U);

  // Host builds deliberately lack ESP_PLATFORM. The method must still claim
  // the exact Running request and record a truthful terminal failure.
  OtaUpdateOwner host(fixture.view());
  assert(host.request(12U) == OtaUpdateCode::Ok);
  assert(host.take(claimed) == OtaUpdateCode::Ok);
  assert(host.acquire(claimed, text(board), text(version)) ==
         OtaUpdateCode::PlatformUnavailable);
  assertSnapshot(host, OtaUpdateState::Failed,
                 OtaUpdateCode::PlatformUnavailable, 12U);
}

static void concurrentLatchMatrix() {
  ConfigurationFixture fixture;
  OtaUpdateOwner owner(fixture.view());
  constexpr std::size_t kThreads = 16U;
  std::array<OtaUpdateCode, kThreads> results{};
  std::array<std::thread, kThreads> threads;
  for (std::size_t at = 0U; at < threads.size(); ++at) {
    threads[at] = std::thread([&, at]() {
      results[at] = owner.request(
          0x100000000ULL + static_cast<std::uint64_t>(at) + 1U);
    });
  }
  for (std::thread& thread : threads) thread.join();
  assert(std::count(results.begin(), results.end(), OtaUpdateCode::Ok) == 1);
  assert(std::count(results.begin(), results.end(), OtaUpdateCode::Busy) ==
         static_cast<std::ptrdiff_t>(kThreads - 1U));
  const OtaUpdateSnapshot requested = owner.snapshot();
  assert(requested.state == OtaUpdateState::Requested);
  assert(requested.request_id > 0x100000000ULL);

  std::array<OtaUpdateCode, 2U> takes{};
  std::array<OtaUpdateRequest, 2U> claimed{};
  std::thread first([&]() { takes[0] = owner.take(claimed[0]); });
  std::thread second([&]() { takes[1] = owner.take(claimed[1]); });
  first.join();
  second.join();
  assert(std::count(takes.begin(), takes.end(), OtaUpdateCode::Ok) == 1);
  assert(std::count(takes.begin(), takes.end(), OtaUpdateCode::NoRequest) == 1);
  const OtaUpdateSnapshot running = owner.snapshot();
  assert(running.state == OtaUpdateState::Running);
  assert(running.request_id == requested.request_id);
}

int main() {
  configurationMatrix();
  twoStepAndFailureMatrix();
  acquisitionClaimMatrix();
  concurrentLatchMatrix();
  return 0;
}
`;

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-ota-owner-"));
  try {
    const source = join(scratch, "ota-owner.cpp");
    const binary = join(scratch, sanitized ? "asan" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-pthread", "-I", main, "-I", join(ota, "include"), source,
      join(main, "ota_update_owner.cpp"),
      join(ota, "ota_https_acquisition.cpp"),
      join(ota, "ota_sha256.cpp"),
      join(ota, "ota_staging.cpp"),
      join(ota, "esp_ota_staging.cpp"),
      "-o", binary,
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

test("OTA production owner passes strict configuration/state matrix", () => {
  run(false);
});

test("OTA production owner passes ASan/UBSan concurrency matrix", () => {
  run(true);
});

test("OTA owner composes only the frozen production stack", () => {
  const header = readFileSync(join(main, "ota_update_owner.hpp"), "utf8");
  const source = readFileSync(join(main, "ota_update_owner.cpp"), "utf8");
  const cmake = readFileSync(join(main, "CMakeLists.txt"), "utf8");
  const kconfig = readFileSync(join(main, "Kconfig.projbuild"), "utf8");

  assert.match(header, /std::uint64_t request_id/);
  assert.match(header, /std::atomic<AtomicStatus> status_/);
  assert.match(header, /OtaUpdateCode take\(OtaUpdateRequest& request\)/);
  assert.match(header, /OtaUpdateCode fail\(const OtaUpdateRequest& request/);
  assert.match(header, /OtaUpdateCode acquire\(const OtaUpdateRequest& request/);
  assert.doesNotMatch(header + source, /acquireRequested/);
  assert.match(source, /OtaUpdateState::Requested/);
  assert.match(source, /OtaUpdateState::Running/);
  assert.match(source, /OtaUpdateState::Acquiring/);
  assert.match(source, /EspOtaEd25519Verifier verifier/);
  assert.match(source, /EspOtaStagingAdapter staging\(systemEspOtaWriterFunctions\(\)/);
  assert.match(source, /EspOtaMonotonicClock clock/);
  assert.match(source, /EspOtaHttpsTransport transport/);
  assert.match(source, /OtaHttpsAcquisition acquisition/);
  assert.match(source, /CONFIG_INKLOOP_OTA_MANIFEST_URL/);
  assert.match(source, /CONFIG_INKLOOP_OTA_ED25519_PUBLIC_KEY_HEX/);
  assert.match(source, /CONFIG_INKLOOP_OTA_TOTAL_DEADLINE_MS/);
  assert.doesNotMatch(
    header + source,
    /ESP_LOG|printf|fprintf|esp_restart|private[_ -]?key|BEGIN [A-Z ]+ KEY|Authorization|Bearer/i,
  );
  assert.doesNotMatch(source, /app_main|inkloop_product|portal|nvs_|fopen/);
  assert.match(cmake,
    /SRCS "app_main\.cpp" "ota_outcome_journal\.cpp" "ota_update_owner\.cpp"[\s\S]*"recovery_action_owner\.cpp"/);

  assert.match(kconfig, /config INKLOOP_OTA_MANIFEST_URL/);
  assert.match(kconfig, /config INKLOOP_OTA_ED25519_PUBLIC_KEY_HEX/);
  assert.match(kconfig, /config INKLOOP_OTA_TOTAL_DEADLINE_MS/);
  assert.equal((kconfig.match(/default ""/g) ?? []).length, 2);
  assert.match(kconfig, /range 1 120000/);
  assert.match(kconfig, /default 120000/);
  assert.doesNotMatch(kconfig, /https:\/\/[A-Za-z0-9]|[0-9a-f]{64}/);
});
