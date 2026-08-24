import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const core = join(repo, "firmware/inkloop-idf/components/inkloop_myai");
const native = join(repo, "firmware/inkloop-idf/components/inkloop_myai_idf");

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "AigcStreamDecoder.h"
#include "inkloop/myai/esp_aigc_output_transport.hpp"

using namespace inkloop::myai;

struct Sink final : IImageSink {
  bool began = false;
  bool committed = false;
  bool aborted = false;
  bool fail_write = false;
  std::vector<uint8_t> bytes;
  Status begin(const AigcOutputMetadata& metadata) override {
    assert(metadata.contentType == "image/png" ||
           metadata.contentType == "image/x-png");
    began = true;
    return Status::success();
  }
  Status write(const uint8_t* data, size_t length) override {
    if (fail_write) return Status(ErrorCode::Storage, 0, "sink failed");
    bytes.insert(bytes.end(), data, data + length);
    return Status::success();
  }
  Status commit(AigcOutputMetadata&) override {
    committed = true;
    return Status::success();
  }
  void abort() override { aborted = true; }
};

Status feed(AigcStreamDecoder& decoder, const std::string& input,
            size_t stride) {
  Status status;
  for (size_t at = 0; at < input.size() && status.ok(); at += stride) {
    const size_t count = std::min(stride, input.size() - at);
    status = decoder.append(
        reinterpret_cast<const uint8_t*>(input.data() + at), count);
  }
  return status;
}

int main() {
  assert(detail::aigcOutputHttpErrorCode(401) == ErrorCode::Unauthorized);
  assert(detail::aigcOutputHttpErrorCode(402) == ErrorCode::PaymentRequired);
  assert(detail::aigcOutputHttpErrorCode(500) == ErrorCode::Protocol);
  const std::string valid =
      "{\"content_type\":\"image/png\",\"content_base64\":\""
      "iVBORw0KGgo=\"}";
  for (size_t stride = 1; stride <= valid.size(); ++stride) {
    Sink sink;
    AigcOutputMetadata metadata;
    AigcStreamDecoder decoder(128, 128, sink, metadata);
    assert(feed(decoder, valid, stride).ok());
    assert(decoder.finish().ok());
    const uint8_t expected[] = {0x89, 0x50, 0x4e, 0x47, 0x0d,
                                0x0a, 0x1a, 0x0a};
    assert(sink.began && sink.bytes.size() == sizeof(expected));
    assert(std::equal(sink.bytes.begin(), sink.bytes.end(), expected));
    assert(metadata.contentType == "image/png" &&
           metadata.decodedBytes == sizeof(expected));
  }

  const char* rejected[] = {
      "{}",
      "{\"content_type\":\"image/jpeg\",\"content_base64\":\"TQ==\"}",
      "{\"content_type\":\"image/png\",\"content_base64\":\"\"}",
      "{\"content_type\":\"image/png\",\"content_base64\":\"A\"}",
      "{\"content_type\":\"image/png\",\"content_base64\":\"T===\"}",
      "{\"content_type\":\"image/png\",\"content_base64\":\"TQ==A\"}",
      "{\"content_type\":\"image/png\",\"content_base64\":\"!!!!\"}",
      "{\"content_type\":\"image/png\",\"content_base64\":\"TQ==\"}x",
  };
  for (const char* input : rejected) {
    Sink sink;
    AigcOutputMetadata metadata;
    AigcStreamDecoder decoder(128, 128, sink, metadata);
    Status status = feed(decoder, input, 1);
    if (status.ok()) status = decoder.finish();
    assert(!status.ok());
  }

  Sink too_small;
  AigcOutputMetadata small_metadata;
  AigcStreamDecoder limited(128, 4, too_small, small_metadata);
  assert(!feed(limited, valid, 3).ok());

  Sink failed;
  failed.fail_write = true;
  AigcOutputMetadata failed_metadata;
  AigcStreamDecoder write_failure(128, 128, failed, failed_metadata);
  assert(feed(write_failure, valid, 2).code == ErrorCode::Storage);

  Sink invalid_sink;
  AigcOutputMetadata invalid_metadata;
  AigcStreamDecoder invalid(0, 1, invalid_sink, invalid_metadata);
  assert(!invalid.append(reinterpret_cast<const uint8_t*>(valid.data()),
                         valid.size()).ok());
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-aigc-stream-"));
  try {
    const source = join(scratch, "aigc.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I",
      join(core, "include/inkloop/myai"),
      "-I",
      join(core, "include"),
      "-I",
      join(native, "include"),
      source,
      join(core, "AigcStreamDecoder.cpp"),
      "-o",
      binary,
    ];
    if (sanitized) {
      args.splice(
        1,
        0,
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
      );
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

test("AIGC JSON/base64 stream decoder is strict and chunk independent", () => {
  buildAndRun(false);
});

test("AIGC stream decoder survives adversarial chunks under ASan/UBSan", () => {
  buildAndRun(true);
});

test("native AIGC transport streams HTTP into a sink with fail-closed policy", () => {
  const source = readFileSync(
    join(native, "esp_aigc_output_transport.cpp"),
    "utf8",
  );
  assert.match(source, /esp_http_client_open/);
  assert.match(source, /validateConnectedSocket/);
  assert.match(source, /esp_http_client_read/);
  assert.match(source, /AigcStreamDecoder decoder/);
  assert.match(source, /esp_http_client_is_complete_data_received/);
  assert.match(source, /kMaximumErrorBodyBytes = 4096U/);
  assert.match(source, /parseErrorCode\(body\)/);
  assert.match(source, /parseErrorDiagnostic\(body\)/);
  assert.match(source, /aigcOutputHttpErrorCode\(http_status\)/);
  assert.match(source, /sink\.commit\(metadata\)/);
  assert.match(source, /sink\.abort\(\)/);
  assert.match(source, /EspNetworkOperationLease network_lease\(request\.timeoutMs\)/);
  assert.match(source, /AIGC network operation gate timed out/);
  assert.match(source, /crt_bundle_attach = endpoint\.tls \? esp_crt_bundle_attach : nullptr/);
  assert.match(source, /plaintextPublicGatewayAllowed/);
  assert.match(source, /skip_cert_common_name_check = false/);
  assert.match(source, /disable_auto_redirect = true/);
  assert.doesNotMatch(source, /std::vector\s*<\s*uint8_t|response\.body|ESP_LOG|printf\s*\(/);
});
