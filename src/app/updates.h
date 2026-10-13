// Updates: is there a newer version than this one, and installing it. vkgame asks the project's updateUrl at start
// (players can turn it off: settings.checkUpdates) and puts the answer in the game state for menus; the editor's
// Help > Check for updates asks about the engine (it only says what's out and where).
//
// Installing (a game's menu: "@update", then "@restart"): the release names this platform's update pack (an export's
// .vkwupdate: the whole exported game as one pack, app/export.h) and its SHA-256. It's downloaded next to the game
// (<game>/.update/), checked against the digest (no digest, or a wrong one: nothing is installed), unpacked, and
// swapped in: each file of the game moved aside to <file>.old-update and the new one put in its place (all undone if
// one fails), so a restart runs the new version; the next start removes what was moved aside.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace updates {

struct Release {
    std::string version;   // "1.2.0"
    std::string url;       // where to get it (a page to open)
    std::string notes;     // what's new (may be empty)
    std::string download;  // this platform's update pack ("" if it has none: then it can only be opened)
    std::string sha256;    // ... and its digest (hex)
};

const char* platformKey();   // "linux", "windows", "macos"

// What an update URL serves: {"version": "1.2.0", "url": "https://...", "notes": "...", "downloads": {"linux":
// {"url": "...", "sha256": "..."}, ...}}, or GitHub's latest release (https://api.github.com/repos/<owner>/<repo>/
// releases/latest: tag_name, html_url, body, and the asset whose name has the platform in it and ends with
// .vkwupdate, its digest from the asset's "digest": "sha256:...")
bool parseRelease(const std::string& json, Release& out, std::string& error, const std::string& platform = platformKey());

// The files of `staged` (an unpacked update) swapped into `installDir` (see above); false and why if any can't be,
// with the game as it was. Unix permissions of the files replaced are kept.
bool swapIn(const std::filesystem::path& staged, const std::filesystem::path& installDir, std::string& error);
// What the last swap moved aside, and the update's leftovers, removed (at start: the old files aren't running now;
// only when <installDir>/.update is there, which an install leaves until then)
void cleanUp(const std::filesystem::path& installDir);

// One check, on a worker thread of its own (a slow network never holds up the game or its exit)
class Check {
public:
    using Fetch = std::function<bool(const std::string& url, std::string& body, std::string& error)>;
    // `current`: this program's version. `fetch`: platform::httpGet by default (tests give their own).
    void start(const std::string& url, const std::string& current, Fetch fetch = nullptr);
    bool started() const { return state_ != nullptr; }
    bool ready() const;          // answered (or failed)
    bool newer() const;          // a version after `current` is out
    Release release() const;     // what's out (when ready and fetched)
    std::string error() const;   // why it failed ("" if it didn't)

private:
    struct State;
    std::shared_ptr<State> state_;
};

// Downloading, checking and unpacking an update, on a worker thread; apply() then swaps it in (the caller's thread)
class Install {
public:
    using Fetch = Check::Fetch;
    enum class Stage { Idle, Downloading, Verifying, Unpacking, Ready, Failed };
    void start(const Release& release, const std::filesystem::path& installDir, Fetch fetch = nullptr);
    Stage stage() const;
    std::string error() const;
    bool apply(std::string& error);   // (when Ready) swapIn
    static const char* stageName(Stage stage);   // "downloading", ..., for the game state

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace updates
