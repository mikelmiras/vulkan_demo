"""Writes models/car/car.glb: a low-poly 1980s saloon, 1.8 x 4.3 m, its middle 0.75 m up (as make_city.py's cars),
facing -z. Three meshes, drawn with the scene's materials: 0 the painted body (rounded sides lofted along it, the
bonnet and boot below the beltline, the roof), 1 the glass all round the cabin (narrower and shorter at the roof:
the windscreen and the rear window slope), 2 a tyre (a unit cylinder along x, scaled by the scene).

    python3 tools/make_car.py
"""
import json
import math
import os
import struct

STATIONS = 28          # cross-sections along the body
CORNER = 7             # points round each rounded corner of a section
BOTTOM, BELT = -0.42, 0.2   # the body's underside and its beltline (metres from the middle)
HALF_WIDTH, HALF_LENGTH = 0.9, 2.15


def body_section(z):
    """The body's cross-section at z: half its width, its top (the bonnet and boot lower than the beltline), the
    radius of its upper corners. The plan rounds off at the nose and the tail."""
    t = abs(z) / HALF_LENGTH
    end = max(0.0, (t - 0.86) / 0.14)                    # 0 along the sides, 1 at the very end
    half = HALF_WIDTH * math.sqrt(max(1.0 - 0.55 * end * end, 0.0))
    if z < -0.95:      # the bonnet: sloping down to the nose
        u = (-0.95 - z) / (HALF_LENGTH - 0.95)
        top = BELT - 0.12 * u - 0.1 * u * u
    elif z > 1.3:      # the boot: a little lower, down at the tail
        u = (z - 1.3) / (HALF_LENGTH - 1.3)
        top = BELT - 0.04 - 0.08 * u * u
    else:
        top = BELT
    return half, top, 0.14


def section_ring(half, bottom, top, radius):
    """A rounded rectangle in (x, y), anticlockwise seen from +z: from the bottom's middle round to it again"""
    pts = []
    r = min(radius, half * 0.9, (top - bottom) * 0.45)
    rb = min(0.05, r)
    corners = [  # (centre x, centre y, radius, start angle)
        (half - rb, bottom + rb, rb, -math.pi / 2),
        (half - r, top - r, r, 0.0),
        (-half + r, top - r, r, math.pi / 2),
        (-half + rb, bottom + rb, rb, math.pi),
    ]
    for cx, cy, cr, a0 in corners:
        for k in range(CORNER):
            a = a0 + (math.pi / 2) * k / (CORNER - 1)
            pts.append((cx + cr * math.cos(a), cy + cr * math.sin(a)))
    return pts


def loft(rings, zs, positions, indices, closed=True):
    """Quads between consecutive rings (each the same count of points, at its z): vertices shared, so the
    importer's normals are smooth round the sides"""
    base = len(positions) // 3
    n = len(rings[0])
    for ring, z in zip(rings, zs):
        for x, y in ring:
            positions += [x, y, z]
    for s in range(len(rings) - 1):
        for k in range(n if closed else n - 1):
            a = base + s * n + k
            b = base + s * n + (k + 1) % n
            c = base + (s + 1) * n + (k + 1) % n
            d = base + (s + 1) * n + k
            # (outward: the rings run anticlockwise seen from +z and the stations go towards +z)
            indices += [a, b, c, a, c, d]


def cap(ring, z, positions, indices, facing):
    """A flat end (its own vertices: a crisp edge), a fan from its middle; `facing` -1 towards -z, +1 towards +z"""
    base = len(positions) // 3
    cx = sum(p[0] for p in ring) / len(ring)
    cy = sum(p[1] for p in ring) / len(ring)
    positions += [cx, cy, z]
    for x, y in ring:
        positions += [x, y, z]
    n = len(ring)
    for k in range(n):
        a, b = base + 1 + k, base + 1 + (k + 1) % n
        indices += [base, a, b] if facing > 0 else [base, b, a]


def plan_ring(half_w, z0, z1, y, radius, points=6):
    """A rounded rectangle in the horizontal plane at height y (x across, z along), anticlockwise seen from above"""
    pts = []
    r = min(radius, half_w * 0.9, (z1 - z0) * 0.45)
    # (anticlockwise from above: from +x towards -z, -x, +z; each corner's arc from one side's direction to the next)
    corners = [(half_w - r, z0 + r, 0.0), (-half_w + r, z0 + r, -math.pi / 2), (-half_w + r, z1 - r, -math.pi),
               (half_w - r, z1 - r, -1.5 * math.pi)]
    for cx, cz, a0 in corners:
        for k in range(points):
            a = a0 - (math.pi / 2) * k / (points - 1)
            pts.append((cx + r * math.cos(a), y, cz + r * math.sin(a)))
    return pts


def band(lower, upper, positions, indices):
    """Quads between two horizontal rings of the same count (lower below upper), shared vertices"""
    base = len(positions) // 3
    n = len(lower)
    for p in lower + upper:
        positions += list(p)
    for k in range(n):
        a, b = base + k, base + (k + 1) % n
        c, d = base + n + (k + 1) % n, base + n + k
        indices += [a, b, c, a, c, d]


def flat_top(ring, positions, indices, up=True):
    base = len(positions) // 3
    cx = sum(p[0] for p in ring) / len(ring)
    cz = sum(p[2] for p in ring) / len(ring)
    positions += [cx, ring[0][1], cz]
    for p in ring:
        positions += list(p)
    n = len(ring)
    for k in range(n):
        a, b = base + 1 + k, base + 1 + (k + 1) % n
        indices += [base, a, b] if up else [base, b, a]


def body():
    positions, indices = [], []
    zs = [-HALF_LENGTH + 2 * HALF_LENGTH * s / (STATIONS - 1) for s in range(STATIONS)]
    rings = []
    for z in zs:
        half, top, radius = body_section(z)
        rings.append(section_ring(half, BOTTOM, top, radius))
    loft(rings, zs, positions, indices)
    cap(rings[0], zs[0], positions, indices, -1)
    cap(rings[-1], zs[-1], positions, indices, +1)
    # The roof: a thin painted slab over the glass (glass(): its top ring)
    roof_low = plan_ring(0.7, -0.22, 0.98, 0.72, 0.18)
    roof_high = plan_ring(0.68, -0.2, 0.96, 0.76, 0.16)
    band(roof_low, roof_high, positions, indices)
    flat_top(roof_high, positions, indices, True)
    return positions, indices


def glass():
    positions, indices = [], []
    # From the beltline (a little inside the body's sides) up to under the roof: shorter and narrower at the top
    belt = plan_ring(0.84, -0.95, 1.28, BELT - 0.02, 0.2)
    top = plan_ring(0.7, -0.22, 0.98, 0.72, 0.18)
    band(belt, top, positions, indices)
    return positions, indices


def tyre(segments=20):
    positions, indices = [], []
    # A unit cylinder along x (radius 0.5, width 1: the scene scales it), its tread smooth, its sides flat
    ring = [(0.5 * math.cos(2 * math.pi * k / segments), 0.5 * math.sin(2 * math.pi * k / segments)) for k in range(segments)]
    base = 0
    for x in (-0.5, 0.5):
        for y, z in ring:
            positions += [x, y, z]
    for k in range(segments):
        a, b = k, (k + 1) % segments
        c, d = segments + (k + 1) % segments, segments + k
        indices += [a, b, c, a, c, d]
    for x, facing in ((-0.5, -1), (0.5, 1)):
        base = len(positions) // 3
        positions += [x, 0.0, 0.0]
        for y, z in ring:
            positions += [x, y, z]
        for k in range(segments):
            a, b = base + 1 + k, base + 1 + (k + 1) % segments
            indices += [base, a, b] if facing > 0 else [base, b, a]
    return positions, indices


def write_glb(path, meshes):
    blob = bytearray()
    views, accessors, gl_meshes = [], [], []
    for name, (positions, indices) in meshes:
        while len(blob) % 4:
            blob += b"\0"
        p_off = len(blob)
        blob += struct.pack("<%df" % len(positions), *positions)
        i_off = len(blob)
        blob += struct.pack("<%dI" % len(indices), *indices)
        xs, ys, zs = positions[0::3], positions[1::3], positions[2::3]
        views.append({"buffer": 0, "byteOffset": p_off, "byteLength": i_off - p_off, "target": 34962})
        views.append({"buffer": 0, "byteOffset": i_off, "byteLength": len(blob) - i_off, "target": 34963})
        accessors.append({"bufferView": len(views) - 2, "componentType": 5126, "count": len(positions) // 3, "type": "VEC3",
                          "min": [min(xs), min(ys), min(zs)], "max": [max(xs), max(ys), max(zs)]})
        accessors.append({"bufferView": len(views) - 1, "componentType": 5125, "count": len(indices), "type": "SCALAR"})
        gl_meshes.append({"name": name, "primitives": [{"attributes": {"POSITION": len(accessors) - 2},
                                                         "indices": len(accessors) - 1}]})
    while len(blob) % 4:
        blob += b"\0"
    doc = {"asset": {"version": "2.0", "generator": "night_drive/tools/make_car.py"}, "buffers": [{"byteLength": len(blob)}],
           "bufferViews": views, "accessors": accessors, "meshes": gl_meshes,
           "nodes": [{"mesh": k, "name": m["name"]} for k, m in enumerate(gl_meshes)],
           "scenes": [{"nodes": list(range(len(gl_meshes)))}], "scene": 0}
    text = json.dumps(doc, separators=(",", ":")).encode()
    while len(text) % 4:
        text += b" "
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob)))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))


here = os.path.dirname(os.path.abspath(__file__))
out_dir = os.path.join(here, "..", "models", "car")
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, "car.glb")
write_glb(out, [("body", body()), ("glass", glass()), ("tyre", tyre())])
print("wrote %s" % os.path.normpath(out))
