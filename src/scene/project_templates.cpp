#include "scene/project_templates.h"

#include <fstream>

#include "assets/meshes.h"
#include "assets/textures.h"
#include "core/project.h"
#include "scene/behaviour.h"
#include "scene/scene.h"
#include "scene/serialization.h"

namespace fs = std::filesystem;

namespace project_templates {
namespace {

const MeshLibrary& builtins() {
    static const MeshLibrary meshes = buildMeshLibrary();
    return meshes;
}

void place(Entity& e, Vec3 pos, Vec3 size = {1, 1, 1}) {
    Transform& t = e.transform;
    t.position[0] = pos.x; t.position[1] = pos.y; t.position[2] = pos.z;
    t.scale[0] = size.x; t.scale[1] = size.y; t.scale[2] = size.z;
}

// A mesh entity with a collider that fits it
Entity& block(Scene& scene, const char* name, MeshKind mesh, Vec3 pos, Vec3 size, Vec3 color, EntityId parent = kNoEntity) {
    Entity& e = scene.create(name, parent);
    place(e, pos, size);
    MeshRenderer mr;
    mr.mesh = (int)mesh;
    mr.color = color;
    e.meshRenderer = mr;
    e.collider = fitCollider(builtins(), (int)mesh);
    return e;
}

void addBehaviour(Entity& e, const char* type, const char* fieldsJson = nullptr) {
    if (fieldsJson) e.behaviours.emplace_back(std::make_unique<MissingBehaviour>(type, fieldsJson));   // (a script)
    else if (auto b = createBehaviour(type)) e.behaviours.emplace_back(std::move(b));
}

// What every template has: a sun and a ground
void basics(Scene& scene) {
    Entity& sun = scene.create("Sun");
    Light l;
    l.type = Light::Type::Directional;
    l.color = {1.f, 0.95f, 0.85f};
    sun.light = l;
    const float unit[3] = {1, 1, 1};
    sun.transform.setMatrix(lookRotation({0, 30, 0}, normalize(Vec3{-0.45f, -1.f, -0.3f}), unit));
    Entity& ground = block(scene, "Ground", MeshKind::Plane, {0, 0, 0}, {200, 1, 200}, {0.42f, 0.55f, 0.38f});
    ground.meshRenderer->checker = true;
    ground.locked = true;
}

// A body for a player: a capsule with its feet at the origin
Collider capsule() {
    Collider body;
    body.shape = Collider::Shape::Capsule;
    body.center = {0, 0.9f, 0};
    body.height = 1.8f;
    body.radius = 0.35f;
    return body;
}

Scene emptyScene() {
    Scene scene;
    basics(scene);
    Entity& cam = scene.create("Camera");
    cam.camera = Camera{};
    const float unit[3] = {1, 1, 1};
    cam.transform.setMatrix(lookRotation({0, 3, 8}, normalize(Vec3{0, -0.3f, -1.f}), unit));
    return scene;
}

Scene firstPersonScene() {
    Scene scene;
    basics(scene);
    Entity& player = scene.create("Player");
    place(player, {0, 0.05f, 6});
    player.collider = capsule();
    addBehaviour(player, "PlayerController");
    EntityId playerId = player.id;
    Entity& eyes = scene.create("Player Camera", playerId);
    eyes.camera = Camera{};
    eyes.transform.position[1] = 1.65f;
    // Things to walk around and climb
    block(scene, "Block", MeshKind::Cube, {-4, 0.5f, -2}, {2, 1, 2}, {0.8f, 0.45f, 0.3f});
    block(scene, "Tall Block", MeshKind::Cube, {4, 1.5f, -4}, {2, 3, 2}, {0.35f, 0.5f, 0.8f});
    Entity& ramp = block(scene, "Ramp", MeshKind::Cube, {0, 0.6f, -8}, {3, 0.2f, 6}, {0.75f, 0.75f, 0.7f});
    ramp.transform.rotation[0] = 20.f;
    block(scene, "Ball", MeshKind::Sphere, {-2, 0.5f, 2}, {1, 1, 1}, {0.9f, 0.85f, 0.2f});
    return scene;
}

Scene topDownScene() {
    Scene scene;
    basics(scene);
    Entity& player = scene.create("Player");
    place(player, {0, 0.05f, 0});
    player.collider = capsule();
    addBehaviour(player, "TopDownPlayer", "{\"camera\": \"Camera\", \"body\": \"Player Body\"}");
    EntityId playerId = player.id;
    Entity& body = block(scene, "Player Body", MeshKind::Sphere, {0, 0.9f, 0}, {0.7f, 1.8f, 0.7f}, {0.2f, 0.6f, 0.9f},
                         playerId);
    body.collider.reset();   // (the player's capsule is the body)
    EntityId bodyId = body.id;
    Entity& nose = block(scene, "Player Nose", MeshKind::Cube, {0, 0.25f, -0.5f}, {0.3f, 0.12f, 0.4f},
                         {0.95f, 0.95f, 0.95f}, bodyId);   // (which way it faces)
    nose.collider.reset();
    Entity& cam = scene.create("Camera");
    cam.camera = Camera{};
    const float unit[3] = {1, 1, 1};
    cam.transform.setMatrix(lookRotation({0, 14, 9}, normalize(Vec3{0, -14, -9}), unit));
    // Walls to walk around, gems to collect
    block(scene, "Wall", MeshKind::Cube, {-5, 1, -3}, {1, 2, 8}, {0.7f, 0.65f, 0.6f});
    block(scene, "Wall", MeshKind::Cube, {5, 1, 2}, {6, 2, 1}, {0.7f, 0.65f, 0.6f});
    const Vec3 gems[] = {{-8, 0, -6}, {8, 0, -5}, {3, 0, 7}, {-3, 0, 8}, {0, 0, -10}};
    int n = 0;
    for (Vec3 at : gems) {
        std::string name = "Gem " + std::to_string(++n);
        Entity& gem = block(scene, name.c_str(), MeshKind::Pyramid, at + Vec3{0, 0.6f, 0}, {0.6f, 0.8f, 0.6f},
                            {0.95f, 0.3f, 0.6f});
        gem.collider->trigger = true;
        gem.meshRenderer->emissive = {0.6f, 0.15f, 0.3f};
        addBehaviour(gem, "Pickup");
        addBehaviour(gem, "Spin");
    }
    Entity& hud = scene.create("Score");
    addBehaviour(hud, "HudText");
    return scene;
}

// The top-down template's player: moves with the move actions (WASD, arrows, a stick), turns its body to face where
// it goes, and keeps the camera at its offset above and behind
const char* kTopDownScript = R"(-- TopDownPlayer: the top-down template's character. Moves on the ground with the move actions (WASD, the arrows,
-- the left stick), turns its body to where it goes, falls with gravity, and keeps the camera where it was from it.
local TopDownPlayer = {
    description = "Walks on the ground seen from above; the camera follows",
    fields = {
        {"camera", "Camera", ref = "entity"},
        {"body", "Player Body", ref = "entity"},
        {"speed", 6, min = 0, max = 50},
        {"turnSpeed", 12, min = 0, max = 50},
        {"gravity", 18, min = 0, max = 100},
    },
}

function TopDownPlayer:start()
    self.fall = 0
    self.yaw = 0
    local camera = game.find(self.camera)
    if camera then self.offset = camera.worldPosition - self.entity.worldPosition end
end

function TopDownPlayer:update(dt)
    local x = input.axis("move_left", "move_right")
    local z = input.axis("move_forward", "move_back")
    local move = vec3(x, 0, z)
    if move:length() > 1 then move = move:normalized() end
    self.fall = self.fall - self.gravity * dt
    local m = self.entity:moveCharacter(move * (self.speed * dt) + vec3(0, self.fall * dt, 0))
    if m.grounded then self.fall = 0 end

    local body = game.find(self.body)
    if body and vec3.length(move) > 0.01 then   -- turn towards where it goes (the shortest way round)
        local want = math.deg(math.atan(-move.x, -move.z))
        local d = (want - self.yaw + 180) % 360 - 180
        self.yaw = self.yaw + d * math.min(1, self.turnSpeed * dt)
        body.rotation = vec3(0, self.yaw, 0)
    end

    local camera = game.find(self.camera)
    if camera and self.offset then camera.position = self.entity.worldPosition + self.offset end
end

return TopDownPlayer
)";

bool writeText(const fs::path& file, const std::string& text, std::string& error) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary);
    f << text;
    if (!f) { error = "can't write " + file.u8string(); return false; }
    return true;
}

}  // namespace

const char* name(Template t) {
    switch (t) {
        case Template::Empty: return "Empty";
        case Template::FirstPerson: return "First person";
        case Template::TopDown: return "Top-down";
        default: return "?";
    }
}

const char* description(Template t) {
    switch (t) {
        case Template::Empty: return "A sun, a ground and a camera";
        case Template::FirstPerson: return "Walk and jump around a few blocks (WASD, the mouse, Space)";
        case Template::TopDown: return "A character seen from above, collecting gems (a Lua script to start from)";
        default: return "";
    }
}

bool create(const fs::path& dir, Template t, const std::string& projectName, std::string& error) {
    if (project::exists(dir)) { error = dir.u8string() + " already has a project"; return false; }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { error = "can't make " + dir.u8string() + ": " + ec.message(); return false; }
    project::Settings settings;
    settings.name = projectName.empty() ? dir.filename().u8string() : projectName;
    settings.startScene = "scenes/main.vkw";
    Scene scene = t == Template::FirstPerson ? firstPersonScene() : t == Template::TopDown ? topDownScene() : emptyScene();
    if (t == Template::TopDown && !writeText(dir / "scripts" / "TopDownPlayer.lua", kTopDownScript, error)) return false;
    fs::create_directories(dir / "scenes", ec);
    static const TextureLibrary textures = buildTextureLibrary();
    if (!saveScene((dir / "scenes" / "main.vkw").u8string(), scene, builtins(), textures, error)) return false;
    return project::save(dir, settings, error);
}

}  // namespace project_templates
