#include "game/particles.h"

#include <algorithm>
#include <cmath>

float ParticleSystem::random01() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return (seed_ & 0xFFFFFF) / float(0x1000000);
}

void ParticleSystem::spawn(Group& g, Vec3 position, Vec3 up, int count) {
    const ParticleEmitter& e = g.look;
    up = length(up) > 1e-6f ? normalize(up) : Vec3{0, 1, 0};
    // Two axes across `up`, for the cone
    Vec3 side = std::fabs(up.y) < 0.9f ? normalize(cross(up, {0, 1, 0})) : normalize(cross(up, {1, 0, 0}));
    Vec3 other = cross(up, side);
    float cosMax = std::cos(std::clamp(e.spread, 0.f, 180.f) * DEG2RAD);
    for (int i = 0; i < count && (int)g.particles.size() < e.maxParticles; ++i) {
        // A direction in the cone, uniform over its area of the sphere
        float z = 1.f - random01() * (1.f - cosMax), phi = random01() * 2.f * PI, r = std::sqrt(std::max(0.f, 1.f - z * z));
        Vec3 dir = up * z + side * (r * std::cos(phi)) + other * (r * std::sin(phi));
        Vec3 offset;
        if (e.radius > 0.f) {   // a point inside the sphere
            Vec3 d;
            do d = Vec3{random01() * 2.f - 1.f, random01() * 2.f - 1.f, random01() * 2.f - 1.f};
            while (dot(d, d) > 1.f);
            offset = d * e.radius;
        }
        Particle p;
        p.position = position + offset;
        p.velocity = dir * (e.speed * (1.f + (random01() * 2.f - 1.f) * e.speedRandom));
        p.life = std::max(0.05f, e.lifetime * (1.f + (random01() * 2.f - 1.f) * e.lifetimeRandom));
        g.particles.push_back(p);
    }
}

void ParticleSystem::update(const Scene& scene, const std::vector<Mat4>& worlds, float dt, bool emitting) {
    // Emitters: follow the components (edits show at once); gone ones hand their particles over
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        int i = scene.indexOf(it->first);
        bool alive = i >= 0 && scene.entities[(size_t)i].particleEmitter && scene.visibleInHierarchy(it->first);
        if (alive) { ++it; continue; }
        if (!it->second.particles.empty()) loose_.push_back(std::move(it->second));
        it = emitters_.erase(it);
    }
    for (size_t i = 0; i < scene.entities.size() && i < worlds.size(); ++i) {
        const Entity& e = scene.entities[i];
        if (!e.particleEmitter || !scene.visibleInHierarchy(e.id)) continue;
        Group& g = emitters_[e.id];
        g.look = *e.particleEmitter;
        const Mat4& m = worlds[i];
        Vec3 pos{m.m[12], m.m[13], m.m[14]}, up = transformDir(m, {0, 1, 0});
        if (!g.started) {
            g.started = true;
            if (emitting && g.look.playOnStart && g.look.burst > 0) spawn(g, pos, up, g.look.burst);
        }
        if (emitting && g.look.playOnStart && (g.look.duration <= 0.f || g.elapsed < g.look.duration)) {
            g.pending += g.look.rate * dt;
            int n = (int)g.pending;
            g.pending -= (float)n;
            spawn(g, pos, up, n);
        }
        g.elapsed += dt;
    }
    // Every particle: age, fall, slow down, move
    auto simulate = [&](Group& g) {
        const ParticleEmitter& e = g.look;
        float keep = std::max(0.f, 1.f - e.drag * dt);
        for (Particle& p : g.particles) {
            p.age += dt;
            p.velocity.y -= e.gravity * dt;
            p.velocity = p.velocity * keep;
            p.position += p.velocity * dt;
        }
        g.particles.erase(std::remove_if(g.particles.begin(), g.particles.end(), [](const Particle& p) { return p.age >= p.life; }),
                          g.particles.end());
    };
    for (auto& [id, g] : emitters_) simulate(g);
    for (Group& g : loose_) simulate(g);
    loose_.erase(std::remove_if(loose_.begin(), loose_.end(), [](const Group& g) { return g.particles.empty(); }), loose_.end());
}

void ParticleSystem::burst(const Scene& scene, EntityId id, int count) {
    const Entity* e = scene.find(id);
    if (!e || !e->particleEmitter) return;
    Group& g = emitters_[id];
    g.look = *e->particleEmitter;
    g.started = true;   // (a burst on request doesn't also fire the start burst)
    Mat4 m = scene.worldMatrix(id);
    spawn(g, {m.m[12], m.m[13], m.m[14]}, transformDir(m, {0, 1, 0}), count >= 0 ? count : std::max(g.look.burst, 1));
}

void ParticleSystem::emit(const ParticleEmitter& look, Vec3 position, int count, Vec3 up) {
    Group g;
    g.look = look;
    g.started = true;
    spawn(g, position, up, count);
    if (!g.particles.empty()) loose_.push_back(std::move(g));
}

void ParticleSystem::collect(std::vector<ParticleDraw>& out) const {
    auto add = [&](const Group& g) {
        const ParticleEmitter& e = g.look;
        for (const Particle& p : g.particles) {
            float t = std::clamp(p.age / p.life, 0.f, 1.f);
            Vec3 srgb = e.startColor + (e.endColor - e.startColor) * t;
            Vec3 c = srgbToLinear(srgb) * e.intensity;
            ParticleDraw d;
            d.position = p.position;
            d.size = e.startSize + (e.endSize - e.startSize) * t;
            d.color[0] = c.x;
            d.color[1] = c.y;
            d.color[2] = c.z;
            d.color[3] = std::clamp(e.startAlpha + (e.endAlpha - e.startAlpha) * t, 0.f, 1.f);
            d.additive = e.additive;
            d.streak = p.velocity * std::max(e.stretch, 0.f);
            d.lit = e.lit;
            out.push_back(d);
        }
    };
    for (const auto& [id, g] : emitters_) add(g);
    for (const Group& g : loose_) add(g);
}

void ParticleSystem::clear() {
    emitters_.clear();
    loose_.clear();
}

size_t ParticleSystem::count() const {
    size_t n = 0;
    for (const auto& [id, g] : emitters_) n += g.particles.size();
    for (const Group& g : loose_) n += g.particles.size();
    return n;
}
