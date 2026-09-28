#pragma once
#include <Arduino.h>
#include <time.h>

// Snapshot of Meshpoint data, filled by a background task polling the
// Meshpoint REST API (/api/...). Access only while holding model::lock().
// UI page ids, shared so the poller can prioritise the visible page.
namespace page {
constexpr int HOME = 0, NODES = 1, MAP = 2, FEED = 3, CHAT = 4, SETTINGS = 5, COUNT = 6;
}

namespace model {

constexpr int MAX_NODES = 80;
constexpr int MAX_MAP_NODES = 500;
constexpr int MAX_LINKS = 800;
constexpr int MAX_PACKETS = 40;
constexpr int MAX_CONVS = 24;
constexpr int MAX_MESSAGES = 30;
constexpr int TIMELINE_BUCKETS = 12;
constexpr int MAX_DIST = 6;

constexpr float NO_VALUE = -9999.0f;
inline bool has(float v) { return v > NO_VALUE + 1; }

struct Dist {
    char name[20];
    int count;
};

struct Summary {
    bool valid = false;
    char name[40] = "";
    char region[12] = "";
    char firmware[16] = "";
    uint32_t uptime = 0;
    uint32_t totalPackets = 0;
    uint32_t packetsLastHour = 0;
    uint32_t packetsLastMinute = 0;
    float packetsPerMinute = 0;
    int totalNodes = 0;
    int active24h = 0;
    float avgRssi = NO_VALUE, avgSnr = NO_VALUE;
    float bestRssi = NO_VALUE, bestSnr = NO_VALUE;
    int timeline[TIMELINE_BUCKETS] = {};
    char timelineStart[6] = "";
    int direct = 0, relayed = 0;
    bool relayEnabled = false;
    int relayCount = 0;
    float farthestMiles = 0;
    char farthestName[40] = "";
    Dist types[MAX_DIST] = {};
    int typeCount = 0;
    Dist protocols[MAX_DIST] = {};
    int protocolCount = 0;
};

struct Host {
    bool valid = false;
    float cpu = NO_VALUE, mem = NO_VALUE, disk = NO_VALUE, temp = NO_VALUE, load1 = NO_VALUE;
    uint32_t uptime = 0;
};

struct Node {
    char id[16];
    char shortName[8];
    char longName[40];
    char hw[32];
    char role[16];
    char protocol[12];
    char firmware[16];
    time_t lastHeard;
    time_t firstSeen;
    uint32_t packets;
    float rssi, snr;
    float battery, voltage, temperature, humidity, chUtil, airUtil;
    int hops;
    bool hasPosition;
    double lat, lon;
};

struct Packet {
    time_t ts;
    char source[16];
    char type[16];
    char protocol[12];
    float rssi, snr;
    int hops;
    char text[72];
};

struct Conversation {
    char id[40];
    char name[40];
    char last[84];
    char protocol[12];
    time_t ts;
    int unread;
    bool broadcast;
};

struct Message {
    time_t ts;
    char from[40];
    char text[200];
    char direction[10];
    float rssi, snr;
};

// A node with a position, from /api/nodes/map.
struct MapNode {
    char id[16];
    char name[28];
    double lat, lon;
    time_t lastHeard;
    float rssi;
    uint32_t packets;
    bool meshcore;
    uint32_t newUntilMs;  // highlight nodes that appeared since the previous fetch
};

// A heard link between two map nodes, from /api/analytics/topology.
struct MeshLink {
    int16_t a, b;  // indices into mapNodes
    float rssi, snr;
    time_t lastSeen;
};

enum class Link { NoWifi, Connecting, AuthFailed, Error, Ok };

// Meshpoint software update state (from /api/device/update-check and /api/update/apply/stream).
enum class UpdateState { Idle, Checking, Running, Done, Failed };

struct UpdateInfo {
    bool checked = false;
    bool available = false;
    char localVersion[16] = "";
    char remoteVersion[16] = "";
    char channel[24] = "stable";   // release channel to install from
    UpdateState state = UpdateState::Idle;
    char step[32] = "";            // current step while running
    char message[80] = "";         // result / error text
    uint32_t finishedMs = 0;
};

struct Status {
    Link link = Link::NoWifi;
    char error[64] = "";
    uint32_t lastOkMs = 0;     // millis() of last successful fetch
    char role[10] = "";
};

struct Model {
    Status status;
    UpdateInfo update;
    Summary summary;
    Host host;
    Node nodes[MAX_NODES];
    int nodeCount = 0;
    uint32_t nodesStamp = 0;
    Packet packets[MAX_PACKETS];
    int packetCount = 0;
    uint32_t packetsStamp = 0;
    Conversation convs[MAX_CONVS];
    int convCount = 0;
    // Messages for the conversation the UI asked for.
    char messagesFor[40] = "";
    Message messages[MAX_MESSAGES];
    int messageCount = 0;
    bool messagesLoading = false;
    // Map tab.
    MapNode mapNodes[MAX_MAP_NODES];
    int mapNodeCount = 0;
    MeshLink links[MAX_LINKS];
    int linkCount = 0;
    int linksUnplaced = 0;  // links whose endpoints have no position
    uint32_t mapStamp = 0;
    bool hasHome = false;   // the Meshpoint's own configured position
    double homeLat = 0, homeLon = 0;
    uint32_t version = 0;      // bumped on every change; UI redraws on change
};

Model& data();   // allocated in PSRAM
void lock();
void unlock();

struct Guard {
    Guard() { lock(); }
    ~Guard() { unlock(); }
};

// Find a node's display name by id; returns id if unknown. Caller holds lock.
const char* nodeName(const char* id);

}  // namespace model

namespace meshpoint {

// Start the background polling task.
void begin();
// Which UI page is showing (page::*), for polling priority.
void setActivePage(int page);
// Request messages for a conversation (async).
void requestConversation(const char* id);
// Force an immediate refresh / re-login (e.g. after settings change).
void refreshNow();
// Re-check GitHub for a newer Meshpoint version now.
void checkForUpdate();
// Install the available Meshpoint update (admin login required). Progress goes to model::data().update.
void startUpdate();

}  // namespace meshpoint
