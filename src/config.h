#pragma once
#include <Arduino.h>

// Persistent settings (NVS). WiFi credentials are stored by the WiFi stack itself.
struct Settings {
    String url = "http://meshpoint.local:8080";  // Meshpoint dashboard base URL
    String user = "viewer";                      // "viewer" (read-only) or "admin"
    String pass;
    String tz = "UTC0";                          // POSIX TZ string for the clock
    bool flip = false;                           // rotate display 180 degrees
    uint8_t brightness = 200;                    // backlight PWM 10..255
    uint8_t volume = 60;                         // alert volume 0..100
    bool muted = false;                          // silence alerts
    bool autoDim = false;                        // dim to 10% after 5 min without touch
};

namespace config {

Settings& get();
void load();
void save();

// Ask for the setup portal on next boot and restart.
[[noreturn]] void rebootIntoPortal();
// True (once) if the previous boot requested the portal.
bool consumePortalRequest();

// Run the WiFiManager captive portal (blocking). Returns true if WiFi connected.
bool runPortal(bool forcePortal);

// Save and apply a POSIX time zone string (e.g. "UTC0", "MST7").
void setTimezone(const String& tz);

// Handle one line of serial input ("help" lists commands).
void handleSerialLine(const String& line);

}  // namespace config
