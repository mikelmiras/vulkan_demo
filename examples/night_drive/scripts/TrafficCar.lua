-- A car in the traffic: drives the street grid in the right-hand lane, choosing at each crossroads to go on or turn
-- (never out of the city), slowing to a stop behind whatever's ahead (another car, the taxi) and pulling away again,
-- and stopping at the line on red (and on amber, unless too close to stop: TrafficLight.lua's cycle; the line before
-- the zebra crossing, which pedestrians walk while every way is red).
-- Its collider moves with it (the physics makes it kinematic: the taxi bumps into it, it isn't pushed).
local TrafficCar = {
    description = "Drives the street grid, turning at crossroads, stopping behind what's ahead",
    fields = {
        {"speed", 9, min = 0, max = 30},          -- cruising speed (m/s)
        {"lane", 3.6, min = 0, max = 8},          -- from the street's middle to the lane's (m)
        {"first", -70, min = -1000, max = 1000},  -- the first street's middle, along x and z (the grid's)
        {"pitch", 60, min = 1, max = 500},        -- from one street to the next
        {"streets", 4, min = 2, max = 32},        -- streets each way
        {"seed", 1, min = 0, max = 100000},       -- its turns (the same drive every time)
        {"street", 16, min = 1, max = 100},       -- a street's width (the stop line: before the crossroads)
    },
}

-- The crossroads' signal for a way through it (TrafficLight.lua's, the same sums): 0 green, 1 amber, 2 red
local function signalState(t, i, j, axis)
    local c = (t + (i + j) * 2.5) % 36
    if axis == "z" then
        if c < 7 then return 0 elseif c < 10 then return 1 end
    else
        if c >= 11 and c < 18 then return 0 elseif c >= 18 and c < 21 then return 1 end
    end
    return 2
end

local function right(d) return {x = -d.z, z = d.x} end   -- (right-hand side, y up, looking along d)

function TrafficCar:random()   -- (its own: a car's drive doesn't depend on the others')
    self.rng = (self.rng * 1103515245 + 12345) % 2147483648
    return self.rng / 2147483648
end

-- The street index of a coordinate on a street's middle (nearest)
function TrafficCar:streetAt(c)
    return math.floor((c - self.first) / self.pitch + 0.5)
end

-- The next crossroads ahead, and what to do there: on, left or right (where a street carries on that way)
function TrafficCar:plan()
    local d, p = self.dir, self.entity.position
    local along = d.x ~= 0 and p.x or p.z
    local sign = d.x ~= 0 and d.x or d.z
    -- (the first street line ahead, half a metre on at least)
    local k = sign > 0 and math.floor((along - self.first) / self.pitch) + 1 or math.ceil((along - self.first) / self.pitch) - 1
    if (self.first + k * self.pitch - along) * sign < 0.5 then k = k + sign end
    local on = self.dir.x ~= 0 and self:streetAt(p.z) or self:streetAt(p.x)   -- (the street it's on, across)
    local choices = {}
    if k + sign >= 0 and k + sign < self.streets then table.insert(choices, d) end            -- on
    for _, turn in ipairs({{x = -d.z, z = d.x}, {x = d.z, z = -d.x}}) do                       -- right, left
        local s = turn.x ~= 0 and turn.x or turn.z
        if on + s >= 0 and on + s < self.streets then table.insert(choices, turn) end
    end
    if k < 0 or k >= self.streets then choices = {} end
    if #choices == 0 then   -- (the city's edge ahead: back the way it came)
        self.next = nil
        self.dir = {x = -d.x, z = -d.z}
        local r = right(self.dir)
        local street = self.first + on * self.pitch
        if self.dir.x ~= 0 then p.z = street + r.z * self.lane else p.x = street + r.x * self.lane end
        self.entity.position = p
        return self:plan()
    end
    local g = choices[math.floor(self:random() * #choices) + 1]
    local cross = self.first + k * self.pitch
    -- (the crossroads' signal: its street indices along x and z, the way this car goes through)
    self.signal = d.x ~= 0 and {i = k, j = on, axis = "x", cross = cross} or {i = on, j = k, axis = "z", cross = cross}
    -- Where its lane meets the new one (turning: the corner of the two lanes; straight on: no turn)
    local rg = right(g)
    local turnAt = cross + (d.x ~= 0 and rg.x or rg.z) * self.lane
    self.next = {dir = g, at = turnAt, straight = (g.x == d.x and g.z == d.z)}
end

function TrafficCar:start()
    self.rng = math.floor(self.seed) + 7
    local f = self.entity.forward
    if math.abs(f.x) > math.abs(f.z) then self.dir = {x = f.x > 0 and 1 or -1, z = 0}
    else self.dir = {x = 0, z = f.z > 0 and 1 or -1} end
    self.v = self.speed * 0.5
    self.yaw = self.entity.rotation.y
    self:plan()
end

function TrafficCar:update(dt)
    local e = self.entity
    local d = self.dir
    -- Something ahead within 7 m (rays from the bumper, the lane's middle and its sides): ease to a stop
    local p = e.worldPosition
    local blocked = false
    for _, side in ipairs({0, -0.7, 0.7}) do
        local r = right(d)
        local from = vec3(p.x + d.x * 2.4 + r.x * side, p.y, p.z + d.z * 2.4 + r.z * side)
        local hit = game.raycast(from, vec3(d.x, 0, d.z), 7, e)
        if hit and hit.entity and hit.entity.id ~= e.id then blocked = true end
    end
    local want = blocked and 0 or self.speed
    -- The signal ahead: red (or amber, while there's room to stop), slow to a stop at the line before the crossroads
    local sig = self.signal
    if sig and not blocked then
        local pos0 = e.position
        local along0 = d.x ~= 0 and pos0.x or pos0.z
        local sign0 = d.x ~= 0 and d.x or d.z
        -- (the front bumper to the line: before the zebra, a sixth of a block on from the crossroads, 3.7 m wide)
        local zebra = (self.pitch - self.street) / 6
        local toLine = (sig.cross - sign0 * (self.street / 2 + zebra + 1.85 + 1.0 + 2.6) - along0) * sign0
        local state = signalState(game.time(), sig.i, sig.j, sig.axis)
        -- (once it's decided to stop it stops, a little over the line if need be, until green)
        if state == 0 then self.stopping = false
        elseif toLine > -0.5 and toLine < 30 and (state == 2 or toLine > 3) then self.stopping = true end
        if self.stopping then want = math.min(want, math.max(toLine, 0) * 0.8) end
        -- (past the line already on red, say starting out there: at the crossroads' edge at least)
        local toEdge = (sig.cross - sign0 * (self.street / 2 + 2.6) - along0) * sign0
        if state == 2 and toLine <= -0.5 and toEdge > -0.5 then want = math.min(want, math.max(toEdge, 0) * 0.8) end
    end
    local rate = want < self.v and 12 or 3.5   -- (brakes harder than it pulls away)
    self.v = self.v + math.max(-rate * dt, math.min(rate * dt, want - self.v))
    -- Along its lane; at the turn point onto the new one
    local step = self.v * dt
    local pos = e.position
    local along = d.x ~= 0 and pos.x or pos.z
    local sign = d.x ~= 0 and d.x or d.z
    local n = self.next
    if n and (n.at - along) * sign <= step then
        local left = step - (n.at - along) * sign
        if d.x ~= 0 then pos.x = n.at else pos.z = n.at end
        self.dir = n.dir
        pos.x = pos.x + self.dir.x * left
        pos.z = pos.z + self.dir.z * left
        e.position = pos
        self.stopping = false   -- (a new crossroads ahead)
        self:plan()
    else
        pos.x = pos.x + d.x * step
        pos.z = pos.z + d.z * step
        e.position = pos
    end
    -- Turning the body towards where it's going (quickly: it looks like steering through the corner)
    local target = math.deg(math.atan(-self.dir.x, -self.dir.z))
    local diff = (target - self.yaw + 540) % 360 - 180
    self.yaw = self.yaw + diff * math.min(1, dt * 7)
    e.rotation = vec3(0, self.yaw, 0)
end

return TrafficCar
