// assets/: built-in meshes, OBJ import, picking
#include "test.h"

#include "assets/meshes.h"

TEST(meshes_builtin_library) {
    MeshLibrary lib = buildMeshLibrary();
    REQUIRE(lib.count() == (int)MeshKind::Count);
    CHECK(lib.find("Cube") == (int)MeshKind::Cube);
    CHECK(lib.find("Nope") == -1);
    const Mesh& cube = lib.get((int)MeshKind::Cube);
    CHECK(cube.indexCount == 36);
    CHECK_VEC(cube.boundsMin, test::v3(-0.5f, -0.5f, -0.5f), 1e-6f);
    CHECK_VEC(cube.boundsMax, test::v3(0.5f, 0.5f, 0.5f), 1e-6f);
}

TEST(obj_import_polygons_normals_crlf) {
    MeshLibrary lib = buildMeshLibrary();
    auto path = test::tempPath("quad.obj");
    test::writeFile(path, "o quad\r\nv 0 0 0\r\nv 10 0 0\r\nv 10 0 20\r\nv 0 0 20\r\nvn 0 1 0\r\nvt 0 0\r\n"
                          "f 1/1/1 2/1/1 3/1/1 4/1/1\r\n");
    std::string err;
    int id = importObj(lib, path.string(), err);
    REQUIRE(id == (int)MeshKind::Count);
    const Mesh& m = lib.get(id);
    CHECK(m.indexCount == 6);                                          // quad -> 2 triangles
    CHECK(lib.vertices.size() - (size_t)m.vertexOffset == 4);          // smooth corners shared
    CHECK_VEC(m.boundsMin, test::v3(-0.25f, 0.f, -0.5f), 1e-5f);           // largest side scaled to 1, centred
    CHECK_VEC(m.boundsMax, test::v3(0.25f, 0.f, 0.5f), 1e-5f);
    CHECK(m.name == "quad");
    CHECK(lib.version == 1);
    CHECK(importObj(lib, path.string(), err) == id);                  // same file: reused
    CHECK(lib.version == 1);
}

TEST(obj_import_negative_indices_and_flat_normals) {
    MeshLibrary lib = buildMeshLibrary();
    auto path = test::tempPath("tet.obj");
    test::writeFile(path, "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf -4 -2 -3\nf 1 2 4\nf 1 4 3\nf 2 3 4\n");
    std::string err;
    int id = importObj(lib, path.string(), err);
    REQUIRE(id >= 0);
    CHECK(lib.get(id).indexCount == 12);
    CHECK(lib.vertices.size() - (size_t)lib.get(id).vertexOffset == 12);   // flat shading: no sharing
}

TEST(obj_import_errors) {
    MeshLibrary lib = buildMeshLibrary();
    std::string err;
    CHECK(importObj(lib, test::tempPath("missing.obj").string(), err) == -1);
    CHECK(err.find("cannot open") != std::string::npos);
    auto empty = test::tempPath("empty.obj");
    test::writeFile(empty, "v 0 0 0\n");
    err.clear();
    CHECK(importObj(lib, empty.string(), err) == -1);
    CHECK(err.find("no faces") != std::string::npos);
    CHECK(lib.count() == (int)MeshKind::Count);
}

TEST(mesh_ray_intersection) {
    MeshLibrary lib = buildMeshLibrary();
    int cube = (int)MeshKind::Cube;
    CHECK_NEAR(intersectMesh(lib, cube, {0, 5, 0}, {0, -1, 0}), 4.5f, 1e-5);
    CHECK(intersectMesh(lib, cube, {2, 5, 0}, {0, -1, 0}) < 0.f);    // misses
    CHECK(intersectMesh(lib, cube, {0, 5, 0}, {0, 1, 0}) < 0.f);     // pointing away
}
