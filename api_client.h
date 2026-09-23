#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <Arduino.h>

// Syncs the ESP32's internal clock via Wi-Fi for accurate timestamps
void setupTime();

// Returns the current time in the required ISO8601 format (e.g., 2026-09-21T08:52:14.000Z)
String getISOTimestamp();

// Sends the 30-second heartbeat with dummy battery data and the current LCD text
bool sendTelemetry(String lcdLine1, String lcdLine2);

// Path A: Sends the lightweight JSON payload for a fingerprint match
bool sendFingerprintLog(int slotNumber);

// Path B: Logs the manual PIN check-in, then streams the JPEG buffer for the audit trail
bool sendManualLogWithPhoto(String manualID, uint8_t* imageBuf, size_t imageLen);

#endif