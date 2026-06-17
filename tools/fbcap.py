#!/usr/bin/env python3
"""Capture framebuffer thumbnails dumped over serial by the lcdc_poc FB_DUMP
build and save each as a PNG.

Firmware emits, per sample:
    @FB <w> <h> <step>
    @R <y> <hex...>      (w pixels, 4 hex chars each, RGB565 big-endian nibble)
    ...
    @END

Usage:
    python fbcap.py [seconds] [out_prefix] [port] [baud]
Defaults: 14s, "fb", COM5, 115200. Saves out_prefix_000.png, _001.png, ...
and prints a per-frame summary (dims, missing rows, mean brightness).
"""
import sys
import re
import time

import serial
from PIL import Image

dur = float(sys.argv[1]) if len(sys.argv) > 1 else 14.0
prefix = sys.argv[2] if len(sys.argv) > 2 else "fb"
port = sys.argv[3] if len(sys.argv) > 3 else "COM5"
baud = int(sys.argv[4]) if len(sys.argv) > 4 else 115200

RE_FB = re.compile(r"@FB (\d+) (\d+) (\d+)")
RE_R = re.compile(r"@R (\d+) ([0-9a-fA-F]+)")


def rgb565_row(hexstr):
    px = []
    for i in range(0, len(hexstr), 4):
        v = int(hexstr[i:i + 4], 16)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        px.append(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31))
    return px


def save_frame(idx, meta, rows):
    w, h, step = meta
    img = Image.new("RGB", (w, h), (255, 0, 255))
    missing = []
    for y in range(h):
        if y not in rows:
            missing.append(y)
            continue
        for x, rgb in enumerate(rows[y]):
            if x < w:
                img.putpixel((x, y), rgb)
    name = f"{prefix}_{idx:03d}.png"
    img.save(name)
    # crude brightness so missing tiles show up numerically
    px = img.getdata()
    mean = sum(sum(p) for p in px) / (len(px) * 3)
    print(f"{name}: {w}x{h} step{step} missing_rows={len(missing)} mean={mean:.1f}")
    return name


def main():
    ser = serial.Serial(port, baud, timeout=1)
    t0 = time.time()
    idx = 0
    meta = None
    rows = {}
    saved = []
    while time.time() - t0 < dur:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("ascii", "replace").strip()
        m = RE_FB.search(line)
        if m:
            meta = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
            rows = {}
            continue
        m = RE_R.search(line)
        if m and meta:
            y = int(m.group(1))
            rows[y] = rgb565_row(m.group(2))
            continue
        if "@END" in line and meta:
            saved.append(save_frame(idx, meta, rows))
            idx += 1
            meta = None
    ser.close()
    print(f"captured {len(saved)} frame(s)")


if __name__ == "__main__":
    main()
