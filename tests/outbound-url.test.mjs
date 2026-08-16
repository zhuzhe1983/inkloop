import test from "node:test";
import assert from "node:assert/strict";

import { outboundUrl } from "../app/lib/outbound-url.ts";
import { isAllowedOutboundHost, resolveOutboundTarget } from "../docker/llm-proxy.mjs";

test("outboundUrl leaves direct HTTPS URLs unchanged without a proxy", () => {
  const target = "https://loremflickr.com/528/792/fashion,model/all?lock=1";
  assert.equal(outboundUrl(target), target);
  assert.equal(outboundUrl(target, "   "), target);
});

test("outboundUrl wraps artwork and weather URLs for the Docker proxy", () => {
  const target = "https://loremflickr.com/528/792/fashion,model/all?lock=1";
  const proxied = outboundUrl(target, "http://llm-proxy:8788/outbound");
  const parsed = new URL(proxied);
  assert.equal(parsed.origin + parsed.pathname, "http://llm-proxy:8788/outbound");
  assert.equal(parsed.searchParams.get("url"), target);
});

test("outbound proxy only allows known public artwork and weather hosts", () => {
  assert.equal(isAllowedOutboundHost("loremflickr.com"), true);
  assert.equal(isAllowedOutboundHost("live.staticflickr.com"), true);
  assert.equal(isAllowedOutboundHost("commons.wikimedia.org"), true);
  assert.equal(isAllowedOutboundHost("upload.wikimedia.org"), true);
  assert.equal(isAllowedOutboundHost("fastly.picsum.photos"), true);
  assert.equal(isAllowedOutboundHost("geocoding-api.open-meteo.com"), true);
  assert.equal(isAllowedOutboundHost("wttr.in"), true);
  assert.equal(isAllowedOutboundHost("example.com"), false);
  assert.equal(isAllowedOutboundHost("127.0.0.1"), false);
  assert.equal(resolveOutboundTarget("https://loremflickr.com/528/792/cat").href, "https://loremflickr.com/528/792/cat");
  assert.equal(resolveOutboundTarget("http://loremflickr.com/528/792/cat"), null);
  assert.equal(resolveOutboundTarget("https://evil.example/x"), null);
  assert.equal(resolveOutboundTarget("https://loremflickr.com:8443/x"), null);
});
