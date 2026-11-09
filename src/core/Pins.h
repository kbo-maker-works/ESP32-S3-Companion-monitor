#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Guition JC3248W535 (ESP32-S3) — pines fijos de la placa.
// Si algo no responde, compáralos con el esquema que da el vendedor:
// hay revisiones de la placa.
// ---------------------------------------------------------------------------

// --- Pantalla AXS15231B por QSPI ---
constexpr int8_t LCD_CS   = 45;
constexpr int8_t LCD_SCK  = 47;
constexpr int8_t LCD_D0   = 21;
constexpr int8_t LCD_D1   = 48;
constexpr int8_t LCD_D2   = 40;
constexpr int8_t LCD_D3   = 39;
constexpr int8_t LCD_BL   = 1;    // backlight (PWM)

// --- Táctil (controlador integrado en el AXS15231B) por I2C ---
constexpr int8_t TP_SDA   = 4;
constexpr int8_t TP_SCL   = 8;
constexpr uint8_t TP_ADDR = 0x3B;

// --- microSD (SPI) — libre para fuentes/imágenes más adelante ---
constexpr int8_t SD_CS    = 10;
constexpr int8_t SD_MOSI  = 11;
constexpr int8_t SD_SCK   = 12;
constexpr int8_t SD_MISO  = 13;

// --- Tamaño lógico (apaisado) ---
constexpr int16_t SCREEN_W = 480;
constexpr int16_t SCREEN_H = 320;
