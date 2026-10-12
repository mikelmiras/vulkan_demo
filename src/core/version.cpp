#include "core/version.h"

#include <cctype>
#include <vector>

namespace version::build {
extern const char* const kVersion;   // (generated: cmake/build_info.cmake)
extern const char* const kCommit;
}  // namespace version::build

namespace version {

const char* string() { return build::kVersion; }
const char* commit() { return build::kCommit; }

std::string full() {
    std::string c = commit();
    return c.empty() ? std::string(string()) : std::string(string()) + " (" + c + ")";
}

namespace {

// "v1.2.3-beta" -> {1, 2, 3}, "beta"
void split(const std::string& v, std::vector<long>& parts, std::string& pre) {
    size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    while (i < v.size()) {
        long n = 0;
        bool digits = false;
        while (i < v.size() && std::isdigit((unsigned char)v[i])) n = n * 10 + (v[i++] - '0'), digits = true;
        if (!digits) break;
        parts.push_back(n);
        if (i < v.size() && v[i] == '.') ++i;
        else break;
    }
    if (i < v.size() && (v[i] == '-' || v[i] == '+')) pre = v[i] == '-' ? v.substr(i + 1) : "";
}

}  // namespace

int compare(const std::string& a, const std::string& b) {
    std::vector<long> pa, pb;
    std::string prea, preb;
    split(a, pa, prea);
    split(b, pb, preb);
    for (size_t i = 0; i < pa.size() || i < pb.size(); ++i) {
        long x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (prea.empty() != preb.empty()) return prea.empty() ? 1 : -1;   // (1.0.0-beta < 1.0.0)
    return prea < preb ? -1 : prea > preb ? 1 : 0;
}

}  // namespace version
