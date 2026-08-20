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

export type FirmwareDeviceEvent = {
  kind: "connected" | "log" | "access-point" | "pair-code" | "error" | "closed";
  message: string;
  accessPoint?: string;
  pairingCode?: string;
};

export type FirmwareMonitorResult = {
  accessPoint: string | null;
  pairingCode: string | null;
  logs: string[];
};

export type FirmwareFlashSession = {
  monitor: Promise<FirmwareMonitorResult>;
  stopMonitoring: () => void;
};

type SerialPortLike = {
  getInfo?: () => { usbVendorId?: number; usbProductId?: number };
  open: (options: { baudRate: number; bufferSize?: number }) => Promise<void>;
  close: () => Promise<void>;
  readable?: ReadableStream<Uint8Array> | null;
};

type SerialNavigator = {
  requestPort: (options?: Record<string, unknown>) => Promise<SerialPortLike>;
  getPorts?: () => Promise<SerialPortLike[]>;
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

function pause(milliseconds: number, signal?: AbortSignal) {
  return new Promise<void>((resolve) => {
    const timer = window.setTimeout(resolve, milliseconds);
    signal?.addEventListener("abort", () => {
      window.clearTimeout(timer);
      resolve();
    }, { once: true });
  });
}

function sameSerialDevice(left: SerialPortLike, right: SerialPortLike) {
  if (left === right) return true;
  const leftInfo = left.getInfo?.() ?? {};
  const rightInfo = right.getInfo?.() ?? {};
  return leftInfo.usbVendorId !== undefined
    && leftInfo.usbVendorId === rightInfo.usbVendorId
    && leftInfo.usbProductId === rightInfo.usbProductId;
}

async function openRuntimeSerialPort(
  serial: SerialNavigator,
  selectedPort: SerialPortLike,
  signal: AbortSignal,
) {
  for (let attempt = 0; attempt < 40 && !signal.aborted; attempt += 1) {
    const authorized = await serial.getPorts?.().catch(() => []) ?? [];
    const candidates = [selectedPort, ...authorized.filter((port) => sameSerialDevice(selectedPort, port))]
      .filter((port, index, all) => all.indexOf(port) === index);
    for (const candidate of candidates) {
      try {
        await candidate.open({ baudRate: 115200, bufferSize: 65536 });
        return candidate;
      } catch {
        // Native USB can disappear briefly while the ESP32-S3 re-enumerates.
      }
    }
    await pause(500, signal);
  }
  return null;
}

async function monitorPaperColorBoot(
  serial: SerialNavigator,
  selectedPort: SerialPortLike,
  signal: AbortSignal,
  onDeviceEvent?: (event: FirmwareDeviceEvent) => void,
): Promise<FirmwareMonitorResult> {
  const logs: string[] = [];
  let accessPoint: string | null = null;
  let pairingCode: string | null = null;
  const runtimePort = await openRuntimeSerialPort(serial, selectedPort, signal);
  if (!runtimePort?.readable || signal.aborted) {
    const message = t("设备已重启，但暂时无法重新连接调试串口");
    onDeviceEvent?.({ kind: "error", message });
    return { accessPoint, pairingCode, logs };
  }

  onDeviceEvent?.({ kind: "connected", message: t("调试串口已连接，正在读取设备启动日志") });
  const reader = runtimePort.readable.getReader();
  const decoder = new TextDecoder();
  let pending = "";
  let finished = false;
  const stopReading = () => {
    if (finished) return;
    void reader.cancel().catch(() => undefined);
  };
  const timeout = window.setTimeout(stopReading, 6 * 60 * 1000);
  signal.addEventListener("abort", stopReading, { once: true });

  const consumeLine = (rawLine: string) => {
    const line = rawLine.trim();
    if (!line) return;
    logs.push(line);
    if (logs.length > 240) logs.shift();
    const apMatch = line.match(/^INKLOOP_WIFI_AP:(.+)$/);
    const codeMatch = line.match(/^INKLOOP_PAIR_CODE:(\d{6})$/);
    if (apMatch) {
      accessPoint = apMatch[1].trim();
      onDeviceEvent?.({ kind: "access-point", message: line, accessPoint });
    } else if (codeMatch) {
      pairingCode = codeMatch[1];
      onDeviceEvent?.({ kind: "pair-code", message: line, pairingCode });
    } else if (/^INKLOOP_(ERROR|WARN):/.test(line) || /Guru Meditation|panic|fatal/i.test(line)) {
      onDeviceEvent?.({ kind: "error", message: line });
    } else {
      onDeviceEvent?.({ kind: "log", message: line });
    }
  };

  try {
    while (!signal.aborted) {
      const { value, done } = await reader.read();
      if (done) break;
      pending += decoder.decode(value, { stream: true });
      const lines = pending.split(/\r?\n/);
      pending = lines.pop() ?? "";
      lines.forEach(consumeLine);
    }
    pending += decoder.decode();
    consumeLine(pending);
  } catch (error) {
    if (!signal.aborted) {
      onDeviceEvent?.({
        kind: "error",
        message: error instanceof Error ? error.message : t("设备调试串口意外断开"),
      });
    }
  } finally {
    finished = true;
    window.clearTimeout(timeout);
    signal.removeEventListener("abort", stopReading);
    reader.releaseLock();
    await runtimePort.close().catch(() => undefined);
    onDeviceEvent?.({ kind: "closed", message: t("设备调试串口监听已结束") });
  }
  return { accessPoint, pairingCode, logs };
}

export async function flashM5PaperColor(
  onProgress: (progress: FirmwareProgress) => void,
  onDeviceEvent?: (event: FirmwareDeviceEvent) => void,
): Promise<FirmwareFlashSession> {
  const serial = (navigator as Navigator & { serial?: SerialNavigator }).serial;
  if (!serial) throw new Error(t("当前浏览器不支持 USB 串口刷机，请使用桌面版 Chrome 或 Edge"));

  // Web Serial requires requestPort() to run directly inside the click's user
  // activation. Any awaited fetch/import before this call prevents Chrome from
  // showing the device chooser.
  const port = await serial.requestPort({});
  onProgress({ phase: "downloading", percent: 4, message: t("正在准备瘦客户端固件…") });
  let manifestResponse: Response;
  try {
    manifestResponse = await fetch("/firmware/m5-papercolor/manifest.json", { cache: "no-store" });
  } catch {
    throw new Error(t("无法下载固件清单，请检查网络后重试"));
  }
  if (!manifestResponse.ok) throw new Error(t("M5 PaperColor 固件尚未发布"));
  const manifest = await manifestResponse.json() as FirmwareManifest;
  const files = await Promise.all(manifest.files.map(async (file) => {
    let response: Response;
    try {
      response = await fetch(file.path, { cache: "no-store" });
    } catch {
      throw new Error(`${t("无法下载固件文件：")}${file.path}`);
    }
    if (!response.ok) throw new Error(`${t("无法下载固件文件：")}${file.path}`);
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (!/^[a-f0-9]{64}$/i.test(file.sha256) || await sha256Hex(bytes) !== file.sha256.toLowerCase()) {
      throw new Error(`${t("固件校验失败：")}${file.path}`);
    }
    if (file.path.endsWith("/firmware.bin")) {
      patchServerUrl(bytes, manifest.serverSlot, new URL("/api/devices", window.location.origin).toString());
    }
    return { data: bytes, address: file.offset };
  }));

  onProgress({ phase: "connecting", percent: 8, message: t("正在连接 M5 PaperColor…") });
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
        onProgress({ phase: "writing", percent, message: `${t("正在写入瘦客户端")} ${percent}%` });
      },
    });
    await loader.after("hard_reset");
    onProgress({ phase: "complete", percent: 100, message: t("刷机完成，设备已自动重启") });
  } finally {
    await transport.disconnect().catch(() => undefined);
  }

  const monitorController = new AbortController();
  return {
    monitor: monitorPaperColorBoot(serial, port, monitorController.signal, onDeviceEvent),
    stopMonitoring: () => monitorController.abort(),
  };
}
