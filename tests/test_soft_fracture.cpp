// Soft bodies (physics/soft_body.h), pre-fractured objects (physics/fracture.h) and buoyancy
#include "test.h"

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
