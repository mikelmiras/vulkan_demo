-- A traffic light: one way through a crossroads (traffic along x or along z), its red, amber and green lamps
-- (children named "Signal <x>,<z> <axis> <colour>", each with its glow) shown in turn; or a walk signal at one end
-- of a crossing (axis "walk": its lamps "Signal <x>,<z> walk<head> green" and "... red", the walking figure and the
-- hand). The cycle is worked out from the clock, the same way the traffic and the pedestrians work it out
-- (TrafficCar.lua's signalState, Pedestrian.lua's walkTime), so no state is shared: 36 seconds, along z green 0-7,
-- amber 7-10; along x green 11-18, amber 18-21; red otherwise (all red a second between); from 22 the walk phase,
-- every way held on red while the crossings are walked (walk shown 22-32, then the hand: no one starts out late);
-- each crossroads offset from its neighbours.
local TrafficLight = {
    description = "Shows a crossroads' red, amber and green in turn",
    fields = {
        {"crossX", 0, min = 0, max = 64},   -- the crossroads (its street along x, its street along z)
        {"crossZ", 0, min = 0, max = 64},
        {"axis", "x"},                      -- the traffic it's for: going along "x" or along "z"; or "walk"
        {"head", 0, min = 0, max = 7},      -- (walk signals: which of the crossroads' eight)
    },
}

-- 0 green, 1 amber, 2 red
local function signalState(t, i, j, axis)
    local c = (t + (i + j) * 2.5) % 36
    if axis == "walk" then return (c >= 22 and c < 32) and 0 or 2 end
    if axis == "z" then
        if c < 7 then return 0 elseif c < 10 then return 1 end
    else
        if c >= 11 and c < 18 then return 0 elseif c >= 18 and c < 21 then return 1 end
    end
    return 2
end

function TrafficLight:start()
    local way = self.axis == "walk" and ("walk" .. math.floor(self.head)) or self.axis
    local base = "Signal " .. math.floor(self.crossX) .. "," .. math.floor(self.crossZ) .. " " .. way .. " "
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
