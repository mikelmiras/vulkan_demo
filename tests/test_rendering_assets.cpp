// Textures, glTF import, materials and lights (the data side; the GPU side is checked by screenshots)
#include "test.h"

#include "assets/gltf.h"
#include "core/json.h"
#include "core/png.h"
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
