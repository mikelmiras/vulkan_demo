// The sample game's own code, built as a game module (see game/module.h): a coin hunt. Coins add to the score kept
// by the ScoreKeeper; drones (Chaser) home in on the player and cost points when they catch it.
//
// Edit this file while the editor is open and rebuild (Game > Rebuild & reload game module, Ctrl+R): the editor
// swaps the new code in and keeps the scene.
#include <algorithm>
#include <cmath>
#include <string>

#include "game/game.h"
#include "game/module.h"

namespace {

struct ScoreKeeper : BehaviourBase<ScoreKeeper> {
    static constexpr const char* kTypeName = "ScoreKeeper";
    float target = 10.f;   // points to win
    float score = 0.f;     // runtime state (not a field: starts at 0 every Play)
    bool won = false;

    void fields(FieldVisitor& v) override { v.field("target", target, 1.f, 1.f, 1000.f); }
    void start(GameContext& ctx, Entity&) override { ctx.log("Collect " + std::to_string((int)target) + " points!"); }
    void add(GameContext& ctx, float points) {
        score = std::max(0.f, score + points);
        ctx.log("Score: " + std::to_string((int)score) + " / " + std::to_string((int)target));
        if (!won && score >= target) {
            won = true;
            ctx.log("You win!");
            ctx.playSound2D("Beep");
        }
    }
};

ScoreKeeper* scoreKeeper(GameContext& ctx) {
    for (Entity& e : ctx.scene().entities)
        if (ScoreKeeper* s = e.behaviour<ScoreKeeper>()) return s;
    return nullptr;
}

struct Coin : BehaviourBase<Coin> {
    static constexpr const char* kTypeName = "Coin";
    float points = 1.f;
    float spin = 180.f;                 // degrees per second
    std::string collector = "Player";   // the entity that picks it up (through the coin's trigger collider)
    bool taken = false;

    void fields(FieldVisitor& v) override {
        v.field("points", points, 0.1f, 0.f, 100.f);
        v.field("spin", spin, 1.f);
        v.field("collector", collector);
    }
    void update(GameContext& ctx, Entity& self) override { self.transform.rotation[1] += spin * ctx.dt(); }
    void onTriggerEnter(GameContext& ctx, Entity& self, Entity& other) override {
        if (taken || other.name != collector) return;
        taken = true;
        if (ScoreKeeper* s = scoreKeeper(ctx)) s->add(ctx, points);
        ctx.playSound("Pickup", ctx.worldPosition(self.id));
        ctx.destroy(self.id);
    }
};

struct Chaser : BehaviourBase<Chaser> {
    static constexpr const char* kTypeName = "Chaser";
    std::string target = "Player";
    float speed = 3.f;      // metres per second
    float range = 14.f;     // starts chasing within this distance
    float penalty = 1.f;    // points lost when it catches the target
    float cooldown = 0.f;   // runtime: seconds until it can catch again

    void fields(FieldVisitor& v) override {
        v.field("target", target);
        v.field("speed", speed, 0.05f, 0.f, 50.f);
        v.field("range", range, 0.1f, 0.f, 500.f);
        v.field("penalty", penalty, 0.1f, 0.f, 100.f);
    }
    void update(GameContext& ctx, Entity& self) override {
        cooldown = std::max(0.f, cooldown - ctx.dt());
        Entity* t = ctx.find(target);
        if (!t) return;
        Vec3 to = ctx.worldPosition(t->id) - ctx.worldPosition(self.id);
        to.y = 0.f;
        float d = length(to);
        if (d > range || d < 1e-3f) return;
        if (d > 0.9f) ctx.moveCharacter(self, to * (speed * ctx.dt() / d));
        if (d < 1.3f && cooldown <= 0.f) {
            cooldown = 1.5f;
            ctx.playSound("Thud", ctx.worldPosition(self.id));
            if (ScoreKeeper* s = scoreKeeper(ctx)) s->add(ctx, -penalty);
        }
    }
};

}  // namespace

VKWORLD_GAME_MODULE() {
    registerBehaviour<ScoreKeeper>("Keeps the score; the game is won at the target");
    registerBehaviour<Coin>("Spins; adds points to the score when the collector touches its trigger");
    registerBehaviour<Chaser>("Homes in on the target when close and costs it points on contact");
}
