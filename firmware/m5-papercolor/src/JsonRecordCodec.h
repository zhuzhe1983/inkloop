#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

namespace inkloop {

template <typename Output>
class JsonAppendWriter {
 public:
  explicit JsonAppendWriter(Output& output) : output_(output) {}

  size_t write(uint8_t value) {
    return output_.concat(static_cast<char>(value)) ? 1 : 0;
  }

  size_t write(const uint8_t* bytes, size_t length) {
    size_t written = 0;
    while (written < length && output_.concat(static_cast<char>(bytes[written]))) ++written;
    return written;
  }

 private:
  Output& output_;
};

// ArduinoJson's direct Arduino String writer clears the String after a caller
// reserves it and may report requested bytes after a failed buffered concat.
// This append writer preserves the exact reservation and verifies both counts.
template <typename JsonSource, typename Output>
bool serializeJsonRecordExactly(const JsonSource& source, Output& output) {
  const size_t expected = measureJson(source);
  output = "";
  if (!expected || !output.reserve(expected)) return false;
  JsonAppendWriter<Output> writer(output);
  const size_t written = serializeJson(source, writer);
  return written == expected && output.length() == expected;
}

}  // namespace inkloop
