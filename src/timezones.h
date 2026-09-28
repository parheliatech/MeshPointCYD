#pragma once
// Common time zones for the Settings picker: display label -> POSIX TZ string.
// Any other POSIX TZ string can still be entered in the setup portal or with the `tz` serial command.

struct TimeZoneOption {
    const char* label;
    const char* posix;
};

static const TimeZoneOption kTimeZones[] = {
    {"UTC", "UTC0"},
    {"US Hawaii", "HST10"},
    {"US Alaska", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"US Arizona (no DST)", "MST7"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"Canada Atlantic", "AST4ADT,M3.2.0,M11.1.0"},
    {"Canada Newfoundland", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"Mexico City", "CST6"},
    {"Brazil (Sao Paulo)", "<-03>3"},
    {"Argentina", "<-03>3"},
    {"UK, Ireland, Portugal", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Moscow", "MSK-3"},
    {"South Africa", "SAST-2"},
    {"Gulf (Dubai)", "<+04>-4"},
    {"India", "IST-5:30"},
    {"Thailand, Vietnam", "<+07>-7"},
    {"China, Singapore, Perth", "<+08>-8"},
    {"Japan, Korea", "JST-9"},
    {"Australia Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia Brisbane", "AEST-10"},
    {"Australia Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
constexpr int kTimeZoneCount = sizeof(kTimeZones) / sizeof(kTimeZones[0]);

// Label for a POSIX TZ string, or nullptr if it isn't in the list.
inline const char* timeZoneLabel(const char* posix) {
    for (const auto& z : kTimeZones) {
        if (!strcmp(z.posix, posix)) return z.label;
    }
    return nullptr;
}
