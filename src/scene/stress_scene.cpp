#include "scene/stress_scene.h"

#include <cmath>
#include <cstdio>

namespace {

struct Rng {
    uint32_t s = 0x9E3779B9u;
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFF) / float(0x1000000); }
    float range(float a, float b) { return a + (b - a) * next(); }
};

Entity& make(Scene& scene, const MeshLibrary& meshes, const char* base, int n, EntityId parent, MeshKind mesh, Vec3 pos,
             Vec3 size, Vec3 color) {
    char name[64];
    std::snprintf(name, sizeof(name), "%s.%04d", base, n);
    Entity& e = scene.create(name, parent);
    Transform& t = e.transform;
    t.position[0] = pos.x; t.position[1] = pos.y; t.position[2] = pos.z;
    t.scale[0] = size.x; t.scale[1] = size.y; t.scale[2] = size.z;
    MeshRenderer mr;
    mr.mesh = (int)mesh;
    mr.color = color;
    e.meshRenderer = mr;
    e.collider = fitCollider(meshes, (int)mesh);
    return e;
}

}  // namespace

Scene buildStressScene(const StressSettings& settings) {
    static const MeshLibrary meshes = buildMeshLibrary();
    Scene scene = buildDefaultScene();
    Rng rng;
    // Laid out on a square grid around the demo (outside its central 60 m), 4 m apart
    int side = (int)std::ceil(std::sqrt((float)settings.statics + 400.f));
    float spacing = 4.f, half = side * spacing * 0.5f;
    auto cell = [&](int i) {
        float x = -half + (float)(i % side) * spacing, z = -half + (float)(i / side) * spacing;
        return Vec3{x + rng.range(-1.f, 1.f), 0.f, z + rng.range(-1.f, 1.f)};
    };
    auto outsideDemo = [](Vec3 p) { return std::fabs(p.x) > 30.f || std::fabs(p.z) > 30.f; };

    EntityId field = scene.create("Stress Field").id;
    int placed = 0;
    for (int i = 0; placed < settings.statics && i < side * side * 2; ++i) {
        Vec3 p = cell(i % (side * side));
        if (!outsideDemo(p)) continue;
        int kind = placed % 3;
        MeshKind mesh = kind == 0 ? MeshKind::Cube : kind == 1 ? MeshKind::Sphere : MeshKind::Pyramid;
        float s = rng.range(0.6f, 1.6f);
        p.y = s * 0.5f;
        Entity& e = make(scene, meshes, "Static", placed, field, mesh, p, {s, s, s},
                         {rng.range(0.3f, 0.9f), rng.range(0.3f, 0.9f), rng.range(0.3f, 0.9f)});
        if (placed % 4 == 0)
            for (BehaviourPtr& b : motionBehaviours(rng.range(0.5f, 2.f), 0.f, 0.f)) e.behaviours.push_back(std::move(b));
        ++placed;
    }

    EntityId bodies = scene.create("Stress Bodies").id;
    for (int i = 0; i < settings.bodies; ++i) {
        Vec3 p{rng.range(-half, half), rng.range(4.f, 20.f), rng.range(-half, half)};
        if (!outsideDemo(p)) p.x += p.x < 0 ? -30.f : 30.f;
        bool ball = i % 2 == 1;
        Entity& e = make(scene, meshes, ball ? "Ball" : "Crate", i, bodies, ball ? MeshKind::Sphere : MeshKind::Cube, p,
                         {0.8f, 0.8f, 0.8f}, ball ? Vec3{0.9f, 0.4f, 0.2f} : Vec3{0.6f, 0.45f, 0.3f});
        RigidBody rb;
        rb.bounciness = ball ? 0.6f : 0.2f;
        e.rigidBody = rb;
    }

    EntityId triggers = scene.create("Stress Triggers").id;
    for (int i = 0; i < settings.triggers; ++i) {
        Vec3 p{rng.range(-half, half), 1.2f, rng.range(-half, half)};
        if (!outsideDemo(p)) p.z += p.z < 0 ? -30.f : 30.f;
        Entity& e = make(scene, meshes, "Trigger", i, triggers, MeshKind::Sphere, p, {0.6f, 0.6f, 0.6f}, {0.3f, 0.9f, 0.6f});
        e.collider->trigger = true;
        for (BehaviourPtr& b : motionBehaviours(0.f, 0.3f, rng.range(0.f, 6.f))) e.behaviours.push_back(std::move(b));
    }
    return scene;
}
