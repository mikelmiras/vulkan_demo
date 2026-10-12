#include "script/lua_script.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "core/log.h"
#include "core/vfs.h"
#include "game/game.h"
#include "game/ui.h"
#include "scene/behaviour.h"
#include "scene/scene.h"

// Lua is compiled as C++ (errors are exceptions, so they unwind through our frames): no extern "C" here
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------------------------
// Script types
// ---------------------------------------------------------------------------------------------
struct FieldDecl {
    enum class Kind { Number, Int, Bool, String, Choice, Vec3 };
    std::string name;
    Kind kind = Kind::Number;
    float number = 0.f, min = 0.f, max = 0.f, speed = 0.05f;
    bool flag = false;
    std::string text;
    Vec3 vec;
    bool isRef = false;
    FieldRef ref = FieldRef::Entity;
    std::vector<std::string> options;
};

// A field's value in a behaviour (the members the declaration's kind doesn't use stay as they are)
struct FieldValue {
    FieldDecl::Kind kind = FieldDecl::Kind::Number;
    float number = 0.f;
    bool flag = false;
    std::string text;
    Vec3 vec;
    bool operator==(const FieldValue& o) const {
        return kind == o.kind && number == o.number && flag == o.flag && text == o.text && vec.x == o.vec.x &&
               vec.y == o.vec.y && vec.z == o.vec.z;
    }
    bool operator!=(const FieldValue& o) const { return !(*this == o); }
};
using FieldValues = std::vector<std::pair<std::string, FieldValue>>;

FieldValue defaultValue(const FieldDecl& d) {
    FieldValue v;
    v.kind = d.kind;
    v.number = d.number;
    v.flag = d.flag;
    v.text = d.text;
    v.vec = d.vec;
    return v;
}

struct ScriptType {
    std::string name;
    fs::path file;                 // "" when define()d
    std::string chunkName;         // "scripts/Door.lua", as errors show it
    fs::file_time_type mtime{};
    bool failed = false;           // the last load failed: tried again when the file changes
    std::string description;
    std::shared_ptr<const std::vector<FieldDecl>> fields = std::make_shared<std::vector<FieldDecl>>();
    int classRef = LUA_NOREF;      // the table the script returned (reloads refill it in place)
    int generation = 0;            // bumped by every successful load
};

struct Engine {
    lua_State* L = nullptr;
    uint64_t stateId = 1;          // a new Lua state gets a new id: references into an old one are dropped
    std::map<std::string, ScriptType> types;   // guarded by typesMutex (fields are read on loading threads)
    std::mutex typesMutex;
    fs::path root;
    std::vector<std::string> errors;
    std::chrono::steady_clock::time_point lastPoll{};
    // The callback running: what game / input / ui and the entity functions work on
    GameContext* ctx = nullptr;
    GameUI* ui = nullptr;
    EntityId self = kNoEntity;

    ~Engine() {
        if (L) lua_close(L);
        L = nullptr;
    }
};

Engine& eng() {
    static Engine e;
    return e;
}

void addError(const std::string& message) {
    Engine& e = eng();
    logLine("script: %s", message.c_str());
    if (e.errors.size() < 50) e.errors.push_back(message);
}

std::shared_ptr<const std::vector<FieldDecl>> declarations(const std::string& type, int* generation = nullptr) {
    Engine& e = eng();
    std::lock_guard<std::mutex> lock(e.typesMutex);
    auto it = e.types.find(type);
    if (it == e.types.end()) return nullptr;
    if (generation) *generation = it->second.generation;
    return it->second.fields;
}

// While a callback runs: the context it gets, restored afterwards (callbacks can nest through messages and timers)
struct CallScope {
    GameContext* ctx;
    GameUI* ui;
    EntityId self;
    CallScope(GameContext* c, GameUI* u, EntityId s) : ctx(eng().ctx), ui(eng().ui), self(eng().self) {
        eng().ctx = c;
        eng().ui = u;
        eng().self = s;
    }
    ~CallScope() {
        eng().ctx = ctx;
        eng().ui = ui;
        eng().self = self;
    }
};

// ---------------------------------------------------------------------------------------------
// Calling Lua
// ---------------------------------------------------------------------------------------------
int traceback(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (!msg) msg = luaL_tolstring(L, 1, nullptr);
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// lua_pcall with a traceback; the function and its arguments are on the stack
bool pcall(lua_State* L, int nargs, int nresults, std::string& error) {
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    int r = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    if (r == LUA_OK) return true;
    const char* msg = lua_tostring(L, -1);
    error = msg ? msg : "(error object is not a string)";
    lua_pop(L, 1);
    return false;
}

// "scripts/Door.lua:12: attempt to ... \nstack traceback: ..." -> the first line (for the viewport)
std::string firstLine(const std::string& s) { return s.substr(0, s.find('\n')); }

void reportRuntimeError(const std::string& what, const std::string& error) {
    logLine("script error in %s: %s", what.c_str(), error.c_str());
    if (eng().ctx) eng().ctx->log("Script error (" + what + "): " + firstLine(error));
}

// ---------------------------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------------------------
const char* kVec3Meta = "vkw.vec3";
const char* kEntityMeta = "vkw.Entity";

void pushVec3(lua_State* L, Vec3 v) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z);
    lua_setfield(L, -2, "z");
    luaL_setmetatable(L, kVec3Meta);
}

bool isVec3(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return false;
    lua_getfield(L, idx, "x");
    bool ok = lua_isnumber(L, -1);
    lua_pop(L, 1);
    if (ok) return true;
    lua_rawgeti(L, idx, 1);
    ok = lua_isnumber(L, -1);
    lua_pop(L, 1);
    return ok;
}

// {x=, y=, z=} or {1, 2, 3}
Vec3 toVec3(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    luaL_checktype(L, idx, LUA_TTABLE);
    Vec3 v;
    float* c[3] = {&v.x, &v.y, &v.z};
    const char* names[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        lua_getfield(L, idx, names[i]);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_rawgeti(L, idx, i + 1);
        }
        *c[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    return v;
}

Vec3 optVec3(lua_State* L, int idx, Vec3 def) { return lua_isnoneornil(L, idx) ? def : toVec3(L, idx); }

// {r=, g=, b=, a=}, {1, 0, 0, 1} or a vec3
Color toColor(lua_State* L, int idx, Color def) {
    if (lua_isnoneornil(L, idx)) return def;
    idx = lua_absindex(L, idx);
    luaL_checktype(L, idx, LUA_TTABLE);
    Color c = def;
    float* out[4] = {&c.r, &c.g, &c.b, &c.a};
    const char* names[4] = {"r", "g", "b", "a"};
    const char* alt[4] = {"x", "y", "z", nullptr};
    for (int i = 0; i < 4; ++i) {
        lua_getfield(L, idx, names[i]);
        if (lua_isnil(L, -1) && alt[i]) {
            lua_pop(L, 1);
            lua_getfield(L, idx, alt[i]);
        }
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_rawgeti(L, idx, i + 1);
        }
        if (lua_isnumber(L, -1)) *out[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    return c;
}

void pushEntity(lua_State* L, EntityId id) {
    if (id == kNoEntity) {
        lua_pushnil(L);
        return;
    }
    auto* p = static_cast<EntityId*>(lua_newuserdatauv(L, sizeof(EntityId), 0));
    *p = id;
    luaL_setmetatable(L, kEntityMeta);
}

GameContext& context(lua_State* L) {
    if (!eng().ctx) luaL_error(L, "this works only while the game runs (in a behaviour's callbacks)");
    return *eng().ctx;
}

// An entity handle, an id or a name -> its id (kNoEntity if there's no such entity)
EntityId toEntityId(lua_State* L, int idx) {
    if (auto* p = static_cast<EntityId*>(luaL_testudata(L, idx, kEntityMeta))) return *p;
    if (lua_type(L, idx) == LUA_TNUMBER) return (EntityId)lua_tointeger(L, idx);
    if (lua_type(L, idx) == LUA_TSTRING) {
        Entity* e = context(L).find(std::string(lua_tostring(L, idx)));
        return e ? e->id : kNoEntity;
    }
    return kNoEntity;
}

Entity* toEntity(lua_State* L, int idx) {
    EntityId id = toEntityId(L, idx);
    GameContext& ctx = context(L);
    return id != kNoEntity && !ctx.isDestroyed(id) ? ctx.find(id) : nullptr;
}

Entity& checkEntity(lua_State* L, int idx) {
    Entity* e = toEntity(L, idx);
    if (!e) luaL_error(L, "the entity no longer exists");
    return *e;
}

// A number for scripts: whole numbers as Lua integers (so 14 prints "14", not "14.0")
void pushNumber(lua_State* L, double v) {
    if (std::floor(v) == v && std::fabs(v) < 9.0e15) lua_pushinteger(L, (lua_Integer)v);
    else lua_pushnumber(L, v);
}

// json state value -> Lua
void pushJson(lua_State* L, const json::Value* v) {
    if (!v) lua_pushnil(L);
    else if (v->type() == json::Value::Type::Number) pushNumber(L, v->asNumber());
    else if (v->type() == json::Value::Type::Bool) lua_pushboolean(L, v->asBool());
    else if (v->type() == json::Value::Type::String) lua_pushstring(L, v->asString().c_str());
    else lua_pushnil(L);
}

// ---------------------------------------------------------------------------------------------
// Behaviour fields from Lua and back (for e:getField / e:setField, on any behaviour)
// ---------------------------------------------------------------------------------------------
struct LuaFieldAccess : FieldVisitor {
    lua_State* L;
    const std::string& name;
    int valueIdx;       // 0: read (push the value), else write the value at this index
    bool found = false;
    LuaFieldAccess(lua_State* l, const std::string& n, int v) : L(l), name(n), valueIdx(v) {}
    bool match(const char* n) {
        if (found || name != n) return false;
        found = true;
        return true;
    }
    void field(const char* n, float& v, float, float, float) override {
        if (!match(n)) return;
        if (valueIdx) v = (float)luaL_checknumber(L, valueIdx);
        else lua_pushnumber(L, v);
    }
    void field(const char* n, int& v, int, int) override {
        if (!match(n)) return;
        if (valueIdx) v = (int)std::lround(luaL_checknumber(L, valueIdx));
        else lua_pushinteger(L, v);
    }
    void field(const char* n, bool& v) override {
        if (!match(n)) return;
        if (valueIdx) v = lua_toboolean(L, valueIdx);
        else lua_pushboolean(L, v);
    }
    void field(const char* n, Vec3& v, float) override {
        if (!match(n)) return;
        if (valueIdx) v = toVec3(L, valueIdx);
        else pushVec3(L, v);
    }
    void field(const char* n, std::string& v) override {
        if (!match(n)) return;
        if (valueIdx) v = luaL_checkstring(L, valueIdx);
        else lua_pushstring(L, v.c_str());
    }
    void field(const char* n, std::string& v, FieldRef) override { field(n, v); }
    void choice(const char* n, int& v, const char* const* options, int count) override {
        if (!match(n)) return;
        if (!valueIdx) {
            lua_pushstring(L, v >= 0 && v < count ? options[v] : "");
            return;
        }
        if (lua_type(L, valueIdx) == LUA_TNUMBER) v = std::clamp((int)lua_tointeger(L, valueIdx), 0, std::max(count - 1, 0));
        else
            for (int i = 0; i < count; ++i)
                if (std::string(luaL_checkstring(L, valueIdx)) == options[i]) v = i;
    }
};

Behaviour* findBehaviour(Entity& e, const std::string& type) {
    for (BehaviourPtr& b : e.behaviours)
        if (b && type == b->typeName()) return b.get();
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The behaviour
// ---------------------------------------------------------------------------------------------
class LuaBehaviour : public Behaviour {
public:
    explicit LuaBehaviour(std::string type) : type_(std::move(type)) {}
    LuaBehaviour(const LuaBehaviour& o) : Behaviour(), type_(o.type_), values_(o.values_) {}   // (not the instance)
    LuaBehaviour& operator=(const LuaBehaviour&) = delete;
    ~LuaBehaviour() override { release(); }

    const char* typeName() const override { return type_.c_str(); }
    std::unique_ptr<Behaviour> clone() const override { return std::make_unique<LuaBehaviour>(*this); }

    void fields(FieldVisitor& v) override {
        auto decls = declarations(type_);
        if (decls) visitFields(v, *decls);
    }
    // Save games: the fields as the instance has them now
    void state(FieldVisitor& v) override {
        auto decls = declarations(type_);
        if (!decls || !live()) return;
        pull(*decls);
        visitFields(v, *decls);
        push(*decls);
    }

    void start(GameContext& ctx, Entity& self) override {
        release();
        failed_ = false;
        Engine& e = eng();
        int classRef = LUA_NOREF;
        {
            std::lock_guard<std::mutex> lock(e.typesMutex);
            auto it = e.types.find(type_);
            if (it != e.types.end()) classRef = it->second.classRef;
        }
        if (!e.L || classRef == LUA_NOREF) return;
        lua_State* L = e.L;
        lua_newtable(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, classRef);
        lua_setmetatable(L, -2);
        stateId_ = e.stateId;
        ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
        if (auto decls = declarations(type_, &pushedGeneration_)) {
            for (const FieldDecl& d : *decls) value(d);   // (defaults for fields added since)
            push(*decls);
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref_);
        pushEntity(L, self.id);
        lua_setfield(L, -2, "entity");
        lua_pop(L, 1);
        call("start", ctx, self, nullptr, [](lua_State*) { return 0; });
    }
    void update(GameContext& ctx, Entity& self) override {
        float dt = ctx.dt();
        call("update", ctx, self, nullptr, [dt](lua_State* L) {
            lua_pushnumber(L, dt);
            return 1;
        });
    }
    void onTriggerEnter(GameContext& ctx, Entity& self, Entity& other) override {
        EntityId id = other.id;
        call("onTriggerEnter", ctx, self, nullptr, [id](lua_State* L) {
            pushEntity(L, id);
            return 1;
        });
    }
    void onTriggerExit(GameContext& ctx, Entity& self, Entity& other) override {
        EntityId id = other.id;
        call("onTriggerExit", ctx, self, nullptr, [id](lua_State* L) {
            pushEntity(L, id);
            return 1;
        });
    }
    void drawUI(GameContext& ctx, Entity& self, GameUI& ui) override {
        call("drawUI", ctx, self, &ui, [](lua_State* L) {
            lua_getglobal(L, "ui");
            return 1;
        });
    }
    void onAnimationEvent(GameContext& ctx, Entity& self, const std::string& event) override {
        call("onAnimationEvent", ctx, self, nullptr, [&event](lua_State* L) {
            lua_pushstring(L, event.c_str());
            return 1;
        });
    }
    void onMessage(GameContext& ctx, Entity& self, const Message& m) override {
        call("onMessage", ctx, self, nullptr, [&m](lua_State* L) {
            lua_createtable(L, 0, 4);
            lua_pushstring(L, m.name.c_str());
            lua_setfield(L, -2, "name");
            pushEntity(L, m.from);
            lua_setfield(L, -2, "from");
            pushNumber(L, m.value);
            lua_setfield(L, -2, "value");
            lua_pushstring(L, m.text.c_str());
            lua_setfield(L, -2, "text");
            return 1;
        });
    }

    // The instance table on the stack (nil if not started)
    void pushInstance(lua_State* L) const {
        if (live()) lua_rawgeti(L, LUA_REGISTRYINDEX, ref_);
        else lua_pushnil(L);
    }

private:
    bool live() const { return ref_ != LUA_NOREF && eng().L && stateId_ == eng().stateId; }
    void release() {
        if (live()) luaL_unref(eng().L, LUA_REGISTRYINDEX, ref_);
        ref_ = LUA_NOREF;
    }

    FieldValue& value(const FieldDecl& d) {
        for (auto& [name, v] : values_)
            if (name == d.name) {
                if (v.kind != d.kind) v = defaultValue(d);   // the script changed the field's type
                return v;
            }
        values_.emplace_back(d.name, defaultValue(d));
        return values_.back().second;
    }

    void visitFields(FieldVisitor& v, const std::vector<FieldDecl>& decls) {
        for (const FieldDecl& d : decls) {
            FieldValue& val = value(d);
            const char* n = d.name.c_str();
            switch (d.kind) {
            case FieldDecl::Kind::Number: v.field(n, val.number, d.speed, d.min, d.max); break;
            case FieldDecl::Kind::Int: {
                int i = (int)std::lround(val.number);
                v.field(n, i, (int)d.min, (int)d.max);
                val.number = (float)i;
                break;
            }
            case FieldDecl::Kind::Bool: v.field(n, val.flag); break;
            case FieldDecl::Kind::String:
                if (d.isRef) v.field(n, val.text, d.ref);
                else v.field(n, val.text);
                break;
            case FieldDecl::Kind::Choice: {
                std::vector<const char*> options;
                for (const std::string& o : d.options) options.push_back(o.c_str());
                int i = 0;
                for (size_t k = 0; k < d.options.size(); ++k)
                    if (d.options[k] == val.text) i = (int)k;
                v.choice(n, i, options.data(), (int)options.size());
                if (i >= 0 && i < (int)d.options.size()) val.text = d.options[(size_t)i];
                break;
            }
            case FieldDecl::Kind::Vec3: v.field(n, val.vec, d.speed); break;
            }
        }
    }

    // values_ -> the instance's members
    void push(const std::vector<FieldDecl>& decls) {
        lua_State* L = eng().L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref_);
        for (const FieldDecl& d : decls) {
            const FieldValue& v = value(d);
            switch (d.kind) {
            case FieldDecl::Kind::Number: lua_pushnumber(L, v.number); break;
            case FieldDecl::Kind::Int: lua_pushinteger(L, (lua_Integer)std::lround(v.number)); break;
            case FieldDecl::Kind::Bool: lua_pushboolean(L, v.flag); break;
            case FieldDecl::Kind::String:
            case FieldDecl::Kind::Choice: lua_pushstring(L, v.text.c_str()); break;
            case FieldDecl::Kind::Vec3: pushVec3(L, v.vec); break;
            }
            lua_setfield(L, -2, d.name.c_str());
        }
        lua_pop(L, 1);
        pushed_ = values_;
    }
    // The instance's members -> values_ (what the inspector shows while playing, and save games)
    void pull(const std::vector<FieldDecl>& decls) {
        lua_State* L = eng().L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref_);
        for (const FieldDecl& d : decls) {
            FieldValue& v = value(d);
            lua_getfield(L, -1, d.name.c_str());
            switch (d.kind) {
            case FieldDecl::Kind::Number:
            case FieldDecl::Kind::Int:
                if (lua_isnumber(L, -1)) v.number = (float)lua_tonumber(L, -1);
                break;
            case FieldDecl::Kind::Bool: v.flag = lua_toboolean(L, -1); break;
            case FieldDecl::Kind::String:
            case FieldDecl::Kind::Choice:
                if (lua_type(L, -1) == LUA_TSTRING) v.text = lua_tostring(L, -1);
                break;
            case FieldDecl::Kind::Vec3:
                if (isVec3(L, -1)) v.vec = toVec3(L, -1);
                break;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        pushed_ = values_;
    }

    template <class PushArgs>
    void call(const char* method, GameContext& ctx, Entity& self, GameUI* ui, PushArgs&& pushArgs) {
        if (!live()) return;
        int generation = 0;
        auto decls = declarations(type_, &generation);
        if (failed_ && generation == failedGeneration_) return;   // (until the script changes)
        failed_ = false;
        lua_State* L = eng().L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref_);
        lua_getfield(L, -1, method);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            return;
        }
        lua_insert(L, -2);                         // function, self
        // Edited in the inspector (or e:setField) since, or the script changed (fields added)
        if (decls && (values_ != pushed_ || generation != pushedGeneration_)) push(*decls);
        pushedGeneration_ = generation;
        int n = pushArgs(L);
        std::string error;
        bool ok;
        {
            CallScope scope(&ctx, ui, self.id);
            ok = pcall(L, 1 + n, 0, error);
        }
        if (!ok) {
            failed_ = true;
            failedGeneration_ = generation;
            CallScope scope(&ctx, ui, self.id);
            reportRuntimeError(type_ + "." + method, error);
        }
        if (decls && live()) pull(*decls);
    }

    std::string type_;
    FieldValues values_, pushed_;
    int ref_ = LUA_NOREF;
    uint64_t stateId_ = 0;
    bool failed_ = false;
    int failedGeneration_ = 0;
    int pushedGeneration_ = -1;
};

namespace {

// ---------------------------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------------------------
Vec3 worldDir(GameContext& ctx, EntityId id, Vec3 local) {
    return normalize(transformDir(ctx.scene().worldMatrix(id), local));
}

int entityIndex(lua_State* L) {   // e.key
    EntityId id = *static_cast<EntityId*>(luaL_checkudata(L, 1, kEntityMeta));
    std::string key = luaL_checkstring(L, 2);
    // Methods first
    luaL_getmetatable(L, kEntityMeta);
    lua_getfield(L, -1, "methods");
    lua_getfield(L, -1, key.c_str());
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 3);

    if (key == "id") { lua_pushinteger(L, id); return 1; }
    GameContext& ctx = context(L);
    Entity* e = ctx.isDestroyed(id) ? nullptr : ctx.find(id);
    if (key == "exists") { lua_pushboolean(L, e != nullptr); return 1; }
    if (!e) return luaL_error(L, "entity %d no longer exists (reading '%s')", (int)id, key.c_str());
    const Transform& t = e->transform;
    if (key == "name") lua_pushstring(L, e->name.c_str());
    else if (key == "position") pushVec3(L, {t.position[0], t.position[1], t.position[2]});
    else if (key == "rotation") pushVec3(L, {t.rotation[0], t.rotation[1], t.rotation[2]});
    else if (key == "scale") pushVec3(L, {t.scale[0], t.scale[1], t.scale[2]});
    else if (key == "worldPosition") pushVec3(L, ctx.worldPosition(id));
    else if (key == "forward") pushVec3(L, worldDir(ctx, id, {0, 0, -1}));
    else if (key == "right") pushVec3(L, worldDir(ctx, id, {1, 0, 0}));
    else if (key == "up") pushVec3(L, worldDir(ctx, id, {0, 1, 0}));
    else if (key == "visible") lua_pushboolean(L, e->visible);
    else if (key == "layer") lua_pushinteger(L, e->layer);
    else if (key == "parent") pushEntity(L, e->parent);
    else if (key == "tags") {
        lua_createtable(L, (int)e->tags.size(), 0);
        for (size_t i = 0; i < e->tags.size(); ++i) {
            lua_pushstring(L, e->tags[i].c_str());
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
    } else if (key == "velocity") {
        if (e->rigidBody) pushVec3(L, e->rigidBody->velocity);
        else lua_pushnil(L);
    } else if (key == "color") {
        if (e->meshRenderer) pushVec3(L, e->meshRenderer->color);
        else lua_pushnil(L);
    } else lua_pushnil(L);
    return 1;
}

int entityNewIndex(lua_State* L) {   // e.key = value
    Entity& e = checkEntity(L, 1);
    std::string key = luaL_checkstring(L, 2);
    Transform& t = e.transform;
    auto set3 = [&](float* out) {
        Vec3 v = toVec3(L, 3);
        out[0] = v.x, out[1] = v.y, out[2] = v.z;
    };
    if (key == "name") e.name = luaL_checkstring(L, 3);
    else if (key == "position") set3(t.position);
    else if (key == "rotation") set3(t.rotation);
    else if (key == "scale") set3(t.scale);
    else if (key == "worldPosition") {
        Vec3 p = toVec3(L, 3);
        if (e.parent != kNoEntity) p = transformPoint(inverseAffine(context(L).scene().worldMatrix(e.parent)), p);
        t.position[0] = p.x, t.position[1] = p.y, t.position[2] = p.z;
    } else if (key == "visible") e.visible = lua_toboolean(L, 3);
    else if (key == "velocity") {
        if (!e.rigidBody) return luaL_error(L, "'%s' has no rigid body", e.name.c_str());
        e.rigidBody->velocity = toVec3(L, 3);
    } else if (key == "color") {
        if (!e.meshRenderer) return luaL_error(L, "'%s' has no mesh renderer", e.name.c_str());
        e.meshRenderer->color = toVec3(L, 3);
    } else return luaL_error(L, "entities have no settable property '%s'", key.c_str());
    return 0;
}

int entityEq(lua_State* L) {
    auto* a = static_cast<EntityId*>(luaL_testudata(L, 1, kEntityMeta));
    auto* b = static_cast<EntityId*>(luaL_testudata(L, 2, kEntityMeta));
    lua_pushboolean(L, a && b && *a == *b);
    return 1;
}

int entityToString(lua_State* L) {
    EntityId id = *static_cast<EntityId*>(luaL_checkudata(L, 1, kEntityMeta));
    Entity* e = eng().ctx ? eng().ctx->find(id) : nullptr;
    lua_pushfstring(L, "Entity(%d %s)", (int)id, e ? e->name.c_str() : "?");
    return 1;
}

int eHasTag(lua_State* L) {
    lua_pushboolean(L, checkEntity(L, 1).hasTag(luaL_checkstring(L, 2)));
    return 1;
}
int eAddTag(lua_State* L) {
    Entity& e = checkEntity(L, 1);
    std::string tag = luaL_checkstring(L, 2);
    if (!e.hasTag(tag)) e.tags.push_back(tag);
    return 0;
}
int eRemoveTag(lua_State* L) {
    Entity& e = checkEntity(L, 1);
    std::string tag = luaL_checkstring(L, 2);
    e.tags.erase(std::remove(e.tags.begin(), e.tags.end(), tag), e.tags.end());
    return 0;
}
int eDestroy(lua_State* L) {
    EntityId id = toEntityId(L, 1);
    if (id != kNoEntity) context(L).destroy(id);
    return 0;
}
Message toMessage(lua_State* L, int nameIdx) {
    Message m;
    m.name = luaL_checkstring(L, nameIdx);
    m.from = eng().self;
    m.value = (float)luaL_optnumber(L, nameIdx + 1, 0.0);
    m.text = luaL_optstring(L, nameIdx + 2, "");
    return m;
}
int eSend(lua_State* L) {   // e:send(name, value, text)
    EntityId id = toEntityId(L, 1);
    if (id != kNoEntity) context(L).send(id, toMessage(L, 2));
    return 0;
}
int eMoveCharacter(lua_State* L) {
    Entity& e = checkEntity(L, 1);
    CharacterMove m = context(L).moveCharacter(e, toVec3(L, 2));
    lua_createtable(L, 0, 4);
    pushVec3(L, m.displacement);
    lua_setfield(L, -2, "displacement");
    lua_pushboolean(L, m.grounded);
    lua_setfield(L, -2, "grounded");
    lua_pushboolean(L, m.hitWall);
    lua_setfield(L, -2, "hitWall");
    lua_pushboolean(L, m.hitCeiling);
    lua_setfield(L, -2, "hitCeiling");
    return 1;
}
int eLookAt(lua_State* L) {   // turns the entity's -Z towards a point (world space), keeping its scale
    Entity& e = checkEntity(L, 1);
    GameContext& ctx = context(L);
    Vec3 target = toVec3(L, 2), up = optVec3(L, 3, {0, 1, 0});
    Vec3 pos = ctx.worldPosition(e.id);
    if (length(target - pos) < 1e-6f) return 0;
    Mat4 world = inverseAffine(lookAt(pos, target, up));
    Mat4 local = e.parent != kNoEntity ? inverseAffine(ctx.scene().worldMatrix(e.parent)) * world : world;
    float scale[3] = {e.transform.scale[0], e.transform.scale[1], e.transform.scale[2]};
    e.transform.setMatrix(local);
    for (int i = 0; i < 3; ++i) e.transform.scale[i] = scale[i];
    return 0;
}
int eEmitParticles(lua_State* L) {
    EntityId id = toEntityId(L, 1);
    context(L).emitParticles(id, (int)luaL_optinteger(L, 2, -1));
    return 0;
}
int ePlaySound(lua_State* L) {
    Entity& e = checkEntity(L, 1);
    GameContext& ctx = context(L);
    ctx.playSound(luaL_checkstring(L, 2), ctx.worldPosition(e.id), (float)luaL_optnumber(L, 3, 1.0));
    return 0;
}
int eSetRagdoll(lua_State* L) {
    context(L).setRagdoll(toEntityId(L, 1), lua_toboolean(L, 2));
    return 0;
}
Animator& checkAnimator(lua_State* L, Entity& e) {
    if (!e.animator) luaL_error(L, "'%s' has no animator", e.name.c_str());
    return *e.animator;
}
int eSetAnimParameter(lua_State* L) {   // e:setAnimParameter(name, value): a number or a boolean
    Entity& e = checkEntity(L, 1);
    float v = lua_isboolean(L, 3) ? (lua_toboolean(L, 3) ? 1.f : 0.f) : (float)luaL_checknumber(L, 3);
    checkAnimator(L, e).setParameter(luaL_checkstring(L, 2), v);
    return 0;
}
int ePlayAnimation(lua_State* L) {   // e:playAnimation(clip, fade, loop)
    Entity& e = checkEntity(L, 1);
    checkAnimator(L, e).play(luaL_checkstring(L, 2), (float)luaL_optnumber(L, 3, 0.2), lua_isnoneornil(L, 4) || lua_toboolean(L, 4));
    return 0;
}
int ePlayLayer(lua_State* L) {   // e:playLayer(layer, clip, weight, loop)
    Entity& e = checkEntity(L, 1);
    checkAnimator(L, e).playLayer(luaL_checkstring(L, 2), luaL_checkstring(L, 3), (float)luaL_optnumber(L, 4, 1.0),
                                  lua_isnoneornil(L, 5) || lua_toboolean(L, 5));
    return 0;
}
int eStopLayer(lua_State* L) {
    Entity& e = checkEntity(L, 1);
    checkAnimator(L, e).stopLayer(luaL_checkstring(L, 2));
    return 0;
}
// e:blackboard(key): an AI brain's value (positions as vec3); e:setBlackboard(key, value): number, boolean, text
// or vec3 (nil removes it)
int eBlackboard(lua_State* L) {
    Entity& e = checkEntity(L, 1);
    if (!e.brain) return luaL_error(L, "'%s' has no AI brain", e.name.c_str());
    const json::Value& v = e.brain->blackboard[luaL_checkstring(L, 2)];
    if (v.isArray() && v.size() == 3) pushVec3(L, {v[0].asFloat(), v[1].asFloat(), v[2].asFloat()});
    else pushJson(L, v.isNull() ? nullptr : &v);
    return 1;
}
int eSetBlackboard(lua_State* L) {
    Entity& e = checkEntity(L, 1);
    if (!e.brain) return luaL_error(L, "'%s' has no AI brain", e.name.c_str());
    std::string key = luaL_checkstring(L, 2);
    json::Value& board = e.brain->blackboard;
    if (lua_isnoneornil(L, 3)) {
        auto& m = board.members();
        m.erase(std::remove_if(m.begin(), m.end(), [&](const auto& kv) { return kv.first == key; }), m.end());
    } else if (lua_isboolean(L, 3)) {
        board.set(key, json::Value((bool)lua_toboolean(L, 3)));
    } else if (lua_type(L, 3) == LUA_TNUMBER) {
        board.set(key, json::Value(lua_tonumber(L, 3)));
    } else if (lua_istable(L, 3)) {
        Vec3 p = toVec3(L, 3);
        json::Value a = json::Value::array();
        a.push(p.x);
        a.push(p.y);
        a.push(p.z);
        board.set(key, a);
    } else {
        board.set(key, json::Value(std::string(luaL_checkstring(L, 3))));
    }
    return 0;
}
int eSetIK(lua_State* L) {   // e:setIK(chain, point, weight): true if the animator has that chain
    Entity& e = checkEntity(L, 1);
    lua_pushboolean(L, checkAnimator(L, e).setIK(luaL_checkstring(L, 2), toVec3(L, 3), (float)luaL_optnumber(L, 4, 1.0)));
    return 1;
}
int eScript(lua_State* L) {   // e:script("Door"): that Lua behaviour's instance (its members and functions)
    Entity& e = checkEntity(L, 1);
    auto* b = dynamic_cast<LuaBehaviour*>(findBehaviour(e, luaL_checkstring(L, 2)));
    if (b) b->pushInstance(L);
    else lua_pushnil(L);
    return 1;
}
int eGetField(lua_State* L) {   // e:getField("Spinner", "speed"): a field of any behaviour on it, C++ or Lua
    Entity& e = checkEntity(L, 1);
    Behaviour* b = findBehaviour(e, luaL_checkstring(L, 2));
    std::string name = luaL_checkstring(L, 3);
    if (auto* lb = dynamic_cast<LuaBehaviour*>(b)) {   // (its live value)
        lb->pushInstance(L);
        if (!lua_isnil(L, -1)) {
            lua_getfield(L, -1, name.c_str());
            return 1;
        }
        lua_pop(L, 1);
    }
    if (!b) return 0;
    LuaFieldAccess get(L, name, 0);
    b->fields(get);
    return get.found ? 1 : 0;
}
int eSetField(lua_State* L) {   // e:setField("Spinner", "speed", 3): true if there's such a field
    Entity& e = checkEntity(L, 1);
    Behaviour* b = findBehaviour(e, luaL_checkstring(L, 2));
    std::string name = luaL_checkstring(L, 3);
    luaL_checkany(L, 4);
    bool found = false;
    if (b) {
        LuaFieldAccess set(L, name, 4);
        b->fields(set);   // (a Lua behaviour passes it on to its instance before its next callback)
        found = set.found;
        if (auto* lb = dynamic_cast<LuaBehaviour*>(b)) {   // and right away, if it's running
            lb->pushInstance(L);
            if (!lua_isnil(L, -1)) {
                lua_pushvalue(L, 4);
                lua_setfield(L, -2, name.c_str());
            }
            lua_pop(L, 1);
        }
    }
    lua_pushboolean(L, found);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// game.*
// ---------------------------------------------------------------------------------------------
int gTime(lua_State* L) { lua_pushnumber(L, context(L).time()); return 1; }
int gDt(lua_State* L) { lua_pushnumber(L, context(L).dt()); return 1; }
int gFind(lua_State* L) {
    Entity* e = toEntity(L, 1);
    pushEntity(L, e ? e->id : kNoEntity);
    return 1;
}
void pushIdList(lua_State* L, const std::vector<EntityId>& ids) {
    lua_createtable(L, (int)ids.size(), 0);
    for (size_t i = 0; i < ids.size(); ++i) {
        pushEntity(L, ids[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
}
int gFindWithTag(lua_State* L) {
    pushIdList(L, context(L).findWithTag(luaL_checkstring(L, 1)));
    return 1;
}
int gSpawn(lua_State* L) {   // game.spawn(prefab, position, forward, parent)
    GameContext& ctx = context(L);
    std::string prefab = luaL_checkstring(L, 1);
    Vec3 pos = optVec3(L, 2, {}), fwd = optVec3(L, 3, {0, 0, -1});
    EntityId parent = lua_isnoneornil(L, 4) ? kNoEntity : toEntityId(L, 4);
    pushEntity(L, ctx.spawn(prefab, pos, fwd, parent));
    return 1;
}
int gLog(lua_State* L) {   // like print
    std::string text;
    for (int i = 1; i <= lua_gettop(L); ++i) {
        if (i > 1) text += ' ';
        text += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    if (eng().ctx) eng().ctx->log(text);
    else {
        std::printf("[lua] %s\n", text.c_str());
        std::fflush(stdout);
    }
    return 0;
}
uint32_t layerMaskArg(lua_State* L, int idx) {   // a list of layer names (or nothing: every layer)
    if (lua_isnoneornil(L, idx)) return ~0u;
    luaL_checktype(L, idx, LUA_TTABLE);
    uint32_t mask = 0;
    const Scene& scene = context(L).scene();
    for (lua_Integer i = 1;; ++i) {
        lua_rawgeti(L, idx, i);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        if (int l = scene.layers.find(luaL_checkstring(L, -1)); l >= 0) mask |= 1u << l;
        lua_pop(L, 1);
    }
    return mask;
}
int gRaycast(lua_State* L) {   // game.raycast(origin, dir, maxDistance, ignore, layers) -> hit or nil
    GameContext& ctx = context(L);
    Vec3 origin = toVec3(L, 1), dir = toVec3(L, 2);
    float maxDist = (float)luaL_optnumber(L, 3, 1000.0);
    EntityId ignore = lua_isnoneornil(L, 4) ? kNoEntity : toEntityId(L, 4);
    RaycastHit hit;
    if (!ctx.raycast(origin, normalize(dir), maxDist, hit, ignore, layerMaskArg(L, 5))) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    pushEntity(L, hit.entity);
    lua_setfield(L, -2, "entity");
    pushVec3(L, hit.point);
    lua_setfield(L, -2, "point");
    pushVec3(L, hit.normal);
    lua_setfield(L, -2, "normal");
    lua_pushnumber(L, hit.distance);
    lua_setfield(L, -2, "distance");
    return 1;
}
int gOverlapSphere(lua_State* L) {
    GameContext& ctx = context(L);
    pushIdList(L, ctx.overlapSphere(toVec3(L, 1), (float)luaL_checknumber(L, 2), layerMaskArg(L, 3)));
    return 1;
}
int gFindPath(lua_State* L) {   // game.findPath(from, to) -> list of points, or nil
    std::vector<Vec3> path;
    if (!context(L).findPath(toVec3(L, 1), toVec3(L, 2), path)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, (int)path.size(), 0);
    for (size_t i = 0; i < path.size(); ++i) {
        pushVec3(L, path[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

// A Lua function kept for a timer (released with it)
struct LuaFunction {
    int ref = LUA_NOREF;
    uint64_t stateId = 0;
    ~LuaFunction() {
        if (ref != LUA_NOREF && eng().L && stateId == eng().stateId) luaL_unref(eng().L, LUA_REGISTRYINDEX, ref);
    }
};
std::function<void(GameContext&)> timerCallback(lua_State* L, int idx) {
    luaL_checktype(L, idx, LUA_TFUNCTION);
    auto fn = std::make_shared<LuaFunction>();
    lua_pushvalue(L, idx);
    fn->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    fn->stateId = eng().stateId;
    EntityId owner = eng().self;
    return [fn, owner](GameContext& ctx) {
        Engine& e = eng();
        if (!e.L || fn->stateId != e.stateId) return;
        CallScope scope(&ctx, nullptr, owner);
        lua_rawgeti(e.L, LUA_REGISTRYINDEX, fn->ref);
        std::string error;
        if (!pcall(e.L, 0, 0, error)) reportRuntimeError("timer", error);
    };
}
int gAfter(lua_State* L) {   // game.after(seconds, fn) -> timer id (dropped with the entity whose script set it)
    GameContext& ctx = context(L);
    float s = (float)luaL_checknumber(L, 1);
    lua_pushinteger(L, ctx.after(s, timerCallback(L, 2), eng().self));
    return 1;
}
int gEvery(lua_State* L) {
    GameContext& ctx = context(L);
    float s = (float)luaL_checknumber(L, 1);
    lua_pushinteger(L, ctx.every(s, timerCallback(L, 2), eng().self));
    return 1;
}
int gCancel(lua_State* L) {
    context(L).cancel((GameContext::TimerId)luaL_checkinteger(L, 1));
    return 0;
}
int gSend(lua_State* L) {   // game.send(entity, name, value, text)
    EntityId id = toEntityId(L, 1);
    if (id != kNoEntity) context(L).send(id, toMessage(L, 2));
    return 0;
}
int gSendToTag(lua_State* L) {
    std::string tag = luaL_checkstring(L, 1);
    context(L).sendToTag(tag, toMessage(L, 2));
    return 0;
}
int gBroadcast(lua_State* L) {
    context(L).broadcast(toMessage(L, 1));
    return 0;
}
int gDestroy(lua_State* L) { return eDestroy(L); }
int gPlaySound(lua_State* L) {
    context(L).playSound(luaL_checkstring(L, 1), toVec3(L, 2), (float)luaL_optnumber(L, 3, 1.0));
    return 0;
}
int gPlaySound2D(lua_State* L) {
    context(L).playSound2D(luaL_checkstring(L, 1), (float)luaL_optnumber(L, 2, 1.0));
    return 0;
}
int gPlayMusic(lua_State* L) {
    context(L).playMusic(luaL_checkstring(L, 1), (float)luaL_optnumber(L, 2, 1.0), (float)luaL_optnumber(L, 3, 1.0));
    return 0;
}
// game.playMusicLayers({"a.ogg", "b.ogg"}, {1, 0}, fade, bpm, beatsPerBar)
int gPlayMusicLayers(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<std::string> clips;
    std::vector<float> volumes;
    for (lua_Integer i = 1;; ++i) {
        if (lua_rawgeti(L, 1, i) != LUA_TSTRING) { lua_pop(L, 1); break; }
        clips.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    if (lua_istable(L, 2))
        for (lua_Integer i = 1; i <= (lua_Integer)clips.size(); ++i) {
            lua_rawgeti(L, 2, i);
            volumes.push_back(lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 1.f);
            lua_pop(L, 1);
        }
    context(L).playMusicLayers(clips, volumes, (float)luaL_optnumber(L, 3, 1.0), (float)luaL_optnumber(L, 4, 0.0),
                               (int)luaL_optinteger(L, 5, 4));
    return 0;
}
// game.setMusicLayer(index, volume, fade): index from 1 (the first clip)
int gSetMusicLayer(lua_State* L) {
    context(L).setMusicLayer((int)luaL_checkinteger(L, 1) - 1, (float)luaL_checknumber(L, 2), (float)luaL_optnumber(L, 3, 1.0));
    return 0;
}
// game.playStinger(clip, "beat" | "bar" | "now", volume)
int gPlayStinger(lua_State* L) {
    context(L).playStinger(luaL_checkstring(L, 1), luaL_optstring(L, 2, "beat"), (float)luaL_optnumber(L, 3, 1.0));
    return 0;
}
int gStopMusic(lua_State* L) {
    context(L).stopMusic((float)luaL_optnumber(L, 1, 1.0));
    return 0;
}
int gSetBusVolume(lua_State* L) {
    context(L).setBusVolume(luaL_checkstring(L, 1), (float)luaL_checknumber(L, 2));
    return 0;
}
int gDuck(lua_State* L) {
    context(L).duck(luaL_checkstring(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3));
    return 0;
}
int gText(lua_State* L) {
    lua_pushstring(L, context(L).text(luaL_checkstring(L, 1), luaL_optstring(L, 2, "")).c_str());
    return 1;
}
int gFormat(lua_State* L) {
    lua_pushstring(L, context(L).format(luaL_checkstring(L, 1)).c_str());
    return 1;
}
int gSetLanguage(lua_State* L) {
    context(L).setLanguage(luaL_checkstring(L, 1));
    return 0;
}
int gLanguage(lua_State* L) {
    lua_pushstring(L, context(L).language().c_str());
    return 1;
}
int gLoadScene(lua_State* L) {
    context(L).loadScene(luaL_checkstring(L, 1));
    return 0;
}
int gScenePath(lua_State* L) {
    lua_pushstring(L, context(L).scenePath().c_str());
    return 1;
}
int gQuit(lua_State* L) {
    context(L).quit();
    return 0;
}
int gSaveGame(lua_State* L) {
    context(L).saveGame(luaL_optstring(L, 1, "quicksave"));
    return 0;
}
int gLoadGame(lua_State* L) {
    context(L).loadGame(luaL_optstring(L, 1, "quicksave"));
    return 0;
}
int gHasSaveGame(lua_State* L) {
    lua_pushboolean(L, context(L).hasSaveGame(luaL_optstring(L, 1, "quicksave")));
    return 1;
}
int gSetPaused(lua_State* L) {
    context(L).setPaused(lua_toboolean(L, 1));
    return 0;
}
int gPaused(lua_State* L) {
    lua_pushboolean(L, context(L).paused());
    return 1;
}
int gEmitParticles(lua_State* L) { return eEmitParticles(L); }
int gSetRagdoll(lua_State* L) { return eSetRagdoll(L); }

// game.state.key: GameState (numbers, strings, booleans; nil erases)
int stateIndex(lua_State* L) {
    pushJson(L, context(L).state().get(luaL_checkstring(L, 2)));
    return 1;
}
int stateNewIndex(lua_State* L) {
    GameState& s = context(L).state();
    std::string key = luaL_checkstring(L, 2);
    switch (lua_type(L, 3)) {
    case LUA_TNIL: s.erase(key); break;
    case LUA_TNUMBER: s.setNumber(key, (float)lua_tonumber(L, 3)); break;
    case LUA_TBOOLEAN: s.setFlag(key, lua_toboolean(L, 3)); break;
    case LUA_TSTRING: s.setText(key, lua_tostring(L, 3)); break;
    default: return luaL_error(L, "game.state.%s: numbers, strings and booleans only", key.c_str());
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// input.*
// ---------------------------------------------------------------------------------------------
int iDown(lua_State* L) { lua_pushboolean(L, context(L).input().down(luaL_checkstring(L, 1))); return 1; }
int iPressed(lua_State* L) { lua_pushboolean(L, context(L).input().pressed(luaL_checkstring(L, 1))); return 1; }
int iReleased(lua_State* L) { lua_pushboolean(L, context(L).input().released(luaL_checkstring(L, 1))); return 1; }
int iPressedThisFrame(lua_State* L) {
    lua_pushboolean(L, context(L).input().pressedThisFrame(luaL_checkstring(L, 1)));
    return 1;
}
int iValue(lua_State* L) { lua_pushnumber(L, context(L).input().value(luaL_checkstring(L, 1))); return 1; }
int iAxis(lua_State* L) {
    lua_pushnumber(L, context(L).input().axis(luaL_checkstring(L, 1), luaL_checkstring(L, 2)));
    return 1;
}
int iMouseDelta(lua_State* L) {
    ImVec2 d = context(L).input().mouseDelta();
    lua_pushnumber(L, d.x);
    lua_pushnumber(L, d.y);
    return 2;
}

// ---------------------------------------------------------------------------------------------
// ui.* (inside drawUI)
// ---------------------------------------------------------------------------------------------
GameUI& gameUi(lua_State* L) {
    if (!eng().ui) luaL_error(L, "ui functions work only in drawUI");
    return *eng().ui;
}
GameUI::Align toAlign(lua_State* L, int idx) {
    std::string a = luaL_optstring(L, idx, "left");
    return a == "center" ? GameUI::Align::Center : a == "right" ? GameUI::Align::Right : GameUI::Align::Left;
}
float num(lua_State* L, int idx) { return (float)luaL_checknumber(L, idx); }
int uWidth(lua_State* L) { lua_pushnumber(L, gameUi(L).width()); return 1; }
int uHeight(lua_State* L) { lua_pushnumber(L, gameUi(L).height()); return 1; }
int uText(lua_State* L) {   // ui.text(x, y, text, size, color, align, font)
    gameUi(L).text(num(L, 1), num(L, 2), luaL_checkstring(L, 3), (float)luaL_optnumber(L, 4, 36.0), toColor(L, 5, {}),
                   toAlign(L, 6), luaL_optstring(L, 7, ""));
    return 0;
}
int uTextWidth(lua_State* L) {
    lua_pushnumber(L, gameUi(L).textWidth(luaL_checkstring(L, 1), (float)luaL_optnumber(L, 2, 36.0), luaL_optstring(L, 3, "")));
    return 1;
}
int uRect(lua_State* L) {
    gameUi(L).rect(num(L, 1), num(L, 2), num(L, 3), num(L, 4), toColor(L, 5, {}), (float)luaL_optnumber(L, 6, 0.0));
    return 0;
}
int uBorder(lua_State* L) {
    gameUi(L).border(num(L, 1), num(L, 2), num(L, 3), num(L, 4), toColor(L, 5, {}), (float)luaL_optnumber(L, 6, 3.0),
                     (float)luaL_optnumber(L, 7, 0.0));
    return 0;
}
int uBar(lua_State* L) {
    gameUi(L).bar(num(L, 1), num(L, 2), num(L, 3), num(L, 4), num(L, 5), toColor(L, 6, {}),
                  toColor(L, 7, {0.f, 0.f, 0.f, 0.5f}));
    return 0;
}
int uImage(lua_State* L) {
    gameUi(L).image(luaL_checkstring(L, 1), num(L, 2), num(L, 3), num(L, 4), num(L, 5), toColor(L, 6, {}));
    return 0;
}
int uButton(lua_State* L) {
    lua_pushboolean(L, gameUi(L).button(num(L, 1), num(L, 2), num(L, 3), num(L, 4), luaL_checkstring(L, 5),
                                        (float)luaL_optnumber(L, 6, 36.0)));
    return 1;
}
int uHovered(lua_State* L) {
    lua_pushboolean(L, gameUi(L).hovered(num(L, 1), num(L, 2), num(L, 3), num(L, 4)));
    return 1;
}
int uShowCursor(lua_State* L) {
    gameUi(L).showCursor();
    return 0;
}

// ---------------------------------------------------------------------------------------------
// The state
// ---------------------------------------------------------------------------------------------
// vec3 and color, in Lua (the metatable comes in as the chunk's argument)
const char* kPrelude = R"lua(
local V = ...
V.__index = V
local function new(x, y, z) return setmetatable({x = x or 0, y = y or 0, z = z or 0}, V) end
vec3 = setmetatable({new = new}, {__call = function(_, x, y, z) return new(x, y, z) end})
function V.__add(a, b) return new(a.x + b.x, a.y + b.y, a.z + b.z) end
function V.__sub(a, b) return new(a.x - b.x, a.y - b.y, a.z - b.z) end
function V.__unm(a) return new(-a.x, -a.y, -a.z) end
function V.__mul(a, b)
    if type(a) == "number" then return new(a * b.x, a * b.y, a * b.z) end
    if type(b) == "number" then return new(a.x * b, a.y * b, a.z * b) end
    return new(a.x * b.x, a.y * b.y, a.z * b.z)
end
function V.__div(a, b)
    if type(b) == "number" then return new(a.x / b, a.y / b, a.z / b) end
    return new(a.x / b.x, a.y / b.y, a.z / b.z)
end
function V.__eq(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end
function V.__tostring(a) return string.format("(%g, %g, %g)", a.x, a.y, a.z) end
function V.length(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end
function V.normalized(a)
    local l = V.length(a)
    if l > 1e-9 then return new(a.x / l, a.y / l, a.z / l) end
    return new(0, 0, 0)
end
function V.copy(a) return new(a.x, a.y, a.z) end
function V.dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
function V.cross(a, b) return new(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x) end
function V.lerp(a, b, t) return new(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t) end
function V.distance(a, b) return V.length(V.__sub(a, b)) end
vec3.dot, vec3.cross, vec3.lerp, vec3.distance, vec3.length = V.dot, V.cross, V.lerp, V.distance, V.length
function color(r, g, b, a) return {r = r or 1, g = g or 1, b = b or 1, a = a or 1} end
)lua";

// require("lib.util"): scripts/lib/util.lua (helper code shared by scripts; not a behaviour type), loaded once
int luaRequire(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "vkw.loaded");
    lua_getfield(L, -1, name.c_str());
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    std::string rel = name;
    std::replace(rel.begin(), rel.end(), '.', '/');
    fs::path file = eng().root / "scripts" / fs::u8path(rel + ".lua");
    std::string source;
    if (!vfs::read(file, source)) return luaL_error(L, "require: no scripts/%s.lua", rel.c_str());
    std::string chunk = "@scripts/" + rel + ".lua";
    if (luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t") != LUA_OK) return lua_error(L);
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name.c_str());
    return 1;
}

void setFunctions(lua_State* L, const luaL_Reg* fns) { luaL_setfuncs(L, fns, 0); }

lua_State* state() {
    Engine& e = eng();
    if (e.L) return e.L;
    lua_State* L = luaL_newstate();
    e.L = L;
    // The safe libraries only: no io, os, debug or loading files
    const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},     {LUA_TABLIBNAME, luaopen_table},
                             {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
                             {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine}};
    for (const luaL_Reg& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    for (const char* unsafe : {"dofile", "loadfile"}) {
        lua_pushnil(L);
        lua_setglobal(L, unsafe);
    }
    lua_pushcfunction(L, gLog);
    lua_setglobal(L, "print");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "vkw.loaded");
    lua_pushcfunction(L, luaRequire);
    lua_setglobal(L, "require");

    // vec3
    luaL_newmetatable(L, kVec3Meta);
    std::string error;
    if (luaL_loadbufferx(L, kPrelude, std::strlen(kPrelude), "=prelude", "t") != LUA_OK) {
        addError(lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        lua_insert(L, -2);   // chunk, metatable
        if (!pcall(L, 1, 0, error)) addError("prelude: " + error);
    }

    // Entities
    luaL_newmetatable(L, kEntityMeta);
    const luaL_Reg meta[] = {{"__index", entityIndex}, {"__newindex", entityNewIndex}, {"__eq", entityEq},
                             {"__tostring", entityToString}, {nullptr, nullptr}};
    setFunctions(L, meta);
    lua_newtable(L);
    const luaL_Reg methods[] = {{"hasTag", eHasTag},           {"addTag", eAddTag},
                                {"removeTag", eRemoveTag},     {"destroy", eDestroy},
                                {"send", eSend},               {"moveCharacter", eMoveCharacter},
                                {"lookAt", eLookAt},           {"emitParticles", eEmitParticles},
                                {"playSound", ePlaySound},     {"setRagdoll", eSetRagdoll},
                                {"script", eScript},           {"getField", eGetField},
                                {"setField", eSetField},       {"setAnimParameter", eSetAnimParameter},
                                {"playAnimation", ePlayAnimation}, {"playLayer", ePlayLayer},
                                {"stopLayer", eStopLayer},     {"setIK", eSetIK}, {"blackboard", eBlackboard}, {"setBlackboard", eSetBlackboard},
                                {nullptr, nullptr}};
    setFunctions(L, methods);
    lua_setfield(L, -2, "methods");
    lua_pop(L, 1);

    // game
    lua_newtable(L);
    const luaL_Reg game[] = {
        {"time", gTime},           {"dt", gDt},                   {"find", gFind},
        {"findWithTag", gFindWithTag}, {"spawn", gSpawn},         {"destroy", gDestroy},
        {"log", gLog},             {"raycast", gRaycast},         {"overlapSphere", gOverlapSphere},
        {"findPath", gFindPath},   {"after", gAfter},             {"every", gEvery},
        {"cancel", gCancel},       {"send", gSend},               {"sendToTag", gSendToTag},
        {"broadcast", gBroadcast}, {"playSound", gPlaySound},     {"playSound2D", gPlaySound2D},
        {"playMusic", gPlayMusic}, {"stopMusic", gStopMusic},     {"setBusVolume", gSetBusVolume},
        {"playMusicLayers", gPlayMusicLayers}, {"setMusicLayer", gSetMusicLayer}, {"playStinger", gPlayStinger},
        {"duck", gDuck},           {"text", gText},               {"format", gFormat},
        {"setLanguage", gSetLanguage}, {"language", gLanguage},   {"loadScene", gLoadScene},
        {"scenePath", gScenePath}, {"quit", gQuit},               {"saveGame", gSaveGame},
        {"loadGame", gLoadGame},   {"hasSaveGame", gHasSaveGame}, {"setPaused", gSetPaused},
        {"paused", gPaused},       {"emitParticles", gEmitParticles}, {"setRagdoll", gSetRagdoll},
        {nullptr, nullptr}};
    setFunctions(L, game);
    lua_newtable(L);   // game.state
    lua_newtable(L);
    lua_pushcfunction(L, stateIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, stateNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "state");
    lua_setglobal(L, "game");

    // input
    lua_newtable(L);
    const luaL_Reg input[] = {{"down", iDown},   {"pressed", iPressed}, {"released", iReleased},
                              {"pressedThisFrame", iPressedThisFrame}, {"value", iValue},
                              {"axis", iAxis},   {"mouseDelta", iMouseDelta}, {nullptr, nullptr}};
    setFunctions(L, input);
    lua_setglobal(L, "input");

    // ui
    lua_newtable(L);
    const luaL_Reg ui[] = {{"width", uWidth},   {"height", uHeight},   {"text", uText},     {"textWidth", uTextWidth},
                           {"rect", uRect},     {"border", uBorder},   {"bar", uBar},       {"image", uImage},
                           {"button", uButton}, {"hovered", uHovered}, {"showCursor", uShowCursor},
                           {nullptr, nullptr}};
    setFunctions(L, ui);
    lua_setglobal(L, "ui");
    return L;
}

// ---------------------------------------------------------------------------------------------
// Loading scripts
// ---------------------------------------------------------------------------------------------
bool parseRef(const std::string& s, FieldRef& out) {
    for (FieldRef r : {FieldRef::Entity, FieldRef::Prefab, FieldRef::Scene, FieldRef::Sound, FieldRef::AnimationClip,
                       FieldRef::InputAction})
        if (s == fieldRefName(r)) {
            out = r;
            return true;
        }
    return false;
}

// The returned table's `fields` (at the top of the stack) -> declarations
bool parseFields(lua_State* L, std::vector<FieldDecl>& out, std::string& error) {
    int list = lua_gettop(L);
    if (lua_isnil(L, list)) return true;
    if (!lua_istable(L, list)) { error = "'fields' must be a list"; return false; }
    for (lua_Integer i = 1;; ++i) {
        lua_rawgeti(L, list, i);
        int f = lua_gettop(L);
        if (lua_isnil(L, f)) {
            lua_pop(L, 1);
            break;
        }
        FieldDecl d;
        if (!lua_istable(L, f)) { error = "fields[" + std::to_string(i) + "]: expected {\"name\", default, ...}"; return false; }
        lua_rawgeti(L, f, 1);
        if (lua_type(L, -1) != LUA_TSTRING) { error = "fields[" + std::to_string(i) + "]: the first item is the name"; return false; }
        d.name = lua_tostring(L, -1);
        lua_pop(L, 1);
        std::string where = "field '" + d.name + "': ";
        lua_rawgeti(L, f, 2);
        int t = lua_type(L, -1);
        if (t == LUA_TNUMBER) {
            d.kind = FieldDecl::Kind::Number;
            d.number = (float)lua_tonumber(L, -1);
        } else if (t == LUA_TBOOLEAN) {
            d.kind = FieldDecl::Kind::Bool;
            d.flag = lua_toboolean(L, -1);
        } else if (t == LUA_TSTRING) {
            d.kind = FieldDecl::Kind::String;
            d.text = lua_tostring(L, -1);
        } else if (t == LUA_TTABLE && isVec3(L, -1)) {
            d.kind = FieldDecl::Kind::Vec3;
            d.vec = toVec3(L, -1);
        } else {
            error = where + "the default must be a number, a boolean, a string or a vec3";
            return false;
        }
        lua_pop(L, 1);
        auto number = [&](const char* key, float& v) {
            lua_getfield(L, f, key);
            if (lua_isnumber(L, -1)) v = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        };
        number("min", d.min);
        number("max", d.max);
        number("speed", d.speed);
        lua_getfield(L, f, "int");
        if (lua_toboolean(L, -1) && d.kind == FieldDecl::Kind::Number) {
            d.kind = FieldDecl::Kind::Int;
            if (d.speed == 0.05f) d.speed = 1.f;
        }
        lua_pop(L, 1);
        lua_getfield(L, f, "ref");
        if (lua_type(L, -1) == LUA_TSTRING) {
            if (!parseRef(lua_tostring(L, -1), d.ref)) {
                error = where + "ref must be entity, prefab, scene, sound, clip or action";
                return false;
            }
            d.isRef = d.kind == FieldDecl::Kind::String;
        }
        lua_pop(L, 1);
        lua_getfield(L, f, "options");
        if (lua_istable(L, -1) && d.kind == FieldDecl::Kind::String) {
            for (lua_Integer k = 1;; ++k) {
                lua_rawgeti(L, -1, k);
                if (lua_type(L, -1) != LUA_TSTRING) {
                    lua_pop(L, 1);
                    break;
                }
                d.options.push_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            if (!d.options.empty()) {
                d.kind = FieldDecl::Kind::Choice;
                if (std::find(d.options.begin(), d.options.end(), d.text) == d.options.end()) d.text = d.options[0];
            }
        }
        lua_pop(L, 1);
        if (d.name == "entity") { error = "a field can't be called 'entity' (self.entity is the entity)"; return false; }
        for (const FieldDecl& o : out)
            if (o.name == d.name) { error = where + "declared twice"; return false; }
        out.push_back(std::move(d));
        lua_pop(L, 1);   // the field's table
    }
    lua_pop(L, 1);   // the list
    return true;
}

// Runs the script and makes (or refills) its type's class table
bool compile(ScriptType& t, const std::string& source, std::string& error) {
    lua_State* L = state();
    int top = lua_gettop(L);
    std::string chunk = "@" + t.chunkName;
    if (luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t") != LUA_OK) {
        error = lua_tostring(L, -1);
        lua_settop(L, top);
        return false;
    }
    CallScope scope(nullptr, nullptr, kNoEntity);   // (no game functions while loading)
    if (!pcall(L, 0, 1, error)) {
        lua_settop(L, top);
        return false;
    }
    if (!lua_istable(L, -1)) {
        error = t.chunkName + ": the script must end with `return` and its behaviour's table";
        lua_settop(L, top);
        return false;
    }
    int cls = lua_gettop(L);
    std::vector<FieldDecl> fields;
    lua_getfield(L, cls, "fields");
    if (!parseFields(L, fields, error)) {
        error = t.chunkName + ": " + error;
        lua_settop(L, top);
        return false;
    }
    lua_getfield(L, cls, "description");
    std::string description = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "Lua script (" + t.chunkName + ")";
    lua_pop(L, 1);

    if (t.classRef == LUA_NOREF) {
        lua_pushvalue(L, cls);
        lua_setfield(L, cls, "__index");
        lua_pushvalue(L, cls);
        t.classRef = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {   // a reload: the same table with the new contents, so running instances (its metatable) see them
        lua_rawgeti(L, LUA_REGISTRYINDEX, t.classRef);
        int old = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, old)) {   // clear it (clearing fields while traversing is allowed)
            lua_pop(L, 1);
            lua_pushvalue(L, -1);
            lua_pushnil(L);
            lua_rawset(L, old);
        }
        lua_pushnil(L);
        while (lua_next(L, cls)) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_rawset(L, old);
        }
        lua_pushvalue(L, old);
        lua_setfield(L, old, "__index");
    }
    lua_settop(L, top);
    {
        std::lock_guard<std::mutex> lock(eng().typesMutex);
        t.fields = std::make_shared<std::vector<FieldDecl>>(std::move(fields));
        t.description = description;
        ++t.generation;
    }
    t.failed = false;
    return true;
}

// Loads (or reloads) a type from its source; registers it the first time
bool load(const std::string& name, const fs::path& file, const std::string& chunkName, const std::string& source,
          std::string& error) {
    Engine& e = eng();
    std::string module;
    if (behaviourTypeModule(name, module) && module != "lua") {
        error = chunkName + ": a C++ behaviour is already called " + name + " (rename the script)";
        return false;
    }
    ScriptType* t;
    bool fresh;
    {
        std::lock_guard<std::mutex> lock(e.typesMutex);
        fresh = !e.types.count(name);
        t = &e.types[name];
        t->name = name;
        t->file = file;
        t->chunkName = chunkName;
    }
    std::error_code ec;
    if (!file.empty()) t->mtime = fs::last_write_time(file, ec);
    if (!compile(*t, source, error)) {
        t->failed = true;
        if (fresh && t->classRef == LUA_NOREF) {   // never loaded: not a type yet
            std::lock_guard<std::mutex> lock(e.typesMutex);
            e.types.erase(name);
        }
        return false;
    }
    setRegisteringModule("lua");
    registerBehaviourType(name.c_str(), t->description.c_str(),
                          [name]() -> std::unique_ptr<Behaviour> { return std::make_unique<LuaBehaviour>(name); });
    setRegisteringModule("");
    return true;
}

bool isLuaName(const std::string& s) {
    if (s.empty() || std::isdigit((unsigned char)s[0])) return false;
    for (char c : s)
        if (!std::isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

// The behaviour scripts in the project: scripts/**.lua but not scripts/lib/ (require()d helpers)
std::vector<fs::path> scriptFiles(const fs::path& root) {
    std::vector<fs::path> out;
    fs::path dir = root / "scripts";
    for (const fs::path& p : vfs::list(dir, ".lua")) {
        fs::path rel = p.lexically_relative(dir);
        if (!rel.empty() && rel.begin()->u8string() == "lib") continue;
        out.push_back(p);
    }
    return out;
}

bool loadFile(const fs::path& file, std::vector<std::string>& loaded) {
    Engine& e = eng();
    std::string name = file.stem().u8string(), source, error;
    std::string chunkName = "scripts/" + file.lexically_relative(e.root / "scripts").generic_u8string();
    if (!isLuaName(name)) {
        addError(chunkName + ": the file name must be a valid name (letters, digits, _): it's the behaviour's");
        return false;
    }
    if (!vfs::read(file, source)) {
        addError(chunkName + ": can't read it");
        return false;
    }
    if (!load(name, file, chunkName, source, error)) {
        addError(error);
        // (remember the time, so it's tried again only when it changes)
        std::lock_guard<std::mutex> lock(e.typesMutex);
        auto it = e.types.find(name);
        if (it == e.types.end()) {
            ScriptType& t = e.types[name];   // a placeholder (no class): not registered, retried on change
            t.name = name;
            t.file = file;
            t.chunkName = chunkName;
            t.failed = true;
            std::error_code ec;
            t.mtime = fs::last_write_time(file, ec);
        }
        return false;
    }
    loaded.push_back(name);
    return true;
}

}  // namespace

namespace scripting {

void reset() {
    Engine& e = eng();
    unregisterModuleBehaviours("lua");
    if (e.L) lua_close(e.L);
    e.L = nullptr;
    ++e.stateId;
    {
        std::lock_guard<std::mutex> lock(e.typesMutex);
        e.types.clear();
    }
    e.errors.clear();
    e.root.clear();
}

int loadProject(const fs::path& projectRoot) {
    reset();
    Engine& e = eng();
    e.root = projectRoot;
    std::vector<std::string> loaded;
    for (const fs::path& file : scriptFiles(projectRoot)) loadFile(file, loaded);
    e.lastPoll = std::chrono::steady_clock::now();
    return (int)loaded.size();
}

bool define(const std::string& typeName, const std::string& source, std::string& error) {
    if (!isLuaName(typeName)) {
        error = "not a valid name: " + typeName;
        return false;
    }
    if (!load(typeName, {}, typeName + ".lua", source, error)) return false;
    return true;
}

std::vector<std::string> poll(bool force) {
    Engine& e = eng();
    std::vector<std::string> loaded;
    auto now = std::chrono::steady_clock::now();
    if (e.root.empty() || (!force && now - e.lastPoll < std::chrono::milliseconds(500))) return loaded;
    e.lastPoll = now;
    std::vector<std::pair<std::string, fs::path>> known;
    {
        std::lock_guard<std::mutex> lock(e.typesMutex);
        for (const auto& [name, t] : e.types)
            if (!t.file.empty()) known.emplace_back(name, t.file);
    }
    std::error_code ec;
    for (const auto& [name, file] : known) {   // edited (only files on disk: a pack doesn't change)
        if (!fs::is_regular_file(file, ec)) continue;
        fs::file_time_type mtime = fs::last_write_time(file, ec);
        fs::file_time_type was;
        {
            std::lock_guard<std::mutex> lock(e.typesMutex);
            was = e.types[name].mtime;
        }
        if (ec || mtime == was) continue;
        if (std::chrono::abs(fs::file_time_type::clock::now() - mtime) < std::chrono::milliseconds(200)) continue;   // still being written
        loadFile(file, loaded);
        std::lock_guard<std::mutex> lock(e.typesMutex);
        e.types[name].mtime = mtime;
    }
    for (const fs::path& file : scriptFiles(e.root)) {   // new
        std::string name = file.stem().u8string();
        bool have;
        {
            std::lock_guard<std::mutex> lock(e.typesMutex);
            have = e.types.count(name) > 0;
        }
        if (!have) loadFile(file, loaded);
    }
    if (!loaded.empty())   // the errors left are those of scripts still broken
        e.errors.erase(std::remove_if(e.errors.begin(), e.errors.end(),
                                      [&](const std::string& err) {
                                          for (const std::string& n : loaded)
                                              if (err.find("/" + n + ".lua:") != std::string::npos ||
                                                  err.rfind(n + ".lua:", 0) == 0)
                                                  return true;
                                          return false;
                                      }),
                       e.errors.end());
    return loaded;
}

std::vector<std::string> errors() { return eng().errors; }

bool isScriptType(const std::string& typeName) {
    std::lock_guard<std::mutex> lock(eng().typesMutex);
    auto it = eng().types.find(typeName);
    return it != eng().types.end() && it->second.classRef != LUA_NOREF;
}

fs::path scriptFile(const std::string& typeName) {
    std::lock_guard<std::mutex> lock(eng().typesMutex);
    auto it = eng().types.find(typeName);
    return it == eng().types.end() ? fs::path() : it->second.file;
}

std::string templateSource(const std::string& typeName) {
    return "-- " + typeName + ": a behaviour in Lua (see README.md, \"Lua scripting\")\n"
           "local " + typeName + " = {\n"
           "    description = \"" + typeName + "\",\n"
           "    fields = {\n"
           "        {\"speed\", 1, min = 0, max = 10},\n"
           "    },\n"
           "}\n\n"
           "function " + typeName + ":start()\n"
           "end\n\n"
           "function " + typeName + ":update(dt)\n"
           "end\n\n"
           "return " + typeName + "\n";
}

}  // namespace scripting
