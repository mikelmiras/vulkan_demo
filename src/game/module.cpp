#include "game/module.h"

#include <chrono>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace {

void* openLibrary(const fs::path& path, std::string& error) {
#if defined(_WIN32)
    HMODULE h = LoadLibraryW(path.wstring().c_str());
    if (!h) error = "cannot load " + path.u8string() + " (error " + std::to_string(GetLastError()) + ")";
    return (void*)h;
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) error = dlerror();
    return h;
#endif
}

void* symbol(void* handle, const char* name) {
#if defined(_WIN32)
    return (void*)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

void closeLibrary(void* handle) {
#if defined(_WIN32)
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

}  // namespace

fs::path GameModule::libraryPath(const fs::path& base) {
    fs::path dir = base.parent_path();
    std::string name = base.filename().u8string();
#if defined(_WIN32)
    const char* candidates[] = {"%.dll", "lib%.dll"};
#elif defined(__APPLE__)
    const char* candidates[] = {"lib%.dylib", "%.dylib", "lib%.so", "%.so"};
#else
    const char* candidates[] = {"lib%.so", "%.so"};
#endif
    std::error_code ec;
    for (const char* pattern : candidates) {
        std::string file = pattern;
        file.replace(file.find('%'), 1, name);
        if (fs::is_regular_file(dir / file, ec)) return dir / file;
    }
    std::string file = candidates[0];
    file.replace(file.find('%'), 1, name);
    return dir / file;
}

bool GameModule::load(const fs::path& library, std::string& error) {
    unload();
    std::error_code ec;
    if (!fs::is_regular_file(library, ec)) { error = "no game module at " + library.u8string(); return false; }

    // Load a copy with a unique name: the build can replace the original while the copy is in use, and loading a new
    // version under the same name could hand back the old one
    static int serial = 0;
    fs::path tempDir = fs::temp_directory_path(ec) / "vkworld-modules";
    fs::create_directories(tempDir, ec);
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path copy = tempDir / (library.stem().u8string() + "-" + std::to_string(stamp) + "-" + std::to_string(++serial) +
                               library.extension().u8string());
    fs::file_time_type time = fs::last_write_time(library, ec);
    if (!fs::copy_file(library, copy, fs::copy_options::overwrite_existing, ec)) {
        error = "cannot copy " + library.u8string() + ": " + ec.message();
        return false;
    }
    void* h = openLibrary(copy, error);
    if (!h) {
        fs::remove(copy, ec);
        return false;
    }
    using VersionFn = int (*)();
    using RegisterFn = void (*)();
    auto version = (VersionFn)symbol(h, "vkworldModuleApiVersion");
    auto reg = (RegisterFn)symbol(h, "vkworldModuleRegister");
    if (!version || !reg) {
        error = library.u8string() + " is not a game module (no VKWORLD_GAME_MODULE)";
    } else if (version() != VKWORLD_MODULE_API_VERSION) {
        error = library.u8string() + " was built for module API " + std::to_string(version()) + ", this engine has " +
                std::to_string(VKWORLD_MODULE_API_VERSION) + ": rebuild it";
    }
    if (!error.empty()) {
        closeLibrary(h);
        fs::remove(copy, ec);
        return false;
    }

    handle_ = h;
    library_ = library;
    loadedCopy_ = copy;
    loadedTime_ = time;
    name_ = "module:" + library.u8string();
    setRegisteringModule(name_);
    reg();
    setRegisteringModule("");
    types_.clear();
    for (const BehaviourType& t : ::behaviourTypes())
        if (t.module == name_) types_.push_back(t.name);
    return true;
}

void GameModule::unload() {
    if (!handle_) return;
    unregisterModuleBehaviours(name_);
    closeLibrary(handle_);
    handle_ = nullptr;
    std::error_code ec;
    fs::remove(loadedCopy_, ec);   // fails on Windows if something still holds it; the temp folder is fine for that
    types_.clear();
}

bool GameModule::changedOnDisk() const {
    if (library_.empty()) return false;
    std::error_code ec;
    fs::file_time_type t = fs::last_write_time(library_, ec);
    if (ec || t == loadedTime_) return false;
    // Wait until the linker has finished writing it
    return fs::file_time_type::clock::now() - t > std::chrono::milliseconds(500);
}
