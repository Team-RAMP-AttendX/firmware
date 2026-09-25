// api_client.h
#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <Arduino.h>

#define USERID_MAX_LEN     32
#define COMMANDID_MAX_LEN  24
#define STRFIELD_MAX_LEN   24

enum OutboundEventType {
  EVT_FINGERPRINT_CHECKIN,
  EVT_MANUAL_CHECKIN_WITH_PHOTO,
  EVT_COMMAND_RESULT
};

// Plain-data struct, no String fields -- safe to copy across a FreeRTOS queue.
struct OutboundEvent {
  OutboundEventType type;
  char userId[USERID_MAX_LEN];
  int slotNumber;
  char commandId[COMMANDID_MAX_LEN];
  char commandType[STRFIELD_MAX_LEN];
  char status[16];
  char errorReason[STRFIELD_MAX_LEN];
  char source[16];          // "dashboard" or "terminal"
  bool offlineBuffered;
  uint8_t* photoBuf;        // heap-allocated by the caller; network task frees it
  size_t photoLen;
};

enum IncomingCommandType { CMD_ENROLL_FINGERPRINT, CMD_DELETE_FINGERPRINT };

struct IncomingCommand {
  char commandId[COMMANDID_MAX_LEN];
  IncomingCommandType type;
  char userId[USERID_MAX_LEN];
  int slotNumber;
};

// Starts the network task (core 0) and creates the queues. Call once from setup().
void initApiClient();
void reportLcdText(const char* line1, const char* line2);
void clearInProgressCommand(); // call once a dashboard-pushed command has been locally executed (success or failure), regardless of whether reporting it back over the network succeeded
// --- Called from the UI task (core 1) only ---
void queueFingerprintCheckin(int slotNumber);
void queueManualCheckinWithPhoto(const String& userId, uint8_t* photoBuf, size_t photoLen);
void queueCommandResult(const IncomingCommand& cmd, bool success, const char* errorReason, int assignedSlot);
void queueTerminalInitiatedResult(const char* type, const String& userId, int slotNumber, bool success, const char* errorReason);
void reportLcdText(const char* line1, const char* line2); // call from UI task after every updateDisplay

// Non-blocking. Returns true and fills `out` if a command is waiting.
bool pollIncomingCommand(IncomingCommand& out);

#endif