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
#include "game/ui.h"

namespace {

struct ScoreKeeper : BehaviourBase<ScoreKeeper> {
    static constexpr const char* kTypeName = "ScoreKeeper";
    float target = 10.f;     // points (in total, over all levels so far) to finish this level
    std::string nextScene;   // where to go then; empty: the game is won
    float wonAt = -1.f;      // runtime state: when the target was reached

    void fields(FieldVisitor& v) override {
        v.field("target", target, 1.f, 1.f, 1000.f);
        v.field("nextScene", nextScene, FieldRef::Scene);
    }
    void state(FieldVisitor& v) override { v.field("wonAt", wonAt); }
    // The score lives in the game state, so it carries over to the next level (and into save games)
    void start(GameContext& ctx, Entity&) override {
        ctx.state().add("level", 1.f);
        ctx.state().setNumber("target", target);   // for the HUD: "Score {score} / {target}"
        if (!ctx.state().has("score")) ctx.state().setNumber("score", 0.f);
    }
    // The game's own UI (see game/ui.h): a banner when the level is done. The score line, the bar and the pause
    // menu are built-in HUD behaviours on the same entity.
    void drawUI(GameContext& ctx, Entity&, GameUI& ui) override {
        float shown = ctx.time() - wonAt;
        if (wonAt < 0.f || shown > 3.f) return;
        float alpha = std::min(1.f, 3.f - shown);
        ui.rect(0.f, 380.f, ui.width(), 200.f, {0.05f, 0.05f, 0.1f, 0.6f * alpha});
        ui.text(ui.width() * 0.5f, 420.f, nextScene.empty() ? ctx.text("game.won", "You win!") : ctx.text("game.complete", "Level complete!"), 110.f,
                {1.f, 0.85f, 0.3f, alpha}, GameUI::Align::Center);
    }
    void update(GameContext& ctx, Entity&) override {
        if (wonAt >= 0.f && ctx.time() - wonAt > 2.f && !nextScene.empty()) {
            wonAt = -1.f;
            ctx.loadScene(nextScene);
        }
    }
    void add(GameContext& ctx, float points) {
        float score = std::max(0.f, ctx.state().number("score") + points);
        ctx.state().setNumber("score", score);
        if (wonAt < 0.f && score >= target) {
            wonAt = ctx.time();
            ctx.log(nextScene.empty() ? "You win!" : "Level complete!");
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
        v.field("collector", collector, FieldRef::Entity);
    }
    void update(GameContext& ctx, Entity& self) override { self.transform.rotation[1] += spin * ctx.dt(); }
    void onTriggerEnter(GameContext& ctx, Entity& self, Entity& other) override {
        if (taken || other.name != collector) return;
        taken = true;
        if (ScoreKeeper* s = scoreKeeper(ctx)) s->add(ctx, points);
        ctx.playSound("Pickup", ctx.worldPosition(self.id));
        ParticleEmitter sparkle;   // gold sparkles, no component needed
        sparkle.spread = 180.f;
        sparkle.speed = 3.f;
        sparkle.gravity = 5.f;
        sparkle.lifetime = 0.7f;
        sparkle.startSize = 0.18f;
        sparkle.startColor = {1.f, 0.9f, 0.4f};
        sparkle.intensity = 4.f;
        ctx.emitParticles(sparkle, ctx.worldPosition(self.id), 24);
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
    std::vector<Vec3> path; // runtime: the way to the target (around the blocks), and when to look again
    size_t next = 0;
    float repathIn = 0.f;

    void fields(FieldVisitor& v) override {
        v.field("target", target, FieldRef::Entity);
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
        // Along a path from the scene's navigation grid, so it goes around obstacles instead of into them
        if ((repathIn -= ctx.dt()) <= 0.f || next >= path.size()) {
            repathIn = 0.5f;
            next = 1;
            if (!ctx.findPath(ctx.worldPosition(self.id), ctx.worldPosition(t->id), path)) path.clear();
        }
        Vec3 step = to;
        while (next < path.size()) {
            step = path[next] - ctx.worldPosition(self.id);
            step.y = 0.f;
            if (length(step) > 0.1f || next + 1 == path.size()) break;
            ++next;
        }
        if (d > 0.9f && length(step) > 1e-4f) ctx.moveCharacter(self, step * (speed * ctx.dt() / length(step)));
        if (d < 1.3f && cooldown <= 0.f) {
            cooldown = 1.5f;
            ctx.playSound("Thud", ctx.worldPosition(self.id));
            if (ScoreKeeper* s = scoreKeeper(ctx)) s->add(ctx, -penalty);
        }
    }
};

// Animation from code: on an entity with an Animator, plays one clip while the target is far and another (cross-
// faded) when it comes close
struct Lurker : BehaviourBase<Lurker> {
    static constexpr const char* kTypeName = "Lurker";
    std::string target = "Player";
    float range = 6.f;
    std::string idleClip = "tentacle/Bend";
    std::string alertClip = "tentacle/Stretch";

    void fields(FieldVisitor& v) override {
        v.field("target", target, FieldRef::Entity);
        v.field("range", range, 0.1f, 0.f, 100.f);
        v.field("idleClip", idleClip, FieldRef::AnimationClip);
        v.field("alertClip", alertClip, FieldRef::AnimationClip);
    }
    void update(GameContext& ctx, Entity& self) override {
        Entity* t = ctx.find(target);
        if (!t || !self.animator) return;
        bool close = length(ctx.worldPosition(t->id) - ctx.worldPosition(self.id)) < range;
        self.animator->play(close ? alertClip : idleClip, 0.4f);   // no-op while that clip already plays
    }
};

}  // namespace

VKWORLD_GAME_MODULE() {
    registerBehaviour<ScoreKeeper>("Keeps the score; the game is won at the target");
    registerBehaviour<Coin>("Spins; adds points to the score when the collector touches its trigger");
    registerBehaviour<Chaser>("Homes in on the target when close and costs it points on contact");
    registerBehaviour<Lurker>("Switches its Animator between two clips depending on how close the target is");
}
