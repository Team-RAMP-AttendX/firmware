#include "pins.h"
#include "ui.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>

// --- LCD Configuration ---

LiquidCrystal_I2C lcd(0x27, 16, 2); 

// --- Keypad Configuration ---
const byte ROWS = 4; 
const byte COLS = 4; 
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// The exact ESP32-S3 pins we verified during testing
byte rowPins[ROWS] = {39, 40, 41, 42}; 
byte colPins[COLS] = {1, 2, 14, 21}; 

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

void initUI() {
  // Start I2C specifically on pins 4 and 5
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

String getManualID() {
  String inputID = "";
  updateDisplay("Enter ID:", "");
  
  while (true) {
    char key = getKeypress();
    
    if (key) {
      if (key == '*') {
        // Submit the accumulated string
        return inputID; 
      } else if (key == '#') {
        // Clear input and start over
        inputID = "";
        updateDisplay("Enter ID:", "");
      } else {
        // Append the new character and update the screen
        inputID += key;
        updateDisplay("Enter ID:", inputID.c_str());
      }
    }
    delay(10); // 10ms delay keeps the loop stable and prevents keypad bouncing
  }
}