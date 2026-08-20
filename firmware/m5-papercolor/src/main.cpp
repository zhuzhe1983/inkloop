#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <M5PM1.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <time.h>

extern "C" {
__attribute__((used)) char inkloop_api_url_slot[192] = "INKLOOP_API_URL_SLOT::";
}

namespace {

constexpr char kFirmwareVersion[] = "0.2.0";
constexpr char kSkuId[] = "m5-papercolor-c151";
constexpr char kDefaultApiUrl[] = "https://inkloop.vibapp.ai/api/devices";
constexpr char kUnpatchedApiPrefix[] = "INKLOOP_";
constexpr char kTasksPath[] = "/tasks.json";
constexpr uint32_t kSyncIntervalMs = 15000;
constexpr uint32_t kScheduleTickMs = 30000;
constexpr size_t kMaxFrameBytes = 1500000;

// GTS Root R4, published by Google Trust Services and valid through 2036.
constexpr char kRootCa[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)EOF";

M5Canvas canvas(&M5.Display);
M5PM1 pm1;
Preferences preferences;
String hardwareId;
String deviceId;
String deviceSecret;
String pairingCode;
String wifiAccessPoint;
String serialCommand;
uint32_t appliedRevision = 0;
uint32_t lastSyncAt = 0;
uint32_t lastScheduleAt = 0;
uint32_t lastHeartbeatAt = 0;
bool paired = false;
bool pm1Ready = false;

void serialEvent(const char* name, const String& value = "") {
  Serial.print("INKLOOP_");
  Serial.print(name);
  if (value.length()) {
    Serial.print(':');
    Serial.print(value);
  }
  Serial.println();
}

void setStatusLed(uint8_t red, uint8_t green, uint8_t blue, uint8_t brightness = 32) {
  M5.Led.setBrightness(brightness);
  M5.Led.setAllColor(red, green, blue);
  M5.Led.display();
}

void playBootTone(uint16_t frequency = 1047, uint32_t duration = 120) {
  if (!M5.Speaker.isEnabled()) M5.Speaker.begin();
  M5.Speaker.setVolume(72);
  M5.Speaker.tone(frequency, duration);
}

void printDiagnosticStatus() {
  JsonDocument status;
  status["firmware"] = kFirmwareVersion;
  status["hardwareId"] = hardwareId;
  status["board"] = static_cast<int>(M5.getBoard());
  status["pm1"] = pm1Ready;
  status["wifi"] = WiFi.status() == WL_CONNECTED;
  status["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  status["deviceId"] = deviceId;
  status["paired"] = paired;
  status["pairingCode"] = pairingCode;
  status["revision"] = appliedRevision;
  status["freeHeap"] = ESP.getFreeHeap();
  status["freePsram"] = ESP.getFreePsram();
  String payload;
  serializeJson(status, payload);
  serialEvent("STATUS", payload);
}

void resetCanvas(bool landscape = false) {
  canvas.deleteSprite();
  M5.Display.setRotation(landscape ? 0 : 3);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  canvas.fillSprite(WHITE);
}

void drawCentered(const String& title, const String& detail, const String& value = "", uint16_t accent = BLUE) {
  serialEvent("DISPLAY_BEGIN", title);
  resetCanvas(false);
  const int width = canvas.width();
  const int height = canvas.height();
  canvas.fillRect(0, 0, width, 18, accent);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(BLACK);
  canvas.setTextFont(4);
  canvas.setTextSize(1);
  canvas.drawString(title, width / 2, height / 2 - (value.length() ? 105 : 45));
  if (value.length()) {
    canvas.setTextFont(7);
    canvas.setTextSize(1);
    canvas.drawString(value, width / 2, height / 2 - 15);
  }
  canvas.setTextFont(2);
  canvas.setTextSize(1);
  canvas.setTextColor(BLACK);
  canvas.drawString(detail, width / 2, height / 2 + (value.length() ? 70 : 25));
  canvas.setTextColor(accent);
  canvas.drawString("INKLOOP · M5 PAPERCOLOR", width / 2, height - 38);
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  canvas.pushSprite(0, 0);
  serialEvent("DISPLAY_READY", title);
}

String hexBytes(const uint8_t* bytes, size_t length) {
  static constexpr char digits[] = "0123456789abcdef";
  String result;
  result.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    result += digits[bytes[i] >> 4];
    result += digits[bytes[i] & 0x0f];
  }
  return result;
}

void ensureIdentity() {
  preferences.begin("inkloop", false);
  deviceId = preferences.getString("device-id", "");
  deviceSecret = preferences.getString("secret", "");
  appliedRevision = preferences.getUInt("revision", 0);
  if (deviceSecret.length() != 64) {
    uint8_t secret[32];
    esp_fill_random(secret, sizeof(secret));
    deviceSecret = hexBytes(secret, sizeof(secret));
    preferences.putString("secret", deviceSecret);
    preferences.remove("device-id");
    preferences.putUInt("revision", 0);
    deviceId = "";
    appliedRevision = 0;
  }
  const uint64_t mac = ESP.getEfuseMac();
  char id[24];
  snprintf(id, sizeof(id), "M5PC-%012llX", static_cast<unsigned long long>(mac));
  hardwareId = id;
  serialEvent("HARDWARE_ID", hardwareId);
}

String apiUrl() {
  return strncmp(inkloop_api_url_slot, kUnpatchedApiPrefix, strlen(kUnpatchedApiPrefix)) == 0
    ? String(kDefaultApiUrl)
    : String(inkloop_api_url_slot);
}

bool allowedPrivateHttp(const String& url) {
  if (url.startsWith("http://192.168.") || url.startsWith("http://10.")) return true;
  if (!url.startsWith("http://172.")) return false;
  const int secondStart = 11;
  const int secondEnd = url.indexOf('.', secondStart);
  if (secondEnd < 0) return false;
  const int secondOctet = url.substring(secondStart, secondEnd).toInt();
  return secondOctet >= 16 && secondOctet <= 31;
}

template <typename Client>
int postJsonWithClient(Client& client, const String& url, const String& body, String& response, bool authenticate) {
  HTTPClient http;
  if (!http.begin(client, url)) return -1;
  http.setTimeout(20000);
  http.addHeader("Content-Type", "application/json");
  if (authenticate && deviceId.length() && deviceSecret.length()) {
    http.addHeader("Authorization", "InkloopDevice " + deviceId + ":" + deviceSecret);
  }
  const int status = http.POST(body);
  if (status > 0) response = http.getString();
  http.end();
  return status;
}

int postJson(const String& body, String& response, bool authenticate) {
  const String url = apiUrl();
  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    client.setCACert(kRootCa);
    client.setTimeout(20);
    return postJsonWithClient(client, url, body, response, authenticate);
  }
  if (allowedPrivateHttp(url)) {
    WiFiClient client;
    return postJsonWithClient(client, url, body, response, authenticate);
  }
  return -1;
}

bool configureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    serialEvent("WIFI_CONNECTED", WiFi.localIP().toString());
    return true;
  }
  const String suffix = hardwareId.substring(hardwareId.length() - 4);
  wifiAccessPoint = "Inkloop-" + suffix;
  setStatusLed(255, 150, 0, 42);
  serialEvent("WIFI_AP", wifiAccessPoint);
  serialEvent("STATE", "WAITING_WIFI");
  drawCentered("Connect Wi-Fi", "Open the Inkloop setup portal", wifiAccessPoint, YELLOW);
  WiFiManager manager;
  manager.setConfigPortalTimeout(300);
  manager.setConnectTimeout(25);
  manager.setShowStaticFields(false);
  manager.setShowDnsFields(false);
  const bool connected = manager.autoConnect(wifiAccessPoint.c_str());
  if (!connected) {
    serialEvent("ERROR", "WIFI_SETUP_TIMEOUT");
    setStatusLed(255, 0, 0, 48);
    playBootTone(330, 260);
    drawCentered("Wi-Fi needed", "Restart to try setup again", "", RED);
  } else {
    serialEvent("WIFI_CONNECTED", WiFi.localIP().toString());
    setStatusLed(0, 90, 255, 36);
  }
  return connected;
}

bool registerDevice() {
  JsonDocument request;
  request["action"] = "register";
  request["hardwareId"] = hardwareId;
  request["secret"] = deviceSecret;
  request["skuId"] = kSkuId;
  request["firmwareVersion"] = kFirmwareVersion;
  String body;
  serializeJson(request, body);
  String response;
  const int status = postJson(body, response, false);
  serialEvent("REGISTER_HTTP", String(status));
  if (status < 200 || status >= 300) {
    serialEvent("ERROR", "DEVICE_REGISTER_FAILED");
    return false;
  }
  JsonDocument result;
  if (deserializeJson(result, response)) {
    serialEvent("ERROR", "DEVICE_REGISTER_RESPONSE_INVALID");
    return false;
  }
  const String nextDeviceId = result["deviceId"] | "";
  if (!nextDeviceId.length()) return false;
  deviceId = nextDeviceId;
  preferences.putString("device-id", deviceId);
  paired = result["paired"] | false;
  if (!paired) {
    pairingCode = result["pairingCode"] | "------";
    serialEvent("PAIR_CODE", pairingCode);
    serialEvent("STATE", "WAITING_BIND");
    setStatusLed(70, 30, 255, 42);
    playBootTone(1319, 100);
    drawCentered("Device code", "Bind in Inkloop > Add Device", pairingCode, BLUE);
  } else {
    pairingCode = "";
    serialEvent("STATE", "PAIRED");
  }
  return true;
}

bool loadTasks(JsonDocument& tasks) {
  tasks.clear();
  File file = LittleFS.open(kTasksPath, FILE_READ);
  if (!file) {
    tasks.to<JsonArray>();
    return true;
  }
  const DeserializationError error = deserializeJson(tasks, file);
  file.close();
  if (error || !tasks.is<JsonArray>()) {
    tasks.clear();
    tasks.to<JsonArray>();
    return false;
  }
  return true;
}

bool saveTasks(JsonArrayConst tasks) {
  File file = LittleFS.open("/tasks.next", FILE_WRITE);
  if (!file) return false;
  const size_t written = serializeJson(tasks, file);
  file.flush();
  file.close();
  if (!written) return false;
  LittleFS.remove(kTasksPath);
  return LittleFS.rename("/tasks.next", kTasksPath);
}

bool replaceTasks(JsonArrayConst incoming) {
  JsonDocument previousDoc;
  loadTasks(previousDoc);
  JsonArrayConst previous = previousDoc.as<JsonArrayConst>();
  JsonDocument nextDoc;
  JsonArray next = nextDoc.to<JsonArray>();
  for (JsonObjectConst source : incoming) {
    JsonObject target = next.add<JsonObject>();
    target.set(source);
    const String id = source["id"] | "";
    const uint32_t revision = source["revision"] | 0;
    uint32_t lastRun = 0;
    uint32_t lastDay = 0;
    for (JsonObjectConst oldTask : previous) {
      if (id == String(oldTask["id"] | "") && revision == static_cast<uint32_t>(oldTask["revision"] | 0)) {
        lastRun = oldTask["lastRun"] | 0;
        lastDay = oldTask["lastDay"] | 0;
        break;
      }
    }
    target["lastRun"] = lastRun;
    target["lastDay"] = lastDay;
  }
  return saveTasks(nextDoc.as<JsonArrayConst>());
}

bool syncTasks() {
  if (!deviceId.length() || WiFi.status() != WL_CONNECTED) return false;
  JsonDocument request;
  request["action"] = "sync";
  request["appliedRevision"] = appliedRevision;
  request["firmwareVersion"] = kFirmwareVersion;
  String body;
  serializeJson(request, body);
  String response;
  const int status = postJson(body, response, true);
  if (status < 200 || status >= 300) return false;
  JsonDocument result;
  if (deserializeJson(result, response)) return false;
  const bool nowPaired = result["paired"] | false;
  if (!nowPaired) {
    paired = false;
    return registerDevice();
  }
  const bool becamePaired = !paired;
  paired = true;
  pairingCode = "";
  const bool changed = result["changed"] | false;
  const uint32_t revision = result["revision"] | appliedRevision;
  if (changed) {
    JsonArrayConst tasks = result["tasks"].as<JsonArrayConst>();
    if (!replaceTasks(tasks)) return false;
    appliedRevision = revision;
    preferences.putUInt("revision", appliedRevision);
  }
  if (becamePaired) {
    serialEvent("STATE", "PAIRED");
    setStatusLed(0, 255, 60, 42);
    playBootTone(1568, 120);
    drawCentered("Inkloop connected", "Schedules now run on this device", "", GREEN);
  }
  return true;
}

template <typename Client>
bool downloadAndDrawWithClient(Client& client, const String& frameUrl) {
  HTTPClient http;
  if (!http.begin(client, frameUrl)) return false;
  http.setTimeout(30000);
  http.addHeader("Authorization", "InkloopDevice " + deviceId + ":" + deviceSecret);
  const int status = http.GET();
  const int size = http.getSize();
  if (status != HTTP_CODE_OK || size <= 24 || static_cast<size_t>(size) > kMaxFrameBytes) {
    http.end();
    return false;
  }
  auto* bytes = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!bytes) bytes = static_cast<uint8_t*>(malloc(size));
  if (!bytes) {
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  const size_t received = stream->readBytes(bytes, size);
  http.end();
  if (received != static_cast<size_t>(size)) {
    free(bytes);
    return false;
  }
  const uint32_t width = (static_cast<uint32_t>(bytes[16]) << 24) |
    (static_cast<uint32_t>(bytes[17]) << 16) |
    (static_cast<uint32_t>(bytes[18]) << 8) | bytes[19];
  const uint32_t height = (static_cast<uint32_t>(bytes[20]) << 24) |
    (static_cast<uint32_t>(bytes[21]) << 16) |
    (static_cast<uint32_t>(bytes[22]) << 8) | bytes[23];
  const bool landscape = width == 600 && height == 400;
  if ((!landscape && (width != 400 || height != 600))) {
    free(bytes);
    return false;
  }
  resetCanvas(landscape);
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  const bool decoded = canvas.drawPng(bytes, received, 0, 0);
  free(bytes);
  if (!decoded) return false;
  canvas.pushSprite(0, 0);
  return true;
}


bool downloadAndDraw(const String& frameUrl) {
  if (frameUrl.startsWith("https://")) {
    WiFiClientSecure client;
    client.setCACert(kRootCa);
    client.setTimeout(30);
    return downloadAndDrawWithClient(client, frameUrl);
  }
  if (allowedPrivateHttp(frameUrl)) {
    WiFiClient client;
    return downloadAndDrawWithClient(client, frameUrl);
  }
  return false;
}

uint32_t localDayStamp(const tm& local) {
  return static_cast<uint32_t>((local.tm_year + 1900) * 1000 + local.tm_yday);
}

bool taskDue(JsonObject task, time_t now, const tm& local) {
  const String mode = task["scheduleMode"] | "once";
  const uint32_t lastRun = task["lastRun"] | 0;
  if (!lastRun) return true;
  if (mode == "once") return false;
  if (mode == "hourly") return now - lastRun >= 3600;
  if (mode == "custom") {
    const uint32_t minutes = constrain(static_cast<uint32_t>(task["customMinutes"] | 30), 1U, 10080U);
    return now - lastRun >= minutes * 60;
  }
  if (mode == "daily") {
    const String daily = task["dailyTime"] | "08:00";
    const int hour = daily.substring(0, 2).toInt();
    const int minute = daily.substring(3, 5).toInt();
    return local.tm_hour == hour && local.tm_min >= minute && localDayStamp(local) != static_cast<uint32_t>(task["lastDay"] | 0);
  }
  return false;
}

void runDueTasks() {
  const time_t now = time(nullptr);
  if (!paired || now < 1700000000 || WiFi.status() != WL_CONNECTED) return;
  tm local{};
  localtime_r(&now, &local);
  JsonDocument tasksDoc;
  loadTasks(tasksDoc);
  JsonArray tasks = tasksDoc.as<JsonArray>();
  bool changed = false;
  for (JsonObject task : tasks) {
    if (!taskDue(task, now, local)) continue;
    const String frameUrl = task["frameUrl"] | "";
    if (!frameUrl.length()) continue;
    if (downloadAndDraw(frameUrl)) {
      task["lastRun"] = static_cast<uint32_t>(now);
      task["lastDay"] = localDayStamp(local);
      changed = true;
    }
    // One E Ink refresh can take 15–30 seconds. Execute at most one due task
    // per loop so sync and power-management work cannot be starved.
    break;
  }
  if (changed) saveTasks(tasksDoc.as<JsonArrayConst>());
}

void initializePaperColorHardware() {
  serialEvent("BOARD", String(static_cast<int>(M5.getBoard())));
  if (M5.getBoard() != m5::board_t::board_M5PaperColor) {
    serialEvent("WARN", "PAPERCOLOR_NOT_DETECTED");
  }

  const m5pm1_err_t pm1Status = pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K);
  pm1Ready = pm1Status == M5PM1_OK;
  serialEvent("PM1", pm1Ready ? "READY" : String("ERROR_") + String(static_cast<int>(pm1Status)));
  if (pm1Ready) {
    pm1.setI2cConfig(0);
    pm1.pinMode(M5PM1_GPIO_NUM_0, OUTPUT);
    pm1.digitalWrite(M5PM1_GPIO_NUM_0, HIGH);
    pm1.setChargeEnable(true);
    pm1.setBoostEnable(true);
  }

  setStatusLed(0, 70, 255, 36);
  playBootTone();
  serialEvent("HARDWARE_READY");
}

void executeSerialCommand(String command) {
  command.trim();
  command.toLowerCase();
  if (!command.length()) return;
  serialEvent("COMMAND", command);
  if (command == "help") {
    serialEvent("HELP", "help,status,pair-code,led-test,sound-test,screen-test,reboot");
  } else if (command == "status" || command == "diag") {
    printDiagnosticStatus();
  } else if (command == "pair-code") {
    serialEvent("PAIR_CODE", pairingCode.length() ? pairingCode : "UNAVAILABLE");
  } else if (command == "led-test") {
    setStatusLed(255, 0, 255, 64);
    serialEvent("TEST", "LED_OK");
  } else if (command == "sound-test") {
    playBootTone(880, 220);
    serialEvent("TEST", "SOUND_OK");
  } else if (command == "screen-test") {
    drawCentered("Inkloop diagnostics", "Screen refresh completed", hardwareId, GREEN);
    serialEvent("TEST", "SCREEN_OK");
  } else if (command == "reboot") {
    serialEvent("STATE", "REBOOTING");
    Serial.flush();
    delay(120);
    ESP.restart();
  } else {
    serialEvent("ERROR", "UNKNOWN_COMMAND");
  }
}

void pollSerialConsole() {
  while (Serial.available()) {
    const char next = static_cast<char>(Serial.read());
    if (next == '\r') continue;
    if (next == '\n') {
      executeSerialCommand(serialCommand);
      serialCommand = "";
    } else if (serialCommand.length() < 96 && next >= 32 && next <= 126) {
      serialCommand += next;
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t serialWaitStartedAt = millis();
  while (!Serial && millis() - serialWaitStartedAt < 2500) delay(20);
  delay(100);
  serialEvent("BOOT", kFirmwareVersion);
  serialEvent("RESET_REASON", String(static_cast<int>(esp_reset_reason())));
  auto config = M5.config();
  config.clear_display = false;
  M5.begin(config);
  initializePaperColorHardware();
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  resetCanvas(false);
  const bool filesystemReady = LittleFS.begin(true);
  serialEvent("LITTLEFS", filesystemReady ? "READY" : "ERROR");
  ensureIdentity();
  if (!configureWifi()) return;
  configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org", "time.cloudflare.com");
  if (!registerDevice()) {
    setStatusLed(255, 0, 0, 48);
    playBootTone(330, 260);
    drawCentered("Server unavailable", "Will retry automatically", "", RED);
  }
  syncTasks();
  lastSyncAt = millis();
  lastScheduleAt = millis();
  lastHeartbeatAt = millis();
  printDiagnosticStatus();
}

void loop() {
  M5.update();
  pollSerialConsole();
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(500);
    return;
  }
  const uint32_t now = millis();
  if (now - lastSyncAt >= kSyncIntervalMs) {
    lastSyncAt = now;
    if (!deviceId.length()) registerDevice();
    syncTasks();
  }
  if (now - lastScheduleAt >= kScheduleTickMs) {
    lastScheduleAt = now;
    runDueTasks();
  }
  if (now - lastHeartbeatAt >= 5000) {
    lastHeartbeatAt = now;
    serialEvent("HEARTBEAT", String(now));
  }
  delay(50);
}
