#include "core/profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
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
    Clock::time_point epoch = Clock::now();
    bool recording = false;
    std::vector<Frame> recorded;
};
constexpr size_t kMaxRecorded = 30000;

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
    s.current.begin = std::chrono::duration<double, std::milli>(s.frameStart - s.epoch).count();
    s.open.clear();
}

void endFrame() {
    State& s = state();
    if (!s.inFrame) return;
    while (!s.open.empty()) pop();   // (unbalanced: close what's left)
    s.current.ms = since(s.frameStart);
    s.current.gpu = s.gpu;
    if (s.recording && s.recorded.size() < kMaxRecorded) s.recorded.push_back(s.current);
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

void startRecording() {
    state().recorded.clear();
    state().recording = true;
}
void stopRecording() { state().recording = false; }
bool recording() { return state().recording; }
const std::vector<Frame>& recorded() { return state().recorded; }

std::string chromeTraceJson(const std::vector<Frame>& frames) {
    // Complete events ("ph": "X") in microseconds; Chrome nests them by time on a track
    std::string out = "{\"displayTimeUnit\": \"ms\", \"traceEvents\": [\n";
    out += "{\"name\": \"thread_name\", \"ph\": \"M\", \"pid\": 1, \"tid\": 1, \"args\": {\"name\": \"CPU (main thread)\"}},\n";
    out += "{\"name\": \"thread_name\", \"ph\": \"M\", \"pid\": 1, \"tid\": 2, \"args\": {\"name\": \"GPU passes\"}}";
    char buf[512];
    auto escape = [](const std::string& s) {
        std::string e;
        for (char c : s) {
            if (c == '"' || c == '\\') e += '\\';
            if ((unsigned char)c >= 0x20) e += c;
        }
        return e;
    };
    for (size_t i = 0; i < frames.size(); ++i) {
        const Frame& f = frames[i];
        std::snprintf(buf, sizeof(buf), ",\n{\"name\": \"Frame\", \"cat\": \"frame\", \"ph\": \"X\", \"pid\": 1, \"tid\": 1, "
                      "\"ts\": %.3f, \"dur\": %.3f}", f.begin * 1000.0, f.ms * 1000.0);
        out += buf;
        for (const Scope& sc : f.scopes) {
            std::snprintf(buf, sizeof(buf), ",\n{\"name\": \"%s\", \"cat\": \"cpu\", \"ph\": \"X\", \"pid\": 1, \"tid\": 1, "
                          "\"ts\": %.3f, \"dur\": %.3f}", escape(sc.name).c_str(), (f.begin + sc.start) * 1000.0, sc.ms * 1000.0);
            out += buf;
        }
        double t = f.begin;   // (GPU passes one after another from the frame's start: their real start isn't known)
        for (const auto& [name, ms] : f.gpu) {
            std::snprintf(buf, sizeof(buf), ",\n{\"name\": \"%s\", \"cat\": \"gpu\", \"ph\": \"X\", \"pid\": 1, \"tid\": 2, "
                          "\"ts\": %.3f, \"dur\": %.3f}", escape(name).c_str(), t * 1000.0, (double)ms * 1000.0);
            out += buf;
            t += ms;
        }
    }
    out += "\n]}\n";
    return out;
}

bool saveChromeTrace(const std::filesystem::path& file, const std::vector<Frame>& frames, std::string& error) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot write " + file.u8string();
        return false;
    }
    out << chromeTraceJson(frames);
    if (!out) {
        error = "cannot write " + file.u8string();
        return false;
    }
    return true;
}

void setGpuTimes(std::vector<std::pair<std::string, float>> times) { state().gpu = std::move(times); }
const std::vector<std::pair<std::string, float>>& gpuTimes() { return state().gpu; }

}  // namespace profiler
