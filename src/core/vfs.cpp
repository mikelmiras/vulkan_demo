#include "core/vfs.h"

#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

// Pack layout: the files' bytes one after another, then a table (per file: path length (u32), path, offset (u64),
// size (u64)), the table's size (u64), and a footer: the table's offset (u64), the file count (u32) and an 8-byte
// magic. Offsets are from the start of the pack, which may sit at the end of an executable: the footer is always at
// the very end of the file, and the table's size and offset tell where the pack starts.
namespace {

const char kMagic[8] = {'V', 'K', 'W', 'P', 'A', 'K', '0', '1'};

struct Entry {
    uint64_t offset, size;
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

}  // namespace

namespace vfs {

bool mount(const fs::path& file, const fs::path& root) {
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
    if (!get(in, tableAt) || !get(in, count) || !in.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0) return false;
    auto pack = std::make_unique<Pack>();
    pack->file = file;
    std::error_code ec;
    pack->root = fs::weakly_canonical(root, ec);
    uint64_t tableSize = 0;
    in.seekg((std::streamoff)(tableEnd - 8));
    if (!get(in, tableSize) || tableSize + 8 > tableEnd || tableAt > tableEnd) return false;
    uint64_t tableStart = tableEnd - 8 - tableSize;
    pack->base = tableStart - tableAt;
    in.seekg((std::streamoff)tableStart);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = 0;
        Entry e{};
        if (!get(in, len) || len > 4096) return false;
        std::string name(len, '\0');
        if (!in.read(name.data(), len) || !get(in, e.offset) || !get(in, e.size)) return false;
        if (pack->base + e.offset + e.size > tableStart) return false;
        pack->entries.emplace(std::move(name), e);
    }
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
            if (it != g_pack->entries.end()) {
                std::ifstream in(g_pack->file, std::ios::binary);
                in.seekg((std::streamoff)(g_pack->base + it->second.offset));
                out.resize((size_t)it->second.size);
                return (bool)in.read(out.data(), (std::streamsize)it->second.size);
            }
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

bool writePack(const fs::path& outPath, const std::vector<std::pair<std::string, fs::path>>& files, bool append,
               std::string& error) {
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
        uint64_t offset, size;
    };
    std::vector<Item> items;
    std::vector<char> buffer(1 << 16);
    for (const auto& [name, file] : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) { error = "cannot read " + file.u8string(); return false; }
        uint64_t start = at;
        while (in) {
            in.read(buffer.data(), (std::streamsize)buffer.size());
            if (in.gcount() > 0) write(buffer.data(), (size_t)in.gcount());
        }
        items.push_back({key(fs::u8path(name)), start, at - start});
    }
    uint64_t tableAt = at;
    for (const Item& i : items) {
        uint32_t len = (uint32_t)i.name.size();
        write(&len, sizeof(len));
        write(i.name.data(), i.name.size());
        write(&i.offset, sizeof(i.offset));
        write(&i.size, sizeof(i.size));
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

}  // namespace vfs
