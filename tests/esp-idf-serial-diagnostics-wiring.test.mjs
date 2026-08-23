import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const componentRoot = join(repo, "firmware/inkloop-idf/components");
const read = (path) => readFileSync(join(componentRoot, path), "utf8");

test("serial diagnostics exposes only four bounded non-destructive commands", () => {
  const parser = read("inkloop_diagnostics/serial_command_parser.cpp");
  const parserHeader = read(
    "inkloop_diagnostics/include/inkloop/diagnostics/serial_command_parser.hpp",
  );

  const commands = [...parser.matchAll(/strcmp\(value, "([^"]+)"\)/g)]
    .map((match) => match[1]);
  assert.deepEqual(commands, [
    "status",
    "album-status",
    "voice-tap",
    "aigc-test",
  ]);
  assert.match(parserHeader, /kMaxLineLength = 96U/);
  assert.match(parserHeader, /std::array<char, kMaxLineLength \+ 1U>/);
  assert.doesNotMatch(
    commands.join(" "),
    /reboot|reset|erase|format|delete|wifi|credential|token/i,
  );
});

test("serial event envelope cannot carry text, URLs, credentials, or responses", () => {
  const header = read(
    "inkloop_diagnostics/include/inkloop/diagnostics/serial_diagnostic_events.hpp",
  );
  const formatter = read("inkloop_diagnostics/serial_diagnostic_events.cpp");
  const eventBody = header.match(
    /struct SerialDiagnosticEvent \{([\s\S]*?)\n\};/,
  )?.[1];
  assert.ok(eventBody);
  assert.doesNotMatch(
    eventBody,
    /std::string|char\s*\*|array\s*<\s*char|span|vector|buffer|payload|body/i,
  );
  assert.match(eventBody, /uint32_t first/);
  assert.match(eventBody, /uint32_t second/);
  assert.match(eventBody, /uint8_t code/);
  assert.match(eventBody, /uint8_t flags/);

  // All variable strings formatted on the wire come from closed enums.
  assert.match(formatter, /serialCommandName\(event\.command\)/);
  assert.match(formatter, /serialParseCodeName/);
  assert.match(formatter, /voiceStateName/);
  assert.match(formatter, /aigcPhaseName/);
  assert.doesNotMatch(
    formatter,
    /transcript|prompt|device[_ ]?code|pairing|token|cookie|password|credential|response|https?:|wss?:/i,
  );
});

test("IDF transport is bounded, optional per SKU, and owns no task", () => {
  const owner = read("inkloop_diagnostics_idf/esp_serial_diagnostics.cpp");
  const ownerHeader = read(
    "inkloop_diagnostics_idf/include/inkloop/diagnostics/esp_serial_diagnostics.hpp",
  );

  assert.match(owner, /CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG/);
  assert.match(owner, /CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG/);
  assert.match(owner, /return nullptr/);
  assert.match(owner, /O_RDWR \| O_NONBLOCK/);
  assert.match(owner, /xQueueSend\(event_queue_, &event, 0\)/);
  assert.match(owner, /xQueueReceive\(event_queue_, &event, 0\)/);
  assert.doesNotMatch(owner + ownerHeader, /xTaskCreate|vTaskDelay|portMAX_DELAY/);
  assert.match(ownerHeader, /kEventDepth = 32U/);
  assert.match(ownerHeader, /kReadBytesPerTick = 64U/);
  assert.match(ownerHeader, /kEventsPerTick = 16U/);
});

test("commands route through the real product owners without a MyAI bypass", () => {
  const runtime = read("inkloop_product/product_runtime.cpp");
  const voice = read("inkloop_product/native_voice_service.cpp");
  const display = read("inkloop_product/native_display_service.cpp");

  const handler = runtime.match(
    /void EspProductRuntime::handleSerialDiagnosticCommand\([\s\S]*?\n\}/,
  )?.[0];
  assert.ok(handler);
  assert.match(handler, /portal_\.readSerialDiagnosticState/);
  assert.match(handler, /portal_\.serialDiagnosticAlbum/);
  assert.match(handler, /voice_\.enqueueTopButton\(\)/);
  assert.match(handler, /voice_\.enqueueDiagnosticImageGeneration/);
  assert.doesNotMatch(
    handler,
    /client_->|startImage|pollImage|downloadImage|renderOrdinal|http|websocket/i,
  );
  assert.match(
    runtime,
    /void EspProductRuntime::servicePortal\(\) \{\s*serial_diagnostics_\.service\(\);/,
  );

  const inspect = voice.match(
    /NativeVoiceService::inspect\([\s\S]*?\n\}/,
  )?.[0];
  assert.ok(inspect);
  assert.match(inspect, /transcript\.size\(\)/);
  assert.match(inspect, /SerialDiagnosticEventKind::VoiceAsrFinal/);
  assert.doesNotMatch(inspect, /event\.(?:text|body|payload|message)/);

  const displayHandler = display.match(
    /WorkDisposition NativeDisplayService::handle\([\s\S]*?\n\}/,
  )?.[0];
  assert.ok(displayHandler);
  assert.match(
    displayHandler,
    /const bool rendered = renderOrdinal\(envelope\.flags\);[\s\S]*if \(rendered\)[\s\S]*DisplayComplete/,
  );
});
