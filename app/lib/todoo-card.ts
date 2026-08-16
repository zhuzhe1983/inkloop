import { t } from "./i18n-runtime.ts";
import CoreTodooCard, { TODOO_PROTOCOL as CORE_PROTOCOL } from "./todoo-card-core.js";

export const TODOO_PROTOCOL = {
  service: CORE_PROTOCOL.uuids.service,
  control: CORE_PROTOCOL.uuids.control,
  data: CORE_PROTOCOL.uuids.data,
  width: CORE_PROTOCOL.image.visibleWidth,
  height: CORE_PROTOCOL.image.visibleHeight,
  payloadBytes: CORE_PROTOCOL.frame.payloadBytes,
  packetBytes: CORE_PROTOCOL.transfer.gattValueBytes,
  dataBytesPerPacket: CORE_PROTOCOL.transfer.dataBytesPerPacket,
  packetCount: CORE_PROTOCOL.transfer.packetCount,
} as const;

export type TodooProgress = {
  phase: "connecting" | "pairing" | "encoding" | "sending" | "refreshing" | "complete";
  percent: number;
  message: string;
};

type ProgressHandler = (progress: TodooProgress) => void;
type AuthorizedBluetoothDevice = { id: string; name?: string | null };
type WriteCanvasOptions = {
  palette?: Array<[number, number, number] | null>;
  dither?: boolean;
};

const stateProgress: Record<string, TodooProgress> = {
  connecting: { phase: "connecting", percent: 4, message: t("正在连接 TodooCard…") },
  pairing: { phase: "pairing", percent: 4, message: t("正在连接安全配对服务…") },
  "verifying-pairing": { phase: "pairing", percent: 7, message: t("请确认系统配对提示…") },
  paired: { phase: "pairing", percent: 9, message: t("安全配对完成") },
  discovering: { phase: "connecting", percent: 6, message: t("正在发现图像服务…") },
  subscribing: { phase: "connecting", percent: 8, message: t("正在订阅设备通知…") },
  "handshake-init": { phase: "sending", percent: 10, message: t("正在初始化写屏协议…") },
  "handshake-length": { phase: "sending", percent: 11, message: t("正在声明图像帧长度…") },
  "handshake-ready": { phase: "sending", percent: 12, message: t("设备已准备接收图像…") },
  "waiting-complete": { phase: "refreshing", percent: 96, message: t("设备已收帧，正在确认刷新…") },
  complete: { phase: "complete", percent: 100, message: t("写入完成") },
};

/**
 * UI compatibility adapter around the full framework-free driver maintained in
 * the original todoo/web directory. The adapter preserves Inkloop's existing
 * callback API while exposing the newer validation and compatibility behavior.
 */
export class TodooCard {
  private core: CoreTodooCard;
  private onProgress?: ProgressHandler;
  private reconnectPromise: Promise<void> | null = null;

  constructor(onProgress?: ProgressHandler) {
    this.onProgress = onProgress;
    this.core = new CoreTodooCard();
    this.core.on("state", ({ detail }) => {
      const state = typeof detail.state === "string" ? detail.state : "";
      const progress = stateProgress[state];
      if (progress) this.onProgress?.(progress);
    });
    this.core.on("progress", ({ detail }) => {
      const corePercent = typeof detail.percent === "number" ? detail.percent : 0;
      const sentPackets = typeof detail.sentPackets === "number" ? detail.sentPackets : 0;
      const totalPackets = typeof detail.totalPackets === "number" ? detail.totalPackets : 0;
      const percent = 12 + Math.round((corePercent / 100) * 78);
      this.onProgress?.({
        phase: "sending",
        percent,
        message: `正在发送 ${sentPackets} / ${totalPackets} 包`,
      });
    });
  }

  get supported() {
    return this.core.isSupported() && globalThis.isSecureContext;
  }

  get selectedDevice() {
    return this.core.device;
  }

  async listAuthorizedDevices() {
    try {
      return await this.core.listAuthorizedDevices();
    } catch (error) {
      if (
        error
        && typeof error === "object"
        && "code" in error
        && error.code === "AUTHORIZED_DEVICE_LIST_UNSUPPORTED"
      ) return [];
      throw error;
    }
  }

  useAuthorizedDevice(device: AuthorizedBluetoothDevice) {
    this.core.useDevice(device);
  }

  async restoreAuthorizedDevice() {
    const devices = await this.listAuthorizedDevices();
    const remembered = devices[0] ?? null;
    if (remembered) this.useAuthorizedDevice(remembered);
    return remembered;
  }

  requestDevice() {
    return this.core.requestDevice({
      allowCompatibleDevices: true,
      includeNameAliases: true,
    });
  }

  pairSecureDevice(disconnectAfterPairing = true) {
    return this.core.pairSecureDevice({ disconnectAfterPairing });
  }

  disconnect() {
    this.core.disconnect();
  }

  /**
   * Rebuild the whole GATT session after a range or transport failure.
   * A disconnected Web Bluetooth characteristic cannot be reused, so the core
   * reconnect performs service and characteristic discovery again. Keep a
   * single promise here so an automatic timer and a manual retry cannot race.
   */
  reconnect() {
    if (this.reconnectPromise) return this.reconnectPromise;
    this.reconnectPromise = (async () => {
      this.onProgress?.({ phase: "connecting", percent: 2, message: t("正在清理旧连接…") });
      this.core.disconnect();
      await new Promise((resolve) => setTimeout(resolve, 350));
      this.onProgress?.({ phase: "connecting", percent: 4, message: t("正在重新连接 TodooCard…") });
      await this.core.connect();
    })().finally(() => {
      this.reconnectPromise = null;
    });
    return this.reconnectPromise;
  }

  static encodeImageData(imageData: ImageData) {
    return CoreTodooCard.encodeImageData(imageData);
  }

  async writeCanvas(canvas: HTMLCanvasElement, disconnectAfterWrite = true, options: WriteCanvasOptions = {}) {
    this.onProgress?.({ phase: "encoding", percent: 8, message: t("正在转换为六色电子纸帧…") });
    const context = canvas.getContext("2d", { willReadFrequently: true });
    if (!context) throw new Error(t("无法读取预览画布"));
    return this.core.writeImageData(
      context.getImageData(0, 0, TODOO_PROTOCOL.width, TODOO_PROTOCOL.height),
      {
        disconnectAfterWrite,
        palette: options.palette,
        // The UI canvas is already rendered to the physical six-colour palette.
        // A second error-diffusion pass creates visible bands and worm patterns.
        dither: options.dither ?? false,
      },
    );
  }

  async writeCalibration(disconnectAfterWrite = true) {
    this.onProgress?.({ phase: "encoding", percent: 8, message: t("正在生成标准六色色卡…") });
    return this.core.writeCalibration({ disconnectAfterWrite });
  }
}
