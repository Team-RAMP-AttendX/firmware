#include <cstdint>
#include "pins.h"
#include "fingerprint.h"
#include "sfm.hpp" // SFM-V1.7 library (Arduino Library Manager: "SFM-V1.7")

// Sensor is an SFM-V1.7 capacitive module -- a completely different
// protocol from the AS608 family (the old DY50), so this file no longer
// touches Adafruit_Fingerprint at all. Differences that shape the code
// below:
//   - UART is fixed at 115200 (the library opens it in its constructor).
//   - Finger presence comes from a capacitive TOUCH_OUT pin, not from
//     polling getImage() -- identification is a single blocking
//     recognition_1vN() call, made only on a fresh touch.
//   - Enrollment is THREE placements (the protocol's "3C3R") and the
//     module assigns the user ID itself; there is no per-slot
//     loadModel() probe, so findNextFreeSlot() is gone in favor of
//     isSensorFull() + module-assigned IDs.
//
// User IDs are 1-10000 per the protocol doc; the module enforces its
// own storage limit with ACK_FULL. The backend still calls this value
// "slotNumber", and slot 0 stays reserved as the "no match" sentinel
// (the module never assigns it), so checkFingerprint() > 0 remains a
// valid match.

static const int SFM_MAX_USERS = 500;
// The datasheet's text layer doesn't state the template capacity; 500
// is the common figure for this module family. This constant only
// feeds the early isSensorFull() check -- the sensor itself is the
// real gate (ACK_FULL at enrollment maps to ENROLL_NO_FREE_SLOT). If
// the module proves to hold more, raise this or rely on ACK_FULL.

static const unsigned long ENROLL_STEP_TIMEOUT_MS = 10000; // per finger removal

static SFM_Module* finger = nullptr;
static bool sensorAvailable = false;
static const int MAX_INIT_ATTEMPTS = 3;
static const unsigned long RETRY_DELAY_MS = 3000;

// The library tracks touch state through a CHANGE interrupt on the
// TOUCH_OUT pin -- without this attached, isTouched() never updates.
static void IRAM_ATTR fingerTouchIsr() {
  if (finger) finger->pinInterrupt();
}

bool initFingerprint(WarningCallback onRetryWarning) {
  for (int attempt = 1; attempt <= MAX_INIT_ATTEMPTS; attempt++) {
    if (!finger) {
      // Constructed lazily (not as a global) so the UART opens after
      // the core is up. The constructor configures TOUCH_OUT as
      // INPUT_PULLDOWN, drives the VCC pin HIGH (dummy -- see pins.h),
      // and begins serial at 115200 8N1 on SFM_UART_INDEX.
      finger = new SFM_Module(FINGERPRINT_VCC, FINGERPRINT_TOUCH,
                              FINGERPRINT_RX, FINGERPRINT_TX, 1);
      finger->setPinInterrupt(fingerTouchIsr);
    }

    // isConnected() exchanges a UUID packet -- the real handshake,
    // replacing Adafruit's verifyPassword(). Slow failure path (the
    // library's serial timeout is 8s) but bounded by MAX_INIT_ATTEMPTS.
    if (finger->isConnected()) {
      sensorAvailable = true;
      return true;
    }

    if (onRetryWarning) onRetryWarning(attempt, MAX_INIT_ATTEMPTS);

    if (attempt < MAX_INIT_ATTEMPTS) {
      delay(RETRY_DELAY_MS);
    }
  }

  sensorAvailable = false;
  return false;
}

bool isFingerprintAvailable() {
  return sensorAvailable;
}

int checkFingerprint() {
  if (!finger) return -1;

  // One press = one identification: a rising edge on the touch pad
  // triggers recognition_1vN(); keeping the finger down does nothing
  // until it's lifted again. Same "no spam" behavior the old
  // getImage() polling had, minus the busy-polling.
  static bool wasTouched = false;
  bool touched = finger->isTouched();

  if (!touched) {
    wasTouched = false;
    return 0;
  }
  if (wasTouched) return 0; // same press as the last poll, already handled
  wasTouched = true;

  uint16_t uid = 0;
  uint8_t p = finger->recognition_1vN(uid);

  // Per the protocol doc: a clean read that matches nothing returns
  // ACK_SUCCESS with ID 00 00 -- that's our -2. Everything else
  // (image-collection timeout on a graze, comms trouble, hardware
  // error) is a retryable -1.
  if (p == SFM_ACK_SUCCESS) {
    return uid > 0 ? (int)uid : -2;
  }
  return -1;
}

const char* enrollResultToString(EnrollResult result) {
  switch (result) {
    case ENROLL_OK:            return "success";
    case ENROLL_TIMEOUT:       return "sensor_timeout";
    case ENROLL_BAD_IMAGE:     return "bad_image";
    case ENROLL_MISMATCH:      return "enroll_mismatch";
    case ENROLL_DUPLICATE:     return "duplicate_finger";
    case ENROLL_STORE_FAILED:  return "store_error";
    case ENROLL_NO_FREE_SLOT:  return "slot_full";
    default:                   return "unknown_error";
  }
}

bool isSensorFull() {
  if (!finger) return true;
  return finger->getUserCount() >= SFM_MAX_USERS;
}

int getSensorCapacity() {
  return SFM_MAX_USERS;
}

int getSensorUserCount() {
  if (!finger) return -1;
  int n = (int)finger->getUserCount();
  if (n < 0 || n > 10000) return -1;   // implausible value: treat the query as failed
  return n;
}

// Best-effort wait for the user to lift off after each of the first
// two scans. Bounded, and proceeds anyway on timeout -- same lenient
// behavior the AS608 flow had (a still-pressed finger just fails the
// next step as a mismatch).
static void waitUntilUntouched(EnrollPromptCallback onPrompt) {
  if (onPrompt) onPrompt("Remove Finger", "");
  unsigned long start = millis();
  while (finger->isTouched()) {
    if (millis() - start > ENROLL_STEP_TIMEOUT_MS) break;
    delay(50);
  }
  delay(500); // settling gap, mirroring the old remove-finger pause
}

EnrollResult enrollFingerprint(int &assignedUid, EnrollPromptCallback onPrompt) {
  assignedUid = 0;
  if (!finger) return ENROLL_STORE_FAILED;

  // --- Scan 1 of 3 ---
  // Passing uid 0 tells the module to auto-assign an unused ID. The
  // module blocks inside this call until it captures a finger (or
  // gives up with its own image-collection timeout), so no local
  // wait-for-finger loop is needed like the AS608 flow had.
  uint8_t p = finger->register_3c3r_1st(0);
  if (p == SFM_ACK_TIMEOUT) return ENROLL_TIMEOUT;
  if (p == SFM_ACK_FULL) return ENROLL_NO_FREE_SLOT;
  if (p != SFM_ACK_SUCCESS) return ENROLL_BAD_IMAGE;

  waitUntilUntouched(onPrompt);

  // --- Scan 2 of 3 ---
  if (onPrompt) onPrompt("Place Same Finger", "");
  p = finger->register_3c3r_2nd();
  if (p == SFM_ACK_TIMEOUT) return ENROLL_TIMEOUT;
  if (p == SFM_ACK_FULL) return ENROLL_NO_FREE_SLOT;
  if (p != SFM_ACK_SUCCESS) return ENROLL_BAD_IMAGE;

  waitUntilUntouched(onPrompt);

  // --- Scan 3 of 3 -- commits the template and returns the ID ---
  if (onPrompt) onPrompt("Place Same Finger", "");
  uint16_t uid = 0;
  p = finger->register_3c3r_3rd(uid);
  if (p == SFM_ACK_TIMEOUT) return ENROLL_TIMEOUT;
  if (p != SFM_ACK_SUCCESS || uid == 0) return ENROLL_MISMATCH;

  // NOTE: this protocol has no pre-store duplicate search -- the old
  // createModel()+fingerFastSearch() trick has no SFM equivalent, and
  // the template is committed to the database in this third step. If
  // the module itself doesn't reject re-enrolling the same finger
  // under a new ID, duplicate detection now relies on the backend's
  // per-user "already_enrolled" pre-check instead. ENROLL_DUPLICATE
  // stays in the enum purely so the backend's errorReason vocabulary
  // is unchanged.

  assignedUid = (int)uid;

  // Don't return while the finger is still on the pad. checkFingerprint()
  // starts a recognition on every fresh touch, so a finger still resting
  // here when the caller's "Enrolled" screen ends could be read as a
  // brand-new touch and logged as a stray check-in for the user who was
  // just enrolled. Bounded, and a no-op when the finger is already up.
  waitUntilUntouched(onPrompt);
  return ENROLL_OK;
}

bool deleteFingerprint(int uid) {
  if (!finger) return false;
  // SFM_ACK_NOUSER on an empty ID also lands here as false, which the
  // callers already surface as "slot_not_found".
  return finger->deleteUser(uid) == SFM_ACK_SUCCESS;
}
