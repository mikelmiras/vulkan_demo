#include "script/lua_debugger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "core/json.h"
#include "core/log.h"
#include "core/project.h"
#include "net/tcp.h"
#include "script/lua_script.h"

#include "lauxlib.h"   // (Lua is compiled as C++ here)
#include "lua.h"

namespace scripting {
void refreshHook();   // (lua_script.cpp: turns the line hook on or off as wantsLines() says)
lua_State* luaState();
}

namespace scripting::debugger {

namespace {

enum class Step { None, In, Over, Out };

struct VarRef {
    enum Kind { Locals, Upvalues, Table } kind = Locals;
    int level = 0;         // (locals, upvalues) the stack level
    int table = LUA_NOREF; // (tables) a registry reference
};

// A breakpoint: plain, or with a condition (stops when it's true), a hit condition ("5", ">= 5", "% 3": which hits
// stop), or a log message (a logpoint: prints "{expressions}" filled in, and goes on)
struct Breakpoint {
    std::string condition, hitCondition, logMessage;
    int hits = 0;
};

struct Debugger {
    net::TcpListener listener;
    std::unique_ptr<net::TcpConnection> client;
    std::string inbox;
    int seq = 1;
    std::map<std::string, std::map<int, Breakpoint>> breakpoints;   // source key ("scripts/Door.lua") -> lines
    std::set<int> anyLines;                             // (quick test: any file has a breakpoint on this line)
    Step step = Step::None;
    int stepDepth = 0;
    bool pauseRequested = false;
    bool stopped = false, resume = false, inside = false;
    bool breakOnErrors = false;                         // (the "Script errors" exception filter)
    lua_State* L = nullptr;                             // (while stopped)
    int base = 0;                                       // (while stopped) the first stack level shown
    std::string exception;                              // (stopped on an error) its message
    std::vector<VarRef> refs;
};

Debugger& dbg() {
    static Debugger d;
    return d;
}

// ---- Messages: "Content-Length: n\r\n\r\n" then n bytes of JSON
void sendMessage(json::Value msg) {
    Debugger& d = dbg();
    if (!d.client) return;
    msg.set("seq", d.seq++);
    std::string body = json::write(msg);
    std::string head = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    if (!d.client->send(head.data(), head.size()) || !d.client->send(body.data(), body.size())) d.client.reset();
}

void respond(const json::Value& request, json::Value body = json::Value::object(), bool success = true,
             const std::string& message = "") {
    json::Value r = json::Value::object();
    r.set("type", "response");
    r.set("request_seq", request["seq"].asInt());
    r.set("success", success);
    r.set("command", request["command"].asString());
    if (!message.empty()) r.set("message", message);
    r.set("body", std::move(body));
    sendMessage(std::move(r));
}

void event(const std::string& name, json::Value body = json::Value::object()) {
    json::Value e = json::Value::object();
    e.set("type", "event");
    e.set("event", name);
    e.set("body", std::move(body));
    sendMessage(std::move(e));
}

bool nextMessage(json::Value& out) {
    Debugger& d = dbg();
    size_t end = d.inbox.find("\r\n\r\n");
    if (end == std::string::npos) return false;
    size_t at = d.inbox.find("Content-Length:");
    if (at == std::string::npos || at > end) {
        d.inbox.erase(0, end + 4);
        return false;
    }
    size_t length = (size_t)std::strtoul(d.inbox.c_str() + at + 15, nullptr, 10);
    if (d.inbox.size() < end + 4 + length) return false;
    std::string body = d.inbox.substr(end + 4, length), error;
    d.inbox.erase(0, end + 4 + length);
    return json::parse(body, out, error);
}

// ---- Sources: a script's key is its chunk name ("scripts/Door.lua"); editors send absolute paths
std::string keyOf(const std::string& path) {
    std::filesystem::path p = std::filesystem::u8path(path);
    std::error_code ec;
    std::filesystem::path root = project::root();
    if (!root.empty() && p.is_absolute()) {
        std::filesystem::path rel = std::filesystem::proximate(p, root, ec);
        if (!ec && !rel.empty() && rel.generic_u8string().rfind("..", 0) != 0) return rel.generic_u8string();
    }
    return p.generic_u8string();
}

bool sameSource(const std::string& key, const std::string& chunk) {
    if (key == chunk) return true;   // (otherwise: one is the end of the other, at a folder boundary)
    const std::string& a = key.size() > chunk.size() ? key : chunk;
    const std::string& b = key.size() > chunk.size() ? chunk : key;
    return a.size() > b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0 && a[a.size() - b.size() - 1] == '/';
}

std::string chunkOf(const char* source) {   // "@scripts/Door.lua" -> "scripts/Door.lua"
    return source && source[0] == '@' ? std::string(source + 1) : std::string(source ? source : "?");
}

json::Value sourceJson(const std::string& chunk) {
    json::Value s = json::Value::object();
    s.set("name", std::filesystem::u8path(chunk).filename().u8string());
    std::filesystem::path root = project::root();
    s.set("path", root.empty() ? chunk : (root / std::filesystem::u8path(chunk)).u8string());
    return s;
}

int depthOf(lua_State* L) {
    lua_Debug ar;
    int n = 0;
    while (lua_getstack(L, n, &ar)) ++n;
    return n;
}

// ---- Values: how one reads in the Variables view; tables get a reference to expand them
json::Value describe(lua_State* L, int index, const std::string& name) {
    Debugger& d = dbg();
    index = lua_absindex(L, index);
    json::Value v = json::Value::object();
    v.set("name", name);
    int type = lua_type(L, index);
    std::string text;
    if (type == LUA_TSTRING) {
        text = "\"" + std::string(lua_tostring(L, index)) + "\"";
    } else {
        size_t n = 0;
        const char* s = luaL_tolstring(L, index, &n);   // (pushes it)
        text.assign(s, n);
        lua_pop(L, 1);
    }
    v.set("value", text);
    v.set("type", lua_typename(L, type));
    int ref = 0;
    if (type == LUA_TTABLE) {
        lua_pushvalue(L, index);
        VarRef r;
        r.kind = VarRef::Table;
        r.table = luaL_ref(L, LUA_REGISTRYINDEX);
        d.refs.push_back(r);
        ref = (int)d.refs.size();
    }
    v.set("variablesReference", ref);
    return v;
}

void dropRefs() {
    Debugger& d = dbg();
    for (const VarRef& r : d.refs)
        if (r.kind == VarRef::Table && d.L) luaL_unref(d.L, LUA_REGISTRYINDEX, r.table);
    d.refs.clear();
}

json::Value variables(lua_State* L, int reference) {
    Debugger& d = dbg();
    json::Value list = json::Value::array();
    if (reference <= 0 || reference > (int)d.refs.size()) return list;
    VarRef r = d.refs[(size_t)reference - 1];
    if (r.kind == VarRef::Table) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, r.table);
        int t = lua_gettop(L);
        lua_pushnil(L);
        int count = 0;
        while (lua_next(L, t) != 0 && count++ < 500) {
            std::string key;
            if (lua_type(L, -2) == LUA_TSTRING) key = lua_tostring(L, -2);
            else { key = "[" + std::string(luaL_tolstring(L, -2, nullptr)) + "]"; lua_pop(L, 1); }
            list.push(describe(L, -1, key));
            lua_pop(L, 1);
        }
        lua_settop(L, t - 1);
        return list;
    }
    lua_Debug ar;
    if (!lua_getstack(L, r.level, &ar)) return list;
    if (r.kind == VarRef::Locals) {
        for (int i = 1;; ++i) {
            const char* name = lua_getlocal(L, &ar, i);
            if (!name) break;
            if (name[0] != '(') list.push(describe(L, -1, name));   // ("(temporary)" ones: not the script's)
            lua_pop(L, 1);
        }
    } else {
        lua_getinfo(L, "f", &ar);
        int f = lua_gettop(L);
        for (int i = 1;; ++i) {
            const char* name = lua_getupvalue(L, f, i);
            if (!name) break;
            if (std::string(name) != "_ENV") list.push(describe(L, -1, name));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    return list;
}

// An expression in a frame, its value pushed: its locals and upvalues first, then the globals (false: an error)
bool pushExpression(lua_State* L, const std::string& expression, int level, std::string& error) {
    int top = lua_gettop(L);
    lua_newtable(L);   // the environment
    int env = lua_gettop(L);
    lua_Debug ar;
    if (level >= 0 && lua_getstack(L, level, &ar)) {
        lua_getinfo(L, "f", &ar);
        int f = lua_gettop(L);
        for (int i = 1;; ++i) {
            const char* name = lua_getupvalue(L, f, i);
            if (!name) break;
            if (std::string(name) != "_ENV") lua_setfield(L, env, name);
            else lua_pop(L, 1);
        }
        lua_pop(L, 1);
        for (int i = 1;; ++i) {   // (locals after upvalues: they're nearer)
            const char* name = lua_getlocal(L, &ar, i);
            if (!name) break;
            if (name[0] != '(') lua_setfield(L, env, name);
            else lua_pop(L, 1);
        }
    }
    lua_newtable(L);   // its metatable: anything else from the globals
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, env);
    std::string code = "return " + expression;
    if (luaL_loadbufferx(L, code.data(), code.size(), "=watch", "t") != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbufferx(L, expression.data(), expression.size(), "=watch", "t") != LUA_OK) {
            error = lua_tostring(L, -1);
            lua_settop(L, top);
            return false;
        }
    }
    lua_pushvalue(L, env);
    lua_setupvalue(L, -2, 1);   // (its _ENV)
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        lua_settop(L, top);
        return false;
    }
    lua_replace(L, top + 1);   // (just the value left)
    lua_settop(L, top + 1);
    return true;
}

json::Value evaluate(lua_State* L, const std::string& expression, int level) {
    json::Value body = json::Value::object();
    std::string error;
    if (!pushExpression(L, expression, level, error)) {
        body.set("result", error);
        body.set("variablesReference", 0);
        return body;
    }
    json::Value v = describe(L, -1, expression);
    lua_pop(L, 1);
    body.set("result", v["value"]);
    body.set("type", v["type"]);
    body.set("variablesReference", v["variablesReference"]);
    return body;
}

// A logpoint's message: each {expression} replaced by its value as text ("{{" and "}}": braces)
std::string interpolate(lua_State* L, const std::string& message, int level) {
    std::string out;
    for (size_t i = 0; i < message.size(); ++i) {
        char c = message[i];
        if ((c == '{' || c == '}') && i + 1 < message.size() && message[i + 1] == c) {
            out += c;
            ++i;
        } else if (c == '{') {
            size_t end = message.find('}', i);
            if (end == std::string::npos) { out += message.substr(i); break; }
            std::string error;
            if (pushExpression(L, message.substr(i + 1, end - i - 1), level, error)) {
                size_t n = 0;
                const char* t = luaL_tolstring(L, -1, &n);
                out.append(t, n);
                lua_pop(L, 2);
            } else {
                out += "<" + error + ">";
            }
            i = end;
        } else {
            out += c;
        }
    }
    return out;
}

// Whether the hit count passes a hit condition: "5" or ">= 5" (from the fifth on), "== 5", "> 5", "< 5", "<= 5",
// "% 3" (every third)
bool hitMatches(const std::string& condition, int hits) {
    std::string c = condition;
    c.erase(std::remove(c.begin(), c.end(), ' '), c.end());
    std::string op;
    while (!c.empty() && std::string("<>=%!").find(c[0]) != std::string::npos) op += c[0], c.erase(0, 1);
    char* end = nullptr;
    long n = std::strtol(c.c_str(), &end, 10);
    if (c.empty() || (end && *end)) return true;   // (not understood: every hit)
    if (op.empty() || op == ">=") return hits >= n;
    if (op == "==" || op == "=") return hits == n;
    if (op == ">") return hits > n;
    if (op == "<") return hits < n;
    if (op == "<=") return hits <= n;
    if (op == "!=") return hits != n;
    if (op == "%") return n > 0 && hits % n == 0;
    return true;
}

void output(const std::string& text, const std::string& category = "console") {
    json::Value body = json::Value::object();
    body.set("category", category);
    body.set("output", text + "\n");
    event("output", body);
}

// ---- Requests (while running, or stopped: then L is the paused state)
void handle(const json::Value& req) {
    Debugger& d = dbg();
    const std::string& cmd = req["command"].asString();
    const json::Value& args = req["arguments"];
    if (cmd == "initialize") {
        json::Value caps = json::Value::object();
        caps.set("supportsConfigurationDoneRequest", true);
        caps.set("supportsEvaluateForHovers", true);
        caps.set("supportsConditionalBreakpoints", true);
        caps.set("supportsHitConditionalBreakpoints", true);
        caps.set("supportsLogPoints", true);
        caps.set("supportsExceptionInfoRequest", true);
        json::Value filters = json::Value::array(), errors = json::Value::object();
        errors.set("filter", "errors");
        errors.set("label", "Script errors");
        errors.set("description", "Stop where a script raises an error, before it's reported");
        errors.set("default", true);
        filters.push(errors);
        caps.set("exceptionBreakpointFilters", filters);
        respond(req, caps);
        event("initialized");
    } else if (cmd == "setExceptionBreakpoints") {
        d.breakOnErrors = false;
        for (const json::Value& f : args["filters"].elements()) d.breakOnErrors = d.breakOnErrors || f.asString() == "errors";
        respond(req);
    } else if (cmd == "attach" || cmd == "launch" || cmd == "configurationDone") {
        respond(req);
    } else if (cmd == "setBreakpoints") {
        std::string key = keyOf(args["source"]["path"].asString());
        std::map<int, Breakpoint>& lines = d.breakpoints[key];
        lines.clear();
        json::Value list = json::Value::array();
        for (const json::Value& b : args["breakpoints"].elements()) {
            int line = b["line"].asInt();
            Breakpoint& bp = lines[line];
            bp.condition = b["condition"].asString();
            bp.hitCondition = b["hitCondition"].asString();
            bp.logMessage = b["logMessage"].asString();
            json::Value v = json::Value::object();
            v.set("verified", true);
            v.set("line", line);
            list.push(std::move(v));
        }
        if (lines.empty()) d.breakpoints.erase(key);
        d.anyLines.clear();
        for (const auto& [k, ls] : d.breakpoints)
            for (const auto& [line, bp] : ls) d.anyLines.insert(line);
        json::Value body = json::Value::object();
        body.set("breakpoints", list);
        respond(req, body);
        refreshHook();
    } else if (cmd == "threads") {
        json::Value t = json::Value::object();
        t.set("id", 1);
        t.set("name", "Lua (main)");
        json::Value list = json::Value::array();
        list.push(t);
        json::Value body = json::Value::object();
        body.set("threads", list);
        respond(req, body);
    } else if (cmd == "pause") {
        d.pauseRequested = true;
        respond(req);
        refreshHook();
    } else if (cmd == "disconnect") {
        d.breakpoints.clear();
        d.anyLines.clear();
        d.step = Step::None;
        d.pauseRequested = false;
        d.resume = true;
        respond(req);
        d.client.reset();
        refreshHook();
    } else if (!d.stopped) {
        respond(req, json::Value::object(), false, "the game is running (pause it first)");
    } else if (cmd == "stackTrace") {
        json::Value frames = json::Value::array();
        lua_Debug ar;
        for (int level = d.base; lua_getstack(d.L, level, &ar) && level < 64; ++level) {
            lua_getinfo(d.L, "Sln", &ar);
            json::Value f = json::Value::object();
            f.set("id", level + 1);
            f.set("name", std::string(ar.name ? ar.name : ar.what && std::string(ar.what) == "main" ? "(script)" : "?"));
            f.set("line", ar.currentline > 0 ? ar.currentline : 0);
            f.set("column", 1);
            if (ar.source && ar.source[0] == '@') f.set("source", sourceJson(chunkOf(ar.source)));
            frames.push(std::move(f));
        }
        json::Value body = json::Value::object();
        body.set("totalFrames", (int)frames.size());
        body.set("stackFrames", frames);
        respond(req, body);
    } else if (cmd == "scopes") {
        int level = args["frameId"].asInt(1) - 1;
        json::Value list = json::Value::array();
        for (VarRef::Kind kind : {VarRef::Locals, VarRef::Upvalues}) {
            VarRef r;
            r.kind = kind;
            r.level = level;
            d.refs.push_back(r);
            json::Value s = json::Value::object();
            s.set("name", kind == VarRef::Locals ? "Locals" : "Upvalues");
            s.set("variablesReference", (int)d.refs.size());
            s.set("expensive", false);
            list.push(std::move(s));
        }
        json::Value body = json::Value::object();
        body.set("scopes", list);
        respond(req, body);
    } else if (cmd == "variables") {
        json::Value body = json::Value::object();
        body.set("variables", variables(d.L, args["variablesReference"].asInt()));
        respond(req, body);
    } else if (cmd == "exceptionInfo") {
        json::Value body = json::Value::object(), details = json::Value::object();
        body.set("exceptionId", "error");
        body.set("description", d.exception.substr(0, d.exception.find('\n')));
        body.set("breakMode", "always");
        details.set("message", d.exception.substr(0, d.exception.find('\n')));
        details.set("stackTrace", d.exception);
        body.set("details", details);
        respond(req, body);
    } else if (cmd == "evaluate") {
        respond(req, evaluate(d.L, args["expression"].asString(), args.has("frameId") ? args["frameId"].asInt() - 1 : d.base));
    } else if (cmd == "continue" || cmd == "next" || cmd == "stepIn" || cmd == "stepOut") {
        d.step = cmd == "next" ? Step::Over : cmd == "stepIn" ? Step::In : cmd == "stepOut" ? Step::Out : Step::None;
        d.stepDepth = depthOf(d.L);
        d.resume = true;
        if (cmd == "continue") {
            json::Value body = json::Value::object();
            body.set("allThreadsContinued", true);
            respond(req, body);
        } else {
            respond(req);
        }
    } else {
        respond(req, json::Value::object(), false, "not supported: " + cmd);
    }
}

void serve() {
    Debugger& d = dbg();
    if (!d.client) return;
    if (!d.client->receive(d.inbox)) {   // gone: the game goes on as if it had disconnected
        d.client.reset();
        d.breakpoints.clear();
        d.anyLines.clear();
        d.step = Step::None;
        d.resume = true;
        refreshHook();
        return;
    }
    json::Value msg;
    while (d.client && nextMessage(msg))
        if (msg["type"].asString() == "request") handle(msg);
}

}  // namespace

bool listen(uint16_t port, std::string& error) {
    Debugger& d = dbg();
    stop();
    if (!d.listener.listen(port, error)) return false;
    logLine("Lua debugger: listening on port %u", d.listener.port());
    return true;
}

void stop() {
    Debugger& d = dbg();
    if (d.client) event("terminated");
    d.client.reset();
    d.listener.close();
    d.breakpoints.clear();
    d.anyLines.clear();
    d.step = Step::None;
    d.pauseRequested = false;
    refreshHook();
}

bool listening() { return dbg().listener.isOpen(); }
uint16_t port() { return dbg().listener.port(); }
bool attached() { return dbg().client != nullptr; }

std::string status() {
    if (!listening()) return "Lua debugger off";
    return "Lua debugger on port " + std::to_string(port()) + (attached() ? ": attached" : ": waiting for an editor");
}

void poll() {
    Debugger& d = dbg();
    if (!d.listener.isOpen()) return;
    if (!d.client)
        if ((d.client = d.listener.accept())) {
            d.inbox.clear();
            d.seq = 1;
            logLine("Lua debugger: an editor attached");
        }
    serve();
}

namespace {

// Stopped: the game waits here, answering the editor, until it says go on; returns the seconds it waited
double stopHere(lua_State* L, const char* reason, int base, const std::string& exception) {
    Debugger& d = dbg();
    auto started = std::chrono::steady_clock::now();
    d.inside = true;
    d.stopped = true;
    d.resume = false;
    d.pauseRequested = false;
    d.step = Step::None;
    d.L = L;
    d.base = base;
    d.exception = exception;
    json::Value body = json::Value::object();
    body.set("reason", reason);
    body.set("threadId", 1);
    body.set("allThreadsStopped", true);
    if (!exception.empty()) {
        body.set("description", "Paused on a script error");
        body.set("text", exception.substr(0, exception.find('\n')));
    }
    event("stopped", body);
    while (!d.resume && d.client) {
        serve();
        if (!d.resume) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!exception.empty()) d.step = Step::None;   // (the error goes on unwinding: nothing to step through)
    dropRefs();
    d.stopped = false;
    d.L = nullptr;
    d.base = 0;
    d.exception.clear();
    d.inside = false;
    refreshHook();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

}  // namespace

bool wantsLines() {
    const Debugger& d = dbg();
    return d.client && (!d.anyLines.empty() || d.step != Step::None || d.pauseRequested);
}

double onLine(lua_State* L, lua_Debug* ar) {
    Debugger& d = dbg();
    if (d.inside || !d.client) return 0.0;
    // Stop here? A pause asked for, a step's end, or a breakpoint on this line of this file
    const char* reason = nullptr;
    if (d.pauseRequested) {
        reason = "pause";
    } else if (d.step != Step::None) {
        int depth = depthOf(L);
        if (d.step == Step::In || (d.step == Step::Over && depth <= d.stepDepth) || (d.step == Step::Out && depth < d.stepDepth))
            reason = "step";
    }
    double spent = 0.0;   // (conditions and logpoints run Lua too: not the script's time)
    if (!reason && d.anyLines.count(ar->currentline)) {
        lua_getinfo(L, "S", ar);
        std::string chunk = chunkOf(ar->source);
        for (auto& [key, lines] : d.breakpoints) {
            auto it = lines.find(ar->currentline);
            if (it == lines.end() || !sameSource(key, chunk)) continue;
            Breakpoint& bp = it->second;
            auto started = std::chrono::steady_clock::now();
            d.inside = true;
            bool hit = true;
            if (!bp.condition.empty()) {   // (an error in it stops, saying so)
                std::string error;
                if (pushExpression(L, bp.condition, 0, error)) {
                    hit = lua_toboolean(L, -1);
                    lua_pop(L, 1);
                } else {
                    output("Breakpoint condition (" + bp.condition + "): " + error, "stderr");
                }
            }
            if (hit) hit = hitMatches(bp.hitCondition, ++bp.hits);
            if (hit && !bp.logMessage.empty()) {   // a logpoint: says it, goes on
                std::string text = interpolate(L, bp.logMessage, 0);
                output(text);
                logLine("lua: %s", text.c_str());
                hit = false;
            }
            d.inside = false;
            spent += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (hit) reason = "breakpoint";
            break;
        }
    }
    if (!reason) return spent;
    return spent + stopHere(L, reason, 0, "");
}

double onError(lua_State* L, const char* message) {
    Debugger& d = dbg();
    if (d.inside || !d.client || !d.breakOnErrors) return 0.0;
    // Shown from the function that raised it: past the error handler (and C functions such as error())
    int base = 0;
    lua_Debug ar;
    for (int level = 0; lua_getstack(L, level, &ar); ++level) {
        lua_getinfo(L, "S", &ar);
        if (std::string(ar.what) != "C") {
            base = level;
            break;
        }
    }
    return stopHere(L, "exception", base, message ? message : "error");
}

}  // namespace scripting::debugger
