// Surfaces: decals (which are drawn, their boxes: render/decals.h, collectDecals), and parallax / detail maps,
// decals and the weather saved with a scene
#include "test.h"

#include "render/decals.h"
#include "render/renderer_internal.h"
#include "render/scene_draw.h"
#include "scene/serialization.h"

namespace {
DecalInfo decalAt(Vec3 c, float radius, int texture) {
    DecalInfo d;
    d.center = c;
    d.radius = radius;
    d.texture = texture;
    return d;
}
}  // namespace

TEST(decals_nearest_and_texture_slots) {
    std::vector<DecalInfo> all = {decalAt({30, 0, 0}, 1.f, 7), decalAt({5, 0, 0}, 1.f, 3), decalAt({10, 0, 0}, 8.f, 3),
                                  decalAt({50, 0, 0}, 1.f, -1), decalAt({2, 0, 0}, 0.5f, 9)};
    DecalPick p = pickDecals(all, {0, 0, 0}, 3, 8);
    // Nearest from their edge: the big one at 10 m (2 m away) before the one at 5 m (4 m); four left out of three
    REQUIRE(p.decals.size() == 3);
    CHECK(p.decals[0] == 4 && p.decals[1] == 2 && p.decals[2] == 1);
    // Textures: a slot each, shared by the decals using the same one
    CHECK(p.textures.size() == 2);
    CHECK(p.slots[0] == 0 && p.textures[0] == 9);
    CHECK(p.slots[1] == 1 && p.slots[2] == 1 && p.textures[1] == 3);
    // Out of slots: the further decals keep their colour alone
    DecalPick tight = pickDecals(all, {0, 0, 0}, 5, 1);
    CHECK(tight.textures.size() == 1 && tight.textures[0] == 9);
    CHECK(tight.slots[1] == -1 && tight.slots[2] == -1);
    for (size_t k = 0; k < tight.decals.size(); ++k)
        if (all[(size_t)tight.decals[k]].texture < 0) CHECK(tight.slots[k] == -1);
    CHECK(pickDecals({}, {0, 0, 0}, 64, 16).decals.empty());
    // Normal maps: slots of their own (none asked for: none given)
    all[4].normalTexture = 11;
    all[2].normalTexture = 11;
    all[1].normalTexture = 12;
    DecalPick withNormals = pickDecals(all, {0, 0, 0}, 3, 8, 1);
    CHECK(withNormals.normalTextures.size() == 1 && withNormals.normalTextures[0] == 11);
    CHECK(withNormals.normalSlots[0] == 0 && withNormals.normalSlots[1] == 0 && withNormals.normalSlots[2] == -1);
    CHECK(pickDecals(all, {0, 0, 0}, 3, 8).normalTextures.empty());
}

TEST(decals_collected_with_their_box) {
    Scene scene;
    Entity& e = scene.create("Puddle");
    e.transform.position[0] = 4.f;
    e.transform.position[1] = 1.f;
    e.transform.scale[0] = 2.f;
    e.transform.scale[1] = 1.f;
    e.transform.scale[2] = 6.f;
    Decal d;
    d.color = {1.f, 1.f, 1.f};
    d.wetness = 1.f;
    d.emissive = 4.f;
    e.decal = d;
    scene.create("Plain");
    std::vector<DecalInfo> decals = collectDecals(scene, scene.worldMatrices());
    REQUIRE(decals.size() == 1);
    const DecalInfo& info = decals[0];
    // The box's corner maps to (0.5, 0.5, 0.5); its middle to the origin; it projects straight down
    Vec3 corner = transformPoint(info.toBox, {5.f, 1.5f, 3.f});
    CHECK_NEAR(corner.x, 0.5, 1e-5);
    CHECK_NEAR(corner.y, 0.5, 1e-5);
    CHECK_NEAR(corner.z, 0.5, 1e-5);
    Vec3 mid = transformPoint(info.toBox, {4.f, 1.f, 0.f});
    CHECK_NEAR(length(mid), 0.0, 1e-5);
    CHECK_NEAR(info.down.y, -1.0, 1e-5);
    CHECK_NEAR(info.radius, length(Vec3{1.f, 0.5f, 3.f}), 1e-4);
    CHECK_NEAR(info.wetness, 1.0, 0.0);
    CHECK_NEAR(info.emissive, 4.0, 0.0);   // (glowing)
}

TEST(surfaces_saved) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    Entity& wall = s.create("Wall");
    MeshRenderer mr;
    mr.texture = textures().find("Bricks");
    mr.normalTexture = textures().find("Bricks Normal");
    mr.heightTexture = textures().find("Tiles");
    mr.parallaxDepth = 0.05f;
    mr.detailTexture = textures().find("Bricks Normal");
    mr.detailTiling = 12.f;
    mr.detailStrength = 0.6f;
    mr.sheltered = true;
    wall.meshRenderer = mr;
    Entity& mark = s.create("Marking");
    Decal d;
    d.texture = textures().find("Checker");
    d.color = {0.9f, 0.8f, 0.1f};
    d.opacity = 0.7f;
    d.roughness = 0.3f;
    d.wetness = 0.4f;
    d.angleFade = 0.2f;
    d.normalTexture = textures().find("Bricks Normal");
    d.normalStrength = 0.8f;
    d.emissive = 2.5f;
    mark.decal = d;
    s.create("Plain mark").decal = Decal{};
    s.render.wetness = 0.8f;
    s.render.puddles = 0.3f;
    s.render.rain = 0.6f;
    std::string error;
    auto path = test::tempPath("surfaces.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, textures(), error));
    const MeshRenderer& l = *loaded.findByName("Wall")->meshRenderer;
    CHECK(l.heightTexture == mr.heightTexture && l.heightTexture >= 0);
    CHECK_NEAR(l.parallaxDepth, 0.05, 1e-6);
    CHECK(l.detailTexture == mr.detailTexture);
    CHECK_NEAR(l.detailTiling, 12.0, 1e-6);
    CHECK_NEAR(l.detailStrength, 0.6, 1e-6);
    CHECK(l.sheltered);
    REQUIRE(loaded.findByName("Marking")->decal.has_value());
    const Decal& ld = *loaded.findByName("Marking")->decal;
    CHECK(ld.texture == d.texture && ld.texture >= 0);
    CHECK_NEAR(ld.color.z, 0.1, 1e-6);
    CHECK_NEAR(ld.opacity, 0.7, 1e-6);
    CHECK_NEAR(ld.roughness, 0.3, 1e-6);
    CHECK_NEAR(ld.wetness, 0.4, 1e-6);
    CHECK_NEAR(ld.angleFade, 0.2, 1e-6);
    CHECK(ld.normalTexture == d.normalTexture && ld.normalTexture >= 0);
    CHECK_NEAR(ld.normalStrength, 0.8, 1e-6);
    CHECK_NEAR(ld.emissive, 2.5, 1e-6);
    CHECK(loaded.findByName("Plain mark")->decal->emissive == 0.f);   // (none: as before)
    CHECK_NEAR(loaded.render.wetness, 0.8, 1e-6);
    CHECK_NEAR(loaded.render.puddles, 0.3, 1e-6);
    CHECK_NEAR(loaded.render.rain, 0.6, 1e-6);
    // Plain: none of it (and dry)
    MeshRenderer plain;
    CHECK(plain.heightTexture < 0 && plain.parallaxDepth == 0.f && plain.detailTexture < 0 && !plain.sheltered);
    CHECK(Scene{}.render.wetness == 0.f && Scene{}.render.rain == 0.f);
}

TEST(decals_culled_to_the_view) {
    // A camera at the origin looking down -z: a sphere ahead is in view, one behind or far off to the side isn't,
    // one just off the edge but reaching in is
    Mat4 vp = perspective(60.f * DEG2RAD, 1.f, 0.1f, 100.f, true) * lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    render_detail::Frustum f(vp);
    CHECK(f.visible(Vec3{0, 0, -10}, 1.f));
    CHECK(!f.visible(Vec3{0, 0, 10}, 1.f));
    CHECK(!f.visible(Vec3{40, 0, -10}, 1.f));
    CHECK(f.visible(Vec3{7, 0, -10}, 2.f));    // (the edge at x = 5.77 there: 7 - 2 reaches in)
    CHECK(!f.visible(Vec3{0, 0, -150}, 1.f));  // (past the far plane)
}

TEST(gpu_particle_emitters_collected_and_saved) {
    Scene scene;
    Entity& rain = scene.create("Rain");
    rain.transform.position[1] = 10.f;
    rain.transform.rotation[0] = 180.f;   // (pointing down)
    ParticleEmitter p;
    p.gpu = true;
    p.rate = 5000.f;
    p.maxParticles = 9000;
    p.intensity = 2.f;
    p.startColor = {1.f, 1.f, 1.f};
    rain.particleEmitter = p;
    const EntityId rainId = rain.id;   // (the next create may move the entities)
    Entity& sparks = scene.create("Sparks");
    sparks.particleEmitter = ParticleEmitter{};   // (the CPU's)
    std::vector<GpuEmitterInfo> gpu = collectGpuEmitters(scene, scene.worldMatrices());
    REQUIRE(gpu.size() == 1);
    CHECK(gpu[0].key == rainId);
    CHECK_NEAR(gpu[0].position.y, 10.0, 1e-5);
    CHECK_NEAR(gpu[0].up.y, -1.0, 1e-4);
    CHECK(gpu[0].maxParticles == 9000);
    CHECK_NEAR(gpu[0].startColor.x, 2.0, 1e-5);   // (linear, times the brightness)
    MeshLibrary meshes = buildMeshLibrary();
    std::string error;
    auto path = test::tempPath("gpu_particles.vkw").string();
    REQUIRE(saveScene(path, scene, meshes, textures(), error));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, textures(), error));
    CHECK(loaded.findByName("Rain")->particleEmitter->gpu);
    CHECK(!loaded.findByName("Sparks")->particleEmitter->gpu);
}
