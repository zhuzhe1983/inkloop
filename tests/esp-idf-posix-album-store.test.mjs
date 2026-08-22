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
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "inkloop/storage/aigc_album_sink.hpp"
#include "inkloop/storage/album_index.hpp"
#include "inkloop/storage/posix_atomic_album_store.hpp"
#include "inkloop/storage/sha256.hpp"

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
    out.push_back(value); crc = crcByte(crc, value);
  }
  for (uint8_t value : data) { out.push_back(value); crc = crcByte(crc, value); }
  append32(out, crc ^ 0xffffffffUL);
}
std::vector<uint8_t> png(uint8_t variant) {
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  std::vector<uint8_t> ihdr;
  append32(ihdr, 400); append32(ihdr, 600);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", {0x78, 0x9c, variant});
  chunk(out, "IEND", {});
  return out;
}
std::string readFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}
void writeFile(const std::string& path, const std::string& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << bytes;
  assert(output.good());
}
myai::AigcOutputMetadata metadata(size_t bytes, unsigned id) {
  myai::AigcOutputMetadata value;
  value.promptId = "prompt-" + std::to_string(id);
  value.filename = "output.png";
  value.contentType = "image/png";
  value.decodedBytes = bytes;
  return value;
}
bool save(storage::PosixAtomicAlbumStore& store, uint8_t variant,
          storage::AlbumCommitResult& result) {
  storage::AigcAlbumSink sink(store, 1500000, "official-quality");
  const std::vector<uint8_t> bytes = png(variant);
  myai::AigcOutputMetadata value = metadata(bytes.size(), variant);
  if (!sink.begin(value).ok()) return false;
  for (size_t at = 0; at < bytes.size(); at += 7) {
    const size_t count = std::min<size_t>(7, bytes.size() - at);
    if (!sink.write(bytes.data() + at, count).ok()) return false;
  }
  return sink.commit(value).ok() && sink.takeCommittedAsset(result);
}
bool saveTask(storage::PosixAtomicAlbumStore& store, uint8_t variant,
              const std::string& task_id,
              storage::AlbumCommitResult& result) {
  const std::vector<uint8_t> bytes = png(variant);
  if (!store.begin(storage::kMaximumAlbumAssetBytes).ok()) return false;
  for (size_t at = 0; at < bytes.size(); at += 5) {
    const size_t count = std::min<size_t>(5, bytes.size() - at);
    if (!store.append(bytes.data() + at, count).ok()) return false;
  }
  storage::AlbumCommitRequest request;
  request.prompt_id = task_id;
  request.task_id = task_id;
  request.source_filename = "task.png";
  request.render_strategy = "official-quality";
  request.bytes = bytes.size();
  request.landscape = false;
  return store.commitValidated(request, result).ok();
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string root = argv[1];
  storage::Sha256 sha;
  const std::string abc = "abc";
  assert(sha.update(reinterpret_cast<const uint8_t*>(abc.data()), abc.size()));
  std::string digest;
  assert(sha.finishHex(digest));
  assert(digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  storage::PosixAtomicAlbumStore store(root, false);
  assert(store.pathsValid());
  storage::AlbumCommitResult first;
  assert(save(store, 1, first));
  assert(first.asset_id.size() == 64 && first.ordinal == 0);
  assert(first.content_sha256 == first.asset_id);
  assert(readFile(root + first.path) == std::string(
      reinterpret_cast<const char*>(png(1).data()), png(1).size()));
  const std::string index_path = root + "/inkloop-album/index.json";
  storage::AlbumIndex index;
  assert(storage::parseAlbumIndex(readFile(index_path), index) ==
         storage::AlbumIndexCode::Ok);
  assert(index.assets.size() == 1 && index.assets[0].id == first.asset_id);

  // Same bytes deduplicate and do not consume the LittleFS two-entry limit.
  storage::AlbumCommitResult duplicate;
  assert(save(store, 1, duplicate));
  assert(duplicate.asset_id == first.asset_id && duplicate.ordinal == 0);
  assert(storage::parseAlbumIndex(readFile(index_path), index) ==
         storage::AlbumIndexCode::Ok && index.assets.size() == 1);

  storage::AlbumCommitResult second;
  assert(save(store, 2, second) && second.ordinal == 1);
  storage::AlbumCommitResult rejected;
  assert(!save(store, 3, rejected));
  assert(storage::parseAlbumIndex(readFile(index_path), index) ==
         storage::AlbumIndexCode::Ok && index.assets.size() == 2);

  // Server task cache metadata can change render mode and prune an exact old
  // task revision without deleting unrelated AIGC/user assets.
  const std::string task_root = root + "/task";
  assert(::mkdir(task_root.c_str(), 0700) == 0);
  storage::PosixAtomicAlbumStore task_store(task_root, true);
  storage::AlbumCommitResult task_one;
  storage::AlbumCommitResult task_two;
  storage::AlbumCommitResult generated;
  assert(saveTask(task_store, 3, "dtask-one", task_one));
  assert(saveTask(task_store, 4, "dtask-two", task_two));
  assert(save(task_store, 5, generated));
  assert(task_store.markCurrent(task_one.asset_id).ok());
  size_t removed = 0;
  const std::vector<storage::AlbumTaskBinding> retained{
      {"dtask-two", task_two.content_sha256, "solid-clean"}};
  assert(task_store.pruneTaskAssets(retained, removed).ok() && removed == 1);
  storage::AlbumIndex task_index;
  assert(task_store.readCatalog(task_index).ok());
  assert(task_index.assets.size() == 2 && task_index.current.empty());
  assert(task_index.assets[0].id == task_two.asset_id);
  assert(task_index.assets[0].render_strategy == "solid-clean");
  assert(task_index.assets[1].id == generated.asset_id);

  // The on-panel render strategy is persisted independently. A strategy-only
  // edit leaves it stale so the display owner must repaint once.
  assert(task_store.markCurrent(task_two.asset_id).ok());
  assert(task_store.updateRenderStrategy(task_two.asset_id,
                                         "reflectance-photo").ok());
  assert(task_store.readCatalog(task_index).ok());
  assert(task_index.current == task_two.asset_id);
  assert(task_index.current_render_strategy == "solid-clean");
  assert(task_index.assets[0].render_strategy == "reflectance-photo");
  assert(task_store.markCurrent(task_two.asset_id).ok());
  assert(task_store.readCatalog(task_index).ok());
  assert(task_index.current_render_strategy == "reflectance-photo");

  // Identical bytes may be owned by an AIGC entry and multiple server tasks.
  // Logical ids stay independent while the physical content-addressed PNG is
  // shared, and pruning one task must not unlink the retained bytes.
  const std::string shared_root = root + "/shared";
  assert(::mkdir(shared_root.c_str(), 0700) == 0);
  storage::PosixAtomicAlbumStore shared_store(shared_root, true);
  storage::AlbumCommitResult shared_generated;
  storage::AlbumCommitResult shared_one;
  storage::AlbumCommitResult shared_two;
  assert(save(shared_store, 6, shared_generated));
  assert(saveTask(shared_store, 6, "dtask-shared-one", shared_one));
  assert(saveTask(shared_store, 6, "dtask-shared-two", shared_two));
  assert(shared_generated.asset_id == shared_generated.content_sha256);
  assert(shared_one.asset_id != shared_generated.asset_id);
  assert(shared_two.asset_id != shared_one.asset_id);
  assert(shared_generated.path == shared_one.path &&
         shared_one.path == shared_two.path);
  removed = 0;
  const std::vector<storage::AlbumTaskBinding> shared_retained{
      {"dtask-shared-two", shared_two.content_sha256,
       "official-quality"}};
  assert(shared_store.pruneTaskAssets(shared_retained, removed).ok());
  assert(removed == 1);
  assert(readFile(shared_root + shared_generated.path) == std::string(
      reinterpret_cast<const char*>(png(6).data()), png(6).size()));
  assert(shared_store.readCatalog(task_index).ok());
  assert(task_index.assets.size() == 2);

  // User deletion removes one logical owner at a time. Shared bytes survive
  // until the final owner is gone, and deleting the current item clears the
  // persisted panel metadata without choosing/refreshing a replacement.
  assert(shared_store.markCurrent(shared_generated.asset_id).ok());
  assert(shared_store.removeAssetById(shared_generated.asset_id) ==
         storage::AlbumMutationCode::Ok);
  assert(::access((shared_root + shared_generated.path).c_str(), F_OK) == 0);
  assert(shared_store.readCatalog(task_index).ok());
  assert(task_index.assets.size() == 1 && task_index.current.empty() &&
         task_index.current_render_strategy.empty());
  assert(shared_store.removeAssetByOrdinal(0) ==
         storage::AlbumMutationCode::Ok);
  assert(::access((shared_root + shared_generated.path).c_str(), F_OK) != 0);
  assert(shared_store.removeAssetByOrdinal(0) ==
         storage::AlbumMutationCode::NotFound);

  size_t cleared = 0;
  assert(task_store.clearAssets(cleared) == storage::AlbumMutationCode::Ok);
  assert(cleared == 2);
  assert(task_store.readCatalog(task_index).ok() && task_index.assets.empty());
  assert(task_store.clearAssets(cleared) == storage::AlbumMutationCode::Ok &&
         cleared == 0);

  // Display, Portal, cloud delivery and local tools are distinct IDF tasks.
  // Two logical-owner removals must serialize at the store boundary: neither
  // update may resurrect the other owner or unlink bytes still referenced by
  // the retained AIGC owner.
  const std::string concurrent_root = root + "/concurrent";
  assert(::mkdir(concurrent_root.c_str(), 0700) == 0);
  storage::PosixAtomicAlbumStore concurrent_store(concurrent_root, true);
  storage::AlbumCommitResult concurrent_generated;
  storage::AlbumCommitResult concurrent_one;
  storage::AlbumCommitResult concurrent_two;
  assert(save(concurrent_store, 7, concurrent_generated));
  assert(saveTask(concurrent_store, 7, "dtask-concurrent-one", concurrent_one));
  assert(saveTask(concurrent_store, 7, "dtask-concurrent-two", concurrent_two));
  storage::AlbumMutationCode concurrent_result_one =
      storage::AlbumMutationCode::Busy;
  storage::AlbumMutationCode concurrent_result_two =
      storage::AlbumMutationCode::Busy;
  std::thread first_removal([&] {
    concurrent_result_one =
        concurrent_store.removeAssetById(concurrent_one.asset_id);
  });
  std::thread second_removal([&] {
    concurrent_result_two =
        concurrent_store.removeAssetById(concurrent_two.asset_id);
  });
  first_removal.join();
  second_removal.join();
  assert(concurrent_result_one == storage::AlbumMutationCode::Ok);
  assert(concurrent_result_two == storage::AlbumMutationCode::Ok);
  assert(concurrent_store.readCatalog(task_index).ok());
  assert(task_index.assets.size() == 1);
  assert(task_index.assets[0].id == concurrent_generated.asset_id);
  assert(::access((concurrent_root + concurrent_generated.path).c_str(), F_OK) == 0);

  // An index commit failure must leave both the old catalog and physical bytes
  // authoritative. This models failure before the durable index.next step;
  // deletion must never unlink first and create a dangling catalog entry.
  const std::string failure_root = root + "/failure";
  assert(::mkdir(failure_root.c_str(), 0700) == 0);
  storage::PosixAtomicAlbumStore failure_store(failure_root, true);
  storage::AlbumCommitResult failure_asset;
  assert(save(failure_store, 8, failure_asset));
  const std::string blocked_next =
      failure_root + "/inkloop-album/index.next";
  assert(::mkdir(blocked_next.c_str(), 0700) == 0);
  assert(failure_store.removeAssetById(failure_asset.asset_id) ==
         storage::AlbumMutationCode::PersistenceFailed);
  cleared = 99;
  assert(failure_store.clearAssets(cleared) ==
         storage::AlbumMutationCode::PersistenceFailed);
  assert(cleared == 0);
  assert(failure_store.readCatalog(task_index).ok());
  assert(task_index.assets.size() == 1 &&
         task_index.assets[0].id == failure_asset.asset_id);
  assert(::access((failure_root + failure_asset.path).c_str(), F_OK) == 0);
  assert(::rmdir(blocked_next.c_str()) == 0);

  // Destructive filesystem maintenance is one store-wide exclusion boundary,
  // not a check of the current asset.part descriptor. Once admitted, every
  // catalog/staging/path operation fails closed until the composition root
  // releases the gate. An in-flight staging transaction prevents admission.
  const std::string maintenance_root = root + "/maintenance";
  assert(::mkdir(maintenance_root.c_str(), 0700) == 0);
  storage::PosixAtomicAlbumStore maintenance_store(maintenance_root, true);
  storage::AlbumCommitResult maintenance_asset;
  assert(save(maintenance_store, 9, maintenance_asset));
  assert(maintenance_store.begin(storage::kMaximumAlbumAssetBytes).ok());
  assert(!maintenance_store.beginMaintenance());
  maintenance_store.abort();
  const std::string maintenance_part =
      maintenance_root + "/inkloop-album/asset.part";
  writeFile(maintenance_part, "maintenance-sentinel");
  assert(maintenance_store.beginMaintenance());
  assert(!maintenance_store.beginMaintenance());
  storage::AlbumIndex maintenance_index;
  assert(!maintenance_store.readCatalog(maintenance_index).ok());
  assert(maintenance_store.removeAssetById(maintenance_asset.asset_id) ==
         storage::AlbumMutationCode::Busy);
  cleared = 99;
  assert(maintenance_store.clearAssets(cleared) ==
         storage::AlbumMutationCode::Busy && cleared == 0);
  std::string maintenance_path;
  assert(!maintenance_store.absoluteAssetPath(maintenance_index.assets.empty()
              ? storage::AlbumIndexAsset{} : maintenance_index.assets[0],
          maintenance_path));
  assert(!maintenance_store.begin(storage::kMaximumAlbumAssetBytes).ok());
  maintenance_store.abort();
  assert(readFile(maintenance_part) == "maintenance-sentinel");
  maintenance_store.endMaintenance();
  assert(maintenance_store.begin(storage::kMaximumAlbumAssetBytes).ok());
  maintenance_store.abort();
  assert(::access(maintenance_part.c_str(), F_OK) != 0);
  assert(maintenance_store.readCatalog(maintenance_index).ok());
  assert(maintenance_index.assets.size() == 1);
  assert(maintenance_store.absoluteAssetPath(maintenance_index.assets[0],
                                              maintenance_path));

  // Interrupted index: valid .next plus corrupt current is recovered before
  // dedup. No index or asset data is silently initialized over corruption.
  const std::string valid_index = readFile(index_path);
  writeFile(root + "/inkloop-album/index.next", valid_index);
  writeFile(index_path, "corrupt");
  assert(save(store, 1, duplicate));
  assert(storage::parseAlbumIndex(readFile(index_path), index) ==
         storage::AlbumIndexCode::Ok && index.assets.size() == 2);

  writeFile(index_path, "bad-current");
  writeFile(root + "/inkloop-album/index.next", "bad-next");
  writeFile(root + "/inkloop-album/index.prev", "bad-prev");
  assert(!save(store, 1, rejected));
  assert(readFile(index_path) == "bad-current");

  storage::PosixAtomicAlbumStore invalid("relative", true);
  assert(!invalid.pathsValid());
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-posix-album-"));
  try {
    const source = join(scratch, "store.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    const data = join(scratch, "data");
    writeFileSync(source, harness);
    execFileSync("mkdir", [data]);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(storage, "include"),
      "-I", join(myai, "include"),
      "-I", join(myai, "include/inkloop/myai"),
      source,
      join(storage, "aigc_album_sink.cpp"),
      join(storage, "album_index.cpp"),
      join(storage, "papercolor_png.cpp"),
      join(storage, "posix_atomic_album_store.cpp"),
      join(storage, "sha256.cpp"),
      "-pthread",
      "-o", binary,
    ];
    if (sanitized) args.splice(
      1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    );
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

test("POSIX album store atomically persists and deduplicates under C++17", () => {
  buildAndRun(false);
});

test("POSIX album store recovery is fail-closed under ASan/UBSan", () => {
  buildAndRun(true);
});
