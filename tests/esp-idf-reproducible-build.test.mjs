import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import {
  chmod,
  mkdir,
  mkdtemp,
  readFile,
  realpath,
  rm,
  writeFile,
} from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";
import test from "node:test";

const firmware = new URL("../firmware/inkloop-idf/", import.meta.url);
const productionVerifier = new URL(
  "../firmware/inkloop-idf/tools/verify_reproducible_builds.sh",
  import.meta.url,
).pathname;
const expectedIdfTag = "v6.0.2";
const expectedIdfCommit = "7101770dc6db2667b3c477cc31365dd1acd6db4e";

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    encoding: "utf8",
    ...options,
  });
  assert.equal(result.error, undefined, result.error?.message);
  return result;
}

function commitChanges(directory, message) {
  assert.equal(run("git", ["add", "."], { cwd: directory }).status, 0);
  assert.equal(
    run(
      "git",
      [
        "-c",
        "user.name=Inkloop Test",
        "-c",
        "user.email=inkloop-test@example.invalid",
        "commit",
        "-qm",
        message,
      ],
      { cwd: directory },
    ).status,
    0,
  );
  const commit = run("git", ["rev-parse", "HEAD"], { cwd: directory });
  assert.equal(commit.status, 0, commit.stderr);
  return commit.stdout.trim();
}

function initializeAndCommit(directory, message) {
  assert.equal(run("git", ["init", "-q"], { cwd: directory }).status, 0);
  return commitChanges(directory, message);
}

async function createCleanProject(directory, idfCommit) {
  const tools = join(directory, "tools");
  await mkdir(tools, { recursive: true });
  const [verifierSource, versionPin] = await Promise.all([
    readFile(productionVerifier, "utf8"),
    readFile(new URL(".idf-version", firmware), "utf8"),
  ]);
  const verifier = join(tools, "verify_reproducible_builds.sh");
  await Promise.all([
    writeFile(verifier, verifierSource),
    writeFile(join(directory, ".idf-version"), versionPin),
    // This unit-test project pins its fake checkout's real commit. The source
    // contract test separately guards the immutable production pin.
    writeFile(join(directory, ".idf-commit"), `${idfCommit}\n`),
  ]);
  await chmod(verifier, 0o755);
  const commit = initializeAndCommit(directory, "clean test project");
  const status = run("git", ["status", "--porcelain"], { cwd: directory });
  assert.equal(status.status, 0, status.stderr);
  assert.equal(status.stdout, "");
  return { commit, verifier };
}

async function createFakeIdf(directory, { tag = expectedIdfTag } = {}) {
  const tools = join(directory, "tools");
  await mkdir(tools, { recursive: true });
  await writeFile(
    join(directory, "export.sh"),
    [
      "#!/usr/bin/env bash",
      "export FAKE_IDF_EXPORTED=1",
      "",
    ].join("\n"),
  );
  const fakeIdf = join(tools, "idf.py");
  await writeFile(
    fakeIdf,
    [
      "#!/usr/bin/env bash",
      "set -euo pipefail",
      'if [[ "${IDF_COMPONENT_STRICT_CHECKSUM:-}" != "1" ]]; then',
      '  echo "strict component checksum was not enabled" >&2',
      "  exit 86",
      "fi",
      'project_dir=""',
      'build_dir=""',
      'board=""',
      'sdkconfig=""',
      'while [[ "$#" -gt 0 ]]; do',
      '  case "$1" in',
      '    -C) project_dir="$2"; shift 2 ;;',
      '    -B) build_dir="$2"; shift 2 ;;',
      '    -DIDF_TARGET=esp32s3) shift ;;',
      '    -DINKLOOP_BOARD=*) board="${1#*=}"; shift ;;',
      '    -DSDKCONFIG=*) sdkconfig="${1#*=}"; shift ;;',
      '    build) shift ;;',
      '    *) echo "unexpected fake idf argument: $1" >&2; exit 2 ;;',
      "  esac",
      "done",
      '[[ -n "$project_dir" && -n "$build_dir" && -n "$board" && -n "$sdkconfig" ]]',
      'mkdir -p "$build_dir"',
      'printf "%s\\n" \\',
      '  "CONFIG_APP_REPRODUCIBLE_BUILD=y" \\',
      '  "CONFIG_IDF_TARGET=\\\"esp32s3\\\"" > "$sdkconfig"',
      'if [[ "${FAKE_COMPILE_TIME:-0}" == "1" ]]; then',
      '  printf "%s\\n" "CONFIG_APP_COMPILE_TIME_DATE=y" >> "$sdkconfig"',
      "fi",
      'printf "INKLOOP_BOARD:STRING=%s\\n" "$board" > "$build_dir/CMakeCache.txt"',
      'payload="$board"',
      'if [[ "${FAKE_NON_REPRODUCIBLE:-0}" == "1" ]]; then',
      '  payload="${payload}:${build_dir}"',
      "fi",
      'printf "%s" "$payload" > "$build_dir/inkloop_idf.bin"',
      "",
    ].join("\n"),
  );
  await chmod(fakeIdf, 0o755);

  const fixtureCommit = initializeAndCommit(directory, "fake idf");
  if (tag !== null) {
    assert.equal(run("git", ["tag", tag, "HEAD"], { cwd: directory }).status, 0);
  }
  const status = run("git", ["status", "--porcelain"], { cwd: directory });
  assert.equal(status.status, 0, status.stderr);
  assert.equal(status.stdout, "");
  return fixtureCommit;
}

test("development defaults and host dependency are explicit and reproducible", async () => {
  const [
    defaults,
    primitive,
    helper,
    platformio,
    manifestText,
    idfVersion,
    idfCommit,
    verifierSource,
  ] =
    await Promise.all([
      readFile(new URL("sdkconfig.defaults", firmware), "utf8"),
      readFile(
        new URL("../tests/papercolor-firmware-primitives.test.mjs", import.meta.url),
        "utf8",
      ),
      readFile(
        new URL("../tests/support/arduinojson-host-dependency.mjs", import.meta.url),
        "utf8",
      ),
      readFile(
        new URL("../firmware/m5-papercolor/platformio.ini", import.meta.url),
        "utf8",
      ),
      readFile(
        new URL("../tests/fixtures/host-cpp-dependencies.json", import.meta.url),
        "utf8",
      ),
      readFile(new URL(".idf-version", firmware), "utf8"),
      readFile(new URL(".idf-commit", firmware), "utf8"),
      readFile(productionVerifier, "utf8"),
    ]);

  assert.equal(
    defaults.match(/^CONFIG_APP_REPRODUCIBLE_BUILD=y$/gm)?.length,
    1,
  );
  assert.match(defaults, /^# CONFIG_APP_COMPILE_TIME_DATE is not set$/m);
  assert.match(defaults, /^# CONFIG_BOOTLOADER_COMPILE_TIME_DATE is not set$/m);
  assert.match(primitive, /materializeArduinoJson/);
  assert.doesNotMatch(primitive, /\.pio|libdeps/);
  assert.doesNotMatch(helper, /\.pio|libdeps/);
  assert.match(platformio, /^\s*bblanchon\/ArduinoJson @ 7\.4\.3$/m);
  assert.equal(idfVersion, `${expectedIdfTag}\n`);
  assert.equal(idfCommit, `${expectedIdfCommit}\n`);
  assert.match(verifierSource, /\.idf-commit/);
  assert.match(verifierSource, /GIT_NO_REPLACE_OBJECTS=1/);
  assert.match(verifierSource, /idf_py_candidate="\$\{idf_path\}\/tools\/idf\.py"/);
  assert.doesNotMatch(verifierSource, /IDF_PY:-/);
  assert.match(verifierSource, /IDF_COMPONENT_STRICT_CHECKSUM=1/);
  assert.match(verifierSource, /PROJECT_COMMIT=%s/);
  assert.match(verifierSource, /IDF_COMMIT=%s/);

  const manifest = JSON.parse(manifestText);
  assert.deepEqual(manifest, {
    schema: 1,
    arduinoJson: {
      version: "7.4.3",
      tagCommit: "77771d3c07668e01d8f52acb03910c1110bb373f",
      upstreamUrl:
        "https://github.com/bblanchon/ArduinoJson/releases/tag/v7.4.3",
      archive: "tests/vendor/arduinojson-7.4.3-src.tar.gz",
      archiveBytes: 60406,
      archiveSha256:
        "2b4eb3accb35d2b2b9f688b4f52aeef0cb489236e441a0a77ae1a0e0dc56a739",
      sourceFiles: 141,
      libraryJsonSha256:
        "45c8072598d097cf03031ee4f3230031105329903e022a2abd15b1c56157c770",
      license: "MIT",
      licenseSha256:
        "4a7ee9c96b28cbf30c5bf7c2d211a0ef57179f0328e68ad7b7fa7d754b7da1a2",
    },
  });
  assert.doesNotMatch(helper, /\bfetch\s*\(|\.pio|libdeps/);
});

test("reproducibility verifier accepts matching isolated target builds", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-repro-verifier-"));
  const project = join(temporary, "project");
  const fakeIdf = join(temporary, "esp-idf");
  await Promise.all([mkdir(project), mkdir(fakeIdf)]);
  try {
    const idfCommit = await createFakeIdf(fakeIdf);
    const projectState = await createCleanProject(project, idfCommit);
    const output = join(temporary, "evidence");
    const verified = run(projectState.verifier, [output], {
      cwd: project,
      env: { ...process.env, IDF_PATH: fakeIdf },
    });
    assert.equal(verified.status, 0, verified.stderr || verified.stdout);

    const c151Hash = createHash("sha256")
      .update("m5_papercolor_c151")
      .digest("hex");
    const mockHash = createHash("sha256")
      .update("mock_minimal")
      .digest("hex");
    assert.match(verified.stdout, new RegExp(`^C151_SHA256=${c151Hash}$`, "m"));
    assert.match(verified.stdout, /^C151_BYTES=18$/m);
    assert.match(verified.stdout, new RegExp(`^MOCK_SHA256=${mockHash}$`, "m"));
    assert.match(verified.stdout, /^MOCK_BYTES=12$/m);
    assert.match(
      verified.stdout,
      new RegExp(`^PROJECT_COMMIT=${projectState.commit}$`, "m"),
    );
    assert.match(
      verified.stdout,
      new RegExp(`^IDF_COMMIT=${idfCommit}$`, "m"),
    );
    assert.match(
      verified.stdout,
      new RegExp(`^EVIDENCE_ROOT=${await realpath(output)}$`, "m"),
    );
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("reproducibility verifier does not allow IDF_PY injection", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-repro-idf-py-"));
  const project = join(temporary, "project");
  const fakeIdf = join(temporary, "esp-idf");
  const injectedIdfPy = join(temporary, "injected-idf.py");
  await Promise.all([mkdir(project), mkdir(fakeIdf)]);
  try {
    const idfCommit = await createFakeIdf(fakeIdf);
    const projectState = await createCleanProject(project, idfCommit);
    await writeFile(
      injectedIdfPy,
      [
        "#!/usr/bin/env bash",
        'echo "IDF_PY injection executed" >&2',
        "exit 97",
        "",
      ].join("\n"),
    );
    await chmod(injectedIdfPy, 0o755);

    const verified = run(projectState.verifier, [join(temporary, "evidence")], {
      cwd: project,
      env: {
        ...process.env,
        IDF_PATH: fakeIdf,
        IDF_PY: injectedIdfPy,
      },
    });
    assert.equal(verified.status, 0, verified.stderr || verified.stdout);
    assert.doesNotMatch(verified.stderr, /IDF_PY injection executed/);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("reproducibility verifier forces strict component checksums", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-repro-strict-"));
  const project = join(temporary, "project");
  const fakeIdf = join(temporary, "esp-idf");
  await Promise.all([mkdir(project), mkdir(fakeIdf)]);
  try {
    const idfCommit = await createFakeIdf(fakeIdf);
    const projectState = await createCleanProject(project, idfCommit);
    const verified = run(projectState.verifier, [join(temporary, "evidence")], {
      cwd: project,
      env: {
        ...process.env,
        IDF_PATH: fakeIdf,
        IDF_COMPONENT_STRICT_CHECKSUM: "0",
      },
    });
    assert.equal(verified.status, 0, verified.stderr || verified.stdout);
    assert.doesNotMatch(verified.stderr, /strict component checksum was not enabled/);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("reproducibility verifier rejects a dirty project worktree", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-repro-project-dirty-"));
  const project = join(temporary, "project");
  const fakeIdf = join(temporary, "esp-idf");
  await Promise.all([mkdir(project), mkdir(fakeIdf)]);
  try {
    const idfCommit = await createFakeIdf(fakeIdf);
    const projectState = await createCleanProject(project, idfCommit);
    await writeFile(join(project, "uncommitted.txt"), "dirty\n");
    const rejected = run(projectState.verifier, [join(temporary, "evidence")], {
      cwd: project,
      env: { ...process.env, IDF_PATH: fakeIdf },
    });
    assert.equal(rejected.status, 2, rejected.stderr || rejected.stdout);
    assert.match(rejected.stderr, /Project Git worktree must be clean/);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("reproducibility verifier rejects a different ESP-IDF commit", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-repro-idf-commit-"));
  const project = join(temporary, "project");
  const fakeIdf = join(temporary, "esp-idf");
  await Promise.all([mkdir(project), mkdir(fakeIdf)]);
  try {
    const pinnedIdfCommit = await createFakeIdf(fakeIdf);
    const projectState = await createCleanProject(project, pinnedIdfCommit);
    await writeFile(join(fakeIdf, "new-head.txt"), "different commit\n");
    const actualIdfCommit = commitChanges(fakeIdf, "different fake idf commit");
    const rejected = run(projectState.verifier, [join(temporary, "evidence")], {
      cwd: project,
      env: { ...process.env, IDF_PATH: fakeIdf },
    });
    assert.equal(rejected.status, 2, rejected.stderr || rejected.stdout);
    assert.match(rejected.stderr, /ESP-IDF commit mismatch/);
    assert.match(rejected.stderr, new RegExp(pinnedIdfCommit));
    assert.match(rejected.stderr, new RegExp(actualIdfCommit));
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("reproducibility verifier rejects a different ESP-IDF tag", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-repro-idf-tag-"));
  const project = join(temporary, "project");
  const fakeIdf = join(temporary, "esp-idf");
  await Promise.all([mkdir(project), mkdir(fakeIdf)]);
  try {
    const idfCommit = await createFakeIdf(fakeIdf, { tag: "v6.0.1" });
    const projectState = await createCleanProject(project, idfCommit);
    const rejected = run(projectState.verifier, [join(temporary, "evidence")], {
      cwd: project,
      env: { ...process.env, IDF_PATH: fakeIdf },
    });
    assert.equal(rejected.status, 2, rejected.stderr || rejected.stdout);
    assert.match(rejected.stderr, /ESP-IDF tag mismatch/);
    assert.match(rejected.stderr, new RegExp(expectedIdfTag.replaceAll(".", "\\.")));
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("reproducibility verifier rejects a dirty ESP-IDF worktree", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-repro-idf-dirty-"));
  const project = join(temporary, "project");
  const fakeIdf = join(temporary, "esp-idf");
  await Promise.all([mkdir(project), mkdir(fakeIdf)]);
  try {
    const idfCommit = await createFakeIdf(fakeIdf);
    const projectState = await createCleanProject(project, idfCommit);
    await writeFile(join(fakeIdf, "uncommitted.txt"), "dirty\n");
    const rejected = run(projectState.verifier, [join(temporary, "evidence")], {
      cwd: project,
      env: { ...process.env, IDF_PATH: fakeIdf },
    });
    assert.equal(rejected.status, 2, rejected.stderr || rejected.stdout);
    assert.match(rejected.stderr, /ESP-IDF Git worktree must be clean/);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("reproducibility verifier rejects ESP-IDF replacement refs", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-repro-idf-replace-"));
  const project = join(temporary, "project");
  const fakeIdf = join(temporary, "esp-idf");
  await Promise.all([mkdir(project), mkdir(fakeIdf)]);
  try {
    const idfCommit = await createFakeIdf(fakeIdf);
    const projectState = await createCleanProject(project, idfCommit);
    const replacement = run(
      "git",
      [
        "-c",
        "user.name=Inkloop Test",
        "-c",
        "user.email=inkloop-test@example.invalid",
        "commit-tree",
        "HEAD^{tree}",
        "-m",
        "replacement object",
      ],
      { cwd: fakeIdf },
    );
    assert.equal(replacement.status, 0, replacement.stderr);
    assert.notEqual(replacement.stdout.trim(), idfCommit);
    assert.equal(
      run(
        "git",
        [
          "update-ref",
          `refs/replace/${idfCommit}`,
          replacement.stdout.trim(),
        ],
        { cwd: fakeIdf },
      ).status,
      0,
    );

    const rejected = run(projectState.verifier, [join(temporary, "evidence")], {
      cwd: project,
      env: { ...process.env, IDF_PATH: fakeIdf },
    });
    assert.equal(rejected.status, 2, rejected.stderr || rejected.stdout);
    assert.match(
      rejected.stderr,
      /ESP-IDF Git worktree must not contain refs\/replace entries/,
    );
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("reproducibility verifier rejects path-dependent output", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-repro-mismatch-"));
  const project = join(temporary, "project");
  const fakeIdf = join(temporary, "esp-idf");
  await Promise.all([mkdir(project), mkdir(fakeIdf)]);
  try {
    const idfCommit = await createFakeIdf(fakeIdf);
    const projectState = await createCleanProject(project, idfCommit);
    const rejected = run(projectState.verifier, [join(temporary, "evidence")], {
      cwd: project,
      env: {
        ...process.env,
        IDF_PATH: fakeIdf,
        FAKE_NON_REPRODUCIBLE: "1",
      },
    });
    assert.equal(rejected.status, 1, rejected.stderr || rejected.stdout);
    assert.match(rejected.stderr, /C151 reproducibility mismatch/);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("reproducibility verifier rejects embedded compile time", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-repro-date-"));
  const project = join(temporary, "project");
  const fakeIdf = join(temporary, "esp-idf");
  await Promise.all([mkdir(project), mkdir(fakeIdf)]);
  try {
    const idfCommit = await createFakeIdf(fakeIdf);
    const projectState = await createCleanProject(project, idfCommit);
    const rejected = run(projectState.verifier, [join(temporary, "evidence")], {
      cwd: project,
      env: {
        ...process.env,
        IDF_PATH: fakeIdf,
        FAKE_COMPILE_TIME: "1",
      },
    });
    assert.equal(rejected.status, 1, rejected.stderr || rejected.stdout);
    assert.match(rejected.stderr, /app compile time is embedded/);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});
