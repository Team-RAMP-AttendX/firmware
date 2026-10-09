#ifndef FINGERPRINT_H
#define FINGERPRINT_H

#include <Arduino.h>

// Called once per failed init attempt, before the next retry, so the
// caller (AttendX.ino) can update the LCD without fingerprint.cpp
// needing to know anything about ui.h.
typedef void (*WarningCallback)(int attemptNumber, int maxAttempts);

// Called at each step of enrollment so the caller can update the LCD,
// e.g. onPrompt("Place Finger", ""), onPrompt("Remove Finger", "").
// The SFM-V1.7 enrolls with THREE finger placements (and two removals
// between them), so this fires up to five times per enrollment.
typedef void (*EnrollPromptCallback)(const char* line1, const char* line2);

bool initFingerprint(WarningCallback onRetryWarning);
bool isFingerprintAvailable();

// Non-blocking poll driven by the sensor's capacitive touch pad.
//  > 0  -> matched SFM user ID (1-10000; this is the value the backend
//          stores/resolves as "slotNumber")
//    0  -> no NEW finger touch -- the normal idle state, checked every
//          loop iteration. Not an event, nothing to show on the LCD.
//          Also returned while the SAME press is still held down, so
//          one touch = one event.
//   -1  -> sensor communication/read error -- "Read failed, retry"
//   -2  -> a finger WAS read cleanly, but matched nothing --
//          "Finger not registered". Distinct from 0 on purpose: this
//          only fires on an actual touched-and-rejected finger, not on
//          every idle poll where no finger is there at all.
int checkFingerprint();

enum EnrollResult {
  ENROLL_OK,
  ENROLL_TIMEOUT,
  ENROLL_BAD_IMAGE,
  ENROLL_MISMATCH,       // the scans didn't match each other
  ENROLL_DUPLICATE,      // kept for backend error-string compat -- see
                         // note in enrollFingerprint(); the SFM flow
                         // has no pre-store duplicate search
  ENROLL_STORE_FAILED,
  ENROLL_NO_FREE_SLOT
};

// Human-readable code for telemetry/logging, e.g. "duplicate_finger" --
// matches the errorReason values the backend expects.
const char* enrollResultToString(EnrollResult result);

// True when the sensor's user database is at capacity. Only an early
// UX check -- the sensor itself remains the real gate and rejects
// enrollment with ACK_FULL, which maps to ENROLL_NO_FREE_SLOT.
bool isSensorFull();

// Sensor queries for telemetry. UART calls -- UI task only, never from
// the network task. getSensorUserCount() returns -1 if the query fails.
// NOTE: SFM_MAX_USERS (500) is an assumed capacity -- confirm it against
// the SFM-V1.7 datasheet; the dashboard will show whatever is returned here.
int getSensorUserCount();
int getSensorCapacity();

// Runs the full three-scan SFM enrollment. Blocking, with per-step
// timeouts. Unlike the old AS608 flow, the caller does NOT pick the
// ID: the module assigns an unused one itself and returns it via
// assignedUid on ENROLL_OK (0 on any failure).
EnrollResult enrollFingerprint(int &assignedUid, EnrollPromptCallback onPrompt);

// Deletes the template at `uid`. Returns true on success; false also
// covers "no such user" (callers already surface that as
// "slot_not_found").
bool deleteFingerprint(int uid);

#endif