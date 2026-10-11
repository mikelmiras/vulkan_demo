// Exporting a game: something that runs on its own (copy it, zip it, ship it). In a folder:
//
//   <Game Name>(.exe)        the game: vkgame named after the project, or, if the project's module was built
//                            standalone (VKWORLD_STANDALONE_GAMES), its <module>_game with the engine and the module
//                            compiled in
//   libengine.so             the engine library (engine.dll, libengine.dylib), unless standalone
//   bin/<module>.so          the game module's library, unless standalone
//   the data                 the project's files (scenes, models, textures, sounds, prefabs; sources, build files
//                            and hidden files are left out) and the compiled shaders, as loose files (Folder), in
//                            game.pak (Pack), or appended to the executable (SingleFile: with a standalone build,
//                            the whole game is that one file)
//
// The game starts with the project's start scene (vkgame looks for project.json, or a pack, next to itself). On
// macOS it's wrapped in <Game Name>.app; on Linux a <Game Name>.desktop entry goes with it if the project has an icon.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct ExportOptions {
    enum class Layout { Folder, Pack, SingleFile };
    Layout layout = Layout::Folder;
};

struct ExportResult {
    std::filesystem::path executable;   // the game to run
    int files = 0;                      // copied or packed
    bool standalone = false;            // the game is a standalone build (no engine or module library)
    std::vector<std::string> warnings;  // e.g. assets outside the project that the game won't have
};

// `runtimeDir`: where vkgame, the engine library and the shaders are (the editor's own folder, normally)
bool exportGame(const std::filesystem::path& projectDir, const std::filesystem::path& outDir,
                const std::filesystem::path& runtimeDir, ExportResult& result, std::string& error,
                const ExportOptions& options = {});
