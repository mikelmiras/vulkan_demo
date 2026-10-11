#!/usr/bin/env python3
"""Checks that screenshots (PNG, from --screenshot) show something: decodable, and not one flat colour.

    python3 tools/check_screenshot.py shot.png [more.png ...]

Exits with 1 if any of them fails. Used by CI's smoke test (software Vulkan under Xvfb)."""
import struct
import sys
import zlib


def load(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    i, idat, w, h, ch = 8, b"", 0, 0, 0
    while i < len(data):
        n, = struct.unpack(">I", data[i:i + 4])
        kind, body = data[i + 4:i + 8], data[i + 8:i + 8 + n]
        i += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype not in (2, 6):
                raise ValueError("unsupported PNG (8-bit RGB / RGBA only)")
            ch = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
    raw, stride = zlib.decompress(idat), w * ch
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b, c = prev[x], prev[x - ch] if x >= ch else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, ch, rows


def check(path):
    w, h, ch, rows = load(path)
    if w < 64 or h < 64:
        return "too small (%dx%d)" % (w, h)
    colours = set()
    for y in range(0, h, max(1, h // 64)):
        row = rows[y]
        for x in range(0, w, max(1, w // 64)):
            colours.add(bytes(row[x * ch:x * ch + 3]))
    if len(colours) < 32:   # a blank or one-colour frame: nothing was drawn
        return "only %d colours: nothing drawn?" % len(colours)
    return None


failed = False
for path in sys.argv[1:]:
    try:
        problem = check(path)
    except (OSError, ValueError, zlib.error) as e:
        problem = str(e)
    print("%s: %s" % (path, problem or "ok"))
    failed |= problem is not None
sys.exit(1 if failed or len(sys.argv) < 2 else 0)
