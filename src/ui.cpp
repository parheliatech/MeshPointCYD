#include "ui.h"

#include <WiFi.h>
#include <time.h>

#include <algorithm>

#include "audio.h"
#include "config.h"
#include "meshpoint.h"
#include "timezones.h"

namespace ui {
namespace {

// ---- Meshpoint design-system palette (docs/DESIGN-SYSTEM.md) ----
constexpr uint32_t C_BG = 0x0A0E17u;
constexpr uint32_t C_BAR = 0x111827u;
constexpr uint32_t C_CARD = 0x162033u;
constexpr uint32_t C_BORDER = 0x233049u;
constexpr uint32_t C_TEXT = 0xE2E8F0u;
constexpr uint32_t C_TEXT2 = 0x94A3B8u;
constexpr uint32_t C_MUTED = 0x64748Bu;
constexpr uint32_t C_CYAN = 0x06B6D4u;
constexpr uint32_t C_GREEN = 0x00E5A0u;
constexpr uint32_t C_BLUE = 0x3B82F6u;
constexpr uint32_t C_PURPLE = 0xA855F7u;
constexpr uint32_t C_AMBER = 0xF59E0Bu;
constexpr uint32_t C_RED = 0xEF4444u;

const lgfx::IFont* F_SMALL = &fonts::DejaVu12;
const lgfx::IFont* F_BODY = &fonts::FreeSans9pt7b;
const lgfx::IFont* F_BODY_B = &fonts::FreeSansBold9pt7b;
const lgfx::IFont* F_TITLE = &fonts::FreeSansBold12pt7b;
const lgfx::IFont* F_BIG = &fonts::FreeSansBold18pt7b;

constexpr int W = panel::WIDTH;
constexpr int H = panel::HEIGHT;
constexpr int HEADER_H = 36;
constexpr int TAB_H = 44;
constexpr int CONTENT_Y = HEADER_H;
constexpr int CONTENT_H = H - HEADER_H - TAB_H;
constexpr int TAB_Y = H - TAB_H;

enum Page { HOME, NODES, MAP, FEED, CHAT, SETTINGS, PAGE_COUNT };
static_assert(MAP == page::MAP && CHAT == page::CHAT && PAGE_COUNT == page::COUNT, "page ids must match meshpoint.h");
const char* const kTabNames[] = {"Home", "Nodes", "Map", "Feed", "Chat", ""};
constexpr int TAB_W = 84;  // five text tabs; the gear takes the rest

LGFX_Sprite* cv = nullptr;

// ---- UI state ----
Page page = HOME;
int scroll[PAGE_COUNT] = {};
int contentHeight = 0;  // height of the current scrollable content
char openNode[16] = "";  // node detail overlay (Nodes page)
bool chatOpen = false;   // conversation view (Chat page)
bool chatStickBottom = true;
int listScroll = 0;       // list position to restore when an overlay closes
uint32_t updateConfirmUntil = 0;
bool tzPickerOpen = false;  // Settings: time zone list showing  // Settings: "Install now?" confirmation showing until this time
uint32_t lastVersion = UINT32_MAX;
uint32_t lastDrawMs = 0;
bool dirty = true;

// Touch gesture tracking.
bool wasDown = false;
bool dragging = false;
int downX = 0, downY = 0, downScroll = 0;

// Tap targets registered during the last render (content coords are screen coords).
enum class Action : uint8_t { None, Tab, OpenNode, OpenConv, Back, Flip, BrightDown, BrightUp, Portal, Refresh,
                              ZoomIn, ZoomOut, ZoomFit, AutoDim, UpdateStart, UpdateConfirm, UpdateCancel,
                              UpdateCheck, UpdateDismiss, OpenTimezone, PickTimezone, Mute, VolDown, VolUp, TestChannel, TestDM, TestNode };
struct Target {
    int16_t x, y, w, h;
    Action action;
    int16_t arg;
};
constexpr int MAX_TARGETS = 64;
Target targets[MAX_TARGETS];
int targetCount = 0;

void addTarget(int x, int y, int w, int h, Action a, int arg = 0) {
    // Only register the visible part of content targets.
    if (a != Action::Tab) {
        int top = max(y, CONTENT_Y), bottom = min(y + h, CONTENT_Y + CONTENT_H);
        if (bottom <= top) return;
        y = top;
        h = bottom - top;
    }
    if (targetCount < MAX_TARGETS) targets[targetCount++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, a, (int16_t)arg};
}

// ---- formatting helpers ----

void fmtAge(char* out, size_t n, time_t ts) {
    time_t now = time(nullptr);
    if (ts <= 0 || now < 1600000000) {
        snprintf(out, n, "-");
        return;
    }
    long d = (long)(now - ts);
    if (d < 0) d = 0;
    if (d < 60) snprintf(out, n, "%lds", d);
    else if (d < 3600) snprintf(out, n, "%ldm", d / 60);
    else if (d < 86400) snprintf(out, n, "%ldh", d / 3600);
    else snprintf(out, n, "%ldd", d / 86400);
}

void fmtDuration(char* out, size_t n, uint32_t s) {
    if (s >= 86400) snprintf(out, n, "%lud %luh", (unsigned long)(s / 86400), (unsigned long)(s % 86400 / 3600));
    else if (s >= 3600) snprintf(out, n, "%luh %lum", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
    else snprintf(out, n, "%lum", (unsigned long)(s / 60));
}

void fmtCount(char* out, size_t n, uint32_t v) {
    if (v >= 1000000) snprintf(out, n, "%.1fM", v / 1e6);
    else if (v >= 10000) snprintf(out, n, "%.1fk", v / 1e3);
    else snprintf(out, n, "%lu", (unsigned long)v);
}

uint32_t rssiColor(float rssi) {
    if (!model::has(rssi)) return C_MUTED;
    if (rssi > -95) return C_GREEN;
    if (rssi > -110) return C_AMBER;
    return C_RED;
}

uint32_t typeColor(const char* t) {
    if (!strcmp(t, "text")) return C_GREEN;
    if (!strcmp(t, "position")) return C_AMBER;
    if (!strcmp(t, "telemetry")) return C_CYAN;
    if (!strcmp(t, "nodeinfo")) return C_PURPLE;
    if (!strcmp(t, "encrypted")) return C_RED;
    if (!strcmp(t, "traceroute") || !strcmp(t, "neighborinfo")) return C_BLUE;
    return C_MUTED;
}

uint32_t protocolColor(const char* p) {
    if (!strcasecmp(p, "meshcore")) return C_PURPLE;
    return C_BLUE;
}

// Stable per-node badge color.
uint32_t idColor(const char* id) {
    static const uint32_t palette[] = {0x0E7490u, 0x1D4ED8u, 0x7E22CEu, 0xB45309u, 0x047857u, 0xBE123Cu, 0x4338CAu, 0x0F766Eu};
    uint32_t h = 2166136261u;
    for (; *id; ++id) h = (h ^ (uint8_t)*id) * 16777619u;
    return palette[h % 8];
}

// Mix 0xRRGGBB colors: t=0 -> a, t=255 -> b.
uint32_t mix(uint32_t a, uint32_t b, uint8_t t) {
    auto ch = [&](int s) {
        int ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
        return (uint32_t)((ca * (255 - t) + cb * t) / 255) << s;
    };
    return ch(16) | ch(8) | ch(0);
}

// ---- drawing helpers ----

void font(const lgfx::IFont* f) { cv->setFont(f); }

void text(const char* s, int x, int y, uint32_t color, lgfx::textdatum_t datum = lgfx::middle_left) {
    cv->setTextDatum(datum);
    cv->setTextColor(color);
    cv->drawString(s, x, y);
}

// Draw text truncated with "..." to fit maxW.
void textFit(const char* s, int x, int y, int maxW, uint32_t color, lgfx::textdatum_t datum = lgfx::middle_left) {
    if (cv->textWidth(s) <= maxW) {
        text(s, x, y, color, datum);
        return;
    }
    char buf[200];
    strlcpy(buf, s, sizeof(buf));
    int len = strlen(buf);
    const int dots = cv->textWidth("...");
    while (len > 0) {
        buf[--len] = 0;
        if (cv->textWidth(buf) + dots <= maxW) break;
    }
    while (len > 0 && buf[len - 1] == ' ') buf[--len] = 0;
    strlcat(buf, "...", sizeof(buf));
    text(buf, x, y, color, datum);
}

// Word-wrap s into at most maxLines lines of width maxW. Returns line count.
int wrap(const char* s, int maxW, char lines[][120], int maxLines) {
    int count = 0;
    const char* p = s;
    while (*p && count < maxLines) {
        while (*p == ' ') ++p;
        if (!*p) break;
        char line[120] = "";
        int lastBreak = -1;
        int len = 0;
        const char* lineStart = p;
        const char* breakAt = nullptr;
        while (*p && len < 118) {
            line[len] = *p;
            line[len + 1] = 0;
            if (cv->textWidth(line) > maxW) {
                line[len] = 0;
                break;
            }
            if (*p == ' ') {
                lastBreak = len;
                breakAt = p;
            }
            ++len;
            ++p;
        }
        if (*p && lastBreak > 0 && p != lineStart) {
            line[lastBreak] = 0;  // break at last space
            p = breakAt + 1;
        }
        if (count == maxLines - 1 && *p) {
            // Last allowed line: ellipsize.
            strlcpy(lines[count], line, 120);
            int l = strlen(lines[count]);
            while (l > 0 && cv->textWidth(lines[count]) + cv->textWidth("...") > maxW) lines[count][--l] = 0;
            strlcat(lines[count], "...", 120);
            return count + 1;
        }
        strlcpy(lines[count++], line, 120);
        if (len == 0) break;  // safety: a single glyph wider than maxW
    }
    return count;
}

void card(int x, int y, int w, int h) {
    cv->fillRoundRect(x, y, w, h, 8, C_CARD);
    cv->drawRoundRect(x, y, w, h, 8, C_BORDER);
}

void chip(const char* label, int x, int cy, uint32_t color, int minW = 0) {
    font(F_SMALL);
    int w = max(minW, (int)cv->textWidth(label) + 12);
    cv->fillRoundRect(x, cy - 9, w, 18, 4, mix(C_CARD, color, 70));
    cv->drawRoundRect(x, cy - 9, w, 18, 4, mix(C_CARD, color, 160));
    text(label, x + w / 2, cy + 1, color, lgfx::middle_center);
}

void button(const char* label, int x, int y, int w, int h, Action a, uint32_t color = C_CYAN) {
    cv->fillRoundRect(x, y, w, h, 8, mix(C_CARD, color, 50));
    cv->drawRoundRect(x, y, w, h, 8, color);
    font(F_BODY_B);
    text(label, x + w / 2, y + h / 2 + 1, C_TEXT, lgfx::middle_center);
    addTarget(x, y, w, h, a);
}

void gearIcon(int cx, int cy, uint32_t color) {
    for (int i = 0; i < 8; ++i) {
        float a = i * PI / 4;
        cv->fillCircle(cx + cosf(a) * 9, cy + sinf(a) * 9, 3, color);
    }
    cv->fillCircle(cx, cy, 8, color);
    cv->fillCircle(cx, cy, 4, C_BAR);
}

void backButton(int y) {
    cv->fillRoundRect(8, y, 76, 30, 8, C_CARD);
    cv->drawRoundRect(8, y, 76, 30, 8, C_BORDER);
    cv->fillTriangle(20, y + 15, 28, y + 8, 28, y + 22, C_CYAN);
    font(F_BODY_B);
    text("Back", 34, y + 16, C_TEXT);
    addTarget(8, y, 76, 30, Action::Back);
}

// ---- header & tabs ----

void drawHeader(const model::Model& m) {
    cv->fillRect(0, 0, W, HEADER_H, C_BAR);
    cv->drawFastHLine(0, HEADER_H - 1, W, C_BORDER);

    uint32_t lamp = C_RED;
    const auto& st = m.status;
    if (st.link == model::Link::Ok) lamp = (millis() - st.lastOkMs < 60000) ? C_GREEN : C_AMBER;
    else if (st.link == model::Link::Connecting) lamp = C_AMBER;
    cv->fillCircle(16, HEADER_H / 2, 8, mix(C_BAR, lamp, 70));
    cv->fillCircle(16, HEADER_H / 2, 5, lamp);

    // Right side: clock, then WiFi bars.
    char clock[8] = "--:--";
    time_t now = time(nullptr);
    if (now > 1600000000) {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(clock, sizeof(clock), "%H:%M", &tm);
    }
    font(F_BODY_B);
    text(clock, W - 10, HEADER_H / 2 + 1, C_TEXT, lgfx::middle_right);
    int clockW = cv->textWidth(clock);

    int bx = W - 10 - clockW - 30;
    int rssi = WiFi.isConnected() ? WiFi.RSSI() : -200;
    int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : rssi > -200 ? 1 : 0;
    for (int i = 0; i < 4; ++i) {
        int bh = 4 + i * 4;
        cv->fillRect(bx + i * 5, HEADER_H / 2 + 8 - bh, 3, bh, i < bars ? C_TEXT2 : C_BORDER);
    }

    const char* name = m.summary.valid && m.summary.name[0] ? m.summary.name : "Meshpoint";
    font(F_TITLE);
    textFit(name, 32, HEADER_H / 2 + 1, bx - 32 - 10, C_TEXT);
}

void drawTabs(const model::Model& m) {
    cv->fillRect(0, TAB_Y, W, TAB_H, C_BAR);
    cv->drawFastHLine(0, TAB_Y, W, C_BORDER);
    int unread = 0;
    for (int i = 0; i < m.convCount; ++i) unread += m.convs[i].unread;

    for (int i = 0; i < PAGE_COUNT; ++i) {
        int x = i * TAB_W;
        int w = (i == SETTINGS) ? W - x : TAB_W;
        bool active = (i == page);
        uint32_t col = active ? C_CYAN : C_TEXT2;
        if (active) cv->fillRect(x + 12, TAB_Y + 1, w - 24, 3, C_CYAN);
        if (i == SETTINGS) {
            // Amber gear (with a dot) when a Meshpoint update is available.
            const bool update = m.update.available && m.update.state != model::UpdateState::Done;
            gearIcon(x + w / 2, TAB_Y + TAB_H / 2 + 1, update ? C_AMBER : col);
            if (update) cv->fillCircle(x + w / 2 + 12, TAB_Y + 10, 4, C_AMBER);
        } else {
            font(F_BODY_B);
            text(kTabNames[i], x + w / 2, TAB_Y + TAB_H / 2 + 2, col, lgfx::middle_center);
        }
        if (i == CHAT && unread > 0) {
            char b[6];
            snprintf(b, sizeof(b), "%d", min(unread, 99));
            int bx = x + w / 2 + cv->textWidth("Chat") / 2 + 4;
            cv->fillRoundRect(bx, TAB_Y + 6, 22, 16, 8, C_CYAN);
            font(F_SMALL);
            text(b, bx + 11, TAB_Y + 14, C_BG, lgfx::middle_center);
        }
        addTarget(x, TAB_Y, w, TAB_H, Action::Tab, i);
    }
}

// Full content-area notice used before data arrives or on errors.
bool drawNotice(const model::Model& m, bool haveData) {
    if (haveData) return false;
    const auto& st = m.status;
    const char* title = "Connecting to Meshpoint...";
    uint32_t col = C_CYAN;
    if (st.link == model::Link::NoWifi) {
        title = "WiFi not connected";
        col = C_RED;
    } else if (st.link == model::Link::AuthFailed) {
        title = "Meshpoint login failed";
        col = C_RED;
    } else if (st.link == model::Link::Error) {
        title = "Can't load data";
        col = C_AMBER;
    }
    card(24, CONTENT_Y + 40, W - 48, 150);
    font(F_TITLE);
    text(title, W / 2, CONTENT_Y + 72, col, lgfx::middle_center);
    font(F_BODY);
    textFit(st.error, W / 2, CONTENT_Y + 106, W - 80, C_TEXT2, lgfx::middle_center);
    textFit(config::get().url.c_str(), W / 2, CONTENT_Y + 134, W - 80, C_MUTED, lgfx::middle_center);
    font(F_SMALL);
    text("Check the Settings tab, or type 'help' on USB serial", W / 2, CONTENT_Y + 164, C_MUTED, lgfx::middle_center);
    return true;
}

// ---- Home ----

void statTile(int x, int y, int w, int h, const char* label, const char* value, const char* sub,
              uint32_t valueColor = C_TEXT) {
    card(x, y, w, h);
    font(F_SMALL);
    text(label, x + 10, y + 13, C_MUTED);
    font(F_BIG);
    textFit(value, x + 10, y + 40, w - 16, valueColor);
    font(F_SMALL);
    textFit(sub, x + 10, y + h - 11, w - 16, C_TEXT2);
}

int drawHome(const model::Model& m, int y0) {
    const auto& s = m.summary;
    if (drawNotice(m, s.valid)) return CONTENT_H;
    char v[24], sub[40];
    int y = y0 + 8;
    const int tw = (W - 8 * 5) / 4, th = 72;

    fmtCount(v, sizeof(v), s.totalPackets);
    snprintf(sub, sizeof(sub), "%lu /hour", (unsigned long)s.packetsLastHour);
    statTile(8, y, tw, th, "PACKETS", v, sub);

    snprintf(v, sizeof(v), "%.1f", s.packetsPerMinute);
    snprintf(sub, sizeof(sub), "%lu last min", (unsigned long)s.packetsLastMinute);
    statTile(8 + (tw + 8), y, tw, th, "PER MINUTE", v, sub, C_CYAN);

    snprintf(v, sizeof(v), "%d", s.active24h);
    snprintf(sub, sizeof(sub), "of %d total", s.totalNodes);
    statTile(8 + 2 * (tw + 8), y, tw, th, "NODES 24H", v, sub, C_GREEN);

    if (model::has(s.avgRssi)) snprintf(v, sizeof(v), "%.0f", s.avgRssi);
    else strcpy(v, "-");
    if (model::has(s.avgSnr)) snprintf(sub, sizeof(sub), "SNR %.1f dB", s.avgSnr);
    else strcpy(sub, "dBm");
    statTile(8 + 3 * (tw + 8), y, tw, th, "AVG RSSI", v, sub, rssiColor(s.avgRssi));

    // Traffic chart (last 60 min, 5-min buckets).
    y += th + 8;
    const int chartW = 300, boxH = 142;
    card(8, y, chartW, boxH);
    font(F_SMALL);
    text("TRAFFIC  last 60 min", 18, y + 13, C_MUTED);
    int maxC = 1;
    for (int c : s.timeline) maxC = max(maxC, c);
    char mx[12];
    snprintf(mx, sizeof(mx), "max %d", maxC);
    text(mx, 8 + chartW - 10, y + 13, C_MUTED, lgfx::middle_right);
    const int gx = 18, gy = y + 26, gw = chartW - 20, gh = boxH - 50;
    cv->drawFastHLine(gx, gy + gh, gw, C_BORDER);
    cv->drawFastHLine(gx, gy + gh / 2, gw, mix(C_CARD, C_BORDER, 128));
    const int slot = gw / model::TIMELINE_BUCKETS;
    for (int i = 0; i < model::TIMELINE_BUCKETS; ++i) {
        int bh = s.timeline[i] * gh / maxC;
        if (s.timeline[i] > 0 && bh < 2) bh = 2;
        uint32_t col = (i == model::TIMELINE_BUCKETS - 1) ? C_CYAN : mix(C_CARD, C_CYAN, 170);
        cv->fillRoundRect(gx + i * slot + 2, gy + gh - bh, slot - 4, bh, 2, col);
    }
    // Server labels are UTC; show the window start in local time instead.
    char startLabel[8] = "-60m";
    time_t start = time(nullptr) - 3600;
    if (start > 1600000000) {
        struct tm tm;
        localtime_r(&start, &tm);
        strftime(startLabel, sizeof(startLabel), "%H:%M", &tm);
    }
    text(startLabel, gx, y + boxH - 12, C_MUTED);
    text("now", gx + gw, y + boxH - 12, C_MUTED, lgfx::middle_right);

    // Host metrics.
    const int hx = 8 + chartW + 8, hw = W - hx - 8;
    card(hx, y, hw, boxH);
    font(F_SMALL);
    text("MESHPOINT HOST", hx + 10, y + 13, C_MUTED);
    const auto& h = m.host;
    struct Row { const char* k; char v[20]; uint32_t c; } rows[5];
    int nr = 0;
    auto addRow = [&](const char* k, uint32_t c) -> char* { rows[nr].k = k; rows[nr].c = c; return rows[nr++].v; };
    if (model::has(h.cpu)) snprintf(addRow("CPU", h.cpu > 80 ? C_AMBER : C_TEXT), 20, "%.0f%%", h.cpu);
    if (model::has(h.temp)) snprintf(addRow("Temp", h.temp > 70 ? C_RED : h.temp > 60 ? C_AMBER : C_TEXT), 20, "%.1f C", h.temp);
    if (model::has(h.mem)) snprintf(addRow("Memory", h.mem > 85 ? C_AMBER : C_TEXT), 20, "%.0f%%", h.mem);
    if (model::has(h.disk)) snprintf(addRow("Disk", h.disk > 90 ? C_RED : C_TEXT), 20, "%.0f%%", h.disk);
    fmtDuration(addRow("Uptime", C_TEXT), 20, h.valid ? h.uptime : s.uptime);
    for (int i = 0; i < nr; ++i) {
        int ry = y + 36 + i * 22;
        font(F_BODY);
        text(rows[i].k, hx + 10, ry, C_TEXT2);
        font(F_BODY_B);
        text(rows[i].v, hx + hw - 10, ry, rows[i].c, lgfx::middle_right);
    }

    // Packet types (scroll down to see).
    y += boxH + 8;
    const int typesH = 112;
    card(8, y, W - 16, typesH);
    font(F_SMALL);
    text("PACKET TYPES", 18, y + 13, C_MUTED);
    int total = 0;
    for (int i = 0; i < s.typeCount; ++i) total += s.types[i].count;
    if (total > 0) {
        int bx = 18, bw = W - 36, px = bx;
        for (int i = 0; i < s.typeCount; ++i) {
            int w = (i == s.typeCount - 1) ? bx + bw - px : s.types[i].count * bw / total;
            if (w > 0) cv->fillRect(px, y + 28, w, 12, typeColor(s.types[i].name));
            px += w;
        }
        for (int i = 0; i < s.typeCount; ++i) {
            int col = i % 3, row = i / 3;
            int lx = 18 + col * 150, ly = y + 60 + row * 26;
            cv->fillRoundRect(lx, ly - 5, 10, 10, 2, typeColor(s.types[i].name));
            char lbl[40];
            snprintf(lbl, sizeof(lbl), "%s %d%%", s.types[i].name, (int)(s.types[i].count * 100L / total));
            font(F_BODY);
            textFit(lbl, lx + 16, ly, 130, C_TEXT);
        }
    }

    // Reach & relay.
    y += typesH + 8;
    const int reachH = 110;
    card(8, y, W - 16, reachH);
    font(F_SMALL);
    text("REACH", 18, y + 13, C_MUTED);
    int dr = s.direct + s.relayed;
    char line[80];
    font(F_BODY);
    if (dr > 0) {
        snprintf(line, sizeof(line), "Direct %d%%   Relayed %d%%", (int)(s.direct * 100L / dr), (int)(s.relayed * 100L / dr));
        text(line, 18, y + 36, C_TEXT);
    }
    if (s.farthestMiles > 0) {
        snprintf(line, sizeof(line), "Farthest via mesh: %.1f mi (%s)", s.farthestMiles, s.farthestName);
        textFit(line, 18, y + 60, W - 52, C_TEXT);
    }
    if (s.relayEnabled) {
        snprintf(line, sizeof(line), "Relay on (%d sent)   Region %s   v%s", s.relayCount, s.region, s.firmware);
    } else {
        snprintf(line, sizeof(line), "Relay off   Region %s   v%s", s.region, s.firmware);
    }
    textFit(line, 18, y + 84, W - 52, C_TEXT2);

    return (y + reachH + 8) - y0;
}

// ---- Nodes ----

// Short name for the badge; nodes without one show the tail of their id.
void badgeText(const model::Node& n, char* out, size_t len) {
    if (n.shortName[0]) {
        strlcpy(out, n.shortName, len);
        return;
    }
    size_t idLen = strlen(n.id);
    strlcpy(out, idLen > 4 ? n.id + idLen - 4 : n.id, len);
}

constexpr int NODE_ROW_H = 44;

int drawNodes(const model::Model& m, int y0) {
    if (drawNotice(m, m.nodeCount > 0 || m.nodesStamp != 0)) return CONTENT_H;
    int y = y0 + 4;
    font(F_SMALL);
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "%d nodes, most recent first", m.nodeCount);
    text(hdr, 12, y + 10, C_MUTED);
    text("SIGNAL", 318, y + 10, C_MUTED, lgfx::middle_center);
    text("BATT", 398, y + 10, C_MUTED, lgfx::middle_center);
    text("HEARD", W - 12, y + 10, C_MUTED, lgfx::middle_right);
    y += 22;

    for (int i = 0; i < m.nodeCount; ++i, y += NODE_ROW_H) {
        if (y + NODE_ROW_H < CONTENT_Y || y > CONTENT_Y + CONTENT_H) continue;  // off-screen
        const model::Node& n = m.nodes[i];
        if (i % 2 == 0) cv->fillRect(0, y, W, NODE_ROW_H, mix(C_BG, C_CARD, 110));

        // Badge with short name.
        cv->fillRoundRect(8, y + 8, 48, 28, 6, idColor(n.id));
        char sn[8];
        badgeText(n, sn, sizeof(sn));
        font(F_BODY_B);
        if (cv->textWidth(sn) > 44) font(F_SMALL);
        text(sn, 32, y + 23, 0xFFFFFFu, lgfx::middle_center);

        font(F_BODY_B);
        textFit(n.longName[0] ? n.longName : n.id, 66, y + 14, 210, C_TEXT);
        char meta[64];
        snprintf(meta, sizeof(meta), "%s%s%s", n.hw[0] ? n.hw : n.id, n.role[0] ? " - " : "", n.role);
        font(F_SMALL);
        textFit(meta, 66, y + 33, 210, C_MUTED);

        // Signal.
        char sig[24];
        if (model::has(n.rssi)) {
            snprintf(sig, sizeof(sig), "%.0f", n.rssi);
            font(F_BODY_B);
            text(sig, 318, y + 15, rssiColor(n.rssi), lgfx::middle_center);
            font(F_SMALL);
            if (model::has(n.snr)) snprintf(sig, sizeof(sig), "%.1fdB", n.snr);
            else strcpy(sig, "");
            text(sig, 318, y + 33, C_TEXT2, lgfx::middle_center);
        } else {
            font(F_BODY);
            text("-", 318, y + 22, C_MUTED, lgfx::middle_center);
        }

        // Battery + hops.
        font(F_BODY);
        char bat[12];
        if (model::has(n.battery) && n.battery > 100) strcpy(bat, "PWR");
        else if (model::has(n.battery)) snprintf(bat, sizeof(bat), "%.0f%%", n.battery);
        else strcpy(bat, "-");
        uint32_t bcol = !model::has(n.battery) ? C_MUTED : n.battery > 100 ? C_CYAN : n.battery < 20 ? C_RED : n.battery < 40 ? C_AMBER : C_TEXT;
        text(bat, 398, y + 15, bcol, lgfx::middle_center);
        font(F_SMALL);
        char hops[12];
        if (n.hops == 0) strcpy(hops, "direct");
        else if (n.hops > 0) snprintf(hops, sizeof(hops), "%d hop%s", n.hops, n.hops == 1 ? "" : "s");
        else hops[0] = 0;
        text(hops, 398, y + 33, C_TEXT2, lgfx::middle_center);

        char age[12];
        fmtAge(age, sizeof(age), n.lastHeard);
        font(F_BODY_B);
        text(age, W - 12, y + 22, C_TEXT, lgfx::middle_right);

        addTarget(0, y, W, NODE_ROW_H, Action::OpenNode, i);
    }
    return (y + 8) - y0;
}

int drawNodeDetail(const model::Model& m, int y0) {
    const model::Node* n = nullptr;
    for (int i = 0; i < m.nodeCount; ++i) {
        if (!strcmp(m.nodes[i].id, openNode)) n = &m.nodes[i];
    }
    int y = y0 + 8;
    backButton(y);
    if (!n) {
        font(F_BODY);
        text("Node no longer in list", 100, y + 16, C_TEXT2);
        return CONTENT_H;
    }
    cv->fillRoundRect(96, y, 52, 30, 6, idColor(n->id));
    char sn[8];
    badgeText(*n, sn, sizeof(sn));
    font(F_BODY_B);
    if (cv->textWidth(sn) > 48) font(F_SMALL);
    text(sn, 122, y + 16, 0xFFFFFFu, lgfx::middle_center);
    font(F_TITLE);
    textFit(n->longName[0] ? n->longName : n->id, 158, y + 16, W - 170, C_TEXT);
    y += 42;

    struct KV { const char* k; char v[48]; uint32_t c; };
    KV kv[20];
    int nk = 0;
    auto add = [&](const char* k, uint32_t c = C_TEXT) -> char* { kv[nk].k = k; kv[nk].c = c; kv[nk].v[0] = 0; return kv[nk++].v; };
    strlcpy(add("Node ID"), n->id, 48);
    strlcpy(add("Protocol", protocolColor(n->protocol)), n->protocol, 48);
    strlcpy(add("Hardware"), n->hw[0] ? n->hw : "-", 48);
    strlcpy(add("Role"), n->role[0] ? n->role : "-", 48);
    char age[12];
    fmtAge(age, sizeof(age), n->lastHeard);
    snprintf(add("Last heard"), 48, "%s ago", age);
    fmtAge(age, sizeof(age), n->firstSeen);
    snprintf(add("First seen"), 48, "%s ago", age);
    snprintf(add("Packets"), 48, "%lu", (unsigned long)n->packets);
    if (model::has(n->rssi)) snprintf(add("RSSI", rssiColor(n->rssi)), 48, "%.0f dBm", n->rssi);
    if (model::has(n->snr)) snprintf(add("SNR"), 48, "%.1f dB", n->snr);
    if (n->hops >= 0) snprintf(add("Hops"), 48, "%d", n->hops);
    if (model::has(n->battery)) {
        if (n->battery > 100) strlcpy(add("Battery", C_CYAN), "Powered", 48);
        else snprintf(add("Battery"), 48, "%.0f%%", n->battery);
    }
    if (model::has(n->voltage)) snprintf(add("Voltage"), 48, "%.2f V", n->voltage);
    if (model::has(n->temperature)) snprintf(add("Temperature"), 48, "%.1f C", n->temperature);
    if (model::has(n->humidity)) snprintf(add("Humidity"), 48, "%.0f%%", n->humidity);
    if (model::has(n->chUtil)) snprintf(add("Channel util"), 48, "%.1f%%", n->chUtil);
    if (model::has(n->airUtil)) snprintf(add("Air util TX"), 48, "%.1f%%", n->airUtil);
    if (n->firmware[0]) strlcpy(add("Firmware"), n->firmware, 48);
    if (n->hasPosition) snprintf(add("Position"), 48, "%.4f, %.4f", n->lat, n->lon);

    // Two columns of key/value rows.
    const int colW = (W - 24) / 2, rowH = 26;
    int rows = (nk + 1) / 2;
    card(8, y, W - 16, rows * rowH + 12);
    for (int i = 0; i < nk; ++i) {
        int col = i / rows, row = i % rows;
        int x = 16 + col * colW, ry = y + 6 + row * rowH + rowH / 2;
        font(F_SMALL);
        text(kv[i].k, x, ry, C_MUTED);
        font(F_BODY_B);
        textFit(kv[i].v, x + colW - 16, ry, colW - 110, kv[i].c, lgfx::middle_right);
    }
    return (y + rows * rowH + 20) - y0;
}

// ---- Feed ----

constexpr int FEED_ROW_H = 40;

int drawFeed(const model::Model& m, int y0) {
    if (drawNotice(m, m.packetCount > 0 || m.packetsStamp != 0)) return CONTENT_H;
    int y = y0 + 4;
    for (int i = 0; i < m.packetCount; ++i, y += FEED_ROW_H) {
        if (y + FEED_ROW_H < CONTENT_Y || y > CONTENT_Y + CONTENT_H) continue;
        const model::Packet& p = m.packets[i];
        if (i % 2 == 0) cv->fillRect(0, y, W, FEED_ROW_H, mix(C_BG, C_CARD, 110));
        char age[12];
        fmtAge(age, sizeof(age), p.ts);
        font(F_BODY_B);
        text(age, 44, y + FEED_ROW_H / 2, C_TEXT2, lgfx::middle_right);

        chip(p.type[0] ? p.type : "?", 52, y + FEED_ROW_H / 2, typeColor(p.type), 92);

        const char* who = model::nodeName(p.source);
        bool hasText = p.text[0] && (!strcmp(p.type, "text") || !strcmp(p.type, "range_test"));
        const int nameY = hasText ? y + 12 : y + FEED_ROW_H / 2;
        cv->fillCircle(156, nameY, 4, protocolColor(p.protocol));
        font(F_BODY_B);
        textFit(who, 166, nameY, 196, C_TEXT);
        if (hasText) {
            font(F_SMALL);
            textFit(p.text, 166, y + 30, 196, C_GREEN);
        }

        char sig[24];
        if (model::has(p.rssi)) {
            snprintf(sig, sizeof(sig), "%.0f", p.rssi);
            font(F_BODY_B);
            text(sig, 400, y + 13, rssiColor(p.rssi), lgfx::middle_right);
            font(F_SMALL);
            if (model::has(p.snr)) {
                snprintf(sig, sizeof(sig), "%.1fdB", p.snr);
                text(sig, 400, y + 30, C_TEXT2, lgfx::middle_right);
            }
        }
        font(F_SMALL);
        char hops[12] = "";
        if (p.hops == 0) strcpy(hops, "direct");
        else if (p.hops > 0) snprintf(hops, sizeof(hops), "%d hop%s", p.hops, p.hops == 1 ? "" : "s");
        text(hops, W - 10, y + FEED_ROW_H / 2, C_TEXT2, lgfx::middle_right);
    }
    if (m.packetCount == 0) {
        font(F_BODY);
        text("No packets captured yet", W / 2, CONTENT_Y + CONTENT_H / 2, C_TEXT2, lgfx::middle_center);
    }
    return (y + 8) - y0;
}

// ---- Chat ----

constexpr int CONV_ROW_H = 52;

int drawConversations(const model::Model& m, int y0) {
    if (drawNotice(m, m.convCount > 0 || m.status.link == model::Link::Ok)) return CONTENT_H;
    int y = y0 + 4;
    if (m.convCount == 0) {
        font(F_BODY);
        text("No conversations yet", W / 2, CONTENT_Y + CONTENT_H / 2, C_TEXT2, lgfx::middle_center);
        return CONTENT_H;
    }
    for (int i = 0; i < m.convCount; ++i, y += CONV_ROW_H) {
        if (y + CONV_ROW_H < CONTENT_Y || y > CONTENT_Y + CONTENT_H) continue;
        const model::Conversation& c = m.convs[i];
        if (i % 2 == 0) cv->fillRect(0, y, W, CONV_ROW_H, mix(C_BG, C_CARD, 110));
        // Channel vs DM marker.
        uint32_t ic = c.broadcast ? C_CYAN : idColor(c.id);
        cv->fillCircle(28, y + CONV_ROW_H / 2, 16, ic);
        font(F_BODY_B);
        text(c.broadcast ? "#" : "@", 28, y + CONV_ROW_H / 2 + 1, 0xFFFFFFu, lgfx::middle_center);

        char age[12];
        fmtAge(age, sizeof(age), c.ts);
        font(F_SMALL);
        text(age, W - 12, y + 15, C_TEXT2, lgfx::middle_right);

        font(F_BODY_B);
        textFit(c.name, 54, y + 15, W - 54 - 70, c.unread ? C_TEXT : C_TEXT2);
        font(F_BODY);
        textFit(c.last, 54, y + 36, W - 54 - 50, c.unread ? C_TEXT : C_MUTED);
        if (c.unread) {
            char b[6];
            snprintf(b, sizeof(b), "%d", min(c.unread, 99));
            cv->fillRoundRect(W - 36, y + 28, 26, 18, 9, C_CYAN);
            font(F_SMALL);
            text(b, W - 23, y + 37, C_BG, lgfx::middle_center);
        }
        addTarget(0, y, W, CONV_ROW_H, Action::OpenConv, i);
    }
    return (y + 8) - y0;
}

int drawConversation(const model::Model& m, int y0) {
    // Title bar is drawn fixed (outside the scroll) by the caller; content starts below it.
    int y = y0;
    if (m.messageCount == 0) {
        font(F_BODY);
        text(m.messagesLoading ? "Loading..." : "No messages", W / 2, CONTENT_Y + CONTENT_H / 2 + 20, C_TEXT2,
             lgfx::middle_center);
        return CONTENT_H - 46;
    }
    const int bubbleMaxW = 340;
    for (int i = 0; i < m.messageCount; ++i) {
        const model::Message& msg = m.messages[i];
        bool mine = !strcmp(msg.direction, "sent");
        char lines[6][120];
        font(F_BODY);
        int nl = wrap(msg.text, bubbleMaxW - 20, lines, 6);
        int textW = 0;
        for (int l = 0; l < nl; ++l) textW = max(textW, (int)cv->textWidth(lines[l]));
        char meta[80], age[12];
        fmtAge(age, sizeof(age), msg.ts);
        if (mine) snprintf(meta, sizeof(meta), "You - %s", age);
        else if (model::has(msg.rssi)) snprintf(meta, sizeof(meta), "%s - %s - %.0fdBm", msg.from, age, msg.rssi);
        else snprintf(meta, sizeof(meta), "%s - %s", msg.from, age);
        font(F_SMALL);
        int metaW = min((int)cv->textWidth(meta), bubbleMaxW - 20);
        int bw = max(textW, metaW) + 20;
        int bh = 22 + nl * 20 + 6;
        int bx = mine ? W - 10 - bw : 10;
        bool visible = !(y + bh < CONTENT_Y || y > CONTENT_Y + CONTENT_H);
        if (visible) {
            cv->fillRoundRect(bx, y, bw, bh, 10, mine ? mix(C_BG, C_CYAN, 70) : C_CARD);
            cv->drawRoundRect(bx, y, bw, bh, 10, mine ? mix(C_BG, C_CYAN, 140) : C_BORDER);
            font(F_SMALL);
            textFit(meta, bx + 10, y + 12, bubbleMaxW - 20, mine ? C_CYAN : C_TEXT2);
            font(F_BODY);
            for (int l = 0; l < nl; ++l) text(lines[l], bx + 10, y + 32 + l * 20, C_TEXT);
        }
        y += bh + 8;
    }
    return y - y0;
}

// ---- Map ----
//
// Plain-background map of positioned nodes and heard links. Equirectangular projection around
// the view centre (fine at mesh scales). Nodes are heat-coloured by how recently they were
// heard; links are coloured the same way but dimmer so the nodes stand out.

constexpr double M_PER_DEG_LAT = 111320.0;
constexpr double MIN_MPP = 2.0;       // metres per pixel, most zoomed in
constexpr double MAX_MPP = 40000.0;   // most zoomed out
constexpr int MAP_CX = W / 2;
constexpr int MAP_CY = CONTENT_Y + CONTENT_H / 2;
constexpr int PICK_RADIUS = 18;       // tap tolerance for selecting a node

bool mapFitted = false;
double viewLat = 0, viewLon = 0, mpp = 100;
double downLat = 0, downLon = 0;      // view centre when a drag started
char mapSelected[16] = "";
uint32_t lastMapTapMs = 0;
int lastMapTapX = 0, lastMapTapY = 0;

double mPerDegLon() { return M_PER_DEG_LAT * cos(viewLat * DEG_TO_RAD); }

void project(double lat, double lon, int& x, int& y) {
    x = MAP_CX + (int)lround((lon - viewLon) * mPerDegLon() / mpp);
    y = MAP_CY - (int)lround((lat - viewLat) * M_PER_DEG_LAT / mpp);
}

void unproject(int x, int y, double& lat, double& lon) {
    lat = viewLat - (y - MAP_CY) * mpp / M_PER_DEG_LAT;
    lon = viewLon + (x - MAP_CX) * mpp / mPerDegLon();
}

double distanceM(double lat1, double lon1, double lat2, double lon2) {
    const double dLat = (lat2 - lat1) * DEG_TO_RAD, dLon = (lon2 - lon1) * DEG_TO_RAD;
    const double a = sin(dLat / 2) * sin(dLat / 2) +
                     cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) * sin(dLon / 2) * sin(dLon / 2);
    return 6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

// Heat scale: position along the bar grows with log(age) from "now" to 30 days.
constexpr float HEAT_MAX_AGE = 30 * 86400.0f;
float heatPos(float ageSec) {
    if (ageSec < 0) ageSec = 0;
    return constrain(logf(1 + ageSec / 60) / logf(1 + HEAT_MAX_AGE / 60), 0.0f, 1.0f);
}

struct HeatStop {
    float ageSec;
    uint32_t color;
};
const HeatStop kHeat[] = {
    {0, 0xFFF4C2u},           // white-hot
    {15 * 60, 0xFFD23Fu},     // yellow
    {3600, 0xFF9F1Cu},        // orange
    {6 * 3600, 0xF2542Du},    // red-orange
    {86400, 0xC0263Fu},       // crimson
    {7 * 86400, 0x7A2A6Eu},   // plum
    {30 * 86400, 0x3B4A66u},  // cold slate
};

uint32_t heatAt(float pos) {
    const int n = sizeof(kHeat) / sizeof(kHeat[0]);
    for (int i = 1; i < n; ++i) {
        float p0 = heatPos(kHeat[i - 1].ageSec), p1 = heatPos(kHeat[i].ageSec);
        if (pos <= p1) return mix(kHeat[i - 1].color, kHeat[i].color, (uint8_t)(255 * (pos - p0) / (p1 - p0)));
    }
    return kHeat[n - 1].color;
}

uint32_t heatColor(time_t ts) {
    time_t now = time(nullptr);
    if (ts <= 0 || now < 1600000000) return kHeat[5].color;
    return heatAt(heatPos((float)(now - ts)));
}

// Round a distance down to 1/2/5 x 10^n in miles (or feet for short ranges).
void niceDistance(double metres, double& niceM, char* label, size_t n) {
    const double miles = metres / 1609.344;
    if (miles < 0.19) {
        const double feet = metres * 3.28084;
        double p = pow(10, floor(log10(feet)));
        double s = feet / p >= 5 ? 5 : feet / p >= 2 ? 2 : 1;
        niceM = s * p / 3.28084;
        snprintf(label, n, "%.0f ft", s * p);
        return;
    }
    double p = pow(10, floor(log10(miles)));
    double s = miles / p >= 5 ? 5 : miles / p >= 2 ? 2 : 1;
    niceM = s * p * 1609.344;
    if (s * p < 1) snprintf(label, n, "%.1f mi", s * p);
    else snprintf(label, n, "%.0f mi", s * p);
}

// Frame the Meshpoint plus its nearest 85% of recently heard nodes (last 7 days; all nodes if
// fewer than 5). Dropping the farthest 15% keeps a few distant or bogus positions from zooming
// the whole map out.
void fitMap(const model::Model& m) {
    struct P {
        double lat, lon, d;
    };
    static P pts[model::MAX_MAP_NODES];
    const time_t now = time(nullptr);
    double refLat = m.homeLat, refLon = m.homeLon;
    if (!m.hasHome && m.mapNodeCount > 0) {
        // No Meshpoint position: use the median node as the reference point.
        static double lats[model::MAX_MAP_NODES], lons[model::MAX_MAP_NODES];
        for (int i = 0; i < m.mapNodeCount; ++i) lats[i] = m.mapNodes[i].lat, lons[i] = m.mapNodes[i].lon;
        std::sort(lats, lats + m.mapNodeCount);
        std::sort(lons, lons + m.mapNodeCount);
        refLat = lats[m.mapNodeCount / 2];
        refLon = lons[m.mapNodeCount / 2];
    }
    int k = 0;
    for (int pass = 0; pass < 2 && k < 5; ++pass) {
        k = 0;
        for (int i = 0; i < m.mapNodeCount; ++i) {
            const model::MapNode& n = m.mapNodes[i];
            const bool recent = now > 1600000000 && now - n.lastHeard < 7 * 86400;
            if (pass == 0 && !recent) continue;
            pts[k++] = {n.lat, n.lon, distanceM(refLat, refLon, n.lat, n.lon)};
        }
    }
    if (k == 0 && !m.hasHome) return;
    std::sort(pts, pts + k, [](const P& a, const P& b) { return a.d < b.d; });
    const int keep = k > 5 ? (int)ceil(k * 0.85) : k;
    double latMin = refLat, latMax = refLat, lonMin = refLon, lonMax = refLon;
    for (int i = 0; i < keep; ++i) {
        latMin = min(latMin, pts[i].lat), latMax = max(latMax, pts[i].lat);
        lonMin = min(lonMin, pts[i].lon), lonMax = max(lonMax, pts[i].lon);
    }
    viewLat = (latMin + latMax) / 2;
    viewLon = (lonMin + lonMax) / 2;
    const double hM = (latMax - latMin) * M_PER_DEG_LAT, wM = (lonMax - lonMin) * mPerDegLon();
    // Leave room for the zoom buttons on the right and some margin all round.
    mpp = constrain(max(max(wM / (W - 140), hM / (CONTENT_H - 50)), 4000.0 / CONTENT_H), MIN_MPP, MAX_MPP);
    mapFitted = true;
}

void zoomBy(double factor) { mpp = constrain(mpp * factor, MIN_MPP, MAX_MPP); }

// Zoom buttons: keep the selected node in view by zooming around it.
void zoomButton(double factor) {
    model::Guard g;
    const model::Model& m = model::data();
    for (int i = 0; mapSelected[0] && i < m.mapNodeCount; ++i) {
        if (!strcmp(m.mapNodes[i].id, mapSelected)) {
            viewLat = m.mapNodes[i].lat;
            viewLon = m.mapNodes[i].lon;
            break;
        }
    }
    zoomBy(factor);
}

int drawMap(const model::Model& m) {
    if (!m.mapStamp && drawNotice(m, false)) return CONTENT_H;
    if (!mapFitted) fitMap(m);
    const time_t now = time(nullptr);
    const bool clockOk = now > 1600000000;

    // Range rings around the Meshpoint.
    double ringM;
    char ringLabel[16];
    niceDistance(mpp * 70, ringM, ringLabel, sizeof(ringLabel));
    int hx = MAP_CX, hy = MAP_CY;
    if (m.hasHome) {
        project(m.homeLat, m.homeLon, hx, hy);
        const int step = (int)(ringM / mpp);
        font(F_SMALL);
        for (int r = 1; r <= 6 && step > 8; ++r) {
            cv->drawCircle(hx, hy, r * step, mix(C_BG, C_BORDER, 200));
            if (r <= 3) {
                char lbl[20];
                double v = strtod(ringLabel, nullptr) * r;
                snprintf(lbl, sizeof(lbl), strchr(ringLabel, '.') ? "%.1f%s" : "%.0f%s", v, strchr(ringLabel, ' '));
                text(lbl, hx + 3, hy - r * step - 7, C_MUTED);
            }
        }
    }

    // Selected node index (if still present).
    int sel = -1;
    for (int i = 0; mapSelected[0] && i < m.mapNodeCount; ++i) {
        if (!strcmp(m.mapNodes[i].id, mapSelected)) sel = i;
    }

    // Links: oldest first so recent ones draw on top; stale links fade almost to the background.
    auto onScreen = [](int x, int y) { return x > -40 && x < W + 40 && y > CONTENT_Y - 40 && y < TAB_Y + 40; };
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < m.linkCount; ++i) {
            const model::MeshLink& l = m.links[i];
            const bool involved = (l.a == sel || l.b == sel);
            if ((pass == 1) != involved) continue;  // selected node's links last, on top
            int x1, y1, x2, y2;
            project(m.mapNodes[l.a].lat, m.mapNodes[l.a].lon, x1, y1);
            project(m.mapNodes[l.b].lat, m.mapNodes[l.b].lon, x2, y2);
            if (!onScreen(x1, y1) && !onScreen(x2, y2)) {
                // Both ends off-screen: skip unless the segment could cross the view.
                if ((x1 < 0 && x2 < 0) || (x1 > W && x2 > W) || (y1 < CONTENT_Y && y2 < CONTENT_Y) ||
                    (y1 > TAB_Y && y2 > TAB_Y))
                    continue;
            }
            uint32_t col = heatColor(l.lastSeen);
            float age = clockOk ? (float)(now - l.lastSeen) : HEAT_MAX_AGE;
            if (involved) {
                cv->drawWideLine(x1, y1, x2, y2, 1.5f, col);
            } else {
                cv->drawLine(x1, y1, x2, y2, mix(C_BG, col, age > 7 * 86400 ? 45 : 130));
            }
        }
    }

    // Nodes: coldest first so the hottest are on top.
    static int16_t order[model::MAX_MAP_NODES];
    for (int i = 0; i < m.mapNodeCount; ++i) order[i] = i;
    std::sort(order, order + m.mapNodeCount,
              [&](int16_t a, int16_t b) { return m.mapNodes[a].lastHeard < m.mapNodes[b].lastHeard; });
    const uint32_t ms = millis();
    int visible = 0;
    for (int k = 0; k < m.mapNodeCount; ++k) {
        const model::MapNode& n = m.mapNodes[order[k]];
        int x, y;
        project(n.lat, n.lon, x, y);
        if (x < -10 || x > W + 10 || y < CONTENT_Y - 10 || y > TAB_Y + 10) continue;
        ++visible;
        float age = clockOk ? (float)(now - n.lastHeard) : HEAT_MAX_AGE;
        uint32_t col = heatColor(n.lastHeard);
        int r = age < 3600 ? 5 : age < 7 * 86400 ? 4 : 3;
        if (age < 15 * 60) cv->fillCircle(x, y, r + 4, mix(C_BG, col, 70));  // glow for very recent
        cv->fillCircle(x, y, r, col);
        if (n.meshcore) cv->drawCircle(x, y, r + 1, C_PURPLE);
        if (n.newUntilMs && (int32_t)(n.newUntilMs - ms) > 0) {
            int pr = r + 6 + (int)((ms / 120) % 6);  // pulsing ring on newly added nodes
            cv->drawCircle(x, y, pr, C_CYAN);
        }
        if (order[k] == sel) {
            cv->drawCircle(x, y, r + 5, C_TEXT);
            cv->drawCircle(x, y, r + 6, C_TEXT);
        }
    }

    // Labels when zoomed in enough, hottest first, skipping overlaps.
    if (visible <= 60) {
        font(F_SMALL);
        struct Box { int16_t x, y, w, h; } boxes[48];
        int nb = 0;
        for (int k = m.mapNodeCount - 1; k >= 0 && nb < 48; --k) {
            const model::MapNode& n = m.mapNodes[order[k]];
            int x, y;
            project(n.lat, n.lon, x, y);
            if (x < 0 || x > W || y < CONTENT_Y + 8 || y > TAB_Y - 8) continue;
            char name[20];
            strlcpy(name, n.name, sizeof(name));
            int w = cv->textWidth(name), bx = x + 8, by = y - 7;
            bool clash = false;
            for (int b = 0; b < nb && !clash; ++b) {
                clash = bx < boxes[b].x + boxes[b].w && boxes[b].x < bx + w && by < boxes[b].y + boxes[b].h &&
                        boxes[b].y < by + 14;
            }
            if (clash) continue;
            boxes[nb++] = {(int16_t)bx, (int16_t)by, (int16_t)w, 14};
            text(name, bx, y, mix(C_BG, C_TEXT2, 220));
        }
    }

    // The Meshpoint itself.
    if (m.hasHome) {
        cv->fillRect(hx - 6, hy - 6, 13, 13, C_BG);
        cv->drawRect(hx - 6, hy - 6, 13, 13, C_CYAN);
        cv->fillRect(hx - 2, hy - 2, 5, 5, C_CYAN);
    }

    // Legend: heat bar + counts.
    const int lx = 8, ly = CONTENT_Y + 6, lw = 176;
    cv->fillRoundRect(lx, ly, lw, 44, 6, mix(C_BG, C_CARD, 230));
    cv->drawRoundRect(lx, ly, lw, 44, 6, C_BORDER);
    const int bx = lx + 8, bw = lw - 16;
    for (int i = 0; i < bw; ++i) cv->drawFastVLine(bx + i, ly + 6, 8, heatAt((float)i / (bw - 1)));
    font(F_SMALL);
    const struct { float age; const char* label; } ticks[] = {{0, "now"}, {3600, "1h"}, {86400, "1d"}, {HEAT_MAX_AGE, "30d+"}};
    for (auto& t : ticks) {
        int tx = bx + (int)(heatPos(t.age) * (bw - 1));
        lgfx::textdatum_t d = t.age == 0 ? lgfx::middle_left : t.age == HEAT_MAX_AGE ? lgfx::middle_right : lgfx::middle_center;
        text(t.label, tx, ly + 22, C_TEXT2, d);
    }
    char counts[48];
    snprintf(counts, sizeof(counts), "%d nodes  %d links", m.mapNodeCount, m.linkCount);
    text(counts, bx, ly + 36, C_MUTED);

    // Scale bar.
    double barM;
    char barLabel[16];
    niceDistance(mpp * 90, barM, barLabel, sizeof(barLabel));
    int barW = (int)(barM / mpp);
    const int sy = TAB_Y - 10;
    cv->drawFastHLine(10, sy, barW, C_TEXT2);
    cv->drawFastVLine(10, sy - 4, 5, C_TEXT2);
    cv->drawFastVLine(10 + barW, sy - 4, 5, C_TEXT2);
    text(barLabel, 16 + barW, sy - 1, C_TEXT2);

    // Zoom controls.
    const int zx = W - 52, zy = CONTENT_Y + 8;
    auto zbtn = [&](int y, const char* label, Action a) {
        cv->fillRoundRect(zx, y, 44, 40, 8, mix(C_BG, C_CARD, 230));
        cv->drawRoundRect(zx, y, 44, 40, 8, C_BORDER);
        font(F_TITLE);
        text(label, zx + 22, y + 21, C_TEXT, lgfx::middle_center);
        addTarget(zx, y, 44, 40, a);
    };
    zbtn(zy, "+", Action::ZoomIn);
    zbtn(zy + 46, "-", Action::ZoomOut);
    cv->fillRoundRect(zx, zy + 92, 44, 40, 8, mix(C_BG, C_CARD, 230));
    cv->drawRoundRect(zx, zy + 92, 44, 40, 8, C_BORDER);
    cv->drawCircle(zx + 22, zy + 112, 9, C_TEXT);
    cv->fillCircle(zx + 22, zy + 112, 3, C_TEXT);
    addTarget(zx, zy + 92, 44, 40, Action::ZoomFit);

    // Selected node info card.
    if (sel >= 0) {
        const model::MapNode& n = m.mapNodes[sel];
        const int cx = 8, cw = W - 16 - 60, ch = 50, cy = TAB_Y - ch - 20;
        cv->fillRoundRect(cx, cy, cw, ch, 8, mix(C_BG, C_CARD, 240));
        cv->drawRoundRect(cx, cy, cw, ch, 8, heatColor(n.lastHeard));
        font(F_BODY_B);
        textFit(n.name, cx + 10, cy + 14, cw - 20, C_TEXT);
        int links = 0;
        for (int i = 0; i < m.linkCount; ++i) links += (m.links[i].a == sel || m.links[i].b == sel);
        char age[12], line[96], dist[20] = "";
        fmtAge(age, sizeof(age), n.lastHeard);
        if (m.hasHome) snprintf(dist, sizeof(dist), "  %.1f mi", distanceM(m.homeLat, m.homeLon, n.lat, n.lon) / 1609.344);
        if (model::has(n.rssi)) {
            snprintf(line, sizeof(line), "heard %s ago  %.0f dBm%s  %d link%s", age, n.rssi, dist, links, links == 1 ? "" : "s");
        } else {
            snprintf(line, sizeof(line), "heard %s ago%s  %d link%s", age, dist, links, links == 1 ? "" : "s");
        }
        font(F_SMALL);
        textFit(line, cx + 10, cy + 36, cw - 20, C_TEXT2);
    }
    return CONTENT_H;
}

// Tap on the map background: select the nearest node, or double-tap to zoom in there.
void mapTap(int x, int y) {
    model::Guard g;
    const model::Model& m = model::data();
    int best = -1, bestD = PICK_RADIUS * PICK_RADIUS;
    for (int i = 0; i < m.mapNodeCount; ++i) {
        int nx, ny;
        project(m.mapNodes[i].lat, m.mapNodes[i].lon, nx, ny);
        int d = (nx - x) * (nx - x) + (ny - y) * (ny - y);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    const uint32_t now = millis();
    const bool doubleTap = now - lastMapTapMs < 350 && abs(x - lastMapTapX) < 30 && abs(y - lastMapTapY) < 30;
    lastMapTapMs = now;
    lastMapTapX = x;
    lastMapTapY = y;
    if (best >= 0) {
        strlcpy(mapSelected, m.mapNodes[best].id, sizeof(mapSelected));
    } else if (doubleTap) {
        unproject(x, y, viewLat, viewLon);
        zoomBy(0.5);
        lastMapTapMs = 0;
    } else {
        mapSelected[0] = 0;
    }
    dirty = true;
}

// ---- Settings ----

// Time zone picker (replaces the Settings page while open).
constexpr int TZ_ROW_H = 38;

int drawTimezonePicker(int y0) {
    const Settings& s = config::get();
    int y = y0 + 8;
    backButton(y);
    font(F_TITLE);
    text("Time zone", 96, y + 16, C_TEXT);
    y += 42;
    for (int i = 0; i < kTimeZoneCount; ++i, y += TZ_ROW_H) {
        if (y + TZ_ROW_H < CONTENT_Y || y > CONTENT_Y + CONTENT_H) continue;
        const bool current = !strcmp(kTimeZones[i].posix, s.tz.c_str());
        if (current) cv->fillRect(0, y, W, TZ_ROW_H, mix(C_BG, C_CYAN, 45));
        else if (i % 2 == 0) cv->fillRect(0, y, W, TZ_ROW_H, mix(C_BG, C_CARD, 110));
        font(F_BODY_B);
        text(kTimeZones[i].label, 16, y + TZ_ROW_H / 2, current ? C_CYAN : C_TEXT);
        font(F_SMALL);
        textFit(kTimeZones[i].posix, W - 16, y + TZ_ROW_H / 2, 200, C_MUTED, lgfx::middle_right);
        addTarget(0, y, W, TZ_ROW_H, Action::PickTimezone, i);
    }
    font(F_SMALL);
    text("Other zones: use the setup portal or the 'tz' serial command.", 16, y + 14, C_MUTED);
    return (y + 30) - y0;
}

// Meshpoint software update banner (top of Settings) while an update is available or running.
int drawUpdateBanner(const model::Model& m, int y) {
    const model::UpdateInfo& u = m.update;
    using S = model::UpdateState;
    const bool show = u.available || u.state == S::Running || u.state == S::Done || u.state == S::Failed;
    if (!show) return y;
    const bool confirming = millis() < updateConfirmUntil && u.state != S::Running;
    uint32_t accent = C_AMBER;
    if (u.state == S::Done) accent = C_GREEN;
    if (u.state == S::Failed) accent = C_RED;
    const int h = 92;
    cv->fillRoundRect(8, y, W - 16, h, 8, mix(C_BG, accent, 35));
    cv->drawRoundRect(8, y, W - 16, h, 8, accent);
    char line[96];
    font(F_BODY_B);
    switch (u.state) {
        case S::Running: {
            static const char* const dots[] = {"", ".", "..", "..."};
            snprintf(line, sizeof(line), "Updating Meshpoint to v%s%s", u.remoteVersion, dots[(millis() / 500) % 4]);
            text(line, 20, y + 20, C_TEXT);
            font(F_BODY);
            snprintf(line, sizeof(line), "Step: %s", u.step[0] ? u.step : "starting");
            textFit(line, 20, y + 46, W - 48, C_TEXT2);
            font(F_SMALL);
            text("Takes a few minutes. The Meshpoint restarts at the end.", 20, y + 72, C_MUTED);
            break;
        }
        case S::Done:
            text("Update installed", 20, y + 20, C_GREEN);
            font(F_BODY);
            textFit(u.message, 20, y + 46, W - 48, C_TEXT2);
            font(F_SMALL);
            text("The display reconnects by itself when it's back.", 20, y + 72, C_MUTED);
            break;
        case S::Failed:
            text("Meshpoint update failed", 20, y + 20, C_RED);
            font(F_BODY);
            textFit(u.message, 20, y + 46, W - 150, C_TEXT2);
            button("Dismiss", W - 128, y + 46, 108, 38, Action::UpdateDismiss, C_RED);
            break;
        default:
            if (confirming) {
                snprintf(line, sizeof(line), "Install v%s now?", u.remoteVersion);
                text(line, 20, y + 20, C_AMBER);
                font(F_SMALL);
                text("It restarts and is offline", 20, y + 48, C_TEXT2);
                text("for a few minutes.", 20, y + 66, C_TEXT2);
                button("Cancel", W - 236, y + 46, 100, 38, Action::UpdateCancel, C_MUTED);
                button("Install", W - 128, y + 46, 108, 38, Action::UpdateConfirm, C_AMBER);
            } else {
                text("Meshpoint update available", 20, y + 20, C_AMBER);
                font(F_BODY);
                snprintf(line, sizeof(line), "v%s  ->  v%s", u.localVersion, u.remoteVersion);
                text(line, 20, y + 46, C_TEXT);
                font(F_SMALL);
                snprintf(line, sizeof(line), "channel: %s", u.channel);
                text(line, 20, y + 70, C_MUTED);
                if (!strcmp(m.status.role, "admin")) {
                    button("Update", W - 128, y + 46, 108, 38, Action::UpdateStart, C_AMBER);
                } else {
                    font(F_SMALL);
                    text("Log in as admin", W - 20, y + 56, C_TEXT2, lgfx::middle_right);
                    text("to install", W - 20, y + 72, C_TEXT2, lgfx::middle_right);
                }
            }
            break;
    }
    return y + h + 10;
}

int drawSettings(const model::Model& m, int y0) {
    const Settings& s = config::get();
    int y = drawUpdateBanner(m, y0 + 8);
    struct KV { const char* k; char v[96]; uint32_t c; };
    KV kv[8];
    int n = 0;
    auto add = [&](const char* k, uint32_t c = C_TEXT) -> char* { kv[n].k = k; kv[n].c = c; kv[n].v[0] = 0; return kv[n++].v; };

    if (WiFi.isConnected()) {
        snprintf(add("WiFi"), 96, "%s  %s  %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        strlcpy(add("WiFi", C_RED), "not connected", 96);
    }
    strlcpy(add("Meshpoint"), s.url.c_str(), 96);
    snprintf(add("Login"), 96, "%s%s%s", s.user.c_str(), m.status.role[0] ? "  role: " : "", m.status.role);
    const char* link = "connecting";
    uint32_t lc = C_AMBER;
    switch (m.status.link) {
        case model::Link::Ok: link = "connected"; lc = C_GREEN; break;
        case model::Link::NoWifi: link = "no WiFi"; lc = C_RED; break;
        case model::Link::AuthFailed: link = "login failed"; lc = C_RED; break;
        case model::Link::Error: link = "error"; lc = C_RED; break;
        default: break;
    }
    if (m.status.error[0] && m.status.link != model::Link::Ok) snprintf(add("Status", lc), 96, "%s: %s", link, m.status.error);
    else strlcpy(add("Status", lc), link, 96);
    if (m.summary.valid) snprintf(add("Node"), 96, "%s  v%s  %s", m.summary.name, m.summary.firmware, m.summary.region);
    {
        const model::UpdateInfo& u = m.update;
        if (u.state == model::UpdateState::Checking) strlcpy(add("Software"), "checking for updates...", 96);
        else if (u.available) snprintf(add("Software", C_AMBER), 96, "v%s available (installed v%s)", u.remoteVersion, u.localVersion);
        else if (u.checked && u.remoteVersion[0]) snprintf(add("Software", C_GREEN), 96, "up to date (v%s)", u.localVersion);
        else if (u.checked) strlcpy(add("Software"), "couldn't check GitHub for updates", 96);
    }
    {
        const char* tzLabel = timeZoneLabel(s.tz.c_str());
        if (tzLabel) snprintf(add("Time zone"), 96, "%s (%s)", tzLabel, s.tz.c_str());
        else snprintf(add("Time zone"), 96, "custom: %s", s.tz.c_str());
    }
    snprintf(add("Display"), 96, "brightness %d%%  %s  auto-dim %s", s.brightness * 100 / 255,
             s.flip ? "flipped" : "normal", s.autoDim ? "on" : "off");
    if (s.muted) strlcpy(add("Alerts", C_AMBER), "muted", 96);
    else snprintf(add("Alerts"), 96, "volume %d%%", s.volume);

    const int rowH = 26;
    card(8, y, W - 16, n * rowH + 10);
    for (int i = 0; i < n; ++i) {
        int ry = y + 5 + i * rowH + rowH / 2;
        font(F_SMALL);
        text(kv[i].k, 18, ry, C_MUTED);
        font(F_BODY);
        textFit(kv[i].v, 100, ry, W - 100 - 20, kv[i].c);
    }
    y += n * rowH + 10 + 10;

    const int bw = (W - 16 - 3 * 8) / 4, bh = 40;
    button("Flip", 8, y, bw, bh, Action::Flip);
    button("Dimmer", 8 + (bw + 8), y, bw, bh, Action::BrightDown);
    button("Brighter", 8 + 2 * (bw + 8), y, bw, bh, Action::BrightUp);
    button("Refresh", 8 + 3 * (bw + 8), y, bw, bh, Action::Refresh);
    y += bh + 8;
    button(s.autoDim ? "Auto-dim after 5 min: ON" : "Auto-dim after 5 min: OFF", 8, y, W - 16, bh, Action::AutoDim,
           s.autoDim ? C_GREEN : C_MUTED);
    y += bh + 8;
    {
        const char* tzLabel = timeZoneLabel(s.tz.c_str());
        char tzText[80];
        snprintf(tzText, sizeof(tzText), "Time zone: %s", tzLabel ? tzLabel : "Custom");
        button(tzText, 8, y, W - 16, bh, Action::OpenTimezone);
    }
    y += bh + 16;

    // Alert sound controls.
    font(F_SMALL);
    text("ALERT SOUNDS", 12, y + 6, C_MUTED);
    y += 16;
    button(s.muted ? "Unmute" : "Mute", 8, y, bw, bh, Action::Mute, s.muted ? C_AMBER : C_CYAN);
    button("Vol -", 8 + (bw + 8), y, bw, bh, Action::VolDown);
    button("Vol +", 8 + 2 * (bw + 8), y, bw, bh, Action::VolUp);
    {
        // Volume meter.
        const int mx = 8 + 3 * (bw + 8);
        card(mx, y, bw, bh);
        const int barW = bw - 20, fill = barW * s.volume / 100;
        cv->fillRoundRect(mx + 10, y + bh - 14, barW, 6, 3, C_BORDER);
        cv->fillRoundRect(mx + 10, y + bh - 14, max(fill, 2), 6, 3, s.muted ? C_MUTED : C_CYAN);
        char v[8];
        snprintf(v, sizeof(v), "%d%%", s.volume);
        font(F_BODY_B);
        text(s.muted ? "muted" : v, mx + bw / 2, y + 14, s.muted ? C_AMBER : C_TEXT, lgfx::middle_center);
    }
    y += bh + 8;
    const int tw = (W - 16 - 2 * 8) / 3;
    button("Test channel", 8, y, tw, bh, Action::TestChannel, C_GREEN);
    button("Test DM", 8 + (tw + 8), y, tw, bh, Action::TestDM, C_GREEN);
    button("Test new node", 8 + 2 * (tw + 8), y, tw, bh, Action::TestNode, C_GREEN);
    y += bh + 16;
    button("Check for Meshpoint update", 8, y, W - 16, bh, Action::UpdateCheck);
    y += bh + 8;
    button("WiFi / Meshpoint setup portal", 8, y, W - 16, bh, Action::Portal, C_AMBER);
    y += bh + 8;
    font(F_SMALL);
    text("Portal: join WiFi 'MeshPointCYD-Setup', open 192.168.4.1", 12, y + 8, C_MUTED);
    text("USB serial (115200): type 'help' for commands", 12, y + 26, C_MUTED);
    return (y + 44) - y0;
}

// ---- actions ----

void closeOverlay() {
    if (openNode[0] || chatOpen || tzPickerOpen) scroll[page] = listScroll;
    openNode[0] = 0;
    chatOpen = false;
    tzPickerOpen = false;
}

void setPage(Page p) {
    if (p == page) {
        // Tapping the active tab closes overlays / scrolls to top.
        if (openNode[0] || chatOpen || tzPickerOpen) closeOverlay();
        else scroll[p] = 0;
    } else {
        openNode[0] = 0;
        chatOpen = false;
        tzPickerOpen = false;
    }
    page = p;
    meshpoint::setActivePage((int)p);
    dirty = true;
}

void onTap(int x, int y) {
    for (int i = targetCount - 1; i >= 0; --i) {
        const Target& t = targets[i];
        if (x < t.x || y < t.y || x >= t.x + t.w || y >= t.y + t.h) continue;
        Settings& s = config::get();
        switch (t.action) {
            case Action::Tab:
                setPage((Page)t.arg);
                break;
            case Action::OpenNode: {
                model::Guard g;
                if (t.arg < model::data().nodeCount) {
                    strlcpy(openNode, model::data().nodes[t.arg].id, sizeof(openNode));
                    listScroll = scroll[NODES];
                    scroll[NODES] = 0;
                }
                break;
            }
            case Action::OpenConv: {
                model::Guard g;
                if (t.arg < model::data().convCount) {
                    meshpoint::requestConversation(model::data().convs[t.arg].id);
                    chatOpen = true;
                    chatStickBottom = true;
                    listScroll = scroll[CHAT];
                }
                break;
            }
            case Action::Back:
                closeOverlay();
                break;
            case Action::Flip:
                {
                    model::Guard g;
                    s.flip = !s.flip;
                    config::save();
                }
                panel::setFlipped(s.flip);
                break;
            case Action::BrightDown:
            case Action::BrightUp: {
                int b = s.brightness + (t.action == Action::BrightUp ? 40 : -40);
                s.brightness = constrain(b, 15, 255);
                panel::setBrightness(s.brightness);
                config::save();
                break;
            }
            case Action::Portal:
                drawSplash("Starting setup portal", "Restarting...");
                delay(300);
                config::rebootIntoPortal();
            case Action::OpenTimezone: {
                listScroll = scroll[SETTINGS];
                tzPickerOpen = true;
                // Start with the current zone in view.
                int idx = 0;
                for (int i = 0; i < kTimeZoneCount; ++i) {
                    if (!strcmp(kTimeZones[i].posix, s.tz.c_str())) idx = i;
                }
                scroll[SETTINGS] = max(0, 50 + idx * TZ_ROW_H - CONTENT_H / 2);
                break;
            }
            case Action::PickTimezone:
                if (t.arg >= 0 && t.arg < kTimeZoneCount) {
                    {
                        model::Guard g;
                        config::setTimezone(kTimeZones[t.arg].posix);
                    }
                    closeOverlay();
                }
                break;
            case Action::UpdateStart:
                updateConfirmUntil = millis() + 15000;
                break;
            case Action::UpdateConfirm:
                updateConfirmUntil = 0;
                meshpoint::startUpdate();
                break;
            case Action::UpdateCancel:
                updateConfirmUntil = 0;
                break;
            case Action::UpdateCheck:
                meshpoint::checkForUpdate();
                break;
            case Action::UpdateDismiss: {
                model::Guard g;
                model::data().update.state = model::UpdateState::Idle;
                model::data().version++;
                break;
            }
            case Action::AutoDim:
                s.autoDim = !s.autoDim;
                config::save();
                break;
            case Action::Mute:
                s.muted = !s.muted;
                config::save();
                break;
            case Action::VolDown:
            case Action::VolUp:
                s.volume = constrain((int)s.volume + (t.action == Action::VolUp ? 10 : -10), 0, 100);
                s.muted = false;
                config::save();
                audio::preview(audio::Sound::ChannelMessage);  // hear the new level
                break;
            case Action::TestChannel:
                audio::preview(audio::Sound::ChannelMessage);
                break;
            case Action::TestDM:
                audio::preview(audio::Sound::DirectMessage);
                break;
            case Action::TestNode:
                audio::preview(audio::Sound::NewNode);
                break;
            case Action::ZoomIn:
                zoomButton(0.5);
                break;
            case Action::ZoomOut:
                zoomButton(2.0);
                break;
            case Action::ZoomFit: {
                model::Guard g;
                fitMap(model::data());
                break;
            }
            case Action::Refresh:
                meshpoint::refreshNow();
                break;
            default:
                break;
        }
        dirty = true;
        return;
    }
    if (page == MAP && y >= CONTENT_Y && y < TAB_Y) mapTap(x, y);
}

}  // namespace

void begin(LGFX_Sprite* canvas) {
    cv = canvas;
    meshpoint::setActivePage(page);
}

void handleTouch(const panel::TouchPoint& tp) {
    if (tp.down && !wasDown) {
        downX = tp.x;
        downY = tp.y;
        dragging = false;
        downScroll = chatOpen && page == CHAT ? scroll[CHAT] : scroll[page];
        downLat = viewLat;
        downLon = viewLon;
    } else if (tp.down && wasDown && page == MAP) {
        int dx = tp.x - downX, dy = tp.y - downY;
        if (!dragging && (abs(dx) > 10 || abs(dy) > 10) && downY >= CONTENT_Y && downY < TAB_Y) dragging = true;
        if (dragging) {
            viewLat = downLat + dy * mpp / M_PER_DEG_LAT;
            viewLon = downLon - dx * mpp / mPerDegLon();
            dirty = true;
        }
    } else if (tp.down && wasDown) {
        int dy = tp.y - downY;
        if (!dragging && abs(dy) > 12 && downY >= CONTENT_Y && downY < TAB_Y) dragging = true;
        if (dragging) {
            int maxScroll = max(0, contentHeight - CONTENT_H);
            int ns = constrain(downScroll - dy, 0, maxScroll);
            if (ns != scroll[page]) {
                scroll[page] = ns;
                if (page == CHAT && chatOpen) chatStickBottom = (ns >= maxScroll - 4);
                dirty = true;
            }
        }
    } else if (!tp.down && wasDown) {
        if (!dragging) onTap(downX, downY);
        dragging = false;
    }
    wasDown = tp.down;
}

bool render(bool force) {
    model::Guard g;
    const model::Model& m = model::data();
    uint32_t now = millis();
    if (!force && !dirty && m.version == lastVersion && now - lastDrawMs < 1000) return false;
    lastVersion = m.version;
    lastDrawMs = now;
    dirty = false;
    targetCount = 0;

    cv->fillScreen(C_BG);

    // Fixed sub-header for an open conversation.
    int fixedH = 0;
    if (page == CHAT && chatOpen) {
        fixedH = 46;
        cv->fillRect(0, CONTENT_Y, W, fixedH, C_BG);
        backButton(CONTENT_Y + 8);
        const char* title = m.messagesFor;
        for (int i = 0; i < m.convCount; ++i) {
            if (!strcmp(m.convs[i].id, m.messagesFor)) title = m.convs[i].name;
        }
        font(F_TITLE);
        textFit(title, 96, CONTENT_Y + 24, W - 110, C_TEXT);
        cv->drawFastHLine(0, CONTENT_Y + fixedH - 1, W, C_BORDER);
    }

    // Scrollable content, clipped to the content area.
    const int clipY = CONTENT_Y + fixedH, clipH = CONTENT_H - fixedH;
    cv->setClipRect(0, clipY, W, clipH);
    int y0 = clipY - scroll[page];
    int h = 0;
    switch (page) {
        case HOME: h = drawHome(m, y0); break;
        case NODES: h = openNode[0] ? drawNodeDetail(m, y0) : drawNodes(m, y0); break;
        case FEED: h = drawFeed(m, y0); break;
        case CHAT: h = chatOpen ? drawConversation(m, y0) : drawConversations(m, y0); break;
        case MAP: h = drawMap(m); break;
        case SETTINGS: h = tzPickerOpen ? drawTimezonePicker(y0) : drawSettings(m, y0); break;
        default: break;
    }
    cv->clearClipRect();
    contentHeight = h + fixedH;

    // Clamp scroll after content changes; keep chat pinned to newest message.
    int maxScroll = max(0, contentHeight - CONTENT_H);
    bool rerender = false;
    if (page == CHAT && chatOpen && chatStickBottom && scroll[page] != maxScroll) {
        scroll[page] = maxScroll;
        rerender = true;
    } else if (scroll[page] > maxScroll) {
        scroll[page] = maxScroll;
        rerender = true;
    }
    if (rerender) {
        dirty = true;
        return render(true);
    }

    // Scrollbar.
    if (maxScroll > 0) {
        int trackH = clipH - 8;
        int thumbH = max(20, trackH * clipH / contentHeight);
        int thumbY = clipY + 4 + (trackH - thumbH) * scroll[page] / maxScroll;
        cv->fillRoundRect(W - 5, thumbY, 3, thumbH, 1, C_BORDER);
    }

    drawHeader(m);
    drawTabs(m);
    panel::pushFrame((const uint16_t*)cv->getBuffer());
    return true;
}

void drawSplash(const char* title, const char* line1, const char* line2, const char* line3) {
    cv->fillScreen(C_BG);
    // Simple mesh motif.
    const int cx = W / 2, cy = 88;
    const int pts[5][2] = {{-60, 10}, {-20, -30}, {30, -18}, {64, 20}, {4, 34}};
    for (int i = 0; i < 5; ++i)
        for (int j = i + 1; j < 5; ++j)
            if ((i + j) % 2 || j == i + 1)
                cv->drawLine(cx + pts[i][0], cy + pts[i][1], cx + pts[j][0], cy + pts[j][1], C_BORDER);
    for (auto& p : pts) cv->fillCircle(cx + p[0], cy + p[1], 6, C_CYAN);
    cv->fillCircle(cx + pts[4][0], cy + pts[4][1], 9, C_GREEN);

    font(F_TITLE);
    text(title, W / 2, 160, C_TEXT, lgfx::middle_center);
    font(F_BODY);
    if (line1) textFit(line1, W / 2, 198, W - 40, C_TEXT2, lgfx::middle_center);
    if (line2) textFit(line2, W / 2, 226, W - 40, C_TEXT2, lgfx::middle_center);
    if (line3) textFit(line3, W / 2, 254, W - 40, C_CYAN, lgfx::middle_center);
    panel::pushFrame((const uint16_t*)cv->getBuffer());
}

}  // namespace ui
