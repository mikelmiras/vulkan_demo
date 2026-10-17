-- A neon sign on the blink: steady for a few seconds, then a burst of quick flickers off and on (its tube and its
-- light together: the sign's group shown and hidden), as a failing transformer does.
local NeonFlicker = {
    description = "Flickers a neon sign (its group) now and then",
    fields = {
        {"seed", 1, min = 0, max = 100000},
        {"steady", 4, min = 0.5, max = 60},   -- seconds lit between bursts, about
    },
}

function NeonFlicker:random()
    self.rng = (self.rng * 1103515245 + 12345) % 2147483648
    return self.rng / 2147483648
end

function NeonFlicker:start()
    self.rng = math.floor(self.seed) * 13 + 5
    self.on = true
    self.flips = 0
    self.wait = self.steady * (0.5 + self:random())
end

function NeonFlicker:update(dt)
    self.wait = self.wait - dt
    if self.wait > 0 then return end
    if self.flips > 0 then   -- in a burst: off and on quickly
        self.on = not self.on
        self.flips = self.flips - 1
        self.wait = 0.03 + self:random() * (self.on and 0.12 or 0.06)
    else                     -- a burst starts (or it settles: lit, steady again)
        if self.on then
            self.flips = 2 * (1 + math.floor(self:random() * 4)) - 1   -- (odd: it ends lit)
            self.on = false
            self.wait = 0.05
        else
            self.on = true
            self.wait = self.steady * (0.5 + self:random())
        end
    end
    self.entity.visible = self.on
end

return NeonFlicker
