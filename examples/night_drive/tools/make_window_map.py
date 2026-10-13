#!/usr/bin/env python3
"""Writes models/city/Textures/night-windows.png: the City Kit's palette (colormap.png, 16 columns of 32 x 128 px
cells) as an emissive map for night: the window cells (the blues) glow warm, the orange and pink ones (signs, shop
fronts) glow softly, the rest is dark. Plain Python (zlib): run it from the project folder."""
import os
import struct
import zlib

W = H = 512
here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "..", "models", "city", "Textures", "night-windows.png")


def cell_glow(col, row):
    # (the palette: row 1 holds greens, yellows, oranges, reds, blues, light blues, purples, pinks; two columns each)
    if row == 1 and col in (8, 9, 10, 11):     # blues and light blues: glass
        return (255, 196, 120)
    if row == 1 and col in (4, 5, 14, 15):     # oranges and pinks: shop fronts and signs
        return (150, 70, 40) if col < 8 else (160, 40, 120)
    return (0, 0, 0)


rows = []
for y in range(H):
    line = bytearray([0])   # (filter: none)
    for x in range(W):
        r, g, b = cell_glow(x // 32, y // 128)
        line += bytes((r, g, b, 255))
    rows.append(bytes(line))
raw = zlib.compress(b"".join(rows), 9)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)) + chunk(b"IDAT", raw) + chunk(b"IEND", b"")
with open(out, "wb") as f:
    f.write(png)
print("wrote", os.path.normpath(out))
