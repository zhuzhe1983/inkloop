import { t } from "./lib/i18n-runtime";
import type { Metadata } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import { headers } from "next/headers";
import "./globals.css";

const geistSans = Geist({
  variable: "--font-geist-sans",
  subsets: ["latin"],
});

const geistMono = Geist_Mono({
  variable: "--font-geist-mono",
  subsets: ["latin"],
});

export async function generateMetadata(): Promise<Metadata> {
  const requestHeaders = await headers();
  const host = requestHeaders.get("x-forwarded-host") ?? requestHeaders.get("host");
  const protocol = requestHeaders.get("x-forwarded-proto") ?? (host?.startsWith("localhost") ? "http" : "https");
  const image = host ? `${protocol}://${host}/og.png` : undefined;

  return {
    title: t("Inkloop · 蓝牙墨水屏应用工坊"),
    description: t("用自然语言创作 TodooCard 应用，预览、保存并定时写入六色蓝牙电子墨水屏。"),
    icons: {
      icon: "/favicon.svg",
      shortcut: "/favicon.svg",
    },
    openGraph: {
      title: t("Inkloop · 说一句，屏上见"),
      description: t("创作、预览并定时写入你的 TodooCard 六色电子墨水屏。"),
      type: "website",
      images: image ? [{ url: image, width: 1200, height: 630, alt: t("Inkloop 蓝牙墨水屏应用工坊") }] : undefined,
    },
    twitter: {
      card: "summary_large_image",
      title: t("Inkloop · 说一句，屏上见"),
      description: t("创作、预览并定时写入你的 TodooCard 六色电子墨水屏。"),
      images: image ? [image] : undefined,
    },
  };
}

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="zh-CN">
      <body
        className={`${geistSans.variable} ${geistMono.variable} antialiased`}
      >
        {children}
      </body>
    </html>
  );
}
