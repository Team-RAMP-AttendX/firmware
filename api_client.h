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

// ---------- user status pre-check (v3) ----------
//
// Separate from OutboundEvent/outboundQueue on purpose: everything else on
// that queue is fire-and-forget (UI task posts, moves on). This one is a
// blocking request/response -- the UI task needs the answer before deciding
// whether to prompt "Place Finger" at all, so enrollment never proceeds
// against an unverified userId. Only the network task is allowed to touch
// WiFi/HTTPS, so this still has to be a hand-off, just a synchronous one.

struct UserStatusRequest {
  char userId[USERID_MAX_LEN];
};

struct UserStatusResult {
  bool requestSucceeded;   // false = couldn't reach the backend at all
                            // (WiFi down, timeout, bad response). Caller
                            // must refuse to enroll in this case -- never
                            // guess when verification itself failed.
  bool exists;
  bool hasFingerprint;
};
enum PreEnrollCheck { PRECHECK_OK, PRECHECK_NOT_FOUND, PRECHECK_ALREADY_ENROLLED, PRECHECK_VERIFY_FAILED };
// Starts the network task (core 0) and creates the queues. Call once from setup().
void initApiClient();

// --- Called from the UI task (core 1) only ---
void queueFingerprintCheckin(int slotNumber);
void queueManualCheckinWithPhoto(const String& userId, uint8_t* photoBuf, size_t photoLen);
void queueCommandResult(const IncomingCommand& cmd, bool success, const char* errorReason, int assignedSlot);
void queueTerminalInitiatedResult(const char* type, const String& userId, int slotNumber, bool success, const char* errorReason);

// Blocking. Sends the request to the network task and waits up to timeoutMs
// for the answer. Returns false only if no answer arrived within timeoutMs
// (queue full, or the network task never replied) -- in that case treat it
// the same as requestSucceeded == false in outResult: refuse to enroll.
bool queryUserStatus(const String& userId, UserStatusResult& outResult, unsigned long timeoutMs = 5000);

// Non-blocking. Returns true and fills `out` if a command is waiting.
bool pollIncomingCommand(IncomingCommand& out);

void clearInProgressCommand(); // call once a dashboard-pushed command has been locally executed (success or failure), regardless of whether reporting it back over the network succeeded

// Call from the UI task after every updateDisplay(), so telemetry always
// reports what's actually on screen.
void reportLcdText(const char* line1, const char* line2);

// True once NTP has completed at least one successful sync. The idle
// screen should show a placeholder ("--:--") until this is true.
bool isTimeSynced();

#endif