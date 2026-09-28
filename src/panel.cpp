#include "panel.h"

#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>

namespace panel {
namespace {

// ---- Display (ST77922, QSPI on SPI2 IOMUX pins) ----
constexpr int PIN_CS = 10;
constexpr int PIN_BL = 41;
constexpr int BL_CHANNEL = 0;  // LEDC PWM channel for the backlight
constexpr int PIN_SCLK = 12;
constexpr int PIN_D0 = 11;
constexpr int PIN_D1 = 13;
constexpr int PIN_D2 = 14;
constexpr int PIN_D3 = 9;

constexpr int PHYS_W = 320;  // native portrait
constexpr int PHYS_H = 480;

constexpr uint8_t QSPI_1W_CMD = 0x02;
constexpr uint8_t QSPI_4W_CMD = 0x32;
constexpr uint8_t WR_RAM_C_CMD = 0x3C;

// Physical rows per DMA chunk; two chunks ping-pong so rotation overlaps transfer.
constexpr int CHUNK_ROWS = 24;
constexpr size_t CHUNK_PIXELS = PHYS_W * CHUNK_ROWS;

// ---- Touch (I2C) ----
constexpr int PIN_TOUCH_SDA = 38;
constexpr int PIN_TOUCH_SCL = 39;
constexpr int PIN_TOUCH_RST = 48;
constexpr uint8_t TOUCH_ADDR = 0x55;
constexpr uint16_t REG_STATUS = 0x0001;
constexpr uint16_t REG_TOUCH_INFO = 0x0010;
constexpr uint16_t REG_TOUCH_POINT0 = 0x0014;
constexpr uint16_t REG_MAX_TOUCHES = 0x0009;
constexpr int TOUCH_POINT_BYTES = 7;
constexpr int MAX_TOUCH_POINTS = 10;

struct InitCmd {
    uint8_t cmd;
    uint8_t len;
    uint16_t delayMs;
    uint8_t data[16];
};

const InitCmd kInit[] = {
    {0xF1, 1, 0, {0x00}},
    {0x60, 3, 0, {0x00, 0x00, 0x00}},
    {0x65, 1, 0, {0x80}},
    {0x79, 1, 0, {0x06}},
    {0x7B, 3, 0, {0x00, 0x08, 0x08}},
    {0x80, 11, 0, {0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA}},
    {0x81, 4, 0, {0x26, 0x52, 0x72, 0x27}},
    {0x84, 2, 0, {0x92, 0x25}},
    {0x87, 6, 0, {0x10, 0x10, 0x58, 0x00, 0x02, 0x3A}},
    {0x88, 15, 0, {0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06}},
    {0x89, 3, 0, {0x00, 0x00, 0x00}},
    {0x8A, 11, 0, {0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19}},
    {0x8B, 9, 0, {0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E}},
    {0x8C, 13, 0, {0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09}},
    {0x8D, 1, 0, {0x0C}},
    {0x8E, 6, 0, {0x33, 0x01, 0x0C, 0x13, 0x01, 0x01}},
    {0xB3, 2, 0, {0x00, 0x30}},
    {0xF1, 1, 0, {0x00}},
    {0x71, 1, 0, {0xD0}},
    {0x66, 2, 0, {0x02, 0x3F}},
    {0xBE, 3, 0, {0x26, 0x00, 0x9D}},
    {0x70, 12, 0, {0x01, 0xA0, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x69, 0x11, 0x00, 0x00, 0x1A}},
    {0x90, 9, 0, {0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x27, 0x27}},
    {0x91, 9, 0, {0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x27, 0x27}},
    {0x92, 10, 0, {0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x27}},
    {0x93, 10, 0, {0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x27}},
    {0x94, 6, 0, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x95, 5, 0, {0x96, 0x16, 0x00, 0x00, 0xFF}},
    {0x96, 12, 0, {0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x94, 0x27, 0x00, 0x44}},
    {0x97, 12, 0, {0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x94, 0x27, 0x00, 0x44}},
    {0xBA, 5, 0, {0x55, 0x94, 0x2D, 0x94, 0x27}},
    {0x9A, 7, 0, {0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}},
    {0x9B, 7, 0, {0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}},
    {0x9C, 13, 0, {0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00}},
    {0x9D, 8, 0, {0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}},
    {0x9E, 7, 0, {0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}},
    {0xB4, 12, 0, {0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10}},
    {0xB5, 12, 0, {0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12}},
    {0xB6, 7, 0, {0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF}},
    {0x86, 14, 0, {0xCD, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5}},
    {0xB7, 16, 0, {0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D}},
    {0xB8, 16, 0, {0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D}},
    {0xB9, 2, 0, {0x23, 0x23}},
    {0xBF, 6, 0, {0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B}},
    {0xF2, 1, 0, {0x00}},
    {0x73, 5, 0, {0x04, 0xDA, 0x12, 0x54, 0x47}},
    {0x77, 5, 0, {0x6B, 0x5B, 0xFD, 0xC3, 0xC5}},
    {0x7A, 2, 0, {0x15, 0x27}},
    {0x7B, 2, 0, {0x04, 0x57}},
    {0x7E, 2, 0, {0x01, 0x0E}},
    {0xBF, 1, 0, {0x36}},
    {0xE3, 2, 0, {0x40, 0x40}},
    {0xF0, 1, 0, {0x00}},
    {0xD0, 1, 0, {0x00}},
    {0x2A, 4, 0, {0x00, 0x00, 0x01, 0x3F}},
    {0x2B, 4, 0, {0x00, 0x00, 0x01, 0xDF}},
    {0x21, 0, 0, {}},
    {0x11, 0, 120, {}},
    {0x29, 0, 0, {}},
    {0x2C, 0, 0, {}},
    {0x3A, 1, 0, {0x01}},
    {0x36, 1, 0, {0x00}},
    {0x35, 1, 20, {0x01}},
};

spi_device_handle_t qspi = nullptr;
uint16_t* chunkBuf[2] = {nullptr, nullptr};
bool flipped = false;
bool touchOk = false;
uint8_t touchPoints = 5;  // from REG_MAX_TOUCHES; the whole report must be read to release it
TouchPoint lastTouch = {false, 0, 0};
uint8_t releaseCount = 0;  // consecutive "no finger" reads
constexpr uint8_t RELEASE_READS = 3;  // debounce: the controller briefly drops contact mid-tap

void writeReg(uint8_t cmd, const uint8_t* data, size_t len) {
    spi_transaction_ext_t t = {};
    t.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
    t.base.cmd = QSPI_1W_CMD;
    t.base.addr = (uint32_t)cmd << 8;
    t.command_bits = 8;
    t.address_bits = 24;
    t.base.tx_buffer = len ? data : nullptr;
    t.base.length = 8 * len;
    spi_device_polling_transmit(qspi, (spi_transaction_t*)&t);
}

void setWindow(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey) {
    const uint8_t xd[] = {(uint8_t)(sx >> 8), (uint8_t)sx, (uint8_t)((ex - 1) >> 8), (uint8_t)(ex - 1)};
    writeReg(0x2A, xd, 4);
    const uint8_t yd[] = {(uint8_t)(sy >> 8), (uint8_t)sy, (uint8_t)((ey - 1) >> 8), (uint8_t)(ey - 1)};
    writeReg(0x2B, yd, 4);
}

// Fill one chunk of physical rows [py0, py0+CHUNK_ROWS) from the logical landscape frame.
void rotateChunk(const uint16_t* src, uint16_t* dst, int py0) {
    for (int r = 0; r < CHUNK_ROWS; ++r) {
        const int py = py0 + r;
        uint16_t* out = dst + r * PHYS_W;
        if (!flipped) {
            // logical (lx, ly) -> physical (319 - ly, lx)
            const uint16_t* col = src + py;  // lx = py
            for (int px = 0; px < PHYS_W; ++px) {
                out[px] = col[(PHYS_W - 1 - px) * WIDTH];
            }
        } else {
            // logical (lx, ly) -> physical (ly, 479 - lx)
            const uint16_t* col = src + (WIDTH - 1 - py);
            for (int px = 0; px < PHYS_W; ++px) {
                out[px] = col[px * WIDTH];
            }
        }
    }
}

bool touchRead(uint16_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)reg);
    if (Wire.endTransmission(true) != 0) return false;  // STOP before read, per Sitronix TDDI spec
    if (Wire.requestFrom((uint16_t)TOUCH_ADDR, (size_t)len) != len) return false;
    for (size_t i = 0; i < len; ++i) buf[i] = Wire.read();
    return true;
}

void touchBegin() {
    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 100000);
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(20);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(100);
    // Wait (bounded) for the controller to report ready.
    uint8_t status = 0xFF;
    for (int i = 0; i < 50; ++i) {
        if (touchRead(REG_STATUS, &status, 1) && (status & 0x0F) == 0) {
            touchOk = true;
            break;
        }
        delay(10);
    }
    uint8_t maxPts = 0;
    if (touchOk && touchRead(REG_MAX_TOUCHES, &maxPts, 1) && maxPts >= 1 && maxPts <= MAX_TOUCH_POINTS) {
        touchPoints = maxPts;
    }
    log_i("touch %s (status=0x%02x, points=%u)", touchOk ? "ready" : "not found", status, touchPoints);
}

}  // namespace

bool begin() {
    ledcSetup(BL_CHANNEL, 5000, 8);
    ledcAttachPin(PIN_BL, BL_CHANNEL);
    ledcWrite(BL_CHANNEL, 0);

    spi_bus_config_t bus = {};
    bus.data0_io_num = PIN_D0;
    bus.data1_io_num = PIN_D1;
    bus.sclk_io_num = PIN_SCLK;
    bus.data2_io_num = PIN_D2;
    bus.data3_io_num = PIN_D3;
    bus.max_transfer_sz = CHUNK_PIXELS * 2 + 8;
    bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_IOMUX_PINS | SPICOMMON_BUSFLAG_QUAD;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;

    spi_device_interface_config_t dev = {};
    dev.mode = 0;
    dev.clock_speed_hz = 80 * 1000 * 1000;
    dev.spics_io_num = PIN_CS;
    dev.flags = SPI_DEVICE_HALFDUPLEX;
    dev.queue_size = 2;
    if (spi_bus_add_device(SPI2_HOST, &dev, &qspi) != ESP_OK) return false;

    for (auto& b : chunkBuf) {
        b = (uint16_t*)heap_caps_malloc(CHUNK_PIXELS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!b) return false;
    }

    for (const auto& c : kInit) {
        writeReg(c.cmd, c.data, c.len);
        if (c.delayMs) delay(c.delayMs);
    }

    touchBegin();
    return true;
}

void setFlipped(bool f) { flipped = f; }

void setBrightness(uint8_t level) { ledcWrite(BL_CHANNEL, level); }

void pushFrame(const uint16_t* pixels) {
    setWindow(0, 0, PHYS_W, PHYS_H);

    spi_transaction_ext_t trans[2] = {};
    constexpr int kChunks = PHYS_H / CHUNK_ROWS;
    int inFlight = 0;
    for (int i = 0; i < kChunks; ++i) {
        const int slot = i & 1;
        if (inFlight == 2) {
            spi_transaction_t* done;
            spi_device_get_trans_result(qspi, &done, portMAX_DELAY);
            --inFlight;
        }
        rotateChunk(pixels, chunkBuf[slot], i * CHUNK_ROWS);

        spi_transaction_ext_t& t = trans[slot];
        t = {};
        t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
        t.base.cmd = QSPI_4W_CMD;
        t.base.addr = (uint32_t)WR_RAM_C_CMD << 8;
        t.command_bits = 8;
        t.address_bits = 24;
        t.base.tx_buffer = chunkBuf[slot];
        t.base.length = CHUNK_PIXELS * 16;
        spi_device_queue_trans(qspi, (spi_transaction_t*)&t, portMAX_DELAY);
        ++inFlight;
    }
    while (inFlight--) {
        spi_transaction_t* done;
        spi_device_get_trans_result(qspi, &done, portMAX_DELAY);
    }
}

void touchDiag() {
    Serial.printf("touch ok=%d\nI2C devices:", touchOk);
    for (uint8_t a = 1; a < 127; ++a) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) Serial.printf(" 0x%02x", a);
    }
    Serial.println();
    uint8_t buf[16] = {};
    bool r = touchRead(0x0000, buf, 16);
    Serial.printf("regs read=%d fw=%02x status=%02x ctrl=%02x maxX=%d maxY=%d maxTouch=%d counter=%d\n", r, buf[0],
                  buf[1], buf[2], (buf[5] << 8) | buf[6], (buf[7] << 8) | buf[8], buf[9], (buf[10] << 8) | buf[11]);
    delay(500);
    touchRead(0x000A, buf, 2);
    Serial.printf("counter after 500ms=%d\n", (buf[0] << 8) | buf[1]);
    for (int i = 0; i < 20; ++i) {
        uint8_t info = 0, p[7] = {};
        bool a = touchRead(REG_TOUCH_INFO, &info, 1);
        bool b = touchRead(REG_TOUCH_POINT0, p, 7);
        Serial.printf("info(%d)=0x%02x pt(%d)=%02x %02x %02x %02x %02x %02x %02x int=%d\n", a, info, b, p[0], p[1],
                      p[2], p[3], p[4], p[5], p[6], digitalRead(47));
        delay(250);
    }
}

void touchRawDump() {
    static uint8_t prev[32] = {};
    static uint32_t lastCounterMs = 0;
    uint8_t regs[0x1B] = {};  // 0x0000..0x001A: status, counter, touch info, point 0
    if (!touchRead(0x0000, regs, sizeof(regs))) {
        Serial.println("raw: read failed");
        return;
    }
    // Ignore the sensing counter (0x0A/0x0B) when deciding whether anything changed.
    bool changed = regs[0x01] != prev[0x01] || memcmp(regs + 0x10, prev + 0x10, 0x0B) != 0;
    if (changed || millis() - lastCounterMs > 3000) {
        lastCounterMs = millis();
        Serial.printf("raw st=%02x cnt=%5u info=%02x ges=%02x p0=", regs[1], (regs[0x0A] << 8) | regs[0x0B],
                      regs[0x10], regs[0x12]);
        for (int i = 0x14; i < 0x1B; ++i) Serial.printf("%02x ", regs[i]);
        Serial.printf("int=%d\n", digitalRead(47));
    }
    memcpy(prev, regs, sizeof(regs));
    uint8_t rest[TOUCH_POINT_BYTES * MAX_TOUCH_POINTS];
    touchRead(REG_TOUCH_POINT0, rest, TOUCH_POINT_BYTES * touchPoints);  // release the report
}

TouchPoint readTouch() {
    if (!touchOk) return {false, 0, 0};
    // Advanced Touch Info (0x0010) bit 3 "With Coord." is set while a finger is down.
    uint8_t info = 0;
    if (!touchRead(REG_TOUCH_INFO, &info, 1)) return lastTouch;
    if (!(info & 0x08)) {
        if (++releaseCount >= RELEASE_READS) lastTouch.down = false;
        return lastTouch;
    }
    // Read every point slot: the controller holds the report (and INT low) until it is read in full.
    uint8_t p[TOUCH_POINT_BYTES * MAX_TOUCH_POINTS];
    if (!touchRead(REG_TOUCH_POINT0, p, TOUCH_POINT_BYTES * touchPoints)) return lastTouch;
    if (!(p[0] & 0x80)) {  // point 0 not valid
        if (++releaseCount >= RELEASE_READS) lastTouch.down = false;
        return lastTouch;
    }
    releaseCount = 0;
    const int rx = ((p[0] & 0x3F) << 8) | p[1];  // physical x, 0..319
    const int ry = ((p[2] & 0x3F) << 8) | p[3];  // physical y, 0..479
    int lx, ly;
    if (!flipped) {
        lx = ry;
        ly = PHYS_W - 1 - rx;
    } else {
        lx = WIDTH - 1 - ry;
        ly = rx;
    }
    lastTouch = {true, (int16_t)constrain(lx, 0, WIDTH - 1), (int16_t)constrain(ly, 0, HEIGHT - 1)};
    return lastTouch;
}

}  // namespace panel
