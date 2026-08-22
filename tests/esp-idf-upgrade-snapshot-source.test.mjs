import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const storage = join(repo, "firmware/inkloop-idf/components/inkloop_storage");
const source = readFileSync(
  join(storage, "esp_upgrade_snapshot_source.cpp"), "utf8",
);
const header = readFileSync(join(
  storage, "include/inkloop/storage/esp_upgrade_snapshot_source.hpp",
), "utf8");
const cmake = readFileSync(join(storage, "CMakeLists.txt"), "utf8");

test("native upgrade source refreshes semantic probes for every collector pass", () => {
  assert.match(header, /public IUpgradeSnapshotSource/);
  assert.match(header, /IUpgradeSnapshotMetadataProvider/);
  assert.match(source, /pass_ready_ = false/);
  assert.match(source, /nvs_classifications_ = nvs_\.inspect\(\)/);
  assert.match(source, /file_classifications_ = paths_\.inspectFiles\(\)/);
  assert.match(source, /if \(!pass_ready_[\s\S]*upgradeRecordIdValid/);
});

test("NVS stream is canonical, bounded and read-only", () => {
  assert.match(source, /kNvsStreamMagic/);
  assert.match(source, /nvs_entry_find_in_handle/);
  assert.match(source, /std::sort\(entries\.begin\(\), entries\.end\(\)/);
  assert.match(source, /NVS_READONLY/);
  assert.match(source, /kMaximumNvsKeys = 640U/);
  assert.match(source, /emitU32[\s\S]*value\.size\(\)/);
  assert.match(source, /std::fill\(value\.begin\(\), value\.end\(\), 0U\)/);
  assert.doesNotMatch(
    source,
    /NVS_READWRITE|nvs_set_|nvs_erase|nvs_commit|printf|ESP_LOG|secret|token|password/,
  );
});

test("protected files stream in fixed chunks and reject in-flight changes", () => {
  assert.match(source, /kFileChunkBytes = 2048U/);
  assert.match(source, /std::fread/);
  assert.match(source, /trailing == EOF/);
  assert.match(source, /after\.st_size == before\.st_size/);
  assert.match(source, /after\.st_ino == before\.st_ino/);
  assert.match(source, /after\.st_mtime == before\.st_mtime/);
  assert.doesNotMatch(
    source,
    /"wb"|"ab"|rename\(|unlink\(|remove\(|format|erase/,
  );
  assert.match(cmake, /esp_upgrade_snapshot_source\.cpp/);
});
