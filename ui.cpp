#include "pins.h"
#include "ui.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>

// 20x4, up from 16x2.
LiquidCrystal_I2C lcd(0x27, 20, 4);

char keys[ROW_NUM][COLUMN_NUM] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// Pulled directly from pins.h rather than duplicated locally -- pins.h
// is now genuinely the single source of truth for these. const_cast is
// safe here specifically because Keypad only ever reads these pins to
// scan the matrix, never writes to the array itself.
Keypad keypad = Keypad(makeKeymap(keys),
                        const_cast<byte*>(KEYPAD_PIN_ROWS),
                        const_cast<byte*>(KEYPAD_PIN_COLS),
                        ROW_NUM, COLUMN_NUM);

static const unsigned long ENTRY_IDLE_TIMEOUT_MS = 15000;

void initUI() {
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  lcd.init();
  lcd.backlight();
}

void updateDisplay(const char* row1, const char* row2, const char* row3, const char* row4) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(row1);
  lcd.setCursor(0, 1);
  lcd.print(row2);
  lcd.setCursor(0, 2);
  lcd.print(row3);
  lcd.setCursor(0, 3);
  lcd.print(row4);
}

char getKeypress() {
  return keypad.getKey();
}

void updateDisplayRow(int row, const char* text) {
  char padded[21];
  size_t len = strlen(text);
  if (len > 20) len = 20;
  memcpy(padded, text, len);
  for (size_t i = len; i < 20; i++) padded[i] = ' ';
  padded[20] = '\0';
  lcd.setCursor(0, row);
  lcd.print(padded); // no lcd.clear() -- that's the whole point here
}

// Shared entry loop backing both getManualID and getMaskedPIN. Only
// '*' and '#' are special-cased -- everything else, including 'A'-'D',
// is appended as a literal character, so NNNL-format userIds (3 digits
// + a letter) already work here without any change.
static String collectInput(const char* prompt, bool masked) {
  String input = "";
  updateDisplay(prompt, "");
  unsigned long lastKeyTime = millis();

  while (true) {
    char key = getKeypress();

    if (key) {
      lastKeyTime = millis();

      if (key == '*') {
        return input;
      } else if (key == '#') {
        if (input.length() > 0) {
          input = "";
          updateDisplay(prompt, "");
        } else {
          return ""; // empty + '#' = cancel
        }
      } else {
        input += key;
        if (masked) {
          String stars = "";
          for (unsigned int i = 0; i < input.length(); i++) stars += '*';
          updateDisplay(prompt, stars.c_str());
        } else {
          updateDisplay(prompt, input.c_str());
        }
      }
    } else if (millis() - lastKeyTime > ENTRY_IDLE_TIMEOUT_MS) {
      return ""; // nobody's there
    }

    delay(10);
  }
}

String getManualID() {
  return collectInput("Enter ID:", false);
}

String getMaskedPIN(const char* prompt) {
  return collectInput(prompt, true);
}