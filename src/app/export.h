// Exporting a game: a folder that runs on its own (copy it, zip it, ship it).
//
//   <out>/<Game Name>(.exe)     vkgame, named after the project
//   <out>/libengine.so          the engine library (engine.dll, libengine.dylib), found next to the executable
//   <out>/shaders/              compiled shaders
//   <out>/project.json, ...     the project's files: scenes, models, textures, sounds, prefabs, and its game
//                               module's library (sources, build files and hidden files are left out)
//
// The game then starts with the project's start scene (vkgame looks for project.json next to itself).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct ExportResult {
    std::filesystem::path executable;   // the game to run
    int files = 0;                      // copied
    std::vector<std::string> warnings;  // e.g. assets outside the project that the game won't have
};

// `runtimeDir`: where vkgame, the engine library and the shaders are (the editor's own folder, normally)
bool exportGame(const std::filesystem::path& projectDir, const std::filesystem::path& outDir,
                const std::filesystem::path& runtimeDir, ExportResult& result, std::string& error);
