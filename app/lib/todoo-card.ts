const UUID_SUFFIX = "-0000-1000-8000-00805f9b34fb";
const uuid16 = (value: number) => `0000${value.toString(16).padStart(4, "0")}${UUID_SUFFIX}`;

export const TODOO_PROTOCOL = {
  service: uuid16(0xfef0),
  control: uuid16(0xfef1),
  data: uuid16(0xfef2),
  width: 528,
  height: 792,
  payloadBytes: 219120,
  packetBytes: 244,
  dataBytesPerPacket: 240,
  packetCount: 913,
} as const;

const TODOO_DEVICE_NAMES = ["NEMR99803797", "PICKSMART"] as const;

function isTodooDeviceName(name?: string) {
  return Boolean(
    name &&
      (name.startsWith("NEMR") ||
        TODOO_DEVICE_NAMES.includes(name as (typeof TODOO_DEVICE_NAMES)[number])),
  );
}

type ValueEvent = Event & { target: EventTarget & { value?: DataView } };

type CharacteristicLike = EventTarget & {
  value?: DataView;
  startNotifications(): Promise<unknown>;
  writeValue?(value: Uint8Array<ArrayBufferLike>): Promise<unknown>;
  writeValueWithResponse?(value: Uint8Array<ArrayBufferLike>): Promise<unknown>;
  writeValueWithoutResponse?(value: Uint8Array<ArrayBufferLike>): Promise<unknown>;
};

type ServiceLike = {
  getCharacteristic(uuid: string): Promise<CharacteristicLike>;
};

type ServerLike = {
  getPrimaryService(uuid: string): Promise<ServiceLike>;
};

export type BluetoothDeviceLike = EventTarget & {
  id: string;
  name?: string;
  gatt?: {
    connected: boolean;
    connect(): Promise<ServerLike>;
    disconnect(): void;
  };
};

type BluetoothLike = {
  requestDevice(options: {
    filters: Array<{ namePrefix?: string; name?: string; services?: string[] }>;
    optionalServices: string[];
  }): Promise<BluetoothDeviceLike>;
  getDevices?(): Promise<BluetoothDeviceLike[]>;
};

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

export type TodooProgress = {
  phase: "connecting" | "encoding" | "sending" | "refreshing" | "complete";
  percent: number;
  message: string;
};

const palette = [
  [0, 0, 0],
  [255, 255, 255],
  [255, 238, 0],
  [226, 52, 38],
  [0, 92, 210],
  [0, 150, 78],
] as const;

const paletteCodes = [0, 1, 2, 3, 5, 6] as const;

function nearestCode(r: number, g: number, b: number) {
  let bestIndex = 0;
  let bestDistance = Number.POSITIVE_INFINITY;
  for (let index = 0; index < palette.length; index += 1) {
    const color = palette[index];
    const dr = r - color[0];
    const dg = g - color[1];
    const db = b - color[2];
    const distance = dr * dr * 0.3 + dg * dg * 0.59 + db * db * 0.11;
    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = index;
    }
  }
  return paletteCodes[bestIndex];
}

export class TodooCard {
  private bluetooth: BluetoothLike | undefined;
  private device: BluetoothDeviceLike | null = null;
  private control: CharacteristicLike | null = null;
  private data: CharacteristicLike | null = null;
  private notificationWaiters = new Map<number, Array<(bytes: Uint8Array) => void>>();
  private onProgress?: (progress: TodooProgress) => void;

  constructor(onProgress?: (progress: TodooProgress) => void) {
    this.bluetooth = (navigator as Navigator & { bluetooth?: BluetoothLike }).bluetooth;
    this.onProgress = onProgress;
  }

  get supported() {
    return Boolean(this.bluetooth?.requestDevice && globalThis.isSecureContext);
  }

  get selectedDevice() {
    return this.device;
  }

  async restoreAuthorizedDevice() {
    if (!this.bluetooth?.getDevices) return null;
    const devices = await this.bluetooth.getDevices();
    const remembered = devices.find((device) => isTodooDeviceName(device.name));
    if (remembered) this.useDevice(remembered);
    return remembered ?? null;
  }

  async requestDevice() {
    if (!this.bluetooth) throw new Error("当前浏览器不支持 Web Bluetooth");
    const device = await this.bluetooth.requestDevice({
      filters: [
        { name: "NEMR99803797" },
        { namePrefix: "NEMR", services: [TODOO_PROTOCOL.service] },
        { name: "PICKSMART" },
      ],
      optionalServices: [TODOO_PROTOCOL.service],
    });
    this.useDevice(device);
    return device;
  }

  private useDevice(device: BluetoothDeviceLike) {
    this.device = device;
    device.addEventListener("gattserverdisconnected", () => {
      this.control = null;
      this.data = null;
    });
  }

  async connect() {
    if (!this.device?.gatt) throw new Error("请先选择 TodooCard 设备");
    if (this.device.gatt.connected && this.control && this.data) return;
    this.emit("connecting", 4, `正在连接 ${this.device.name ?? "TodooCard"}…`);
    const server = await this.device.gatt.connect();
    const service = await server.getPrimaryService(TODOO_PROTOCOL.service);
    this.control = await service.getCharacteristic(TODOO_PROTOCOL.control);
    this.data = await service.getCharacteristic(TODOO_PROTOCOL.data);
    this.control.addEventListener("characteristicvaluechanged", (event) => {
      const value = (event as ValueEvent).target.value ?? this.control?.value;
      if (!value) return;
      const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      const waiters = this.notificationWaiters.get(bytes[0]) ?? [];
      this.notificationWaiters.delete(bytes[0]);
      waiters.forEach((resolve) => resolve(Uint8Array.from(bytes)));
    });
    await this.control.startNotifications();
    await sleep(750);
  }

  disconnect() {
    this.device?.gatt?.disconnect();
    this.control = null;
    this.data = null;
  }

  static encodeImageData(imageData: ImageData) {
    if (imageData.width !== TODOO_PROTOCOL.width || imageData.height !== TODOO_PROTOCOL.height) {
      throw new Error(`Canvas 必须为 ${TODOO_PROTOCOL.width}×${TODOO_PROTOCOL.height}`);
    }
    const pixelCount = TODOO_PROTOCOL.width * TODOO_PROTOCOL.height;
    const visible = new Uint8Array(pixelCount);
    for (let index = 0; index < pixelCount; index += 1) {
      const offset = index * 4;
      visible[index] = nearestCode(
        imageData.data[offset],
        imageData.data[offset + 1],
        imageData.data[offset + 2],
      );
    }

    const rotated = new Uint8Array(pixelCount);
    for (let y = 0; y < TODOO_PROTOCOL.height; y += 1) {
      for (let x = 0; x < TODOO_PROTOCOL.width; x += 1) {
        const rotatedX = y;
        const rotatedY = TODOO_PROTOCOL.width - 1 - x;
        rotated[rotatedY * TODOO_PROTOCOL.height + rotatedX] =
          visible[y * TODOO_PROTOCOL.width + x];
      }
    }

    const packed = new Uint8Array(pixelCount / 2);
    for (let index = 0; index < packed.length; index += 1) {
      packed[index] = (rotated[index * 2] << 4) | rotated[index * 2 + 1];
    }
    const payload = new Uint8Array(TODOO_PROTOCOL.payloadBytes);
    let inputOffset = 0;
    let outputOffset = 4;
    for (let block = 0; block < 3267; block += 1) {
      payload.set([0x74, 0x43, 0x40], outputOffset);
      outputOffset += 3;
      payload.set(packed.subarray(inputOffset, inputOffset + 64), outputOffset);
      inputOffset += 64;
      outputOffset += 64;
    }
    return payload;
  }

  async writeCanvas(canvas: HTMLCanvasElement, disconnectAfterWrite = true) {
    this.emit("encoding", 8, "正在转换为六色电子纸帧…");
    const context = canvas.getContext("2d", { willReadFrequently: true });
    if (!context) throw new Error("无法读取预览画布");
    const payload = TodooCard.encodeImageData(
      context.getImageData(0, 0, TODOO_PROTOCOL.width, TODOO_PROTOCOL.height),
    );
    await this.connect();
    await this.exchange([0x01], 0x01);
    await sleep(800);
    await this.exchange([0x02, 0xf0, 0x57, 0x03, 0x00, 0x01], 0x02);
    await sleep(400);
    await this.exchange([0x03], 0x05);
    await sleep(30);

    const completion = this.waitFor(0x05, 45000, (bytes) => bytes[1] === 0x08);
    for (let packetIndex = 0; packetIndex < TODOO_PROTOCOL.packetCount; packetIndex += 1) {
      const packet = new Uint8Array(TODOO_PROTOCOL.packetBytes);
      packet[0] = packetIndex & 0xff;
      packet[1] = (packetIndex >>> 8) & 0xff;
      packet[2] = (packetIndex >>> 16) & 0xff;
      packet[3] = (packetIndex >>> 24) & 0xff;
      const offset = packetIndex * TODOO_PROTOCOL.dataBytesPerPacket;
      packet.set(payload.subarray(offset, offset + TODOO_PROTOCOL.dataBytesPerPacket), 4);
      await this.writeData(packet);
      if (packetIndex % 12 === 0 || packetIndex === TODOO_PROTOCOL.packetCount - 1) {
        const percent = 12 + Math.round(((packetIndex + 1) / TODOO_PROTOCOL.packetCount) * 78);
        this.emit("sending", percent, `正在发送 ${packetIndex + 1} / ${TODOO_PROTOCOL.packetCount} 包`);
      }
      if (packetIndex + 1 < TODOO_PROTOCOL.packetCount) await sleep(28);
    }
    await completion;
    this.emit("refreshing", 96, "设备已收帧，电子纸正在显色…");
    if (disconnectAfterWrite) this.disconnect();
    this.emit("complete", 100, "写入完成");
  }

  private async exchange(command: number[], opcode: number) {
    const notification = this.waitFor(opcode, 12000);
    await this.writeControl(Uint8Array.from(command));
    return notification;
  }

  private waitFor(opcode: number, timeoutMs: number, predicate = (_bytes: Uint8Array) => true) {
    return new Promise<Uint8Array>((resolve, reject) => {
      const resolver = (bytes: Uint8Array) => {
        if (predicate(bytes)) resolve(bytes);
        else reject(new Error(`设备返回了未识别的 0x${opcode.toString(16)} 通知`));
      };
      const waiters = this.notificationWaiters.get(opcode) ?? [];
      waiters.push(resolver);
      this.notificationWaiters.set(opcode, waiters);
      setTimeout(() => {
        const active = this.notificationWaiters.get(opcode) ?? [];
        const index = active.indexOf(resolver);
        if (index >= 0) active.splice(index, 1);
        reject(new Error("等待设备响应超时，请唤醒设备后重试"));
      }, timeoutMs);
    });
  }

  private async writeControl(value: Uint8Array) {
    if (!this.control) throw new Error("控制通道未连接");
    if (this.control.writeValueWithResponse) await this.control.writeValueWithResponse(value);
    else if (this.control.writeValue) await this.control.writeValue(value);
    else throw new Error("浏览器不支持蓝牙控制写入");
  }

  private async writeData(value: Uint8Array) {
    if (!this.data) throw new Error("数据通道未连接");
    if (this.data.writeValueWithoutResponse) await this.data.writeValueWithoutResponse(value);
    else if (this.data.writeValue) await this.data.writeValue(value);
    else throw new Error("浏览器不支持蓝牙数据写入");
  }

  private emit(phase: TodooProgress["phase"], percent: number, message: string) {
    this.onProgress?.({ phase, percent, message });
  }
}
