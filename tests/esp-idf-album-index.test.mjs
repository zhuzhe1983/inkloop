import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_storage",
);

const harness = String.raw`
#include <cassert>
#include <string>

#include "inkloop/storage/album_index.hpp"

using namespace inkloop::storage;

AlbumIndexAsset asset(char digit, size_t bytes = 100) {
  AlbumIndexAsset value;
  value.id = std::string(64, digit);
  value.content_sha256 = value.id;
  value.path = "/inkloop-album/" + value.id + ".png";
  value.bytes = bytes;
  value.landscape = false;
  value.created = 123;
  value.task_id = "aigc:提示词";
  value.render_strategy = "solid-clean";
  return value;
}

int main() {
  AlbumIndex input;
  input.assets.push_back(asset('a'));
  input.assets.push_back(asset('b', 200));
  input.current = input.assets[1].id;
  input.current_render_strategy = input.assets[1].render_strategy;
  std::string encoded;
  assert(encodeAlbumIndex(input, encoded) == AlbumIndexCode::Ok);
  AlbumIndex parsed;
  assert(parseAlbumIndex(encoded, parsed) == AlbumIndexCode::Ok);
  assert(parsed.current == input.current && parsed.assets.size() == 2);
  assert(parsed.current_render_strategy == "solid-clean");
  assert(parsed.assets[0].content_sha256 == parsed.assets[0].id);
  assert(parsed.assets[0].task_id == "aigc:提示词");
  assert(parsed.assets[0].render_strategy == "solid-clean");

  // Existing Arduino indexes may omit renderStrategy; the native reader keeps
  // the compatibility default and accepts arbitrary field order/whitespace.
  const std::string id(64, 'c');
  const std::string legacy =
      " { \"assets\" : [ {\"taskId\":\"upload:test\","
      "\"created\":0,\"landscape\":true,\"bytes\":45,"
      "\"path\":\"/inkloop-album/" + id + ".png\","
      "\"id\":\"" + id + "\"}],\"current\":\"\",\"schema\":1 } ";
  assert(parseAlbumIndex(legacy, parsed) == AlbumIndexCode::Ok);
  assert(parsed.assets.size() == 1 && parsed.assets[0].landscape &&
         parsed.assets[0].render_strategy == "official-quality" &&
         parsed.assets[0].content_sha256 == id &&
         parsed.current_render_strategy.empty());

  const std::string valid_asset =
      "{\"id\":\"" + id + "\",\"path\":\"/inkloop-album/" + id +
      ".png\",\"bytes\":45,\"landscape\":false,\"created\":0,"
      "\"taskId\":\"x\"}";
  const std::string bad[] = {
      "", "{}", "{\"schema\":2,\"current\":\"\",\"assets\":[]}",
      "{\"schema\":1,\"current\":\"x\",\"assets\":[]}",
      "{\"schema\":1,\"current\":\"\",\"assets\":[" + valid_asset +
          "," + valid_asset + "]}",
      "{\"schema\":1,\"current\":\"\",\"assets\":[{\"id\":\"" +
          id + "\",\"path\":\"/outside.png\",\"bytes\":45,"
          "\"landscape\":false,\"created\":0,\"taskId\":\"x\"}]}",
      "{\"schema\":1,\"current\":\"\",\"assets\":[{\"id\":\"" +
          id + "\",\"path\":\"/inkloop-album/" + id +
          ".png\",\"bytes\":0,\"landscape\":false,\"created\":0,"
          "\"taskId\":\"x\"}]}",
      "{\"schema\":1,\"current\":\"\",\"assets\":[{\"id\":\"" +
          id + "\",\"path\":\"/inkloop-album/" + id +
          ".png\",\"bytes\":45,\"landscape\":false,\"created\":0,"
          "\"taskId\":false}]}",
      encoded + "x",
  };
  for (const std::string& value : bad)
    assert(parseAlbumIndex(value, parsed) != AlbumIndexCode::Ok);

  AlbumIndex invalid = input;
  invalid.assets[0].render_strategy = "mystery";
  assert(encodeAlbumIndex(invalid, encoded) == AlbumIndexCode::InvalidAsset);
  invalid = input;
  invalid.current = std::string(64, 'f');
  assert(encodeAlbumIndex(invalid, encoded) == AlbumIndexCode::InvalidCurrent);
  invalid = input;
  invalid.current_render_strategy = "mystery";
  assert(encodeAlbumIndex(invalid, encoded) == AlbumIndexCode::InvalidCurrent);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-album-index-"));
  try {
    const source = join(scratch, "index.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I",
      join(component, "include"),
      source,
      join(component, "album_index.cpp"),
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

test("album index codec preserves Arduino schema under strict C++17", () => {
  buildAndRun(false);
});

test("album index codec rejects adversarial records under ASan/UBSan", () => {
  buildAndRun(true);
});
