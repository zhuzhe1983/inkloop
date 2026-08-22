import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const storage = join(repo, "firmware/inkloop-idf/components/inkloop_storage");
const cjson = join(
  repo,
  "firmware/inkloop-idf/managed_components/espressif__cjson/cJSON",
);

const espHeaderStub = String.raw`
#pragma once

#include <string>

#include "inkloop/storage/legacy_display_recovery.hpp"

namespace myai {

enum class ErrorCode { None, InvalidState, IoError };

struct Status {
  ErrorCode code = ErrorCode::None;
  bool ok() const { return code == ErrorCode::None; }
};

}  // namespace myai

namespace inkloop {
namespace storage {

class PosixAtomicAlbumStore {
 public:
  myai::Status markCurrent(const std::string& asset_id);

  myai::Status status{};
  int mark_calls = 0;
  std::string marked_asset;
};

class EspStorageMountOwner {
 public:
  EspStorageMountOwner(std::string task, std::string internal,
                       std::string removable);

  const char* taskRoot() const;
  const char* internalRoot() const;
  const char* removableRoot() const;
  PosixAtomicAlbumStore* albumStoreForLegacyIdentity(const char* identity);

  std::string task_root;
  std::string internal_root;
  std::string removable_root;
  std::string last_identity;
  PosixAtomicAlbumStore internal_album;
  PosixAtomicAlbumStore removable_album;
};

class EspLegacyDisplayRecovery final : public ILegacyDisplayRecoverySource,
                                       public ILegacyDisplayResolutionAdapter {
 public:
  explicit EspLegacyDisplayRecovery(EspStorageMountOwner& storage);

  LegacyDisplayRecoveryProbe inspect(
      LegacyDisplayRecoverySnapshot& output) const override;
  LegacyDisplayResolutionCode resolve(
      const LegacyDisplayRecoverySnapshot& expected,
      LegacyDisplayResolutionChoice choice);
  LegacyDisplayResolutionAdapterCode applyTargetCurrent(
      const LegacyDisplayJournal& journal) override;
  LegacyDisplayResolutionAdapterCode acknowledgeTask(
      const LegacyDisplayJournal& journal) override;
  LegacyDisplayResolutionAdapterCode clearJournalSet() override;

 private:
  static bool readRecord(const std::string& path,
                         RawLegacyDisplayRecord& output);
  static LegacyDisplayResolutionAdapterCode albumResult(
      const myai::Status& status);

  EspStorageMountOwner& storage_;
};

}  // namespace storage
}  // namespace inkloop
`;

const inventoryHeaderStub = String.raw`
#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace inkloop {
namespace storage {

enum class RecordProbe { Missing, Valid, Invalid, IoError };
enum class TransactionAudit { Empty, Clean, RecoveryRequired };

struct TransactionProbe {
  RecordProbe current = RecordProbe::Missing;
  RecordProbe next = RecordProbe::Missing;
  RecordProbe previous = RecordProbe::Missing;
};

inline constexpr std::array<const char*, 20> kProtectedFilePaths{};

TransactionAudit classifyTransaction(const TransactionProbe& input);

class PosixUpgradeInventory {
 public:
  explicit PosixUpgradeInventory(std::string root);
  std::array<RecordProbe, kProtectedFilePaths.size()> inspectFiles() const;

 private:
  std::string root_;
};

}  // namespace storage
}  // namespace inkloop
`;

const taskHeaderStub = String.raw`
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace inkloop {
namespace storage {

enum class TaskStoreCode { Ok, RecoveryRequired, IoError };

class PosixTaskStore {
 public:
  explicit PosixTaskStore(std::string root);
  TaskStoreCode initialize();
  TaskStoreCode markRun(const std::string& id, std::uint32_t revision,
                        std::time_t run_at, std::uint32_t local_day);

 private:
  std::string root_;
};

}  // namespace storage
}  // namespace inkloop
`;

const harness = String.raw`
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "inkloop/storage/esp_legacy_display_recovery.hpp"
#include "inkloop/storage/posix_task_store.hpp"
#include "inkloop/storage/posix_upgrade_inventory.hpp"

using namespace inkloop::storage;

namespace inkloop {
namespace storage {
namespace {

std::array<RecordProbe, kProtectedFilePaths.size()> g_files{};
std::string g_last_inventory_root;
int g_inventory_calls = 0;
int g_task_constructions = 0;
int g_task_initializations = 0;
int g_task_marks = 0;
std::string g_task_root;
std::string g_task_id;
std::uint32_t g_task_revision = 0U;
std::time_t g_task_run_at = 0;
std::uint32_t g_task_run_day = 0U;
TaskStoreCode g_initialize_result = TaskStoreCode::Ok;
TaskStoreCode g_mark_result = TaskStoreCode::Ok;

}  // namespace

myai::Status PosixAtomicAlbumStore::markCurrent(
    const std::string& asset_id) {
  ++mark_calls;
  marked_asset = asset_id;
  return status;
}

EspStorageMountOwner::EspStorageMountOwner(
    std::string task, std::string internal, std::string removable)
    : task_root(std::move(task)), internal_root(std::move(internal)),
      removable_root(std::move(removable)) {}

const char* EspStorageMountOwner::taskRoot() const {
  return task_root.empty() ? nullptr : task_root.c_str();
}

const char* EspStorageMountOwner::internalRoot() const {
  return internal_root.empty() ? nullptr : internal_root.c_str();
}

const char* EspStorageMountOwner::removableRoot() const {
  return removable_root.empty() ? nullptr : removable_root.c_str();
}

PosixAtomicAlbumStore* EspStorageMountOwner::albumStoreForLegacyIdentity(
    const char* identity) {
  last_identity = identity ? identity : "";
  if (last_identity == "littlefs") return &internal_album;
  if (last_identity == "sd") return &removable_album;
  return nullptr;
}

TransactionAudit classifyTransaction(const TransactionProbe& input) {
  if (input.current == RecordProbe::Missing &&
      input.next == RecordProbe::Missing &&
      input.previous == RecordProbe::Missing) {
    return TransactionAudit::Empty;
  }
  if (input.current == RecordProbe::Valid &&
      input.next == RecordProbe::Valid &&
      input.previous == RecordProbe::Valid) {
    return TransactionAudit::Clean;
  }
  return TransactionAudit::RecoveryRequired;
}

PosixUpgradeInventory::PosixUpgradeInventory(std::string root)
    : root_(std::move(root)) {}

std::array<RecordProbe, kProtectedFilePaths.size()>
PosixUpgradeInventory::inspectFiles() const {
  ++g_inventory_calls;
  g_last_inventory_root = root_;
  return g_files;
}

PosixTaskStore::PosixTaskStore(std::string root) : root_(std::move(root)) {
  ++g_task_constructions;
  g_task_root = root_;
}

TaskStoreCode PosixTaskStore::initialize() {
  ++g_task_initializations;
  return g_initialize_result;
}

TaskStoreCode PosixTaskStore::markRun(
    const std::string& id, std::uint32_t revision,
    std::time_t run_at, std::uint32_t local_day) {
  ++g_task_marks;
  g_task_id = id;
  g_task_revision = revision;
  g_task_run_at = run_at;
  g_task_run_day = local_day;
  return g_mark_result;
}

}  // namespace storage
}  // namespace inkloop

static std::string path(const std::string& root, const char* leaf) {
  return root + leaf;
}

static bool exists(const std::string& selected) {
  struct stat info {};
  return ::lstat(selected.c_str(), &info) == 0;
}

static void writeExact(const std::string& selected,
                       const std::string& bytes) {
  std::FILE* file = std::fopen(selected.c_str(), "wb");
  assert(file);
  assert(std::fwrite(bytes.data(), 1U, bytes.size(), file) == bytes.size());
  assert(std::fclose(file) == 0);
}

static std::string journal(unsigned stage, bool task, char asset,
                           const std::string& backend = "sd") {
  const std::string id(64, asset);
  const std::string previous(64, 'b');
  std::string value =
      "{\"schema\":1,\"stage\":" + std::to_string(stage) +
      ",\"backend\":\"" + backend +
      "\",\"assetId\":\"" + id +
      "\",\"assetPath\":\"/inkloop-album/" + id +
      ".png\",\"previousCurrent\":\"" + previous +
      "\",\"operation\":\"" + (task ? "task" : "page") +
      "\",\"bytes\":363024,\"landscape\":false,\"page\":2,"
      "\"hasTask\":" + (task ? "true" : "false") +
      ",\"runAt\":" + (task ? "1787440000" : "0") +
      ",\"runDay\":2026234";
  if (task) {
    value += ",\"taskId\":\"dtask-native\",\"taskRevision\":9,"
        "\"taskFrameUrl\":\"https://example.invalid/frame\","
        "\"taskFrameHash\":\"" + id + "\"";
  }
  return value + "}";
}

static void resetDependencies() {
  g_files.fill(RecordProbe::Valid);
  g_last_inventory_root.clear();
  g_inventory_calls = 0;
  g_task_constructions = 0;
  g_task_initializations = 0;
  g_task_marks = 0;
  g_task_root.clear();
  g_task_id.clear();
  g_task_revision = 0U;
  g_task_run_at = 0;
  g_task_run_day = 0U;
  g_initialize_result = TaskStoreCode::Ok;
  g_mark_result = TaskStoreCode::Ok;
}

static void nativePathAndDeletionMatrix(EspStorageMountOwner& owner) {
  EspLegacyDisplayRecovery adapter(owner);
  const std::string current = path(owner.task_root, "/display-txn.json");
  const std::string next = path(owner.task_root, "/display-txn.next");
  const std::string previous = path(owner.task_root, "/display-txn.prev");

  writeExact(next, journal(1U, true, 'c'));
  writeExact(previous, journal(2U, false, 'd'));
  LegacyDisplayRecoverySnapshot snapshot;
  assert(adapter.inspect(snapshot) ==
         LegacyDisplayRecoveryProbe::Recoverable);
  assert(snapshot.selected_slot == LegacyDisplayRecordSlot::Next);
  assert(snapshot.journal.asset_id == std::string(64, 'c'));

  writeExact(current, journal(3U, false, 'e'));
  writeExact(next, "corrupt-candidate");
  assert(adapter.inspect(snapshot) ==
         LegacyDisplayRecoveryProbe::Recoverable);
  assert(snapshot.selected_slot == LegacyDisplayRecordSlot::Current);
  assert(snapshot.journal.asset_id == std::string(64, 'e'));

  assert(::unlink(next.c_str()) == 0);
  assert(::unlink(previous.c_str()) == 0);
  writeExact(next, journal(1U, false, 'f'));
  assert(::mkdir(previous.c_str(), 0700) == 0);
  assert(adapter.clearJournalSet() ==
         LegacyDisplayResolutionAdapterCode::IoError);
  assert(!exists(next));
  assert(exists(previous));
  // The current record must still exist when deleting previous fails.
  assert(exists(current));

  assert(::rmdir(previous.c_str()) == 0);
  assert(adapter.clearJournalSet() ==
         LegacyDisplayResolutionAdapterCode::Ok);
  assert(!exists(current) && !exists(next) && !exists(previous));
}

static LegacyDisplayJournal resolutionJournal(const std::string& backend) {
  LegacyDisplayJournal output;
  output.backend = backend;
  output.asset_id = std::string(64, 'a');
  output.has_task = true;
  output.task_id = "dtask-native";
  output.task_revision = 9U;
  output.run_at = 1787440000U;
  output.run_day = 2026234U;
  return output;
}

static void backendAndCleanGateMatrix(EspStorageMountOwner& owner) {
  EspLegacyDisplayRecovery adapter(owner);
  resetDependencies();
  LegacyDisplayJournal selected = resolutionJournal("sd");
  assert(adapter.applyTargetCurrent(selected) ==
         LegacyDisplayResolutionAdapterCode::Ok);
  assert(g_last_inventory_root == owner.removable_root);
  assert(owner.last_identity == "sd");
  assert(owner.removable_album.mark_calls == 1);
  assert(owner.removable_album.marked_asset == selected.asset_id);
  assert(owner.internal_album.mark_calls == 0);

  selected.backend = "littlefs";
  assert(adapter.applyTargetCurrent(selected) ==
         LegacyDisplayResolutionAdapterCode::Ok);
  assert(g_last_inventory_root == owner.internal_root);
  assert(owner.last_identity == "littlefs");
  assert(owner.internal_album.mark_calls == 1);

  selected.backend = "unknown";
  assert(adapter.applyTargetCurrent(selected) ==
         LegacyDisplayResolutionAdapterCode::Unavailable);
  assert(owner.internal_album.mark_calls == 1 &&
         owner.removable_album.mark_calls == 1);

  selected.backend = "sd";
  g_files.fill(RecordProbe::Missing);
  assert(adapter.applyTargetCurrent(selected) ==
         LegacyDisplayResolutionAdapterCode::Conflict);
  assert(owner.removable_album.mark_calls == 1);
  g_files.fill(RecordProbe::Valid);
  g_files[7] = RecordProbe::Invalid;
  assert(adapter.applyTargetCurrent(selected) ==
         LegacyDisplayResolutionAdapterCode::Conflict);
  assert(owner.removable_album.mark_calls == 1);

  resetDependencies();
  selected.has_task = false;
  assert(adapter.acknowledgeTask(selected) ==
         LegacyDisplayResolutionAdapterCode::Ok);
  assert(g_inventory_calls == 0 && g_task_constructions == 0);

  selected.has_task = true;
  g_files.fill(RecordProbe::Missing);
  assert(adapter.acknowledgeTask(selected) ==
         LegacyDisplayResolutionAdapterCode::Conflict);
  assert(g_task_constructions == 0);
  g_files.fill(RecordProbe::Valid);
  g_files[1] = RecordProbe::Invalid;
  assert(adapter.acknowledgeTask(selected) ==
         LegacyDisplayResolutionAdapterCode::Conflict);
  assert(g_task_constructions == 0);

  g_files.fill(RecordProbe::Valid);
  assert(adapter.acknowledgeTask(selected) ==
         LegacyDisplayResolutionAdapterCode::Ok);
  assert(g_last_inventory_root == owner.task_root);
  assert(g_task_constructions == 1 && g_task_initializations == 1 &&
         g_task_marks == 1);
  assert(g_task_root == owner.task_root);
  assert(g_task_id == selected.task_id);
  assert(g_task_revision == selected.task_revision);
  assert(g_task_run_at == static_cast<std::time_t>(selected.run_at));
  assert(g_task_run_day == selected.run_day);
}

int main(int argc, char** argv) {
  assert(argc == 4);
  EspStorageMountOwner owner(argv[1], argv[2], argv[3]);
  nativePathAndDeletionMatrix(owner);
  backendAndCleanGateMatrix(owner);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-native-display-"));
  try {
    const stubs = join(scratch, "stubs/inkloop/storage");
    const taskRoot = join(scratch, "tasks");
    const internalRoot = join(scratch, "internal");
    const removableRoot = join(scratch, "sd");
    mkdirSync(stubs, { recursive: true });
    mkdirSync(taskRoot);
    mkdirSync(internalRoot);
    mkdirSync(removableRoot);
    writeFileSync(join(stubs, "esp_legacy_display_recovery.hpp"), espHeaderStub);
    writeFileSync(join(stubs, "posix_upgrade_inventory.hpp"), inventoryHeaderStub);
    writeFileSync(join(stubs, "posix_task_store.hpp"), taskHeaderStub);
    const source = join(scratch, "native-display.cpp");
    const cObject = join(scratch, "cJSON.o");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const sanitizer = sanitized
      ? ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
      : [];
    execFileSync("cc", [
      "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-Wno-deprecated-declarations", ...sanitizer,
      "-I", cjson, "-c", join(cjson, "cJSON.c"), "-o", cObject,
    ], { stdio: "pipe" });
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      ...sanitizer, "-I", join(scratch, "stubs"),
      "-I", join(storage, "include"), "-I", cjson,
      source,
      join(storage, "legacy_display_recovery.cpp"),
      join(storage, "esp_legacy_display_recovery.cpp"),
      cObject, "-lm", "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [taskRoot, internalRoot, removableRoot], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("native legacy display adapter passes strict C++17 path and clean-gate matrix", () => {
  buildAndRun(false);
});

test("native legacy display adapter passes ASan/UBSan acceptance matrix", () => {
  buildAndRun(true);
});

test("native source is exact-path, current-last and dependency isolated", () => {
  const portable = readFileSync(
    join(storage, "legacy_display_recovery.cpp"), "utf8",
  );
  const native = readFileSync(
    join(storage, "esp_legacy_display_recovery.cpp"), "utf8",
  );
  assert.doesNotMatch(portable, /const_cast/);
  assert.match(
    native,
    /kJournalPaths\{\{\s*"\/display-txn\.json",\s*"\/display-txn\.next",\s*"\/display-txn\.prev"\}\}/,
  );
  for (const path of [
    "/display-txn.json", "/display-txn.next", "/display-txn.prev",
  ]) assert.equal(native.split(`"${path}"`).length - 1, 1, path);
  assert.match(native, /const std::size_t order\[\]\s*=\s*\{1U, 2U, 0U\}/);
  assert.match(
    native,
    /journal\.backend == "sd"\s*\? storage_\.removableRoot\(\)\s*:\s*storage_\.internalRoot\(\)/,
  );
  assert.match(
    native,
    /albumStoreForLegacyIdentity\(journal\.backend\.c_str\(\)\)/,
  );
  assert.match(
    native,
    /classifyTransaction\(transaction\(files, 6U\)\)\s*!=\s*TransactionAudit::Clean/,
  );
  assert.match(
    native,
    /classifyTransaction\(transaction\(files, 0U\)\)\s*!=\s*TransactionAudit::Clean/,
  );
  assert.equal(native.match(/::unlink\(/g)?.length, 1);
  assert.doesNotMatch(native, /::(?:remove|unlinkat|rename|rmdir)\s*\(/);
  assert.doesNotMatch(native, /\b(?:format|erase|truncate|fwrite|creat)\s*\(/i);
  assert.doesNotMatch(native, /fopen\([^\n]*"(?:w|a|r\+)b?"/i);
  assert.doesNotMatch(
    native,
    /#include\s*[<"][^>"]*(?:product|voice|inkloop\/display|cloud|http|wifi|esp_http)[^>"]*[>"]/i,
  );
  assert.doesNotMatch(native, /https?:\/\/|bearer|token|credential|reboot/i);
  const cmake = readFileSync(join(storage, "CMakeLists.txt"), "utf8");
  assert.equal(cmake.split('"legacy_display_recovery.cpp"').length - 1, 1);
  assert.equal(
    cmake.split('"esp_legacy_display_recovery.cpp"').length - 1,
    1,
  );
});
