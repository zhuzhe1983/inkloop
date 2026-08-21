import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import test from "node:test";
import { build } from "esbuild";
import { Miniflare } from "miniflare";
import {
  DUPLICATE_PAIRING_CODE_QUERY,
  PairingCodeDuplicateError,
  runPreflight,
} from "../scripts/preflight-pairing-code-unique.mjs";

const buildResult = await build({
  entryPoints: ["app/api/devices/route.ts"],
  bundle: true,
  write: false,
  format: "esm",
  platform: "browser",
  external: ["cloudflare:workers"],
  logLevel: "silent",
});
const workerScript = `${buildResult.outputFiles[0].text}\nexport default {
  fetch(request) { return POST(request); }
};`;

const injectedBuildResult = await build({
  entryPoints: ["app/api/devices/route.ts"],
  bundle: true,
  write: false,
  format: "esm",
  platform: "node",
  logLevel: "silent",
  plugins: [{
    name: "injected-cloudflare-env",
    setup(build_) {
      build_.onResolve({ filter: /^cloudflare:workers$/ }, () => ({
        path: "cloudflare:workers",
        namespace: "injected-env",
      }));
      build_.onLoad({ filter: /.*/, namespace: "injected-env" }, () => ({
        contents: `export const env = new Proxy({}, {
          get(_target, property) { return globalThis.__INKLOOP_DEVICE_TEST_ENV?.[property]; }
        });`,
        loader: "js",
      }));
    },
  }],
});
const injectedRoute = await import(
  `data:text/javascript;base64,${Buffer.from(injectedBuildResult.outputFiles[0].text).toString("base64")}`
);

let databaseSequence = 0;
const migrations = readdirSync("drizzle")
  .filter((name) => /^[0-9]{4}_.+\.sql$/.test(name))
  .sort()
  .map((name) => ({ name, sql: readFileSync(`drizzle/${name}`, "utf8") }));
const pairingMigrationIndex = migrations.findIndex(
  ({ sql }) => sql.includes("idx_devices_pairing_code_unique"),
);
const pairingMigration = migrations[pairingMigrationIndex];

async function applyMigrations(worker, count = migrations.length) {
  const db = await worker.getD1Database("DB");
  for (const migration of migrations.slice(0, count)) {
    await applyMigration(db, migration);
  }
  return db;
}

async function applyMigration(db, migration) {
  const statements = migration.sql
    .replaceAll("--> statement-breakpoint", "")
    .split(";")
    .map((statement) => statement.trim())
    .filter(Boolean);
  await db.batch(statements.map((statement) => db.prepare(statement)));
}

async function createWorker({ migrationCount = migrations.length } = {}) {
  databaseSequence += 1;
  const worker = new Miniflare({
    modules: true,
    script: workerScript,
    compatibilityDate: "2026-05-15",
    compatibilityFlags: ["nodejs_compat"],
    d1Databases: { DB: `pairing-code-api-${databaseSequence}` },
    r2Buckets: { ASSETS: `pairing-code-assets-${databaseSequence}` },
  });
  await applyMigrations(worker, migrationCount);
  return worker;
}

const skuId = "m5-papercolor-c151";
const firstSecret = "a".repeat(64);
const secondSecret = "b".repeat(64);

async function request(worker, payload, headers = {}) {
  const response = await worker.dispatchFetch("http://inkloop.test/api/devices", {
    method: "POST",
    headers: { "content-type": "application/json", ...headers },
    body: JSON.stringify(payload),
  });
  return { response, body: await response.json() };
}

function registration(hardwareId, secret = firstSecret, extra = {}) {
  return {
    action: "register",
    hardwareId,
    secret,
    skuId,
    firmwareVersion: "0.3.0-test",
    ...extra,
  };
}

function metadataFailureDatabase(phase, message) {
  let mutationStatements = 0;
  const database = {
    prepare(sql) {
      if (/^\s*(?:INSERT|UPDATE|DELETE|REPLACE)\b/i.test(sql)) mutationStatements += 1;
      const statement = {
        bind() {
          return statement;
        },
        async all() {
          if (sql.startsWith("PRAGMA index_list")) {
            if (phase === "index_list") throw new Error(message);
            return {
              results: [{ name: "idx_devices_pairing_code_unique", unique: 1, partial: 1 }],
            };
          }
          if (sql.startsWith("PRAGMA index_info")) {
            if (phase === "index_info") throw new Error(message);
            return { results: [{ seqno: 0, name: "pairing_code" }] };
          }
          throw new Error("unexpected metadata all query");
        },
        async first() {
          if (/FROM sqlite_master/i.test(sql)) {
            if (phase === "sqlite_master") throw new Error(message);
            return {
              tbl_name: "devices",
              sql: `CREATE UNIQUE INDEX idx_devices_pairing_code_unique
                ON devices(pairing_code) WHERE pairing_code IS NOT NULL`,
            };
          }
          throw new Error("unexpected metadata first query");
        },
      };
      return statement;
    },
    async batch() {
      return [];
    },
  };
  return { database, mutationStatements: () => mutationStatements };
}

test("register remains backward compatible when pairingCode is absent", async (t) => {
  const worker = await createWorker();
  t.after(() => worker.dispose());

  const first = await request(worker, registration("M5PC:LEGACY01"));
  assert.equal(first.response.status, 200);
  assert.match(first.body.pairingCode, /^[0-9]{6}$/);
  assert.equal(first.body.paired, false);

  const retry = await request(worker, registration("M5PC:LEGACY01"));
  assert.equal(retry.response.status, 200);
  assert.equal(retry.body.deviceId, first.body.deviceId);
  assert.equal(retry.body.pairingCode, first.body.pairingCode);
  assert.equal(retry.body.pairingExpiresAt, first.body.pairingExpiresAt);
});

test("register reuses the exact MyAI code and rejects non-ASCII or non-exact values", async (t) => {
  const worker = await createWorker();
  t.after(() => worker.dispose());

  const exact = await request(worker, registration("M5PC:MYAI0001", firstSecret, { pairingCode: "000042" }));
  assert.equal(exact.response.status, 200);
  assert.equal(exact.body.pairingCode, "000042");

  const malformed = [
    null,
    123456,
    "",
    "12345",
    "1234567",
    " 123456",
    "123456 ",
    "123\n456",
    "１２３４５６",
    "١٢٣٤٥٦",
    "+12345",
  ];
  for (const [index, pairingCode] of malformed.entries()) {
    const result = await request(worker, registration(`M5PC:BAD${String(index).padStart(4, "0")}`, firstSecret, {
      pairingCode,
    }));
    assert.equal(result.response.status, 422, `accepted malformed value ${JSON.stringify(pairingCode)}`);
    assert.deepEqual(result.body, { error: "设备码必须是六位 ASCII 数字" });
  }
});

test("active codes cannot be stolen or rotated and errors do not reveal device identity", async (t) => {
  const worker = await createWorker();
  t.after(() => worker.dispose());

  const first = await request(worker, registration("M5PC:COLLIDE1", firstSecret, { pairingCode: "111111" }));
  assert.equal(first.response.status, 200);
  const db = await worker.getD1Database("DB");
  const beforeRetry = await db.prepare(`SELECT secret_hash, desired_revision, applied_revision
    FROM devices WHERE hardware_id = ?`).bind("M5PC:COLLIDE1").first();

  const collision = await request(worker, registration("M5PC:COLLIDE2", secondSecret, { pairingCode: "111111" }));
  assert.equal(collision.response.status, 409);
  assert.deepEqual(collision.body, { error: "设备码不可用，请重新获取" });
  assert.doesNotMatch(JSON.stringify(collision.body), /COLLIDE1|esp32-|hardware|deviceId/i);

  const unsafeRotation = await request(worker, registration("M5PC:COLLIDE1", firstSecret, {
    pairingCode: "111112",
  }));
  assert.equal(unsafeRotation.response.status, 409);

  const idempotent = await request(worker, registration("M5PC:COLLIDE1", firstSecret, {
    pairingCode: "111111",
  }));
  assert.equal(idempotent.response.status, 200);
  assert.equal(idempotent.body.pairingExpiresAt, first.body.pairingExpiresAt);
  const afterRetry = await db.prepare(`SELECT secret_hash, desired_revision, applied_revision
    FROM devices WHERE hardware_id = ?`).bind("M5PC:COLLIDE1").first();
  assert.deepEqual(afterRetry, beforeRetry);

  const secretConflict = await request(worker, registration("M5PC:COLLIDE1", secondSecret, {
    pairingCode: "111111",
  }));
  assert.equal(secretConflict.response.status, 409);
  const afterSecretConflict = await db.prepare(`SELECT secret_hash, pairing_code
    FROM devices WHERE hardware_id = ?`).bind("M5PC:COLLIDE1").first();
  assert.equal(afterSecretConflict.secret_hash, beforeRetry.secret_hash);
  assert.equal(afterSecretConflict.pairing_code, "111111");
});

test("expired MyAI code is not silently renewed, but a new authoritative code can rotate it", async (t) => {
  const worker = await createWorker();
  t.after(() => worker.dispose());

  const first = await request(worker, registration("M5PC:EXPIRE01", firstSecret, { pairingCode: "222222" }));
  assert.equal(first.response.status, 200);
  const db = await worker.getD1Database("DB");
  await db.prepare("UPDATE devices SET pairing_expires_at = ? WHERE hardware_id = ?")
    .bind("2000-01-01T00:00:00.000Z", "M5PC:EXPIRE01").run();

  const staleRetry = await request(worker, registration("M5PC:EXPIRE01", firstSecret, {
    pairingCode: "222222",
  }));
  assert.equal(staleRetry.response.status, 409);

  const rotated = await request(worker, registration("M5PC:EXPIRE01", firstSecret, { pairingCode: "222223" }));
  assert.equal(rotated.response.status, 200);
  assert.equal(rotated.body.pairingCode, "222223");
  assert.ok(new Date(rotated.body.pairingExpiresAt).getTime() > Date.now());

  await db.prepare("UPDATE devices SET pairing_expires_at = ? WHERE hardware_id = ?")
    .bind("2000-01-01T00:00:00.000Z", "M5PC:EXPIRE01").run();
  const recycled = await request(worker, registration("M5PC:EXPIRE02", secondSecret, { pairingCode: "222223" }));
  assert.equal(recycled.response.status, 200);
  const retired = await db.prepare("SELECT pairing_code FROM devices WHERE hardware_id = ?")
    .bind("M5PC:EXPIRE01").first();
  assert.equal(retired.pairing_code, null);
});

test("owned devices clear historical codes and reject supplied-code re-registration", async (t) => {
  const worker = await createWorker();
  t.after(() => worker.dispose());

  const registered = await request(worker, registration("M5PC:OWNED001", firstSecret, {
    pairingCode: "333333",
  }));
  assert.equal(registered.response.status, 200);
  const ownerHeaders = { "x-inkloop-owner": "c".repeat(32) };
  const claimed = await request(worker, { action: "claim", code: "333333" }, ownerHeaders);
  assert.equal(claimed.response.status, 200);

  const legacyRetry = await request(worker, registration("M5PC:OWNED001"));
  assert.equal(legacyRetry.response.status, 200);
  assert.equal(legacyRetry.body.paired, true);
  assert.equal(legacyRetry.body.pairingCode, null);
  assert.equal(legacyRetry.body.pairingExpiresAt, null);

  const suppliedRetry = await request(worker, registration("M5PC:OWNED001", firstSecret, {
    pairingCode: "333333",
  }));
  assert.equal(suppliedRetry.response.status, 409);
  assert.deepEqual(suppliedRetry.body, { error: "设备码不可用，请重新获取" });

  const db = await worker.getD1Database("DB");
  const stored = await db.prepare(`SELECT pairing_code, pairing_expires_at, desired_revision,
    applied_revision FROM devices WHERE hardware_id = ?`).bind("M5PC:OWNED001").first();
  assert.equal(stored.pairing_code, null);
  assert.equal(stored.pairing_expires_at, null);
  assert.equal(stored.desired_revision, 0);
  assert.equal(stored.applied_revision, 0);
});

test("unique constraint resolves concurrent code races without fallback codes", async (t) => {
  const worker = await createWorker();
  t.after(() => worker.dispose());

  const collisionResults = await Promise.all([
    request(worker, registration("M5PC:RACE0001", firstSecret, { pairingCode: "444444" })),
    request(worker, registration("M5PC:RACE0002", secondSecret, { pairingCode: "444444" })),
  ]);
  assert.deepEqual(collisionResults.map(({ response }) => response.status).sort(), [200, 409]);
  const winner = collisionResults.find(({ response }) => response.status === 200);
  assert.equal(winner.body.pairingCode, "444444");

  const idempotentResults = await Promise.all([
    request(worker, registration("M5PC:RACE0003", firstSecret, { pairingCode: "555555" })),
    request(worker, registration("M5PC:RACE0003", firstSecret, { pairingCode: "555555" })),
  ]);
  assert.deepEqual(idempotentResults.map(({ response }) => response.status), [200, 200]);
  assert.equal(idempotentResults[0].body.deviceId, idempotentResults[1].body.deviceId);
  assert.equal(idempotentResults[0].body.pairingCode, "555555");
  assert.equal(idempotentResults[1].body.pairingCode, "555555");

  const db = await worker.getD1Database("DB");
  const uniqueIndex = await db.prepare(`SELECT sql FROM sqlite_master
    WHERE type = 'index' AND name = 'idx_devices_pairing_code_unique'`).first();
  assert.match(uniqueIndex.sql, /CREATE UNIQUE INDEX/i);
});

test("PaperColor render strategy survives task creation, storage, and device sync", async (t) => {
  const worker = await createWorker();
  t.after(() => worker.dispose());
  const owner = "a".repeat(32);
  const ownerHeaders = { "x-inkloop-owner": owner };
  const registered = await request(worker, registration("M5PC:RENDER01", firstSecret, {
    pairingCode: "818181",
  }));
  assert.equal(registered.response.status, 200);
  const claimed = await request(worker, { action: "claim", code: "818181" }, ownerHeaders);
  assert.equal(claimed.response.status, 200);

  const png = Buffer.alloc(24);
  png.set([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  png.writeUInt32BE(400, 16);
  png.writeUInt32BE(600, 20);
  const created = await request(worker, {
    action: "upsert-task",
    deviceId: registered.body.deviceId,
    renderStrategy: "reflectance-photo",
    frameDataUrl: `data:image/png;base64,${png.toString("base64")}`,
    app: {
      id: "render-app",
      title: "Reflectance sample",
      scheduleMode: "hourly",
      customMinutes: 30,
      dailyTime: "08:00",
      spec: { kind: "card", orientation: "portrait", display: { renderMode: "reflectance-photo" } },
    },
  }, ownerHeaders);
  assert.equal(created.response.status, 200);
  assert.equal(created.body.task.renderStrategy, "reflectance-photo");

  const synced = await request(worker, {
    action: "sync",
    appliedRevision: 0,
    firmwareVersion: "0.3.0-test",
  }, {
    authorization: `InkloopDevice ${registered.body.deviceId}:${firstSecret}`,
  });
  assert.equal(synced.response.status, 200);
  assert.equal(synced.body.tasks.length, 1);
  assert.equal(synced.body.tasks[0].renderStrategy, "reflectance-photo");

  const invalid = await request(worker, {
    action: "upsert-task",
    deviceId: registered.body.deviceId,
    renderStrategy: "unknown-renderer",
    frameDataUrl: `data:image/png;base64,${png.toString("base64")}`,
    app: { id: "bad-render", title: "Bad", spec: { kind: "card" } },
  }, ownerHeaders);
  assert.equal(invalid.response.status, 400);
});

test("formal migration succeeds only after the read-only duplicate preflight passes", async (t) => {
  assert.equal(pairingMigration?.name, "0005_wide_lionheart.sql");
  assert.match(pairingMigration.sql, /CREATE UNIQUE INDEX `idx_devices_pairing_code_unique`/);
  assert.match(pairingMigration.sql, /WHERE .*pairing_code.* IS NOT NULL/);
  const routeSource = readFileSync("app/api/devices/route.ts", "utf8");
  const schemaSource = readFileSync("db/schema.ts", "utf8");
  assert.doesNotMatch(routeSource, /\bCREATE\s+(?:UNIQUE\s+)?INDEX\b/i);
  assert.match(schemaSource, /uniqueIndex\("idx_devices_pairing_code_unique"\)/);
  const migrationSource = migrations.map(({ sql }) => sql).join("\n");
  for (const indexName of [
    "idx_devices_owner_updated_at",
    "idx_devices_pairing_code",
    "idx_device_tasks_device_revision",
    "idx_device_tasks_owner_updated_at",
    "idx_device_pairing_attempts_updated_at",
    "idx_devices_pairing_code_unique",
  ]) {
    assert.ok(schemaSource.includes(indexName), `${indexName} missing from canonical schema`);
    assert.ok(migrationSource.includes(indexName), `${indexName} missing from canonical migrations`);
  }
  const worker = await createWorker({ migrationCount: pairingMigrationIndex });
  t.after(() => worker.dispose());
  const db = await worker.getD1Database("DB");
  const aggregate = await db.prepare(DUPLICATE_PAIRING_CODE_QUERY).first();
  const fakeRunner = (_command, args) => {
    const query = args[args.indexOf("--command") + 1];
    assert.equal(query, DUPLICATE_PAIRING_CODE_QUERY);
    assert.match(query, /^SELECT/i);
    assert.doesNotMatch(query, /\b(?:INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|REPLACE)\b/i);
    return {
      status: 0,
      stdout: JSON.stringify([{ success: true, results: [aggregate] }]),
    };
  };
  const preflight = runPreflight(["--database", "fixture", "--local"], fakeRunner);
  assert.deepEqual(preflight, { duplicateGroups: 0, excessRows: 0, safe: true });

  await applyMigration(db, pairingMigration);
  const uniqueIndex = await db.prepare(`SELECT sql FROM sqlite_master
    WHERE type = 'index' AND name = 'idx_devices_pairing_code_unique'`).first();
  assert.match(uniqueIndex.sql, /CREATE UNIQUE INDEX/i);
  assert.match(uniqueIndex.sql, /WHERE .*pairing_code.* IS NOT NULL/i);
});

test("counterfeit same-name indexes fail closed before duplicate-code registration", async () => {
  const counterfeitIndexes = [
    {
      name: "wrong table",
      sql: "CREATE UNIQUE INDEX idx_devices_pairing_code_unique ON public_apps(id)",
      permitsDuplicate: true,
    },
    {
      name: "nonunique pairing code",
      sql: `CREATE INDEX idx_devices_pairing_code_unique ON devices(pairing_code)
        WHERE pairing_code IS NOT NULL`,
      permitsDuplicate: true,
    },
    {
      name: "wrong column",
      sql: `CREATE UNIQUE INDEX idx_devices_pairing_code_unique ON devices(id)
        WHERE pairing_code IS NOT NULL`,
      permitsDuplicate: true,
    },
    {
      name: "extra indexed column",
      sql: `CREATE UNIQUE INDEX idx_devices_pairing_code_unique ON devices(pairing_code, id)
        WHERE pairing_code IS NOT NULL`,
      permitsDuplicate: true,
    },
    {
      name: "wrong partial predicate",
      sql: `CREATE UNIQUE INDEX idx_devices_pairing_code_unique ON devices(pairing_code)
        WHERE pairing_code IS NOT NULL AND pairing_code <> '777777'`,
      permitsDuplicate: true,
    },
    {
      name: "missing partial predicate",
      sql: "CREATE UNIQUE INDEX idx_devices_pairing_code_unique ON devices(pairing_code)",
      permitsDuplicate: false,
    },
  ];

  for (const [index, counterfeit] of counterfeitIndexes.entries()) {
    const worker = await createWorker({ migrationCount: pairingMigrationIndex });
    try {
      const db = await worker.getD1Database("DB");
      await db.prepare(counterfeit.sql).run();
      if (counterfeit.permitsDuplicate) {
        const insert = `INSERT INTO devices
          (id, hardware_id, owner_id, sku_id, name, secret_hash, pairing_code, pairing_expires_at)
          VALUES (?, ?, NULL, ?, ?, ?, '777777', '2099-01-01T00:00:00.000Z')`;
        await db.batch([
          db.prepare(insert).bind(`fake-${index}-a`, `M5PC:FAKE${index}A`, skuId, "M5 PaperColor", firstSecret),
          db.prepare(insert).bind(`fake-${index}-b`, `M5PC:FAKE${index}B`, skuId, "M5 PaperColor", secondSecret),
        ]);
        const duplicates = await db.prepare(`SELECT COUNT(*) AS copies FROM devices
          WHERE pairing_code = '777777'`).first();
        assert.equal(duplicates.copies, 2, `${counterfeit.name} unexpectedly enforced the invariant`);
      }

      const before = await db.prepare("SELECT id, hardware_id, pairing_code FROM devices ORDER BY id").all();
      const api = await request(worker, registration(`M5PC:BLOCK${index}`, firstSecret, {
        pairingCode: "777777",
      }));
      assert.equal(api.response.status, 503, counterfeit.name);
      assert.deepEqual(api.body, { error: "设备服务正在升级，请稍后重试" });
      assert.doesNotMatch(
        JSON.stringify(api.body),
        /D1|SQLITE|UNIQUE|constraint|devices|pairing_code|777777/i,
      );
      const after = await db.prepare("SELECT id, hardware_id, pairing_code FROM devices ORDER BY id").all();
      assert.deepEqual(after.results, before.results, `${counterfeit.name} request mutated device rows`);
    } finally {
      await worker.dispose();
    }
  }
});

test("attestation exceptions are typed as 503 even when engine text resembles a unique race", async () => {
  const phases = ["index_list", "index_info", "sqlite_master"];
  const messages = [
    "D1_ERROR: forced metadata read failure",
    "SQLITE_CONSTRAINT: forced PRAGMA failure sentinel",
    "UNIQUE constraint failed: devices.pairing_code [forced metadata failure]",
    "D1_ERROR: SQLITE_CONSTRAINT UNIQUE constraint failed during metadata proof",
  ];

  for (const phase of phases) {
    for (const [messageIndex, message] of messages.entries()) {
      const fixture = metadataFailureDatabase(phase, message);
      globalThis.__INKLOOP_DEVICE_TEST_ENV = { DB: fixture.database };
      const response = await injectedRoute.POST(new Request("http://inkloop.test/api/devices", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(registration(`M5PC:META${phase.length}${messageIndex}`, firstSecret, {
          pairingCode: "888888",
        })),
      }));
      const body = await response.json();
      assert.equal(response.status, 503, `${phase}: ${message}`);
      assert.deepEqual(body, { error: "设备服务正在升级，请稍后重试" });
      assert.equal(fixture.mutationStatements(), 0, `${phase} reached a registration mutation`);
      assert.doesNotMatch(
        JSON.stringify(body),
        /D1|SQLITE|UNIQUE|constraint|devices|pairing_code|888888|forced|metadata/i,
      );
    }
  }
  delete globalThis.__INKLOOP_DEVICE_TEST_ENV;
});

test("legacy duplicate preflight blocks migration without changing or disclosing rows", async (t) => {
  const worker = await createWorker({ migrationCount: pairingMigrationIndex });
  t.after(() => worker.dispose());
  const db = await worker.getD1Database("DB");
  const insert = `INSERT INTO devices
    (id, hardware_id, owner_id, sku_id, name, secret_hash, pairing_code, pairing_expires_at)
    VALUES (?, ?, NULL, ?, ?, ?, ?, ?)`;
  await db.batch([
    db.prepare(insert).bind("legacy-a", "M5PC:LEGACYA", skuId, "M5 PaperColor", firstSecret,
      "666666", "2099-01-01T00:00:00.000Z"),
    db.prepare(insert).bind("legacy-b", "M5PC:LEGACYB", skuId, "M5 PaperColor", secondSecret,
      "666666", "2099-01-01T00:00:00.000Z"),
  ]);

  const aggregate = await db.prepare(DUPLICATE_PAIRING_CODE_QUERY).first();
  const fakeRunner = () => ({
    status: 0,
    stdout: JSON.stringify([{ success: true, results: [aggregate] }]),
  });
  assert.throws(
    () => runPreflight(["--database", "fixture", "--local"], fakeRunner),
    (error) => error instanceof PairingCodeDuplicateError
      && error.result.duplicateGroups === 1
      && error.result.excessRows === 1
      && !error.message.includes("666666"),
  );

  const before = await db.prepare("SELECT id, pairing_code FROM devices ORDER BY id").all();
  await assert.rejects(() => applyMigration(db, pairingMigration));
  const after = await db.prepare("SELECT id, pairing_code FROM devices ORDER BY id").all();
  assert.deepEqual(after.results, before.results);
  const uniqueIndex = await db.prepare(`SELECT name FROM sqlite_master
    WHERE type = 'index' AND name = 'idx_devices_pairing_code_unique'`).first();
  assert.equal(uniqueIndex, null);

  const api = await request(worker, registration("M5PC:LEGACYC", firstSecret, { pairingCode: "666666" }));
  assert.equal(api.response.status, 503);
  assert.deepEqual(api.body, { error: "设备服务正在升级，请稍后重试" });
  assert.doesNotMatch(
    JSON.stringify(api.body),
    /D1|SQLITE|UNIQUE|constraint|devices|pairing_code|666666/i,
  );
  const finalRows = await db.prepare("SELECT id, pairing_code FROM devices ORDER BY id").all();
  assert.deepEqual(finalRows.results, before.results);
});
