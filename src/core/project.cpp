#include "core/project.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/vfs.h"
#include "core/json.h"

namespace fs = std::filesystem;

namespace project {

namespace {

fs::path& rootStorage() {
    static fs::path root = [] {
        std::error_code ec;
        return fs::current_path(ec);
    }();
    return root;
}

fs::path normal(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    fs::path canon = fs::weakly_canonical(abs, ec);
    return ec ? abs.lexically_normal() : canon;
}

// Splits "file#fragment"; the fragment includes the '#'
void split(const std::string& s, std::string& file, std::string& fragment) {
    size_t hash = s.rfind('#');
    file = hash == std::string::npos ? s : s.substr(0, hash);
    fragment = hash == std::string::npos ? "" : s.substr(hash);
}

}  // namespace

void setRoot(const fs::path& root) { rootStorage() = normal(root); }

const fs::path& root() { return rootStorage(); }

std::string toStored(const std::string& path) {
    if (path.empty()) return path;
    std::string file, fragment;
    split(path, file, fragment);
    fs::path abs = normal(fs::u8path(file));
    fs::path rel = abs.lexically_relative(root());
    bool inside = !rel.empty() && *rel.begin() != "..";
    return (inside ? rel.generic_u8string() : abs.generic_u8string()) + fragment;
}

std::string resolve(const std::string& stored) {
    if (stored.empty()) return stored;
    std::string file, fragment;
    split(stored, file, fragment);
    fs::path p = fs::u8path(file);
    if (p.is_relative()) p = root() / p;
    return normal(p).u8string() + fragment;
}

fs::path saveDir(const std::string& projectName) {
    if (const char* over = std::getenv("VKWORLD_SAVE_DIR"); over && *over) return fs::u8path(over);
    fs::path base;
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA")) base = fs::u8path(appData);
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) base = fs::u8path(home) / "Library" / "Application Support";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) base = fs::u8path(xdg);
    else if (const char* home = std::getenv("HOME")) base = fs::u8path(home) / ".local" / "share";
#endif
    if (base.empty()) base = fs::temp_directory_path();
    // Keep the name usable as a folder name everywhere
    std::string name;
    for (char c : projectName) name += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') ? c : '_';
    if (name.empty()) name = "Untitled";
    return base / "vkworld" / name / "saves";
}

bool exists(const fs::path& dir) {
    return vfs::exists(dir / "project.json");
}

bool load(const fs::path& dir, Settings& out, std::string& error) {
    std::string text;
    if (!vfs::read(dir / "project.json", text)) { error = "no project.json in " + dir.u8string(); return false; }
    json::Value doc;
    if (!json::parse(text, doc, error)) { error = "project.json: " + error; return false; }
    if (doc["format"].asString() != "vkworld-project") { error = "project.json: not a vkworld project"; return false; }
    Settings s;
    s.name = doc["name"].asString();
    if (s.name.empty()) s.name = dir.filename().u8string();
    if (!doc["startScene"].asString().empty()) s.startScene = doc["startScene"].asString();
    s.module = doc["module"].asString();
    s.moduleBuild = doc["moduleBuild"].asString();
    s.icon = doc["icon"].asString();
    out = s;
    return true;
}

bool save(const fs::path& dir, const Settings& settings, std::string& error) {
    json::Value doc = json::Value::object();
    doc.set("format", "vkworld-project");
    doc.set("version", 1);
    doc.set("name", settings.name);
    doc.set("startScene", settings.startScene);
    if (!settings.module.empty()) doc.set("module", settings.module);
    if (!settings.moduleBuild.empty()) doc.set("moduleBuild", settings.moduleBuild);
    if (!settings.icon.empty()) doc.set("icon", settings.icon);
    std::ofstream f(dir / "project.json", std::ios::binary);
    if (!f) { error = "cannot write " + (dir / "project.json").u8string(); return false; }
    f << json::write(doc);
    return (bool)f;
}

}  // namespace project
