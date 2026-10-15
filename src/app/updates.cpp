#include "app/updates.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "core/json.h"
#include "core/sha256.h"
#include "core/version.h"
#include "core/vfs.h"
#include "net/crypto.h"
#include "platform/platform.h"

namespace fs = std::filesystem;

namespace updates {

const char* platformKey() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string runningPlatform() {
#if !defined(_WIN32) && !defined(__APPLE__)
    if (!appImagePath().empty()) return "linux-appimage";
#endif
    return platformKey();
}

fs::path appImagePath() {
    const char* p = std::getenv("APPIMAGE");
    return p && *p ? fs::u8path(p) : fs::path();
}

// ---------------------------------------------------------------------------------------------
// Chunks: a gear rolling hash (one random number per byte value, shifted in) cuts where its top 16 bits are all 0
// ---------------------------------------------------------------------------------------------
namespace {

constexpr size_t kMinChunk = 16 * 1024, kMaxChunk = 256 * 1024;
constexpr uint64_t kCutMask = 0xFFFFull << 48;   // (the top bits: they depend on the last 64 bytes)

const uint64_t* gearTable() {
    static uint64_t table[256];
    static bool made = false;
    if (!made) {   // (splitmix64 from a fixed seed: the same cuts on every machine and build)
        uint64_t x = 0x9E3779B97F4A7C15ull;
        for (uint64_t& t : table) {
            uint64_t z = (x += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            t = z ^ (z >> 31);
        }
        made = true;
    }
    return table;
}

bool readFile(const fs::path& file, std::string& out) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeFileAtomically(const fs::path& file, const std::string& bytes) {
    fs::path part = fs::path(file) += ".part";
    {
        std::ofstream f(part, std::ios::binary);
        if (!f.write(bytes.data(), (std::streamsize)bytes.size())) return false;
    }
    std::error_code ec;
    fs::rename(part, file, ec);
    return !ec;
}

}  // namespace

std::vector<std::pair<size_t, size_t>> cutChunks(const uint8_t* data, size_t size) {
    const uint64_t* gear = gearTable();
    std::vector<std::pair<size_t, size_t>> out;
    size_t at = 0;
    while (at < size) {
        size_t left = size - at;
        if (left <= kMinChunk) {
            out.push_back({at, left});
            break;
        }
        size_t end = std::min(left, kMaxChunk), i = kMinChunk;
        uint64_t h = 0;
        for (; i < end; ++i) {
            h = (h << 1) + gear[data[at + i]];
            if (!(h & kCutMask)) {
                ++i;
                break;
            }
        }
        out.push_back({at, i});
        at += i;
    }
    return out;
}

bool describeFile(const fs::path& file, const std::string& path, ReleaseFile& out, std::string& error, const fs::path& chunkDir) {
    std::string bytes;
    if (!readFile(file, bytes)) {
        error = "cannot read " + file.u8string();
        return false;
    }
    out = {};
    out.path = path;
    out.size = bytes.size();
    out.sha256 = sha256::of(bytes);
    std::error_code ec;
    out.executable = (fs::status(file, ec).permissions() & fs::perms::owner_exec) != fs::perms::none;
    for (const auto& [offset, size] : cutChunks(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())) {
        std::string piece = bytes.substr(offset, size);
        Chunk c{sha256::of(piece), (uint32_t)size};
        if (!chunkDir.empty() && !fs::exists(chunkDir / c.sha256, ec)) {
            fs::create_directories(chunkDir, ec);
            if (!writeFileAtomically(chunkDir / c.sha256, piece)) {
                error = "cannot write " + (chunkDir / c.sha256).u8string();
                return false;
            }
        }
        out.chunks.push_back(std::move(c));
    }
    return true;
}

json::Value filesToJson(const std::vector<ReleaseFile>& files) {
    json::Value list = json::Value::array();
    for (const ReleaseFile& f : files) {
        json::Value v = json::Value::object();
        v.set("path", f.path);
        v.set("size", (double)f.size);
        v.set("sha256", f.sha256);
        if (f.executable) v.set("executable", true);
        json::Value& chunks = v.set("chunks", json::Value::array());
        for (const Chunk& c : f.chunks) {
            json::Value pair = json::Value::array();
            pair.push(c.sha256);
            pair.push((double)c.size);
            chunks.push(std::move(pair));
        }
        list.push(std::move(v));
    }
    return list;
}

std::vector<ReleaseFile> filesFromJson(const json::Value& v) {
    std::vector<ReleaseFile> out;
    for (const json::Value& e : v.elements()) {
        ReleaseFile f;
        f.path = e["path"].asString();
        f.size = (uint64_t)e["size"].asNumber(0.0);
        f.sha256 = e["sha256"].asString();
        f.executable = e["executable"].asBool(false);
        for (const json::Value& c : e["chunks"].elements()) f.chunks.push_back({c[0].asString(), (uint32_t)c[1].asNumber(0.0)});
        // (no escaping the game's folder)
        if (f.path.empty() || f.path.find("..") != std::string::npos || f.path[0] == '/' || f.path[0] == '\\' ||
            f.path.find(':') != std::string::npos)
            continue;
        out.push_back(std::move(f));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Signed manifests
// ---------------------------------------------------------------------------------------------
std::string signedText(const json::Value& manifest) {
    json::Value copy = manifest;
    auto& members = copy.members();
    members.erase(std::remove_if(members.begin(), members.end(), [](const auto& m) { return m.first == "signature"; }),
                  members.end());
    return json::write(copy);
}

bool signManifest(json::Value& manifest, const std::string& secretKeyHex, std::string& error) {
    uint8_t seed[32];
    if (!crypto::fromHex(secretKeyHex, seed, 32)) {
        error = "the secret key isn't 64 hex digits";
        return false;
    }
    crypto::SigningKey key = crypto::SigningKey::fromSeed(seed);
    std::string text = signedText(manifest);
    std::array<uint8_t, 64> sig = crypto::sign(key, text.data(), text.size());
    auto& members = manifest.members();
    members.erase(std::remove_if(members.begin(), members.end(), [](const auto& m) { return m.first == "signature"; }),
                  members.end());
    manifest.set("signature", crypto::toHex(sig.data(), sig.size()));
    return true;
}

bool checkSignature(const json::Value& manifest, const std::string& publicKeyHex, std::string& error) {
    uint8_t key[32], sig[64];
    if (!crypto::fromHex(publicKeyHex, key, 32)) {
        error = "the game's updateKey isn't a public key (64 hex digits)";
        return false;
    }
    if (!crypto::fromHex(manifest["signature"].asString(), sig, 64)) {
        error = "the update isn't signed: ignored";
        return false;
    }
    std::string text = signedText(manifest);
    if (!crypto::verify(key, text.data(), text.size(), sig)) {
        error = "the update's signature isn't this game's: ignored";
        return false;
    }
    return true;
}

std::string resolveUrl(const std::string& base, const std::string& relative) {
    if (relative.find("://") != std::string::npos) return relative;
    size_t scheme = base.find("://");
    if (!relative.empty() && relative[0] == '/') {   // (from the host's root)
        size_t path = scheme == std::string::npos ? std::string::npos : base.find('/', scheme + 3);
        return (path == std::string::npos ? base : base.substr(0, path)) + relative;
    }
    size_t slash = base.rfind('/');
    if (slash == std::string::npos || (scheme != std::string::npos && slash < scheme + 3)) return base + "/" + relative;
    return base.substr(0, slash + 1) + relative;
}

bool parseRelease(const std::string& text, Release& out, std::string& error, const std::string& platform,
                  const std::string& publicKey) {
    json::Value doc;
    if (!json::parse(text, doc, error)) {
        error = "not JSON: " + error;
        return false;
    }
    if (!publicKey.empty() && !checkSignature(doc, publicKey, error)) return false;
    Release r;
    if (!doc["version"].asString().empty()) {   // ours
        r.version = doc["version"].asString();
        r.url = doc["url"].asString();
        r.notes = doc["notes"].asString();
        const json::Value& d = doc["downloads"][platform];
        r.download = d["url"].asString();
        r.sha256 = d["sha256"].asString();
        if (d["files"].isArray()) {   // a chunked release
            r.files = filesFromJson(d["files"]);
            r.chunkUrl = d["chunks"].asString().empty() ? "chunks/" : d["chunks"].asString();
        }
    } else if (!doc["tag_name"].asString().empty()) {   // a GitHub release
        r.version = doc["tag_name"].asString();
        if (!r.version.empty() && (r.version[0] == 'v' || r.version[0] == 'V')) r.version.erase(0, 1);
        r.url = doc["html_url"].asString();
        r.notes = doc["body"].asString();
        for (const json::Value& a : doc["assets"].elements()) {   // (this platform's update pack)
            std::string name = a["name"].asString(), lower = name;
            for (char& c : lower) c = (char)std::tolower((unsigned char)c);
            bool pack = lower.size() > 10 && lower.compare(lower.size() - 10, 10, ".vkwupdate") == 0;
            if (!pack || lower.find(platform) == std::string::npos) continue;
            r.download = a["browser_download_url"].asString();
            std::string digest = a["digest"].asString();
            if (digest.rfind("sha256:", 0) == 0) r.sha256 = digest.substr(7);
            break;
        }
    } else {
        error = "no \"version\" (or GitHub \"tag_name\") in the answer";
        return false;
    }
    for (char& c : r.sha256) c = (char)std::tolower((unsigned char)c);
    out = r;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------------------------
namespace {

const char* kAside = ".old-update";

bool moveFile(const fs::path& from, const fs::path& to, std::error_code& ec) {
    fs::rename(from, to, ec);
    if (!ec) return true;
    ec.clear();   // (another drive: copied, then removed)
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::remove(from, ec);
    ec.clear();
    return true;
}

}  // namespace

bool swapIn(const fs::path& staged, const fs::path& installDir, std::string& error) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(staged, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(fs::relative(it->path(), staged, ec));
    if (ec || files.empty()) {
        error = "nothing to install in " + staged.u8string();
        return false;
    }
    std::sort(files.begin(), files.end());
    struct Done {
        fs::path dest;
        bool hadOld;
    };
    std::vector<Done> done;
    auto undo = [&] {   // (the game as it was)
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            std::error_code e;
            fs::remove(it->dest, e);
            if (it->hadOld) fs::rename(fs::path(it->dest) += kAside, it->dest, e);
        }
    };
    for (const fs::path& rel : files) {
        fs::path dest = installDir / rel, aside = fs::path(dest) += kAside, from = staged / rel;
        fs::create_directories(dest.parent_path(), ec);
        bool hadOld = fs::exists(dest, ec);
        if (hadOld) {
            fs::permissions(from, fs::status(dest, ec).permissions(), ec);   // (executables stay executable)
            fs::remove(aside, ec);
            fs::rename(dest, aside, ec);   // (a running executable can be renamed, also on Windows)
            if (ec) {
                error = "cannot move " + dest.u8string() + " aside: " + ec.message();
                undo();
                return false;
            }
        }
        if (!moveFile(from, dest, ec)) {
            error = "cannot put " + dest.u8string() + " in place: " + ec.message();
            if (hadOld) fs::rename(aside, dest, ec);
            undo();
            return false;
        }
        done.push_back({dest, hadOld});
    }
    fs::create_directories(installDir / ".update", ec);   // (installed: the next start clears it all away)
    std::ofstream(installDir / ".update" / "applied") << "installed\n";
    return true;
}

void cleanUp(const fs::path& installDir) {
    std::error_code ec;
    if (!fs::exists(installDir / ".update", ec)) return;   // (no update since: nothing aside, and no folder walked)
    if (!fs::exists(installDir / ".update" / "applied", ec)) {   // not installed: the chunks stay, to resume with
        fs::remove_all(installDir / ".update" / "staged", ec);
        for (auto it = fs::directory_iterator(installDir / ".update", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->path().extension() == ".vkwupdate") fs::remove(it->path(), ec);
        return;
    }
    std::vector<fs::path> aside;
    for (auto it = fs::recursive_directory_iterator(installDir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && it->path().filename() == ".update") {
            it.disable_recursion_pending();
            continue;
        }
        std::string name = it->path().filename().u8string();
        if (name.size() > 11 && name.compare(name.size() - 11, 11, kAside) == 0) aside.push_back(it->path());
    }
    for (const fs::path& p : aside) fs::remove(p, ec);
    fs::remove_all(installDir / ".update", ec);
}

struct Install::State {
    std::mutex m;
    Stage stage = Stage::Idle;
    std::string error;
    fs::path staged, installDir;
    std::atomic<float> progress{0.f};
    std::atomic<uint64_t> downloaded{0};
};

namespace {

// A chunked release installed into `dir` (Install's thread): the files that changed are rebuilt in .update/staged
// from the chunks this machine has (in its old files, or downloaded before) and the ones it fetches (kept as they come)
bool installChunked(const Release& release, const fs::path& installDir, const Install::Fetch& fetch, const std::string& renameTo,
                    const std::function<void(Install::Stage)>& stage, std::atomic<float>& progress,
                    std::atomic<uint64_t>& downloaded, fs::path& stagedOut, std::string& error) {
    std::error_code ec;
    fs::path dir = installDir / ".update", chunks = dir / "chunks", staged = dir / "staged";
    fs::remove_all(staged, ec);
    fs::create_directories(chunks, ec);
    fs::create_directories(staged, ec);
    auto localName = [&](const ReleaseFile& f) { return renameTo.empty() || release.files.size() != 1 ? f.path : renameTo; };
    // Which files change, and the chunks the old ones have
    struct Local {
        fs::path file;
        size_t offset, size;
    };
    std::map<std::string, Local> have;
    std::vector<const ReleaseFile*> changed;
    for (const ReleaseFile& f : release.files) {
        fs::path local = installDir / fs::u8path(localName(f));
        std::string bytes;
        if (readFile(local, bytes)) {
            if (bytes.size() == f.size && sha256::of(bytes) == f.sha256) continue;   // (as it is)
            for (const auto& [offset, size] : cutChunks(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()))
                have.emplace(sha256::of(bytes.substr(offset, size)), Local{local, offset, size});
        }
        changed.push_back(&f);
    }
    // The chunks missing: downloaded (those from an earlier try are there already)
    std::vector<Chunk> missing;
    std::set<std::string> listed;
    uint64_t total = 0;
    for (const ReleaseFile* f : changed)
        for (const Chunk& c : f->chunks) {
            if (have.count(c.sha256) || !listed.insert(c.sha256).second) continue;
            if (fs::exists(chunks / c.sha256, ec) && fs::file_size(chunks / c.sha256, ec) == c.size) continue;
            missing.push_back(c);
            total += c.size;
        }
    uint64_t done = 0;
    for (const Chunk& c : missing) {
        std::string body, why;
        if (!fetch(release.chunkUrl + c.sha256, body, why)) {
            error = "download: " + why;
            return false;
        }
        if (body.size() != c.size || sha256::of(body) != c.sha256) {
            error = "a downloaded piece doesn't match its SHA-256 (" + c.sha256.substr(0, 12) + "...): not installed";
            return false;
        }
        if (!writeFileAtomically(chunks / c.sha256, body)) {
            error = "cannot write " + (chunks / c.sha256).u8string();
            return false;
        }
        done += c.size;
        downloaded += c.size;
        progress = total ? (float)((double)done / (double)total) : 1.f;
    }
    progress = 1.f;
    // Each changed file put together, and checked whole
    stage(Install::Stage::Verifying);
    for (const ReleaseFile* f : changed) {
        fs::path out = staged / fs::u8path(localName(*f));
        fs::create_directories(out.parent_path(), ec);
        std::ofstream file(out, std::ios::binary);
        sha256::Hasher hash;
        for (const Chunk& c : f->chunks) {
            std::string piece;
            auto local = have.find(c.sha256);
            if (local != have.end()) {
                std::ifstream in(local->second.file, std::ios::binary);
                piece.resize(local->second.size);
                in.seekg((std::streamoff)local->second.offset);
                in.read(&piece[0], (std::streamsize)piece.size());
                if (!in) piece.clear();
            } else {
                readFile(chunks / c.sha256, piece);
            }
            if (piece.size() != c.size) {
                error = "a piece of " + f->path + " went missing: try again";
                return false;
            }
            hash.add(piece.data(), piece.size());
            file.write(piece.data(), (std::streamsize)piece.size());
        }
        file.close();
        if (!file || hash.hex() != f->sha256) {
            error = f->path + " doesn't match its SHA-256 once put together: not installed";
            return false;
        }
        if (f->executable)
            fs::permissions(out, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    }
    stagedOut = staged;
    return true;
}

}  // namespace

const char* Install::stageName(Stage stage) {
    switch (stage) {
    case Stage::Downloading: return "downloading";
    case Stage::Verifying: return "verifying";
    case Stage::Unpacking: return "unpacking";
    case Stage::Ready: return "ready";
    case Stage::Failed: return "failed";
    default: return "";
    }
}

void Install::start(const Release& release, const fs::path& installDir, Fetch fetch, const std::string& renameTo) {
    if (!fetch)   // (a few minutes for a big game)
        fetch = [](const std::string& u, std::string& body, std::string& error) { return platform::httpGet(u, body, error, 600); };
    auto state = std::make_shared<State>();
    state->stage = Stage::Downloading;
    state->installDir = installDir;
    state_ = state;
    std::thread([state, release, installDir, fetch, renameTo] {
        auto set = [&](Stage s, const std::string& error = "") {
            std::lock_guard<std::mutex> lock(state->m);
            state->stage = s;
            state->error = error;
        };
        std::error_code ec;
        fs::path dir = installDir / ".update";
        fs::remove(dir / "applied", ec);
        if (release.chunked()) {
            fs::path staged;
            std::string error;
            if (!installChunked(release, installDir, fetch, renameTo, set, state->progress, state->downloaded, staged, error))
                return set(Stage::Failed, error);
            {
                std::lock_guard<std::mutex> lock(state->m);
                state->staged = staged;
            }
            return set(Stage::Ready);
        }
        if (release.download.empty()) return set(Stage::Failed, "this release has no update for " + std::string(runningPlatform()));
        if (release.sha256.size() != 64) return set(Stage::Failed, "the update has no SHA-256 to check it against: not installed");
        fs::remove_all(dir / "staged", ec);
        fs::create_directories(dir, ec);
        std::string body, error;
        if (!fetch(release.download, body, error)) return set(Stage::Failed, "download: " + error);
        state->downloaded += body.size();
        state->progress = 1.f;
        set(Stage::Verifying);
        std::string digest = sha256::of(body);
        if (digest != release.sha256) return set(Stage::Failed, "the download doesn't match its SHA-256 (" + digest + "): not installed");
        fs::path pack = dir / ("update-" + release.version + ".vkwupdate");
        {
            std::ofstream f(pack, std::ios::binary);
            if (!f.write(body.data(), (std::streamsize)body.size())) return set(Stage::Failed, "cannot write " + pack.u8string());
        }
        body = std::string();
        set(Stage::Unpacking);
        std::vector<std::string> files;
        if (!vfs::extractPack(pack, dir / "staged", files, error)) return set(Stage::Failed, "unpacking: " + error);
        {
            std::lock_guard<std::mutex> lock(state->m);
            state->staged = dir / "staged";
        }
        set(Stage::Ready);
    }).detach();
}

Install::Stage Install::stage() const {
    if (!state_) return Stage::Idle;
    std::lock_guard<std::mutex> lock(state_->m);
    return state_->stage;
}

std::string Install::error() const {
    if (!state_) return "";
    std::lock_guard<std::mutex> lock(state_->m);
    return state_->error;
}

float Install::progress() const { return state_ ? state_->progress.load() : 0.f; }
uint64_t Install::downloaded() const { return state_ ? state_->downloaded.load() : 0; }

bool Install::apply(std::string& error) {
    if (stage() != Stage::Ready) {
        error = "no update ready";
        return false;
    }
    fs::path staged, dir;
    {
        std::lock_guard<std::mutex> lock(state_->m);
        staged = state_->staged;
        dir = state_->installDir;
    }
    std::error_code ec;
    bool any = false;   // (a chunked release with nothing changed: nothing to swap)
    for (auto it = fs::recursive_directory_iterator(staged, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        any = any || it->is_regular_file(ec);
    if (any && !swapIn(staged, dir, error)) return false;
    if (!any) std::ofstream(dir / ".update" / "applied") << "installed\n";   // (swapIn says so itself)
    std::lock_guard<std::mutex> lock(state_->m);
    state_->stage = Stage::Idle;
    return true;
}

struct Check::State {
    std::mutex m;
    std::atomic<bool> done{false};
    bool newer = false;
    Release release;
    std::string error;
};

void Check::start(const std::string& url, const std::string& current, Fetch fetch, const std::string& publicKey,
                  const std::string& platform) {
    if (!fetch)
        fetch = [](const std::string& u, std::string& body, std::string& error) { return platform::httpGet(u, body, error, 10); };
    auto state = std::make_shared<State>();
    state_ = state;
    std::string which = platform.empty() ? runningPlatform() : platform;
    std::thread([state, url, current, fetch, publicKey, which] {   // (detached: holds the state itself; nobody waits)
        std::string body, error;
        Release r;
        bool ok = fetch(url, body, error) && parseRelease(body, r, error, which, publicKey);
        if (ok && r.chunked()) r.chunkUrl = resolveUrl(url, r.chunkUrl);
        {
            std::lock_guard<std::mutex> lock(state->m);
            if (ok) {
                state->release = r;
                state->newer = version::compare(r.version, current) > 0;
            } else {
                state->error = error;
            }
        }
        state->done = true;
    }).detach();
}

bool Check::ready() const { return state_ && state_->done; }

bool Check::newer() const {
    if (!ready()) return false;
    std::lock_guard<std::mutex> lock(state_->m);
    return state_->newer;
}

Release Check::release() const {
    if (!ready()) return {};
    std::lock_guard<std::mutex> lock(state_->m);
    return state_->release;
}

std::string Check::error() const {
    if (!ready()) return "";
    std::lock_guard<std::mutex> lock(state_->m);
    return state_->error;
}

}  // namespace updates
