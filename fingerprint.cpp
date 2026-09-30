#include <cstdint>
#include "pins.h"
#include "fingerprint.h"
#include <Adafruit_Fingerprint.h>

// Sensor is a DY50 module, AS608-protocol-compatible -- default baud
// 57600. Valid IDs are 0-126 (127 slots total), but slot 0 is
// reserved as our internal "no match" sentinel and is never assigned
// during enrollment -- this also matches the backend's own validation
// (slotNumber >= 1 on all operations).
HardwareSerial fingerSerial(1);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

static bool sensorAvailable = false;
static const int MAX_INIT_ATTEMPTS = 3;
static const unsigned long RETRY_DELAY_MS = 3000;

static const int MAX_SLOTS = 126; // usable range is 1-126; slot 0 reserved

static const unsigned long ENROLL_STEP_TIMEOUT_MS = 10000; // per finger placement

bool initFingerprint(WarningCallback onRetryWarning) {
  fingerSerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);

  for (int attempt = 1; attempt <= MAX_INIT_ATTEMPTS; attempt++) {
    finger.begin(57600);
    if (finger.verifyPassword()) {
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
  uint8_t p = finger.getImage();

  if (p == FINGERPRINT_NOFINGER) return 0;
  if (p != FINGERPRINT_OK) return -1;   // real comms/sensor error

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) return finger.fingerID;

  return -2; // read cleanly, but no match -- see fingerprint.h for why
             // this is distinct from the "no finger present" 0 case
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

int findNextFreeSlot() {
  // Ask the sensor itself rather than tracking a local slot bitmap --
  // this keeps the sensor as the single source of truth, so there's
  // no way for our bookkeeping and the sensor's actual contents to
  // drift out of sync (e.g. after a reflash or a manual sensor reset).
  // Cost: up to MAX_SLOTS UART round-trips, but this only runs during
  // enrollment, never in the polling loop, so it's fine.
  for (int id = 1; id <= MAX_SLOTS; id++) {
    if (finger.loadModel(id) != FINGERPRINT_OK) {
      return id; // nothing stored here
    }
  }
  return 0; // sensor full
}

// Waits for a finger to be placed and imaged, with a timeout.
// Returns FINGERPRINT_OK, FINGERPRINT_NOFINGER (only on timeout), or
// a genuine sensor error code.
static uint8_t waitForImage(EnrollPromptCallback onPrompt, const char* line1) {
  unsigned long start = millis();
  uint8_t p;
  do {
    if (onPrompt) onPrompt(line1, "");
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      if (millis() - start > ENROLL_STEP_TIMEOUT_MS) return FINGERPRINT_NOFINGER;
      delay(50);
      continue;
    }
    return p;
  } while (true);
}

EnrollResult enrollFingerprint(int slot, EnrollPromptCallback onPrompt) {
  if (slot < 1 || slot > MAX_SLOTS) return ENROLL_NO_FREE_SLOT;

  // --- First scan ---
  uint8_t p = waitForImage(onPrompt, "Place Finger");
  if (p == FINGERPRINT_NOFINGER) return ENROLL_TIMEOUT;
  if (p != FINGERPRINT_OK) return ENROLL_BAD_IMAGE;

  if (finger.image2Tz(1) != FINGERPRINT_OK) return ENROLL_BAD_IMAGE;

  if (onPrompt) onPrompt("Remove Finger", "");
  delay(1000);
  unsigned long removeStart = millis();
  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    if (millis() - removeStart > ENROLL_STEP_TIMEOUT_MS) break;
    delay(50);
  }

  // --- Second scan ---
  p = waitForImage(onPrompt, "Place Same Finger");
  if (p == FINGERPRINT_NOFINGER) return ENROLL_TIMEOUT;
  if (p != FINGERPRINT_OK) return ENROLL_BAD_IMAGE;

  if (finger.image2Tz(2) != FINGERPRINT_OK) return ENROLL_BAD_IMAGE;

  if (finger.createModel() != FINGERPRINT_OK) return ENROLL_MISMATCH;

  // Duplicate check -- search the existing database with this freshly
  // created model before writing it anywhere.
  if (finger.fingerFastSearch() == FINGERPRINT_OK) {
    return ENROLL_DUPLICATE;
  }

  if (finger.storeModel(slot) != FINGERPRINT_OK) return ENROLL_STORE_FAILED;

  return ENROLL_OK;
}

bool deleteFingerprint(int slot) {
  return finger.deleteModel(slot) == FINGERPRINT_OK;
}