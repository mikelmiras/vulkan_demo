#include "app/crash_reports.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "platform/platform.h"

namespace crashreports {

namespace {

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}

}  // namespace

std::string prepare(const std::string& report, const std::string& home, const std::string& user) {
    std::string out = report;
    std::string h = home;
    while (h.size() > 1 && (h.back() == '/' || h.back() == '\\')) h.pop_back();
    if (h.size() > 1) replaceAll(out, h, "~");
    if (user.size() >= 3) replaceAll(out, user, "<user>");   // (in other paths, a machine name made from it)
    return out;
}

std::string prepare(const std::string& report) {
#if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
    const char* user = std::getenv("USERNAME");
#else
    const char* home = std::getenv("HOME");
    const char* user = std::getenv("USER");
#endif
    return prepare(report, home ? home : "", user ? user : "");
}

struct Upload::State {
    std::mutex m;
    std::atomic<bool> done{false};
    bool sent = false;
    std::string error;
};

void Upload::start(const std::string& url, const std::filesystem::path& report, Post post) {
    if (!post)
        post = [](const std::string& u, const std::string& body, std::string& error) {
            std::string answer;
            return platform::httpPost(u, body, "text/plain; charset=utf-8", answer, error, 20);
        };
    auto state = std::make_shared<State>();
    state_ = state;
    std::thread([state, url, report, post] {
        std::ifstream f(report, std::ios::binary);
        std::ostringstream text;
        text << f.rdbuf();
        std::string error;
        bool ok = false;
        if (!f || text.str().empty()) error = "no report at " + report.u8string();
        else if (url.empty()) error = "the project has no crashReportUrl";
        else ok = post(url, prepare(text.str()), error);
        if (ok) {   // (sent once: renamed)
            std::error_code ec;
            std::filesystem::path sentName = report;
            std::string name = report.filename().u8string();
            size_t at = name.find("-crash");
            sentName.replace_filename((at == std::string::npos ? name : name.substr(0, at)) + "-crash-sent.txt");
            std::filesystem::rename(report, sentName, ec);
        }
        {
            std::lock_guard<std::mutex> lock(state->m);
            state->sent = ok;
            state->error = error;
        }
        state->done = true;
    }).detach();
}

bool Upload::busy() const { return state_ && !state_->done; }
bool Upload::done() const { return state_ && state_->done; }

bool Upload::sent() const {
    if (!done()) return false;
    std::lock_guard<std::mutex> lock(state_->m);
    return state_->sent;
}

std::string Upload::error() const {
    if (!done()) return "";
    std::lock_guard<std::mutex> lock(state_->m);
    return state_->error;
}

}  // namespace crashreports
