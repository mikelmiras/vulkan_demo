// game/particles: emitters, bursts, lifetimes, and particles outliving their entity
#include "test.h"

#include "game/game.h"
#include "game/particles.h"
#include "imgui.h"
#include "scene/serialization.h"

namespace {

EntityId addEmitter(Scene& s, ParticleEmitter p) {
    Entity& e = s.create("Emitter");
    e.particleEmitter = p;
    return e.id;
}

void run(ParticleSystem& ps, const Scene& s, float seconds, float dt = 1.f / 60.f) {
    for (float t = 0.f; t < seconds - 1e-4f; t += dt) ps.update(s, s.worldMatrices(), dt);
}

}  // namespace

TEST(particles_rate_lifetime_and_limits) {
    Scene s;
    ParticleEmitter p;
    p.rate = 60.f;
    p.lifetime = 10.f;
    p.lifetimeRandom = 0.f;
    EntityId id = addEmitter(s, p);
    ParticleSystem ps;
    run(ps, s, 1.f);
    CHECK(ps.count() >= 58 && ps.count() <= 60);   // 60 per second
    s.find(id)->particleEmitter->maxParticles = 70;
    run(ps, s, 1.f);
    CHECK(ps.count() == 70);                         // capped
    s.find(id)->particleEmitter->rate = 0.f;
    s.find(id)->particleEmitter->lifetime = 0.5f;    // (each keeps the life it was born with)
    run(ps, s, 10.f);
    CHECK(ps.count() == 0);                          // all lived out their 10 s
}

TEST(particles_bursts_motion_and_orphans) {
    Scene s;
    ParticleEmitter p;
    p.rate = 0.f;
    p.burst = 25;
    p.speed = 4.f;
    p.speedRandom = 0.f;
    p.spread = 0.f;          // straight up
    p.lifetime = 2.f;
    p.lifetimeRandom = 0.f;
    EntityId id = addEmitter(s, p);
    ParticleSystem ps;
    run(ps, s, 0.5f);
    CHECK(ps.count() == 25);   // the start burst
    std::vector<ParticleDraw> draws;
    ps.collect(draws);
    REQUIRE(draws.size() == 25);
    CHECK_NEAR(draws[0].position.y, 2.f, 0.1);   // 4 m/s for 0.5 s
    CHECK(draws[0].additive);
    ps.burst(s, id, 10);
    CHECK(ps.count() == 35);
    s.remove(id);                                // the entity goes: its particles don't
    run(ps, s, 1.f);
    CHECK(ps.count() == 35);
    run(ps, s, 1.5f);
    CHECK(ps.count() == 0);
    // No component needed
    ps.emit(p, {5, 0, 0}, 12, {1, 0, 0});
    draws.clear();
    ps.collect(draws);
    CHECK(draws.size() == 12);
    run(ps, s, 0.25f);
    draws.clear();
    ps.collect(draws);
    CHECK(draws[0].position.x > 5.5f);           // out along +X
}

TEST(particles_hidden_or_not_playing) {
    Scene s;
    ParticleEmitter p;
    p.rate = 100.f;
    EntityId id = addEmitter(s, p);
    s.find(id)->visible = false;
    ParticleSystem ps;
    run(ps, s, 0.5f);
    CHECK(ps.count() == 0);
    s.find(id)->visible = true;
    s.find(id)->particleEmitter->playOnStart = false;   // waits for game code
    run(ps, s, 0.5f);
    CHECK(ps.count() == 0);
}

// In the game: a Pickup with an emitter bursts when collected, and the sparkles outlive it
TEST(particles_pickup_burst_in_game) {
    test::ImGuiContextGuard imgui;
    Scene s = buildDefaultScene();
    MeshLibrary meshes = buildMeshLibrary();
    Game g;
    g.start(s, meshes);
    const Entity* gem = nullptr;
    for (const Entity& e : s.entities)
        if (e.particleEmitter && e.name.rfind("Gem", 0) == 0) gem = &e;
    REQUIRE(gem);
    EntityId gemId = gem->id;
    Mat4 m = s.worldMatrix(gemId);
    Entity* player = s.findByName("Player");
    player->transform.position[0] = m.m[12];
    player->transform.position[1] = 0.f;
    player->transform.position[2] = m.m[14];
    for (int i = 0; i < 4; ++i) {
        ImGui::NewFrame();
        g.step();
        ImGui::EndFrame();
    }
    CHECK(!s.find(gemId));
    CHECK(g.particles().count() >= 30);   // the sparkle burst (plus the centre's embers)
}

TEST(particles_saved_with_scene) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    ParticleEmitter p;
    p.rate = 7.f;
    p.burst = 3;
    p.additive = false;
    p.endColor = {0.1f, 0.2f, 0.3f};
    p.maxParticles = 42;
    addEmitter(s, p);
    std::string error;
    auto path = test::tempPath("particles.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, textures(), error));
    const ParticleEmitter& l = *loaded.findByName("Emitter")->particleEmitter;
    CHECK_NEAR(l.rate, 7.0, 0.0);
    CHECK(l.burst == 3 && !l.additive && l.maxParticles == 42);
    CHECK_VEC(l.endColor, test::v3(0.1f, 0.2f, 0.3f), 1e-6f);
}
