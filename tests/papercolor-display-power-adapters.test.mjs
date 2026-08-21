import assert from "node:assert/strict";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import { deflateSync } from "node:zlib";
import test from "node:test";

const moduleSource = new URL(
  "../firmware/m5-papercolor/lib/InkloopDisplayPower/",
  import.meta.url,
);

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const typeBytes = Buffer.from(type, "ascii");
  const result = Buffer.alloc(12 + data.length);
  result.writeUInt32BE(data.length, 0);
  typeBytes.copy(result, 4);
  data.copy(result, 8);
  result.writeUInt32BE(crc32(Buffer.concat([typeBytes, data])), 8 + data.length);
  return result;
}

function makePng(width, height, idatType = "IDAT") {
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(height, 4);
  header[8] = 8;
  header[9] = 2;
  const raw = Buffer.alloc((width * 3 + 1) * height);
  for (let y = 0; y < height; y += 1) {
    const row = y * (width * 3 + 1);
    raw[row] = 0;
    for (let x = 0; x < width; x += 1) {
      const pixel = row + 1 + x * 3;
      raw[pixel] = (x * 5 + y) & 255;
      raw[pixel + 1] = (x + y * 3) & 255;
      raw[pixel + 2] = (x * 7 + y * 11) & 255;
    }
  }
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk("IHDR", header),
    chunk(idatType, deflateSync(raw)),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

test("attested display runtime performs real deterministic conversion and safe power sequencing", async () => {
  const temporaryDirectory = await mkdtemp(join(tmpdir(), "inkloop-display-power-adapter-"));
  const harnessPath = join(temporaryDirectory, "display_power_adapter_test.cpp");
  const executablePath = join(temporaryDirectory, "display_power_adapter_test");
  const validPngPath = join(temporaryDirectory, "valid.png");
  const bottomDownPngPath = join(temporaryDirectory, "bottom-down.png");
  const wrongSizePngPath = join(temporaryDirectory, "wrong-size.png");
  const reservedChunkPngPath = join(temporaryDirectory, "reserved-chunk.png");
  const validPng = makePng(400, 600);
  await writeFile(validPngPath, validPng);
  await writeFile(bottomDownPngPath, makePng(600, 400));
  await writeFile(wrongSizePngPath, makePng(401, 600));
  await writeFile(reservedChunkPngPath, makePng(400, 600, "IDaT"));

  const harness = String.raw`
#include <cassert>
#include <cstdint>
      #include <fstream>
      #include <iterator>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

#include "DisplayPowerRuntime.h"

using namespace inkloop::displaypower;

std::vector<uint8_t> load(const char* path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<uint8_t>(
      std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

uint64_t hashPixels(const std::vector<RgbPixel>& pixels) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t index = 0; index < pixels.size(); ++index) {
    hash ^= pixels[index].red; hash *= 1099511628211ULL;
    hash ^= pixels[index].green; hash *= 1099511628211ULL;
    hash ^= pixels[index].blue; hash *= 1099511628211ULL;
  }
  return hash;
}

class FakeClock final : public IDisplayPowerClock {
 public:
  mutable uint32_t now = 0;
  uint32_t nowMilliseconds() const override { return now++; }
};

class FakeLock final : public IDisplayPowerLock {
 public:
  bool locked = false;
  int acquisitions = 0;
  bool tryLock() override {
    if (locked) return false;
    locked = true;
    ++acquisitions;
    return true;
  }
  void unlock() override { assert(locked); locked = false; }
};

class FakeLed final : public IImageLedSink {
 public:
  std::vector<ImageLedState> states;
  ImageLedState current = ImageLedState::Off;
  ImageLedState rejected = static_cast<ImageLedState>(255);
  int ticks = 0;
  int quiesces = 0;

  bool setImageState(ImageLedState state, uint32_t) override {
    states.push_back(state);
    current = state;
    return state != rejected;
  }
  bool tick(uint32_t) override { ++ticks; return true; }
  bool quiesce(uint32_t now) override {
    ++quiesces;
    return setImageState(ImageLedState::Off, now);
  }
};

class FakeDecoder final : public IPngPixelDecoder {
 public:
  explicit FakeDecoder(FakeLed& led) : led_(led) {}
  bool fail = false;
  bool ready = false;
  size_t index = 0;
  int decodes = 0;
  int resets = 0;
  bool conversionStateObserved = false;

  bool decode(const ValidatedPng& png, const uint8_t* bytes, size_t length) override {
    ++decodes;
    conversionStateObserved = led_.current == ImageLedState::Converting;
    ready = !fail && png.matchesExactBytes(bytes, length);
    index = 0;
    return ready;
  }
  void reset() override { ready = false; index = 0; ++resets; }
  uint16_t width() const override { return ready ? 400 : 0; }
  uint16_t height() const override { return ready ? 600 : 0; }
  bool read(RgbPixel* pixel) override {
    assert(led_.current == ImageLedState::Converting);
    const size_t count = static_cast<size_t>(400) * 600;
    if (!ready || !pixel || index >= count) return false;
    *pixel = RgbPixel(
        static_cast<uint8_t>(index % 256),
        static_cast<uint8_t>((index / 7) % 256),
        static_cast<uint8_t>((index / 31) % 256));
    ++index;
    return true;
  }

 private:
  FakeLed& led_;
};

class FakeDisplay final : public IFullScreenDisplay {
 public:
  const void* writer = nullptr;
  int officialRenders = 0;
  int experimentalRenders = 0;
  bool renderResult = true;
  bool capabilityValid = false;
  bool paletteValid = false;
  uint64_t paletteHash = 0;
  DisplayRefreshRuntime* nestedRuntime = nullptr;
  const EncodedFrameRequest* nestedRequest = nullptr;
  DisplayRefreshResult nestedResult = DisplayRefreshResult::Complete;

  bool claimSoleWriter(const void* candidate) override {
    if (!candidate || writer) return false;
    writer = candidate;
    return true;
  }
  bool renderOfficialPng(
      const PhysicalRefreshCapability& capability,
      const ValidatedPng& png,
      const uint8_t* bytes,
      size_t length) override {
    capabilityValid = capability.validFor(this, writer);
    ++officialRenders;
    if (nestedRuntime && nestedRequest) {
      DisplayRefreshRuntime* runtime = nestedRuntime;
      const EncodedFrameRequest* request = nestedRequest;
      nestedRuntime = nullptr;
      nestedRequest = nullptr;
      nestedResult = runtime->refresh(*request);
    }
    return renderResult && capabilityValid && png.matchesExactBytes(bytes, length);
  }
  bool renderExperimentalPalette(
      const PhysicalRefreshCapability& capability,
      const PaletteFrame& frame) override {
    capabilityValid = capability.validFor(this, writer);
    ++experimentalRenders;
    paletteValid = frame.valid();
    paletteHash = hashPixels(frame.pixels());
    for (size_t index = 0; index < frame.pixels().size(); ++index) {
      assert(isPaperColorPalettePixel(frame.pixels()[index]));
    }
    return renderResult && capabilityValid && paletteValid;
  }
};

EncodedFrameRequest requestFor(
    const std::vector<uint8_t>& bytes,
    RenderStrategy strategy = RenderStrategy::OfficialQuality) {
  EncodedFrameRequest request;
  request.assetId = "asset:one";
  request.bytes = bytes.data();
  request.length = bytes.size();
  request.strategy = strategy;
  return request;
}

class FakeSleep final : public IDeepSleepPlatform {
 public:
  int resets = 0;
  int timers = 0;
  int buttons = 0;
  int enters = 0;
  uint64_t timerSeconds = 0;
  uint64_t buttonMask = 0;
  bool resetResult = true;
  bool timerResult = true;
  bool buttonResult = true;
  bool enterResult = true;
  bool resetWakeSources() override { ++resets; return resetResult; }
  bool enableTimerWakeAfterSeconds(uint64_t value) override {
    ++timers; timerSeconds = value; return timerResult;
  }
  bool enableAnyLowWake(uint64_t value) override {
    ++buttons; buttonMask = value; return buttonResult;
  }
  bool enterDeepSleep() override { ++enters; return enterResult; }
};

PowerInputs idleInputs(uint64_t epoch = 1000) {
  PowerInputs inputs;
  inputs.nowMilliseconds = 120000;
  inputs.lastMeaningfulActivityMilliseconds = 0;
  inputs.rtcNowEpochSeconds = epoch;
  inputs.nextHeartbeatEpochSeconds = epoch + 300;
  inputs.rtcSynchronized = true;
  inputs.wakeButtonsReleased = true;
  return inputs;
}

class FakeQuiescence final : public IPreSleepQuiescenceHooks {
 public:
  std::vector<std::string> calls;
  PowerInputs initial = idleInputs();
  PowerInputs final = idleInputs(1001);
  int captures = 0;
  bool captureResult = true;
  bool finalizeResult = true;
  bool audioResult = true;
  bool rgbResult = true;
  bool networkResult = true;

  bool capturePowerInputs(PowerInputs* inputs) override {
    calls.push_back(captures == 0 ? "snapshot-initial" : "snapshot-final");
    if (!captureResult || !inputs) return false;
    *inputs = captures++ == 0 ? initial : final;
    return true;
  }
  bool finalizeTaskAndDisplay() override {
    calls.push_back("finalize"); return finalizeResult;
  }
  bool stopAudio() override { calls.push_back("audio"); return audioResult; }
  bool stopImageRgb() override { calls.push_back("rgb"); return rgbResult; }
  bool closeNetwork() override { calls.push_back("network"); return networkResult; }
};

class FakeWakeHooks final : public IWakeRecoveryHooks {
 public:
  int reconnects = 0;
  int syncs = 0;
  int rearms = 0;
  bool wifi = false;
  bool sync = false;
  bool released = false;
  bool reconnectWifi() override { ++reconnects; return wifi; }
  bool syncInkloopSchedules() override { ++syncs; return sync; }
  bool allWakeButtonsReleased() override { return released; }
  bool rearmButtonInput() override { ++rearms; return true; }
};

int main(int argc, char** argv) {
  assert(argc == 5);
  const std::vector<uint8_t> png = load(argv[1]);
  const std::vector<uint8_t> bottomDownPng = load(argv[2]);
  const std::vector<uint8_t> wrongSizePng = load(argv[3]);
  const std::vector<uint8_t> reservedChunkPng = load(argv[4]);
  assert(!png.empty() && !bottomDownPng.empty() &&
         !wrongSizePng.empty() && !reservedChunkPng.empty());
  static_assert(!std::is_default_constructible<PhysicalRefreshCapability>::value,
      "physical capability must not be forgeable");
  static_assert(!std::is_copy_constructible<PhysicalRefreshCapability>::value,
      "physical capability must not escape the call");

  const PngValidationResult valid = validatePaperColorPng(png.data(), png.size());
  assert(valid.error == PngValidationError::None && valid.png.valid());
  assert(valid.png.width() == 400 && valid.png.height() == 600);
  assert(valid.png.matchesExactBytes(png.data(), png.size()));
  const PngValidationResult bottomDown = validatePaperColorPng(
      bottomDownPng.data(), bottomDownPng.size());
  assert(bottomDown.error == PngValidationError::None &&
         bottomDown.png.width() == 600 && bottomDown.png.height() == 400);
  assert(validatePaperColorPng(wrongSizePng.data(), wrongSizePng.size()).error ==
      PngValidationError::WrongDimensions);
  assert(validatePaperColorPng(reservedChunkPng.data(), reservedChunkPng.size()).error ==
      PngValidationError::InvalidChunkType);
  std::vector<uint8_t> tampered = png;
  tampered[tampered.size() - 20] ^= 1;
  assert(validatePaperColorPng(tampered.data(), tampered.size()).error ==
      PngValidationError::InvalidChunkCrc);
  std::vector<uint8_t> trailing = png;
  trailing.push_back(0);
  assert(validatePaperColorPng(trailing.data(), trailing.size()).error ==
      PngValidationError::TrailingData);
  assert(validatePaperColorPng(png.data(), 20).error == PngValidationError::TruncatedChunk);
  assert(validatePaperColorPng(png.data(), png.size(), png.size() - 1).error ==
      PngValidationError::EncodedSizeOutOfRange);
  const char abc[] = "abc";
  const Sha256Digest abcDigest = sha256Bytes(
      reinterpret_cast<const uint8_t*>(abc), 3);
  const uint8_t expectedSha[32] = {
      0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
      0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
  for (size_t index = 0; index < 32; ++index) assert(abcDigest.bytes[index] == expectedSha[index]);

  FakeDisplay disabledDisplay;
  FakeLed disabledLed;
  FakeDecoder disabledDecoder(disabledLed);
  FakeLock disabledLock;
  FakeClock disabledClock;
  DisplayRefreshRuntimeConfig disabledConfig;
  DisplayRefreshRuntime disabled(
      disabledDisplay, disabledLed, disabledDecoder, disabledLock, disabledClock, disabledConfig);
  EncodedFrameRequest officialRequest = requestFor(png);
  assert(disabled.refresh(officialRequest) == DisplayRefreshResult::Disabled);
  assert(disabledDisplay.writer == nullptr);

  DisplayRefreshRuntimeConfig config;
  config.enabled = true;
  config.cooldownMilliseconds = 0;
  FakeDisplay officialDisplay;
  FakeLed officialLed;
  FakeDecoder officialDecoder(officialLed);
  FakeLock officialLock;
  FakeClock officialClock;
  DisplayRefreshRuntime official(
      officialDisplay, officialLed, officialDecoder, officialLock, officialClock, config);
  assert(official.refresh(officialRequest) == DisplayRefreshResult::Complete);
  assert(officialDisplay.officialRenders == 1 && officialDisplay.capabilityValid);
  assert(officialDecoder.decodes == 0);
  assert(officialLed.states[0] == ImageLedState::Writing);
  assert(officialLed.states[1] == ImageLedState::Complete);
  EncodedFrameRequest duplicateOfficial = officialRequest;
  duplicateOfficial.assetId = "different-id-same-attested-frame";
  assert(official.refresh(duplicateOfficial) == DisplayRefreshResult::Unchanged);
  assert(officialDisplay.officialRenders == 1);
  assert(officialLed.states.size() == 2);
  EncodedFrameRequest bottomDownOfficial = requestFor(bottomDownPng);
  bottomDownOfficial.assetId = "asset:bottom-down";
  assert(official.refresh(bottomDownOfficial) == DisplayRefreshResult::Complete);
  assert(officialDisplay.officialRenders == 2);

  EncodedFrameRequest fakeBytes;
  const uint8_t fiveBytes[] = {137, 80, 78, 71, 1};
  fakeBytes.assetId = "fake";
  fakeBytes.bytes = fiveBytes;
  fakeBytes.length = sizeof(fiveBytes);
  assert(official.refresh(fakeBytes) == DisplayRefreshResult::InvalidEncodedPng);
  EncodedFrameRequest wrongRequest = requestFor(wrongSizePng);
  assert(official.refresh(wrongRequest) == DisplayRefreshResult::InvalidEncodedPng);

  EncodedFrameRequest experimentalRequest = requestFor(
      png, RenderStrategy::ExperimentalSixColor);
  assert(official.refresh(experimentalRequest) == DisplayRefreshResult::ExperimentalDisabled);

  DisplayRefreshRuntimeConfig experimentalConfig = config;
  experimentalConfig.experimentalPrequantizationEnabled = true;
  FakeDisplay experimentalDisplay;
  FakeLed experimentalLed;
  FakeDecoder experimentalDecoder(experimentalLed);
  FakeLock experimentalLock;
  FakeClock experimentalClock;
  DisplayRefreshRuntime experimental(
      experimentalDisplay,
      experimentalLed,
      experimentalDecoder,
      experimentalLock,
      experimentalClock,
      experimentalConfig);
  assert(experimental.refresh(experimentalRequest) == DisplayRefreshResult::Complete);
  assert(experimentalDecoder.conversionStateObserved);
  assert(experimentalDecoder.decodes == 1 && experimentalDecoder.resets == 1);
  assert(experimentalDisplay.experimentalRenders == 1 && experimentalDisplay.paletteValid);
  assert(experimentalDisplay.officialRenders == 0);
  assert(experimentalLed.states.front() == ImageLedState::Converting);
  assert(experimentalLed.states[1] == ImageLedState::Writing);
  assert(experimentalLed.states.back() == ImageLedState::Complete);
  assert(experimentalLed.ticks >= 600);
  EncodedFrameRequest bottomDownExperimental = requestFor(
      bottomDownPng, RenderStrategy::ExperimentalSixColor);
  bottomDownExperimental.assetId = "asset:bottom-down-experimental";
  assert(experimental.refresh(bottomDownExperimental) == DisplayRefreshResult::Complete);
  assert(experimentalDisplay.officialRenders == 1);
  assert(experimentalDecoder.decodes == 1);

  FakeDisplay deterministicDisplay;
  FakeLed deterministicLed;
  FakeDecoder deterministicDecoder(deterministicLed);
  FakeLock deterministicLock;
  FakeClock deterministicClock;
  DisplayRefreshRuntime deterministic(
      deterministicDisplay,
      deterministicLed,
      deterministicDecoder,
      deterministicLock,
      deterministicClock,
      experimentalConfig);
  assert(deterministic.refresh(experimentalRequest) == DisplayRefreshResult::Complete);
  assert(deterministicDisplay.paletteHash == experimentalDisplay.paletteHash);

  FakeDisplay nestedDisplay;
  FakeLed nestedLed;
  FakeDecoder nestedDecoder(nestedLed);
  FakeLock nestedLock;
  FakeClock nestedClock;
  DisplayRefreshRuntime nested(
      nestedDisplay, nestedLed, nestedDecoder, nestedLock, nestedClock, config);
  nestedDisplay.nestedRuntime = &nested;
  nestedDisplay.nestedRequest = &officialRequest;
  assert(nested.refresh(officialRequest) == DisplayRefreshResult::Complete);
  assert(nestedDisplay.nestedResult == DisplayRefreshResult::Busy);
  assert(nestedDisplay.officialRenders == 1);

  FakeDisplay secondWriterDisplay;
  FakeLed firstWriterLed;
  FakeDecoder firstWriterDecoder(firstWriterLed);
  FakeLock firstWriterLock;
  FakeClock firstWriterClock;
  DisplayRefreshRuntime firstWriter(
      secondWriterDisplay,
      firstWriterLed,
      firstWriterDecoder,
      firstWriterLock,
      firstWriterClock,
      config);
  FakeLed secondWriterLed;
  FakeDecoder secondWriterDecoder(secondWriterLed);
  FakeLock secondWriterLock;
  FakeClock secondWriterClock;
  DisplayRefreshRuntime secondWriter(
      secondWriterDisplay,
      secondWriterLed,
      secondWriterDecoder,
      secondWriterLock,
      secondWriterClock,
      config);
  assert(secondWriter.refresh(officialRequest) == DisplayRefreshResult::SoleWriterUnavailable);

  FakeDisplay decodeFailureDisplay;
  FakeLed decodeFailureLed;
  FakeDecoder decodeFailureDecoder(decodeFailureLed);
  decodeFailureDecoder.fail = true;
  FakeLock decodeFailureLock;
  FakeClock decodeFailureClock;
  DisplayRefreshRuntime decodeFailure(
      decodeFailureDisplay,
      decodeFailureLed,
      decodeFailureDecoder,
      decodeFailureLock,
      decodeFailureClock,
      experimentalConfig);
  assert(decodeFailure.refresh(experimentalRequest) == DisplayRefreshResult::DecodeFailed);
  assert(decodeFailureLed.states.back() == ImageLedState::Error);

  FakeDisplay ledFailureDisplay;
  FakeLed ledFailureLed;
  ledFailureLed.rejected = ImageLedState::Writing;
  FakeDecoder ledFailureDecoder(ledFailureLed);
  FakeLock ledFailureLock;
  FakeClock ledFailureClock;
  DisplayRefreshRuntime ledFailure(
      ledFailureDisplay,
      ledFailureLed,
      ledFailureDecoder,
      ledFailureLock,
      ledFailureClock,
      config);
  assert(ledFailure.refresh(officialRequest) == DisplayRefreshResult::LedUnavailable);
  assert(ledFailureDisplay.officialRenders == 0);

  PowerPolicyConfig powerConfig;
  powerConfig.mode = PowerMode::BatteryOptIn;
  PowerPolicy powerPolicy(powerConfig);
  FakeQuiescence quiescence;
  FakeSleep sleep;
  assert(prepareAndExecuteSleep(powerPolicy, quiescence, sleep) == PrepareSleepResult::Entered);
  const std::vector<std::string> expectedOrder = {
      "snapshot-initial", "finalize", "audio", "rgb", "network", "snapshot-final"};
  assert(quiescence.calls == expectedOrder);
  assert(sleep.enters == 1 && sleep.timerSeconds == 270 && sleep.buttonMask == 0x602ULL);

  FakeQuiescence finalBlocked;
  finalBlocked.final.blockers.writeActive = true;
  FakeSleep blockedSleep;
  assert(prepareAndExecuteSleep(powerPolicy, finalBlocked, blockedSleep) ==
      PrepareSleepResult::RecheckNotEligible);
  assert(blockedSleep.enters == 0);
  FakeQuiescence audioFailure;
  audioFailure.audioResult = false;
  FakeSleep audioSleep;
  assert(prepareAndExecuteSleep(powerPolicy, audioFailure, audioSleep) ==
      PrepareSleepResult::AudioQuiescenceFailed);
  assert(audioFailure.calls.size() == 3 && audioSleep.enters == 0);
  FakeQuiescence networkFailure;
  networkFailure.networkResult = false;
  FakeSleep networkSleep;
  assert(prepareAndExecuteSleep(powerPolicy, networkFailure, networkSleep) ==
      PrepareSleepResult::NetworkQuiescenceFailed);
  assert(networkSleep.enters == 0);

  FakeWakeHooks retryHooks;
  WakeRecoveryConfig retryConfig;
  retryConfig.releaseDebounceMilliseconds = 50;
  retryConfig.retryIntervalMilliseconds = 10;
  retryConfig.maximumAttemptsPerStage = 2;
  WakeRecoveryRuntime retry(retryHooks, retryConfig);
  assert(retry.configurationValid() && retry.beginAfterHardwareReady(WakeReason::RtcTimer));
  retry.poll(0);
  assert(retryHooks.reconnects == 1);
  retry.poll(9);
  assert(retryHooks.reconnects == 1);
  retry.poll(10);
  assert(retryHooks.reconnects == 2 && retry.state().stage() == ReconnectStage::Fault);

  FakeWakeHooks invalidWakeHooks;
  WakeRecoveryConfig invalidWakeConfig;
  invalidWakeConfig.releaseDebounceMilliseconds = 0x80000000UL;
  WakeRecoveryRuntime invalidWake(invalidWakeHooks, invalidWakeConfig);
  assert(!invalidWake.configurationValid());
  assert(!invalidWake.beginAfterHardwareReady(WakeReason::TopButton));
  assert(invalidWake.state().stage() == ReconnectStage::Fault);

  FakeWakeHooks wakeHooks;
  wakeHooks.wifi = true;
  wakeHooks.sync = true;
  WakeRecoveryConfig wakeConfig;
  wakeConfig.retryIntervalMilliseconds = 1;
  WakeRecoveryRuntime wake(wakeHooks, wakeConfig);
  assert(wake.beginAfterHardwareReady(WakeReason::TopButton));
  wake.poll(0);
  wake.poll(1);
  wakeHooks.released = true;
  wake.poll(0xfffffff0U);
  wakeHooks.released = false;
  wake.poll(10);
  wakeHooks.released = true;
  wake.poll(20);
  wake.poll(69);
  assert(!wake.state().readyForUserInput());
  wake.poll(70);
  assert(wake.state().stage() == ReconnectStage::ArmInput);
  assert(wake.poll(71));
  assert(wakeHooks.rearms == 1);

  std::cout << "papercolor attested display-power checks passed\n";
  return 0;
}
`;

  await writeFile(harnessPath, harness);
  const compiler = process.env.CXX || "c++";
  const sources = [
    harnessPath,
    new URL("ImageProcessing.cpp", moduleSource).pathname,
    new URL("PngAttestation.cpp", moduleSource).pathname,
    new URL("RefreshControl.cpp", moduleSource).pathname,
    new URL("PowerPolicy.cpp", moduleSource).pathname,
    new URL("DisplayPowerRuntime.cpp", moduleSource).pathname,
  ];
  const common = [
    "-std=c++11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-I",
    moduleSource.pathname,
  ];
  const compile = spawnSync(compiler, [...common, ...sources, "-o", executablePath], {
    encoding: "utf8",
  });
  assert.equal(compile.status, 0, `${compile.stdout}\n${compile.stderr}`);
  const run = spawnSync(
    executablePath,
    [validPngPath, bottomDownPngPath, wrongSizePngPath, reservedChunkPngPath],
    {
      encoding: "utf8",
    },
  );
  assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
  assert.match(run.stdout, /papercolor attested display-power checks passed/);

  const sanitizedPath = `${executablePath}_sanitized`;
  const sanitized = spawnSync(
    compiler,
    [
      ...common,
      "-O1",
      "-g",
      "-fsanitize=address,undefined",
      "-fno-omit-frame-pointer",
      ...sources,
      "-o",
      sanitizedPath,
    ],
    { encoding: "utf8" },
  );
  assert.equal(sanitized.status, 0, `${sanitized.stdout}\n${sanitized.stderr}`);
  const sanitizedRun = spawnSync(
    sanitizedPath,
    [validPngPath, bottomDownPngPath, wrongSizePngPath, reservedChunkPngPath],
    {
      encoding: "utf8",
      env: {
        ...process.env,
        ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
        UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
      },
    },
  );
  assert.equal(sanitizedRun.status, 0, `${sanitizedRun.stdout}\n${sanitizedRun.stderr}`);
  await rm(temporaryDirectory, { recursive: true, force: true });
});

test("M5 adapter decodes exact PNG bytes, animates image RGB, and selects no-second-dither mode", async () => {
  const source = await readFile(
    new URL("../firmware/m5-papercolor/src/DisplayPowerAdapters.cpp", import.meta.url),
    "utf8",
  );
  const header = await readFile(
    new URL("../firmware/m5-papercolor/src/DisplayPowerAdapters.h", import.meta.url),
    "utf8",
  );
  const config = await readFile(
    new URL("../firmware/m5-papercolor/src/AppConfig.h", import.meta.url),
    "utf8",
  );
  assert.match(source, /png\.matchesExactBytes\(bytes, length\)/);
  assert.match(source, /canvas_\.setColorDepth\(24\)/);
  assert.match(source, /canvas_\.drawPng\(bytes, length, 0, 0\)/);
  assert.match(source, /renderExperimentalPalette/);
  assert.match(source, /setEpdMode\(epd_mode_t::epd_fastest\)/);
  assert.match(source, /`_dither_row_none`/);
  assert.match(source, /M5\.Display\.pushImage/);
  assert.match(source, /setEpdMode\(epd_mode_t::epd_quality\)/);
  assert.match(header, /std::atomic<const void\*> writer_/);
  assert.match(source, /xTaskCreate\([\s\S]*inkloop-image-led/);
  assert.match(source, /kLedAnimationTickMilliseconds = 50/);
  assert.match(source, /esp_sleep_enable_timer_wakeup/);
  assert.match(source, /esp_sleep_enable_ext1_wakeup\(gpioMask, ESP_EXT1_WAKEUP_ANY_LOW\)/);
  assert.match(source, /GPIO_NUM_1[\s\S]*GPIO_NUM_9[\s\S]*GPIO_NUM_10/);
  assert.match(header, /PaperColorPreSleepQuiescenceHooks/);
  assert.match(config, /bool experimentalRenderEnabled = false/);
  assert.match(config, /bool deepSleepEnabled = false/);
});
