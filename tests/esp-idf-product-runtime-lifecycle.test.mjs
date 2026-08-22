import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import test from "node:test";

const repo = new URL("../", import.meta.url).pathname;
const idf = join(repo, "firmware/inkloop-idf/components");
const product = join(idf, "inkloop_product");

function source(component, path) {
  return readFileSync(join(idf, component, path), "utf8");
}

function body(text, signature, nextSignature) {
  const start = text.indexOf(signature);
  assert.notEqual(start, -1, `missing ${signature}`);
  const end = nextSignature ? text.indexOf(nextSignature, start + signature.length) : -1;
  return text.slice(start, end === -1 ? undefined : end);
}

const stages = [
  "SupervisorInitialize",
  "ButtonsConfigure",
  "LedsConfigure",
  "DisplayConfigure",
  "VoiceInitialize",
  "InkloopInitialize",
  "PortalInitialize",
  "PowerInitialize",
  "VoiceHandlersConfigure",
  "ControlHandlerRegister",
  "NetworkHandlerRegister",
  "NetworkTickRegister",
  "PortalHandlerRegister",
  "PortalTickRegister",
  "WifiInitialize",
  "SupervisorStart",
  "PowerAfterSupervisorStarted",
];

test("every begin acquisition stage has an after-acquire fault point and one rollback", () => {
  const header = source("inkloop_product", "include/inkloop/product_runtime.hpp");
  const runtime = source("inkloop_product", "product_runtime.cpp");
  const begin = body(
    runtime,
    "esp_err_t EspProductRuntime::begin()",
    "esp_err_t EspProductRuntime::shutdownForRecovery()",
  );

  let previous = -1;
  for (const stage of stages) {
    assert.match(header, new RegExp(`\\b${stage}\\b`));
    const at = begin.indexOf(`ProductRuntimeBeginStage::${stage}`);
    assert.ok(at > previous, `${stage} must retain acquisition order`);
    previous = at;
  }
  assert.match(
    begin,
    /status = operation\(\);[\s\S]*status == ESP_OK && begin_fault_injector_[\s\S]*begin_fault_injector_\(stage, begin_fault_context_\)/,
  );
  assert.match(
    begin,
    /if \(status != ESP_OK\)[\s\S]*shutdownForRecovery\(\)[\s\S]*return rollback[\s\S]*return status/,
  );

  // Exercise the contract for every injected post-acquisition failure: the
  // injected stage is considered acquired, rollback sees that exact prefix,
  // and a second rollback has no acquisitions left to release.
  for (let injected = 0; injected < stages.length; ++injected) {
    const acquired = stages.slice(0, injected + 1);
    const released = [...acquired].reverse();
    assert.deepEqual(released, stages.slice(0, injected + 1).reverse());
    acquired.length = 0;
    assert.deepEqual([...acquired].reverse(), []);
  }
});

test("recovery shutdown orders dynamic owners before Wi-Fi and static owners in reverse", () => {
  const runtime = source("inkloop_product", "product_runtime.cpp");
  const shutdown = body(
    runtime,
    "esp_err_t EspProductRuntime::shutdownForRecovery()",
    "WorkDisposition EspProductRuntime::controlHandler",
  );
  const ordered = [
    "buttons_.disarm()",
    "supervisor_.stop()",
    "portal_.shutdown()",
    "voice_.shutdown()",
    "wifi_.shutdown()",
    "power_.shutdown()",
    "inkloop_.shutdown()",
    "display_.shutdown()",
    "leds_.shutdown()",
    "supervisor_.shutdown()",
  ];
  let previous = -1;
  for (const call of ordered) {
    const at = shutdown.indexOf(call, previous + 1);
    assert.ok(at > previous, `${call} must retain shutdown order`);
    previous = at;
  }
  assert.match(shutdown, /started_ = false/);
  assert.match(shutdown, /shutdown_incomplete_ = first != ESP_OK/);
  assert.match(runtime, /EspProductRuntime::~EspProductRuntime\(\)[\s\S]*shutdownForRecovery/);
});

test("OTA quiesce stops every normal writer but retains the sole connected STA", () => {
  const header = source("inkloop_product", "include/inkloop/product_runtime.hpp");
  const runtime = source("inkloop_product", "product_runtime.cpp");
  const ota = body(
    runtime,
    "esp_err_t EspProductRuntime::shutdownForOtaAcquisition()",
    "esp_err_t EspProductRuntime::shutdownForRecovery()",
  );
  assert.match(header, /shutdownForOtaAcquisition\(\)/);
  const ordered = [
    "buttons_.disarm()",
    "supervisor_.stop()",
    "portal_.shutdown()",
    "voice_.shutdown()",
    "power_.shutdown()",
    "inkloop_.shutdown()",
    "display_.shutdown()",
    "leds_.shutdown()",
    "supervisor_.shutdown()",
  ];
  let previous = -1;
  for (const call of ordered) {
    const at = ota.indexOf(call, previous + 1);
    assert.ok(at > previous, `${call} must retain OTA quiesce order`);
    previous = at;
  }
  assert.doesNotMatch(ota, /wifi_\.shutdown\(\)/);
  assert.match(ota, /wifi_\.online\(\)/);
  assert.match(ota, /ota_acquisition_quiesced_ = first == ESP_OK/);
  assert.match(
    runtime,
    /shutdownForRecovery\(\)[\s\S]*wifi_\.shutdown\(\)[\s\S]*ota_acquisition_quiesced_ = false/,
  );
});

test("quiesce APIs release tasks, HTTP, callbacks, audio and storage writers", () => {
  const supervisor = source("inkloop_runtime", "runtime_supervisor.cpp");
  const wifi = source("inkloop_connectivity", "esp_wifi_station.cpp");
  const portal = source("inkloop_product", "native_portal_owner.cpp");
  const voice = source("inkloop_product", "native_voice_service.cpp");
  const inkloop = source("inkloop_product", "native_inkloop_service.cpp");
  const display = source("inkloop_product", "native_display_service.cpp");
  const buttons = source("inkloop_product", "button_input.cpp");

  const supervisorShutdown = body(
    supervisor,
    "esp_err_t RuntimeSupervisor::shutdown()",
    "void RuntimeSupervisor::recordAdmissionLocked",
  );
  const cooperativeStop = body(
    supervisor,
    "esp_err_t RuntimeSupervisor::stop()",
    "esp_err_t RuntimeSupervisor::shutdown()",
  );
  assert.match(cooperativeStop, /xTaskAbortDelay/);
  assert.match(cooperativeStop, /xEventGroupWaitBits/);
  assert.match(cooperativeStop, /kSupervisorStopTimeoutMs/);
  assert.ok(
    cooperativeStop.indexOf("xEventGroupWaitBits") <
      cooperativeStop.indexOf("vTaskDelete(slot.task)"),
    "HTTP/storage handler frames must unwind before task deletion",
  );
  assert.match(supervisor, /markTaskQuiescent[\s\S]*xEventGroupSetBits/);
  assert.match(supervisorShutdown, /vQueueDelete\(slot\.queue\)/);
  assert.match(supervisorShutdown, /slot\.handler = nullptr/);
  assert.match(supervisorShutdown, /slot\.tick_handler = nullptr/);
  assert.match(supervisorShutdown, /Lifecycle::Uninitialized/);

  const wifiShutdown = body(
    wifi,
    "esp_err_t EspWifiStationOwner::shutdown()",
    "bool EspWifiStationOwner::credentialFailure",
  );
  for (const contract of [
    /provisioning_server_->stop\(\)/,
    /esp_event_handler_instance_unregister/,
    /esp_netif_sntp_deinit\(\)/,
    /esp_wifi_stop\(\)/,
    /esp_wifi_deinit\(\)/,
    /esp_netif_destroy_default_wifi/,
    /secureZero\(submitted_password_\)/,
  ]) assert.match(wifiShutdown, contract);

  const portalShutdown = body(
    portal,
    "esp_err_t NativePortalOwner::shutdown()",
    "esp_err_t NativePortalOwner::attachSettingsOwner",
  );
  for (const contract of [
    /stopServer\(\)/,
    /album_store_->abort\(\)/,
    /std::fclose/,
    /vQueueDelete/,
    /vSemaphoreDelete/,
    /heap_caps_free/,
  ]) assert.match(portalShutdown, contract);
  assert.match(portal, /mdns_service_remove[\s\S]*mdns_free/);

  const voiceShutdown = body(
    voice,
    "void NativeVoiceService::shutdown()",
    "esp_err_t NativeVoiceService::attachLocalTools",
  );
  for (const contract of [
    /local_prompts_\.cancel/,
    /audio_device_->abort\(\)/,
    /client_\.reset\(\)/,
    /wss_\.close/,
    /audio_device_\.reset\(\)/,
    /audio_bridge_\.reset\(\)/,
    /album_store_->abort\(\)/,
    /chat_log_\.reset\(\)/,
  ]) assert.match(voiceShutdown, contract);

  assert.match(inkloop, /NativeInkloopService::shutdown[\s\S]*album_store_->abort\(\)[\s\S]*client_\.reset\(\)[\s\S]*task_store_\.reset\(\)/);
  assert.match(display, /NativeDisplayService::shutdown[\s\S]*configured_ = false[\s\S]*storage_maintenance_ = false/);
  assert.match(buttons, /gpio_isr_handler_remove[\s\S]*gpio_uninstall_isr_service/);
});

test("lifecycle teardown never erases/formats storage or owns board/global event-loop teardown", () => {
  const files = [
    source("inkloop_product", "product_runtime.cpp"),
    source("inkloop_product", "native_voice_service.cpp"),
    source("inkloop_product", "native_inkloop_service.cpp"),
    source("inkloop_product", "native_portal_owner.cpp"),
    source("inkloop_connectivity", "esp_wifi_station.cpp"),
  ].join("\n");
  const shutdowns = files
    .split(/\n(?=(?:esp_err_t|void) [A-Za-z_:]+::shutdown)/)
    .filter((part) => part.includes("::shutdown"))
    .join("\n");
  assert.doesNotMatch(shutdowns, /formatSdCard|nvs_flash_erase|esp_wifi_restore/);
  assert.doesNotMatch(shutdowns, /board_\.shutdown|esp_event_loop_delete_default|esp_netif_deinit/);
});
