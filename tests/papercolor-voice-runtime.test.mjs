import assert from "node:assert/strict";
import { access, mkdtemp, rm, writeFile, readFile } from "node:fs/promises";
import { homedir, tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const sourceUrl = new URL(
  "../firmware/m5-papercolor/lib/InkloopVoice/src/",
  import.meta.url,
);

async function platformioCommand() {
  const managed = join(homedir(), ".platformio", "penv", "bin", "pio");
  try {
    await access(managed);
    return managed;
  } catch {
    return "pio";
  }
}

test("PaperColor voice runtime enforces half-duplex, freshness, and display commit contracts", async () => {
  const temporaryDirectory = await mkdtemp(join(tmpdir(), "inkloop-voice-runtime-"));
  const harnessPath = join(temporaryDirectory, "voice_runtime_test.cpp");
  const executablePath = join(temporaryDirectory, "voice_runtime_test");
  const harness = String.raw`
#include <cassert>
#include <string>
#include <vector>

#include "AudioPromptController.h"
#include "LocalCommandParser.h"
#include "VoiceRuntime.h"

using namespace inkloop::voice;

struct FakeClock final : IClock {
  uint64_t now = 1000;
  uint64_t monotonicMs() const override { return now; }
  void advance(uint64_t milliseconds) { now += milliseconds; }
};

struct FakePlayer;

struct FakeTransport final : IConversationTransport {
  FakePlayer* player = nullptr;
  VoiceRuntime* runtime = nullptr;
  bool capturing = false;
  bool remotePlaying = false;
  bool failStart = false;
  bool failStop = false;
  bool failResponse = false;
  bool failCancelTts = false;
  bool failCancelTurn = false;
  bool cancelClosesSession = false;
  bool synchronousTtsStopOnCancel = false;
  int starts = 0, stops = 0, responses = 0, ttsCancels = 0, turnCancels = 0;
  std::string lastStream, lastResponse;
  std::vector<std::string>* sequence = nullptr;

  Status startListening(const std::string& streamId) override;
  Status stopListeningAndSend() override {
    ++stops;
    if (sequence) sequence->push_back("capture.stop");
    if (failStop) return Status::error("stop_failed", "capture stop failed");
    capturing = false;
    return Status::ok();
  }
  Status requestResponse(const std::string& transcript) override {
    ++responses; lastResponse = transcript;
    if (failResponse) return Status::error("response_failed", "response failed");
    return Status::ok();
  }
  Status cancelTts() override {
    ++ttsCancels;
    if (failCancelTts) return Status::error("tts_cancel_failed", "tts cancel failed");
    remotePlaying = false;
    if (synchronousTtsStopOnCancel && runtime)
      runtime->onTtsStop(runtime->activeTurnGeneration());
    return Status::ok();
  }
  bool cancelTurnClosesSession() const override {
    return cancelClosesSession;
  }
  Status cancelTurn() override {
    ++turnCancels;
    if (failCancelTurn) return Status::error("turn_cancel_failed", "turn cancel failed");
    capturing = false;
    return Status::ok();
  }
};

struct FakePlayer final : IAudioPromptPlayer {
  struct Played { std::string file; uint32_t argument; };
  FakeTransport* transport = nullptr;
  bool playing = false;
  bool failPlay = false;
  bool failStop = false;
  int stops = 0;
  std::vector<Played> played;
  std::vector<std::string>* sequence = nullptr;

  bool busy() const override { return playing; }
  Status play(const std::string& fileId, uint32_t argument = 0) override {
    assert(!transport || (!transport->capturing && !transport->remotePlaying));
    if (failPlay) return Status::error("prompt_play_failed", "prompt play failed");
    played.push_back(Played{fileId, argument});
    playing = true;
    return Status::ok();
  }
  Status stop() override {
    ++stops;
    if (sequence) sequence->push_back("prompt.stop");
    if (failStop) return Status::error("prompt_stop_failed", "prompt stop failed");
    playing = false;
    return Status::ok();
  }
  void complete() { playing = false; }
  size_t count(const std::string& file) const {
    size_t result = 0;
    for (size_t i = 0; i < played.size(); ++i) if (played[i].file == file) ++result;
    return result;
  }
};

Status FakeTransport::startListening(const std::string& streamId) {
  ++starts; lastStream = streamId;
  if (sequence) sequence->push_back("capture.start");
  assert(!player || !player->busy());
  capturing = true;
  if (failStart) return Status::error("start_failed", "capture start failed");
  return Status::ok();
}

struct FakeLed final : IVoiceLed {
  std::vector<VoiceLedState> states;
  void setLeftVoiceState(VoiceLedState state) override { states.push_back(state); }
  VoiceLedState last() const { return states.empty() ? VoiceLedState::Off : states.back(); }
};

struct FakeDisplay final : IDisplayActivity {
  bool refreshing = false;
  uint32_t generation = 0;
  bool refreshBusy() const override { return refreshing; }
  uint32_t refreshGeneration() const override { return generation; }
  void begin(uint32_t nextGeneration) { refreshing = true; generation = nextGeneration; }
  void end() { refreshing = false; }
};

struct FakeEvents final : IVoiceRuntimeEvents {
  struct Result { std::string command; std::string detail; };
  std::vector<RuntimeState> states;
  std::vector<Result> results;
  std::vector<std::string> confirmations;
  std::vector<Status> errors;
  void onRuntimeState(RuntimeState state) override { states.push_back(state); }
  void onCommandResult(const std::string& name, const std::string& detail) override {
    results.push_back(Result{name, detail});
  }
  void onConfirmationRequired(const std::string& phrase, bool, uint64_t) override {
    confirmations.push_back(phrase);
  }
  void onError(const Status& status) override { errors.push_back(status); }
};

struct FakeActions final : ILocalDeviceActions {
  VoiceRuntime* runtime = nullptr;
  int queryCalls = 0, listCalls = 0, selectCalls = 0, deleteCalls = 0;
  int clearCalls = 0, volumeCalls = 0, formatCalls = 0, promptCalls = 0;
  int imageSettingCalls = 0, resetCalls = 0;
  std::string target, key, value;
  int volume = -1;
  AlbumRevision album;

  FakeActions() { album.albumId = "user-album"; album.revision = 10; }
  Status queryFreeSpace(StorageSpace& output) override {
    ++queryCalls; output.storageId = "sd"; output.freeBytes = 80; output.totalBytes = 100;
    return Status::ok();
  }
  Status listImages(std::vector<ImageEntry>& output) override {
    ++listCalls;
    ImageEntry first; first.id = "asset-a"; first.ordinal = 1;
    ImageEntry second; second.id = "asset-b"; second.ordinal = 2;
    output.push_back(first); output.push_back(second); return Status::ok();
  }
  Status currentAlbumRevision(AlbumRevision& output) override {
    output = album; return Status::ok();
  }
  Status selectImage(const std::string& nextTarget, ImageSelection& output) override {
    ++selectCalls; target = nextTarget;
    output.id = nextTarget == "@2" ? "asset-b" : "asset-a";
    output.albumId = album.albumId;
    output.frameId = nextTarget == "@2" ? "frame-b" : "frame-a";
    output.revision = album.revision;
    output.ordinal = nextTarget == "@2" ? 2 : 1;
    output.total = 2;
    return Status::ok();
  }
  Status deleteImageById(const std::string& exactId) override {
    assert(runtime && !runtime->pendingConfirmation().active());
    ++deleteCalls; target = exactId; return Status::ok();
  }
  Status clearAllUserImages() override {
    assert(runtime && !runtime->pendingConfirmation().active());
    ++clearCalls; return Status::ok();
  }
  Status setVolumePercent(uint8_t nextVolume) override {
    ++volumeCalls; volume = nextVolume; return Status::ok();
  }
  Status formatStorage(const std::string& exactStorageId) override {
    assert(runtime && !runtime->pendingConfirmation().active());
    ++formatCalls; target = exactStorageId; return Status::ok();
  }
  Status setAssistantPrompt(const std::string& prompt) override {
    ++promptCalls; value = prompt; return Status::ok();
  }
  Status setImageSetting(const std::string& nextKey,
                         const std::string& nextValue) override {
    ++imageSettingCalls; key = nextKey; value = nextValue; return Status::ok();
  }
  Status resetTarget(const std::string& exactTargetId) override {
    assert(runtime && !runtime->pendingConfirmation().active());
    ++resetCalls; target = exactTargetId; return Status::ok();
  }
};

TranscriptDecision voiceCommand(VoiceRuntime& runtime, FakePlayer& player,
                                const std::string& text) {
  player.complete();
  Status start = runtime.onTopButtonTap();
  assert(start.success);
  const uint32_t generation = runtime.activeTurnGeneration();
  Status stop = runtime.onTopButtonTap();
  assert(stop.success);
  return runtime.onAsrFinal(text, generation);
}

int main() {
  LocalCommandParser parser;
  assert(parser.parse("查询剩余空间。").kind == CommandKind::QueryFreeSpace);
  assert(parser.parse("第二张").targetId == "@2");
  assert(parser.parse("delete image asset-123").kind == CommandKind::DeleteImage);
  const char* invalidIds[] = {".", "..", ".hidden", "a..", "a..b", "../x", "a\\b", "a%2fb", "c:"};
  for (size_t i = 0; i < sizeof(invalidIds) / sizeof(invalidIds[0]); ++i)
    assert(!parser.parse(std::string("delete image ") + invalidIds[i]).matched());
  assert(!parser.parse("volume 101").matched());
  assert(parser.parse("格式化 TF卡").targetId == "sd");

  FakeClock clock;
  FakeTransport transport;
  FakeLed led;
  FakeDisplay display;
  FakePlayer player;
  FakeActions actions;
  FakeEvents events;
  std::vector<std::string> sequence;
  transport.player = &player;
  transport.sequence = &sequence;
  player.transport = &transport;
  player.sequence = &sequence;
  AudioPromptController prompts(clock, display, player);
  VoiceRuntimeConfig config;
  config.listeningTimeoutMs = 100;
  config.thinkingTimeoutMs = 200;
  config.speakingTimeoutMs = 300;
  config.confirmationTimeoutMs = 500;
  config.cleanupRetryMs = 10;
  VoiceRuntime runtime(config, clock, transport, led, display, actions, prompts, events);
  transport.runtime = &runtime;
  actions.runtime = &runtime;
  runtime.setEnabled(true);
  runtime.onSessionReady();
  assert(runtime.state() == RuntimeState::Ready);

  // Packaged playback stops successfully before microphone capture starts.
  assert(player.play("voice.listening").success);
  sequence.clear();
  assert(runtime.onTopButtonTap().success);
  assert(sequence.size() >= 2 && sequence[0] == "prompt.stop" &&
         sequence[1] == "capture.start");
  assert(transport.capturing && !player.playing);
  const uint32_t firstTurn = runtime.activeTurnGeneration();
  assert(runtime.onTopButtonTap().success);
  assert(!transport.capturing && runtime.state() == RuntimeState::Thinking);
  TranscriptDecision unmatched = runtime.onAsrFinal("给我讲个故事", firstTurn);
  assert(!unmatched.handledLocally && transport.responses == 1);

  // The concrete MyAI Thinking cancel closes its socket/lease. Readiness is
  // invalidated inside VoiceRuntime before cleanup returns, so no transient
  // Ready event or Off LED is allowed until a fresh session.ready callback.
  transport.cancelClosesSession = true;
  const size_t thinkingCancelStateStart = events.states.size();
  const size_t thinkingCancelLedStart = led.states.size();
  assert(runtime.onTopButtonTap().success);
  assert(runtime.state() == RuntimeState::Error && !runtime.turnActive());
  for (size_t index = thinkingCancelStateStart; index < events.states.size(); ++index)
    assert(events.states[index] != RuntimeState::Ready);
  for (size_t index = thinkingCancelLedStart; index < led.states.size(); ++index)
    assert(led.states[index] != VoiceLedState::Off);
  const int responsesAfterCancel = transport.responses;
  assert(runtime.onAsrFinal("late response", firstTurn).commandName == "stale_turn");
  assert(transport.responses == responsesAfterCancel);
  assert(!runtime.onTopButtonTap().success);
  runtime.onSessionReady();
  assert(events.states.back() == RuntimeState::Ready && led.last() == VoiceLedState::Off);
  assert(runtime.onTopButtonTap().success && transport.capturing);
  assert(runtime.onTopButtonTap().success && runtime.state() == RuntimeState::Thinking);
  assert(runtime.onTopButtonTap().success && runtime.state() == RuntimeState::Error);
  runtime.onSessionReady();
  transport.cancelClosesSession = false;

  // A failed packaged stop prevents capture entirely and Error is locally resettable.
  player.failStop = true;
  const int startsBeforeStopFailure = transport.starts;
  assert(!runtime.onTopButtonTap().success);
  assert(transport.starts == startsBeforeStopFailure && runtime.state() == RuntimeState::Error);
  player.failStop = false;
  assert(runtime.onTopButtonTap().success && runtime.state() == RuntimeState::Ready);

  // A partially failed capture start and failed capture stop both force cleanup.
  transport.failStart = true;
  assert(!runtime.onTopButtonTap().success);
  assert(runtime.state() == RuntimeState::Error && !transport.capturing && !player.playing);
  transport.failStart = false;
  assert(!runtime.onTopButtonTap().success);  // reconnect is required after transport failure
  runtime.onSessionReady();
  assert(runtime.onTopButtonTap().success);
  transport.failStop = true;
  assert(!runtime.onTopButtonTap().success);
  assert(runtime.state() == RuntimeState::Error && !transport.capturing && !player.playing);
  transport.failStop = false;
  runtime.onSessionReady();

  // TTS may enter Speaking only after microphone stop succeeds.
  assert(runtime.onTopButtonTap().success);
  uint32_t ttsTurn = runtime.activeTurnGeneration();
  transport.failStop = true;
  assert(!runtime.onTtsStart(ttsTurn).success);
  assert(runtime.state() == RuntimeState::Error && !transport.capturing);
  transport.failStop = false;
  runtime.onSessionReady();
  assert(runtime.onTopButtonTap().success);
  ttsTurn = runtime.activeTurnGeneration();
  assert(runtime.onTtsStart(ttsTurn).success);
  transport.remotePlaying = true;  // integration enables speaker only after success
  assert(runtime.state() == RuntimeState::Speaking && !transport.capturing);
  transport.cancelClosesSession = true;
  transport.synchronousTtsStopOnCancel = true;
  const size_t speakingCancelStateStart = events.states.size();
  const size_t speakingCancelLedStart = led.states.size();
  assert(runtime.onTopButtonTap().success);
  assert(runtime.state() == RuntimeState::Error && !runtime.turnActive() &&
         !transport.remotePlaying);
  for (size_t index = speakingCancelStateStart; index < events.states.size(); ++index)
    assert(events.states[index] != RuntimeState::Ready);
  for (size_t index = speakingCancelLedStart; index < led.states.size(); ++index)
    assert(led.states[index] != VoiceLedState::Off);
  assert(!runtime.onTopButtonTap().success);
  runtime.onSessionReady();
  assert(events.states.back() == RuntimeState::Ready && led.last() == VoiceLedState::Off);
  transport.cancelClosesSession = false;
  transport.synchronousTtsStopOnCancel = false;

  // Display-busy Button C never starts voice and its cue is generation-bound.
  display.begin(77);
  player.complete();
  const int startsBeforeBusy = transport.starts;
  assert(!runtime.onTopButtonTap().success);
  assert(transport.starts == startsBeforeBusy && player.count("display.please_wait") == 1);
  display.end();
  assert(runtime.onDisplayRefreshEnded(77).success);
  assert(!player.playing);

  // A queued busy cue disappears if its matching generation ends before playback.
  assert(player.play("settings.saved").success);
  display.begin(78);
  assert(!runtime.onTopButtonTap().success);
  const size_t waitsBeforeEnd = player.count("display.please_wait");
  display.end();
  player.complete();
  assert(prompts.tick().success);
  assert(player.count("display.please_wait") == waitsBeforeEnd);

  // Selection queues an ordinal; only an exact successful display commit plays it.
  TranscriptDecision select = voiceCommand(runtime, player, "第二张");
  assert(select.handledLocally && actions.selectCalls == 1);
  assert(player.count("ordinal.second") == 0);
  assert(!runtime.onDisplayCommitSuccess("wrong", 10, 2).success);
  assert(player.count("ordinal.second") == 0);
  runtime.onDisplayCommitFailure("frame-b", 10);
  assert(!runtime.onDisplayCommitSuccess("frame-b", 10, 2).success);
  voiceCommand(runtime, player, "第二张");
  assert(runtime.onDisplayCommitSuccess("frame-b", 10, 2).success);
  assert(player.count("ordinal.second") == 1);

  // Pending destructive intents bind album+revision and expire on revision changes.
  TranscriptDecision deleteStart = voiceCommand(runtime, player, "删除图片 asset-a");
  assert(deleteStart.awaitingConfirmation);
  assert(runtime.pendingConfirmation().targetAlbumId == "user-album");
  assert(runtime.pendingConfirmation().targetRevision == 10);
  actions.album.revision = 11;
  voiceCommand(runtime, player, "确认删除图片 asset-a");
  assert(actions.deleteCalls == 0 && !runtime.pendingConfirmation().active());
  assert(events.results.back().detail == "confirmation_context_changed");

  // Transport error clears pending intent; reconnect never restores it.
  actions.album.revision = 12;
  voiceCommand(runtime, player, "删除图片 asset-a");
  assert(runtime.pendingConfirmation().active());
  const uint32_t staleConfirmationTurn = runtime.activeTurnGeneration();
  runtime.onTransportError(Status::error("disconnect", "link lost"));
  assert(!runtime.pendingConfirmation().active());
  assert(!transport.capturing && !player.playing && runtime.state() == RuntimeState::Error);
  assert(!runtime.onTopButtonTap().success);  // safe Error reset waits for reconnect
  runtime.onSessionReady();
  assert(!runtime.pendingConfirmation().active() && runtime.state() == RuntimeState::Ready);
  runtime.onAsrFinal("确认删除图片 asset-a", staleConfirmationTurn);
  assert(actions.deleteCalls == 0);

  // Any ambiguous or wrong confirmation consumes destructive authority. A
  // later exact phrase cannot revive or execute the old request.
  voiceCommand(runtime, player, "删除图片 asset-a");
  assert(runtime.pendingConfirmation().active());
  TranscriptDecision wrongConfirmation =
      voiceCommand(runtime, player, "this is not the confirmation");
  assert(wrongConfirmation.handledLocally &&
         !wrongConfirmation.awaitingConfirmation);
  assert(!runtime.pendingConfirmation().active() && actions.deleteCalls == 0);
  assert(events.results.back().detail == "spoken_confirmation_mismatch");
  voiceCommand(runtime, player, "确认删除图片 asset-a");
  assert(!runtime.pendingConfirmation().active() && actions.deleteCalls == 0);
  assert(runtime.onTopButtonTap().success);  // cancel unmatched remote fallback

  // A second destructive phrase and an empty/ambiguous final are mismatches,
  // not replacement authorization or invitations to keep retrying.
  voiceCommand(runtime, player, "clear all images");
  voiceCommand(runtime, player, "delete image asset-a");
  assert(!runtime.pendingConfirmation().active());
  assert(actions.clearCalls == 0 && actions.deleteCalls == 0);
  voiceCommand(runtime, player, "confirm clear all images");
  assert(actions.clearCalls == 0);
  assert(runtime.onTopButtonTap().success);
  voiceCommand(runtime, player, "delete image asset-a");
  voiceCommand(runtime, player, "");
  assert(!runtime.pendingConfirmation().active() && actions.deleteCalls == 0);

  // A voice event arriving after promotion to physical confirmation is not a
  // trusted button gesture and irrevocably invalidates that pending action.
  voiceCommand(runtime, player, "clear all images");
  voiceCommand(runtime, player, "confirm clear all images");
  assert(runtime.pendingConfirmation().stage == ConfirmationStage::Physical);
  const uint32_t physicalStageTurn = runtime.activeTurnGeneration();
  TranscriptDecision invalidPhysical =
      runtime.onAsrFinal("wrong late final", physicalStageTurn);
  assert(invalidPhysical.handledLocally &&
         !invalidPhysical.awaitingConfirmation);
  assert(!runtime.pendingConfirmation().active() && actions.clearCalls == 0);
  assert(events.results.back().detail ==
         "physical_confirmation_invalidated_by_asr");
  assert(!runtime.onTrustedPhysicalConfirmation().success);

  // Session-ready itself invalidates intent, even without an explicit loss callback.
  voiceCommand(runtime, player, "删除图片 asset-a");
  assert(runtime.pendingConfirmation().active());
  runtime.onSessionReady();
  assert(!runtime.pendingConfirmation().active());

  // Confirmation expiry during capture never starts packaged audio over the mic.
  voiceCommand(runtime, player, "删除图片 asset-a");
  player.complete();
  assert(runtime.onTopButtonTap().success);
  const uint32_t expiredCaptureTurn = runtime.activeTurnGeneration();
  clock.advance(501);
  runtime.tick();
  assert(!player.playing && !runtime.pendingConfirmation().active());
  assert(!transport.capturing && runtime.state() == RuntimeState::Thinking);
  assert(runtime.onAsrFinal("确认删除图片 asset-a", expiredCaptureTurn).handledLocally == false);
  assert(runtime.onTopButtonTap().success);  // cancel unmatched fallback

  // Exact confirmation is consumed before dispatch; replay/bounce cannot repeat it.
  voiceCommand(runtime, player, "删除图片 asset-a");
  voiceCommand(runtime, player, "确认删除图片 asset-a");
  assert(actions.deleteCalls == 1 && !runtime.pendingConfirmation().active());
  voiceCommand(runtime, player, "确认删除图片 asset-a");
  assert(actions.deleteCalls == 1 && transport.responses >= 2);
  assert(runtime.onTopButtonTap().success);  // cancel unmatched remote fallback

  voiceCommand(runtime, player, "clear all images");
  voiceCommand(runtime, player, "confirm clear all images");
  assert(runtime.pendingConfirmation().stage == ConfirmationStage::Physical);
  assert(runtime.onTopButtonTap().success);
  assert(actions.clearCalls == 1 && !runtime.pendingConfirmation().active());
  assert(!runtime.onTrustedPhysicalConfirmation().success && actions.clearCalls == 1);

  // Timeout and generic transport failures release every tracked audio mode.
  runtime.onSessionReady();
  assert(runtime.onTopButtonTap().success);
  assert(runtime.onTopButtonTap().success);
  clock.advance(201);
  runtime.tick();
  assert(runtime.state() == RuntimeState::Error && !transport.capturing && !player.playing);
  runtime.onSessionReady();
  transport.failResponse = true;
  const int responsesBeforeFailure = transport.responses;
  voiceCommand(runtime, player, "unmatched request");
  assert(transport.responses == responsesBeforeFailure + 1);
  assert(runtime.state() == RuntimeState::Error && !transport.capturing && !player.playing);
  transport.failResponse = false;

  // A current-generation response.done is invalid while Listening. It must
  // fail closed and retain an Error cleanup barrier until capture is proven
  // stopped, retrying at most once per configured interval.
  runtime.onSessionReady();
  assert(runtime.onTopButtonTap().success);
  const uint32_t earlyDoneTurn = runtime.activeTurnGeneration();
  transport.failCancelTurn = true;
  const int cancelsBeforeEarlyDone = transport.turnCancels;
  runtime.onResponseDone(earlyDoneTurn);
  assert(runtime.state() == RuntimeState::Error && runtime.cleanupPending());
  assert(runtime.captureActive() && runtime.turnActive() && transport.capturing);
  assert(led.last() == VoiceLedState::Error);
  assert(transport.turnCancels == cancelsBeforeEarlyDone + 1);
  runtime.tick();
  assert(transport.turnCancels == cancelsBeforeEarlyDone + 1);
  clock.advance(10);
  runtime.tick();
  assert(transport.turnCancels == cancelsBeforeEarlyDone + 2);
  assert(runtime.state() == RuntimeState::Error && runtime.cleanupPending());
  transport.failCancelTurn = false;
  clock.advance(10);
  runtime.tick();
  assert(runtime.state() == RuntimeState::Error && !runtime.cleanupPending());
  assert(!runtime.captureActive() && !runtime.turnActive() && !transport.capturing);
  assert(events.errors.back().code == "turn_cancel_failed");
  runtime.onSessionReady();
  assert(runtime.state() == RuntimeState::Ready);

  // Disable/sleep entry never publishes Disabled/Off over possibly live
  // capture. Disabled session callbacks remain behind the same retry barrier.
  assert(runtime.onTopButtonTap().success);
  transport.failCancelTurn = true;
  runtime.setEnabled(false);
  assert(runtime.state() == RuntimeState::Error && runtime.cleanupPending());
  assert(runtime.captureActive() && transport.capturing);
  assert(led.last() == VoiceLedState::Error);
  runtime.onSessionReady();
  assert(runtime.state() == RuntimeState::Error && runtime.cleanupPending());
  assert(led.last() == VoiceLedState::Error && transport.capturing);
  const int cancelsBeforeDisableRetry = transport.turnCancels;
  runtime.tick();
  assert(transport.turnCancels == cancelsBeforeDisableRetry);
  transport.failCancelTurn = false;
  clock.advance(10);
  runtime.tick();
  assert(runtime.state() == RuntimeState::Disabled && !runtime.cleanupPending());
  assert(!runtime.captureActive() && !runtime.turnActive() && !transport.capturing);
  assert(led.last() == VoiceLedState::Off);
  runtime.onSessionReady();
  assert(runtime.state() == RuntimeState::Disabled && led.last() == VoiceLedState::Off);
  runtime.setEnabled(true);
  runtime.onSessionReady();
  assert(runtime.state() == RuntimeState::Ready);

  return 0;
}
`;

  try {
    await writeFile(harnessPath, harness);
    const includePath = sourceUrl.pathname;
    const compile = spawnSync(
      "c++",
      [
        "-std=c++11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        `-I${includePath}`,
        harnessPath,
        join(includePath, "LocalCommandParser.cpp"),
        join(includePath, "AudioPromptController.cpp"),
        join(includePath, "VoiceRuntime.cpp"),
        "-o",
        executablePath,
      ],
      { encoding: "utf8" },
    );
    assert.equal(compile.status, 0, compile.stderr || compile.stdout);
    const run = spawnSync(executablePath, [], {
      encoding: "utf8",
      env: { ...process.env, ASAN_OPTIONS: "abort_on_error=1" },
    });
    assert.equal(run.status, 0, run.stderr || run.stdout);
  } finally {
    await rm(temporaryDirectory, { recursive: true, force: true });
  }
});

test("PaperColor voice package remains service-independent and exposes guarded seams", async () => {
  const files = await Promise.all([
    "LocalCommandParser.cpp",
    "VoiceRuntime.cpp",
    "AudioPromptController.cpp",
    "VoiceAdapters.h",
  ].map((name) => readFile(new URL(name, sourceUrl), "utf8")));
  const combined = files.join("\n");
  assert.match(combined, /stop\(\)[\s\S]*startListening/);
  assert.match(combined, /activeTurnGeneration/);
  assert.match(combined, /targetAlbumId/);
  assert.match(combined, /targetRevision/);
  assert.match(combined, /displayCommitSuccess/);
  assert.match(combined, /refreshGeneration/);
  assert.match(combined, /confirmation_context_changed/);
  assert.doesNotMatch(combined, /myai\.mess\.host|\/api\/v1\/|\/gateway\/v1\//);
  assert.doesNotMatch(combined, /HTTPClient|WebSocketsClient|WiFiClient/);
});

test("PaperColor offline prompt manifest resolves every runtime prompt with strict WAV checks", async () => {
  const moduleUrl = new URL("../firmware/m5-papercolor/lib/InkloopVoice/", import.meta.url);
  const verifier = new URL("scripts/generate-prompts.mjs", moduleUrl);
  const verified = spawnSync(process.execPath, [verifier.pathname, "--verify"], {
    encoding: "utf8",
  });
  assert.equal(verified.status, 0, verified.stderr || verified.stdout);
  assert.match(verified.stdout, /verified 19 offline prompt entries/);

  const manifest = JSON.parse(await readFile(new URL("assets/prompts.v1.json", moduleUrl), "utf8"));
  assert.equal(manifest.schema, "inkloop.prompt-manifest");
  assert.equal(manifest.version, 1);
  assert.deepEqual(manifest.format, {
    container: "wav",
    codec: "pcm_s16le",
    channels: 1,
    sampleRate: 16000,
  });
  const entries = new Map(manifest.entries.map((entry) => [entry.id, entry]));
  for (const id of [
    "ordinal.first",
    "ordinal.second",
    "ordinal.third",
    "display.please_wait",
    "voice.error",
    "voice.listening",
  ]) {
    assert(entries.has(id), `missing required prompt ${id}`);
    assert.match(entries.get(id).source, /^macos-say:Tingting$/);
  }

  const runtimeSources = await Promise.all([
    "VoiceRuntime.cpp",
    "AudioPromptController.cpp",
  ].map((name) => readFile(new URL(`src/${name}`, moduleUrl), "utf8")));
  const referenced = new Set(
    [...runtimeSources.join("\n").matchAll(/"((?:ordinal|display|confirmation|storage|images|settings|voice)\.[a-z_]+)"/g)]
      .map((match) => match[1]),
  );
  for (const id of referenced) assert(entries.has(id), `unresolved runtime prompt ${id}`);
});

test("PaperColor full PlatformIO build executes the InkloopVoice prompt gate", { timeout: 180_000 }, async () => {
  const firmwareDirectory = new URL("../firmware/m5-papercolor/", import.meta.url).pathname;
  const build = spawnSync(
    await platformioCommand(),
    ["run", "-d", firmwareDirectory, "-e", "m5stack-papercolor"],
    { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 },
  );
  const transcript = `${build.stdout || ""}\n${build.stderr || ""}`;
  assert.equal(build.error, undefined, transcript);
  assert.equal(build.status, 0, transcript);
  assert.match(transcript, /\[SUCCESS\]/);
  assert.doesNotMatch(transcript, /missing SConscript|pre:scripts/);
});
