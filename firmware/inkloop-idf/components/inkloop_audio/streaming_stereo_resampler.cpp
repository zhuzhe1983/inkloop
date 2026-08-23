#include "inkloop/streaming_stereo_resampler.hpp"

#include <algorithm>

namespace inkloop {

bool StreamingStereoResampler::begin(uint32_t source_rate_hz,
                                     uint32_t output_rate_hz,
                                     uint8_t source_channels) {
  reset();
  if (source_rate_hz < kMinimumRateHz ||
      source_rate_hz > kMaximumRateHz ||
      output_rate_hz < kMinimumRateHz ||
      output_rate_hz > kMaximumRateHz ||
      (source_channels != 1U && source_channels != 2U)) {
    return false;
  }
  source_rate_hz_ = source_rate_hz;
  output_rate_hz_ = output_rate_hz;
  source_channels_ = source_channels;
  valid_ = true;
  return true;
}

void StreamingStereoResampler::reset() {
  source_rate_hz_ = 0;
  output_rate_hz_ = 0;
  phase_numerator_ = 0;
  source_channels_ = 0;
  previous_left_ = 0;
  previous_right_ = 0;
  input_frames_ = 0;
  output_frames_ = 0;
  have_previous_ = false;
  finished_ = false;
  valid_ = false;
}

int16_t StreamingStereoResampler::scale(int16_t sample,
                                        uint8_t volume_percent) {
  const uint8_t bounded = std::min<uint8_t>(volume_percent, 100U);
  return static_cast<int16_t>(
      static_cast<int32_t>(sample) * bounded / 100);
}

int16_t StreamingStereoResampler::interpolate(int16_t previous,
                                              int16_t current) const {
  const int64_t before = static_cast<int64_t>(previous) *
      (output_rate_hz_ - phase_numerator_);
  const int64_t after =
      static_cast<int64_t>(current) * phase_numerator_;
  return static_cast<int16_t>((before + after) /
                              static_cast<int64_t>(output_rate_hz_));
}

size_t StreamingStereoResampler::push(
    int16_t left, int16_t right, uint8_t volume_percent,
    std::array<StereoPcm16Frame,
               kMaximumOutputFramesPerInput>& output) {
  output = {};
  if (!valid_ || finished_) return 0;
  const int16_t scaled_left = scale(left, volume_percent);
  const int16_t scaled_right = scale(
      source_channels_ == 1U ? left : right, volume_percent);
  ++input_frames_;
  if (!have_previous_) {
    previous_left_ = scaled_left;
    previous_right_ = scaled_right;
    have_previous_ = true;
    return 0;
  }

  size_t count = 0;
  while (phase_numerator_ < output_rate_hz_) {
    if (count >= output.size()) {
      // The validated 8..48 kHz range bounds output/source at six. Reaching
      // this branch would indicate arithmetic/state corruption, so invalidate
      // the stream instead of writing beyond the caller's fixed array.
      reset();
      return 0;
    }
    output[count].left = interpolate(previous_left_, scaled_left);
    output[count].right = interpolate(previous_right_, scaled_right);
    ++count;
    phase_numerator_ += source_rate_hz_;
  }
  phase_numerator_ -= output_rate_hz_;
  previous_left_ = scaled_left;
  previous_right_ = scaled_right;
  output_frames_ += count;
  return count;
}

size_t StreamingStereoResampler::finish(
    std::array<StereoPcm16Frame,
               kMaximumOutputFramesPerInput>& output) {
  output = {};
  if (!valid_ || finished_) return 0;
  finished_ = true;
  if (!have_previous_) return 0;

  size_t count = 0;
  while (phase_numerator_ < output_rate_hz_) {
    if (count >= output.size()) {
      reset();
      return 0;
    }
    output[count].left = previous_left_;
    output[count].right = previous_right_;
    ++count;
    phase_numerator_ += source_rate_hz_;
  }
  phase_numerator_ -= output_rate_hz_;
  output_frames_ += count;
  return count;
}

}  // namespace inkloop
