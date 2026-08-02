import type { Metadata } from "next";
import { Geist, Geist_Mono } from "next/font/google";
import { headers } from "next/headers";
import "@fontsource/noto-sans-sc/chinese-simplified-700.css";
import "@fontsource/noto-serif-sc/chinese-simplified-700.css";
import "@fontsource/m-plus-rounded-1c/japanese-700.css";
import "@fontsource/ma-shan-zheng/chinese-simplified.css";
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
    title: "Inkloop · 蓝牙墨水屏应用工坊",
    description: "用自然语言创作 TodooCard 应用，预览、保存并定时写入六色蓝牙电子墨水屏。",
    icons: {
      icon: "/favicon.svg",
      shortcut: "/favicon.svg",
    },
    openGraph: {
      title: "Inkloop · 说一句，屏上见",
      description: "创作、预览并定时写入你的 TodooCard 六色电子墨水屏。",
      type: "website",
      images: image ? [{ url: image, width: 1200, height: 630, alt: "Inkloop 蓝牙墨水屏应用工坊" }] : undefined,
    },
    twitter: {
      card: "summary_large_image",
      title: "Inkloop · 说一句，屏上见",
      description: "创作、预览并定时写入你的 TodooCard 六色电子墨水屏。",
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
