import { execFileSync } from "node:child_process";
import { mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
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
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "inkloop/storage/posix_task_store.hpp"

using namespace inkloop::storage;

InkloopTaskRecord task(const std::string& id, uint32_t revision,
                       const std::string& mode = "once") {
  InkloopTaskRecord value;
  value.id = id;
  value.title = "task " + id;
  value.schedule_mode = mode;
  value.custom_minutes = 30;
  value.daily_time = "08:00";
  value.revision = revision;
  value.frame_url = "https://inkloop.mess.host/api/devices?mode=frame&taskId=" + id;
  value.frame_hash = std::string(64, id == "one" ? 'a' : 'b');
  value.render_strategy = "solid-clean";
  return value;
}

std::string read(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void write(const std::string& path, const std::string& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << bytes;
  assert(output.good());
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string root = argv[1];
  PosixTaskStore store(root);
  assert(store.pathsValid());
  assert(store.initialize() == TaskStoreCode::Ok);

  std::vector<InkloopTaskRecord> loaded;
  assert(store.load(loaded) == TaskStoreCode::Ok && loaded.empty());

  // Revision zero is an intentional Arduino-upgrade compatibility case.
  auto zero = task("one", 0);
  auto hourly = task("two", 9, "hourly");
  assert(store.replace({zero, hourly}) == TaskStoreCode::Ok);
  assert(store.load(loaded) == TaskStoreCode::Ok && loaded.size() == 2);

  std::tm local{};
  local.tm_year = 126;
  local.tm_yday = 233;
  local.tm_hour = 9;
  local.tm_min = 5;
  const std::time_t now = 1787360700;
  InkloopTaskRecord due;
  assert(store.firstDue(now, local, due) == TaskStoreCode::Ok);
  assert(due.id == "one" && due.revision == 0);
  assert(store.markRun("one", 0, now, PosixTaskStore::localDayStamp(local)) ==
         TaskStoreCode::Ok);

  // Exact id+revision replacement preserves acknowledgements; a new revision
  // becomes due and never inherits the old acknowledgement.
  assert(store.replace({zero, hourly}) == TaskStoreCode::Ok);
  assert(store.load(loaded) == TaskStoreCode::Ok);
  assert(loaded[0].last_run == static_cast<uint32_t>(now));
  zero.revision = 1;
  assert(store.replace({zero, hourly}) == TaskStoreCode::Ok);
  assert(store.load(loaded) == TaskStoreCode::Ok);
  assert(loaded[0].last_run == 0 && loaded[0].last_day == 0);

  // Daily tasks missed while powered off run once after the scheduled time,
  // then wait until the next local day.
  auto daily = task("one", 2, "daily");
  daily.last_run = static_cast<uint32_t>(now - 86400);
  daily.last_day = PosixTaskStore::localDayStamp(local) - 1;
  assert(store.replace({daily}) == TaskStoreCode::Ok);
  assert(store.load(loaded) == TaskStoreCode::Ok);
  loaded[0].last_run = static_cast<uint32_t>(now - 86400);
  loaded[0].last_day = PosixTaskStore::localDayStamp(local) - 1;
  // replace() intentionally resets server-supplied acknowledgement fields, so
  // persist this equivalent state through the public acknowledgement path.
  assert(store.markRun("one", 2, now - 86400,
                       PosixTaskStore::localDayStamp(local) - 1) ==
         TaskStoreCode::Ok);
  assert(store.firstDue(now, local, due) == TaskStoreCode::Ok && due.id == "one");
  assert(store.markRun("one", 2, now, PosixTaskStore::localDayStamp(local)) ==
         TaskStoreCode::Ok);
  assert(store.firstDue(now, local, due) == TaskStoreCode::Ok && due.id.empty());

  // Full replacement implements server-side deletion.
  assert(store.replace({}) == TaskStoreCode::Ok);
  assert(store.load(loaded) == TaskStoreCode::Ok && loaded.empty());

  // Corrupt current plus valid .next recovers without inventing records.
  assert(store.replace({hourly}) == TaskStoreCode::Ok);
  const std::string valid = read(root + "/tasks.json");
  write(root + "/tasks.next", valid);
  write(root + "/tasks.json", "corrupt");
  assert(store.load(loaded) == TaskStoreCode::Ok && loaded.size() == 1);
  assert(loaded[0].id == "two");

  InkloopTaskRecord invalid = hourly;
  invalid.frame_hash = std::string(64, 'G');
  assert(store.replace({invalid}) == TaskStoreCode::InvalidRecord);
  assert(store.replace({hourly, hourly}) == TaskStoreCode::InvalidRecord);
  assert(store.markRun("missing", 1, now, 1) == TaskStoreCode::InvalidRecord);

  PosixTaskStore bad("relative");
  assert(!bad.pathsValid());
  assert(bad.initialize() == TaskStoreCode::InvalidArgument);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-task-store-"));
  try {
    const source = join(scratch, "task_store.cpp");
    const cObject = join(scratch, "cJSON.o");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    const data = join(scratch, "data");
    writeFileSync(source, harness);
    mkdirSync(data);
    const sanitizer = sanitized
      ? ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
      : [];
    execFileSync("cc", [
      "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
      // Upstream cJSON still uses sprintf; isolate the macOS SDK deprecation
      // warning to the vendored C object while keeping our code at -Werror.
      "-Wno-deprecated-declarations",
      ...sanitizer,
      "-I", cjson,
      "-c", join(cjson, "cJSON.c"),
      "-o", cObject,
    ], { stdio: "pipe" });
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      ...sanitizer,
      "-I", join(storage, "include"),
      "-I", cjson,
      source,
      join(storage, "posix_task_store.cpp"),
      join(storage, "album_index.cpp"),
      cObject,
      "-lm",
      "-o", binary,
    ], { stdio: "pipe" });
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

test("native task store preserves schedules and atomic replacements", () => {
  buildAndRun(false);
});

test("native task store rejects corruption under ASan/UBSan", () => {
  buildAndRun(true);
});
