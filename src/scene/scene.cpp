#include "scene/scene.h"

#include <algorithm>
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

Mat4 Animator::apply(const Transform& t, float time) const {
    Mat4 base = t.matrix();
    if (spin == 0.f && bob == 0.f) return base;
    // Spin around the parent's Y axis through the entity's origin, and bob up and down.
    // Both start from the edit-time pose at time 0, so pressing Play doesn't make anything jump.
    Vec3 p = t.pos();
    Vec3 offset{0, bob * (std::sin(time * 1.5f + phase) - std::sin(phase)), 0};
    return translate(p + offset) * rotateY(spin * time) * translate(p * -1.f) * base;
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
    for (Entity& e : entities)
        if (e.name == name) return &e;
    return nullptr;
}

std::vector<Mat4> Scene::worldMatrices(bool animate, float time) const {
    std::unordered_map<EntityId, size_t> index;
    index.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) index[entities[i].id] = i;

    std::vector<Mat4> world(entities.size());
    std::vector<char> done(entities.size(), 0);
    std::function<const Mat4&(size_t, int)> compute = [&](size_t i, int depth) -> const Mat4& {
        if (done[i]) return world[i];
        const Entity& e = entities[i];
        Mat4 local = animate && e.animator ? e.animator->apply(e.transform, time) : e.transform.matrix();
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

Entity& make(Scene& scene, std::string name, EntityId parent, MeshKind mesh, Vec3 pos, Vec3 size, Vec3 color) {
    Entity& e = scene.create(name, parent);
    Transform& t = e.transform;
    t.position[0] = pos.x; t.position[1] = pos.y; t.position[2] = pos.z;
    t.scale[0] = size.x; t.scale[1] = size.y; t.scale[2] = size.z;
    MeshRenderer mr;
    mr.mesh = (int)mesh;
    mr.color = color;
    e.meshRenderer = mr;
    return e;
}

Entity& group(Scene& scene, const char* name, Vec3 pos = {0, 0, 0}) {
    Entity& e = scene.create(name);
    e.transform.position[0] = pos.x; e.transform.position[1] = pos.y; e.transform.position[2] = pos.z;
    return e;
}

Animator& animate(Entity& e, float spin, float bob, float phase = 0.f) {
    e.animator = Animator{spin, bob, phase};
    return *e.animator;
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

    Entity& cam = group(scene, "Main Camera", {0, 7, 34});
    cam.camera = Camera{};
    cam.transform.rotation[0] = -9.f;   // look slightly down at the centrepiece

    Entity& ground = make(scene, "Ground", kNoEntity, MeshKind::Plane, {0, 0, 0}, {600, 1, 600}, {0.42f, 0.55f, 0.38f});
    ground.meshRenderer->checker = true;
    ground.locked = true;

    // Centrepiece: golden sphere inside a spinning tilted torus
    EntityId centre = group(scene, "Centrepiece", {0, 5, 0}).id;
    animate(make(scene, "Centre Sphere", centre, MeshKind::Sphere, {0, 0, 0}, {3, 3, 3}, {0.95f, 0.75f, 0.25f}), 0.f, 0.4f);
    Entity& torus = make(scene, "Centre Torus", centre, MeshKind::Torus, {0, 0, 0}, {7, 7, 7}, {0.85f, 0.2f, 0.25f});
    torus.transform.rotation[0] = 0.6f / DEG2RAD;
    animate(torus, 0.8f, 0.4f);

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
                         {std::cos(a) * d, 0, std::sin(a) * d}, {s, s * 0.8f, s}, {0.85f, 0.75f, 0.55f});
        e.transform.rotation[1] = a / DEG2RAD;
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
    }
    return scene;
}
