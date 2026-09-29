# Why the Meshpoint web page shows ~22% CPU and the CYD shows ~0%

Date: 2026-09-29. Meshpoint v0.7.7 on a Raspberry Pi 4 (192.168.86.106:8080), CYD firmware from this repo.

## About the CYD viewer (for Meshpoint developers)

**Repo:** https://github.com/parheliatech/MeshPointCYD (AGPL-3.0)

MeshPoint CYD is a small desk display for a Meshpoint node. It runs on a Freenove FNK0104N
(ESP32-S3, 3.5" touchscreen, "cheap yellow display" class hardware) and shows the mesh at a
glance without a browser: packet rate and signal levels, the node list, a live map, the packet
feed, channel and direct messages, and the health of the Meshpoint's Pi (CPU, temperature,
memory, disk). It plays alert chimes for new messages and nodes, and can tell you about a
Meshpoint update and trigger it (after confirmation). Apart from that update, it only reads from
the Meshpoint.

How it talks to Meshpoint:

- It uses the same HTTP API as the web dashboard, at the Meshpoint's normal address
  (for example `http://192.168.86.106:8080`).
- It logs in with `POST /api/auth/login` and sends the session back as the
  `meshpoint_session` cookie. On v0.7.7 the token comes only in `Set-Cookie`, not in the body.
- It polls a few endpoints on timers. The one this document is about, `GET /api/device/metrics`,
  is fetched every 15 s while the Home tab is showing, on its own, not batched with other
  requests (`fetchHost()` in `src/meshpoint.cpp`).
- The display shows `cpu_percent` as received, with one decimal place under 10%.

The firmware is Arduino/PlatformIO C++ for the ESP32-S3. See the README in the repo for setup.

## Summary

The Meshpoint web dashboard reports CPU around 20-24%. The CYD reports 0.0% for the same
Meshpoint. Both read the same API field, so neither is misreading it. The web page's number is
inflated by the page's own requests, and the CYD's number is closer to the Pi's real
steady-state load. The Pi is close to idle when nobody has the dashboard open.

Both claims are now backed by measurement: the false peak was reproduced from outside, and the
Pi's real CPU was measured directly over SSH (about 2% busy, see "Direct measurement on the Pi").

## What both clients read

- Web page: `frontend/js/app.js:395` sets the CPU tile from `metrics.cpu_percent`.
- CYD: `fetchHost()` in `src/meshpoint.cpp` reads `cpu_percent` from the same endpoint,
  `GET /api/device/metrics`, every 15 s while on the Home page.
- Server: `src/api/routes/system_metrics.py` returns `psutil.cpu_percent(interval=0.5)`.

That call blocks for half a second and reports the average CPU busy share of the whole machine
over that window. So every reading is "how busy was the Pi during the 0.5 s in which this
request was handled", not a long-term average.

## Why the web page reads high

Every 15 s the page runs `_refreshData()` and `_updateStats()`. `_updateStats()` requests these
at the same time (`frontend/js/app.js:356-360`):

- `/api/analytics/traffic`
- `/api/analytics/signal/summary`
- `/api/nodes/count`
- `/api/device/status`
- `/api/device/metrics`

`_refreshData()` adds `/api/nodes?enrich=true` (787 nodes) and `/api/packets?limit=50`, and the
page also keeps WebSocket streams open. Building those responses takes real CPU on the Pi.
Because the metrics request is in the same batch, its 0.5 s sampling window overlaps the work
needed to answer the others. The page ends up measuring the cost of loading itself.

Since this happens on every refresh, the sample always lands in the burst and never in the idle
time between bursts. That is why the tile hovers steadily around 20-24% instead of moving
between low and high values.

## Tests run from the laptop (curl, one login per test)

| Test | cpu_percent |
|---|---|
| Metrics alone, repeated (what the CYD does) | 0.0, 0.0, 0.0, 1.0, 0.0 |
| Metrics alone, browser tab open on laptop | 0.0 on six samples, 2 s apart |
| Dashboard requests replayed one after another, sampling in between | 5.0, 0.0, 0.5 |
| Metrics fired at the same instant as the dashboard's other requests | 0.5, 25.2, 21.6 |

RAM and disk matched the web page exactly (14.1%, 536 / 3796 MB, 67.3%), so the same data
source is being read. Only CPU differed. The concurrent test (21.6% and 25.2%) matches what
the page shows (22.6% at the time of the last observation).

## Direct measurement on the Pi

Taken over SSH on 2026-09-29 (Debian 13 "trixie", 4 cores, up 55 minutes):

```
$ top -bn2 -d5 | grep "^%Cpu" | tail -1
%Cpu(s):  1.3 us,  0.8 sy,  0.0 ni, 97.8 id,  0.0 wa,  0.0 hi,  0.0 si,  0.0 st
$ uptime
 load average: 0.19, 0.82, 2.29
```

- Real CPU over a 5 s average: about 2.2% busy (97.8% idle). This is the steady-state figure
  and is close to what the CYD shows (0-1%). The web page's 20-24% is about ten times higher.
- The 1-minute load average was 0.19, the same range as the earlier 0.07-0.20 readings. The
  15-minute figure (2.29) is inflated by startup: the Pi had rebooted 55 minutes earlier
  (the web page had shown 1d 15h uptime before that).
- Earlier indirect evidence agrees: 22% of 4 cores would put the load average near 1.0, and
  it stayed far below that. CPU temperature was steady at 45-47 C.

Normal operation here means the Pi receiving and storing LoRa packets (about 5 per minute),
with nobody browsing the dashboard. The spikes are caused by viewing the dashboard.

## Caveats

- I did not record whether the dashboard was open in a browser during the `top` run. If it
  was closed, 2.2% is the idle baseline. If it was open, the bursts every 15 s are short enough
  to barely show in a 5 s average, which would still support the same conclusion.
- One `top` run is a small sample. A longer capture (for example `top -bn12 -d5`, a minute)
  with the dashboard closed and then open would show the burst directly.
- I could not run the real browser session from here, so any extra load from the WebSocket
  streams is untested.
- The 0.5 s window in Meshpoint's own reading is short, so single readings are noisy. That is
  why I sampled repeatedly.

## Consequences

- The CYD's 0% is a fair reading of an idle Pi. It is not hiding a real 22%.
- Keep the Meshpoint dashboard closed while running extra software on the Pi (for example an
  RTL-SDR scanner). Having the page open costs a burst of CPU every 15 s.
- The CYD could show the 1-minute load average, or a CPU value averaged over several polls, if
  a steadier measure is wanted. Not implemented.

## Suggestion for Meshpoint (not implemented, untested)

`/api/device/metrics` calls `psutil.cpu_percent(interval=0.5)`, so each reading measures
whatever the Pi happens to be doing in a 0.5 s window, including the work of answering the
requests that arrived with it. Two options that would make the number match reality:

- Sample in the background. Call `psutil.cpu_percent(interval=None)` on a timer (for example
  every few seconds) and have the endpoint return the latest value or a short moving average.
  This also stops the request handler from blocking for 0.5 s.
- Or have the web page request metrics separately from the heavy node and packet requests.
  That only helps the page, not other clients.

I have not tried either change. The point is that the current number depends on when and with
what other requests it is taken, so the CYD and the web page can legitimately disagree.

## Other findings from the SSH check

- An RTL-SDR dongle (Realtek RTL2838, `0bda:2838`) is already plugged into the Pi's USB 2.0
  bus behind a VIA hub.
- A USB keyboard and mouse are attached and 3 users were logged in, so a desktop session may be
  running. It was not checked.
