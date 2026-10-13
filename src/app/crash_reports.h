// Sending crash reports (core/crash.h writes them): only with the player's consent, either always (their
// settings.sendCrashReports) or once (a menu's "@sendcrashreport" after the game says the last run crashed), to the
// project's crashReportUrl (project.json) as a plain-text POST. The home folder and the user's name are taken out of
// the report first; once it's accepted the report is renamed <name>-crash-sent.txt (so it's sent once).
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace crashreports {

// The report as it's sent: paths under the home folder start with "~", the user's name is "<user>"
std::string prepare(const std::string& report, const std::string& home, const std::string& user);
std::string prepare(const std::string& report);   // (this machine's home and user)

class Upload {
public:
    using Post = std::function<bool(const std::string& url, const std::string& body, std::string& error)>;
    // On a worker thread; `post`: platform::httpPost by default (tests give their own)
    void start(const std::string& url, const std::filesystem::path& report, Post post = nullptr);
    bool busy() const;
    bool done() const;           // finished (sent or failed)
    bool sent() const;
    std::string error() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace crashreports
