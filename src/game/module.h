// Game modules: a project's own code (behaviours) in a shared library that the editor and vkgame load at run time.
//
// A module is built against the engine (see examples/sample_game for a project with one, and
// vkworld_add_game_module() in CMakeLists.txt) and has one entry point that registers its behaviours:
//
//   #include "game/module.h"
//   struct Hover : BehaviourBase<Hover> { ... };          // see scene/behaviour.h
//   VKWORLD_GAME_MODULE() {
//       registerBehaviour<Hover>("Floats up and down");
//   }
//
// The editor reloads a module when its library changes on disk (or on Game > Reload game module, which first runs
// the project's build command). Behaviours from the old code are kept as their saved fields while it's swapped
// (MissingBehaviour), so the scene and undo history survive. A module must be built with the same compiler and
// engine headers as the programs that load it.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "scene/behaviour.h"

#define VKWORLD_MODULE_API_VERSION 68  // bump when Behaviour, GameContext or the scene types change layout

#if defined(_WIN32)
#define VKWORLD_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define VKWORLD_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Defines the module's entry points; the body that follows registers the behaviours
#define VKWORLD_GAME_MODULE()                                                                \
    static void vkworldRegisterModuleTypes();                                                \
    VKWORLD_MODULE_EXPORT int vkworldModuleApiVersion() { return VKWORLD_MODULE_API_VERSION; } \
    VKWORLD_MODULE_EXPORT void vkworldModuleRegister() { vkworldRegisterModuleTypes(); }     \
    static void vkworldRegisterModuleTypes()

class GameModule {
public:
    GameModule() = default;
    GameModule(const GameModule&) = delete;
    GameModule& operator=(const GameModule&) = delete;
    ~GameModule() { unload(); }

    // Loads the library and registers its behaviours. The library is copied first and the copy is loaded, so the
    // original can be rebuilt while it's in use. Unloads the previous one (detach its behaviours first!).
    bool load(const std::filesystem::path& library, std::string& error);
    void unload();
    bool loaded() const { return handle_ != nullptr; }

    const std::filesystem::path& library() const { return library_; }
    const std::vector<std::string>& behaviourTypes() const { return types_; }   // what it registered
    // The library file is newer than what was loaded (and complete: not modified in the last half second)
    bool changedOnDisk() const;

    // A project's module setting ("bin/game") -> the library file for this platform ("bin/libgame.so",
    // "bin/game.dll", ...): the first that exists, else the platform's usual name
    static std::filesystem::path libraryPath(const std::filesystem::path& base);

private:
    void* handle_ = nullptr;
    std::filesystem::path library_, loadedCopy_;
    std::filesystem::file_time_type loadedTime_{};
    std::string name_;   // the registry's owner tag
    std::vector<std::string> types_;
};
