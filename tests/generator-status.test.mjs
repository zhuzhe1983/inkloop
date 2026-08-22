import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

test("generator readiness distinguishes model discovery from a successful inference", async () => {
  const route = await readFile(new URL("../app/api/generate/route.ts", import.meta.url), "utf8");
  const studio = await readFile(new URL("../app/ink-studio.tsx", import.meta.url), "utf8");

  const getStart = route.indexOf("export async function GET()");
  const postStart = route.indexOf("export async function POST(", getStart);
  const getHandler = route.slice(getStart, postStart);

  assert.match(getHandler, /available:\s*Boolean\(apiKey && gatewayModels\.length\)/);
  assert.match(getHandler, /models:\s*gatewayModels/);
  assert.doesNotMatch(getHandler, /\.\.\.\(defaultModel === AUTO_MODEL/);
  assert.match(studio, /type GeneratorStatus = "checking" \| "online" \| "degraded" \| "local"/);
  assert.match(studio, /result\.mode === "llm"[\s\S]*setGeneratorStatus\("online"\)[\s\S]*setGeneratorStatus\("degraded"\)/);
  assert.match(studio, /在线模型已发现 ·/);
  assert.match(studio, /在线模型暂不可用 · 当前使用本地模板/);
});
