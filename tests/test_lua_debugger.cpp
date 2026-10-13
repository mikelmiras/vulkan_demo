// Scripting tools: the CPU time limit, and the Lua debugger (script/lua_debugger.h) driven by a client over TCP,
// as VS Code would
#include "test.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "core/profiler.h"
#include "game/game.h"
#include "imgui.h"
#include "net/tcp.h"
#include "script/lua_debugger.h"
#include "script/lua_script.h"

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

void steps(Game& g, int n) {
    for (int i = 0; i < n; ++i) {
        ImGui::NewFrame();
        g.step();
        ImGui::EndFrame();
    }
}

// A Debug Adapter Protocol client: requests out, messages in (on its own thread)
struct Client {
    std::unique_ptr<net::TcpConnection> c;
    std::string inbox;
    int seq = 1;
    bool request(const std::string& command, json::Value args = json::Value::object()) {
        json::Value r = json::Value::object();
        r.set("seq", seq++);
        r.set("type", "request");
        r.set("command", command);
        r.set("arguments", std::move(args));
        std::string body = json::write(r), head = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        return c->send(head.data(), head.size()) && c->send(body.data(), body.size());
    }
    // The next message that matches (5 s at most)
    bool waitFor(const std::function<bool(const json::Value&)>& match, json::Value& out) {
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < until) {
            c->receive(inbox);
            size_t end = inbox.find("\r\n\r\n");
            if (end != std::string::npos) {
                size_t length = (size_t)std::strtoul(inbox.c_str() + inbox.find("Content-Length:") + 15, nullptr, 10);
                if (inbox.size() >= end + 4 + length) {
                    std::string body = inbox.substr(end + 4, length), error;
                    inbox.erase(0, end + 4 + length);
                    json::Value msg;
                    if (json::parse(body, msg, error) && match(msg)) {
                        out = msg;
                        return true;
                    }
                    continue;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    bool response(const std::string& command, json::Value& out) {
        return waitFor([&](const json::Value& m) { return m["type"].asString() == "response" && m["command"].asString() == command; }, out);
    }
    bool event(const std::string& name, json::Value& out) {
        return waitFor([&](const json::Value& m) { return m["type"].asString() == "event" && m["event"].asString() == name; }, out);
    }
};

const char* kCounter = R"lua(local Counter = { fields = {{"step", 2}} }
function Counter:start() self.count = 0 end
function Counter:update(dt)
    local before = self.count
    self.count = before + self.step
    local info = {a = 1, b = "x"}
end
return Counter
)lua";

}  // namespace

// An endless loop is stopped by the time limit (an error like any other); the game goes on
TEST(lua_time_limit) {
    test::ImGuiContextGuard imgui;
    scripting::reset();
    std::string error;
    REQUIRE(scripting::define("Spinner", "local S = {}\nfunction S:update(dt) while true do end end\nreturn S", error));
    scripting::setTimeLimit(0.1f);
    Scene s;
    s.create("Spin").behaviours.emplace_back(createBehaviour("Spinner"));
    Game g;
    g.start(s, meshes());
    auto started = std::chrono::steady_clock::now();
    steps(g, 3);
    float took = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
    CHECK(took < 1.f);   // (once: then that instance is stopped)
    bool logged = false;
    for (const auto& [t, line] : g.log()) logged = logged || line.find("ran for over 100 ms") != std::string::npos;
    CHECK(logged);
    CHECK(scripting::timeLimit() == 0.1f);
    scripting::setTimeLimit(0.5f);
    g.stop();
    scripting::reset();
}

// Attach, a breakpoint, the stack, locals and a table, an expression, a step, continue
TEST(lua_debugger_breakpoints_and_stepping) {
    test::ImGuiContextGuard imgui;
    scripting::reset();
    std::string error;
    REQUIRE(scripting::define("Counter", kCounter, error));
    REQUIRE(scripting::debugger::listen(0, error));
    uint16_t port = scripting::debugger::port();
    Scene s;
    s.create("C").behaviours.emplace_back(createBehaviour("Counter"));
    Game g;
    g.start(s, meshes());

    std::atomic<bool> done{false};
    struct Seen {
        int stoppedLine = 0, steppedLine = 0;
        std::string before, evaluated, after, tableA;
        bool ok = false;
    } seen;
    std::thread client([&] {
        Client c;
        net::Address to;
        std::string err;
        if (!net::resolve("127.0.0.1:" + std::to_string(port), port, to, err) || !(c.c = net::TcpConnection::connect(to, err))) {
            done = true;
            return;
        }
        json::Value m;
        c.request("initialize");
        if (!c.response("initialize", m) || !c.event("initialized", m)) { done = true; return; }
        c.request("attach");
        json::Value bp = json::Value::object(), source = json::Value::object(), lines = json::Value::array(), one = json::Value::object();
        source.set("path", "/somewhere/project/scripts/Counter.lua");   // (an editor's absolute path)
        one.set("line", 5);
        lines.push(one);
        bp.set("source", source);
        bp.set("breakpoints", lines);
        c.request("setBreakpoints", bp);
        c.response("setBreakpoints", m);
        c.request("configurationDone");
        if (!c.event("stopped", m)) { done = true; return; }
        auto frame0 = [&](int& line) {
            json::Value a = json::Value::object();
            a.set("threadId", 1);
            c.request("stackTrace", a);
            json::Value r;
            if (c.response("stackTrace", r)) line = r["body"]["stackFrames"][0]["line"].asInt();
        };
        frame0(seen.stoppedLine);
        json::Value a = json::Value::object();
        a.set("frameId", 1);
        c.request("scopes", a);
        c.response("scopes", m);
        int locals = m["body"]["scopes"][0]["variablesReference"].asInt();
        json::Value v = json::Value::object();
        v.set("variablesReference", locals);
        c.request("variables", v);
        c.response("variables", m);
        for (const json::Value& var : m["body"]["variables"].elements())
            if (var["name"].asString() == "before") seen.before = var["value"].asString();
        json::Value e = json::Value::object();
        e.set("expression", "self.count + self.step");
        e.set("frameId", 1);
        c.request("evaluate", e);
        c.response("evaluate", m);
        seen.evaluated = m["body"]["result"].asString();
        c.request("next", a);
        c.response("next", m);
        if (!c.event("stopped", m)) { done = true; return; }
        frame0(seen.steppedLine);
        json::Value e2 = json::Value::object();
        e2.set("expression", "self.count");
        e2.set("frameId", 1);
        c.request("evaluate", e2);
        c.response("evaluate", m);
        seen.after = m["body"]["result"].asString();
        c.request("next", a);   // (onto the table's line: then look into it)
        c.response("next", m);
        c.event("stopped", m);
        json::Value e3 = json::Value::object();
        e3.set("expression", "info");
        e3.set("frameId", 1);
        c.request("evaluate", e3);
        c.response("evaluate", m);
        json::Value tv = json::Value::object();
        tv.set("variablesReference", m["body"]["variablesReference"].asInt());
        c.request("variables", tv);
        c.response("variables", m);
        for (const json::Value& var : m["body"]["variables"].elements())
            if (var["name"].asString() == "a") seen.tableA = var["value"].asString();
        bp.set("breakpoints", json::Value::array());   // no more breakpoints; go on
        c.request("setBreakpoints", bp);
        c.response("setBreakpoints", m);
        c.request("continue", a);
        c.response("continue", m);
        c.request("disconnect");
        c.response("disconnect", m);
        seen.ok = true;
        done = true;
    });
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done && std::chrono::steady_clock::now() < until) {   // the game runs (and stops at the breakpoint)
        scripting::debugger::poll();
        steps(g, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.join();
    CHECK(seen.ok);
    CHECK(seen.stoppedLine == 5);
    // (the breakpoint arrives a few updates in: the count has grown by then)
    REQUIRE(!seen.before.empty() && !seen.evaluated.empty());
    CHECK(std::stoi(seen.evaluated) == std::stoi(seen.before) + 2);   // self.count + self.step, before line 5 ran
    CHECK(seen.steppedLine == 6);
    CHECK(seen.after == seen.evaluated);                                // ... and after it did
    CHECK(seen.tableA == "1");
    steps(g, 3);   // (detached: runs on, no more stops)
    CHECK(!scripting::debugger::attached());
    scripting::debugger::stop();
    g.stop();
    scripting::reset();
}

namespace {

// Runs the game (polling the debugger) while `script` drives a client on its own thread; the game only starts
// updating once the script says go (its breakpoints set)
void withClient(Game& g, uint16_t port, const std::function<void(Client&, std::atomic<bool>& go)>& script) {
    std::atomic<bool> done{false}, go{false};
    std::thread client([&] {
        Client c;
        net::Address to;
        std::string err;
        if (net::resolve("127.0.0.1:" + std::to_string(port), port, to, err) && (c.c = net::TcpConnection::connect(to, err))) {
            json::Value m;
            c.request("initialize");
            if (c.response("initialize", m) && c.event("initialized", m)) {
                c.request("attach");
                c.response("attach", m);
                script(c, go);
            }
        }
        done = true;
    });
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done && std::chrono::steady_clock::now() < until) {
        scripting::debugger::poll();
        if (go) steps(g, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client.join();
}

json::Value breakpointsOn(const std::string& path, std::vector<json::Value> bps) {
    json::Value args = json::Value::object(), source = json::Value::object(), list = json::Value::array();
    source.set("path", path);
    for (json::Value& b : bps) list.push(std::move(b));
    args.set("source", source);
    args.set("breakpoints", list);
    return args;
}

json::Value breakpoint(int line, const char* key = nullptr, const std::string& value = "") {
    json::Value b = json::Value::object();
    b.set("line", line);
    if (key) b.set(key, value);
    return b;
}

std::string evaluated(Client& c, const std::string& expression, int frameId = 1) {
    json::Value e = json::Value::object(), m;
    e.set("expression", expression);
    e.set("frameId", frameId);
    c.request("evaluate", e);
    return c.response("evaluate", m) ? m["body"]["result"].asString() : "";
}

}  // namespace

// A condition (stops once it's true), logpoints (print, don't stop), a hit count, a condition with an error
TEST(lua_debugger_conditions_and_logpoints) {
    test::ImGuiContextGuard imgui;
    scripting::reset();
    std::string error;
    REQUIRE(scripting::define("Counter", kCounter, error));
    REQUIRE(scripting::debugger::listen(0, error));
    Scene s;
    s.create("C").behaviours.emplace_back(createBehaviour("Counter"));
    Game g;
    g.start(s, meshes());
    struct Seen {
        std::vector<std::string> logged;
        std::string atCondition, atHits, conditionError;
        bool stoppedOnError = false, ok = false;
    } seen;
    const std::string path = "/project/scripts/Counter.lua";
    withClient(g, scripting::debugger::port(), [&](Client& c, std::atomic<bool>& go) {
        json::Value m, a = json::Value::object();
        a.set("threadId", 1);
        // (the logpoint on line 6, after count grew; the condition on line 5, before)
        c.request("setBreakpoints", breakpointsOn(path, {breakpoint(5, "condition", "self.count >= 6"),
                                                        breakpoint(6, "logMessage", "count is {self.count}, {{braces}}")}));
        c.response("setBreakpoints", m);
        c.request("configurationDone");
        c.response("configurationDone", m);
        go = true;
        bool stopped = false;
        while (!stopped && c.waitFor([](const json::Value& x) {
                   return x["type"].asString() == "event" && (x["event"].asString() == "output" || x["event"].asString() == "stopped");
               }, m)) {
            if (m["event"].asString() == "stopped") stopped = true;
            else seen.logged.push_back(m["body"]["output"].asString());
        }
        if (!stopped) return;
        seen.atCondition = evaluated(c, "self.count");
        // Every second hit of line 5 from now on
        c.request("setBreakpoints", breakpointsOn(path, {breakpoint(5, "hitCondition", "% 2")}));
        c.response("setBreakpoints", m);
        c.request("continue", a);
        c.response("continue", m);
        if (!c.event("stopped", m)) return;
        seen.atHits = evaluated(c, "self.count");
        // A condition that can't be worked out: it stops, saying why
        c.request("setBreakpoints", breakpointsOn(path, {breakpoint(5, "condition", "self.nothing.here")}));
        c.response("setBreakpoints", m);
        c.request("continue", a);
        c.response("continue", m);
        if (!c.waitFor([](const json::Value& x) { return x["type"].asString() == "event" && x["event"].asString() == "output"; }, m)) return;
        seen.conditionError = m["body"]["output"].asString();
        seen.stoppedOnError = c.event("stopped", m);
        c.request("setBreakpoints", breakpointsOn(path, {}));
        c.response("setBreakpoints", m);
        c.request("continue", a);
        c.response("continue", m);
        c.request("disconnect");
        c.response("disconnect", m);
        seen.ok = true;
    });
    CHECK(seen.ok);
    // count goes 0, 2, 4, 6 ... (2 a step: a number field, so a float) logged after each step until it was 6 before
    // line 5
    REQUIRE(seen.logged.size() == 3);
    CHECK(seen.logged[0] == "count is 2.0, {braces}\n");
    CHECK(seen.logged[2] == "count is 6.0, {braces}\n");
    CHECK(seen.atCondition == "6.0");
    CHECK(seen.atHits == "10.0");   // (the hits from 8 on: 8 is the first, 10 the second)
    CHECK(seen.conditionError.find("Breakpoint condition (self.nothing.here)") != std::string::npos);
    CHECK(seen.stoppedOnError);
    scripting::debugger::stop();
    g.stop();
    scripting::reset();
}

// With the "Script errors" filter on, the game stops where a script raises an error: its frame, its locals, the
// message; then the error is reported as usual
TEST(lua_debugger_breaks_on_errors) {
    test::ImGuiContextGuard imgui;
    scripting::reset();
    std::string error;
    REQUIRE(scripting::define("Faulty", R"lua(local Faulty = {}
local function check(n)
    local limit = 2
    if n > limit then error("too many: " .. n) end
end
function Faulty:start() self.n = 0 end
function Faulty:update(dt)
    self.n = self.n + 1
    check(self.n)
end
return Faulty
)lua", error));
    REQUIRE(scripting::debugger::listen(0, error));
    Scene s;
    s.create("F").behaviours.emplace_back(createBehaviour("Faulty"));
    Game g;
    g.start(s, meshes());
    struct Seen {
        std::string reason, text, function, limit, n, description;
        int line = 0;
        bool ok = false;
    } seen;
    withClient(g, scripting::debugger::port(), [&](Client& c, std::atomic<bool>& go) {
        json::Value m, a = json::Value::object(), filters = json::Value::object(), list = json::Value::array();
        a.set("threadId", 1);
        list.push(json::Value("errors"));
        filters.set("filters", list);
        c.request("setExceptionBreakpoints", filters);
        c.response("setExceptionBreakpoints", m);
        c.request("configurationDone");
        c.response("configurationDone", m);
        go = true;
        if (!c.event("stopped", m)) return;
        seen.reason = m["body"]["reason"].asString();
        seen.text = m["body"]["text"].asString();
        c.request("stackTrace", a);
        if (c.response("stackTrace", m)) {
            const json::Value& top = m["body"]["stackFrames"][0];
            seen.line = top["line"].asInt();
            seen.function = top["name"].asString();
            int id = top["id"].asInt();
            seen.limit = evaluated(c, "limit", id);
            seen.n = evaluated(c, "n", id);
        }
        c.request("exceptionInfo", a);
        if (c.response("exceptionInfo", m)) seen.description = m["body"]["description"].asString();
        c.request("continue", a);
        c.response("continue", m);
        c.request("disconnect");
        c.response("disconnect", m);
        seen.ok = true;
    });
    CHECK(seen.ok);
    CHECK(seen.reason == "exception");
    CHECK(seen.text.find("too many: 3") != std::string::npos);
    CHECK(seen.line == 4);           // (in check(), where error() was called: not inside error itself)
    CHECK(seen.function == "check");
    CHECK(seen.limit == "2" && seen.n == "3");
    CHECK(seen.description.find("too many: 3") != std::string::npos);
    bool reported = false;   // (and then reported, as without the debugger)
    for (const auto& [t, line] : g.log()) reported = reported || line.find("too many: 3") != std::string::npos;
    CHECK(reported);
    scripting::debugger::stop();
    g.stop();
    scripting::reset();
}

// Profiling: each callback is a profiler scope (named after the script and callback, every instance's call), and
// sampling finds the function the work is in
TEST(lua_profiling) {
    test::ImGuiContextGuard imgui;
    scripting::reset();
    std::string error;
    REQUIRE(scripting::define("Busy", R"lua(local Busy = {}
local function heavy(n)
    local s = 0
    for i = 1, n do s = s + i % 7 end
    return s
end
function Busy:update(dt) self.total = heavy(20000) end
return Busy
)lua", error));
    Scene s;
    s.create("B1").behaviours.emplace_back(createBehaviour("Busy"));
    s.create("B2").behaviours.emplace_back(createBehaviour("Busy"));
    Game g;
    g.start(s, meshes());
    scripting::setSampling(true);
    CHECK(scripting::sampling());
    for (int i = 0; i < 5; ++i) {
        profiler::beginFrame();
        steps(g, 1);
        profiler::endFrame();
    }
    REQUIRE(!profiler::history().empty());
    int calls = 0;
    for (const profiler::Scope& sc : profiler::history().back().scopes) calls += std::string(sc.name) == "Busy.update";
    CHECK(calls == 2);   // (both instances, one scope each)
    bool summarized = false;
    for (const profiler::Summary& sm : profiler::summarize()) summarized = summarized || (sm.name == "Busy.update" && sm.avgMs > 0.0);
    CHECK(summarized);
    std::vector<scripting::Sample> samples = scripting::samples();
    REQUIRE(!samples.empty());
    CHECK(samples[0].function.find("heavy") != std::string::npos);   // (where the loop is)
    CHECK(samples[0].count > 100);
    scripting::clearSamples();
    CHECK(scripting::samples().empty());
    scripting::setSampling(false);
    g.stop();
    scripting::reset();
}
