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
std::vector<lua_State*> coroutines();   // (the scripts' coroutines that still exist)
}

namespace scripting::debugger {

namespace {

enum class Step { None, In, Over, Out };

struct VarRef {
    enum Kind { Locals, Upvalues, Table } kind = Locals;
    lua_State* thread = nullptr;   // (locals, upvalues) the thread (the main one, or a coroutine) and stack level
    int level = 0;
    int table = LUA_NOREF;         // (tables) a registry reference
};

// A data breakpoint: a table's field (self.count), stopping when its value changes
struct Watch {
    std::string name;              // as shown ("count")
    int table = LUA_NOREF;         // registry references: the table, and the value last seen
    int last = LUA_NOREF;
    std::string key;               // a string key, or (numberKey) a number's
    bool numberKey = false;
    double number = 0.0;
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
    std::map<lua_State*, int> threadIds;                // coroutines' thread ids (the main thread is 1)
    int nextThreadId = 2;
    std::map<std::string, Watch> watchable;             // data breakpoints the editor may set (dataBreakpointInfo)
    std::vector<std::string> watching;                  // ... and those it set (their ids)
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

// ---- Threads: the main one (1) and the scripts' coroutines (each listed as a thread of its own)
int threadId(lua_State* t) {
    Debugger& d = dbg();
    if (!t || t == luaState()) return 1;
    auto it = d.threadIds.find(t);
    if (it == d.threadIds.end()) it = d.threadIds.emplace(t, d.nextThreadId++).first;
    return it->second;
}

lua_State* threadById(int id) {
    if (id <= 1) return luaState();
    for (lua_State* co : coroutines())   // (only one that still exists)
        if (threadId(co) == id) return co;
    return nullptr;
}

// A coroutine that finished (or failed) can't be looked at: no stack left
bool alive(lua_State* co) {
    lua_Debug ar;
    return lua_status(co) == LUA_YIELD || lua_getstack(co, 0, &ar) || lua_gettop(co) > 0;
}

// Frame ids carry their thread: thread id * 10000 + level + 1 (0 as the thread: the one stopped)
int frameId(lua_State* t, int level) { return threadId(t) * 10000 + level + 1; }
lua_State* frameThread(int id, int& level) {
    Debugger& d = dbg();
    level = id % 10000 - 1;
    return id < 10000 ? d.L : threadById(id / 10000);
}

// The value on top of `from` moved to `to` (lua_getlocal and the like push on the thread they look at)
void moveTop(lua_State* from, lua_State* to) {
    if (from != to) lua_xmove(from, to, 1);
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
    lua_State* t = r.thread ? r.thread : L;
    lua_Debug ar;
    if (!lua_getstack(t, r.level, &ar)) return list;
    if (r.kind == VarRef::Locals) {
        for (int i = 1;; ++i) {
            const char* name = lua_getlocal(t, &ar, i);
            if (!name) break;
            moveTop(t, L);
            if (name[0] != '(') list.push(describe(L, -1, name));   // ("(temporary)" ones: not the script's)
            lua_pop(L, 1);
        }
    } else {
        lua_getinfo(t, "f", &ar);
        moveTop(t, L);
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

// An expression in a frame (of thread `t`: L itself, or a coroutine), evaluated on L, its value pushed: the frame's
// locals and upvalues first, then the globals (false: an error)
bool pushExpression(lua_State* L, lua_State* t, const std::string& expression, int level, std::string& error) {
    int top = lua_gettop(L);
    lua_newtable(L);   // the environment
    int env = lua_gettop(L);
    lua_Debug ar;
    if (t && level >= 0 && lua_getstack(t, level, &ar)) {
        lua_getinfo(t, "f", &ar);
        moveTop(t, L);
        int f = lua_gettop(L);
        for (int i = 1;; ++i) {
            const char* name = lua_getupvalue(L, f, i);
            if (!name) break;
            if (std::string(name) != "_ENV") lua_setfield(L, env, name);
            else lua_pop(L, 1);
        }
        lua_pop(L, 1);
        for (int i = 1;; ++i) {   // (locals after upvalues: they're nearer)
            const char* name = lua_getlocal(t, &ar, i);
            if (!name) break;
            moveTop(t, L);
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

json::Value evaluate(lua_State* L, lua_State* t, const std::string& expression, int level) {
    json::Value body = json::Value::object();
    std::string error;
    if (!pushExpression(L, t, expression, level, error)) {
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

// A table key as the Variables view shows it: a string as it is, anything else in brackets ("[3]", "[true]")
void pushKey(lua_State* L, const std::string& name) {
    if (name.size() < 2 || name.front() != '[' || name.back() != ']') {
        lua_pushstring(L, name.c_str());
        return;
    }
    std::string inner = name.substr(1, name.size() - 2);
    char* end = nullptr;
    double n = std::strtod(inner.c_str(), &end);
    if (!inner.empty() && end && !*end) {
        if (n == (double)(lua_Integer)n) lua_pushinteger(L, (lua_Integer)n);
        else lua_pushnumber(L, n);
    } else if (inner == "true" || inner == "false") {
        lua_pushboolean(L, inner == "true");
    } else {
        lua_pushstring(L, inner.c_str());
    }
}

// Sets a variable of the Variables view (a local, an upvalue, a table's field) to an expression's value, evaluated
// in its frame (a table's: in the frame stopped in); `out` describes the new value
bool setVariable(lua_State* L, int reference, const std::string& name, const std::string& expression, json::Value& out,
                 std::string& error) {
    Debugger& d = dbg();
    if (reference <= 0 || reference > (int)d.refs.size()) {
        error = "no such variable";
        return false;
    }
    VarRef r = d.refs[(size_t)reference - 1];
    lua_State* t = r.kind == VarRef::Table ? L : (r.thread ? r.thread : L);
    int top = lua_gettop(L);
    if (!pushExpression(L, t, expression, r.kind == VarRef::Table ? d.base : r.level, error)) return false;
    int value = lua_gettop(L);
    bool set = false;
    if (r.kind == VarRef::Table) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, r.table);
        pushKey(L, name);
        lua_pushvalue(L, value);
        lua_rawset(L, -3);
        set = true;
    } else {
        lua_Debug ar;
        if (lua_getstack(t, r.level, &ar)) {
            if (r.kind == VarRef::Locals) {
                int index = 0;   // (the last by that name: the innermost one in scope)
                for (int i = 1;; ++i) {
                    const char* n = lua_getlocal(t, &ar, i);
                    if (!n) break;
                    lua_pop(t, 1);
                    if (name == n) index = i;
                }
                if (index > 0) {
                    lua_pushvalue(L, value);
                    moveTop(L, t);
                    set = lua_setlocal(t, &ar, index) != nullptr;
                }
            } else {
                lua_getinfo(t, "f", &ar);
                moveTop(t, L);
                int f = lua_gettop(L);
                for (int i = 1; !set; ++i) {
                    const char* n = lua_getupvalue(L, f, i);
                    if (!n) break;
                    lua_pop(L, 1);
                    if (name == n) {
                        lua_pushvalue(L, value);
                        set = lua_setupvalue(L, f, i) != nullptr;
                    }
                }
            }
        }
    }
    if (set) out = describe(L, value, name);
    else error = "no variable " + name + " there";
    lua_settop(L, top);
    return set;
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
            if (pushExpression(L, L, message.substr(i + 1, end - i - 1), level, error)) {
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
        caps.set("supportsSetVariable", true);
        caps.set("supportsDataBreakpoints", true);
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
    } else if (cmd == "setDataBreakpoints") {
        lua_State* L = d.stopped ? d.L : luaState();
        for (const std::string& id : d.watching) {
            Watch& w = d.watchable[id];
            luaL_unref(L, LUA_REGISTRYINDEX, w.last);
            w.last = LUA_NOREF;
        }
        d.watching.clear();
        json::Value list = json::Value::array();
        for (const json::Value& b : args["breakpoints"].elements()) {
            auto it = d.watchable.find(b["dataId"].asString());
            json::Value v = json::Value::object();
            v.set("verified", it != d.watchable.end());
            list.push(std::move(v));
            if (it == d.watchable.end()) continue;
            Watch& w = it->second;   // (its value now: a change from this one stops)
            lua_rawgeti(L, LUA_REGISTRYINDEX, w.table);
            pushKey(L, w.key);
            lua_rawget(L, -2);
            w.last = luaL_ref(L, LUA_REGISTRYINDEX);
            lua_pop(L, 1);
            d.watching.push_back(it->first);
        }
        json::Value body = json::Value::object();
        body.set("breakpoints", list);
        respond(req, body);
        refreshHook();
    } else if (cmd == "threads") {   // the main thread, and each coroutine still there
        json::Value list = json::Value::array();
        json::Value main = json::Value::object();
        main.set("id", 1);
        main.set("name", "Lua (main)");
        list.push(main);
        std::vector<std::pair<int, lua_State*>> cos;
        for (lua_State* co : coroutines())
            if (alive(co)) cos.push_back({threadId(co), co});
        std::sort(cos.begin(), cos.end());
        for (const auto& [id, co] : cos) {
            json::Value t = json::Value::object();
            t.set("id", id);
            const char* state = co == d.L ? "running" : lua_status(co) == LUA_YIELD ? "suspended" : "not started";
            t.set("name", "Coroutine " + std::to_string(id - 1) + " (" + state + ")");
            list.push(std::move(t));
        }
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
        d.watching.clear();   // (their registry references go with the state)
        d.watchable.clear();
        d.step = Step::None;
        d.pauseRequested = false;
        d.resume = true;
        respond(req);
        d.client.reset();
        refreshHook();
    } else if (!d.stopped) {
        respond(req, json::Value::object(), false, "the game is running (pause it first)");
    } else if (cmd == "stackTrace") {
        lua_State* t = args.has("threadId") ? threadById(args["threadId"].asInt()) : d.L;
        if (!t) t = d.L;
        json::Value frames = json::Value::array();
        lua_Debug ar;
        int first = t == d.L ? d.base : 0;
        while (t != d.L && lua_getstack(t, first, &ar) && lua_getinfo(t, "S", &ar) && std::string(ar.what) == "C")
            ++first;   // (another thread: from its script, past coroutine.resume and yield)
        for (int level = first; lua_getstack(t, level, &ar) && level < 64; ++level) {
            lua_getinfo(t, "Sln", &ar);
            json::Value f = json::Value::object();
            f.set("id", frameId(t, level));
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
        int level = 0;
        lua_State* t = frameThread(args["frameId"].asInt(1), level);
        json::Value list = json::Value::array();
        for (VarRef::Kind kind : {VarRef::Locals, VarRef::Upvalues}) {
            VarRef r;
            r.kind = kind;
            r.thread = t ? t : d.L;
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
        int level = d.base;
        lua_State* t = args.has("frameId") ? frameThread(args["frameId"].asInt(), level) : d.L;
        respond(req, evaluate(d.L, t ? t : d.L, args["expression"].asString(), level));
    } else if (cmd == "setVariable") {
        json::Value v;
        std::string error;
        if (setVariable(d.L, args["variablesReference"].asInt(), args["name"].asString(), args["value"].asString(), v, error)) {
            json::Value body = json::Value::object();
            body.set("value", v["value"]);
            body.set("type", v["type"]);
            body.set("variablesReference", v["variablesReference"]);
            respond(req, body);
        } else {
            respond(req, json::Value::object(), false, error);
        }
    } else if (cmd == "dataBreakpointInfo") {   // a table's field can be watched (a local can't: it's gone on return)
        int ref = args["variablesReference"].asInt();
        std::string name = args["name"].asString();
        json::Value body = json::Value::object();
        if (ref > 0 && ref <= (int)d.refs.size() && d.refs[(size_t)ref - 1].kind == VarRef::Table) {
            Watch w;
            w.name = name;
            w.key = name;
            lua_rawgeti(d.L, LUA_REGISTRYINDEX, d.refs[(size_t)ref - 1].table);
            w.table = luaL_ref(d.L, LUA_REGISTRYINDEX);
            std::string id = "field " + std::to_string(d.watchable.size() + 1) + " " + name;
            d.watchable[id] = w;
            json::Value access = json::Value::array();
            access.push(json::Value("write"));
            body.set("dataId", id);
            body.set("description", "When " + name + " changes");
            body.set("accessTypes", access);
            body.set("canPersist", false);
        } else {
            body.set("dataId", json::Value());
            body.set("description", "Only a table's fields can be watched (expand self for its fields)");
        }
        respond(req, body);
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
        d.watching.clear();
        d.watchable.clear();
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
    d.watching.clear();
    d.watchable.clear();
    d.threadIds.clear();
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
double stopHere(lua_State* L, const char* reason, int base, const std::string& exception, const std::string& text = "") {
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
    body.set("threadId", threadId(L));
    body.set("allThreadsStopped", true);
    if (!text.empty()) {
        body.set("description", text);
        body.set("text", text);
    }
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
    return d.client && (!d.anyLines.empty() || !d.watching.empty() || d.step != Step::None || d.pauseRequested);
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
    std::string text;
    if (!reason && !d.watching.empty()) {   // a watched field changed (the line before this one changed it)
        for (const std::string& id : d.watching) {
            Watch& w = d.watchable[id];
            lua_rawgeti(L, LUA_REGISTRYINDEX, w.table);
            pushKey(L, w.key);
            lua_rawget(L, -2);
            lua_rawgeti(L, LUA_REGISTRYINDEX, w.last);
            bool changed = !lua_rawequal(L, -1, -2);
            lua_pop(L, 1);
            if (changed) {
                luaL_unref(L, LUA_REGISTRYINDEX, w.last);
                lua_pushvalue(L, -1);
                w.last = luaL_ref(L, LUA_REGISTRYINDEX);
                size_t n = 0;
                const char* now = luaL_tolstring(L, -1, &n);
                text = w.name + " changed: now " + std::string(now, n);
                lua_pop(L, 1);
                reason = "data breakpoint";
            }
            lua_pop(L, 2);
            if (reason) break;
        }
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
                if (pushExpression(L, L, bp.condition, 0, error)) {
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
    return spent + stopHere(L, reason, 0, "", text);
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
