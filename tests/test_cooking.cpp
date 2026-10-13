// assets/bc_codec and assets/cooking: block compression round trips, cooked textures and models read back as the
// importers make them, loading preferring cooked files, and an export cooking its assets
#include "test.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "app/export.h"
#include "assets/bc_codec.h"
#include "assets/cooking.h"
#include "assets/gltf.h"
#include "core/png.h"
#include "platform/platform.h"
#include "render/texture_streaming.h"

namespace fs = std::filesystem;

namespace {

// A w x h test image: gradients, a few hard edges, alpha ramping down across when `alpha`
std::vector<uint8_t> testImage(uint32_t w, uint32_t h, bool alpha) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &px[((size_t)y * w + x) * 4];
            // (like a photo's: colours changing together, gently, with an edge here and there)
            uint32_t t = x * 2 + y;
            bool edge = (x / 8 + y / 8) % 2 != 0;
            p[0] = (uint8_t)std::min(40 + t * 2 + (edge ? 60 : 0), 255u);
            p[1] = (uint8_t)std::min(30 + t * 3 / 2 + (edge ? 50 : 0), 255u);
            p[2] = (uint8_t)std::min(20 + t + (edge ? 30 : 0), 255u);
            p[3] = alpha ? (uint8_t)(255 - x * 4) : 255;
        }
    return px;
}

// Mean absolute difference per channel
double meanError(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int channel) {
    double sum = 0;
    size_t n = 0;
    for (size_t i = (size_t)channel; i < a.size() && i < b.size(); i += 4, ++n) sum += std::fabs((double)a[i] - b[i]);
    return n ? sum / (double)n : 1e9;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(bc_sizes) {
    CHECK(bc::blockBytes(bc::Format::BC1) == 8);
    CHECK(bc::blockBytes(bc::Format::BC3) == 16);
    CHECK(bc::blockBytes(bc::Format::BC7) == 16);
    CHECK(bc::imageBytes(bc::Format::BC1, 5, 4) == 16);        // two blocks across, one down
    CHECK(bc::imageBytes(bc::Format::BC7, 1, 1) == 16);        // a partial block is a whole one
    CHECK(bc::imageBytes(bc::Format::RGBA8, 3, 2) == 24);
    // Streaming's budget counts blocks for compressed textures
    CHECK(streaming::chainBytes(8, 8, 0, 4, 16) == (4 + 1 + 1 + 1) * 16);
    CHECK(streaming::chainBytes(8, 8, 0, 4) == (64 + 16 + 4 + 1) * 4);
}

TEST(bc_round_trips) {
    const uint32_t w = 37, h = 23;   // (not multiples of 4: edge blocks)
    std::vector<uint8_t> opaque = testImage(w, h, false), clear = testImage(w, h, true);
    std::vector<uint8_t> bc1 = bc::decode(bc::Format::BC1, bc::encode(bc::Format::BC1, opaque.data(), w, h).data(), w, h);
    std::vector<uint8_t> bc3 = bc::decode(bc::Format::BC3, bc::encode(bc::Format::BC3, clear.data(), w, h).data(), w, h);
    std::vector<uint8_t> bc7 = bc::decode(bc::Format::BC7, bc::encode(bc::Format::BC7, clear.data(), w, h).data(), w, h);
    REQUIRE(bc1.size() == opaque.size() && bc3.size() == clear.size() && bc7.size() == clear.size());
    for (int c = 0; c < 3; ++c) {
        CHECK(meanError(opaque, bc1, c) < 3.0);
        CHECK(meanError(clear, bc3, c) < 3.0);
        CHECK(meanError(clear, bc7, c) < 1.5);
    }
    CHECK(meanError(opaque, bc1, 3) == 0.0);   // BC1 here: always opaque
    CHECK(meanError(clear, bc3, 3) < 1.0);
    CHECK(meanError(clear, bc7, 3) < 2.0);
    // BC7 is the better of the two on colour
    double e1 = 0, e7 = 0;
    std::vector<uint8_t> bc7o = bc::decode(bc::Format::BC7, bc::encode(bc::Format::BC7, opaque.data(), w, h).data(), w, h);
    for (int c = 0; c < 3; ++c) {
        e1 += meanError(opaque, bc1, c);
        e7 += meanError(opaque, bc7o, c);
    }
    CHECK(e7 < e1);
}

TEST(bc_flat_blocks_stay_exact) {
    uint8_t px[64], out[64], block[16];
    for (int i = 0; i < 16; ++i) {
        px[i * 4] = 10;
        px[i * 4 + 1] = 120;
        px[i * 4 + 2] = 250;
        px[i * 4 + 3] = 77;
    }
    bc::encodeBC7(px, block);
    bc::decodeBC7(block, out);
    for (int i = 0; i < 64; ++i) CHECK(std::abs((int)out[i] - (int)px[i]) <= 1);
    bc::encodeBC3(px, block);
    bc::decodeBC3(block, out);
    for (int i = 0; i < 16; ++i) CHECK(out[i * 4 + 3] == 77);
    // White and black, two colours: both come back exactly
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 4; ++c) px[i * 4 + c] = c == 3 ? 255 : (i % 3 ? 255 : 0);
    bc::encodeBC1(px, block);
    bc::decodeBC1(block, out);
    for (int i = 0; i < 64; ++i) CHECK(out[i] == px[i]);
}

TEST(cooked_texture_round_trip) {
    Texture t;
    t.width = 20;
    t.height = 12;
    t.rgba = testImage(t.width, t.height, true);
    t.nearest = true;
    CHECK(cook::formatFor(t, cook::Compression::Small) == bc::Format::BC3);   // (it has alpha)
    CHECK(cook::formatFor(t, cook::Compression::Quality) == bc::Format::BC7);
    CHECK(cook::formatFor(t, cook::Compression::None) == bc::Format::RGBA8);
    Texture opaque = t;
    opaque.rgba = testImage(t.width, t.height, false);
    CHECK(cook::formatFor(opaque, cook::Compression::Small) == bc::Format::BC1);

    Texture back;
    std::string error;
    REQUIRE(cook::readTexture(cook::cookTexture(t, cook::Compression::Quality), back, error));
    CHECK(back.width == 20 && back.height == 12);
    CHECK(back.format == bc::Format::BC7);
    CHECK(back.nearest && back.mipmaps);
    CHECK(back.cooked() && back.rgba.empty());
    REQUIRE(back.levels.size() == streaming::mipCount(20, 12));   // 20x12 down to 1x1: 5 levels
    for (size_t l = 0; l < back.levels.size(); ++l)
        CHECK(back.levels[l].size() == bc::imageBytes(bc::Format::BC7, std::max(20u >> l, 1u), std::max(12u >> l, 1u)));
    // Level 1 is the image filtered down (as streaming makes it), compressed
    uint32_t lw = 0, lh = 0;
    std::vector<uint8_t> half = streaming::downsample(t.rgba.data(), 20, 12, 1, lw, lh);
    std::vector<uint8_t> got = bc::decode(bc::Format::BC7, back.levels[1].data(), lw, lh);
    CHECK(meanError(half, got, 0) < 2.0);
    // Without mipmaps: one level; uncompressed: the pixels as they are
    t.mipmaps = false;
    REQUIRE(cook::readTexture(cook::cookTexture(t, cook::Compression::None), back, error));
    REQUIRE(back.levels.size() == 1);
    CHECK(back.format == bc::Format::RGBA8 && back.levels[0] == t.rgba && !back.mipmaps);
    // Damaged: refused
    std::string bytes = cook::cookTexture(t, cook::Compression::Quality);
    CHECK(!cook::readTexture(bytes.substr(0, bytes.size() - 5), back, error));
    CHECK(!cook::readTexture("not a texture", back, error));
}

TEST(load_texture_prefers_cooked) {
    fs::path dir = test::tempPath("cooked_textures");
    fs::create_directories(dir);
    std::vector<uint8_t> px = testImage(8, 8, false);
    std::string png = (dir / "wall.png").u8string();
    REQUIRE(writePng(png, 8, 8, px.data()));
    // A cooked copy next to it: that's what loads
    std::string cooked, error;
    REQUIRE(cook::cookImageFile(png, cook::Compression::Small, cooked, error));
    std::ofstream(cook::texturePath(png), std::ios::binary) << cooked;
    TextureLibrary lib = buildTextureLibrary();
    int id = loadTexture(lib, png, error);
    REQUIRE(id >= 0);
    CHECK(lib.get(id).cooked());
    CHECK(lib.get(id).format == bc::Format::BC1);
    CHECK(lib.get(id).width == 8);
    // ... even without the source (an export's)
    fs::remove(png);
    TextureLibrary again = buildTextureLibrary();
    id = loadTexture(again, png, error);
    REQUIRE(id >= 0);
    CHECK(again.get(id).cooked());
    fs::remove_all(dir);
}

#ifdef VKWORLD_SAMPLE_GAME
TEST(cooked_model_matches_the_import) {
    fs::path dir = test::tempPath("cooked_model");
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path gltf = dir / "tentacle.gltf";
    fs::copy_file(fs::u8path(VKWORLD_SAMPLE_GAME) / "models" / "tentacle.gltf", gltf);
    std::string cooked, error;
    REQUIRE(cook::cookModel(gltf.u8string(), cook::Compression::Quality, cooked, error));
    MeshLibrary imported = buildMeshLibrary();
    TextureLibrary importedImages = buildTextureLibrary();
    REQUIRE(importGltfMeshes(imported, gltf.u8string(), error));
    REQUIRE(importGltfImages(importedImages, gltf.u8string(), error));
    REQUIRE(importGltfAnimations(imported, gltf.u8string(), error));

    // The cooked file next to it, the glTF gone: the importers read that instead
    std::ofstream(cook::modelPath(gltf.u8string()), std::ios::binary) << cooked;
    fs::remove(gltf);
    MeshLibrary meshes = buildMeshLibrary();
    TextureLibrary images = buildTextureLibrary();
    REQUIRE(importGltfMeshes(meshes, gltf.u8string(), error));
    REQUIRE(importGltfImages(images, gltf.u8string(), error));
    REQUIRE(importGltfAnimations(meshes, gltf.u8string(), error));
    REQUIRE(meshes.count() == imported.count());
    CHECK(meshes.count() > (int)MeshKind::Count);
    for (int i = (int)MeshKind::Count; i < meshes.count(); ++i) {
        const Mesh &a = imported.get(i), &b = meshes.get(i);
        CHECK(a.name == b.name);
        CHECK(a.source == b.source);
        CHECK(a.indexCount == b.indexCount && a.skinned == b.skinned && a.lods.size() == b.lods.size());
        CHECK_NEAR(a.boundsMax.y, b.boundsMax.y, 1e-6f);
        for (uint32_t k = 0; k < a.indexCount; k += 7) {   // the same triangles, vertex for vertex
            const Vertex& va = imported.vertices[(size_t)a.vertexOffset + imported.indices[a.firstIndex + k]];
            const Vertex& vb = meshes.vertices[(size_t)b.vertexOffset + meshes.indices[b.firstIndex + k]];
            CHECK_NEAR(va.pos.x, vb.pos.x, 1e-6f);
            CHECK_NEAR(va.normal.z, vb.normal.z, 1e-6f);
            CHECK_NEAR(va.v, vb.v, 1e-6f);
        }
        if (a.skinned) {
            const SkinVertex& sa = imported.skin[(size_t)a.vertexOffset];
            const SkinVertex& sb = meshes.skin[(size_t)b.vertexOffset];
            CHECK(sa.joints[0] == sb.joints[0]);
            CHECK_NEAR(sa.weights[0], sb.weights[0], 1e-6f);
        }
    }
    CHECK(meshes.vertices.size() == meshes.skin.size());
    REQUIRE(meshes.animations.size() == imported.animations.size());
    for (size_t i = 0; i < meshes.animations.size(); ++i) {
        CHECK(meshes.animations[i].name == imported.animations[i].name);
        CHECK(meshes.animations[i].source == imported.animations[i].source);
        CHECK(keyCount(meshes.animations[i]) == keyCount(imported.animations[i]));
    }
    CHECK(images.count() == importedImages.count());
    // Twice: nothing more
    int before = meshes.count();
    REQUIRE(importGltfMeshes(meshes, gltf.u8string(), error));
    CHECK(meshes.count() == before);
    // Damaged: refused
    std::ofstream(cook::modelPath(gltf.u8string()), std::ios::binary) << cooked.substr(0, cooked.size() / 2);
    MeshLibrary broken = buildMeshLibrary();
    CHECK(!importGltfMeshes(broken, gltf.u8string(), error));
    CHECK(broken.count() == (int)MeshKind::Count);
    fs::remove_all(dir);
}

// A cooked export: the model cooked in place of its glTF, the icon (not a texture) as it is
TEST(export_cooks_assets) {
    fs::path sample = fs::u8path(VKWORLD_SAMPLE_GAME), out = test::tempPath("exported_cooked");
    fs::remove_all(out);
    ExportOptions options;
    options.cook = true;
    ExportResult result;
    std::string error;
    REQUIRE(exportGame(sample, out, platform::executableDir(), result, error, options));
    fs::path dir = result.executable.parent_path();
    CHECK(fs::is_regular_file(dir / "models" / "tentacle.gltf.vkmodel"));
    CHECK(!fs::exists(dir / "models" / "tentacle.gltf"));
    CHECK(fs::is_regular_file(dir / "icon.png"));
    CHECK(!fs::exists(dir / "icon.png.vktex"));
    CHECK(result.cooked >= 1);
    CHECK(result.warnings.empty());
    fs::remove_all(out);
}
#endif

// Which images an export cooks: those scenes use as textures; one also named elsewhere keeps its source. (And
// files called building-something are exported: only build folders stay behind.)
TEST(export_cooks_texture_images) {
    fs::path project = test::tempPath("cook_project"), out = test::tempPath("cook_project_out");
    fs::remove_all(project);
    fs::remove_all(out);
    fs::create_directories(project / "textures");
    std::vector<uint8_t> px = testImage(16, 16, false);
    REQUIRE(writePng((project / "textures" / "wall.png").u8string(), 16, 16, px.data()));
    REQUIRE(writePng((project / "textures" / "sky.png").u8string(), 16, 16, px.data()));
    REQUIRE(writePng((project / "textures" / "loose.png").u8string(), 16, 16, px.data()));
    fs::create_directories(project / "buildings");
    fs::create_directories(project / "build");
    REQUIRE(writePng((project / "buildings" / "building-a.png").u8string(), 16, 16, px.data()));
    REQUIRE(writePng((project / "build" / "left-over.png").u8string(), 16, 16, px.data()));
    std::ofstream(project / "project.json") << R"({"format": "vkworld-project", "version": 1, "name": "Cook Test", "startScene": "level.vkw"})";
    std::ofstream(project / "level.vkw") << R"({"format": "vkworld-scene", "version": 40, "entities": [],
        "textures": [{"name": "Wall", "source": "textures/wall.png"}, {"name": "Sky", "source": "textures/sky.png"}],
        "settings": {"render": {"environmentMap": "textures/sky.png"}}})";
    ExportOptions options;
    options.cook = true;
    options.textureCompression = "bc1";
    options.layout = ExportOptions::Layout::Folder;
    ExportResult result;
    std::string error;
    REQUIRE(exportGame(project, out, platform::executableDir(), result, error, options));
    fs::path dir = result.executable.parent_path();
    CHECK(fs::is_regular_file(dir / "textures" / "wall.png.vktex"));
    CHECK(!fs::exists(dir / "textures" / "wall.png"));            // only a texture: cooked only
    CHECK(fs::is_regular_file(dir / "textures" / "sky.png.vktex"));
    CHECK(fs::is_regular_file(dir / "textures" / "sky.png"));     // named in the settings too: kept
    CHECK(fs::is_regular_file(dir / "textures" / "loose.png"));   // not a texture of any scene: as it is
    CHECK(!fs::exists(dir / "textures" / "loose.png.vktex"));
    CHECK(fs::is_regular_file(dir / "buildings" / "building-a.png"));   // (named "build...", not a build folder)
    CHECK(!fs::exists(dir / "build"));
    Texture t;
    REQUIRE(cook::readTexture(readFile(dir / "textures" / "wall.png.vktex"), t, error));
    CHECK(t.format == bc::Format::BC1);
    fs::remove_all(project);
    fs::remove_all(out);
}
