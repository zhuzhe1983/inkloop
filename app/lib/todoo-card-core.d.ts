export type CoreEvent<T = Record<string, unknown>> = { detail: T };

export type CoreBluetoothDevice = {
  id: string;
  name?: string | null;
  gatt?: { connected: boolean };
};

export const TODOO_PROTOCOL: {
  uuids: { service: string; control: string; data: string };
  image: { visibleWidth: number; visibleHeight: number };
  frame: { payloadBytes: number };
  transfer: { gattValueBytes: number; dataBytesPerPacket: number; packetCount: number };
};

export const TODOO_SECURE_PAIRING: {
  manufacturerId: number;
  screenType: number;
  minimumSecureFirmware: number;
  latestReviewedFirmware: number;
  flags: { encryptedGatt: number; pairingWindow: number; otaRecovery: number };
  battery: { service: string; level: string };
  rearButtonHoldSeconds: number;
  pairingWindowSeconds: number;
  encryptionTimeoutRetryMs: readonly number[];
};

export function parseTodooAdvertisementData(
  input: ArrayBuffer | ArrayBufferView | number[],
  options?: { includesManufacturerId?: boolean },
): {
  manufacturerId: number;
  screenType: number;
  capabilityFlags: number;
  firmwareVersion: number;
  secure: boolean;
  pairingWindowOpen: boolean;
  otaRecoveryMode: boolean;
};

export default class CoreTodooCard {
  constructor(options?: Record<string, unknown>);
  readonly device: CoreBluetoothDevice | null;
  isSupported(): boolean;
  on(type: string, handler: (event: CoreEvent<Record<string, unknown>>) => void): () => void;
  listAuthorizedDevices(options?: Record<string, unknown>): Promise<CoreBluetoothDevice[]>;
  useDevice(device: CoreBluetoothDevice, options?: Record<string, unknown>): this;
  requestDevice(options?: Record<string, unknown>): Promise<CoreBluetoothDevice>;
  pairSecureDevice(options?: Record<string, unknown>): Promise<{
    verified: true;
    batteryPercent: number;
    verification: "encrypted-battery-level";
  }>;
  connect(options?: Record<string, unknown>): Promise<unknown>;
  disconnect(): void;
  writeImageData(imageData: ImageData, options?: Record<string, unknown>): Promise<unknown>;
  writeCalibration(options?: Record<string, unknown>): Promise<unknown>;
  static encodeImageData(imageData: ImageData, options?: Record<string, unknown>): Uint8Array;
}
