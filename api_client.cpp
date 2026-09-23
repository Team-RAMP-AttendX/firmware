#include "api_client.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

const char* host = "atendx.ai.studio";
const String deviceId = "DEV_TERM_01";

void setupTime() {
  // 3600 seconds = +1 hour offset for WAT, 0 daylight saving time
  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");
  
  Serial.print("Syncing time");
  time_t now = time(nullptr);
  while (now < 100000) { // Wait until time is valid
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println(" Time synced!");
}

String getISOTimestamp() {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  
  char buf[30];
  // Format: 2026-09-21T08:52:14.000Z
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000+01:00", &timeinfo);
  return String(buf);
}

bool sendTelemetry(String lcdLine1, String lcdLine2) {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  HTTPClient http;
  http.begin("https://atendx.ai.studio/api/devices/telemetry");
  http.addHeader("Content-Type", "application/json");

  // Build the 2-line JSON payload using dummy hardware metrics for the demo
  String payload = "{";
  payload += "\"deviceId\":\"" + deviceId + "\",";
  payload += "\"wifiStatus\":\"Connected\",";
  payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"ipAddress\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"powerStatus\":\"AC\",";
  payload += "\"batteryStatus\":100,";
  payload += "\"esp32Heap\":\"" + String(ESP.getFreeHeap() / 1024) + " KB Free\",";
  payload += "\"lcdText\":[\"" + lcdLine1 + "\",\"" + lcdLine2 + "\"]";
  payload += "}";

  int httpCode = http.POST(payload);
  http.end();
  return (httpCode >= 200 && httpCode < 300);
}

bool sendFingerprintLog(int slotNumber) {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  HTTPClient http;
  http.begin("https://atendx.ai.studio/api/attendance/checkin");
  http.addHeader("Content-Type", "application/json");

  String payload = "{";
  payload += "\"deviceId\":\"" + deviceId + "\",";
  payload += "\"slotNumber\":" + String(slotNumber) + ",";
  payload += "\"authMode\":\"fingerprint\",";
  payload += "\"timestamp\":\"" + getISOTimestamp() + "\",";
  payload += "\"offlineBuffered\":false";
  payload += "}";

  int httpCode = http.POST(payload);
  http.end();
  return (httpCode >= 200 && httpCode < 300);
}

bool sendManualLogWithPhoto(String manualID, uint8_t* imageBuf, size_t imageLen) {
  if (WiFi.status() != WL_CONNECTED) return false;

  // 1. First, send the check-in event so the backend creates the attendance record
  HTTPClient http;
  http.begin("https://atendx.ai.studio/api/attendance/checkin");
  http.addHeader("Content-Type", "application/json");
  
  String checkinPayload = "{";
  checkinPayload += "\"deviceId\":\"" + deviceId + "\",";
  checkinPayload += "\"userId\":\"" + manualID + "\",";
  checkinPayload += "\"authMode\":\"pin\",";
  checkinPayload += "\"timestamp\":\"" + getISOTimestamp() + "\",";
  checkinPayload += "\"offlineBuffered\":false";
  checkinPayload += "}";
  
  int checkinCode = http.POST(checkinPayload);
  http.end();
  
  if (checkinCode < 200 || checkinCode >= 300) return false;

  // 2. Stream the evidence photo via multipart/form-data
  WiFiClientSecure client;
  client.setInsecure(); // Bypass SSL check for hackathon speed
  
  if (!client.connect(host, 443)) return false;

  String boundary = "AttendXBoundary123";
  String head = "--" + boundary + "\r\n"
              + "Content-Disposition: form-data; name=\"userId\"\r\n\r\n"
              + manualID + "\r\n"
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

  // Safely stream the JPEG directly from the PSRAM buffer
  uint8_t *fbBuf = imageBuf;
  size_t fbLen = imageLen;
  for (size_t n = 0; n < fbLen; n += 1024) {
    if (n + 1024 < fbLen) {
      client.write(fbBuf, 1024);
      fbBuf += 1024;
    } else {
      client.write(fbBuf, fbLen % 1024);
    }
  }
  
  client.print(tail);
  
  // Read response header to ensure completion
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break; 
  }
  client.stop();
  return true;
}