// AI (the AIBrain component, scene/scene.h): behaviour trees, perception and the blackboard.
//
// A tree is ticked from its root every update; each node answers Success, Failure or Running (still at it: asked
// again next update). Composites run their children: a Selector tries them in order until one doesn't fail (and
// checks the earlier ones again every update, so a higher-priority branch takes over as soon as it can: seeing the
// player interrupts a patrol); a Sequence runs them in order while they succeed (re-checking its conditions while a
// later action runs: losing sight of the player stops the chase). Decorators change their one child's answer;
// conditions look at the blackboard and perception; actions do things (walk somewhere over the navigation, wait,
// set values, send messages...).
//
// Targets: a node's `key` names a blackboard value holding an entity's name or a position ([x, y, z]); if there's
// no such value, the key itself is taken as an entity's name.
//
// Perception writes: "seeing" (true / false), "target" (the nearest entity seen: its name), "targetPosition",
// "lastSeen" (where it was last seen), "lastSeenTime", "noise" (where something was last heard) and "heardTime".
// Sounds are heard from ctx.playSound and ctx.makeNoise.
#pragma once

#include <string>
#include <vector>

#include "core/vecmath.h"
#include "scene/scene.h"

class GameContext;

namespace ai {

enum class Status : int8_t { Success = 0, Failure = 1, Running = 2 };

// What the editor shows for a node type, and what its parameters mean (nullptr: unused)
struct NodeType {
    const char* type;
    const char* group;      // "Composite", "Decorator", "Condition", "Action"
    int children;           // 0: none, 1: one, -1: any
    const char* help;
    const char* key;        // what `key` is for
    const char* text;       // `text`
    const char* number;     // `number`
    const char* number2;    // `number2`
};
const std::vector<NodeType>& nodeTypes();
const NodeType* nodeType(const std::string& type);

struct Noise {
    Vec3 position;
    float loudness = 1.f;   // (times the hearing range)
    float time = 0.f;
    EntityId source = kNoEntity;
    uint64_t serial = 0;   // (in the order they were made: a brain hears each once)
};

// A brain's memory between updates (Game keeps one per entity with an AIBrain)
struct BrainState {
    struct Node {
        int child = 0;              // composites: the child running (-1: none chosen)
        float timer = 0.f;          // waits, timeouts; cooldowns: when it's ready again
        int count = 0;              // repeats; patrols: the waypoint
        bool started = false;
        std::vector<Vec3> path;     // walking: the way there
        size_t next = 0;
        float repathIn = 0.f;
    };
    std::vector<Node> nodes;
    uint64_t lastNoise = 0;         // (the last noise heard: later ones are new)
};

// Fills the blackboard's perception values from what it sees and hears now
void perceive(GameContext& ctx, Entity& self, AIBrain& brain, BrainState& state, const std::vector<Noise>& noises);
// One update of the tree (status per node in brain.status)
Status tick(GameContext& ctx, Entity& self, AIBrain& brain, BrainState& state);

// Blackboard helpers: a target's world position (see above); a value as text ("12", "true", "Guard")
bool targetPosition(GameContext& ctx, const AIBrain& brain, const std::string& key, Vec3& out);
json::Value parseValue(const std::string& text);   // a number, true / false, [x, y, z] or else the text itself

// A starter tree: patrol the given waypoints, chase what it sees, look where it heard something
void makeGuard(AIBrain& brain, const std::string& waypoints);

}  // namespace ai
