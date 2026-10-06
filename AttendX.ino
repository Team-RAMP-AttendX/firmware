#include <WiFi.h>
#include "pins.h"
#include "ui.h"
#include "fingerprint.h"
#include "api_client.h"
#include "secrets.h"
#include "esp_camera.h"
#include <time.h>

static bool fingerprintOK = false;
static bool cameraOK = false;

// --- wraps updateDisplay so the network task always knows what's on screen ---
// Only reports the top two lines to telemetry -- that's the contract the
// backend already expects; rows 3/4 (idle clock) are purely local.
static void display(const char* l1, const char* l2, const char* l3 = "", const char* l4 = "") {
  updateDisplay(l1, l2, l3, l4);
  reportLcdText(l1, l2);
}

// ---------------- idle screen (v3: shows date/time now that the LCD is 20x4) ----------------

static void formatIdleClock(char* dateBuf, size_t dateLen, char* timeBuf, size_t timeLen) {
  if (!isTimeSynced()) {
    snprintf(dateBuf, dateLen, "%s", "");
    snprintf(timeBuf, timeLen, "%s", "--:--");
    return;
  }
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  strftime(dateBuf, dateLen, "%a %b %d", &timeinfo);   // e.g. "Mon Sep 29"
  strftime(timeBuf, timeLen, "%I:%M %p", &timeinfo);   // 12-hour, e.g. "02:47 PM"
}

// True while the idle screen is the thing currently on the LCD --
// tracks whether the NEXT call needs a full redraw (just arrived at
// idle, title/status text needs (re)writing) or just a clock tick
// (already idle, only the time changed).
static bool onIdleScreen = false;
static char lastShownTime[9] = "";

static void showIdleScreen(bool forceFullRedraw = false) {
  char dateBuf[21], timeBuf[21];
  formatIdleClock(dateBuf, sizeof(dateBuf), timeBuf, sizeof(timeBuf));

  if (forceFullRedraw || !onIdleScreen) {
    // Entering idle, or first draw -- this is the only case that
    // touches lcd.clear() (via display()/updateDisplay()).
    display("AttendX Ready", "Scan or Press #", dateBuf, timeBuf);
    onIdleScreen = true;
  } else if (strcmp(timeBuf, lastShownTime) != 0) {
    // Already showing idle, just the clock advanced -- rewrite only
    // that row, no clear(), so there's nothing to flicker.
    updateDisplayRow(3, timeBuf);
  }
  strncpy(lastShownTime, timeBuf, sizeof(lastShownTime) - 1);
}

// Call this from anywhere that's about to put something else on the
// LCD (admin menu, a scan result, etc.) so the next return to idle
// does a full redraw instead of assuming the title/status is still there.
static void leavingIdleScreen() {
  onIdleScreen = false;
}

// ---------------- camera ----------------

static bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    // fb_count = 1, not 2 -- we take single, infrequent shots here, not
    // continuous video. With 2 buffers the sensor's free-running capture
    // can queue up a second frame before you ever call fb_get(), and
    // your NEXT call gets handed that stale leftover instead of a fresh
    // frame -- symptom: photo N looks like a slightly-shifted duplicate
    // of photo N-1. fb_count = 1 removes the backlog entirely.
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  return esp_camera_init(&config) == ESP_OK;
}

// Captures one JPEG into a heap buffer the caller owns (must free() it,
// or hand it to queueManualCheckinWithPhoto, which frees it for you).
// Returns nullptr on failure.
static uint8_t* capturePhoto(size_t* outLen) {
  // Discard one frame before the real capture -- even with fb_count == 1,
  // a frame that was sitting mid-capture when the sensor was last idle
  // can still come back slightly stale. This grab-and-return is cheap
  // insurance, not a fix for the fb_count=2 bug above (that's fixed at
  // the source) -- belt and suspenders for single-shot reliability.
  camera_fb_t* warm = esp_camera_fb_get();
  if (warm) esp_camera_fb_return(warm);

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return nullptr;

  uint8_t* copy = (uint8_t*)malloc(fb->len);
  if (copy) {
    memcpy(copy, fb->buf, fb->len);
    *outLen = fb->len;
  }
  esp_camera_fb_return(fb); // must return the driver's buffer, never free() it directly
  return copy;
}

// Note: PWDN_GPIO_NUM is -1 in pins.h (not wired), so real hardware
// power-down isn't possible on this board -- device is mains-powered
// anyway (powerStatus: "AC" in telemetry), so no sleepCamera()/wakeCamera().

// ---------------- fingerprint init warning ----------------

static void onFingerprintRetry(int attempt, int max) {
  char line2[21];
  snprintf(line2, sizeof(line2), "Retry %d/%d", attempt, max);
  display("Sensor Error", line2);
}

// ---------------- pre-enroll user verification (v3, item 3) ----------------
// Shared by both the terminal-initiated (admin menu) and dashboard-initiated
// (handlePendingCommand) enroll paths, so there's exactly one place that
// decides whether it's safe to start a fingerprint capture.
// PreEnrollCheck itself is declared in api_client.h, not here -- see the
// comment there for why (Arduino auto-prototype ordering).

static PreEnrollCheck checkUserBeforeEnroll(const String& userId) {
  display("Checking ID...", userId.c_str());

  UserStatusResult result;
  bool gotAnswer = queryUserStatus(userId, result, 5000);

  // Both "no answer in time" and "answer arrived but says it couldn't
  // verify" mean the same thing here: never guess, refuse to enroll.
  if (!gotAnswer || !result.requestSucceeded) return PRECHECK_VERIFY_FAILED;
  if (!result.exists) return PRECHECK_NOT_FOUND;
  if (result.hasFingerprint) return PRECHECK_ALREADY_ENROLLED;
  return PRECHECK_OK;
}

// Turns a failed precheck into the matching LCD message + errorReason
// string, so both call sites report identically.
static const char* precheckFailureLine1(PreEnrollCheck check) {
  switch (check) {
    case PRECHECK_NOT_FOUND:        return "User Not Found";
    case PRECHECK_ALREADY_ENROLLED: return "Already Enrolled";
    default:                        return "Cannot Verify";
  }
}

static const char* precheckFailureReason(PreEnrollCheck check) {
  switch (check) {
    case PRECHECK_NOT_FOUND:        return "user_not_found";
    case PRECHECK_ALREADY_ENROLLED: return "already_enrolled";
    default:                        return "verify_failed";
  }
}

// ---------------- admin menu ----------------

static void runAdminMenu() {
  leavingIdleScreen();
  String pin = getMaskedPIN("Admin PIN:");
  if (pin != ADMIN_PIN) {
    display("Wrong PIN", "");
    delay(1500);
    return;
  }

  display("A:Enroll B:Delete", "C:Exit");
  char choice = 0;
  unsigned long start = millis();
  while (millis() - start < 10000) { // 10s to pick, then auto-exit
    choice = getKeypress();
    if (choice) break;
    delay(10);
  }

  if (choice == 'A') {
    if (!isFingerprintAvailable()) {
      display("Sensor Offline", "Cannot Enroll");
      delay(1500);
      return;
    }

    display("Enter User ID:", "e.g. 123A");
    String userId = getManualID();
    if (userId.length() == 0) {
      display("Cancelled", "");
      delay(1000);
      return;
    }

    // v3 item 3: verify the ID is real and unenrolled BEFORE the human
    // places a finger at all -- never scan against an unverified ID.
    PreEnrollCheck check = checkUserBeforeEnroll(userId);
    if (check != PRECHECK_OK) {
      display(precheckFailureLine1(check), "");
      delay(1500);
      queueTerminalInitiatedResult("ENROLL_FINGERPRINT", userId, 0, false, precheckFailureReason(check));
      return;
    }

    int slot = findNextFreeSlot();
    if (slot == 0) {
      display("Enroll Failed", "Sensor Full");
      delay(1500);
      queueTerminalInitiatedResult("ENROLL_FINGERPRINT", userId, 0, false, "slot_full");
      return;
    }

    EnrollResult result = enrollFingerprint(slot, [](const char* l1, const char* l2) {
      display(l1, l2);
    });

    if (result == ENROLL_OK) {
      display("Enrolled", ("Slot " + String(slot)).c_str());
      queueTerminalInitiatedResult("ENROLL_FINGERPRINT", userId, slot, true, nullptr);
    } else {
      display("Enroll Failed", enrollResultToString(result));
      queueTerminalInitiatedResult("ENROLL_FINGERPRINT", userId, 0, false, enrollResultToString(result));
    }
    delay(2000);

  } else if (choice == 'B') {
    display("Enter Slot #:", "");
    String slotStr = getManualID();
    int slot = slotStr.toInt();
    if (slot < 1 || slot > 126) {
      display("Invalid Slot", "");
      delay(1500);
      return;
    }

    bool ok = deleteFingerprint(slot);
    display(ok ? "Deleted" : "Delete Failed", ("Slot " + String(slot)).c_str());
    queueTerminalInitiatedResult("DELETE_FINGERPRINT", "", slot, ok, ok ? nullptr : "slot_not_found");
    delay(2000);
  }
  // 'C' or timeout: just fall through and return to idle
}

// ---------------- dashboard command execution ----------------
// Runs on the UI task -- this is the one place fingerprint.h functions
// get called in response to something the network task noticed.

static void handlePendingCommand() {
  IncomingCommand cmd;
  if (!pollIncomingCommand(cmd)) return;
  leavingIdleScreen();

  if (cmd.type == CMD_ENROLL_FINGERPRINT) {
    if (!isFingerprintAvailable()) {
      display("Enroll Failed", "Sensor Offline");
      delay(1500);
      queueCommandResult(cmd, false, "sensor_offline", 0);
      showIdleScreen();
      clearInProgressCommand();
      return;
    }

    // Same v3 pre-check as the terminal-initiated path -- if the
    // dashboard queued a command for a userId that's since been
    // deleted, already enrolled, or never existed, catch it before
    // making someone place their finger for nothing.
    String userId = String(cmd.userId);
    PreEnrollCheck check = checkUserBeforeEnroll(userId);
    if (check != PRECHECK_OK) {
      display(precheckFailureLine1(check), "");
      delay(1500);
      queueCommandResult(cmd, false, precheckFailureReason(check), 0);
      showIdleScreen();
      clearInProgressCommand();
      return;
    }

    int slot = findNextFreeSlot();
    if (slot == 0) {
      display("Enroll Failed", "Sensor Full");
      delay(1500);
      queueCommandResult(cmd, false, "slot_full", 0);
      showIdleScreen();
      clearInProgressCommand();
      return;
    }
    EnrollResult result = enrollFingerprint(slot, [](const char* l1, const char* l2) {
      display(l1, l2);
    });
    // This confirmation was missing entirely before -- the function
    // went straight from enrollFingerprint()'s last prompt callback to
    // showIdleScreen() with nothing shown in between, so a successful
    // dashboard-triggered enroll looked, from the terminal, exactly
    // like nothing had happened at all.
    if (result == ENROLL_OK) {
      display("Enrolled", ("Slot " + String(slot)).c_str());
      queueCommandResult(cmd, true, nullptr, slot);
    } else {
      display("Enroll Failed", enrollResultToString(result));
      queueCommandResult(cmd, false, enrollResultToString(result), 0);
    }
    delay(2000);
    showIdleScreen();

  } else if (cmd.type == CMD_DELETE_FINGERPRINT) {
    bool ok = deleteFingerprint(cmd.slotNumber);
    display(ok ? "Deleted" : "Delete Failed", ("Slot " + String(cmd.slotNumber)).c_str());
    queueCommandResult(cmd, ok, ok ? nullptr : "slot_not_found", cmd.slotNumber);
    delay(1500);
    showIdleScreen();
  }
  clearInProgressCommand();
}

// ---------------- setup / loop ----------------

void setup() {
  Serial.begin(115200);
  initUI();
  display("Booting...", "");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  display("Connecting", "WiFi...");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(200);
  }

  fingerprintOK = initFingerprint(onFingerprintRetry);
  cameraOK = initCamera();

  initApiClient(); // starts the network task on core 0 -- this is also
                    // what kicks off the bounded NTP sync (setupTime())
                    // for the idle-screen clock

  if (!fingerprintOK) {
    display("Keypad Mode", "Press # for ID");
    delay(2000);
  }
  showIdleScreen();
}

void loop() {
  char key = getKeypress(); // read once per iteration, use everywhere below
  bool handledSomething = false;

  if (key == 'A') {
    runAdminMenu();
    handledSomething = true;
  }

  handlePendingCommand();

  if (fingerprintOK) {
    int slot = checkFingerprint();
    if (slot > 0) {
      leavingIdleScreen();
      display("Welcome", ("Slot " + String(slot)).c_str());
      queueFingerprintCheckin(slot);
      delay(2000);
      handledSomething = true;
    } else if (slot == -1) {
      leavingIdleScreen();
      display("Read Failed", "Try Again");
      Serial.println("Fingerprint sensor read error");
      delay(1000);
      handledSomething = true;
    } else if (slot == -2) {
      // A finger WAS placed and read cleanly, just matched nothing --
      // distinct from slot == 0 (no finger present at all, the normal
      // idle state), which deliberately does nothing here.
      leavingIdleScreen();
      display("Finger Not", "Registered");
      delay(1500);
      handledSomething = true;
    }
    // slot == 0: no finger present, nothing to do -- falls through to
    // the idle-screen refresh below.
  }

  if (key == '#') {
    leavingIdleScreen();
    String userId = getManualID();
    if (userId.length() > 0) {
      if (cameraOK) {
        display("Hold Still", "Taking Photo");
        delay(500); // gives the LCD message time to actually be visible
                    // before the near-instant capture below -- otherwise
                    // this flashes by unseen
        size_t photoLen;
        uint8_t* photo = capturePhoto(&photoLen);
        if (photo) {
          display("Check-In OK", "Photo Logged");
          queueManualCheckinWithPhoto(userId, photo, photoLen);
        } else {
          display("Camera Error", "Try Again");
        }
      } else {
        display("Camera Offline", "Cannot Log");
      }
      delay(2000);
    }
    handledSomething = true;
  }

  // Idle-screen refresh: only when nothing else happened this
  // iteration, and only every 1s -- keeps the clock ticking without
  // hammering the I2C bus on every single loop() pass.
  static unsigned long lastIdleRefresh = 0;
  if (!handledSomething && millis() - lastIdleRefresh > 1000) {
    showIdleScreen();
    lastIdleRefresh = millis();
  } else if (handledSomething) {
    lastIdleRefresh = 0; // force an immediate refresh next idle iteration
  }

  delay(10);
}