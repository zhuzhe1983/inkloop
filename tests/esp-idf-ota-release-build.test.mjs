import assert from "node:assert/strict";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const root = process.cwd();
const project = path.join(root, "firmware/inkloop-idf");
const builder = path.join(project, "tools/build_ota_release.py");
const source = fs.readFileSync(builder, "utf8");
const version = fs.readFileSync(path.join(project, "version.txt"), "utf8").trim();
const boardSku = "m5-papercolor-c151";
const manifestUrl = `https://inkloop.mess.host/ota/${boardSku}/manifest.json`;
const publicKey = "d75a980182b10ab7d54bfed3c964073a" +
  "0ee172f3daa62325af021a68f707511a";

function run(command, args, options = {}) {
  return spawnSync(command, args, { encoding: "utf8", ...options });
}

function defaultsText(overrides = {}) {
  return [
    `CONFIG_INKLOOP_OTA_MANIFEST_URL="${overrides.url ?? manifestUrl}"`,
    `CONFIG_INKLOOP_OTA_ED25519_PUBLIC_KEY_HEX="${overrides.key ?? publicKey}"`,
    `CONFIG_INKLOOP_OTA_TOTAL_DEADLINE_MS=${overrides.deadline ?? 120000}`,
  ].join("\n") + "\n";
}

function fixture(prefix = "inkloop-ota-release-build-") {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), prefix));
  const idf = path.join(directory, "esp-idf");
  const bin = path.join(idf, "fake-bin");
  const defaults = path.join(directory, "ota-release.defaults");
  fs.mkdirSync(bin, { recursive: true });
  fs.writeFileSync(path.join(idf, "README"), "fake pinned ESP-IDF\n");
  assert.equal(run("git", ["init", "-q", idf]).status, 0);
  assert.equal(run("git", ["-C", idf, "config", "user.email", "test@example.com"]).status, 0);
  assert.equal(run("git", ["-C", idf, "config", "user.name", "Inkloop Test"]).status, 0);
  assert.equal(run("git", ["-C", idf, "add", "README"]).status, 0);
  assert.equal(run("git", ["-C", idf, "commit", "-qm", "fixture"]).status, 0);
  assert.equal(run("git", ["-C", idf, "tag", "v6.0.2"]).status, 0);
  fs.writeFileSync(
    path.join(idf, "export.sh"),
    `export PATH="${bin}:$PATH"\n`,
  );
  const fakeIdf = String.raw`#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

args = sys.argv[1:]
build = Path(args[args.index("-B") + 1])
definitions = {}
for argument in args:
    if argument.startswith("-D") and "=" in argument:
        key, value = argument[2:].split("=", 1)
        definitions[key] = value
mode = os.environ.get("INKLOOP_FAKE_IDF_MODE", "ok")
if mode == "tool_failure":
    (build / "partial.bin").write_bytes(b"partial")
    raise SystemExit(19)

defaults = Path(definitions["SDKCONFIG_DEFAULTS"]).read_text(encoding="ascii")
assignments = {}
for line in defaults.splitlines():
    if line.startswith("CONFIG_"):
        key, value = line.split("=", 1)
        assignments[key] = value
url = assignments["CONFIG_INKLOOP_OTA_MANIFEST_URL"].strip('"')
key = assignments["CONFIG_INKLOOP_OTA_ED25519_PUBLIC_KEY_HEX"].strip('"')
deadline = assignments["CONFIG_INKLOOP_OTA_TOTAL_DEADLINE_MS"]
if mode == "wrong_config":
    url = url.replace("inkloop.mess.host", "wrong.example.com")
target = "esp32c3" if mode == "wrong_target" else "esp32s3"
board = "mock_minimal" if mode == "wrong_board" else "m5_papercolor_c151"
project_version = "9.9.9" if mode == "wrong_version" else "${version}"

sdkconfig = "\n".join([
    f'CONFIG_INKLOOP_OTA_MANIFEST_URL="{url}"',
    f'CONFIG_INKLOOP_OTA_ED25519_PUBLIC_KEY_HEX="{key}"',
    f"CONFIG_INKLOOP_OTA_TOTAL_DEADLINE_MS={deadline}",
    f'CONFIG_IDF_TARGET="{target}"',
    "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y",
    f"CONFIG_SPIRAM={assignments.get('CONFIG_SPIRAM', 'n')}",
    f"CONFIG_SPIRAM_MODE_OCT={'n' if mode == 'wrong_psram' else assignments.get('CONFIG_SPIRAM_MODE_OCT', 'n')}",
    f"CONFIG_SPIRAM_TYPE_AUTO={assignments.get('CONFIG_SPIRAM_TYPE_AUTO', 'n')}",
    f"CONFIG_SPIRAM_SPEED_80M={assignments.get('CONFIG_SPIRAM_SPEED_80M', 'n')}",
    f"CONFIG_SPIRAM_BOOT_INIT={assignments.get('CONFIG_SPIRAM_BOOT_INIT', 'n')}",
    f"CONFIG_SPIRAM_USE_MALLOC={assignments.get('CONFIG_SPIRAM_USE_MALLOC', 'n')}",
    f"CONFIG_SPIRAM_MEMTEST={assignments.get('CONFIG_SPIRAM_MEMTEST', 'n')}",
    f"CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL={assignments.get('CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL', '0')}",
    f"CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL={assignments.get('CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL', '0')}",
    f"CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC={assignments.get('CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC', 'n')}",
    f"CONFIG_LITTLEFS_MULTIVERSION={assignments.get('CONFIG_LITTLEFS_MULTIVERSION', 'n')}",
    f"CONFIG_LITTLEFS_DISK_VERSION_2_0={assignments.get('CONFIG_LITTLEFS_DISK_VERSION_2_0', 'n')}",
]) + "\n"
if mode == "duplicate_config":
    sdkconfig += f'CONFIG_INKLOOP_OTA_MANIFEST_URL="{url}"\n'
(build / "sdkconfig").write_text(sdkconfig, encoding="ascii")
(build / "CMakeCache.txt").write_text(
    f"IDF_TARGET:STRING={target}\nINKLOOP_BOARD:STRING={board}\n", encoding="utf-8")
(build / "project_description.json").write_text(json.dumps({
    "project_name": "inkloop_idf",
    "project_version": project_version,
    "target": target,
    "app_bin": "inkloop_idf.bin",
}), encoding="utf-8")

for relative, data in {
    "bootloader/bootloader.bin": b"bootloader",
    "partition_table/partition-table.bin": b"partitions",
    "ota_data_initial.bin": b"otadata",
}.items():
    destination = build / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
app = b"\xe9FAKE-C151\x00" + url.encode("ascii") + b"\x00"
if mode != "binary_missing_key":
    app += key.encode("ascii")
if mode == "symlink_binary":
    external = build.parent / "external-app.bin"
    external.write_bytes(app)
    (build / "inkloop_idf.bin").symlink_to(external)
elif mode != "missing_binary":
    (build / "inkloop_idf.bin").write_bytes(app)

flash_args = (
    "--flash-mode dio --flash-freq 80m --flash-size 16MB\n"
    "0x0 bootloader/bootloader.bin\n"
    "0x8000 partition_table/partition-table.bin\n"
    "0xe000 ota_data_initial.bin\n"
    "0x10000 inkloop_idf.bin\n"
)
if mode == "bad_flash":
    flash_args = flash_args.replace("0x10000", "0x20000")
(build / "flash_args").write_text(flash_args, encoding="ascii")
flash_files = {
    "0x0": "bootloader/bootloader.bin",
    "0x8000": "partition_table/partition-table.bin",
    "0xe000": "ota_data_initial.bin",
    "0x10000": "inkloop_idf.bin",
}
flasher = {
    "write_flash_args": ["--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m"],
    "flash_settings": {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"},
    "flash_files": flash_files,
    "bootloader": {"offset": "0x0", "file": "bootloader/bootloader.bin", "encrypted": "false"},
    "partition-table": {"offset": "0x8000", "file": "partition_table/partition-table.bin", "encrypted": "false"},
    "otadata": {"offset": "0xe000", "file": "ota_data_initial.bin", "encrypted": "false"},
    "app": {"offset": "0x10000", "file": "inkloop_idf.bin", "encrypted": "false"},
    "extra_esptool_args": {"after": "hard-reset", "before": "default-reset", "stub": True, "chip": target},
}
(build / "flasher_args.json").write_text(json.dumps(flasher), encoding="utf-8")
`;
  fs.writeFileSync(path.join(bin, "idf.py"), fakeIdf);
  fs.chmodSync(path.join(bin, "idf.py"), 0o755);
  fs.writeFileSync(defaults, defaultsText());
  return { directory, idf, defaults, nextOutput: 0 };
}

function buildRelease(f, options = {}) {
  const output = options.output ?? path.join(f.directory, `build-${f.nextOutput++}`);
  const args = [
    builder,
    "--idf-path", options.idf ?? f.idf,
    "--public-defaults", options.defaults ?? f.defaults,
    "--output-dir", output,
    "--board-sku", options.boardSku ?? boardSku,
    "--firmware-version", options.version ?? version,
  ];
  if (options.extraArgs) args.push(...options.extraArgs);
  const environment = { ...process.env };
  if (options.mode) environment.INKLOOP_FAKE_IDF_MODE = options.mode;
  const result = run("python3", args, { env: environment });
  return { ...result, output };
}

test("release builder help and fail-closed ownership are explicit", () => {
  const help = run("python3", [builder, "--help"]);
  assert.equal(help.status, 0, help.stderr);
  for (const option of [
    "--idf-path", "--public-defaults", "--output-dir",
    "--board-sku", "--firmware-version",
  ]) assert.match(help.stdout, new RegExp(option));
  assert.doesNotMatch(help.stdout, /--private|--sign|--flash|--deploy/i);
  assert.match(source, /SDKCONFIG_DEFAULTS/);
  assert.match(source, /-DINKLOOP_BOARD=m5_papercolor_c151/);
  assert.match(source, /app_data\[0\] != 0xE9/);
  assert.match(source, /manifest_url\.encode\("ascii"\) not in app_data/);
  assert.match(source, /public_key_hex\.encode\("ascii"\) not in app_data/);
  assert.match(source, /shutil\.rmtree\(output_directory\)/);
  assert.match(source, /os\.fsync\(directory_descriptor\)/);
  assert.doesNotMatch(source, /private[_-]?key|BEGIN [A-Z ]+ KEY|sign_ota_manifest/i);
  const developmentDefaults = fs.readFileSync(path.join(project, "sdkconfig.defaults"), "utf8");
  assert.doesNotMatch(developmentDefaults, /CONFIG_INKLOOP_OTA_/);
});

test("fake pinned IDF builds a deterministic credential-free verified receipt", () => {
  const f = fixture();
  try {
    const beforeDefaults = fs.readFileSync(f.defaults);
    const first = buildRelease(f);
    const second = buildRelease(f);
    assert.equal(first.status, 0, first.stderr);
    assert.equal(second.status, 0, second.stderr);
    const one = JSON.parse(first.stdout);
    const two = JSON.parse(second.stdout);
    assert.deepEqual(one, two);
    assert.deepEqual(Object.keys(one), [
      "schema_version", "board_sku", "firmware_version", "target",
      "app_size", "app_sha256", "ota_manifest_url",
      "ota_public_key_sha256", "ota_total_deadline_ms",
      "sdkconfig_sha256", "flash_args_sha256", "flasher_args_sha256",
    ]);
    assert.equal(one.board_sku, boardSku);
    assert.equal(one.firmware_version, version);
    assert.equal(one.target, "esp32s3");
    assert.equal(one.ota_manifest_url, manifestUrl);
    assert.equal(one.ota_total_deadline_ms, 120000);
    assert.equal(
      one.ota_public_key_sha256,
      crypto.createHash("sha256").update(Buffer.from(publicKey, "hex")).digest("hex"),
    );
    const app = fs.readFileSync(path.join(first.output, "inkloop_idf.bin"));
    assert.equal(one.app_size, app.length);
    assert.equal(one.app_sha256, crypto.createHash("sha256").update(app).digest("hex"));
    const receiptBytes = fs.readFileSync(
      path.join(first.output, "release-build-receipt.json"), "utf8");
    assert.deepEqual(JSON.parse(receiptBytes), one);
    assert.doesNotMatch(receiptBytes, new RegExp(publicKey));
    assert.doesNotMatch(receiptBytes, new RegExp(f.directory.replaceAll("/", "\\/")));
    assert.doesNotMatch(receiptBytes, /BEGIN|pem|private|export\.sh/i);
    assert.deepEqual(fs.readFileSync(f.defaults), beforeDefaults);
  } finally {
    fs.rmSync(f.directory, { recursive: true, force: true });
  }
});

test("release builder rejects a C151 image without OPI PSRAM", () => {
  const f = fixture();
  try {
    const result = buildRelease(f, { mode: "wrong_psram" });
    assert.equal(result.status, 2);
    assert.match(result.stderr, /generated_sdkconfig_mismatch/);
  } finally {
    fs.rmSync(f.directory, { recursive: true, force: true });
  }
});

test("malformed public defaults, SKU and version publish no output", () => {
  const f = fixture("inkloop-ota-release-defaults-");
  try {
    const invalid = [
      { text: defaultsText({ url: "https://evil.example.com/ota/m5-papercolor-c151/manifest.json" }), error: /invalid_ota_manifest_url/ },
      { text: defaultsText({ url: `${manifestUrl}?x=1` }), error: /invalid_ota_manifest_url/ },
      { text: defaultsText({ url: manifestUrl.replace("manifest.json", "0.4.0/manifest.json") }), error: /invalid_ota_manifest_url/ },
      { text: defaultsText({ key: publicKey.toUpperCase() }), error: /invalid_ota_public_key/ },
      { text: defaultsText({ key: "0".repeat(64) }), error: /invalid_ota_public_key/ },
      { text: defaultsText({ key: "a".repeat(62) }), error: /invalid_ota_public_key/ },
      { text: defaultsText({ deadline: 0 }), error: /invalid_ota_deadline/ },
      { text: defaultsText({ deadline: 120001 }), error: /invalid_ota_deadline/ },
      { text: defaultsText() + "CONFIG_UNEXPECTED=y\n", error: /invalid_public_defaults/ },
      { text: defaultsText().replace("CONFIG_INKLOOP_OTA_TOTAL_DEADLINE_MS=120000\n", ""), error: /invalid_public_defaults/ },
      { text: defaultsText().replace("CONFIG_INKLOOP_OTA_MANIFEST_URL", "CONFIG_INKLOOP_OTA_ED25519_PUBLIC_KEY_HEX"), error: /invalid_public_defaults/ },
    ];
    let index = 0;
    for (const entry of invalid) {
      const defaults = path.join(f.directory, `invalid-${index}.defaults`);
      fs.writeFileSync(defaults, entry.text);
      const result = buildRelease(f, {
        defaults,
        output: path.join(f.directory, `invalid-output-${index++}`),
      });
      assert.equal(result.status, 2, result.stderr);
      assert.match(result.stderr, entry.error);
      assert.equal(fs.existsSync(result.output), false);
    }
    const wrongSku = buildRelease(f, { boardSku: "mock_minimal" });
    assert.equal(wrongSku.status, 2);
    assert.match(wrongSku.stderr, /invalid_board_sku/);
    assert.equal(fs.existsSync(wrongSku.output), false);
    const wrongVersion = buildRelease(f, { version: "9.9.9" });
    assert.equal(wrongVersion.status, 2);
    assert.match(wrongVersion.stderr, /firmware_version_mismatch/);
    assert.equal(fs.existsSync(wrongVersion.output), false);
    const projectDefaults = buildRelease(f, {
      defaults: path.join(project, "sdkconfig.defaults"),
    });
    assert.equal(projectDefaults.status, 2);
    assert.match(projectDefaults.stderr, /public_defaults_inside_project/);
    assert.equal(fs.existsSync(projectDefaults.output), false);
  } finally {
    fs.rmSync(f.directory, { recursive: true, force: true });
  }
});

test("path, stale-output, IDF-version and tool failures fail closed", () => {
  const f = fixture("inkloop-ota-release-paths-");
  try {
    const existing = path.join(f.directory, "existing-output");
    fs.mkdirSync(existing);
    fs.writeFileSync(path.join(existing, "owner-data"), "preserve");
    const stale = buildRelease(f, { output: existing });
    assert.equal(stale.status, 2);
    assert.match(stale.stderr, /output_dir_exists/);
    assert.equal(fs.readFileSync(path.join(existing, "owner-data"), "utf8"), "preserve");

    const linkedDefaults = path.join(f.directory, "linked.defaults");
    fs.symlinkSync(f.defaults, linkedDefaults);
    const defaultsLink = buildRelease(f, { defaults: linkedDefaults });
    assert.equal(defaultsLink.status, 2);
    assert.match(defaultsLink.stderr, /invalid_public_defaults/);
    assert.equal(fs.existsSync(defaultsLink.output), false);

    const linkedIdf = path.join(f.directory, "linked-idf");
    fs.symlinkSync(f.idf, linkedIdf);
    const idfLink = buildRelease(f, { idf: linkedIdf });
    assert.equal(idfLink.status, 2);
    assert.match(idfLink.stderr, /invalid_idf_path/);
    assert.equal(fs.existsSync(idfLink.output), false);

    const relative = buildRelease(f, { output: "relative-build" });
    assert.equal(relative.status, 2);
    assert.match(relative.stderr, /invalid_output_dir_path/);
    const traversal = buildRelease(f, {
      output: `${f.directory}/unused/../traversal-build`,
    });
    assert.equal(traversal.status, 2);
    assert.match(traversal.stderr, /invalid_output_dir_path/);

    const realParent = path.join(f.directory, "real-parent");
    const linkedParent = path.join(f.directory, "linked-parent");
    fs.mkdirSync(realParent);
    fs.symlinkSync(realParent, linkedParent);
    const parentLink = buildRelease(f, {
      output: path.join(linkedParent, "build"),
    });
    assert.equal(parentLink.status, 2);
    assert.match(parentLink.stderr, /invalid_output_parent/);
    assert.equal(fs.existsSync(parentLink.output), false);

    const projectOutput = path.join(project, `.ws46-output-${process.pid}`);
    const insideProject = buildRelease(f, { output: projectOutput });
    assert.equal(insideProject.status, 2);
    assert.match(insideProject.stderr, /output_inside_project/);
    assert.equal(fs.existsSync(projectOutput), false);

    const privateOption = buildRelease(f, {
      extraArgs: ["--private-key", path.join(f.directory, "forbidden.pem")],
    });
    assert.equal(privateOption.status, 2);
    assert.match(privateOption.stderr, /unrecognized arguments: --private-key/);
    assert.equal(fs.existsSync(privateOption.output), false);

    assert.equal(run("git", ["-C", f.idf, "tag", "-d", "v6.0.2"]).status, 0);
    assert.equal(run("git", ["-C", f.idf, "tag", "v6.0.1"]).status, 0);
    const wrongIdf = buildRelease(f);
    assert.equal(wrongIdf.status, 2);
    assert.match(wrongIdf.stderr, /invalid_idf_version/);
    assert.equal(fs.existsSync(wrongIdf.output), false);
    assert.equal(run("git", ["-C", f.idf, "tag", "-d", "v6.0.1"]).status, 0);
    assert.equal(run("git", ["-C", f.idf, "tag", "v6.0.2"]).status, 0);

    const toolFailure = buildRelease(f, { mode: "tool_failure" });
    assert.equal(toolFailure.status, 2);
    assert.match(toolFailure.stderr, /idf_build_failed/);
    assert.equal(fs.existsSync(toolFailure.output), false);
  } finally {
    fs.rmSync(f.directory, { recursive: true, force: true });
  }
});

test("generated config, target, binary and flash-argument defects are removed", () => {
  const f = fixture("inkloop-ota-release-artifacts-");
  try {
    for (const mode of [
      "wrong_config", "duplicate_config", "wrong_target", "wrong_board",
      "wrong_version", "bad_flash", "missing_binary", "symlink_binary",
      "binary_missing_key",
    ]) {
      const result = buildRelease(f, { mode });
      assert.equal(result.status, 2, `${mode}: ${result.stderr}`);
      assert.match(
        result.stderr,
        /sdkconfig|target|project_description|flash_arguments|app_binary|ambiguous|invalid_/,
        mode,
      );
      assert.equal(fs.existsSync(result.output), false, mode);
    }
  } finally {
    fs.rmSync(f.directory, { recursive: true, force: true });
  }
});
