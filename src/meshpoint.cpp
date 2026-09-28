#include "meshpoint.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <new>

#include "audio.h"
#include "config.h"
#include "hw_names.h"

// ---------------------------------------------------------------------------
// Model storage
// ---------------------------------------------------------------------------
namespace model {
namespace {
Model* gModel = nullptr;
SemaphoreHandle_t gMutex = nullptr;
}  // namespace

Model& data() { return *gModel; }
void lock() { xSemaphoreTakeRecursive(gMutex, portMAX_DELAY); }
void unlock() { xSemaphoreGiveRecursive(gMutex); }

const char* nodeName(const char* id) {
    Model& m = *gModel;
    for (int i = 0; i < m.nodeCount; ++i) {
        if (strcmp(m.nodes[i].id, id) == 0) {
            if (m.nodes[i].longName[0]) return m.nodes[i].longName;
            if (m.nodes[i].shortName[0]) return m.nodes[i].shortName;
            break;
        }
    }
    return id;
}

}  // namespace model

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------
namespace meshpoint {
namespace {

using model::NO_VALUE;

struct SpiRamAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override {
        return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    }
};
SpiRamAllocator gAllocator;

enum class Result { Ok, Unauthorized, Failed };

String gToken;  // session JWT, sent as the meshpoint_session cookie (all Meshpoint versions accept it)
Settings gCfg;  // snapshot of config::get(), taken under the model lock
volatile int gActivePage = 0;
volatile bool gRefreshNow = false;
volatile bool gNodeCountChanged = false;
volatile bool gUpdateRequested = false;
char gWantConversation[40] = "";
volatile bool gWantConversationPending = false;

// --- helpers -------------------------------------------------------------

void copyStr(char* dst, size_t n, const char* src) {
    if (!src) src = "";
    // Keep printable ASCII only: the bitmap fonts have no glyphs for emoji etc.
    size_t o = 0;
    for (const unsigned char* s = (const unsigned char*)src; *s && o + 1 < n; ++s) {
        if (*s >= 0x20 && *s < 0x7F) {
            dst[o++] = (char)*s;
        } else if (*s == '\n' || *s == '\t') {
            dst[o++] = ' ';
        }
    }
    // Trim whitespace left by stripped characters.
    while (o > 0 && dst[o - 1] == ' ') --o;
    dst[o] = 0;
    size_t lead = 0;
    while (dst[lead] == ' ') ++lead;
    if (lead) memmove(dst, dst + lead, o - lead + 1);
}

template <size_t N>
void copyStr(char (&dst)[N], JsonVariantConst v) {
    copyStr(dst, N, v.is<const char*>() ? v.as<const char*>() : "");
}

float numOr(JsonVariantConst v, float def = NO_VALUE) {
    return (v.is<float>() || v.is<int>()) ? v.as<float>() : def;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// Parse ISO-8601 ("2026-09-27T01:23:45.123+00:00", "...Z", or naive = UTC).
time_t parseIso(const char* s) {
    if (!s) return 0;
    int Y, M, D, h, mi, sec;
    if (sscanf(s, "%4d-%2d-%2d%*c%2d:%2d:%2d", &Y, &M, &D, &h, &mi, &sec) != 6) return 0;
    int64_t t = daysFromCivil(Y, M, D) * 86400 + h * 3600 + mi * 60 + sec;
    const char* p = s + 19;
    if (*p == '.') {
        ++p;
        while (isdigit((unsigned char)*p)) ++p;
    }
    if (*p == '+' || *p == '-') {
        int oh = 0, om = 0;
        sscanf(p + 1, "%2d:%2d", &oh, &om);
        int off = oh * 3600 + om * 60;
        t -= (*p == '+') ? off : -off;
    }
    return (time_t)t;
}

void setStatus(model::Link link, const char* err = "") {
    model::Guard g;
    auto& st = model::data().status;
    if (st.link != link || strcmp(st.error, err) != 0) {
        st.link = link;
        copyStr(st.error, sizeof(st.error), err);
        model::data().version++;
    }
}

void markOk() {
    model::Guard g;
    auto& st = model::data().status;
    st.lastOkMs = millis();
    if (st.link != model::Link::Ok) {
        st.link = model::Link::Ok;
        st.error[0] = 0;
    }
    model::data().version++;
}

struct Conn {
    WiFiClient plain;
    WiFiClientSecure secure;
    HTTPClient http;
};

bool beginRequest(Conn& c, const String& path) {
    const Settings& s = gCfg;
    String url = s.url + path;
    c.http.useHTTP10(true);  // no chunked encoding, so we can stream-parse
    c.http.setConnectTimeout(4000);
    c.http.setTimeout(10000);
    bool ok;
    if (url.startsWith("https://")) {
        c.secure.setInsecure();  // LAN device, typically self-signed
        ok = c.http.begin(c.secure, url);
    } else {
        ok = c.http.begin(c.plain, url);
    }
    c.http.addHeader("Accept", "application/json");
    c.http.addHeader("X-Meshpoint-Client", "cyd");
    return ok;
}

bool login() {
    const Settings& s = gCfg;
    if (s.pass.isEmpty()) {
        setStatus(model::Link::AuthFailed, "No password set - see Settings");
        return false;
    }
    Conn c;
    if (!beginRequest(c, "/api/auth/login")) {
        setStatus(model::Link::Error, "Bad Meshpoint URL");
        return false;
    }
    c.http.addHeader("Content-Type", "application/json");
    JsonDocument body;
    body["username"] = s.user;
    body["password"] = s.pass;
    String payload;
    serializeJson(body, payload);
    const char* collect[] = {"Set-Cookie"};
    c.http.collectHeaders(collect, 1);
    int code = c.http.POST(payload);
    if (code <= 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Can't reach Meshpoint (%s)", HTTPClient::errorToString(code).c_str());
        setStatus(model::Link::Error, msg);
        c.http.end();
        return false;
    }
    String cookie = c.http.header("Set-Cookie");
    JsonDocument resp(&gAllocator);
    deserializeJson(resp, c.http.getStream());
    c.http.end();

    // Newer Meshpoints return the token in the body for non-browser clients;
    // older ones only set it as a cookie.
    String token;
    if (resp["token"].is<const char*>()) {
        token = resp["token"].as<const char*>();
    } else {
        int start = cookie.indexOf("meshpoint_session=");
        if (start >= 0) {
            start += strlen("meshpoint_session=");
            int end = cookie.indexOf(';', start);
            token = end < 0 ? cookie.substring(start) : cookie.substring(start, end);
        }
    }
    if (code == 200 && token.length()) {
        gToken = token;
        model::Guard g;
        copyStr(model::data().status.role, resp["role"]);
        log_i("logged in as %s", model::data().status.role);
        return true;
    }
    const char* detail = resp["detail"].is<const char*>() ? resp["detail"].as<const char*>() : "";
    char msg[64];
    if (code == 401) {
        snprintf(msg, sizeof(msg), "Login rejected: check user/password");
    } else if (code == 429) {
        snprintf(msg, sizeof(msg), "Login locked out, retrying later");
    } else if (code == 409) {
        snprintf(msg, sizeof(msg), "Meshpoint setup not finished");
    } else if (code == 200) {
        snprintf(msg, sizeof(msg), "Login OK but no session token returned");
    } else {
        snprintf(msg, sizeof(msg), "Login HTTP %d %s", code, detail);
    }
    setStatus(model::Link::AuthFailed, msg);
    return false;
}

// GET a JSON endpoint into doc (optionally filtered). Handles re-login on 401.
Result getJson(const String& path, JsonDocument& doc, const JsonDocument* filter) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (gToken.isEmpty() && !login()) return Result::Unauthorized;
        Conn c;
        if (!beginRequest(c, path)) {
            setStatus(model::Link::Error, "Bad Meshpoint URL");
            return Result::Failed;
        }
        c.http.addHeader("Cookie", "meshpoint_session=" + gToken);
        int code = c.http.GET();
        if (code == 401) {
            c.http.end();
            gToken = "";
            continue;  // token expired: log in again
        }
        if (code != 200) {
            char msg[64];
            if (code < 0) {
                snprintf(msg, sizeof(msg), "Can't reach Meshpoint (%s)",
                         HTTPClient::errorToString(code).c_str());
            } else {
                snprintf(msg, sizeof(msg), "HTTP %d on %s", code, path.c_str());
            }
            c.http.end();
            setStatus(model::Link::Error, msg);
            return Result::Failed;
        }
        DeserializationError err =
            filter ? deserializeJson(doc, c.http.getStream(), DeserializationOption::Filter(*filter))
                   : deserializeJson(doc, c.http.getStream());
        c.http.end();
        if (err) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Bad JSON from %s: %s", path.c_str(), err.c_str());
            setStatus(model::Link::Error, msg);
            return Result::Failed;
        }
        markOk();
        return Result::Ok;
    }
    return Result::Unauthorized;
}

void fillDist(JsonObjectConst obj, model::Dist* out, int& count) {
    count = 0;
    model::Dist tmp[16];
    int n = 0;
    for (JsonPairConst kv : obj) {
        if (n >= 16) break;
        copyStr(tmp[n].name, sizeof(tmp[n].name), kv.key().c_str());
        tmp[n].count = kv.value().as<int>();
        ++n;
    }
    std::sort(tmp, tmp + n, [](const model::Dist& a, const model::Dist& b) { return a.count > b.count; });
    for (int i = 0; i < n && i < model::MAX_DIST; ++i) out[count++] = tmp[i];
}

// --- fetchers ----------------------------------------------------------------

bool fetchSummary() {
    JsonDocument doc(&gAllocator);
    if (getJson("/api/stats/summary", doc, nullptr) != Result::Ok) return false;

    model::Guard g;
    auto& s = model::data().summary;
    JsonObjectConst dev = doc["device"];
    copyStr(s.name, dev["name"]);
    copyStr(s.region, dev["region"]);
    copyStr(s.firmware, dev["firmware"]);
    s.uptime = dev["uptime_seconds"] | 0;

    JsonObjectConst tr = doc["traffic"];
    s.totalPackets = tr["total_packets"] | 0;
    s.packetsLastHour = tr["packets_last_hour"] | 0;
    s.packetsLastMinute = tr["packets_last_minute"] | 0;
    s.packetsPerMinute = tr["packets_per_minute"] | 0.0f;
    fillDist(tr["type_distribution"], s.types, s.typeCount);
    fillDist(tr["protocol_distribution"], s.protocols, s.protocolCount);

    JsonObjectConst net = doc["network"];
    s.totalNodes = net["total_nodes"] | 0;
    s.active24h = net["active_24h"] | 0;

    JsonObjectConst sig = doc["signal"];
    s.avgRssi = numOr(sig["avg_rssi"]);
    s.avgSnr = numOr(sig["avg_snr"]);
    s.bestRssi = numOr(sig["best_rssi"]);
    s.bestSnr = numOr(sig["best_snr"]);

    JsonArrayConst counts = doc["traffic_timeline"]["counts"];
    JsonArrayConst labels = doc["traffic_timeline"]["labels"];
    int n = counts.size();
    int start = n > model::TIMELINE_BUCKETS ? n - model::TIMELINE_BUCKETS : 0;
    memset(s.timeline, 0, sizeof(s.timeline));
    int off = model::TIMELINE_BUCKETS - (n - start);
    for (int i = start; i < n; ++i) s.timeline[off + i - start] = counts[i] | 0;
    copyStr(s.timelineStart, labels[start]);

    s.direct = doc["direct_relayed"]["direct"] | 0;
    s.relayed = doc["direct_relayed"]["relayed"] | 0;
    s.relayEnabled = doc["relay"]["enabled"] | false;
    s.relayCount = doc["relay"]["relayed"] | 0;
    s.farthestMiles = doc["farthest_mesh"]["miles"] | 0.0f;
    copyStr(s.farthestName, doc["farthest_mesh"]["node_name"]);
    s.valid = true;
    model::data().version++;
    return true;
}

bool fetchHost() {
    JsonDocument doc(&gAllocator);
    if (getJson("/api/device/metrics", doc, nullptr) != Result::Ok) return false;
    model::Guard g;
    auto& h = model::data().host;
    h.cpu = numOr(doc["cpu_percent"]);
    h.mem = numOr(doc["memory_percent"]);
    h.disk = numOr(doc["disk_percent"]);
    h.temp = numOr(doc["cpu_temp_c"]);
    h.load1 = numOr(doc["load_avg"][0]);
    h.uptime = doc["system_uptime_seconds"] | 0;
    h.valid = true;
    model::data().version++;
    return true;
}

bool fetchNodes() {
    JsonDocument filter;
    JsonObject f = filter[0].to<JsonObject>();
    for (const char* k : {"node_id", "long_name", "short_name", "hardware_model", "role", "protocol",
                          "firmware_version", "last_heard", "first_seen", "packet_count",
                          "latest_rssi", "latest_snr", "latest_battery", "latest_voltage",
                          "latest_temperature", "latest_humidity", "latest_channel_util",
                          "latest_air_util", "latest_hops", "has_position", "latitude", "longitude"}) {
        f[k] = true;
    }
    JsonDocument doc(&gAllocator);
    String path = "/api/nodes?limit=" + String(model::MAX_NODES);
    if (getJson(path, doc, &filter) != Result::Ok) return false;

    model::Guard g;
    auto& m = model::data();
    int i = 0;
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
        if (i >= model::MAX_NODES) break;
        model::Node& n = m.nodes[i++];
        copyStr(n.id, o["node_id"]);
        copyStr(n.shortName, o["short_name"]);
        copyStr(n.longName, o["long_name"]);
        copyStr(n.hw, sizeof(n.hw), hwModelName(o["hardware_model"] | ""));
        copyStr(n.role, o["role"]);
        copyStr(n.protocol, o["protocol"]);
        copyStr(n.firmware, o["firmware_version"]);
        n.lastHeard = parseIso(o["last_heard"]);
        n.firstSeen = parseIso(o["first_seen"]);
        n.packets = o["packet_count"] | 0;
        n.rssi = numOr(o["latest_rssi"]);
        n.snr = numOr(o["latest_snr"]);
        n.battery = numOr(o["latest_battery"]);
        n.voltage = numOr(o["latest_voltage"]);
        n.temperature = numOr(o["latest_temperature"]);
        n.humidity = numOr(o["latest_humidity"]);
        n.chUtil = numOr(o["latest_channel_util"]);
        n.airUtil = numOr(o["latest_air_util"]);
        n.hops = o["latest_hops"].is<int>() ? o["latest_hops"].as<int>() : -1;
        n.hasPosition = o["has_position"] | false;
        n.lat = o["latitude"] | 0.0;
        n.lon = o["longitude"] | 0.0;
    }
    m.nodeCount = i;
    m.nodesStamp = millis();
    m.version++;
    return true;
}

bool fetchPackets() {
    JsonDocument filter;
    JsonObject f = filter[0].to<JsonObject>();
    for (const char* k : {"source_id", "protocol", "packet_type", "hop_count", "timestamp"}) f[k] = true;
    f["signal"]["rssi"] = true;
    f["signal"]["snr"] = true;
    f["decoded_payload"]["text"] = true;

    JsonDocument doc(&gAllocator);
    String path = "/api/packets?limit=" + String(model::MAX_PACKETS);
    if (getJson(path, doc, &filter) != Result::Ok) return false;

    model::Guard g;
    auto& m = model::data();
    int i = 0;
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
        if (i >= model::MAX_PACKETS) break;
        model::Packet& p = m.packets[i++];
        p.ts = parseIso(o["timestamp"]);
        copyStr(p.source, o["source_id"]);
        copyStr(p.type, o["packet_type"]);
        copyStr(p.protocol, o["protocol"]);
        p.rssi = numOr(o["signal"]["rssi"]);
        p.snr = numOr(o["signal"]["snr"]);
        p.hops = o["hop_count"].is<int>() ? o["hop_count"].as<int>() : -1;
        copyStr(p.text, o["decoded_payload"]["text"]);
    }
    m.packetCount = i;
    m.packetsStamp = millis();
    m.version++;
    return true;
}

bool fetchConversations() {
    // Channel list maps broadcast conversation ids ("broadcast:meshtastic:0") to names ("MediumFast").
    JsonDocument channels(&gAllocator);
    if (getJson("/api/messages/channels", channels, nullptr) != Result::Ok) channels.clear();

    JsonDocument doc(&gAllocator);
    if (getJson("/api/messages/conversations", doc, nullptr) != Result::Ok) return false;
    model::Guard g;
    auto& m = model::data();
    // Snapshot previous unread counts to detect newly received messages.
    static bool firstLoad = true;
    struct Prev {
        char id[40];
        int unread;
    };
    static Prev prev[model::MAX_CONVS];
    const int prevCount = m.convCount;
    for (int p = 0; p < prevCount; ++p) {
        strlcpy(prev[p].id, m.convs[p].id, sizeof(prev[p].id));
        prev[p].unread = m.convs[p].unread;
    }
    bool newChannelMsg = false, newDirectMsg = false;

    int i = 0;
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
        if (i >= model::MAX_CONVS) break;
        model::Conversation& c = m.convs[i++];
        copyStr(c.id, o["node_id"]);
        copyStr(c.name, o["node_name"]);
        copyStr(c.last, o["last_message"]);
        copyStr(c.protocol, o["protocol"]);
        c.ts = parseIso(o["last_timestamp"]);
        c.unread = o["unread_count"] | 0;
        c.broadcast = o["is_broadcast"] | false;
        if (c.broadcast) {
            for (JsonObjectConst ch : channels.as<JsonArrayConst>()) {
                if (strcmp(ch["node_id"] | "", c.id) == 0 && ch["name"].is<const char*>()) {
                    copyStr(c.name, ch["name"]);
                    break;
                }
            }
        }
    }
    m.convCount = i;
    for (int k = 0; k < m.convCount && !firstLoad; ++k) {
        const model::Conversation& c = m.convs[k];
        int before = 0;
        for (int p = 0; p < prevCount; ++p) {
            if (!strcmp(prev[p].id, c.id)) before = prev[p].unread;
        }
        if (c.unread > before) (c.broadcast ? newChannelMsg : newDirectMsg) = true;
    }
    firstLoad = false;
    if (newDirectMsg) audio::play(audio::Sound::DirectMessage);
    if (newChannelMsg) audio::play(audio::Sound::ChannelMessage);
    m.version++;
    return true;
}

// Total node count: an increase means Meshpoint discovered a new node (alert); any change
// (new node or retention cleanup) refreshes the map.
bool fetchNodeCount() {
    JsonDocument doc;
    if (getJson("/api/nodes/count", doc, nullptr) != Result::Ok) return false;
    static int lastCount = -1;
    const int count = doc["count"] | -1;
    if (count < 0) return true;
    if (lastCount >= 0 && count != lastCount) {
        if (count > lastCount) audio::play(audio::Sound::NewNode);
        gNodeCountChanged = true;  // scheduler refreshes the map and node list right away
    }
    lastCount = count;
    return true;
}

bool validPosition(double lat, double lon) {
    // Meshtastic uses 0,0 for "no fix"; also reject out-of-range junk.
    if (fabs(lat) < 0.001 && fabs(lon) < 0.001) return false;
    return lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
}

bool fetchDevice() {
    JsonDocument doc(&gAllocator);
    if (getJson("/api/device", doc, nullptr) != Result::Ok) return false;
    model::Guard g;
    auto& m = model::data();
    double lat = doc["latitude"] | 0.0, lon = doc["longitude"] | 0.0;
    m.hasHome = validPosition(lat, lon);
    m.homeLat = lat;
    m.homeLon = lon;
    m.version++;
    return true;
}

// Positioned nodes + links for the Map tab. Nodes added since the last fetch are flagged
// so the map can highlight them; removed nodes simply drop out.
bool fetchMap() {
    JsonDocument filter;
    JsonObject f = filter[0].to<JsonObject>();
    for (const char* k : {"node_id", "display_name", "latitude", "longitude", "last_heard", "packet_count", "protocol"}) {
        f[k] = true;
    }
    f["signal"]["rssi"] = true;
    JsonDocument nodes(&gAllocator);
    if (getJson("/api/nodes/map", nodes, &filter) != Result::Ok) return false;

    JsonDocument lfilter;
    JsonObject lf = lfilter[0].to<JsonObject>();
    for (const char* k : {"source", "target", "rssi", "snr", "last_seen"}) lf[k] = true;
    JsonDocument links(&gAllocator);
    if (getJson("/api/analytics/topology", links, &lfilter) != Result::Ok) return false;

    // Build the new node list outside the lock, then swap it in.
    static model::MapNode* next = nullptr;
    if (!next) {
        next = (model::MapNode*)heap_caps_calloc(model::MAX_MAP_NODES, sizeof(model::MapNode), MALLOC_CAP_SPIRAM);
        if (!next) return false;
    }
    int n = 0;
    for (JsonObjectConst o : nodes.as<JsonArrayConst>()) {
        if (n >= model::MAX_MAP_NODES) break;
        double lat = o["latitude"] | 0.0, lon = o["longitude"] | 0.0;
        if (!validPosition(lat, lon)) continue;
        model::MapNode& mn = next[n++];
        copyStr(mn.id, o["node_id"]);
        copyStr(mn.name, o["display_name"]);
        mn.lat = lat;
        mn.lon = lon;
        mn.lastHeard = parseIso(o["last_heard"]);
        mn.rssi = numOr(o["signal"]["rssi"]);
        mn.packets = o["packet_count"] | 0;
        mn.meshcore = strcmp(o["protocol"] | "", "meshcore") == 0;
        mn.newUntilMs = 0;
    }

    model::Guard g;
    auto& m = model::data();
    const bool firstLoad = m.mapStamp == 0;
    const uint32_t now = millis();
    for (int i = 0; i < n; ++i) {
        int prev = -1;
        for (int j = 0; j < m.mapNodeCount; ++j) {
            if (!strcmp(m.mapNodes[j].id, next[i].id)) {
                prev = j;
                break;
            }
        }
        if (prev >= 0) next[i].newUntilMs = m.mapNodes[prev].newUntilMs;
        else if (!firstLoad) next[i].newUntilMs = now + 120000;  // highlight for 2 minutes
    }
    memcpy(m.mapNodes, next, sizeof(model::MapNode) * n);
    m.mapNodeCount = n;

    auto indexOf = [&](const char* id) -> int {
        for (int i = 0; i < m.mapNodeCount; ++i) {
            if (!strcmp(m.mapNodes[i].id, id)) return i;
        }
        return -1;
    };
    int l = 0, unplaced = 0;
    for (JsonObjectConst o : links.as<JsonArrayConst>()) {
        if (l >= model::MAX_LINKS) break;
        int a = indexOf(o["source"] | ""), b = indexOf(o["target"] | "");
        if (a < 0 || b < 0 || a == b) {
            ++unplaced;
            continue;
        }
        model::MeshLink& ml = m.links[l++];
        ml.a = a;
        ml.b = b;
        ml.rssi = numOr(o["rssi"]);
        ml.snr = numOr(o["snr"]);
        ml.lastSeen = parseIso(o["last_seen"]);
    }
    m.linkCount = l;
    m.linksUnplaced = unplaced;
    m.mapStamp = millis();
    m.version++;
    return true;
}

// Is a newer Meshpoint release available? Also records which channel the install tracks.
bool fetchUpdateCheck() {
    JsonDocument doc;
    if (getJson("/api/device/update-check", doc, nullptr) != Result::Ok) return false;
    char channel[24] = "stable";
    {
        JsonDocument st;
        if (getJson("/api/update/install_status", st, nullptr) == Result::Ok && st["active_channel_id"].is<const char*>()) {
            copyStr(channel, sizeof(channel), st["active_channel_id"].as<const char*>());
        }
    }
    model::Guard g;
    auto& u = model::data().update;
    u.checked = true;
    u.available = doc["update_available"] | false;
    copyStr(u.localVersion, doc["local_version"]);
    copyStr(u.remoteVersion, doc["remote_version"]);
    strlcpy(u.channel, channel, sizeof(u.channel));
    if (u.state == model::UpdateState::Checking) u.state = model::UpdateState::Idle;
    // After an update, clear the "done" banner once the Meshpoint reports it is current.
    if (u.state == model::UpdateState::Done && !u.available) u.state = model::UpdateState::Idle;
    model::data().version++;
    return true;
}

void setUpdate(model::UpdateState state, const char* step, const char* message) {
    model::Guard g;
    auto& u = model::data().update;
    u.state = state;
    if (step) copyStr(u.step, sizeof(u.step), step);
    if (message) copyStr(u.message, sizeof(u.message), message);
    if (state == model::UpdateState::Done || state == model::UpdateState::Failed) u.finishedMs = millis();
    model::data().version++;
}

// Run the Meshpoint's own update chain (git fetch/checkout/reset, then pip install + service
// restart) and follow its NDJSON progress stream. The final step restarts the service, so a
// dropped connection after it has started counts as success.
void applyUpdate() {
    char channel[24];
    {
        model::Guard g;
        strlcpy(channel, model::data().update.channel, sizeof(channel));
        if (strcmp(model::data().status.role, "admin") != 0) {
            model::data().update.state = model::UpdateState::Failed;
            strlcpy(model::data().update.message, "Updating needs the admin login", sizeof(model::data().update.message));
            model::data().version++;
            return;
        }
    }
    setUpdate(model::UpdateState::Running, "starting", "");
    if (gToken.isEmpty() && !login()) {
        setUpdate(model::UpdateState::Failed, nullptr, "Login failed");
        return;
    }
    Conn c;
    if (!beginRequest(c, "/api/update/apply/stream")) {
        setUpdate(model::UpdateState::Failed, nullptr, "Bad Meshpoint URL");
        return;
    }
    c.http.setTimeout(65000);  // steps like pip install can be quiet for a while
    c.http.addHeader("Content-Type", "application/json");
    c.http.addHeader("Cookie", "meshpoint_session=" + gToken);
    JsonDocument body;
    body["channel_id"] = channel;
    String payload;
    serializeJson(body, payload);
    int code = c.http.POST(payload);
    if (code != 200) {
        char msg[64];
        snprintf(msg, sizeof(msg), code == 403 ? "Updating needs the admin login" : "Update request failed (HTTP %d)", code);
        c.http.end();
        setUpdate(model::UpdateState::Failed, nullptr, msg);
        return;
    }

    WiFiClient* s = c.http.getStreamPtr();
    bool gotResult = false, restartStarted = false;
    String line;
    uint32_t lastData = millis();
    while (s && (s->connected() || s->available()) && millis() - lastData < 180000) {
        if (!s->available()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        lastData = millis();
        char ch = (char)s->read();
        if (ch != '\n') {
            if (line.length() < 2048) line += ch;
            continue;
        }
        JsonDocument ev;
        if (deserializeJson(ev, line) == DeserializationError::Ok) {
            const char* type = ev["type"] | "";
            if (!strcmp(type, "step")) {
                const char* step = ev["step"] | "";
                const char* phase = ev["phase"] | "";
                if (!strcmp(step, "upgrade")) restartStarted = true;
                char text[32];
                snprintf(text, sizeof(text), "%s%s%s", step, *phase ? ": " : "", phase);
                setUpdate(model::UpdateState::Running, text, nullptr);
            } else if (!strcmp(type, "result")) {
                gotResult = true;
                if (ev["result"]["success"] | false) {
                    setUpdate(model::UpdateState::Done, "", "Update installed. Meshpoint is restarting.");
                } else {
                    char msg[80];
                    snprintf(msg, sizeof(msg), "Update failed at: %s", ev["result"]["failed_step"] | "unknown step");
                    setUpdate(model::UpdateState::Failed, nullptr, msg);
                }
            } else if (!strcmp(type, "error")) {
                gotResult = true;
                setUpdate(model::UpdateState::Failed, nullptr, ev["message"] | "Update error");
            }
        }
        line = "";
    }
    c.http.end();
    if (!gotResult) {
        if (restartStarted) {
            setUpdate(model::UpdateState::Done, "", "Update installed. Meshpoint is restarting.");
        } else {
            setUpdate(model::UpdateState::Failed, nullptr, "Lost connection during update; check the Meshpoint");
        }
    }
    gToken = "";  // log in fresh once the restarted service is back
}

String urlEncode(const char* s) {
    String out;
    for (; *s; ++s) {
        char c = *s;
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ':' || c == '!') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            out += buf;
        }
    }
    return out;
}

bool fetchMessages(const char* id) {
    JsonDocument filter;
    JsonObject f = filter[0].to<JsonObject>();
    for (const char* k : {"timestamp", "node_name", "text", "direction", "rssi", "snr"}) f[k] = true;
    JsonDocument doc(&gAllocator);
    String path = "/api/messages/conversation/" + urlEncode(id) + "?limit=" + String(model::MAX_MESSAGES);
    Result r = getJson(path, doc, &filter);

    model::Guard g;
    auto& m = model::data();
    if (strcmp(m.messagesFor, id) != 0) return false;  // user moved on
    m.messagesLoading = false;
    if (r != Result::Ok) {
        m.version++;
        return false;
    }
    int i = 0;
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
        if (i >= model::MAX_MESSAGES) break;
        model::Message& msg = m.messages[i++];
        msg.ts = parseIso(o["timestamp"]);
        copyStr(msg.from, o["node_name"]);
        copyStr(msg.text, o["text"]);
        copyStr(msg.direction, o["direction"]);
        msg.rssi = numOr(o["rssi"]);
        msg.snr = numOr(o["snr"]);
    }
    m.messageCount = i;
    std::sort(m.messages, m.messages + i,
              [](const model::Message& a, const model::Message& b) { return a.ts < b.ts; });
    m.version++;
    return true;
}

// --- scheduler -------------------------------------------------------------

struct Job {
    bool (*fn)();
    uint32_t fastMs;  // when its page is showing
    uint32_t slowMs;  // otherwise
    int page;         // page that makes it "fast" (-1 none)
    uint32_t last;
    bool done;
};

Job gJobs[] = {
    {fetchNodeCount, 15000, 15000, -1, 0, false},
    {fetchNodes, 15000, 60000, page::NODES, 0, false},  // first: names for packets/feed
    {fetchSummary, 10000, 30000, page::HOME, 0, false},
    {fetchHost, 15000, 60000, page::HOME, 0, false},
    {fetchPackets, 4000, 30000, page::FEED, 0, false},
    {fetchConversations, 8000, 15000, page::CHAT, 0, false},  // also drives message alerts
    {fetchDevice, 600000, 600000, -1, 0, false},
    {fetchMap, 20000, 300000, page::MAP, 0, false},
    {fetchUpdateCheck, 1800000, 1800000, -1, 0, false},
};

void task(void*) {
    for (;;) {
        if (!WiFi.isConnected()) {
            setStatus(model::Link::NoWifi, "WiFi not connected");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        {
            model::Guard g;
            if (model::data().status.link == model::Link::NoWifi) {
                model::data().status.link = model::Link::Connecting;
                model::data().version++;
            }
        }
        {
            model::Guard g;
            if (gCfg.url != config::get().url || gCfg.user != config::get().user ||
                gCfg.pass != config::get().pass) {
                gRefreshNow = true;
            }
            gCfg = config::get();
        }
        if (gRefreshNow) {
            gRefreshNow = false;
            gToken = "";
            for (auto& j : gJobs) j.done = false;
        }

        if (gUpdateRequested) {
            gUpdateRequested = false;
            applyUpdate();
        }
        {
            // After an update, re-check every minute for 15 minutes so the banner clears
            // (or reappears) once the Meshpoint is back.
            model::Guard g;
            const auto& u = model::data().update;
            const uint32_t since = millis() - u.finishedMs;
            if (u.state == model::UpdateState::Done && since > 45000 && since < 900000) {
                for (auto& j : gJobs) {
                    if (j.fn == fetchUpdateCheck && millis() - j.last > 60000) j.done = false;
                }
            }
        }
        if (gNodeCountChanged) {
            gNodeCountChanged = false;
            for (auto& j : gJobs) {
                if (j.fn == fetchMap || j.fn == fetchNodes) j.done = false;
            }
        }

        if (gWantConversationPending) {
            char id[sizeof(gWantConversation)];
            {
                model::Guard g;
                strlcpy(id, gWantConversation, sizeof(id));
                gWantConversationPending = false;
            }
            fetchMessages(id);
        }

        const uint32_t now = millis();
        bool ran = false;
        for (auto& j : gJobs) {
            uint32_t interval = (j.page == gActivePage) ? j.fastMs : j.slowMs;
            if (!j.done || now - j.last >= interval) {
                bool ok = j.fn();
                j.last = millis();
                j.done = true;
                ran = true;
                if (!ok) break;  // don't hammer a failing server with every job
            }
        }
        {
            // Keep the open conversation live while on the chat page.
            static uint32_t lastMsg = 0;
            char id[40] = "";
            {
                model::Guard g;
                if (gActivePage == page::CHAT) strlcpy(id, model::data().messagesFor, sizeof(id));
            }
            if (id[0] && millis() - lastMsg > 10000) {
                lastMsg = millis();
                fetchMessages(id);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ran ? 50 : 200));
    }
}

}  // namespace

void begin() {
    model::gMutex = xSemaphoreCreateRecursiveMutex();
    void* mem = heap_caps_calloc(1, sizeof(model::Model), MALLOC_CAP_SPIRAM);
    model::gModel = new (mem) model::Model();
    xTaskCreatePinnedToCore(task, "meshpoint", 12288, nullptr, 1, nullptr, 0);
}

void setActivePage(int page) {
    if (gActivePage != page) {
        gActivePage = page;
        // Make the newly visible page's data fresh soon.
        for (auto& j : gJobs) {
            if (j.page == page && millis() - j.last > 2000) j.done = false;
        }
    }
}

void requestConversation(const char* id) {
    model::Guard g;
    auto& m = model::data();
    if (strcmp(m.messagesFor, id) != 0) {
        strlcpy(m.messagesFor, id, sizeof(m.messagesFor));
        m.messageCount = 0;
    }
    m.messagesLoading = true;
    m.version++;
    strlcpy(gWantConversation, id, sizeof(gWantConversation));
    gWantConversationPending = true;
}

void refreshNow() { gRefreshNow = true; }

void checkForUpdate() {
    {
        model::Guard g;
        auto& u = model::data().update;
        if (u.state == model::UpdateState::Running) return;
        u.state = model::UpdateState::Checking;
        model::data().version++;
    }
    for (auto& j : gJobs) {
        if (j.fn == fetchUpdateCheck) j.done = false;
    }
}

void startUpdate() {
    model::Guard g;
    if (model::data().update.state == model::UpdateState::Running) return;
    gUpdateRequested = true;
}

}  // namespace meshpoint
