// assets/asset_db: ids in .meta files, references that survive moves, import settings
#include "test.h"

#include <fstream>

#include "assets/asset_db.h"
#include "core/png.h"
#include "core/vfs.h"
#include "core/project.h"
#include "scene/serialization.h"

namespace fs = std::filesystem;

namespace {

// A project folder with an image, a sound and a model
fs::path makeProject(const char* name) {
    fs::path root = test::tempPath(name);
    fs::remove_all(root);
    fs::create_directories(root / "textures");
    std::vector<uint8_t> rgba(64 * 32 * 4, 200);
    REQUIRE(writePng((root / "textures" / "wall.png").u8string(), 64, 32, rgba.data()));
    test::writeFile(root / "sounds" / "ding.wav", "RIFF....WAVE");   // (only needs to exist here)
    test::writeFile(root / "models" / "block.obj",
                    "v 0 0 0\nv 4 0 0\nv 4 2 0\nv 0 2 0\nv 0 0 2\nf 1 2 3\nf 1 3 4\nf 1 2 5\n");
    test::writeFile(root / "project.json", "{\"format\": \"vkworld-project\", \"name\": \"T\", \"startScene\": \"scenes/main.vkw\"}");
    project::setRoot(root);
    return root;
}

// The same file, however it's spelled (macOS's temp folder is behind a symlink; Windows has short names)
bool same(const fs::path& a, const fs::path& b) { return fs::weakly_canonical(a) == fs::weakly_canonical(b); }

struct Done {
    ~Done() {
        assetdb::close();
        project::setRoot(fs::current_path());
    }
};

// A scene using the image (a mesh's texture), the sound (an audio source) and a scene field (a behaviour's)
Scene sceneUsing(MeshLibrary& lib, TextureLibrary& tex, const fs::path& root) {
    std::string error;
    Scene s;
    Entity& e = s.create("Wall");
    e.meshRenderer = MeshRenderer{};
    e.meshRenderer->texture = loadTexture(tex, (root / "textures" / "wall.png").u8string(), error);
    REQUIRE(e.meshRenderer->texture >= 0);
    e.audioSource = AudioSource{};
    e.audioSource->clip = "sounds/ding.wav";
    e.behaviours.emplace_back(createBehaviour("SceneExit"));   // (its "scene" field names a scene file)
    REQUIRE(setField(*e.behaviours[0], "scene", std::string("scenes/next.vkw")));
    (void)lib;
    return s;
}

}  // namespace

TEST(assetdb_ids) {
    fs::path root = makeProject("adb_ids");
    Done done;
    assetdb::open(root, true);
    std::string id = assetdb::idOf(root / "textures" / "wall.png");
    CHECK(id.size() == 32);
    CHECK(fs::exists(root / "textures" / "wall.png.meta"));
    CHECK(assetdb::idOf(root / "textures" / "wall.png") == id);
    CHECK(assetdb::idOfStored("textures/wall.png") == id);
    CHECK(assetdb::idOfStored("models/block.obj#0") == assetdb::idOf(root / "models" / "block.obj"));
    CHECK(assetdb::idOfStored("Hum").empty());   // a built-in
    CHECK(assetdb::idOf(root / "textures").empty());   // folders have none
    CHECK(assetdb::idOf(test::tempPath("elsewhere.png")).empty());
    CHECK(same(assetdb::pathOf(id), root / "textures" / "wall.png"));
    // Read back from the .meta files
    assetdb::open(root, false);
    CHECK(assetdb::idOf(root / "textures" / "wall.png") == id);
    CHECK(assetdb::idOf(root / "sounds" / "ding.wav").empty());   // read-only: no new .meta
    CHECK(!fs::exists(root / "sounds" / "ding.wav.meta"));
    // A copy made with its .meta gets an id of its own
    assetdb::open(root, true);
    fs::copy_file(root / "textures" / "wall.png", root / "textures" / "copy.png");
    fs::copy_file(root / "textures" / "wall.png.meta", root / "textures" / "copy.png.meta");
    assetdb::open(root, true);
    std::string a = assetdb::idOf(root / "textures" / "wall.png"), b = assetdb::idOf(root / "textures" / "copy.png");
    CHECK(!a.empty() && !b.empty() && a != b);
}

// Files moved outside the editor (with their .meta): scenes find them again by id
TEST(assetdb_heal_on_load) {
    fs::path root = makeProject("adb_heal");
    Done done;
    assetdb::open(root, true);
    MeshLibrary lib = buildMeshLibrary();
    TextureLibrary tex = buildTextureLibrary();
    Scene s = sceneUsing(lib, tex, root);
    test::writeFile(root / "scenes" / "next.vkw", "{}");
    std::string error;
    fs::path scene = root / "scenes" / "main.vkw";
    REQUIRE(saveScene(scene.u8string(), s, lib, tex, error));
    std::string text;
    REQUIRE(vfs::read(scene, text));
    CHECK(text.find("\"clipId\"") != std::string::npos);
    CHECK(text.find("\"$refs\"") != std::string::npos);

    // A file manager moves them (each with its .meta)
    fs::create_directories(root / "art");
    for (auto [from, to] : {std::pair<fs::path, fs::path>{"textures/wall.png", "art/brick.png"},
                            {"sounds/ding.wav", "art/ding.wav"}, {"scenes/next.vkw", "art/level2.vkw"}}) {
        fs::rename(root / from, root / to);
        fs::rename(assetdb::metaPath(root / from), assetdb::metaPath(root / to));
    }
    assetdb::open(root, false);   // (a game: read-only)
    MeshLibrary lib2 = buildMeshLibrary();
    TextureLibrary tex2 = buildTextureLibrary();
    Scene back;
    REQUIRE(loadScene(scene.u8string(), back, lib2, tex2, error));
    const Entity* e = back.findByName("Wall");
    REQUIRE(e && e->meshRenderer && e->meshRenderer->texture >= 0);
    CHECK(fs::path(tex2.get(e->meshRenderer->texture).source).filename() == "brick.png");
    CHECK(e->audioSource->clip == "art/ding.wav");
    CHECK(behaviourToJson(*e->behaviours[0])["scene"].asString() == "art/level2.vkw");
}

// The editor's move: files and .meta, and the references in the project's files rewritten
TEST(assetdb_move_and_retarget) {
    fs::path root = makeProject("adb_move");
    Done done;
    assetdb::open(root, true);
    MeshLibrary lib = buildMeshLibrary();
    TextureLibrary tex = buildTextureLibrary();
    Scene s = sceneUsing(lib, tex, root);
    s.create("textures");   // an entity named like the folder: not a reference
    std::string error;
    fs::create_directories(root / "scenes");
    REQUIRE(saveScene((root / "scenes" / "main.vkw").u8string(), s, lib, tex, error));
    std::string id = assetdb::idOf(root / "textures" / "wall.png");

    REQUIRE(assetdb::move(root / "textures", root / "art" / "textures", error));
    CHECK(fs::exists(root / "art" / "textures" / "wall.png.meta"));
    CHECK(same(assetdb::pathOf(id), root / "art" / "textures" / "wall.png"));
    std::vector<fs::path> files;
    int refs = assetdb::retargetProjectFiles(root / "textures", root / "art" / "textures", &files);
    CHECK(refs == 1);
    CHECK(files.size() == 1);
    std::string text;
    REQUIRE(vfs::read(root / "scenes" / "main.vkw", text));
    CHECK(text.find("art/textures/wall.png") != std::string::npos);
    CHECK(text.find("\"name\": \"textures\"") != std::string::npos);

    // The start scene, in project.json
    REQUIRE(assetdb::move(root / "scenes" / "main.vkw", root / "scenes" / "first.vkw", error));
    CHECK(assetdb::retargetProjectFiles(root / "scenes" / "main.vkw", root / "scenes" / "first.vkw") == 1);
    project::Settings settings;
    REQUIRE(project::load(root, settings, error));
    CHECK(settings.startScene == "scenes/first.vkw");

    // One reference at a time: relative stays relative, absolute stays absolute, fragments are kept
    CHECK(assetdb::retarget("models/block.obj#2", root / "models", root / "geo") == "geo/block.obj#2");
    CHECK(same(fs::u8path(assetdb::retarget((root / "models" / "block.obj").u8string(), root / "models" / "block.obj",
                                            root / "b.obj")),
               root / "b.obj"));
    CHECK(assetdb::retarget("models2/block.obj", root / "models", root / "geo") == "models2/block.obj");
    REQUIRE(assetdb::remove(root / "art", error));
    CHECK(!fs::exists(root / "art"));
    CHECK(assetdb::pathOf(id).empty());
}

// Import settings: textures (size, filter, mipmaps) and OBJ models (fit, scale), reloaded in place
TEST(assetdb_import_settings) {
    fs::path root = makeProject("adb_import");
    Done done;
    assetdb::open(root, true);
    fs::path png = root / "textures" / "wall.png", obj = root / "models" / "block.obj";
    CHECK(assetdb::importSettings(png).isObject() && assetdb::importSettings(png).size() == 0);
    std::string error;
    TextureLibrary tex = buildTextureLibrary();
    int t = loadTexture(tex, png.u8string(), error);
    REQUIRE(t >= 0);
    CHECK(tex.get(t).width == 64 && !tex.get(t).nearest && tex.get(t).mipmaps);
    json::Value s = json::Value::object();
    s.set("maxSize", 16);
    s.set("filter", "nearest");
    s.set("mipmaps", false);
    REQUIRE(assetdb::setImportSettings(png, s, error));
    CHECK(assetdb::importSettings(png)["maxSize"].asInt() == 16);
    uint32_t version = tex.version;
    REQUIRE(reloadTexture(tex, t, error) == t);
    CHECK(tex.get(t).width == 16 && tex.get(t).height == 8);
    CHECK(tex.get(t).nearest && !tex.get(t).mipmaps && tex.get(t).revision == 1);
    CHECK(tex.version > version);

    MeshLibrary lib = buildMeshLibrary();
    int m = importMesh(lib, obj.u8string(), error);
    REQUIRE(m >= 0);
    CHECK_NEAR(lib.get(m).boundsMax.x - lib.get(m).boundsMin.x, 1.f, 1e-4);   // fitted: largest side 1
    json::Value ms = json::Value::object();
    ms.set("fitToUnitCube", false);
    ms.set("scale", 0.5f);
    REQUIRE(assetdb::setImportSettings(obj, ms, error));
    int count = lib.count();
    REQUIRE(reloadMesh(lib, m, error) == m);
    CHECK(lib.count() == count);
    CHECK_NEAR(lib.get(m).boundsMax.x - lib.get(m).boundsMin.x, 2.f, 1e-4);   // 4 units, times 0.5
    CHECK(same(fs::u8path(lib.get(m).source), obj));
    CHECK(reloadMesh(lib, (int)MeshKind::Cube, error) < 0);

    assetdb::open(root, false);
    CHECK(!assetdb::setImportSettings(png, s, error));   // a game never writes them
}
