import { execFileSync } from "node:child_process";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const cloud = join(repo, "firmware/inkloop-idf/components/inkloop_cloud");
const storage = join(repo, "firmware/inkloop-idf/components/inkloop_storage");
const myai = join(repo, "firmware/inkloop-idf/components/inkloop_myai");
const cjson = join(
  repo,
  "firmware/inkloop-idf/managed_components/espressif__cjson/cJSON",
);

const harness = String.raw`
#include <cassert>
#include <deque>
#include <string>
#include <vector>

#include "inkloop/inkloop_cloud_client.hpp"

using namespace inkloop;

class Identity final : public cloud::IInkloopIdentityStore {
 public:
  cloud::InkloopIdentitySnapshot snapshot;
  bool fail_device = false;
  bool fail_revision = false;
  unsigned device_writes = 0;
  unsigned revision_writes = 0;

  cloud::InkloopCloudStatus loadOrCreate(
      cloud::InkloopIdentitySnapshot& output) override {
    output = snapshot;
    return cloud::InkloopCloudStatus::success();
  }
  cloud::InkloopCloudStatus saveDeviceId(
      const std::string& device_id) override {
    ++device_writes;
    if (fail_device) return {cloud::InkloopCloudCode::Storage, 0, 0, "device"};
    snapshot.device_id = device_id;
    return cloud::InkloopCloudStatus::success();
  }
  cloud::InkloopCloudStatus saveAppliedRevision(uint32_t revision) override {
    ++revision_writes;
    if (fail_revision) return {cloud::InkloopCloudCode::Storage, 0, 0, "revision"};
    snapshot.applied_revision = revision;
    return cloud::InkloopCloudStatus::success();
  }
};

struct Reply { int status; std::string body; };

class Http final : public myai::IHttpTransport {
 public:
  std::deque<Reply> replies;
  std::vector<myai::HttpRequest> requests;
  myai::Status transport = myai::Status::success();

  myai::Status perform(const myai::HttpRequest& request,
                       myai::HttpResponse& response) override {
    requests.push_back(request);
    if (!transport.ok()) return transport;
    assert(!replies.empty());
    response.status = replies.front().status;
    response.body = replies.front().body;
    replies.pop_front();
    return myai::Status::success();
  }
};

std::string registration(bool paired, const std::string& code = "692639") {
  return std::string("{\"deviceId\":\"esp32-12345678-1234-1234-1234-123456789abc\",") +
      "\"paired\":" + (paired ? "true" : "false") +
      ",\"pairingCode\":" + (paired ? "null" : "\"" + code + "\"") +
      ",\"pairingExpiresAt\":" +
      (paired ? "null" : "\"2026-08-22T12:00:00.000Z\"") +
      ",\"pollSeconds\":15}";
}

std::string taskJson(const std::string& id, uint32_t revision,
                     const std::string& strategy = "solid-clean") {
  return "{\"id\":\"" + id + "\",\"title\":\"东方明珠地图\"," +
      "\"scheduleMode\":\"once\",\"customMinutes\":30," +
      "\"dailyTime\":\"08:00\",\"revision\":" +
      std::to_string(revision) +
      ",\"frameUrl\":\"https://inkloop.mess.host/api/devices?mode=frame&taskId=" +
      id + "\",\"frameHash\":\"" + std::string(64, 'a') +
      "\",\"renderStrategy\":\"" + strategy + "\"}";
}

std::string changed(uint32_t revision, const std::string& tasks) {
  return "{\"paired\":true,\"changed\":true,\"replace\":true," +
      std::string("\"revision\":") + std::to_string(revision) +
      ",\"pollSeconds\":15,\"tasks\":[" + tasks + "]}";
}

int main(int argc, char** argv) {
  assert(argc == 2);
  Identity identity;
  identity.snapshot.hardware_id = "M5PC-0CDA43858428";
  identity.snapshot.secret = std::string(64, 'a');
  Http http;
  storage::PosixTaskStore tasks(argv[1]);
  cloud::InkloopCloudClient client({}, http, identity, tasks);
  assert(client.initialize().ok());

  http.replies.push_back({200, registration(false)});
  cloud::InkloopRegistrationResult registered;
  assert(client.registerDevice("692639", registered).ok());
  assert(!registered.paired && registered.pairing_code == "692639");
  assert(registered.requested_pairing_code_accepted);
  assert(identity.device_writes == 1 && !identity.snapshot.device_id.empty());
  assert(http.requests.back().url == "https://inkloop.mess.host/api/devices");
  assert(http.requests.back().body.find("\"pairingCode\":\"692639\"") !=
         std::string::npos);
  assert(http.requests.back().redirectsAllowed == false);
  assert(http.requests.back().tlsPeerVerificationRequired);
  assert(http.requests.back().rejectPrivateResolvedAddresses);

  const std::string first_task = taskJson("dtask-one", 7);
  http.replies.push_back({200, changed(7, first_task)});
  cloud::InkloopSyncResult synced;
  assert(client.syncTasks(synced).ok());
  assert(synced.paired && synced.changed && synced.became_paired &&
         synced.task_count == 1);
  assert(identity.snapshot.applied_revision == 7);
  assert(http.requests.back().headers.at("Authorization") ==
         "InkloopDevice " + identity.snapshot.device_id + ":" +
             identity.snapshot.secret);
  std::vector<storage::InkloopTaskRecord> loaded;
  assert(tasks.load(loaded) == storage::TaskStoreCode::Ok && loaded.size() == 1);
  assert(loaded[0].render_strategy == "solid-clean");

  http.replies.push_back({200,
      "{\"paired\":true,\"changed\":false,\"revision\":7,\"pollSeconds\":15}"});
  assert(client.syncTasks(synced).ok() && !synced.changed);

  // A complete empty replacement is the server deletion contract.
  http.replies.push_back({200, changed(8, "")});
  assert(client.syncTasks(synced).ok() && synced.task_count == 0);
  assert(tasks.load(loaded) == storage::TaskStoreCode::Ok && loaded.empty());

  // If revision persistence fails, tasks remain safely committed but the old
  // revision is retained so the next 30-second sync repeats idempotently.
  identity.fail_revision = true;
  http.replies.push_back({200, changed(9, first_task)});
  auto failed_revision = client.syncTasks(synced);
  assert(failed_revision.code == cloud::InkloopCloudCode::Storage);
  assert(client.identity().applied_revision == 8);
  assert(tasks.load(loaded) == storage::TaskStoreCode::Ok && loaded.size() == 1);
  identity.fail_revision = false;
  http.replies.push_back({200, changed(9, first_task)});
  assert(client.syncTasks(synced).ok());
  assert(client.identity().applied_revision == 9);

  // Pairing code mismatches and malformed/duplicate task payloads fail closed.
  http.replies.push_back({200, registration(false, "123456")});
  auto mismatch = client.registerDevice("692639", registered);
  assert(mismatch.code == cloud::InkloopCloudCode::Conflict);
  http.replies.push_back({200,
      changed(10, first_task + "," + first_task)});
  auto duplicate = client.syncTasks(synced);
  assert(duplicate.code == cloud::InkloopCloudCode::Protocol);
  http.replies.push_back({200,
      changed(10, taskJson("dtask-one", 10, "unknown"))});
  auto strategy = client.syncTasks(synced);
  assert(strategy.code == cloud::InkloopCloudCode::Protocol);
  http.replies.push_back({200,
      "{\"paired\":true,\"changed\":false,\"revision\":9,"
      "\"pollSeconds\":15,\"pollSeconds\":30}"});
  assert(client.syncTasks(synced).code == cloud::InkloopCloudCode::Protocol);

  http.replies.push_back({401, "{\"error\":\"no\"}"});
  assert(client.syncTasks(synced).code == cloud::InkloopCloudCode::Unauthorized);

  cloud::InkloopCloudConfig insecure;
  insecure.api_url = "http://inkloop.mess.host/api/devices";
  storage::PosixTaskStore other(argv[1]);
  cloud::InkloopCloudClient rejected(insecure, http, identity, other);
  assert(rejected.initialize().code == cloud::InkloopCloudCode::InvalidArgument);
  return 0;
}
`;

function buildAndRun(sanitized) {
  const scratch = mkdtempSync(join(tmpdir(), "inkloop-cloud-client-"));
  try {
    const source = join(scratch, "client.cpp");
    const cObject = join(scratch, "cJSON.o");
    const binary = join(scratch, sanitized ? "sanitized" : "strict");
    const data = join(scratch, "data");
    writeFileSync(source, harness);
    mkdirSync(data);
    const sanitizer = sanitized
      ? ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
      : [];
    execFileSync("cc", [
      "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
      "-Wno-deprecated-declarations", ...sanitizer,
      "-I", cjson, "-c", join(cjson, "cJSON.c"), "-o", cObject,
    ], { stdio: "pipe" });
    execFileSync("c++", [
      "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
      ...sanitizer,
      "-I", join(cloud, "include"),
      "-I", join(storage, "include"),
      "-I", join(myai, "include"),
      "-I", join(myai, "include/inkloop/myai"),
      "-I", cjson,
      source,
      join(cloud, "inkloop_cloud_client.cpp"),
      join(storage, "posix_task_store.cpp"),
      join(storage, "album_index.cpp"),
      cObject, "-lm", "-o", binary,
    ], { stdio: "pipe" });
    execFileSync(binary, [data], {
      env: sanitized
        ? { ...process.env, ASAN_OPTIONS: "detect_leaks=0:halt_on_error=1" }
        : process.env,
      stdio: "pipe",
    });
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

test("portable Inkloop client registers exact MyAI code and applies task replacements", () => {
  buildAndRun(false);
});

test("portable Inkloop client fails closed under ASan/UBSan", () => {
  buildAndRun(true);
});
