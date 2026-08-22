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
#include <filesystem>
#include <fstream>
#include <string>

#include "inkloop/storage/posix_upgrade_inventory.hpp"

using namespace inkloop::storage;

static void put(const std::filesystem::path& path, const std::string& data) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  assert(output.good());
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  output.close();
  assert(output.good());
}

static std::string get(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

static std::string assetId() { return std::string(64, 'a'); }
static std::string album() {
  const std::string id = assetId();
  return "{\"schema\":1,\"current\":\"" + id +
      "\",\"assets\":[{\"id\":\"" + id +
      "\",\"path\":\"/inkloop-album/" + id +
      ".png\",\"bytes\":45,\"landscape\":false,\"created\":1,"
      "\"taskId\":\"\",\"renderStrategy\":\"official-quality\"}]}";
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::filesystem::path root(argv[1]);
  std::filesystem::create_directories(root);
  const std::array<RecordProbe, kProtectedNvsNamespaces.size()> empty{};
  PosixUpgradeInventory inventory(root.string());
  assert(inventory.pathsValid());
  UpgradeAuditInput input = inventory.inspect(empty);
  assert(auditUpgrade(input).result == UpgradeAuditResult::Fresh);
  auto files = inventory.inspectFiles();
  for (RecordProbe probe : files) assert(probe == RecordProbe::Missing);

  const std::string tasks =
      "[{\"id\":\"one\",\"title\":\"Old task\","
      "\"scheduleMode\":\"once\",\"customMinutes\":30,"
      "\"dailyTime\":\"08:00\",\"revision\":1,"
      "\"frameUrl\":\"https://inkloop.mess.host/frame/one\","
      "\"frameHash\":\"" + std::string(64, 'b') + "\","
      "\"renderStrategy\":\"official-quality\","
      "\"lastRun\":0,\"lastDay\":0}]";
  const std::string chat =
      "{\"v\":1,\"sequence\":1,\"role\":\"user\","
      "\"kind\":\"asr.final\",\"text\":\"你好\"}\n";
  put(root / "tasks.json", tasks);
  put(root / "inkloop-album/index.json", album());
  put(root / "inkloop/myai-chat.txt", chat);
  input = inventory.inspect(empty);
  UpgradeAuditReport report = auditUpgrade(input);
  assert(report.result == UpgradeAuditResult::Compatible);
  files = inventory.inspectFiles();
  assert(files[0] == RecordProbe::Valid);
  assert(files[6] == RecordProbe::Valid);
  assert(files[9] == RecordProbe::Valid);
  assert(get(root / "tasks.json") == tasks);
  assert(get(root / "inkloop-album/index.json") == album());
  assert(get(root / "inkloop/myai-chat.txt") == chat);

  // A generic JSON array is not enough: the native runtime must be able to
  // consume every task before the audit allows product writers to start.
  put(root / "tasks.json", "[{\"id\":\"shape-only\"}]");
  input = inventory.inspect(empty);
  assert(input.tasks.current == RecordProbe::Invalid);
  assert(auditUpgrade(input).result == UpgradeAuditResult::Ambiguous);
  put(root / "tasks.json", tasks);

  put(root / "tasks.json", "[{bad]");
  put(root / "tasks.next", tasks);
  input = inventory.inspect(empty);
  report = auditUpgrade(input);
  assert(report.tasks == TransactionAudit::RecoveryRequired);
  assert(report.result == UpgradeAuditResult::RecoveryRequired);
  files = inventory.inspectFiles();
  assert(files[0] == RecordProbe::Invalid);
  assert(files[1] == RecordProbe::Valid);
  assert(get(root / "tasks.json") == "[{bad]");
  assert(get(root / "tasks.next") == tasks);

  put(root / "display-txn.json", "{\"schema\":1,\"stage\":1}");
  input = inventory.inspect(empty);
  report = auditUpgrade(input);
  assert(input.display_transaction == RecordProbe::Unvalidated);
  assert(inventory.inspectFiles()[3] == RecordProbe::Unvalidated);
  assert(report.result == UpgradeAuditResult::DisplayResolutionRequired);
  assert(get(root / "display-txn.json") == "{\"schema\":1,\"stage\":1}");

  put(root / "display-txn.json", std::string(17U * 1024U, 'x'));
  input = inventory.inspect(empty);
  assert(input.display_transaction == RecordProbe::Invalid);
  assert(auditUpgrade(input).result == UpgradeAuditResult::DisplayResolutionRequired);

  std::filesystem::remove(root / "display-txn.json");
  put(root / "tasks.next", tasks);
  put(root / "tasks.prev", tasks);
  input = inventory.inspect(empty);
  assert(auditUpgrade(input).result == UpgradeAuditResult::Ambiguous);

  assert(!PosixUpgradeInventory("relative").pathsValid());
  assert(!PosixUpgradeInventory("/bad/../root").pathsValid());
  return 0;
}
`;

function run(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-upgrade-inventory-"));
  try {
    const source = join(scratch, "inventory.cpp");
    const cObject = join(scratch, "cJSON.o");
    const binary = join(scratch, sanitized ? "asan" : "strict");
    const fsroot = join(scratch, "littlefs");
    writeFileSync(source, harness);
    const sanitizer = sanitized
      ? ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
      : [];
    execFileSync("cc", [
      "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-Wno-deprecated-declarations", ...sanitizer,
      "-I", cjson, "-c", join(cjson, "cJSON.c"), "-o", cObject,
    ], { stdio: "pipe" });
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      ...sanitizer,
      "-I", join(storage, "include"), "-I", cjson, source,
      join(storage, "posix_upgrade_inventory.cpp"),
      join(storage, "upgrade_audit.cpp"), join(storage, "album_index.cpp"),
      join(storage, "local_chat_log.cpp"), join(storage, "posix_chat_store.cpp"),
      join(storage, "posix_task_store.cpp"), cObject, "-lm",
      "-o", binary,
    ];
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [fsroot], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("read-only upgrade inventory preserves legacy files under strict C++17", () => {
  run(false);
});

test("read-only upgrade inventory fails closed under ASan/UBSan", () => {
  run(true);
});

test("inventory source contains no mutation or format primitives", () => {
  const source = readFileSync(join(storage, "posix_upgrade_inventory.cpp"), "utf8");
  assert.doesNotMatch(source, /remove_all|rename\(|unlink\(|format|erase/);
});
