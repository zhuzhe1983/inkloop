import { t } from "./i18n-runtime.ts";
import type { InkApp } from "./app-model";
import type { DeviceSkuId } from "./device-catalog";

const OWNER_KEY = "inkloop-device-owner-v1";

export type Esp32DeviceTask = {
  id: string;
  app: InkApp;
  scheduleMode: InkApp["scheduleMode"];
  customMinutes: number;
  dailyTime: string;
  revision: number;
  updatedAt: string;
};

export type Esp32DeviceRecord = {
  id: string;
  name: string;
  skuId: DeviceSkuId;
  hardwareId: string;
  firmwareVersion: string | null;
  batteryPercent: number | null;
  lastSeenAt: string | null;
  online: boolean;
  desiredRevision: number;
  appliedRevision: number;
  tasks: Esp32DeviceTask[];
};

type DeviceListPayload = { devices?: Esp32DeviceRecord[]; error?: string };

export function deviceOwnerToken() {
  let token = localStorage.getItem(OWNER_KEY)?.trim() ?? "";
  if (!/^[a-f0-9-]{32,64}$/i.test(token)) {
    token = crypto.randomUUID();
    localStorage.setItem(OWNER_KEY, token);
  }
  return token;
}

async function deviceRequest<T>(url: string, init: RequestInit = {}) {
  const headers = new Headers(init.headers);
  headers.set("X-Inkloop-Owner", deviceOwnerToken());
  if (init.body && !headers.has("Content-Type")) headers.set("Content-Type", "application/json");
  const response = await fetch(url, { ...init, headers, cache: "no-store" });
  const payload = await response.json().catch(() => ({})) as T & { error?: string };
  if (!response.ok) throw new Error(payload.error || t("设备服务暂时不可用"));
  return payload;
}

export async function listEsp32Devices() {
  const payload = await deviceRequest<DeviceListPayload>("/api/devices");
  return payload.devices ?? [];
}

export async function claimEsp32Device(code: string) {
  const payload = await deviceRequest<{ device: Esp32DeviceRecord }>("/api/devices", {
    method: "POST",
    body: JSON.stringify({ action: "claim", code }),
  });
  return payload.device;
}

export async function publishEsp32Task(
  deviceId: string,
  app: InkApp,
  frame: Blob,
) {
  const frameDataUrl = await new Promise<string>((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result));
    reader.onerror = () => reject(reader.error ?? new Error(t("无法读取设备画面")));
    reader.readAsDataURL(frame);
  });
  const payload = await deviceRequest<{ device: Esp32DeviceRecord; task: Esp32DeviceTask }>(
    "/api/devices",
    {
      method: "POST",
      body: JSON.stringify({ action: "upsert-task", deviceId, app, frameDataUrl }),
    },
  );
  return payload;
}

export async function deleteEsp32Task(deviceId: string, taskId: string) {
  return deviceRequest<{ device: Esp32DeviceRecord }>("/api/devices", {
    method: "POST",
    body: JSON.stringify({ action: "delete-task", deviceId, taskId }),
  });
}

export type FirmwareProgress = {
  phase: "connecting" | "downloading" | "erasing" | "writing" | "complete";
  percent: number;
  message: string;
};

type FirmwareManifest = {
  chipFamily: "ESP32-S3";
  version: string;
  baudRate: number;
  serverSlot: { marker: string; length: number };
  files: Array<{ path: string; offset: number; sha256: string }>;
};

async function sha256Hex(bytes: Uint8Array) {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", new Uint8Array(bytes)));
  return Array.from(digest, (part) => part.toString(16).padStart(2, "0")).join("");
}

function patchServerUrl(bytes: Uint8Array, slot: FirmwareManifest["serverSlot"], serverUrl: string) {
  const marker = new TextEncoder().encode(slot.marker);
  const target = new TextEncoder().encode(serverUrl);
  if (target.length + 1 > slot.length) throw new Error(t("当前创作台地址过长，无法写入固件"));
  let found = -1;
  for (let offset = 0; offset <= bytes.length - marker.length; offset += 1) {
    if (marker.every((part, index) => bytes[offset + index] === part)) {
      found = offset;
      break;
    }
  }
  if (found < 0 || found + slot.length > bytes.length) throw new Error(t("固件缺少服务器地址槽位"));
  bytes.fill(0, found, found + slot.length);
  bytes.set(target, found);
}

export async function flashM5PaperColor(
  onProgress: (progress: FirmwareProgress) => void,
) {
  const serial = (navigator as Navigator & {
    serial?: { requestPort(options?: Record<string, unknown>): Promise<unknown> };
  }).serial;
  if (!serial) throw new Error(t("当前浏览器不支持 USB 串口刷机，请使用桌面版 Chrome 或 Edge"));

  onProgress({ phase: "downloading", percent: 4, message: t("正在准备瘦客户端固件…") });
  const manifestResponse = await fetch("/firmware/m5-papercolor/manifest.json", { cache: "no-store" });
  if (!manifestResponse.ok) throw new Error(t("M5 PaperColor 固件尚未发布"));
  const manifest = await manifestResponse.json() as FirmwareManifest;
  const files = await Promise.all(manifest.files.map(async (file) => {
    const response = await fetch(file.path, { cache: "no-store" });
    if (!response.ok) throw new Error(`无法下载固件文件：${file.path}`);
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (!/^[a-f0-9]{64}$/i.test(file.sha256) || await sha256Hex(bytes) !== file.sha256.toLowerCase()) {
      throw new Error(`固件校验失败：${file.path}`);
    }
    if (file.path.endsWith("/firmware.bin")) {
      patchServerUrl(bytes, manifest.serverSlot, new URL("/api/devices", window.location.origin).toString());
    }
    return { data: bytes, address: file.offset };
  }));

  onProgress({ phase: "connecting", percent: 8, message: t("请选择刚插入的 M5 PaperColor…") });
  const port = await serial.requestPort({});
  const { ESPLoader, Transport } = await import("esptool-js");
  const transport = new Transport(port as ConstructorParameters<typeof Transport>[0], true);
  const loader = new ESPLoader({
    transport,
    baudrate: manifest.baudRate || 460800,
    terminal: {
      clean() {},
      writeLine(data: string) {
        if (data.includes("erase")) {
          onProgress({ phase: "erasing", percent: 12, message: t("正在清理设备 Flash…") });
        }
      },
      write() {},
    },
  });
  try {
    await loader.main();
    await loader.writeFlash({
      fileArray: files,
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (_fileIndex: number, written: number, total: number) => {
        const percent = 15 + Math.round((written / Math.max(1, total)) * 82);
        onProgress({ phase: "writing", percent, message: `正在写入瘦客户端 ${percent}%` });
      },
    });
    await loader.after();
    onProgress({ phase: "complete", percent: 100, message: t("刷机完成，设备正在重新启动") });
  } finally {
    await transport.disconnect().catch(() => undefined);
  }
}
