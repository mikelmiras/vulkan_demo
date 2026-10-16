-- A traffic light: one way through a crossroads (traffic along x or along z), its red, amber and green lamps
-- (children named "Signal <x>,<z> <axis> <colour>", each with its glow) shown in turn. The cycle is worked out from
-- the clock, the same way the traffic works it out (TrafficCar.lua's signalState), so no state is shared:
-- 20 seconds, along z green 0-7, amber 7-9; along x green 10-17, amber 17-19; red otherwise (all red a second
-- between); each crossroads offset from its neighbours.
local TrafficLight = {
    description = "Shows a crossroads' red, amber and green in turn",
    fields = {
        {"crossX", 0, min = 0, max = 64},   -- the crossroads (its street along x, its street along z)
        {"crossZ", 0, min = 0, max = 64},
        {"axis", "x"},                      -- the traffic it's for: going along "x" or along "z"
    },
}

-- 0 green, 1 amber, 2 red
local function signalState(t, i, j, axis)
    local c = (t + (i + j) * 2.5) % 20
    if axis == "z" then
        if c < 7 then return 0 elseif c < 9 then return 1 end
    else
        if c >= 10 and c < 17 then return 0 elseif c >= 17 and c < 19 then return 1 end
    end
    return 2
end

function TrafficLight:start()
    local base = "Signal " .. math.floor(self.crossX) .. "," .. math.floor(self.crossZ) .. " " .. self.axis .. " "
    self.lamps = {game.find(base .. "green"), game.find(base .. "amber"), game.find(base .. "red")}
    self.shown = -1
end

function TrafficLight:update(dt)
    local s = signalState(game.time(), self.crossX, self.crossZ, self.axis)
    if s == self.shown then return end
    for k, lamp in ipairs(self.lamps) do
        if lamp then lamp.visible = (k - 1) == s end
    end
    self.shown = s
end

return TrafficLight
