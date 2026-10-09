#include "scene/scene.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>

#include "imgui.h"
#include "ImGuizmo.h"

Mat4 SceneObject::baseMatrix() const {
    Mat4 m;
    ImGuizmo::RecomposeMatrixFromComponents(position, rotation, scale, m.m);
    return m;
}

Mat4 SceneObject::animatedMatrix(float t) const {
    Mat4 base = baseMatrix();
    if (spin == 0.f && bob == 0.f) return base;
    // Spin around the world Y axis through the object's origin, then bob up and down.
    Vec3 p = pos();
    Vec3 offset{0, bob * std::sin(t * 1.5f + phase), 0};
    return translate(p + offset) * rotateY(spin * t + phase) * translate(p * -1.f) * base;
}

// ---------------------------------------------------------------------------------------------
// Default scene
// ---------------------------------------------------------------------------------------------
namespace {

struct Rng {
    uint32_t s = 0x12345678u;
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFF) / float(0x1000000); }
    float range(float a, float b) { return a + (b - a) * next(); }
};

Vec3 hsv(float h, float s, float v) {
    auto f = [&](float n) {
        float k = std::fmod(n + h * 6.f, 6.f);
        return v - v * s * std::max(0.f, std::min({k, 4.f - k, 1.f}));
    };
    return {f(5), f(3), f(1)};
}

SceneObject make(std::string name, MeshKind mesh, Vec3 pos, Vec3 size, Vec3 color) {
    SceneObject o;
    o.name = std::move(name);
    o.mesh = (int)mesh;
    o.position[0] = pos.x; o.position[1] = pos.y; o.position[2] = pos.z;
    o.scale[0] = size.x; o.scale[1] = size.y; o.scale[2] = size.z;
    o.color = color;
    return o;
}

std::string numbered(const char* base, int n) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s.%03d", base, n);
    return buf;
}

}  // namespace

std::vector<SceneObject> buildDefaultScene() {
    std::vector<SceneObject> objs;
    Rng rng;

    SceneObject ground = make("Ground", MeshKind::Plane, {0, 0, 0}, {600, 1, 600}, {0.42f, 0.55f, 0.38f});
    ground.checker = true;
    ground.locked = true;
    objs.push_back(ground);

    // Centrepiece: golden sphere inside a spinning tilted torus
    SceneObject sphere = make("Centre Sphere", MeshKind::Sphere, {0, 5, 0}, {3, 3, 3}, {0.95f, 0.75f, 0.25f});
    sphere.bob = 0.4f;
    objs.push_back(sphere);
    SceneObject torus = make("Centre Torus", MeshKind::Torus, {0, 5, 0}, {7, 7, 7}, {0.85f, 0.2f, 0.25f});
    torus.rotation[0] = 0.6f / DEG2RAD;
    torus.spin = 0.8f;
    torus.bob = 0.4f;
    objs.push_back(torus);

    // Ring of pillars, each with a floating spinning cube on top
    const int pillars = 12;
    for (int i = 0; i < pillars; ++i) {
        float a = i * 2.f * PI / pillars, h = (i % 2) ? 5.f : 8.f;
        Vec3 p{std::cos(a) * 20.f, 0, std::sin(a) * 20.f};
        objs.push_back(make(numbered("Pillar", i + 1), MeshKind::Cube, p + Vec3{0, h * 0.5f, 0}, {1.6f, h, 1.6f},
                            {0.82f, 0.8f, 0.76f}));
        SceneObject top = make(numbered("Pillar Top", i + 1), MeshKind::Cube, p + Vec3{0, h + 1.8f, 0}, {1, 1, 1},
                               hsv((float)i / pillars, 0.7f, 0.95f));
        top.rotation[0] = 0.6f / DEG2RAD;
        top.spin = 1.5f;
        top.bob = 0.5f;
        top.phase = a;
        objs.push_back(top);
    }

    // Distant landmark pyramids
    for (int i = 0; i < 5; ++i) {
        float a = i * 2.f * PI / 5 + 0.3f, d = rng.range(110, 160), s = rng.range(20, 35);
        SceneObject o = make(numbered("Landmark", i + 1), MeshKind::Pyramid, {std::cos(a) * d, 0, std::sin(a) * d},
                             {s, s * 0.8f, s}, {0.85f, 0.75f, 0.55f});
        o.rotation[1] = a / DEG2RAD;
        objs.push_back(o);
    }

    // Random scattered objects
    const MeshKind kinds[4] = {MeshKind::Cube, MeshKind::Sphere, MeshKind::Pyramid, MeshKind::Torus};
    const char* kNames[4] = {"Cube", "Sphere", "Pyramid", "Torus"};
    int counters[4] = {};
    for (int i = 0; i < 120; ++i) {
        float a = rng.range(0, 2 * PI), d = rng.range(28, 100);
        int k = (int)(rng.next() * 4) % 4;
        float s = rng.range(1.f, 4.f);
        SceneObject o = make(numbered(kNames[k], ++counters[k]), kinds[k],
                             {std::cos(a) * d, 0, std::sin(a) * d}, {s, s, s},
                             hsv(rng.next(), rng.range(0.4f, 0.8f), rng.range(0.7f, 0.95f)));
        o.phase = rng.range(0, 2 * PI);
        o.rotation[1] = rng.range(0, 360);
        switch (k) {
            case 0: o.position[1] = s * 0.5f; break;
            case 1: o.position[1] = s * 0.5f + 1.f; o.bob = 0.8f; break;
            case 2: o.scale[1] = s * rng.range(0.8f, 2.f); break;
            case 3: o.position[1] = s * 0.5f + 0.5f; o.spin = 1.f; o.rotation[0] = 90.f; break;
        }
        if (k == 0 && rng.next() < 0.3f) {
            o.position[1] += 2.f; o.spin = 0.7f; o.bob = 0.5f; o.rotation[0] = 0.6f / DEG2RAD;
        }
        objs.push_back(o);
    }
    return objs;
}

// ---------------------------------------------------------------------------------------------
// Save / load: one object per line, whitespace-separated, names quoted. Imported meshes are listed first
// as `mesh "name" "path"` lines (v2); v1 files have only objects with built-in meshes.
// ---------------------------------------------------------------------------------------------
static const char* kHeader = "# vkworld scene v2";

bool saveScene(const std::string& path, const std::vector<SceneObject>& objects, const MeshLibrary& meshes,
               std::string& error) {
    std::ofstream f(path);
    if (!f) { error = "cannot write " + path; return false; }
    f << kHeader << "\n";
    std::vector<bool> used((size_t)meshes.count(), false);
    for (const SceneObject& o : objects) used[(size_t)o.mesh] = true;
    for (int i = (int)MeshKind::Count; i < meshes.count(); ++i)
        if (used[(size_t)i]) f << "mesh " << std::quoted(meshes.name(i)) << ' ' << std::quoted(meshes.get(i).source) << '\n';
    f << "# name mesh pos[3] rot[3] scale[3] color[3] checker visible locked spin bob phase\n";
    f << std::setprecision(6);
    for (const SceneObject& o : objects) {
        f << std::quoted(o.name) << ' ' << std::quoted(meshes.name(o.mesh));
        for (float v : o.position) f << ' ' << v;
        for (float v : o.rotation) f << ' ' << v;
        for (float v : o.scale) f << ' ' << v;
        f << ' ' << o.color.x << ' ' << o.color.y << ' ' << o.color.z << ' ' << o.checker << ' ' << o.visible
          << ' ' << o.locked << ' ' << o.spin << ' ' << o.bob << ' ' << o.phase << '\n';
    }
    if (!f) { error = "write failed for " + path; return false; }
    return true;
}

bool loadScene(const std::string& path, std::vector<SceneObject>& objects, MeshLibrary& meshes, std::string& error) {
    std::ifstream f(path);
    if (!f) { error = "cannot open " + path; return false; }
    std::vector<SceneObject> result;
    std::map<std::string, int> fileMeshes;   // mesh name in this file -> library id (names may differ if renamed on import)
    for (int i = 0; i < (int)MeshKind::Count; ++i) fileMeshes[meshes.name(i)] = i;
    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        auto fail = [&](const std::string& msg) { error = path + ":" + std::to_string(lineNo) + ": " + msg; return false; };
        if (line.compare(0, 5, "mesh ") == 0) {
            std::string keyword, name, source;
            in >> keyword >> std::quoted(name) >> std::quoted(source);
            if (!in) return fail("malformed mesh line");
            std::string importError;
            int id = importObj(meshes, source, importError);
            if (id < 0) return fail(importError);
            fileMeshes[name] = id;
            continue;
        }
        SceneObject o;
        std::string mesh;
        in >> std::quoted(o.name) >> std::quoted(mesh);
        for (float& v : o.position) in >> v;
        for (float& v : o.rotation) in >> v;
        for (float& v : o.scale) in >> v;
        in >> o.color.x >> o.color.y >> o.color.z >> o.checker >> o.visible >> o.locked >> o.spin >> o.bob >> o.phase;
        if (!in) return fail("malformed line");
        auto it = fileMeshes.find(mesh);
        if (it == fileMeshes.end()) return fail("unknown mesh '" + mesh + "'");
        o.mesh = it->second;
        result.push_back(std::move(o));
    }
    objects = std::move(result);
    return true;
}
