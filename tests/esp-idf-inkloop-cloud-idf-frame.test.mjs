import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const adapter = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_cloud_idf",
);
const cloud = join(repo, "firmware/inkloop-idf/components/inkloop_cloud");
const storage = join(repo, "firmware/inkloop-idf/components/inkloop_storage");
const myai = join(repo, "firmware/inkloop-idf/components/inkloop_myai");

const harness = String.raw`
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "inkloop/cloud/inkloop_frame_album_sink.hpp"
#include "inkloop/cloud/inkloop_frame_stream.hpp"

using namespace inkloop;

uint32_t crcUpdate(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (unsigned bit = 0; bit < 8; ++bit)
    crc = (crc & 1U) ? (crc >> 1U) ^ 0xedb88320UL : crc >> 1U;
  return crc;
}

void appendU32(std::vector<uint8_t>& output, uint32_t value) {
  output.push_back(static_cast<uint8_t>(value >> 24U));
  output.push_back(static_cast<uint8_t>(value >> 16U));
  output.push_back(static_cast<uint8_t>(value >> 8U));
  output.push_back(static_cast<uint8_t>(value));
}

void chunk(std::vector<uint8_t>& output, const char type[5],
           const std::vector<uint8_t>& data) {
  appendU32(output, static_cast<uint32_t>(data.size()));
  uint32_t crc = 0xffffffffUL;
  for (size_t index = 0; index < 4; ++index) {
    const uint8_t value = static_cast<uint8_t>(type[index]);
    output.push_back(value);
    crc = crcUpdate(crc, value);
  }
  for (const uint8_t value : data) {
    output.push_back(value);
    crc = crcUpdate(crc, value);
  }
  appendU32(output, crc ^ 0xffffffffUL);
}

std::vector<uint8_t> png(uint32_t width, uint32_t height) {
  std::vector<uint8_t> output{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  std::vector<uint8_t> ihdr;
  appendU32(ihdr, width);
  appendU32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk(output, "IHDR", ihdr);
  chunk(output, "IDAT", {0x78, 0x01, 0x00, 0x00, 0xff, 0xff});
  chunk(output, "IEND", {});
  return output;
}

std::string sha256(const std::vector<uint8_t>& bytes) {
  storage::Sha256 sha;
  assert(sha.update(bytes.data(), bytes.size()));
  std::string output;
  assert(sha.finishHex(output));
  return output;
}

struct Sink final : cloud::IInkloopFrameStagingSink {
  bool began = false;
  bool committed = false;
  bool fail_write = false;
  bool fail_commit = false;
  unsigned aborts = 0;
  cloud::InkloopFrameStagingRequest request;
  cloud::InkloopFrameMetadata metadata;
  std::vector<uint8_t> bytes;

  cloud::InkloopCloudStatus begin(
      const cloud::InkloopFrameStagingRequest& value) override {
    began = true;
    request = value;
    return cloud::InkloopCloudStatus::success();
  }
  cloud::InkloopCloudStatus append(
      const uint8_t* data, size_t length) override {
    if (fail_write)
      return {cloud::InkloopCloudCode::Storage, 0, 0, "write"};
    bytes.insert(bytes.end(), data, data + length);
    return cloud::InkloopCloudStatus::success();
  }
  cloud::InkloopCloudStatus commit(
      const cloud::InkloopFrameMetadata& value) override {
    if (fail_commit)
      return {cloud::InkloopCloudCode::Storage, 0, 0, "commit"};
    metadata = value;
    committed = true;
    return cloud::InkloopCloudStatus::success();
  }
  void abort() override { ++aborts; }
};

cloud::InkloopFrameStagingRequest request(size_t bytes) {
  cloud::InkloopFrameStagingRequest output;
  output.task_id = "dtask-exact-server-id";
  output.render_strategy = "solid-clean";
  output.content_length = bytes;
  return output;
}

struct Album final : storage::IAlbumStagingStore {
  bool active = false;
  bool aborted = false;
  std::vector<uint8_t> bytes;
  storage::AlbumCommitRequest committed;
  std::string asset;

  myai::Status begin(size_t maximum) override {
    assert(maximum == cloud::kMaximumInkloopFrameBytes);
    active = true;
    aborted = false;
    bytes.clear();
    return myai::Status::success();
  }
  myai::Status append(const uint8_t* data, size_t length) override {
    assert(active);
    bytes.insert(bytes.end(), data, data + length);
    return myai::Status::success();
  }
  myai::Status commitValidated(
      const storage::AlbumCommitRequest& value,
      storage::AlbumCommitResult& result) override {
    assert(active);
    committed = value;
    active = false;
    result.asset_id = asset;
    result.content_sha256 = asset;
    result.path = "/inkloop-album/" + asset + ".png";
    result.ordinal = 3;
    return myai::Status::success();
  }
  void abort() override {
    aborted = true;
    active = false;
  }
};

int main() {
  for (const auto dimensions : {std::pair<uint32_t, uint32_t>{400, 600},
                                std::pair<uint32_t, uint32_t>{600, 400}}) {
    const auto image = png(dimensions.first, dimensions.second);
    const std::string expected = sha256(image);
    for (size_t stride = 1; stride <= image.size(); ++stride) {
      Sink sink;
      cloud::InkloopFrameStream stream(sink);
      assert(stream.begin(request(image.size()), expected).ok());
      for (size_t at = 0; at < image.size(); at += stride) {
        const size_t count = std::min(stride, image.size() - at);
        assert(stream.append(image.data() + at, count).ok());
      }
      cloud::InkloopFrameMetadata metadata;
      assert(stream.finish(metadata).ok());
      assert(sink.began && sink.committed && sink.bytes == image);
      assert(metadata.bytes == image.size());
      assert(metadata.landscape == (dimensions.first == 600));
      assert(metadata.sha256 == expected);
      assert(sink.request.task_id == "dtask-exact-server-id");
    }
  }

  const auto image = png(400, 600);
  const std::string expected = sha256(image);

  Sink too_large;
  cloud::InkloopFrameStream oversized(too_large);
  auto oversized_request = request(cloud::kMaximumInkloopFrameBytes + 1U);
  assert(oversized.begin(oversized_request, expected).code ==
         cloud::InkloopCloudCode::TooLarge);
  assert(!too_large.began);

  Sink short_sink;
  cloud::InkloopFrameStream short_stream(short_sink);
  auto longer = request(image.size() + 1U);
  assert(short_stream.begin(longer, expected).ok());
  assert(short_stream.append(image.data(), image.size()).ok());
  cloud::InkloopFrameMetadata metadata;
  assert(short_stream.finish(metadata).code == cloud::InkloopCloudCode::Protocol);
  assert(short_sink.aborts > 0 && !short_sink.committed);

  Sink excess_sink;
  cloud::InkloopFrameStream excess(excess_sink);
  assert(excess.begin(request(image.size() - 1U), expected).ok());
  assert(excess.append(image.data(), image.size()).code ==
         cloud::InkloopCloudCode::TooLarge);
  assert(excess_sink.bytes.empty() && excess_sink.aborts > 0);

  const auto wrong_dimensions = png(401, 600);
  Sink dimension_sink;
  cloud::InkloopFrameStream dimensions(dimension_sink);
  assert(dimensions.begin(request(wrong_dimensions.size()),
                          sha256(wrong_dimensions)).ok());
  assert(dimensions.append(wrong_dimensions.data(), wrong_dimensions.size()).code ==
         cloud::InkloopCloudCode::Protocol);
  assert(!dimension_sink.committed && dimension_sink.aborts > 0);

  Sink hash_sink;
  cloud::InkloopFrameStream hash_stream(hash_sink);
  assert(hash_stream.begin(request(image.size()), std::string(64, '0')).ok());
  assert(hash_stream.append(image.data(), image.size()).ok());
  assert(hash_stream.finish(metadata).code == cloud::InkloopCloudCode::Security);
  assert(!hash_sink.committed && hash_sink.aborts > 0);

  Sink write_sink;
  write_sink.fail_write = true;
  cloud::InkloopFrameStream write_stream(write_sink);
  assert(write_stream.begin(request(image.size()), expected).ok());
  assert(write_stream.append(image.data(), image.size()).code ==
         cloud::InkloopCloudCode::Storage);
  assert(write_sink.aborts > 0 && !write_sink.committed);

  Sink commit_sink;
  commit_sink.fail_commit = true;
  cloud::InkloopFrameStream commit_stream(commit_sink);
  assert(commit_stream.begin(request(image.size()), expected).ok());
  assert(commit_stream.append(image.data(), image.size()).ok());
  assert(commit_stream.finish(metadata).code == cloud::InkloopCloudCode::Storage);
  assert(commit_sink.aborts > 0 && !commit_sink.committed);

  // The production album bridge writes the exact task id, never an AIGC alias.
  Album album;
  album.asset = expected;
  cloud::InkloopFrameAlbumSink album_sink(album);
  cloud::InkloopFrameStream album_stream(album_sink);
  assert(album_stream.begin(request(image.size()), expected).ok());
  for (size_t at = 0; at < image.size(); at += 7U) {
    const size_t count = std::min<size_t>(7U, image.size() - at);
    assert(album_stream.append(image.data() + at, count).ok());
  }
  assert(album_stream.finish(metadata).ok());
  storage::AlbumCommitResult committed;
  assert(album_sink.takeCommittedAsset(committed));
  assert(committed.asset_id == expected && committed.ordinal == 3);
  assert(album.committed.task_id == "dtask-exact-server-id");
  assert(album.committed.prompt_id == "dtask-exact-server-id");
  assert(album.committed.render_strategy == "solid-clean");
  assert(album.committed.bytes == image.size() && !album.committed.landscape);
  assert(album.bytes == image);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-cloud-idf-frame-"));
  try {
    const source = join(scratch, "frame.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const sanitizer = sanitized
      ? ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
      : [];
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      ...sanitizer,
      "-I", join(adapter, "include"),
      "-I", join(cloud, "include"),
      "-I", join(storage, "include"),
      "-I", join(myai, "include"),
      "-I", join(myai, "include/inkloop/myai"),
      source,
      join(adapter, "inkloop_frame_stream.cpp"),
      join(adapter, "inkloop_frame_album_sink.cpp"),
      join(storage, "papercolor_png.cpp"),
      join(storage, "sha256.cpp"),
      "-o", binary,
    ], { stdio: "pipe" });
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

test("Inkloop frame stream validates PNG dimensions, exact bytes and SHA", () => {
  buildAndRun(false);
});

test("Inkloop frame stream remains fail closed under ASan/UBSan", () => {
  buildAndRun(true);
});

test("native frame downloader pins TLS, public peer and bounded streaming", () => {
  const source = readFileSync(join(adapter, "esp_frame_downloader.cpp"), "utf8");
  const header = readFileSync(join(
    adapter,
    "include/inkloop/cloud/esp_frame_downloader.hpp",
  ), "utf8");
  assert.match(header, /public IInkloopFrameDownloader/);
  assert.match(source, /validatePublicTlsEndpoint/);
  assert.match(source, /validateConnectedSocket/);
  assert.match(source, /crt_bundle_attach = esp_crt_bundle_attach/);
  assert.match(source, /skip_cert_common_name_check = false/);
  assert.match(source, /disable_auto_redirect = true/);
  assert.match(source, /max_redirection_count = 0/);
  assert.match(source, /esp_http_client_is_chunked_response/);
  assert.match(source, /content_length < 45/);
  assert.match(source, /kMaximumInkloopFrameBytes/);
  assert.match(source, /std::array<uint8_t, kReadBufferBytes>/);
  assert.match(source, /esp_http_client_is_complete_data_received/);
  assert.match(source, /stream\.append/);
  assert.match(source, /request\.task_id = task\.id/);
  assert.doesNotMatch(source, /std::vector\s*<\s*uint8_t|response\.body/);
  assert.doesNotMatch(source, /ESP_LOG|printf\s*\(|puts\s*\(/);
  assert.doesNotMatch(source, /skip_cert_common_name_check\s*=\s*true/);
});
