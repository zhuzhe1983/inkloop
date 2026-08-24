#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace inkloop {

// Portable producer/consumer timing model for the bounded I2S DMA queue.
// Hardware queue-overflow callbacks remain the authoritative underrun signal;
// this model explains the lead and cadence that preceded one without touching
// FreeRTOS, the filesystem or the network stack.
struct PlaybackFeedDiagnostics {
  uint32_t streams = 0;
  uint32_t submit_calls = 0;
  uint32_t late_submit_count = 0;
  uint32_t estimated_underrun_count = 0;
  uint32_t queue_clamp_count = 0;
  uint32_t max_submit_gap_us = 0;
  uint32_t minimum_queue_lead_us = 0;
  uint32_t maximum_queue_lead_us = 0;
  uint64_t preloaded_frames = 0;
  uint64_t submitted_frames = 0;
  uint64_t consumed_frames = 0;
  uint64_t estimated_underrun_frames = 0;
  uint64_t queue_overflow_frames = 0;
  size_t current_queue_frames = 0;
  size_t peak_queue_frames = 0;
};

class PlaybackFeedMonitor final {
 public:
  bool begin(uint32_t output_rate_hz, size_t queue_capacity_frames) {
    stop();
    if (output_rate_hz == 0U || queue_capacity_frames == 0U) return false;
    output_rate_hz_ = output_rate_hz;
    queue_capacity_frames_ = queue_capacity_frames;
    configured_ = true;
    // A source exists before DMA starts. A short prompt may close while all
    // of its PCM is still preloaded; start() must preserve that closed state
    // so the intentional drain is not reported as producer starvation.
    source_open_ = true;
    diagnostics_.streams = saturatingIncrement(diagnostics_.streams);
    return true;
  }

  bool preload(size_t frames) {
    if (!configured_ || running_ || frames == 0U) return false;
    diagnostics_.preloaded_frames =
        saturatingAdd(diagnostics_.preloaded_frames, frames);
    append(frames);
    return true;
  }

  bool start(uint64_t now_us) {
    if (!configured_ || running_ || queue_frames_ == 0U) return false;
    running_ = true;
    starving_ = false;
    last_update_us_ = now_us;
    last_submit_us_ = now_us;
    updateLead();
    return true;
  }

  bool submit(size_t frames, uint64_t now_us) {
    if (!configured_ || !running_ || !source_open_ || frames == 0U)
      return false;
    const size_t lead_before = queue_frames_;
    advance(now_us);
    const uint64_t gap = now_us >= last_submit_us_
        ? now_us - last_submit_us_
        : 0U;
    diagnostics_.max_submit_gap_us = maximum32(
        diagnostics_.max_submit_gap_us, saturating32(gap));
    if (framesForDuration(gap) > lead_before) {
      diagnostics_.late_submit_count =
          saturatingIncrement(diagnostics_.late_submit_count);
    }
    diagnostics_.submit_calls =
        saturatingIncrement(diagnostics_.submit_calls);
    diagnostics_.submitted_frames =
        saturatingAdd(diagnostics_.submitted_frames, frames);
    append(frames);
    starving_ = false;
    last_submit_us_ = now_us;
    updateLead();
    return true;
  }

  // A logical TTS segment has no more ingress, but the hardware queue and
  // resampler may remain alive briefly for an adjacent same-format segment.
  // Advance once while the segment is still open, then suspend starvation
  // classification until resumeSource(). Expected DMA drain during that
  // continuation window must not be reported as producer starvation.
  bool pauseSource(uint64_t now_us) {
    if (!configured_) return false;
    if (running_) advance(now_us);
    source_open_ = false;
    starving_ = false;
    return true;
  }

  // Reopen a paused logical segment without resetting the bounded queue or
  // resampler. Time spent paused is consumed with source_open_ false, and the
  // next submit cadence starts at this resume point rather than including the
  // intentional continuation gap.
  bool resumeSource(uint64_t now_us) {
    if (!configured_) return false;
    if (source_open_) return true;
    if (running_) advance(now_us);
    source_open_ = true;
    starving_ = false;
    last_submit_us_ = now_us;
    return true;
  }

  // The streaming resampler may emit a final held interval only when the
  // continuation grace expires. Feed that terminal tail into the queue while
  // keeping logical ingress closed; reopening here would make an expected
  // drain look like a new producer starvation window.
  bool submitTerminal(size_t frames, uint64_t now_us) {
    if (!configured_ || !running_ || source_open_ || frames == 0U)
      return false;
    advance(now_us);
    diagnostics_.submit_calls =
        saturatingIncrement(diagnostics_.submit_calls);
    diagnostics_.submitted_frames =
        saturatingAdd(diagnostics_.submitted_frames, frames);
    append(frames);
    starving_ = false;
    last_submit_us_ = now_us;
    return true;
  }

  void finishSource(uint64_t now_us) {
    (void)pauseSource(now_us);
  }

  void stop() {
    configured_ = false;
    running_ = false;
    source_open_ = false;
    starving_ = false;
    output_rate_hz_ = 0U;
    queue_capacity_frames_ = 0U;
    queue_frames_ = 0U;
    fractional_frame_numerator_ = 0U;
    last_update_us_ = 0U;
    last_submit_us_ = 0U;
    diagnostics_.current_queue_frames = 0U;
  }

  const PlaybackFeedDiagnostics& diagnostics() const {
    return diagnostics_;
  }

  bool sourceOpen() const { return configured_ && source_open_; }

 private:
  static uint32_t saturatingIncrement(uint32_t value) {
    return value == std::numeric_limits<uint32_t>::max() ? value : value + 1U;
  }

  static uint32_t saturating32(uint64_t value) {
    return value > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(value);
  }

  static uint32_t maximum32(uint32_t left, uint32_t right) {
    return left > right ? left : right;
  }

  static uint64_t saturatingAdd(uint64_t left, uint64_t right) {
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max()
        : left + right;
  }

  uint64_t framesForDuration(uint64_t duration_us) const {
    if (output_rate_hz_ == 0U) return 0U;
    if (duration_us >
        (std::numeric_limits<uint64_t>::max() - 999999U) /
            output_rate_hz_) {
      return std::numeric_limits<uint64_t>::max();
    }
    return (duration_us * output_rate_hz_ + 999999U) / 1000000U;
  }

  uint32_t leadUs(size_t frames) const {
    if (output_rate_hz_ == 0U) return 0U;
    if (frames > std::numeric_limits<uint64_t>::max() / 1000000ULL)
      return std::numeric_limits<uint32_t>::max();
    return saturating32(
        (static_cast<uint64_t>(frames) * 1000000ULL) / output_rate_hz_);
  }

  void updateLead() {
    diagnostics_.current_queue_frames = queue_frames_;
    if (queue_frames_ > diagnostics_.peak_queue_frames)
      diagnostics_.peak_queue_frames = queue_frames_;
    if (!running_) return;
    const uint32_t lead = leadUs(queue_frames_);
    if (!lead_observed_ ||
        lead < diagnostics_.minimum_queue_lead_us) {
      diagnostics_.minimum_queue_lead_us = lead;
    }
    lead_observed_ = true;
    if (lead > diagnostics_.maximum_queue_lead_us)
      diagnostics_.maximum_queue_lead_us = lead;
  }

  void append(size_t frames) {
    const size_t available = queue_capacity_frames_ - queue_frames_;
    if (frames > available) {
      diagnostics_.queue_clamp_count =
          saturatingIncrement(diagnostics_.queue_clamp_count);
      diagnostics_.queue_overflow_frames = saturatingAdd(
          diagnostics_.queue_overflow_frames,
          static_cast<uint64_t>(frames - available));
      queue_frames_ = queue_capacity_frames_;
    } else {
      queue_frames_ += frames;
    }
    updateLead();
  }

  void advance(uint64_t now_us) {
    if (!running_ || now_us <= last_update_us_) return;
    const uint64_t elapsed = now_us - last_update_us_;
    uint64_t numerator = fractional_frame_numerator_;
    if (elapsed >
        (std::numeric_limits<uint64_t>::max() - numerator) /
            output_rate_hz_) {
      numerator = std::numeric_limits<uint64_t>::max();
    } else {
      numerator += elapsed * output_rate_hz_;
    }
    const uint64_t consumed = numerator / 1000000U;
    fractional_frame_numerator_ = numerator % 1000000U;
    diagnostics_.consumed_frames =
        saturatingAdd(diagnostics_.consumed_frames, consumed);
    if (consumed > queue_frames_) {
      const uint64_t deficit = consumed - queue_frames_;
      queue_frames_ = 0U;
      if (source_open_) {
        if (!starving_) {
          diagnostics_.estimated_underrun_count =
              saturatingIncrement(diagnostics_.estimated_underrun_count);
        }
        diagnostics_.estimated_underrun_frames = saturatingAdd(
            diagnostics_.estimated_underrun_frames, deficit);
        starving_ = true;
      }
    } else {
      queue_frames_ -= static_cast<size_t>(consumed);
    }
    last_update_us_ = now_us;
    updateLead();
  }

  PlaybackFeedDiagnostics diagnostics_{};
  uint32_t output_rate_hz_ = 0U;
  size_t queue_capacity_frames_ = 0U;
  size_t queue_frames_ = 0U;
  uint64_t fractional_frame_numerator_ = 0U;
  uint64_t last_update_us_ = 0U;
  uint64_t last_submit_us_ = 0U;
  bool configured_ = false;
  bool running_ = false;
  bool source_open_ = false;
  bool starving_ = false;
  bool lead_observed_ = false;
};

}  // namespace inkloop
