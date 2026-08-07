type BluetoothErrorLike = {
  code?: unknown;
  message?: unknown;
  name?: unknown;
};

const RECOVERABLE_CODES = new Set([
  "DISCONNECTED",
  "TIMEOUT",
  "CONNECTION_FAILED",
  "NETWORK_ERROR",
]);

export function isRecoverableBluetoothError(error: unknown) {
  const candidate = error && typeof error === "object" ? error as BluetoothErrorLike : {};
  const code = typeof candidate.code === "string" ? candidate.code.toUpperCase() : "";
  if (RECOVERABLE_CODES.has(code)) return true;
  const message = typeof candidate.message === "string" ? candidate.message : String(error ?? "");
  return /no longer in range|gatt.*(?:断开|disconnected|connection)|连接 TodooCard 失败|connection failed|networkerror|device.*(?:范围|range)|发送过程中.*断开/i.test(message);
}

export async function writeWithBluetoothRecovery<T>({
  write,
  reconnect,
  forceReconnect = false,
  retryDelayMs = 650,
  onRecovering,
}: {
  write: () => Promise<T>;
  reconnect: () => Promise<unknown>;
  forceReconnect?: boolean;
  retryDelayMs?: number;
  onRecovering?: (error: unknown) => void;
}) {
  let reconnectBeforeWrite = forceReconnect;
  let lastError: unknown;
  for (let attempt = 0; attempt < 2; attempt += 1) {
    try {
      if (reconnectBeforeWrite) await reconnect();
      return await write();
    } catch (error) {
      lastError = error;
      if (!isRecoverableBluetoothError(error) || attempt === 1) throw error;
      onRecovering?.(error);
      reconnectBeforeWrite = true;
      if (retryDelayMs > 0) await new Promise((resolve) => setTimeout(resolve, retryDelayMs));
    }
  }
  throw lastError;
}
