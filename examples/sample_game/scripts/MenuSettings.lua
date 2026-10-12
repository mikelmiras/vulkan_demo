-- MenuSettings: what the title menu's controls (UI elements bound to game state values) change. The slider sets
-- game.state.musicVolume and the toggle game.state.spanish; this applies them.
local MenuSettings = {
    description = "Applies the menu's music volume and language",
}

function MenuSettings:start()
    if game.state.musicVolume == nil then game.state.musicVolume = 0.8 end
    if game.state.spanish == nil then game.state.spanish = game.language() == "es" end
end

function MenuSettings:update(dt)
    game.setBusVolume("music", game.state.musicVolume)
    local wanted = game.state.spanish and "es" or "en"
    if game.language() ~= wanted then game.setLanguage(wanted) end
end

return MenuSettings
