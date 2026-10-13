#include "scene/water.h"

#include <algorithm>
#include <cmath>

#include "assets/meshes.h"

namespace water {

namespace {

struct Wave {
    float dx, dz, k, amplitude, speed, steepness;
};

// The three waves, in the world (`yaw` turns them with the entity)
void waves(const Water& w, float yaw, Wave out[3]) {
    const float turns[3] = {0.f, 35.f, -25.f}, lengths[3] = {1.f, 0.57f, 0.31f}, heights[3] = {1.f, 0.45f, 0.2f};
    for (int i = 0; i < 3; ++i) {
        float a = (w.waveDirection + turns[i]) * DEG2RAD - yaw;
        float len = std::max(w.waveLength * lengths[i], 0.05f);
        float k = 6.2831853f / len;
        Wave& v = out[i];
        v.dx = std::cos(a);
        v.dz = std::sin(a);
        v.k = k;
        v.amplitude = std::max(w.waveHeight, 0.f) * heights[i];
        v.speed = std::sqrt(9.81f / k);   // (deep water: longer waves run faster)
        // (sharper crests with choppiness; never so sharp the surface loops over itself)
        v.steepness = v.amplitude > 0.f ? std::clamp(w.choppiness, 0.f, 1.f) / (k * v.amplitude * 3.f) : 0.f;
    }
}

}  // namespace

Sample sample(const Water& w, float yaw, float x, float z, float t) {
    Wave ws[3];
    waves(w, yaw, ws);
    Sample s;
    Vec3 tx{1.f, 0.f, 0.f}, tz{0.f, 0.f, 1.f};   // (the moved surface's slopes along x and z: its normal from them)
    for (const Wave& v : ws) {
        if (v.amplitude <= 0.f) continue;
        float f = v.k * (v.dx * x + v.dz * z - v.speed * t), c = std::cos(f), sn = std::sin(f);
        float omega = v.k * v.speed, qa = v.steepness * v.amplitude;
        s.offset = s.offset + Vec3{qa * v.dx * c, v.amplitude * sn, qa * v.dz * c};
        s.velocity = s.velocity + Vec3{qa * v.dx * omega * sn, -v.amplitude * omega * c, qa * v.dz * omega * sn};
        float ka = v.k * v.amplitude, kqa = v.k * qa;
        tx = tx + Vec3{-kqa * v.dx * v.dx * sn, ka * v.dx * c, -kqa * v.dx * v.dz * sn};
        tz = tz + Vec3{-kqa * v.dx * v.dz * sn, ka * v.dz * c, -kqa * v.dz * v.dz * sn};
    }
    s.normal = normalize(cross(tz, tx));
    return s;
}

float heightAt(const Water& w, float yaw, float x, float z, float t) {
    float px = x, pz = z;
    for (int i = 0; i < 2; ++i) {   // (the rest point that ends up over (x, z))
        Sample s = sample(w, yaw, px, pz, t);
        px = x - s.offset.x;
        pz = z - s.offset.z;
    }
    return sample(w, yaw, px, pz, t).offset.y;
}

void syncMeshes(Scene& scene, MeshLibrary& meshes) {
    int grid = -1;
    for (Entity& e : scene.entities) {
        if (!e.water) continue;
        if (e.water->mesh >= 0 && e.water->mesh < meshes.count()) continue;
        if (grid < 0) grid = meshes.find("Water grid");
        if (grid < 0) {
            const int n = 128;
            std::vector<Vertex> v;
            std::vector<uint32_t> idx;
            for (int j = 0; j <= n; ++j)
                for (int i = 0; i <= n; ++i)
                    v.push_back({{(float)i / n - 0.5f, 0.f, (float)j / n - 0.5f}, {0.f, 1.f, 0.f}, (float)i / n, (float)j / n});
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    uint32_t a = (uint32_t)(j * (n + 1) + i), b = a + 1, c = a + (uint32_t)(n + 1), d = c + 1;
                    idx.insert(idx.end(), {a, c, b, b, c, d});   // (facing +y)
                }
            grid = addMesh(meshes, "Water grid", "", v, idx);
            // (its waves move it up and down: room for them in its bounds, so it isn't culled at a crest)
            meshes.meshes[(size_t)grid].boundsMin.y = -4.f;
            meshes.meshes[(size_t)grid].boundsMax.y = 4.f;
        }
        e.water->mesh = grid;
    }
}

}  // namespace water
