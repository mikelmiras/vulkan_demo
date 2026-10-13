// Soft bodies (physics/soft_body.h), pre-fractured objects (physics/fracture.h) and buoyancy
#include "test.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "game/game.h"
#include "imgui.h"
#include "physics/fracture.h"
#include "physics/soft_body.h"
#include "scene/scene_diff.h"
#include "scene/serialization.h"

namespace {

void run(Game& g, int frames) {
    for (int i = 0; i < frames; ++i) {
        ImGui::GetIO().DeltaTime = 1.f / 60.f;
        ImGui::NewFrame();
        g.frame(1.f / 60.f, true, true);
        ImGui::EndFrame();
    }
}

// The volume a closed triangle soup encloses (divergence theorem: signed tetrahedra from the origin)
float volume(const std::vector<Vertex>& tris) {
    double v = 0;
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
        v += dot(tris[t].pos, cross(tris[t + 1].pos, tris[t + 2].pos)) / 6.0;
    return (float)v;
}

// A prism from z = -0.5 to 0.5: its cap's triangles (counter-clockwise, x and y) and its outline's edges (inside on
// the left)
fracture::Piece prism(const std::vector<std::array<Vec3, 3>>& caps, const std::vector<std::pair<Vec3, Vec3>>& edges) {
    fracture::Piece p;
    auto put = [&](Vec3 a, Vec3 b, Vec3 c) {
        for (Vec3 q : {a, b, c}) {
            Vertex v;
            v.pos = q;
            p.vertices.push_back(v);
        }
    };
    Vec3 up{0, 0, 0.5f};
    for (const auto& t : caps) {
        put(t[0] + up, t[1] + up, t[2] + up);
        put(t[0] - up, t[2] - up, t[1] - up);
    }
    for (const auto& [a, b] : edges) {
        put(a - up, b - up, b + up);
        put(a - up, b + up, a + up);
    }
    return p;
}

// An L: a 2 x 1 bar along x and a 1 x 2 one along y, sharing the corner square
fracture::Piece lShape() {
    std::vector<Vec3> c = {{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}, {1, 2, 0}, {0, 2, 0}};
    std::vector<std::array<Vec3, 3>> caps;
    for (size_t i = 1; i + 1 < c.size(); ++i) caps.push_back({c[0], c[i], c[i + 1]});   // (the corner sees it all)
    std::vector<std::pair<Vec3, Vec3>> edges;
    for (size_t i = 0; i < c.size(); ++i) edges.push_back({c[i], c[(i + 1) % c.size()]});
    return prism(caps, edges);
}

Entity& floor(Scene& s, float y) {
    Entity& g = s.create("Floor");
    g.transform.position[1] = y - 0.5f;
    g.collider = Collider{};
    g.collider->size = {40, 1, 40};
    return g;
}

}  // namespace

TEST(fracture_cuts_closed_pieces) {
    MeshLibrary lib = buildMeshLibrary();
    const Mesh& cube = lib.get((int)MeshKind::Cube);
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    for (uint32_t k = 0; k < cube.indexCount; ++k) {
        idx.push_back((uint32_t)verts.size());
        verts.push_back(lib.vertices[(size_t)cube.vertexOffset + lib.indices[cube.firstIndex + k]]);
    }
    Vec3 e = cube.boundsMax - cube.boundsMin;
    float whole = e.x * e.y * e.z;
    std::vector<fracture::Piece> pieces = fracture::cut(verts, idx, 8, 7);
    REQUIRE(pieces.size() == 8);
    float total = 0.f;
    for (const fracture::Piece& p : pieces) {
        float v = volume(p.vertices);
        CHECK(v > 0.f);   // closed and facing out (a hole or a wrong-way cap would spoil it)
        total += v;
        for (const Vertex& q : p.vertices) {
            CHECK(q.pos.x >= cube.boundsMin.x - 1e-4f && q.pos.x <= cube.boundsMax.x + 1e-4f);
            CHECK(q.pos.y >= cube.boundsMin.y - 1e-4f && q.pos.y <= cube.boundsMax.y + 1e-4f);
        }
    }
    CHECK_NEAR(total, whole, whole * 0.01);   // all of it, nothing twice
    std::vector<fracture::Piece> again = fracture::cut(verts, idx, 8, 7);   // the same seed: the same pieces
    REQUIRE(again.size() == pieces.size());
    CHECK(again[3].vertices.size() == pieces[3].vertices.size());
    CHECK(fracture::cut(verts, idx, 8, 8)[0].vertices.size() != pieces[0].vertices.size() ||
          volume(fracture::cut(verts, idx, 8, 8)[0].vertices) != volume(pieces[0].vertices));
}

// Concave meshes cut cleanly: a cut crossing both arms of an L closes each arm on its own, a cut through a frame
// leaves its hole open; concave pieces split into convex parts
TEST(fracture_concave_caps_and_parts) {
    fracture::Piece l = lShape();
    CHECK_NEAR(volume(l.vertices), 3.f, 1e-4);
    Vec3 n = normalize(Vec3{1, 1, 0});
    auto [corner, rest] = fracture::split(l, n, 2.5f / std::sqrt(2.f));   // (two separate triangles cut off the ends)
    CHECK_NEAR(volume(corner.vertices), 0.25f, 1e-3);
    CHECK_NEAR(volume(rest.vertices), 2.75f, 1e-3);

    std::vector<Vec3> o = {{-2, -2, 0}, {2, -2, 0}, {2, 2, 0}, {-2, 2, 0}}, in = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    std::vector<std::array<Vec3, 3>> caps;
    std::vector<std::pair<Vec3, Vec3>> edges;
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) % 4;
        caps.push_back({o[i], o[j], in[j]});
        caps.push_back({o[i], in[j], in[i]});
        edges.push_back({o[i], o[j]});
        edges.push_back({in[j], in[i]});   // (the hole's edges go round the other way)
    }
    fracture::Piece frame = prism(caps, edges);
    CHECK_NEAR(volume(frame.vertices), 12.f, 1e-4);
    auto [top, bottom] = fracture::split(frame, {0, 0, 1}, 0.f);   // (a ring-shaped cut: a cap with a hole)
    CHECK_NEAR(volume(top.vertices), 6.f, 1e-3);
    CHECK_NEAR(volume(bottom.vertices), 6.f, 1e-3);
    auto [side, other] = fracture::split(frame, {1, 0, 0}, 0.f);   // (across both sides of the hole: two caps)
    CHECK_NEAR(volume(side.vertices), 6.f, 1e-3);
    CHECK_NEAR(volume(other.vertices), 6.f, 1e-3);

    CHECK(!fracture::isConvex(l));
    CHECK(!fracture::isConvex(corner));   // (two apart)
    fracture::Piece end = fracture::split(l, {1, 0, 0}, 1.5f).first;   // (a box off the end of one arm)
    CHECK(fracture::isConvex(end));
    std::vector<fracture::Piece> parts = fracture::convexParts(l);
    CHECK(parts.size() > 1);
    float total = 0.f;
    for (const fracture::Piece& p : parts) {
        CHECK(fracture::isConvex(p));
        total += volume(p.vertices);
    }
    CHECK_NEAR(total, 3.f, 1e-3);
    CHECK(fracture::convexParts(end).size() == 1);
    std::vector<fracture::Piece> ring = fracture::convexParts(frame);
    CHECK(ring.size() > 1 && ring.size() <= 8);
}

// A crate dropped hard enough breaks into its pieces, which fall apart and are cleared away later
TEST(fracture_breaks_in_game) {
    test::ImGuiContextGuard imgui;
    MeshLibrary lib = buildMeshLibrary();
    Scene s;
    floor(s, 0.f);
    Entity& crate = s.create("Crate");
    crate.transform.position[1] = 4.f;
    crate.meshRenderer = MeshRenderer{};
    crate.meshRenderer->mesh = (int)MeshKind::Cube;
    crate.collider = Collider{};
    crate.rigidBody = RigidBody{};
    crate.fracture = Fracture{};
    crate.fracture->pieces = 6;
    crate.fracture->breakSpeed = 5.f;
    crate.fracture->pieceLife = 2.f;
    fracture::syncMeshes(s, lib);
    REQUIRE(s.findByName("Crate")->fracture->pieceMeshes.size() == 6);
    int meshes = lib.count();
    fracture::syncMeshes(s, lib);   // (made once)
    CHECK(lib.count() == meshes);
    Game g;
    g.start(s, lib);
    auto debris = [&] {
        int n = 0;
        for (const Entity& e : s.entities) n += e.hasTag("Debris");
        return n;
    };
    run(g, 90);   // falls 3.5 m: ~8 m/s on landing
    CHECK(!s.findByName("Crate"));
    CHECK(debris() == 6);
    run(g, 60);
    for (const Entity& e : s.entities)
        if (e.hasTag("Debris")) CHECK(e.transform.position[1] > -1.f);   // (on the floor, not through it)
    run(g, 90);   // past their 2 s
    CHECK(debris() == 0);
    // A soft landing doesn't break it
    Scene calm;
    floor(calm, 0.f);
    Entity& box = calm.create("Box");
    box.transform.position[1] = 0.6f;
    box.meshRenderer = MeshRenderer{};
    box.meshRenderer->mesh = (int)MeshKind::Cube;
    box.collider = Collider{};
    box.rigidBody = RigidBody{};
    box.fracture = Fracture{};
    fracture::syncMeshes(calm, lib);
    Game g2;
    g2.start(calm, lib);
    run(g2, 60);
    CHECK(calm.findByName("Box"));
    g2.context().shatter(calm.findByName("Box")->id);   // ... but being told to does
    run(g2, 2);
    CHECK(!calm.findByName("Box"));
    g.stop();
    g2.stop();
}

TEST(softbody_builds_particles) {
    MeshLibrary lib = buildMeshLibrary();
    SoftBody cloth;
    cloth.resolution = 8;
    softbody::Build b;
    REQUIRE(softbody::build(cloth, lib, -1, b));
    CHECK(b.particles.size() == 64);
    CHECK(b.faces.size() == 7u * 7u * 6u);
    CHECK(b.vertices.size() == 128);   // (both sides)
    for (int x = 0; x < 8; ++x) CHECK(b.invMass[(size_t)x] == 0.f);   // the top edge pinned
    CHECK(b.invMass[8] > 0.f);
    cloth.pinned = "corners";
    REQUIRE(softbody::build(cloth, lib, -1, b));
    CHECK(b.invMass[0] == 0.f && b.invMass[7] == 0.f && b.invMass[3] > 0.f);
    // A mesh: its corners welded (the cube's 36 vertices, three per triangle, are 8 corners), every face kept
    SoftBody jelly;
    jelly.shape = SoftBody::Shape::Mesh;
    REQUIRE(softbody::build(jelly, lib, (int)MeshKind::Cube, b));
    CHECK(b.particles.size() == 8);
    CHECK(b.faces.size() == 36);
    CHECK(b.vertices.size() == 36);
    CHECK(!softbody::build(jelly, lib, -1, b));
}

// A cloth pinned along its top sags; the drawn mesh follows its particles; it's saved with the scene
TEST(softbody_cloth_in_game) {
    test::ImGuiContextGuard imgui;
    MeshLibrary lib = buildMeshLibrary();
    Scene s;
    Entity& flag = s.create("Flag");
    flag.transform.position[1] = 3.f;
    flag.meshRenderer = MeshRenderer{};
    flag.meshRenderer->mesh = (int)MeshKind::Plane;
    flag.softBody = SoftBody{};
    flag.softBody->resolution = 10;
    flag.softBody->stiffness = 0.95f;
    softbody::syncMeshes(s, lib);
    int mesh = s.findByName("Flag")->softBody->mesh;
    REQUIRE(mesh >= 0);
    Game g;
    g.start(s, lib);
    run(g, 90);
    const SoftBody& sb = *s.findByName("Flag")->softBody;
    REQUIRE(sb.positions.size() == 100);
    CHECK_NEAR(sb.positions[0].y, 1.0, 1e-3);    // pinned: where it started (the top edge, 1 m above the entity)
    CHECK_NEAR(sb.positions[9].x, 1.0, 1e-3);
    float before = sb.positions[95].y;           // (the bottom row started at -1: it hangs there, or a little lower)
    CHECK(before < -0.98f && before > -1.3f);
    uint64_t serial = lib.rewriteSerial;
    softbody::syncMeshes(s, lib);   // the drawn mesh rewritten in place, as the particles are
    CHECK(s.findByName("Flag")->softBody->mesh == mesh && lib.rewriteSerial == serial + 1);
    CHECK_NEAR(lib.vertices[(size_t)lib.get(mesh).vertexOffset + 95].pos.y, before, 1e-4);
    // Unpinned, it falls away
    Scene drop;
    Entity& sheet = drop.create("Sheet");
    sheet.transform.position[1] = 3.f;
    sheet.softBody = SoftBody{};
    sheet.softBody->pinned = "none";
    Game g2;
    g2.start(drop, lib);
    run(g2, 30);
    REQUIRE(!drop.findByName("Sheet")->softBody->positions.empty());
    CHECK(drop.findByName("Sheet")->softBody->positions[0].y < 0.5f);
    g.stop();
    g2.stop();
    // Saved and loaded
    TextureLibrary tex;
    Scene back;
    std::string error;
    REQUIRE(sceneFromJson(sceneToJson(s, lib, tex), back, lib, tex, error));
    REQUIRE(back.findByName("Flag") && back.findByName("Flag")->softBody);
    CHECK(back.findByName("Flag")->softBody->resolution == 10);
    CHECK(sameEntity(*back.findByName("Flag"), *s.findByName("Flag")));
}

// Water: a light ball floats up to the surface, a heavy one sinks to the bottom
TEST(buoyancy_in_water) {
    test::ImGuiContextGuard imgui;
    MeshLibrary lib = buildMeshLibrary();
    Scene s;
    floor(s, -6.f);
    Entity& pool = s.create("Pool");
    pool.transform.position[1] = -3.f;
    pool.collider = Collider{};
    pool.collider->size = {20, 6, 20};   // water from -6 up to 0
    pool.collider->trigger = true;
    pool.tags.push_back("Water");
    auto ball = [&](const char* name, float x, float buoyancy) {
        Entity& b = s.create(name);
        b.transform.position[0] = x;
        b.transform.position[1] = -3.f;
        b.collider = Collider{};
        b.collider->shape = Collider::Shape::Sphere;
        b.rigidBody = RigidBody{};
        b.rigidBody->buoyancy = buoyancy;
    };
    ball("Cork", -2.f, 2.f);
    ball("Stone", 2.f, 0.4f);
    Game g;
    g.start(s, lib);
    run(g, 300);
    float cork = s.findByName("Cork")->transform.position[1], stone = s.findByName("Stone")->transform.position[1];
    CHECK(cork > -0.8f && cork < 0.6f);   // bobbing at the surface
    CHECK(stone < -5.f);                  // on the bottom
    g.stop();
}

// A cloth that tears: triangles stretched past tearStretch go (the drawn mesh folds them away, its size kept), the
// rest carries on where it was (and lands on the floor); one that doesn't tear keeps them all
TEST(softbody_cloth_tears) {
    test::ImGuiContextGuard imgui;
    MeshLibrary lib = buildMeshLibrary();
    std::vector<Vec3> rest = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    std::vector<uint32_t> faces = {0, 2, 1, 1, 2, 3};
    std::vector<Vec3> now = rest;
    now[3] = {1, 2, 0};   // (the second triangle's edges 1-3 and 2-3 stretched)
    CHECK(softbody::tearing(rest, now, faces, {}, 1.5f) == std::vector<uint32_t>{1});
    CHECK(softbody::tearing(rest, now, faces, {1}, 1.5f).empty());   // (already torn)
    CHECK(softbody::tearing(rest, now, faces, {}, 0.f).empty());

    SoftBody tough;
    tough.resolution = 8;
    softbody::Build b;
    REQUIRE(softbody::build(tough, lib, -1, b));
    CHECK(b.vertices.size() == 128);   // (shared vertices: it never tears)
    SoftBody weak = tough;
    weak.tearStretch = 1.05f;
    REQUIRE(softbody::build(weak, lib, -1, b));
    CHECK(b.vertices.size() == b.faces.size() * 2);   // (every corner of every triangle, both sides)

    Scene s;
    auto cloth = [&](const char* name, float x, float tear) {
        Entity& e = s.create(name);
        e.transform.position[0] = x;
        e.transform.position[1] = 3.f;
        e.meshRenderer = MeshRenderer{};
        e.meshRenderer->mesh = (int)MeshKind::Plane;
        e.softBody = SoftBody{};
        e.softBody->resolution = 8;
        e.softBody->pinned = "corners";
        e.softBody->stiffness = 0.f;
        e.softBody->mass = 20.f;
        e.softBody->tearStretch = tear;
    };
    cloth("Tough", -3.f, 0.f);
    cloth("Weak", 3.f, 1.05f);
    floor(s, 0.f);
    softbody::syncMeshes(s, lib);
    int weakMesh = s.findByName("Weak")->softBody->mesh;
    uint32_t weakVertices = lib.get(weakMesh).indexCount;
    Game g;
    g.start(s, lib);
    run(g, 60);
    const SoftBody& tore = *s.findByName("Weak")->softBody;
    CHECK(s.findByName("Tough")->softBody->torn.empty());
    REQUIRE(!tore.torn.empty());
    CHECK(std::is_sorted(tore.torn.begin(), tore.torn.end()));
    REQUIRE(tore.positions.size() == 64);
    float lowest = 0.f;
    for (const Vec3& p : tore.positions) lowest = std::min(lowest, p.y);
    CHECK(lowest < -1.2f);   // (sagging, falling: not put back at rest by the tearing)
    run(g, 120);
    lowest = 0.f;
    for (const Vec3& p : tore.positions) lowest = std::min(lowest, p.y);
    CHECK(lowest > -3.05f);   // (on the floor, 3 m down: made again as it tears, it still collides)
    softbody::syncMeshes(s, lib);
    CHECK(s.findByName("Weak")->softBody->mesh == weakMesh && lib.get(weakMesh).indexCount == weakVertices);
    const Vertex* v = &lib.vertices[(size_t)lib.get(weakMesh).vertexOffset + (size_t)tore.torn[0] * 3];
    CHECK(length(v[0].pos - v[1].pos) < 1e-6f && length(v[1].pos - v[2].pos) < 1e-6f);   // (a torn triangle folded into a point)
    g.stop();
}

// Soft bodies in water: an inflated ball that's buoyant floats up, one that isn't sinks
TEST(softbody_buoyancy) {
    test::ImGuiContextGuard imgui;
    MeshLibrary lib = buildMeshLibrary();
    Scene s;
    floor(s, -6.f);
    Entity& pool = s.create("Pool");
    pool.transform.position[1] = -3.f;
    pool.collider = Collider{};
    pool.collider->size = {20, 6, 20};   // water from -6 up to 0
    pool.collider->trigger = true;
    pool.tags.push_back("Water");
    auto ball = [&](const char* name, float x, float buoyancy) {
        Entity& b = s.create(name);
        b.transform.position[0] = x;
        b.transform.position[1] = -3.f;
        b.meshRenderer = MeshRenderer{};
        b.meshRenderer->mesh = (int)MeshKind::Sphere;
        b.softBody = SoftBody{};
        b.softBody->shape = SoftBody::Shape::Mesh;
        b.softBody->pressure = 2000.f;
        b.softBody->buoyancy = buoyancy;
    };
    ball("Float", -3.f, 3.f);
    ball("Sink", 3.f, 0.f);
    Game g;
    g.start(s, lib);
    auto height = [&](const char* name) {   // (its particles' average, in the world)
        const Entity& e = *s.findByName(name);
        float y = 0.f;
        for (const Vec3& p : e.softBody->positions) y += p.y;
        return e.transform.position[1] + y / (float)std::max<size_t>(e.softBody->positions.size(), 1);
    };
    run(g, 300);
    CHECK(height("Float") > -0.5f && height("Float") < 0.6f);   // up at the surface, settled
    CHECK(height("Sink") < -5.f);                               // on the bottom
    g.stop();
}

// Held together: the pieces stay joined (a tree of fixed joints) as one, until knocked apart; weakly held ones come
// apart and are cleared away, what they hung on stays
TEST(fracture_held_together) {
    test::ImGuiContextGuard imgui;
    MeshLibrary lib = buildMeshLibrary();
    auto drop = [&](Scene& s, float strength) {
        floor(s, 0.f);
        Entity& crate = s.create("Crate");
        crate.transform.position[1] = 4.f;
        crate.meshRenderer = MeshRenderer{};
        crate.meshRenderer->mesh = (int)MeshKind::Cube;
        crate.collider = Collider{};
        crate.rigidBody = RigidBody{};
        crate.fracture = Fracture{};
        crate.fracture->pieces = 6;
        crate.fracture->breakSpeed = 5.f;
        crate.fracture->pieceLife = 1.f;
        crate.fracture->holdTogether = true;
        crate.fracture->holdStrength = strength;
        fracture::syncMeshes(s, lib);
    };
    auto pieces = [](const Scene& s, int& joined) {
        int n = 0;
        joined = 0;
        for (const Entity& e : s.entities)
            if (e.hasTag("Debris")) {
                ++n;
                joined += e.joint && !e.joint->broken;
            }
        return n;
    };
    Scene s;
    drop(s, 1e6f);
    Game g;
    g.start(s, lib);
    run(g, 90);   // cracked on landing
    int joined = 0;
    CHECK(!s.findByName("Crate"));
    CHECK(pieces(s, joined) == 6);
    CHECK(joined == 5);   // (a tree: one fewer than the pieces)
    run(g, 90);   // past their life: still there, held
    CHECK(pieces(s, joined) == 6);
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi = lo * -1.f;
    for (const Entity& e : s.entities)
        if (e.hasTag("Debris")) {
            Vec3 p{e.transform.position[0], e.transform.position[1], e.transform.position[2]};
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
    CHECK(length(hi - lo) < 0.1f);   // (all where the crate was: as one)
    g.stop();

    Scene weak;
    drop(weak, 1e-3f);
    Game g2;
    g2.start(weak, lib);
    run(g2, 90);
    CHECK(pieces(weak, joined) == 6);
    CHECK(joined < 5);   // knocked apart by the landing
    run(g2, 90);
    int left = pieces(weak, joined);
    CHECK(left >= 1 && left < 6);   // the loose ones cleared away
    g2.stop();

    // Saved and loaded, with the cloth's and the soft body's new settings
    Scene saved;
    Entity& e = saved.create("Thing");
    e.fracture = Fracture{};
    e.fracture->holdTogether = true;
    e.fracture->holdStrength = 123.f;
    e.softBody = SoftBody{};
    e.softBody->tearStretch = 1.4f;
    e.softBody->buoyancy = 2.5f;
    e.softBody->torn = {3};   // (runtime: not saved)
    TextureLibrary tex;
    Scene back;
    std::string error;
    REQUIRE(sceneFromJson(sceneToJson(saved, lib, tex), back, lib, tex, error));
    const Entity& b = *back.findByName("Thing");
    CHECK(b.fracture->holdTogether && b.fracture->holdStrength == 123.f);
    CHECK(b.softBody->tearStretch == 1.4f && b.softBody->buoyancy == 2.5f && b.softBody->torn.empty());
    CHECK(sameEntity(b, *saved.findByName("Thing")));
    Entity changed = b;
    changed.fracture->holdTogether = false;
    CHECK(!sameEntity(changed, b));
}
