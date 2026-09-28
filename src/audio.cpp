#include "audio.h"

#include <Wire.h>
#include <driver/i2s.h>

#include "config.h"

namespace audio {
namespace {

// Freenove FNK0104N audio wiring (Sketch_07.1_Music).
constexpr int PIN_MCLK = 17;
constexpr int PIN_BCLK = 18;
constexpr int PIN_WS = 21;
constexpr int PIN_DOUT = 15;
constexpr int PIN_AMP_EN = 1;  // FM8002E enable, active low
constexpr uint8_t ES8311_ADDR = 0x18;

constexpr int SAMPLE_RATE = 16000;
constexpr int MCLK_HZ = SAMPLE_RATE * 384;  // 6.144 MHz
constexpr i2s_port_t PORT = I2S_NUM_0;
constexpr float MAX_AMPLITUDE = 14000;      // headroom below full scale at 100% volume

struct Request {
    Sound sound;
    bool force;
};

QueueHandle_t gQueue = nullptr;
bool gReady = false;

bool writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

uint8_t readReg(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.endTransmission(true);
    Wire.requestFrom(ES8311_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

// Register sequence from Espressif's es8311 driver (as shipped in Freenove's example):
// slave I2S, 16-bit, MCLK from pin at 6.144 MHz, 16 kHz, DAC to the output driver.
bool codecInit() {
    if (!writeReg(0x00, 0x1F)) return false;  // reset
    delay(20);
    writeReg(0x00, 0x00);
    writeReg(0x00, 0x80);                     // power on
    writeReg(0x01, 0x3F);                     // all clocks on, MCLK from MCLK pin
    writeReg(0x06, readReg(0x06) & ~0x20);    // SCLK not inverted
    // Clock dividers for {6144000, 16000}: pre_div 3, pre_multi 1 (x2), adc/dac div 1, ss, lrck 0x00ff, bclk 4, osr 0x10.
    writeReg(0x02, (readReg(0x02) & 0x07) | ((3 - 1) << 5) | (1 << 3));
    writeReg(0x03, 0x10);
    writeReg(0x04, 0x10);
    writeReg(0x05, 0x00);
    writeReg(0x06, (readReg(0x06) & 0xE0) | (4 - 1));
    writeReg(0x07, readReg(0x07) & 0xC0);
    writeReg(0x08, 0xFF);
    writeReg(0x00, readReg(0x00) & 0xBF);     // slave serial port
    writeReg(0x09, 3 << 2);                   // SDP in: 16-bit I2S
    writeReg(0x0A, 3 << 2);                   // SDP out: 16-bit I2S
    writeReg(0x0D, 0x01);                     // power up analog
    writeReg(0x0E, 0x02);
    writeReg(0x12, 0x00);                     // power up DAC
    writeReg(0x13, 0x10);                     // enable output driver
    writeReg(0x1C, 0x6A);
    writeReg(0x37, 0x08);                     // bypass DAC equalizer
    writeReg(0x32, 85 * 256 / 100 - 1);       // DAC volume fixed; user volume is applied in software
    writeReg(0x31, readReg(0x31) & ~0x60);    // unmute
    return true;
}

bool i2sInit() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = 256;
    cfg.tx_desc_auto_clear = true;  // output silence when idle
    cfg.fixed_mclk = MCLK_HZ;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    if (i2s_driver_install(PORT, &cfg, 0, nullptr) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.mck_io_num = PIN_MCLK;
    pins.bck_io_num = PIN_BCLK;
    pins.ws_io_num = PIN_WS;
    pins.data_out_num = PIN_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    return i2s_set_pin(PORT, &pins) == ESP_OK;
}

// ---- synthesis ----

struct Note {
    float freq;      // start frequency (Hz); 0 = rest
    float freqEnd;   // end frequency for sweeps (0 = same as freq)
    uint16_t ms;     // duration
    float decay;     // envelope decay rate (1/s); higher = shorter ring
    float harmonic;  // amount of 2nd harmonic (brightness)
};

// Channel message: gentle two-note chime (E6 -> A6), long bell decay.
const Note kChannel[] = {
    {1318.5f, 0, 170, 9, 0.15f},
    {1760.0f, 0, 380, 7, 0.15f},
};
// Direct message: brighter, quicker rising arpeggio (C6 E6 G6 C7) - more urgent.
const Note kDirect[] = {
    {1046.5f, 0, 85, 14, 0.35f},
    {1318.5f, 0, 85, 14, 0.35f},
    {1568.0f, 0, 85, 14, 0.35f},
    {2093.0f, 0, 300, 8, 0.35f},
};
// New node: two soft sonar-style upward sweeps.
const Note kNewNode[] = {
    {520, 1040, 220, 6, 0.0f},
    {0, 0, 90, 0, 0},
    {520, 1040, 220, 6, 0.0f},
};

void playNotes(const Note* notes, int count, float amplitude) {
    constexpr int FRAMES = 128;
    int16_t buf[FRAMES * 2];
    for (int n = 0; n < count; ++n) {
        const Note& note = notes[n];
        const int total = SAMPLE_RATE * note.ms / 1000;
        const float f1 = note.freqEnd > 0 ? note.freqEnd : note.freq;
        float phase = 0;
        for (int start = 0; start < total; start += FRAMES) {
            const int len = min(FRAMES, total - start);
            for (int i = 0; i < len; ++i) {
                const int s = start + i;
                const float t = (float)s / SAMPLE_RATE;
                float v = 0;
                if (note.freq > 0) {
                    const float f = note.freq + (f1 - note.freq) * s / total;
                    phase += 2 * PI * f / SAMPLE_RATE;
                    if (phase > 2 * PI) phase -= 2 * PI;
                    // Quick attack, exponential decay, short fade at the end to avoid clicks.
                    float env = min(1.0f, t * 400) * expf(-note.decay * t);
                    const int remain = total - s;
                    if (remain < 80) env *= remain / 80.0f;
                    v = env * (sinf(phase) + note.harmonic * sinf(2 * phase)) / (1 + note.harmonic);
                }
                const int16_t sample = (int16_t)(v * amplitude);
                buf[2 * i] = sample;
                buf[2 * i + 1] = sample;
            }
            size_t written;
            i2s_write(PORT, buf, len * 4, &written, portMAX_DELAY);
        }
    }
}

void task(void*) {
    Request req;
    for (;;) {
        if (xQueueReceive(gQueue, &req, portMAX_DELAY) != pdTRUE) continue;
        const Settings& s = config::get();
        if (s.muted && !req.force) continue;
        const float vol = s.volume / 100.0f;
        if (vol <= 0) continue;
        const float amplitude = MAX_AMPLITUDE * vol * vol;  // perceptual-ish curve

        digitalWrite(PIN_AMP_EN, LOW);  // amp on
        delay(8);
        switch (req.sound) {
            case Sound::ChannelMessage: playNotes(kChannel, sizeof(kChannel) / sizeof(Note), amplitude); break;
            case Sound::DirectMessage: playNotes(kDirect, sizeof(kDirect) / sizeof(Note), amplitude); break;
            case Sound::NewNode: playNotes(kNewNode, sizeof(kNewNode) / sizeof(Note), amplitude); break;
        }
        // Let the DMA drain (auto-cleared to silence) before switching the amp off.
        delay(80);
        if (uxQueueMessagesWaiting(gQueue) == 0) digitalWrite(PIN_AMP_EN, HIGH);
        delay(150);  // gap between back-to-back alerts
    }
}

}  // namespace

bool begin() {
    pinMode(PIN_AMP_EN, OUTPUT);
    digitalWrite(PIN_AMP_EN, HIGH);  // amp off until something plays
    gReady = codecInit() && i2sInit();
    if (!gReady) {
        Serial.println("audio: ES8311 codec not found");
        return false;
    }
    gQueue = xQueueCreate(6, sizeof(Request));
    xTaskCreatePinnedToCore(task, "audio", 4096, nullptr, 2, nullptr, 0);
    return true;
}

void play(Sound s) {
    if (!gReady) return;
    Request r = {s, false};
    xQueueSend(gQueue, &r, 0);
}

void preview(Sound s) {
    if (!gReady) return;
    Request r = {s, true};
    xQueueSend(gQueue, &r, 0);
}

}  // namespace audio
