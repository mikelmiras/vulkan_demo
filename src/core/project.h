// The project: a folder holding a game's scenes and assets, with a project.json. Files inside it are referred
// to by paths relative to it (so a project can be moved or shared); files outside it keep absolute paths.
// There is one current project per program (by default the working directory).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace project {

struct Settings {
    std::string name = "Untitled";
    std::string startScene = "scene.vkw";   // what vkgame plays and the editor opens (relative to the project)
    // The project's game module (its own behaviours, see game/module.h): the library's path without the platform's
    // prefix and extension ("bin/game" -> bin/libgame.so, bin/game.dll, ...). Empty: none.
    std::string module;
    // The command that builds the module, run in the project folder by the editor's Game > Reload game module.
    // "{engineBuild}" stands for the editor's own build folder (for projects built along with the engine).
    std::string moduleBuild;
    std::string icon;   // the game's icon: a PNG in the project (window icon; exported games' desktop entries)
    std::string language = "en";   // the strings/<language>.json the game starts with (see game/strings.h)
    std::string gameVersion = "1.0.0";   // the game's own version (exports: the app's; update checks compare it)
    // The anti-aliasing the game starts with: "msaa", "taa", "off" or "" (the renderer's: MSAA). Players can change
    // it (settings.antialiasing); vkgame --aa and VKWORLD_AA override both.
    std::string antialiasing;
    // Where the game asks whether there's a newer version (vkgame at start; players can turn it off in
    // settings.checkUpdates): JSON {"version": "1.1.0", "url": "...", "notes": "..."}, or a GitHub API "releases/latest"
    // URL. Empty: no check.
    std::string updateUrl;
    // The Ed25519 public key (64 hex digits: vkgame --update-keygen) its update manifests must be signed with; empty:
    // unsigned ones are taken too (app/updates.h)
    std::string updateKey;
    // The relay server its multiplayer lobbies are on ("relay.example.com[:27100]", net/relay.h; vkrelay runs one).
    // Empty: games are joined by address only.
    std::string relay;
    // What that relay asks hosts for, if it runs with a secret (vkrelay --secret): only games that know it can host
    std::string relaySecret;
    // Where crash reports go when a player agrees to send them (app/crash_reports.h): an http(s) URL taking a POST of
    // the report's text. Empty: they stay on the player's machine.
    std::string crashReportUrl;
    // The engine version that last saved project.json (save() writes the running engine's): opening a project made
    // with a newer engine warns that this one may not read everything in it
    std::string engineVersion;
    // Who makes it, and its reverse-DNS identifier (macOS bundle id: "com.example.coinhunt"; "" for one from the
    // name): exported executables carry them (app/exe_metadata.h)
    std::string publisher, identifier;
    // Build profiles (the editor's Build Settings, vkworld <project> --build <name>): how to export the game
    struct Build {
        std::string name = "Release";
        std::string layout = "pack";   // "folder", "pack" or "single" (ExportOptions::Layout)
        bool package = false;          // and an AppImage / .dmg / installer
        bool updatePack = false;       // and an update pack
        bool shipping = true;          // without the developer tools (app/export.h)
        bool cook = true;              // textures and models cooked (ExportOptions::cook)
        std::string textureCompression = "bc7";   // "bc7", "bc1" or "none"
        std::string out = "builds/Release";   // relative to the project, or absolute
    };
    std::vector<Build> builds;
};

// A project folder at a glance (the editor's launcher): whether it's there and a project, its name and versions
struct Info {
    std::filesystem::path dir;
    bool found = false;         // a folder with a readable project.json
    std::string name, gameVersion, engineVersion, error;
};
Info describe(const std::filesystem::path& dir);

// How the project's engine version compares with this engine's
enum class EngineMatch { Same, Older, Newer, Unknown };   // (Older: made with an older engine, fine to open)
EngineMatch engineMatch(const std::string& projectVersion, const std::string& engineVersion);

void setRoot(const std::filesystem::path& root);
const std::filesystem::path& root();

// For saving: relative to the project (forward slashes) when inside it, else absolute. A "#fragment" suffix
// (glTF sub-assets: "model.glb#2.0") is kept as is.
std::string toStored(const std::string& path);
// For loading: stored paths (relative or absolute, with or without "#fragment") -> absolute
std::string resolve(const std::string& stored);

bool exists(const std::filesystem::path& dir);   // has a project.json

// Where a game's save games go: <user data>/vkworld/<project name>/saves (~/.local/share on Linux, %APPDATA% on
// Windows, ~/Library/Application Support on macOS). VKWORLD_SAVE_DIR overrides it.
std::filesystem::path saveDir(const std::string& projectName);
bool load(const std::filesystem::path& dir, Settings& out, std::string& error);
bool save(const std::filesystem::path& dir, const Settings& settings, std::string& error);

}  // namespace project
