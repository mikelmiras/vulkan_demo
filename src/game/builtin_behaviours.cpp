// The engine's built-in behaviours. A game adds its own the same way (see scene/behaviour.h).
#include <algorithm>
#include <cmath>

#include "game/game.h"
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
        v.field("target", target);
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
// First-person controller. Put it on the player's body: an entity with a capsule collider (feet at its origin) and
// a child entity with the camera at eye height. The body turns left/right, the camera looks up/down, so the capsule
// stays upright. Walking slides along solid colliders; without a collider it walks at its starting height.
struct PlayerController : BehaviourBase<PlayerController> {
    static constexpr const char* kTypeName = "PlayerController";
    float moveSpeed = 6.f;
    float sprintMultiplier = 2.f;
    float lookSensitivity = 0.15f;   // degrees per pixel
    float stickLookSpeed = 150.f;    // degrees per second at full gamepad stick
    float jumpSpeed = 6.5f;
    float gravity = 18.f;
    float respawnBelow = -50.f;      // falling under this height puts the player back at the start
    bool fly = false;                // no gravity or collision; E / Q move up and down
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
            if (grounded && in.pressed("jump")) vy = jumpSpeed;
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
    std::string collector = "Player";   // entity name
    float radius = 1.8f;                // only used without a trigger collider
    bool collected = false;             // runtime state

    void fields(FieldVisitor& v) override {
        v.field("collector", collector);
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
    float hits = 1.f;          // how many it takes
    std::string dropPrefab;    // spawned where it was destroyed (empty: nothing)
    float remaining = 0.f;     // runtime state
    bool destroyed = false;

    void fields(FieldVisitor& v) override {
        v.field("hits", hits, 0.1f, 0.f, 1000.f);
        v.field("dropPrefab", dropPrefab);
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
        ctx.destroy(self.id);
        if (!dropPrefab.empty()) ctx.spawn(dropPrefab, at);
        ctx.log("Destroyed " + name);
    }
};

// ---------------------------------------------------------------------------------------------
// Flies along its -Z; anything solid it reaches stops it (and takes a hit if it's Destructible)
struct Projectile : BehaviourBase<Projectile> {
    static constexpr const char* kTypeName = "Projectile";
    float speed = 40.f;
    float gravity = 0.f;
    float lifetime = 3.f;      // seconds, then it disappears
    float damage = 1.f;
    // runtime state
    EntityId owner = kNoEntity;   // never hits this one (set by whoever fires it)
    Vec3 velocity;
    float age = 0.f;

    void fields(FieldVisitor& v) override {
        v.field("speed", speed, 0.1f, 0.f, 10000.f);
        v.field("gravity", gravity, 0.1f);
        v.field("lifetime", lifetime, 0.05f, 0.f, 1000.f);
        v.field("damage", damage, 0.1f);
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
            if (Entity* target = ctx.find(hit.entity))
                if (Destructible* d = target->behaviour<Destructible>()) d->hit(ctx, *target, damage);
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
    std::string prefab = "Bullet";
    float cooldown = 0.15f;     // seconds between shots
    float muzzle = 0.6f;        // spawn this far in front
    float nextShot = 0.f;       // runtime state

    void fields(FieldVisitor& v) override {
        v.field("prefab", prefab);
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
        if (Entity* e = ctx.find(shot))
            if (Projectile* p = e->behaviour<Projectile>()) p->owner = owner;
    }
};

// ---------------------------------------------------------------------------------------------
// Spawns a prefab at its position (facing its -Z) every `interval` seconds, keeping at most `maxAlive` around
struct Spawner : BehaviourBase<Spawner> {
    static constexpr const char* kTypeName = "Spawner";
    std::string prefab;
    float interval = 2.f;
    float maxAlive = 5.f;
    float timer = 0.f;                  // runtime state
    std::vector<EntityId> alive;

    void fields(FieldVisitor& v) override {
        v.field("prefab", prefab);
        v.field("interval", interval, 0.05f, 0.01f, 3600.f);
        v.field("maxAlive", maxAlive, 1.f, 0.f, 10000.f);
    }
    void start(GameContext&, Entity&) override {
        timer = 0.f;
        alive.clear();
    }
    void update(GameContext& ctx, Entity& self) override {
        alive.erase(std::remove_if(alive.begin(), alive.end(),
                                   [&](EntityId id) { return !ctx.find(id) || ctx.isDestroyed(id); }),
                    alive.end());
        timer -= ctx.dt();
        if (timer > 0.f || prefab.empty() || (float)alive.size() >= maxAlive) return;
        timer = interval;
        Mat4 m = ctx.scene().worldMatrix(self.id);
        EntityId id = ctx.spawn(prefab, {m.m[12], m.m[13], m.m[14]}, transformDir(m, {0, 0, -1}));
        if (id != kNoEntity) alive.push_back(id);
    }
};

}  // namespace

void registerBuiltinBehaviours() {
    registerBehaviour<Spin>("Rotates around the parent's Y axis (degrees per second)");
    registerBehaviour<Bob>("Floats up and down around its starting height");
    registerBehaviour<Patrol>("Moves back and forth between its start and start + offset");
    registerBehaviour<LookAt>("Turns to face another entity (by name) every update");
    registerBehaviour<PlayerController>(
        "First-person walking: WASD / arrows to move, hold the right mouse button to look, Space to jump, Shift to "
        "sprint. Put it on a body with a capsule collider and a camera child at eye height.");
    registerBehaviour<Pickup>("Collected when the collector entity (by name) touches its trigger collider");
    registerBehaviour<Shooter>("Fires a prefab along its -Z while the fire action (left mouse) is held");
    registerBehaviour<Projectile>("Flies along its -Z and stops at the first solid collider, damaging Destructibles");
    registerBehaviour<Destructible>("Destroyed after enough hits, optionally leaving a prefab behind");
    registerBehaviour<Spawner>("Spawns a prefab every few seconds, up to a maximum alive at once");
}
