// Update checks: is there a newer version than this one? vkgame asks the project's updateUrl at start (players can
// turn it off: settings.checkUpdates) and puts the answer in the game state for menus; the editor's Help > Check
// for updates asks about the engine. Nothing is ever downloaded or installed: it says what's out and where.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace updates {

struct Release {
    std::string version;   // "1.2.0"
    std::string url;       // where to get it (a page to open)
    std::string notes;     // what's new (may be empty)
};

// What an update URL serves: {"version": "1.2.0", "url": "https://...", "notes": "..."}, or GitHub's latest release
// (https://api.github.com/repos/<owner>/<repo>/releases/latest: tag_name, html_url, body)
bool parseRelease(const std::string& json, Release& out, std::string& error);

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

}  // namespace updates
