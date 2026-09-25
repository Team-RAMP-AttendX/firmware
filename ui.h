#ifndef UI_H
#define UI_H

#include <Arduino.h>

void initUI();
void updateDisplay(const char* row1, const char* row2);
char getKeypress();
String getManualID();
String getMaskedPIN(const char* prompt);

#endif