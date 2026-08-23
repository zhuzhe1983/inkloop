import assert from "node:assert/strict";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const moduleSource = new URL(
  "../firmware/m5-papercolor/lib/InkloopDisplayPower/",
  import.meta.url,
);

test("PaperColor display and power policies are deterministic under C++11", async () => {
  const temporaryDirectory = await mkdtemp(join(tmpdir(), "inkloop-display-power-"));
  const harnessPath = join(temporaryDirectory, "display_power_test.cpp");
  const executablePath = join(temporaryDirectory, "display_power_test");
  const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "ImageProcessing.h"
#include "PowerPolicy.h"
#include "RefreshControl.h"

using namespace inkloop::displaypower;

bool same(const RgbPixel& left, const RgbPixel& right) {
  return left.red == right.red && left.green == right.green && left.blue == right.blue;
}

class VectorSource final : public IPixelSource {
 public:
  VectorSource(uint16_t width, uint16_t height, const std::vector<RgbPixel>& pixels)
      : width_(width), height_(height), pixels_(pixels), index_(0) {}

  uint16_t width() const override { return width_; }
  uint16_t height() const override { return height_; }
  bool read(RgbPixel* pixel) override {
    if (!pixel || index_ >= pixels_.size()) return false;
    *pixel = pixels_[index_++];
    return true;
  }
  size_t reads() const { return index_; }

 private:
  uint16_t width_;
  uint16_t height_;
  const std::vector<RgbPixel>& pixels_;
  size_t index_;
};

class VectorSink final : public IPixelSink {
 public:
  std::vector<RgbPixel> pixels;
  bool finished = false;
  bool reject = false;
  bool finishReject = false;

  bool write(const RgbPixel& pixel) override {
    if (reject) return false;
    pixels.push_back(pixel);
    return true;
  }
  bool finish() override {
    finished = true;
    return !finishReject;
  }
};

uint64_t hashPixels(const std::vector<RgbPixel>& pixels) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t index = 0; index < pixels.size(); ++index) {
    hash ^= pixels[index].red; hash *= 1099511628211ULL;
    hash ^= pixels[index].green; hash *= 1099511628211ULL;
    hash ^= pixels[index].blue; hash *= 1099511628211ULL;
  }
  return hash;
}

PowerInputs idleInputs() {
  PowerInputs inputs;
  inputs.nowMilliseconds = 120000;
  inputs.lastMeaningfulActivityMilliseconds = 0;
  inputs.rtcNowEpochSeconds = 1000;
  inputs.nextHeartbeatEpochSeconds = 1300;
  inputs.rtcSynchronized = true;
  inputs.wakeButtonsReleased = true;
  return inputs;
}

int main() {
  ImageMetadata metadata;
  metadata.format = ImageFormat::Png;
  metadata.width = 400;
  metadata.height = 600;
  metadata.encodedBytes = 123456;
  metadata.orientationNormalized = true;
  ImageValidation validation = validateImageMetadata(metadata);
  assert(validation.valid);
  assert(validation.error == ImageValidationError::None);
  assert(validation.decodedRgbBytes == 720000);
  metadata.orientationNormalized = false;
  assert(validateImageMetadata(metadata).error == ImageValidationError::OrientationNotNormalized);
  metadata.orientationNormalized = true;
  metadata.format = ImageFormat::Unknown;
  assert(validateImageMetadata(metadata).error == ImageValidationError::UnsupportedFormat);
  metadata.format = ImageFormat::Jpeg;
  metadata.encodedBytes = 200;
  assert(validateImageMetadata(metadata, 199).error == ImageValidationError::EncodedSizeOutOfRange);
  metadata.encodedBytes = 100;
  metadata.width = 0;
  assert(validateImageMetadata(metadata).error == ImageValidationError::InvalidDimensions);
  metadata.width = 8192;
  metadata.height = 8192;
  metadata.encodedBytes = 16ULL * 1024ULL * 1024ULL;
  assert(validateImageMetadata(metadata).valid);
  metadata.format = ImageFormat::Bmp;
  assert(validateImageMetadata(metadata).valid);
  metadata.format = ImageFormat::Png;
  assert(validateImageMetadata(metadata).valid);
  metadata.format = static_cast<ImageFormat>(255);
  assert(!validImageFormat(metadata.format));
  assert(validateImageMetadata(metadata).error == ImageValidationError::UnsupportedFormat);

  GeometryPlan exact = planPaperColorGeometry(400, 600, FitMode::CoverCrop);
  assert(exact.valid);
  assert(exact.source.x == 0 && exact.source.y == 0);
  assert(exact.source.width == 400 && exact.source.height == 600);
  assert(exact.destination.width == 400 && exact.destination.height == 600);

  GeometryPlan wideCover = planPaperColorGeometry(800, 600, FitMode::CoverCrop);
  assert(wideCover.valid);
  assert(wideCover.source.x == 200 && wideCover.source.y == 0);
  assert(wideCover.source.width == 400 && wideCover.source.height == 600);
  GeometryPlan wideContain = planPaperColorGeometry(800, 600, FitMode::ContainLetterbox);
  assert(wideContain.valid);
  assert(wideContain.destination.x == 0 && wideContain.destination.y == 150);
  assert(wideContain.destination.width == 400 && wideContain.destination.height == 300);
  assert(wideContain.clearTargetToWhite);

  GeometryPlan tallCover = planPaperColorGeometry(400, 1200, FitMode::CoverCrop);
  assert(tallCover.valid);
  assert(tallCover.source.x == 0 && tallCover.source.y == 300);
  assert(tallCover.source.width == 400 && tallCover.source.height == 600);
  GeometryPlan tallContain = planPaperColorGeometry(400, 1200, FitMode::ContainLetterbox);
  assert(tallContain.valid);
  assert(tallContain.destination.x == 100 && tallContain.destination.y == 0);
  assert(tallContain.destination.width == 200 && tallContain.destination.height == 600);
  assert(tallContain.clearTargetToWhite);
  assert(!planPaperColorGeometry(0, 600, FitMode::CoverCrop).valid);
  assert(!validFitMode(static_cast<FitMode>(255)));
  GeometryPlan invalidFit = planPaperColorGeometry(400, 600, static_cast<FitMode>(255));
  assert(!invalidFit.valid);
  assert(!planPaperColorGeometry(8192, 1, FitMode::CoverCrop).valid);

  const RenderPolicyDescriptor official = renderPolicy(RenderStrategy::OfficialQuality);
  const RenderPolicyDescriptor experimental = renderPolicy(RenderStrategy::ExperimentalSixColor);
  const RenderPolicyDescriptor reflectance = renderPolicy(RenderStrategy::ReflectancePhoto);
  const RenderPolicyDescriptor solid = renderPolicy(RenderStrategy::SolidClean);
  assert(official.valid);
  assert(std::string(official.id) == "papercolor-m5gfx-quality-v1");
  assert(!official.experimental && official.preprocessesToSixColors);
  assert(official.fullScreenRefreshRequired && !official.partialRefreshSupported);
  assert(std::string(experimental.id) == "papercolor-sixcolor-rgb-fs-v1");
  assert(experimental.experimental && experimental.preprocessesToSixColors);
  assert(experimental.fullScreenRefreshRequired && !experimental.partialRefreshSupported);
  assert(std::string(reflectance.id) == "papercolor-reflectance-yn-stucki-v1");
  assert(reflectance.experimental && reflectance.preprocessesToSixColors);
  assert(std::string(solid.id) == "papercolor-solid-clean-v1");
  assert(!solid.experimental && solid.preprocessesToSixColors);
  assert(std::string(renderStrategyId(RenderStrategy::OfficialQuality)) == "official-quality");
  assert(std::string(renderStrategyId(RenderStrategy::ExperimentalSixColor)) == "classic-six-color");
  assert(std::string(renderStrategyId(RenderStrategy::ReflectancePhoto)) == "reflectance-photo");
  assert(std::string(renderStrategyId(RenderStrategy::SolidClean)) == "solid-clean");
  RenderStrategy parsed = RenderStrategy::OfficialQuality;
  assert(parseRenderStrategyId("reflectance-photo", &parsed));
  assert(parsed == RenderStrategy::ReflectancePhoto);
  assert(parseRenderStrategyId("inkloop-text", &parsed));
  assert(parsed == RenderStrategy::SolidClean);
  assert(!parseRenderStrategyId("unknown", &parsed));
  const RenderStrategy invalidStrategy = static_cast<RenderStrategy>(255);
  assert(!validRenderStrategy(invalidStrategy));
  const RenderPolicyDescriptor invalidPolicy = renderPolicy(invalidStrategy);
  assert(!invalidPolicy.valid);
  assert(std::string(invalidPolicy.id) == "invalid-render-strategy");
  assert(!invalidPolicy.experimental && !invalidPolicy.preprocessesToSixColors);
  assert(!invalidPolicy.fullScreenRefreshRequired && !invalidPolicy.partialRefreshSupported);
  for (unsigned int raw = 0; raw <= 255; ++raw) {
    const ImageFormat format = static_cast<ImageFormat>(raw);
    assert(validImageFormat(format) == (raw <= 2));
    const FitMode fit = static_cast<FitMode>(raw);
    assert(validFitMode(fit) == (raw <= 1));
    assert(planPaperColorGeometry(400, 600, fit).valid == (raw <= 1));
    const RenderStrategy render = static_cast<RenderStrategy>(raw);
    assert(validRenderStrategy(render) == (raw <= 3));
    assert(renderPolicy(render).valid == (raw <= 3));
    const LedOutput led = imageLedOutput(static_cast<ImageLedState>(raw), 0, 0);
    assert(led.valid == (raw <= 7));
    assert(PowerPolicy::validPowerMode(static_cast<PowerMode>(raw)) == (raw <= 1));
    assert(validWakeReason(static_cast<WakeReason>(raw)) == (raw <= 6));
  }

  const std::vector<RgbPixel>& palette = paperColorPalette();
  assert(palette.size() == 6);
  assert(same(palette[0], RgbPixel(0, 0, 0)));
  assert(same(palette[1], RgbPixel(255, 255, 255)));
  assert(same(palette[2], RgbPixel(255, 243, 56)));
  assert(same(palette[3], RgbPixel(191, 0, 0)));
  assert(same(palette[4], RgbPixel(100, 64, 255)));
  assert(same(palette[5], RgbPixel(67, 138, 28)));
  for (size_t index = 0; index < palette.size(); ++index) {
    assert(isPaperColorPalettePixel(palette[index]));
    assert(same(nearestPaperColorColor(palette[index]), palette[index]));
  }
  assert(!isPaperColorPalettePixel(RgbPixel(1, 2, 3)));

  const size_t targetPixels = static_cast<size_t>(400) * 600;
  std::vector<RgbPixel> sourcePixels;
  sourcePixels.reserve(targetPixels);
  for (size_t index = 0; index < targetPixels; ++index) {
    sourcePixels.push_back(RgbPixel(
        static_cast<uint8_t>(index % 256),
        static_cast<uint8_t>((index / 7) % 256),
        static_cast<uint8_t>((index / 31) % 256)));
  }

  std::string error;
  VectorSource officialSource(400, 600, sourcePixels);
  VectorSink officialSink;
  assert(streamRenderPixels(
      officialSource, officialSink, RenderStrategy::OfficialQuality, &error));
  assert(officialSink.finished);
  assert(officialSink.pixels.size() == sourcePixels.size());
  assert(hashPixels(officialSink.pixels) != hashPixels(sourcePixels));
  assert(hashPixels(officialSink.pixels) == 11650483720347881026ULL);
  for (size_t index = 0; index < officialSink.pixels.size(); ++index) {
    assert(isPaperColorPalettePixel(officialSink.pixels[index]));
  }
  VectorSource secondOfficialSource(400, 600, sourcePixels);
  VectorSink secondOfficialSink;
  assert(streamRenderPixels(
      secondOfficialSource, secondOfficialSink, RenderStrategy::OfficialQuality, &error));
  assert(hashPixels(officialSink.pixels) == hashPixels(secondOfficialSink.pixels));

  VectorSource firstDitherSource(400, 600, sourcePixels);
  VectorSink firstDitherSink;
  assert(streamRenderPixels(
      firstDitherSource, firstDitherSink, RenderStrategy::ExperimentalSixColor, &error));
  VectorSource secondDitherSource(400, 600, sourcePixels);
  VectorSink secondDitherSink;
  assert(streamRenderPixels(
      secondDitherSource, secondDitherSink, RenderStrategy::ExperimentalSixColor, &error));
  assert(firstDitherSink.pixels.size() == targetPixels);
  assert(hashPixels(firstDitherSink.pixels) == hashPixels(secondDitherSink.pixels));
  assert(hashPixels(firstDitherSink.pixels) != hashPixels(sourcePixels));
  for (size_t index = 0; index < firstDitherSink.pixels.size(); ++index) {
    assert(isPaperColorPalettePixel(firstDitherSink.pixels[index]));
  }

  VectorSource reflectanceSource(400, 600, sourcePixels);
  VectorSink reflectanceSink;
  assert(streamRenderPixels(
      reflectanceSource, reflectanceSink, RenderStrategy::ReflectancePhoto, &error));
  assert(reflectanceSink.finished && reflectanceSink.pixels.size() == targetPixels);
  assert(hashPixels(reflectanceSink.pixels) != hashPixels(sourcePixels));
  VectorSource solidSource(400, 600, sourcePixels);
  VectorSink solidSink;
  assert(streamRenderPixels(
      solidSource, solidSink, RenderStrategy::SolidClean, &error));
  assert(solidSink.finished && solidSink.pixels.size() == targetPixels);
  assert(hashPixels(solidSink.pixels) != hashPixels(reflectanceSink.pixels));
  for (size_t index = 0; index < targetPixels; ++index) {
    assert(isPaperColorPalettePixel(reflectanceSink.pixels[index]));
    assert(isPaperColorPalettePixel(solidSink.pixels[index]));
  }

  VectorSource invalidRenderSource(400, 600, sourcePixels);
  VectorSink invalidRenderSink;
  assert(!streamRenderPixels(
      invalidRenderSource, invalidRenderSink, invalidStrategy, &error));
  assert(error == "invalid_render_strategy");
  assert(invalidRenderSource.reads() == 0);
  assert(invalidRenderSink.pixels.empty() && !invalidRenderSink.finished);

  VectorSource wrongSize(600, 400, sourcePixels);
  VectorSink wrongSizeSink;
  assert(!streamRenderPixels(
      wrongSize, wrongSizeSink, RenderStrategy::OfficialQuality, &error));
  assert(error == "pixel_source_must_be_400x600");
  std::vector<RgbPixel> shortPixels(targetPixels - 1, RgbPixel());
  VectorSource shortSource(400, 600, shortPixels);
  VectorSink shortSink;
  assert(!streamRenderPixels(
      shortSource, shortSink, RenderStrategy::OfficialQuality, &error));
  assert(error == "pixel_source_ended_early");
  std::vector<RgbPixel> extraPixels(targetPixels + 1, RgbPixel());
  VectorSource extraSource(400, 600, extraPixels);
  VectorSink extraSink;
  assert(!streamRenderPixels(
      extraSource, extraSink, RenderStrategy::OfficialQuality, &error));
  assert(error == "pixel_source_has_extra_data");
  assert(!extraSink.finished);
  VectorSource rejectingSource(400, 600, sourcePixels);
  VectorSink rejectingSink;
  rejectingSink.reject = true;
  assert(!streamRenderPixels(
      rejectingSource, rejectingSink, RenderStrategy::OfficialQuality, &error));
  assert(error == "pixel_sink_rejected_data");
  assert(!rejectingSink.finished);
  VectorSource finishRejectSource(400, 600, sourcePixels);
  VectorSink finishRejectSink;
  finishRejectSink.finishReject = true;
  assert(!streamRenderPixels(
      finishRejectSource, finishRejectSink, RenderStrategy::OfficialQuality, &error));
  assert(error == "pixel_sink_finish_failed");
  assert(finishRejectSink.finished);

  assert(imageLedOutput(ImageLedState::Off, 0, 0).valid);
  assert(!imageLedOutput(ImageLedState::Off, 0, 0).illuminated);
  LedOutput generating = imageLedOutput(ImageLedState::Generating, 1000, 0);
  assert(generating.illuminated && generating.color.green > 0 &&
         generating.color.green > generating.color.red &&
         generating.color.green > generating.color.blue);
  LedOutput downloading = imageLedOutput(ImageLedState::Downloading, 300, 0);
  assert(downloading.illuminated && downloading.color.blue > downloading.color.red);
  LedOutput caching = imageLedOutput(ImageLedState::Caching, 400, 0);
  assert(caching.illuminated && caching.color.red > 0 && caching.color.green > 0);
  LedOutput converting = imageLedOutput(ImageLedState::Converting, 500, 0);
  assert(converting.illuminated && converting.color.green > converting.color.red);
  assert(!same(converting.color, caching.color));
  assert(imageLedOutput(ImageLedState::Writing, 0, 0).illuminated);
  assert(!imageLedOutput(ImageLedState::Writing, 250, 0).illuminated);
  assert(imageLedOutput(ImageLedState::Complete, 1999, 0).illuminated);
  assert(!imageLedOutput(ImageLedState::Complete, 2000, 0).illuminated);
  assert(imageLedOutput(ImageLedState::Error, 0, 0).illuminated);
  assert(!imageLedOutput(ImageLedState::Error, 200, 0).illuminated);
  assert(imageLedOutput(ImageLedState::Error, 400, 0).illuminated);
  assert(!imageLedOutput(ImageLedState::Error, 1200, 0).illuminated);
  assert(imageLedOutput(ImageLedState::Writing, 0x20U, 0xfffffff0U).illuminated);
  LedOutput invalidLed = imageLedOutput(static_cast<ImageLedState>(255), 0, 0);
  assert(!invalidLed.valid && !invalidLed.illuminated);
  const ImageLedState ledStates[] = {
      ImageLedState::Generating, ImageLedState::Downloading, ImageLedState::Caching,
      ImageLedState::Converting,
      ImageLedState::Writing, ImageLedState::Complete, ImageLedState::Error};
  for (size_t index = 0; index < sizeof(ledStates) / sizeof(ledStates[0]); ++index) {
    const LedOutput normal = imageLedOutput(ledStates[index], 37U, 0U);
    const LedOutput wrapped = imageLedOutput(ledStates[index], 21U, 0xfffffff0U);
    assert(normal.valid && wrapped.valid);
    assert(normal.illuminated == wrapped.illuminated);
    assert(same(normal.color, wrapped.color));
  }

  assert(elapsedAtLeast32(0x20U, 0xfffffff0U, 48U));
  assert(!elapsedAtLeast32(0x20U, 0xfffffff0U, 49U));
  assert(deadlineReached32(0x10U, 0xfffffff0U));
  assert(!deadlineReached32(0xfffffff0U, 0x10U));

  static_assert(!std::is_default_constructible<RefreshTicket>::value,
      "callers must not forge a default refresh ticket");
  static_assert(std::is_copy_constructible<RefreshTicket>::value,
      "a writer may retain an opaque capability copy");
  static_assert(!std::is_copy_assignable<RefreshTicket>::value,
      "opaque ticket capabilities must not be mutable by assignment");

  RefreshArbiter arbiter(1000);
  RefreshRequest invalidRequest;
  assert(arbiter.acquire(invalidRequest, 0).result() == RefreshAcquireResult::InvalidRequest);
  RefreshRequest request;
  request.assetId = "sha256:one";
  request.strategy = RenderStrategy::ExperimentalSixColor;
  RefreshRequest invalidStrategyRequest = request;
  invalidStrategyRequest.strategy = static_cast<RenderStrategy>(255);
  assert(arbiter.acquire(invalidStrategyRequest, 0).result() ==
      RefreshAcquireResult::InvalidRequest);
  assert(!arbiter.busy());
  RefreshRequest unsafeAssetRequest = request;
  unsafeAssetRequest.assetId = "../../tasks.json";
  assert(arbiter.acquire(unsafeAssetRequest, 0).result() ==
      RefreshAcquireResult::InvalidRequest);
  RefreshAcquire first = arbiter.acquire(request, 100);
  assert(first.result() == RefreshAcquireResult::Accepted && first.accepted());
  assert(first.ticket() != 0);
  assert(first.ticket()->fullScreenRefreshRequired());
  assert(first.ticket()->strategy() == RenderStrategy::ExperimentalSixColor);
  RefreshTicket firstTicket = *first.ticket();
  RefreshTicket copiedFirstTicket = firstTicket;
  assert(arbiter.acquire(request, 101).result() == RefreshAcquireResult::Busy);

  RefreshArbiter otherOwner(0);
  RefreshAcquire otherAcquire = otherOwner.acquire(request, 100);
  assert(otherAcquire.accepted());
  assert(arbiter.finish(*otherAcquire.ticket(), 150) == RefreshFinishResult::WrongTransaction);
  assert(arbiter.busy());
  assert(arbiter.finish(firstTicket, 200) == RefreshFinishResult::Finished);
  assert(!arbiter.busy());
  assert(arbiter.finish(copiedFirstTicket, 201) == RefreshFinishResult::NotBusy);
  assert(otherOwner.finish(*otherAcquire.ticket(), 200) == RefreshFinishResult::Finished);
  assert(arbiter.acquire(request, 1199).result() == RefreshAcquireResult::Cooldown);
  RefreshAcquire second = arbiter.acquire(request, 1200);
  assert(second.result() == RefreshAcquireResult::Accepted);
  assert(arbiter.finish(copiedFirstTicket, 1201) == RefreshFinishResult::WrongTransaction);
  assert(arbiter.busy());
  assert(arbiter.finish(*second.ticket(), 0xfffffff0U) == RefreshFinishResult::Finished);
  assert(arbiter.acquire(request, 0x000003d7U).result() == RefreshAcquireResult::Cooldown);
  RefreshAcquire afterCooldown = arbiter.acquire(request, 0x000003d8U);
  assert(afterCooldown.result() == RefreshAcquireResult::Accepted);
  assert(arbiter.finish(*afterCooldown.ticket(), 0x000003d9U) == RefreshFinishResult::Finished);

  RefreshArbiter generationWrapArbiter(0);
  RefreshAcquire beforeWrap = generationWrapArbiter.acquire(request, 1);
  RefreshTicket beforeWrapTicket = *beforeWrap.ticket();
  assert(generationWrapArbiter.finish(beforeWrapTicket, 2) == RefreshFinishResult::Finished);
  RefreshArbiterTestAccess::forceGeneration(
      &generationWrapArbiter, 7, std::numeric_limits<uint64_t>::max());
  RefreshAcquire afterGenerationWrap = generationWrapArbiter.acquire(request, 3);
  assert(afterGenerationWrap.accepted());
  assert(generationWrapArbiter.finish(beforeWrapTicket, 4) ==
      RefreshFinishResult::WrongTransaction);
  assert(generationWrapArbiter.busy());
  assert(generationWrapArbiter.finish(*afterGenerationWrap.ticket(), 5) ==
      RefreshFinishResult::Finished);

  RefreshArbiter zeroCooldown(0);
  assert(zeroCooldown.configurationValid());
  RefreshAcquire zeroFirst = zeroCooldown.acquire(request, 1);
  assert(zeroCooldown.finish(*zeroFirst.ticket(), 2) == RefreshFinishResult::Finished);
  assert(zeroCooldown.acquire(request, 2).result() == RefreshAcquireResult::Accepted);
  RefreshArbiter invalidCooldown(0x80000000UL);
  assert(!invalidCooldown.configurationValid());
  assert(invalidCooldown.acquire(request, 1).result() == RefreshAcquireResult::InvalidRequest);
  assert(!invalidCooldown.busy());

  PowerPolicy defaultPolicy;
  PowerInputs inputs = idleInputs();
  SleepDecision defaultDecision = defaultPolicy.evaluate(inputs);
  assert(!defaultDecision.shouldSleep);
  assert(defaultDecision.reason == SleepDecisionReason::AlwaysAwakeMode);

  PowerPolicyConfig badConfig;
  badConfig.mode = PowerMode::BatteryOptIn;
  badConfig.eligibleIdleMilliseconds = 119999;
  assert(!defaultPolicy.setConfig(badConfig));
  assert(defaultPolicy.config().mode == PowerMode::AlwaysAwake);
  badConfig = PowerPolicyConfig();
  badConfig.mode = static_cast<PowerMode>(255);
  assert(!PowerPolicy::validPowerMode(badConfig.mode));
  assert(!defaultPolicy.setConfig(badConfig));
  badConfig = PowerPolicyConfig(); badConfig.heartbeatWakeIntervalSeconds = 59;
  assert(!defaultPolicy.setConfig(badConfig));
  badConfig = PowerPolicyConfig(); badConfig.heartbeatWakeIntervalSeconds = 86401;
  assert(!defaultPolicy.setConfig(badConfig));
  badConfig = PowerPolicyConfig(); badConfig.rtcConnectionMarginSeconds = 300;
  badConfig.heartbeatWakeIntervalSeconds = 300;
  assert(!defaultPolicy.setConfig(badConfig));
  badConfig = PowerPolicyConfig(); badConfig.minimumUsefulSleepSeconds = 3601;
  assert(!defaultPolicy.setConfig(badConfig));
  badConfig = PowerPolicyConfig(); badConfig.eligibleIdleMilliseconds = 0x80000000UL;
  assert(!defaultPolicy.setConfig(badConfig));

  PowerPolicyConfig batteryConfig;
  batteryConfig.mode = PowerMode::BatteryOptIn;
  batteryConfig.eligibleIdleMilliseconds = 120000;
  batteryConfig.heartbeatWakeIntervalSeconds = 300;
  batteryConfig.rtcConnectionMarginSeconds = 30;
  batteryConfig.minimumUsefulSleepSeconds = 10;
  PowerPolicy batteryPolicy(batteryConfig);
  assert(batteryPolicy.config().mode == PowerMode::BatteryOptIn);

  inputs = idleInputs();
  inputs.rtcSynchronized = false;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::InvalidClock);
  inputs = idleInputs();
  inputs.nowMilliseconds = 119999;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::IdlePeriodNotReached);
  inputs.nowMilliseconds = 119984;
  inputs.lastMeaningfulActivityMilliseconds = 0xfffffff0U;
  SleepDecision wrappedIdle = batteryPolicy.evaluate(inputs);
  assert(wrappedIdle.shouldSleep);

  inputs = idleInputs();
  inputs.nextLocalTaskEpochSeconds = 1200;
  SleepDecision eligible = batteryPolicy.evaluate(inputs);
  assert(eligible.shouldSleep);
  assert(eligible.reason == SleepDecisionReason::Eligible);
  assert(eligible.wake.timerEnabled);
  assert(eligible.wake.timerWakeEpochSeconds == 1170);
  assert(eligible.wake.ext1AnyLow);
  assert(eligible.wake.ext1AnyLowMask == 0x602ULL);
  assert(eligible.wake.topButtonGpio == 1);
  assert(eligible.wake.previousButtonGpio == 10);
  assert(eligible.wake.nextButtonGpio == 9);

  inputs = idleInputs();
  inputs.nextHeartbeatEpochSeconds = 0;
  SleepDecision derivedHeartbeat = batteryPolicy.evaluate(inputs);
  assert(derivedHeartbeat.shouldSleep);
  assert(derivedHeartbeat.wake.timerWakeEpochSeconds == 1270);
  inputs = idleInputs();
  inputs.nextHeartbeatEpochSeconds = 1000000;
  SleepDecision boundedHeartbeat = batteryPolicy.evaluate(inputs);
  assert(boundedHeartbeat.shouldSleep);
  assert(boundedHeartbeat.wake.timerWakeEpochSeconds == 1270);
  inputs = idleInputs();
  inputs.nextLocalTaskEpochSeconds = 1300;
  assert(batteryPolicy.evaluate(inputs).wake.timerWakeEpochSeconds == 1270);
  inputs = idleInputs();
  inputs.rtcNowEpochSeconds = std::numeric_limits<uint64_t>::max() - 100;
  inputs.nextHeartbeatEpochSeconds = 0;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::InvalidClock);

  inputs = idleInputs();
  inputs.nextLocalTaskEpochSeconds = 1000;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::WakeDeadlineDue);
  inputs.nextLocalTaskEpochSeconds = 1020;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::WakeDeadlineDue);
  inputs.nextLocalTaskEpochSeconds = 1035;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::SleepWindowTooShort);

  inputs = idleInputs(); inputs.blockers.voiceActive = true;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::VoiceActive);
  inputs = idleInputs(); inputs.blockers.audioActive = true;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::AudioActive);
  inputs = idleInputs(); inputs.blockers.generationActive = true;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::GenerationActive);
  inputs = idleInputs(); inputs.blockers.conversionActive = true;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::ConversionActive);
  inputs = idleInputs(); inputs.blockers.writeActive = true;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::WriteActive);
  inputs = idleInputs(); inputs.blockers.taskFinalizationActive = true;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::TaskFinalizationActive);
  inputs = idleInputs(); inputs.blockers.displayActive = true;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::DisplayActive);
  inputs = idleInputs(); inputs.blockers.downloadActive = true;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::DownloadActive);
  inputs = idleInputs(); inputs.blockers.pendingJournal = true;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::PendingJournal);
  inputs = idleInputs(); inputs.blockers.portalActive = true;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::PortalActive);
  inputs = idleInputs(); inputs.blockers.unacknowledgedTask = true;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::UnacknowledgedTask);
  inputs = idleInputs(); inputs.wakeButtonsReleased = false;
  assert(batteryPolicy.evaluate(inputs).reason == SleepDecisionReason::WakeButtonsHeld);

  assert(PowerPolicy::paperColorExt1AnyLowMask() == 0x602ULL);
  assert(wakeReasonFromExt1Mask(1ULL << 1U) == WakeReason::TopButton);
  assert(wakeReasonFromExt1Mask(1ULL << 10U) == WakeReason::PreviousButton);
  assert(wakeReasonFromExt1Mask(1ULL << 9U) == WakeReason::NextButton);
  assert(wakeReasonFromExt1Mask((1ULL << 1U) | (1ULL << 9U)) ==
      WakeReason::MultipleButtons);
  assert(wakeReasonFromExt1Mask(1ULL << 8U) == WakeReason::Unknown);
  assert(wakeReasonFromExt1Mask((1ULL << 1U) | (1ULL << 8U)) == WakeReason::Unknown);
  assert(validWakeReason(WakeReason::Unknown));
  assert(!validWakeReason(static_cast<WakeReason>(255)));

  WakeReconnectState recovery;
  assert(recovery.stage() == ReconnectStage::NotStarted);
  assert(recovery.begin(WakeReason::RtcTimer));
  assert(!recovery.begin(WakeReason::TopButton));
  assert(recovery.wakeReason() == WakeReason::RtcTimer);
  assert(recovery.stage() == ReconnectStage::ReinitializeHardware);
  assert(!recovery.markWifiConnected());
  assert(recovery.markHardwareReady());
  assert(recovery.stage() == ReconnectStage::ReconnectWifi);
  assert(recovery.wifiReconnectRequired());
  assert(recovery.markWifiConnected());
  assert(recovery.stage() == ReconnectStage::SyncInkloop);
  assert(recovery.wifiReconnectRequired());
  assert(!recovery.readyForUserInput());
  assert(recovery.markInkloopSynced());
  assert(recovery.stage() == ReconnectStage::AwaitWakeButtonsReleased);
  assert(!recovery.readyForUserInput());
  assert(!recovery.markWakeButtonsReleasedDebounced(false, true, true));
  assert(!recovery.markWakeButtonsReleasedDebounced(true, false, true));
  assert(!recovery.markWakeButtonsReleasedDebounced(true, true, false));
  assert(recovery.stage() == ReconnectStage::AwaitWakeButtonsReleased);
  assert(recovery.markWakeButtonsReleasedDebounced(true, true, true));
  assert(recovery.stage() == ReconnectStage::ArmInput);
  assert(!recovery.readyForUserInput());
  assert(!recovery.markWakeButtonsReleasedDebounced(true, true, true));
  assert(recovery.markInputRearmed());
  assert(recovery.readyForUserInput());
  assert(recovery.begin(WakeReason::TopButton));
  recovery.markFault();
  assert(recovery.stage() == ReconnectStage::Fault);
  assert(!recovery.readyForUserInput());
  assert(!recovery.begin(WakeReason::RtcTimer));

  WakeReconnectState invalidWake;
  assert(!invalidWake.begin(static_cast<WakeReason>(255)));
  assert(invalidWake.stage() == ReconnectStage::Fault);
  assert(invalidWake.wakeReason() == WakeReason::Unknown);

  std::cout << "papercolor display-power checks passed\n";
  return 0;
}
`;

  await writeFile(harnessPath, harness);
  const compiler = process.env.CXX || "c++";
  const compile = spawnSync(
    compiler,
    [
      "-std=c++11",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-DINKLOOP_DISPLAYPOWER_TESTING=1",
      "-I",
      moduleSource.pathname,
      harnessPath,
      new URL("ImageProcessing.cpp", moduleSource).pathname,
      new URL("RefreshControl.cpp", moduleSource).pathname,
      new URL("PowerPolicy.cpp", moduleSource).pathname,
      "-o",
      executablePath,
    ],
    { encoding: "utf8" },
  );
  assert.equal(compile.status, 0, `${compile.stdout}\n${compile.stderr}`);

  const run = spawnSync(executablePath, [], { encoding: "utf8" });
  assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
  assert.match(run.stdout, /papercolor display-power checks passed/);

  const sanitizedExecutablePath = join(temporaryDirectory, "display_power_test_sanitized");
  const sanitizedCompile = spawnSync(
    compiler,
    [
      "-std=c++11",
      "-O1",
      "-g",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-DINKLOOP_DISPLAYPOWER_TESTING=1",
      "-fsanitize=address,undefined",
      "-fno-omit-frame-pointer",
      "-I",
      moduleSource.pathname,
      harnessPath,
      new URL("ImageProcessing.cpp", moduleSource).pathname,
      new URL("RefreshControl.cpp", moduleSource).pathname,
      new URL("PowerPolicy.cpp", moduleSource).pathname,
      "-o",
      sanitizedExecutablePath,
    ],
    { encoding: "utf8" },
  );
  assert.equal(
    sanitizedCompile.status,
    0,
    `${sanitizedCompile.stdout}\n${sanitizedCompile.stderr}`,
  );
  const sanitizedRun = spawnSync(sanitizedExecutablePath, [], {
    encoding: "utf8",
    env: {
      ...process.env,
      ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
      UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
    },
  });
  assert.equal(sanitizedRun.status, 0, `${sanitizedRun.stdout}\n${sanitizedRun.stderr}`);
  assert.match(sanitizedRun.stdout, /papercolor display-power checks passed/);

  await rm(temporaryDirectory, { recursive: true, force: true });
});
