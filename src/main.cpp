// MeshPoint CYD: a touchscreen dashboard for a Meshpoint (github.com/KMX415/meshpoint)
// running on the Freenove FNK0104N ESP32-S3 3.5" display.

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <esp_wifi.h>

#include "audio.h"
#include "config.h"
#include "meshpoint.h"
#include "panel.h"
#include "ui.h"

static LGFX_Sprite canvas;
static bool touchLog = false;
static bool touchRaw = false;

// Auto-dim: after DIM_AFTER_MS without a touch (when enabled in Settings) the backlight drops to
// 10%. The touch that wakes the screen is swallowed so it can't press anything by accident.
constexpr uint32_t DIM_AFTER_MS = 5 * 60 * 1000;
constexpr uint8_t DIM_LEVEL = 26;  // ~10% of full
static uint32_t lastInteractionMs = 0;
static bool dimmed = false;
static bool swallowTouch = false;

static bool hasSavedWifi() {
    wifi_config_t conf;
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return false;
    return conf.sta.ssid[0] != 0;
}

static void applyDisplaySettings() {
    panel::setFlipped(config::get().flip);
    panel::setBrightness(config::get().brightness);
}

static void noteInteraction() {
    lastInteractionMs = millis();
    if (dimmed) {
        dimmed = false;
        panel::setBrightness(config::get().brightness);
        Serial.println("auto-dim: wake");
    }
}

static void updateAutoDim() {
    const Settings& s = config::get();
    if (!dimmed && s.autoDim && millis() - lastInteractionMs > DIM_AFTER_MS) {
        dimmed = true;
        panel::setBrightness(min(DIM_LEVEL, s.brightness));
        Serial.println("auto-dim: dimmed");
    } else if (dimmed && !s.autoDim) {
        noteInteraction();  // feature switched off while dimmed
    }
}

static void startWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);

    // Hold a finger on the screen during boot to force the setup portal.
    bool held = panel::readTouch().down;
    bool portal = config::consumePortalRequest() || held || !hasSavedWifi();

    if (portal) {
        ui::drawSplash("WiFi setup", "On your phone, join WiFi  'MeshPointCYD-Setup'",
                       "then open  http://192.168.4.1  and enter your WiFi",
                       "and Meshpoint URL / username / password");
        config::runPortal(true);
        if (!WiFi.isConnected()) ESP.restart();  // portal timed out; try again
        return;
    }

    String ssid = WiFi.SSID();
    WiFi.begin();
    ui::drawSplash("MeshPoint CYD", "Connecting to WiFi...", nullptr, config::get().url.c_str());
    uint32_t start = millis();
    while (!WiFi.isConnected() && millis() - start < 20000) delay(100);
    // If it didn't connect, carry on: the dashboard shows the state and WiFi keeps retrying.
}

void setup() {
    Serial.begin(115200);
    config::load();

    if (!panel::begin()) {
        Serial.println("panel init failed");
    }
    canvas.setColorDepth(16);
    canvas.setPsram(true);
    if (!canvas.createSprite(panel::WIDTH, panel::HEIGHT)) {
        Serial.println("framebuffer allocation failed (PSRAM?)");
        delay(5000);
        ESP.restart();
    }
    applyDisplaySettings();
    audio::begin();  // after panel::begin(): shares the touch I2C bus
    ui::begin(&canvas);
    ui::drawSplash("MeshPoint CYD", "Starting...");

    meshpoint::begin();
    startWifi();

    configTzTime(config::get().tz.c_str(), "pool.ntp.org", "time.google.com");
    Serial.println("MeshPoint CYD ready. Type 'help' for commands.");
}

void loop() {
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c != '\n') {
            if (line.length() < 200) line += c;
            continue;
        }
        int x, y, y2;
        if (line == "touchlog") {
            touchLog = !touchLog;
            Serial.printf("touch logging %s\n", touchLog ? "on" : "off");
        } else if (line == "touchraw") {
            touchRaw = !touchRaw;
            Serial.printf("raw touch logging %s\n", touchRaw ? "on" : "off");
        } else if (line == "status") {
            panel::TouchPoint t = panel::readTouch();
            Serial.printf("idle=%lus dimmed=%d autodim=%d touch=%d(%d,%d) bright=%u\n",
                          (unsigned long)((millis() - lastInteractionMs) / 1000), dimmed, config::get().autoDim, t.down, t.x,
                          t.y, config::get().brightness);
            line = "";
            continue;  // don't count as interaction
        } else if (line == "touchdiag") {
            panel::touchDiag();
        } else if (line == "screenshot") {
            // Raw big-endian RGB565 frame for tools/screenshot.py.
            ui::render(true);
            Serial.printf("SCREENSHOT %d %d\n", panel::WIDTH, panel::HEIGHT);
            Serial.write((const uint8_t*)canvas.getBuffer(), panel::WIDTH * panel::HEIGHT * 2);
            Serial.print("\nEND\n");
        } else if (sscanf(line.c_str(), "tap %d %d", &x, &y) == 2) {
            ui::handleTouch({true, (int16_t)x, (int16_t)y});
            ui::handleTouch({false, (int16_t)x, (int16_t)y});
        } else if (int x2; sscanf(line.c_str(), "drag %d %d %d %d", &x, &y, &x2, &y2) == 4) {
            for (int i = 0; i <= 10; ++i) {
                ui::handleTouch({true, (int16_t)(x + (x2 - x) * i / 10), (int16_t)(y + (y2 - y) * i / 10)});
            }
            ui::handleTouch({false, (int16_t)x2, (int16_t)y2});
        } else if (sscanf(line.c_str(), "swipe %d %d %d", &x, &y, &y2) == 3) {
            for (int i = 0; i <= 10; ++i) ui::handleTouch({true, (int16_t)x, (int16_t)(y + (y2 - y) * i / 10)});
            ui::handleTouch({false, (int16_t)x, (int16_t)y2});
        } else {
            model::Guard g;  // the network task reads settings under this lock
            config::handleSerialLine(line);
        }
        line = "";
        applyDisplaySettings();
        dimmed = false;
        noteInteraction();  // serial commands count as activity
        ui::render(true);
    }

    if (touchRaw) {
        panel::touchRawDump();
        delay(20);
        return;
    }
    panel::TouchPoint tp = panel::readTouch();
    static panel::TouchPoint lastTp = {false, 0, 0};
    if (touchLog && (tp.down != lastTp.down || (tp.down && (tp.x != lastTp.x || tp.y != lastTp.y)))) {
        Serial.printf("touch %s %d,%d\n", tp.down ? "down" : "up", tp.x, tp.y);
    }
    lastTp = tp;

    if (tp.down) {
        if (dimmed) swallowTouch = true;  // this touch only wakes the screen
        noteInteraction();
    }
    if (swallowTouch) {
        if (!tp.down) swallowTouch = false;
    } else {
        ui::handleTouch(tp);
    }
    updateAutoDim();
    ui::render();
    delay(10);
}
