#include <WiFi.h>
#include "pins.h"
#include "ui.h"
#include "fingerprint.h"
#include "api_client.h"
#include "secrets.h"
#include "esp_camera.h"

static bool fingerprintOK = false;

// --- wraps updateDisplay so the network task always knows what's on screen ---
static void display(const char* l1, const char* l2) {
  updateDisplay(l1, l2);
  reportLcdText(l1, l2);
}

// ---------------- camera ----------------

static bool cameraOK = false;

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
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init FAILED, error 0x%x\n", err);
    return false;
  }
  Serial.println("Camera init OK");
  return true;
}

// Captures one JPEG into a heap buffer the caller owns (must free() it,
// or hand it to queueManualCheckinWithPhoto, which frees it for you).
// Returns nullptr on failure.
static uint8_t* capturePhoto(size_t* outLen) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture FAILED: esp_camera_fb_get() returned null");
    return nullptr;
  }

  Serial.printf("Captured frame: %u bytes, %ux%u\n", fb->len, fb->width, fb->height);
  if (fb->len > 2 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8) {
    Serial.println("  -> looks like a valid JPEG (FF D8 header present)");
  } else {
    Serial.println("  -> WARNING: does not look like a valid JPEG");
  }

  uint8_t* copy = (uint8_t*)malloc(fb->len);
  if (copy) {
    memcpy(copy, fb->buf, fb->len);
    *outLen = fb->len;
  } else {
    Serial.println("  -> malloc FAILED, out of heap");
  }
  esp_camera_fb_return(fb);
  return copy;
}

// Note: PWDN_GPIO_NUM is -1 in pins.h (not wired), so real hardware
// power-down isn't possible on this board -- that's why there's no
// sleepCamera()/wakeCamera() here. Given the terminal also reports
// powerStatus: "AC" (mains-powered, not battery), the power saving
// wasn't buying anything real; dropping it is the honest fix rather
// than keeping dead functions around.

// ---------------- fingerprint init warning ----------------

static void onFingerprintRetry(int attempt, int max) {
  char line2[17];
  snprintf(line2, sizeof(line2), "Retry %d/%d", attempt, max);
  display("Sensor Error", line2);
}

// ---------------- admin menu ----------------

static void runAdminMenu() {
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
    int slot = findNextFreeSlot();
    if (slot == 0) {
      display("Enroll Failed", "Sensor Full");
      delay(1500);
      queueTerminalInitiatedResult("ENROLL_FINGERPRINT", "", 0, false, "slot_full");
      return;
    }

    String userId = getManualID();
    if (userId.length() == 0) {
      display("Cancelled", "");
      delay(1000);
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

  if (cmd.type == CMD_ENROLL_FINGERPRINT) {
    if (!isFingerprintAvailable()) {
      queueCommandResult(cmd, false, "sensor_offline", 0);
      return;
    }
    int slot = findNextFreeSlot();
    if (slot == 0) {
      queueCommandResult(cmd, false, "slot_full", 0);
      return;
    }
    EnrollResult result = enrollFingerprint(slot, [](const char* l1, const char* l2) {
      display(l1, l2);
    });
    if (result == ENROLL_OK) {
      queueCommandResult(cmd, true, nullptr, slot);
    } else {
      queueCommandResult(cmd, false, enrollResultToString(result), 0);
    }
    display("Ready", "Scan Finger");

  } else if (cmd.type == CMD_DELETE_FINGERPRINT) {
    bool ok = deleteFingerprint(cmd.slotNumber);
    queueCommandResult(cmd, ok, ok ? nullptr : "slot_not_found", cmd.slotNumber);
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

  initApiClient(); // starts the network task on core 0

  if (!fingerprintOK) {
    display("Keypad Mode", "Press # for ID");
    delay(2000);
  }
  display("Ready", "Scan Finger");
}

void loop() {
  char key = getKeypress();
  if (key == 'A') {
    runAdminMenu();
    display("Ready", "Scan Finger");
  }

  handlePendingCommand();

  if (fingerprintOK) {
    int slot = checkFingerprint();
    if (slot > 0) {
      display("Welcome", ("Employee " + String(slot)).c_str());
      queueFingerprintCheckin(slot);
      delay(2000);
      display("Ready", "Scan Finger");
    } else if (slot == -1) {
      Serial.println("Fingerprint sensor read error");
    }
  }

  if (key == '#') {
    String userId = getManualID();
    if (userId.length() > 0) {
      if (cameraOK) {
        display("Hold Still", "Taking Photo");
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
    display("Ready", "Scan Finger");
  }

  delay(10);
}