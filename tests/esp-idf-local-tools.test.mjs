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
  int image_steps = 20;
  std::string negative = "watermark";
  std::string render = "solid-clean";
  AlbumSummary album{4, 2};

  AdapterResult queryStorage(StorageInfo& output) override {
    ++storage_queries;
    output.remaining_bytes = 25;
    output.total_bytes = 100;
    return {};
  }
  AdapterResult queryAlbumSummary(AlbumSummary& output) override {
    output = album;
    return {};
  }
  AdapterResult resolveImageByOrdinal(
      uint32_t ordinal, AlbumSelection& output) override {
    if (ordinal == 0 || ordinal > album.count)
      return {AdapterCode::NotFound};
    output.asset_id = std::string(64, 'a');
    output.zero_based_index = ordinal - 1;
    output.ordinal = ordinal;
    output.total = album.count;
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
  AdapterResult queryAigcSteps(uint8_t& output) override {
    output = static_cast<uint8_t>(image_steps);
    return {};
  }
  AdapterResult queryAigcNegativePrompt(std::string& output) override {
    output = negative;
    return {AdapterCode::Ok};
  }
  AdapterResult queryDefaultRenderStrategy(std::string& output) override {
    output = render;
    return {AdapterCode::Ok};
  }
  AdapterResult setAigcPrompt(const std::string& prompt) override {
    aigc = prompt;
    return {};
  }
  AdapterResult setAigcSteps(uint8_t steps) override {
    image_steps = steps;
    return {};
  }
  AdapterResult setAigcNegativePrompt(const std::string& prompt) override {
    negative = prompt;
    return {};
  }
  AdapterResult setDefaultRenderStrategy(const std::string& strategy) override {
    if (strategy != "official-quality" && strategy != "solid-clean")
      return {AdapterCode::Unsupported};
    render = strategy;
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
  assert(parser.parseFinalAsr("query free space").matched());
  assert(parser.parseFinalAsr("query total space").command.storage_metric ==
         StorageMetric::Total);

  parsed = parser.parseFinalAsr("列出相册");
  assert(parsed.matched() && parsed.command.kind == CommandKind::ListAlbum);
  assert(parser.parseFinalAsr("list images").command.kind ==
         CommandKind::ListAlbum);
  parsed = parser.parseFinalAsr("上屏第二十七张");
  assert(parsed.matched() &&
         parsed.command.kind == CommandKind::SelectImageOrdinal &&
         parsed.command.number == 27);
  parsed = parser.parseFinalAsr("show the 12th image");
  assert(parsed.matched() && parsed.command.number == 12);
  parsed = parser.parseFinalAsr("display image number 3");
  assert(parsed.matched() && parsed.command.number == 3);
  assert(parser.parseFinalAsr("show image 0").code == ParseCode::NoMatch);
  assert(parser.parseFinalAsr("show image 97").code == ParseCode::NoMatch);

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
  assert(parser.parseFinalAsr(std::string("bad") + char(0x7f)).code ==
         ParseCode::InvalidEncoding);
  assert(parser.parseFinalAsr(std::string("bad\xef\xbf\xbe")).code ==
         ParseCode::InvalidEncoding);
  assert(parser.parseFinalAsr("查询空间并把音量调到50").code ==
         ParseCode::Ambiguous);
  assert(parser.parseFinalAsr("query storage and set volume 50").code ==
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
  parsed = parser.parseFinalAsr("设置图片生成步数为三十七");
  assert(parsed.matched() && parsed.command.kind == CommandKind::SetAigcSteps &&
         parsed.command.number == 37);
  assert(parser.parseFinalAsr("query image generation steps").command.kind ==
         CommandKind::QueryAigcSteps);
  assert(parser.parseFinalAsr("image steps 1").command.number == 1);
  assert(parser.parseFinalAsr("set image steps to 50").command.number == 50);
  assert(parser.parseFinalAsr("image steps 0").code == ParseCode::InvalidValue);
  assert(parser.parseFinalAsr("image steps 51").code == ParseCode::InvalidValue);
  parsed = parser.parseFinalAsr("设置AIGC负面提示词为watermark, blurry");
  assert(parsed.matched() &&
         parsed.command.kind == CommandKind::SetAigcNegativePrompt &&
         parsed.command.text == "watermark, blurry");
  assert(parser.parseFinalAsr("query negative prompt").command.kind ==
         CommandKind::QueryAigcNegativePrompt);
  parsed = parser.parseFinalAsr("clear negative prompt");
  assert(parsed.matched() &&
         parsed.command.kind == CommandKind::SetAigcNegativePrompt &&
         parsed.command.text.empty());
  assert(parser.parseFinalAsr(
      "设置AIGC负面提示词为" + std::string(385, 'x')).code ==
      ParseCode::InvalidValue);
  parsed = parser.parseFinalAsr("设置默认渲染方式为 solid-clean");
  assert(parsed.matched() &&
         parsed.command.kind == CommandKind::SetDefaultRenderStrategy &&
         parsed.command.text == "solid-clean");
  assert(parser.parseFinalAsr("query default render strategy").command.kind ==
         CommandKind::QueryDefaultRenderStrategy);
  assert(parser.parseFinalAsr("设置默认渲染方式为 ../bad").code ==
         ParseCode::InvalidValue);
  assert(LocalCommandParser::validRenderStrategyId("official-quality"));
  assert(!LocalCommandParser::validRenderStrategyId("Official-Quality"));
  assert(!LocalCommandParser::validRenderStrategyId("bad--strategy"));
  assert(LocalCommandParser::validNegativePrompt("", true));
  assert(!LocalCommandParser::validNegativePrompt(""));

  // Legacy Arduino accepted these strings but either overrode SKU metadata,
  // selected a model without an input-image contract, or routed credential
  // destruction through a voice transcript. Native local tools fail closed;
  // those policies remain owned by Board/MyAI/Portal composition.
  for (const char* retired : {
           "image size 400x600", "image model t2i",
           "reset wifi", "reset myai"}) {
    assert(parser.parseFinalAsr(retired).code == ParseCode::NoMatch);
  }
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
  outcome = session.handleFinalAsr("列出相册", 1001, device, tokens);
  assert(outcome.code == ExecutionCode::Executed &&
         outcome.album_summary.count == 4 &&
         outcome.album_summary.current_ordinal == 2 && outcome.text.empty());
  outcome = session.handleFinalAsr("显示第三张", 1001, device, tokens);
  assert(outcome.code == ExecutionCode::Executed &&
         outcome.album_selection.ordinal == 3 &&
         outcome.album_selection.zero_based_index == 2 &&
         outcome.album_selection.total == 4 &&
         outcome.album_selection.asset_id == std::string(64, 'a'));
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
  outcome = session.handleFinalAsr("set image steps to 33", 1009, device, tokens);
  assert(outcome.code == ExecutionCode::Executed && outcome.steps == 33 &&
         device.image_steps == 33);
  outcome = session.handleFinalAsr("查询图片生成步数", 1009, device, tokens);
  assert(outcome.code == ExecutionCode::Executed && outcome.steps == 33);
  outcome = session.handleFinalAsr(
      "set aigc negative prompt to watermark, blurry", 1009, device, tokens);
  assert(outcome.code == ExecutionCode::Executed &&
         device.negative == "watermark, blurry");
  outcome = session.handleFinalAsr("query negative prompt", 1009, device, tokens);
  assert(outcome.code == ExecutionCode::Executed &&
         outcome.text == "watermark, blurry");
  outcome = session.handleFinalAsr(
      "set default render strategy to official-quality", 1009, device, tokens);
  assert(outcome.code == ExecutionCode::Executed &&
         device.render == "official-quality");
  outcome = session.handleFinalAsr(
      "set default render strategy to future-policy", 1009, device, tokens);
  assert(outcome.code == ExecutionCode::AdapterFailure &&
         outcome.adapter_code == AdapterCode::Unsupported &&
         device.render == "official-quality");
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
  device.album = {3, 4};
  outcome = session.handleFinalAsr("list album", 6001, device, tokens);
  assert(outcome.code == ExecutionCode::AdapterContractViolation);
  device.album = {4, 2};
  device.render = "BAD/strategy";
  outcome = session.handleFinalAsr(
      "query default render strategy", 6002, device, tokens);
  assert(outcome.code == ExecutionCode::AdapterContractViolation &&
         outcome.text.empty());
  device.negative.assign(385, 'x');
  outcome = session.handleFinalAsr("query negative prompt", 6003, device, tokens);
  assert(outcome.code == ExecutionCode::AdapterContractViolation &&
         outcome.text.empty());
  device.image_steps = 0;
  outcome = session.handleFinalAsr("query image generation steps", 6004,
                                   device, tokens);
  assert(outcome.code == ExecutionCode::AdapterContractViolation &&
         outcome.steps == 0);
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

test("native Voice owner defers selection success until correlated Display completion", () => {
  const voice = readFileSync(
    join(
      repo,
      "firmware/inkloop-idf/components/inkloop_product/native_voice_service.cpp",
    ),
    "utf8",
  );
  for (const command of [
    "ListAlbum",
    "SelectImageOrdinal",
    "QueryAigcNegativePrompt",
    "SetAigcNegativePrompt",
    "QueryAigcSteps",
    "SetAigcSteps",
    "QueryDefaultRenderStrategy",
    "SetDefaultRenderStrategy",
  ]) {
    assert.match(
      voice,
      new RegExp(`CommandKind::${command}`),
      `missing NativeVoiceService integration for ${command}`,
    );
  }
  const describe = voice.slice(
    voice.indexOf("NativeVoiceService::describeLocalToolOutcome"),
    voice.indexOf("void NativeVoiceService::publishLocalToolOutcome"),
  );
  assert.match(describe, /album_summary\.count/);
  assert.match(describe, /album_summary\.current_ordinal/);
  assert.match(describe, /album_selection\.ordinal/);
  assert.match(describe, /album_selection\.total/);
  assert.doesNotMatch(describe, /album_selection\.asset_id/);

  const publish = voice.slice(
    voice.indexOf("void NativeVoiceService::publishLocalToolOutcome"),
    voice.indexOf("WorkDisposition NativeVoiceService::handleLocalToolCommand"),
  );
  assert.match(publish, /local_tool_display_correlation_\.arm/);
  assert.match(
    publish,
    /stageInteractiveAlbumSelection[\s\S]*postLocalToolDisplaySelection/,
  );
  assert.doesNotMatch(
    publish,
    /display_queued=1 voice_prompt_queued=1/,
  );
  const settle = voice.slice(
    voice.indexOf("void NativeVoiceService::serviceLocalToolDisplayResult"),
    voice.indexOf("std::string NativeVoiceService::describeLocalToolOutcome"),
  );
  assert.match(settle, /terminal\.disposition != WorkDisposition::Complete/);
  assert.match(settle, /local_tool\.failed[\s\S]*stage=display/);
  assert.match(
    settle,
    /enqueueAlbumOrdinal\([\s\S]*terminal\.ordinal, false, terminal\.request_id/,
  );
  assert.match(settle, /describeLocalToolOutcome\(completed\)/);
});
