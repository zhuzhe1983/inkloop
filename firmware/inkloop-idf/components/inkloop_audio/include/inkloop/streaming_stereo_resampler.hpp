#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace inkloop {

struct StereoPcm16Frame {
  int16_t left = 0;
  int16_t right = 0;
};
static_assert(sizeof(StereoPcm16Frame) == 2U * sizeof(int16_t),
              "stereo PCM frame must be tightly packed");

// Allocation-free streaming PCM16 converter used between the MyAI/local
// prompt rate and a board's fixed speaker clock.  The rational phase keeps
// chunk boundaries bit-for-bit deterministic; retaining the previous frame
// avoids restarting interpolation at every WebSocket packet or scheduler
// tick.
class StreamingStereoResampler final {
 public:
  static constexpr uint32_t kMinimumRateHz = 8000U;
  static constexpr uint32_t kMaximumRateHz = 48000U;
  static constexpr size_t kMaximumOutputFramesPerInput = 6U;

  bool begin(uint32_t source_rate_hz, uint32_t output_rate_hz,
             uint8_t source_channels);
  void reset();

  size_t push(int16_t left, int16_t right, uint8_t volume_percent,
              std::array<StereoPcm16Frame,
                         kMaximumOutputFramesPerInput>& output);
  // Close the final source-frame interval by holding the last sample. This is
  // required for one-frame/short streams and is idempotent; no push is
  // accepted after finish until begin() starts a new stream.
  size_t finish(std::array<StereoPcm16Frame,
                           kMaximumOutputFramesPerInput>& output);

  bool valid() const { return valid_; }
  uint32_t sourceRateHz() const { return source_rate_hz_; }
  uint32_t outputRateHz() const { return output_rate_hz_; }
  uint8_t sourceChannels() const { return source_channels_; }
  uint64_t inputFrames() const { return input_frames_; }
  uint64_t outputFrames() const { return output_frames_; }
  bool finished() const { return finished_; }

 private:
  static int16_t scale(int16_t sample, uint8_t volume_percent);
  int16_t interpolate(int16_t previous, int16_t current) const;

  uint32_t source_rate_hz_ = 0;
  uint32_t output_rate_hz_ = 0;
  // Position of the next output inside the previous->current source segment,
  // expressed with output_rate_hz_ as its exact denominator.
  uint32_t phase_numerator_ = 0;
  uint8_t source_channels_ = 0;
  int16_t previous_left_ = 0;
  int16_t previous_right_ = 0;
  uint64_t input_frames_ = 0;
  uint64_t output_frames_ = 0;
  bool have_previous_ = false;
  bool finished_ = false;
  bool valid_ = false;
};

}  // namespace inkloop
