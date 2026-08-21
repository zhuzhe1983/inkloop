#!/usr/bin/env node

import { spawnSync } from "node:child_process";
import { fileURLToPath, pathToFileURL } from "node:url";

export const DUPLICATE_PAIRING_CODE_QUERY = `SELECT
  COUNT(*) AS duplicate_groups,
  COALESCE(SUM(copies - 1), 0) AS excess_rows
FROM (
  SELECT COUNT(*) AS copies
  FROM devices
  WHERE pairing_code IS NOT NULL
  GROUP BY pairing_code
  HAVING COUNT(*) > 1
) AS duplicate_codes`;

export function evaluatePreflightOutput(output) {
  if (!Array.isArray(output) || output.length !== 1 || output[0]?.success !== true) {
    throw new Error("D1 预检没有返回可验证的成功结果");
  }
  const row = output[0]?.results?.[0];
  const duplicateGroups = Number(row?.duplicate_groups);
  const excessRows = Number(row?.excess_rows);
  if (!Number.isSafeInteger(duplicateGroups) || duplicateGroups < 0
    || !Number.isSafeInteger(excessRows) || excessRows < 0) {
    throw new Error("D1 预检结果格式无效");
  }
  return { duplicateGroups, excessRows, safe: duplicateGroups === 0 && excessRows === 0 };
}

export class PairingCodeDuplicateError extends Error {
  constructor(result) {
    super(
      `发现 ${result.duplicateGroups} 组重复的非空设备码，涉及 ${result.excessRows} 条额外记录`,
    );
    this.name = "PairingCodeDuplicateError";
    this.result = result;
  }
}

function usage() {
  return `用法：
  node scripts/preflight-pairing-code-unique.mjs --database <D1_NAME> --local
  node scripts/preflight-pairing-code-unique.mjs --database <D1_NAME> --remote

该命令只执行聚合 SELECT，不显示设备码，也不会修改、删除或轮换任何数据。`;
}

function parseArguments(arguments_) {
  const databaseIndex = arguments_.indexOf("--database");
  const database = databaseIndex >= 0 ? arguments_[databaseIndex + 1] : "";
  const local = arguments_.includes("--local");
  const remote = arguments_.includes("--remote");
  const databaseFlags = arguments_.filter((argument) => argument === "--database").length;
  if (arguments_.length !== 3 || databaseFlags !== 1 || database.startsWith("-")
    || !/^[A-Za-z0-9_-]{1,128}$/.test(database) || local === remote) {
    throw new Error(usage());
  }
  return { database, location: local ? "--local" : "--remote" };
}

export function runPreflight(arguments_, runner = spawnSync) {
  const { database, location } = parseArguments(arguments_);
  const wrangler = fileURLToPath(new URL("../node_modules/wrangler/bin/wrangler.js", import.meta.url));
  const execution = runner(process.execPath, [
    wrangler,
    "d1",
    "execute",
    database,
    location,
    "--command",
    DUPLICATE_PAIRING_CODE_QUERY,
    "--json",
  ], { encoding: "utf8", maxBuffer: 1024 * 1024 });
  if (execution.error) throw execution.error;
  if (execution.status !== 0) {
    throw new Error("D1 只读预检执行失败；未运行迁移，也未修改任何数据");
  }
  let output;
  try {
    output = JSON.parse(execution.stdout);
  } catch {
    throw new Error("无法解析 D1 只读预检结果；未运行迁移，也未修改任何数据");
  }
  const result = evaluatePreflightOutput(output);
  if (!result.safe) throw new PairingCodeDuplicateError(result);
  return result;
}

const isMain = process.argv[1]
  && import.meta.url === pathToFileURL(process.argv[1]).href;

if (isMain) {
  try {
    runPreflight(process.argv.slice(2));
    console.log("预检通过：未发现重复的非空设备码，可以应用 pairing_code 唯一索引迁移。");
  } catch (error) {
    if (error instanceof PairingCodeDuplicateError) {
      console.error(`阻止迁移：${error.message}。`);
      console.error("请先按经审核的恢复策略处理；本预检未显示设备码，也未修改任何数据。");
      process.exitCode = 2;
    } else {
      console.error(error instanceof Error ? error.message : "D1 只读预检失败");
      process.exitCode = 1;
    }
  }
}
