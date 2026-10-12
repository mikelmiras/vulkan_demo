// The engine's version (CMake's project version) and the git commit it was built from, and comparing versions
// (update checks: a project's own version, in project.json, against the one an update URL offers).
#pragma once

#include <string>

namespace version {

const char* string();   // "0.5.0"
const char* commit();   // "e7b72ac1d2" (git describe; "-dirty" with uncommitted changes), "" outside git
std::string full();     // "0.5.0 (e7b72ac1d2)"

// Versions compared part by part as numbers ("1.10" > "1.9.2"; missing parts are 0, a leading "v" is ignored), and
// a pre-release ("1.2.0-beta") before its release: < 0, 0, > 0
int compare(const std::string& a, const std::string& b);

}  // namespace version
