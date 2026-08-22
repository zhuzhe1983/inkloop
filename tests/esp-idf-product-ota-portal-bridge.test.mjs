import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const root = join(repo, "firmware/inkloop-idf/components/inkloop_product");
const header = readFileSync(
  join(root, "include/inkloop/native_portal_owner.hpp"),
  "utf8",
);
const runtimeHeader = readFileSync(
  join(root, "include/inkloop/product_runtime.hpp"),
  "utf8",
);
const source = readFileSync(join(root, "native_portal_owner.cpp"), "utf8");

function body(signature, nextSignature) {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, `missing ${signature}`);
  const end = source.indexOf(nextSignature, start + signature.length);
  assert.notEqual(end, -1, `missing ${nextSignature}`);
  return source.slice(start, end);
}

test("Product exposes only an atomic credential-free Portal OTA bridge", () => {
  assert.match(header, /class IPortalFirmwareUpdateOwner/);
  assert.match(header, /readPortalFirmwareUpdate/);
  assert.match(header, /requestPortalFirmwareUpdate/);
  assert.match(header, /IPortalFirmwareUpdateOwner\* firmware_update_owner_/);
  assert.match(runtimeHeader, /attachPortalFirmwareUpdateOwner/);

  const admission = body(
    "portal::PortalResult NativePortalOwner::tryEnqueue",
    "bool NativePortalOwner::takeCommand",
  );
  assert.match(admission, /PortalCommandType::RequestFirmwareUpdate/);
  assert.match(admission, /!firmware_update_owner_[\s\S]*Unavailable/);
  assert.match(admission, /update\.accepted_offline/);

  const dispatch = body(
    "void NativePortalOwner::serviceCommand",
    "bool NativePortalOwner::loadSettings",
  );
  assert.match(dispatch, /requestPortalFirmwareUpdate\([\s\S]*command\.request_id/);
  assert.doesNotMatch(dispatch, /OtaHttps|esp_ota|esp_http|esp_restart/);
});

test("Portal state copies a bounded OTA snapshot and fails closed without owner", () => {
  const refresh = body(
    "void NativePortalOwner::refreshState()",
    "void NativePortalOwner::refreshAlbum()",
  );
  assert.match(
    refresh,
    /next\.firmware_update = portal::PortalFirmwareUpdateSnapshot\{\}/,
  );
  assert.match(refresh, /readPortalFirmwareUpdate\(update\)/);
  assert.doesNotMatch(refresh, /manifest|public_key|signature|endpoint|token/i);
});
