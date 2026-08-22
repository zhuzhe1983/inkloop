import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo,
  "firmware/inkloop-idf/components/inkloop_local_tools",
);

const harness = String.raw`
#include <cassert>
#include <cstdint>
#include <string>

#include "inkloop/local_tools/local_tools.hpp"

using namespace inkloop::local_tools;

struct Tokens final : IConfirmationTokenSource {
  std::string next = "confirm_token_123456789";
  bool issue(std::string& token) override { token = next; return true; }
};

struct Device final : ILocalToolsAdapter {
  int storage_queries = 0;
  int deleted_ordinal = 0;
  std::string deleted_id;
  int clears = 0;
  int formats = 0;
  int volume = 22;
  int led = -1;
  std::string assistant = "简洁回答";
  std::string aigc = "水墨画";

  AdapterResult queryStorage(StorageInfo& output) override {
    ++storage_queries;
    output.remaining_bytes = 25;
    output.total_bytes = 100;
    return {};
  }
  AdapterResult deleteImageByOrdinal(uint32_t ordinal) override {
    deleted_ordinal = static_cast<int>(ordinal);
    return {};
  }
  AdapterResult deleteImageById(const std::string& id) override {
    deleted_id = id;
    return {};
  }
  AdapterResult clearAlbum() override { ++clears; return {}; }
  AdapterResult queryVolume(uint8_t& percent) override {
    percent = static_cast<uint8_t>(volume);
    return {};
  }
  AdapterResult setVolume(uint8_t percent) override {
    volume = percent;
    return {};
  }
  AdapterResult formatTfCard() override { ++formats; return {}; }
  AdapterResult queryAssistantPrompt(std::string& output) override {
    output = assistant;
    return {};
  }
  AdapterResult setAssistantPrompt(const std::string& prompt) override {
    assistant = prompt;
    return {};
  }
  AdapterResult queryAigcPrompt(std::string& output) override {
    output = aigc;
    return {};
  }
  AdapterResult queryAigcNegativePrompt(std::string& output) override {
    output = "watermark";
    return {AdapterCode::Ok};
  }
  AdapterResult queryDefaultRenderStrategy(std::string& output) override {
    output = "solid-clean";
    return {AdapterCode::Ok};
  }
  AdapterResult setAigcPrompt(const std::string& prompt) override {
    aigc = prompt;
    return {};
  }
  AdapterResult setLedMaximumBrightness(uint8_t percent) override {
    led = percent;
    return {};
  }
};

int main() {
  LocalCommandParser parser;
  ParseResult parsed = parser.parseFinalAsr("还有多少剩余空间");
  assert(parsed.matched() && parsed.command.kind == CommandKind::QueryStorage);
  assert(parsed.command.storage_metric == StorageMetric::Remaining);
  parsed = parser.parseFinalAsr("总容量是多少");
  assert(parsed.matched() && parsed.command.storage_metric == StorageMetric::Total);
  assert(parser.parseFinalAsr("查询剩余空间。").matched());
  parsed = parser.parseFinalAsr("查询总空间和剩余空间");
  assert(parsed.matched() && parsed.command.storage_metric == StorageMetric::Both);

  parsed = parser.parseFinalAsr("删除第二十七张图片");
  assert(parsed.matched() && parsed.command.kind == CommandKind::DeleteImageOrdinal);
  assert(parsed.command.number == 27);
  parsed = parser.parseFinalAsr("删除图片 Asset_42:front.png");
  assert(parsed.matched() && parsed.command.kind == CommandKind::DeleteImageId);
  assert(parsed.command.text == "Asset_42:front.png");
  assert(parser.parseFinalAsr("删除图片 Asset_42:front.png。").matched());
  assert(parser.parseFinalAsr("删除第0张").code == ParseCode::InvalidValue);
  assert(parser.parseFinalAsr("删除第97张").code == ParseCode::InvalidValue);
  assert(parser.parseFinalAsr("删除图片 ../secret").code == ParseCode::InvalidValue);

  assert(parser.parseFinalAsr("").code == ParseCode::IgnoredEmpty);
  assert(parser.parseFinalAsr("   \t").code == ParseCode::IgnoredEmpty);
  assert(parser.parseFinalAsr("blank_audio").code == ParseCode::IgnoredBlankAudio);
  assert(parser.parseFinalAsr(" [BLANK-AUDIO] ").code == ParseCode::IgnoredBlankAudio);
  assert(parser.parseFinalAsr(std::string(513, 'a')).code == ParseCode::TooLong);
  assert(parser.parseFinalAsr(std::string("bad") + char(0xff)).code ==
         ParseCode::InvalidEncoding);
  assert(parser.parseFinalAsr("查询空间并把音量调到50").code ==
         ParseCode::Ambiguous);

  parsed = parser.parseFinalAsr("设置音量为百分之三十");
  assert(parsed.matched() && parsed.command.number == 30);
  parsed = parser.parseFinalAsr("把音量调整为四十五");
  assert(parsed.matched() && parsed.command.number == 45);
  assert(parser.parseFinalAsr("设置音量101%").code == ParseCode::InvalidValue);
  parsed = parser.parseFinalAsr("设置LED最大亮度为80%");
  assert(parsed.matched() && parsed.command.number == 80);
  assert(parser.parseFinalAsr("设置LED最大亮度为0%").code ==
         ParseCode::InvalidValue);

  parsed = parser.parseFinalAsr("设置智能体提示词为你是一个简洁的助手");
  assert(parsed.matched() && parsed.command.kind == CommandKind::SetAssistantPrompt);
  assert(parsed.command.text == "你是一个简洁的助手");
  assert(parser.parseFinalAsr("查询智能体提示词").command.kind ==
         CommandKind::QueryAssistantPrompt);
  parsed = parser.parseFinalAsr("设置AIGC图片提示词为暖色水墨风格");
  assert(parsed.matched() && parsed.command.kind == CommandKind::SetAigcPrompt);
  assert(parsed.command.text == "暖色水墨风格");
  assert(parser.parseFinalAsr("查询AIGC图片提示词").command.kind ==
         CommandKind::QueryAigcPrompt);
  parsed = parser.parseFinalAsr("设置智能体提示词为" + std::string(400, 'x'));
  assert(parsed.matched() && parsed.command.text.size() == 400);
  assert(!LocalCommandParser::validPrompt(std::string(513, 'x')));
  assert(LocalCommandParser::validStoredPrompt(std::string(1024, 'x')));
  assert(!LocalCommandParser::validStoredPrompt(std::string(1025, 'x')));

  Device device;
  Tokens tokens;
  LocalToolsSession session;
  ToolOutcome outcome = session.handleFinalAsr("查询剩余空间", 1000, device, tokens);
  assert(outcome.code == ExecutionCode::Executed && device.storage_queries == 1);
  assert(outcome.storage.remaining_bytes == 25 && outcome.storage.total_bytes == 100);
  assert(outcome.storage_metric == StorageMetric::Remaining);
  outcome = session.handleFinalAsr("删除第十张", 1001, device, tokens);
  assert(outcome.code == ExecutionCode::ConfirmationRequired &&
         device.deleted_ordinal == 0);
  outcome = session.confirm(outcome.confirmation_token, 1002, device);
  assert(outcome.code == ExecutionCode::Executed && device.deleted_ordinal == 10);
  outcome = session.handleFinalAsr("删除图片 keep-case_ID", 1002, device, tokens);
  assert(outcome.code == ExecutionCode::ConfirmationRequired &&
         device.deleted_id.empty());
  outcome = session.confirm(outcome.confirmation_token, 1003, device);
  assert(outcome.code == ExecutionCode::Executed &&
         device.deleted_id == "keep-case_ID");
  outcome = session.handleFinalAsr("设置音量为65%", 1003, device, tokens);
  assert(outcome.code == ExecutionCode::Executed && device.volume == 65);
  outcome = session.handleFinalAsr("查询音量", 1004, device, tokens);
  assert(outcome.code == ExecutionCode::Executed && outcome.percent == 65);
  outcome = session.handleFinalAsr("设置LED最大亮度为75", 1005, device, tokens);
  assert(outcome.code == ExecutionCode::Executed && device.led == 75);
  outcome = session.handleFinalAsr("设置智能体提示词为只说事实", 1006, device, tokens);
  assert(outcome.code == ExecutionCode::Executed && device.assistant == "只说事实");
  outcome = session.handleFinalAsr("查询智能体提示词", 1007, device, tokens);
  assert(outcome.code == ExecutionCode::Executed && outcome.text == "只说事实");
  outcome = session.handleFinalAsr("设置AIGC图片提示词为版画风格", 1008, device, tokens);
  assert(outcome.code == ExecutionCode::Executed && device.aigc == "版画风格");
  outcome = session.handleFinalAsr("查询AIGC图片提示词", 1009, device, tokens);
  assert(outcome.code == ExecutionCode::Executed && outcome.text == "版画风格");
  device.assistant.assign(1024, 's');
  outcome = session.handleFinalAsr("查询智能体提示词", 1010, device, tokens);
  assert(outcome.code == ExecutionCode::Executed && outcome.text.size() == 1024);
  device.assistant.push_back('x');
  outcome = session.handleFinalAsr("查询智能体提示词", 1011, device, tokens);
  assert(outcome.code == ExecutionCode::AdapterContractViolation);

  // A destructive phrase never mutates immediately, and confirmation words
  // inside one ASR utterance cannot bypass the token API.
  outcome = session.handleFinalAsr("清空相册", 2000, device, tokens);
  assert(outcome.code == ExecutionCode::ConfirmationRequired);
  assert(device.clears == 0 && session.confirmationPending());
  const std::string clear_token = outcome.confirmation_token;
  outcome = session.handleFinalAsr("确认清空相册", 2001, device, tokens);
  assert(outcome.code == ExecutionCode::Rejected && device.clears == 0);
  outcome = session.confirm("wrong_token_1234567", 2002, device);
  assert(outcome.code == ExecutionCode::ConfirmationMismatch && device.clears == 0);
  outcome = session.confirm(clear_token, 2003, device);
  assert(outcome.code == ExecutionCode::Executed && device.clears == 1);
  assert(!session.confirmationPending());
  assert(session.confirm(clear_token, 2004, device).code ==
         ExecutionCode::ConfirmationMissing);

  // Only the exact TF-card command is recognized. Generic, SD, and internal
  // targets fail closed and never reach the formatting adapter.
  assert(session.handleFinalAsr("格式化存储", 3000, device, tokens).code ==
         ExecutionCode::Rejected);
  assert(session.handleFinalAsr("格式化SD卡", 3000, device, tokens).code ==
         ExecutionCode::Rejected);
  assert(session.handleFinalAsr("格式化内部存储", 3000, device, tokens).code ==
         ExecutionCode::Rejected);
  outcome = session.handleFinalAsr("格式化TF卡", 3000, device, tokens);
  assert(outcome.code == ExecutionCode::ConfirmationRequired && device.formats == 0);
  const std::string format_token = outcome.confirmation_token;
  outcome = session.confirm(format_token, 3000 + kConfirmationLifetimeMs + 1, device);
  assert(outcome.code == ExecutionCode::ConfirmationExpired && device.formats == 0);
  outcome = session.handleFinalAsr("格式化 TF 卡", 4000, device, tokens);
  assert(outcome.code == ExecutionCode::ConfirmationRequired);
  outcome = session.confirm(outcome.confirmation_token, 4001, device);
  assert(outcome.code == ExecutionCode::Executed && device.formats == 1);

  tokens.next = "short";
  outcome = session.handleFinalAsr("清空所有图片", 5000, device, tokens);
  assert(outcome.code == ExecutionCode::TokenUnavailable && device.clears == 1);

  device.volume = 101;
  outcome = session.handleFinalAsr("查询音量", 6000, device, tokens);
  assert(outcome.code == ExecutionCode::AdapterContractViolation);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-local-tools-"));
  try {
    const source = join(scratch, "local_tools.cpp");
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
      join(component, "local_tools.cpp"),
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

test("local voice tools compile and run under strict C++17", () => {
  buildAndRun(false);
});

test("local voice tools survive bounded adversarial input under ASan and UBSan", () => {
  buildAndRun(true);
});

test("local tools remain portable and carry no audio payload or runtime dependency", () => {
  const header = readFileSync(
    join(component, "include/inkloop/local_tools/local_tools.hpp"),
    "utf8",
  );
  const implementation = readFileSync(join(component, "local_tools.cpp"), "utf8");
  const all = `${header}\n${implementation}`;
  assert.doesNotMatch(
    all,
    /Arduino|esp_http|esp_wifi|WebSocket|freertos|audio_base64|base64|PCM|I2S/,
  );
  assert.match(header, /class ILocalToolsAdapter/);
  assert.match(header, /virtual AdapterResult formatTfCard\(\)/);
  assert.doesNotMatch(header, /format(?:Storage|TfCard)\([^)]*string/);
  assert.match(header, /kMaximumTranscriptBytes/);
  assert.match(header, /kMaximumConfirmationTokenBytes/);
});
