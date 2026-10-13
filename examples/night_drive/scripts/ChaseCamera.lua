-- An isometric-style camera that follows a target from a fixed direction (as GTA: Chinatown Wars or The Precinct
-- do): it keeps the view's angle, eases after the target, and looks a little ahead of where it's going. Buildings
-- (entities tagged "Building") between it and the target are hidden, so the target is never lost behind one.
local ChaseCamera = {
    description = "Follows the target from above at a fixed angle",
    fields = {
        {"target", "Taxi", ref = "entity"},
        {"distance", 85, min = 5, max = 300},   -- how far back along the view
        {"pitch", 66, min = 10, max = 89},      -- degrees down from the horizon
        {"yaw", 45, min = -180, max = 180},     -- the view's compass direction
        {"follow", 4, min = 0.1, max = 30},     -- how quickly it catches up (per second)
        {"lead", 0.6, min = 0, max = 3},        -- seconds of the target's speed to look ahead
        {"seeThrough", true},                   -- hide buildings in the way
    },
}

function ChaseCamera:start()
    self.focus = nil
    self.hidden = {}   -- id -> the building hidden for being in the way
end

local function isBuilding(e)
    for _, t in ipairs(e.tags or {}) do
        if t == "Building" then return true end
    end
    return false
end

-- Buildings between the camera and the target are hidden (rays to its middle and to points round it, 4.5 m out, so
-- one covering only its front or the street just ahead counts too); ones no longer in the way come back
local kAround = {vec3(0, 1, 0), vec3(4.5, 1, 0), vec3(-4.5, 1, 0), vec3(0, 1, 4.5), vec3(0, 1, -4.5),
                 vec3(3.2, 1, 3.2), vec3(-3.2, 1, 3.2), vec3(3.2, 1, -3.2), vec3(-3.2, 1, -3.2)}
function ChaseCamera:hideBlockers(from, target)
    local blocking = {}
    for _, offset in ipairs(kAround) do
        local to = vec3(target.x + offset.x, target.y + offset.y, target.z + offset.z)
        local dir = vec3(to.x - from.x, to.y - from.y, to.z - from.z)
        local length = vec3.length(dir)
        local hit = game.raycast(from, dir, length - 1.5)
        if hit and hit.entity and isBuilding(hit.entity) then blocking[hit.entity.id] = hit.entity end
    end
    for id, e in pairs(self.hidden) do
        if not blocking[id] and e.exists then e.visible = true end
    end
    for id, e in pairs(blocking) do e.visible = false end
    self.hidden = blocking
end

function ChaseCamera:update(dt)
    local target = game.find(self.target)
    if not target then return end
    local p = target.worldPosition
    local v = target.velocity or vec3(0, 0, 0)
    local want = vec3(p.x + v.x * self.lead, p.y, p.z + v.z * self.lead)
    self.focus = self.focus and vec3.lerp(self.focus, want, math.min(1, dt * self.follow)) or want
    -- Back from the focus along the view's direction
    local pitch, yaw = math.rad(self.pitch), math.rad(self.yaw)
    local back = vec3(math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch))
    local f = self.focus
    self.entity.position = vec3(f.x + back.x * self.distance, f.y + back.y * self.distance, f.z + back.z * self.distance)
    self.entity:lookAt(f)
    if self.seeThrough then self:hideBlockers(self.entity.worldPosition, p) end
end

return ChaseCamera
