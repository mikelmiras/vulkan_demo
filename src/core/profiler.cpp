#include "core/profiler.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <thread>

namespace profiler {
namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kHistory = 120;

struct State {
    std::thread::id thread;
    bool inFrame = false;
    Clock::time_point frameStart;
    Frame current;
    std::vector<size_t> open;   // indices into current.scopes
    std::vector<Frame> history;
    std::vector<std::pair<std::string, float>> gpu;
};

State& state() {
    static State s;
    return s;
}

double since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

}  // namespace

void beginFrame() {
    State& s = state();
    s.thread = std::this_thread::get_id();
    s.inFrame = true;
    s.frameStart = Clock::now();
    s.current = {};
    s.open.clear();
}

void endFrame() {
    State& s = state();
    if (!s.inFrame) return;
    while (!s.open.empty()) pop();   // (unbalanced: close what's left)
    s.current.ms = since(s.frameStart);
    s.history.push_back(std::move(s.current));
    if (s.history.size() > kHistory) s.history.erase(s.history.begin());
    s.inFrame = false;
}

void push(const char* name) {
    State& s = state();
    if (!s.inFrame || std::this_thread::get_id() != s.thread) return;
    s.open.push_back(s.current.scopes.size());
    s.current.scopes.push_back({name, (int)s.open.size() - 1, since(s.frameStart), 0.0});
}

void pop() {
    State& s = state();
    if (!s.inFrame || std::this_thread::get_id() != s.thread || s.open.empty()) return;
    Scope& sc = s.current.scopes[s.open.back()];
    sc.ms = since(s.frameStart) - sc.start;
    s.open.pop_back();
}

const std::vector<Frame>& history() { return state().history; }

std::vector<Summary> summarize() {
    // Scopes are told apart by where they ran (their parents' names): "Editor/Fixed update/Physics"
    const std::vector<Frame>& frames = history();
    struct Node {
        Summary s;
        std::string parent;
        std::vector<std::string> children;   // first seen first
    };
    std::map<std::string, Node> nodes;
    std::vector<std::string> roots;
    for (const Frame& f : frames) {
        std::map<std::string, double> thisFrame;
        std::vector<std::string> stack;   // paths of the open scopes, by depth
        for (const Scope& sc : f.scopes) {
            stack.resize((size_t)sc.depth);
            std::string parent = stack.empty() ? "" : stack.back();
            std::string path = parent.empty() ? sc.name : parent + "/" + sc.name;
            stack.push_back(path);
            thisFrame[path] += sc.ms;
            if (!nodes.count(path)) {
                nodes[path] = {{sc.name, sc.depth, 0.0, 0.0}, parent, {}};
                if (parent.empty()) roots.push_back(path);
                else if (nodes.count(parent)) nodes[parent].children.push_back(path);
            }
        }
        for (const auto& [path, ms] : thisFrame) {
            Summary& s = nodes[path].s;
            s.avgMs += ms;
            s.maxMs = std::max(s.maxMs, ms);
        }
    }
    std::vector<Summary> out;   // depth first: each scope followed by the ones inside it
    std::vector<std::string> todo(roots.rbegin(), roots.rend());
    while (!todo.empty()) {
        std::string path = todo.back();
        todo.pop_back();
        Node& n = nodes[path];
        n.s.avgMs /= (double)std::max<size_t>(frames.size(), 1);
        out.push_back(n.s);
        for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) todo.push_back(*it);
    }
    return out;
}

void setGpuTimes(std::vector<std::pair<std::string, float>> times) { state().gpu = std::move(times); }
const std::vector<std::pair<std::string, float>>& gpuTimes() { return state().gpu; }

}  // namespace profiler
