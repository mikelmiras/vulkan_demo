// A frame profiler: named, nested CPU scopes on the main thread, and GPU pass times from the renderer, for the last
// couple of seconds of frames. The editor shows them (View > Profiler), vkgame as an overlay (F3).
//
//   void Physics::step(...) { PROFILE_SCOPE("Physics"); ... }
//
// Scopes only count between beginFrame() and endFrame() (the Application calls them), on the thread that called
// beginFrame(): anywhere else (tests, loading threads) they cost two clock reads and record nothing.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace profiler {

struct Scope {
    const char* name;   // (a string literal)
    int depth;
    double start, ms;   // ms since the frame began, and how long it took
};
struct Frame {
    std::vector<Scope> scopes;   // in the order they began
    double ms = 0.0;
};

void beginFrame();
void endFrame();
void push(const char* name);
void pop();

const std::vector<Frame>& history();   // oldest first, the last ~2 s
// Each scope's time per frame (all its calls added up), averaged over the history, and the worst frame
struct Summary {
    std::string name;
    int depth;
    double avgMs, maxMs;
};
std::vector<Summary> summarize();

// GPU pass times (ms) of the latest frame the renderer could read back
void setGpuTimes(std::vector<std::pair<std::string, float>> times);
const std::vector<std::pair<std::string, float>>& gpuTimes();

}  // namespace profiler

struct ProfileScope {
    explicit ProfileScope(const char* name) { profiler::push(name); }
    ~ProfileScope() { profiler::pop(); }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
};
#define PROFILE_CONCAT2(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT2(a, b)
#define PROFILE_SCOPE(name) ProfileScope PROFILE_CONCAT(profileScope_, __LINE__)(name)
