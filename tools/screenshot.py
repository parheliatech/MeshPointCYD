#!/usr/bin/env python3
"""Grab the CYD framebuffer over USB serial and save it as a PNG.

    python3 tools/screenshot.py out.png [--port /dev/ttyACM0] [--cmd "tap 150 300"]...

Each --cmd is sent (with a short pause) before the screenshot is taken.
Requires pyserial and Pillow.
"""

import argparse
import time

import serial
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--wait", type=float, default=0.6, help="seconds to wait after each command")
    a = ap.parse_args()

    s = serial.Serial(a.port, 115200, timeout=5)
    time.sleep(0.2)
    s.reset_input_buffer()
    for c in a.cmd:
        s.write((c + "\n").encode())
        time.sleep(a.wait)
    s.reset_input_buffer()
    s.write(b"screenshot\n")
    while True:
        line = s.readline()
        if not line:
            raise SystemExit("timeout waiting for SCREENSHOT header")
        if line.startswith(b"SCREENSHOT"):
            _, w, h = line.split()
            w, h = int(w), int(h)
            break
    raw = s.read(w * h * 2)
    if len(raw) != w * h * 2:
        raise SystemExit(f"short read: {len(raw)}")
    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(w * h):
        v = (raw[2 * i] << 8) | raw[2 * i + 1]
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        px[i % w, i // w] = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
    img.save(a.out)
    print(f"saved {a.out}")


if __name__ == "__main__":
    main()
