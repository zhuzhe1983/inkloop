import type { Locale } from "./i18n-types";
import type { PaperColorRenderStrategy } from "./papercolor-render";

export type DeviceFamily = "bluetooth" | "esp32";
export type DeviceTransport = "web-bluetooth" | "wifi-pull";
export type DeviceScheduleOwner = "browser" | "device";
export type DeviceRenderStrategy = "browser-canvas" | "server-raster";

export type DeviceSku = {
  id: string;
  family: DeviceFamily;
  manufacturer: string;
  model: string;
  displayName: string;
  description: string;
  sizeInches: number;
  officialUrls: Record<Locale, string>;
  screen: {
    technology: "spectra6" | "six-color-epaper";
    width: number;
    height: number;
    orientations: Array<"portrait" | "landscape">;
    colors: readonly ["black", "white", "yellow", "red", "blue", "green"];
  };
  render: {
    strategy: DeviceRenderStrategy;
    sourceWidth: number;
    sourceHeight: number;
    outputFormat: "todoocard-frame" | "png";
    colorOptimization: "device-calibrated-six-color" | "spectra6-server-quantized";
    supportedStrategies: readonly PaperColorRenderStrategy[];
  };
  write: {
    transport: DeviceTransport;
    strategy: "gatt-packets" | "https-image-pull";
    scheduleOwner: DeviceScheduleOwner;
    requiresOpenBrowser: boolean;
  };
  provisioning: {
    method: "bluetooth-picker" | "web-serial-flash-and-code";
    firmwareManifest?: string;
  };
};

const SIX_COLORS = ["black", "white", "yellow", "red", "blue", "green"] as const;

export const DEVICE_SKUS = {
  "todoo-card-3.7": {
    id: "todoo-card-3.7",
    family: "bluetooth",
    manufacturer: "Todoo",
    model: "TodooCard 3.7",
    displayName: "TodooCard",
    description: "528 × 792 六色蓝牙电子纸卡片",
    sizeInches: 3.7,
    officialUrls: {
      zh: "https://p.todoo.tech/?lang=zh",
      en: "https://p.todoo.tech/?lang=en",
      ja: "https://p.todoo.tech/?lang=ja",
    },
    screen: {
      technology: "six-color-epaper",
      width: 528,
      height: 792,
      orientations: ["portrait", "landscape"],
      colors: SIX_COLORS,
    },
    render: {
      strategy: "browser-canvas",
      sourceWidth: 528,
      sourceHeight: 792,
      outputFormat: "todoocard-frame",
      colorOptimization: "device-calibrated-six-color",
      supportedStrategies: ["official-quality", "solid-clean"],
    },
    write: {
      transport: "web-bluetooth",
      strategy: "gatt-packets",
      scheduleOwner: "browser",
      requiresOpenBrowser: true,
    },
    provisioning: { method: "bluetooth-picker" },
  },
  "m5-papercolor-c151": {
    id: "m5-papercolor-c151",
    family: "esp32",
    manufacturer: "M5Stack",
    model: "PaperColor C151",
    displayName: "M5 PaperColor",
    description: "400 × 600 Spectra 6 Wi‑Fi 彩色电子纸",
    sizeInches: 4,
    officialUrls: {
      zh: "https://docs.m5stack.com/zh_CN/core/PaperColor",
      en: "https://docs.m5stack.com/en/core/PaperColor",
      ja: "https://docs.m5stack.com/ja/core/PaperColor",
    },
    screen: {
      technology: "spectra6",
      width: 400,
      height: 600,
      orientations: ["portrait", "landscape"],
      colors: SIX_COLORS,
    },
    render: {
      strategy: "server-raster",
      sourceWidth: 528,
      sourceHeight: 792,
      outputFormat: "png",
      colorOptimization: "spectra6-server-quantized",
      supportedStrategies: [
        "official-quality",
        "classic-six-color",
        "reflectance-photo",
        "solid-clean",
      ],
    },
    write: {
      transport: "wifi-pull",
      strategy: "https-image-pull",
      scheduleOwner: "device",
      requiresOpenBrowser: false,
    },
    provisioning: {
      method: "web-serial-flash-and-code",
      firmwareManifest: "/firmware/m5-papercolor/test-channel/0.3.0-beta.1/manifest.json",
    },
  },
} as const satisfies Record<string, DeviceSku>;

export type DeviceSkuId = keyof typeof DEVICE_SKUS;

export type DeviceAdapter = {
  id: string;
  skuId: DeviceSkuId;
  taskExecution: "browser-bluetooth" | "device-wifi";
  supportsCalibration: boolean;
  requiresBrowserDriver: boolean;
  renderTarget(orientation: "portrait" | "landscape"): { width: number; height: number };
  taskStatusCopy: string;
};

export const DEVICE_ADAPTERS: Record<DeviceSkuId, DeviceAdapter> = {
  "todoo-card-3.7": {
    id: "todoo-gatt-v1",
    skuId: "todoo-card-3.7",
    taskExecution: "browser-bluetooth",
    supportsCalibration: true,
    requiresBrowserDriver: true,
    renderTarget: () => ({ width: 528, height: 792 }),
    taskStatusCopy: "浏览器渲染 · GATT 分包写入",
  },
  "m5-papercolor-c151": {
    id: "m5-papercolor-wifi-v1",
    skuId: "m5-papercolor-c151",
    taskExecution: "device-wifi",
    supportsCalibration: false,
    requiresBrowserDriver: false,
    renderTarget: (orientation) => orientation === "landscape"
      ? { width: 600, height: 400 }
      : { width: 400, height: 600 },
    taskStatusCopy: "服务端 PNG · HTTPS 主动拉取",
  },
};

export function deviceSku(id: string | null | undefined): DeviceSku | null {
  return id && id in DEVICE_SKUS ? DEVICE_SKUS[id as DeviceSkuId] : null;
}

export function deviceSkusForFamily(family: DeviceFamily) {
  return Object.values(DEVICE_SKUS).filter((sku) => sku.family === family);
}

export function allDeviceSkus() {
  return Object.values(DEVICE_SKUS);
}

export function deviceManufacturers() {
  return [...new Set(Object.values(DEVICE_SKUS).map((sku) => sku.manufacturer))];
}

export function filterDeviceSkus(filters: {
  family?: DeviceFamily | "all" | null;
  manufacturer?: string | "all" | null;
} = {}) {
  return Object.values(DEVICE_SKUS).filter((sku) => {
    if (filters.family && filters.family !== "all" && sku.family !== filters.family) return false;
    if (filters.manufacturer && filters.manufacturer !== "all" && sku.manufacturer !== filters.manufacturer) return false;
    return true;
  });
}

export function officialProductUrl(sku: DeviceSku, locale: Locale) {
  return sku.officialUrls[locale] ?? sku.officialUrls.zh;
}

export function deviceAdapter(id: string | null | undefined) {
  return id && id in DEVICE_ADAPTERS ? DEVICE_ADAPTERS[id as DeviceSkuId] : DEVICE_ADAPTERS["todoo-card-3.7"];
}
