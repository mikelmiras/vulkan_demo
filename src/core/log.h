// The log file: what went wrong (and what happened) in a run, kept after the program is gone. vkgame writes
// game.log, the editor editor.log, in the user's data folder (next to the save games); the previous run's log is
// kept as *.1. logLine() prints to stderr too.
#pragma once

#include <filesystem>
#include <string>

namespace logfile {

bool open(const std::filesystem::path& file);   // starts a new log (the old one becomes <file>.1)
void close();
bool isOpen();
const std::filesystem::path& path();
void write(const std::string& line);   // with the time; no-op without a log open

}  // namespace logfile

// printf-style: to stderr and the log file
void logLine(const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
