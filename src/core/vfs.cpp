#include "core/vfs.h"

#include "core/compress.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

// Pack layout: the files' bytes one after another, then a table (per file: path length (u32), path, offset (u64),
// size (u64), and from version 2 the size as stored (u64): smaller when compressed, see core/compress.h), the
// table's size (u64), and a footer: the table's offset (u64), the file count (u32) and an 8-byte magic. Offsets are from the start of the pack, which may sit at the end of an executable: the footer is always at
// the very end of the file, and the table's size and offset tell where the pack starts.
namespace {

const char kMagic[8] = {'V', 'K', 'W', 'P', 'A', 'K', '0', '2'};
const char kMagicV1[8] = {'V', 'K', 'W', 'P', 'A', 'K', '0', '1'};   // (no compression: still read)

struct Entry {
    uint64_t offset, size, stored;   // stored < size: compressed
};

struct Pack {
    fs::path file, root;
    uint64_t base = 0;   // where the pack starts in the file
    std::unordered_map<std::string, Entry> entries;
};

std::mutex g_mutex;
Pack* g_pack = nullptr;

template <class T> bool get(std::istream& in, T& v) { return (bool)in.read(reinterpret_cast<char*>(&v), sizeof(T)); }

std::string key(const fs::path& p) {
    std::string s = p.lexically_normal().generic_u8string();
    while (s.rfind("./", 0) == 0) s = s.substr(2);
    return s;
}

// The pack's name for a path (relative to its root), or "" if the path is outside it
std::string inPack(const Pack& pack, const fs::path& path) {
    if (path.is_relative()) return key(path);
    std::error_code ec;
    fs::path rel = fs::relative(fs::weakly_canonical(path, ec), pack.root, ec);
    if (ec || rel.empty() || rel.begin()->u8string() == "..") return {};
    return key(rel);
}

// A pack's table: its entries and where it starts in the file (false if `file` has no pack)
bool openPack(const fs::path& file, Pack& pack) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    uint64_t end = (uint64_t)in.tellg();
    const uint64_t footer = 8 + 4 + sizeof(kMagic);
    if (end < footer) return false;
    in.seekg((std::streamoff)(end - footer));
    uint64_t tableEnd = end - footer, tableAt = 0;
    uint32_t count = 0;
    char magic[8];
    if (!get(in, tableAt) || !get(in, count) || !in.read(magic, 8)) return false;
    bool v1 = std::memcmp(magic, kMagicV1, 8) == 0;
    if (!v1 && std::memcmp(magic, kMagic, 8) != 0) return false;
    pack.file = file;
    uint64_t tableSize = 0;
    in.seekg((std::streamoff)(tableEnd - 8));
    if (!get(in, tableSize) || tableSize + 8 > tableEnd || tableAt > tableEnd) return false;
    uint64_t tableStart = tableEnd - 8 - tableSize;
    pack.base = tableStart - tableAt;
    in.seekg((std::streamoff)tableStart);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = 0;
        Entry e{};
        if (!get(in, len) || len > 4096) return false;
        std::string name(len, '\0');
        if (!in.read(name.data(), len) || !get(in, e.offset) || !get(in, e.size)) return false;
        e.stored = e.size;
        if (!v1 && !get(in, e.stored)) return false;
        if (e.stored > e.size || pack.base + e.offset + e.stored > tableStart) return false;
        pack.entries.emplace(std::move(name), e);
    }
    return true;
}

// One file of a pack, unpacked
bool readEntry(const Pack& pack, const Entry& e, std::string& out) {
    std::ifstream in(pack.file, std::ios::binary);
    in.seekg((std::streamoff)(pack.base + e.offset));
    if (e.stored == e.size) {
        out.resize((size_t)e.size);
        return (bool)in.read(out.data(), (std::streamsize)e.size);
    }
    std::string packed((size_t)e.stored, '\0');
    if (!in.read(packed.data(), (std::streamsize)e.stored)) return false;
    return lz::decompress(packed.data(), packed.size(), out, (size_t)e.size);
}

}  // namespace

namespace vfs {

bool mount(const fs::path& file, const fs::path& root) {
    auto pack = std::make_unique<Pack>();
    if (!openPack(file, *pack)) return false;
    std::error_code ec;
    pack->root = fs::weakly_canonical(root, ec);
    std::lock_guard<std::mutex> lock(g_mutex);
    delete g_pack;
    g_pack = pack.release();
    return true;
}

void unmount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    delete g_pack;
    g_pack = nullptr;
}

bool mounted() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_pack != nullptr;
}

bool read(const fs::path& path, std::string& out) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pack) {
            std::string name = inPack(*g_pack, path);
            auto it = name.empty() ? g_pack->entries.end() : g_pack->entries.find(name);
            if (it != g_pack->entries.end()) return readEntry(*g_pack, it->second, out);
        }
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool exists(const fs::path& path) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pack) {
            std::string name = inPack(*g_pack, path);
            if (!name.empty() && g_pack->entries.count(name)) return true;
        }
    }
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

std::vector<fs::path> list(const fs::path& dir, const std::string& extension) {
    auto matches = [&](const std::string& name) {
        return name.size() >= extension.size() && name.compare(name.size() - extension.size(), extension.size(), extension) == 0;
    };
    std::vector<std::string> rel;   // inside `dir`, generic
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pack) {
            std::string prefix = inPack(*g_pack, dir);
            if (!prefix.empty() || dir.empty() || key(dir) == ".") {
                if (!prefix.empty() && prefix != ".") prefix += '/';
                else prefix.clear();
                for (const auto& [name, entry] : g_pack->entries)
                    if (name.compare(0, prefix.size(), prefix) == 0 && matches(name)) rel.push_back(name.substr(prefix.size()));
            }
        }
    }
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string name = it->path().lexically_relative(dir).generic_u8string();
        if (matches(name)) rel.push_back(name);
    }
    std::sort(rel.begin(), rel.end());
    rel.erase(std::unique(rel.begin(), rel.end()), rel.end());
    std::vector<fs::path> out;
    for (const std::string& r : rel) out.push_back(dir / fs::u8path(r));
    return out;
}

bool writePack(const fs::path& outPath, const std::vector<std::pair<std::string, fs::path>>& files, bool append,
               std::string& error, bool compress) {
    std::ofstream out(outPath, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (!out) { error = "cannot write " + outPath.u8string(); return false; }
    // Positions are counted here, from the pack's start (a stream opened for appending may not report them)
    uint64_t at = 0;
    auto write = [&](const void* data, size_t size) {
        out.write(static_cast<const char*>(data), (std::streamsize)size);
        at += size;
    };
    struct Item {
        std::string name;
        uint64_t offset, size, stored;
    };
    std::vector<Item> items;
    for (const auto& [name, file] : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) { error = "cannot read " + file.u8string(); return false; }
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string bytes = ss.str();
        uint64_t start = at;
        // Compressed when that saves something worth it (images and sounds usually won't shrink)
        std::string packed = compress && bytes.size() >= 64 ? lz::compress(bytes.data(), bytes.size()) : std::string();
        bool useIt = !packed.empty() && packed.size() < bytes.size() - bytes.size() / 20;
        const std::string& stored = useIt ? packed : bytes;
        write(stored.data(), stored.size());
        items.push_back({key(fs::u8path(name)), start, bytes.size(), stored.size()});
    }
    uint64_t tableAt = at;
    for (const Item& i : items) {
        uint32_t len = (uint32_t)i.name.size();
        write(&len, sizeof(len));
        write(i.name.data(), i.name.size());
        write(&i.offset, sizeof(i.offset));
        write(&i.size, sizeof(i.size));
        write(&i.stored, sizeof(i.stored));
    }
    uint64_t tableSize = at - tableAt;
    uint32_t count = (uint32_t)items.size();
    write(&tableSize, sizeof(tableSize));
    write(&tableAt, sizeof(tableAt));
    write(&count, sizeof(count));
    write(kMagic, sizeof(kMagic));
    if (!out) { error = "cannot write " + outPath.u8string(); return false; }
    return true;
}

bool extractPack(const fs::path& file, const fs::path& outDir, std::vector<std::string>& files, std::string& error) {
    Pack pack;
    if (!openPack(file, pack)) {
        error = "not a pack: " + file.u8string();
        return false;
    }
    files.clear();
    for (const auto& [name, e] : pack.entries) {
        fs::path rel = fs::u8path(name).lexically_normal();
        if (rel.empty() || rel.is_absolute() || rel.begin()->u8string() == "..") {   // (nothing outside the folder)
            error = "a file outside the pack's folder: " + name;
            return false;
        }
        std::string bytes;
        if (!readEntry(pack, e, bytes)) {
            error = "damaged: " + name;
            return false;
        }
        fs::path to = outDir / rel;
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        std::ofstream f(to, std::ios::binary);
        if (!f.write(bytes.data(), (std::streamsize)bytes.size())) {
            error = "cannot write " + to.u8string();
            return false;
        }
        files.push_back(rel.generic_u8string());
    }
    std::sort(files.begin(), files.end());
    return true;
}

}  // namespace vfs
