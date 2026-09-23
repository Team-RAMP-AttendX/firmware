#include "pins.h"
#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"

#include "ui.h"
#include "fingerprint.h"
#include "api_client.h"

const char* ssid = "";
const char* password = "";

unsigned long lastTelemetryTime = 0;
const unsigned long telemetryInterval = 30000; // 30 seconds

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  // Set to SVGA (800x600) to match the backend telemetry contract
  config.frame_size = FRAMESIZE_SVGA; 
  config.jpeg_quality = 20;
  config.grab_mode = CAMERA_GRAB_LATEST;

  // Crucial: Route buffer to PSRAM and use double-buffering to prevent FB-OVF
  if (psramFound()) {
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    Serial.println("PSRAM found. Using double-buffering.");
  } else {
    // If PSRAM isn't enabled in Arduino IDE, SVGA will crash. 
    Serial.println("WARNING: PSRAM not detected! Check IDE Tools menu.");
    config.frame_size = FRAMESIZE_QVGA; 
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  // Attempt initialization with a small delay for stability
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera Init Failed with error 0x%x\n", err);
    updateDisplay("Camera Error", "Continuing w/o Cam");
    delay(2000);
    return; // Don't trap the boot sequence in a freeze!
  }
  Serial.println("Camera Init Success!");
}

void setup() {
  Serial.begin(115200);
  
  // 1. Boot LCD and Keypad
  initUI();
  updateDisplay("Booting System", "Please wait...");
  delay(1000);

  // 2. Boot Fingerprint Scanner
  updateDisplay("Starting Sensor", "Checking UART...");
  if (!initFingerprint()) {
    updateDisplay("Sensor Error", "Halted.");
    while(1);
  }

  // 3. Boot Camera
  updateDisplay("Starting Camera", "OV2640 Init...");
  initCamera();


  // 4. Connect WiFi
  updateDisplay("Connecting WiFi", ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(800);
    Serial.print(".");
  }

  // 5. Sync Time (WAT)
  updateDisplay("Syncing Time", "WAT (+01:00)...");
  setupTime();

  updateDisplay("AttendX Ready", "Scan or Press #");
}

void loop() {
  // --- 1. NON-BLOCKING TELEMETRY ---
  if (millis() - lastTelemetryTime >= telemetryInterval) {
    lastTelemetryTime = millis();
    // Fires in the background without freezing the UI
    sendTelemetry("** ATTENDX TERMINAL **", "Ready for Scan..."); 
  }

  // --- 2. NON-BLOCKING BIOMETRIC SCAN ---
  int slotNumber = checkFingerprint();
  if (slotNumber > 0) {
    updateDisplay("Processing...", "Sending Data");
    
    if (sendFingerprintLog(slotNumber)) {
      updateDisplay("Check-In OK", "Access Granted");
    } else {
      updateDisplay("Network Error", "Try Again");
    }
    
    delay(2000);
    updateDisplay("AttendX Ready", "Scan or Press #");
  }

  // --- 3. KEYPAD FALLBACK ---
  char key = getKeypress();
  if (key == '#') {
    // getManualID() loops locally until '*' is pressed or '#' cancels
    String manualID = getManualID(); 
    
    if (manualID.length() > 0) {
      updateDisplay("Look at Camera", "Capturing...");
      delay(500); // Give user a moment to look up
      
      // Flush the stale frame out of the DMA buffer first
      camera_fb_t *dummy_fb = esp_camera_fb_get();
      if (dummy_fb) esp_camera_fb_return(dummy_fb);

      // Grab the fresh frame
      camera_fb_t *fb = esp_camera_fb_get();
      if (!fb) {
        updateDisplay("Camera Error", "Failed to capture");
      } else {
        updateDisplay("Uploading...", "Please wait");
        
        if (sendManualLogWithPhoto(manualID, fb->buf, fb->len)) {
          updateDisplay("Check-In OK", "Audit Logged");
        } else {
          updateDisplay("Upload Failed", "Try Again");
        }
        
        // CRITICAL: Free PSRAM buffer immediately
        esp_camera_fb_return(fb); 
      }
    } else {
      updateDisplay("Cancelled", "Returning...");
    }
    
    delay(2000);
    updateDisplay("AttendX Ready", "Scan or Press #");
  }
}