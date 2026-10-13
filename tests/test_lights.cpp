// Clustered lighting and the shadow atlas (render/light_clusters.h): no GPU needed
#include "test.h"

#include <set>

#include "render/light_clusters.h"

namespace {

// A camera at the origin looking down -Z, 16:9, 60 degrees
ClusterSetup setup() {
    ClusterSetup s;
    s.camPos = {0, 0, 0};
    s.camForward = {0, 0, -1};
    s.viewProj = perspective(60.f * DEG2RAD, 16.f / 9.f, 0.1f, 1000.f, true) * lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    return s;
}

// The lights listed for the cluster holding p
std::set<uint32_t> lightsAt(const ClusterSetup& s, const std::vector<uint32_t>& data, Vec3 p) {
    int c = clusterOf(s, p);
    std::set<uint32_t> out;
    if (c < 0) return out;
    uint32_t offset = data[(size_t)c * 2], count = data[(size_t)c * 2 + 1];
    for (uint32_t k = 0; k < count; ++k) out.insert(data[(size_t)s.clusters() * 2 + offset + k]);
    return out;
}

}  // namespace

TEST(clusters_slices_are_logarithmic) {
    ClusterSetup s = setup();
    CHECK(clusterSlice(s, 0.05f) == 0);
    CHECK(clusterSlice(s, 2000.f) == s.slices - 1);
    // Each slice is the same ratio deeper: 0.1 m to 1 km over 24 is about x1.47
    for (int k = 1; k < s.slices; ++k) {
        float start = s.near * std::pow(s.far / s.near, (float)k / (float)s.slices);
        CHECK(clusterSlice(s, start * 1.001f) == k);
        CHECK(clusterSlice(s, start * 0.999f) == k - 1);
    }
    CHECK(clusterOf(s, {0, 0, 5}) == -1);      // behind
    CHECK(clusterOf(s, {100, 0, -1}) == -1);   // off to the side
    CHECK(clusterOf(s, {0, 0, -5}) >= 0);
}

TEST(clusters_list_every_light_that_reaches) {
    ClusterSetup s = setup();
    // A grid of lights in front of the camera, and a big one around it, and one behind it
    std::vector<ClusterLight> lights;
    for (int x = -10; x <= 10; x += 2)
        for (int z = 2; z <= 60; z += 4) lights.push_back({{(float)x, 0.f, -(float)z}, 3.f});
    lights.push_back({{0, 0, 0}, 8.f});      // the camera is inside it
    lights.push_back({{0, 0, 40}, 5.f});     // behind, out of reach of anything visible
    std::vector<uint32_t> data;
    REQUIRE(buildLightClusters(s, lights, data, 1 << 17));
    REQUIRE(data.size() >= (size_t)s.clusters() * 2);
    // Every visible point a light reaches finds that light in its cluster (conservative: maybe more, never fewer)
    int checked = 0;
    uint32_t behind = (uint32_t)lights.size() - 1;
    for (int x = -15; x <= 15; ++x)
        for (int y = -3; y <= 3; ++y)
            for (int z = 1; z <= 70; z += 3) {
                Vec3 p{(float)x, (float)y * 0.7f, -(float)z};
                if (clusterOf(s, p) < 0) continue;
                std::set<uint32_t> listed = lightsAt(s, data, p);
                CHECK(!listed.count(behind));
                for (uint32_t i = 0; i + 1 < lights.size(); ++i)
                    if (length(lights[i].position - p) < lights[i].range) {
                        CHECK(listed.count(i));
                        ++checked;
                    }
            }
    CHECK(checked > 200);
    // ... and a far corner of the view lists few: clusters are a lot smaller than "everything"
    std::set<uint32_t> far = lightsAt(s, data, {-40.f, 20.f, -50.f});
    CHECK(far.size() < 4);
}

TEST(clusters_cap_the_list) {
    ClusterSetup s = setup();
    std::vector<ClusterLight> lights(200, ClusterLight{{0, 0, -10}, 50.f});   // everyone everywhere
    std::vector<uint32_t> data;
    CHECK(!buildLightClusters(s, lights, data, 1000));   // (not all of them fit)
    CHECK(data.size() == (size_t)s.clusters() * 2 + 1000);
    uint32_t total = 0;
    for (int c = 0; c < s.clusters(); ++c) total += data[(size_t)c * 2 + 1];
    CHECK(total == 1000);
}

TEST(shadow_atlas_packs_tiles) {
    // Two 1024s, four 512s, sixteen 256s and a 4096 that can't fit any more: placed without overlapping
    std::vector<int> sizes = {512, 1024, 256, 512, 1024, 512, 4096, 512};
    for (int i = 0; i < 16; ++i) sizes.push_back(256);
    std::vector<AtlasTile> tiles = allocateAtlas(4096, sizes);
    REQUIRE(tiles.size() == sizes.size());
    CHECK(tiles[6].size == 4096 || tiles[6].size == 0);
    // The 4096 is placed first (biggest first): then nothing else fits
    CHECK(tiles[6].size == 4096 && tiles[0].size == 0);
    sizes.erase(sizes.begin() + 6);
    tiles = allocateAtlas(4096, sizes);
    for (size_t i = 0; i < tiles.size(); ++i) {
        CHECK(tiles[i].size == sizes[i]);
        CHECK(tiles[i].x % tiles[i].size == 0 && tiles[i].y % tiles[i].size == 0);   // aligned
        CHECK(tiles[i].x + tiles[i].size <= 4096 && tiles[i].y + tiles[i].size <= 4096);
        for (size_t j = 0; j < i; ++j) {
            bool apart = tiles[i].x + tiles[i].size <= tiles[j].x || tiles[j].x + tiles[j].size <= tiles[i].x ||
                         tiles[i].y + tiles[i].size <= tiles[j].y || tiles[j].y + tiles[j].size <= tiles[i].y;
            CHECK(apart);
        }
    }
    // Full: 16 tiles of 1024 fill it, the 17th doesn't fit
    std::vector<AtlasTile> full = allocateAtlas(4096, std::vector<int>(17, 1024));
    int placed = 0;
    for (const AtlasTile& t : full) placed += t.size > 0;
    CHECK(placed == 16);
}

// The clusters' slices fitted to what's in view: an overhead camera looking down at a street 100 m below spans
// about 99-101 m, not 0.1-1000 m; boxes out of view don't count; nothing in view leaves the range as it was
TEST(cluster_depth_range_fits_the_view) {
    Vec3 cam{0, 100, 0}, fwd{0, -1, 0};
    Mat4 vp = perspective(0.6f, 16.f / 9.f, 1.f, 500.f, true) * lookAt(cam, {0, 0, 0}, {0, 0, -1});
    std::vector<std::pair<Vec3, Vec3>> boxes = {{{-20, 0, -20}, {20, 1, 20}},        // the street (in view)
                                                {{500, 0, 500}, {510, 80, 510}}};    // a tower far off to the side
    float near = 0.1f, far = 1000.f;
    fitDepthRange(vp, cam, fwd, boxes, near, far);
    CHECK_NEAR(near, 99.f, 0.01f);
    CHECK_NEAR(far, 148.5f, 0.01f);   // (at least half as deep again as near: 100 m, made 1.5 x 99)
    boxes.push_back({{-5, 0, -5}, {5, 60, 5}});   // a building 60 m tall under the camera
    near = 0.1f, far = 1000.f;
    fitDepthRange(vp, cam, fwd, boxes, near, far);
    CHECK_NEAR(near, 40.f, 0.01f);
    CHECK_NEAR(far, 100.f, 0.01f);
    near = 0.1f, far = 1000.f;
    fitDepthRange(vp, cam, fwd, {{{500, 0, 500}, {510, 1, 510}}}, near, far);
    CHECK(near == 0.1f && far == 1000.f);
}
