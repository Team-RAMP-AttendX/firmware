#include "pins.h"
#include "ui.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {39, 40, 41, 42};
byte colPins[COLS] = {1, 2, 14, 21};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

static const unsigned long ENTRY_IDLE_TIMEOUT_MS = 15000;

void initUI() {
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  lcd.init();
  lcd.backlight();
}

void updateDisplay(const char* row1, const char* row2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(row1);
  lcd.setCursor(0, 1);
  lcd.print(row2);
}

char getKeypress() {
  return keypad.getKey();
}

// Shared entry loop backing both getManualID and getMaskedPIN.
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