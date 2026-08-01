/**
 * TodooCard Web Bluetooth driver and six-colour image codec.
 *
 * This module contains no UI, framework, account, AI, storage or routing logic.
 * It is based on one reverse-engineered TodooCard and is not an official SDK.
 */

const UUID_BASE_SUFFIX = "-0000-1000-8000-00805f9b34fb";
const uuid16 = (value) => `0000${value.toString(16).padStart(4, "0")}${UUID_BASE_SUFFIX}`;

function deepFreeze(value) {
  if (value && typeof value === "object" && !Object.isFrozen(value)) {
    for (const child of Object.values(value)) deepFreeze(child);
    Object.freeze(value);
  }
  return value;
}

export const TODOO_COLOR_CODES = deepFreeze({
  black: 0,
  white: 1,
  yellow: 2,
  red: 3,
  blue: 5,
  green: 6,
});

export const TODOO_PALETTE = deepFreeze([
  [0, 0, 0],
  [255, 255, 255],
  [255, 255, 0],
  [255, 0, 0],
  null,
  [0, 0, 255],
  [0, 160, 0],
  null,
]);

export const TODOO_GATT_PROFILES = deepFreeze({
  fef: {
    id: "fef",
    service: uuid16(0xfef0),
    control: uuid16(0xfef1),
    data: uuid16(0xfef2),
    evidence: "本项目 TodooCard / 99803797 真机验证",
    verified: true,
  },
  fdf: {
    id: "fdf",
    service: uuid16(0xfdf0),
    control: uuid16(0xfdf1),
    data: uuid16(0xfdf2),
    evidence: "Sunbelife/TodooCard_Skills 兼容设备实现",
    verified: false,
  },
});

export const TODOO_RENDER_PROFILES = deepFreeze({
  verified: {
    id: "verified",
    description: "本项目真机色板；加权 RGB 距离和蛇形 Floyd-Steinberg",
    palette: TODOO_PALETTE,
    distanceMetric: "weighted-rgb",
    ditherScan: "serpentine",
    verified: true,
  },
  skillT3: {
    id: "skill-t3",
    description: "复现 TodooCard_Skills 的 T3 色板与普通逐行 Floyd-Steinberg",
    palette: [
      [0, 0, 0],
      [255, 255, 255],
      [255, 255, 0],
      [255, 0, 0],
      null,
      [0, 0, 255],
      [0, 255, 0],
      null,
    ],
    distanceMetric: "rgb",
    ditherScan: "raster",
    verified: false,
  },
});

export const TODOO_TRANSFER_PROFILES = deepFreeze({
  verified: {
    id: "verified",
    description: "真机与官方抓包验证的固定 219120-byte 传输",
    payloadMode: "verified-padded",
    payloadBytes: 219120,
    adaptiveBlockSize: false,
    lengthFlags: 0x01,
    completionTimeoutMs: 30000,
    verified: true,
  },
  skillT3: {
    id: "skill-t3",
    description: "TodooCard_Skills 的 218893-byte stored-chunk 兼容传输",
    payloadMode: "skill-stored-short",
    payloadBytes: 218893,
    adaptiveBlockSize: true,
    lengthFlags: 0x01,
    completionTimeoutMs: 90000,
    verified: false,
  },
});

export const TODOO_INPUT_LIMITS = deepFreeze({
  maxBytes: 100000000,
  maxPixels: 50000000,
});

export const TODOO_SKILL_INTEGRATION = deepFreeze({
  source: "https://github.com/Sunbelife/TodooCard_Skills",
  reviewedCommit: "f8b4eaca2d5ad9cb6cf381f68c55864bce665158",
  reviewedAt: "2026-08-01",
  integrated: [
    "FDF0/FDF1/FDF2 兼容 GATT profile",
    "已授权设备列表和只读 GATT probe",
    "可选动态数据块尺寸、短 stored payload 和起始块恢复",
    "设备按钮 flag、方向修正、源图旋转和输入资源上限",
    "可选 skill-t3 色板/抖动 profile",
  ],
  deliberatelyNotDefault: [
    "218893-byte 短帧未在本项目设备上真机验证",
    "FDF profile 未在本项目设备上真机验证",
    "skill-t3 的纯绿色代表值和控制器方向不替换本项目真机默认值",
  ],
});

export const TODOO_PRODUCT_INFO = deepFreeze({
  productName: "TodooCard",
  deviceNumber: "99803797",
  advertisedName: "NEMR99803797",
  observedAddress: "FF:FF:99:80:37:97",
  productUrl: "https://p.todoo.tech/?lang=zh",
  description: "3.7 英寸级六色高分辨率彩色电子纸卡片",
  compatibleNamesReportedBySkill: ["TodooCard", "PotatoCard", "PICKSMART", "NEMR", "T3"],
  screen: {
    technology: "六色彩色电子纸",
    colors: ["黑", "白", "黄", "红", "蓝", "绿"],
    width: 528,
    height: 792,
    orientation: "portrait",
    reportedDiagonalInches: 3.68,
    marketedDiagonalInches: 3.7,
    estimatedDpi: 259,
  },
  evidence: {
    official: ["六色", "3.7 英寸彩色电子纸"],
    communityReported: ["528×792", "3.68 英寸", "约 259 DPI"],
    deviceVerified: [
      "NEMR99803797 广播名",
      "FEF0/FEF1/FEF2 GATT 接口",
      "528×792 图像布局",
      "六色色码和完整 BLE 写入链路",
    ],
  },
});

export const TODOO_PROTOCOL = deepFreeze({
  status: "reverse-engineered-and-device-verified",
  verifiedAt: "2026-08-01",
  transport: "BLE GATT",
  nfcRole: "图片不经 NFC；短贴 NFC 曾使休眠设备恢复 BLE 广播，唤醒机制仍属观察结论",
  uuids: {
    service: TODOO_GATT_PROFILES.fef.service,
    control: TODOO_GATT_PROFILES.fef.control,
    data: TODOO_GATT_PROFILES.fef.data,
  },
  compatibleGattProfiles: TODOO_GATT_PROFILES,
  image: {
    visibleWidth: 528,
    visibleHeight: 792,
    wireWidth: 792,
    wireHeight: 528,
    rotation: "90deg-counter-clockwise",
    allowedCodes: [0, 1, 2, 3, 5, 6],
    forbiddenPixelCodes: [4, 7],
    pixelsPerByte: 2,
    highNibbleFirst: true,
    pixelBytes: 209088,
  },
  frame: {
    prefix: [0, 0, 0, 0],
    blockHeader: [0x74, 0x43, 0x40],
    pixelBytesPerBlock: 64,
    blockCount: 3267,
    framedBytes: 218893,
    transportPaddingBytes: 227,
    payloadBytes: 219120,
  },
  transfer: {
    requestedMtu: 517,
    verifiedMtu: 247,
    gattValueBytes: 244,
    sequenceBytes: 4,
    dataBytesPerPacket: 240,
    packetCount: 913,
    packetIntervalMs: 28,
    controlWriteType: "with-response",
    dataWriteType: "without-response",
  },
  handshake: [
    { phase: "init", write: [0x01], notify: [0x01, 0xf4, 0x00] },
    {
      phase: "length",
      write: [0x02, 0xf0, 0x57, 0x03, 0x00, 0x01],
      notify: [0x02, 0x00, 0x57],
    },
    { phase: "ready", write: [0x03], notify: [0x05, 0, 0, 0, 0, 0] },
  ],
  completionNotification: [0x05, 0x08, 0, 0, 0, 0],
});

export const TODOO_DOCUMENTATION = deepFreeze({
  summary:
    "选择 NEMR 设备，订阅 FEF1，完成三阶段控制握手，再把固定帧拆成 913 个 FEF2 包。05 08 只确认收帧和开始刷新。",
  browserRequirements: [
    "必须在支持 Web Bluetooth 的浏览器中运行",
    "页面必须使用 HTTPS；localhost 可用于本地开发",
    "requestDevice 必须由点击等用户手势直接触发",
    "传输期间页面应保持前台，不能依赖 Service Worker 后台写屏",
    "网页无法主动执行 NFC 唤醒；设备休眠时需要用户手动短贴手机 NFC 区域",
  ],
  writeSequence: [
    "requestDevice() 选择设备",
    "connect() 发现 FEF0、订阅 FEF1",
    "writePayload()/writeImageData() 完成握手和 913 包发送",
    "等待 05 08 协议确认",
    "继续等待实体电子纸刷新稳定；复杂画面实测可能约 3 分钟",
  ],
  safety: [
    "只使用已验证的 FEF1/FEF2 图片写入流程",
    "不访问 OTA、固件或未识别特征",
    "拒绝长度、块头、填充或色码不合法的帧",
    "同一实例拒绝并发写入",
    "可用 expectedDeviceId 把写入锁定到浏览器的稳定授权 ID",
  ],
  limitations: [
    "协议来自单台 TodooCard/当前固件的真机逆向，不是厂商公开 API",
    "Web Bluetooth 不向网页暴露真实 BLE MAC 地址",
    "协议成功不等于相机已经观察到最终显色",
    "skill-t3/FDF 兼容模式来自外部实现，必须显式选择，不替代本机验证默认值",
  ],
  compatibility: {
    default: "verified：FEF profile、219120-byte 固定帧、244-byte GATT value",
    optional:
      "skill-t3：FEF/FDF profile、218893-byte 短 stored 帧、设备协商块长、可选恢复起始包",
    upstream: TODOO_SKILL_INTEGRATION.source,
    upstreamCommit: TODOO_SKILL_INTEGRATION.reviewedCommit,
  },
});

const IMAGE_CODES = Object.freeze([0, 1, 2, 3, 5, 6]);
const IMAGE_CODE_SET = new Set(IMAGE_CODES);
const GATT_PROFILE_LIST = Object.freeze(Object.values(TODOO_GATT_PROFILES));
const COMPATIBLE_NAME_PREFIXES = Object.freeze(["NEMR", "TodooCard", "PotatoCard", "PICKSMART", "T3"]);
const COLOR_NAME_TO_CODE = new Map([
  ...Object.entries(TODOO_COLOR_CODES),
  ["黑", 0],
  ["白", 1],
  ["黄", 2],
  ["红", 3],
  ["蓝", 5],
  ["绿", 6],
]);

const VISIBLE_WIDTH = TODOO_PROTOCOL.image.visibleWidth;
const VISIBLE_HEIGHT = TODOO_PROTOCOL.image.visibleHeight;
const WIRE_WIDTH = TODOO_PROTOCOL.image.wireWidth;
const PIXEL_COUNT = VISIBLE_WIDTH * VISIBLE_HEIGHT;
const PIXEL_BYTES = TODOO_PROTOCOL.image.pixelBytes;
const FRAME_PREFIX_BYTES = TODOO_PROTOCOL.frame.prefix.length;
const BLOCK_HEADER = Uint8Array.from(TODOO_PROTOCOL.frame.blockHeader);
const BLOCK_HEADER_BYTES = BLOCK_HEADER.length;
const BLOCK_PIXEL_BYTES = TODOO_PROTOCOL.frame.pixelBytesPerBlock;
const BLOCK_BYTES = BLOCK_HEADER_BYTES + BLOCK_PIXEL_BYTES;
const BLOCK_COUNT = TODOO_PROTOCOL.frame.blockCount;
const FRAMED_BYTES = TODOO_PROTOCOL.frame.framedBytes;
const PAYLOAD_BYTES = TODOO_PROTOCOL.frame.payloadBytes;
const DATA_BYTES_PER_PACKET = TODOO_PROTOCOL.transfer.dataBytesPerPacket;
const PACKET_BYTES = TODOO_PROTOCOL.transfer.gattValueBytes;
const TOTAL_PACKETS = TODOO_PROTOCOL.transfer.packetCount;

function resolveTransferProfile(value = "verified") {
  if (value === TODOO_TRANSFER_PROFILES.verified) return TODOO_TRANSFER_PROFILES.verified;
  if (value === TODOO_TRANSFER_PROFILES.skillT3) return TODOO_TRANSFER_PROFILES.skillT3;
  if (value && typeof value === "object" && value.id) return resolveTransferProfile(value.id);
  if (value === "verified" || value === "verified-padded") return TODOO_TRANSFER_PROFILES.verified;
  if (value === "skillT3" || value === "skill-t3" || value === "skill-stored-short") {
    return TODOO_TRANSFER_PROFILES.skillT3;
  }
  throw new TodooCardError(`未知传输 profile：${String(value)}`, { code: "INVALID_TRANSFER_PROFILE" });
}

function resolvePayloadMode(value = "verified-padded") {
  if (value === "auto") return "auto";
  return resolveTransferProfile(value).payloadMode;
}

function resolveRenderProfile(value = "verified") {
  if (value && typeof value === "object" && value.palette) return value;
  if (value === "verified") return TODOO_RENDER_PROFILES.verified;
  if (value === "skillT3" || value === "skill-t3") return TODOO_RENDER_PROFILES.skillT3;
  throw new TodooCardError(`未知渲染 profile：${String(value)}`, { code: "INVALID_RENDER_PROFILE" });
}

function normalizeSourceRotation(value = 0) {
  const aliases = new Map([
    ["normal", 0],
    ["rotate-right-90", 90],
    ["right-90", 90],
    ["rotate-180", 180],
    ["rotate-left-90", 270],
    ["left-90", 270],
  ]);
  const numeric = typeof value === "string" ? aliases.get(value) : Number(value);
  if (![0, 90, 180, 270].includes(numeric)) {
    throw new TodooCardError("sourceRotation 只能是 0/90/180/270 或方向别名", {
      code: "INVALID_ORIENTATION",
    });
  }
  return numeric;
}

function normalizeScreenOrientation(value = "normal") {
  const aliases = {
    normal: "normal",
    "rotate-180": "rotate-180",
    "flip-horizontal": "flip-horizontal",
    "flip-vertical": "flip-vertical",
    "rotate-180-then-flip-horizontal": "flip-vertical",
  };
  const normalized = aliases[value];
  if (!normalized) {
    throw new TodooCardError(`未知屏幕方向修正：${String(value)}`, {
      code: "INVALID_ORIENTATION",
    });
  }
  return normalized;
}

const DEFAULT_TIMING = Object.freeze({
  connectTimeoutMs: 15000,
  discoveryTimeoutMs: 12000,
  characteristicWriteTimeoutMs: 8000,
  notificationSettleMs: 750,
  afterInitMs: 800,
  afterLengthMs: 400,
  beforeDataMs: 30,
  packetIntervalMs: 28,
  controlTimeoutMs: 10000,
  completionTimeoutMs: 30000,
  disconnectDelayMs: 350,
});

export class TodooCardError extends Error {
  constructor(message, { code = "TODOO_ERROR", cause, hint, details } = {}) {
    super(message);
    this.name = new.target.name;
    this.code = code;
    if (cause !== undefined) this.cause = cause;
    if (hint !== undefined) this.hint = hint;
    if (details !== undefined) this.details = details;
  }
}

export class TodooCardConnectionError extends TodooCardError {
  constructor(message, options = {}) {
    super(message, { code: "CONNECTION_FAILED", ...options });
  }
}

export class TodooCardProtocolError extends TodooCardError {
  constructor(message, options = {}) {
    super(message, { code: "PROTOCOL_ERROR", ...options });
  }
}

export class TodooCardAbortError extends TodooCardError {
  constructor(message = "TodooCard 操作已取消", options = {}) {
    super(message, { code: "ABORTED", ...options });
  }
}

function toUint8Array(value, label = "数据") {
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value)) {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  }
  if (Array.isArray(value)) return Uint8Array.from(value);
  throw new TypeError(`${label}必须是 Uint8Array、ArrayBuffer、TypedArray 或数字数组`);
}

function bytesEqual(actual, expected) {
  if (actual.length !== expected.length) return false;
  for (let index = 0; index < actual.length; index += 1) {
    if (actual[index] !== expected[index]) return false;
  }
  return true;
}

function hex(bytes) {
  return Array.from(bytes, (value) => value.toString(16).padStart(2, "0")).join(" ").toUpperCase();
}

function resolveColorCode(value) {
  if (typeof value === "string") {
    const code = COLOR_NAME_TO_CODE.get(value.trim().toLowerCase());
    if (code !== undefined) return code;
  }
  if (Number.isInteger(value) && IMAGE_CODE_SET.has(value)) return value;
  throw new TodooCardError(`图片色码只能是 0/1/2/3/5/6，收到 ${String(value)}`, {
    code: "INVALID_COLOR_CODE",
  });
}

function throwIfAborted(signal) {
  if (signal?.aborted) {
    throw new TodooCardAbortError("TodooCard 操作已取消", { cause: signal.reason });
  }
}

function defaultSleep(milliseconds, signal) {
  throwIfAborted(signal);
  if (milliseconds <= 0) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      signal?.removeEventListener("abort", onAbort);
      resolve();
    }, milliseconds);
    const onAbort = () => {
      clearTimeout(timer);
      signal?.removeEventListener("abort", onAbort);
      reject(new TodooCardAbortError("TodooCard 操作已取消", { cause: signal.reason }));
    };
    signal?.addEventListener("abort", onAbort, { once: true });
  });
}

function normalizePalette(palette) {
  if (!Array.isArray(palette) || palette.length !== 8) {
    throw new TodooCardError("色板必须是按协议码索引的 8 项数组", { code: "INVALID_PALETTE" });
  }
  const normalized = new Array(8).fill(null);
  for (const code of IMAGE_CODES) {
    const rgb = palette[code];
    if (!Array.isArray(rgb) || rgb.length !== 3 || rgb.some((part) => !Number.isFinite(part))) {
      throw new TodooCardError(`色板缺少色码 ${code} 的 [R,G,B]`, { code: "INVALID_PALETTE" });
    }
    normalized[code] = rgb.map((part) => Math.max(0, Math.min(255, Number(part))));
  }
  return normalized;
}

function validationFailure(code, error, details = {}) {
  return { valid: false, code, error, details };
}

function serializeError(error) {
  return {
    name: error?.name ?? "Error",
    code: error?.code,
    message: error?.message ?? String(error),
    hint: error?.hint,
  };
}

function getDrawableDimensions(source) {
  const width = Number(source?.naturalWidth ?? source?.videoWidth ?? source?.width);
  const height = Number(source?.naturalHeight ?? source?.videoHeight ?? source?.height);
  if (!(width > 0) || !(height > 0)) {
    throw new TodooCardError("图片尚未加载，或无法读取宽高", { code: "INVALID_IMAGE_SOURCE" });
  }
  return { width, height };
}

/**
 * Framework-free TodooCard driver. A single instance represents one selected browser device.
 */
export class TodooCard {
  static get productInfo() {
    return TODOO_PRODUCT_INFO;
  }

  static get protocol() {
    return TODOO_PROTOCOL;
  }

  static get documentation() {
    return TODOO_DOCUMENTATION;
  }

  static get colorCodes() {
    return TODOO_COLOR_CODES;
  }

  static get gattProfiles() {
    return TODOO_GATT_PROFILES;
  }

  static get renderProfiles() {
    return TODOO_RENDER_PROFILES;
  }

  static get transferProfiles() {
    return TODOO_TRANSFER_PROFILES;
  }

  static get inputLimits() {
    return TODOO_INPUT_LIMITS;
  }

  static get skillIntegration() {
    return TODOO_SKILL_INTEGRATION;
  }

  constructor({
    bluetooth = globalThis.navigator?.bluetooth,
    logger = null,
    timing = {},
    sleep = defaultSleep,
    deviceNamePrefix = "NEMR",
    expectedDeviceId = null,
  } = {}) {
    this._bluetooth = bluetooth;
    this._logger = logger;
    this._timing = Object.freeze({ ...DEFAULT_TIMING, ...timing });
    this._sleepImpl = sleep;
    this._deviceNamePrefix = deviceNamePrefix;
    this._expectedDeviceId = expectedDeviceId;

    this._device = null;
    this._server = null;
    this._service = null;
    this._gattProfile = null;
    this._control = null;
    this._data = null;
    this._notificationReadyAt = 0;
    this._state = "idle";
    this._listeners = new Map();
    this._notificationWaiters = new Set();
    this._connectPromise = null;
    this._activeConnectController = null;
    this._writePromise = null;
    this._activeWriteController = null;
    this._boundNotification = (event) => this._handleNotification(event);
    this._boundDisconnected = (event) => this._handleDisconnected(event);
  }

  get productInfo() {
    return TODOO_PRODUCT_INFO;
  }

  get protocol() {
    return TODOO_PROTOCOL;
  }

  get documentation() {
    return TODOO_DOCUMENTATION;
  }

  get skillIntegration() {
    return TODOO_SKILL_INTEGRATION;
  }

  get device() {
    return this._device;
  }

  get state() {
    return this._state;
  }

  get isWriting() {
    return this._writePromise !== null;
  }

  get isConnected() {
    return Boolean(this._device?.gatt?.connected && this._control && this._data);
  }

  get status() {
    return {
      state: this._state,
      supported: this.isSupported(),
      writing: this.isWriting,
      device: this._device
        ? {
            id: this._device.id ?? null,
            name: this._device.name ?? null,
            connected: Boolean(this._device.gatt?.connected),
          }
        : null,
      gattProfile: this._gattProfile?.id ?? null,
    };
  }

  isSupported() {
    return Boolean(this._bluetooth && typeof this._bluetooth.requestDevice === "function");
  }

  async getAvailability() {
    if (!this.isSupported()) return false;
    if (typeof this._bluetooth.getAvailability !== "function") return true;
    try {
      return Boolean(await this._bluetooth.getAvailability());
    } catch {
      return false;
    }
  }

  on(type, handler) {
    if (typeof handler !== "function") throw new TypeError("事件处理器必须是函数");
    let handlers = this._listeners.get(type);
    if (!handlers) {
      handlers = new Set();
      this._listeners.set(type, handlers);
    }
    handlers.add(handler);
    return () => this.off(type, handler);
  }

  once(type, handler) {
    const unsubscribe = this.on(type, (event) => {
      unsubscribe();
      handler(event);
    });
    return unsubscribe;
  }

  off(type, handler) {
    const handlers = this._listeners.get(type);
    if (!handlers) return false;
    const removed = handlers.delete(handler);
    if (handlers.size === 0) this._listeners.delete(type);
    return removed;
  }

  /**
   * Opens the browser chooser. Call this method directly from a click/tap handler.
   */
  async requestDevice({
    deviceNumber = TODOO_PRODUCT_INFO.deviceNumber,
    name,
    namePrefix = this._deviceNamePrefix,
    allowCompatibleDevices = true,
    includeNameAliases = true,
    expectedDeviceId = this._expectedDeviceId,
  } = {}) {
    this._assertBrowserAccess();
    if (this.isWriting) {
      throw new TodooCardError("写屏进行中，不能重新选择设备", { code: "BUSY" });
    }

    const exactName = name ?? (deviceNumber ? `NEMR${deviceNumber}` : null);
    const filters = [];
    if (exactName) filters.push({ name: exactName });
    if (allowCompatibleDevices) {
      for (const profile of GATT_PROFILE_LIST) filters.push({ services: [profile.service] });
      if (includeNameAliases) {
        for (const prefix of COMPATIBLE_NAME_PREFIXES) filters.push({ namePrefix: prefix });
      } else if (namePrefix) {
        filters.push({ namePrefix });
      }
    }
    if (filters.length === 0) filters.push({ namePrefix });

    this._setState("selecting-device");
    this._log("info", "打开 Web Bluetooth 设备选择器", { filters });
    try {
      const device = await this._bluetooth.requestDevice({
        filters,
        optionalServices: GATT_PROFILE_LIST.map((profile) => profile.service),
      });
      if (expectedDeviceId && device.id !== expectedDeviceId) {
        throw new TodooCardError(
          `选择的设备 ID 不匹配：收到 ${device.id ?? "(empty)"}`,
          {
            code: "DEVICE_ID_MISMATCH",
            details: { expectedDeviceId, actualDeviceId: device.id ?? null },
          },
        );
      }
      this.useDevice(device, { expectedDeviceId });
      return device;
    } catch (error) {
      if (error instanceof TodooCardError) {
        this._setState("error", { error: serializeError(error) });
        throw error;
      }
      const cancelled = error?.name === "NotFoundError";
      const mapped = new TodooCardError(cancelled ? "未选择 TodooCard 设备" : "无法选择蓝牙设备", {
        code: cancelled ? "DEVICE_NOT_SELECTED" : "DEVICE_SELECTION_FAILED",
        cause: error,
        hint: cancelled ? undefined : "确认页面使用 HTTPS，并允许浏览器访问附近设备。",
      });
      this._setState("error", { error: serializeError(mapped) });
      throw mapped;
    }
  }

  /** Lists devices already authorized for this origin; it does not open a BLE scan chooser. */
  async listAuthorizedDevices({
    compatibleOnly = true,
    expectedDeviceId = this._expectedDeviceId,
  } = {}) {
    this._assertBrowserAccess();
    if (typeof this._bluetooth.getDevices !== "function") {
      throw new TodooCardError("当前浏览器不支持 navigator.bluetooth.getDevices()", {
        code: "AUTHORIZED_DEVICE_LIST_UNSUPPORTED",
      });
    }
    const devices = await this._bluetooth.getDevices();
    return devices.filter((device) => {
      if (expectedDeviceId) return device.id === expectedDeviceId;
      if (!compatibleOnly) return true;
      const name = device.name ?? "";
      return COMPATIBLE_NAME_PREFIXES.some((prefix) =>
        name.toLocaleLowerCase().startsWith(prefix.toLocaleLowerCase()),
      );
    });
  }

  /** Attach a BluetoothDevice obtained by requestDevice() or restored by getDevices(). */
  useDevice(device, { expectedDeviceId = this._expectedDeviceId } = {}) {
    if (!device?.gatt || typeof device.gatt.connect !== "function") {
      throw new TodooCardError("传入对象不是可连接的 BluetoothDevice", { code: "INVALID_DEVICE" });
    }
    if (expectedDeviceId && device.id !== expectedDeviceId) {
      throw new TodooCardError(`设备 ID 不匹配：收到 ${device.id ?? "(empty)"}`, {
        code: "DEVICE_ID_MISMATCH",
        details: { expectedDeviceId, actualDeviceId: device.id ?? null },
      });
    }
    if (this.isWriting) throw new TodooCardError("写屏进行中，不能更换设备", { code: "BUSY" });
    if (this._device && this._device !== device) this._releaseGatt({ preserveState: true });
    if (this._device !== device) {
      this._device?.removeEventListener?.("gattserverdisconnected", this._boundDisconnected);
      this._device = device;
      device.addEventListener?.("gattserverdisconnected", this._boundDisconnected);
    }
    this._setState("device-selected", { device: this.status.device });
    return this;
  }

  /** Connects and reports the selected GATT profile/properties without sending image commands. */
  async probe({ signal, disconnectAfterProbe = false } = {}) {
    await this.connect({ signal });
    const propertiesOf = (characteristic) => ({
      read: Boolean(characteristic?.properties?.read),
      write: Boolean(characteristic?.properties?.write),
      writeWithoutResponse: Boolean(characteristic?.properties?.writeWithoutResponse),
      notify: Boolean(characteristic?.properties?.notify),
      indicate: Boolean(characteristic?.properties?.indicate),
    });
    const result = {
      device: this.status.device,
      gattProfile: this._gattProfile ? { ...this._gattProfile } : null,
      control: {
        uuid: this._control?.uuid ?? this._gattProfile?.control ?? null,
        properties: propertiesOf(this._control),
      },
      data: {
        uuid: this._data?.uuid ?? this._gattProfile?.data ?? null,
        properties: propertiesOf(this._data),
      },
      imageCommandsSent: false,
      notificationSubscriptionEnabled: true,
    };
    this._emit("probe", result);
    if (disconnectAfterProbe) this._releaseGatt();
    return result;
  }

  /** Connects, discovers a supported FEF/FDF profile, and enables control notifications. */
  connect({ signal } = {}) {
    if (this.isConnected) return Promise.resolve(this.status.device);
    if (this._connectPromise) return this._connectPromise;
    if (!this._device) {
      return Promise.reject(
        new TodooCardError("尚未选择设备；请先在用户点击事件中调用 requestDevice()", {
          code: "NO_DEVICE",
        }),
      );
    }
    this._assertBrowserAccess();

    const controller = new AbortController();
    const relayAbort = () => controller.abort(signal.reason);
    if (signal?.aborted) relayAbort();
    else signal?.addEventListener("abort", relayAbort, { once: true });
    this._activeConnectController = controller;
    const run = this._connectInternal(controller.signal);
    const tracked = run.finally(() => {
      signal?.removeEventListener("abort", relayAbort);
      if (this._activeConnectController === controller) this._activeConnectController = null;
      if (this._connectPromise === tracked) this._connectPromise = null;
    });
    this._connectPromise = tracked;
    return tracked;
  }

  async _connectInternal(signal) {
    throwIfAborted(signal);
    this._setState("connecting");
    this._log("info", "连接 TodooCard", {
      name: this._device.name ?? null,
      id: this._device.id ?? null,
    });

    try {
      this._server = await this._runWithTimeout(
        () => this._device.gatt.connect(),
        this._timing.connectTimeoutMs,
        signal,
        "连接 TodooCard 超时",
      );
      this._setState("discovering");
      const discovered = await this._runWithTimeout(
        () => this._discoverSupportedGatt(),
        this._timing.discoveryTimeoutMs,
        signal,
        "发现 FEF/FDF 图像服务超时",
      );
      this._gattProfile = discovered.profile;
      this._service = discovered.service;
      this._control = discovered.control;
      this._data = discovered.data;

      if (typeof this._control.startNotifications !== "function") {
        throw new TodooCardConnectionError("浏览器或设备不支持控制特征 notification");
      }
      this._control.addEventListener?.("characteristicvaluechanged", this._boundNotification);
      this._setState("subscribing");
      await this._runWithTimeout(
        () => this._control.startNotifications(),
        this._timing.discoveryTimeoutMs,
        signal,
        "订阅 FEF1 notification 超时",
      );
      this._notificationReadyAt = Date.now() + this._timing.notificationSettleMs;
      this._setState("ready");
      this._log("info", `已连接并订阅 ${this._gattProfile.id.toUpperCase()}1`, this.status);
      return this.status.device;
    } catch (error) {
      const mapped = this._mapConnectionError(error);
      this._setState(mapped.code === "ABORTED" ? "cancelled" : "error", {
        error: serializeError(mapped),
      });
      this._releaseGatt({ preserveState: true, reason: mapped });
      throw mapped;
    }
  }

  async _discoverSupportedGatt() {
    let lastError;
    for (const profile of GATT_PROFILE_LIST) {
      try {
        const service = await this._server.getPrimaryService(profile.service);
        const [control, data] = await Promise.all([
          service.getCharacteristic(profile.control),
          service.getCharacteristic(profile.data),
        ]);
        return { profile, service, control, data };
      } catch (error) {
        lastError = error;
        this._log("debug", `未发现 ${profile.id.toUpperCase()} profile`, {
          error: serializeError(error),
        });
      }
    }
    throw new TodooCardConnectionError("设备缺少 FEF0/1/2 和 FDF0/1/2 图像服务", {
      code: "GATT_SERVICE_MISSING",
      cause: lastError,
    });
  }

  /** Disconnects and cancels an active transfer, if any. */
  disconnect() {
    this.cancel("用户断开 TodooCard");
    this._releaseGatt({ preserveState: true });
    this._setState("disconnected");
  }

  cancel(reason = "用户取消写屏") {
    let cancelled = false;
    if (this._activeWriteController && !this._activeWriteController.signal.aborted) {
      this._activeWriteController.abort(reason);
      cancelled = true;
    }
    if (this._activeConnectController && !this._activeConnectController.signal.aborted) {
      this._activeConnectController.abort(reason);
      cancelled = true;
    }
    return cancelled;
  }

  /** Validates and writes a payload using an explicit transfer profile. */
  writePayload(payload, options = {}) {
    if (this.isWriting) {
      return Promise.reject(new TodooCardError("已有写屏任务进行中", { code: "BUSY" }));
    }
    let bytes;
    let transferProfile;
    try {
      transferProfile = resolveTransferProfile(options.transferProfile ?? "verified");
      bytes = Uint8Array.from(toUint8Array(payload, "TodooCard 帧"));
      TodooCard.assertValidPayload(bytes, { payloadMode: transferProfile.payloadMode });
    } catch (error) {
      return Promise.reject(error);
    }

    const run = this._performWrite(bytes, { ...options, transferProfile });
    const tracked = run.finally(() => {
      if (this._writePromise === tracked) this._writePromise = null;
    });
    this._writePromise = tracked;
    return tracked;
  }

  async _performWrite(
    payload,
    {
      signal: externalSignal,
      disconnectAfterWrite = true,
      disconnectOnError = true,
      transferProfile = TODOO_TRANSFER_PROFILES.verified,
      allowDeviceButton = false,
      resumeFromDevice = false,
      packetIntervalMs,
      completionTimeoutMs,
      expectedDeviceId = this._expectedDeviceId,
    } = {},
  ) {
    const controller = new AbortController();
    this._activeWriteController = controller;
    const relayAbort = () => controller.abort(externalSignal.reason);
    if (externalSignal?.aborted) relayAbort();
    else externalSignal?.addEventListener("abort", relayAbort, { once: true });
    const signal = controller.signal;
    let completed = false;

    try {
      if (!this._device) {
        throw new TodooCardError("尚未选择设备；请先在点击事件中调用 requestDevice()", {
          code: "NO_DEVICE",
        });
      }
      if (expectedDeviceId && this._device.id !== expectedDeviceId) {
        throw new TodooCardError(`拒绝写入非目标设备：${this._device.id ?? "(empty)"}`, {
          code: "DEVICE_ID_MISMATCH",
          details: { expectedDeviceId, actualDeviceId: this._device.id ?? null },
        });
      }
      const transfer = resolveTransferProfile(transferProfile);
      await this.connect({ signal });
      if (!this._gattProfile?.verified && transfer.verified) {
        throw new TodooCardProtocolError(
          `${this._gattProfile?.id?.toUpperCase() ?? "未知"} GATT profile 未验证默认 219120-byte 写法`,
          {
            code: "GATT_PROFILE_REQUIRES_COMPATIBILITY_MODE",
            hint: "确认设备型号后，显式传 transferProfile: \"skill-t3\"。",
          },
        );
      }
      if (this._data?.properties?.writeWithoutResponse === false) {
        throw new TodooCardProtocolError("当前图像 profile 需要数据特征支持 Write Without Response", {
          code: "DATA_WRITE_WITHOUT_RESPONSE_UNSUPPORTED",
        });
      }
      if (!transfer.verified || !this._gattProfile?.verified) {
        this._emit("warning", {
          code: "EXPERIMENTAL_COMPATIBILITY_PROFILE",
          message: `正在使用 ${transfer.id}/${this._gattProfile?.id ?? "unknown"} 兼容模式`,
          transferProfile: transfer.id,
          gattProfile: this._gattProfile?.id ?? null,
        });
      }
      const settleRemaining = Math.max(0, this._notificationReadyAt - Date.now());
      if (settleRemaining > 0) {
        this._setState("notification-settle");
        await this._sleep(settleRemaining, signal);
      }

      this._setState("handshake-init");
      const init = await this._exchangeControl([0x01], 0x01, signal, "初始化");
      if (init.length < 3 || init[2] !== 0) {
        throw new TodooCardProtocolError(`设备拒绝块长协商：${hex(init)}`, {
          code: "BLOCK_SIZE_REJECTED",
        });
      }
      const acceptedValueBytes = init[1];
      if (!transfer.adaptiveBlockSize && acceptedValueBytes !== PACKET_BYTES) {
        throw new TodooCardProtocolError(
          `设备声明的 GATT value 上限为 ${acceptedValueBytes} bytes，需要 ${PACKET_BYTES} bytes`,
          {
            code: "UNSUPPORTED_PACKET_SIZE",
            hint: "当前网页驱动只实现了真机验证过的 MTU 247 / 244-byte 固定分包。",
          },
        );
      }
      if (acceptedValueBytes <= 4) {
        throw new TodooCardProtocolError(`设备声明的 GATT value 上限无效：${acceptedValueBytes}`, {
          code: "UNSUPPORTED_PACKET_SIZE",
        });
      }
      if (transfer.verified) {
        this._assertNotification(init, TODOO_PROTOCOL.handshake[0].notify, "初始化");
      }
      const dataBytesPerPacket = acceptedValueBytes - 4;
      const totalPackets = Math.ceil(payload.length / dataBytesPerPacket);

      await this._sleep(this._timing.afterInitMs, signal);
      this._setState("handshake-length");
      const lengthCommand = Uint8Array.from([
        0x02,
        payload.length & 0xff,
        (payload.length >>> 8) & 0xff,
        (payload.length >>> 16) & 0xff,
        (payload.length >>> 24) & 0xff,
        transfer.lengthFlags | (allowDeviceButton ? 0x10 : 0),
      ]);
      const lengthAck = await this._exchangeControl(
        lengthCommand,
        0x02,
        signal,
        "声明帧长度",
      );
      if (lengthAck.length < 2 || lengthAck[1] !== 0) {
        throw new TodooCardProtocolError(`设备拒绝帧长度：${hex(lengthAck)}`, {
          code: "PAYLOAD_LENGTH_REJECTED",
        });
      }
      if (transfer.verified) {
        this._assertNotification(lengthAck, TODOO_PROTOCOL.handshake[1].notify, "帧长度");
      }

      await this._sleep(this._timing.afterLengthMs, signal);
      this._setState("handshake-ready");
      const ready = await this._exchangeControl([0x03], 0x05, signal, "进入数据阶段");
      if (ready.length < 6 || ready[1] !== 0) {
        throw new TodooCardProtocolError(`设备未进入数据接收状态：${hex(ready)}`, {
          code: "DATA_STAGE_REJECTED",
        });
      }
      const requestedStartIndex =
        ready[2] | (ready[3] << 8) | (ready[4] << 16) | (ready[5] << 24);
      if (transfer.verified) {
        this._assertNotification(ready, TODOO_PROTOCOL.handshake[2].notify, "数据就绪");
      } else if (requestedStartIndex !== 0 && !resumeFromDevice) {
        throw new TodooCardProtocolError(
          `设备请求从第 ${requestedStartIndex} 包恢复，但 resumeFromDevice 未开启`,
          {
            code: "RESUME_NOT_ALLOWED",
            hint: "只有确认设备缓存的是同一 payload 时才应开启 resumeFromDevice。",
          },
        );
      }
      if (requestedStartIndex < 0 || requestedStartIndex >= totalPackets) {
        throw new TodooCardProtocolError(`设备请求的起始包越界：${requestedStartIndex}/${totalPackets}`, {
          code: "INVALID_RESUME_INDEX",
        });
      }

      await this._sleep(this._timing.beforeDataMs, signal);
      this._setState("sending");
      this._emitProgress(requestedStartIndex, totalPackets, dataBytesPerPacket, payload.length);
      let completionWaiter = null;
      const resolvedPacketIntervalMs = packetIntervalMs ?? this._timing.packetIntervalMs;
      const resolvedCompletionTimeoutMs =
        completionTimeoutMs ??
        (transfer.verified
          ? this._timing.completionTimeoutMs
          : Math.max(this._timing.completionTimeoutMs, transfer.completionTimeoutMs));

      for (let packetIndex = requestedStartIndex; packetIndex < totalPackets; packetIndex += 1) {
        throwIfAborted(signal);
        if (!this.isConnected) {
          throw new TodooCardConnectionError("发送过程中 TodooCard 已断开", {
            hint: "让卡片保持有电并靠近手机；若不广播，可手动短贴 NFC 区域后重试。",
          });
        }

        if (packetIndex === totalPackets - 1) {
          completionWaiter = this._createNotificationWaiter({
            opcode: 0x05,
            timeoutMs: resolvedCompletionTimeoutMs,
            signal,
            label: "完整帧确认",
            predicate: (bytes) => bytes.length >= 2 && bytes[1] === 0x08,
          });
        }

        const payloadOffset = packetIndex * dataBytesPerPacket;
        const chunk = payload.subarray(
          payloadOffset,
          Math.min(payload.length, payloadOffset + dataBytesPerPacket),
        );
        const packet = new Uint8Array(4 + chunk.length);
        packet[0] = packetIndex & 0xff;
        packet[1] = (packetIndex >>> 8) & 0xff;
        packet[2] = (packetIndex >>> 16) & 0xff;
        packet[3] = (packetIndex >>> 24) & 0xff;
        packet.set(chunk, 4);

        try {
          await this._writeData(packet, signal);
        } catch (error) {
          completionWaiter?.cancel(error);
          await completionWaiter?.promise.catch(() => {});
          throw error;
        }
        this._emitProgress(packetIndex + 1, totalPackets, dataBytesPerPacket, payload.length);

        if (packetIndex + 1 < totalPackets) {
          await this._sleep(resolvedPacketIntervalMs, signal);
        }
      }

      this._setState("waiting-complete");
      const completion = await completionWaiter.promise;
      if (transfer.verified) {
        this._assertNotification(
          completion,
          TODOO_PROTOCOL.completionNotification,
          "完整帧确认",
        );
      }
      completed = true;
      const result = {
        success: true,
        protocolConfirmed: true,
        physicalRefreshPending: true,
        payloadBytes: payload.length,
        packets: totalPackets,
        packetValueBytes: acceptedValueBytes,
        dataBytesPerPacket,
        resumedFromPacket: requestedStartIndex,
        transferProfile: transfer.id,
        gattProfile: this._gattProfile?.id ?? null,
        completionNotification: Uint8Array.from(completion),
        message: "设备已用 05 08 确认完整帧；电子纸仍可能继续刷新约数分钟",
      };
      this._setState("complete", result);
      this._emit("complete", result);

      if (disconnectAfterWrite) {
        await this._sleep(this._timing.disconnectDelayMs);
        this._releaseGatt({ preserveState: true });
      }
      return result;
    } catch (error) {
      const mapped = this._mapWriteError(error);
      this._setState(mapped.code === "ABORTED" ? "cancelled" : "error", {
        error: serializeError(mapped),
      });
      this._emit("error", { error: mapped });
      if (disconnectOnError) this._releaseGatt({ preserveState: true, reason: mapped });
      throw mapped;
    } finally {
      externalSignal?.removeEventListener("abort", relayAbort);
      if (this._activeWriteController === controller) this._activeWriteController = null;
      if (!completed) this._rejectNotificationWaiters(new TodooCardAbortError("写屏任务已结束"));
    }
  }

  /** Quantizes exact-size ImageData and writes it. */
  writeImageData(imageData, options = {}) {
    const {
      dither = true,
      renderProfile = "verified",
      palette,
      distanceMetric,
      ditherScan,
      screenOrientation = "normal",
      transferProfile = "verified",
      payloadMode,
      ...writeOptions
    } = options;
    const transfer = resolveTransferProfile(transferProfile);
    const payload = TodooCard.encodeImageData(imageData, {
      dither,
      renderProfile,
      palette,
      distanceMetric,
      ditherScan,
      screenOrientation,
      payloadMode: payloadMode ?? transfer.payloadMode,
    });
    return this.writePayload(payload, { ...writeOptions, transferProfile: transfer });
  }

  /** Center-crops a browser image source, quantizes it, and writes it. */
  async writeImageSource(source, options = {}) {
    const {
      dither = true,
      renderProfile = "verified",
      palette,
      distanceMetric,
      ditherScan,
      screenOrientation = "normal",
      sourceRotation = 0,
      transferProfile = "verified",
      payloadMode,
      maxInputBytes = TODOO_INPUT_LIMITS.maxBytes,
      maxSourcePixels = TODOO_INPUT_LIMITS.maxPixels,
      canvasFactory,
      ...writeOptions
    } = options;
    const transfer = resolveTransferProfile(transferProfile);
    const payload = await TodooCard.encodeImageSource(source, {
      dither,
      renderProfile,
      palette,
      distanceMetric,
      ditherScan,
      screenOrientation,
      sourceRotation,
      payloadMode: payloadMode ?? transfer.payloadMode,
      maxInputBytes,
      maxSourcePixels,
      canvasFactory,
    });
    return this.writePayload(payload, { ...writeOptions, transferProfile: transfer });
  }

  writeCalibration(options = {}) {
    const {
      transferProfile = "verified",
      screenOrientation = "normal",
      payloadMode,
      ...writeOptions
    } = options;
    const transfer = resolveTransferProfile(transferProfile);
    return this.writePayload(
      TodooCard.createCalibrationPayload({
        screenOrientation,
        payloadMode: payloadMode ?? transfer.payloadMode,
      }),
      { ...writeOptions, transferProfile: transfer },
    );
  }

  writeSolid(colorOrCode, options = {}) {
    const {
      transferProfile = "verified",
      screenOrientation = "normal",
      payloadMode,
      ...writeOptions
    } = options;
    const transfer = resolveTransferProfile(transferProfile);
    return this.writePayload(
      TodooCard.createSolidPayload(colorOrCode, {
        screenOrientation,
        payloadMode: payloadMode ?? transfer.payloadMode,
      }),
      { ...writeOptions, transferProfile: transfer },
    );
  }

  static createSolidPayload(colorOrCode, options = {}) {
    const code = resolveColorCode(colorOrCode);
    const visibleCodes = new Uint8Array(PIXEL_COUNT);
    visibleCodes.fill(code);
    return TodooCard.encodeVisibleCodes(visibleCodes, options);
  }

  static createCalibrationPayload(options = {}) {
    const bands = [1, 0, 3, 2, 6, 5];
    const visibleCodes = new Uint8Array(PIXEL_COUNT);
    for (let y = 0; y < VISIBLE_HEIGHT; y += 1) {
      const code = bands[Math.min(bands.length - 1, Math.floor((y * bands.length) / VISIBLE_HEIGHT))];
      visibleCodes.fill(code, y * VISIBLE_WIDTH, (y + 1) * VISIBLE_WIDTH);
    }
    TodooCard._fillVisibleRect(visibleCodes, 8, 8, 64, 64, 3);
    TodooCard._fillVisibleRect(visibleCodes, VISIBLE_WIDTH - 72, 8, 64, 64, 6);
    TodooCard._fillVisibleRect(visibleCodes, 8, VISIBLE_HEIGHT - 72, 64, 64, 5);
    TodooCard._fillVisibleRect(
      visibleCodes,
      VISIBLE_WIDTH - 72,
      VISIBLE_HEIGHT - 72,
      64,
      64,
      2,
    );
    return TodooCard.encodeVisibleCodes(visibleCodes, options);
  }

  /** Applies a logical panel-mount correction before the verified controller rotation. */
  static transformVisibleCodes(input, orientation = "normal") {
    const source = toUint8Array(input, "可见像素色码");
    if (source.length !== PIXEL_COUNT) {
      throw new TodooCardError(`可见像素数量必须为 ${PIXEL_COUNT}`, {
        code: "INVALID_PIXEL_COUNT",
      });
    }
    const normalized = normalizeScreenOrientation(orientation);
    const output = new Uint8Array(PIXEL_COUNT);
    for (let y = 0; y < VISIBLE_HEIGHT; y += 1) {
      for (let x = 0; x < VISIBLE_WIDTH; x += 1) {
        const sourceIndex = y * VISIBLE_WIDTH + x;
        const code = source[sourceIndex];
        if (!IMAGE_CODE_SET.has(code)) {
          throw new TodooCardError(
            `图片色码只能是 0/1/2/3/5/6；像素 (${x},${y}) 收到 ${code}`,
            { code: "INVALID_COLOR_CODE", details: { sourceX: x, sourceY: y, value: code } },
          );
        }
        let destinationX = x;
        let destinationY = y;
        if (normalized === "rotate-180") {
          destinationX = VISIBLE_WIDTH - 1 - x;
          destinationY = VISIBLE_HEIGHT - 1 - y;
        } else if (normalized === "flip-horizontal") {
          destinationX = VISIBLE_WIDTH - 1 - x;
        } else if (normalized === "flip-vertical") {
          destinationY = VISIBLE_HEIGHT - 1 - y;
        }
        output[destinationY * VISIBLE_WIDTH + destinationX] = code;
      }
    }
    return output;
  }

  /**
   * Converts native 528×792 protocol colour codes to a verified or skill-compatible payload.
   */
  static encodeVisibleCodes(
    input,
    { screenOrientation = "normal", payloadMode = "verified-padded" } = {},
  ) {
    const visibleCodes = TodooCard.transformVisibleCodes(input, screenOrientation);
    const resolvedPayloadMode = resolvePayloadMode(payloadMode);
    if (resolvedPayloadMode === "auto") {
      throw new TodooCardError("编码时 payloadMode 不能是 auto", { code: "INVALID_PAYLOAD_MODE" });
    }

    const rotatedCodes = new Uint8Array(PIXEL_COUNT);
    for (let sourceY = 0; sourceY < VISIBLE_HEIGHT; sourceY += 1) {
      for (let sourceX = 0; sourceX < VISIBLE_WIDTH; sourceX += 1) {
        const code = visibleCodes[sourceY * VISIBLE_WIDTH + sourceX];
        const rotatedX = sourceY;
        const rotatedY = VISIBLE_WIDTH - 1 - sourceX;
        rotatedCodes[rotatedY * WIRE_WIDTH + rotatedX] = code;
      }
    }

    const packed = new Uint8Array(PIXEL_BYTES);
    for (let index = 0; index < PIXEL_BYTES; index += 1) {
      packed[index] = (rotatedCodes[index * 2] << 4) | rotatedCodes[index * 2 + 1];
    }

    const outputBytes =
      resolvedPayloadMode === "skill-stored-short" ? FRAMED_BYTES : PAYLOAD_BYTES;
    const payload = new Uint8Array(outputBytes);
    let inputOffset = 0;
    let outputOffset = FRAME_PREFIX_BYTES;
    for (let block = 0; block < BLOCK_COUNT; block += 1) {
      payload.set(BLOCK_HEADER, outputOffset);
      outputOffset += BLOCK_HEADER_BYTES;
      payload.set(packed.subarray(inputOffset, inputOffset + BLOCK_PIXEL_BYTES), outputOffset);
      inputOffset += BLOCK_PIXEL_BYTES;
      outputOffset += BLOCK_PIXEL_BYTES;
    }
    if (inputOffset !== PIXEL_BYTES || outputOffset !== FRAMED_BYTES) {
      throw new TodooCardProtocolError("TodooCard 分块常量不一致");
    }
    return payload;
  }

  /** Returns a structured result and never transmits malformed data. */
  static validatePayload(input, { payloadMode = "auto" } = {}) {
    let payload;
    try {
      payload = toUint8Array(input, "TodooCard 帧");
    } catch (error) {
      return validationFailure("INVALID_TYPE", error.message);
    }
    let resolvedPayloadMode;
    try {
      resolvedPayloadMode = resolvePayloadMode(payloadMode);
    } catch (error) {
      return validationFailure("INVALID_PAYLOAD_MODE", error.message);
    }
    if (resolvedPayloadMode === "auto") {
      if (payload.length === PAYLOAD_BYTES) resolvedPayloadMode = "verified-padded";
      else if (payload.length === FRAMED_BYTES) resolvedPayloadMode = "skill-stored-short";
      else {
        return validationFailure(
          "INVALID_LENGTH",
          `帧长度必须为 ${PAYLOAD_BYTES} 或 ${FRAMED_BYTES} bytes`,
          {
            actual: payload.length,
            expected: [PAYLOAD_BYTES, FRAMED_BYTES],
            payloadMode: "auto",
          },
        );
      }
    }
    const expectedLength =
      resolvedPayloadMode === "skill-stored-short" ? FRAMED_BYTES : PAYLOAD_BYTES;
    if (payload.length !== expectedLength) {
      return validationFailure("INVALID_LENGTH", `帧长度必须为 ${expectedLength} bytes`, {
        actual: payload.length,
        expected: expectedLength,
        payloadMode: resolvedPayloadMode,
      });
    }
    for (let index = 0; index < FRAME_PREFIX_BYTES; index += 1) {
      if (payload[index] !== 0) {
        return validationFailure("INVALID_PREFIX", "4-byte 帧前缀必须全为 00", {
          offset: index,
          value: payload[index],
        });
      }
    }

    let offset = FRAME_PREFIX_BYTES;
    let pixelBytes = 0;
    for (let block = 0; block < BLOCK_COUNT; block += 1) {
      for (let headerIndex = 0; headerIndex < BLOCK_HEADER_BYTES; headerIndex += 1) {
        if (payload[offset + headerIndex] !== BLOCK_HEADER[headerIndex]) {
          return validationFailure("INVALID_BLOCK_HEADER", `第 ${block} 块缺少 74 43 40 头`, {
            block,
            offset: offset + headerIndex,
            actual: payload[offset + headerIndex],
            expected: BLOCK_HEADER[headerIndex],
          });
        }
      }
      offset += BLOCK_HEADER_BYTES;
      for (let index = 0; index < BLOCK_PIXEL_BYTES; index += 1) {
        const value = payload[offset + index];
        const high = value >>> 4;
        const low = value & 0x0f;
        if (!IMAGE_CODE_SET.has(high) || !IMAGE_CODE_SET.has(low)) {
          return validationFailure("INVALID_COLOR_CODE", "图片像素中出现了非 0/1/2/3/5/6 色码", {
            block,
            offset: offset + index,
            byte: value,
            high,
            low,
          });
        }
      }
      offset += BLOCK_PIXEL_BYTES;
      pixelBytes += BLOCK_PIXEL_BYTES;
    }
    if (offset !== FRAMED_BYTES || pixelBytes !== PIXEL_BYTES) {
      return validationFailure("INVALID_FRAME_LAYOUT", "帧分块长度不一致", { offset, pixelBytes });
    }
    if (resolvedPayloadMode === "verified-padded") {
      for (let index = FRAMED_BYTES; index < PAYLOAD_BYTES; index += 1) {
        if (payload[index] !== 0) {
          return validationFailure("INVALID_PADDING", "末尾 227-byte BLE 填充必须全为 00", {
            offset: index,
            value: payload[index],
          });
        }
      }
    }
    return {
      valid: true,
      code: null,
      error: null,
      details: {
        payloadBytes: payload.length,
        payloadMode: resolvedPayloadMode,
        pixelBytes: PIXEL_BYTES,
        blocks: BLOCK_COUNT,
        packets: Math.ceil(payload.length / DATA_BYTES_PER_PACKET),
        packetsAt240Bytes: Math.ceil(payload.length / DATA_BYTES_PER_PACKET),
      },
    };
  }

  static assertValidPayload(payload, options = {}) {
    const validation = TodooCard.validatePayload(payload, options);
    if (!validation.valid) {
      throw new TodooCardProtocolError(`拒绝发送非官方布局帧：${validation.error}`, {
        code: "INVALID_PAYLOAD",
        details: validation,
      });
    }
    return true;
  }

  /** Decodes a validated payload back to native portrait colour codes for diagnostics. */
  static decodePayloadToVisibleCodes(input) {
    const payload = toUint8Array(input, "TodooCard 帧");
    TodooCard.assertValidPayload(payload, { payloadMode: "auto" });
    const rotatedCodes = new Uint8Array(PIXEL_COUNT);
    let rotatedOffset = 0;
    let frameOffset = FRAME_PREFIX_BYTES;
    for (let block = 0; block < BLOCK_COUNT; block += 1) {
      frameOffset += BLOCK_HEADER_BYTES;
      for (let index = 0; index < BLOCK_PIXEL_BYTES; index += 1) {
        const value = payload[frameOffset + index];
        rotatedCodes[rotatedOffset] = value >>> 4;
        rotatedCodes[rotatedOffset + 1] = value & 0x0f;
        rotatedOffset += 2;
      }
      frameOffset += BLOCK_PIXEL_BYTES;
    }

    const visibleCodes = new Uint8Array(PIXEL_COUNT);
    for (let sourceY = 0; sourceY < VISIBLE_HEIGHT; sourceY += 1) {
      for (let sourceX = 0; sourceX < VISIBLE_WIDTH; sourceX += 1) {
        const rotatedX = sourceY;
        const rotatedY = VISIBLE_WIDTH - 1 - sourceX;
        visibleCodes[sourceY * VISIBLE_WIDTH + sourceX] =
          rotatedCodes[rotatedY * WIRE_WIDTH + rotatedX];
      }
    }
    return visibleCodes;
  }

  /** Adds or removes only the verified 227-byte transport padding. */
  static convertPayloadMode(input, payloadMode) {
    const payload = toUint8Array(input, "TodooCard 帧");
    TodooCard.assertValidPayload(payload, { payloadMode: "auto" });
    const targetMode = resolvePayloadMode(payloadMode);
    if (targetMode === "auto") {
      throw new TodooCardError("目标 payloadMode 不能是 auto", { code: "INVALID_PAYLOAD_MODE" });
    }
    if (targetMode === "skill-stored-short") return payload.slice(0, FRAMED_BYTES);
    const padded = new Uint8Array(PAYLOAD_BYTES);
    padded.set(payload.subarray(0, FRAMED_BYTES));
    return padded;
  }

  /** Quantizes exact-size 528×792 ImageData into a selected six-colour frame. */
  static encodeImageData(
    imageData,
    {
      dither = true,
      renderProfile = "verified",
      palette,
      distanceMetric,
      ditherScan,
      screenOrientation = "normal",
      payloadMode = "verified-padded",
    } = {},
  ) {
    if (
      !imageData ||
      imageData.width !== VISIBLE_WIDTH ||
      imageData.height !== VISIBLE_HEIGHT ||
      imageData.data?.length < PIXEL_COUNT * 4
    ) {
      throw new TodooCardError(
        `ImageData 必须精确为 ${VISIBLE_WIDTH}×${VISIBLE_HEIGHT} RGBA`,
        { code: "INVALID_IMAGE_DATA" },
      );
    }
    const resolvedRenderProfile = resolveRenderProfile(renderProfile);
    const codePalette = normalizePalette(palette ?? resolvedRenderProfile.palette);
    const resolvedDistanceMetric = distanceMetric ?? resolvedRenderProfile.distanceMetric;
    const resolvedDitherScan = ditherScan ?? resolvedRenderProfile.ditherScan;
    if (!["weighted-rgb", "rgb"].includes(resolvedDistanceMetric)) {
      throw new TodooCardError(`未知颜色距离：${String(resolvedDistanceMetric)}`, {
        code: "INVALID_RENDER_PROFILE",
      });
    }
    if (!["serpentine", "raster"].includes(resolvedDitherScan)) {
      throw new TodooCardError(`未知抖动扫描方式：${String(resolvedDitherScan)}`, {
        code: "INVALID_RENDER_PROFILE",
      });
    }
    const rgba = imageData.data;
    const visibleCodes = new Uint8Array(PIXEL_COUNT);
    let currentR = new Float32Array(VISIBLE_WIDTH + 2);
    let currentG = new Float32Array(VISIBLE_WIDTH + 2);
    let currentB = new Float32Array(VISIBLE_WIDTH + 2);
    let nextR = new Float32Array(VISIBLE_WIDTH + 2);
    let nextG = new Float32Array(VISIBLE_WIDTH + 2);
    let nextB = new Float32Array(VISIBLE_WIDTH + 2);

    const clamp = (value) => Math.max(0, Math.min(255, value));
    const diffuse = (r, g, b, slot, errorR, errorG, errorB, amount) => {
      if (slot < 0 || slot >= r.length) return;
      r[slot] += errorR * amount;
      g[slot] += errorG * amount;
      b[slot] += errorB * amount;
    };

    for (let y = 0; y < VISIBLE_HEIGHT; y += 1) {
      const reverse = Boolean(dither && resolvedDitherScan === "serpentine" && (y & 1));
      const start = reverse ? VISIBLE_WIDTH - 1 : 0;
      const end = reverse ? -1 : VISIBLE_WIDTH;
      const step = reverse ? -1 : 1;
      for (let x = start; x !== end; x += step) {
        const pixelIndex = y * VISIBLE_WIDTH + x;
        const rgbaOffset = pixelIndex * 4;
        const alpha = rgba[rgbaOffset + 3] / 255;
        const slot = x + 1;
        const r = clamp(rgba[rgbaOffset] * alpha + 255 * (1 - alpha) + currentR[slot]);
        const g = clamp(rgba[rgbaOffset + 1] * alpha + 255 * (1 - alpha) + currentG[slot]);
        const b = clamp(rgba[rgbaOffset + 2] * alpha + 255 * (1 - alpha) + currentB[slot]);

        let bestCode = 0;
        let bestDistance = Number.POSITIVE_INFINITY;
        for (const code of IMAGE_CODES) {
          const selected = codePalette[code];
          const deltaR = r - selected[0];
          const deltaG = g - selected[1];
          const deltaB = b - selected[2];
          const distance =
            resolvedDistanceMetric === "rgb"
              ? deltaR * deltaR + deltaG * deltaG + deltaB * deltaB
              : deltaR * deltaR * 0.3 + deltaG * deltaG * 0.59 + deltaB * deltaB * 0.11;
          if (distance < bestDistance) {
            bestDistance = distance;
            bestCode = code;
          }
        }
        visibleCodes[pixelIndex] = bestCode;
        if (!dither) continue;

        const selected = codePalette[bestCode];
        const errorR = r - selected[0];
        const errorG = g - selected[1];
        const errorB = b - selected[2];
        const forward = reverse ? -1 : 1;
        diffuse(currentR, currentG, currentB, slot + forward, errorR, errorG, errorB, 7 / 16);
        diffuse(nextR, nextG, nextB, slot - forward, errorR, errorG, errorB, 3 / 16);
        diffuse(nextR, nextG, nextB, slot, errorR, errorG, errorB, 5 / 16);
        diffuse(nextR, nextG, nextB, slot + forward, errorR, errorG, errorB, 1 / 16);
      }
      [currentR, nextR] = [nextR, currentR];
      [currentG, nextG] = [nextG, currentG];
      [currentB, nextB] = [nextB, currentB];
      nextR.fill(0);
      nextG.fill(0);
      nextB.fill(0);
    }
    return TodooCard.encodeVisibleCodes(visibleCodes, { screenOrientation, payloadMode });
  }

  /**
   * Accepts HTMLImageElement, ImageBitmap, canvas, video, or Blob; center-crops to 528×792.
   */
  static async encodeImageSource(
    source,
    {
      dither = true,
      renderProfile = "verified",
      palette,
      distanceMetric,
      ditherScan,
      screenOrientation = "normal",
      sourceRotation = 0,
      payloadMode = "verified-padded",
      maxInputBytes = TODOO_INPUT_LIMITS.maxBytes,
      maxSourcePixels = TODOO_INPUT_LIMITS.maxPixels,
      canvasFactory,
    } = {},
  ) {
    let drawable = source;
    let ownsDrawable = false;
    if (typeof Blob !== "undefined" && source instanceof Blob) {
      if (source.size > maxInputBytes) {
        throw new TodooCardError(`输入图片超过 ${maxInputBytes} bytes 上限`, {
          code: "INPUT_TOO_LARGE",
          details: { actualBytes: source.size, maxInputBytes },
        });
      }
      if (typeof globalThis.createImageBitmap !== "function") {
        throw new TodooCardError("当前浏览器不能把 Blob 解码为 ImageBitmap", {
          code: "IMAGE_DECODE_UNSUPPORTED",
        });
      }
      drawable = await globalThis.createImageBitmap(source);
      ownsDrawable = true;
    }

    try {
      const sourceSize = getDrawableDimensions(drawable);
      const sourcePixels = sourceSize.width * sourceSize.height;
      if (sourcePixels > maxSourcePixels) {
        throw new TodooCardError(`输入图片超过 ${maxSourcePixels} 像素上限`, {
          code: "INPUT_TOO_LARGE",
          details: { sourcePixels, maxSourcePixels },
        });
      }
      const rotation = normalizeSourceRotation(sourceRotation);
      let canvas;
      if (canvasFactory) canvas = canvasFactory(VISIBLE_WIDTH, VISIBLE_HEIGHT);
      else if (typeof OffscreenCanvas !== "undefined") {
        canvas = new OffscreenCanvas(VISIBLE_WIDTH, VISIBLE_HEIGHT);
      } else if (globalThis.document?.createElement) {
        canvas = globalThis.document.createElement("canvas");
        canvas.width = VISIBLE_WIDTH;
        canvas.height = VISIBLE_HEIGHT;
      } else {
        throw new TodooCardError("当前环境没有 Canvas；请改用 encodeImageData()", {
          code: "CANVAS_UNAVAILABLE",
        });
      }
      if (canvas.width !== VISIBLE_WIDTH) canvas.width = VISIBLE_WIDTH;
      if (canvas.height !== VISIBLE_HEIGHT) canvas.height = VISIBLE_HEIGHT;
      const context = canvas.getContext?.("2d", { willReadFrequently: true });
      if (!context) {
        throw new TodooCardError("无法创建 2D Canvas", { code: "CANVAS_UNAVAILABLE" });
      }
      context.fillStyle = "#ffffff";
      context.fillRect(0, 0, VISIBLE_WIDTH, VISIBLE_HEIGHT);
      context.imageSmoothingEnabled = true;
      context.imageSmoothingQuality = "high";

      const swapsAxes = rotation === 90 || rotation === 270;
      const orientedWidth = swapsAxes ? sourceSize.height : sourceSize.width;
      const orientedHeight = swapsAxes ? sourceSize.width : sourceSize.height;
      const scale = Math.max(VISIBLE_WIDTH / orientedWidth, VISIBLE_HEIGHT / orientedHeight);
      const drawWidth = sourceSize.width * scale;
      const drawHeight = sourceSize.height * scale;
      context.save();
      context.translate(VISIBLE_WIDTH / 2, VISIBLE_HEIGHT / 2);
      context.rotate((rotation * Math.PI) / 180);
      context.drawImage(drawable, -drawWidth / 2, -drawHeight / 2, drawWidth, drawHeight);
      context.restore();
      return TodooCard.encodeImageData(context.getImageData(0, 0, VISIBLE_WIDTH, VISIBLE_HEIGHT), {
        dither,
        renderProfile,
        palette,
        distanceMetric,
        ditherScan,
        screenOrientation,
        payloadMode,
      });
    } finally {
      if (ownsDrawable) drawable.close?.();
    }
  }

  static _fillVisibleRect(pixels, left, top, width, height, code) {
    const right = Math.min(VISIBLE_WIDTH, Math.max(0, left + width));
    const bottom = Math.min(VISIBLE_HEIGHT, Math.max(0, top + height));
    const clippedLeft = Math.max(0, left);
    const clippedTop = Math.max(0, top);
    for (let y = clippedTop; y < bottom; y += 1) {
      pixels.fill(code, y * VISIBLE_WIDTH + clippedLeft, y * VISIBLE_WIDTH + right);
    }
  }

  async _exchangeControl(command, opcode, signal, label) {
    const waiter = this._createNotificationWaiter({
      opcode,
      timeoutMs: this._timing.controlTimeoutMs,
      signal,
      label,
    });
    try {
      await this._writeControl(Uint8Array.from(command), signal);
    } catch (error) {
      waiter.cancel(error);
      await waiter.promise.catch(() => {});
      throw error;
    }
    return waiter.promise;
  }

  _assertNotification(actual, expectedInput, label) {
    const expected = Uint8Array.from(expectedInput);
    if (!bytesEqual(actual, expected)) {
      throw new TodooCardProtocolError(
        `${label}通知不符合已验证协议：收到 ${hex(actual)}，期望 ${hex(expected)}`,
        { details: { label, actual: Array.from(actual), expected: Array.from(expected) } },
      );
    }
  }

  async _writeControl(value, signal) {
    if (!this.isConnected) throw new TodooCardConnectionError("控制通道未连接");
    const channel = `${this._gattProfile?.id?.toUpperCase() ?? "GATT"}1`;
    this._log("debug", `WRITE ${channel}`, { hex: hex(value), bytes: value.length });
    const properties = this._control.properties;
    let method = null;
    if (
      properties?.write === false &&
      properties?.writeWithoutResponse &&
      typeof this._control.writeValueWithoutResponse === "function"
    ) {
      method = "writeValueWithoutResponse";
    } else if (typeof this._control.writeValueWithResponse === "function") {
      method = "writeValueWithResponse";
    } else if (typeof this._control.writeValueWithoutResponse === "function") {
      method = "writeValueWithoutResponse";
    } else if (typeof this._control.writeValue === "function") {
      method = "writeValue";
    }
    if (!method) {
      throw new TodooCardConnectionError("浏览器没有可用的控制特征写入 API");
    }
    await this._runWithTimeout(
      () => this._control[method](value),
      this._timing.characteristicWriteTimeoutMs,
      signal,
      `${channel} 控制写入超时`,
    );
  }

  async _writeData(value, signal) {
    const method =
      typeof this._data?.writeValueWithoutResponse === "function"
        ? "writeValueWithoutResponse"
        : typeof this._data?.writeValue === "function"
          ? "writeValue"
          : null;
    if (!method) {
      throw new TodooCardConnectionError("浏览器没有数据特征 Write Without Response API");
    }
    try {
      await this._runWithTimeout(
        () => this._data[method](value),
        this._timing.characteristicWriteTimeoutMs,
        signal,
        `${this._gattProfile?.id?.toUpperCase() ?? "GATT"}2 数据写入超时`,
      );
    } catch (error) {
      if (error?.name === "DataError" || error?.name === "InvalidModificationError") {
        throw new TodooCardProtocolError(
          `浏览器拒绝 ${value.length}-byte 数据包，当前连接的 ATT MTU 可能不足`,
          {
            code: "UNSUPPORTED_PACKET_SIZE",
            cause: error,
            hint: "使用支持 Web Bluetooth 的新版 Android/桌面 Chromium，并重新连接设备。",
          },
        );
      }
      throw error;
    }
  }

  _createNotificationWaiter({ opcode, timeoutMs, signal, label, predicate = () => true }) {
    let resolvePromise;
    let rejectPromise;
    let settled = false;
    let timer = null;
    const promise = new Promise((resolve, reject) => {
      resolvePromise = resolve;
      rejectPromise = reject;
    });
    const waiter = {
      opcode,
      predicate,
      promise,
      resolve: (bytes) => settle(resolvePromise, bytes),
      reject: (error) => settle(rejectPromise, error),
      cancel: (error = new TodooCardAbortError(`${label}等待已取消`)) => settle(rejectPromise, error),
    };
    const cleanup = () => {
      if (timer !== null) clearTimeout(timer);
      signal?.removeEventListener("abort", onAbort);
      this._notificationWaiters.delete(waiter);
    };
    const settle = (callback, value) => {
      if (settled) return;
      settled = true;
      cleanup();
      callback(value);
    };
    const onAbort = () =>
      waiter.reject(new TodooCardAbortError(`${label}等待已取消`, { cause: signal.reason }));
    if (signal?.aborted) onAbort();
    else signal?.addEventListener("abort", onAbort, { once: true });
    if (!settled) {
      this._notificationWaiters.add(waiter);
      if (timeoutMs > 0) {
        timer = setTimeout(
          () =>
            waiter.reject(
              new TodooCardProtocolError(`等待${label}通知超时`, {
                code: "NOTIFICATION_TIMEOUT",
                hint: "断开后重连；若设备不广播，可手动短贴 NFC 区域唤醒。",
              }),
            ),
          timeoutMs,
        );
      }
    }
    return waiter;
  }

  _handleNotification(event) {
    try {
      const value = event?.target?.value ?? event?.value ?? this._control?.value;
      const channel = `${this._gattProfile?.id?.toUpperCase() ?? "GATT"}1`;
      const bytes = Uint8Array.from(toUint8Array(value, `${channel} notification`));
      this._log("debug", `NOTIFY ${channel}`, { hex: hex(bytes), state: this._state });
      this._emit("notification", { characteristic: channel, bytes, hex: hex(bytes) });
      if (bytes.length === 0) return;
      for (const waiter of [...this._notificationWaiters]) {
        if (waiter.opcode !== bytes[0]) continue;
        let matches = false;
        try {
          matches = waiter.predicate(bytes);
        } catch (error) {
          waiter.reject(error);
          continue;
        }
        if (matches) waiter.resolve(bytes);
      }
    } catch (error) {
      this._log("warn", "无法解析控制 notification", { error: serializeError(error) });
    }
  }

  _handleDisconnected() {
    const wasConnected = Boolean(this._server || this._control || this._data);
    this._control?.removeEventListener?.("characteristicvaluechanged", this._boundNotification);
    this._server = null;
    this._service = null;
    this._gattProfile = null;
    this._control = null;
    this._data = null;
    this._notificationReadyAt = 0;
    const error = new TodooCardConnectionError("TodooCard GATT 已断开", {
      hint: "若设备已休眠，可手动短贴手机 NFC 区域约 2 秒后重连。",
    });
    this._rejectNotificationWaiters(error);
    if (!["complete", "error", "cancelled", "disconnected"].includes(this._state)) {
      this._setState("disconnected");
    }
    if (wasConnected) this._emit("disconnected", { device: this.status.device });
  }

  _releaseGatt({ preserveState = false, reason } = {}) {
    this._rejectNotificationWaiters(
      reason ?? new TodooCardConnectionError("TodooCard 连接已释放", { code: "DISCONNECTED" }),
    );
    this._control?.removeEventListener?.("characteristicvaluechanged", this._boundNotification);
    try {
      if (this._device?.gatt?.connected) this._device.gatt.disconnect();
    } catch (error) {
      this._log("warn", "释放 GATT 时发生异常", { error: serializeError(error) });
    }
    this._server = null;
    this._service = null;
    this._gattProfile = null;
    this._control = null;
    this._data = null;
    this._notificationReadyAt = 0;
    if (!preserveState) this._setState("disconnected");
  }

  _rejectNotificationWaiters(error) {
    for (const waiter of [...this._notificationWaiters]) waiter.reject(error);
  }

  async _sleep(milliseconds, signal) {
    throwIfAborted(signal);
    await this._sleepImpl(milliseconds, signal);
    throwIfAborted(signal);
  }

  _runWithTimeout(operation, timeoutMs, signal, message) {
    throwIfAborted(signal);
    return new Promise((resolve, reject) => {
      let settled = false;
      let timer = null;
      const cleanup = () => {
        if (timer !== null) clearTimeout(timer);
        signal?.removeEventListener("abort", onAbort);
      };
      const settle = (callback, value) => {
        if (settled) return;
        settled = true;
        cleanup();
        callback(value);
      };
      const onAbort = () =>
        settle(reject, new TodooCardAbortError("TodooCard 操作已取消", { cause: signal.reason }));
      signal?.addEventListener("abort", onAbort, { once: true });
      if (timeoutMs > 0) {
        timer = setTimeout(
          () =>
            settle(
              reject,
              new TodooCardConnectionError(message, {
                code: "TIMEOUT",
                hint: "检查卡片电量和距离；设备休眠时可手动短贴 NFC 区域后重试。",
              }),
            ),
          timeoutMs,
        );
      }
      Promise.resolve()
        .then(operation)
        .then((value) => settle(resolve, value), (error) => settle(reject, error));
    });
  }

  _assertBrowserAccess() {
    if (!this.isSupported()) {
      throw new TodooCardError("当前浏览器不支持 Web Bluetooth", {
        code: "UNSUPPORTED_BROWSER",
        hint: "请使用支持 Web Bluetooth 的 Android 或桌面 Chromium 浏览器。",
      });
    }
    if (globalThis.isSecureContext === false) {
      throw new TodooCardError("Web Bluetooth 只能在 HTTPS 安全上下文中使用", {
        code: "INSECURE_CONTEXT",
        hint: "部署为 HTTPS；本地开发可使用 localhost。",
      });
    }
  }

  _mapConnectionError(error) {
    if (error instanceof TodooCardError) return error;
    if (error?.name === "AbortError") return new TodooCardAbortError(undefined, { cause: error });
    if (error?.name === "NotFoundError") {
      return new TodooCardConnectionError("设备缺少 FEF0/1/2 或 FDF0/1/2 服务", {
        code: "GATT_SERVICE_MISSING",
        cause: error,
      });
    }
    return new TodooCardConnectionError(`连接 TodooCard 失败：${error?.message ?? String(error)}`, {
      cause: error,
      hint: "让卡片保持有电并靠近手机；若不广播，可手动短贴 NFC 区域约 2 秒后重试。",
    });
  }

  _mapWriteError(error) {
    if (error instanceof TodooCardError) return error;
    if (error?.name === "AbortError") return new TodooCardAbortError(undefined, { cause: error });
    return new TodooCardProtocolError(`TodooCard 写入失败：${error?.message ?? String(error)}`, {
      cause: error,
    });
  }

  _emitProgress(
    sentPackets,
    totalPackets = TOTAL_PACKETS,
    dataBytesPerPacket = DATA_BYTES_PER_PACKET,
    totalBytes = PAYLOAD_BYTES,
  ) {
    const bytesSent = Math.min(totalBytes, sentPackets * dataBytesPerPacket);
    this._emit("progress", {
      phase: "transfer",
      sentPackets,
      totalPackets,
      bytesSent,
      totalBytes,
      percent: totalBytes === 0 ? 100 : (bytesSent * 100) / totalBytes,
    });
  }

  _setState(state, detail = {}) {
    const previous = this._state;
    this._state = state;
    this._emit("state", { state, previous, ...detail });
  }

  _emit(type, detail = {}) {
    const handlers = this._listeners.get(type);
    if (!handlers?.size) return;
    const event = Object.freeze({ type, detail, target: this, timeStamp: Date.now() });
    for (const handler of [...handlers]) {
      try {
        handler(event);
      } catch (error) {
        this._log("warn", `事件处理器 ${type} 抛出异常`, { error: serializeError(error) }, false);
      }
    }
  }

  _log(level, message, data, emit = true) {
    const entry = Object.freeze({ level, message, data, timeStamp: Date.now() });
    try {
      if (typeof this._logger === "function") this._logger(entry);
      else this._logger?.[level]?.(message, data);
    } catch {
      // A business logger must never interrupt the device protocol.
    }
    if (emit) this._emit("log", entry);
  }
}

export default TodooCard;
