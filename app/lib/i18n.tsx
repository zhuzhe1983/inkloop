"use client";

import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useState,
  type ReactNode,
} from "react";
import en from "./i18n/en";
import ja from "./i18n/ja";
import { setRuntimeLocale } from "./i18n-runtime";

export type Locale = "zh" | "en" | "ja";

export const localeOptions: Array<{ value: Locale; label: string }> = [
  { value: "zh", label: "中文" },
  { value: "en", label: "English" },
  { value: "ja", label: "日本語" },
];

export const localeTags: Record<Locale, string> = {
  zh: "zh-CN",
  en: "en-US",
  ja: "ja-JP",
};

export const LOCALE_STORAGE_KEY = "inkloop-locale-v1";

export function normalizeLocale(value: string | null | undefined): Locale | null {
  if (!value) return null;
  const lower = value.toLowerCase();
  if (lower.startsWith("zh")) return "zh";
  if (lower.startsWith("ja")) return "ja";
  if (lower.startsWith("en")) return "en";
  return null;
}

export function detectLocale(): Locale {
  if (typeof window === "undefined") return "zh";
  const params = new URLSearchParams(window.location.search);
  const fromUrl = normalizeLocale(params.get("lang"));
  if (fromUrl) return fromUrl;
  const stored = normalizeLocale(window.localStorage.getItem(LOCALE_STORAGE_KEY));
  if (stored) return stored;
  return normalizeLocale(window.navigator.language) ?? "zh";
}

const dictionaries: Record<Exclude<Locale, "zh">, Record<string, string>> = { en, ja };

export function translate(locale: Locale, zh: string): string {
  if (locale === "zh") return zh;
  return dictionaries[locale][zh] ?? zh;
}

export const I18nContext = createContext<{
  locale: Locale;
  setLocale: (locale: Locale) => void;
  t: (zh: string) => string;
}>({
  locale: "zh",
  setLocale: () => undefined,
  t: (zh) => zh,
});

export function I18nProvider({ children }: { children: ReactNode }) {
  const [locale, setLocaleState] = useState<Locale>("zh");

  useEffect(() => {
    setLocaleState(detectLocale());
  }, []);

  useEffect(() => {
    document.documentElement.lang = localeTags[locale];
    window.localStorage.setItem(LOCALE_STORAGE_KEY, locale);
    setRuntimeLocale(locale);
  }, [locale]);

  const setLocale = useCallback((next: Locale) => setLocaleState(next), []);
  const t = useCallback((zh: string) => translate(locale, zh), [locale]);

  return (
    <I18nContext.Provider value={{ locale, setLocale, t }}>
      {children}
    </I18nContext.Provider>
  );
}

export function useI18n() {
  return useContext(I18nContext);
}
