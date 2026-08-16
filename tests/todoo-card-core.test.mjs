import test from "node:test";
import assert from "node:assert/strict";

import TodooCard, {
  TODOO_COLOR_CODES,
  TODOO_DOCUMENTATION,
  TODOO_GATT_PROFILES,
  TODOO_INPUT_LIMITS,
  TODOO_PRODUCT_INFO,
  TODOO_PROTOCOL,
  TODOO_RENDER_PROFILES,
  TODOO_SECURE_PAIRING,
  TODOO_SKILL_INTEGRATION,
  TODOO_TRANSFER_PROFILES,
  parseTodooAdvertisementData,
} from "../app/lib/todoo-card-core.js";

const WIDTH = TODOO_PROTOCOL.image.visibleWidth;
const HEIGHT = TODOO_PROTOCOL.image.visibleHeight;
const PIXELS = WIDTH * HEIGHT;

function payloadCodeAtRotatedIndex(payload, rotatedIndex) {
  const pixelByteIndex = Math.floor(rotatedIndex / 2);
  const block = Math.floor(pixelByteIndex / TODOO_PROTOCOL.frame.pixelBytesPerBlock);
  const withinBlock = pixelByteIndex % TODOO_PROTOCOL.frame.pixelBytesPerBlock;
  const frameOffset =
    TODOO_PROTOCOL.frame.prefix.length +
    block * (TODOO_PROTOCOL.frame.blockHeader.length + TODOO_PROTOCOL.frame.pixelBytesPerBlock) +
    TODOO_PROTOCOL.frame.blockHeader.length +
    withinBlock;
  const value = payload[frameOffset];
  return rotatedIndex % 2 === 0 ? value >>> 4 : value & 0x0f;
}

class FakeEventTarget {
  constructor() {
    this.listeners = new Map();
  }

  addEventListener(type, handler) {
    let handlers = this.listeners.get(type);
    if (!handlers) {
      handlers = new Set();
      this.listeners.set(type, handlers);
    }
    handlers.add(handler);
  }

  removeEventListener(type, handler) {
    this.listeners.get(type)?.delete(handler);
  }

  emit(type, event = {}) {
    for (const handler of [...(this.listeners.get(type) ?? [])]) {
      handler({ type, target: this, ...event });
    }
  }
}

class FakeCharacteristic extends FakeEventTarget {
  constructor(
    uuid,
    role,
    { expectedPacketCount = TODOO_PROTOCOL.transfer.packetCount, readValue = 73 } = {},
  ) {
    super();
    this.uuid = uuid;
    this.role = role;
    this.writes = [];
    this.control = null;
    this.expectedPacketCount = expectedPacketCount;
    this.initResponse = Uint8Array.from([0x01, 0xf4, 0x00]);
    this.readResult = readValue;
    this.properties =
      role === "control"
        ? { write: true, writeWithoutResponse: true, notify: true, indicate: false, read: false }
        : role === "battery"
          ? { write: false, writeWithoutResponse: false, notify: false, indicate: false, read: true }
          : { write: false, writeWithoutResponse: true, notify: false, indicate: false, read: false };
  }

  async readValue() {
    assert.equal(this.role, "battery");
    if (this.readResult instanceof Error) throw this.readResult;
    const bytes = Uint8Array.from([this.readResult]);
    return new DataView(bytes.buffer);
  }

  async startNotifications() {
    this.notificationsStarted = true;
    return this;
  }

  async writeValueWithResponse(value) {
    assert.equal(this.role, "control");
    const bytes = Uint8Array.from(value);
    this.writes.push(bytes);
    let response;
    if (bytes[0] === 0x01) response = this.initResponse;
    else if (bytes[0] === 0x02) response = Uint8Array.from([0x02, 0x00, 0x57]);
    else if (bytes[0] === 0x03) response = Uint8Array.from([0x05, 0, 0, 0, 0, 0]);
    if (response) queueMicrotask(() => this.notify(response));
  }

  async writeValueWithoutResponse(value) {
    assert.equal(this.role, "data");
    this.writes.push(Uint8Array.from(value));
    if (this.writes.length === this.expectedPacketCount) {
      queueMicrotask(() => this.control.notify(Uint8Array.from(TODOO_PROTOCOL.completionNotification)));
    }
  }

  notify(bytes) {
    const copy = Uint8Array.from(bytes);
    this.value = new DataView(copy.buffer, copy.byteOffset, copy.byteLength);
    this.emit("characteristicvaluechanged");
  }
}

function createFakeStack({
  gattProfile = TODOO_GATT_PROFILES.fef,
  expectedPacketCount = TODOO_PROTOCOL.transfer.packetCount,
  batteryValue = 73,
  batteryServiceAvailable = true,
} = {}) {
  const control = new FakeCharacteristic(gattProfile.control, "control");
  const data = new FakeCharacteristic(gattProfile.data, "data", { expectedPacketCount });
  data.control = control;
  const service = {
    async getCharacteristic(uuid) {
      if (uuid === gattProfile.control) return control;
      if (uuid === gattProfile.data) return data;
      throw new DOMException("missing", "NotFoundError");
    },
  };
  const battery = new FakeCharacteristic(TODOO_SECURE_PAIRING.battery.level, "battery", {
    readValue: batteryValue,
  });
  const batteryService = {
    async getCharacteristic(uuid) {
      if (uuid === TODOO_SECURE_PAIRING.battery.level) return battery;
      throw new DOMException("missing", "NotFoundError");
    },
  };
  const device = new FakeEventTarget();
  device.id = "opaque-browser-device-id";
  device.name = TODOO_PRODUCT_INFO.advertisedName;
  const server = {
    async getPrimaryService(uuid) {
      if (uuid === gattProfile.service) return service;
      if (uuid === TODOO_SECURE_PAIRING.battery.service && batteryServiceAvailable) {
        return batteryService;
      }
      throw new DOMException("missing", "NotFoundError");
    },
  };
  device.gatt = {
    connected: false,
    async connect() {
      this.connected = true;
      return server;
    },
    disconnect() {
      if (!this.connected) return;
      this.connected = false;
      device.emit("gattserverdisconnected");
    },
  };
  const bluetooth = {
    requestOptions: null,
    async getAvailability() {
      return true;
    },
    async getDevices() {
      return [device];
    },
    async requestDevice(options) {
      this.requestOptions = options;
      return device;
    },
  };
  return { bluetooth, device, control, data, battery };
}

function zeroDelayTiming() {
  return {
    notificationSettleMs: 0,
    afterInitMs: 0,
    afterLengthMs: 0,
    beforeDataMs: 0,
    packetIntervalMs: 0,
    disconnectDelayMs: 0,
  };
}

test("产品说明和协议常量与真机结果一致且只读", () => {
  assert.equal(TODOO_PRODUCT_INFO.screen.width, 528);
  assert.equal(TODOO_PRODUCT_INFO.screen.height, 792);
  assert.equal(TODOO_PRODUCT_INFO.screen.estimatedDpi, 259);
  assert.deepEqual(TODOO_COLOR_CODES, { black: 0, white: 1, yellow: 2, red: 3, blue: 5, green: 6 });
  assert.equal(TODOO_PROTOCOL.frame.payloadBytes, 219120);
  assert.equal(TODOO_PROTOCOL.transfer.packetCount, 913);
  assert.equal(TODOO_PROTOCOL.uuids.service, "0000fef0-0000-1000-8000-00805f9b34fb");
  assert.equal(TODOO_GATT_PROFILES.fdf.service, "0000fdf0-0000-1000-8000-00805f9b34fb");
  assert.equal(TODOO_TRANSFER_PROFILES.skillT3.payloadBytes, 218893);
  assert.deepEqual(TODOO_RENDER_PROFILES.skillT3.palette[6], [0, 255, 0]);
  assert.equal(TODOO_INPUT_LIMITS.maxPixels, 50000000);
  assert.equal(
    TODOO_SKILL_INTEGRATION.reviewedCommit,
    "990f21caeaa74e2488ab72f9e343c04b1586689e",
  );
  assert.equal(TODOO_SECURE_PAIRING.manufacturerId, 0x5053);
  assert.equal(TODOO_SECURE_PAIRING.screenType, 0x134c);
  assert.equal(TODOO_SECURE_PAIRING.latestReviewedFirmware, 0x95);
  assert.match(TODOO_DOCUMENTATION.summary, /913/);
  assert.equal(Object.isFrozen(TODOO_PRODUCT_INFO.screen), true);
  assert.equal(Object.isFrozen(TODOO_PROTOCOL.handshake), true);
  assert.equal(Object.isFrozen(TODOO_RENDER_PROFILES.skillT3.palette), true);
});

test("解析 v0x95 安全广播和实体配对窗口", () => {
  const raw = Uint8Array.from([0x53, 0x50, 0x4c, 0x03, 0x95, 0x00, 0x13]);
  const parsed = parseTodooAdvertisementData(raw);
  assert.deepEqual(parsed, {
    manufacturerId: 0x5053,
    screenType: 0x134c,
    capabilityFlags: 0x03,
    firmwareVersion: 0x95,
    secure: true,
    pairingWindowOpen: true,
    otaRecoveryMode: false,
  });
  assert.deepEqual(
    parseTodooAdvertisementData(raw.subarray(2), { includesManufacturerId: false }),
    parsed,
  );
});

test("安全配对以加密 Battery Level 读取成功为准，并保持旧写屏路径独立", async () => {
  const fake = createFakeStack({ batteryValue: 81 });
  const card = new TodooCard({ bluetooth: fake.bluetooth, timing: zeroDelayTiming() });
  card.useDevice(fake.device);
  const result = await card.pairSecureDevice();
  assert.deepEqual(result, {
    verified: true,
    batteryPercent: 81,
    verification: "encrypted-battery-level",
  });
  assert.equal(card.state, "paired");
  assert.equal(fake.device.gatt.connected, false);
  assert.equal(fake.control.writes.length, 0);
});

test("纯色帧具有精确长度、块头、填充和可逆像素", () => {
  const payload = TodooCard.createSolidPayload("green");
  assert.equal(payload.length, 219120);
  assert.deepEqual(Array.from(payload.subarray(0, 7)), [0, 0, 0, 0, 0x74, 0x43, 0x40]);
  assert.equal(payload.at(-1), 0);
  assert.equal(TodooCard.validatePayload(payload).valid, true);
  const decoded = TodooCard.decodePayloadToVisibleCodes(payload);
  assert.equal(decoded.length, PIXELS);
  assert.equal(decoded.every((code) => code === TODOO_COLOR_CODES.green), true);
});

test("skill-t3 短帧可显式生成、校验，并与验证帧互转", () => {
  const shortPayload = TodooCard.createSolidPayload("blue", { payloadMode: "skill-t3" });
  assert.equal(shortPayload.length, 218893);
  const shortValidation = TodooCard.validatePayload(shortPayload);
  assert.equal(shortValidation.valid, true);
  assert.equal(shortValidation.details.payloadMode, "skill-stored-short");

  const padded = TodooCard.convertPayloadMode(shortPayload, "verified");
  assert.equal(padded.length, 219120);
  assert.deepEqual(padded.subarray(0, shortPayload.length), shortPayload);
  assert.equal(padded.subarray(shortPayload.length).every((value) => value === 0), true);
  assert.equal(TodooCard.validatePayload(padded).details.payloadMode, "verified-padded");
  assert.deepEqual(TodooCard.convertPayloadMode(padded, "skill-t3"), shortPayload);
  assert.throws(
    () => TodooCard.createSolidPayload("blue", {
      payloadMode: "skill-t3",
      screenOrientation: "rotate-180",
    }),
    { code: "INVALID_ORIENTATION" },
  );
});

test("屏幕倒装方向别名在可见像素空间中执行，默认控制器旋转不变", () => {
  const visible = new Uint8Array(PIXELS);
  visible.fill(TODOO_COLOR_CODES.white);
  const sourceX = 12;
  const sourceY = 34;
  visible[sourceY * WIDTH + sourceX] = TODOO_COLOR_CODES.red;
  const transformed = TodooCard.transformVisibleCodes(
    visible,
    "rotate-180-then-flip-horizontal",
  );
  assert.equal(
    transformed[(HEIGHT - 1 - sourceY) * WIDTH + sourceX],
    TODOO_COLOR_CODES.red,
  );
  assert.equal(transformed[sourceY * WIDTH + sourceX], TODOO_COLOR_CODES.white);

  const payload = TodooCard.encodeVisibleCodes(visible, {
    screenOrientation: "rotate-180-then-flip-horizontal",
  });
  assert.deepEqual(TodooCard.decodePayloadToVisibleCodes(payload), transformed);
});

test("528×792 图像按逆时针 90°映射并以高半字节优先打包", () => {
  const visible = new Uint8Array(PIXELS);
  visible.fill(TODOO_COLOR_CODES.white);
  visible[0] = TODOO_COLOR_CODES.red;
  visible[WIDTH - 1] = TODOO_COLOR_CODES.green;
  visible[(HEIGHT - 1) * WIDTH] = TODOO_COLOR_CODES.blue;
  visible[PIXELS - 1] = TODOO_COLOR_CODES.yellow;

  const payload = TodooCard.encodeVisibleCodes(visible);
  assert.equal(payloadCodeAtRotatedIndex(payload, 527 * 792), TODOO_COLOR_CODES.red);
  assert.equal(payloadCodeAtRotatedIndex(payload, 0), TODOO_COLOR_CODES.green);
  assert.equal(payloadCodeAtRotatedIndex(payload, 527 * 792 + 791), TODOO_COLOR_CODES.blue);
  assert.equal(payloadCodeAtRotatedIndex(payload, 791), TODOO_COLOR_CODES.yellow);
  assert.deepEqual(TodooCard.decodePayloadToVisibleCodes(payload), visible);
});

test("校验器拒绝错误长度、块头、色码和尾部填充", () => {
  const valid = TodooCard.createSolidPayload("white");
  assert.equal(TodooCard.validatePayload(valid.subarray(1)).code, "INVALID_LENGTH");

  const badHeader = valid.slice();
  badHeader[4] = 0;
  assert.equal(TodooCard.validatePayload(badHeader).code, "INVALID_BLOCK_HEADER");

  const badCode = valid.slice();
  badCode[7] = 0x47;
  assert.equal(TodooCard.validatePayload(badCode).code, "INVALID_COLOR_CODE");

  const badPadding = valid.slice();
  badPadding[badPadding.length - 1] = 1;
  assert.equal(TodooCard.validatePayload(badPadding).code, "INVALID_PADDING");
  assert.throws(() => TodooCard.assertValidPayload(badPadding), { code: "INVALID_PAYLOAD" });
});

test("ImageData 被量化为协议六色，透明像素合成到白底", () => {
  const rgba = new Uint8ClampedArray(PIXELS * 4);
  const colors = [
    [0, 0, 0, 255],
    [255, 255, 255, 255],
    [255, 255, 0, 255],
    [255, 0, 0, 255],
    [0, 0, 255, 255],
    [0, 160, 0, 255],
  ];
  const expectedCodes = [0, 1, 2, 3, 5, 6];
  for (let y = 0; y < HEIGHT; y += 1) {
    const color = colors[Math.min(5, Math.floor((y * 6) / HEIGHT))];
    for (let x = 0; x < WIDTH; x += 1) rgba.set(color, (y * WIDTH + x) * 4);
  }
  rgba.set([0, 0, 0, 0], 0);
  const payload = TodooCard.encodeImageData({ width: WIDTH, height: HEIGHT, data: rgba }, { dither: false });
  const decoded = TodooCard.decodePayloadToVisibleCodes(payload);
  assert.equal(decoded[0], TODOO_COLOR_CODES.white);
  for (let band = 0; band < 6; band += 1) {
    const y = Math.min(HEIGHT - 1, Math.floor(((band + 0.5) * HEIGHT) / 6));
    assert.equal(decoded[y * WIDTH + Math.floor(WIDTH / 2)], expectedCodes[band]);
  }
});

function officialSkillSixColorCodes(rgba, width, height) {
  const palette = [
    [0, 0, 0, 0],
    [255, 255, 255, 1],
    [255, 255, 0, 2],
    [255, 0, 0, 3],
    [0, 0, 255, 5],
    [0, 255, 0, 6],
  ];
  const working = Float32Array.from(rgba);
  const codes = new Uint8Array(width * height);
  const addError = (x, y, red, green, blue, weight) => {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    const offset = (y * width + x) * 4;
    working[offset] += red * weight;
    working[offset + 1] += green * weight;
    working[offset + 2] += blue * weight;
  };
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const offset = (y * width + x) * 4;
      const red = Math.min(255, Math.max(0, working[offset]));
      const green = Math.min(255, Math.max(0, working[offset + 1]));
      const blue = Math.min(255, Math.max(0, working[offset + 2]));
      let best = palette[0];
      let bestDistance = Number.POSITIVE_INFINITY;
      for (const color of palette) {
        const distance =
          (red - color[0]) ** 2 +
          (green - color[1]) ** 2 +
          (blue - color[2]) ** 2;
        if (distance < bestDistance) {
          bestDistance = distance;
          best = color;
        }
      }
      codes[y * width + x] = best[3];
      addError(x + 1, y, red - best[0], green - best[1], blue - best[2], 7 / 16);
      addError(x - 1, y + 1, red - best[0], green - best[1], blue - best[2], 3 / 16);
      addError(x, y + 1, red - best[0], green - best[1], blue - best[2], 5 / 16);
      addError(x + 1, y + 1, red - best[0], green - best[1], blue - best[2], 1 / 16);
    }
  }
  return codes;
}

test("skillT3 写入复现官方 TodooCard_Skills 的 raster Floyd-Steinberg", () => {
  const rgba = new Uint8ClampedArray(PIXELS * 4);
  for (let y = 0; y < HEIGHT; y += 1) {
    for (let x = 0; x < WIDTH; x += 1) {
      const offset = (y * WIDTH + x) * 4;
      rgba[offset] = Math.round((x / (WIDTH - 1)) * 255);
      rgba[offset + 1] = Math.round((y / (HEIGHT - 1)) * 180);
      rgba[offset + 2] = 48;
      rgba[offset + 3] = 255;
    }
  }
  rgba.set([255, 255, 0, 255], 0);
  rgba.set([0, 255, 0, 255], ((HEIGHT * WIDTH) - 1) * 4);
  const payload = TodooCard.encodeImageData(
    { width: WIDTH, height: HEIGHT, data: rgba },
    { dither: true, renderProfile: "skill-t3" },
  );
  const decoded = TodooCard.decodePayloadToVisibleCodes(payload);
  const expected = officialSkillSixColorCodes(rgba, WIDTH, HEIGHT);
  assert.equal(decoded[0], TODOO_COLOR_CODES.yellow);
  assert.equal(decoded[decoded.length - 1], TODOO_COLOR_CODES.green);
  assert.deepEqual(Array.from(decoded), Array.from(expected));

  const verified = TodooCard.decodePayloadToVisibleCodes(
    TodooCard.encodeImageData(
      { width: WIDTH, height: HEIGHT, data: rgba },
      { dither: true, renderProfile: "verified" },
    ),
  );
  assert.notDeepEqual(Array.from(verified), Array.from(expected));
});

test("源图在 Canvas 分配前执行 50MP 资源上限", async () => {
  await assert.rejects(
    TodooCard.encodeImageSource(new Blob(["x"]), { maxInputBytes: 0 }),
    (error) => error.code === "INPUT_TOO_LARGE" && error.details.actualBytes === 1,
  );
  await assert.rejects(
    TodooCard.encodeImageSource({ width: 10000, height: 6000 }),
    (error) => error.code === "INPUT_TOO_LARGE" && error.details.sourcePixels === 60000000,
  );
});

test("skill-t3 渲染 profile 和单图右转 90°可独立使用", async () => {
  const rgba = new Uint8ClampedArray(PIXELS * 4);
  new Uint32Array(rgba.buffer).fill(0xffffffff);
  let observedRotation = null;
  const context = {
    fillStyle: "",
    imageSmoothingEnabled: false,
    imageSmoothingQuality: "low",
    fillRect() {},
    save() {},
    translate() {},
    rotate(radians) {
      observedRotation = radians;
    },
    drawImage() {},
    restore() {},
    getImageData() {
      return { width: WIDTH, height: HEIGHT, data: rgba };
    },
  };
  const payload = await TodooCard.encodeImageSource(
    { width: 792, height: 528 },
    {
      sourceRotation: "rotate-right-90",
      renderProfile: "skill-t3",
      payloadMode: "skill-t3",
      dither: true,
      canvasFactory: (width, height) => ({
        width,
        height,
        getContext: () => context,
      }),
    },
  );
  assert.equal(observedRotation, Math.PI / 2);
  assert.equal(payload.length, 218893);
  assert.equal(TodooCard.validatePayload(payload).valid, true);
});

test("模拟 GATT 完整执行设备选择、三段握手、913 包和 05 08", async () => {
  const fake = createFakeStack();
  const card = new TodooCard({ bluetooth: fake.bluetooth, timing: zeroDelayTiming() });
  const states = [];
  const progress = [];
  card.on("state", ({ detail }) => states.push(detail.state));
  card.on("progress", ({ detail }) => progress.push(detail));

  const selected = await card.requestDevice();
  assert.equal(selected, fake.device);
  assert.deepEqual(fake.bluetooth.requestOptions.optionalServices, [
    TODOO_GATT_PROFILES.fef.service,
    TODOO_GATT_PROFILES.fdf.service,
    TODOO_SECURE_PAIRING.battery.service,
  ]);
  assert.equal(fake.bluetooth.requestOptions.filters[0].name, TODOO_PRODUCT_INFO.advertisedName);
  assert.equal(
    fake.bluetooth.requestOptions.filters.some(
      (filter) => filter.services?.[0] === TODOO_GATT_PROFILES.fdf.service,
    ),
    true,
  );
  assert.equal(
    fake.bluetooth.requestOptions.filters.some(
      (filter) => filter.manufacturerData?.[0]?.companyIdentifier === 0x5053,
    ),
    true,
  );

  const payload = TodooCard.createSolidPayload("red");
  const result = await card.writePayload(payload);
  assert.equal(result.success, true);
  assert.equal(result.protocolConfirmed, true);
  assert.equal(result.physicalRefreshPending, true);
  assert.deepEqual(
    fake.control.writes.map((bytes) => Array.from(bytes)),
    TODOO_PROTOCOL.handshake.map((step) => step.write),
  );
  assert.equal(fake.data.writes.length, 913);
  assert.equal(fake.data.writes.every((packet) => packet.length === 244), true);
  assert.deepEqual(Array.from(fake.data.writes[0].subarray(0, 4)), [0, 0, 0, 0]);
  assert.deepEqual(Array.from(fake.data.writes.at(-1).subarray(0, 4)), [0x90, 0x03, 0, 0]);

  for (let index = 0; index < fake.data.writes.length; index += 1) {
    const packet = fake.data.writes[index];
    const sequence = packet[0] | (packet[1] << 8) | (packet[2] << 16) | (packet[3] << 24);
    assert.equal(sequence, index);
    assert.deepEqual(
      packet.subarray(4),
      payload.subarray(index * 240, (index + 1) * 240),
    );
  }
  assert.equal(progress[0].sentPackets, 0);
  assert.equal(progress.at(-1).sentPackets, 913);
  assert.equal(progress.at(-1).percent, 100);
  assert.equal(states.includes("handshake-init"), true);
  assert.equal(states.includes("sending"), true);
  assert.equal(states.includes("waiting-complete"), true);
  assert.equal(states.includes("complete"), true);
  assert.equal(card.state, "complete");
  assert.equal(card.isConnected, false);
});

test("skill-t3 在 FDF profile 上执行 probe、动态块长、短尾包和设备按钮 flag", async () => {
  const payloadBytes = TODOO_TRANSFER_PROFILES.skillT3.payloadBytes;
  const acceptedValueBytes = 100;
  const dataBytesPerPacket = acceptedValueBytes - 4;
  const expectedPackets = Math.ceil(payloadBytes / dataBytesPerPacket);
  const fake = createFakeStack({
    gattProfile: TODOO_GATT_PROFILES.fdf,
    expectedPacketCount: expectedPackets,
  });
  fake.control.initResponse = Uint8Array.from([0x01, acceptedValueBytes, 0x00]);
  const card = new TodooCard({ bluetooth: fake.bluetooth, timing: zeroDelayTiming() });
  const warnings = [];
  card.on("warning", ({ detail }) => warnings.push(detail));

  assert.deepEqual(await card.listAuthorizedDevices(), [fake.device]);
  assert.deepEqual(await card.listAuthorizedDevices({ expectedDeviceId: "wrong" }), []);
  card.useDevice(fake.device);
  await assert.rejects(
    card.writeSolid("white"),
    (error) => error.code === "GATT_PROFILE_REQUIRES_COMPATIBILITY_MODE",
  );
  assert.equal(fake.control.writes.length, 0);
  assert.equal(fake.data.writes.length, 0);
  const probe = await card.probe();
  assert.equal(probe.gattProfile.id, "fdf");
  assert.equal(probe.gattProfile.verified, false);
  assert.equal(probe.control.properties.notify, true);
  assert.equal(probe.data.properties.writeWithoutResponse, true);
  assert.equal(probe.imageCommandsSent, false);
  assert.equal(fake.control.writes.length, 0);

  const result = await card.writeSolid("green", {
    transferProfile: "skill-t3",
    allowDeviceButton: true,
  });
  assert.equal(result.transferProfile, "skill-t3");
  assert.equal(result.gattProfile, "fdf");
  assert.equal(result.payloadBytes, 218893);
  assert.equal(result.packetValueBytes, acceptedValueBytes);
  assert.equal(result.dataBytesPerPacket, dataBytesPerPacket);
  assert.equal(result.packets, expectedPackets);
  assert.equal(warnings.some((warning) => warning.code === "EXPERIMENTAL_COMPATIBILITY_PROFILE"), true);

  assert.deepEqual(Array.from(fake.control.writes[0]), [0x01]);
  assert.deepEqual(Array.from(fake.control.writes[1]), [0x02, 0x0d, 0x57, 0x03, 0x00, 0x11]);
  assert.deepEqual(Array.from(fake.control.writes[2]), [0x03]);
  assert.equal(fake.data.writes.length, expectedPackets);
  assert.equal(fake.data.writes[0].length, acceptedValueBytes);
  assert.equal(fake.data.writes.at(-1).length, 17);
  assert.deepEqual(Array.from(fake.data.writes.at(-1).subarray(0, 4)), [0xe8, 0x08, 0, 0]);
});

test("设备声明非 244-byte 上限时，在发送任何 FEF2 数据前停止", async () => {
  const fake = createFakeStack();
  fake.control.initResponse = Uint8Array.from([0x01, 0x14, 0x00]);
  const card = new TodooCard({ bluetooth: fake.bluetooth, timing: zeroDelayTiming() });
  card.useDevice(fake.device);
  await assert.rejects(card.writeSolid("white"), (error) => {
    assert.equal(error.code, "UNSUPPORTED_PACKET_SIZE");
    return true;
  });
  assert.equal(fake.data.writes.length, 0);
});
