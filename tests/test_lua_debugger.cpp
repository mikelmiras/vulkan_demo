// Scripting tools: the CPU time limit, and the Lua debugger (script/lua_debugger.h) driven by a client over TCP,
// as VS Code would
#include "test.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

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
