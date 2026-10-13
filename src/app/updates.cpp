#include "app/updates.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "core/json.h"
#include "core/sha256.h"
#include "core/version.h"
#include "core/vfs.h"
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

bool parseRelease(const std::string& text, Release& out, std::string& error, const std::string& platform) {
    json::Value doc;
    if (!json::parse(text, doc, error)) {
        error = "not JSON: " + error;
        return false;
    }
    Release r;
    if (!doc["version"].asString().empty()) {   // ours
        r.version = doc["version"].asString();
        r.url = doc["url"].asString();
        r.notes = doc["notes"].asString();
        const json::Value& d = doc["downloads"][platform];
        r.download = d["url"].asString();
        r.sha256 = d["sha256"].asString();
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
    return true;
}

void cleanUp(const fs::path& installDir) {
    std::error_code ec;
    if (!fs::exists(installDir / ".update", ec)) return;   // (no update since: nothing aside, and no folder walked)
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
};

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

void Install::start(const Release& release, const fs::path& installDir, Fetch fetch) {
    if (!fetch)   // (a few minutes for a big game)
        fetch = [](const std::string& u, std::string& body, std::string& error) { return platform::httpGet(u, body, error, 600); };
    auto state = std::make_shared<State>();
    state->stage = Stage::Downloading;
    state->installDir = installDir;
    state_ = state;
    std::thread([state, release, installDir, fetch] {
        auto set = [&](Stage s, const std::string& error = "") {
            std::lock_guard<std::mutex> lock(state->m);
            state->stage = s;
            state->error = error;
        };
        if (release.download.empty()) return set(Stage::Failed, "this release has no update for " + std::string(platformKey()));
        if (release.sha256.size() != 64) return set(Stage::Failed, "the update has no SHA-256 to check it against: not installed");
        std::error_code ec;
        fs::path dir = installDir / ".update";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        std::string body, error;
        if (!fetch(release.download, body, error)) return set(Stage::Failed, "download: " + error);
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
    if (!swapIn(staged, dir, error)) return false;
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

void Check::start(const std::string& url, const std::string& current, Fetch fetch) {
    if (!fetch)
        fetch = [](const std::string& u, std::string& body, std::string& error) { return platform::httpGet(u, body, error, 10); };
    auto state = std::make_shared<State>();
    state_ = state;
    std::thread([state, url, current, fetch] {   // (detached: holds the state itself; nobody waits for it)
        std::string body, error;
        Release r;
        bool ok = fetch(url, body, error) && parseRelease(body, r, error);
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
