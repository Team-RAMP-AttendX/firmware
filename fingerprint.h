#ifndef FINGERPRINT_H
#define FINGERPRINT_H

#include <Arduino.h>

// Starts the UART serial connection and checks if the sensor is responding
bool initFingerprint();

// Non-blocking poll. Returns the matched slot number (e.g., 1, 2, 3) or 0 if no match
int checkFingerprint();

#endif