#include "game/behaviour_tree.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "game/game.h"

namespace ai {

const std::vector<NodeType>& nodeTypes() {
    static const std::vector<NodeType> types = {
        // Composites
        {"Selector", "Composite", -1, "Tries its children in order until one doesn't fail (earlier ones are checked again every update)", nullptr, nullptr, nullptr, nullptr},
        {"Sequence", "Composite", -1, "Runs its children in order while they succeed (its conditions are checked again while an action runs)", nullptr, nullptr, nullptr, nullptr},
        {"Parallel", "Composite", -1, "Runs all its children every update: fails when one fails, succeeds when all have", nullptr, nullptr, nullptr, nullptr},
        {"Random", "Composite", -1, "Runs one of its children, picked at random each time", nullptr, nullptr, nullptr, nullptr},
        // Decorators
        {"Inverter", "Decorator", 1, "Success becomes failure and failure success", nullptr, nullptr, nullptr, nullptr},
        {"Succeeder", "Decorator", 1, "Succeeds whatever its child answers (once it's done)", nullptr, nullptr, nullptr, nullptr},
        {"Repeat", "Decorator", 1, "Runs its child again each time it succeeds, so many times (0: for ever); fails if it fails", nullptr, nullptr, "Times", nullptr},
        {"UntilFail", "Decorator", 1, "Runs its child again until it fails, then succeeds", nullptr, nullptr, nullptr, nullptr},
        {"Cooldown", "Decorator", 1, "After its child finishes, fails for so many seconds before running it again", nullptr, nullptr, "Seconds", nullptr},
        {"Timeout", "Decorator", 1, "Fails if its child is still running after so many seconds", nullptr, nullptr, "Seconds", nullptr},
        // Conditions
        {"Has", "Condition", 0, "The blackboard value is set (and not false, 0 or empty)", "Key", nullptr, nullptr, nullptr},
        {"Compare", "Condition", 0, "The blackboard number compared with a number (op: < <= > >= == !=)", "Key", "Op", "Number", nullptr},
        {"CanSee", "Condition", 0, "It sees something it notices (perception: \"target\" is it)", nullptr, nullptr, nullptr, nullptr},
        {"Heard", "Condition", 0, "It heard something within the last so many seconds (\"noise\": where)", nullptr, nullptr, "Seconds", nullptr},
        {"Near", "Condition", 0, "The target is within this distance", "Target", nullptr, "Distance", nullptr},
        {"Chance", "Condition", 0, "Succeeds this often (0..1)", nullptr, nullptr, "Probability", nullptr},
        // Actions
        {"MoveTo", "Action", 0, "Walks to the target over the navigation (running until there)", "Target", nullptr, "Speed", "Stop distance"},
        {"Flee", "Action", 0, "Walks away from the target until this far from it", "Target", nullptr, "Speed", "Distance"},
        {"Patrol", "Action", 0, "Walks from waypoint to waypoint (entity names, comma separated), for ever", nullptr, "Waypoints", "Speed", "Pause"},
        {"Wait", "Action", 0, "Waits so many seconds (plus up to the extra, at random)", nullptr, nullptr, "Seconds", "Extra"},
        {"LookAt", "Action", 0, "Turns to face the target", "Target", nullptr, nullptr, nullptr},
        {"Set", "Action", 0, "Sets a blackboard value (a number, true / false, [x, y, z] or text)", "Key", "Value", nullptr, nullptr},
        {"Clear", "Action", 0, "Removes a blackboard value", "Key", nullptr, nullptr, nullptr},
        {"Message", "Action", 0, "Sends a message to the target (empty: to everyone)", "Target", "Message", "Value", nullptr},
        {"Sound", "Action", 0, "Plays a sound where it is (others' brains hear it)", nullptr, "Clip", "Volume", nullptr},
        {"Animate", "Action", 0, "Sets an animator parameter", "Parameter", nullptr, "Value", nullptr},
        {"Log", "Action", 0, "Writes to the game's log ({key} shows a blackboard value)", nullptr, "Text", nullptr, nullptr},
    };
    return types;
}

const NodeType* nodeType(const std::string& type) {
    for (const NodeType& t : nodeTypes())
        if (type == t.type) return &t;
    return nullptr;
}

json::Value parseValue(const std::string& text) {
    if (text == "true") return json::Value(true);
    if (text == "false") return json::Value(false);
    json::Value parsed;
    std::string error;
    if (!text.empty() && (std::isdigit((unsigned char)text[0]) || text[0] == '-' || text[0] == '[') &&
        json::parse(text, parsed, error))
        return parsed;
    return json::Value(text);
}

namespace {

bool truthy(const json::Value& v) {
    switch (v.type()) {
        case json::Value::Type::Null: return false;
        case json::Value::Type::Bool: return v.asBool();
        case json::Value::Type::Number: return v.asNumber() != 0.0;
        case json::Value::Type::String: return !v.asString().empty();
        default: return true;
    }
}

const json::Value& lookup(const AIBrain& brain, const std::string& key) {
    static const json::Value none;
    return key.empty() ? none : brain.blackboard[key];
}

Entity* targetEntity(GameContext& ctx, const AIBrain& brain, const std::string& key) {
    const json::Value& v = lookup(brain, key);
    if (v.type() == json::Value::Type::String) return ctx.find(v.asString());
    if (v.isNull() && !key.empty()) return ctx.find(key);
    return nullptr;
}

void placeAt(GameContext& ctx, Entity& self, Vec3 world) {
    Vec3 local = transformPoint(inverseAffine(ctx.scene().parentWorldMatrix(self.id)), world);
    self.transform.position[0] = local.x;
    self.transform.position[1] = local.y;
    self.transform.position[2] = local.z;
}

void face(GameContext& ctx, Entity& self, Vec3 direction) {
    direction.y = 0.f;
    if (length(direction) < 1e-4f) return;
    Vec3 pos = ctx.worldPosition(self.id);
    Mat4 world = lookRotation(pos, normalize(direction), self.transform.scale);
    Mat4 local = inverseAffine(ctx.scene().parentWorldMatrix(self.id)) * world;
    Vec3 at = self.transform.pos();
    self.transform.setMatrix(local);
    self.transform.position[0] = at.x;
    self.transform.position[1] = at.y;
    self.transform.position[2] = at.z;
}

// A step along a path to `goal` (new paths every half second); true once within `stop` of it
enum class Walk { Arrived, Walking, NoWay };
Walk walkTowards(GameContext& ctx, Entity& self, BrainState::Node& m, Vec3 goal, float speed, float stop) {
    Vec3 pos = ctx.worldPosition(self.id), flat = goal - pos;
    flat.y = 0.f;
    if (length(flat) <= stop) {
        m.path.clear();
        return Walk::Arrived;
    }
    m.repathIn -= ctx.dt();
    if (m.repathIn <= 0.f || m.next >= m.path.size()) {
        m.repathIn = 0.5f;
        m.next = 1;
        if (!ctx.findPath(pos, goal, m.path)) {
            m.path.clear();
            return Walk::NoWay;
        }
    }
    Vec3 to;
    float dist = 0.f;
    for (; m.next < m.path.size(); ++m.next) {   // (corners already reached)
        to = m.path[m.next] - pos;
        to.y = 0.f;
        dist = length(to);
        if (dist > 0.05f || m.next + 1 == m.path.size()) break;
    }
    if (m.next >= m.path.size() || dist < 1e-4f) return Walk::Walking;
    Vec3 delta = to * (std::min(std::max(speed, 0.f) * ctx.dt(), dist) / dist);
    if (self.collider) ctx.moveCharacter(self, delta);
    else placeAt(ctx, self, pos + delta);
    face(ctx, self, delta);
    return Walk::Walking;
}

std::string formatText(const AIBrain& brain, const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        size_t close = text[i] == '{' ? text.find('}', i) : std::string::npos;
        if (close == std::string::npos) {
            out += text[i];
            continue;
        }
        const json::Value& v = brain.blackboard[text.substr(i + 1, close - i - 1)];
        if (v.type() == json::Value::Type::String) out += v.asString();
        else if (!v.isNull()) out += json::write(v);
        i = close;
    }
    return out;
}

struct Runner {
    GameContext& ctx;
    Entity& self;
    AIBrain& brain;
    BrainState& state;
    int depth = 0;

    BrainState::Node& mem(int i) { return state.nodes[(size_t)i]; }
    bool isCondition(int i) {
        const NodeType* t = nodeType(brain.nodes[(size_t)i].type);
        return t && std::string(t->group) == "Condition";
    }
    void reset(int i) {   // (abandoned: starts afresh next time)
        if (i < 0 || i >= (int)brain.nodes.size()) return;
        BrainState::Node& m = mem(i);
        float cooldown = brain.nodes[(size_t)i].type == "Cooldown" ? m.timer : 0.f;
        m = {};
        m.timer = cooldown;
        for (int c : brain.nodes[(size_t)i].children) reset(c);
    }

    Status run(int i) {
        if (i < 0 || i >= (int)brain.nodes.size() || ++depth > 64) {
            --depth;
            return Status::Failure;
        }
        Status s = evaluate(i);
        brain.status[(size_t)i] = (int8_t)s;
        --depth;
        return s;
    }

    Status evaluate(int i) {
        AIBrain::Node& n = brain.nodes[(size_t)i];
        BrainState::Node& m = mem(i);
        const std::vector<int>& kids = n.children;
        const std::string& type = n.type;
        float now = ctx.time();
        // ---- Composites
        if (type == "Selector") {
            for (int c = 0; c < (int)kids.size(); ++c) {
                Status s = run(kids[(size_t)c]);
                if (s == Status::Failure) continue;
                if (m.child != c && m.child >= 0 && m.child < (int)kids.size()) reset(kids[(size_t)m.child]);   // (interrupted)
                m.child = s == Status::Running ? c : -1;
                return s;
            }
            m.child = -1;
            return Status::Failure;
        }
        if (type == "Sequence") {
            int from = std::max(m.child, 0);
            for (int c = 0; c < from; ++c)   // conditions before the running child: still true?
                if (isCondition(kids[(size_t)c]) && run(kids[(size_t)c]) == Status::Failure) {
                    if (from < (int)kids.size()) reset(kids[(size_t)from]);
                    m.child = 0;
                    return Status::Failure;
                }
            for (int c = from; c < (int)kids.size(); ++c) {
                Status s = run(kids[(size_t)c]);
                if (s == Status::Running) {
                    m.child = c;
                    return s;
                }
                if (s == Status::Failure) {
                    m.child = 0;
                    return s;
                }
            }
            m.child = 0;
            return Status::Success;
        }
        if (type == "Parallel") {
            bool all = true;
            for (int c : kids) {
                Status s = run(c);
                if (s == Status::Failure) {
                    for (int k : kids) reset(k);
                    return s;
                }
                all = all && s == Status::Success;
            }
            return all ? Status::Success : Status::Running;
        }
        if (type == "Random") {
            if (kids.empty()) return Status::Failure;
            if (!m.started) {
                m.child = std::rand() % (int)kids.size();
                m.started = true;
            }
            Status s = run(kids[(size_t)m.child]);
            if (s != Status::Running) m.started = false;
            return s;
        }
        // ---- Decorators
        int child = kids.empty() ? -1 : kids[0];
        if (type == "Inverter") {
            Status s = run(child);
            return s == Status::Running ? s : s == Status::Success ? Status::Failure : Status::Success;
        }
        if (type == "Succeeder") {
            Status s = run(child);
            return s == Status::Running ? s : Status::Success;
        }
        if (type == "Repeat") {
            Status s = run(child);
            if (s != Status::Success) {
                if (s == Status::Failure) m.count = 0;
                return s;
            }
            ++m.count;
            int times = (int)std::lround(n.number);
            if (times > 0 && m.count >= times) {
                m.count = 0;
                return Status::Success;
            }
            reset(child);
            return Status::Running;   // (again next update)
        }
        if (type == "UntilFail") {
            Status s = run(child);
            if (s == Status::Failure) return Status::Success;
            if (s == Status::Success) reset(child);
            return Status::Running;
        }
        if (type == "Cooldown") {
            if (now < m.timer) return Status::Failure;
            Status s = run(child);
            if (s != Status::Running) m.timer = now + std::max(n.number, 0.f);
            return s;
        }
        if (type == "Timeout") {
            if (!m.started) {
                m.started = true;
                m.timer = now + std::max(n.number, 0.f);
            }
            if (now >= m.timer) {
                reset(child);
                m.started = false;
                return Status::Failure;
            }
            Status s = run(child);
            if (s != Status::Running) m.started = false;
            return s;
        }
        // ---- Conditions
        if (type == "Has") return truthy(lookup(brain, n.key)) ? Status::Success : Status::Failure;
        if (type == "Compare") {
            double v = lookup(brain, n.key).asNumber(0.0), x = n.number;
            const std::string& op = n.text;
            bool ok = op == "<" ? v < x : op == "<=" ? v <= x : op == ">" ? v > x : op == ">=" ? v >= x
                    : op == "!=" ? v != x : v == x;
            return ok ? Status::Success : Status::Failure;
        }
        if (type == "CanSee") return brain.blackboard["seeing"].asBool() ? Status::Success : Status::Failure;
        if (type == "Heard") {
            const json::Value& t = brain.blackboard["heardTime"];
            return !t.isNull() && now - t.asFloat() <= std::max(n.number, 0.f) ? Status::Success : Status::Failure;
        }
        if (type == "Near") {
            Vec3 p;
            if (!targetPosition(ctx, brain, n.key, p)) return Status::Failure;
            return length(p - ctx.worldPosition(self.id)) <= n.number ? Status::Success : Status::Failure;
        }
        if (type == "Chance") return (float)std::rand() / (float)RAND_MAX < n.number ? Status::Success : Status::Failure;
        // ---- Actions
        if (type == "MoveTo" || type == "Flee") {
            Vec3 p;
            if (!targetPosition(ctx, brain, n.key, p)) return Status::Failure;
            float speed = n.number > 0.f ? n.number : 3.f;
            if (type == "MoveTo") {
                Walk w = walkTowards(ctx, self, m, p, speed, n.number2 > 0.f ? n.number2 : 1.f);
                return w == Walk::Arrived ? Status::Success : w == Walk::NoWay ? Status::Failure : Status::Running;
            }
            Vec3 pos = ctx.worldPosition(self.id), away = pos - p;
            away.y = 0.f;
            float dist = length(away), wanted = n.number2 > 0.f ? n.number2 : 10.f;
            if (dist >= wanted) return Status::Success;
            Vec3 dir = dist > 1e-4f ? away * (1.f / dist) : Vec3{1, 0, 0};
            Walk w = walkTowards(ctx, self, m, pos + dir * (wanted - dist + 1.f), speed, 0.5f);
            return w == Walk::NoWay ? Status::Failure : Status::Running;
        }
        if (type == "Patrol") {
            std::vector<std::string> points;
            std::stringstream ss(n.text);
            for (std::string p; std::getline(ss, p, ',');) {
                p.erase(0, p.find_first_not_of(' '));
                p.erase(p.find_last_not_of(' ') + 1);
                if (!p.empty()) points.push_back(p);
            }
            if (points.empty()) return Status::Failure;
            if (m.timer > now) return Status::Running;   // (pausing at a waypoint)
            Entity* to = ctx.find(points[(size_t)m.count % points.size()]);
            if (!to) {
                ++m.count;
                return Status::Running;
            }
            Walk w = walkTowards(ctx, self, m, ctx.worldPosition(to->id), n.number > 0.f ? n.number : 2.f, 0.6f);
            if (w != Walk::Walking) {   // (there, or no way there: on to the next)
                ++m.count;
                m.path.clear();
                m.timer = now + std::max(n.number2, 0.f);
            }
            return Status::Running;
        }
        if (type == "Wait") {
            if (!m.started) {
                m.started = true;
                m.timer = now + std::max(n.number, 0.f) + std::max(n.number2, 0.f) * (float)std::rand() / (float)RAND_MAX;
            }
            if (now < m.timer) return Status::Running;
            m.started = false;
            return Status::Success;
        }
        if (type == "LookAt") {
            Vec3 p;
            if (!targetPosition(ctx, brain, n.key, p)) return Status::Failure;
            face(ctx, self, p - ctx.worldPosition(self.id));
            return Status::Success;
        }
        if (type == "Set") {
            if (n.key.empty()) return Status::Failure;
            brain.blackboard.set(n.key, parseValue(n.text));
            return Status::Success;
        }
        if (type == "Clear") {
            auto& members = brain.blackboard.members();
            members.erase(std::remove_if(members.begin(), members.end(), [&](const auto& kv) { return kv.first == n.key; }),
                          members.end());
            return Status::Success;
        }
        if (type == "Message") {
            Message msg{n.text, self.id, n.number, ""};
            if (n.key.empty()) {
                ctx.broadcast(msg);
            } else if (Entity* to = targetEntity(ctx, brain, n.key)) {
                ctx.send(to->id, msg);
            } else {
                return Status::Failure;
            }
            return Status::Success;
        }
        if (type == "Sound") {
            ctx.playSound(n.text, ctx.worldPosition(self.id), n.number > 0.f ? n.number : 1.f);
            return Status::Success;
        }
        if (type == "Animate") {
            if (!self.animator) return Status::Failure;
            self.animator->setParameter(n.key, n.number);
            return Status::Success;
        }
        if (type == "Log") {
            ctx.log(self.name + ": " + formatText(brain, n.text));
            return Status::Success;
        }
        return Status::Failure;   // (an unknown type)
    }
};

}  // namespace

bool targetPosition(GameContext& ctx, const AIBrain& brain, const std::string& key, Vec3& out) {
    const json::Value& v = lookup(brain, key);
    if (v.isArray() && v.size() == 3) {
        out = {v[0].asFloat(), v[1].asFloat(), v[2].asFloat()};
        return true;
    }
    if (Entity* e = targetEntity(ctx, brain, key)) {
        out = ctx.worldPosition(e->id);
        return true;
    }
    return false;
}

void perceive(GameContext& ctx, Entity& self, AIBrain& brain, BrainState& state, const std::vector<Noise>& noises) {
    auto vec3 = [](Vec3 p) {
        json::Value a = json::Value::array();
        a.push(p.x);
        a.push(p.y);
        a.push(p.z);
        return a;
    };
    float now = ctx.time();
    Mat4 world = ctx.scene().worldMatrix(self.id);
    Vec3 eye = Vec3{world.m[12], world.m[13], world.m[14]} + Vec3{0.f, brain.eyeHeight, 0.f};
    Vec3 forward = normalize(transformDir(world, {0, 0, -1}));
    float cosHalf = std::cos(std::clamp(brain.sightAngle, 1.f, 360.f) * 0.5f * DEG2RAD);
    // Sight: the nearest entity with the tag, within range and the cone, nothing solid in between
    Entity* seen = nullptr;
    float best = 1e30f;
    Vec3 seenAt;
    if (!brain.perceives.empty() && brain.sightRange > 0.f)
        for (EntityId id : ctx.findWithTag(brain.perceives)) {
            if (id == self.id) continue;
            Entity* o = ctx.find(id);
            if (!o) continue;
            Vec3 p = ctx.worldPosition(id), centre = p + Vec3{0.f, 0.9f, 0.f};   // (about its middle)
            Vec3 d = centre - eye;
            float dist = length(d);
            if (dist > brain.sightRange || dist >= best) continue;
            if (dist > 1e-3f && dot(d * (1.f / dist), forward) < cosHalf) continue;
            RaycastHit hit;
            if (dist > 1e-3f && ctx.raycast(eye, d, dist, hit, self.id) && hit.entity != id &&
                !ctx.scene().isAncestor(id, hit.entity) && hit.distance < dist - 0.3f)
                continue;   // (something in the way)
            seen = o;
            best = dist;
            seenAt = p;
        }
    if (seen) {
        brain.blackboard.set("seeing", json::Value(true));
        brain.blackboard.set("target", json::Value(seen->name));
        brain.blackboard.set("targetPosition", vec3(seenAt));
        brain.blackboard.set("lastSeen", vec3(seenAt));
        brain.blackboard.set("lastSeenTime", json::Value(now));
    } else {
        brain.blackboard.set("seeing", json::Value(false));
        const json::Value& t = brain.blackboard["lastSeenTime"];
        if (!t.isNull() && now - t.asFloat() > brain.memory) {   // (forgotten)
            auto& members = brain.blackboard.members();
            members.erase(std::remove_if(members.begin(), members.end(),
                                         [](const auto& kv) { return kv.first == "target" || kv.first == "targetPosition"; }),
                          members.end());
        }
    }
    // Hearing: sounds made since the last update, within range (louder: further)
    for (const Noise& n : noises) {
        if (n.serial <= state.lastNoise || n.source == self.id) continue;
        if (length(n.position - eye) > brain.hearingRange * std::max(n.loudness, 0.f)) continue;
        brain.blackboard.set("noise", vec3(n.position));
        brain.blackboard.set("heardTime", json::Value(n.time));
    }
    if (!noises.empty()) state.lastNoise = std::max(state.lastNoise, noises.back().serial);
}

Status tick(GameContext& ctx, Entity& self, AIBrain& brain, BrainState& state) {
    if (brain.nodes.empty()) return Status::Failure;
    state.nodes.resize(brain.nodes.size());
    brain.status.assign(brain.nodes.size(), -1);
    Runner r{ctx, self, brain, state};
    return r.run(0);
}

void makeGuard(AIBrain& brain, const std::string& waypoints) {
    auto node = [&](const char* type, std::string key = "", std::string text = "", float number = 0.f, float number2 = 0.f) {
        AIBrain::Node n;
        n.type = type;
        n.key = std::move(key);
        n.text = std::move(text);
        n.number = number;
        n.number2 = number2;
        brain.nodes.push_back(n);
        return (int)brain.nodes.size() - 1;
    };
    brain.nodes.clear();
    int root = node("Selector");
    int chase = node("Sequence");
    brain.nodes[(size_t)chase].name = "Chase what it sees";
    int see = node("CanSee");
    int run = node("MoveTo", "target", "", 4.f, 1.5f);
    int look = node("Sequence");
    brain.nodes[(size_t)look].name = "Go where it heard something";
    int heard = node("Heard", "", "", 4.f);
    int go = node("MoveTo", "noise", "", 2.5f, 1.f);
    int wait = node("Wait", "", "", 1.5f);
    int done = node("Clear", "heardTime");
    int patrol = node("Patrol", "", waypoints, 2.f, 1.f);
    brain.nodes[(size_t)root].children = {chase, look, patrol};
    brain.nodes[(size_t)chase].children = {see, run};
    brain.nodes[(size_t)look].children = {heard, go, wait, done};
}

}  // namespace ai
