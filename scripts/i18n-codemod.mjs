/**
 * Build-time i18n codemod.
 *
 * Wraps Chinese string literals in UI helper files with `t("...")` so they
 * resolve through app/lib/i18n dictionaries at render time. `t` is imported
 * from the runtime module, which reads the locale synced by I18nProvider.
 *
 * Usage: node scripts/i18n-codemod.mjs [--check]
 */
import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const root = dirname(dirname(fileURLToPath(import.meta.url)));
const checkOnly = process.argv.includes("--check");

/** Files that get automatic t() wrapping (the main studio file is manual). */
const targets = [
  "app/ink-studio.tsx",
  "app/lib/app-model.ts",
  "app/lib/device-catalog.ts",
  "app/lib/todoo-card.ts",
  "app/lib/esp32-device.ts",
  "app/lib/device-calibration.ts",
  "app/lib/bluetooth-recovery.ts",
  "app/layout.tsx",
];

const CJK = /[一-鿿]/;
const WRAPPER_CALL = /(?:^|[^\w$])(?:t|translate|tr)\(\s*$/;
const RUNTIME_WRAPPER_CALL = /(?:^|[^\w$])(?:t|tRuntime|translate|tr)\(\s*$/;

/**
 * Walk source and return string-literal spans, correctly handling escapes,
 * line/block comments, template literals, and regex literals (so CJK inside a
 * regex like /闪卡|全息/ never gets treated as a string).
 */
function collectStringSpans(source) {
  const spans = [];
  const length = source.length;
  let index = 0;
  let mode = "code";
  let quote = "";
  let start = 0;
  let previousSignificant = "";

  const regexAllowedAfter = new Set([
    "", "(", ",", "=", ":", "[", "!", "&", "|", "?", "{", "}", ";",
    "return", "=>", "typeof", "case", "in", "of", "new", "delete", "void",
  ]);

  while (index < length) {
    const char = source[index];
    const next = source[index + 1];

    if (mode === "line-comment") {
      if (char === "\n") mode = "code";
      index += 1;
      continue;
    }
    if (mode === "block-comment") {
      if (char === "*" && next === "/") {
        mode = "code";
        index += 2;
        continue;
      }
      index += 1;
      continue;
    }
    if (mode === "template") {
      if (char === "\\") {
        index += 2;
        continue;
      }
      if (char === "`") mode = "code";
      index += 1;
      continue;
    }
    if (mode === "regex") {
      if (char === "\\") {
        index += 2;
        continue;
      }
      if (char === "/") mode = "code";
      index += 1;
      continue;
    }
    if (mode === "string") {
      if (char === "\\") {
        index += 2;
        continue;
      }
      if (char === quote) {
        spans.push({ start, end: index, content: source.slice(start + 1, index) });
        mode = "code";
        previousSignificant = "literal";
      }
      index += 1;
      continue;
    }

    // code mode
    if (char === "/" && next === "/") {
      mode = "line-comment";
      index += 2;
      continue;
    }
    if (char === "/" && next === "*") {
      mode = "block-comment";
      index += 2;
      continue;
    }
    if (char === '"' || char === "'") {
      mode = "string";
      quote = char;
      start = index;
      index += 1;
      continue;
    }
    if (char === "`") {
      mode = "template";
      index += 1;
      continue;
    }
    if (char === "/" && next !== "/" && next !== "*" && next !== "=") {
      if (regexAllowedAfter.has(previousSignificant)) {
        mode = "regex";
        index += 1;
        continue;
      }
    }
    if (!/\s/.test(char)) {
      if (/[\w$]/.test(char)) {
        // accumulate identifiers/keywords
        let end = index;
        while (end < length && /[\w$]/.test(source[end])) end += 1;
        previousSignificant = source.slice(index, end);
        index = end;
        continue;
      }
      previousSignificant = char;
    }
    index += 1;
  }
  return spans;
}

let updated = 0;

for (const relative of targets) {
  const path = join(root, relative);
  const source = readFileSync(path, "utf8");
  const spans = collectStringSpans(source);
  let output = "";
  let cursor = 0;
  let wrapped = 0;

  for (const span of spans) {
    output += source.slice(cursor, span.start);
    const literal = source.slice(span.start, span.end + 1);
    const before = source.slice(Math.max(0, span.start - 80), span.start);
    const isModuleSpecifier = /\bfrom\s*$/.test(before) || /\bimport\s*(?:\(\s*)?$/.test(before) || /\brequire\s*\(\s*$/.test(before);
    const alreadyWrapped = relative === "app/ink-studio.tsx"
      ? RUNTIME_WRAPPER_CALL.test(before)
      : WRAPPER_CALL.test(before);
    // In the main studio file, wrap with tRuntime to match the existing import.
    const wrapName = relative === "app/ink-studio.tsx" ? "tRuntime" : "t";
    if (CJK.test(span.content) && !isModuleSpecifier && !alreadyWrapped) {
      output += `${wrapName}(${literal})`;
      wrapped += 1;
    } else {
      output += literal;
    }
    cursor = span.end + 1;
  }
  output += source.slice(cursor);

  if (wrapped > 0) {
    const importLine = relative === "app/layout.tsx"
      ? 'import { t } from "./lib/i18n-runtime";\n'
      : relative === "app/ink-studio.tsx"
        ? null // main file already imports the runtime translator as tRuntime
        : 'import { t } from "./i18n-runtime";\n';
    if (importLine && !output.includes("i18n-runtime")) output = importLine + output;
    updated += 1;
    if (!checkOnly) writeFileSync(path, output);
    console.log(`${checkOnly ? "[needs wrap]" : "[wrapped]"} ${relative}: ${wrapped} literals`);
  }
}

if (checkOnly && updated > 0) process.exitCode = 1;
if (!checkOnly) console.log(`codemod done, ${updated} file(s) updated`);
