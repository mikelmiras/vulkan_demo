// scene/terrain: heights, brushes, encoding, meshes, collision and navigation
#include "test.h"

#include <cstring>

#include "core/json.h"
#include "core/png.h"
#include "game/navigation.h"
#include "physics/physics.h"
#include "scene/scene_diff.h"
#include "scene/serialization.h"
#include "scene/terrain.h"

namespace {

Terrain flat(int res = 64, float size = 32.f) {
    Terrain t;
    terrain::init(t, res, size);
    return t;
}

int sumWeights(const Terrain& t, size_t i) {
    int s = 0;
    for (int k = 0; k < 4; ++k) s += t.weights[i * 4 + (size_t)k];
    return s;
}

}  // namespace

TEST(terrain_heights_and_brushes) {
    Terrain t = flat();
    CHECK(t.heights.size() == 64u * 64u && t.weights.size() == 64u * 64u * 4u);
    CHECK_NEAR(terrain::heightAt(t, 3.f, -5.f), 0.f, 1e-6);
    uint64_t rev = t.revision;
    terrain::BrushSettings b;
    b.radius = 4.f;
    b.strength = 1.f;
    for (int i = 0; i < 30; ++i) CHECK(terrain::applyBrush(t, b, {0, 0, 0}, 1.f / 30.f));
    CHECK(t.revision != rev);
    float centre = terrain::heightAt(t, 0.f, 0.f);
    CHECK(centre > 3.f && centre < 4.01f);   // 4 m/s at full strength for 1 s, full at the centre
    CHECK(terrain::heightAt(t, 2.f, 0.f) < centre);
    CHECK_NEAR(terrain::heightAt(t, 6.f, 0.f), 0.f, 1e-6);   // outside the brush
    // Smooth pulls the peak down; flatten brings it to a height; lower digs
    b.mode = terrain::Brush::Smooth;
    terrain::applyBrush(t, b, {0, 0, 0}, 0.1f);
    CHECK(terrain::heightAt(t, 0.f, 0.f) < centre);
    b.mode = terrain::Brush::Flatten;
    b.flattenHeight = 1.f;
    for (int i = 0; i < 60; ++i) terrain::applyBrush(t, b, {0, 0, 0}, 0.05f);
    CHECK_NEAR(terrain::heightAt(t, 0.f, 0.f), 1.f, 0.01);
    b.mode = terrain::Brush::Lower;
    terrain::applyBrush(t, b, {0, 0, 0}, 0.5f);
    CHECK(terrain::heightAt(t, 0.f, 0.f) < 0.f);
    // Paint: the layer grows where painted, the weights still add up to 255
    b.mode = terrain::Brush::Paint;
    b.layer = 2;
    for (int i = 0; i < 20; ++i) terrain::applyBrush(t, b, {8, 0, 8}, 0.05f);
    int ix = (int)std::lround((8.f + 16.f) / terrain::cellSize(t));   // the sample nearest (8, 8)
    size_t mid = (size_t)ix * 64 + (size_t)ix;
    CHECK(t.weights[mid * 4 + 2] > 200);
    CHECK(sumWeights(t, mid) == 255);
    CHECK(sumWeights(t, 0) == 255 && t.weights[0] == 255);
    CHECK(!terrain::applyBrush(t, b, {100, 0, 100}, 0.1f));   // off the terrain
}

TEST(terrain_raycast_and_resize) {
    Terrain t = flat(128, 64.f);
    terrain::addNoise(t, 5.f, 10.f, 7);
    float hit = 0.f;
    REQUIRE(terrain::raycast(t, {3.f, 50.f, -4.f}, {0, -1, 0}, 100.f, hit));
    CHECK_NEAR(50.f - hit, terrain::heightAt(t, 3.f, -4.f), 0.01);
    // A slanted ray lands on the surface
    REQUIRE(terrain::raycast(t, {-20.f, 30.f, -20.f}, normalize(Vec3{1, -1, 0.5f}), 200.f, hit));
    Vec3 at = Vec3{-20.f, 30.f, -20.f} + normalize(Vec3{1, -1, 0.5f}) * hit;
    CHECK_NEAR(at.y, terrain::heightAt(t, at.x, at.z), 0.01);
    CHECK(!terrain::raycast(t, {0.f, 50.f, 0.f}, {0, 1, 0}, 100.f, hit));   // up: misses
    CHECK(!terrain::raycast(t, {100.f, 50.f, 0.f}, {0, -1, 0}, 100.f, hit));   // beside it
    // Resampled to a coarser grid: about the same shape
    Terrain coarse = t;
    terrain::resize(coarse, 64);
    CHECK(coarse.heights.size() == 64u * 64u);
    CHECK_NEAR(terrain::heightAt(coarse, 3.f, -4.f), terrain::heightAt(t, 3.f, -4.f), 0.6);
}

TEST(terrain_encoding_and_scene_files) {
    Terrain t = flat(64, 40.f);
    terrain::addNoise(t, 12.f, 8.f, 3);
    terrain::BrushSettings b;
    b.mode = terrain::Brush::Paint;
    b.layer = 1;
    terrain::applyBrush(t, b, {0, 0, 0}, 1.f);
    float lo = 0.f, hi = 0.f;
    std::string h = terrain::encodeHeights(t, lo, hi), w = terrain::encodeWeights(t);
    Terrain back;
    back.resolution = 64;
    REQUIRE(terrain::decodeHeights(back, h, lo, hi));
    REQUIRE(terrain::decodeWeights(back, w));
    for (size_t i = 0; i < t.heights.size(); ++i) CHECK_NEAR(back.heights[i], t.heights[i], 0.001);
    CHECK(back.weights == t.weights);
    CHECK(!terrain::decodeHeights(back, "not base64!", lo, hi));
    Terrain flatOne = flat(128);
    std::string flatHeights = terrain::encodeHeights(flatOne, lo, hi);
    CHECK(flatHeights.size() < 2000);   // (compressed: a flat terrain is nearly nothing)

    // In a scene file, with its layers (a texture by name)
    Scene s;
    Entity& e = s.create("Ground");
    e.terrain = t;
    e.terrain->layers[1].texture = 1;   // the built-in Bricks
    e.terrain->layers[2].color = {0.2f, 0.4f, 0.6f};
    e.terrain->layers[3].tiling = 2.f;
    e.terrain->layers[1].normalTexture = 3;   // the built-in Bricks Normal
    e.terrain->layers[1].normalStrength = 0.5f;
    MeshLibrary lib = buildMeshLibrary();
    TextureLibrary tex = buildTextureLibrary();
    std::string error, path = test::tempPath("terrain.vkw").u8string();
    REQUIRE(saveScene(path, s, lib, tex, error));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, lib, tex, error));
    const Entity* g = loaded.findByName("Ground");
    REQUIRE(g && g->terrain);
    CHECK(g->terrain->resolution == 64 && g->terrain->size == 40.f);
    CHECK(g->terrain->layers[1].texture == 1);
    CHECK_NEAR(g->terrain->layers[2].color.y, 0.4f, 1e-6);
    CHECK(g->terrain->layers[3].tiling == 2.f);
    CHECK(g->terrain->layers[1].normalTexture == 3 && g->terrain->layers[1].normalStrength == 0.5f);
    CHECK(g->terrain->layers[0].normalTexture == -1 && g->terrain->layers[0].normalStrength == 1.f);
    Entity edited = *g;   // (undo sees the strength change)
    edited.terrain->layers[1].normalStrength = 1.f;
    CHECK(sameEntity(*g, *g) && !sameEntity(*g, edited));
    CHECK_NEAR(terrain::heightAt(*g->terrain, 5.f, 5.f), terrain::heightAt(t, 5.f, 5.f), 0.002);
    CHECK(g->terrain->revision != 0);
}

// Meshes: made once, rewritten in place as it changes; a copy gets its own
TEST(terrain_meshes) {
    MeshLibrary lib = buildMeshLibrary();
    Scene s;
    Entity& e = s.create("T");
    e.terrain = flat(64);   // 63 cells: chunks of 32 and 31
    int before = lib.count();
    terrain::syncMeshes(s, lib);
    Terrain& t = *s.findByName("T")->terrain;
    REQUIRE(t.meshes.size() == 4);
    CHECK(lib.count() == before + 4);
    const Mesh& first = lib.get(t.meshes[0]);
    // Every chunk: its cells (two triangles each), skirts on four sides (both faces), two coarser levels; all over
    // the same vertices
    CHECK(first.indexCount == 32u * 32u * 6u + 4u * 32u * 12u);
    CHECK(lib.get(t.meshes[3]).indexCount == 31u * 31u * 6u + 4u * 31u * 12u);
    for (int id : t.meshes) {
        const Mesh& m = lib.get(id);
        CHECK(m.vertexOffset == first.vertexOffset);
        REQUIRE(m.lods.size() == 2);
        CHECK(m.lods[0].indexCount < m.indexCount / 3);
        CHECK(m.lods[1].indexCount < m.lods[0].indexCount / 2);
        uint32_t highest = 0;
        for (uint32_t k = 0; k < m.indexCount; ++k) highest = std::max(highest, lib.indices[m.firstIndex + k]);
        CHECK((size_t)m.vertexOffset + highest < lib.vertices.size());
    }
    uint32_t version = lib.version;
    terrain::syncMeshes(s, lib);   // nothing changed
    CHECK(lib.version == version);
    terrain::BrushSettings b;
    terrain::applyBrush(t, b, {0, 0, 0}, 0.5f);
    terrain::syncMeshes(s, lib);
    CHECK(lib.count() == before + 4);   // the same meshes, rewritten
    CHECK(lib.version != version && lib.rewriteSerial == 1);
    CHECK(lib.get(t.meshes[3]).boundsMax.y > 0.5f);   // (the centre is the corner of the four chunks)
    size_t centre = (size_t)first.vertexOffset + 32 * 64 + 32;
    CHECK(lib.vertices[centre].pos.y > 0.5f);
    CHECK_NEAR(lib.skin[centre].weights[0], 1.f, 1e-6);
    // A hole: its point flagged in the vertices, its chunk drawn at full detail only
    b.mode = terrain::Brush::Holes;
    b.radius = 2.f;
    terrain::applyBrush(t, b, {-10.f, 0.f, -10.f}, 0.1f);
    terrain::syncMeshes(s, lib);
    CHECK(lib.get(t.meshes[0]).lods.empty());
    CHECK(lib.get(t.meshes[3]).lods.size() == 2);
    float cell = terrain::cellSize(t);
    size_t holePoint = (size_t)std::lround((-10.f + 16.f) / cell) * 64 + (size_t)std::lround((-10.f + 16.f) / cell);
    CHECK(lib.skin[(size_t)first.vertexOffset + holePoint].joints[0] == 1);
    Entity copy = *s.findByName("T");   // a duplicate shares the mesh ids at first
    copy.id = 0;
    Entity& dup = s.create("T2");
    EntityId dupId = dup.id;
    dup = copy;
    dup.id = dupId;
    dup.name = "T2";
    terrain::syncMeshes(s, lib);
    CHECK(s.findByName("T2")->terrain->meshes[0] != s.findByName("T")->terrain->meshes[0]);
}

// Holes: cut with the brush, filled back; rays go through them, and so do things falling
TEST(terrain_holes) {
    Terrain t = flat(64, 32.f);
    CHECK(!terrain::holeAt(t, 0.f, 0.f));
    terrain::BrushSettings b;
    b.mode = terrain::Brush::Holes;
    b.radius = 3.f;
    REQUIRE(terrain::applyBrush(t, b, {0, 0, 0}, 0.1f));
    CHECK(terrain::holeAt(t, 0.f, 0.f));
    CHECK(terrain::holeAt(t, 1.f, 0.5f));
    CHECK(!terrain::holeAt(t, 6.f, 0.f));
    float hit = 0.f;
    CHECK(!terrain::raycast(t, {0.f, 5.f, 0.f}, {0, -1, 0}, 20.f, hit));   // straight through
    REQUIRE(terrain::raycast(t, {6.f, 5.f, 0.f}, {0, -1, 0}, 20.f, hit));
    CHECK_NEAR(hit, 5.0, 0.01);
    // Collision: Jolt's height field has the same hole
    MeshLibrary lib = buildMeshLibrary();
    Scene s;
    s.create("Ground").terrain = t;
    Physics physics;
    physics.step(s, lib, 0.f);
    RaycastHit rh;
    CHECK(!physics.raycast({0.f, 5.f, 0.f}, {0, -1, 0}, 20.f, rh));
    CHECK(physics.raycast({6.f, 5.f, 0.f}, {0, -1, 0}, 20.f, rh));
    // Filled back in
    b.mode = terrain::Brush::Fill;
    b.radius = 5.f;
    terrain::applyBrush(t, b, {0, 0, 0}, 0.1f);
    CHECK(!terrain::holeAt(t, 0.f, 0.f));
    CHECK(terrain::raycast(t, {0.f, 5.f, 0.f}, {0, -1, 0}, 20.f, hit));
}

// Foliage: painted where it grows, scattered the same way every time, standing on the ground, never in holes
TEST(terrain_foliage) {
    Terrain t = flat(64, 32.f);
    terrain::addNoise(t, 3.f, 10.f, 7);
    t.foliage.emplace_back();
    t.foliage[0].density = 4.f;
    terrain::BrushSettings b;
    b.mode = terrain::Brush::Foliage;
    b.layer = 0;
    b.radius = 6.f;
    for (int i = 0; i < 40; ++i) terrain::applyBrush(t, b, {-8.f, 0.f, -8.f}, 0.05f);   // (fully painted in the middle)
    terrain::scatterFoliage(t);
    size_t count = t.plants.size();
    CHECK(count > 100 && count < 4.f * 3.1416f * 36.f * 1.2f);   // up to 4 per m2 over the brush's disc
    for (const Terrain::Plant& p : t.plants) {
        CHECK(length(Vec3{p.pos[0] + 8.f, 0.f, p.pos[2] + 8.f}) < 6.f + terrain::cellSize(t) * 1.5f);
        CHECK_NEAR(p.pos[1], terrain::heightAt(t, p.pos[0], p.pos[2]), 1e-4);
        CHECK(p.scale >= 0.7f - 1e-5f && p.scale <= 1.3f + 1e-5f);
    }
    uint32_t inRanges = 0;
    for (const Terrain::PlantRange& r : t.plantRanges) {
        inRanges += r.count;
        CHECK(r.layer == 0);
        for (uint32_t k = r.first; k < r.first + r.count; ++k) {   // (each range's box holds its plants)
            CHECK(t.plants[k].pos[0] >= r.lo.x - 1e-4f && t.plants[k].pos[0] <= r.hi.x + 1e-4f);
            CHECK(t.plants[k].pos[2] >= r.lo.z - 1e-4f && t.plants[k].pos[2] <= r.hi.z + 1e-4f);
        }
    }
    CHECK(inRanges == count);
    std::vector<Terrain::Plant> first = t.plants;
    terrain::scatterFoliage(t);   // the same again
    REQUIRE(t.plants.size() == first.size());
    CHECK(std::memcmp(t.plants.data(), first.data(), first.size() * sizeof(Terrain::Plant)) == 0);
    // A hole where they grow: none there
    b.mode = terrain::Brush::Holes;
    b.radius = 2.f;
    terrain::applyBrush(t, b, {-8.f, 0.f, -8.f}, 0.1f);
    terrain::scatterFoliage(t);
    CHECK(t.plants.size() < count);
    for (const Terrain::Plant& p : t.plants) CHECK(!terrain::holeAt(t, p.pos[0], p.pos[2]));
    // Erasing thins them out
    b.mode = terrain::Brush::Foliage;
    b.erase = true;
    b.radius = 10.f;
    for (int i = 0; i < 60; ++i) terrain::applyBrush(t, b, {-8.f, 0.f, -8.f}, 0.05f);
    terrain::scatterFoliage(t);
    CHECK(t.plants.empty());
    // The built-in grass: made once
    MeshLibrary lib = buildMeshLibrary();
    int grass = terrain::grassBladeMesh(lib);
    CHECK(grass >= 0 && terrain::grassBladeMesh(lib) == grass);
    CHECK(lib.get(grass).boundsMax.y > 0.4f);
}

TEST(terrain_heightmap_and_saving) {
    // A 16 x 8 image: black on the left, white on the right
    std::vector<uint8_t> rgba(16 * 8 * 4);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x)
            for (int c = 0; c < 4; ++c) rgba[((size_t)y * 16 + (size_t)x) * 4 + (size_t)c] = c == 3 ? 255 : (uint8_t)(x * 17);
    std::string file = test::tempPath("heightmap.png").u8string();
    REQUIRE(writePng(file, 16, 8, rgba.data()));
    Terrain t = flat(64, 32.f);
    std::string error;
    REQUIRE(terrain::importHeightmap(t, file, 10.f, error));
    CHECK_NEAR(terrain::heightAt(t, -16.f, 0.f), 0.0, 1e-3);
    CHECK_NEAR(terrain::heightAt(t, 16.f, 0.f), 10.0, 1e-3);
    CHECK_NEAR(terrain::heightAt(t, 0.f, 5.f), 5.0, 0.4);
    CHECK(!terrain::importHeightmap(t, test::tempPath("missing.png").u8string(), 10.f, error));
    // Holes and foliage are saved with the scene
    terrain::BrushSettings b;
    b.mode = terrain::Brush::Holes;
    terrain::applyBrush(t, b, {0, 0, 0}, 0.1f);
    t.foliage.emplace_back();
    t.foliage[0].mesh = "Sphere";
    t.foliage[0].density = 2.5f;
    b.mode = terrain::Brush::Foliage;
    terrain::applyBrush(t, b, {5, 0, 5}, 0.2f);
    MeshLibrary lib = buildMeshLibrary();
    TextureLibrary tex;
    Scene s;
    s.create("Ground").terrain = t;
    Scene back;
    REQUIRE(sceneFromJson(sceneToJson(s, lib, tex), back, lib, tex, error));
    const Terrain& r = *back.findByName("Ground")->terrain;
    CHECK(r.holes == t.holes);
    CHECK(r.foliageDensity == t.foliageDensity);
    REQUIRE(r.foliage.size() == 1);
    CHECK(r.foliage[0].mesh == "Sphere" && r.foliage[0].density == 2.5f);
}

// It collides (a height field) and navigation walks over it
TEST(terrain_collision_and_navigation) {
    MeshLibrary lib = buildMeshLibrary();
    Scene s;
    Entity& g = s.create("Ground");
    g.transform.position[1] = -1.f;
    g.transform.scale[0] = g.transform.scale[2] = 2.f;   // 64 m wide in the world
    g.terrain = flat(64, 32.f);
    terrain::BrushSettings b;
    b.radius = 3.f;
    b.strength = 1.f;
    terrain::applyBrush(*g.terrain, b, {5.f, 0.f, 0.f}, 0.5f);   // a 2 m mound at local (5, 0): world (10, 1, 0)
    Physics physics;
    physics.step(s, lib, 0.f);
    RaycastHit hit;
    REQUIRE(physics.raycast({10.f, 20.f, 0.f}, {0, -1, 0}, 50.f, hit));
    CHECK(hit.entity == s.findByName("Ground")->id);
    CHECK_NEAR(hit.point.y, -1.f + terrain::heightAt(*s.findByName("Ground")->terrain, 5.f, 0.f), 0.02);
    REQUIRE(physics.raycast({-20.f, 20.f, 7.f}, {0, -1, 0}, 50.f, hit));
    CHECK_NEAR(hit.point.y, -1.f, 0.02);
    // A ball dropped on it comes to rest on the ground
    Entity& ball = s.create("Ball");
    ball.transform.position[0] = -10.f;
    ball.transform.position[1] = 3.f;
    ball.collider = Collider{};
    ball.collider->shape = Collider::Shape::Sphere;
    ball.rigidBody = RigidBody{};
    for (int i = 0; i < 180; ++i) physics.step(s, lib, 1.f / 60.f);
    CHECK_NEAR(s.findByName("Ball")->transform.position[1], -1.f + 0.5f, 0.05);
    // Navigation: walkable, with the mound on the way
    std::vector<Vec3> path;
    NavGrid nav;
    REQUIRE(nav.bake(s, lib, s.navigation));
    REQUIRE(nav.findPath({-20.f, 0.f, 0.f}, {20.f, 0.f, 0.f}, path));
    CHECK_NEAR(path.back().x, 20.f, 0.6);
    CHECK_NEAR(path.front().y, -1.f, 0.3);
    // The level's hash follows the terrain's shape
    uint64_t before = prepareNavLevel(s, lib).hash;
    terrain::applyBrush(*s.findByName("Ground")->terrain, b, {-5.f, 0.f, 0.f}, 0.2f);
    CHECK(prepareNavLevel(s, lib).hash != before);
}
