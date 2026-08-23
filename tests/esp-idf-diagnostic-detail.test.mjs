import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const component = join(
  repo, "firmware/inkloop-idf/components/inkloop_diagnostics");

const harness = String.raw`
#include <cassert>
#include <string>

#include "inkloop/diagnostics/diagnostic_detail.hpp"

using namespace inkloop::diagnostics;

int main() {
  assert(sanitizeDiagnosticDetail("authorization rejected by MyAI") ==
         "authorization rejected by MyAI");
  assert(sanitizeDiagnosticDetail("token expired") == "token expired");
  assert(sanitizeDiagnosticDetail("line one\r\nline two\t done") ==
         "line one line two done");
  assert(sanitizeDiagnosticDetail("invalid_pairing_token") ==
         "invalid_pairing_token");
  assert(sanitizeDiagnosticDetail("Bearer device-secret").empty());
  assert(sanitizeDiagnosticDetail("device_token=secret").empty());
  assert(sanitizeDiagnosticDetail("token short123").empty());
  assert(sanitizeDiagnosticDetail("password hunter2").empty());
  assert(sanitizeDiagnosticDetail("authorization: secret").empty());
  assert(sanitizeDiagnosticDetail("https://internal.example/path").empty());
  assert(sanitizeDiagnosticDetail(
      "Abcdefghijklmnopqrstuvwxyz0123456789ABCDEF").empty());
  assert(sanitizeDiagnosticDetail(std::string("bad") + char(0xff)).empty());

  const std::string bounded = sanitizeDiagnosticDetail(std::string(200, 'x'));
  assert(bounded.size() == kMaximumDiagnosticDetailBytes);
  assert(isCanonicalDiagnosticDetail(bounded));

  const std::string utf8 = sanitizeDiagnosticDetail(
      std::string(158, 'x') + "好", kMaximumDiagnosticDetailBytes);
  assert(utf8.size() == 158);
  assert(isCanonicalDiagnosticDetail(utf8));
  assert(!isCanonicalDiagnosticDetail("line one\nline two"));
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-diagnostic-detail-"));
  try {
    const source = join(scratch, "diagnostic_detail.cpp");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    writeFileSync(source, harness);
    const args = [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-I", join(component, "include"), source,
      join(component, "diagnostic_detail.cpp"), "-o", binary,
    ];
    if (sanitized) {
      args.splice(1, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer");
    }
    execFileSync("c++", args, { stdio: "pipe" });
    execFileSync(binary, [], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("diagnostic detail is bounded, UTF-8 safe, and credential-free", () => {
  buildAndRun(false);
  buildAndRun(true);
});
