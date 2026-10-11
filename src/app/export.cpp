#include "app/export.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

#include "core/json.h"
#include "core/project.h"
#include "game/module.h"

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
                   std::string& error);
}

bool exportGame(const fs::path& projectDir, const fs::path& outDir, const fs::path& runtimeDir, ExportResult& result,
                std::string& error) {
    fs::path previous = project::root();   // (paths in the project resolve against it while exporting)
    project::setRoot(projectDir);
    bool ok = exportProject(projectDir, outDir, runtimeDir, result, error);
    project::setRoot(previous);
    return ok;
}

namespace {

bool exportProject(const fs::path& projectDir, const fs::path& outDir, const fs::path& runtimeDir, ExportResult& result,
                   std::string& error) {
    result = {};
    std::error_code ec;
    project::Settings settings;
    if (!project::load(projectDir, settings, error)) return false;
    fs::path project = fs::weakly_canonical(projectDir, ec), out = fs::weakly_canonical(fs::absolute(outDir, ec), ec);
    if (out == project) {
        error = "export into another folder, not the project itself";
        return false;
    }

    // The runtime: vkgame, the engine library, the shaders
    fs::path game = runtimeDir / (std::string("vkgame") + kExeSuffix), engine = runtimeDir / kEngineLib;
    fs::path shaders = fs::is_directory(runtimeDir / "shaders", ec) ? runtimeDir / "shaders" : runtimeDir.parent_path() / "shaders";
    for (const fs::path& p : {game, engine})
        if (!fs::is_regular_file(p, ec)) { error = "can't find " + p.u8string() + " (build vkgame first)"; return false; }
    if (!fs::is_directory(shaders, ec)) { error = "can't find the compiled shaders next to " + runtimeDir.u8string(); return false; }

    fs::create_directories(out, ec);
    if (ec) { error = "cannot create " + out.u8string() + ": " + ec.message(); return false; }
    std::string name;
    for (char c : settings.name) name += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') ? c : '_';
    if (name.empty()) name = "game";
    result.executable = out / (name + kExeSuffix);
    if (!copyFile(game, result.executable, result, error) || !copyFile(engine, out / kEngineLib, result, error)) return false;
#if !defined(_WIN32)
    fs::permissions(result.executable, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
#endif
    for (const auto& entry : fs::directory_iterator(shaders, ec))
        if (entry.path().extension() == ".spv" && !copyFile(entry.path(), out / "shaders" / entry.path().filename(), result, error))
            return false;

    // The project, without its code; the game module's library is the exception
    fs::path module;
    if (!settings.module.empty()) {
        module = fs::weakly_canonical(GameModule::libraryPath(fs::u8path(project::resolve(settings.module))), ec);
        if (!fs::is_regular_file(module, ec)) {
            error = "the game module isn't built (" + module.u8string() + ")";
            return false;
        }
    }
    for (auto it = fs::recursive_directory_iterator(project, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        fs::path rel = fs::relative(p, project, ec);
        if (fs::weakly_canonical(p, ec) == out) { it.disable_recursion_pending(); continue; }   // (exporting inside it)
        bool isModule = !module.empty() && fs::weakly_canonical(p, ec) == module;
        if (it->is_directory(ec)) {   // (a skipped folder is still entered if the module is in it)
            if (skipped(rel) && !(!module.empty() && within(module, p))) it.disable_recursion_pending();
            continue;
        }
        bool inModuleFolder = !module.empty() && rel.has_parent_path() && (project / rel).parent_path() == module.parent_path();
        if (!isModule && (skipped(rel) || inModuleFolder)) continue;   // the module folder: only the module itself
        if (!copyFile(p, out / rel, result, error)) return false;
        std::string ext = lower(p.extension().u8string());
        if (ext == ".vkw" || ext == ".prefab") checkExternalAssets(p, project, result);
    }
    if (!settings.module.empty() && !fs::is_regular_file(out / fs::relative(module, project, ec), ec)) {
        error = "the game module couldn't be copied";
        return false;
    }
    return true;
}

}  // namespace
