"use client";

import en from "./i18n/en";
import ja from "./i18n/ja";
import type { Locale } from "./i18n";

const dictionaries: Record<Exclude<Locale, "zh">, Record<string, string>> = { en, ja };

/**
 * Module-level translator for non-React helpers (app-model, device drivers,
 * error messages). The provider syncs the active locale here on every change,
 * so plain functions can call t() without React context.
 */
let activeLocale: Locale = "zh";

export function setRuntimeLocale(locale: Locale) {
  activeLocale = locale;
}

export function activeLocaleTag(): string {
  return activeLocale === "en" ? "en-US" : activeLocale === "ja" ? "ja-JP" : "zh-CN";
}

export function t(zh: string): string {
  if (activeLocale === "zh") return zh;
  return dictionaries[activeLocale][zh] ?? zh;
}
