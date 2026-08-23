import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const idf = join(repo, "firmware/inkloop-idf");
const onboarding = join(idf, "components/inkloop_onboarding");

test("durable tutorial core resumes, replays corrupt state, and advances only after a verified save", () => {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-tutorial-core-"));
  const harness = join(scratch, "tutorial_core.cpp");
  const binary = join(scratch, "tutorial_core");
  writeFileSync(harness, String.raw`
#include <cassert>
#include <cstdint>
#include <string>

#include "inkloop/onboarding/tutorial_state.hpp"

using namespace inkloop::onboarding;

struct Store final : ITutorialStateStore {
  TutorialStateResult load_result = TutorialStateResult::Absent;
  TutorialStateResult save_result = TutorialStateResult::Ok;
  TutorialStep loaded = TutorialStep::PressToTalk;
  TutorialStep saved = TutorialStep::PressToTalk;
  unsigned loads = 0;
  unsigned saves = 0;

  TutorialStateResult load(TutorialStep& output) override {
    ++loads;
    output = loaded;
    return load_result;
  }
  TutorialStateResult save(TutorialStep value) override {
    ++saves;
    saved = value;
    return save_result;
  }
};

int main() {
  Store absent;
  TutorialStateCore fresh(absent);
  assert(fresh.initialize() == TutorialStateResult::Absent);
  assert(fresh.step() == TutorialStep::PressToTalk);
  assert(!fresh.complete());
  assert(!fresh.persistenceError());
  assert(fresh.initialize() == TutorialStateResult::Ok);
  assert(absent.loads == 1);
  assert(fresh.set(TutorialStep::VoiceLedStates) == TutorialStateResult::Ok);
  assert(absent.saves == 1 && absent.saved == TutorialStep::VoiceLedStates);
  assert(fresh.step() == TutorialStep::VoiceLedStates);

  Store restored;
  restored.load_result = TutorialStateResult::Ok;
  restored.loaded = TutorialStep::LocalPortal;
  TutorialStateCore resumed(restored);
  assert(resumed.initialize() == TutorialStateResult::Ok);
  assert(resumed.step() == TutorialStep::LocalPortal);
  assert(resumed.set(TutorialStep::Complete) == TutorialStateResult::Ok);
  assert(resumed.complete());

  Store corrupt;
  corrupt.load_result = TutorialStateResult::Corrupt;
  corrupt.loaded = TutorialStep::Complete;
  TutorialStateCore replay(corrupt);
  assert(replay.initialize() == TutorialStateResult::Corrupt);
  assert(replay.step() == TutorialStep::PressToTalk);
  assert(replay.persistenceError());

  Store failing;
  failing.load_result = TutorialStateResult::Ok;
  failing.loaded = TutorialStep::GalleryPaging;
  failing.save_result = TutorialStateResult::Storage;
  TutorialStateCore guarded(failing);
  assert(guarded.initialize() == TutorialStateResult::Ok);
  assert(guarded.set(TutorialStep::DisplayBusyGuard) == TutorialStateResult::Storage);
  assert(guarded.step() == TutorialStep::GalleryPaging);
  assert(guarded.persistenceError());
  assert(guarded.set(static_cast<TutorialStep>(255)) == TutorialStateResult::InvalidArgument);
  assert(failing.saves == 1);

  assert(validTutorialStep(TutorialStep::Complete));
  assert(!validTutorialStep(static_cast<TutorialStep>(6)));
  assert(std::string(tutorialStepName(TutorialStep::PressToTalk)) == "press_to_talk");
  assert(std::string(tutorialStepName(TutorialStep::Complete)) == "complete");
  return 0;
}
`);

  try {
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
      "-I", join(onboarding, "include"),
      harness,
      join(onboarding, "tutorial_state.cpp"),
      "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [], { stdio: "pipe" });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
});

test("ESP-IDF tutorial journal is CRC-checked, committed, and read back", () => {
  const source = readFileSync(join(
    idf,
    "components/inkloop_tutorial_state_idf/esp_nvs_tutorial_state_store.cpp",
  ), "utf8");
  const cmake = readFileSync(join(
    idf,
    "components/inkloop_tutorial_state_idf/CMakeLists.txt",
  ), "utf8");

  assert.match(source, /kNamespace\[\] = "ink-tutorial"/);
  assert.match(source, /kMagic\{\{'I', 'N', 'K', 'T'\}\}/);
  assert.match(source, /crc32\(input\.data\(\), 8U\)/);
  assert.match(source, /nvs_commit\(handle\)/);
  assert.match(source, /TutorialStep verified/);
  assert.match(source, /load\(verified\).*verified == value/s);
  assert.match(cmake, /esp_nvs_tutorial_state_store\.cpp/);
  assert.match(cmake, /nvs_flash/);
  assert.doesNotMatch(cmake, /qrcode|esp_pairing_frame/);
});

test("native tutorial waits for live authorized voice, persists each spoken step, and is replayable from Portal", () => {
  const header = readFileSync(join(
    idf,
    "components/inkloop_product/include/inkloop/native_voice_service.hpp",
  ), "utf8");
  const voice = readFileSync(join(
    idf,
    "components/inkloop_product/native_voice_service.cpp",
  ), "utf8");
  const portal = readFileSync(join(
    idf,
    "components/inkloop_portal/portal_core.cpp",
  ), "utf8");
  const owner = readFileSync(join(
    idf,
    "components/inkloop_product/native_portal_owner.cpp",
  ), "utf8");

  assert.match(header, /EspNvsTutorialStateStore tutorial_store_/);
  assert.match(header, /TutorialStateCore tutorial_\{tutorial_store_\}/);
  assert.match(voice, /activation_state_ != myai::ActivationState::Bound/);
  assert.match(voice, /!authorization_verified_/);
  assert.match(voice, /network_voice_state_ != myai::VoiceState::Ready/);
  assert.match(voice, /!voice_assistance/);
  assert.match(voice, /!wss_\.connected\(\)/);
  assert.match(voice, /client_->requestResponse\(prompt\)/);
  assert.match(voice, /state == myai::VoiceState::Speaking[\s\S]*tutorial_response_observed_ = true/);
  assert.match(voice, /state == myai::VoiceState::Ready[\s\S]*nextTutorialStep\(spoken_step\)/);
  assert.match(voice, /StorageSetTutorialState/);
  assert.match(voice, /tutorial_response_timeout/);
  assert.match(portal, /\/api\/tutorial\/restart/);
  assert.match(portal, /id="tutorial-restart"/);
  assert.match(portal, /persistenceError/);
  assert.match(owner, /PortalCommandType::RestartMyAiTutorial/);
  assert.match(owner, /voice_\.enqueueRestartTutorial\(\)/);
});
