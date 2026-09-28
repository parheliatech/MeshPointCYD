#include "config.h"

#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "audio.h"

namespace config {
namespace {

Settings settings;
constexpr const char* kNamespace = "mpcyd";
constexpr const char* kApName = "MeshPointCYD-Setup";

void printSettings() {
    Serial.printf("url   = %s\n", settings.url.c_str());
    Serial.printf("user  = %s\n", settings.user.c_str());
    Serial.printf("pass  = %s\n", settings.pass.length() ? "(set)" : "(empty)");
    Serial.printf("tz    = %s\n", settings.tz.c_str());
    Serial.printf("flip  = %d\n", settings.flip);
    Serial.printf("bright= %u\n", settings.brightness);
    Serial.printf("volume= %u%s\n", settings.volume, settings.muted ? " (muted)" : "");
    Serial.printf("autodim=%d\n", settings.autoDim);
    Serial.printf("wifi  = %s (%s) ip=%s rssi=%d\n", WiFi.SSID().c_str(),
                  WiFi.isConnected() ? "connected" : "disconnected",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

void printHelp() {
    Serial.println(
        "Commands:\n"
        "  show                      print settings\n"
        "  url <http://host:8080>    Meshpoint base URL\n"
        "  user <viewer|admin>       login username\n"
        "  pass <password>           login password\n"
        "  tz <POSIX TZ>             e.g. EST5EDT,M3.2.0,M11.1.0\n"
        "  flip <0|1>                rotate display 180 degrees\n"
        "  volume <0-100>            alert volume\n"
        "  mute <0|1>                silence alerts\n"
        "  autodim <0|1>             dim to 10% after 5 min without touch\n"
        "  beep <channel|dm|node>    preview an alert sound\n"
        "  wifi <ssid> [password]    connect to WiFi (ssid without spaces)\n"
        "  portal                    reboot into WiFi setup portal\n"
        "  reboot                    restart");
}

}  // namespace

Settings& get() { return settings; }

void load() {
    Preferences p;
    p.begin(kNamespace, true);
    settings.url = p.getString("url", settings.url);
    settings.user = p.getString("user", settings.user);
    settings.pass = p.getString("pass", settings.pass);
    settings.tz = p.getString("tz", settings.tz);
    settings.flip = p.getBool("flip", settings.flip);
    settings.brightness = p.getUChar("bright", settings.brightness);
    settings.volume = p.getUChar("volume", settings.volume);
    settings.muted = p.getBool("muted", settings.muted);
    settings.autoDim = p.getBool("autodim", settings.autoDim);
    p.end();
    while (settings.url.endsWith("/")) settings.url.remove(settings.url.length() - 1);
}

void save() {
    while (settings.url.endsWith("/")) settings.url.remove(settings.url.length() - 1);
    Preferences p;
    p.begin(kNamespace, false);
    p.putString("url", settings.url);
    p.putString("user", settings.user);
    p.putString("pass", settings.pass);
    p.putString("tz", settings.tz);
    p.putBool("flip", settings.flip);
    p.putUChar("bright", settings.brightness);
    p.putUChar("volume", settings.volume);
    p.putBool("muted", settings.muted);
    p.putBool("autodim", settings.autoDim);
    p.end();
}

void setTimezone(const String& tz) {
    settings.tz = tz.length() ? tz : String("UTC0");
    save();
    setenv("TZ", settings.tz.c_str(), 1);
    tzset();
}

void rebootIntoPortal() {
    Preferences p;
    p.begin(kNamespace, false);
    p.putBool("portal", true);
    p.end();
    ESP.restart();
    while (true) {}
}

bool consumePortalRequest() {
    Preferences p;
    p.begin(kNamespace, false);
    bool requested = p.getBool("portal", false);
    if (requested) p.remove("portal");
    p.end();
    return requested;
}

bool runPortal(bool forcePortal) {
    WiFiManager wm;
    WiFiManagerParameter pUrl("url", "Meshpoint URL (e.g. http://192.168.1.50:8080)",
                              settings.url.c_str(), 96);
    WiFiManagerParameter pUser("user", "Meshpoint username (viewer or admin)",
                               settings.user.c_str(), 16);
    WiFiManagerParameter pPass("pass", "Meshpoint password", "", 64,
                               "type='password' placeholder='leave blank to keep'");
    WiFiManagerParameter pTz("tz", "Time zone (POSIX, e.g. EST5EDT,M3.2.0,M11.1.0)",
                             settings.tz.c_str(), 48);
    wm.addParameter(&pUrl);
    wm.addParameter(&pUser);
    wm.addParameter(&pPass);
    wm.addParameter(&pTz);
    wm.setTitle("MeshPoint CYD");
    wm.setConfigPortalTimeout(forcePortal ? 600 : 300);
    wm.setConnectTimeout(20);

    bool saveRequested = false;
    wm.setSaveParamsCallback([&]() { saveRequested = true; });
    wm.setSaveConfigCallback([&]() { saveRequested = true; });

    bool ok = forcePortal ? wm.startConfigPortal(kApName) : wm.autoConnect(kApName);

    if (saveRequested) {
        settings.url = pUrl.getValue();
        settings.url.trim();
        settings.user = pUser.getValue();
        settings.user.trim();
        String pw = pPass.getValue();
        if (pw.length()) settings.pass = pw;
        settings.tz = pTz.getValue();
        settings.tz.trim();
        if (settings.tz.isEmpty()) settings.tz = "UTC0";
        save();
    }
    return ok;
}

void handleSerialLine(const String& raw) {
    String line = raw;
    line.trim();
    if (line.isEmpty()) return;
    int sp = line.indexOf(' ');
    String cmd = sp < 0 ? line : line.substring(0, sp);
    String arg = sp < 0 ? "" : line.substring(sp + 1);
    arg.trim();
    cmd.toLowerCase();

    if (cmd == "show") {
        printSettings();
    } else if (cmd == "url" && arg.length()) {
        settings.url = arg;
        save();
        Serial.println("ok (applies immediately)");
    } else if (cmd == "user" && arg.length()) {
        settings.user = arg;
        save();
        Serial.println("ok");
    } else if (cmd == "pass") {
        settings.pass = arg;
        save();
        Serial.println("ok");
    } else if (cmd == "tz" && arg.length()) {
        setTimezone(arg);
        Serial.println("ok");
    } else if (cmd == "flip") {
        settings.flip = arg.toInt() != 0;
        save();
        Serial.println("ok");
    } else if (cmd == "volume" && arg.length()) {
        settings.volume = constrain(arg.toInt(), 0, 100);
        save();
        Serial.println("ok");
    } else if (cmd == "mute") {
        settings.muted = arg.toInt() != 0;
        save();
        Serial.println("ok");
    } else if (cmd == "autodim") {
        settings.autoDim = arg.toInt() != 0;
        save();
        Serial.println("ok");
    } else if (cmd == "beep") {
        audio::preview(arg == "dm" ? audio::Sound::DirectMessage
                       : arg == "node" ? audio::Sound::NewNode
                                       : audio::Sound::ChannelMessage);
    } else if (cmd == "wifi" && arg.length()) {
        int s2 = arg.indexOf(' ');
        String ssid = s2 < 0 ? arg : arg.substring(0, s2);
        String pw = s2 < 0 ? "" : arg.substring(s2 + 1);
        Serial.printf("connecting to %s...\n", ssid.c_str());
        WiFi.disconnect();
        WiFi.begin(ssid.c_str(), pw.c_str());  // persisted by the WiFi stack
    } else if (cmd == "portal") {
        rebootIntoPortal();
    } else if (cmd == "reboot") {
        ESP.restart();
    } else {
        printHelp();
    }
}

}  // namespace config
