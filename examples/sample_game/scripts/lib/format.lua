-- Helpers shared by the scripts: require("lib.format"). Files in scripts/lib/ aren't behaviours.
local format = {}

-- 75.3 -> "1:15.3"
function format.clock(seconds)
    local m = math.floor(seconds / 60)
    return string.format("%d:%04.1f", m, seconds - m * 60)
end

return format
