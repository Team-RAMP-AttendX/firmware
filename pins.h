#include <cstdint>
#ifndef PINS_H
#define PINS_H


// ==========================================
// I2C DISPLAY (16x2 LCD)
// ==========================================
#define LCD_SDA_PIN      38
#define LCD_SCL_PIN      3

#define FINGERPRINT_RX        47 
#define FINGERPRINT_TX        48 

// ==========================================
// 4x4 KEYPAD
// ==========================================
const int ROW_NUM = 4;
const int COLUMN_NUM = 4;

const uint8_t KEYPAD_PIN_ROWS[ROW_NUM] = {39, 40, 41, 42}; 
const uint8_t KEYPAD_PIN_COLS[COLUMN_NUM] = {1, 2, 14, 21}; 

// ==========================================
// OV2640 CAMERA PINS (ESP32-S3 WROOM)
// ==========================================
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    15
#define SIOD_GPIO_NUM    4  
#define SIOC_GPIO_NUM    5  

#define Y9_GPIO_NUM      16
#define Y8_GPIO_NUM      17
#define Y7_GPIO_NUM      18
#define Y6_GPIO_NUM      12
#define Y5_GPIO_NUM      10
#define Y4_GPIO_NUM      8
#define Y3_GPIO_NUM      9
#define Y2_GPIO_NUM      11
#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM    13

#endif 