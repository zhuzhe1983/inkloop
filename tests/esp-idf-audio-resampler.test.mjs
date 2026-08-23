import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const audio = join(repo, "firmware/inkloop-idf/components/inkloop_audio");

const harness = String.raw`
#include <array>
#include <cassert>
#include <cstdint>

#include "inkloop/streaming_stereo_resampler.hpp"

using inkloop::StereoPcm16Frame;
using inkloop::StreamingStereoResampler;

uint64_t render(uint32_t source_rate, uint32_t output_rate,
                uint8_t channels, uint32_t source_frames) {
  StreamingStereoResampler resampler;
  assert(resampler.begin(source_rate, output_rate, channels));
  uint64_t emitted = 0;
  for (uint32_t frame = 0; frame < source_frames; ++frame) {
    std::array<StereoPcm16Frame,
               StreamingStereoResampler::kMaximumOutputFramesPerInput> out{};
    const int16_t left = static_cast<int16_t>((frame % 200U) - 100);
    const int16_t right = channels == 1U ? left : static_cast<int16_t>(-left);
    const size_t count = resampler.push(left, right, 100U, out);
    assert(count <= out.size());
    for (size_t index = 0; index < count; ++index) {
      if (channels == 1U) assert(out[index].left == out[index].right);
    }
    emitted += count;
  }
  std::array<StereoPcm16Frame,
             StreamingStereoResampler::kMaximumOutputFramesPerInput> tail{};
  emitted += resampler.finish(tail);
  assert(resampler.finished());
  assert(resampler.finish(tail) == 0);
  assert(resampler.inputFrames() == source_frames);
  assert(resampler.outputFrames() == emitted);
  return emitted;
}

int main() {
  StreamingStereoResampler invalid;
  assert(!invalid.begin(7999, 44100, 1));
  assert(!invalid.begin(16000, 48001, 1));
  assert(!invalid.begin(16000, 44100, 3));

  // N real PCM frames represent one second. finish() holds the last sample for
  // its final source interval, so exact rational phase neither drops the tail
  // nor depends on packet boundaries.
  assert(render(16000, 44100, 1, 16000) == 44100);
  assert(render(48000, 44100, 2, 48000) == 44100);
  assert(render(8000, 48000, 1, 8000) == 48000);
  assert(render(44100, 44100, 2, 44100) == 44100);

  StreamingStereoResampler volume;
  assert(volume.begin(16000, 16000, 2));
  std::array<StereoPcm16Frame,
             StreamingStereoResampler::kMaximumOutputFramesPerInput> out{};
  assert(volume.push(10000, -10000, 50, out) == 0);
  assert(volume.push(10000, -10000, 50, out) == 1);
  assert(out[0].left == 5000 && out[0].right == -5000);

  StreamingStereoResampler single;
  assert(single.begin(16000, 16000, 1));
  assert(single.push(1234, 0, 100, out) == 0);
  assert(single.finish(out) == 1);
  assert(out[0].left == 1234 && out[0].right == 1234);
  assert(single.push(999, 0, 100, out) == 0);

  StreamingStereoResampler empty;
  assert(empty.begin(16000, 44100, 1));
  assert(empty.finish(out) == 0);
  assert(empty.finished());

  // Golden non-integer short stream: exact rational interpolation, signed
  // truncation and the held final interval are all observable here.
  StreamingStereoResampler ramp;
  assert(ramp.begin(16000, 44100, 1));
  assert(ramp.push(0, 0, 100, out) == 0);
  assert(ramp.push(16000, 0, 100, out) == 3);
  assert(out[0].left == 0 && out[1].left == 5804 &&
         out[2].left == 11609);
  assert(ramp.push(-16000, 0, 100, out) == 3);
  assert(out[0].left == 13170 && out[1].left == 1560 &&
         out[2].left == -10049);
  assert(ramp.finish(out) == 3);
  assert(out[0].left == -16000 && out[1].left == -16000 &&
         out[2].left == -16000);

  // A 6:1 downsample has five consecutive source intervals with no output.
  // The final held interval must still preserve the last source sample.
  StreamingStereoResampler downsample;
  assert(downsample.begin(48000, 8000, 1));
  assert(downsample.push(1000, 0, 100, out) == 0);
  assert(downsample.push(2000, 0, 100, out) == 1);
  assert(out[0].left == 1000);
  for (int16_t value : {3000, 4000, 5000, 6000, 7000})
    assert(downsample.push(value, 0, 100, out) == 0);
  assert(downsample.finish(out) == 1);
  assert(out[0].left == 7000 && out[0].right == 7000);

  StreamingStereoResampler stereo;
  assert(stereo.begin(16000, 16000, 2));
  assert(stereo.push(100, -100, 100, out) == 0);
  assert(stereo.push(200, -300, 100, out) == 1);
  assert(out[0].left == 100 && out[0].right == -100);
  assert(stereo.finish(out) == 1);
  assert(out[0].left == 200 && out[0].right == -300);
  assert(stereo.begin(16000, 16000, 1));
  assert(!stereo.finished() && stereo.inputFrames() == 0);
  assert(stereo.push(32767, 0, 50, out) == 0);
  assert(stereo.finish(out) == 1);
  assert(out[0].left == 16383 && out[0].right == 16383);

  StreamingStereoResampler negative;
  assert(negative.begin(16000, 16000, 1));
  assert(negative.push(-32768, 0, 100, out) == 0);
  assert(negative.finish(out) == 1);
  assert(out[0].left == -32768 && out[0].right == -32768);
  volume.reset();
  assert(!volume.valid());
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-resampler-"));
  try {
    const source = join(scratch, "resampler.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(audio, "include"), source,
      join(audio, "streaming_stereo_resampler.cpp"), "-o", binary,
    ];
    if (sanitized) {
      args.splice(1, 0, "-fsanitize=address,undefined",
                  "-fno-omit-frame-pointer");
    }
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

test("PaperColor fixed-rate streaming resampler passes strict C++17", () => {
  buildAndRun(false);
});

test("PaperColor fixed-rate streaming resampler passes ASan/UBSan", () => {
  buildAndRun(true);
});
