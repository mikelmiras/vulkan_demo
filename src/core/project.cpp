#include "core/project.h"

#include <fstream>
#include <sstream>

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

bool exists(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "project.json", ec);
}

bool load(const fs::path& dir, Settings& out, std::string& error) {
    std::ifstream f(dir / "project.json", std::ios::binary);
    if (!f) { error = "no project.json in " + dir.u8string(); return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    json::Value doc;
    if (!json::parse(ss.str(), doc, error)) { error = "project.json: " + error; return false; }
    if (doc["format"].asString() != "vkworld-project") { error = "project.json: not a vkworld project"; return false; }
    Settings s;
    s.name = doc["name"].asString();
    if (s.name.empty()) s.name = dir.filename().u8string();
    if (!doc["startScene"].asString().empty()) s.startScene = doc["startScene"].asString();
    out = s;
    return true;
}

bool save(const fs::path& dir, const Settings& settings, std::string& error) {
    json::Value doc = json::Value::object();
    doc.set("format", "vkworld-project");
    doc.set("version", 1);
    doc.set("name", settings.name);
    doc.set("startScene", settings.startScene);
    std::ofstream f(dir / "project.json", std::ios::binary);
    if (!f) { error = "cannot write " + (dir / "project.json").u8string(); return false; }
    f << json::write(doc);
    return (bool)f;
}

}  // namespace project
