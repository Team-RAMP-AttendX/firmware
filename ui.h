#ifndef UI_H
#define UI_H

#include <Arduino.h>

// Initializes the I2C bus, LCD, and sets up the keypad matrix
void initUI();

// Clears the screen and prints new text to the top (row1) and bottom (row2)
void updateDisplay(const char* row1, const char* row2);

// Non-blocking check for a key press (returns '\0' if nothing is pressed)
char getKeypress();

// Blocking loop that collects keystrokes for a manual ID until '*' is pressed
String getManualID();

#endif