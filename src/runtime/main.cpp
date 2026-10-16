// The standalone game: plays a scene without the editor.
//
//   vkgame [scene.vkw | project folder | --stress] [--fullscreen] [--language xx] [--aa msaa|taa|off]
//          [--host [port] | --join address[:port] | --join-lobby CODE] [--relay host[:port]] [--lobby "name"]
//          [--max-players N] [--screenshot out.png [--frames N]] [--trace out.json] [--lua-debug [port]]
//          [--benchmark [frames] [--benchmark-out results.json]]   (times the GPU's passes, the camera turning: exits)
//          [--check]   (loads everything but opens no window: for testing an exported game anywhere)
//          [--server [port] [--seconds N]]   (a dedicated multiplayer server: no window, no GPU; N: stop after)
//          [--lobbies]   (lists the game's lobbies on the relay, and exits)
//          [--install-update]   (checks the project's updateUrl, installs a newer version if there is one, and exits)
//   vkgame --update-keygen key.txt | --sign-manifest manifest.json key.txt | --merge-manifests out.json a.json ...
//          (signing update manifests: app/updates.h)
//
// Without a scene it plays the start scene of the project (a folder with project.json: the one given, the working
// directory or the executable's folder), then scene.vkw in those folders, and falls back to the built-in demo. The mouse is captured for the game: Esc releases it, a click takes it back.
// F11 toggles fullscreen.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#include "app/application.h"
#include "app/profiler_view.h"
#include "core/crash.h"
#include "core/json.h"
#include "script/lua_debugger.h"
#include "physics/fracture.h"
#include "scene/water.h"
#include "physics/soft_body.h"
#include "core/log.h"
#include "core/profiler.h"
#include "audio/audio.h"
#include "core/project.h"
#include "core/vfs.h"
#include "assets/meshes.h"
#include "game/game.h"
#include "script/lua_script.h"
#include "assets/asset_db.h"
#include "net/crypto.h"
#include "net/net_session.h"
#include "net/relay.h"
#include "scene/terrain.h"
#include "render/environment.h"
#include "game/module.h"
#include "imgui.h"
#include "app/crash_reports.h"
#include "app/updates.h"
#include "platform/platform.h"
#include "render/scene_draw.h"
#include "render/gpu_benchmark.h"
#include "scene/serialization.h"
#include "scene/stress_scene.h"

#ifdef VKWORLD_STANDALONE_MODULE
extern "C" void vkworldModuleRegister();   // VKWORLD_GAME_MODULE() in the game's sources
#endif

namespace {

namespace fs = std::filesystem;

// Picks the project (setting project::root) and returns the scene to play, or "" for the demo
std::string findScene(int argc, char** argv) {
    std::error_code ec;
    if (argc > 1 && !fs::is_directory(argv[1], ec)) {   // a scene file: its project is the folder above it with one
        fs::path file = fs::absolute(fs::u8path(argv[1]), ec);
        for (fs::path dir = file.parent_path(); !dir.empty(); dir = dir.parent_path()) {
            if (project::exists(dir)) { project::setRoot(dir); break; }
            if (dir == dir.parent_path()) break;
        }
        return file.u8string();
    }
    std::vector<fs::path> folders;
    if (argc > 1) folders.push_back(argv[1]);
    else folders = {fs::current_path(ec), platform::executableDir()};
    for (const fs::path& dir : folders) {
        project::Settings settings;
        std::string error;
        if (project::exists(dir) && project::load(dir, settings, error)) {
            project::setRoot(dir);
            return project::resolve(settings.startScene);
        }
        if (fs::exists(dir / "scene.vkw", ec)) {
            project::setRoot(dir);
            return (dir / "scene.vkw").u8string();
        }
    }
    return {};
}

std::atomic<bool> g_stopServer{false};
void onStopSignal(int) { g_stopServer = true; }

// Where a multiplayer session stands, for the game's menus (the game state's net.* values); and the lobbies last
// listed by "@lobbies"
void publishNetwork(Game& game, const NetSession& net, const relay::LobbyQuery& query, const std::vector<relay::Lobby>& lobbies,
                    std::string& published) {
    std::string now = (net.active() ? net.status() : "") + "|" + net.lobbyCode() + "|" + (query.busy() ? "1" : "0") + query.error();
    for (const relay::Lobby& l : lobbies) now += "|" + l.code + std::to_string(l.players);
    if (now == published) return;   // (only when something changed)
    published = now;
    GameState& st = game.state();
    st.set("net.active", json::Value(net.active()));
    st.set("net.hosting", json::Value(net.active() && net.isServer()));
    st.set("net.connected", json::Value(net.connected()));
    st.set("net.refused", json::Value(net.refused()));
    st.set("net.status", json::Value(net.active() ? net.status() : std::string("Offline")));
    st.set("net.code", json::Value(net.lobbyCode()));
    st.set("net.searching", json::Value(query.busy()));
    st.set("net.lobbiesError", json::Value(query.error()));
    st.set("net.lobbies", json::Value((double)lobbies.size()));
    for (size_t i = 0; i < lobbies.size(); ++i) {
        std::string k = "net.lobby." + std::to_string(i) + ".";
        st.set(k + "code", json::Value(lobbies[i].code));
        st.set(k + "name", json::Value(lobbies[i].name));
        st.set(k + "players", json::Value(std::to_string(lobbies[i].players) + " / " + std::to_string(lobbies[i].maxPlayers)));
        st.set(k + "locked", json::Value(lobbies[i].password));
    }
}

// A multiplayer game's options: its password (--password), whether its lobby is listed (--private: not), the relay's
// secret (--relay-secret, else project.json's)
struct NetOptions {
    std::string password, relaySecret;
    bool listed = true;
    void apply(NetSession& net) const {
        net.setPassword(password);
        net.setRelaySecret(relaySecret);
    }
};

// --server: the game alone, without a window or a GPU, as a dedicated server (no player of its own; a lobby on the
// relay when there is one), at a steady 60 updates a second until Ctrl+C / SIGTERM (or `seconds`)
int runServer(Game& game, Scene& scene, MeshLibrary& meshes, const std::string& path, uint16_t port,
              const std::string& relayAddress, const std::string& lobbyGame, const std::string& lobbyName, int maxPlayers,
              const NetOptions& options, double seconds) {
    if (path.empty()) {
        logLine("server: no scene to serve (a project or a scene file)");
        return 1;
    }
    ImGui::CreateContext();   // (behaviours and the input map read ImGui's state: an empty one, never drawn)
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.IniFilename = nullptr;
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    game.start(scene, meshes);
    game.loadScene(project::toStored(path));
    game.finishLoading();
    if (!game.loadError().empty()) {
        logLine("server: %s", game.loadError().c_str());
        ImGui::DestroyContext();
        return 1;
    }
    NetSession net;
    options.apply(net);
    std::string error;
    if (!net.host(game, port, error, true)) {
        logLine("server: %s", error.c_str());
        ImGui::DestroyContext();
        return 1;
    }
    if (!relayAddress.empty() && !net.advertise(relayAddress, lobbyGame, lobbyName, maxPlayers, error, options.listed))
        logLine("server: the relay: %s", error.c_str());
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);
    const auto step = std::chrono::microseconds(16667);
    auto next = std::chrono::steady_clock::now();
    std::string said;
    double elapsed = 0.0;
    while (!g_stopServer && !game.quitRequested() && (seconds <= 0.0 || elapsed < seconds)) {
        const float dt = 1.f / 60.f;
        io.DeltaTime = dt;
        ImGui::NewFrame();
        game.frame(dt, false, false, ImVec2(0, 0));
        ImGui::EndFrame();
        net.update(dt);
        terrain::syncMeshes(scene, meshes);   // (pieces and soft bodies need their meshes here too)
        softbody::syncMeshes(scene, meshes);
        fracture::syncMeshes(scene, meshes);
        water::syncMeshes(scene, meshes);
        scripting::debugger::poll();
        if (net.status() != said) {   // (what's going on, when it changes)
            said = net.status();
            logLine("server: %s", said.c_str());
        }
        elapsed += dt;
        next += step;
        std::this_thread::sleep_until(next);
        if (std::chrono::steady_clock::now() - next > std::chrono::milliseconds(250)) next = std::chrono::steady_clock::now();   // (fell behind: no rush to catch up)
    }
    logLine("server: stopping");
    net.stop();
    game.stop();
    ImGui::DestroyContext();
    return 0;
}

// --install-update: the check, then (if a newer version is out for this platform) the install, waited for; 0 when up
// to date or installed
// Where an update goes: the game's folder (the one with the .app on macOS), or, run from an AppImage, the AppImage's
// (the AppImage file itself replaced: renameTo is its name)
struct UpdateTarget {
    std::filesystem::path dir;
    std::string renameTo;
};
UpdateTarget updateTarget() {
    std::filesystem::path appImage = updates::appImagePath();
    if (!appImage.empty()) return {appImage.parent_path(), appImage.filename().u8string()};
    std::filesystem::path dir = platform::executableDir();
    if (dir.parent_path().filename() == "Contents" && dir.parent_path().parent_path().extension() == ".app")
        dir = dir.parent_path().parent_path().parent_path();
    return {dir, ""};
}

// --update-keygen key.txt: a new signing key for update manifests (the secret into the file; the public key printed);
// --sign-manifest manifest.json key.txt; --merge-manifests out.json a.json b.json ... (each platform's downloads)
int updateTool(const std::string& verb, const std::vector<std::string>& args) {
    std::string error;
    auto load = [&](const std::string& path, json::Value& doc) {
        std::ifstream f(std::filesystem::u8path(path));
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return f.good() || !text.empty() ? json::parse(text, doc, error) : (error = "cannot read " + path, false);
    };
    if (verb == "--update-keygen") {
        crypto::SigningKey key = crypto::SigningKey::generate();
        std::ofstream f(std::filesystem::u8path(args[0]));
        f << crypto::toHex(key.seed.data(), 32) << "\n";
        if (!f) {
            std::fprintf(stderr, "update-keygen: cannot write %s\n", args[0].c_str());
            return 1;
        }
        std::printf("secret key: %s (keep it out of the project and out of version control)\n", args[0].c_str());
        std::printf("public key: %s\n(put it in project.json as \"updateKey\")\n", crypto::toHex(key.publicKey.data(), 32).c_str());
        return 0;
    }
    if (verb == "--sign-manifest") {
        json::Value doc;
        std::string key;
        std::ifstream(std::filesystem::u8path(args[1])) >> key;
        if (!load(args[0], doc) || !updates::signManifest(doc, key, error)) {
            std::fprintf(stderr, "sign-manifest: %s\n", error.c_str());
            return 1;
        }
        std::ofstream(std::filesystem::u8path(args[0])) << json::write(doc);
        std::printf("signed %s\n", args[0].c_str());
        return 0;
    }
    // --merge-manifests: the first one's version, url and notes, everyone's downloads (unsigned: sign it after)
    json::Value out;
    for (size_t i = 1; i < args.size(); ++i) {
        json::Value doc;
        if (!load(args[i], doc)) {
            std::fprintf(stderr, "merge-manifests: %s: %s\n", args[i].c_str(), error.c_str());
            return 1;
        }
        if (out.isNull()) {
            out = json::Value::object();
            for (const char* k : {"version", "url", "notes"}) out.set(k, doc[k]);
            out.set("downloads", json::Value::object());
        }
        for (const auto& [platform, d] : doc["downloads"].members()) out.members().back().second.set(platform, d);
    }
    std::ofstream(std::filesystem::u8path(args[0])) << json::write(out);
    std::printf("merged %zu manifests into %s\n", args.size() - 1, args[0].c_str());
    return 0;
}

int installUpdateNow(const project::Settings& settings) {
    if (settings.updateUrl.empty()) {
        std::fprintf(stderr, "update: the project has no updateUrl\n");
        return 1;
    }
    updates::Check check;
    check.start(settings.updateUrl, settings.gameVersion, nullptr, settings.updateKey);
    while (!check.ready()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!check.error().empty()) {
        std::fprintf(stderr, "update: %s\n", check.error().c_str());
        return 1;
    }
    if (!check.newer()) {
        std::printf("update: %s %s is the latest\n", settings.name.c_str(), settings.gameVersion.c_str());
        return 0;
    }
    updates::Release r = check.release();
    UpdateTarget target = updateTarget();
    std::filesystem::path dir = target.dir;
    updates::cleanUp(dir);   // (the last update's leftovers)
    std::printf("update: installing %s %s into %s\n", settings.name.c_str(), r.version.c_str(), dir.u8string().c_str());
    updates::Install install;
    install.start(r, dir, nullptr, target.renameTo);
    for (;;) {
        updates::Install::Stage st = install.stage();
        if (st == updates::Install::Stage::Ready || st == updates::Install::Stage::Failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::string error;
    if (!install.error().empty() || !install.apply(error)) {
        std::fprintf(stderr, "update: %s\n", install.error().empty() ? error.c_str() : install.error().c_str());
        return 1;
    }
    std::printf("update: %s %s installed\n", settings.name.c_str(), r.version.c_str());
    return 0;
}

// --lobbies: the game's lobbies on the relay, printed (CODE, players, name)
int listLobbies(const std::string& relayAddress, const std::string& game) {
    if (relayAddress.empty()) {
        std::fprintf(stderr, "lobbies: no relay (project.json's \"relay\", or --relay host[:port])\n");
        return 1;
    }
    relay::LobbyQuery query;
    std::string error;
    if (!query.start(relayAddress, game, error)) {
        std::fprintf(stderr, "lobbies: %s\n", error.c_str());
        return 1;
    }
    while (!query.done()) {
        query.update(0.01f);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!query.error().empty()) {
        std::fprintf(stderr, "lobbies: %s\n", query.error().c_str());
        return 1;
    }
    std::printf("%zu lobbies of %s on %s\n", query.lobbies().size(), game.c_str(), relayAddress.c_str());
    for (const relay::Lobby& l : query.lobbies())
        std::printf("  %s  %d/%d  %s%s\n", l.code.c_str(), l.players, l.maxPlayers, l.name.c_str(), l.password ? " (password)" : "");
    return 0;
}

// Messages logged by behaviours (fading out) and the mouse hint
void drawHud(const Game& game, bool captured, float scale, bool shipping) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float lineH = ImGui::GetTextLineHeightWithSpacing(), y = io.DisplaySize.y - 12.f * scale;
    int shown = 0;
    for (auto it = game.log().rbegin(); it != game.log().rend() && shown < 6; ++it, ++shown) {
        float age = game.time() - it->first;
        if (age > 6.f) break;
        int alpha = (int)(255.f * std::min(6.f - age, 1.f));
        y -= lineH;
        dl->AddText(ImVec2(13.f * scale, y + 1), IM_COL32(0, 0, 0, alpha / 2), it->second.c_str());
        dl->AddText(ImVec2(12.f * scale, y), IM_COL32(255, 235, 140, alpha), it->second.c_str());
    }
    char top[128];
    if (shipping && captured) return;   // (a shipping game: no frame counter, only the hint when the mouse is free)
    if (shipping) std::snprintf(top, sizeof(top), "click to play, Esc releases the mouse");
    else std::snprintf(top, sizeof(top), "%.0f FPS  |  update %.2f ms%s", io.Framerate, game.updateMs(),
                       captured ? "" : "   |   click to play, Esc releases the mouse");
    dl->AddText(ImVec2(13.f * scale, 11.f * scale), IM_COL32(0, 0, 0, 140), top);
    dl->AddText(ImVec2(12.f * scale, 10.f * scale), IM_COL32(255, 255, 255, 200), top);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && (!std::strcmp(argv[1], "--update-keygen") || (!std::strcmp(argv[1], "--sign-manifest") && argc >= 4) ||
                      (!std::strcmp(argv[1], "--merge-manifests") && argc >= 4)))
        return updateTool(argv[1], std::vector<std::string>(argv + 2, argv + argc));
    std::vector<std::string> launchArgs(argv, argv + argc);   // (to start again the same way after an update)
    Application::Options defaults;
    defaults.title = "Vulkan World";
    defaults.hiDpi = false;   // (a game on a Retina Mac: a point per pixel, the frame rate before sharpness)
    Application::Options options = Application::parseArgs(argc, argv, defaults);

    MeshLibrary meshes = buildMeshLibrary();
    TextureLibrary textures = buildTextureLibrary();
    // A packed game: its files in a pack appended to this executable, or in game.pak next to it
    if (vfs::mount(platform::executablePath(), platform::executableDir()) ||
        vfs::mount(platform::executableDir() / "game.pak", platform::executableDir()))
        std::printf("Playing from a pack\n");
    std::string language;   // --language es: play in another language (strings/es.json) than the project's
    bool check = false;     // --check: load the game (pack, project, module, start scene) without a window, and exit
    std::string aa;         // --aa msaa | taa | off: anti-aliasing (the renderer's default: MSAA)
    std::string host, join; // --host [port] / --join address[:port]: a multiplayer game (net/net_session.h)
    std::string joinLobby;  // --join-lobby CODE: a game listed on the relay (net/relay.h)
    std::string relayAddress, lobbyName;   // --relay host[:port] (else project.json's), --lobby "name" (what it's listed as)
    int maxPlayers = 16;    // --max-players N: what a lobby says it takes
    NetOptions netOptions;  // --password P, --private, --relay-secret S
    std::string server;     // --server [port]: a dedicated server, headless
    double serverSeconds = 0.0;   // --seconds N: ... stopping after N seconds (tests)
    bool lobbies = false;   // --lobbies: list the relay's lobbies of this game and exit
    bool installUpdate = false;   // --install-update: install a newer version (app/updates.h) and exit
    std::string trace;      // --trace out.json: every frame's profile, saved as a Chrome trace on exit
    int benchmarkFrames = 0;    // --benchmark [frames]: the GPU's passes timed (render/gpu_benchmark.h), then out
    std::string benchmarkOut;   // --benchmark-out results.json
    int luaDebugPort = 0;   // --lua-debug [port]: the Lua debugger listens there (script/lua_debugger.h)
    {
        int out = 1;
        for (int i = 1; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--language") && i + 1 < argc) language = argv[++i];
            else if (!std::strcmp(argv[i], "--aa") && i + 1 < argc) aa = argv[++i];
            else if (!std::strcmp(argv[i], "--host"))
                host = i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]) ? argv[++i] : "27015";
            else if (!std::strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
            else if (!std::strcmp(argv[i], "--join-lobby") && i + 1 < argc) joinLobby = argv[++i];
            else if (!std::strcmp(argv[i], "--relay") && i + 1 < argc) relayAddress = argv[++i];
            else if (!std::strcmp(argv[i], "--lobby") && i + 1 < argc) lobbyName = argv[++i];
            else if (!std::strcmp(argv[i], "--max-players") && i + 1 < argc) maxPlayers = std::max(std::atoi(argv[++i]), 1);
            else if (!std::strcmp(argv[i], "--password") && i + 1 < argc) netOptions.password = argv[++i];
            else if (!std::strcmp(argv[i], "--relay-secret") && i + 1 < argc) netOptions.relaySecret = argv[++i];
            else if (!std::strcmp(argv[i], "--private")) netOptions.listed = false;
            else if (!std::strcmp(argv[i], "--server"))
                server = i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]) ? argv[++i] : "27015";
            else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) serverSeconds = std::atof(argv[++i]);
            else if (!std::strcmp(argv[i], "--lobbies")) lobbies = true;
            else if (!std::strcmp(argv[i], "--install-update")) installUpdate = true;
            else if (!std::strcmp(argv[i], "--check")) check = true;
            else if (!std::strcmp(argv[i], "--lua-debug"))
                luaDebugPort = i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]) ? std::atoi(argv[++i]) : 4711;
            else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
            else if (!std::strcmp(argv[i], "--benchmark"))
                benchmarkFrames = i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]) ? std::max(std::atoi(argv[++i]), 10) : 300;
            else if (!std::strcmp(argv[i], "--benchmark-out") && i + 1 < argc) benchmarkOut = argv[++i];
            else argv[out++] = argv[i];
        }
        argc = out;
    }
    GameModule module;   // declared before the scene: outlives the behaviours it made
    Scene scene;
    bool stress = argc > 1 && !std::strcmp(argv[1], "--stress");
    std::string path = stress ? "" : findScene(argc, argv), error;
    // The project's own behaviours: before the scene loads, so it finds them
    project::Settings settings;
    bool haveProject = project::exists(project::root()) && project::load(project::root(), settings, error);
#ifdef VKWORLD_STANDALONE_MODULE
    vkworldModuleRegister();   // the module's code is compiled into this executable
    (void)haveProject;
#else
    if (haveProject && !settings.module.empty()) {
        fs::path lib = GameModule::libraryPath(fs::u8path(project::resolve(settings.module)));
        if (module.load(lib, error)) std::printf("Game module %s: %zu behaviours\n", lib.u8string().c_str(), module.behaviourTypes().size());
        else std::fprintf(stderr, "warning: %s\n", error.c_str());
    }
#endif
    if (haveProject) assetdb::open(project::root(), false);   // (asset ids: files moved since a scene was saved)
    // A shipping build (app/export.h: build.json in its data): the developer tools are off
    bool shipping = false;
    {
        std::string text, jsonError;
        json::Value build;
        if (vfs::read(project::root() / "build.json", text) && json::parse(text, build, jsonError)) shipping = build["shipping"].asBool(false);
    }
    if (shipping) {
        if (luaDebugPort > 0) logLine("shipping build: no Lua debugger");
        if (!trace.empty()) logLine("shipping build: no traces");
        luaDebugPort = 0;
        trace.clear();
        options.shipping = true;
    }
    if (haveProject) {   // its Lua behaviours (scripts/), also before the scene
        int scripts = scripting::loadProject(project::root());
        if (scripts) std::printf("Lua scripts: %d behaviours\n", scripts);
        for (const std::string& e : scripting::errors()) std::fprintf(stderr, "warning: script: %s\n", e.c_str());
    }
    error.clear();
    if (check) {   // everything a player's machine needs to start the game, short of a GPU
        if (path.empty()) {
            std::printf("check: no project or scene found (the built-in demo would play)\n");
            return stress ? 0 : 1;
        }
        Scene checked;
        if (!loadScene(path, checked, meshes, textures, error)) {
            logLine("check failed: %s", error.c_str());
            return 1;
        }
        int missing = 0, behaviours = 0;
        for (const Entity& e : checked.entities)
            for (const BehaviourPtr& b : e.behaviours) {
                ++behaviours;
                if (dynamic_cast<const MissingBehaviour*>(b.get())) {
                    ++missing;
                    std::printf("check: behaviour %s on %s has no code\n", b->typeName(), e.name.c_str());
                }
            }
        std::printf("check: %s: %zu entities, %d behaviours, %zu meshes, %d textures, %s%s\n", path.c_str(),
                    checked.entities.size(), behaviours, (size_t)meshes.count(), textures.count(),
                    vfs::mounted() ? "from a pack" : "from files", missing ? ": MISSING CODE" : ": ok");
        for (const std::string& e : scripting::errors()) std::printf("check: script error: %s\n", e.c_str());
        return missing || !scripting::errors().empty() ? 1 : 0;
    }
    if (relayAddress.empty()) relayAddress = settings.relay;
    if (netOptions.relaySecret.empty()) netOptions.relaySecret = settings.relaySecret;
    if (lobbyName.empty()) lobbyName = settings.name;
    if (lobbies) return listLobbies(relayAddress, settings.name);
    if (installUpdate) return installUpdateNow(settings);
    if (stress) {
        scene = buildStressScene();
    } else if (path.empty()) {
        scene = buildDefaultScene();
    } else if (!vfs::exists(fs::u8path(path))) {   // (it loads once the window is up, behind the loading screen)
        logLine("fatal: cannot open %s", path.c_str());
        return 1;
    }
    // The log: in the user's data folder, next to the save games
    logfile::open(project::saveDir(settings.name.empty() ? "Untitled" : settings.name).parent_path() / "game.log");
    crash::install(project::saveDir(settings.name.empty() ? "Untitled" : settings.name).parent_path(), "game");
    crash::setProduct((settings.name.empty() ? std::string("Untitled") : settings.name) + " " + settings.gameVersion);
    if (!crash::previousReport().empty()) logLine("The last run crashed: %s", crash::previousReport().u8string().c_str());
    std::printf("Playing %s\n", stress ? "the stress test scene" : path.empty() ? "the built-in demo scene" : path.c_str());
    logfile::write("Playing " + (stress ? std::string("the stress test scene") : path.empty() ? std::string("the built-in demo scene") : path));

    if (!server.empty()) {   // (a dedicated server: the game without a window, then out)
        Game game;
        game.setAssetLibraries(meshes, textures);
        game.setLanguage(language.empty() ? settings.language : language);
        game.setScenePath(path.empty() ? "" : project::toStored(path));
        game.setSaveDirectory(project::saveDir(settings.name.empty() ? "Untitled" : settings.name));
        if (luaDebugPort > 0) {
            std::string debugError;
            if (!scripting::debugger::listen((uint16_t)luaDebugPort, debugError)) logLine("lua debugger: %s", debugError.c_str());
        }
        return runServer(game, scene, meshes, path, (uint16_t)std::atoi(server.c_str()), relayAddress, settings.name, lobbyName,
                         maxPlayers, netOptions, serverSeconds);
    }

    options.pipelineCache = project::saveDir(settings.name.empty() ? "Untitled" : settings.name).parent_path() / "pipelines.cache";
    Application app;
    if (!app.init(options, meshes, textures)) return 1;
    if (!trace.empty()) profiler::startRecording();
    if (luaDebugPort > 0) {
        std::string error;
        if (!scripting::debugger::listen((uint16_t)luaDebugPort, error)) logLine("lua debugger: %s", error.c_str());
    }
    const bool aaGiven = !aa.empty() || std::getenv("VKWORLD_AA");   // (--aa / VKWORLD_AA: over the settings)
    if (aa.empty() && !std::getenv("VKWORLD_AA")) aa = settings.antialiasing;   // (the project's, "": the renderer's)
    if (aa == "taa") app.renderer().setAntialiasing(Renderer::Antialiasing::Temporal);
    else if (aa == "off") app.renderer().setAntialiasing(Renderer::Antialiasing::Off);
    else if (aa == "msaa") app.renderer().setAntialiasing(Renderer::Antialiasing::Msaa);
    if (!settings.icon.empty()) {   // the project's icon on the window (and the task bar / Dock)
        TextureLibrary icon;
        std::string iconError;
        int id = loadTexture(icon, project::resolve(settings.icon), iconError);
        if (id >= 0) platform::setWindowIcon(icon.get(id).rgba.data(), icon.get(id).width, icon.get(id).height);
        else logLine("icon: %s", iconError.c_str());
    }
    AudioEngine audio;
    if (!audio.init()) std::fprintf(stderr, "%s\n", audio.error().c_str());
    Game game;
    game.setAudio(&audio);
    game.setGamepadOutput({[](int pad, float low, float high, float seconds) { platform::gamepadRumble(pad, low, high, seconds); },
                           [](Vec3 c) { platform::setGamepadLight(c.x, c.y, c.z); }});
    game.setUrlOpener([](const std::string& url) { platform::openWithSystem(fs::u8path(url)); });
    game.setAssetLibraries(meshes, textures);
    game.setLanguage(language.empty() ? settings.language : language);
    game.setAsyncLoading(true);   // scene changes load in the background, behind a loading screen
    game.setScenePath(path.empty() ? "" : project::toStored(path));
    game.setSaveDirectory(project::saveDir(settings.name.empty() ? "Untitled" : settings.name));
    game.start(scene, meshes);   // (empty until the scene file has loaded)
    if (!path.empty()) {
        game.loadScene(project::toStored(path));
        if (!options.screenshotPath.empty()) game.finishLoading();   // screenshots show the scene, not the loading
    }
    if (options.screenshotPath.empty()) platform::setCursorCaptured(true);

    GameUI ui;
    ui.setTextureLookup(app.uiTextures(textures));
    bool uiWantsCursor = false;   // the game's UI had a menu up last frame: keep the pointer free

    // Is there a newer version of this game? (asked once, in the background; menus show update.* when there is)
    updates::Check updateCheck;
    if (!settings.updateUrl.empty() && options.screenshotPath.empty() && game.state().flag("settings.checkUpdates"))
        updateCheck.start(settings.updateUrl, settings.gameVersion, nullptr, settings.updateKey);
    // ... and installing it from the game's menus ("@update", then "@restart"), into the folder the game is in (the
    // one with the .app on macOS); the last update's leftovers go now
    UpdateTarget updateTo = updateTarget();
    std::filesystem::path installDir = updateTo.dir;
    if (!updates::appImagePath().empty()) launchArgs[0] = updates::appImagePath().u8string();   // (the AppImage again)
    updates::cleanUp(installDir);
    updates::Release outRelease;   // (what the check found, when it's newer)
    updates::Install install;
    // Crash reports: the last run's, sent if the player always agrees (settings.sendCrashReports) or asks to now
    crashreports::Upload crashUpload;
    std::filesystem::path lastCrash = crash::previousReport();
    if (!lastCrash.empty()) {
        game.state().set("crash.lastRun", json::Value(true));
        if (game.state().flag("settings.sendCrashReports") && !settings.crashReportUrl.empty())
            crashUpload.start(settings.crashReportUrl, lastCrash);
    }
    std::string hostPublished = "-";
    game.setHostHandler([&](const std::string& verb, const std::string&) {
        std::string hostError;
        if (verb == "update") {
            if (outRelease.version.empty()) logLine("update: no newer version is out");
            else if (install.stage() == updates::Install::Stage::Idle || install.stage() == updates::Install::Stage::Failed)
                install.start(outRelease, installDir, nullptr, updateTo.renameTo);
        } else if (verb == "restart") {
            if (!install.apply(hostError)) logLine("update: %s", hostError.c_str());
            else if (!platform::startProgram(launchArgs)) logLine("update: installed, but the game couldn't start again: start it yourself");
            else game.context().quit();
        } else if (verb == "sendcrashreport") {
            if (!lastCrash.empty() && !crashUpload.busy()) crashUpload.start(settings.crashReportUrl, lastCrash);
        } else if (verb == "dismisscrashreport") {
            game.state().set("crash.lastRun", json::Value(false));
        }
    });

    NetSession net;
    // The game's menus can host, join and list lobbies (UI actions: Game::setNetworkHandler)
    relay::LobbyQuery lobbyQuery;
    std::vector<relay::Lobby> lobbyList;
    std::string netPublished = "-";
    auto startHosting = [&](uint16_t port) {
        std::string netError;
        netOptions.apply(net);
        if (!net.host(game, port, netError)) logLine("multiplayer: %s", netError.c_str());
        else if (!relayAddress.empty() &&
                 !net.advertise(relayAddress, settings.name, lobbyName, maxPlayers, netError, netOptions.listed))
            logLine("multiplayer: the relay: %s", netError.c_str());
    };
    game.setNetworkHandler([&](const std::string& verb, const std::string& arg) {
        std::string netError;
        bool ok = true;
        if (verb == "password") netOptions.password = arg;   // (for the next host or join)
        else if (verb == "private") netOptions.listed = arg != "1" && arg != "true";
        else if (verb == "host") startHosting(arg.empty() ? NetSession::kDefaultPort : (uint16_t)std::atoi(arg.c_str()));
        else if (verb == "join") ok = (netOptions.apply(net), net.join(game, arg, netError));
        else if (verb == "joinlobby") ok = !relayAddress.empty() ? (netOptions.apply(net), net.joinLobby(game, relayAddress, arg, netError))
                                                                 : (netError = "no relay to find lobbies on", false);
        else if (verb == "leave") net.stop();
        else if (verb == "lobbies") ok = !relayAddress.empty() ? lobbyQuery.start(relayAddress, settings.name, netError)
                                                               : (netError = "no relay to find lobbies on", false);
        if (!ok) logLine("multiplayer: %s", netError.c_str());
    });
    float dt = 0.f;
    // The benchmark's run, and the first run's choice of quality (unless asked for: VKWORLD_QUALITY, a saved choice,
    // VKWORLD_AUTO_QUALITY=0, a screenshot)
    constexpr int kBenchmarkWarmup = 30;
    int frameNumber = 0;
    auto frameStart = std::chrono::steady_clock::now();
    std::optional<CameraView> benchmarkCamera;
    GpuBenchmark benchmark, qualityProbe;
    const char* autoEnv = std::getenv("VKWORLD_AUTO_QUALITY");
    bool autoQuality = benchmarkFrames == 0 && options.screenshotPath.empty() && !std::getenv("VKWORLD_QUALITY") &&
                       !(autoEnv && !std::strcmp(autoEnv, "0")) && !game.state().flag("settings.qualityChosen");
    if (autoQuality) game.state().setText("settings.quality", "high");   // (measured at high)
    bool firstLoad = !path.empty();
    bool showProfiler = false;   // F3
    while (!game.quitRequested() && app.beginFrame(dt)) {
        if (updateCheck.ready()) {
            if (updateCheck.newer()) {
                updates::Release r = updateCheck.release();
                outRelease = r;
                game.state().set("update.installable", json::Value(r.chunked() || (!r.download.empty() && r.sha256.size() == 64)));
                game.state().set("update.available", json::Value(true));
                game.state().set("update.version", json::Value(r.version));
                game.state().set("update.url", json::Value(r.url));
                game.state().set("update.notes", json::Value(r.notes));
                logLine("An update is out: %s %s (%s)", settings.name.c_str(), r.version.c_str(), r.url.c_str());
            } else if (!updateCheck.error().empty()) {
                logLine("update check: %s", updateCheck.error().c_str());
            }
            updateCheck = updates::Check{};   // (once)
        }
        if (firstLoad && !game.loading()) {   // the start scene is in, or didn't load: that's fatal
            firstLoad = false;
            if (!game.loadError().empty()) {
                logLine("fatal: %s", game.loadError().c_str());
                app.shutdown();
                return 1;
            }
        }
        if (!firstLoad && !net.active() && (!host.empty() || !join.empty() || !joinLobby.empty())) {   // (once the scene is in)
            std::string netError;
            bool ok = true;
            if (!host.empty()) startHosting((uint16_t)std::atoi(host.c_str()));
            else if (!join.empty()) ok = (netOptions.apply(net), net.join(game, join, netError));
            else if (relayAddress.empty()) ok = (netError = "--join-lobby needs a relay (--relay, or project.json's)", false);
            else ok = (netOptions.apply(net), net.joinLobby(game, relayAddress, joinLobby, netError));
            if (!ok) logLine("multiplayer: %s", netError.c_str());
            host.clear();
            join.clear();
            joinLobby.clear();
        }
        ImGuiIO& io = ImGui::GetIO();
        if (platform::cursorCaptured() && (uiWantsCursor || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            platform::setCursorCaptured(false);
        else if (!platform::cursorCaptured() && !uiWantsCursor && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            platform::setCursorCaptured(true);
        if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) platform::setFullscreen(!platform::isFullscreen());
        if (!shipping && ImGui::IsKeyPressed(ImGuiKey_F3, false)) showProfiler = !showProfiler;

        bool captured = platform::cursorCaptured();
        float mdx = 0.f, mdy = 0.f;
        platform::takeMouseDelta(mdx, mdy);
        game.input().setMouseCaptured(captured);
        scripting::poll();   // (scripts edited on disk while it runs: reloaded, keeping their state)
        {
            PROFILE_SCOPE("Game");
            game.setGamepads(platform::gamepads());   // (local players: a pad each)
            game.frame(dt, !io.WantTextInput, true, captured ? ImVec2(mdx, mdy) : io.MouseDelta);
        }
        net.update(dt);   // (multiplayer: sends and receives after the game's updates)
        if (lobbyQuery.busy()) {
            lobbyQuery.update(dt);
            if (lobbyQuery.done()) lobbyList = lobbyQuery.lobbies();
        }
        publishNetwork(game, net, lobbyQuery, lobbyList, netPublished);
        {   // where an update being installed and a crash report being sent stand (when that changes)
            int percent = (int)(install.progress() * 100.f);
            std::string now = std::string(updates::Install::stageName(install.stage())) + "|" + install.error() + "|" +
                              std::to_string(percent) + "|" +
                              (crashUpload.busy() ? "1" : "0") + (crashUpload.sent() ? "1" : "0") + crashUpload.error();
            if (now != hostPublished) {
                hostPublished = now;
                game.state().set("update.stage", json::Value(std::string(updates::Install::stageName(install.stage()))));
                game.state().set("update.error", json::Value(install.error()));
                game.state().set("update.downloaded", json::Value((double)install.downloaded()));
                game.state().set("update.progress", json::Value((double)percent));
                game.state().set("crash.sending", json::Value(crashUpload.busy()));
                game.state().set("crash.sent", json::Value(crashUpload.sent()));
                game.state().set("crash.error", json::Value(crashUpload.error()));
                if (crashUpload.sent()) game.state().set("crash.lastRun", json::Value(false));
                if (!install.error().empty()) logLine("update: %s", install.error().c_str());
            }
        }
        scripting::debugger::poll();

        uint32_t w = 0, h = 0;
        app.windowSize(w, h);
        // The cameras: one, or split-screen players' (the first is drawn as usual in its part of the screen, the
        // others into images of their own, put in theirs under the UI)
        std::vector<CameraView> cams = game.cameras();
        auto pixels = [&](const CameraView& c) {
            return std::array<float, 4>{std::floor(c.rect[0] * (float)w), std::floor(c.rect[1] * (float)h),
                                        std::floor(c.rect[2] * (float)w), std::floor(c.rect[3] * (float)h)};
        };
        for (size_t k = 1; k < cams.size() && k <= (size_t)Renderer::kMaxViews; ++k) {
            std::array<float, 4> r = pixels(cams[k]);
            if (r[2] < 1.f || r[3] < 1.f) continue;
            ImTextureID image = app.renderer().viewTexture((int)k - 1, (uint32_t)r[2], (uint32_t)r[3]);
            if (image) ImGui::GetBackgroundDrawList()->AddImage(image, ImVec2(r[0], r[1]), ImVec2(r[0] + r[2], r[1] + r[3]));
        }
        if (cams.size() >= 2) {   // the lines between them
            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            dl->AddLine(ImVec2((float)w * 0.5f, 0.f), ImVec2((float)w * 0.5f, (float)h), IM_COL32(0, 0, 0, 255), 3.f);
            if (cams.size() > 2) dl->AddLine(ImVec2(0.f, (float)h * 0.5f), ImVec2((float)w, (float)h * 0.5f), IM_COL32(0, 0, 0, 255), 3.f);
        }
        ui.begin(ImGui::GetBackgroundDrawList(), 0.f, 0.f, (float)w, (float)h, !captured);
        game.drawUI(ui);
        ui.end();
        uiWantsCursor = ui.wantsCursor();
        drawHud(game, captured || uiWantsCursor, app.uiScale(), shipping);
        if (showProfiler) drawProfilerOverlay(app.uiScale());
        if (net.active()) {   // where the multiplayer game stands, in the top right corner
            const std::string& st = net.status();
            ImVec2 size = ImGui::CalcTextSize(st.c_str());
            ImGui::GetForegroundDrawList()->AddText(ImVec2((float)w - size.x - 12.f, 8.f), IM_COL32(255, 255, 255, 200),
                                                    st.c_str());
        }
        CameraView cam;
        if (!cams.empty()) cam = cams[0];
        else cam.view = lookAt({0, 10, 40}, {0, 0, 0}, {0, 1, 0});   // no camera: a fixed view
        if (benchmarkFrames > 0) {   // the benchmark: the camera held where it starts, turning a whole circle over the run
            if (!benchmarkCamera) benchmarkCamera = cam;
            cam = *benchmarkCamera;
            const float turn = 2.f * PI * (float)std::max(frameNumber - kBenchmarkWarmup, 0) / (float)benchmarkFrames;
            cam.view = cam.view * translate(cam.position) * rotateAxis({0.f, 1.f, 0.f}, turn) * translate(cam.position * -1.f);
        }
        std::array<float, 4> rect = pixels(cam);
        float aspect = rect[3] > 0.f ? rect[2] / rect[3] : 1.f;
        PROFILE_SCOPE("Draw and render");   // (to the end of the frame)
        std::vector<Mat4> worlds = game.worldMatrices();
        std::vector<Mat4> joints;
        terrain::syncMeshes(scene, meshes);
        softbody::syncMeshes(scene, meshes);
        fracture::syncMeshes(scene, meshes);
        water::syncMeshes(scene, meshes);
        std::vector<DrawItem> items = collectDrawItems(scene, worlds, kNoEntity, &joints, &meshes);
        std::vector<LightInfo> lights = collectLights(scene, worlds);
        std::vector<PlantSet> plants = collectPlants(scene, worlds, meshes);
        std::vector<ProbeInfo> probes = collectProbes(scene, worlds);
        std::vector<DecalInfo> decals = collectDecals(scene, worlds);
        std::vector<GpuEmitterInfo> gpuEmitters = collectGpuEmitters(scene, worlds);
        std::vector<ParticleDraw> particles;
        game.particles().collect(particles);
        SceneRenderInfo info;
        info.items = &items;
        info.plants = &plants;
        info.lights = &lights;
        info.probes = &probes;
        info.decals = &decals;
        info.gpuEmitters = &gpuEmitters;
        info.joints = &joints;
        info.particles = &particles;
        info.sky = scene.render.sky;
        info.zenith = scene.render.zenith;
        info.ground = scene.render.ground;
        info.fovY = cam.fovY;
        info.aspect = aspect;
        info.fogDensity = scene.render.fogDensity;
        info.ambient = scene.render.ambient;
        info.exposure = scene.render.exposure;
        info.bloomStrength = scene.render.bloomStrength;
        info.bloomThreshold = scene.render.bloomThreshold;
        info.aoStrength = scene.render.aoStrength;
        info.aoRadius = scene.render.aoRadius;
        info.gi = scene.render.gi;
        info.giSpacing = scene.render.giSpacing;
        info.giIntensity = scene.render.giIntensity;
        info.reflections = scene.render.reflections;
        info.ssgi = scene.render.ssgi;
        info.contactShadows = scene.render.contactShadows;
        info.ssgiIntensity = scene.render.ssgiIntensity;
        info.shadowSoftness = scene.render.shadowSoftness;
        info.volumetricFog = scene.render.volumetricFog;
        info.fogLight = scene.render.fogLight;
        info.fogAnisotropy = scene.render.fogAnisotropy;
        info.fogHeightFalloff = scene.render.fogHeightFalloff;
        info.tonemapper = scene.render.tonemapper;
        info.contrast = scene.render.contrast;
        info.saturation = scene.render.saturation;
        info.vignette = scene.render.vignette;
        info.autoExposure = scene.render.autoExposure;
        info.exposureMin = scene.render.exposureMin;
        info.exposureMax = scene.render.exposureMax;
        info.adaptationSpeed = scene.render.adaptationSpeed;
        info.localToneMapping = scene.render.localToneMapping;
        info.lensDirt = scene.render.lensDirt;
        info.lightShafts = scene.render.lightShafts;
        info.wetness = scene.render.wetness;
        info.puddles = scene.render.puddles;
        info.rain = scene.render.rain;
        info.physicalSky = scene.render.physicalSky;
        info.timeOfDay = scene.render.timeOfDay;
        info.latitude = scene.render.latitude;
        info.haze = scene.render.haze;
        info.aerialPerspective = scene.render.aerialPerspective;
        info.cloudCover = scene.render.cloudCover;
        info.frameSeconds = std::clamp(dt, 0.f, 0.25f);
        info.waterTime = game.physics().time();   // (the waves as the physics has them: what floats rides them)
        info.colorFilter = game.colorFilter();
        if (!scene.render.skyImage.empty()) {   // (loaded and prefiltered once, then cached)
            std::string skyError;
            info.environment = environment::load(project::resolve(scene.render.skyImage), skyError);
        }
        info.skyIntensity = scene.render.skyIntensity;
        info.skyRotation = scene.render.skyRotation * DEG2RAD;
        info.zNear = cam.zNear;
        info.zFar = cam.zFar;
        info.camForward = normalize(transformDir(inverseAffine(cam.view), {0, 0, -1}));
        info.viewProj = perspective(cam.fovY, aspect, cam.zNear, cam.zFar, true) * cam.view;
        info.camPos = cam.position;
        info.viewportX = rect[0];
        info.viewportY = rect[1];
        info.viewportW = rect[2];
        info.viewportH = rect[3];
        std::vector<SceneRenderInfo> others;   // (split screen: the other players' cameras)
        for (size_t k = 1; k < cams.size() && k <= (size_t)Renderer::kMaxViews; ++k) {
            const CameraView& c = cams[k];
            std::array<float, 4> r = pixels(c);
            SceneRenderInfo v = info;
            v.aspect = r[3] > 0.f ? r[2] / r[3] : 1.f;
            v.fovY = c.fovY;
            v.zNear = c.zNear;
            v.zFar = c.zFar;
            v.viewProj = perspective(c.fovY, v.aspect, c.zNear, c.zFar, true) * c.view;
            v.camPos = c.position;
            v.camForward = normalize(transformDir(inverseAffine(c.view), {0, 0, -1}));
            v.viewportX = v.viewportY = 0.f;
            v.viewportW = r[2];
            v.viewportH = r[3];
            others.push_back(v);
        }
        if (!others.empty()) info.views = &others;
        // The player's graphics settings: quality, and anti-aliasing unless it's the project's ("default") or given
        {
            std::string q = game.state().text("settings.quality");
            if (!std::getenv("VKWORLD_QUALITY"))
                app.renderer().setQuality(q == "low" ? Renderer::Quality::Low : q == "medium" ? Renderer::Quality::Medium
                                                                                          : Renderer::Quality::High);
            // Render scale (0.5..1) and dynamic resolution (holding 60 frames a second, or VKWORLD_TARGET_FPS)
            app.renderer().setRenderScale((float)game.state().number("settings.renderScale", 1.f));
            const char* fps = std::getenv("VKWORLD_TARGET_FPS");
            app.renderer().setDynamicResolution(game.state().flag("settings.dynamicResolution"), fps ? (float)std::atof(fps) : 60.f);
            std::string a = game.state().text("settings.antialiasing");
            if (!aaGiven && (a == "msaa" || a == "taa" || a == "off"))
                app.renderer().setAntialiasing(a == "taa" ? Renderer::Antialiasing::Temporal
                                               : a == "off" ? Renderer::Antialiasing::Off : Renderer::Antialiasing::Msaa);
        }
        // HDR output: the player's setting (or VKWORLD_HDR=1), where the display takes it (screenshots: SDR)
        if (options.screenshotPath.empty())
            app.renderer().setHdrOutput(game.state().flag("settings.hdr") || (std::getenv("VKWORLD_HDR") && std::string(std::getenv("VKWORLD_HDR")) == "1"));
        app.endFrame(info);
        ++frameNumber;
        const auto now = std::chrono::steady_clock::now();   // (the whole frame, start to start: the benchmark's)
        const float wallMs = std::chrono::duration<float, std::milli>(now - frameStart).count();
        frameStart = now;
        // The benchmark: the passes' times after a warm-up (pipelines made, shadows cached), then the results and out
        if (benchmarkFrames > 0 && frameNumber > kBenchmarkWarmup) {
            benchmark.add(profiler::gpuTimes(), wallMs);
            if ((int)benchmark.frames() >= benchmarkFrames) {
                const std::string quality = game.state().text("settings.quality");
                const std::string sceneName = stress ? "--stress" : path;
                std::printf("Benchmark: %s, %s, %zu frames (median ms)\n", app.renderer().gpuName().c_str(), sceneName.c_str(),
                            benchmark.frames());
                for (const auto& [name, ms] : benchmark.medians()) std::printf("  %-36s %8.2f\n", name.c_str(), ms);
                std::printf("  %-36s %8.2f\n  %-36s %8.2f\n", "GPU frame", benchmark.medianTotal(), "Frame (wall clock)",
                            benchmark.medianCpu());
                if (!benchmarkOut.empty()) {
                    std::ofstream out(fs::u8path(benchmarkOut));
                    out << benchmark.json(app.renderer().gpuName(), sceneName, quality);
                }
                break;
            }
        }
        // The first run: the quality picked from a few frames' GPU time at high (60 a second), then kept
        if (autoQuality && frameNumber > kBenchmarkWarmup) {
            qualityProbe.add(profiler::gpuTimes(), wallMs);
            if (qualityProbe.frames() >= 60) {
                const int q = chooseQuality(qualityProbe.medianTotal(), 1000.f / 60.f);
                const char* names[] = {"low", "medium", "high"};
                game.state().setText("settings.quality", names[q]);
                game.state().setFlag("settings.qualityChosen", true);
                game.saveSettings();
                logLine("Graphics quality: %s (the GPU took %.1f ms a frame at high)", names[q], qualityProbe.medianTotal());
                autoQuality = false;
            }
        }
    }
    game.stop();
    platform::setCursorCaptured(false);
    if (!trace.empty()) {
        profiler::stopRecording();
        std::string error;
        if (profiler::saveChromeTrace(fs::u8path(trace), profiler::recorded(), error))
            std::printf("Saved a trace of %zu frames to %s\n", profiler::recorded().size(), trace.c_str());
        else
            logLine("trace: %s", error.c_str());
    }
    int code = app.exitCode();
    app.shutdown();
    return code;
}
