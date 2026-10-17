#include "app/export.h"

#include <algorithm>
#include <functional>
#include <cstdlib>
#include <initializer_list>
#include <cstring>
#include <iterator>
#include <map>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

#include "app/exe_metadata.h"
#include "app/updates.h"
#include "assets/cooking.h"
#include "assets/gltf.h"
#include "core/json.h"
#include "core/png.h"
#include "core/project.h"
#include "core/sha256.h"
#include "core/version.h"
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
bool skipped(const fs::path& rel, bool directory = false) {
    for (auto it = rel.begin(); it != rel.end(); ++it) {
        std::string p = it->u8string();
        if (p.empty() || p[0] == '.') return true;
        if (!directory && std::next(it) == rel.end()) break;   // (folders only: a file may be called building.glb)
        std::string l = lower(p);
        bool build = l == "build" || l == "builds" || l.rfind("build-", 0) == 0 || l.rfind("build_", 0) == 0 ||
                     l.rfind("cmake-build", 0) == 0;
        if (l == "src" || l == "cmakefiles" || build) return true;
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

// Cooking (ExportOptions::cook): `data`'s glTF models, and the images scenes and prefabs use as textures, swapped for
// their cooked files (made in `scratch`). An image whose path turns up more often in the project's text files than
// its uses as a texture is named somewhere else too (an icon, a sky, a height map, a script): its source stays.
void cookData(std::vector<std::pair<std::string, fs::path>>& data, const fs::path& project, const fs::path& scratch,
              cook::Compression compression, ExportResult& result) {
    auto read = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };
    auto relative = [&](const std::string& source) {   // (sources are relative to the project, or absolute in it)
        fs::path p = fs::u8path(source);
        if (!p.is_absolute()) return p.generic_u8string();
        std::error_code ec;
        fs::path rel = fs::relative(p, project, ec);
        return ec ? std::string() : rel.generic_u8string();
    };
    std::map<std::string, int> asTexture;   // project-relative image path -> its uses in "textures" lists
    std::set<std::string> asData;           // ... those a material reads as numbers (a normal map): mips made so
    std::vector<std::string> texts;
    for (const auto& [rel, file] : data) {
        std::string ext = lower(file.extension().u8string());
        if (ext == ".glb" || ext == ".gltf") {   // (a model's normal maps in files of their own)
            std::vector<std::string> files;
            std::vector<bool> isData;
            std::string error;
            if (gltfImageFiles(file.u8string(), files, error, &isData))
                for (size_t i = 0; i < files.size(); ++i)
                    if (isData[i] && !files[i].empty()) asData.insert(relative(files[i]));
            continue;
        }
        if (ext != ".vkw" && ext != ".prefab" && ext != ".json" && ext != ".lua" && ext != ".txt" && ext != ".ui") continue;
        texts.push_back(read(file));
        json::Value doc;
        std::string error;
        if ((ext != ".vkw" && ext != ".prefab") || !json::parse(texts.back(), doc, error)) continue;
        std::map<std::string, std::string> sources;   // (texture name -> its file)
        for (const json::Value& t : doc["textures"].elements()) {
            std::string source = t["source"].asString();
            if (!source.empty() && source.find('#') == std::string::npos) {
                ++asTexture[relative(source)];
                sources[t["name"].asString()] = relative(source);
            }
        }
        std::function<void(const json::Value&)> walk = [&](const json::Value& v) {   // (components naming data maps)
            for (const auto& [key, value] : v.members()) {
                if (key == "normalTexture" || key == "metalRoughTexture" || key == "heightTexture" || key == "detailTexture") {
                    if (auto it = sources.find(value.asString()); it != sources.end()) asData.insert(it->second);
                }
                walk(value);
            }
            for (const json::Value& e : v.elements()) walk(e);
        };
        walk(doc["entities"]);
    }
    auto mentions = [&](const std::string& rel) {
        int n = 0;
        for (const std::string& text : texts)
            for (size_t at = text.find(rel); at != std::string::npos; at = text.find(rel, at + rel.size())) ++n;
        return n;
    };
    static const std::set<std::string> images = {".png", ".jpg", ".jpeg", ".tga", ".bmp"};
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    for (const auto& [rel, file] : data) {
        std::string ext = lower(file.extension().u8string());
        bool model = ext == ".glb" || ext == ".gltf";
        auto use = asTexture.find(rel);
        if (!model && !(images.count(ext) && use != asTexture.end())) {
            out.emplace_back(rel, file);
            continue;
        }
        std::string cooked, error;
        bool ok = model ? cook::cookModel(file.u8string(), compression, cooked, error)
                        : cook::cookImageFile(file.u8string(), compression, cooked, error, asData.count(rel) > 0);
        if (!ok) {   // (shipped as it is: still loads, only slower)
            result.warnings.push_back("not cooked: " + error);
            out.emplace_back(rel, file);
            continue;
        }
        std::string cookedRel = model ? cook::modelPath(rel) : cook::texturePath(rel);
        fs::path at = scratch / fs::u8path(cookedRel);
        fs::create_directories(at.parent_path(), ec);
        std::ofstream(at, std::ios::binary) << cooked;
        out.emplace_back(cookedRel, at);
        ++result.cooked;
        if (!model && mentions(rel) > use->second) out.emplace_back(rel, file);
    }
    data = std::move(out);
}

}  // namespace

namespace {
bool exportProject(const fs::path& projectDir, const fs::path& outDir, const fs::path& runtimeDir, ExportResult& result,
                   std::string& error, const ExportOptions& options);
}

ExportOptions optionsFor(const project::Settings::Build& build) {
    ExportOptions o;
    o.layout = build.layout == "folder" ? ExportOptions::Layout::Folder
               : build.layout == "single" ? ExportOptions::Layout::SingleFile : ExportOptions::Layout::Pack;
    o.package = build.package;
    o.updatePack = build.updatePack;
    o.shipping = build.shipping;
    o.cook = build.cook;
    o.textureCompression = build.textureCompression;
    return o;
}

fs::path buildFolder(const fs::path& projectDir, const project::Settings::Build& build) {
    fs::path out = fs::u8path(build.out.empty() ? "builds/" + build.name : build.out);
    return out.is_absolute() ? out : projectDir / out;
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

// A tool's path: the environment variable naming it, else the name (the PATH), else where it's usually installed
std::string findTool(const char* env, const char* name, std::initializer_list<const char*> usual = {}) {
    if (const char* e = std::getenv(env); e && *e) return e;
    std::error_code ec;
    for (const char* u : usual)
        if (fs::is_regular_file(fs::u8path(u), ec)) return u;
    return name;
}

bool runTool(const std::vector<std::string>& args, const char* what, std::string& error) {
    std::string output;
    int code = 0;
    if (!platform::runProgram(args, output, code)) {
        error = std::string("can't run ") + args[0] + " (" + what + ")";
        return false;
    }
    if (code != 0) {
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
        size_t from = output.size() > 600 ? output.size() - 600 : 0;   // (its last words)
        error = args[0] + " failed (exit " + std::to_string(code) + "): " + output.substr(from);
        return false;
    }
    return true;
}

bool copyTree(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks, ec);
    if (ec) error = "can't copy " + from.u8string() + ": " + ec.message();
    return !ec;
}

// The export as an update pack (ExportOptions::updatePack): every file in `out` but the packages and earlier update
// packs, then its digest
bool makeUpdatePack(const fs::path& out, const std::string& name, const project::Settings& settings, ExportResult& result,
                    std::string& error) {
    std::string base = name + "-" + settings.gameVersion + "-" + updates::platformKey();
    for (char& c : base) if (c == ' ') c = '_';
    result.updatePack = out / (base + ".vkwupdate");
    std::vector<std::pair<std::string, fs::path>> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(out, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string rel = fs::relative(it->path(), out, ec).generic_u8string(), ext = it->path().extension().u8string();
        if (ext == ".vkwupdate" || ext == ".sha256" || ext == ".dmg" || ext == ".AppImage" || rel.find("-setup.exe") != std::string::npos ||
            rel.find(".dmg-staging") != std::string::npos || rel.rfind("update/", 0) == 0)
            continue;
        files.push_back({rel, it->path()});
    }
    std::sort(files.begin(), files.end());
    if (!vfs::writePack(result.updatePack, files, false, error)) return false;
    result.updateSha256 = sha256::ofFile(result.updatePack);
    std::ofstream(fs::path(result.updatePack) += ".sha256") << result.updateSha256 << "  " << result.updatePack.filename().u8string() << "\n";
    // The chunked release (app/updates.h): update/manifest.json and update/chunks/, to put on a web server as they are
    fs::path folder = out / "update";
    std::vector<updates::ReleaseFile> described;
    for (const auto& [rel, path] : files) {
        updates::ReleaseFile f;
        if (!updates::describeFile(path, rel, f, error, folder / "chunks")) return false;
        described.push_back(std::move(f));
    }
    json::Value manifest = json::Value::object();
    manifest.set("version", settings.gameVersion);
    manifest.set("url", "");
    manifest.set("notes", "");
    json::Value& downloads = manifest.set("downloads", json::Value::object());
    json::Value& mine = downloads.set(updates::platformKey(), json::Value::object());
    mine.set("files", updates::filesToJson(described));
    mine.set("chunks", "chunks/");
    if (result.package.extension() == ".AppImage") {   // (the AppImage, updated as one file)
        updates::ReleaseFile f;
        if (!updates::describeFile(result.package, result.package.filename().u8string(), f, error, folder / "chunks")) return false;
        json::Value& app = downloads.set("linux-appimage", json::Value::object());
        app.set("files", updates::filesToJson({f}));
        app.set("chunks", "chunks/");
    }
    if (const char* keyFile = std::getenv("VKWORLD_UPDATE_KEY_FILE")) {   // (signed with the game's key, if given)
        std::string key;
        std::ifstream(keyFile) >> key;
        if (!updates::signManifest(manifest, key, error)) {
            error = "signing the update: " + error;
            return false;
        }
    }
    result.updateManifest = folder / "manifest.json";
    std::ofstream(result.updateManifest) << json::write(manifest);
    return true;
}

// The package around an export in `out`: see ExportOptions::package
bool makePackage(const fs::path& out, const std::string& name, const project::Settings& settings, const std::string& iconPng,
                 ExportResult& result, std::string& error) {
    std::error_code ec;
    std::string base = name + "-" + settings.gameVersion;
    for (char& c : base) if (c == ' ') c = '_';
#if defined(__APPLE__)
    // A disk image: the app and a link to Applications (drag one onto the other)
    fs::path staging = out / (base + ".dmg-staging");
    fs::remove_all(staging, ec);
    if (!copyTree(out / (name + ".app"), staging / (name + ".app"), error)) return false;
    fs::create_directory_symlink("/Applications", staging / "Applications", ec);
    result.package = out / (base + ".dmg");
    fs::remove(result.package, ec);
    bool ok = runTool({"hdiutil", "create", "-volname", name, "-srcfolder", staging.u8string(), "-ov", "-format", "UDZO",
                       result.package.u8string()}, "makes the .dmg", error);
    fs::remove_all(staging, ec);
    return ok;
#elif defined(_WIN32)
    // An NSIS installer: installs for this user (no administrator needed), a Start menu entry, an uninstaller
    std::string exe = result.executable.filename().u8string();
    result.package = out / (base + "-setup.exe");
    fs::path script = out / (base + ".nsi");
    {
        std::string key = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + name;
        std::ofstream nsi(script, std::ios::binary);
        nsi << "Unicode true\n"
               "Name \"" << settings.name << "\"\n"
               "OutFile \"" << result.package.u8string() << "\"\n"
               "InstallDir \"$LOCALAPPDATA\\Programs\\" << name << "\"\n"
               "RequestExecutionLevel user\n"
               "Page directory\nPage instfiles\nUninstPage uninstConfirm\nUninstPage instfiles\n"
               "Section\n"
               "  SetOutPath \"$INSTDIR\"\n"
               "  File /r /x \"*.nsi\" /x \"*-setup.exe\" \"" << out.u8string() << "\\*.*\"\n"
               "  WriteUninstaller \"$INSTDIR\\Uninstall.exe\"\n"
               "  CreateDirectory \"$SMPROGRAMS\\" << name << "\"\n"
               "  CreateShortcut \"$SMPROGRAMS\\" << name << "\\" << name << ".lnk\" \"$INSTDIR\\" << exe << "\"\n"
               "  WriteRegStr HKCU \"" << key << "\" \"DisplayName\" \"" << settings.name << "\"\n"
               "  WriteRegStr HKCU \"" << key << "\" \"DisplayVersion\" \"" << settings.gameVersion << "\"\n"
               "  WriteRegStr HKCU \"" << key << "\" \"DisplayIcon\" \"$INSTDIR\\" << exe << "\"\n"
               "  WriteRegStr HKCU \"" << key << "\" \"UninstallString\" \"$\\\"$INSTDIR\\Uninstall.exe$\\\"\"\n"
               "SectionEnd\n"
               "Section \"Uninstall\"\n"
               "  RMDir /r \"$INSTDIR\"\n"
               "  RMDir /r \"$SMPROGRAMS\\" << name << "\"\n"
               "  DeleteRegKey HKCU \"" << key << "\"\n"
               "SectionEnd\n";
    }
    std::string makensis = findTool("VKWORLD_MAKENSIS", "makensis",
                                    {"C:\\Program Files (x86)\\NSIS\\makensis.exe", "C:\\Program Files\\NSIS\\makensis.exe"});
    bool ok = runTool({makensis, "/V2", script.u8string()}, "makes the installer: install NSIS", error);
    fs::remove(script, ec);
    return ok;
#else
    // An AppImage: the export in an AppDir (usr/bin), with AppRun, a desktop entry and an icon
    fs::path appDir = out / (base + ".AppDir");
    fs::remove_all(appDir, ec);
    fs::path bin = appDir / "usr" / "bin";
    fs::create_directories(bin, ec);
    for (const auto& entry : fs::directory_iterator(out, ec)) {
        std::string file = entry.path().filename().u8string();
        if (entry.path() == appDir || file.size() > 9 && file.compare(file.size() - 9, 9, ".AppImage") == 0) continue;
        if (entry.is_directory(ec) ? !copyTree(entry.path(), bin / file, error)
                                   : !fs::copy_file(entry.path(), bin / file, fs::copy_options::overwrite_existing, ec)) {
            if (error.empty()) error = "can't copy " + entry.path().u8string() + ": " + ec.message();
            return false;
        }
    }
    std::string exe = result.executable.filename().u8string(), id = lower(name);
    for (char& c : id) if (c == ' ') c = '-';
    {
        std::ofstream run(appDir / "AppRun");
        run << "#!/bin/sh\nHERE=\"$(dirname \"$(readlink -f \"$0\")\")\"\nexec \"$HERE/usr/bin/" << exe << "\" \"$@\"\n";
    }
    fs::permissions(appDir / "AppRun", fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                           fs::perms::others_read | fs::perms::others_exec, ec);
    std::ofstream(appDir / (id + ".desktop")) << "[Desktop Entry]\nType=Application\nName=" << settings.name
                                              << "\nExec=" << exe << "\nIcon=" << id << "\nTerminal=false\nCategories=Game;\n"
                                              << "X-AppImage-Version=" << settings.gameVersion << "\n";
    if (!iconPng.empty()) {
        std::ofstream(appDir / (id + ".png"), std::ios::binary) << iconPng;
    } else {   // (an AppImage needs one: a plain tile)
        std::vector<uint8_t> rgba(64 * 64 * 4);
        for (size_t i = 0; i < rgba.size(); i += 4) rgba[i] = 60, rgba[i + 1] = 110, rgba[i + 2] = 190, rgba[i + 3] = 255;
        writePng((appDir / (id + ".png")).u8string(), 64, 64, rgba.data());
    }
    result.package = out / (base + "-x86_64.AppImage");
    fs::remove(result.package, ec);
    setenv("ARCH", "x86_64", 0);
    setenv("APPIMAGE_EXTRACT_AND_RUN", "1", 0);   // (appimagetool is an AppImage itself: no FUSE needed this way)
    bool ok = runTool({findTool("VKWORLD_APPIMAGETOOL", "appimagetool"), "--no-appstream", appDir.u8string(),
                       result.package.u8string()}, "makes the AppImage: install appimagetool", error);
    fs::remove_all(appDir, ec);
    return ok;
#endif
}

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
    exemeta::Info meta;   // (what the executable says about itself)
    meta.product = settings.name;
    meta.company = settings.publisher;
    meta.version = settings.gameVersion;
    meta.identifier = settings.identifier;
    meta.executable = result.executable.filename().u8string();
    meta.icon = !iconPng.empty();
#if defined(_WIN32)
    if (!iconPng.empty()) {
        std::string iconError;
        if (!platform::setExecutableIcon(result.executable, makeIco(iconPng), iconError)) result.warnings.push_back(iconError);
    }
    {
        std::string versionError;
        if (!platform::setExecutableVersion(result.executable, exemeta::versionResource(meta), versionError)) result.warnings.push_back(versionError);
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
            if (skipped(rel, true) && !(!module.empty() && within(module, p))) it.disable_recursion_pending();
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
    // What the build is (vkgame reads it: a shipping build hides the developer tools)
    fs::path buildInfo = fs::temp_directory_path(ec) / ("vkworld-build-" + std::to_string(std::hash<std::string>()(out.u8string())) + ".json");
    {
        json::Value b = json::Value::object();
        b.set("shipping", options.shipping);
        b.set("engine", std::string(version::string()));
        b.set("gameVersion", settings.gameVersion);
        std::ofstream(buildInfo) << json::write(b);
    }
    data.emplace_back("build.json", buildInfo);
    fs::path scratch = fs::temp_directory_path(ec) / ("vkworld-cook-" + std::to_string(std::hash<std::string>()(out.u8string())));
    if (options.cook) {
        fs::remove_all(scratch, ec);
        cookData(data, project, scratch, cook::compressionFromName(options.textureCompression), result);
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
    fs::remove(buildInfo, ec);
    fs::remove_all(scratch, ec);

#if defined(__APPLE__)
    std::ofstream(out / (name + ".app") / "Contents" / "Info.plist") << exemeta::infoPlist(meta);   // (the bundle's description)
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
    if (options.package && !makePackage(out, name, settings, iconPng, result, error)) return false;
    if (options.updatePack && !makeUpdatePack(out, name, settings, result, error)) return false;
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
