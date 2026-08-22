import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const storage = join(repo, "firmware/inkloop-idf/components/inkloop_storage");
const myai = join(repo, "firmware/inkloop-idf/components/inkloop_myai");

const harness = String.raw`
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "inkloop/storage/aigc_album_sink.hpp"

using namespace inkloop;

uint32_t crcByte(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (unsigned bit = 0; bit < 8; ++bit)
    crc = (crc & 1U) ? (crc >> 1U) ^ 0xedb88320UL : crc >> 1U;
  return crc;
}

void append32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24U));
  out.push_back(static_cast<uint8_t>(value >> 16U));
  out.push_back(static_cast<uint8_t>(value >> 8U));
  out.push_back(static_cast<uint8_t>(value));
}

void chunk(std::vector<uint8_t>& out, const char* type,
           const std::vector<uint8_t>& data) {
  append32(out, static_cast<uint32_t>(data.size()));
  uint32_t crc = 0xffffffffUL;
  for (unsigned index = 0; index < 4; ++index) {
    const uint8_t value = static_cast<uint8_t>(type[index]);
    out.push_back(value);
    crc = crcByte(crc, value);
  }
  for (uint8_t value : data) {
    out.push_back(value);
    crc = crcByte(crc, value);
  }
  append32(out, crc ^ 0xffffffffUL);
}

std::vector<uint8_t> png(bool landscape = false) {
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  const uint32_t width = landscape ? 600 : 400;
  const uint32_t height = landscape ? 400 : 600;
  std::vector<uint8_t> ihdr;
  append32(ihdr, width);
  append32(ihdr, height);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", {0x78, 0x9c, 0x01});
  chunk(out, "IEND", {});
  return out;
}

struct Store final : storage::IAlbumStagingStore {
  bool active = false;
  bool fail_begin = false;
  bool fail_write = false;
  bool fail_commit = false;
  unsigned begins = 0;
  unsigned commits = 0;
  unsigned aborts = 0;
  size_t maximum = 0;
  std::vector<uint8_t> bytes;
  storage::AlbumCommitRequest request;

  myai::Status begin(size_t cap) override {
    ++begins;
    if (fail_begin) return myai::Status(myai::ErrorCode::Storage);
    active = true;
    maximum = cap;
    bytes.clear();
    return myai::Status::success();
  }
  myai::Status append(const uint8_t* data, size_t length) override {
    if (!active || fail_write) return myai::Status(myai::ErrorCode::Storage);
    bytes.insert(bytes.end(), data, data + length);
    return myai::Status::success();
  }
  myai::Status commitValidated(
      const storage::AlbumCommitRequest& input,
      storage::AlbumCommitResult& output) override {
    ++commits;
    if (!active || fail_commit) return myai::Status(myai::ErrorCode::Storage);
    storage::PaperColorPngValidator validator(maximum);
    assert(validator.append(bytes.data(), bytes.size()));
    assert(validator.finish(bytes.size()));
    assert(input.bytes == bytes.size());
    request = input;
    output.asset_id = std::string(64, 'a');
    output.path = "/inkloop-album/" + output.asset_id + ".png";
    output.ordinal = 3;
    active = false;
    return myai::Status::success();
  }
  void abort() override {
    ++aborts;
    active = false;
    bytes.clear();
  }
};

myai::AigcOutputMetadata metadata(size_t bytes) {
  myai::AigcOutputMetadata value;
  value.promptId = "prompt-123";
  value.filename = "output.png";
  value.contentType = "image/png";
  value.decodedBytes = bytes;
  return value;
}

int main() {
  const std::vector<uint8_t> portrait = png();
  for (size_t stride = 1; stride <= portrait.size(); ++stride) {
    Store store;
    storage::AigcAlbumSink sink(store, 1500000, "official-quality");
    myai::AigcOutputMetadata value = metadata(portrait.size());
    assert(sink.begin(value).ok() && sink.active());
    for (size_t at = 0; at < portrait.size(); at += stride) {
      const size_t length = std::min(stride, portrait.size() - at);
      assert(sink.write(portrait.data() + at, length).ok());
    }
    assert(sink.commit(value).ok() && !sink.active());
    assert(store.commits == 1 && store.request.bytes == portrait.size());
    assert(!store.request.landscape &&
           store.request.render_strategy == "official-quality");
    storage::AlbumCommitResult asset;
    assert(sink.takeCommittedAsset(asset) && asset.ordinal == 3);
    assert(!sink.takeCommittedAsset(asset));
  }

  Store landscape_store;
  storage::AigcAlbumSink landscape_sink(
      landscape_store, 1500000, "solid-clean");
  const std::vector<uint8_t> landscape = png(true);
  myai::AigcOutputMetadata landscape_meta = metadata(landscape.size());
  assert(landscape_sink.begin(landscape_meta).ok());
  assert(landscape_sink.write(landscape.data(), landscape.size()).ok());
  assert(landscape_sink.commit(landscape_meta).ok());
  assert(landscape_store.request.landscape);

  Store corrupt_store;
  storage::AigcAlbumSink corrupt(corrupt_store, 1500000, "quality");
  myai::AigcOutputMetadata corrupt_meta = metadata(portrait.size());
  assert(corrupt.begin(corrupt_meta).ok());
  std::vector<uint8_t> broken = portrait;
  broken[12] ^= 1;
  assert(!corrupt.write(broken.data(), broken.size()).ok());
  assert(corrupt_store.aborts == 1 && corrupt_store.commits == 0);

  Store failed_store;
  failed_store.fail_write = true;
  storage::AigcAlbumSink failed(failed_store, 1500000, "quality");
  myai::AigcOutputMetadata failed_meta = metadata(portrait.size());
  assert(failed.begin(failed_meta).ok());
  assert(failed.write(portrait.data(), 1).code == myai::ErrorCode::Storage);
  assert(failed_store.aborts == 1);

  Store mismatch_store;
  storage::AigcAlbumSink mismatch(mismatch_store, 1500000, "quality");
  myai::AigcOutputMetadata mismatch_meta = metadata(portrait.size() - 1);
  assert(mismatch.begin(mismatch_meta).ok());
  assert(mismatch.write(portrait.data(), portrait.size()).ok());
  assert(mismatch.commit(mismatch_meta).code == myai::ErrorCode::Protocol);
  assert(mismatch_store.commits == 0);

  Store commit_store;
  commit_store.fail_commit = true;
  storage::AigcAlbumSink commit_failure(commit_store, 1500000, "quality");
  myai::AigcOutputMetadata commit_meta = metadata(portrait.size());
  assert(commit_failure.begin(commit_meta).ok());
  assert(commit_failure.write(portrait.data(), portrait.size()).ok());
  assert(commit_failure.commit(commit_meta).code == myai::ErrorCode::Storage);
  assert(commit_store.aborts == 1);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-aigc-album-"));
  try {
    const source = join(scratch, "album.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I",
      join(storage, "include"),
      "-I",
      join(myai, "include"),
      "-I",
      join(myai, "include/inkloop/myai"),
      source,
      join(storage, "papercolor_png.cpp"),
      join(storage, "aigc_album_sink.cpp"),
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

test("AIGC album sink validates and commits every chunk boundary", () => {
  buildAndRun(false);
});

test("AIGC album sink fails closed under ASan/UBSan", () => {
  buildAndRun(true);
});
