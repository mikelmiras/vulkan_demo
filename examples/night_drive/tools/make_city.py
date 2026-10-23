#!/usr/bin/env python3
"""Writes scenes/city.vkw: a few blocks of a dark, wet, 1980s New York-ish street grid at night, and a taxi to drive.
The buildings are Kenney's City Kit (Commercial) models (models/city, CC0), tinted for night, their windows lit by
models/city/Textures/night-windows.png (tools/make_window_map.py).

The same city every time (a fixed seed). Change the numbers below and run it again:

    python3 tools/make_city.py          (from the project folder)
"""
import json
import math
import os
import random

SEED = 1984
BLOCKS = 3            # blocks along each side
BLOCK = 44.0          # a block's side (m), sidewalk included
STREET = 16.0         # road width between sidewalks (m)
SIDEWALK = 3.5
LAMP_EVERY = 22.0     # street lamps along each street (m)

rng = random.Random(SEED)
puddle_rng = random.Random(SEED + 1)   # (its own: the rest of the city stays as it was without puddles)
traffic_rng = random.Random(SEED + 2)  # (and the traffic's)
neon_rng = random.Random(SEED + 4)     # (and which neon signs flicker)
word_rng = random.Random(SEED + 5)     # (and the words they spell)
TRAFFIC = 6           # cars driving the streets
LANE = 3.6            # from a street's middle to its lanes'
entities = []
next_id = [1]


def new_id():
    i = next_id[0]
    next_id[0] += 1
    return i


def entity(name, pos, rot=(0, 0, 0), scale=(1, 1, 1), parent=None, **parts):
    e = {"id": new_id(), "name": name, "transform": {"position": list(pos), "rotation": list(rot), "scale": list(scale)}}
    if parent is not None:
        e["parent"] = parent
    e.update(parts)
    entities.append(e)
    return e["id"]


def look(mesh, color, roughness=0.5, metallic=0.0, emissive=None, strength=1.0, clearcoat=0.0, coat_roughness=0.03):
    m = {"mesh": mesh, "color": list(color)}
    if roughness != 0.5:
        m["roughness"] = roughness
    if metallic:
        m["metallic"] = metallic
    if clearcoat:
        m["clearcoat"] = clearcoat
        m["clearcoatRoughness"] = coat_roughness
    if emissive:
        m["emissive"] = list(emissive)
        if strength != 1.0:
            m["emissiveStrength"] = strength
    return m


def box_collider(size):
    return {"shape": "box", "center": [0, 0, 0], "size": list(size)}


def point_light(color, intensity, rng_, shadows=False):
    return {"type": "point", "color": list(color), "intensity": intensity, "range": rng_, "castShadows": shadows}


# ---- Neon lettering: a stroke font (each letter's strokes in a unit cell, x right, y up) and the words signs spell
FONT = {
    "A": [((0, 0), (0.5, 1)), ((0.5, 1), (1, 0)), ((0.25, 0.5), (0.75, 0.5))],
    "B": [((0, 0), (0, 1)), ((0, 1), (0.8, 1)), ((0.8, 1), (0.8, 0.5)), ((0, 0.5), (1, 0.5)), ((1, 0.5), (1, 0)), ((1, 0), (0, 0))],
    "C": [((1, 1), (0, 1)), ((0, 1), (0, 0)), ((0, 0), (1, 0))],
    "D": [((0, 0), (0, 1)), ((0, 1), (0.7, 1)), ((0.7, 1), (1, 0.7)), ((1, 0.7), (1, 0.3)), ((1, 0.3), (0.7, 0)), ((0.7, 0), (0, 0))],
    "E": [((1, 1), (0, 1)), ((0, 1), (0, 0)), ((0, 0), (1, 0)), ((0, 0.5), (0.7, 0.5))],
    "G": [((1, 1), (0, 1)), ((0, 1), (0, 0)), ((0, 0), (1, 0)), ((1, 0), (1, 0.5)), ((1, 0.5), (0.5, 0.5))],
    "H": [((0, 0), (0, 1)), ((1, 0), (1, 1)), ((0, 0.5), (1, 0.5))],
    "I": [((0.5, 0), (0.5, 1)), ((0.2, 1), (0.8, 1)), ((0.2, 0), (0.8, 0))],
    "K": [((0, 0), (0, 1)), ((1, 1), (0, 0.5)), ((0, 0.5), (1, 0))],
    "L": [((0, 1), (0, 0)), ((0, 0), (1, 0))],
    "M": [((0, 0), (0, 1)), ((0, 1), (0.5, 0.5)), ((0.5, 0.5), (1, 1)), ((1, 1), (1, 0))],
    "N": [((0, 0), (0, 1)), ((0, 1), (1, 0)), ((1, 0), (1, 1))],
    "O": [((0, 0), (0, 1)), ((0, 1), (1, 1)), ((1, 1), (1, 0)), ((1, 0), (0, 0))],
    "P": [((0, 0), (0, 1)), ((0, 1), (1, 1)), ((1, 1), (1, 0.5)), ((1, 0.5), (0, 0.5))],
    "R": [((0, 0), (0, 1)), ((0, 1), (1, 1)), ((1, 1), (1, 0.5)), ((1, 0.5), (0, 0.5)), ((0.4, 0.5), (1, 0))],
    "S": [((1, 1), (0, 1)), ((0, 1), (0, 0.5)), ((0, 0.5), (1, 0.5)), ((1, 0.5), (1, 0)), ((1, 0), (0, 0))],
    "T": [((0, 1), (1, 1)), ((0.5, 1), (0.5, 0))],
    "U": [((0, 1), (0, 0)), ((0, 0), (1, 0)), ((1, 0), (1, 1))],
    "Z": [((0, 1), (1, 1)), ((1, 1), (0, 0)), ((0, 0), (1, 0))],
}
WORDS = ["BAR", "CLUB", "OPEN", "DELI", "HOTEL", "MOTEL", "PIZZA", "DINER", "GRILL", "BOOKS", "SUSHI", "DRUGS", "SALOON",
         "KEBAB"]
LETTER_H, LETTER_W, LETTER_GAP, STROKE = 0.6, 0.42, 0.14, 0.07


def neon_word(sign, word_rng, room, colour):
    """A word that fits in `room` metres, in glowing strokes on a dark board, centred on the sign's middle (its x
    along the sign, facing +z); its width"""
    width = lambda w: len(w) * LETTER_W + (len(w) - 1) * LETTER_GAP
    fitting = [w for w in WORDS if width(w) <= room - 0.3] or ["BAR"]
    word = word_rng.choice(fitting)
    total = width(word)
    entity("Board", (0, 0, -0.05), scale=(total + 0.4, LETTER_H + 0.35, 0.05), parent=sign,
           meshRenderer=look("Cube", (0.02, 0.02, 0.025), roughness=0.6))
    left = -total / 2
    for k, letter in enumerate(word):
        x0 = left + k * (LETTER_W + LETTER_GAP)
        for (ax, ay), (bx, by) in FONT[letter]:
            ax, bx = x0 + ax * LETTER_W, x0 + bx * LETTER_W
            ay, by = (ay - 0.5) * LETTER_H, (by - 0.5) * LETTER_H
            length = math.hypot(bx - ax, by - ay)
            angle = math.degrees(math.atan2(by - ay, bx - ax))
            entity("Neon", ((ax + bx) / 2, (ay + by) / 2, 0.02), (0, 0, angle), (length + STROKE, STROKE, STROKE),
                   parent=sign, meshRenderer=look("Cube", colour, emissive=colour, strength=3.0))
    return total


PITCH = BLOCK + STREET                      # from one block's start to the next's
SPAN = BLOCKS * BLOCK + (BLOCKS + 1) * STREET
HALF = SPAN / 2


def block_origin(i):                        # a block's low corner along one axis
    return -HALF + STREET + i * PITCH


# ---- The ground: a dark base under it all (the city's edge), then Kenney's City Kit Roads tiles on every street ------
ground = entity("Ground", (0, -0.01, 0), scale=(SPAN + 60, 1, SPAN + 60),
                meshRenderer=look("Plane", (0.02, 0.02, 0.024), roughness=0.3),
                collider={"shape": "box", "center": [0, -0.5, 0], "size": [1, 1, 1]})
# The tiles are 1 x 1 kit units: 0.8 of road with a kerb 0.1 wide each side, 0.02 high. Across, one spans the street;
# up, the kerbs match the sidewalks' 0.15 m. road-straight runs along x (turned 90 degrees for the streets along z).
ROAD_UP = 0.15 / 0.02
meshes_used = {}


def road_tile(name, pos, yaw, along):
    meshes_used[name] = "models/roads/%s.glb#0.0" % name
    m = look(name, (0.55, 0.55, 0.47), roughness=0.12)   # (wet: the lights reflect in it)
    m["texture"] = "Road colours"
    entity("Road", pos, (0, yaw, 0), (along, ROAD_UP, STREET), meshRenderer=m)


for k in range(BLOCKS + 1):
    c = -HALF + STREET / 2 + k * PITCH
    for j in range(BLOCKS + 1):
        a = -HALF + STREET / 2 + j * PITCH
        meshes_used["road-crossroad"] = "models/roads/road-crossroad.glb#0.0"
        m = look("road-crossroad", (0.55, 0.55, 0.47), roughness=0.12)
        m["texture"] = "Road colours"
        entity("Crossroad", (c, 0, a), scale=(STREET, ROAD_UP, STREET), meshRenderer=m)
    # Between the crossroads: each block's length in three tiles, zebra crossings at both ends
    for j in range(BLOCKS):
        a0 = block_origin(j)
        for t in range(3):
            name = "road-crossing" if t != 1 else "road-straight"
            a = a0 + BLOCK / 6 + t * BLOCK / 3
            road_tile(name, (a, 0, c), 0 if t != 2 else 180, BLOCK / 3)
            road_tile(name, (c, 0, a), 90 if t != 2 else 270, BLOCK / 3)
    # ... and out of the city on each side, into the dark
    for end in (-1, 1):
        a = end * (HALF + 15)
        road_tile("road-straight", (a, 0, c), 0, 30)
        road_tile("road-straight", (c, 0, a), 90, 30)
# Puddles: darker, glassier patches
for _ in range(26):
    x, z = rng.uniform(-HALF, HALF), rng.uniform(-HALF, HALF)
    entity("Puddle", (x, 0.012, z), (0, rng.uniform(0, 180), 0), (rng.uniform(2, 6), 1, rng.uniform(1.5, 4)),
           meshRenderer=look("Plane", (0.01, 0.012, 0.016), roughness=0.02, metallic=0.2))

# ---- Blocks: a sidewalk, Kenney's City Kit buildings round its edge facing the streets, a few neon signs ----------
# The kit's models: 1 unit is about 12 m here; their fronts face +Z, their bases sit at y = 0 (sizes from the GLBs)
KIT = 12.0
MODELS = {   # name: (width x, height, depth z) in kit units
    "building-a": (0.88, 1.29, 0.94), "building-b": (0.97, 1.29, 0.94), "building-c": (0.88, 0.89, 1.09),
    "building-d": (0.84, 1.29, 0.90), "building-e": (1.64, 0.89, 1.01), "building-f": (0.84, 1.69, 1.03),
    "building-g": (0.97, 1.69, 0.92), "building-h": (0.88, 1.29, 1.01), "building-i": (1.24, 1.68, 1.30),
    "building-j": (2.08, 1.69, 1.34), "building-k": (2.08, 1.47, 0.94), "building-l": (1.37, 2.27, 1.40),
    "building-m": (1.24, 3.15, 1.24), "building-n": (2.32, 2.48, 1.82),
    "building-skyscraper-a": (1.36, 2.88, 1.36), "building-skyscraper-c": (1.28, 4.08, 1.39),
}
TALL = {"building-l", "building-m", "building-n", "building-skyscraper-a", "building-skyscraper-c"}
# Night tints for the kit's white walls (the colour multiplies its palette texture), and for the lit windows
WALLS = [(0.32, 0.22, 0.2), (0.26, 0.24, 0.24), (0.3, 0.26, 0.22), (0.2, 0.2, 0.24), (0.34, 0.25, 0.22)]
GLOWS = [(1.0, 0.85, 0.6), (1.0, 0.75, 0.45), (0.85, 0.9, 1.0), (1.0, 0.8, 0.55)]
NEON = [(1.0, 0.1, 0.55), (0.1, 0.85, 1.0), (1.0, 0.35, 0.05), (0.6, 0.2, 1.0), (0.2, 1.0, 0.4)]

for bx in range(BLOCKS):
    for bz in range(BLOCKS):
        x0, z0 = block_origin(bx), block_origin(bz)
        cx, cz = x0 + BLOCK / 2, z0 + BLOCK / 2
        entity("Sidewalk", (cx, 0.075, cz), scale=(BLOCK, 0.15, BLOCK),
               meshRenderer=look("Cube", (0.11, 0.11, 0.115), roughness=0.75), collider=box_collider((1, 1, 1)))
        inner = BLOCK - 2 * SIDEWALK
        central = bx == BLOCKS // 2 and bz == BLOCKS // 2   # (the middle block: the tall ones)
        for side in range(4):   # along each side, its fronts on the street
            along = 0.0
            while along < inner - 6:
                names = [n for n in MODELS if (n in TALL) == (central and rng.random() < 0.6)]
                name = rng.choice(names)
                kw, kh, kd = MODELS[name]
                width, height, depth = kw * KIT, kh * KIT, kd * KIT
                if along + width > inner:   # (doesn't fit: a narrow one, or the side is done)
                    narrow = [n for n in names if MODELS[n][0] * KIT <= inner - along]
                    if not narrow:
                        break
                    name = rng.choice(narrow)
                    kw, kh, kd = MODELS[name]
                    width, height, depth = kw * KIT, kh * KIT, kd * KIT
                t = -inner / 2 + along + width / 2       # along the side
                n = inner / 2 - depth / 2                # in from the block's centre
                if side == 0:
                    px, pz, yaw, face = cx + t, cz - n, 180, (0, 0, -1)
                elif side == 1:
                    px, pz, yaw, face = cx + t, cz + n, 0, (0, 0, 1)
                elif side == 2:
                    px, pz, yaw, face = cx - n, cz + t, -90, (-1, 0, 0)
                else:
                    px, pz, yaw, face = cx + n, cz + t, 90, (1, 0, 0)
                sx, sz = (width, depth) if face[2] != 0 else (depth, width)
                # The building: an unscaled root (its collider; tagged so the camera can see through it) and the model
                # under it, so hiding the root hides it all
                root = entity("Building", (px, 0.15 + height / 2, pz), collider=box_collider((sx, height, sz)), tags=["Building"])
                meshes_used[name] = "models/city/%s.glb#0.0" % name
                model = look(name, rng.choice(WALLS), roughness=0.7, emissive=rng.choice(GLOWS), strength=rng.uniform(0.5, 1.6))
                model["texture"] = "City colours"
                model["emissiveTexture"] = "City windows"
                entity("Model", (0, -height / 2, 0), (0, yaw, 0), (KIT, KIT, KIT), parent=root, meshRenderer=model)
                # Now and then a neon sign over the door, and its coloured glow on the street
                if rng.random() < 0.3:
                    neon = rng.choice(NEON)
                    sw = min(width * 0.6, rng.uniform(3, 5))
                    y = rng.uniform(3.4, 5.0)
                    front = (px + face[0] * (sx / 2 + 0.3), pz + face[2] * (sz / 2 + 0.3))
                    # (a group turned with the building, its x along the sign: the tube, and a tube light as long a
                    # little out over the street, so the wet street mirrors a streak, not a dot; about one in eight on the blink)
                    flicker = neon_rng.random() < 0.12
                    sign = entity("Neon sign", (front[0] - px, 0.15 + y - (0.15 + height / 2), front[1] - pz), (0, yaw, 0),
                                  parent=root, **({"behaviours": [{"type": "NeonFlicker", "seed": neon_rng.randrange(1, 100000),
                                                                   "steady": round(neon_rng.uniform(2, 9), 2)}]}
                                                  if flicker else {}))
                    # (its word in glass tubes on a dark board: neon_word's strokes; the tube light as long as the word)
                    word_width = neon_word(sign, word_rng, sw, neon)
                    tube = point_light(neon, 2.2, 11.0)
                    tube["length"] = round(word_width * 0.9, 2)
                    entity("Neon light", (0, -0.4, 1.2), parent=sign, light=tube)
                along += width + rng.uniform(0.2, 1.2)

# ---- Street lamps: sodium orange, along the sidewalks' edges -----------------------------------------------------
lamp_count = 0
for k in range(BLOCKS + 1):
    road = -HALF + STREET / 2 + k * PITCH
    for side in (-1, 1):
        edge = road + side * (STREET / 2 - 0.6)
        a = -HALF + 8
        while a < HALF - 8:
            for (px, pz, arm) in ((edge, a, (-side, 0)), (a, edge, (0, -side))):
                # (only on sidewalks: not in a crossing)
                gap = (a + HALF) % PITCH
                if gap < STREET + 2:
                    continue
                # The kit's lamp post (its arm along -z, 0.6 x 0.21 kit units), turned so the arm leans over the road
                meshes_used["light-square"] = "models/roads/light-square.glb#0.0"
                post = look("light-square", (0.12, 0.12, 0.13), roughness=0.4, metallic=0.6)
                post["texture"] = "Road colours"
                yaw = math.degrees(math.atan2(-arm[0], -arm[1]))
                entity("Lamp post", (px, 0.15, pz), (0, yaw, 0), (10.5, 10.5, 10.5), meshRenderer=post)
                entity("Lamp post collider", (px, 3.3, pz), collider=box_collider((0.3, 6.3, 0.3)))
                entity("Lamp", (px + arm[0] * 1.9, 6.15, pz + arm[1] * 1.9), scale=(0.5, 0.08, 0.3),
                       meshRenderer=look("Cube", (0.3, 0.2, 0.1), emissive=(1.0, 0.55, 0.18), strength=4.0))
                entity("Lamp light", (px + arm[0] * 1.9, 5.8, pz + arm[1] * 1.9),
                       light=point_light((1.0, 0.58, 0.25), 2.4, 14.0))
                if puddle_rng.random() < 0.45:   # a puddle in the gutter under it: the lamp mirrored in the wet road
                    gx, gz = px + arm[0] * 2.6, pz + arm[1] * 2.6
                    size = (puddle_rng.uniform(2.5, 4.0), 1.0, puddle_rng.uniform(1.5, 2.5))
                    entity("Puddle", (gx, 0.1, gz), (0, puddle_rng.uniform(0, 180), 0), size,
                           decal={"color": [1, 1, 1], "opacity": 0.0, "roughness": -1, "wetness": 1.0, "angleFade": 0.5})
                lamp_count += 1
            a += LAMP_EVERY

# ---- Steam from the manholes (New York's) ---------------------------------------------------------------------------
for _ in range(7):
    k = rng.randrange(BLOCKS + 1)
    road = -HALF + STREET / 2 + k * PITCH + rng.uniform(-3, 3)
    a = rng.uniform(-HALF + 10, HALF - 10)
    pos = (road, 0.05, a) if rng.random() < 0.5 else (a, 0.05, road)
    entity("Manhole", (pos[0], 0.012, pos[2]), scale=(1.1, 1, 1.1), meshRenderer=look("Plane", (0.05, 0.05, 0.05), roughness=0.6, metallic=0.6))
    entity("Steam", pos, particles={
        "rate": 14, "burst": 0, "playOnStart": True, "duration": 0, "lifetime": 3.2, "lifetimeRandom": 0.4,
        "speed": 1.3, "speedRandom": 0.4, "spread": 18, "radius": 0.4, "gravity": -0.25, "drag": 0.6,
        "startSize": 0.7, "endSize": 3.4, "startColor": [0.5, 0.5, 0.55], "endColor": [0.35, 0.35, 0.4],
        "startAlpha": 0.22, "endAlpha": 0.0, "intensity": 1.0, "additive": False, "maxParticles": 120, "gpu": True})

# ---- Cars: painted metal (a metallic base under a clear coat, so the street's lights run along them), glass all round
# the cabin under a painted roof, chrome bumpers and tyres; 1.8 x 4.3 m, their entity's middle 0.75 m up, facing -z
PAINT = [(0.35, 0.03, 0.03), (0.05, 0.08, 0.2), (0.2, 0.2, 0.22), (0.02, 0.02, 0.02), (0.25, 0.18, 0.08)]


def paint_look(colour, mesh="Cube"):
    return look(mesh, colour, roughness=0.4, metallic=0.6, clearcoat=1.0, coat_roughness=0.04)


GLASS_COLOUR = (0.015, 0.018, 0.022)   # (dark: the reflection of the street is what shows)
CHROME = look("Cube", (0.75, 0.75, 0.78), roughness=0.18, metallic=1.0)
# The car model (tools/make_car.py: models/car/car.glb): its painted body, its glass, a tyre
for part, index in (("car-body", 0), ("car-glass", 1), ("car-tyre", 2)):
    meshes_used[part] = "models/car/car.glb#%d.0" % index


def car_body(car, paint, scale=(1.0, 1.0, 1.0), y=0.0, tyres=True):
    """A car's look on `car` (its middle 0.75 m up, facing -z): the model's body and glass, chrome bumpers, tyres;
    `scale` and `y` fit it to another size and ride height (the taxi)"""
    sx, sy, sz = scale
    entity("Body", (0, y, 0), scale=scale, parent=car, meshRenderer=paint_look(paint, "car-body"))
    entity("Glass", (0, y, 0), scale=scale, parent=car, meshRenderer=look("car-glass", GLASS_COLOUR, roughness=0.03))
    for z in (-1, 1):
        entity("Bumper", (0, y - 0.31 * sy, z * 2.17 * sz), scale=(1.66 * sx, 0.11, 0.1), parent=car, meshRenderer=CHROME)
    if tyres:
        for x in (-1, 1):
            for z in (-1, 1):
                entity("Tyre", (x * 0.82 * sx, -0.75 + 0.34, z * 1.42 * sz), scale=(0.24, 0.68, 0.68), parent=car,
                       meshRenderer=look("car-tyre", (0.02, 0.02, 0.02), roughness=0.9))


# ---- Parked cars along the kerbs ---------------------------------------------------------------------------------
for _ in range(16):
    k = rng.randrange(BLOCKS + 1)
    road = -HALF + STREET / 2 + k * PITCH
    side = rng.choice((-1, 1))
    lane = road + side * (STREET / 2 - 1.6)
    a = block_origin(rng.randrange(BLOCKS)) + rng.uniform(6, BLOCK - 6)
    along_x = rng.random() < 0.5
    pos = (a, 0.75, lane) if along_x else (lane, 0.75, a)
    paint = rng.choice(PAINT)
    car = entity("Parked car", pos, (0, 90 if along_x else 0, 0), collider=box_collider((1.8, 1.0, 4.3)))
    car_body(car, paint)

# ---- Traffic lights: two poles at each crossroads, one for each way through it (scripts/TrafficLight.lua: red,
# amber and green lamps shown in turn, each with its glow on the wet road; the traffic stops for them) ---------------
SIGNAL_LAMPS = (("red", (1.0, 0.06, 0.03), 4.62), ("amber", (1.0, 0.55, 0.05), 4.3), ("green", (0.1, 1.0, 0.45), 3.98))
for i in range(BLOCKS + 1):
    for j in range(BLOCKS + 1):
        cx, cz = -HALF + STREET / 2 + i * PITCH, -HALF + STREET / 2 + j * PITCH
        corner = STREET / 2 + 0.7
        # (the pole for traffic along x on the far right corner as it comes; for traffic along z on the other)
        for axis, (px, pz) in (("x", (cx + corner, cz - corner)), ("z", (cx - corner, cz + corner))):
            pole = entity("Signal pole", (px, 2.15, pz), scale=(0.14, 4.0, 0.14),
                          meshRenderer=look("Cube", (0.06, 0.06, 0.065), roughness=0.5, metallic=0.6))
            sig = entity("Signal", (px, 4.3, pz), behaviours=[{"type": "TrafficLight", "crossX": i, "crossZ": j, "axis": axis}])
            entity("Housing", (0, 0, 0), scale=(0.36, 1.05, 0.36), parent=sig,
                   meshRenderer=look("Cube", (0.02, 0.02, 0.022), roughness=0.4))
            for name, colour, y in SIGNAL_LAMPS:
                lamp = entity("Signal %d,%d %s %s" % (i, j, axis, name), (0, y - 4.3, 0), scale=(0.4, 0.26, 0.4), parent=sig,
                              meshRenderer=look("Cube", colour, emissive=colour, strength=6.0))
                entity("Glow", (0, -0.2, 0), parent=lamp, light=point_light(colour, 1.4, 7.0))

# ---- Traffic: cars driving the grid (scripts/TrafficCar.lua) in the right-hand lanes, turning at the crossroads,
# with headlights (no shadows: there are a few), tail lamps and spray from their wet tyres ---------------------------
def spray(parent, z):
    # (behind the back wheels, thrown up and back; its rate by speed: scripts/SprayBySpeed.lua)
    entity("Spray", (0, -0.55, z), rot=(60, 0, 0), parent=parent, particles={
        "rate": 0, "burst": 0, "playOnStart": True, "duration": 0, "lifetime": 0.45, "lifetimeRandom": 0.15,
        "speed": 3.0, "speedRandom": 0.4, "spread": 35, "radius": 0.8, "gravity": 9.0, "drag": 1.5,
        "startSize": 0.25, "endSize": 0.9, "startColor": [0.6, 0.65, 0.7], "endColor": [0.5, 0.55, 0.6],
        "startAlpha": 0.25, "endAlpha": 0.0, "intensity": 1.0, "additive": False, "maxParticles": 160,
        "lit": True, "gpu": True},
        behaviours=[{"type": "SprayBySpeed", "maxRate": 150, "fullSpeed": 14, "from": 1.5}])


for n in range(TRAFFIC):
    k = traffic_rng.randrange(BLOCKS + 1)
    street = -HALF + STREET / 2 + k * PITCH
    a = block_origin(traffic_rng.randrange(BLOCKS)) + traffic_rng.uniform(8, BLOCK - 8)
    sign = traffic_rng.choice((-1, 1))
    if traffic_rng.random() < 0.5:   # along x: its lane on the right of where it's heading
        pos, yaw = (a, 0.75, street + sign * LANE), -90 * sign
    else:
        pos, yaw = (street - sign * LANE, 0.75, a), 90 + 90 * sign
    paint = traffic_rng.choice(PAINT)
    car = entity("Traffic car", pos, (0, yaw, 0), collider=box_collider((1.8, 1.0, 4.3)),
                 behaviours=[{"type": "TrafficCar", "speed": traffic_rng.uniform(7.5, 10.5), "lane": LANE,
                              "first": -HALF + STREET / 2, "pitch": PITCH, "streets": BLOCKS + 1, "seed": n + 1}])
    car_body(car, paint)
    for side in (-1, 1):
        entity("Headlamp", (side * 0.5, -0.18, -2.14), scale=(0.3, 0.14, 0.04), parent=car,
               meshRenderer=look("Cube", (1, 1, 1), emissive=(1.0, 0.95, 0.8), strength=5.0))
        entity("Tail lamp", (side * 0.55, -0.1, 2.14), scale=(0.3, 0.12, 0.04), parent=car,
               meshRenderer=look("Cube", (0.5, 0.0, 0.0), emissive=(1.0, 0.05, 0.02), strength=6.0))
    entity("Headlights", (0, 0.2, -2.3), rot=(-14, 0, 0), parent=car,
           light={"type": "spot", "color": [1.0, 0.93, 0.78], "intensity": 3.5, "range": 22.0, "spotAngle": 48.0,
                  "spotSoftness": 0.8, "castShadows": False})
    entity("Tail glow", (0, 0.3, 2.5), parent=car, light=point_light((1.0, 0.08, 0.04), 0.5, 4.0))
    spray(car, 1.9)

# ---- The taxi (yours): a Jolt vehicle, front wheels steering, back ones driven, headlights -------------------------
start = (-HALF + STREET / 2 + PITCH + 3.5, 1.3, 10.0)
taxi = entity("Taxi", start, meshRenderer=look("Cube", (0.95, 0.68, 0.05), roughness=0.3, metallic=0.4), scale=(1, 1, 1),
              collider={"shape": "box", "center": [0, 0, 0], "size": [1.9, 0.8, 4.4]},
              rigidBody={"mass": 1300, "useGravity": True, "bounciness": 0.1, "friction": 0.6, "damping": 0.05},
              vehicle={"wheels": [
                  {"position": [-0.85, -0.25, -1.45], "radius": 0.38, "width": 0.26, "steering": True, "driven": False, "entity": "Wheel FL"},
                  {"position": [0.85, -0.25, -1.45], "radius": 0.38, "width": 0.26, "steering": True, "driven": False, "entity": "Wheel FR"},
                  {"position": [-0.85, -0.25, 1.45], "radius": 0.38, "width": 0.26, "steering": False, "driven": True, "entity": "Wheel BL"},
                  {"position": [0.85, -0.25, 1.45], "radius": 0.38, "width": 0.26, "steering": False, "driven": True, "entity": "Wheel BR"}],
                  "engineTorque": 520, "maxRpm": 6000, "maxSteer": 32, "suspensionLength": 0.3,
                  "suspensionFrequency": 2.2, "suspensionDamping": 0.55},
              behaviours=[{"type": "VehicleDriver", "steerSpeed": 3.0, "enabled": True}])
# (the body's look: a box scaled by a child, so the collider and the vehicle keep the entity's own scale)
for e in entities:
    if e["id"] == taxi:
        del e["meshRenderer"]
TAXI_YELLOW = (0.95, 0.66, 0.04)
# (the car model fitted to the taxi: 1.9 x 4.4 m, its underside where the box's was, its tyres on the wheels below)
TAXI_FIT = (1.9 / 1.8, 1.0, 4.4 / 4.3)
car_body(taxi, TAXI_YELLOW, TAXI_FIT, 0.12, tyres=False)
for e in entities:
    if e.get("parent") == taxi and e["name"] == "Body":   # (the taxi's paint: a little less metallic)
        e["meshRenderer"]["metallic"] = 0.3
        e["meshRenderer"]["roughness"] = 0.35
# (the livery: a chequered band along each side, black squares on the yellow, two rows staggered)
for x in (-1, 1):
    for row in range(2):
        for k in range(8):
            entity("Chequer", (x * 0.96, 0.1 - row * 0.16, -1.5 + (2 * k + row) * 0.18 + 0.09), scale=(0.02, 0.16, 0.18),
                   parent=taxi, meshRenderer=look("Cube", (0.015, 0.015, 0.015), roughness=0.3, clearcoat=1.0))
entity("Roof sign", (0, 0.99, 0.4), scale=(0.8, 0.22, 0.3), parent=taxi,
       meshRenderer=look("Cube", (1, 1, 0.9), emissive=(1.0, 0.9, 0.6), strength=3.0))
for side in (-1, 1):
    entity("Headlamp", (side * 0.52, -0.06, -2.19), scale=(0.3, 0.15, 0.04), parent=taxi,
           meshRenderer=look("Cube", (1, 1, 1), emissive=(1.0, 0.95, 0.8), strength=5.0))
    entity("Tail lamp", (side * 0.58, 0.02, 2.19), scale=(0.3, 0.12, 0.04), parent=taxi,
           meshRenderer=look("Cube", (0.5, 0.0, 0.0), emissive=(1.0, 0.05, 0.02), strength=6.0))
# (low beams: aimed down and wide with a soft edge, so the road is lit from just ahead of the bumper and the pool fades
# out instead of starting a few metres ahead behind a sharp edge)
entity("Headlights", (0, 0.2, -2.4), rot=(-14, 0, 0), parent=taxi,
       light={"type": "spot", "color": [1.0, 0.93, 0.78], "intensity": 5.0, "range": 28.0, "spotAngle": 50.0,
              "spotSoftness": 0.8, "castShadows": True})
entity("Tail glow", (0, 0.3, 2.6), parent=taxi, light=point_light((1.0, 0.08, 0.04), 0.6, 4.0))
spray(taxi, 2.1)
# Rain round the taxi (so wherever the camera follows it): streaks falling from a sphere above it, slanting a little
# in the wind, lit by the lamps, the neon and the headlights (dark elsewhere: they show where there's light)
# (seen from 100 m up: drops drawn a few centimetres wide, streaks a metre long, or they'd be under a pixel; on the
# GPU: thousands, splashing where they land)
entity("Rain", (0, 9, 0), rot=(172, 0, 0), parent=taxi, particles={
    "rate": 7000, "burst": 0, "playOnStart": True, "duration": 0, "lifetime": 0.95, "lifetimeRandom": 0.15,
    "speed": 11.0, "speedRandom": 0.15, "spread": 3, "radius": 20, "gravity": 0.0, "drag": 0.0,
    "startSize": 0.08, "endSize": 0.08, "startColor": [0.75, 0.8, 0.9], "endColor": [0.75, 0.8, 0.9],
    "startAlpha": 0.45, "endAlpha": 0.45, "intensity": 2.5, "additive": True, "maxParticles": 8000,
    "stretch": 0.09, "lit": True, "gpu": True})
for name, (x, z) in (("Wheel FL", (-0.85, -1.45)), ("Wheel FR", (0.85, -1.45)), ("Wheel BL", (-0.85, 1.45)), ("Wheel BR", (0.85, 1.45))):
    entity(name, (x, -0.25, z), parent=taxi)
    wheel = next_id[0] - 1
    entity("Tyre", (0, 0, 0), scale=(0.26, 0.76, 0.76), parent=wheel,
           meshRenderer=look("car-tyre", (0.02, 0.02, 0.02), roughness=0.9))

# ---- Light: a dim, cold moon over the haze; the camera, isometric-ish, following the taxi -------------------------
entity("Moon", (0, 60, 0), rot=(-58, 32, 0),
       light={"type": "directional", "color": [0.45, 0.55, 0.85], "intensity": 0.35, "castShadows": True})
entity("Camera", (start[0] + 20, 25, start[2] + 20), camera={"fov": 20, "near": 1.0, "far": 500},
       behaviours=[{"type": "ChaseCamera", "target": "Taxi", "distance": 115, "pitch": 66, "yaw": 45, "follow": 4, "lead": 0.5}])

# ---- Walk signals: a pole at each end of every zebra crossing, its walking figure (white) or hand (red) shown by
# scripts/TrafficLight.lua (axis "walk": every way red at the crossroads while the crossings are walked) ---------------
walk_rng = random.Random(SEED + 3)   # (its own, and the pedestrians': the rest of the city stays as it was)
ZEBRA = BLOCK / 6                    # a crossing's middle, from the block's corner (the road-crossing tile's middle)
for i in range(BLOCKS + 1):
    for j in range(BLOCKS + 1):
        cx, cz = -HALF + STREET / 2 + i * PITCH, -HALF + STREET / 2 + j * PITCH
        # (the four ways out of the crossroads that lead between blocks: a zebra on each, a pole at both its ends)
        arms = [(1, 0, i < BLOCKS), (-1, 0, i > 0), (0, 1, j < BLOCKS), (0, -1, j > 0)]
        for arm, (ax, az, inside) in enumerate(arms):
            if not inside:
                continue
            along = STREET / 2 + ZEBRA + 2.4   # (beside the stripes, not in the way)
            for end, side in enumerate((-1, 1)):
                across = side * (STREET / 2 + 0.6)
                px = cx + ax * along + (across if ax == 0 else 0)
                pz = cz + az * along + (across if az == 0 else 0)
                head = arm * 2 + end
                entity("Walk pole", (px, 1.3, pz), scale=(0.1, 2.6, 0.1),
                       meshRenderer=look("Cube", (0.06, 0.06, 0.065), roughness=0.5, metallic=0.6))
                sig = entity("Walk signal", (px, 2.75, pz),
                             behaviours=[{"type": "TrafficLight", "crossX": i, "crossZ": j, "axis": "walk", "head": head}])
                entity("Housing", (0, 0, 0), scale=(0.3, 0.6, 0.3), parent=sig,
                       meshRenderer=look("Cube", (0.02, 0.02, 0.022), roughness=0.4))
                for name, colour, y in (("red", (1.0, 0.12, 0.04), 0.14), ("green", (0.85, 0.95, 1.0), -0.14)):
                    entity("Signal %d,%d walk%d %s" % (i, j, head, name), (0, y, 0), scale=(0.34, 0.22, 0.34), parent=sig,
                           meshRenderer=look("Cube", colour, emissive=colour, strength=5.0))

# ---- Pedestrians: a few round each block's sidewalk, crossing at the zebras on the walk signal (scripts/
# Pedestrian.lua), under umbrellas some of them; their colliders stop the traffic ------------------------------------
COATS = [(0.25, 0.08, 0.06), (0.1, 0.12, 0.2), (0.3, 0.27, 0.2), (0.08, 0.08, 0.08), (0.4, 0.3, 0.1), (0.15, 0.2, 0.12)]
UMBRELLAS = [(0.02, 0.02, 0.02), (0.5, 0.05, 0.05), (0.05, 0.1, 0.35), (0.6, 0.5, 0.05)]
SKIN = [(0.85, 0.62, 0.5), (0.6, 0.42, 0.3), (0.38, 0.25, 0.18), (0.95, 0.75, 0.62)]
n_walkers = 0
for bx in range(BLOCKS):
    for bz in range(BLOCKS):
        for k in range(3):
            n_walkers += 1
            walker = entity("Pedestrian", (block_origin(bx) + 2, 0.15, block_origin(bz) + 2),
                            collider={"shape": "box", "center": [0, 0.875, 0], "size": [0.5, 1.75, 0.5]},
                            behaviours=[{"type": "Pedestrian", "blockX": bx, "blockZ": bz, "blocks": BLOCKS,
                                         "first": -HALF + STREET / 2, "pitch": PITCH, "street": STREET,
                                         "inset": round(walk_rng.uniform(1.3, 1.9), 2),
                                         "speed": round(walk_rng.uniform(1.15, 1.45), 2),
                                         "at": round(walk_rng.random(), 3), "clockwise": walk_rng.random() < 0.5,
                                         "seed": n_walkers}])
            coat = walk_rng.choice(COATS)
            entity("Legs", (0, 0.43, 0), scale=(0.34, 0.86, 0.2), parent=walker,
                   meshRenderer=look("Cube", (0.04, 0.04, 0.05), roughness=0.7))
            entity("Coat", (0, 1.16, 0), scale=(0.44, 0.64, 0.26), parent=walker,
                   meshRenderer=look("Cube", coat, roughness=0.6))
            entity("Head", (0, 1.62, 0), scale=(0.23, 0.26, 0.23), parent=walker,
                   meshRenderer=look("Sphere", walk_rng.choice(SKIN), roughness=0.55))
            if walk_rng.random() < 0.6:
                entity("Umbrella", (0, 2.05, 0), scale=(1.05, 0.14, 1.05), parent=walker,
                       meshRenderer=look("Sphere", walk_rng.choice(UMBRELLAS), roughness=0.25))

scene = {
    "format": "vkworld-scene",
    "version": 41,
    "meshes": [{"name": n, "source": meshes_used[n]} for n in sorted(meshes_used)],
    "textures": [{"name": "City colours", "source": "models/city/Textures/colormap.png"},
                 {"name": "City windows", "source": "models/city/Textures/night-windows.png"},
                 {"name": "Road colours", "source": "models/roads/Textures/colormap.png"}],
    "entities": entities,
    "settings": {
        "render": {
            "sky": [0.02, 0.024, 0.04], "zenith": [0.004, 0.005, 0.012], "ground": [0.01, 0.01, 0.012],
            "ambient": 0.1, "exposure": 1.15,
            "bloomStrength": 0.16, "bloomThreshold": 0.7, "aoStrength": 0.8, "aoRadius": 0.8,
            "reflections": True,
            # Street-level mist (thinning with height: from above, the rooftops stay clear) lit by the lamps, the
            # neon and the headlights; a filmic tone mapper and a little grading for the night look
            "volumetricFog": True, "fogDensity": 0.03, "fogHeightFalloff": 0.15, "fogLight": 3.0, "fogAnisotropy": 0.4,
            "tonemapper": "filmic", "contrast": 1.15, "saturation": 1.1, "vignette": 0.3, "shadowSoftness": 1.0,
            # Rain: everything facing up wet (darker, glossy: the lamps and the neon in it), standing water in the
            # hollows and the gutters' puddles, rings on it
            "wetness": 0.85, "puddles": 0.4, "rain": 0.7,
            # Contact shadows: tyres on the road, kerbs, the lamp posts' feet (the shadow maps are coarse from up there)
            "contactShadows": True,
            # The street lamps drawn as a camera's iris would: faint six-pointed stars
            "starburst": 0.5, "starburstBlades": 6,
        }
    },
}

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "..", "scenes", "city.vkw")
with open(out, "w") as f:
    json.dump(scene, f, indent=1)
print("wrote %s: %d entities, %d street lamps, %d pedestrians" % (os.path.normpath(out), len(entities), lamp_count, n_walkers))
