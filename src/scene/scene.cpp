#include "scene/scene.h"

#include "assets/gltf.h"

#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <functional>
#include <set>
#include <unordered_map>

#include "imgui.h"
#include "ImGuizmo.h"

// ---------------------------------------------------------------------------------------------
// Transform / components
// ---------------------------------------------------------------------------------------------
Mat4 Transform::matrix() const {
    Mat4 m;
    ImGuizmo::RecomposeMatrixFromComponents(position, rotation, scale, m.m);
    return m;
}

void Transform::setMatrix(const Mat4& m) {
    ImGuizmo::DecomposeMatrixToComponents(m.m, position, rotation, scale);
}

const char* lightTypeName(Light::Type type) { return type == Light::Type::Directional ? "directional" : "point"; }

bool importGltfPrefab(const std::string& path, MeshLibrary& meshes, TextureLibrary& textures, Prefab& out,
                      std::string& error, std::string& warning) {
    GltfModel model;
    std::string message;
    if (!importGltf(path, meshes, textures, model, message)) { error = message; return false; }
    warning = message;
    Prefab p;
    p.name = std::filesystem::path(path).stem().u8string();
    EntityId nextId = 1;
    Entity root;
    root.id = nextId++;
    root.name = p.name;
    p.entities.push_back(root);
    std::vector<EntityId> nodeIds;
    for (const GltfModel::Node& n : model.nodes) {
        Entity e;
        e.id = nextId++;
        e.name = n.name;
        e.parent = n.parent >= 0 ? nodeIds[(size_t)n.parent] : root.id;
        e.transform.setMatrix(n.local);
        nodeIds.push_back(e.id);
        // The first primitive goes on the node, any others on children of it
        for (size_t k = 0; k < n.meshes.size(); ++k) {
            MeshRenderer mr;
            mr.mesh = n.meshes[k];
            mr.color = {1, 1, 1};
            if (int m = n.materials[k]; m >= 0) {
                mr.color = model.materials[(size_t)m].color;
                mr.texture = model.materials[(size_t)m].texture;
            }
            if (k == 0) {
                e.meshRenderer = mr;
                continue;
            }
            if (k == 1) p.entities.push_back(e);   // the node first, so its children come after it
            Entity part;
            part.id = nextId++;
            part.name = n.name + " part " + std::to_string(k);
            part.parent = e.id;
            part.meshRenderer = mr;
            p.entities.push_back(part);
        }
        if (n.meshes.size() < 2) p.entities.push_back(e);
    }
    out = std::move(p);
    return true;
}

const char* colliderShapeName(Collider::Shape shape) {
    static const char* names[] = {"box", "sphere", "capsule", "mesh"};
    return names[(int)shape];
}

Collider fitCollider(const MeshLibrary& meshes, int mesh) {
    Collider c;
    const Mesh& m = meshes.get(mesh);
    Vec3 size = m.boundsMax - m.boundsMin;
    c.center = (m.boundsMin + m.boundsMax) * 0.5f;
    c.size = size;
    if (mesh == (int)MeshKind::Sphere) {
        c.shape = Collider::Shape::Sphere;
        c.radius = 0.5f;
    } else if (mesh == (int)MeshKind::Plane) {
        c.center = {0, -0.5f, 0};   // 1 unit thick (before scaling), top face at the plane
        c.size = {1, 1, 1};
    } else if (mesh != (int)MeshKind::Cube) {
        c.shape = Collider::Shape::Mesh;
    }
    return c;
}

// ---------------------------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------------------------
Entity& Scene::create(const std::string& name, EntityId parent) {
    Entity e;
    e.name = name;
    e.parent = parent;
    e.id = nextId_++;
    entities.push_back(std::move(e));
    return entities.back();
}

Entity& Scene::add(Entity e) {
    if (e.id == kNoEntity || find(e.id)) e.id = nextId_;
    nextId_ = std::max(nextId_, e.id + 1);
    entities.push_back(std::move(e));
    return entities.back();
}

int Scene::indexOf(EntityId id) const {
    if (id == kNoEntity) return -1;
    for (size_t i = 0; i < entities.size(); ++i)
        if (entities[i].id == id) return (int)i;
    return -1;
}

Entity* Scene::find(EntityId id) {
    int i = indexOf(id);
    return i >= 0 ? &entities[(size_t)i] : nullptr;
}

const Entity* Scene::find(EntityId id) const {
    int i = indexOf(id);
    return i >= 0 ? &entities[(size_t)i] : nullptr;
}

Entity* Scene::findByName(const std::string& name) {
    return const_cast<Entity*>(static_cast<const Scene*>(this)->findByName(name));
}

const Entity* Scene::findByName(const std::string& name) const {
    for (const Entity& e : entities)
        if (e.name == name) return &e;
    return nullptr;
}

std::vector<Mat4> Scene::worldMatrices() const {
    std::unordered_map<EntityId, size_t> index;
    index.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) index[entities[i].id] = i;

    std::vector<Mat4> world(entities.size());
    std::vector<char> done(entities.size(), 0);
    std::function<const Mat4&(size_t, int)> compute = [&](size_t i, int depth) -> const Mat4& {
        if (done[i]) return world[i];
        const Entity& e = entities[i];
        Mat4 local = e.transform.matrix();
        auto p = index.find(e.parent);
        // depth guard: a corrupted file with a parent cycle degrades to "root" instead of recursing forever
        world[i] = p != index.end() && depth < 256 ? compute(p->second, depth + 1) * local : local;
        done[i] = 1;
        return world[i];
    };
    for (size_t i = 0; i < entities.size(); ++i) compute(i, 0);
    return world;
}

Mat4 Scene::worldMatrix(EntityId id) const {
    Mat4 m;
    int depth = 0;
    for (const Entity* e = find(id); e && depth < 256; e = find(e->parent), ++depth) m = e->transform.matrix() * m;
    return m;
}

Mat4 Scene::parentWorldMatrix(EntityId id) const {
    const Entity* e = find(id);
    return e ? worldMatrix(e->parent) : Mat4{};
}

bool Scene::isAncestor(EntityId ancestor, EntityId id) const {
    int depth = 0;
    for (const Entity* e = find(id); e && depth < 256; ++depth) {
        if (e->parent == ancestor && ancestor != kNoEntity) return true;
        e = find(e->parent);
    }
    return false;
}

bool Scene::visibleInHierarchy(EntityId id) const {
    int depth = 0;
    for (const Entity* e = find(id); e && depth < 256; e = find(e->parent), ++depth)
        if (!e->visible) return false;
    return true;
}

std::vector<EntityId> Scene::childrenOf(EntityId parent) const {
    std::vector<EntityId> out;
    for (const Entity& e : entities) {
        // entities whose parent no longer exists count as roots
        bool isRoot = e.parent == kNoEntity || !find(e.parent);
        if (parent == kNoEntity ? isRoot : e.parent == parent) out.push_back(e.id);
    }
    return out;
}

std::vector<EntityId> Scene::subtree(EntityId root) const {
    std::vector<EntityId> out;
    if (!find(root)) return out;
    out.push_back(root);
    for (size_t i = 0; i < out.size(); ++i)   // breadth-first is enough: order only matters for "root first"
        for (const Entity& e : entities)
            if (e.parent == out[i]) out.push_back(e.id);
    return out;
}

bool Scene::setParent(EntityId id, EntityId newParent, bool keepWorld) {
    Entity* e = find(id);
    if (!e || id == newParent || (newParent != kNoEntity && !find(newParent)) || isAncestor(id, newParent))
        return false;
    if (keepWorld) {
        Mat4 world = worldMatrix(id);
        Mat4 local = inverseAffine(worldMatrix(newParent)) * world;
        e->transform.setMatrix(local);
    }
    e->parent = newParent;
    return true;
}

void Scene::remove(EntityId id) {
    std::vector<EntityId> doomed = subtree(id);
    std::set<EntityId> set(doomed.begin(), doomed.end());
    entities.erase(std::remove_if(entities.begin(), entities.end(), [&](const Entity& e) { return set.count(e.id) > 0; }),
                   entities.end());
}

EntityId Scene::duplicate(EntityId id) {
    std::vector<EntityId> ids = subtree(id);
    if (ids.empty()) return kNoEntity;
    std::unordered_map<EntityId, EntityId> remap;
    for (EntityId old : ids) remap[old] = nextId_++;
    std::vector<Entity> copies;
    for (EntityId old : ids) {
        Entity c = *find(old);
        c.id = remap[old];
        if (old != id) c.parent = remap[c.parent];   // the copied root keeps the original's parent
        copies.push_back(std::move(c));
    }
    copies[0].name = uniqueName(copies[0].name);
    // Insert right after the original subtree's last entity, so the copy shows up next to it
    int insertAt = 0;
    for (EntityId e : ids) insertAt = std::max(insertAt, indexOf(e) + 1);
    entities.insert(entities.begin() + insertAt, copies.begin(), copies.end());
    return remap[id];
}

std::string Scene::uniqueName(const std::string& wanted) const {
    std::set<std::string> names;
    for (const auto& e : entities) names.insert(e.name);
    if (!names.count(wanted)) return wanted;
    // Strip a trailing ".NNN" and find the next free number, Blender style
    std::string base = wanted;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < base.size() &&
        base.find_first_not_of("0123456789", dot + 1) == std::string::npos)
        base = base.substr(0, dot);
    for (int n = 1;; ++n) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), ".%03d", n);
        if (!names.count(base + buf)) return base + buf;
    }
}

std::vector<BehaviourPtr> motionBehaviours(float spin, float bob, float phase) {
    std::vector<BehaviourPtr> out;
    if (spin != 0.f)
        if (auto b = createBehaviour("Spin")) {
            setField(*b, "speed", spin / DEG2RAD);
            out.emplace_back(std::move(b));
        }
    if (bob != 0.f)
        if (auto b = createBehaviour("Bob")) {
            setField(*b, "height", bob);
            setField(*b, "phase", phase / DEG2RAD);
            out.emplace_back(std::move(b));
        }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Prefabs
// ---------------------------------------------------------------------------------------------
Prefab& Scene::addPrefab(Prefab p) {
    if (p.id == 0 || findPrefab(p.id)) p.id = nextPrefabId_;
    nextPrefabId_ = std::max(nextPrefabId_, p.id + 1);
    prefabs.push_back(std::move(p));
    return prefabs.back();
}

Prefab* Scene::findPrefab(uint32_t id) {
    return const_cast<Prefab*>(static_cast<const Scene*>(this)->findPrefab(id));
}

const Prefab* Scene::findPrefab(uint32_t id) const {
    for (const Prefab& p : prefabs)
        if (p.id == id) return &p;
    return nullptr;
}

Prefab* Scene::findPrefab(const std::string& name) {
    return const_cast<Prefab*>(static_cast<const Scene*>(this)->findPrefab(name));
}

const Prefab* Scene::findPrefab(const std::string& name) const {
    for (const Prefab& p : prefabs)
        if (p.name == name) return &p;
    return nullptr;
}

Prefab Scene::makePrefab(EntityId root, const std::string& name) const {
    Prefab p;
    p.name = name;
    for (EntityId id : subtree(root)) {
        p.entities.push_back(*find(id));
        p.entities.back().prefab.reset();
    }
    if (!p.entities.empty()) {
        Entity& r = p.entities[0];
        r.parent = kNoEntity;
        r.transform.position[0] = r.transform.position[1] = r.transform.position[2] = 0.f;
    }
    return p;
}

std::vector<Entity> Scene::instantiate(const Prefab& prefab, EntityId parent) {
    std::unordered_map<EntityId, EntityId> remap;
    for (const Entity& e : prefab.entities) remap[e.id] = nextId_++;
    std::vector<Entity> out;
    for (size_t i = 0; i < prefab.entities.size(); ++i) {
        Entity e = prefab.entities[i];
        e.id = remap[e.id];
        e.parent = i == 0 ? parent : remap.count(e.parent) ? remap[e.parent] : kNoEntity;
        e.prefab = PrefabLink{prefab.id, prefab.entities[i].id};
        out.push_back(std::move(e));
    }
    if (!out.empty()) out[0].name = uniqueName(prefab.name);
    return out;
}

std::string Scene::uniquePrefabName(const std::string& wanted) const {
    std::string base = wanted.empty() ? "Prefab" : wanted, name = base;
    for (int n = 2; findPrefab(name); ++n) name = base + " " + std::to_string(n);
    return name;
}

// ---------------------------------------------------------------------------------------------
// Default scene
// ---------------------------------------------------------------------------------------------
namespace {

struct Rng {
    uint32_t s = 0x12345678u;
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFF) / float(0x1000000); }
    float range(float a, float b) { return a + (b - a) * next(); }
};

Vec3 hsv(float h, float s, float v) {
    auto f = [&](float n) {
        float k = std::fmod(n + h * 6.f, 6.f);
        return v - v * s * std::max(0.f, std::min({k, 4.f - k, 1.f}));
    };
    return {f(5), f(3), f(1)};
}

// A mesh entity with a collider that fits it
Entity& make(Scene& scene, std::string name, EntityId parent, MeshKind mesh, Vec3 pos, Vec3 size, Vec3 color) {
    static const MeshLibrary builtins = buildMeshLibrary();
    Entity& e = scene.create(name, parent);
    Transform& t = e.transform;
    t.position[0] = pos.x; t.position[1] = pos.y; t.position[2] = pos.z;
    t.scale[0] = size.x; t.scale[1] = size.y; t.scale[2] = size.z;
    MeshRenderer mr;
    mr.mesh = (int)mesh;
    mr.color = color;
    e.meshRenderer = mr;
    e.collider = fitCollider(builtins, (int)mesh);
    return e;
}

Entity& group(Scene& scene, const char* name, Vec3 pos = {0, 0, 0}) {
    Entity& e = scene.create(name);
    e.transform.position[0] = pos.x; e.transform.position[1] = pos.y; e.transform.position[2] = pos.z;
    return e;
}

// Spin (rad/s) and/or bob (amplitude) behaviours
void animate(Entity& e, float spin, float bob, float phase = 0.f) {
    for (BehaviourPtr& b : motionBehaviours(spin, bob, phase)) e.behaviours.push_back(std::move(b));
}

void addBehaviour(Entity& e, const char* type) {
    if (auto b = createBehaviour(type)) e.behaviours.emplace_back(std::move(b));
}

std::string numbered(const char* base, int n) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s.%03d", base, n);
    return buf;
}

}  // namespace

Scene buildDefaultScene() {
    Scene scene;
    Rng rng;

    // The player: press Play and walk around (WASD, hold the right mouse button to look). The body is a capsule
    // with its feet at the entity's origin; the camera is a child at eye height.
    Entity& player = group(scene, "Player", {0, 0.05f, 34});
    Collider body;
    body.shape = Collider::Shape::Capsule;
    body.center = {0, 0.9f, 0};
    body.height = 1.8f;
    body.radius = 0.35f;
    player.collider = body;
    addBehaviour(player, "PlayerController");
    EntityId playerId = player.id;
    Entity& eyes = scene.create("Player Camera", playerId);   // invalidates `player`: use the id from here on
    eyes.camera = Camera{};
    eyes.transform.position[1] = 1.65f;
    addBehaviour(eyes, "Shooter");   // left mouse fires the Bullet prefab

    // Lighting: a sun (casts the shadows) and a warm light in the centrepiece
    {
        Entity& sun = group(scene, "Sun", {0, 40, 0});
        Light l;
        l.type = Light::Type::Directional;
        l.color = {1.f, 0.95f, 0.85f};
        sun.light = l;
        const float unit[3] = {1, 1, 1};
        sun.transform.setMatrix(lookRotation({0, 40, 0}, normalize(Vec3{-0.45f, -1.f, -0.3f}), unit));
    }

    Entity& ground = make(scene, "Ground", kNoEntity, MeshKind::Plane, {0, 0, 0}, {600, 1, 600}, {0.42f, 0.55f, 0.38f});
    ground.meshRenderer->checker = true;
    ground.locked = true;

    // Centrepiece: golden sphere inside a spinning tilted torus
    EntityId centre = group(scene, "Centrepiece", {0, 5, 0}).id;
    animate(make(scene, "Centre Sphere", centre, MeshKind::Sphere, {0, 0, 0}, {3, 3, 3}, {0.95f, 0.75f, 0.25f}), 0.f, 0.4f);
    Entity& torus = make(scene, "Centre Torus", centre, MeshKind::Torus, {0, 0, 0}, {7, 7, 7}, {0.85f, 0.2f, 0.25f});
    torus.transform.rotation[0] = 0.6f / DEG2RAD;
    animate(torus, 0.8f, 0.4f);
    {
        Entity& glow = scene.create("Centre Light", centre);
        glow.transform.position[1] = -2.f;
        Light l;
        l.color = {1.f, 0.62f, 0.3f};
        l.intensity = 2.5f;
        l.range = 22.f;
        glow.light = l;
        AudioSource hum;   // the centrepiece hums: louder as you get close
        hum.volume = 0.6f;
        hum.minDistance = 3.f;
        hum.maxDistance = 30.f;
        glow.audioSource = hum;
    }

    // Prefabs: templates that game code spawns (and the gems below are made from). Each is built as an entity,
    // turned into a prefab, and removed again.
    {
        Entity& b = make(scene, "Bullet", kNoEntity, MeshKind::Sphere, {}, {0.2f, 0.2f, 0.2f}, {1.f, 0.55f, 0.15f});
        b.collider.reset();   // it finds what it hits by raycasting
        addBehaviour(b, "Projectile");
        EntityId id = b.id;
        scene.addPrefab(scene.makePrefab(id, "Bullet"));
        scene.remove(id);
    }
    {
        Entity& g = make(scene, "Gem", kNoEntity, MeshKind::Cube, {}, {0.7f, 0.7f, 0.7f}, hsv(0.5f, 0.75f, 1.f));
        g.transform.rotation[0] = 45.f;
        g.transform.rotation[2] = 35.f;
        animate(g, 120.f * DEG2RAD, 0.25f);
        Collider trigger;   // a generous sphere around the gem
        trigger.shape = Collider::Shape::Sphere;
        trigger.radius = 1.6f;
        trigger.trigger = true;
        g.collider = trigger;
        addBehaviour(g, "Pickup");
        EntityId id = g.id;
        scene.addPrefab(scene.makePrefab(id, "Gem"));
        scene.remove(id);
    }

    {   // A bouncy ball, dropped by the spawner below
        Entity& b = make(scene, "Ball", kNoEntity, MeshKind::Sphere, {}, {0.8f, 0.8f, 0.8f}, {0.95f, 0.3f, 0.25f});
        b.meshRenderer->texture = 0;   // built-in "Checker": shows it rolling
        b.meshRenderer->color = {1.f, 0.45f, 0.35f};
        RigidBody rb;
        rb.bounciness = 0.75f;
        rb.friction = 0.3f;
        rb.mass = 0.5f;
        b.rigidBody = rb;
        AudioSource boing;
        boing.clip = "Bounce";
        boing.loop = boing.playOnStart = false;
        boing.playOnImpact = true;
        b.audioSource = boing;
        EntityId id = b.id;
        scene.addPrefab(scene.makePrefab(id, "Ball"));
        scene.remove(id);
    }

    // Physics playground near the start: a stack of crates to push and shoot, and balls raining down
    EntityId playground = group(scene, "Playground", {-7, 0, 25}).id;
    for (int row = 0, n = 0; row < 3; ++row)
        for (int i = 0; i < 3 - row; ++i, ++n) {
            Entity& c = make(scene, numbered("Crate", n + 1), playground, MeshKind::Cube,
                             {(i - (2 - row) * 0.5f) * 1.05f, 0.5f + row * 1.0f, 0}, {1, 1, 1}, {0.85f, 0.65f, 0.4f});
            c.meshRenderer->texture = 2;   // built-in "Tiles"
            c.rigidBody = RigidBody{};
            AudioSource thud;
            thud.clip = "Thud";
            thud.loop = thud.playOnStart = false;
            thud.playOnImpact = true;
            c.audioSource = thud;
        }
    {
        Entity& sp = group(scene, "Ball Spawner", {6, 7, 25});
        addBehaviour(sp, "Spawner");
        setField(*sp.behaviours.back(), "prefab", std::string("Ball"));
        setField(*sp.behaviours.back(), "interval", 2.5f);
        setField(*sp.behaviours.back(), "maxAlive", 6.f);
    }

    // Gems to collect: walk into them
    EntityId gems = group(scene, "Gems").id;
    for (int i = 0; i < 8; ++i) {
        float a = i * 2.f * PI / 8 + PI / 8;
        std::vector<Entity> copy = scene.instantiate(*scene.findPrefab("Gem"), gems);
        Entity& g = copy[0];
        g.name = numbered("Gem", i + 1);
        g.transform.position[0] = std::cos(a) * 26.f;
        g.transform.position[1] = 1.2f;
        g.transform.position[2] = std::sin(a) * 26.f;
        g.meshRenderer->color = hsv(0.5f + 0.06f * (i % 3), 0.75f, 1.f);
        for (BehaviourPtr& b : g.behaviours) setField(*b, "phase", a / DEG2RAD);   // Bob: out of step with each other
        for (Entity& e : copy) scene.add(std::move(e));
    }

    // Ring of pillars, each with a floating spinning cube on top
    EntityId ring = group(scene, "Pillar Ring").id;
    const int pillars = 12;
    for (int i = 0; i < pillars; ++i) {
        float a = i * 2.f * PI / pillars, h = (i % 2) ? 5.f : 8.f;
        Vec3 p{std::cos(a) * 20.f, 0, std::sin(a) * 20.f};
        make(scene, numbered("Pillar", i + 1), ring, MeshKind::Cube, p + Vec3{0, h * 0.5f, 0}, {1.6f, h, 1.6f},
             {0.82f, 0.8f, 0.76f});
        Entity& top = make(scene, numbered("Pillar Top", i + 1), ring, MeshKind::Cube, p + Vec3{0, h + 1.8f, 0},
                           {1, 1, 1}, hsv((float)i / pillars, 0.7f, 0.95f));
        top.transform.rotation[0] = 0.6f / DEG2RAD;
        animate(top, 1.5f, 0.5f, a);
    }

    // Distant landmark pyramids
    EntityId landmarks = group(scene, "Landmarks").id;
    for (int i = 0; i < 5; ++i) {
        float a = i * 2.f * PI / 5 + 0.3f, d = rng.range(110, 160), s = rng.range(20, 35);
        Entity& e = make(scene, numbered("Landmark", i + 1), landmarks, MeshKind::Pyramid,
                         {std::cos(a) * d, 0, std::sin(a) * d}, {s, s * 0.8f, s}, {1.f, 0.95f, 0.85f});
        e.transform.rotation[1] = a / DEG2RAD;
        e.meshRenderer->texture = 1;   // built-in "Bricks"
        e.meshRenderer->tiling = std::round(s / 3.f);
    }

    // Random scattered objects
    EntityId scatter = group(scene, "Scattered").id;
    const MeshKind kinds[4] = {MeshKind::Cube, MeshKind::Sphere, MeshKind::Pyramid, MeshKind::Torus};
    const char* kNames[4] = {"Cube", "Sphere", "Pyramid", "Torus"};
    int counters[4] = {};
    for (int i = 0; i < 120; ++i) {
        float a = rng.range(0, 2 * PI), d = rng.range(28, 100);
        int k = (int)(rng.next() * 4) % 4;
        float s = rng.range(1.f, 4.f);
        Entity& o = make(scene, numbered(kNames[k], ++counters[k]), scatter, kinds[k],
                         {std::cos(a) * d, 0, std::sin(a) * d}, {s, s, s},
                         hsv(rng.next(), rng.range(0.4f, 0.8f), rng.range(0.7f, 0.95f)));
        Transform& t = o.transform;
        float phase = rng.range(0, 2 * PI);
        t.rotation[1] = rng.range(0, 360);
        switch (k) {
            case 0: t.position[1] = s * 0.5f; break;
            case 1: t.position[1] = s * 0.5f + 1.f; animate(o, 0.f, 0.8f, phase); break;
            case 2: t.scale[1] = s * rng.range(0.8f, 2.f); break;
            case 3: t.position[1] = s * 0.5f + 0.5f; animate(o, 1.f, 0.f, phase); t.rotation[0] = 90.f; break;
        }
        if (k == 0 && rng.next() < 0.3f) {
            t.position[1] += 2.f;
            t.rotation[0] = 0.6f / DEG2RAD;
            animate(o, 0.7f, 0.5f, phase);
        }
        if (k == 0) {   // brick cubes
            o.meshRenderer->texture = 1;
            o.meshRenderer->color = {1.f, 1.f, 1.f};
        }
        // Shoot them: cubes leave a gem behind
        addBehaviour(o, "Destructible");
        if (k == 0) setField(*o.behaviours.back(), "dropPrefab", std::string("Gem"));
    }
    return scene;
}
