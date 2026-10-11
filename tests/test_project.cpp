// core/project: project-relative paths, project.json, moving a project
#include "test.h"

#include "app/export.h"
#include "game/module.h"
#include "platform/platform.h"

#include "core/png.h"
#include "core/project.h"
#include "scene/serialization.h"

namespace fs = std::filesystem;

namespace {

// Restores the project root when a test ends
struct RootGuard {
    fs::path saved = project::root();
    ~RootGuard() { project::setRoot(saved); }
};

}  // namespace

TEST(project_paths_relative_inside_absolute_outside) {
    RootGuard guard;
    fs::path root = test::tempPath("proj");
    fs::create_directories(root / "models");
    project::setRoot(root);
    std::string inside = (project::root() / "models" / "a.obj").u8string();
    CHECK(project::toStored(inside) == "models/a.obj");
    CHECK(project::toStored(inside + "#2.0") == "models/a.obj#2.0");   // glTF sub-asset suffix kept
    CHECK(project::resolve("models/a.obj") == inside);
    CHECK(project::resolve("models/a.obj#image1") == inside + "#image1");
    std::string outside = (project::root().parent_path() / "elsewhere.png").u8string();
    CHECK(project::toStored(outside) == fs::u8path(outside).generic_u8string());   // stays absolute
    CHECK(project::resolve(project::toStored(outside)) == outside);
    CHECK(project::toStored("").empty());
}

TEST(project_settings_roundtrip) {
    fs::path dir = test::tempPath("settings");
    fs::create_directories(dir);
    CHECK(!project::exists(dir));
    project::Settings s;
    s.name = "Space Game";
    s.startScene = "levels/one.vkw";
    std::string err;
    REQUIRE(project::save(dir, s, err));
    CHECK(project::exists(dir));
    project::Settings loaded;
    REQUIRE(project::load(dir, loaded, err));
    CHECK(loaded.name == "Space Game" && loaded.startScene == "levels/one.vkw");
    test::writeFile(dir / "project.json", "{\"format\":\"something-else\"}");
    CHECK(!project::load(dir, loaded, err));
}

TEST(project_can_be_moved) {
    RootGuard guard;
    fs::path a = test::tempPath("ProjectA");
    fs::create_directories(a / "assets");
    test::writeFile(a / "assets" / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    uint8_t px[4] = {10, 20, 30, 255};
    writePng((a / "assets" / "dot.png").u8string(), 1, 1, px);
    project::setRoot(a);

    MeshLibrary meshes = buildMeshLibrary();
    TextureLibrary texs = buildTextureLibrary();
    std::string err;
    int mesh = importObj(meshes, project::resolve("assets/tri.obj"), err);
    int tex = loadTexture(texs, project::resolve("assets/dot.png"), err);
    REQUIRE(mesh >= 0 && tex >= 0);
    Scene s;
    MeshRenderer mr;
    mr.mesh = mesh;
    mr.texture = tex;
    s.create("Thing").meshRenderer = mr;
    REQUIRE(saveScene(project::resolve("level.vkw"), s, meshes, texs, err));

    // Move the whole folder; the scene must find its files in the new place
    fs::path b = test::tempPath("ProjectB");
    fs::rename(a, b);
    project::setRoot(b);
    MeshLibrary meshes2 = buildMeshLibrary();
    TextureLibrary texs2 = buildTextureLibrary();
    Scene loaded;
    REQUIRE(loadScene(project::resolve("level.vkw"), loaded, meshes2, texs2, err));
    const MeshRenderer& r = *loaded.entities[0].meshRenderer;
    CHECK(meshes2.get(r.mesh).source.find("ProjectB") != std::string::npos);
    CHECK(texs2.get(r.texture).source.find("ProjectB") != std::string::npos);
}

#ifdef VKWORLD_SAMPLE_GAME
// Exporting the sample game: the runtime, the shaders, the project's files and its module, but not its sources
TEST(export_game_folder) {
    namespace fs = std::filesystem;
    fs::path sample = fs::u8path(VKWORLD_SAMPLE_GAME), out = test::tempPath("exported");
    ExportResult result;
    std::string error;
    REQUIRE(exportGame(sample, out, platform::executableDir(), result, error));
    CHECK(fs::is_regular_file(result.executable));
    CHECK(result.executable.filename().u8string().rfind("Sample Game", 0) == 0);
    CHECK(fs::is_regular_file(out / "project.json"));
    CHECK(fs::is_regular_file(out / "scenes" / "main.vkw"));
    CHECK(fs::is_regular_file(out / "models" / "tentacle.gltf"));
    CHECK(fs::is_regular_file(GameModule::libraryPath(out / "bin" / "sample_game")));
    CHECK(fs::is_regular_file(out / "shaders" / "scene.frag.spv"));
    CHECK(!fs::exists(out / "src"));               // no sources
    CHECK(!fs::exists(out / "CMakeLists.txt"));
    CHECK(result.warnings.empty());
    CHECK(project::root() != sample);              // (the current project is left as it was)
    // Not over the project itself
    CHECK(!exportGame(sample, sample, platform::executableDir(), result, error));
}
#endif

// A scene using a file outside the project gets a warning: the exported game won't have it
TEST(export_warns_about_outside_assets) {
    namespace fs = std::filesystem;
    fs::path proj = test::tempPath("export_proj"), out = test::tempPath("export_out");
    fs::create_directories(proj / "scenes");
    test::writeFile(proj / "project.json", R"({"format":"vkworld-project","version":1,"name":"Tiny","startScene":"scenes/a.vkw"})");
    test::writeFile(proj / "scenes" / "a.vkw", R"({"format":"vkworld-scene","version":12,"meshes":[{"name":"Rock","source":"/nowhere/rock.obj"}],"entities":[]})");
    ExportResult result;
    std::string error;
    REQUIRE(exportGame(proj, out, platform::executableDir(), result, error));
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("/nowhere/rock.obj") != std::string::npos);
}
