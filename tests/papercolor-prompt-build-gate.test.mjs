import assert from "node:assert/strict";
import { access, cp, mkdir, mkdtemp, rm } from "node:fs/promises";
import { homedir, tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

async function platformioCommand() {
  const managed = join(homedir(), ".platformio", "penv", "bin", "pio");
  try {
    await access(managed);
    return managed;
  } catch {
    return "pio";
  }
}

test("PaperColor project-level pre-build rejects a missing required WAV", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-prompt-gate-"));
  const source = new URL("../firmware/m5-papercolor/", import.meta.url);
  try {
    await cp(new URL("platformio.ini", source), join(temporary, "platformio.ini"));
    await mkdir(join(temporary, "lib"), { recursive: true });
    await cp(new URL("lib/InkloopVoice/", source), join(temporary, "lib", "InkloopVoice"), {
      recursive: true,
    });
    await rm(join(
      temporary,
      "lib",
      "InkloopVoice",
      "assets",
      "speech",
      "voice_listening.wav",
    ));
    const result = spawnSync(await platformioCommand(), ["run", "-d", temporary], {
      encoding: "utf8",
      timeout: 120_000,
    });
    const output = `${result.stdout || ""}\n${result.stderr || ""}`;
    assert.notEqual(result.status, 0, output);
    assert.match(output, /missing prompt asset: speech\/voice_listening\.wav/);
    assert.doesNotMatch(output, /Linking .*firmware\.elf/);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});
