#include "core/log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

namespace logfile {
namespace {

struct State {
    std::mutex mutex;   // (loading threads log too)
    std::ofstream out;
    std::filesystem::path path;
};
State& state() {
    static State s;
    return s;
}

}  // namespace

bool open(const std::filesystem::path& file) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.out.close();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (std::filesystem::exists(file, ec)) {
        std::filesystem::path old = file;
        old += ".1";
        std::filesystem::rename(file, old, ec);
    }
    s.out.open(file, std::ios::out | std::ios::trunc);
    s.path = s.out ? file : std::filesystem::path();
    return (bool)s.out;
}

void close() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.out.close();
    s.path.clear();
}

bool isOpen() { return state().out.is_open(); }

const std::filesystem::path& path() { return state().path; }

void write(const std::string& line) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.out.is_open()) return;
    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&now));
    s.out << stamp << "  " << line << '\n';
    s.out.flush();   // (a crash right after shouldn't lose it)
}

}  // namespace logfile

void logLine(const char* format, ...) {
    char buf[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    std::fprintf(stderr, "%s\n", buf);
    logfile::write(buf);
}
