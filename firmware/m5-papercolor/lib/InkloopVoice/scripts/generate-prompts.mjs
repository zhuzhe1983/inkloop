#!/usr/bin/env node

import { createHash } from "node:crypto";
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const moduleRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const assetsRoot = join(moduleRoot, "assets");
const manifestPath = join(assetsRoot, "prompts.v1.json");
const sampleRate = 16000;
const voice = "Tingting";

const prompts = [
  ["ordinal.first", "第一张", 523],
  ["ordinal.second", "第二张", 587],
  ["ordinal.third", "第三张", 659],
  ["ordinal.number", "当前图片", 698],
  ["display.please_wait", "请稍候，屏幕正在刷新", 440],
  ["display.refresh_start", "开始刷新", 466],
  ["device.restored", "设备已恢复", 784],
  ["ordinal.prefix", "第", 806],
  ["ordinal.digit_zero", "零", 831],
  ["ordinal.digit_one", "一", 854],
  ["ordinal.digit_two", "二", 880],
  ["ordinal.digit_three", "三", 907],
  ["ordinal.digit_four", "四", 932],
  ["ordinal.digit_five", "五", 960],
  ["ordinal.digit_six", "六", 988],
  ["ordinal.digit_seven", "七", 1016],
  ["ordinal.digit_eight", "八", 1046],
  ["ordinal.digit_nine", "九", 1077],
  ["ordinal.ten", "十", 1109],
  ["ordinal.suffix", "张", 1142],
  ["confirmation.repeat_exactly", "请准确重复确认语句", 392],
  ["confirmation.press_top_button", "请按顶部按键确认", 349],
  ["confirmation.cancelled", "已取消", 330],
  ["confirmation.expired", "确认已过期", 311],
  ["storage.free_space", "已查询剩余空间", 294],
  ["storage.formatted", "存储已格式化", 277],
  ["images.empty", "没有图片", 262],
  ["images.list_ready", "图片列表已准备", 247],
  ["images.deleted", "图片已删除", 233],
  ["images.cleared", "图片已清空", 220],
  ["settings.saved", "设置已保存", 208],
  ["settings.reset", "设置已重置", 196],
  ["voice.error", "出错了，请重试", 185],
  ["voice.listening", "请说", 175],
];

function fileName(id) {
  return `${id.replaceAll(".", "_")}.wav`;
}

function wavBuffer(frequency, milliseconds = 220) {
  const frames = Math.floor(sampleRate * milliseconds / 1000);
  const dataBytes = frames * 2;
  const output = Buffer.alloc(44 + dataBytes);
  output.write("RIFF", 0);
  output.writeUInt32LE(36 + dataBytes, 4);
  output.write("WAVEfmt ", 8);
  output.writeUInt32LE(16, 16);
  output.writeUInt16LE(1, 20);
  output.writeUInt16LE(1, 22);
  output.writeUInt32LE(sampleRate, 24);
  output.writeUInt32LE(sampleRate * 2, 28);
  output.writeUInt16LE(2, 32);
  output.writeUInt16LE(16, 34);
  output.write("data", 36);
  output.writeUInt32LE(dataBytes, 40);
  for (let frame = 0; frame < frames; frame += 1) {
    const envelope = Math.min(1, frame / 160) * Math.min(1, (frames - frame) / 240);
    const sample = Math.round(Math.sin(2 * Math.PI * frequency * frame / sampleRate) *
      5200 * envelope);
    output.writeInt16LE(sample, 44 + frame * 2);
  }
  return output;
}

function sha256(path) {
  return createHash("sha256").update(readFileSync(path)).digest("hex");
}

function inspectWav(path) {
  const bytes = readFileSync(path);
  if (bytes.length < 44 || bytes.toString("ascii", 0, 4) !== "RIFF" ||
      bytes.toString("ascii", 8, 12) !== "WAVE")
    throw new Error(`${path}: not a RIFF/WAVE file`);
  const formatOffset = bytes.indexOf(Buffer.from("fmt "));
  if (formatOffset < 0 || bytes.readUInt16LE(formatOffset + 8) !== 1 ||
      bytes.readUInt16LE(formatOffset + 10) !== 1 ||
      bytes.readUInt32LE(formatOffset + 12) !== sampleRate ||
      bytes.readUInt16LE(formatOffset + 22) !== 16)
    throw new Error(`${path}: expected PCM16 mono ${sampleRate} Hz`);
}

function commandAvailable(command) {
  return spawnSync("/usr/bin/env", ["sh", "-c", `command -v ${command}`],
    { stdio: "ignore" }).status === 0;
}

function synthesize(text, destination, temporaryDirectory) {
  const aiff = join(temporaryDirectory, "prompt.aiff");
  const spoken = spawnSync("say", ["-v", voice, "-r", "205", "-o", aiff, text],
    { encoding: "utf8" });
  if (spoken.status !== 0)
    throw new Error(spoken.stderr || `say failed for ${text}`);
  const converted = spawnSync("afconvert",
    ["-f", "WAVE", "-d", `LEI16@${sampleRate}`, "-c", "1", aiff, destination],
    { encoding: "utf8" });
  if (converted.status !== 0)
    throw new Error(converted.stderr || `afconvert failed for ${text}`);
}

function generate() {
  mkdirSync(join(assetsRoot, "speech"), { recursive: true });
  mkdirSync(join(assetsRoot, "fallback"), { recursive: true });
  const canSpeak = process.platform === "darwin" && commandAvailable("say") &&
    commandAvailable("afconvert");
  const temporaryDirectory = mkdtempSync(join(tmpdir(), "inkloop-prompts-"));
  const entries = [];
  try {
    for (const [id, text, frequency] of prompts) {
      const speechRelative = `speech/${fileName(id)}`;
      const fallbackRelative = `fallback/${fileName(id)}`;
      const speechPath = join(assetsRoot, speechRelative);
      const fallbackPath = join(assetsRoot, fallbackRelative);
      writeFileSync(fallbackPath, wavBuffer(frequency));
      let source = "tone-fallback";
      if (canSpeak) {
        synthesize(text, speechPath, temporaryDirectory);
        source = `macos-say:${voice}`;
      } else {
        writeFileSync(speechPath, wavBuffer(frequency));
      }
      inspectWav(speechPath);
      inspectWav(fallbackPath);
      entries.push({
        id,
        file: speechRelative,
        fallback: fallbackRelative,
        sha256: sha256(speechPath),
        fallbackSha256: sha256(fallbackPath),
        source,
      });
    }
  } finally {
    rmSync(temporaryDirectory, { recursive: true, force: true });
  }
  const manifest = {
    schema: "inkloop.prompt-manifest",
    version: 1,
    format: { container: "wav", codec: "pcm_s16le", channels: 1, sampleRate },
    entries,
  };
  writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
  verify();
  process.stdout.write(`generated ${entries.length} offline prompts (${canSpeak ? "macOS TTS" : "tones"})\n`);
}

function verify() {
  if (!existsSync(manifestPath)) throw new Error(`missing prompt manifest: ${manifestPath}`);
  const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
  if (manifest.schema !== "inkloop.prompt-manifest" || manifest.version !== 1)
    throw new Error("unsupported prompt manifest schema/version");
  const expectedIds = new Set(prompts.map(([id]) => id));
  if (!Array.isArray(manifest.entries) || manifest.entries.length !== expectedIds.size)
    throw new Error("prompt manifest is incomplete");
  for (const entry of manifest.entries) {
    if (!expectedIds.delete(entry.id)) throw new Error(`unexpected/duplicate prompt: ${entry.id}`);
    for (const [field, checksumField] of [["file", "sha256"], ["fallback", "fallbackSha256"]]) {
      const path = resolve(assetsRoot, entry[field]);
      if (!path.startsWith(`${assetsRoot}/`) || !existsSync(path))
        throw new Error(`missing prompt asset: ${entry[field]}`);
      inspectWav(path);
      if (sha256(path) !== entry[checksumField])
        throw new Error(`prompt checksum mismatch: ${entry[field]}`);
    }
  }
  if (expectedIds.size) throw new Error(`missing prompt IDs: ${[...expectedIds].join(", ")}`);
  process.stdout.write(`verified ${manifest.entries.length} offline prompt entries\n`);
}

const mode = process.argv[2] ?? "--verify";
if (mode === "--generate") generate();
else if (mode === "--verify") verify();
else throw new Error("usage: generate-prompts.mjs [--generate|--verify]");
