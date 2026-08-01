export type CoreEvent<T = Record<string, unknown>> = { detail: T };

export type CoreBluetoothDevice = {
  id: string;
  name?: string;
  gatt?: { connected: boolean };
};

export const TODOO_PROTOCOL: {
  uuids: { service: string; control: string; data: string };
  image: { visibleWidth: number; visibleHeight: number };
  frame: { payloadBytes: number };
  transfer: { gattValueBytes: number; dataBytesPerPacket: number; packetCount: number };
};

export default class CoreTodooCard {
  constructor(options?: Record<string, unknown>);
  readonly device: CoreBluetoothDevice | null;
  isSupported(): boolean;
  on(type: string, handler: (event: CoreEvent<Record<string, unknown>>) => void): () => void;
  listAuthorizedDevices(options?: Record<string, unknown>): Promise<CoreBluetoothDevice[]>;
  useDevice(device: CoreBluetoothDevice, options?: Record<string, unknown>): this;
  requestDevice(options?: Record<string, unknown>): Promise<CoreBluetoothDevice>;
  disconnect(): void;
  writeImageData(imageData: ImageData, options?: Record<string, unknown>): Promise<unknown>;
  static encodeImageData(imageData: ImageData, options?: Record<string, unknown>): Uint8Array;
}
