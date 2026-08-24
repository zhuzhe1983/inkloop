import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const components = join(repo, "firmware/inkloop-idf/components");
const read = (path) => readFileSync(join(components, path), "utf8");
const voiceHeader = read(
  "inkloop_product/include/inkloop/native_voice_service.hpp",
);
const voice = read("inkloop_product/native_voice_service.cpp");
const owner = read("inkloop_product/native_portal_owner.cpp");
const portalHeader = read(
  "inkloop_portal/include/inkloop/portal/portal_core.hpp",
);
const portal = read("inkloop_portal/portal_core.cpp");
const runtime = read("inkloop_product/product_runtime.cpp");

function between(source, begin, end) {
  const first = source.indexOf(begin);
  const last = source.indexOf(end, first + begin.length);
  assert.ok(first >= 0 && last > first, `missing ${begin}`);
  return source.slice(first, last);
}

test("Voice owner publishes one coherent complete I2S/feed snapshot", () => {
  const native = between(
    voiceHeader,
    "struct NativeVoiceDiagnostics",
    "inline constexpr size_t kNativeLocalChatPageItems",
  );
  assert.match(native, /bool audio_available = false/);
  assert.match(native, /EspI2sAudioDiagnostics audio\{\}/);

  const publish = between(
    voice,
    "void NativeVoiceService::publishAudioDiagnostics(bool force)",
    "void NativeVoiceService::serviceVoice()",
  );
  assert.match(
    publish,
    /!force && audio_diagnostics_published_[\s\S]*kAudioDiagnosticsPublishMs/,
  );
  assert.match(publish, /audio_device_->diagnostics\(\)/);
  assert.match(
    publish,
    /portENTER_CRITICAL\(&diagnostics_mux_\)[\s\S]*diagnostics_\.audio_available = available[\s\S]*diagnostics_\.audio = audio[\s\S]*portEXIT_CRITICAL\(&diagnostics_mux_\)/,
  );
  assert.equal((voice.match(/audio_device_->diagnostics\(\)/g) ?? []).length, 1);
  const service = between(
    voice,
    "void NativeVoiceService::serviceVoice()",
    "bool NativeVoiceService::handleControlResult",
  );
  assert.match(service, /local_prompts_\.service[\s\S]*publishAudioDiagnostics\(/);
  assert.match(service, /servicePlayback[\s\S]*publishAudioDiagnostics\(\)/);
  assert.match(service, /captureStep[\s\S]*publishAudioDiagnostics\(\)/);

  const readSnapshot = between(
    voice,
    "NativeVoiceDiagnostics NativeVoiceService::diagnostics() const",
    "NativeMyAiOnboardingSnapshot NativeVoiceService::onboardingSnapshot",
  );
  assert.match(
    readSnapshot,
    /portENTER_CRITICAL\(&diagnostics_mux_\)[\s\S]*const NativeVoiceDiagnostics value = diagnostics_[\s\S]*portEXIT_CRITICAL\(&diagnostics_mux_\)/,
  );
});

test("Portal maps every I2S/feed counter without strings or credentials", () => {
  const schema = between(
    portalHeader,
    "struct PortalAudioDiagnostics",
    "struct PortalRenderStrategyCapability",
  );
  assert.doesNotMatch(schema, /std::string|char\s*\*|token|credential|url/i);
  const expectedPortalFields = [
    "available", "prepared", "capture_channel_enabled",
    "playback_channel_enabled", "prepare_attempts", "prepare_successes",
    "prepare_failures",
    "shutdowns", "shutdown_failures", "playback_clock_reconfigurations",
    "playback_clock_reconfiguration_failures", "shared_pin_selections",
    "shared_pin_selection_failures", "prepared_playback_rate_hz",
    "last_prepare_error", "capture_starts", "playback_starts", "capture_timeouts",
    "playback_timeouts", "capture_failures", "playback_failures",
    "playback_preload_starts", "forced_aborts", "captured_bytes",
    "played_source_bytes", "played_output_frames", "peak_preloaded_bytes",
    "playback_dma_callbacks", "playback_dma_underruns",
    "playback_dma_expected_drain_overflows", "feed_streams",
    "feed_submit_calls", "feed_late_submits", "feed_estimated_underruns",
    "feed_queue_clamps", "feed_max_submit_gap_us",
    "feed_minimum_queue_lead_us", "feed_maximum_queue_lead_us",
    "feed_preloaded_frames", "feed_submitted_frames", "feed_consumed_frames",
    "feed_estimated_underrun_frames", "feed_queue_overflow_frames",
    "feed_current_queue_frames", "feed_peak_queue_frames",
  ];
  for (const field of expectedPortalFields) {
    assert.match(schema, new RegExp(`\\b${field}\\b`), field);
  }

  const mapping = between(
    owner,
    "portal::PortalAudioDiagnostics portalAudioDiagnostics",
    "}  // namespace",
  );
  for (const source of [
    "prepared", "capture_channel_enabled", "playback_channel_enabled",
    "prepare_attempts", "prepare_successes", "prepare_failures", "shutdowns",
    "shutdown_failures", "playback_clock_reconfigurations",
    "playback_clock_reconfiguration_failures", "shared_pin_selections",
    "shared_pin_selection_failures", "prepared_playback_rate_hz",
    "last_prepare_error", "capture_starts", "playback_starts", "capture_timeouts",
    "playback_timeouts", "capture_failures", "playback_failures",
    "playback_preload_starts", "forced_aborts", "captured_bytes",
    "played_source_bytes", "played_output_frames", "peak_preloaded_bytes",
    "playback_dma_callbacks", "playback_dma_underruns",
    "playback_dma_expected_drain_overflows", "streams", "submit_calls",
    "late_submit_count", "estimated_underrun_count", "queue_clamp_count",
    "max_submit_gap_us", "minimum_queue_lead_us", "maximum_queue_lead_us",
    "preloaded_frames", "submitted_frames", "consumed_frames",
    "estimated_underrun_frames", "queue_overflow_frames",
    "current_queue_frames", "peak_queue_frames",
  ]) {
    assert.match(mapping, new RegExp(`\\b${source}\\b`), source);
  }
  assert.match(owner, /portalAudioDiagnostics\(voice_\.diagnostics\(\)\)/);
  for (const wireField of [
    "audioDiagnostics", "lifecycle", "prepared", "captureChannelEnabled",
    "playbackChannelEnabled", "prepareAttempts", "prepareSuccesses",
    "prepareFailures", "shutdownFailures", "preparedPlaybackRateHz",
    "lastPrepareError", "underruns", "expectedDrainOverflows",
    "lateSubmits", "estimatedUnderruns", "maxSubmitGapUs",
    "minimumQueueLeadUs", "maximumQueueLeadUs", "currentQueueFrames",
  ]) {
    assert.ok(portal.includes(`\\\"${wireField}\\\"`), wireField);
  }
});

test("serial status exposes stable bounded audio counters", () => {
  for (const kind of ["AudioDma", "AudioFeed", "AudioTiming", "AudioQueue"]) {
    assert.match(runtime, new RegExp(`SerialDiagnosticEventKind::${kind}`));
  }
  for (const source of [
    "playback_dma_underruns", "estimated_underrun_count",
    "late_submit_count", "max_submit_gap_us", "minimum_queue_lead_us",
    "maximum_queue_lead_us",
  ]) {
    assert.match(runtime, new RegExp(`\\b${source}\\b`), source);
  }
  assert.doesNotMatch(
    between(
      runtime,
      "case diagnostics::SerialCommand::Status:",
      "case diagnostics::SerialCommand::AlbumStatus:",
    ),
    /token|credential|password|cookie|transcript|prompt|response.body/i,
  );
});
