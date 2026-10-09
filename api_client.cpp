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
static QueueHandle_t checkinResultQueue;      // depth 1, written with xQueueOverwrite
static uint32_t nextCheckinSeq = 1;
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

// ---------- device status snapshot (cross-core, critical-section protected) ----------
// The UI task owns the sensor's UART, so it measures things and publishes
// them here; the network task only ever reads this copy for telemetry.

struct DeviceStatusSnapshot {
  bool fingerprintOk;
  bool cameraOk;
  int enrolledCount;   // < 0 = unknown
  int capacity;
};
static DeviceStatusSnapshot deviceStatus = { false, false, -1, 0 };
static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;

void reportDeviceStatus(bool fingerprintOk, bool cameraOk, int enrolledCount, int capacity) {
  portENTER_CRITICAL(&statusMux);
  deviceStatus.fingerprintOk = fingerprintOk;
  deviceStatus.cameraOk = cameraOk;
  deviceStatus.enrolledCount = enrolledCount;
  deviceStatus.capacity = capacity;
  portEXIT_CRITICAL(&statusMux);
}

static DeviceStatusSnapshot getDeviceStatusSnapshot() {
  portENTER_CRITICAL(&statusMux);
  DeviceStatusSnapshot copy = deviceStatus;
  portEXIT_CRITICAL(&statusMux);
  return copy;
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

// codeOut is optional and defaults to nullptr so existing call sites that
// only care about success/fail don't need to change. Added alongside the
// command-result logging below -- true/false alone doesn't distinguish a
// 400 (bad request -- a firmware bug) from a 404 (route/host issue) from
// a 500 (backend bug) from a transport failure (code stays 0), and those
// each point somewhere different when diagnosing a silent failure.
static bool httpPostJson(const char* url, const String& body, String* respOut = nullptr, int* codeOut = nullptr) {
  if (WiFi.status() != WL_CONNECTED) {
    if (codeOut) *codeOut = 0;
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();     // TODO: pin the real CA cert before this leaves the demo
  client.setTimeout(5000);

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, url)) {
    if (codeOut) *codeOut = 0;
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(body);
  bool ok = (code >= 200 && code < 300);
  if (respOut) *respOut = http.getString(); // capture body even on non-2xx, e.g. {"exists":false}
  if (codeOut) *codeOut = code;
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
  // 408 (request timeout) and 429 (rate limited) are 4xx but transient --
  // retrying them later is correct, so they are not "permanent".
  return httpCode >= 400 && httpCode < 500 && httpCode != 408 && httpCode != 429;
}

// Counts files waiting in the offline buffer (for telemetry's pendingRecords).
static int countBufferedEvents() {
  int n = 0;
  File dir = LittleFS.open(BUFFER_DIR);
  if (!dir) return 0;
  File f = dir.openNextFile();
  while (f) { n++; f = dir.openNextFile(); }
  dir.close();
  return n;
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

// Pulls only the fields the LCD needs out of a check-in response. A full
// success reply embeds the whole attendance record array (~900 bytes);
// a filter keeps the parse small no matter how large the reply grows.
// Error replies carry displayMessage too (e.g. "SLOT NOT FOUND").
static void copyTrunc(char* dst, size_t dstSize, const char* src) {
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

static void parseCheckinResponse(const String& respBody, CheckinResult& res, String* attendanceIdOut) {
  StaticJsonDocument<192> filter;
  filter["eventType"] = true;
  filter["userName"] = true;
  filter["displayMessage"] = true;
  filter["isLate"] = true;
  filter["lateMinutes"] = true;
  filter["attendanceId"] = true;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, respBody, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[checkin] response parse failed: %s\n", err.c_str());
    return;
  }
  copyTrunc(res.eventType, sizeof(res.eventType), doc["eventType"] | "");
  copyTrunc(res.userName, sizeof(res.userName), doc["userName"] | "");
  copyTrunc(res.displayMessage, sizeof(res.displayMessage), doc["displayMessage"] | "");
  res.isLate = doc["isLate"] | false;
  res.lateMinutes = doc["lateMinutes"] | 0;
  if (attendanceIdOut) *attendanceIdOut = String((const char*)(doc["attendanceId"] | ""));
}

static void handleOutboundEvent(OutboundEvent& evt) {
  bool sent = false;
  int httpCode = -1;
  String url, body, respBody;
  bool isCheckin = false;
  CheckinResult res = {};

  if (evt.type == EVT_FINGERPRINT_CHECKIN || evt.type == EVT_MANUAL_CHECKIN_WITH_PHOTO) {
    isCheckin = true;
    buildCheckinJson(body, evt.type, evt.userId, evt.slotNumber, evt.offlineBuffered);
    url = "https://attendx-ramp.vercel.app/api/attendance/checkin";
    sent = httpPostJson(url.c_str(), body, &respBody, &httpCode);
    Serial.printf("[checkin] HTTP %d sent=%s body=%s resp=%s\n",
                  httpCode, sent ? "true" : "false", body.c_str(), respBody.c_str());

    res.seq = evt.seq;
    res.httpCode = httpCode;

    if (sent) {
      res.delivered = true;
      String attendanceId = "";
      parseCheckinResponse(respBody, res, &attendanceId);
      // Tell the UI task right away so the LCD isn't held up by the photo upload below.
      if (evt.seq != 0) xQueueOverwrite(checkinResultQueue, &res);

      if (evt.type == EVT_MANUAL_CHECKIN_WITH_PHOTO && evt.photoBuf) {
        if (!sendEvidencePhoto(evt.userId, attendanceId, evt.photoBuf, evt.photoLen)) {
          Serial.println("Evidence photo failed to upload -- not retried/buffered (see design note)");
        }
      }
    } else if (isPermanentRejection(httpCode)) {
      res.rejected = true;
      parseCheckinResponse(respBody, res, nullptr);
    } else {
      res.buffered = true;
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
    sent = httpPostJson(url.c_str(), body, &respBody, &httpCode);
    Serial.printf("[commands/result] HTTP %d sent=%s body=%s resp=%s\n",
                  httpCode, sent ? "true" : "false", body.c_str(), respBody.c_str());
    // Note: local dedup no longer waits on this succeeding -- see
    // clearInProgressCommand(), called from AttendX.ino right after
    // a dashboard command finishes executing.
  }

  if (!sent) {
    if (isPermanentRejection(httpCode)) {
      // The backend said no and will keep saying no -- buffering it would
      // just retry a doomed request until the drain loop drops it anyway.
      Serial.printf("Event permanently rejected (HTTP %d), not buffered\n", httpCode);
    } else {
      persistJsonToBuffer(url, body);
    }
    // Result goes out AFTER the buffer write, so "Saved Offline" on the
    // LCD is only ever shown once it is actually saved.
    if (isCheckin && evt.seq != 0) xQueueOverwrite(checkinResultQueue, &res);
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
  // Sized up from 256: the extra status fields below would not fit.
  StaticJsonDocument<1024> reqDoc;
  reqDoc["deviceId"] = deviceId;
  reqDoc["wifiStatus"] = "Connected";
  reqDoc["rssi"] = WiFi.RSSI();
  reqDoc["ipAddress"] = WiFi.localIP().toString();
  reqDoc["macAddress"] = WiFi.macAddress();
  reqDoc["powerStatus"] = "AC";
  reqDoc["batteryStatus"] = 100;
  reqDoc["voltage"] = "N/A (AC)";   // not measured -- better than the backend's invented "4.15V (Li-ion)"
  reqDoc["esp32Heap"] = String(ESP.getFreeHeap() / 1024) + " KB Free";
  reqDoc["firmwareVersion"] = FW_VERSION;
  reqDoc["pendingRecords"] = countBufferedEvents();   // offline-buffer depth

  // Real sensor/camera state. Without these the backend fills in its own
  // hard-coded defaults ("DY50 Ready", 300 slots, ...), which stop being
  // true the moment the sensor is offline or swapped.
  DeviceStatusSnapshot st = getDeviceStatusSnapshot();
  if (st.fingerprintOk) {
    char fpStatus[48];
    if (st.enrolledCount >= 0) snprintf(fpStatus, sizeof(fpStatus), "SFM-V1.7 Ready (%d/%d)", st.enrolledCount, st.capacity);
    else                       snprintf(fpStatus, sizeof(fpStatus), "SFM-V1.7 Ready");
    reqDoc["fingerprintStatus"] = fpStatus;
  } else {
    reqDoc["fingerprintStatus"] = "SFM-V1.7 OFFLINE (keypad-only mode)";
  }
  reqDoc["cameraStatus"] = st.cameraOk ? "OV2640 Ready" : "OV2640 OFFLINE";
  if (st.capacity > 0) reqDoc["maxSlots"] = st.capacity;
  if (st.enrolledCount >= 0) {
    reqDoc["enrolledFingerprints"] = st.enrolledCount;
    if (st.capacity > 0) reqDoc["freeSlots"] = st.capacity - st.enrolledCount;
  }
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
  checkinResultQueue = xQueueCreate(1, sizeof(CheckinResult));   // depth 1: xQueueOverwrite keeps only the latest

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

uint32_t queueFingerprintCheckin(int slotNumber) {
  OutboundEvent evt = {};
  evt.type = EVT_FINGERPRINT_CHECKIN;
  evt.slotNumber = slotNumber;
  evt.seq = nextCheckinSeq++;
  xQueueReset(checkinResultQueue);   // drop any stale result from an earlier attempt
  if (xQueueSend(outboundQueue, &evt, 0) != pdTRUE) {
    String body;
    buildCheckinJson(body, EVT_FINGERPRINT_CHECKIN, "", slotNumber, true);
    persistJsonToBuffer("https://attendx-ramp.vercel.app/api/attendance/checkin", body);
    return 0;   // saved straight to the buffer; no result will arrive
  }
  return evt.seq;
}

uint32_t queueManualCheckinWithPhoto(const String& userId, uint8_t* photoBuf, size_t photoLen) {
  OutboundEvent evt = {};
  evt.type = EVT_MANUAL_CHECKIN_WITH_PHOTO;
  strncpy(evt.userId, userId.c_str(), USERID_MAX_LEN - 1);
  evt.photoBuf = photoBuf;
  evt.photoLen = photoLen;
  evt.seq = nextCheckinSeq++;
  xQueueReset(checkinResultQueue);
  if (xQueueSend(outboundQueue, &evt, 0) != pdTRUE) {
    // Queue full: still buffer the check-in itself, even though the
    // photo can't be retried offline -- losing the whole event here
    // would be worse than just losing the photo.
    String body;
    buildCheckinJson(body, EVT_MANUAL_CHECKIN_WITH_PHOTO, userId.c_str(), 0, true);
    persistJsonToBuffer("https://attendx-ramp.vercel.app/api/attendance/checkin", body);
    free(photoBuf);
    return 0;
  }
  return evt.seq;
}

bool waitForCheckinResult(uint32_t seq, CheckinResult& out, unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    unsigned long remaining = timeoutMs - (millis() - start);
    if (xQueueReceive(checkinResultQueue, &out, pdMS_TO_TICKS(remaining)) != pdTRUE) break;
    if (out.seq == seq) return true;   // anything else is a stale result from an older attempt
  }
  return false;
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