#include "app/updates.h"

#include <atomic>
#include <mutex>
#include <thread>

#include "core/json.h"
#include "core/version.h"
#include "platform/platform.h"

namespace updates {

bool parseRelease(const std::string& text, Release& out, std::string& error) {
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
    } else if (!doc["tag_name"].asString().empty()) {   // a GitHub release
        r.version = doc["tag_name"].asString();
        if (!r.version.empty() && (r.version[0] == 'v' || r.version[0] == 'V')) r.version.erase(0, 1);
        r.url = doc["html_url"].asString();
        r.notes = doc["body"].asString();
    } else {
        error = "no \"version\" (or GitHub \"tag_name\") in the answer";
        return false;
    }
    out = r;
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
