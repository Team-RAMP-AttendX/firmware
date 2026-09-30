#ifndef FINGERPRINT_H
#define FINGERPRINT_H

#include <Arduino.h>

// Called once per failed init attempt, before the next retry, so the
// caller (AttendX.ino) can update the LCD without fingerprint.cpp
// needing to know anything about ui.h.
typedef void (*WarningCallback)(int attemptNumber, int maxAttempts);

// Called at each step of enrollment so the caller can update the LCD,
// e.g. onPrompt("Place Finger", ""), onPrompt("Remove Finger", "").
typedef void (*EnrollPromptCallback)(const char* line1, const char* line2);

bool initFingerprint(WarningCallback onRetryWarning);
bool isFingerprintAvailable();

// Non-blocking poll.
//  > 0  -> matched slot ID
//    0  -> no finger present -- the normal idle state, checked every
//          loop iteration. Not an event, nothing to show on the LCD.
//   -1  -> sensor communication/read error -- "Read failed, retry"
//   -2  -> a finger WAS placed and read cleanly, but matched nothing --
//          "Finger not registered". Distinct from 0 on purpose: this
//          only fires on an actual placed-and-rejected finger, not on
//          every idle poll where no finger is there at all.
int checkFingerprint();

enum EnrollResult {
  ENROLL_OK,
  ENROLL_TIMEOUT,
  ENROLL_BAD_IMAGE,
  ENROLL_MISMATCH,       // the two scans didn't match each other
  ENROLL_DUPLICATE,      // this finger is already enrolled elsewhere
  ENROLL_STORE_FAILED,
  ENROLL_NO_FREE_SLOT
};

// Human-readable code for telemetry/logging, e.g. "duplicate_finger" --
// matches the errorReason values the backend expects.
const char* enrollResultToString(EnrollResult result);

// Asks the sensor directly (not a locally tracked list) for the lowest
// unused slot ID, 1-indexed. Returns 0 if the sensor is full.
int findNextFreeSlot();

// Runs the full two-scan enrollment into `slot`. Blocking, with a
// per-step timeout so it can never hang forever. Checks for duplicates
// against the existing database before storing.
EnrollResult enrollFingerprint(int slot, EnrollPromptCallback onPrompt);

// Deletes the template at `slot`. Returns true on success.
bool deleteFingerprint(int slot);

#endif