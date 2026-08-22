import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo, "firmware/inkloop-idf/components/inkloop_ota",
);

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>
#include <string>

#include "inkloop/esp_ota_boot_health.hpp"

using namespace inkloop;

static OtaBootHealthConfig config(std::uint32_t soak = 100U,
                                  std::uint32_t deadline = 500U,
                                  std::uint32_t maximum_age = 50U) {
  OtaBootHealthConfig output;
  output.soak_window_ms = soak;
  output.pending_deadline_ms = deadline;
  output.telemetry_max_age_ms = maximum_age;
  output.mandatory_lane_mask = 0x0007U;
  return output;
}

static OtaBootHealthEvidence healthy(
    std::uint32_t now,
    OtaRunningImageState state = OtaRunningImageState::PendingVerify) {
  OtaBootHealthEvidence output;
  output.now_ms = now;
  output.running_image = state;
  output.storage_upgrade_gate = {true, true, now};
  output.board_initialized = {true, true, now};
  output.product_runtime_started = {true, true, now};
  output.fatal_status_clear = {true, true, now};
  for (std::size_t at = 0U; at < 3U; ++at)
    output.supervisor_lanes[at] = {true, 1U, now};
  return output;
}

static void ordinaryAndConfigurationMatrix() {
  OtaBootHealthCore core(config());
  assert(core.configurationValid());
  OtaBootHealthDecision decision = core.evaluate(
      healthy(0U, OtaRunningImageState::Ordinary));
  assert(decision.state == OtaBootHealthState::OrdinaryBoot);
  assert(decision.action == OtaBootHealthAction::None);
  decision = core.evaluate(healthy(1U, OtaRunningImageState::Confirmed));
  assert(decision.state == OtaBootHealthState::ConfirmedBoot);
  decision = core.evaluate(healthy(2U, OtaRunningImageState::Unknown));
  assert(decision.state == OtaBootHealthState::Refused);
  decision = core.evaluate(healthy(3U, OtaRunningImageState::Invalid));
  assert(decision.state == OtaBootHealthState::Refused);
  decision = core.evaluate(healthy(
      4U, static_cast<OtaRunningImageState>(0xFFU)));
  assert(decision.state == OtaBootHealthState::Refused);
  assert(decision.action == OtaBootHealthAction::None);

  for (OtaBootHealthConfig invalid : {
           config(0U, 500U, 50U), config(100U, 100U, 50U),
           config(100U, kOtaHalfTimeRange, 50U), config(100U, 500U, 0U)}) {
    OtaBootHealthCore refused(invalid);
    assert(!refused.configurationValid());
    decision = refused.evaluate(healthy(0U));
    assert(decision.state == OtaBootHealthState::Refused);
    assert(decision.reason ==
           OtaBootHealthReason::InvalidConfiguration);
  }
  OtaBootHealthConfig no_lanes = config();
  no_lanes.mandatory_lane_mask = 0U;
  assert(!OtaBootHealthCore(no_lanes).configurationValid());
}

static void healthAndSoakMatrix() {
  OtaBootHealthCore core(config());
  OtaBootHealthDecision decision = core.evaluate(healthy(0U));
  assert(decision.state == OtaBootHealthState::Soaking);
  decision = core.evaluate(healthy(99U));
  // The 99 ms polling gap exceeds max age and restarts the continuous soak.
  assert(decision.state == OtaBootHealthState::Soaking);
  assert(decision.healthy_soak_elapsed_ms == 0U);
  for (std::uint32_t now : {119U, 139U, 159U, 179U}) {
    decision = core.evaluate(healthy(now));
    assert(decision.action == OtaBootHealthAction::None);
  }
  decision = core.evaluate(healthy(199U));
  assert(decision.action == OtaBootHealthAction::Confirm);
  assert(decision.reason == OtaBootHealthReason::HealthySoakComplete);
  decision = core.evaluate(healthy(200U));
  assert(decision.action == OtaBootHealthAction::None);
  assert(decision.reason == OtaBootHealthReason::ActionAlreadyRequested);
  decision = core.evaluate(healthy(201U, OtaRunningImageState::Confirmed));
  assert(decision.state == OtaBootHealthState::ConfirmedBoot);

  struct FlagCase {
    int field;
    OtaBootHealthReason reason;
  };
  for (const FlagCase& item : std::array<FlagCase, 4>{{
           {0, OtaBootHealthReason::AwaitingMandatoryEvidence},
           {1, OtaBootHealthReason::MandatoryEvidenceUnhealthy},
           {2, OtaBootHealthReason::MandatoryTelemetryStale},
           {3, OtaBootHealthReason::AwaitingMandatoryEvidence},
       }}) {
    OtaBootHealthCore candidate(config());
    OtaBootHealthEvidence input = healthy(100U);
    if (item.field == 0) input.storage_upgrade_gate.present = false;
    if (item.field == 1) input.board_initialized.healthy = false;
    if (item.field == 2) input.product_runtime_started.observed_ms = 0U;
    if (item.field == 3) input.fatal_status_clear.present = false;
    decision = candidate.evaluate(input);
    assert(decision.state == OtaBootHealthState::AwaitingEvidence);
    assert(decision.reason == item.reason);
  }
  for (int mode = 0; mode < 3; ++mode) {
    OtaBootHealthCore candidate(config());
    OtaBootHealthEvidence input = healthy(100U);
    if (mode == 0) input.supervisor_lanes[1].present = false;
    if (mode == 1) input.supervisor_lanes[1].progress_count = 0U;
    if (mode == 2) input.supervisor_lanes[1].observed_ms = 0U;
    decision = candidate.evaluate(input);
    assert(decision.state == OtaBootHealthState::AwaitingEvidence);
    assert(decision.reason == (mode == 0
        ? OtaBootHealthReason::MandatoryLaneMissing
        : mode == 1
            ? OtaBootHealthReason::MandatoryLaneNotProgressed
            : OtaBootHealthReason::MandatoryLaneStale));
  }

  OtaBootHealthCore reset(config(100U, 500U, 50U));
  assert(reset.evaluate(healthy(0U)).state == OtaBootHealthState::Soaking);
  OtaBootHealthEvidence unhealthy = healthy(40U);
  unhealthy.storage_upgrade_gate.healthy = false;
  assert(reset.evaluate(unhealthy).state ==
         OtaBootHealthState::AwaitingEvidence);
  assert(reset.evaluate(healthy(50U)).healthy_soak_elapsed_ms == 0U);
  assert(reset.evaluate(healthy(100U)).action == OtaBootHealthAction::None);
  assert(reset.evaluate(healthy(150U)).action == OtaBootHealthAction::Confirm);
}

static void rollbackAndWrapMatrix() {
  {
    OtaBootHealthCore deadline(config(100U, 200U, 50U));
    assert(deadline.evaluate(healthy(0U)).action == OtaBootHealthAction::None);
    // Deadline has precedence when soak and deadline are both reached.
    OtaBootHealthDecision decision = deadline.evaluate(healthy(200U));
    assert(decision.action == OtaBootHealthAction::Rollback);
    assert(decision.reason == OtaBootHealthReason::PendingDeadlineExpired);
    decision = deadline.evaluate(healthy(201U));
    assert(decision.action == OtaBootHealthAction::None);
    assert(decision.reason == OtaBootHealthReason::ActionAlreadyRequested);
  }
  {
    OtaBootHealthCore fatal(config());
    OtaBootHealthEvidence input = healthy(0U);
    input.explicit_fatal_health_failure = true;
    OtaBootHealthDecision decision = fatal.evaluate(input);
    assert(decision.action == OtaBootHealthAction::Rollback);
    assert(decision.reason ==
           OtaBootHealthReason::ExplicitFatalHealthFailure);
  }
  {
    OtaBootHealthCore fatal(config());
    OtaBootHealthEvidence input = healthy(0U);
    input.fatal_status_clear.healthy = false;
    OtaBootHealthDecision decision = fatal.evaluate(input);
    assert(decision.action == OtaBootHealthAction::Rollback);
    assert(decision.reason == OtaBootHealthReason::FatalResetCondition);
  }
  {
    OtaBootHealthCore wrap(config(32U, 100U, 16U));
    const std::uint32_t start = 0xFFFFFFF0UL;
    assert(wrap.evaluate(healthy(start)).action == OtaBootHealthAction::None);
    assert(wrap.evaluate(healthy(0U)).action == OtaBootHealthAction::None);
    OtaBootHealthDecision decision = wrap.evaluate(healthy(16U));
    assert(decision.action == OtaBootHealthAction::Confirm);
    assert(decision.healthy_soak_elapsed_ms == 32U);
    assert(otaElapsedAtLeast32(16U, start, 32U));
    assert(otaTelemetryFresh32(5U, 0xFFFFFFFDUL, 8U));
    assert(!otaTelemetryFresh32(5U, 0xFFFFFFF0UL, 8U));
    assert(!otaElapsedAtLeast32(0U, 0U, kOtaHalfTimeRange));
  }
}

static OtaRunningImageState g_state = OtaRunningImageState::PendingVerify;
static OtaRunningImageState g_second_state =
    OtaRunningImageState::PendingVerify;
static bool g_use_second = false;
static int g_read_calls = 0;
static int g_fail_read_call = 0;
static int g_confirm_calls = 0;
static int g_rollback_calls = 0;
static int g_confirm_status = 0;
static int g_rollback_status = 0;

static EspOtaStateReadCode readState(OtaRunningImageState& output) {
  ++g_read_calls;
  if (g_fail_read_call != 0 && g_read_calls == g_fail_read_call)
    return EspOtaStateReadCode::StateUnavailable;
  output = g_use_second && g_read_calls % 2 == 0 ? g_second_state : g_state;
  return EspOtaStateReadCode::Ok;
}

static int confirm() {
  ++g_confirm_calls;
  return g_confirm_status;
}

static int rollback() {
  ++g_rollback_calls;
  return g_rollback_status;
}

static constexpr EspOtaSystemFunctions kFunctions{
    &readState, &confirm, &rollback};

static void resetSystem() {
  g_state = OtaRunningImageState::PendingVerify;
  g_second_state = OtaRunningImageState::PendingVerify;
  g_use_second = false;
  g_read_calls = 0;
  g_fail_read_call = 0;
  g_confirm_calls = 0;
  g_rollback_calls = 0;
  g_confirm_status = 0;
  g_rollback_status = 0;
}

static void adapterMatrix() {
  {
    resetSystem();
    OtaBootHealthCore core(config(10U, 100U, 20U));
    EspOtaBootHealthAdapter adapter(core, kFunctions);
    EspOtaBootHealthObservation result = adapter.tick(healthy(0U));
    assert(result.code == EspOtaBootHealthCode::NoAction);
    result = adapter.tick(healthy(10U));
    assert(result.code == EspOtaBootHealthCode::ConfirmationSucceeded);
    assert(result.action_attempted);
    assert(g_confirm_calls == 1 && g_rollback_calls == 0);
    result = adapter.tick(healthy(20U));
    assert(result.code == EspOtaBootHealthCode::NoAction);
    assert(g_confirm_calls == 1);
    g_state = OtaRunningImageState::Confirmed;
    result = adapter.tick(healthy(21U));
    assert(result.decision.state == OtaBootHealthState::ConfirmedBoot);
  }
  {
    resetSystem();
    OtaBootHealthCore core(config(50U, 100U, 20U));
    EspOtaBootHealthAdapter adapter(core, kFunctions);
    OtaBootHealthEvidence missing = healthy(0U);
    missing.board_initialized.present = false;
    assert(adapter.tick(missing).code == EspOtaBootHealthCode::NoAction);
    EspOtaBootHealthObservation result = adapter.tick(healthy(100U));
    assert(result.code == EspOtaBootHealthCode::RollbackInvoked);
    assert(g_rollback_calls == 1 && g_confirm_calls == 0);
    result = adapter.tick(healthy(101U));
    assert(result.code == EspOtaBootHealthCode::NoAction);
    assert(g_rollback_calls == 1);
  }
  {
    resetSystem();
    g_use_second = true;
    g_second_state = OtaRunningImageState::Confirmed;
    OtaBootHealthCore core(config(10U, 100U, 20U));
    EspOtaBootHealthAdapter adapter(core, kFunctions);
    assert(adapter.tick(healthy(0U)).code == EspOtaBootHealthCode::NoAction);
    // Reset parity so the action tick reads Pending, then Confirmed.
    g_read_calls = 0;
    EspOtaBootHealthObservation result = adapter.tick(healthy(10U));
    assert(result.code == EspOtaBootHealthCode::PendingStateChanged);
    assert(g_confirm_calls == 0 && g_rollback_calls == 0);
  }
  for (bool confirmation : {true, false}) {
    resetSystem();
    if (confirmation) g_confirm_status = 7;
    else g_rollback_status = 9;
    OtaBootHealthCore core(confirmation
        ? config(10U, 100U, 20U)
        : config(50U, 100U, 20U));
    EspOtaBootHealthAdapter adapter(core, kFunctions);
    OtaBootHealthEvidence first = healthy(0U);
    if (!confirmation) first.explicit_fatal_health_failure = true;
    EspOtaBootHealthObservation result = adapter.tick(first);
    if (confirmation) result = adapter.tick(healthy(10U));
    assert(result.code == (confirmation
        ? EspOtaBootHealthCode::ConfirmationFailed
        : EspOtaBootHealthCode::RollbackFailed));
    assert(confirmation ? g_confirm_calls == 1 : g_rollback_calls == 1);
    (void)adapter.tick(healthy(11U));
    assert(confirmation ? g_confirm_calls == 1 : g_rollback_calls == 1);
  }
  {
    resetSystem();
    g_fail_read_call = 1;
    OtaBootHealthCore core(config());
    EspOtaBootHealthAdapter adapter(core, kFunctions);
    assert(adapter.tick(healthy(0U)).code ==
           EspOtaBootHealthCode::StateReadFailed);
    assert(g_confirm_calls == 0 && g_rollback_calls == 0);
  }
  {
    resetSystem();
    OtaBootHealthCore core(config());
    EspOtaSystemFunctions invalid;
    EspOtaBootHealthAdapter adapter(core, invalid);
    assert(adapter.tick(healthy(0U)).code ==
           EspOtaBootHealthCode::InvalidFunctions);
  }
}

int main() {
  ordinaryAndConfigurationMatrix();
  healthAndSoakMatrix();
  rollbackAndWrapMatrix();
  adapterMatrix();
  assert(std::string(otaBootHealthActionName(OtaBootHealthAction::Confirm)) ==
         "CONFIRM");
  assert(std::string(espOtaBootHealthCodeName(
             EspOtaBootHealthCode::RollbackInvoked)) ==
         "ROLLBACK_INVOKED");
  return 0;
}
`;

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-ota-health-"));
  try {
    const source = join(scratch, "ota-health.cpp");
    const binary = join(scratch, sanitized ? "asan" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(component, "include"), source,
      join(component, "ota_boot_health.cpp"),
      join(component, "esp_ota_boot_health.cpp"),
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

test("OTA boot-health core and adapter pass strict C++17 state matrix", () => {
  run(false);
});

test("OTA boot-health core and adapter pass ASan/UBSan fault matrix", () => {
  run(true);
});

test("ESP OTA adapter uses only running-state, confirm and rollback APIs", () => {
  const system = readFileSync(
    join(component, "esp_ota_system_api.cpp"), "utf8",
  );
  const adapter = readFileSync(
    join(component, "esp_ota_boot_health.cpp"), "utf8",
  );
  const portable = readFileSync(
    join(component, "ota_boot_health.cpp"), "utf8",
  );
  for (const api of [
    "esp_ota_get_running_partition",
    "esp_ota_get_state_partition",
    "esp_ota_mark_app_valid_cancel_rollback",
    "esp_ota_mark_app_invalid_rollback_and_reboot",
  ]) assert.equal(system.split(api).length - 1, 1, api);
  assert.doesNotMatch(
    system + adapter,
    /esp_ota_(?:begin|write|end|set_boot_partition|erase)|esp_partition_(?:write|erase)|https?:\/\/|download/i,
  );
  assert.doesNotMatch(system + adapter, /app_main|Arduino|MyAI|Wi-?Fi|cloud/i);
  assert.doesNotMatch(portable, /#include\s*[<"](?:esp_|freertos|nvs|Arduino)/);
  assert.doesNotMatch(portable, /wifi|myauth|myai|cloud/i);
  const cmake = readFileSync(join(component, "CMakeLists.txt"), "utf8");
  assert.match(cmake, /REQUIRES app_update/);
});
