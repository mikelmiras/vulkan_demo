// Textures, glTF import, materials and lights (the data side; the GPU side is checked by screenshots)
#include "test.h"

#include "assets/gltf.h"
#include "core/json.h"
#include "core/png.h"
#include "render/scene_draw.h"
#include "scene/scene_diff.h"
#include "scene/serialization.h"

namespace {

std::string base64(const std::vector<uint8_t>& data) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < data.size() ? (uint32_t)data[i + 1] << 8 : 0) |
                     (i + 2 < data.size() ? data[i + 2] : 0);
        out += abc[(v >> 18) & 63];
        out += abc[(v >> 12) & 63];
        out += i + 1 < data.size() ? abc[(v >> 6) & 63] : '=';
        out += i + 2 < data.size() ? abc[v & 63] : '=';
    }
    return out;
}

template <class T>
void append(std::vector<uint8_t>& buf, std::initializer_list<T> values) {
    for (T v : values) {
        const uint8_t* p = (const uint8_t*)&v;
        buf.insert(buf.end(), p, p + sizeof(T));
    }
}

// A glTF with a parent node (translated) and a child (scaled) holding a red, textured triangle. The buffer is
// embedded (base64), the texture is a PNG next to the file.
std::string writeTestGltf() {
    uint8_t px[2 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    writePng(test::tempPath("tex.png").string(), 2, 2, px);
    std::vector<uint8_t> buf;
    append<float>(buf, {0, 0, 0, 1, 0, 0, 0, 1, 0});   // positions @0
    append<float>(buf, {0, 0, 1, 0, 0, 1});            // uvs @36
    append<uint16_t>(buf, {0, 1, 2, 0});               // indices @60 (+ padding)
    std::string gltf = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
      "nodes":[{"name":"Parent","translation":[1,2,3],"children":[1]},{"name":"Tri","mesh":0,"scale":[2,2,2]}],
      "meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]}],
      "materials":[{"name":"Red","pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1],"baseColorTexture":{"index":0}}}],
      "textures":[{"source":0}],"images":[{"uri":"tex.png"}],
      "buffers":[{"byteLength":68,"uri":"data:application/octet-stream;base64,)" + base64(buf) + R"("}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24},
                     {"buffer":0,"byteOffset":60,"byteLength":6}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
                   {"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
                   {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}]})";
    std::string path = test::tempPath("model.gltf").string();
    test::writeFile(path, gltf);
    return path;
}

}  // namespace

TEST(textures_builtin_and_png_roundtrip) {
    TextureLibrary lib = buildTextureLibrary();
    REQUIRE(lib.count() == kBuiltinTextureCount);
    CHECK(lib.find("Bricks") == 1);
    CHECK(lib.get(0).rgba.size() == (size_t)lib.get(0).width * lib.get(0).height * 4);

    uint8_t px[3 * 2 * 4];
    for (int i = 0; i < 24; ++i) px[i] = (uint8_t)(i * 10);
    auto path = test::tempPath("small.png").string();
    REQUIRE(writePng(path, 3, 2, px));
    std::string err;
    int id = loadTexture(lib, path, err);
    REQUIRE(id == kBuiltinTextureCount);
    CHECK(lib.get(id).width == 3 && lib.get(id).height == 2);
    CHECK(std::equal(px, px + 24, lib.get(id).rgba.begin()));   // our PNG writer and stb_image agree
    CHECK(lib.get(id).name == "small");
    CHECK(loadTexture(lib, path, err) == id);                    // same file: reused
    CHECK(loadTexture(lib, test::tempPath("nope.png").string(), err) == -1);
    test::writeFile(test::tempPath("bad.png"), "not an image");
    CHECK(loadTexture(lib, test::tempPath("bad.png").string(), err) == -1);
}

TEST(obj_import_texture_coordinates) {
    MeshLibrary lib = buildMeshLibrary();
    auto path = test::tempPath("uv.obj");
    test::writeFile(path, "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n");
    std::string err;
    int id = importObj(lib, path.string(), err);
    REQUIRE(id >= 0);
    const Vertex* v = &lib.vertices[(size_t)lib.get(id).vertexOffset];
    CHECK_NEAR(v[1].u, 1.f, 1e-6);
    CHECK_NEAR(v[2].v, 0.f, 1e-6);   // OBJ v = 1 (up) -> our v = 0 (top)
    CHECK_NEAR(v[0].v, 1.f, 1e-6);
}

TEST(gltf_import_as_prefab) {
    std::string path = writeTestGltf();
    MeshLibrary meshes = buildMeshLibrary();
    TextureLibrary texs = buildTextureLibrary();
    Prefab p;
    std::string err, warning;
    REQUIRE(importGltfPrefab(path, meshes, texs, p, err, warning));
    CHECK(warning.empty());
    REQUIRE(p.entities.size() == 3);   // root, Parent, Tri
    CHECK(p.entities[0].name == "model");
    CHECK(p.entities[1].name == "Parent" && p.entities[1].parent == p.entities[0].id);
    CHECK_VEC(p.entities[1].transform.pos(), test::v3(1, 2, 3), 1e-5f);
    const Entity& tri = p.entities[2];
    CHECK(tri.parent == p.entities[1].id);
    CHECK_NEAR(tri.transform.scale[0], 2.f, 1e-5);
    REQUIRE(tri.meshRenderer.has_value());
    CHECK_VEC(tri.meshRenderer->color, test::v3(1, 0, 0), 1e-5f);
    REQUIRE(tri.meshRenderer->texture >= kBuiltinTextureCount);
    CHECK(texs.get(tri.meshRenderer->texture).width == 2);
    const Mesh& m = meshes.get(tri.meshRenderer->mesh);
    CHECK(m.indexCount == 3);
    const Vertex* v = &meshes.vertices[(size_t)m.vertexOffset];
    CHECK_VEC(v[0].normal, test::v3(0, 0, 1), 1e-5f);   // computed: the file has no normals
    CHECK_NEAR(v[2].v, 1.f, 1e-6);
    CHECK_VEC(m.boundsMax, test::v3(1, 1, 0), 1e-6f);   // real size, not normalised like OBJ

    // In a scene: saved by source and re-imported into fresh libraries
    Scene s;
    s.addPrefab(p);
    for (Entity& e : s.instantiate(s.prefabs[0])) s.add(std::move(e));
    auto scenePath = test::tempPath("gltf.vkw").string();
    REQUIRE(saveScene(scenePath, s, meshes, texs, err));
    MeshLibrary meshes2 = buildMeshLibrary();
    TextureLibrary texs2 = buildTextureLibrary();
    Scene loaded;
    REQUIRE(loadScene(scenePath, loaded, meshes2, texs2, err));
    const Entity* t2 = loaded.findByName("Tri");
    REQUIRE(t2 && t2->meshRenderer);
    CHECK(meshes2.get(t2->meshRenderer->mesh).indexCount == 3);
    CHECK(texs2.get(t2->meshRenderer->texture).source.find("#image0") != std::string::npos);

    CHECK(!importGltfPrefab(test::tempPath("missing.gltf").string(), meshes, texs, p, err, warning));
}

TEST(lights_and_materials_saved_with_scene) {
    MeshLibrary meshes = buildMeshLibrary();
    TextureLibrary texs = buildTextureLibrary();
    uint8_t px[4] = {1, 2, 3, 255};
    writePng(test::tempPath("one.png").string(), 1, 1, px);
    std::string err;
    int tex = loadTexture(texs, test::tempPath("one.png").string(), err);
    Scene s;
    Entity& e = s.create("Lamp");
    e.light = Light{Light::Type::Point, {1, 0.5f, 0.25f}, 3.f, 7.f, false};
    MeshRenderer mr;
    mr.texture = tex;
    mr.tiling = 4.f;
    e.meshRenderer = mr;
    Entity& sun = s.create("Sun");
    sun.light = Light{};
    sun.light->type = Light::Type::Directional;
    sun.light->castShadows = false;
    auto path = test::tempPath("lights.vkw").string();
    REQUIRE(saveScene(path, s, meshes, texs, err));
    TextureLibrary texs2 = buildTextureLibrary();
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, texs2, err));
    for (size_t i = 0; i < s.entities.size(); ++i)
        CHECK(json::write(entityToJson(s.entities[i], meshes, texs)) ==
              json::write(entityToJson(loaded.entities[i], meshes, texs2)));
    CHECK(texs2.count() == kBuiltinTextureCount + 1);
}

// PBR materials and the scene's look survive a save / load
TEST(materials_and_render_settings_saved) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    Entity& e = s.create("Thing");
    MeshRenderer mr;
    mr.metallic = 0.7f;
    mr.roughness = 0.2f;
    mr.normalTexture = textures().find("Bricks Normal");
    mr.normalScale = 1.5f;
    mr.metalRoughTexture = textures().find("Tiles");
    mr.emissive = {1.f, 0.5f, 0.f};
    mr.emissiveStrength = 3.f;
    mr.emissiveTexture = textures().find("Checker");
    mr.clearcoat = 0.9f;
    mr.clearcoatRoughness = 0.1f;
    mr.sheen = 0.6f;
    mr.anisotropy = -0.4f;
    mr.translucency = 0.45f;
    mr.subsurface = 0.8f;
    mr.subsurfaceColor = {0.9f, 0.4f, 0.3f};
    e.meshRenderer = mr;
    s.create("Plain").meshRenderer = MeshRenderer{};
    s.render.exposure = 1.3f;
    s.render.bloomStrength = 0.2f;
    s.render.sky = {0.1f, 0.2f, 0.3f};
    s.render.fogDensity = 0.01f;
    s.render.gi = true;
    s.render.giSpacing = 2.5f;
    s.render.giIntensity = 1.4f;
    s.render.reflections = true;
    s.render.shadowSoftness = 1.7f;
    s.render.volumetricFog = true;
    s.render.fogLight = 2.5f;
    s.render.fogAnisotropy = 0.3f;
    s.render.fogHeightFalloff = 0.15f;
    s.render.tonemapper = 1;
    s.render.contrast = 1.2f;
    s.render.saturation = 0.8f;
    s.render.vignette = 0.4f;
    s.render.autoExposure = true;
    s.render.exposureMin = -2.f;
    s.render.exposureMax = 3.f;
    s.render.adaptationSpeed = 2.5f;
    s.render.localToneMapping = 0.6f;
    s.render.lensDirt = 0.3f;
    s.render.ssgi = true;
    s.render.contactShadows = true;
    s.render.ssgiIntensity = 0.7f;
    std::string error;
    auto path = test::tempPath("materials.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, textures(), error));
    const MeshRenderer& l = *loaded.findByName("Thing")->meshRenderer;
    CHECK_NEAR(l.metallic, 0.7, 1e-6);
    CHECK_NEAR(l.roughness, 0.2, 1e-6);
    CHECK(l.normalTexture == mr.normalTexture && l.normalTexture >= 0);
    CHECK_NEAR(l.normalScale, 1.5, 1e-6);
    CHECK(l.metalRoughTexture == mr.metalRoughTexture);
    CHECK(l.emissiveTexture == mr.emissiveTexture);
    CHECK_VEC(l.emissive, test::v3(1.f, 0.5f, 0.f), 1e-6f);
    CHECK_NEAR(l.emissiveStrength, 3.0, 1e-6);
    CHECK_NEAR(l.clearcoat, 0.9, 1e-6);
    CHECK_NEAR(l.clearcoatRoughness, 0.1, 1e-6);
    CHECK_NEAR(l.sheen, 0.6, 1e-6);
    CHECK_NEAR(l.anisotropy, -0.4, 1e-6);
    CHECK_NEAR(l.translucency, 0.45, 1e-6);
    CHECK_NEAR(l.subsurface, 0.8, 1e-6);
    CHECK_VEC(l.subsurfaceColor, test::v3(0.9f, 0.4f, 0.3f), 1e-6f);
    const MeshRenderer& unlayered = *loaded.findByName("Plain")->meshRenderer;   // (none: as before)
    CHECK(unlayered.clearcoat == 0.f && unlayered.sheen == 0.f && unlayered.anisotropy == 0.f);
    CHECK(unlayered.translucency == 0.f && unlayered.subsurface == 0.f);
    CHECK_NEAR(loaded.render.exposure, 1.3, 1e-6);
    CHECK_NEAR(loaded.render.bloomStrength, 0.2, 1e-6);
    CHECK_NEAR(loaded.render.fogDensity, 0.01, 1e-6);
    CHECK_VEC(loaded.render.sky, test::v3(0.1f, 0.2f, 0.3f), 1e-6f);
    CHECK(loaded.render.gi && loaded.render.reflections);
    CHECK_NEAR(loaded.render.giSpacing, 2.5, 1e-6);
    CHECK_NEAR(loaded.render.giIntensity, 1.4, 1e-6);
    CHECK(loaded.render.volumetricFog && loaded.render.tonemapper == 1);
    CHECK_NEAR(loaded.render.shadowSoftness, 1.7, 1e-6);
    CHECK_NEAR(loaded.render.fogLight, 2.5, 1e-6);
    CHECK_NEAR(loaded.render.fogHeightFalloff, 0.15, 1e-6);
    CHECK_NEAR(loaded.render.vignette, 0.4, 1e-6);
    CHECK(loaded.render.autoExposure);
    CHECK_NEAR(loaded.render.exposureMin, -2.0, 1e-6);
    CHECK_NEAR(loaded.render.exposureMax, 3.0, 1e-6);
    CHECK_NEAR(loaded.render.adaptationSpeed, 2.5, 1e-6);
    CHECK_NEAR(loaded.render.localToneMapping, 0.6, 1e-6);
    CHECK_NEAR(loaded.render.lensDirt, 0.3, 1e-6);
    CHECK(loaded.render.ssgi && loaded.render.contactShadows);
    CHECK_NEAR(loaded.render.ssgiIntensity, 0.7, 1e-6);
    CHECK(sameRenderSettings(loaded.render, s.render));
    Scene off;
    CHECK(!off.render.volumetricFog && off.render.tonemapper == 0 && off.render.shadowSoftness == 1.f);
    CHECK(!off.render.gi && !off.render.reflections);   // (off unless asked for)
    CHECK(!off.render.autoExposure && off.render.localToneMapping == 0.f);
    CHECK(!off.render.ssgi && !off.render.contactShadows);
    CHECK(!sameRenderSettings(off.render, s.render));
    // A plain one keeps the defaults (and doesn't write them)
    MeshRenderer plain;
    CHECK_NEAR(plain.metallic, 0.0, 0.0);
    CHECK_NEAR(plain.roughness, 0.5, 0.0);
}

// The procedural brick normal map: flat (pointing out of the surface) in the middle of a brick, leaning at its edges
TEST(builtin_brick_normal_map) {
    int id = textures().find("Bricks Normal");
    REQUIRE(id >= 0);
    const Texture& t = textures().get(id);
    auto at = [&](int x, int y) { return &t.rgba[((size_t)y * t.width + (size_t)x) * 4]; };
    const uint8_t* mid = at(32, 16);   // a brick's middle (bricks are 64 x 32 texels)
    CHECK(std::abs(mid[0] - 128) <= 2 && std::abs(mid[1] - 128) <= 2 && mid[2] >= 250);
    int leaning = 0;
    for (uint32_t x = 0; x < t.width; ++x) leaning += std::abs(at((int)x, 16)[0] - 128) > 40;
    CHECK(leaning > 4);   // the bevels at the brick ends
}

TEST(spot_lights_and_sky_saved) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    Light l;
    l.type = Light::Type::Spot;
    l.spotAngle = 22.f;
    l.spotSoftness = 0.5f;
    l.range = 12.f;
    l.castShadows = false;
    s.create("Spot").light = l;
    s.render.zenith = {0.1f, 0.2f, 0.9f};
    s.render.ground = {0.3f, 0.2f, 0.1f};
    std::string error;
    auto path = test::tempPath("spot.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, textures(), error));
    const Light& got = *loaded.findByName("Spot")->light;
    CHECK(got.type == Light::Type::Spot);
    CHECK_NEAR(got.spotAngle, 22.0, 1e-6);
    CHECK_NEAR(got.spotSoftness, 0.5, 1e-6);
    CHECK_NEAR(got.range, 12.0, 1e-6);
    CHECK(!got.castShadows);
    CHECK_VEC(loaded.render.zenith, test::v3(0.1f, 0.2f, 0.9f), 1e-6f);
    CHECK_VEC(loaded.render.ground, test::v3(0.3f, 0.2f, 0.1f), 1e-6f);
}

// A lamp's size (its bulb's radius: soft shadows) and a tube's length, kept by a save; older scenes get the defaults
TEST(light_sizes_saved) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    Light point, spot;
    point.size = 0.3f;
    point.length = 2.5f;   // (a tube)
    spot.type = Light::Type::Spot;
    spot.size = 0.f;
    s.create("Bulb").light = point;
    s.create("Spot").light = spot;
    std::string error;
    auto path = test::tempPath("sizes.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, textures(), error));
    CHECK_NEAR(loaded.findByName("Bulb")->light->size, 0.3, 1e-6);
    CHECK_NEAR(loaded.findByName("Bulb")->light->length, 2.5, 1e-6);
    CHECK_NEAR(loaded.findByName("Spot")->light->length, 0.0, 1e-6);   // (bulbs and spots: none)
    CHECK_NEAR(loaded.findByName("Spot")->light->size, 0.0, 1e-6);
    CHECK_NEAR(Light{}.size, 0.05, 1e-6);   // (old scenes: a small bulb)
    Scene after = loaded;   // (undo sees the edit)
    after.findByName("Bulb")->light->size = 0.1f;
    CHECK(!diffScenes(loaded, after).empty());
}

#ifdef VKWORLD_TEST_ASSETS
// glTF alpha modes come through import and a save / load
TEST(gltf_alpha_modes) {
    MeshLibrary meshes = buildMeshLibrary();
    Prefab p;
    std::string error, warning;
    REQUIRE(importGltfPrefab(std::string(VKWORLD_TEST_ASSETS) + "/alpha_modes.gltf", meshes, textures(), p, error, warning));
    const MeshRenderer* glass = nullptr;
    const MeshRenderer* leaf = nullptr;
    for (const Entity& e : p.entities) {
        if (e.name == "Glass") glass = &*e.meshRenderer;
        if (e.name == "Leaf") leaf = &*e.meshRenderer;
    }
    REQUIRE(glass && leaf);
    CHECK(glass->alphaMode == MeshRenderer::AlphaMode::Blend);
    CHECK_NEAR(glass->opacity, 0.4, 1e-6);
    CHECK(leaf->alphaMode == MeshRenderer::AlphaMode::Mask);
    CHECK_NEAR(leaf->alphaCutoff, 0.3, 1e-6);

    Scene s;
    for (Entity& e : s.instantiate(p)) s.add(std::move(e));
    auto path = test::tempPath("alpha.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    Scene loaded;
    MeshLibrary fresh = buildMeshLibrary();
    REQUIRE(loadScene(path, loaded, fresh, textures(), error));
    CHECK(loaded.findByName("Glass")->meshRenderer->alphaMode == MeshRenderer::AlphaMode::Blend);
    CHECK_NEAR(loaded.findByName("Glass")->meshRenderer->opacity, 0.4, 1e-6);
    CHECK_NEAR(loaded.findByName("Leaf")->meshRenderer->alphaCutoff, 0.3, 1e-6);
}
#endif

TEST(materials_light_inside_undone) {
    // Translucency and subsurface edits are changes the editor's undo sees
    Scene before;
    before.create("Leaf").meshRenderer = MeshRenderer{};
    Scene after = before;
    after.findByName("Leaf")->meshRenderer->translucency = 0.7f;
    CHECK(!diffScenes(before, after).empty());
    after = before;
    after.findByName("Leaf")->meshRenderer->subsurfaceColor.y = 0.9f;
    CHECK(!diffScenes(before, after).empty());
    CHECK(diffScenes(before, before).empty());
}

TEST(materials_with_layers_drawn_layered) {
    // Plain, metal, glowing, see-through: the lean lit pass; any layer: the layered one
    DrawItem plain;
    plain.metallic = 1.f;
    plain.emissive = {1.f, 1.f, 1.f};
    plain.alphaMode = 2;
    CHECK(!hasMaterialLayers(plain));
    for (int k = 0; k < 5; ++k) {
        DrawItem d;
        (k == 0 ? d.clearcoat : k == 1 ? d.sheen : k == 2 ? d.anisotropy : k == 3 ? d.translucency : d.subsurface) = 0.5f;
        CHECK(hasMaterialLayers(d));
    }
    DrawItem barely;
    barely.anisotropy = 0.005f;   // (too little to show)
    CHECK(!hasMaterialLayers(barely));
}

TEST(shadow_casters_that_tint) {
    // Glass under 75% opaque and thin translucent things tint the sun's light; solid, nearly opaque, water: not
    DrawItem glass;
    glass.alphaMode = 2;
    glass.opacity = 0.4f;
    CHECK(tintsShadow(glass));
    DrawItem smoked = glass;
    smoked.opacity = 0.9f;
    CHECK(!tintsShadow(smoked));
    DrawItem curtain;
    curtain.translucency = 0.7f;
    CHECK(tintsShadow(curtain));
    DrawItem leaf;
    leaf.translucency = 0.3f;   // (a little: still blocks)
    CHECK(!tintsShadow(leaf));
    CHECK(!tintsShadow(DrawItem{}));
    DrawItem water = glass;
    water.water = true;
    CHECK(!tintsShadow(water));
}
