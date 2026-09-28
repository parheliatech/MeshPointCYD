#pragma once
#include <LovyanGFX.hpp>

#include "panel.h"

namespace ui {

void begin(LGFX_Sprite* canvas);
// Feed the current touch state; call every loop.
void handleTouch(const panel::TouchPoint& tp);
// Redraw if anything changed (or once a second for clocks/ages). Returns true if a frame was drawn.
bool render(bool force = false);

// Full-screen message (boot, WiFi portal instructions, etc.).
void drawSplash(const char* title, const char* line1, const char* line2 = nullptr,
                const char* line3 = nullptr);

}  // namespace ui
