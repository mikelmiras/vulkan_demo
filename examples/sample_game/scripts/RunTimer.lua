-- RunTimer: how long this run has taken, in the top-right corner (a Lua behaviour drawing game UI)
local format = require("lib.format")

local RunTimer = {
    description = "Shows the time since the game started",
    fields = {
        {"size", 40, min = 10, max = 120},
        {"textColor", vec3(1, 1, 1)},
    },
}

function RunTimer:start()
    game.state.runStarted = game.state.runStarted or game.time()
end

function RunTimer:drawUI(ui)
    local elapsed = game.time() - (game.state.runStarted or 0)
    local c = self.textColor
    ui.text(ui.width() - 40, 130, format.clock(elapsed), self.size, color(c.x, c.y, c.z, 0.9), "right")
end

return RunTimer
