import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const myai = join(repo, "firmware/inkloop-idf/components/inkloop_myai");
const native = join(repo, "firmware/inkloop-idf/components/inkloop_myai_idf");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "WssIngress.h"

using namespace inkloop::myai;

Status append(WssIngressAssembler& parser, uint8_t opcode, bool fin,
              size_t total, size_t offset, const uint8_t* data, size_t length,
              bool& complete, WssCompletedMessage& output) {
  WssIngressChunk chunk;
  chunk.opcode = opcode;
  chunk.finalFrame = fin;
  chunk.framePayloadBytes = total;
  chunk.frameOffset = offset;
  chunk.bytes = data;
  chunk.length = length;
  return parser.append(chunk, complete, output);
}

int main() {
  WssIngressAssembler parser;
  bool complete = false;
  WssCompletedMessage output;
  const uint8_t hello[] = {'h','e','l','l','o'};
  assert(append(parser, 1, true, 5, 0, hello, 2, complete, output).ok());
  assert(!complete);
  assert(append(parser, 1, true, 5, 2, hello + 2, 3, complete, output).ok());
  assert(complete && output.kind == WssMessageKind::Text && output.length == 5);
  assert(std::memcmp(output.bytes, hello, 5) == 0);

  const uint8_t first[] = {'a','s','r'};
  const uint8_t second[] = {'.','f','i','n','a','l'};
  assert(append(parser, 1, false, 3, 0, first, 3, complete, output).ok());
  assert(!complete);
  assert(append(parser, 0, true, 6, 0, second, 1, complete, output).ok());
  assert(!complete);
  assert(append(parser, 0, true, 6, 1, second + 1, 5, complete, output).ok());
  assert(complete && output.kind == WssMessageKind::Text && output.length == 9);
  assert(std::string(reinterpret_cast<const char*>(output.bytes), output.length) ==
         "asr.final");

  std::vector<uint8_t> audio(WssIngressAssembler::kMaximumMessageBytes, 0x5a);
  for (size_t offset = 0; offset < audio.size(); offset += 997) {
    const size_t count = std::min<size_t>(997, audio.size() - offset);
    assert(append(parser, 2, true, audio.size(), offset, audio.data() + offset,
                  count, complete, output).ok());
  }
  assert(complete && output.kind == WssMessageKind::Binary &&
         output.length == audio.size());

  uint8_t byte = 1;
  assert(append(parser, 2, true, audio.size() + 1, 0, &byte, 1,
                complete, output).code == ErrorCode::TooLarge);
  assert(append(parser, 0, true, 1, 0, &byte, 1,
                complete, output).code == ErrorCode::Protocol);
  assert(append(parser, 9, true, 1, 0, &byte, 1,
                complete, output).code == ErrorCode::Protocol);

  assert(append(parser, 1, true, 2, 0, &byte, 1, complete, output).ok());
  assert(append(parser, 1, true, 1, 0, &byte, 1,
                complete, output).code == ErrorCode::Protocol);
  assert(append(parser, 1, true, 2, 0, &byte, 1, complete, output).ok());
  assert(append(parser, 1, true, 2, 0, &byte, 1,
                complete, output).code == ErrorCode::Protocol);

  // A failure resets the parser and never exposes a partial message.
  assert(append(parser, 1, true, 1, 0, &byte, 1, complete, output).ok());
  assert(complete && output.length == 1);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-wss-ingress-"));
  try {
    const source = join(scratch, "wss.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(myai, "include"),
      "-I", join(myai, "include/inkloop/myai"), source,
      join(myai, "WssIngress.cpp"), "-o", binary,
    ];
    if (sanitized) {
      args.splice(1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer");
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

test("WSS ingress is bounded and fragment-safe under strict C++17", () => {
  buildAndRun(false);
});

test("WSS ingress survives adversarial fragmentation under ASan and UBSan", () => {
  buildAndRun(true);
});

test("native WS/WSS adapter rejects redirects and checks the connected public peer", () => {
  const source = readFileSync(join(native, "esp_wss_transport.cpp"), "utf8");
  const cmake = readFileSync(join(native, "CMakeLists.txt"), "utf8");
  const defaults = readFileSync(join(repo, "firmware/inkloop-idf/sdkconfig.defaults"), "utf8");
  assert.match(source, /endpoint\.tls \? esp_transport_ssl_init\(\) : esp_transport_tcp_init\(\)/);
  assert.match(source, /esp_transport_ssl_crt_bundle_attach\(network_, esp_crt_bundle_attach\)/);
  assert.match(source, /esp_transport_ssl_set_common_name\(network_, host_\.c_str\(\)\)/);
  assert.match(source, /"ws:\/\/"/);
  assert.match(source, /result != 0[\s\S]+redirect[\s\S]+Security/);
  assert.match(source, /get_upgrade_request_status\(websocket_\) != 101/);
  assert.match(source, /validateConnectedSocket[\s\S]+esp_transport_get_socket/);
  assert.match(source, /esp_transport_poll_read\(websocket_, 0\)/);
  assert.match(source, /std::array<uint8_t, 2048> chunk/);
  assert.match(source, /currentFrameOffset_/);
  assert.match(source, /CONFIG_LOG_MAXIMUM_LEVEL > 3/);
  assert.match(cmake, /tcp_transport/);
  assert.match(defaults, /CONFIG_WS_BUFFER_SIZE=4096/);
  assert.doesNotMatch(source, /esp_websocket_client|xTaskCreate|std::thread/);
  assert.doesNotMatch(source, /ESP_LOG|printf\s*\(|puts\s*\(/);
});
