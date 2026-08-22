import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { deflateSync } from "node:zlib";

const sourceRoot = new URL("../firmware/m5-papercolor/src/", import.meta.url);
const portalRoot = new URL(
  "../firmware/m5-papercolor/lib/InkloopPortal/", import.meta.url,
);

function compileAndRun(source, output, sanitized, runArguments = []) {
  const args = [
    "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
    "-I", sourceRoot.pathname, source, "-o", output,
  ];
  if (sanitized) args.unshift(
    "-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
  );
  const built = spawnSync(process.env.CXX || "c++", args, { encoding: "utf8" });
  assert.equal(built.status, 0, built.stderr || built.stdout);
  const ran = spawnSync(output, runArguments, {
    encoding: "utf8",
    env: sanitized ? {
      ...process.env,
      ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1",
      UBSAN_OPTIONS: "halt_on_error=1:print_stacktrace=1",
    } : process.env,
  });
  assert.equal(ran.status, 0, ran.stderr || ran.stdout);
}

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
  }
  const output = Buffer.alloc(4);
  output.writeUInt32BE((crc ^ 0xffffffff) >>> 0);
  return output;
}

function pngChunk(type, payload) {
  const name = Buffer.from(type, "ascii");
  const length = Buffer.alloc(4);
  length.writeUInt32BE(payload.length);
  return Buffer.concat([length, name, payload, crc32(Buffer.concat([name, payload]))]);
}

const pngSignature = Buffer.from([
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
]);

function canvasStylePaperColorPng() {
  const width = 400;
  const height = 600;
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(height, 4);
  header[8] = 8; // Browser canvas-style RGBA, 8 bits per sample.
  header[9] = 6;
  const scanlines = Buffer.alloc((width * 4 + 1) * height);
  return Buffer.concat([
    pngSignature,
    pngChunk("IHDR", header),
    pngChunk("IDAT", deflateSync(scanlines)),
    pngChunk("IEND", Buffer.alloc(0)),
  ]);
}

test("upload PNG, title, and chunk caps reject adversarial inputs", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "inkloop-upload-"));
  try {
    const source = join(temporary, "upload.cpp");
    const executable = join(temporary, "upload");
    const fixture = join(temporary, "canvas-400x600.png");
    const noIdatFixture = join(temporary, "no-idat.png");
    const emptyIdatFixture = join(temporary, "empty-idat.png");
    const badIhdrCrcFixture = join(temporary, "bad-ihdr-crc.png");
    const badCrcFixture = join(temporary, "bad-crc.png");
    const overrunFixture = join(temporary, "chunk-overrun.png");
    const trailingFixture = join(temporary, "trailing.png");
    const wrongOrderFixture = join(temporary, "wrong-order.png");
    const real = canvasStylePaperColorPng();
    const ihdr = real.subarray(8, 33);
    const iend = pngChunk("IEND", Buffer.alloc(0));
    await writeFile(fixture, real);
    await writeFile(noIdatFixture, Buffer.concat([pngSignature, ihdr, iend]));
    await writeFile(emptyIdatFixture, Buffer.concat([
      pngSignature, ihdr, pngChunk("IDAT", Buffer.alloc(0)), iend,
    ]));
    const badIhdrCrc = Buffer.from(real);
    badIhdrCrc[32] ^= 0x01;
    await writeFile(badIhdrCrcFixture, badIhdrCrc);
    const badCrc = Buffer.from(real);
    badCrc[45] ^= 0x01;
    await writeFile(badCrcFixture, badCrc);
    const overrun = Buffer.from(real);
    overrun.writeUInt32BE(0x7fffffff, 33);
    await writeFile(overrunFixture, overrun);
    await writeFile(trailingFixture, Buffer.concat([real, Buffer.from([0])]));
    const idatLength = real.readUInt32BE(33);
    const idat = real.subarray(33, 33 + 12 + idatLength);
    await writeFile(wrongOrderFixture, Buffer.concat([
      pngSignature, ihdr, idat, pngChunk("tEXt", Buffer.from("closed")), idat,
      iend,
    ]));
    await writeFile(source, String.raw`
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "AlbumUploadPrimitives.h"

struct FakeBackend {
  std::vector<uint8_t> part;
  size_t removals = 0;
  bool prepare(bool uploadActive) {
    if (!inkloop::albumPrepareMayMutate(uploadActive)) return false;
    part.clear();
    ++removals;
    return true;
  }
};

std::vector<uint8_t> load(const char* path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<uint8_t>(
      (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool validatesIncrementally(const std::vector<uint8_t>& png) {
  inkloop::PaperColorPngStreamValidator validator(1500000);
  static const size_t chunks[] = {1, 2, 3, 7, 31, 257, 1024};
  size_t offset = 0;
  size_t cycle = 0;
  while (offset < png.size()) {
    size_t amount = chunks[cycle++ % (sizeof(chunks) / sizeof(chunks[0]))];
    if (amount > png.size() - offset) amount = png.size() - offset;
    if (!validator.append(png.data() + offset, amount)) return false;
    offset += amount;
  }
  return validator.finish(png.size());
}

int main(int argc, char** argv) {
  using namespace inkloop;
  assert(argc == 9);
  const std::vector<uint8_t> png = load(argv[1]);
  assert(png.size() >= 45);
  PaperColorPngHeader portrait = parsePaperColorPngHeader(png.data(), png.size());
  assert(portrait.valid && !portrait.landscape);
  assert(validPaperColorPng(png.data(), png.size()));
  assert(validatesIncrementally(png));
  assert(validPaperColorPngTrailer(png.data(), png.size()));
  for (int index = 2; index < argc; ++index) {
    const std::vector<uint8_t> attack = load(argv[index]);
    assert(!validatesIncrementally(attack));
    assert(!validPaperColorPng(attack.data(), attack.size()));
  }
  std::vector<uint8_t> landscapePng = png;
  landscapePng[18] = 2; landscapePng[19] = 88;
  landscapePng[22] = 1; landscapePng[23] = 144;
  PaperColorPngHeader landscape = parsePaperColorPngHeader(
      landscapePng.data(), landscapePng.size());
  assert(landscape.valid && landscape.landscape);
  std::vector<uint8_t> bad = png;
  bad[19] = 145;
  assert(!parsePaperColorPngHeader(bad.data(), bad.size()).valid);
  bad = png; bad[8] = 1;
  assert(!parsePaperColorPngHeader(bad.data(), bad.size()).valid);
  bad = png; bad[12] = 'X';
  assert(!parsePaperColorPngHeader(bad.data(), bad.size()).valid);
  bad = png; bad[24] = 4; bad[25] = 2;
  assert(!parsePaperColorPngHeader(bad.data(), bad.size()).valid);
  bad = png; bad[26] = 1;
  assert(!parsePaperColorPngHeader(bad.data(), bad.size()).valid);
  bad = png; bad[27] = 1;
  assert(!parsePaperColorPngHeader(bad.data(), bad.size()).valid);
  bad = png; bad[28] = 2;
  assert(!parsePaperColorPngHeader(bad.data(), bad.size()).valid);
  bad = png; bad.pop_back();
  assert(!validPaperColorPng(bad.data(), bad.size()));
  const uint8_t shortFake[24] = {0x89,'P','N','G',13,10,26,10};
  assert(!parsePaperColorPngHeader(shortFake, sizeof(shortFake)).valid);
  assert(!validPaperColorPngTrailer(shortFake, sizeof(shortFake)));
  assert(boundedUploadAppend(0, 24, 1500000, 1500000));
  assert(boundedUploadAppend(1499999, 1, 1500000, 1500000));
  assert(!boundedUploadAppend(1499999, 2, 1500000, 1500000));
  assert(!boundedUploadAppend(0, 1, 1500001, 1500000));
  assert(validUploadTitle("paper color.png", 64));
  assert(!validUploadTitle("../escape.png", 64));
  assert(!validUploadTitle("bad<script>.png", 64));
  assert(!validUploadTitle(std::string(65, 'A'), 64));
  FakeBackend backend;
  backend.part.assign(png.begin(), png.begin() + 64);
  const std::vector<uint8_t> preserved = backend.part;
  assert(!backend.prepare(true));
  assert(backend.removals == 0 && backend.part == preserved);
  assert(backend.prepare(false));
  assert(backend.removals == 1 && backend.part.empty());
  return 0;
}
`, "utf8");
    const runArgs = [fixture, noIdatFixture, emptyIdatFixture,
      badIhdrCrcFixture, badCrcFixture, overrunFixture, trailingFixture,
      wrongOrderFixture];
    compileAndRun(source, executable, false, runArgs);
    compileAndRun(source, `${executable}-sanitized`, true, runArgs);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("Portal upload is streamed, authenticated, transactional, and UI-bounded", async () => {
  const [portal, portalHeader, runtime, runtimeHeader, application, album, main] =
    await Promise.all([
      readFile(new URL("InkloopPortal.cpp", portalRoot), "utf8"),
      readFile(new URL("InkloopPortal.h", portalRoot), "utf8"),
      readFile(new URL("PaperColorPortalRuntime.cpp", sourceRoot), "utf8"),
      readFile(new URL("PaperColorPortalRuntime.h", sourceRoot), "utf8"),
      readFile(new URL("PaperColorApplicationRuntime.cpp", sourceRoot), "utf8"),
      readFile(new URL("AlbumStore.cpp", sourceRoot), "utf8"),
      readFile(new URL("main.cpp", sourceRoot), "utf8"),
    ]);
  assert.match(portalHeader, /authorizeStreamingAlbumUpload/);
  assert.match(portal, /authorizeStreamingAlbumUpload[\s\S]*sessionAuthorized/);
  assert.match(portal, /authorizeStreamingAlbumUpload[\s\S]*mutationAuthorized/);
  assert.match(portal, /multipart\/form-data[\s\S]*boundary=/);
  assert.match(portal, /canvas\.toBlob/);
  assert.match(portal, /Math\.max\(targetW\/img\.width,targetH\/img\.height\)/);
  assert.match(portal, /X-Inkloop-Image-Bytes/);
  assert.match(portal, /fetch\("\/api\/album"/);
  assert.match(portal, /used=total-remaining/);
  assert.match(portal, /activeMounted===true&&state\.storage\.activeWritable===true/);
  assert.match(portal, /未挂载或不可写，需要恢复后才能上传/);
  assert.doesNotMatch(portal, /eval\s*\(|https:\/\/.*\.js/);
  assert.match(runtimeHeader, /beginAlbumUpload[\s\S]*writeAlbumUpload[\s\S]*finishAlbumUpload/);
  assert.match(runtime, /UPLOAD_FILE_START[\s\S]*authorizeStreamingAlbumUpload/);
  assert.match(runtime, /X-Inkloop-Image-Bytes[\s\S]*Content-Length/);
  assert.match(runtime, /imageBytes < 45/);
  assert.match(runtime, /UPLOAD_FILE_WRITE[\s\S]*writeAlbumUpload/);
  assert.match(runtime, /UPLOAD_FILE_ABORTED[\s\S]*abortAlbumUpload/);
  assert.match(album, /kAssetPartPath[\s\S]*FILE_WRITE/);
  assert.match(album, /uploadBytes_ != uploadMaximumBytes_/);
  assert.match(album, /PaperColorPngStreamValidator validator\(expectedBytes\)/);
  assert.match(album, /uint8_t buffer\[1024\]/);
  assert.doesNotMatch(album, /heap_caps_malloc\(expectedBytes/);
  assert.match(album, /uploadValidator_\.append\(bytes, length\)/);
  assert.match(album, /uploadValidator_\.finish\(uploadMaximumBytes_\)/);
  assert.match(album, /validateAssetFile[\s\S]*sha256File[\s\S]*rename[\s\S]*commitIndex/);
  assert.match(album, /ALBUM_INDEX_COMMIT_RETRY/);
  assert.match(album, /recoverTransactionalRecord/);
  assert.match(album, /commitValidatedRecordDetailed/);
  assert.match(album, /ALBUM_INDEX_COMMIT_FAILED/);
  assert.match(album, /duplicate[\s\S]*validateAssetFile[\s\S]*sha256File/);
  assert.match(application, /mutationBusy\(\) const[\s\S]*userUploadActive/);
  assert.match(application, /userUploadActive\(\)[\s\S]*externalPagePending/);
  assert.match(application, /beginAlbumUpload[\s\S]*ImageLedState::Downloading/);
  assert.match(application, /finishAlbumUpload[\s\S]*ImageLedState::Writing[\s\S]*ImageLedState::Complete/);
  const myAiAdapters = await readFile(
    new URL("PaperColorMyAiAdapters.cpp", sourceRoot), "utf8",
  );
  assert.match(myAiAdapters, /quartetLength_ == 2 \|\| quartetLength_ == 3/);
  assert.match(myAiAdapters, /while \(quartetLength_ < 4\) quartet_\[quartetLength_\+\+\] = '='/);
  assert.match(myAiAdapters, /HTTPClient::getStreamPtr\(\)[\s\S]*chunk-size lines/);
  assert.match(myAiAdapters, /http\.writeToStream\(&decoder\)/);
  const outputTransport = myAiAdapters.slice(
    myAiAdapters.indexOf("Status Esp32AigcOutputTransport::postAndDecodeBase64"),
    myAiAdapters.indexOf("Status AlbumImageSink::begin"),
  );
  assert.doesNotMatch(outputTransport, /getStreamPtr\(\)/);
  assert.match(application, /readAlbumPage[\s\S]*userUploadActive\(\)[\s\S]*readCatalogPage/);
  assert.match(application, /findAlbumItem[\s\S]*userUploadActive\(\)[\s\S]*findCatalogEntry/);
  assert.match(runtime, /authorizeStreamingAlbumPreview/);
  assert.match(runtime, /streamFile\(\*work->file, "image\/png"\)/);
  assert.match(runtime, /ResponsiveWorkKind::PortalTransfer/);
  assert.match(runtime, /X-Content-Type-Options/);
  assert.match(application, /openAlbumPreview[\s\S]*mutationBusy\(\)[\s\S]*findCatalogEntry[\s\S]*FILE_READ/);
  assert.match(application, /readAllEntries[\s\S]*userUploadActive\(\)[\s\S]*readCatalogPage/);
  const prepare = album.slice(album.indexOf("bool AlbumStore::prepare("),
    album.indexOf("bool AlbumStore::begin()"));
  assert.match(prepare, /albumPrepareMayMutate\(uploadActive_\)/);
  assert.ok(prepare.indexOf("albumPrepareMayMutate(uploadActive_)") <
    prepare.indexOf("storage.remove(kAssetPartPath)"));
  const due = main.slice(main.indexOf("bool runDueTasks()"),
    main.indexOf("bool processPendingPage()"));
  assert.match(due, /mutationBusy\(\)/);
  assert.ok(due.indexOf("mutationBusy()") < due.indexOf("downloadFrame("));
  const page = main.slice(main.indexOf("bool processPendingPage()"),
    main.indexOf("void printDiagnosticStatus()", main.indexOf("bool processPendingPage()")));
  assert.match(page, /albumUploadActive\(\)[\s\S]*album\.loadPage/);
  const buttonStart = main.indexOf("void onButton(");
  const button = main.slice(buttonStart,
    main.indexOf("bool queueRuntimePage(", buttonStart));
  assert.match(button, /albumUploadActive\(\)[\s\S]*album\.pageState/);
  const voiceQueue = main.slice(main.indexOf("bool queueRuntimePage("),
    main.indexOf("bool safeShowStatus("));
  assert.match(voiceQueue, /albumUploadActive\(\)/);
  const setup = main.slice(main.indexOf("void setup()"), main.indexOf("void loop()"));
  assert.match(setup, /const bool boardReady = board\.begin\(\)/);
  assert.match(setup, /if \(!boardReady\) haltCritical/);
  assert.ok(setup.indexOf("if (!boardReady) haltCritical") <
    setup.indexOf("beginSettingsWithRetry()"));
  assert.ok(setup.indexOf("if (!boardReady) haltCritical") <
    setup.indexOf("leds.begin()"));
  assert.ok(setup.indexOf("if (!boardReady) haltCritical") <
    setup.indexOf("beginWifiProvisioning()"));
});
