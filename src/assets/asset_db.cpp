#include "assets/asset_db.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <random>
#include <unordered_map>

#include "core/project.h"
#include "core/vfs.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kMetaFormat = "vkworld-meta";

struct Db {
    std::mutex m;
    fs::path root;
    bool open = false, writable = false;
    std::unordered_map<std::string, std::string> idToRel, relToId;   // paths relative to the root, generic
    std::chrono::steady_clock::time_point lastScan{};
};

Db& db() {
    static Db d;
    return d;
}

fs::path normal(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    fs::path canon = fs::weakly_canonical(abs, ec);
    return ec ? abs.lexically_normal() : canon;
}

// Relative to the root (generic), or "" outside it
std::string relative(const Db& d, const fs::path& file) {
    fs::path rel = normal(file).lexically_relative(d.root);
    if (rel.empty() || *rel.begin() == "..") return {};
    std::string s = rel.generic_u8string();
    return s == "." ? std::string() : s;
}

std::string newId() {
    static std::mutex m;
    static std::mt19937_64 rng{std::random_device{}() ^
                               (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count()};
    std::lock_guard<std::mutex> lock(m);
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", (unsigned long long)rng(), (unsigned long long)rng());
    return buf;
}

bool readMeta(const fs::path& meta, json::Value& out) {
    std::string text, error;
    if (!vfs::read(meta, text) || !json::parse(text, out, error)) return false;
    return out["format"].asString() == kMetaFormat && !out["id"].asString().empty();
}

bool writeMeta(const fs::path& meta, const json::Value& doc) {
    fs::path tmp = meta;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << json::write(doc);
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, meta, ec);
    return !ec;
}

json::Value newMeta(const std::string& id) {
    json::Value doc = json::Value::object();
    doc.set("format", kMetaFormat);
    doc.set("id", id);
    doc.set("import", json::Value::object());
    return doc;
}

void scanLocked(Db& d) {
    d.idToRel.clear();
    d.relToId.clear();
    d.lastScan = std::chrono::steady_clock::now();
    if (d.root.empty()) return;
    for (const fs::path& meta : vfs::list(d.root, ".meta")) {
        std::string m = meta.u8string();
        fs::path file = fs::u8path(m.substr(0, m.size() - 5));
        json::Value doc;
        if (!vfs::exists(file) && !fs::is_directory(file)) continue;   // a .meta left behind
        if (!readMeta(meta, doc)) continue;
        std::string id = doc["id"].asString(), rel = relative(d, file);
        if (rel.empty()) continue;
        if (d.idToRel.count(id)) {   // a copy made with its .meta: the copy gets an id of its own
            if (!d.writable) continue;
            id = newId();
            doc.set("id", id);
            writeMeta(meta, doc);
        }
        d.idToRel[id] = rel;
        d.relToId[rel] = id;
    }
}

// Moves the map's entries for `from` (and under it) to `to`
void rekeyLocked(Db& d, const std::string& from, const std::string& to) {
    std::vector<std::pair<std::string, std::string>> moved;   // (old rel, id)
    for (const auto& [rel, id] : d.relToId)
        if (rel == from || rel.compare(0, from.size() + 1, from + "/") == 0) moved.emplace_back(rel, id);
    for (const auto& [rel, id] : moved) {
        std::string now = to + rel.substr(from.size());
        d.relToId.erase(rel);
        d.relToId[now] = id;
        d.idToRel[id] = now;
    }
}

void split(const std::string& s, std::string& file, std::string& fragment) {
    size_t hash = s.rfind('#');
    file = hash == std::string::npos ? s : s.substr(0, hash);
    fragment = hash == std::string::npos ? "" : s.substr(hash);
}

}  // namespace

namespace assetdb {

void open(const fs::path& projectRoot, bool writable) {
    Db& d = db();
    std::lock_guard<std::mutex> lock(d.m);
    d.root = normal(projectRoot);
    d.open = true;
    d.writable = writable;
    scanLocked(d);
}

void close() {
    Db& d = db();
    std::lock_guard<std::mutex> lock(d.m);
    d.open = false;
    d.writable = false;
    d.root.clear();
    d.idToRel.clear();
    d.relToId.clear();
}

bool isOpen() {
    std::lock_guard<std::mutex> lock(db().m);
    return db().open;
}

bool writable() {
    std::lock_guard<std::mutex> lock(db().m);
    return db().open && db().writable;
}

fs::path metaPath(const fs::path& file) {
    fs::path m = file;
    m += ".meta";
    return m;
}

std::string idOf(const fs::path& file) {
    Db& d = db();
    std::lock_guard<std::mutex> lock(d.m);
    if (!d.open) return {};
    std::string rel = relative(d, file);
    if (rel.empty()) return {};
    if (auto it = d.relToId.find(rel); it != d.relToId.end()) return it->second;
    fs::path abs = d.root / fs::u8path(rel), meta = metaPath(abs);
    json::Value doc;
    if (readMeta(meta, doc) && !d.idToRel.count(doc["id"].asString())) {   // made since the scan
        d.idToRel[doc["id"].asString()] = rel;
        d.relToId[rel] = doc["id"].asString();
        return doc["id"].asString();
    }
    std::error_code ec;
    if (!d.writable || !fs::is_regular_file(abs, ec)) return {};
    std::string id = newId();
    if (!writeMeta(meta, newMeta(id))) return {};
    d.idToRel[id] = rel;
    d.relToId[rel] = id;
    return id;
}

std::string idOfStored(const std::string& stored) {
    if (stored.empty()) return {};
    std::string file, fragment;
    split(stored, file, fragment);
    fs::path abs = fs::u8path(project::resolve(file));
    if (!vfs::exists(abs)) return {};
    return idOf(abs);
}

fs::path pathOf(const std::string& id) {
    Db& d = db();
    std::lock_guard<std::mutex> lock(d.m);
    if (!d.open || id.empty()) return {};
    auto it = d.idToRel.find(id);
    if (it == d.idToRel.end() && std::chrono::steady_clock::now() - d.lastScan > std::chrono::seconds(1)) {
        scanLocked(d);   // (moved outside the editor since: look again)
        it = d.idToRel.find(id);
    }
    return it == d.idToRel.end() ? fs::path() : d.root / fs::u8path(it->second);
}

std::string heal(const std::string& stored, const std::string& id, bool* healed) {
    if (healed) *healed = false;
    if (stored.empty() || id.empty()) return stored;
    std::string file, fragment;
    split(stored, file, fragment);
    if (vfs::exists(fs::u8path(project::resolve(file)))) return stored;
    fs::path now = pathOf(id);
    if (now.empty()) return stored;
    if (healed) *healed = true;
    return project::toStored(now.u8string()) + fragment;
}

json::Value importSettings(const fs::path& file) {
    json::Value doc;
    if (readMeta(metaPath(file), doc) && doc["import"].isObject()) return doc["import"];
    return json::Value::object();
}

bool setImportSettings(const fs::path& file, const json::Value& settings, std::string& error) {
    if (!writable()) { error = "the asset database is read-only"; return false; }
    if (idOf(file).empty()) { error = "not a file in the project: " + file.u8string(); return false; }
    json::Value doc;
    if (!readMeta(metaPath(file), doc)) { error = "can't read " + metaPath(file).u8string(); return false; }
    doc.set("import", settings);
    if (!writeMeta(metaPath(file), doc)) { error = "can't write " + metaPath(file).u8string(); return false; }
    return true;
}

bool move(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    if (!fs::exists(from, ec)) { error = from.u8string() + " doesn't exist"; return false; }
    if (fs::exists(to, ec)) { error = to.u8string() + " already exists"; return false; }
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (ec) { error = "can't move " + from.u8string() + ": " + ec.message(); return false; }
    if (fs::exists(metaPath(from), ec)) fs::rename(metaPath(from), metaPath(to), ec);   // (a folder's contents went along)
    Db& d = db();
    std::lock_guard<std::mutex> lock(d.m);
    std::string a = relative(d, from), b = relative(d, to);
    if (!a.empty() && !b.empty()) rekeyLocked(d, a, b);
    return true;
}

bool remove(const fs::path& file, std::string& error) {
    std::error_code ec;
    fs::remove_all(file, ec);
    if (ec) { error = "can't delete " + file.u8string() + ": " + ec.message(); return false; }
    fs::remove(metaPath(file), ec);
    Db& d = db();
    std::lock_guard<std::mutex> lock(d.m);
    std::string rel = relative(d, file);
    if (rel.empty()) return true;
    for (auto it = d.relToId.begin(); it != d.relToId.end();) {
        if (it->first == rel || it->first.compare(0, rel.size() + 1, rel + "/") == 0) {
            d.idToRel.erase(it->second);
            it = d.relToId.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

std::string retarget(const std::string& ref, const fs::path& from, const fs::path& to, bool* changed) {
    if (changed) *changed = false;
    std::string name = from.filename().u8string();
    if (ref.empty() || name.empty() || ref.find(name) == std::string::npos) return ref;   // (cheap: most strings aren't)
    std::string file, fragment;
    split(ref, file, fragment);
    fs::path abs = normal(fs::u8path(project::resolve(file)));
    fs::path rel = abs.lexically_relative(normal(from));
    if (rel.empty() || *rel.begin() == "..") return ref;
    std::error_code ec;
    if (rel == "." && fs::is_directory(to, ec)) return ref;   // (a folder's own name: an entity's, say; never a reference)
    fs::path now = rel == "." ? normal(to) : normal(to) / rel;
    if (changed) *changed = true;
    bool wasRelative = fs::u8path(file).is_relative();
    return (wasRelative ? project::toStored(now.u8string()) : now.generic_u8string()) + fragment;
}

int retargetJson(json::Value& doc, const fs::path& from, const fs::path& to) {
    int count = 0;
    if (doc.type() == json::Value::Type::String) {
        bool changed = false;
        std::string s = retarget(doc.asString(), from, to, &changed);
        if (changed) {
            doc = json::Value(s);
            ++count;
        }
    } else if (doc.isArray()) {
        for (json::Value& v : doc.elements()) count += retargetJson(v, from, to);
    } else if (doc.isObject()) {
        for (auto& [key, v] : doc.members()) count += retargetJson(v, from, to);
    }
    return count;
}

int retargetProjectFiles(const fs::path& from, const fs::path& to, std::vector<fs::path>* changedFiles) {
    fs::path root;
    {
        std::lock_guard<std::mutex> lock(db().m);
        root = db().root;
    }
    if (root.empty()) return 0;
    int total = 0;
    std::vector<fs::path> files = vfs::list(root, ".vkw");
    for (const fs::path& p : vfs::list(root, ".prefab")) files.push_back(p);
    files.push_back(root / "project.json");
    std::error_code ec;
    for (const fs::path& file : files) {
        if (!fs::is_regular_file(file, ec)) continue;   // (on disk only: packs don't change)
        std::string text, error;
        json::Value doc;
        if (!vfs::read(file, text) || !json::parse(text, doc, error)) continue;
        int n = retargetJson(doc, from, to);
        if (!n) continue;
        std::ofstream f(file, std::ios::binary);
        f << json::write(doc);
        if (!f) continue;
        total += n;
        if (changedFiles) changedFiles->push_back(file);
    }
    return total;
}

}  // namespace assetdb
