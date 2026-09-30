#include "api_client.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char* host = "attendx-ramp.vercel.app";
static const char* deviceId = "DEV_TERM_01";
static const char* BUFFER_DIR = "/buf";

static QueueHandle_t outboundQueue;
static QueueHandle_t inboundCommandQueue;
static QueueHandle_t userStatusRequestQueue;
static QueueHandle_t userStatusResultQueue;
static char inProgressCommandId[COMMANDID_MAX_LEN] = "";
static uint32_t nextBufferSeq = 1;
static volatile bool timeSynced = false;

// ---------- time ----------

static void setupTime() {
  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  unsigned long start = millis();
  // Bounded wait -- v2 blocked here forever; NTP servers being briefly
  // unreachable shouldn't hold up the whole network task startup.
  while (now < 100000 && millis() - start < 15000) {
    vTaskDelay(pdMS_TO_TICKS(500)); // note: vTaskDelay, not delay() -- we're inside a task
    now = time(nullptr);
  }
  timeSynced = (now >= 100000);
}

bool isTimeSynced() {
  return timeSynced;
}

static String getISOTimestamp() {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000+01:00", &timeinfo);
  return String(buf);
}

// ---------- LCD text snapshot (cross-core, critical-section protected) ----------

static portMUX_TYPE lcdMux = portMUX_INITIALIZER_UNLOCKED;
static char lastLcdLine1[21] = "Booting";           // 20 cols + null, matches the 20x4 LCD
static char lastLcdLine2[21] = "";

void reportLcdText(const char* line1, const char* line2) {
  portENTER_CRITICAL(&lcdMux);
  strncpy(lastLcdLine1, line1, 20);
  strncpy(lastLcdLine2, line2, 20);
  portEXIT_CRITICAL(&lcdMux);
}

static void getLcdTextSnapshot(char* out1, char* out2) {
  portENTER_CRITICAL(&lcdMux);
  strncpy(out1, lastLcdLine1, 21);
  strncpy(out2, lastLcdLine2, 21);
  portEXIT_CRITICAL(&lcdMux);
}

// ---------- command in-progress dedup ----------
// Cleared by clearInProgressCommand() once the UI task finishes locally
// executing a command -- deliberately NOT tied to whether the result
// report reaches the backend, since that's a separate concern (retried
// via the offline buffer if needed) and gating on it risks getting
// permanently stuck if that first report attempt fails.

void clearInProgressCommand() {
  inProgressCommandId[0] = '\0';
}

// ---------- bounded HTTP helpers ----------

static bool httpPostJson(const char* url, const String& body, String* respOut = nullptr) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();     // TODO: pin the real CA cert before this leaves the demo
  client.setTimeout(5000);

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(body);
  bool ok = (code >= 200 && code < 300);
  if (respOut) *respOut = http.getString(); // capture body even on non-2xx, e.g. {"exists":false}
  http.end();
  return ok;
}

// Returns the HTTP status code (0 on transport failure, e.g. no connection).
// Unlike httpPostJson, callers need to distinguish 404 ("verified: doesn't
// exist") from a transport failure ("couldn't verify at all") -- those
// mean different things to queryUserStatus, so this hands back the raw code
// instead of collapsing it to a bool.
static int httpGetJson(const char* url, String& respOut) {
  if (WiFi.status() != WL_CONNECTED) return 0;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5000);

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, url)) return 0;

  int code = http.GET();
  if (code > 0) respOut = http.getString();
  http.end();
  return code;
}

// ---------- shared checkin JSON builder ----------
// Used both by the normal send path and the queue-full fallback, so
// there's exactly one place that knows the checkin payload shape.

static void buildCheckinJson(String& out, OutboundEventType type, const char* userId,
                              int slotNumber, bool offlineBuffered) {
  StaticJsonDocument<256> doc;
  doc["deviceId"] = deviceId;
  doc["timestamp"] = getISOTimestamp();
  doc["offlineBuffered"] = offlineBuffered;

  if (type == EVT_FINGERPRINT_CHECKIN) {
    // v3: check-in sends slotNumber only -- backend resolves the userId.
    doc["slotNumber"] = slotNumber;
    doc["authMode"] = "fingerprint";
  } else {
    doc["userId"] = userId;
    doc["authMode"] = "pin";
  }
  serializeJson(doc, out);
}

// ---------- offline buffer (LittleFS, JSON-only, photos excluded) ----------

// Normalizes a File::name() result to a full "/buf/xxxxx.json" path,
// regardless of whether this core version's name() already includes
// the directory prefix or not -- that behavior has changed across
// arduino-esp32 core versions, so don't assume either way.
static String normalizeBufferPath(const char* rawName) {
  String name = String(rawName);
  if (!name.startsWith("/")) {
    name = String(BUFFER_DIR) + "/" + name;
  }
  return name;
}

static void initOfflineBuffer() {
  if (!LittleFS.begin(true)) return;
  LittleFS.mkdir(BUFFER_DIR);

  File dir = LittleFS.open(BUFFER_DIR);
  File f = dir.openNextFile();
  while (f) {
    String path = normalizeBufferPath(f.name());
    int slash = path.lastIndexOf('/');
    String basename = path.substring(slash + 1); // "00001.json"
    uint32_t seq = (uint32_t)basename.toInt();     // toInt() stops at the first non-digit, e.g. '.'
    if (seq >= nextBufferSeq) nextBufferSeq = seq + 1;
    f = dir.openNextFile();
  }
  dir.close();
}

static void persistJsonToBuffer(const String& url, const String& json) {
  // Sized to comfortably hold the longest URL + body we send (command
  // result / checkin bodies run up to ~256 bytes) plus ArduinoJson's
  // own overhead.
  StaticJsonDocument<512> wrapper;
  wrapper["url"] = url;
  wrapper["body"] = json;
  String wrapped;
  serializeJson(wrapper, wrapped);

  char path[48];
  snprintf(path, sizeof(path), "%s/%05lu.json", BUFFER_DIR, (unsigned long)nextBufferSeq++);
  File f = LittleFS.open(path, "w");
  if (f) { f.print(wrapped); f.close(); }
}

// Permanent (4xx) rejections are dropped rather than retried forever --
// only a transport/5xx failure gets buffered. lastHttpCode is passed in
// by the caller so this function doesn't need to know about HTTPClient.
static bool isPermanentRejection(int httpCode) {
  return httpCode >= 400 && httpCode < 500;
}

// Tries to send the single oldest buffered event. Returns true if this
// call is done for the cycle (nothing left to usefully retry right now),
// false if it succeeded/dropped an entry and there may be more to drain.
static bool drainOneBufferedEvent() {
  File dir = LittleFS.open(BUFFER_DIR);
  File f = dir.openNextFile();
  String oldestPath = "";
  while (f) {
    String path = normalizeBufferPath(f.name());
    if (oldestPath == "" || path < oldestPath) oldestPath = path;
    f = dir.openNextFile();
  }
  dir.close();
  if (oldestPath == "") return true; // nothing buffered

  File ev = LittleFS.open(oldestPath, "r");
  if (!ev) return true;
  String wrapped = ev.readString();
  ev.close();

  StaticJsonDocument<512> wrapper;
  if (deserializeJson(wrapper, wrapped) != DeserializationError::Ok) {
    LittleFS.remove(oldestPath); // corrupt entry, drop it rather than loop forever
    return false;
  }

  String url = wrapper["url"];
  String body = wrapper["body"];

  if (WiFi.status() != WL_CONNECTED) return true; // don't even try, stop for this cycle

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5000);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, url.c_str())) return true;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();

  if (code >= 200 && code < 300) {
    LittleFS.remove(oldestPath);
    return false; // there may be more to drain, keep going next tick
  }
  if (isPermanentRejection(code)) {
    Serial.printf("Dropping permanently-rejected buffered event (HTTP %d): %s\n", code, oldestPath.c_str());
    LittleFS.remove(oldestPath);
    return false; // don't let a dead entry block newer ones behind it
  }
  return true; // transport failure or 5xx -- still failing, stop for this cycle
}

// ---------- evidence photo upload ----------

static bool sendEvidencePhoto(const String& userId, const String& attendanceId,
                               uint8_t* imageBuf, size_t imageLen) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8000);
  if (!client.connect(host, 443)) return false;

  String boundary = "AttendXBoundary123";
  String head = "--" + boundary + "\r\n"
              + "Content-Disposition: form-data; name=\"userId\"\r\n\r\n" + userId + "\r\n"
              + "--" + boundary + "\r\n"
              + "Content-Disposition: form-data; name=\"attendanceId\"\r\n\r\n" + attendanceId + "\r\n"
              + "--" + boundary + "\r\n"
              + "Content-Disposition: form-data; name=\"deviceId\"\r\n\r\n" + String(deviceId) + "\r\n"
              + "--" + boundary + "\r\n"
              + "Content-Disposition: form-data; name=\"image\"; filename=\"audit.jpg\"\r\n"
              + "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";
  uint32_t totalLen = head.length() + imageLen + tail.length();

  client.println("POST /api/attendance/evidence HTTP/1.1");
  client.println("Host: " + String(host));
  client.println("Content-Length: " + String(totalLen));
  client.println("Content-Type: multipart/form-data; boundary=" + boundary);
  client.println();
  client.print(head);

  uint8_t* p = imageBuf;
  size_t remaining = imageLen;
  while (remaining > 0) {
    size_t chunk = remaining < 1024 ? remaining : 1024;
    client.write(p, chunk);
    p += chunk;
    remaining -= chunk;
  }
  client.print(tail);

  String statusLine = client.readStringUntil('\n');
  bool ok = statusLine.indexOf(" 200 ") > 0 || statusLine.indexOf(" 201 ") > 0;

  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }
  client.stop();
  return ok;
}

// ---------- outbound event handling ----------

static void handleOutboundEvent(OutboundEvent& evt) {
  bool sent = false;
  String url, body, respBody;

  if (evt.type == EVT_FINGERPRINT_CHECKIN || evt.type == EVT_MANUAL_CHECKIN_WITH_PHOTO) {
    buildCheckinJson(body, evt.type, evt.userId, evt.slotNumber, evt.offlineBuffered);
    url = "https://attendx-ramp.vercel.app/api/attendance/checkin";
    sent = httpPostJson(url.c_str(), body, &respBody);

    if (sent && evt.type == EVT_MANUAL_CHECKIN_WITH_PHOTO && evt.photoBuf) {
      StaticJsonDocument<256> respDoc;
      deserializeJson(respDoc, respBody);
      String attendanceId = respDoc["attendanceId"] | "";
      if (!sendEvidencePhoto(evt.userId, attendanceId, evt.photoBuf, evt.photoLen)) {
        Serial.println("Evidence photo failed to upload -- not retried/buffered (see design note)");
      }
    }
  } else if (evt.type == EVT_COMMAND_RESULT) {
    StaticJsonDocument<256> doc;
    doc["deviceId"] = deviceId;
    if (strlen(evt.commandId) > 0) doc["commandId"] = evt.commandId;
    doc["type"] = evt.commandType;
    doc["status"] = evt.status;
    doc["source"] = evt.source;
    if (strlen(evt.errorReason) > 0) doc["errorReason"] = evt.errorReason;
    if (evt.slotNumber > 0) doc["slotNumber"] = evt.slotNumber;
    if (strlen(evt.userId) > 0) doc["userId"] = evt.userId;
    doc["timestamp"] = getISOTimestamp();

    url = "https://attendx-ramp.vercel.app/api/devices/commands/result";
    serializeJson(doc, body);
    sent = httpPostJson(url.c_str(), body, &respBody);
    // Note: local dedup no longer waits on this succeeding -- see
    // clearInProgressCommand(), called from AttendX.ino right after
    // a dashboard command finishes executing.
  }

  if (!sent) {
    persistJsonToBuffer(url, body);
  }

  if (evt.photoBuf) free(evt.photoBuf);
}

// ---------- user status pre-check (v3) ----------
// Handled entirely inside the network task, same as everything else that
// touches WiFi/HTTPS. The UI task's queryUserStatus() just sends a
// request and blocks on the matching result queue.

static void handleUserStatusRequest(const UserStatusRequest& req) {
  UserStatusResult result = {};

  char url[96];
  snprintf(url, sizeof(url), "https://attendx-ramp.vercel.app/api/users/%s/status", req.userId);

  String respBody;
  int code = httpGetJson(url, respBody);

  if (code == 0) {
    // No transport response at all -- WiFi down, timeout, connect failure.
    result.requestSucceeded = false;
  } else {
    // Both 200 and 404 are "the backend answered us" -- 404 legitimately
    // means exists:false per the contract, so that's still a successful
    // verification, not a failure to verify.
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, respBody) == DeserializationError::Ok) {
      result.requestSucceeded = true;
      result.exists = doc["exists"] | false;
      result.hasFingerprint = doc["hasFingerprint"] | false;
    } else {
      result.requestSucceeded = false; // got a response but couldn't parse it -- treat as unverified
    }
  }

  xQueueSend(userStatusResultQueue, &result, 0);
}

bool queryUserStatus(const String& userId, UserStatusResult& outResult, unsigned long timeoutMs) {
  UserStatusRequest req = {};
  strncpy(req.userId, userId.c_str(), USERID_MAX_LEN - 1);

  if (xQueueSend(userStatusRequestQueue, &req, 0) != pdTRUE) {
    return false; // request queue full -- extremely unlikely (depth 1), but don't hang if so
  }

  return xQueueReceive(userStatusResultQueue, &outResult, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

// ---------- telemetry + command retrieval ----------

static void doTelemetryTick(const char* lcdLine1, const char* lcdLine2) {
  StaticJsonDocument<256> reqDoc;
  reqDoc["deviceId"] = deviceId;
  reqDoc["wifiStatus"] = "Connected";
  reqDoc["rssi"] = WiFi.RSSI();
  reqDoc["ipAddress"] = WiFi.localIP().toString();
  reqDoc["powerStatus"] = "AC";
  reqDoc["batteryStatus"] = 100;
  reqDoc["esp32Heap"] = String(ESP.getFreeHeap() / 1024) + " KB Free";
  JsonArray lcd = reqDoc.createNestedArray("lcdText");
  lcd.add(lcdLine1);
  lcd.add(lcdLine2);

  String body;
  serializeJson(reqDoc, body);

  String respBody;
  if (!httpPostJson("https://attendx-ramp.vercel.app/api/devices/telemetry", body, &respBody)) return;

  StaticJsonDocument<512> respDoc;
  if (deserializeJson(respDoc, respBody) != DeserializationError::Ok) {
    Serial.println("Telemetry response failed to parse -- possible buffer overflow (F1)");
    Serial.println(respBody);
    return;
  }

  if (strlen(inProgressCommandId) > 0) return; // already executing one locally

  JsonArray commands = respDoc["commands"].as<JsonArray>();
  for (JsonObject cmd : commands) {
    const char* cmdId = cmd["commandId"] | "";
    const char* type = cmd["type"] | "";

    IncomingCommand ic = {};
    strncpy(ic.commandId, cmdId, COMMANDID_MAX_LEN - 1);

    if (strcmp(type, "ENROLL_FINGERPRINT") == 0) {
      ic.type = CMD_ENROLL_FINGERPRINT;
      strncpy(ic.userId, cmd["userId"] | "", USERID_MAX_LEN - 1);
    } else if (strcmp(type, "DELETE_FINGERPRINT") == 0) {
      ic.type = CMD_DELETE_FINGERPRINT;
      ic.slotNumber = cmd["slotNumber"] | 0;
    } else {
      continue;
    }

    if (xQueueSend(inboundCommandQueue, &ic, 0) == pdTRUE) {
      strncpy(inProgressCommandId, cmdId, COMMANDID_MAX_LEN - 1);
      break;
    }
  }
}

// ---------- the network task itself ----------

static void networkTaskFn(void* param) {
  initOfflineBuffer();

  unsigned long lastReconnectAttempt = 0;
  unsigned long lastTelemetry = 0;
  unsigned long lastBufferDrain = 0;

  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(200));
  setupTime();

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastReconnectAttempt > 5000) {
        WiFi.reconnect();
        lastReconnectAttempt = millis();
      }
    }

    // Status-check requests are served first and promptly -- the UI task
    // is blocked waiting on one of these, so don't let it wait behind a
    // slow checkin/telemetry cycle.
    UserStatusRequest statusReq;
    if (xQueueReceive(userStatusRequestQueue, &statusReq, 0) == pdTRUE) {
      handleUserStatusRequest(statusReq);
    }

    OutboundEvent evt;
    if (xQueueReceive(outboundQueue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
      handleOutboundEvent(evt);
    }

    if (millis() - lastTelemetry > 30000) {
      char l1[21], l2[21];
      getLcdTextSnapshot(l1, l2);
      doTelemetryTick(l1, l2);
      lastTelemetry = millis();
    }

    if (WiFi.status() == WL_CONNECTED && millis() - lastBufferDrain > 5000) {
      drainOneBufferedEvent();
      lastBufferDrain = millis();
    }
  }
}

// ---------- public API (called from the UI task) ----------

void initApiClient() {
  outboundQueue = xQueueCreate(10, sizeof(OutboundEvent));
  inboundCommandQueue = xQueueCreate(3, sizeof(IncomingCommand));
  userStatusRequestQueue = xQueueCreate(1, sizeof(UserStatusRequest));
  userStatusResultQueue = xQueueCreate(1, sizeof(UserStatusResult));

  xTaskCreatePinnedToCore(
    networkTaskFn,
    "NetworkTask",
    8192,   // stack size in bytes -- HTTPS + JSON parsing is stack-hungry; watch uxTaskGetStackHighWaterMark if you see crashes
    NULL,
    1,      // priority
    NULL,
    0       // core 0, alongside the WiFi driver's own tasks
  );
}

void queueFingerprintCheckin(int slotNumber) {
  OutboundEvent evt = {};
  evt.type = EVT_FINGERPRINT_CHECKIN;
  evt.slotNumber = slotNumber;
  if (xQueueSend(outboundQueue, &evt, 0) != pdTRUE) {
    String body;
    buildCheckinJson(body, EVT_FINGERPRINT_CHECKIN, "", slotNumber, true);
    persistJsonToBuffer("https://attendx-ramp.vercel.app/api/attendance/checkin", body);
  }
}

void queueManualCheckinWithPhoto(const String& userId, uint8_t* photoBuf, size_t photoLen) {
  OutboundEvent evt = {};
  evt.type = EVT_MANUAL_CHECKIN_WITH_PHOTO;
  strncpy(evt.userId, userId.c_str(), USERID_MAX_LEN - 1);
  evt.photoBuf = photoBuf;
  evt.photoLen = photoLen;
  if (xQueueSend(outboundQueue, &evt, 0) != pdTRUE) {
    // Queue full: still buffer the check-in itself, even though the
    // photo can't be retried offline -- losing the whole event here
    // would be worse than just losing the photo.
    String body;
    buildCheckinJson(body, EVT_MANUAL_CHECKIN_WITH_PHOTO, userId.c_str(), 0, true);
    persistJsonToBuffer("https://attendx-ramp.vercel.app/api/attendance/checkin", body);
    free(photoBuf);
  }
}

void queueCommandResult(const IncomingCommand& cmd, bool success, const char* errorReason, int assignedSlot) {
  OutboundEvent evt = {};
  evt.type = EVT_COMMAND_RESULT;
  strncpy(evt.commandId, cmd.commandId, COMMANDID_MAX_LEN - 1);
  strncpy(evt.commandType, cmd.type == CMD_ENROLL_FINGERPRINT ? "ENROLL_FINGERPRINT" : "DELETE_FINGERPRINT", STRFIELD_MAX_LEN - 1);
  strncpy(evt.status, success ? "success" : "error", 15);
  strncpy(evt.source, "dashboard", 15);
  if (errorReason) strncpy(evt.errorReason, errorReason, STRFIELD_MAX_LEN - 1);
  evt.slotNumber = assignedSlot;
  strncpy(evt.userId, cmd.userId, USERID_MAX_LEN - 1);
  xQueueSend(outboundQueue, &evt, 0);
}

void queueTerminalInitiatedResult(const char* type, const String& userId, int slotNumber, bool success, const char* errorReason) {
  OutboundEvent evt = {};
  evt.type = EVT_COMMAND_RESULT;
  strncpy(evt.commandType, type, STRFIELD_MAX_LEN - 1);
  strncpy(evt.status, success ? "success" : "error", 15);
  strncpy(evt.source, "terminal", 15);
  if (errorReason) strncpy(evt.errorReason, errorReason, STRFIELD_MAX_LEN - 1);
  evt.slotNumber = slotNumber;
  strncpy(evt.userId, userId.c_str(), USERID_MAX_LEN - 1);
  xQueueSend(outboundQueue, &evt, 0);
}

bool pollIncomingCommand(IncomingCommand& out) {
  return xQueueReceive(inboundCommandQueue, &out, 0) == pdTRUE;
}