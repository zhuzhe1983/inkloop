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
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "inkloop/storage/aigc_album_sink.hpp"
#include "inkloop/storage/album_index.hpp"
#include "inkloop/storage/posix_atomic_album_store.hpp"

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
std::vector<uint8_t> png() {
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  std::vector<uint8_t> ihdr;
  append32(ihdr, 400);
  append32(ihdr, 600);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", {0x78, 0x9c, 0x2a});
  chunk(out, "IEND", {});
  return out;
}

storage::AlbumCommitResult save(
    storage::PosixAtomicAlbumStore& store, const std::string& prompt,
    const std::string& owner, const std::string& strategy) {
  const std::vector<uint8_t> bytes = png();
  assert(store.begin(storage::kMaximumAlbumAssetBytes).ok());
  for (size_t at = 0; at < bytes.size(); at += 3U) {
    const size_t count = std::min<size_t>(3U, bytes.size() - at);
    assert(store.append(bytes.data() + at, count).ok());
  }
  storage::AlbumCommitRequest request;
  request.prompt_id = prompt;
  request.task_id = owner;
  request.source_filename = "delivery.png";
  request.render_strategy = strategy;
  request.bytes = bytes.size();
  request.landscape = false;
  storage::AlbumCommitResult result;
  assert(store.commitValidated(request, result).ok());
  return result;
}

const storage::AlbumIndexAsset& owned(
    const storage::AlbumIndex& index, const std::string& task_id) {
  const auto found = std::find_if(
      index.assets.begin(), index.assets.end(), [&](const auto& asset) {
        return asset.task_id == task_id;
      });
  assert(found != index.assets.end());
  return *found;
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string root = argv[1];
  storage::PosixAtomicAlbumStore store(root, true);
  assert(store.pathsValid());

  // Identical PNG bytes can have independent AIGC and cloud-task ownership.
  const auto generated = save(
      store, "prompt-generated", "aigc:prompt-generated", "official-quality");
  const auto task_one = save(
      store, "dtask-one", "dtask-one", "solid-clean");
  const auto task_two = save(
      store, "dtask-two", "dtask-two", "reflectance-photo");
  assert(generated.asset_id != task_one.asset_id);
  assert(task_one.asset_id != task_two.asset_id);

  storage::AlbumIndex index;
  assert(store.readCatalog(index).ok());
  assert(index.assets.size() == 3U);
  const auto& generated_asset = owned(index, "aigc:prompt-generated");
  const auto& first_asset = owned(index, "dtask-one");
  const auto& second_asset = owned(index, "dtask-two");
  assert(generated_asset.content_sha256 == first_asset.content_sha256);
  assert(first_asset.content_sha256 == second_asset.content_sha256);
  assert(generated_asset.path == first_asset.path &&
         first_asset.path == second_asset.path);
  assert(first_asset.render_strategy == "solid-clean");
  assert(second_asset.render_strategy == "reflectance-photo");
  const std::string shared_path = root + generated_asset.path;
  assert(::access(shared_path.c_str(), F_OK) == 0);

  // Sync can update one task's strategy and delete another without changing
  // the independent owner or unlinking their shared content.
  size_t removed = 0;
  const std::vector<storage::AlbumTaskBinding> retained{
      {"dtask-two", second_asset.content_sha256, "classic-six-color"}};
  assert(store.pruneTaskAssets(retained, removed).ok());
  assert(removed == 1U);
  assert(store.readCatalog(index).ok());
  assert(index.assets.size() == 2U);
  assert(owned(index, "dtask-two").render_strategy == "classic-six-color");
  assert(owned(index, "aigc:prompt-generated").id == generated.asset_id);
  assert(::access(shared_path.c_str(), F_OK) == 0);

  // Deleting the final task owner still leaves the unrelated generated album
  // entry and its physical PNG readable.
  assert(store.pruneTaskAssets({}, removed).ok());
  assert(removed == 1U);
  assert(store.readCatalog(index).ok() && index.assets.size() == 1U);
  assert(index.assets[0].id == generated.asset_id);
  assert(::access(shared_path.c_str(), F_OK) == 0);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-native-delivery-store-"));
  try {
    const source = join(scratch, "delivery.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    const data = join(scratch, "data");
    writeFileSync(source, harness);
    execFileSync("mkdir", [data]);
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
      join(storage, "album_index.cpp"),
      join(storage, "papercolor_png.cpp"),
      join(storage, "posix_atomic_album_store.cpp"),
      join(storage, "sha256.cpp"),
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
    execFileSync(binary, [data], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("cloud-task cache ownership survives same-content dedup under strict C++17", () => {
  buildAndRun(false);
});

test("shared cloud-task content prune is safe under ASan/UBSan", () => {
  buildAndRun(true);
});
