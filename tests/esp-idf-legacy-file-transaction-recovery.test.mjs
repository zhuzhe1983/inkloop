import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const storage = join(repo, "firmware/inkloop-idf/components/inkloop_storage");
const cjson = join(
  repo,
  "firmware/inkloop-idf/managed_components/espressif__cjson/cJSON",
);

const harness = String.raw`
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "inkloop/storage/legacy_file_transaction_recovery.hpp"

using namespace inkloop::storage;

static constexpr LegacyFileTransactionTarget kTasks{
    LegacyFileTransactionDomain::Tasks,
    LegacyFileTransactionBackend::TaskRoot};
static constexpr LegacyFileTransactionTarget kInternalAlbum{
    LegacyFileTransactionDomain::Album,
    LegacyFileTransactionBackend::Internal};
static constexpr LegacyFileTransactionTarget kRemovableAlbum{
    LegacyFileTransactionDomain::Album,
    LegacyFileTransactionBackend::Removable};

static std::string taskManifest(char id) {
  return "[{\"id\":\"dtask-" + std::string(1U, id) +
      "\",\"title\":\"Task " + std::string(1U, id) +
      "\",\"scheduleMode\":\"once\",\"customMinutes\":30,"
      "\"dailyTime\":\"08:00\",\"revision\":1,"
      "\"frameUrl\":\"https://example.invalid/frame\","
      "\"frameHash\":\"" + std::string(64U, id) +
      "\",\"renderStrategy\":\"official-quality\","
      "\"lastRun\":0,\"lastDay\":0}]";
}

static std::string albumManifest(char id) {
  const std::string hash(64U, id);
  return "{\"schema\":1,\"current\":\"" + hash +
      "\",\"currentRenderStrategy\":\"official-quality\","
      "\"assets\":[{\"id\":\"" + hash +
      "\",\"path\":\"/inkloop-album/" + hash +
      ".png\",\"contentSha256\":\"" + hash +
      "\",\"bytes\":45,\"landscape\":false,\"created\":1,"
      "\"taskId\":\"\",\"renderStrategy\":\"official-quality\"}]}";
}

struct Roots {
  std::string internal;
  std::string removable;
};

static std::array<std::string, 3> paths(
    const Roots& roots, LegacyFileTransactionTarget target) {
  if (target.domain == LegacyFileTransactionDomain::Tasks) {
    return {{roots.internal + "/tasks.json",
             roots.internal + "/tasks.next",
             roots.internal + "/tasks.prev"}};
  }
  const std::string& root =
      target.backend == LegacyFileTransactionBackend::Internal
          ? roots.internal : roots.removable;
  const std::string directory = root + "/inkloop-album";
  return {{directory + "/index.json", directory + "/index.next",
           directory + "/index.prev"}};
}

static std::string manifest(LegacyFileTransactionTarget target, char id) {
  return target.domain == LegacyFileTransactionDomain::Tasks
      ? taskManifest(id) : albumManifest(id);
}

static void writeExact(const std::string& path, const std::string& bytes) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  assert(file);
  assert(std::fwrite(bytes.data(), 1U, bytes.size(), file) == bytes.size());
  assert(std::fflush(file) == 0);
  assert(::fsync(::fileno(file)) == 0);
  assert(std::fclose(file) == 0);
}

static bool exists(const std::string& path) {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0;
}

static void clearSlots(const Roots& roots,
                       LegacyFileTransactionTarget target) {
  for (const std::string& path : paths(roots, target)) {
    if (::unlink(path.c_str()) != 0) assert(errno == ENOENT);
  }
}

static void seedAll(const Roots& roots,
                    LegacyFileTransactionTarget target) {
  clearSlots(roots, target);
  const auto selected = paths(roots, target);
  writeExact(selected[0], manifest(target, 'a'));
  writeExact(selected[1], manifest(target, 'b'));
  writeExact(selected[2], manifest(target, 'c'));
}

static PosixLegacyFileTransactionRecovery adapter(const Roots& roots) {
  LegacyFileTransactionRecoveryConfig config;
  config.task_root = roots.internal;
  config.internal_root = roots.internal;
  config.removable_root = roots.removable;
  return PosixLegacyFileTransactionRecovery(std::move(config));
}

class CutObserver final : public ILegacyFileRecoveryCutObserver {
 public:
  explicit CutObserver(int fail_after = 0) : fail_after(fail_after) {}

  bool continueAfter(const LegacyFileRecoveryCutPoint& point) override {
    points.push_back(point);
    return fail_after == 0 ||
        static_cast<int>(points.size()) != fail_after;
  }

  int fail_after = 0;
  std::vector<LegacyFileRecoveryCutPoint> points;
};

static LegacyFileTransactionSlot findCandidate(
    const LegacyFileTransactionSnapshot& snapshot,
    const LegacyFileCandidateSummary& selected) {
  for (std::size_t at = 0U; at < snapshot.candidates.size(); ++at) {
    if (legacyFileCandidateEqual(snapshot.candidates[at], selected))
      return static_cast<LegacyFileTransactionSlot>(at);
  }
  assert(false && "selected candidate disappeared");
  return LegacyFileTransactionSlot::Current;
}

static bool containsCandidate(
    const LegacyFileTransactionSnapshot& snapshot,
    const LegacyFileCandidateSummary& selected) {
  for (const LegacyFileCandidateSummary& candidate : snapshot.candidates) {
    if (legacyFileCandidateEqual(candidate, selected)) return true;
  }
  return false;
}

static LegacyFileTransactionSnapshot inspectOk(
    PosixLegacyFileTransactionRecovery& recovery,
    LegacyFileTransactionTarget target) {
  LegacyFileTransactionSnapshot output;
  const LegacyFileTransactionProbe probe = recovery.inspect(target, output);
  assert(probe != LegacyFileTransactionProbe::IoError);
  assert(probe != LegacyFileTransactionProbe::InvalidTarget);
  return output;
}

static void assertSelectedCurrent(
    PosixLegacyFileTransactionRecovery& recovery,
    LegacyFileTransactionTarget target,
    const LegacyFileCandidateSummary& selected) {
  const LegacyFileTransactionSnapshot final = inspectOk(recovery, target);
  assert(legacyFileCandidateEqual(final.candidates[0], selected));
  assert(final.candidates[1].probe == LegacyFileCandidateProbe::Missing);
  assert(final.candidates[2].probe == LegacyFileCandidateProbe::Missing ||
         final.candidates[2].probe == LegacyFileCandidateProbe::Valid);
}

static void inspectionAndChoiceMatrix(const Roots& roots) {
  PosixLegacyFileTransactionRecovery recovery = adapter(roots);
  for (LegacyFileTransactionTarget target :
       {kTasks, kInternalAlbum, kRemovableAlbum}) {
    clearSlots(roots, target);
    LegacyFileTransactionSnapshot snapshot;
    assert(recovery.inspect(target, snapshot) ==
           LegacyFileTransactionProbe::Empty);
    const auto selected = paths(roots, target);
    writeExact(selected[0], manifest(target, 'a'));
    assert(recovery.inspect(target, snapshot) ==
           LegacyFileTransactionProbe::Recoverable);
    assert(snapshot.valid_candidates == 1U);
    assert(snapshot.candidates[0].probe ==
           LegacyFileCandidateProbe::Valid);
    assert(snapshot.candidates[0].digest_present);
    assert(snapshot.candidates[0].item_count_present);
    assert(snapshot.candidates[0].item_count == 1U);
    assert(snapshot.candidates[0].modified_time_present);
    assert(snapshot.candidates[0].modified_unix_seconds > 0U);
    writeExact(selected[1], manifest(target, 'b'));
    assert(recovery.inspect(target, snapshot) ==
           LegacyFileTransactionProbe::ChoiceRequired);
    assert(snapshot.valid_candidates == 2U);

    LegacyFileTransactionChoice missing{target,
                                         LegacyFileTransactionSlot::Previous};
    assert(recovery.resolve(snapshot, missing) ==
           LegacyFileTransactionResolveCode::SelectedUnavailable);
    const LegacyFileTransactionSnapshot unchanged = inspectOk(recovery, target);
    assert(legacyFileTransactionSnapshotEqual(snapshot, unchanged));

    writeExact(selected[2], "not-a-runtime-schema");
    assert(recovery.inspect(target, snapshot) ==
           LegacyFileTransactionProbe::ChoiceRequired);
    assert(snapshot.candidates[2].probe ==
           LegacyFileCandidateProbe::Invalid);
    assert(snapshot.candidates[2].digest_present);
  }

  LegacyFileTransactionSnapshot invalid;
  const LegacyFileTransactionTarget task_on_album{
      LegacyFileTransactionDomain::Tasks,
      LegacyFileTransactionBackend::Internal};
  assert(recovery.inspect(task_on_album, invalid) ==
         LegacyFileTransactionProbe::InvalidTarget);
  const LegacyFileTransactionTarget album_on_task{
      LegacyFileTransactionDomain::Album,
      LegacyFileTransactionBackend::TaskRoot};
  assert(recovery.inspect(album_on_task, invalid) ==
         LegacyFileTransactionProbe::InvalidTarget);

  LegacyFileTransactionRecoveryConfig partial_config;
  partial_config.task_root = roots.internal;
  partial_config.internal_root = roots.internal;
  PosixLegacyFileTransactionRecovery partial(std::move(partial_config));
  LegacyFileTransactionSnapshot partial_snapshot;
  assert(partial.inspect(kTasks, partial_snapshot) !=
         LegacyFileTransactionProbe::InvalidTarget);
  assert(partial.inspect(kInternalAlbum, partial_snapshot) !=
         LegacyFileTransactionProbe::InvalidTarget);
  assert(partial.inspect(kRemovableAlbum, partial_snapshot) ==
         LegacyFileTransactionProbe::InvalidTarget);

  seedAll(roots, kInternalAlbum);
  LegacyFileTransactionSnapshot expected = inspectOk(recovery, kInternalAlbum);
  LegacyFileTransactionChoice cross{kRemovableAlbum,
                                     LegacyFileTransactionSlot::Current};
  assert(recovery.resolve(expected, cross) ==
         LegacyFileTransactionResolveCode::CrossBackend);
  assert(legacyFileTransactionSnapshotEqual(
      expected, inspectOk(recovery, kInternalAlbum)));

  const auto internal_paths = paths(roots, kInternalAlbum);
  writeExact(internal_paths[1], albumManifest('d'));
  LegacyFileTransactionChoice stale{kInternalAlbum,
                                     LegacyFileTransactionSlot::Next};
  assert(recovery.resolve(expected, stale) ==
         LegacyFileTransactionResolveCode::SourceChanged);
  assert(exists(internal_paths[0]) && exists(internal_paths[1]) &&
         exists(internal_paths[2]));
}

static void runPowerCutScenario(
    const Roots& roots, LegacyFileTransactionTarget target,
    LegacyFileTransactionSlot selected_slot) {
  PosixLegacyFileTransactionRecovery recovery = adapter(roots);
  seedAll(roots, target);
  LegacyFileTransactionSnapshot initial = inspectOk(recovery, target);
  const std::size_t selected_at =
      legacyFileTransactionSlotIndex(selected_slot);
  const LegacyFileCandidateSummary selected = initial.candidates[selected_at];
  const LegacyFileCandidateSummary rollback =
      selected_slot == LegacyFileTransactionSlot::Current
          ? initial.candidates[2] : initial.candidates[0];
  LegacyFileTransactionChoice choice{target, selected_slot};
  CutObserver recording;
  assert(recovery.resolve(initial, choice, &recording) ==
         LegacyFileTransactionResolveCode::Ok);
  assert(!recording.points.empty());
  assertSelectedCurrent(recovery, target, selected);

  bool saw_file_fsync = false;
  bool saw_directory_fsync = false;
  bool saw_unlink = false;
  bool saw_rename = false;
  for (const LegacyFileRecoveryCutPoint& point : recording.points) {
    saw_file_fsync = saw_file_fsync ||
        point.operation == LegacyFileRecoveryCutOperation::FileFsync;
    saw_directory_fsync = saw_directory_fsync ||
        point.operation == LegacyFileRecoveryCutOperation::DirectoryFsync;
    saw_unlink = saw_unlink ||
        point.operation == LegacyFileRecoveryCutOperation::Unlink;
    saw_rename = saw_rename ||
        point.operation == LegacyFileRecoveryCutOperation::Rename;
  }
  assert(saw_file_fsync);
  if (selected_slot != LegacyFileTransactionSlot::Current) {
    assert(saw_directory_fsync && saw_unlink && saw_rename);
  }

  for (int cut = 1; cut <= static_cast<int>(recording.points.size()); ++cut) {
    seedAll(roots, target);
    initial = inspectOk(recovery, target);
    const LegacyFileCandidateSummary retained =
        initial.candidates[selected_at];
    CutObserver observer(cut);
    assert(recovery.resolve(initial, choice, &observer) ==
           LegacyFileTransactionResolveCode::PowerCutSimulated);
    assert(static_cast<int>(observer.points.size()) == cut);

    LegacyFileTransactionSnapshot after_cut = inspectOk(recovery, target);
    assert(containsCandidate(after_cut, retained));
    assert(containsCandidate(after_cut, rollback));
    const LegacyFileTransactionSlot retained_slot =
        findCandidate(after_cut, retained);
    if (!legacyFileTransactionSnapshotEqual(initial, after_cut)) {
      assert(recovery.resolve(initial, choice) ==
             LegacyFileTransactionResolveCode::SourceChanged);
      after_cut = inspectOk(recovery, target);
    }
    const LegacyFileTransactionChoice retry{target, retained_slot};
    assert(recovery.resolve(after_cut, retry) ==
           LegacyFileTransactionResolveCode::Ok);
    assertSelectedCurrent(recovery, target, retained);
  }
}

static void powerCutMatrix(const Roots& roots) {
  for (LegacyFileTransactionTarget target : {kTasks, kInternalAlbum}) {
    for (LegacyFileTransactionSlot slot : {
             LegacyFileTransactionSlot::Current,
             LegacyFileTransactionSlot::Next,
             LegacyFileTransactionSlot::Previous}) {
      runPowerCutScenario(roots, target, slot);
    }
  }
}

static void operationFaultMatrix(const Roots& roots) {
  for (LegacyFileTransactionTarget target : {kTasks, kInternalAlbum}) {
    PosixLegacyFileTransactionRecovery recovery = adapter(roots);
    seedAll(roots, target);
    const auto selected_paths = paths(roots, target);
    assert(::unlink(selected_paths[2].c_str()) == 0);
    assert(::mkdir(selected_paths[2].c_str(), 0700) == 0);
    LegacyFileTransactionSnapshot snapshot = inspectOk(recovery, target);
    const LegacyFileCandidateSummary selected = snapshot.candidates[1];
    const LegacyFileTransactionChoice choice{
        target, LegacyFileTransactionSlot::Next};
    assert(recovery.resolve(snapshot, choice) ==
           LegacyFileTransactionResolveCode::IoError);
    // Failed cleanup cannot remove either current authority or selected next.
    LegacyFileTransactionSnapshot failed = inspectOk(recovery, target);
    assert(failed.candidates[0].probe == LegacyFileCandidateProbe::Valid);
    assert(legacyFileCandidateEqual(failed.candidates[1], selected));
    assert(::rmdir(selected_paths[2].c_str()) == 0);
    failed = inspectOk(recovery, target);
    assert(recovery.resolve(failed, choice) ==
           LegacyFileTransactionResolveCode::Ok);
    assertSelectedCurrent(recovery, target, selected);
  }
}

int main(int argc, char** argv) {
  assert(argc == 3);
  Roots roots{argv[1], argv[2]};
  inspectionAndChoiceMatrix(roots);
  powerCutMatrix(roots);
  operationFaultMatrix(roots);
  assert(std::string(legacyFileTransactionProbeName(
             LegacyFileTransactionProbe::ChoiceRequired)) ==
         "CHOICE_REQUIRED");
  assert(std::string(legacyFileCandidateProbeName(
             LegacyFileCandidateProbe::Invalid)) == "INVALID");
  assert(std::string(legacyFileTransactionResolveCodeName(
             LegacyFileTransactionResolveCode::SourceChanged)) ==
         "SOURCE_CHANGED");
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-file-recovery-"));
  try {
    const internal = join(scratch, "internal");
    const removable = join(scratch, "removable");
    execFileSync("mkdir", [internal, removable]);
    execFileSync("mkdir", [join(internal, "inkloop-album")]);
    execFileSync("mkdir", [join(removable, "inkloop-album")]);
    const source = join(scratch, "file-recovery.cpp");
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
      ...sanitizer, "-I", join(storage, "include"), "-I", cjson,
      source,
      join(storage, "legacy_file_transaction_recovery.cpp"),
      join(storage, "posix_task_store.cpp"),
      join(storage, "album_index.cpp"),
      join(storage, "sha256.cpp"),
      cObject, "-lm", "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [internal, removable], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("explicit task/album recovery passes strict C++17 real-POSIX matrix", () => {
  buildAndRun(false);
});

test("explicit task/album recovery passes ASan/UBSan power-cut matrix", () => {
  buildAndRun(true);
});

test("recovery source uses runtime parsers and only fixed transaction paths", () => {
  const source = readFileSync(
    join(storage, "legacy_file_transaction_recovery.cpp"), "utf8",
  );
  const header = readFileSync(
    join(
      storage,
      "include/inkloop/storage/legacy_file_transaction_recovery.hpp",
    ),
    "utf8",
  );
  assert.equal(source.split("PosixTaskStore::decodeManifest").length - 1, 1);
  assert.equal(source.split("parseAlbumIndex").length - 1, 1);
  assert.match(header, /item_count/);
  assert.match(header, /modified_unix_seconds/);
  for (const path of [
    "/tasks.json", "/tasks.next", "/tasks.prev",
    "/inkloop-album", "/index.json", "/index.next", "/index.prev",
  ]) assert.equal(source.split(`"${path}"`).length - 1, 1, path);
  assert.match(source, /::unlink\(paths\.slots\[at\]\.c_str\(\)\)/);
  assert.match(
    source,
    /::rename\(paths\.slots\[from\]\.c_str\(\), paths\.slots\[to\]\.c_str\(\)\)/,
  );
  assert.doesNotMatch(
    source + header,
    /remove_all|recursive|rmtree|nftw|fts_|glob\(|wordexp|format|erase/i,
  );
  assert.doesNotMatch(
    source,
    /#include\s*[<"][^>"]*(?:product|runtime|voice|inkloop\/display|cloud|http|wifi|portal)[^>"]*[>"]/i,
  );
  assert.doesNotMatch(
    source,
    /https?:\/\/|bearer|token|credential|reboot|asset\.part|\.png|removeAsset|clearAssets/i,
  );
  assert.doesNotMatch(
    header,
    /resolve\([^;]*(?:std::string|char\s*\*)/s,
  );
  const cmake = readFileSync(join(storage, "CMakeLists.txt"), "utf8");
  assert.equal(
    cmake.split('"legacy_file_transaction_recovery.cpp"').length - 1,
    1,
  );
});
