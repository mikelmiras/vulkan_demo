// The engine's built-in behaviours. A game adds its own the same way (see scene/behaviour.h).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "game/game.h"
#include "game/ui.h"
#include "scene/behaviour.h"
#include "scene/scene.h"

namespace {

void movePosition(Transform& t, Vec3 d) {
    t.position[0] += d.x;
    t.position[1] += d.y;
    t.position[2] += d.z;
}


// ---------------------------------------------------------------------------------------------
struct Spin : BehaviourBase<Spin> {
    static constexpr const char* kTypeName = "Spin";
    float speed = 90.f;   // degrees per second, around the parent's Y axis

    void fields(FieldVisitor& v) override { v.field("speed", speed, 1.f); }
    void update(GameContext& ctx, Entity& self) override {
        Transform& t = self.transform;
        float pos[3] = {t.position[0], t.position[1], t.position[2]}, scale[3] = {t.scale[0], t.scale[1], t.scale[2]};
        t.setMatrix(rotateY(speed * DEG2RAD * ctx.dt()) * t.matrix());
        // Only the rotation should change: restoring these exactly keeps rounding from creeping into them
        std::copy(pos, pos + 3, t.position);
        std::copy(scale, scale + 3, t.scale);
    }
};

// ---------------------------------------------------------------------------------------------
struct Bob : BehaviourBase<Bob> {
    static constexpr const char* kTypeName = "Bob";
    void state(FieldVisitor& v) override { v.field("baseY", baseY); }
    float height = 0.5f;   // amplitude
    float speed = 1.5f;    // radians per second
    float phase = 0.f;     // degrees
    float baseY = 0.f;     // runtime state: the height it started at

    void fields(FieldVisitor& v) override {
        v.field("height", height, 0.01f, 0.f, 100.f);
        v.field("speed", speed, 0.01f);
        v.field("phase", phase, 1.f);
    }
    void start(GameContext&, Entity& self) override { baseY = self.transform.position[1]; }
    void update(GameContext& ctx, Entity& self) override {
        // Relative to the starting height, so pressing Play doesn't make the object jump
        float p = phase * DEG2RAD;
        self.transform.position[1] = baseY + height * (std::sin(ctx.time() * speed + p) - std::sin(p));
    }
};

// ---------------------------------------------------------------------------------------------
struct Patrol : BehaviourBase<Patrol> {
    static constexpr const char* kTypeName = "Patrol";
    void state(FieldVisitor& v) override { v.field("origin", origin); }
    Vec3 offset{0, 0, 6};   // goes back and forth between its start and start + offset (parent space)
    float speed = 3.f;      // units per second
    Vec3 origin;

    void fields(FieldVisitor& v) override {
        v.field("offset", offset);
        v.field("speed", speed, 0.05f, 0.f, 1000.f);
    }
    void start(GameContext&, Entity& self) override { origin = self.transform.pos(); }
    void update(GameContext& ctx, Entity& self) override {
        float d = length(offset);
        if (d < 1e-4f) return;
        float s = std::fmod(ctx.time() * speed, 2.f * d);
        float u = s < d ? s / d : 2.f - s / d;   // 0 -> 1 -> 0
        Vec3 p = origin + offset * u;
        self.transform.position[0] = p.x; self.transform.position[1] = p.y; self.transform.position[2] = p.z;
    }
};

// ---------------------------------------------------------------------------------------------
struct LookAt : BehaviourBase<LookAt> {
    static constexpr const char* kTypeName = "LookAt";
    std::string target = "Player";   // entity name
    bool yawOnly = false;            // only turn around the vertical axis

    void fields(FieldVisitor& v) override {
        v.field("target", target, FieldRef::Entity);
        v.field("yawOnly", yawOnly);
    }
    void update(GameContext& ctx, Entity& self) override {
        Entity* t = ctx.find(target);
        if (!t || t == &self) return;
        Vec3 from = ctx.worldPosition(self.id), dir = ctx.worldPosition(t->id) - from;
        if (yawOnly) dir.y = 0.f;
        if (length(dir) < 1e-4f || length(cross(normalize(dir), {0, 1, 0})) < 1e-4f) return;   // on top: no heading
        Mat4 world = lookRotation(from, normalize(dir), self.transform.scale);
        self.transform.setMatrix(inverseAffine(ctx.scene().parentWorldMatrix(self.id)) * world);
    }
};

// ---------------------------------------------------------------------------------------------
// Walks to another entity around walls and over steps, along a path from the scene's navigation grid (recomputed
// every `repath` seconds, as the target moves), hopping across off-mesh links and keeping apart from other
// NavAgents. With a capsule or sphere collider it moves as a character.
struct NavAgent : BehaviourBase<NavAgent> {
    static constexpr const char* kTypeName = "NavAgent";
    std::string target = "Player";
    float speed = 3.f;          // metres per second
    float stopDistance = 1.2f;  // close enough
    float range = 0.f;          // only follows within this distance (0: any)
    float repath = 0.5f;        // seconds between new paths
    bool turn = true;           // faces where it's going
    float personalSpace = 1.f;  // other agents closer than this are steered away from
    // runtime
    std::vector<Vec3> path;
    std::vector<uint8_t> jumps;
    size_t next = 0;
    float repathIn = 0.f;
    bool hopping = false;       // crossing an off-mesh link
    Vec3 hopFrom, hopTo;
    float hopT = 0.f, hopTime = 0.f, hopHeight = 0.f, hopLift = 0.f;

    void fields(FieldVisitor& v) override {
        v.field("target", target, FieldRef::Entity);
        v.field("speed", speed, 0.05f, 0.f, 100.f);
        v.field("stopDistance", stopDistance, 0.05f, 0.f, 100.f);
        v.field("range", range, 0.1f, 0.f, 10000.f);
        v.field("repath", repath, 0.01f, 0.02f, 60.f);
        v.field("turn", turn);
        v.field("personalSpace", personalSpace, 0.05f, 0.f, 20.f);
    }
    void placeAt(GameContext& ctx, Entity& self, Vec3 world) {
        Vec3 local = transformPoint(inverseAffine(ctx.scene().parentWorldMatrix(self.id)), world);
        self.transform.position[0] = local.x;
        self.transform.position[1] = local.y;
        self.transform.position[2] = local.z;
    }
    void update(GameContext& ctx, Entity& self) override {
        if (hopping) {   // along an arc from one end of the link to the other
            hopT += ctx.dt();
            float u = std::min(hopT / std::max(hopTime, 1e-3f), 1.f);
            Vec3 p = hopFrom + (hopTo - hopFrom) * u;
            p.y += hopLift + 4.f * hopHeight * u * (1.f - u);
            placeAt(ctx, self, p);
            if (u >= 1.f) {
                hopping = false;
                ++next;
            }
            return;
        }
        Entity* t = ctx.find(target);
        if (!t || t == &self) return;
        Vec3 pos = ctx.worldPosition(self.id), goal = ctx.worldPosition(t->id), flat = goal - pos;
        flat.y = 0.f;
        float d = length(flat);
        if ((range > 0.f && d > range) || d <= stopDistance) {
            path.clear();
            return;
        }
        repathIn -= ctx.dt();
        if (repathIn <= 0.f || next >= path.size()) {
            repathIn = repath;
            next = 1;
            if (!ctx.findPath(pos, goal, path, &jumps)) path.clear();
        }
        float step = speed * ctx.dt();
        Vec3 to;
        float dist = 0.f;
        for (; next < path.size(); ++next) {   // skip corners already reached
            to = path[next] - pos;
            to.y = 0.f;
            dist = length(to);
            if (next > 0 && next - 1 < jumps.size() && jumps[next - 1] && dist > 0.05f) {   // at a link: hop
                hopFrom = path[next - 1];
                hopTo = path[next];
                hopLift = pos.y - hopFrom.y;   // (its origin's height above its feet)
                float span = length(hopTo - hopFrom);
                hopTime = std::max(span / std::max(speed, 0.1f), 0.3f);
                hopHeight = 0.3f + 0.2f * span;
                hopT = 0.f;
                hopping = true;
                return;
            }
            if (dist > 0.05f || next + 1 == path.size()) break;
        }
        if (next >= path.size() || dist < 1e-4f) return;
        Vec3 delta = to * (std::min(step, dist) / dist);
        // Keep apart from other agents: a push away from each one within personal space
        if (personalSpace > 0.f) {
            Vec3 push;
            for (const Entity& o : ctx.scene().entities) {
                if (o.id == self.id || !o.behaviours.size()) continue;
                bool agent = false;
                for (const BehaviourPtr& b : o.behaviours) agent = agent || std::string(b->typeName()) == kTypeName;
                if (!agent) continue;
                Vec3 away = pos - ctx.worldPosition(o.id);
                away.y = 0.f;
                float d2 = dot(away, away);
                if (d2 >= personalSpace * personalSpace) continue;
                float d = std::sqrt(d2);
                if (d < 1e-4f) away = {((self.id * 7919u) % 100) / 100.f - 0.5f, 0.f, 0.3f}, d = length(away);   // (on top of each other)
                push = push + away * ((1.f - d / personalSpace) / d);
            }
            delta = delta + push * (step * 0.8f);
        }
        ctx.moveCharacter(self, delta);
        if (turn) {
            Mat4 world = lookRotation(pos, normalize(delta), self.transform.scale);
            Mat4 local = inverseAffine(ctx.scene().parentWorldMatrix(self.id)) * world;
            Vec3 at = self.transform.pos();   // keep where moveCharacter put it
            self.transform.setMatrix(local);
            self.transform.position[0] = at.x;
            self.transform.position[1] = at.y;
            self.transform.position[2] = at.z;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// First-person controller. Put it on the player's body: an entity with a capsule collider (feet at its origin) and
// a child entity with the camera at eye height. The body turns left/right, the camera looks up/down, so the capsule
// stays upright. Walking slides along solid colliders; without a collider it walks at its starting height.
struct PlayerController : BehaviourBase<PlayerController> {
    static constexpr const char* kTypeName = "PlayerController";
    void state(FieldVisitor& v) override {
        v.field("yaw", yaw);
        v.field("pitch", pitch);
        v.field("vy", vy);
        v.field("grounded", grounded);
        v.field("spawn", spawn);
    }
    float moveSpeed = 6.f;
    float sprintMultiplier = 2.f;
    float lookSensitivity = 0.15f;   // degrees per pixel
    float stickLookSpeed = 150.f;    // degrees per second at full gamepad stick
    float jumpSpeed = 6.5f;
    float gravity = 18.f;
    float respawnBelow = -50.f;      // falling under this height puts the player back at the start
    bool fly = false;                // no gravity or collision; E / Q move up and down
    std::string jumpSound = "Jump";
    // runtime state
    float yaw = 0.f, pitch = 0.f, vy = 0.f;
    bool grounded = false;
    Vec3 spawn;

    void fields(FieldVisitor& v) override {
        v.field("moveSpeed", moveSpeed, 0.05f, 0.f, 1000.f);
        v.field("sprintMultiplier", sprintMultiplier, 0.05f, 1.f, 20.f);
        v.field("lookSensitivity", lookSensitivity, 0.005f, 0.f, 5.f);
        v.field("stickLookSpeed", stickLookSpeed, 1.f, 0.f, 2000.f);
        v.field("jumpSpeed", jumpSpeed, 0.05f, 0.f, 100.f);
        v.field("gravity", gravity, 0.1f, 0.f, 200.f);
        v.field("respawnBelow", respawnBelow, 0.5f);
        v.field("fly", fly);
        v.field("jumpSound", jumpSound, FieldRef::Sound);
    }

    // The entity that looks up and down: the first child with a camera, else the body itself
    static Entity* head(GameContext& ctx, Entity& self) {
        for (EntityId id : ctx.scene().childrenOf(self.id))
            if (Entity* c = ctx.find(id); c && c->camera) return c;
        return &self;
    }

    void start(GameContext& ctx, Entity& self) override {
        Entity* h = head(ctx, self);
        Vec3 f = normalize(transformDir(ctx.scene().worldMatrix(h->id), {0, 0, -1}));
        yaw = std::atan2(f.z, f.x);
        pitch = std::asin(std::clamp(f.y, -1.f, 1.f));
        vy = 0.f;
        grounded = false;
        spawn = self.transform.pos();
    }

    void update(GameContext& ctx, Entity& self) override {
        const InputMap& in = ctx.input();
        float dt = ctx.dt();
        if (in.down("look") || in.mouseCaptured()) {
            ImVec2 d = in.mouseDelta();
            yaw += d.x * lookSensitivity * DEG2RAD;
            pitch -= d.y * lookSensitivity * DEG2RAD;
        }
        yaw += in.axis("look_left", "look_right") * stickLookSpeed * DEG2RAD * dt;
        pitch += in.axis("look_down", "look_up") * stickLookSpeed * DEG2RAD * dt;
        pitch = std::clamp(pitch, -1.5f, 1.5f);
        Vec3 flat{std::cos(yaw), 0.f, std::sin(yaw)}, right{-flat.z, 0.f, flat.x};
        Vec3 move = flat * in.axis("move_back", "move_forward") + right * in.axis("move_left", "move_right");
        float speed = moveSpeed * (in.down("sprint") ? sprintMultiplier : 1.f);
        // Keyboard: full speed in any direction. Stick: proportional to how far it's pushed
        if (length(move) > 1.f) move = normalize(move);
        move = move * speed;

        if (fly) {
            movePosition(self.transform, (move + Vec3{0, in.axis("move_down", "move_up") * speed, 0}) * dt);
            vy = 0.f;
        } else {
            if (grounded && in.pressed("jump")) {
                vy = jumpSpeed;
                ctx.playSound2D(jumpSound, 0.5f);
            }
            vy -= gravity * dt;
            Vec3 delta = (move + Vec3{0, vy, 0}) * dt;
            if (self.collider) {
                CharacterMove m = ctx.moveCharacter(self, delta);
                grounded = m.grounded;
                if ((grounded && vy < 0.f) || (m.hitCeiling && vy > 0.f)) vy = 0.f;
            } else {   // no collider: walk at the starting height
                movePosition(self.transform, delta);
                grounded = self.transform.position[1] <= spawn.y;
                if (grounded) { self.transform.position[1] = spawn.y; vy = std::max(vy, 0.f); }
            }
            if (ctx.worldPosition(self.id).y < respawnBelow) {
                self.transform.position[0] = spawn.x; self.transform.position[1] = spawn.y; self.transform.position[2] = spawn.z;
                vy = 0.f;
                ctx.log("Fell off the world: back to the start");
            }
        }

        // Body: yaw only. Head: pitch (or both, when the body is the camera)
        Entity* h = head(ctx, self);
        Vec3 forward{std::cos(pitch) * flat.x, std::sin(pitch), std::cos(pitch) * flat.z};
        self.transform.setMatrix(lookRotation(self.transform.pos(), h == &self ? forward : flat, self.transform.scale));
        if (h != &self) {
            h->transform.rotation[0] = pitch / DEG2RAD;
            h->transform.rotation[1] = h->transform.rotation[2] = 0.f;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Collected (destroyed, with a log message) when the collector entity touches it. With a trigger collider it
// reacts to trigger overlaps; without one it checks the distance to the collector every update.
struct Pickup : BehaviourBase<Pickup> {
    static constexpr const char* kTypeName = "Pickup";
    void state(FieldVisitor& v) override { v.field("collected", collected); }
    std::string collector = "Player";   // entity name
    float radius = 1.8f;                // only used without a trigger collider
    bool collected = false;             // runtime state

    void fields(FieldVisitor& v) override {
        v.field("collector", collector, FieldRef::Entity);
        v.field("radius", radius, 0.05f, 0.f, 1000.f);
    }
    void update(GameContext& ctx, Entity& self) override {
        if (self.collider && self.collider->trigger) return;
        Entity* c = ctx.find(collector);
        if (c && length(ctx.worldPosition(c->id) - ctx.worldPosition(self.id)) <= radius) collect(ctx, self);
    }
    void onTriggerEnter(GameContext& ctx, Entity& self, Entity& other) override {
        if (other.name == collector) collect(ctx, self);
    }
    void collect(GameContext& ctx, Entity& self) {
        if (collected) return;
        collected = true;
        ctx.playSound("Pickup", ctx.worldPosition(self.id));
        if (self.particleEmitter) ctx.emitParticles(self.id);   // (they outlive the entity)
        ctx.destroy(self.id);
        int left = 0;
        for (Entity& e : ctx.scene().entities)
            if (Pickup* p = e.behaviour<Pickup>(); p && !p->collected) ++left;
        ctx.log("Collected " + self.name + (left ? " (" + std::to_string(left) + " left)" : ". All collected!"));
    }
};

// ---------------------------------------------------------------------------------------------
// Takes hits (from Projectile, or any behaviour calling hit()); destroyed when they run out, optionally leaving a
// prefab behind
struct Destructible : BehaviourBase<Destructible> {
    static constexpr const char* kTypeName = "Destructible";
    void state(FieldVisitor& v) override {
        v.field("remaining", remaining);
        v.field("destroyed", destroyed);
    }
    float hits = 1.f;          // how many it takes
    std::string dropPrefab;    // spawned where it was destroyed (empty: nothing)
    float remaining = 0.f;     // runtime state
    bool destroyed = false;

    void fields(FieldVisitor& v) override {
        v.field("hits", hits, 0.1f, 0.f, 1000.f);
        v.field("dropPrefab", dropPrefab, FieldRef::Prefab);
    }
    void start(GameContext&, Entity&) override {
        remaining = hits;
        destroyed = false;
    }
    void hit(GameContext& ctx, Entity& self, float damage) {
        if (destroyed) return;
        remaining -= damage;
        if (remaining > 0.f) return;
        destroyed = true;
        Vec3 at = ctx.worldPosition(self.id);
        std::string name = self.name;   // spawn() below may move `self` if it's still in the spawn queue
        if (self.particleEmitter) ctx.emitParticles(self.id);
        else {   // a puff of dust and chips
            ParticleEmitter dust;
            dust.spread = 180.f;
            dust.speed = 3.f;
            dust.radius = 0.3f;
            dust.gravity = 6.f;
            dust.drag = 1.5f;
            dust.lifetime = 0.9f;
            dust.startSize = 0.25f;
            dust.endSize = 0.5f;
            dust.startColor = dust.endColor = {0.75f, 0.7f, 0.62f};
            dust.startAlpha = 0.8f;
            dust.additive = false;
            ctx.emitParticles(dust, at, 30);
        }
        ctx.destroy(self.id);
        ctx.playSound("Break", at);
        if (!dropPrefab.empty()) ctx.spawn(dropPrefab, at);
        ctx.log("Destroyed " + name);
    }
};

// ---------------------------------------------------------------------------------------------
// Flies along its -Z; anything solid it reaches stops it (and takes a hit if it's Destructible)
struct Projectile : BehaviourBase<Projectile> {
    static constexpr const char* kTypeName = "Projectile";
    void state(FieldVisitor& v) override {
        v.field("owner", owner);
        v.field("velocity", velocity);
        v.field("age", age);
    }
    float speed = 40.f;
    float gravity = 0.f;
    float lifetime = 3.f;      // seconds, then it disappears
    float damage = 1.f;
    float impulse = 6.f;       // speed change given to a rigid body of mass 1 it hits
    // runtime state
    EntityId owner = kNoEntity;   // never hits this one (set by whoever fires it)
    Vec3 velocity;
    float age = 0.f;

    void fields(FieldVisitor& v) override {
        v.field("speed", speed, 0.1f, 0.f, 10000.f);
        v.field("gravity", gravity, 0.1f);
        v.field("lifetime", lifetime, 0.05f, 0.f, 1000.f);
        v.field("damage", damage, 0.1f);
        v.field("impulse", impulse, 0.1f, 0.f, 1000.f);
    }
    void start(GameContext& ctx, Entity& self) override {
        velocity = normalize(transformDir(ctx.scene().worldMatrix(self.id), {0, 0, -1})) * speed;
        age = 0.f;
    }
    void update(GameContext& ctx, Entity& self) override {
        float dt = ctx.dt();
        age += dt;
        if (age > lifetime) { ctx.destroy(self.id); return; }
        velocity.y -= gravity * dt;
        Vec3 from = ctx.worldPosition(self.id), step = velocity * dt;
        RaycastHit hit;
        if (ctx.raycast(from, step, length(step), hit, owner)) {
            ctx.destroy(self.id);
            if (Entity* target = ctx.find(hit.entity)) {
                if (target->rigidBody) target->rigidBody->velocity += normalize(velocity) * (impulse / target->rigidBody->mass);
                if (Destructible* d = target->behaviour<Destructible>()) d->hit(ctx, *target, damage);
            }
            return;
        }
        Vec3 local = transformDir(inverseAffine(ctx.scene().parentWorldMatrix(self.id)), step);
        movePosition(self.transform, local);
    }
};

// ---------------------------------------------------------------------------------------------
// Fires a prefab (usually one with a Projectile) along its -Z while the "fire" action is held. Put it on a camera.
struct Shooter : BehaviourBase<Shooter> {
    static constexpr const char* kTypeName = "Shooter";
    void state(FieldVisitor& v) override { v.field("nextShot", nextShot); }
    std::string prefab = "Bullet";
    float cooldown = 0.15f;     // seconds between shots
    float muzzle = 0.6f;        // spawn this far in front
    float nextShot = 0.f;       // runtime state

    void fields(FieldVisitor& v) override {
        v.field("prefab", prefab, FieldRef::Prefab);
        v.field("cooldown", cooldown, 0.01f, 0.f, 60.f);
        v.field("muzzle", muzzle, 0.01f, 0.f, 100.f);
    }
    void start(GameContext&, Entity&) override { nextShot = 0.f; }
    void update(GameContext& ctx, Entity& self) override {
        if (!ctx.input().down("fire") || ctx.time() < nextShot) return;
        nextShot = ctx.time() + cooldown;
        Mat4 m = ctx.scene().worldMatrix(self.id);
        Vec3 pos{m.m[12], m.m[13], m.m[14]}, fwd = normalize(transformDir(m, {0, 0, -1}));
        // The shooter's body (first entity up the tree with a collider) must not stop its own bullets
        EntityId owner = self.id;
        for (Entity* e = &self; e; e = ctx.find(e->parent))
            if (e->collider) { owner = e->id; break; }
        EntityId shot = ctx.spawn(prefab, pos + fwd * muzzle, fwd);
        ctx.playSound("Shot", pos, 0.6f);
        if (Entity* e = ctx.find(shot))
            if (Projectile* p = e->behaviour<Projectile>()) p->owner = owner;
    }
};

// ---------------------------------------------------------------------------------------------
// Spawns a prefab at its position (facing its -Z) every `interval` seconds, keeping at most `maxAlive` around
struct Spawner : BehaviourBase<Spawner> {
    static constexpr const char* kTypeName = "Spawner";
    void state(FieldVisitor& v) override {
        v.field("timer", timer);
        std::string ids;   // the spawned entities still alive, as "12 15 31"
        for (EntityId id : alive) ids += (ids.empty() ? "" : " ") + std::to_string(id);
        v.field("alive", ids);
        alive.clear();
        std::istringstream in(ids);
        for (EntityId id; in >> id;) alive.push_back(id);
    }
    std::string prefab;
    float interval = 2.f;
    int maxAlive = 5;
    float spread = 0.5f;                // random offset (radius, horizontal) so spawns don't land on each other
    float timer = 0.f;                  // runtime state
    std::vector<EntityId> alive;
    uint32_t rng = 0;

    void fields(FieldVisitor& v) override {
        v.field("prefab", prefab, FieldRef::Prefab);
        v.field("interval", interval, 0.05f, 0.01f, 3600.f);
        v.field("maxAlive", maxAlive, 0, 10000);
        v.field("spread", spread, 0.05f, 0.f, 1000.f);
    }
    void start(GameContext&, Entity& self) override {
        timer = 0.f;
        alive.clear();
        rng = self.id * 2654435761u + 1u;   // deterministic per spawner
    }
    float random01() {   // xorshift
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (float)(rng & 0xFFFFFF) / (float)0x1000000;
    }
    void update(GameContext& ctx, Entity& self) override {
        alive.erase(std::remove_if(alive.begin(), alive.end(),
                                   [&](EntityId id) { return !ctx.find(id) || ctx.isDestroyed(id); }),
                    alive.end());
        timer -= ctx.dt();
        if (timer > 0.f || prefab.empty() || (int)alive.size() >= maxAlive) return;
        timer = interval;
        Mat4 m = ctx.scene().worldMatrix(self.id);
        float a = random01() * 2.f * PI, r = std::sqrt(random01()) * spread;
        Vec3 at{m.m[12] + std::cos(a) * r, m.m[13], m.m[14] + std::sin(a) * r};
        EntityId id = ctx.spawn(prefab, at, transformDir(m, {0, 0, -1}));
        if (id != kNoEntity) alive.push_back(id);
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Scene flow
// ---------------------------------------------------------------------------------------------
struct SceneExit : BehaviourBase<SceneExit> {
    static constexpr const char* kTypeName = "SceneExit";
    std::string scene;                  // the scene file to go to (relative to the project)
    std::string collector = "Player";   // who sets it off, by touching its trigger collider
    void fields(FieldVisitor& v) override {
        v.field("scene", scene, FieldRef::Scene);
        v.field("collector", collector, FieldRef::Entity);
    }
    void start(GameContext& ctx, Entity& self) override {
        if (!self.collider || !self.collider->trigger) ctx.log(self.name + ": SceneExit needs a trigger collider");
    }
    void onTriggerEnter(GameContext& ctx, Entity&, Entity& other) override {
        if (other.name == collector && !scene.empty()) ctx.loadScene(scene);
    }
};

// Streams a scene file in (placed where this entity is) while the collector is within `radius`, and out again
// once it's `margin` further away: big worlds in pieces
struct StreamingZone : BehaviourBase<StreamingZone> {
    static constexpr const char* kTypeName = "StreamingZone";
    std::string scene;
    std::string collector = "Player";
    float radius = 40.f;
    float margin = 10.f;
    EntityId chunk = kNoEntity;   // runtime: the streamed-in part's root
    void fields(FieldVisitor& v) override {
        v.field("scene", scene, FieldRef::Scene);
        v.field("collector", collector, FieldRef::Entity);
        v.field("radius", radius, 0.5f, 0.f, 100000.f);
        v.field("margin", margin, 0.5f, 0.f, 100000.f);
    }
    void state(FieldVisitor& v) override { v.field("chunk", chunk); }
    void update(GameContext& ctx, Entity& self) override {
        Entity* c = ctx.find(collector);
        if (!c || scene.empty()) return;
        Vec3 here = ctx.worldPosition(self.id);
        float d = length(ctx.worldPosition(c->id) - here);
        if (chunk == kNoEntity && d < radius) {
            chunk = ctx.streamIn(scene, here);
        } else if (chunk != kNoEntity && d > radius + margin) {
            ctx.streamOut(chunk);
            chunk = kNoEntity;
        }
    }
};

// Plays a music track (streamed, looping, cross-fading from whatever played before) when the scene starts
struct MusicPlayer : BehaviourBase<MusicPlayer> {
    static constexpr const char* kTypeName = "MusicPlayer";
    std::string clip;
    float volume = 0.8f;
    float fade = 1.5f;   // seconds of cross-fade
    void fields(FieldVisitor& v) override {
        v.field("clip", clip, FieldRef::Sound);
        v.field("volume", volume, 0.01f, 0.f, 2.f);
        v.field("fade", fade, 0.05f, 0.f, 30.f);
    }
    void start(GameContext& ctx, Entity&) override {
        if (!clip.empty()) ctx.playMusic(clip, fade, volume);
    }
};

struct SaveLoadKeys : BehaviourBase<SaveLoadKeys> {
    static constexpr const char* kTypeName = "SaveLoadKeys";
    std::string slot = "quicksave";
    std::string saveAction = "quicksave";   // F5 by default
    std::string loadAction = "quickload";   // F9
    void fields(FieldVisitor& v) override {
        v.field("slot", slot);
        v.field("saveAction", saveAction, FieldRef::InputAction);
        v.field("loadAction", loadAction, FieldRef::InputAction);
    }
    void update(GameContext& ctx, Entity&) override {
        if (ctx.input().pressed(saveAction)) ctx.saveGame(slot);
        if (ctx.input().pressed(loadAction)) {
            if (ctx.hasSaveGame(slot)) ctx.loadGame(slot);
            else ctx.log("No save game '" + slot + "' yet");
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Game UI
// ---------------------------------------------------------------------------------------------
namespace {


}  // namespace

struct HudText : BehaviourBase<HudText> {
    static constexpr const char* kTypeName = "HudText";
    std::string text = "Score: {score}";   // {key} shows the game state's value
    float x = 0.03f, y = 0.03f;            // position as a fraction of the game view (0..1)
    float size = 40.f;                     // canvas units: 1080 = the view's height
    int align = 0;                         // left, center, right (GameUI::Align)
    Vec3 color{1.f, 1.f, 1.f};
    void fields(FieldVisitor& v) override {
        v.field("text", text);
        v.field("x", x, 0.005f, 0.f, 1.f);
        v.field("y", y, 0.005f, 0.f, 1.f);
        v.field("size", size, 0.5f, 4.f, 400.f);
        v.field("align", align, {"left", "center", "right"});
        v.field("color", color, 0.01f);
    }
    void drawUI(GameContext& ctx, Entity&, GameUI& ui) override {
        ui.text(x * ui.width(), y * ui.height(), ctx.state().format(text), size, {color.x, color.y, color.z, 1.f}, (GameUI::Align)align);
    }
};

struct HudBar : BehaviourBase<HudBar> {
    static constexpr const char* kTypeName = "HudBar";
    std::string value = "health";   // game state number shown
    float max = 100.f;              // ... out of this
    std::string label;              // drawn on the bar ({key} works here too)
    float x = 0.03f, y = 0.9f, w = 0.25f, h = 0.035f;   // fractions of the game view
    Vec3 color{0.3f, 0.85f, 0.4f};
    void fields(FieldVisitor& v) override {
        v.field("value", value);
        v.field("max", max, 1.f, 0.f, 1e6f);
        v.field("label", label);
        v.field("x", x, 0.005f, 0.f, 1.f);
        v.field("y", y, 0.005f, 0.f, 1.f);
        v.field("w", w, 0.005f, 0.f, 1.f);
        v.field("h", h, 0.001f, 0.f, 1.f);
        v.field("color", color, 0.01f);
    }
    void drawUI(GameContext& ctx, Entity&, GameUI& ui) override {
        float bx = x * ui.width(), by = y * ui.height(), bw = w * ui.width(), bh = h * ui.height();
        ui.bar(bx, by, bw, bh, max > 0.f ? ctx.state().number(value) / max : 0.f, {color.x, color.y, color.z, 0.95f});
        if (!label.empty()) ui.text(bx + bw * 0.5f, by + bh * 0.12f, ctx.state().format(label), bh * 0.75f, {}, GameUI::Align::Center);
    }
};

struct PauseMenu : BehaviourBase<PauseMenu> {
    static constexpr const char* kTypeName = "PauseMenu";
    std::string action = "pause";   // opens and closes it (P / gamepad Start by default)
    std::string title = "Paused";
    std::string slot = "quicksave"; // what Save and Load use
    bool open = false;              // runtime: this menu paused the game
    void fields(FieldVisitor& v) override {
        v.field("action", action, FieldRef::InputAction);
        v.field("title", title);
        v.field("slot", slot);
    }
    void drawUI(GameContext& ctx, Entity&, GameUI& ui) override {
        if (ctx.input().pressedThisFrame(action)) {
            if (!open && !ctx.paused()) { open = true; ctx.setPaused(true); }
            else if (open) { open = false; ctx.setPaused(false); return; }
        }
        if (!open) return;
        if (!ctx.paused()) { open = false; return; }   // resumed from elsewhere (the editor's toolbar)
        ui.showCursor();
        float cx = ui.width() * 0.5f, w = 420.f, h = 72.f, gap = 22.f, top = 330.f;
        ui.rect(0.f, 0.f, ui.width(), ui.height(), {0.f, 0.f, 0.f, 0.55f});
        ui.text(cx, top - 130.f, title, 84.f, {}, GameUI::Align::Center);
        auto item = [&](int row, const char* label) { return ui.button(cx - w * 0.5f, top + row * (h + gap), w, h, label, 38.f); };
        if (item(0, "Resume")) { open = false; ctx.setPaused(false); }
        if (item(1, "Save")) ctx.saveGame(slot);
        bool canLoad = ctx.hasSaveGame(slot);
        if (item(2, canLoad ? "Load" : "Load (no save yet)") && canLoad) {
            open = false;
            ctx.setPaused(false);
            ctx.loadGame(slot);
        }
        if (item(3, "Quit")) ctx.quit();
    }
};

void registerBuiltinBehaviours() {
    registerBehaviour<Spin>("Rotates around the parent's Y axis (degrees per second)");
    registerBehaviour<Bob>("Floats up and down around its starting height");
    registerBehaviour<Patrol>("Moves back and forth between its start and start + offset");
    registerBehaviour<LookAt>("Turns to face another entity (by name) every update");
    registerBehaviour<NavAgent>("Walks to another entity along paths around walls (the scene's navigation)");
    registerBehaviour<PlayerController>(
        "First-person walking: WASD / arrows to move, hold the right mouse button to look, Space to jump, Shift to "
        "sprint. Put it on a body with a capsule collider and a camera child at eye height.");
    registerBehaviour<Pickup>("Collected when the collector entity (by name) touches its trigger collider");
    registerBehaviour<Shooter>("Fires a prefab along its -Z while the fire action (left mouse) is held");
    registerBehaviour<Projectile>("Flies along its -Z and stops at the first solid collider, damaging Destructibles");
    registerBehaviour<Destructible>("Destroyed after enough hits, optionally leaving a prefab behind");
    registerBehaviour<Spawner>("Spawns a prefab every few seconds, up to a maximum alive at once");
    registerBehaviour<SceneExit>("Goes to another scene when the collector touches its trigger collider");
    registerBehaviour<StreamingZone>("Streams a scene in around it while the collector is near, and out when it leaves");
    registerBehaviour<MusicPlayer>("Plays a music track (streamed, looping, cross-faded) when the scene starts");
    registerBehaviour<SaveLoadKeys>("Saves the game on the save action (F5) and loads it on the load action (F9)");
    registerBehaviour<HudText>("Text over the game view; {key} shows a game state value (\"Score: {score}\")");
    registerBehaviour<HudBar>("A bar over the game view showing a game state number out of a maximum");
    registerBehaviour<PauseMenu>("Pauses on the pause action (P) and shows Resume / Save / Load / Quit");
}
