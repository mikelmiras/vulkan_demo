-- Spray from wet tyres: its particle emitter's rate follows how fast it moves (its own world position, so it works
-- on whatever it's attached to: the taxi, a car in the traffic); none standing still.
local SprayBySpeed = {
    description = "A particle emitter's rate by how fast it moves",
    fields = {
        {"maxRate", 150, min = 0, max = 2000},   -- particles a second at full speed
        {"fullSpeed", 14, min = 0.1, max = 100}, -- m/s for the full rate
        {"from", 1.5, min = 0, max = 20},        -- m/s below which there's none
    },
}

function SprayBySpeed:update(dt)
    local p = self.entity.worldPosition
    if self.last and dt > 0 then
        local moved = vec3.distance(p, self.last) / dt
        self.speed = self.speed and self.speed + (moved - self.speed) * math.min(1, dt * 6) or moved
    end
    self.last = p
    local s = self.speed or 0
    local share = s < self.from and 0 or math.min(1, (s - self.from) / math.max(self.fullSpeed - self.from, 0.1))
    self.entity.particleRate = self.maxRate * share
end

return SprayBySpeed
