-- MenuSettings: what the title menu's language toggle (bound to game.state.spanish) changes. The volumes and
-- headphones on its Settings screen need nothing here: they're bound to the player's settings (settings.*),
-- which the engine applies and saves itself.
local MenuSettings = {
    description = "Applies the menu's language",
}

function MenuSettings:start()
    if game.state.spanish == nil then game.state.spanish = game.language() == "es" end
end

function MenuSettings:update(dt)
    local wanted = game.state.spanish and "es" or "en"
    if game.language() ~= wanted then game.setLanguage(wanted) end
end

return MenuSettings
