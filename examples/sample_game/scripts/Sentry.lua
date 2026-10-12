-- Sentry: a block that glows when the player comes near. A Lua behaviour: this file is the "Sentry" type in the
-- inspector's Add component menu (see README.md, "Lua scripting"); edit it while the game runs and it reloads.
local Sentry = {
    description = "Glows when the player is near",
    fields = {
        {"player", "Player", ref = "entity"},
        {"alertRadius", 6, min = 0, max = 30},
        {"alertColor", vec3(1, 0.25, 0.2)},
        {"pulseSpeed", 6, min = 0, max = 30},
    },
}

function Sentry:start()
    self.calmColor = self.entity.color
    self.alert = false
end

function Sentry:update(dt)
    local player = game.find(self.player)
    local near = player ~= nil and vec3.distance(player.worldPosition, self.entity.worldPosition) < self.alertRadius
    if near and not self.alert then
        game.log(self.entity.name .. " spotted you")
    end
    self.alert = near
    if self.calmColor == nil then return end   -- (no mesh renderer)
    if near then
        local pulse = 0.75 + 0.25 * math.sin(game.time() * self.pulseSpeed)
        self.entity.color = self.alertColor * pulse
    else
        self.entity.color = self.calmColor
    end
end

return Sentry
