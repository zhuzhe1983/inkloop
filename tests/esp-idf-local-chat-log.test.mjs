import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import {
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
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
#include <fstream>
#include <iterator>
#include <string>

#include "inkloop/storage/local_chat_log.hpp"
#include "inkloop/storage/posix_chat_store.hpp"

using namespace inkloop::storage;

std::string bytes(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string root = argv[1];
  const std::string current = root + "/chat.jsonl";
  const std::string previous = root + "/chat.prev.jsonl";
  PosixChatLineStore files(current, previous);
  assert(files.pathsValid());
  LocalChatLog log(files);
  ChatRecovery recovery;
  assert(log.recover(recovery).ok());
  assert(recovery.next_sequence == 1 && recovery.valid_records == 0);

  assert(log.appendAsr("临时部分", false, "2026-08-22T05:00:00Z").code ==
         ChatLogCode::IgnoredPartial);
  assert(log.appendAsr("   ", true, "2026-08-22T05:00:00Z").code ==
         ChatLogCode::IgnoredEmpty);
  const char* artifacts[] = {
      "blank_audio", " BLANK AUDIO ", "[blank_audio]", "blank-audio"};
  for (const char* artifact : artifacts) {
    assert(log.appendAsr(artifact, true, "2026-08-22T05:00:00Z").code ==
           ChatLogCode::IgnoredBlankAudio);
  }
  assert(log.appendAsr("请画一张高对比海报", true,
                       "2026-08-22T05:00:01Z").ok());
  assert(log.appendAssistant("生成任务已经开始", false,
                             "2026-08-22T05:00:02Z").code ==
         ChatLogCode::IgnoredPartial);
  assert(log.appendAssistant("生成任务已经开始", true,
                             "2026-08-22T05:00:03Z").ok());
  assert(log.appendToolState("storage: ready", "2026-08-22T05:00:04Z").ok());
  assert(log.appendAigcState("aigc: downloading", "2026-08-22T05:00:05Z").ok());
  assert(log.appendToolState("quote=\" slash=\\ line\nnext", "").ok());

  const std::string raw = bytes(current);
  assert(raw.find("blank_audio") == std::string::npos);
  assert(raw.find("audio_base64") == std::string::npos);
  assert(raw.find("asr.final") != std::string::npos);
  assert(raw.find("assistant.final") != std::string::npos);
  assert(raw.find("aigc.state") != std::string::npos);

  ChatPage first;
  assert(log.readPage(0, 2, first).ok());
  assert(first.records.size() == 2 && first.has_more);
  assert(first.records[0].kind == ChatRecordKind::AsrFinal);
  assert(first.records[0].text == "请画一张高对比海报");
  assert(first.records[1].kind == ChatRecordKind::AssistantFinal);
  ChatPage second;
  assert(log.readPage(first.next_cursor, 32, second).ok());
  assert(second.records.size() == 3 && !second.has_more);
  assert(second.records[1].kind == ChatRecordKind::AigcState);
  assert(second.records[2].text == "quote=\" slash=\\ line\nnext");
  assert(log.readPage(0, 0, second).code == ChatLogCode::InvalidArgument);

  // A reboot recovers the same local source of truth and sequence.
  LocalChatLog rebooted(files);
  assert(rebooted.recover(recovery).ok());
  assert(recovery.valid_records == 5 && recovery.next_sequence == 6);
  assert(rebooted.appendAsr(std::string("ok") + char(0xff) + "尾", true,
                            "2026-08-22T05:00:06Z").ok());
  ChatPage after;
  assert(rebooted.readPage(5, 4, after).ok());
  assert(after.records.size() == 1 && after.records[0].text == "ok尾");

  // A torn final line is ignored and marked; valid durable lines survive.
  {
    std::ofstream output(current, std::ios::binary | std::ios::app);
    output << "{\"sequence\":7,\"role\":\"user\",\"text\":\"torn";
  }
  LocalChatLog torn(files);
  assert(torn.recover(recovery).ok());
  assert(recovery.corruption_observed && recovery.valid_records == 6);
  assert(recovery.next_sequence == 7);

  assert(torn.clear().ok());
  ChatPage empty;
  assert(torn.readPage(0, 8, empty).ok() && empty.records.empty());

  // Existing Arduino JSONL (without v/kind) remains readable. Escaped Unicode
  // is decoded locally; no server migration is required.
  {
    std::ofstream output(current, std::ios::binary);
    output << "{\"sequence\":40,\"time\":\"2026-08-21T00:00:00Z\","
              "\"role\":\"user\",\"text\":\"\\u4f60\\u597d\"}\n";
  }
  LocalChatLog legacy(files);
  assert(legacy.recover(recovery).ok());
  assert(recovery.valid_records == 1 && recovery.next_sequence == 41);
  assert(legacy.readPage(0, 8, empty).ok());
  assert(empty.records.size() == 1 && empty.records[0].text == "你好");

  // Historical blank_audio rows consume their old sequence but are silently
  // omitted rather than being misreported as storage corruption.
  {
    std::ofstream output(current, std::ios::binary | std::ios::app);
    output << "{\"sequence\":41,\"role\":\"user\","
              "\"text\":\"blank_audio\"}\n";
  }
  LocalChatLog legacy_artifact(files);
  assert(legacy_artifact.recover(recovery).ok());
  assert(!recovery.corruption_observed && recovery.valid_records == 1 &&
         recovery.next_sequence == 42);
  assert(legacy_artifact.readPage(0, 8, empty).ok());
  assert(empty.records.size() == 1 && !empty.corruption_observed);

  // Small caps rotate deterministically; only current+previous are retained.
  const std::string small_current = root + "/small.jsonl";
  const std::string small_previous = root + "/small.prev.jsonl";
  PosixChatLineStore small_files(small_current, small_previous);
  LocalChatLog small(small_files, 256);
  assert(small.recover(recovery).ok());
  for (int index = 0; index < 8; ++index) {
    assert(small.appendToolState(std::string(70, char('a' + index)), "t").ok());
  }
  assert(small_files.rotatedHistoryPresent());
  assert(bytes(small_current).size() <= 256);
  assert(bytes(small_previous).size() <= 256);
  LocalChatLog small_reboot(small_files, 256);
  assert(small_reboot.recover(recovery).ok());
  assert(recovery.valid_records > 0 && recovery.next_sequence == 9);

  PosixChatLineStore invalid("relative", root + "/other");
  assert(!invalid.pathsValid());
  assert(small_files.appendLine(std::string(256, 'x'), 256).code ==
         ChatLogCode::TooLarge);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-local-chat-"));
  try {
    const source = join(scratch, "local_chat.cpp");
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
      join(component, "include"),
      source,
      join(component, "local_chat_log.cpp"),
      join(component, "posix_chat_store.cpp"),
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

test("local chat log persists only final useful text under strict C++17", () => {
  buildAndRun(false);
});

test("local chat log survives adversarial records under ASan and UBSan", () => {
  buildAndRun(true);
});

test("chat component has no remote history or audio-payload dependency", () => {
  const header = readFileSync(
    join(component, "include/inkloop/storage/local_chat_log.hpp"),
    "utf8",
  );
  const implementation = readFileSync(
    join(component, "local_chat_log.cpp"),
    "utf8",
  );
  const posix = readFileSync(join(component, "posix_chat_store.cpp"), "utf8");
  const all = `${header}\n${implementation}\n${posix}`;
  assert.doesNotMatch(all, /esp_http|WebSocket|myai\.mess|fetch\s*\(|curl|audio_base64/);
  assert.match(header, /appendAsr\([^;]+bool final/);
  assert.match(implementation, /IgnoredBlankAudio/);
  assert.match(implementation, /IgnoredPartial/);
  assert.match(posix, /O_APPEND/);
  assert.match(posix, /fsync/);
  assert.match(posix, /kMaximumChatLineBytes \+ 2/);
});
