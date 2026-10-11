// Particle simulation: ParticleEmitter components emit, and every particle ages, moves and fades. The game runs one
// (in its fixed update); the editor runs another to preview emitters outside Play.
#pragma once

#include <unordered_map>
#include <vector>

#include "render/renderer.h"
#include "scene/scene.h"

class ParticleSystem {
public:
    // Emitters on visible entities emit (if `emitting`), then everything moves on by dt. Particles of entities that
    // are gone (or hidden) live out their lives.
    void update(const Scene& scene, const std::vector<Mat4>& worlds, float dt, bool emitting = true);
    // A burst from an entity's emitter: `count` particles (or its burst size)
    void burst(const Scene& scene, EntityId id, int count = -1);
    // Particles with any look, anywhere (no component needed): `up` is the direction they go out along
    void emit(const ParticleEmitter& look, Vec3 position, int count, Vec3 up = {0, 1, 0});
    void collect(std::vector<ParticleDraw>& out) const;   // appends what to draw
    void clear();
    size_t count() const;

private:
    struct Particle {
        Vec3 position, velocity;
        float age = 0.f, life = 1.f;
    };
    struct Group {
        ParticleEmitter look;
        std::vector<Particle> particles;
        float pending = 0.f;   // fractional particles owed by the rate
        float elapsed = 0.f;
        bool started = false;
    };
    void spawn(Group& g, Vec3 position, Vec3 up, int count);
    float random01();

    std::unordered_map<EntityId, Group> emitters_;
    std::vector<Group> loose_;   // from emit(), and from entities that are gone
    uint32_t seed_ = 0x2545F491u;
};
