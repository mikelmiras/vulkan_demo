// core/project: project-relative paths, project.json, moving a project
#include "test.h"

#include <fstream>

#include "core/compress.h"

#include "app/export.h"
#include "core/vfs.h"
#include "scene/serialization.h"
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
    out = result.executable.parent_path();   // (on macOS: inside the .app bundle)
    CHECK(result.executable.filename().u8string().rfind("Sample Game", 0) == 0);
    CHECK(fs::is_regular_file(out / "project.json"));
    CHECK(fs::is_regular_file(out / "scenes" / "main.vkw"));
    CHECK(fs::is_regular_file(out / "models" / "tentacle.gltf"));
    if (!result.standalone) CHECK(fs::is_regular_file(GameModule::libraryPath(out / "bin" / "sample_game")));
    CHECK(fs::is_regular_file(out / "shaders" / "scene.frag.spv"));
    CHECK(!fs::exists(out / "src"));               // no sources
    CHECK(!fs::exists(out / "CMakeLists.txt"));
    CHECK(result.warnings.empty());
    CHECK(project::root() != sample);              // (the current project is left as it was)
    // Not over the project itself
    CHECK(!exportGame(sample, sample, platform::executableDir(), result, error));
}

#if defined(__linux__)
// Packaging: without its tool the export says which is missing (the plain export is there, nothing half-made)
TEST(export_package_needs_its_tool) {
    namespace fs = std::filesystem;
    fs::path sample = fs::u8path(VKWORLD_SAMPLE_GAME), out = test::tempPath("exported_package");
    fs::remove_all(out);
    setenv("VKWORLD_APPIMAGETOOL", "/nonexistent/appimagetool", 1);
    ExportOptions options;
    options.package = true;
    ExportResult result;
    std::string error;
    CHECK(!exportGame(sample, out, platform::executableDir(), result, error, options));
    unsetenv("VKWORLD_APPIMAGETOOL");
    CHECK(error.find("appimagetool") != std::string::npos);
    CHECK(fs::is_regular_file(out / "project.json"));
    bool leftovers = false;
    for (const auto& e : fs::directory_iterator(out))
        if (e.path().extension() == ".AppDir" || e.path().extension() == ".AppImage") leftovers = true;
    CHECK(!leftovers);
    // A program that runs, and one that isn't there
    std::string output;
    int code = -1;
    CHECK(platform::runProgram({"sh", "-c", "echo hi; exit 3"}, output, code));
    CHECK(output == "hi\n" && code == 3);
    CHECK(!platform::runProgram({"/nonexistent/tool"}, output, code));
}
#endif
#endif

// A scene using a file outside the project gets a warning: the exported game won't have it
TEST(export_warns_about_outside_assets) {
    namespace fs = std::filesystem;
    fs::path proj = test::tempPath("export_proj"), out = test::tempPath("export_out");
    fs::create_directories(proj / "scenes");
    test::writeFile(proj / "project.json", R"({"format":"vkworld-project","version":1,"name":"Tiny","startScene":"scenes/a.vkw"})");
    // An absolute path on every platform (C:/... on Windows), outside the project
    std::string outside = (fs::temp_directory_path() / "nowhere" / "rock.obj").generic_u8string();
    test::writeFile(proj / "scenes" / "a.vkw", std::string(R"({"format":"vkworld-scene","version":12,"meshes":[{"name":"Rock","source":")") +
                                                   outside + R"("}],"entities":[]})");
    ExportResult result;
    std::string error;
    REQUIRE(exportGame(proj, out, platform::executableDir(), result, error));
    REQUIRE(result.warnings.size() == 1);
    CHECK(result.warnings[0].find("rock.obj") != std::string::npos);
}

// Packs: written, mounted (on their own, or appended to an executable), read; the disk underneath still works
TEST(vfs_packs) {
    namespace fs = std::filesystem;
    fs::path dir = test::tempPath("vfs_src");
    fs::create_directories(dir / "sub");
    test::writeFile(dir / "a.txt", "alpha");
    test::writeFile(dir / "sub" / "b.txt", "bravo");
    fs::path pak = test::tempPath("test.pak");
    std::string error, text;
    REQUIRE(vfs::writePack(pak, {{"a.txt", dir / "a.txt"}, {"sub/b.txt", dir / "sub" / "b.txt"}}, false, error));
    fs::path root = test::tempPath("vfs_root");   // where the pack's files appear
    fs::create_directories(root);
    test::writeFile(root / "disk.txt", "on disk");
    REQUIRE(vfs::mount(pak, root));
    CHECK(vfs::mounted());
    CHECK(vfs::read(root / "sub" / "b.txt", text) && text == "bravo");
    CHECK(vfs::read(fs::path("a.txt"), text) && text == "alpha");   // relative: from the pack's root
    CHECK(vfs::exists(root / "a.txt"));
    CHECK(vfs::read(root / "disk.txt", text) && text == "on disk"); // not in the pack: the disk
    CHECK(!vfs::exists(root / "nope.txt"));
    vfs::unmount();
    CHECK(!vfs::exists(root / "a.txt"));

    // Appended to an "executable": found from the end of the file
    fs::path exe = test::tempPath("game.exe");
    test::writeFile(exe, std::string(10000, 'x'));
    REQUIRE(vfs::writePack(exe, {{"sub/b.txt", dir / "sub" / "b.txt"}}, true, error));
    REQUIRE(vfs::mount(exe, root));
    CHECK(vfs::read(root / "sub" / "b.txt", text) && text == "bravo");
    vfs::unmount();
    CHECK(!vfs::mount(dir / "a.txt", root));   // not a pack
}

#ifdef VKWORLD_SAMPLE_GAME
// The sample game packed: its project and scenes load from the pack alone
TEST(export_game_pack_loads) {
    namespace fs = std::filesystem;
    fs::path sample = fs::u8path(VKWORLD_SAMPLE_GAME), out = test::tempPath("exported_pack");
    ExportResult result;
    std::string error;
    ExportOptions options;
    options.layout = ExportOptions::Layout::Pack;
    REQUIRE(exportGame(sample, out, platform::executableDir(), result, error, options));
    fs::path dir = result.executable.parent_path();
    REQUIRE(fs::is_regular_file(dir / "game.pak"));
    CHECK(!fs::exists(dir / "scenes"));   // only in the pack
    REQUIRE(vfs::mount(dir / "game.pak", dir));
    project::Settings settings;
    REQUIRE(project::load(dir, settings, error));
    fs::path previous = project::root();
    project::setRoot(dir);
    MeshLibrary meshes = buildMeshLibrary();
    Scene menu, scene;
    bool loadedMenu = loadScene((dir / settings.startScene).u8string(), menu, meshes, textures(), error);   // (the menu)
    bool loaded = loadScene((dir / "scenes" / "main.vkw").u8string(), scene, meshes, textures(), error);
    project::setRoot(previous);
    vfs::unmount();
    REQUIRE(loadedMenu);
    CHECK(menu.findByName("Play") && menu.findByName("Play")->ui);
    REQUIRE(loaded);
    CHECK(scene.findByName("Coin.001") != nullptr);
    CHECK(!meshes.animations.empty());   // the tentacle model, from the pack too
}
#endif

// The pack codec: round trips of every kind of data, smaller for repetitive data, damaged input refused
TEST(lz_compression) {
    auto roundTrip = [](const std::string& in) {
        std::string packed = lz::compress(in.data(), in.size()), out;
        return lz::decompress(packed.data(), packed.size(), out, in.size()) && out == in ? packed.size() : (size_t)-1;
    };
    CHECK(roundTrip("") != (size_t)-1);
    CHECK(roundTrip("a") != (size_t)-1);
    CHECK(roundTrip("hello, hello, hello, hello!") != (size_t)-1);
    std::string text;
    for (int i = 0; i < 2000; ++i) text += "{\"name\": \"Coin." + std::to_string(i % 37) + "\", \"position\": [1, 2, 3]},\n";
    size_t packed = roundTrip(text);
    REQUIRE(packed != (size_t)-1);
    CHECK(packed < text.size() / 4);   // scene-like text shrinks a lot
    std::string noise(100000, '\0');
    uint32_t s = 12345;
    for (char& c : noise) { s = s * 1664525u + 1013904223u; c = (char)(s >> 24); }
    CHECK(roundTrip(noise) != (size_t)-1);   // incompressible: still right
    std::string runs(70000, 'x');           // long runs: overlapping matches, long lengths
    CHECK(roundTrip(runs) < 1000);
    std::string bad = lz::compress(text.data(), text.size()), out;
    bad.resize(bad.size() / 2);
    CHECK(!lz::decompress(bad.data(), bad.size(), out, text.size()));
}

TEST(vfs_compressed_packs) {
    auto dir = test::tempPath("lzpack");
    std::filesystem::create_directories(dir);
    std::string text;
    for (int i = 0; i < 5000; ++i) text += "line " + std::to_string(i % 50) + " of a fairly repetitive scene file\n";
    std::ofstream(dir / "scene.vkw", std::ios::binary) << text;
    std::string noise(20000, '\0');
    uint32_t s = 99;
    for (char& c : noise) { s = s * 1664525u + 1013904223u; c = (char)(s >> 24); }
    std::ofstream(dir / "image.png", std::ios::binary) << noise;
    std::string error;
    auto pak = dir / "game.pak";
    REQUIRE(vfs::writePack(pak, {{"scene.vkw", dir / "scene.vkw"}, {"image.png", dir / "image.png"}}, false, error));
    CHECK(std::filesystem::file_size(pak) < text.size() / 2 + noise.size() + 1000);   // the text shrank, the noise didn't grow
    REQUIRE(vfs::mount(pak, dir / "game"));
    std::string got;
    REQUIRE(vfs::read("scene.vkw", got));
    CHECK(got == text);
    REQUIRE(vfs::read("image.png", got));
    CHECK(got == noise);
    vfs::unmount();
    // Uncompressed packs (and old ones) read the same
    REQUIRE(vfs::writePack(pak, {{"scene.vkw", dir / "scene.vkw"}}, false, error, false));
    CHECK(std::filesystem::file_size(pak) > text.size());
    REQUIRE(vfs::mount(pak, dir / "game"));
    REQUIRE(vfs::read("scene.vkw", got));
    CHECK(got == text);
    vfs::unmount();
}

// Icons: a PNG's size from its header, wrapped as .ico and .icns
TEST(export_icon_formats) {
    std::ifstream in(std::string(VKWORLD_SAMPLE_GAME) + "/icon.png", std::ios::binary);
    std::string png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    uint32_t w = 0, h = 0;
    REQUIRE(pngSize(png, w, h));
    CHECK(w == 128 && h == 128);
    CHECK(!pngSize("not a png at all, just text", w, h));
    std::string ico = makeIco(png);
    REQUIRE(ico.size() == 22 + png.size());
    CHECK(ico[2] == 1 && ico[4] == 1);                 // an icon, one image
    CHECK((uint8_t)ico[6] == 128);                     // its width
    CHECK(ico.compare(22, png.size(), png) == 0);      // the PNG as it is, after the directory
    std::string icns = makeIcns(png);
    REQUIRE(icns.size() == 16 + png.size());
    CHECK(icns.compare(0, 4, "icns") == 0 && icns.compare(8, 4, "ic07") == 0);   // 128 px
    CHECK((uint8_t)icns[7] == (uint8_t)icns.size() && (uint8_t)icns[6] == (uint8_t)(icns.size() >> 8));
    CHECK(makeIco("nope").empty() && makeIcns("nope").empty());
}
