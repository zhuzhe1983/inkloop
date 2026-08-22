import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const runtime = readFileSync(
  join(repo, "firmware/m5-papercolor/src/PaperColorApplicationRuntime.cpp"),
  "utf8",
);
const portal = readFileSync(
  join(repo, "firmware/m5-papercolor/lib/InkloopPortal/InkloopPortal.cpp"),
  "utf8",
);

test("Arduino compatibility runtime stores final local ASR and filters blank audio", () => {
  assert.match(
    runtime,
    /onTranscript\([\s\S]*if \(!final\)[\s\S]*onAsrPartial[\s\S]*return;[\s\S]*appendMyAiChatMessage\("user", text\)/,
  );
  assert.match(runtime, /isBlankAudioChatArtifact\(bounded\)/);
  assert.match(runtime, /normalized == "blank_audio"/);
  assert.match(runtime, /kMyAiChatLogPath\[\] = "\/inkloop\/myai-chat\.txt"/);
  assert.match(runtime, /appendMyAiChatLogRecord/);
});

test("PaperColor WebUI chat endpoint reads the device-local adapter only", () => {
  assert.match(portal, /\/api\/myai\/chat/);
  assert.match(portal, /adapter_\.readMyAiChatHistory\(&history\)/);
  assert.doesNotMatch(portal, /\/api\/v1\/.*chat|fetchMyAiChat|remoteChatHistory/);
});
