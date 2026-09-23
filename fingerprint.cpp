#include <cstdint>
#include "pins.h"
#include "fingerprint.h"
#include <Adafruit_Fingerprint.h>

// Use Hardware Serial 1 so we don't conflict with USB Serial debugging
HardwareSerial fingerSerial(1);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

bool initFingerprint() {
  //DY50 communicates at 57600 baud by default
  fingerSerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  finger.begin(57600);
  
  if (finger.verifyPassword()) {
    return true;
  } else {
    return false;
  }
}

int checkFingerprint() {
  uint8_t p = finger.getImage();
  
  // Non-blocking exit: If nobody is touching the sensor, return 0 immediately
  if (p == FINGERPRINT_NOFINGER) return 0;
  
  // If there's an image error or dirty sensor, return 0
  if (p != FINGERPRINT_OK) return 0; 

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return 0;

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    // Found a match! Return the slot ID (e.g., 1, 2, 3) to the main sketch
    return finger.fingerID;
  } 
  
  // Finger was read but didn't match any enrolled slots
  return 0; 
}