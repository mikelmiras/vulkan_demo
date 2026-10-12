// Crash reports: once install()ed, a crash (a bad memory access, abort(), an illegal instruction, a division by
// zero, an exception nothing caught) writes what happened and the stack to stderr, the log file and
// <folder>/<name>-crash.txt before the program goes. The next run's install() moves that report to
// <name>-crash-last.txt and previousReport() names it, so the editor or game can say the last run crashed.
//
//   logfile::open(...);                    // (first: the report goes into the log too)
//   crash::install(dataFolder, "editor");
#pragma once

#include <filesystem>
#include <string>

namespace crash {

void install(const std::filesystem::path& folder, const std::string& name);
// What's running, for the report's "Product:" line (a game's name and version: "Coin Hunt 1.2.0")
void setProduct(const std::string& product);
const std::filesystem::path& previousReport();   // empty: the last run didn't crash (or nothing was installed)
const std::filesystem::path& reportPath();       // where a crash now would write

// The calling thread's stack, one frame per line ("#3 libengine.so(_ZN4Game5frameEfbb+0x1c4) [0x7f...]"), from
// whoever called this (skip: callers above it to leave out)
std::string stackTrace(int skip = 0);
// Writes a report now, as a crash would (what: the reason). Tests and fatal errors use it; the program goes on.
void writeReport(const char* what);

}  // namespace crash
