#ifndef UI_H
#define UI_H

#include <Arduino.h>

void initUI();

// row3/row4 default to "" so every existing 2-line call site keeps
// compiling unchanged. Passing all four is how the idle screen shows
// a title, status, and date/time on the 20x4 display.
void updateDisplay(const char* row1, const char* row2, const char* row3 = "", const char* row4 = "");

// Overwrites a single row (0-3) in place, padded to the full 20 columns
// so leftover characters from whatever was there before get blanked --
// without calling lcd.clear(), which visibly flashes the whole screen.
// For a value that changes on a timer (the idle clock) on a screen
// that's otherwise static, this avoids a per-tick full-screen flicker.
void updateDisplayRow(int row, const char* text);

char getKeypress();

// Blocking loop that collects characters. '*' submits (possibly empty).
// '#' clears the current input if there's text; if input is already
// empty, '#' cancels (returns ""). Also auto-cancels (returns "")
// after 15s of no keypress, so the terminal can't get stuck here.
//
// Accepts digits and A-D as literal characters -- this is unchanged
// from before and already covers entering a NNNL-format userId
// (3 digits + A-D), since only '*' and '#' are treated specially here.
String getManualID();

// Same interaction pattern, but echoes '*' for each typed character
// instead of the character itself -- for entering an admin PIN where
// someone might be watching over your shoulder.
String getMaskedPIN(const char* prompt);

#endif