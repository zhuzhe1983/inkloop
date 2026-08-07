import test from "node:test";
import assert from "node:assert/strict";

import {
  isRecoverableBluetoothError,
  writeWithBluetoothRecovery,
} from "../app/lib/bluetooth-recovery.ts";

test("识别浏览器离开蓝牙范围和 GATT 断开错误", () => {
  assert.equal(isRecoverableBluetoothError(new Error("Bluetooth Device is no longer in range.")), true);
  assert.equal(isRecoverableBluetoothError(new Error("TodooCard GATT 已断开")), true);
  assert.equal(isRecoverableBluetoothError({ code: "TIMEOUT", message: "连接超时" }), true);
  assert.equal(isRecoverableBluetoothError(new Error("设备拒绝块长协商")), false);
});

test("首次断联会静默重连并复用同一次写入操作", async () => {
  let writes = 0;
  let reconnects = 0;
  let recovering = 0;
  const result = await writeWithBluetoothRecovery({
    retryDelayMs: 0,
    write: async () => {
      writes += 1;
      if (writes === 1) throw new Error("Bluetooth Device is no longer in range.");
      return "ok";
    },
    reconnect: async () => { reconnects += 1; },
    onRecovering: () => { recovering += 1; },
  });
  assert.equal(result, "ok");
  assert.equal(writes, 2);
  assert.equal(reconnects, 1);
  assert.equal(recovering, 1);
});

test("连续离线两次后交给定时任务后续退避重试", async () => {
  let writes = 0;
  let reconnects = 0;
  await assert.rejects(
    () => writeWithBluetoothRecovery({
      retryDelayMs: 0,
      write: async () => {
        writes += 1;
        throw new Error("GATT connection failed");
      },
      reconnect: async () => { reconnects += 1; },
    }),
    /connection failed/,
  );
  assert.equal(writes, 2);
  assert.equal(reconnects, 1);
});
