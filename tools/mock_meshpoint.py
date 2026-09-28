#!/usr/bin/env python3
"""Mock Meshpoint API for testing the CYD display without a real Meshpoint.

Mirrors the response shapes of the real endpoints (src/api/routes/*.py in
KMX415/meshpoint) including Bearer-token auth via POST /api/auth/login.

    python3 tools/mock_meshpoint.py [port] [--old] [--any-password] [--update] [--update-fail]

Default port 8080, password "test".
  --old           mimic Meshpoint versions that return the session only as a cookie
  --any-password  accept any password (handy for pointing a configured display at the mock)
  --update        report an available update (0.7.7 -> 0.7.9) and simulate installing it
  --update-fail   like --update, but the install fails at "git fetch"
"""

import json
import random
import sys
import time
import urllib.parse
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PASSWORD = "test"
TOKEN = "mock-jwt-token"
START = time.time()
UPDATE = {"available": "--update" in sys.argv or "--update-fail" in sys.argv, "local": "0.7.7"}

NODES = [
    ("!a1b2c3d4", "Ridge Relay \U0001F3D4", "RDG", "RAK4631", "ROUTER"),
    ("!0badcafe", "Kendel Base", "KB", "HELTEC_V3", "CLIENT"),
    ("!13572468", "Water Tower", "WTR", "STATION_G2", "ROUTER"),
    ("!deadbeef", "Mobile Truck", "TRK", "T_ECHO", "TRACKER"),
    ("!feedf00d", "Solar Sensor 1", "SS1", "RAK4631", "SENSOR"),
    ("!24681357", "Library Node", "LIB", "TBEAM", "CLIENT"),
    ("!55aa55aa", "Hilltop Repeater with a very long name", "HTR", "RAK4631", "REPEATER"),
    ("!99887766", "Cafe", "CAF", "HELTEC_V3", "CLIENT"),
    ("mc:4f2a91", "MeshCore Companion", "MC", "", "CLIENT"),
    ("!31415926", "Pi Node", "PI", "PORTDUINO", "CLIENT_MUTE"),
]
TYPES = ["position", "telemetry", "nodeinfo", "text", "routing", "encrypted", "traceroute"]


def iso(dt):
    return dt.isoformat()


def now():
    return datetime.now(timezone.utc)


def node_list():
    out = []
    for i, (nid, ln, sn, hw, role) in enumerate(NODES):
        heard = now() - timedelta(seconds=30 + i * i * 170)
        out.append({
            "node_id": nid, "long_name": ln, "short_name": sn, "hardware_model": hw or None,
            "firmware_version": "2.5.6", "protocol": "meshcore" if nid.startswith("mc:") else "meshtastic",
            "role": role, "public_key": "x" * 44, "latitude": 40.01 + i / 100, "longitude": -105.27 - i / 100,
            "altitude": 1600, "last_heard": iso(heard), "first_seen": iso(heard - timedelta(days=i + 1)),
            "packet_count": 1200 // (i + 1), "display_name": ln, "has_position": i % 3 != 2,
            "latest_rssi": -60 - i * 7 if i != 9 else None, "latest_snr": 9.5 - i * 1.7 if i != 9 else None,
            "latest_capture_source": "concentrator", "latest_battery": [101, 87, 64, 32, 15, 100, 76, None, 55, None][i],
            "latest_voltage": 4.1 - i * 0.05, "latest_temperature": 21.5 + i if i % 2 else None,
            "latest_humidity": 40 + i if i % 2 else None, "latest_channel_util": 12.5 + i,
            "latest_air_util": 1.2 + i / 10, "latest_hops": [0, 0, 1, 2, 0, 3, 1, 0, 0, 0][i],
        })
    return out


def packets(limit):
    out = []
    random.seed(int(time.time() / 4))
    for i in range(min(limit, 40)):
        nid = NODES[random.randrange(len(NODES))][0]
        t = random.choice(TYPES)
        payload = {"text": random.choice(["Hello mesh!", "Anyone on the ridge tonight? Signal is great from up here \U0001F4E1", "ack", "Testing 1 2 3"])} if t == "text" else {"battery_level": 80}
        hs, hl = 3, 3 - random.randrange(0, 3)
        out.append({
            "packet_id": f"{random.getrandbits(32):08x}", "source_id": nid, "destination_id": "!ffffffff",
            "protocol": "meshcore" if nid.startswith("mc:") else "meshtastic", "packet_type": t,
            "hop_limit": hl, "hop_start": hs, "hop_count": hs - hl, "channel_hash": 8, "want_ack": False,
            "via_mqtt": False, "relay_node": None, "decoded_payload": payload, "decrypted": t != "encrypted",
            "capture_source": "concentrator", "timestamp": iso(now() - timedelta(seconds=i * 23 + 2)),
            "signal": {"rssi": -55 - random.random() * 70, "snr": 12 - random.random() * 20, "frequency_mhz": 906.875,
                       "spreading_factor": 11, "bandwidth_khz": 250, "coding_rate": "4/5",
                       "signal_quality_percent": 70, "timestamp": iso(now())},
        })
    return out


def summary():
    random.seed(int(time.time() / 10))
    return {
        "device": {"name": "Cozy Meshpoint", "region": "US", "firmware": "0.7.9",
                   "uptime_seconds": int(time.time() - START) + 3 * 86400 + 5000, "days_online": 3},
        "first_packet_time": iso(now() - timedelta(days=3)),
        "live": {"total_packets": 100, "packets_per_minute": 3.2},
        "signal": {"avg_rssi": -93.4, "min_rssi": -127, "max_rssi": -41, "avg_snr": 4.2, "sample_count": 200,
                   "best_rssi": -41.0, "best_snr": 13.2},
        "rssi_distribution": {}, "snr_distribution": {},
        "traffic": {"total_packets": 48213 + int(time.time() - START) // 5, "packets_last_hour": 212,
                    "packets_last_minute": 4, "packets_per_minute": 3.5,
                    "protocol_distribution": {"meshtastic": 45000, "meshcore": 3213},
                    "type_distribution": {"position": 15000, "telemetry": 14000, "nodeinfo": 7000, "text": 2100,
                                          "routing": 5000, "encrypted": 4000, "traceroute": 1113}},
        "traffic_timeline": {"labels": [(now() - timedelta(minutes=60 - 5 * i)).strftime("%H:%M") for i in range(12)],
                             "counts": [random.randint(5, 30) for _ in range(12)]},
        "network": {"total_nodes": len(NODES), "nodes_with_position": 7, "total_packets_seen": 40000,
                    "protocols": {"meshtastic": 9, "meshcore": 1}, "roles": {}, "hw_models": {},
                    "active_24h": 8, "total_nodes": 57},
        "relay": {"enabled": True, "relayed": 312, "rejected": 20},
        "direct_relayed": {"direct": 30000, "relayed": 18213},
        "farthest_mesh": {"miles": 23.4, "node_id": "!55aa55aa", "node_name": "Hilltop Repeater"},
    }


def metrics():
    return {"cpu_percent": 12.5 + random.random() * 10, "memory_percent": 41.2, "memory_used_mb": 800,
            "memory_total_mb": 1900, "disk_percent": 23.4, "disk_used_gb": 6.1, "disk_total_gb": 29.0,
            "cpu_temp_c": 52.3, "load_avg": [0.31, 0.25, 0.2], "system_uptime_seconds": 12 * 86400 + 3600}


def conversations():
    return [
        {"node_id": "broadcast:LongFast", "node_name": "broadcast:LongFast", "protocol": "meshtastic",
         "last_message": "Anyone on the ridge tonight?", "last_timestamp": iso(now() - timedelta(minutes=2)),
         "unread_count": 3, "is_broadcast": True},
        {"node_id": "!0badcafe", "node_name": "Kendel Base", "protocol": "meshtastic",
         "last_message": "ok see you there", "last_timestamp": iso(now() - timedelta(hours=1)),
         "unread_count": 0, "is_broadcast": False},
        {"node_id": "mc:4f2a91", "node_name": "MeshCore Companion", "protocol": "meshcore",
         "last_message": "ping", "last_timestamp": iso(now() - timedelta(days=1)),
         "unread_count": 1, "is_broadcast": False},
    ]


def conversation(node_id, limit):
    msgs = []
    texts = ["Anyone on the ridge tonight?", "I'm up at the water tower, hearing you at -80",
             "Nice. Relay is working great today \U0001F44D", "Heading up in 20 min, will check in",
             "This is a longer message to verify word wrapping across several lines on the small display works properly."]
    for i, t in enumerate(texts):
        sent = i == 3
        msgs.append({"id": i, "direction": "sent" if sent else "received", "text": t, "node_id": node_id,
                     "node_name": "" if sent else NODES[i][1], "protocol": "meshtastic", "channel": 0,
                     "timestamp": iso(now() - timedelta(minutes=50 - i * 10)), "status": "read",
                     "packet_id": None, "rx_count": 1, **({} if sent else {"rssi": -80.0 - i, "snr": 5.5})})
    return list(reversed(msgs))[:limit]  # newest first, like the real query


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code, obj, cookie=None):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        if cookie:
            self.send_header("Set-Cookie", f"meshpoint_session={cookie}; HttpOnly; Max-Age=3600; Path=/; SameSite=lax")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        data = json.loads(self.rfile.read(length) or b"{}")
        if self.path == "/api/auth/login":
            if data.get("username") in ("admin", "viewer") and (data.get("password") == PASSWORD or "--any-password" in sys.argv):
                body = {"role": data["username"]}
                if self.headers.get("X-Meshpoint-Client") and "--old" not in sys.argv:
                    body["token"] = TOKEN  # newer Meshpoints; older ones only set the cookie
                return self._send(200, body, cookie=TOKEN)
            return self._send(401, {"detail": "invalid_credentials"})
        if self.path == "/api/update/apply/stream":
            return self._stream_update(data.get("channel_id"))
        self._send(404, {"detail": "Not Found"})

    def _stream_update(self, channel):
        """Mimic the NDJSON progress stream; the final step restarts the service (connection drops)."""
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        def emit(obj, pause=1.5):
            self.wfile.write((json.dumps(obj) + "\n").encode())
            self.wfile.flush()
            time.sleep(pause)

        emit({"type": "started", "mode": "apply", "branch": "main"})
        for step in ("git fetch", "git checkout", "git reset"):
            emit({"type": "step", "step": step, "phase": "started"})
            if step == "git fetch" and "--update-fail" in sys.argv:
                emit({"type": "step", "step": step, "phase": "failed"})
                emit({"type": "result", "result": {"success": False, "duration_seconds": 3.0, "pre_update_sha": None,
                                                   "target_branch": "main", "failed_step": step, "log": []}})
                return
            emit({"type": "step", "step": step, "phase": "ok"})
        emit({"type": "step", "step": "upgrade", "phase": "started"}, pause=1)
        UPDATE["local"], UPDATE["available"] = "0.7.9", False  # "service restarted" on the new version
        # connection closes here without a result line, like the real restart

    def do_GET(self):
        authed = (self.headers.get("Authorization") == f"Bearer {TOKEN}"
                  or f"meshpoint_session={TOKEN}" in (self.headers.get("Cookie") or ""))
        if not authed:
            return self._send(401, {"detail": "authentication required"})
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        limit = int(q.get("limit", ["100"])[0])
        p = u.path
        if p == "/api/stats/summary":
            return self._send(200, summary())
        if p == "/api/device/update-check":
            return self._send(200, {"update_available": UPDATE["available"], "local_version": UPDATE["local"],
                                    "remote_version": "0.7.9"})
        if p == "/api/update/install_status":
            return self._send(200, {"local_version": UPDATE["local"], "active_channel_id": None})
        if p == "/api/device/metrics":
            return self._send(200, metrics())
        if p == "/api/nodes":
            return self._send(200, node_list()[:limit])
        if p == "/api/packets":
            return self._send(200, packets(limit))
        if p == "/api/messages/conversations":
            return self._send(200, conversations())
        if p.startswith("/api/messages/conversation/"):
            nid = urllib.parse.unquote(p[len("/api/messages/conversation/"):])
            return self._send(200, conversation(nid, limit))
        self._send(404, {"detail": "Not Found"})

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = int(args[0]) if args else 8080
    print(f"Mock Meshpoint on :{port} (user viewer/admin, password '{PASSWORD}')")
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
