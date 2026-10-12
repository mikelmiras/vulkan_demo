#include "app/export.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

#include "core/json.h"
#include "core/project.h"
#include "core/vfs.h"
#include "game/module.h"
#include "platform/platform.h"

namespace fs = std::filesystem;

namespace {

// Is `p` inside `dir` (or `dir` itself)?
bool within(const fs::path& p, const fs::path& dir) {
    std::error_code ec;
    fs::path rel = fs::relative(p, dir, ec);
    return !ec && !rel.empty() && rel.begin()->u8string() != "..";
}

#if defined(_WIN32)
const char* kExeSuffix = ".exe";
const char* kEngineLib = "engine.dll";
#elif defined(__APPLE__)
const char* kExeSuffix = "";
const char* kEngineLib = "libengine.dylib";
#else
const char* kExeSuffix = "";
const char* kEngineLib = "libengine.so";
#endif

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Source code and build clutter stay behind
bool skipped(const fs::path& rel) {
    for (const fs::path& part : rel) {
        std::string p = part.u8string();
        if (p.empty() || p[0] == '.') return true;
        std::string l = lower(p);
        if (l == "src" || l == "cmakefiles" || l.rfind("build", 0) == 0 || l.rfind("cmake-build", 0) == 0) return true;
    }
    std::string name = lower(rel.filename().u8string());
    static const std::set<std::string> code = {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".inl", ".cmake", ".mm"};
    if (code.count(rel.extension().u8string().empty() ? "" : lower(rel.extension().u8string())) || name == "cmakelists.txt")
        return true;
    return false;
}

bool copyFile(const fs::path& from, const fs::path& to, ExportResult& result, std::string& error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) {
        error = "cannot copy " + from.u8string() + " to " + to.u8string() + ": " + ec.message();
        return false;
    }
    ++result.files;
    return true;
}

// Absolute paths (outside the project) in a scene or prefab file: the exported game won't find them
void checkExternalAssets(const fs::path& file, const fs::path& projectDir, ExportResult& result) {
    std::ifstream f(file, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    json::Value doc;
    std::string error;
    if (!json::parse(ss.str(), doc, error)) return;
    std::set<std::string> reported;
    std::vector<const json::Value*> stack{&doc};
    while (!stack.empty()) {
        const json::Value* v = stack.back();
        stack.pop_back();
        if (v->isObject()) {
            for (const auto& [key, child] : v->members()) {
                if (child.type() == json::Value::Type::String && (key == "source" || key == "clip")) {
                    std::string s = child.asString();
                    std::string path = s.substr(0, s.find('#'));
                    fs::path p = fs::u8path(path);
                    if (!p.is_absolute()) continue;
                    std::error_code ec;
                    fs::path rel = fs::relative(p, projectDir, ec);
                    bool outside = ec || rel.empty() || rel.begin()->u8string() == "..";
                    if (outside && reported.insert(path).second)
                        result.warnings.push_back(fs::relative(file, projectDir).u8string() + " uses " + path +
                                                  ", outside the project: the exported game won't have it");
                }
                stack.push_back(&child);
            }
        } else if (v->isArray()) {
            for (const json::Value& child : v->elements()) stack.push_back(&child);
        }
    }
}

}  // namespace

namespace {
bool exportProject(const fs::path& projectDir, const fs::path& outDir, const fs::path& runtimeDir, ExportResult& result,
                   std::string& error, const ExportOptions& options);
}

bool exportGame(const fs::path& projectDir, const fs::path& outDir, const fs::path& runtimeDir, ExportResult& result,
                std::string& error, const ExportOptions& options) {
    fs::path previous = project::root();   // (paths in the project resolve against it while exporting)
    project::setRoot(projectDir);
    bool ok = exportProject(projectDir, outDir, runtimeDir, result, error, options);
    project::setRoot(previous);
    return ok;
}

namespace {

bool exportProject(const fs::path& projectDir, const fs::path& outDir, const fs::path& runtimeDir, ExportResult& result,
                   std::string& error, const ExportOptions& options) {
    result = {};
    std::error_code ec;
    project::Settings settings;
    if (!project::load(projectDir, settings, error)) return false;
    fs::path project = fs::weakly_canonical(projectDir, ec), out = fs::weakly_canonical(fs::absolute(outDir, ec), ec);
    if (out == project) {
        error = "export into another folder, not the project itself";
        return false;
    }
    std::string name;
    for (char c : settings.name) name += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') ? c : '_';
    if (name.empty()) name = "game";

    // The game module, and its standalone build if there is one
    fs::path module, standalone;
    if (!settings.module.empty()) {
        module = fs::weakly_canonical(GameModule::libraryPath(fs::u8path(project::resolve(settings.module))), ec);
        fs::path base = fs::u8path(project::resolve(settings.module));
        fs::path candidate = base.parent_path() / (base.filename().u8string() + "_game" + kExeSuffix);
        if (fs::is_regular_file(candidate, ec)) standalone = candidate;
        else if (!fs::is_regular_file(module, ec)) {
            error = "the game module isn't built (" + module.u8string() + ")";
            return false;
        }
    }
    result.standalone = !standalone.empty();

    // The runtime: the game executable (and the engine library), the shaders
    fs::path game = result.standalone ? standalone : runtimeDir / (std::string("vkgame") + kExeSuffix);
    fs::path engine = runtimeDir / kEngineLib;
    fs::path shaders = fs::is_directory(runtimeDir / "shaders", ec) ? runtimeDir / "shaders" : runtimeDir.parent_path() / "shaders";
    if (!fs::is_regular_file(game, ec)) { error = "can't find " + game.u8string() + " (build vkgame first)"; return false; }
    if (!result.standalone && !fs::is_regular_file(engine, ec)) { error = "can't find " + engine.u8string(); return false; }
    if (!fs::is_directory(shaders, ec)) { error = "can't find the compiled shaders next to " + runtimeDir.u8string(); return false; }

    // macOS: an application bundle, the game and everything it reads inside Contents/MacOS
    fs::path dir = out;
#if defined(__APPLE__)
    dir = out / (name + ".app") / "Contents" / "MacOS";
#endif
    fs::create_directories(dir, ec);
    if (ec) { error = "cannot create " + dir.u8string() + ": " + ec.message(); return false; }
    result.executable = dir / (name + kExeSuffix);
    if (!copyFile(game, result.executable, result, error)) return false;
    if (!result.standalone && !copyFile(engine, dir / kEngineLib, result, error)) return false;
#if !defined(_WIN32)
    fs::permissions(result.executable, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
#endif

    // The icon (a PNG): Windows puts it in the executable (before any pack goes on the end of it), macOS in the bundle
    std::string iconPng;
    if (!settings.icon.empty()) {
        std::ifstream in(fs::u8path(project::resolve(settings.icon)), std::ios::binary);
        iconPng.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        uint32_t iw = 0, ih = 0;
        if (!pngSize(iconPng, iw, ih)) {
            result.warnings.push_back("the icon must be a PNG: " + settings.icon);
            iconPng.clear();
        }
    }
#if defined(_WIN32)
    if (!iconPng.empty()) {
        std::string iconError;
        if (!platform::setExecutableIcon(result.executable, makeIco(iconPng), iconError)) result.warnings.push_back(iconError);
    }
#elif defined(__APPLE__)
    if (!iconPng.empty()) {
        fs::path resources = out / (name + ".app") / "Contents" / "Resources";
        fs::create_directories(resources, ec);
        std::ofstream(resources / "icon.icns", std::ios::binary) << makeIcns(iconPng);
    }
#endif

    // The data: the shaders and the project's files (not its code; the module's library goes on the disk, where it
    // can be loaded from, unless the game is standalone)
    std::vector<std::pair<std::string, fs::path>> data;
    for (const auto& entry : fs::directory_iterator(shaders, ec))
        if (entry.path().extension() == ".spv") data.emplace_back("shaders/" + entry.path().filename().u8string(), entry.path());
    for (auto it = fs::recursive_directory_iterator(project, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        fs::path rel = fs::relative(p, project, ec);
        if (within(p, out)) { it.disable_recursion_pending(); continue; }   // (exporting into a folder inside it)
        if (it->is_directory(ec)) {   // (a skipped folder is still entered if the module is in it)
            if (skipped(rel) && !(!module.empty() && within(module, p))) it.disable_recursion_pending();
            continue;
        }
        bool isModule = !module.empty() && fs::weakly_canonical(p, ec) == module;
        bool inModuleFolder = !module.empty() && rel.has_parent_path() && (project / rel).parent_path() == module.parent_path();
        if (isModule) {
            if (!result.standalone && !copyFile(p, dir / rel, result, error)) return false;
            continue;
        }
        if (skipped(rel) || inModuleFolder) continue;   // the module folder: only the module itself
        data.emplace_back(rel.generic_u8string(), p);
        std::string ext = lower(p.extension().u8string());
        if (ext == ".vkw" || ext == ".prefab") checkExternalAssets(p, project, result);
    }
    switch (options.layout) {
        case ExportOptions::Layout::Folder:
            for (const auto& [rel, file] : data)
                if (!copyFile(file, dir / fs::u8path(rel), result, error)) return false;
            break;
        case ExportOptions::Layout::Pack:
            if (!vfs::writePack(dir / "game.pak", data, false, error)) return false;
            result.files += (int)data.size();
            break;
        case ExportOptions::Layout::SingleFile:
            if (!vfs::writePack(result.executable, data, true, error)) return false;
            result.files += (int)data.size();
            break;
    }

#if defined(__APPLE__)
    {   // the bundle's description
        std::ofstream plist(out / (name + ".app") / "Contents" / "Info.plist");
        plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                 "<plist version=\"1.0\"><dict>\n"
                 "  <key>CFBundleName</key><string>" << name << "</string>\n"
                 "  <key>CFBundleExecutable</key><string>" << name << "</string>\n"
                 "  <key>CFBundleIdentifier</key><string>org.vkworld.game." << lower(name) << "</string>\n"
                 "  <key>CFBundlePackageType</key><string>APPL</string>\n"
              << (iconPng.empty() ? "" : "  <key>CFBundleIconFile</key><string>icon</string>\n") <<
                 "  <key>NSHighResolutionCapable</key><true/>\n"
                 "</dict></plist>\n";
    }
#elif !defined(_WIN32)
    if (!settings.icon.empty()) {   // a desktop entry, next to the game (install it with the absolute paths filled in)
        fs::path icon = fs::u8path(project::resolve(settings.icon));
        if (copyFile(icon, out / ("icon" + icon.extension().u8string()), result, error)) {
            std::ofstream desktop(out / (name + ".desktop"));
            desktop << "[Desktop Entry]\nType=Application\nName=" << settings.name << "\nExec=" << result.executable.filename().u8string()
                    << "\nIcon=" << ("icon" + icon.extension().u8string()) << "\nTerminal=false\nCategories=Game;\n";
        } else {
            result.warnings.push_back("the icon couldn't be copied: " + error);
            error.clear();
        }
    }
#endif
    return true;
}

}  // namespace

bool pngSize(const std::string& png, uint32_t& width, uint32_t& height) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (png.size() < 24 || std::memcmp(png.data(), sig, 8) != 0 || png.compare(12, 4, "IHDR") != 0) return false;
    auto be32 = [&](size_t at) {
        return ((uint32_t)(uint8_t)png[at] << 24) | ((uint32_t)(uint8_t)png[at + 1] << 16) |
               ((uint32_t)(uint8_t)png[at + 2] << 8) | (uint8_t)png[at + 3];
    };
    width = be32(16);
    height = be32(20);
    return width > 0 && height > 0;
}

std::string makeIco(const std::string& png) {
    uint32_t w = 0, h = 0;
    if (!pngSize(png, w, h)) return {};
    std::string out;
    auto u8 = [&](uint32_t v) { out += (char)(v & 0xFF); };
    auto u16 = [&](uint32_t v) { u8(v); u8(v >> 8); };
    auto u32 = [&](uint32_t v) { u16(v); u16(v >> 16); };
    u16(0); u16(1); u16(1);                                 // an icon, one image
    u8(w >= 256 ? 0 : w); u8(h >= 256 ? 0 : h); u8(0); u8(0);  // (0 means 256 or more)
    u16(1); u16(32); u32((uint32_t)png.size()); u32(6 + 16);
    return out + png;
}

std::string makeIcns(const std::string& png) {
    uint32_t w = 0, h = 0;
    if (!pngSize(png, w, h)) return {};
    // The type says the size it stands for; the nearest one at or above the image's (macOS scales it)
    const std::pair<uint32_t, const char*> types[] = {{16, "icp4"}, {32, "icp5"}, {64, "icp6"}, {128, "ic07"},
                                                      {256, "ic08"}, {512, "ic09"}, {1024, "ic10"}};
    const char* type = "ic10";
    for (const auto& t : types)
        if (std::max(w, h) <= t.first) { type = t.second; break; }
    std::string out;
    auto be32 = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) out += (char)((v >> s) & 0xFF); };
    out += "icns";
    be32((uint32_t)(8 + 8 + png.size()));
    out += type;
    be32((uint32_t)(8 + png.size()));
    return out + png;
}
