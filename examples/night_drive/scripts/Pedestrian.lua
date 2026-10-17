-- Someone walking the sidewalks: round their block (a ring a little in from the kerb), and now and then over a zebra
-- crossing to the next block, waiting at the kerb for the walk signal (TrafficLight.lua's cycle: every crossroads
-- has a phase with all the traffic held on red, the walk phase, when its four crossings are walked). Their collider
-- moves with them, so the traffic's look-ahead stops for them too.
local Pedestrian = {
    description = "Walks round a block, crossing to the next at the zebras on the walk signal",
    fields = {
        {"blockX", 0, min = 0, max = 64},           -- the block they start round (indices along x and z)
        {"blockZ", 0, min = 0, max = 64},
        {"blocks", 3, min = 1, max = 64},           -- blocks each way
        {"first", -90, min = -1000, max = 1000},    -- the first street's middle, along x and z (the grid's)
        {"pitch", 60, min = 1, max = 500},          -- from one street to the next
        {"street", 16, min = 1, max = 100},         -- a street's width
        {"inset", 1.6, min = 0.2, max = 5},         -- their ring, in from the kerb
        {"speed", 1.3, min = 0.1, max = 5},         -- walking (m/s); crossing a little faster
        {"at", 0, min = 0, max = 1},                -- where round the ring they start (0..1)
        {"clockwise", false},
        {"seed", 1, min = 0, max = 100000},
        {"curb", 0.15, min = 0, max = 1},           -- the sidewalks' height
    },
}

-- The crossroads' cycle (TrafficLight.lua's, the same sums): 36 seconds; the walk phase from 22 on (how far into it)
local function walkTime(t, i, j)
    return (t + (i + j) * 2.5) % 36 - 22
end

function Pedestrian:random()
    self.rng = (self.rng * 1103515245 + 12345) % 2147483648
    return self.rng / 2147483648
end

-- The block's ring: its low corner along x and z (in from the kerb), its side's length
function Pedestrian:ring()
    local block = self.pitch - self.street
    local lowX = self.first + self.street / 2 + self.bx * self.pitch + self.inset
    local lowZ = self.first + self.street / 2 + self.bz * self.pitch + self.inset
    return lowX, lowZ, block - 2 * self.inset
end

-- A place round the ring (s: 0..4L, anticlockwise from the low corner seen from above: +x, +z, -x, -z)
function Pedestrian:place(s)
    local x0, z0, L = self:ring()
    s = s % (4 * L)
    if s < L then return x0 + s, z0 end
    if s < 2 * L then return x0 + L, z0 + (s - L) end
    if s < 3 * L then return x0 + L - (s - 2 * L), z0 + L end
    return x0, z0 + L - (s - 3 * L)
end

-- The ring's crossings: where each side meets a zebra (a sixth of a block in from each end), the street it crosses,
-- its crossroads, and the block on the other side
function Pedestrian:crossings()
    local x0, z0, L = self:ring()
    local zebra = (self.pitch - self.street) / 6 - self.inset   -- (along the side from its corner)
    local list = {}
    local sides = {   -- side: its direction across, the neighbour, the crossroads at its start and end corners
        {dx = 0, dz = -1, nx = 0, nz = -1, i0 = 0, j0 = 0, i1 = 1, j1 = 0},
        {dx = 1, dz = 0, nx = 1, nz = 0, i0 = 1, j0 = 0, i1 = 1, j1 = 1},
        {dx = 0, dz = 1, nx = 0, nz = 1, i0 = 1, j0 = 1, i1 = 0, j1 = 1},
        {dx = -1, dz = 0, nx = -1, nz = 0, i0 = 0, j0 = 1, i1 = 0, j1 = 0},
    }
    for k, side in ipairs(sides) do
        local bx, bz = self.bx + side.nx, self.bz + side.nz
        if bx >= 0 and bx < self.blocks and bz >= 0 and bz < self.blocks then
            for _, near in ipairs({{s = zebra, i = side.i0, j = side.j0}, {s = L - zebra, i = side.i1, j = side.j1}}) do
                table.insert(list, {s = (k - 1) * L + near.s, side = k, bx = bx, bz = bz, dx = side.dx, dz = side.dz,
                                    i = self.bx + near.i, j = self.bz + near.j})
            end
        end
    end
    return list
end

function Pedestrian:start()
    self.rng = math.floor(self.seed) * 7 + 3
    self.bx, self.bz = math.floor(self.blockX), math.floor(self.blockZ)
    local _, _, L = self:ring()
    self.s = self.at * 4 * L
    self.dir = self.clockwise and -1 or 1
    self.state = "walk"
    self.skip = nil   -- (the crossing just arrived at or passed: not considered again until away from it)
    self.phase = self:random() * 6.28
    self:put(self:place(self.s))
end

function Pedestrian:put(x, z, onRoad)
    self.phase = self.phase + (self.v or 0) * 0.1
    local bob = math.abs(math.sin(self.phase * 4)) * 0.05
    self.entity.position = vec3(x, (onRoad and 0 or self.curb) + bob, z)
end

function Pedestrian:face(dx, dz)
    self.entity.rotation = vec3(0, math.deg(math.atan(-dx, -dz)), 0)
end

function Pedestrian:update(dt)
    local t = game.time()
    if self.state == "wait" then   -- at the kerb: the walk signal, early in it (time to get over)
        local c = self.crossing
        local w = walkTime(t, c.i, c.j)
        self.v = 0
        if w >= 0.5 and w < 2 then
            self.state = "cross"
            local x, z = self:place(self.s)
            self.from = {x = x, z = z}
            self.walked = 0
            self:face(c.dx, c.dz)
        end
        return
    end
    if self.state == "cross" then   -- straight over, to the other block's ring
        local c = self.crossing
        local across = self.street + 2 * self.inset
        self.v = self.speed * 1.4
        self.walked = math.min(self.walked + self.v * dt, across)
        local onRoad = self.walked > self.inset and self.walked < across - self.inset
        self:put(self.from.x + c.dx * self.walked, self.from.z + c.dz * self.walked, onRoad)
        if self.walked >= across then   -- (round the new block, the same way, from the side facing back)
            local _, _, L = self:ring()
            local along = self.s - (c.side - 1) * L
            self.bx, self.bz = c.bx, c.bz
            local opposite = (c.side + 1) % 4   -- (sides 1-4 face -z, +x, +z, -x: the other block's facing this one)
            self.s = opposite * L + (L - along)
            self.skip = self.s
            self.state = "walk"
        end
        return
    end
    -- Round the ring; at a crossing, sometimes over it (waiting for the walk signal)
    local _, _, L = self:ring()
    self.v = self.speed
    local before = self.s
    self.s = (self.s + self.dir * self.speed * dt) % (4 * L)
    if self.skip and math.abs(((self.s - self.skip + 2 * L) % (4 * L)) - 2 * L) > 1 then self.skip = nil end
    for _, c in ipairs(self:crossings()) do
        local a, b = before, self.s
        if self.dir < 0 then a, b = b, a end
        local passed = (b >= a and c.s > a and c.s <= b) or (b < a and (c.s > a or c.s <= b))
        if passed and not self.skip then
            self.skip = c.s
            if self:random() < 0.35 then
                self.s = c.s
                self.crossing = c
                self.state = "wait"
                self:face(c.dx, c.dz)
                self:put(self:place(self.s))
                return
            end
        end
    end
    local x, z = self:place(self.s)
    local nx, nz = self:place(self.s + self.dir * 0.5)
    self:face(nx - x, nz - z)
    self:put(x, z)
end

return Pedestrian
