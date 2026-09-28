#pragma once
#include <Arduino.h>

// Freenove FNK0104N: ST77922 over QSPI + capacitive touch over I2C.
// Pin map and init sequence taken from Freenove's reference driver.
namespace panel {

constexpr int WIDTH = 480;   // logical (landscape)
constexpr int HEIGHT = 320;

bool begin();
void setFlipped(bool flipped);   // rotate 180 degrees
void setBrightness(uint8_t level);  // 0 = off, 255 = full

// Push a full 480x320 frame of byte-swapped RGB565 (LovyanGFX sprite layout).
void pushFrame(const uint16_t* pixels);

struct TouchPoint {
    bool down;
    int16_t x;
    int16_t y;
};
TouchPoint readTouch();
void touchDiag();
void touchRawDump();  // print raw touch registers when they change  // print I2C scan + raw touch registers to Serial

}  // namespace panel
