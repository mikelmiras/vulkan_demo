// Behaviours: game logic attached to entities as components.
//
// A behaviour is a small C++ class with editable parameters ("fields") and start/update callbacks:
//
//   struct Hover : BehaviourBase<Hover> {
//       static constexpr const char* kTypeName = "Hover";
//       float height = 1.f;                                     // a field: edited in the inspector, saved in scenes
//       void fields(FieldVisitor& v) override { v.field("Height", height); }
//       void update(GameContext& ctx, Entity& self) override { ... }   // every fixed update (60 Hz) in play mode
//   };
//   registerBehaviour<Hover>("Floats up and down");             // once at startup, before scenes are loaded
//
// Behaviours are cloned with their entity (play mode runs on a copy of the scene, undo keeps copies), so any
// state they keep in member variables is per play session and starts fresh on every Play.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "core/vecmath.h"

struct Entity;
class GameContext;   // game/game.h
class GameUI;        // game/ui.h

// What a string field names, so the inspector offers a picker instead of a text box. References are by name: an
// entity's name (found with GameContext::findByName / findEntity, so it also matches entities spawned later), a
// prefab's, a scene file (stored relative to the project), a sound (a built-in clip or a file), an animation clip,
// an input action.
enum class FieldRef { Entity, Prefab, Scene, Sound, AnimationClip, InputAction };

// Visits a behaviour's parameters by name: the same fields() function drives the inspector UI, saving and loading.
// Names are camelCase keys ("moveSpeed"); the inspector shows them as "Move speed".
//
//   v.field("speed", speed, 0.05f, 0.f, 10.f);           // float: drag speed, range (min < max clamps)
//   v.field("count", count, 0, 20);                      // int, with a range
//   v.field("align", align, {"left", "center", "right"}); // int choosing one of these (saved by name)
//   v.field("target", target, FieldRef::Entity);         // string naming an entity (a picker)
class FieldVisitor {
public:
    virtual ~FieldVisitor() = default;
    virtual void field(const char* name, float& v, float speed = 0.05f, float min = 0.f, float max = 0.f) = 0;
    virtual void field(const char* name, bool& v) = 0;
    virtual void field(const char* name, Vec3& v, float speed = 0.05f) = 0;
    virtual void field(const char* name, std::string& v) = 0;
    // The typed ones: visitors that don't know them see a float / a string
    virtual void field(const char* name, int& v, int min = 0, int max = 0) {
        float f = (float)v;
        field(name, f, 1.f, (float)min, (float)max);
        v = (int)(f < 0.f ? f - 0.5f : f + 0.5f);
    }
    virtual void choice(const char* name, int& v, const char* const* /*options*/, int count) {
        field(name, v, 0, count > 0 ? count - 1 : 0);
    }
    void field(const char* name, int& v, std::initializer_list<const char*> options) {
        choice(name, v, options.begin(), (int)options.size());
    }
    virtual void field(const char* name, std::string& v, FieldRef /*kind*/) { field(name, v); }
    // Entity ids (runtime state): stored as numbers, exact up to 2^24
    void field(const char* name, uint32_t& id) {
        float f = (float)id;
        field(name, f, 1.f);
        id = f >= 0.f ? (uint32_t)(f + 0.5f) : 0u;
    }
};

class Behaviour {
public:
    virtual ~Behaviour();   // out of line: the type info lives in the engine, shared with game modules
    virtual const char* typeName() const = 0;
    virtual std::unique_ptr<Behaviour> clone() const = 0;

    virtual void fields(FieldVisitor&) {}
    // Runtime state worth keeping in a save game (what start() and update() build up: timers, counters, where it
    // started), visited like fields() but never shown in the inspector or saved in scene files. A loaded save
    // calls start() first, then restores this.
    virtual void state(FieldVisitor&) {}
    virtual void start(GameContext&, Entity& /*self*/) {}    // when Play is pressed
    virtual void update(GameContext&, Entity& /*self*/) {}   // every fixed update; ctx.dt() is the step length
    // A trigger collider started / stopped overlapping another collider. Called on the behaviours of both entities
    // (`self` is the entity the behaviour is on), after every update() of the step.
    virtual void onTriggerEnter(GameContext&, Entity& /*self*/, Entity& /*other*/) {}
    virtual void onTriggerExit(GameContext&, Entity& /*self*/, Entity& /*other*/) {}
    // Once per rendered frame while playing (also when paused), after the updates: draw the game's interface
    // (see game/ui.h). Skipped for hidden entities, so hiding a HUD entity hides its UI.
    virtual void drawUI(GameContext&, Entity& /*self*/, GameUI&) {}
    // The entity's Animator passed one of its events (see AnimationEvent)
    virtual void onAnimationEvent(GameContext&, Entity& /*self*/, const std::string& /*event*/) {}
};

// CRTP helper: implements typeName() and clone() from T::kTypeName and T's copy constructor
template <class T>
class BehaviourBase : public Behaviour {
public:
    const char* typeName() const override { return T::kTypeName; }
    std::unique_ptr<Behaviour> clone() const override { return std::make_unique<T>(static_cast<const T&>(*this)); }
};

// Owning pointer with value semantics, so entities (and whole scenes) stay copyable
class BehaviourPtr {
public:
    BehaviourPtr() = default;
    explicit BehaviourPtr(std::unique_ptr<Behaviour> b) : p_(std::move(b)) {}
    BehaviourPtr(const BehaviourPtr& o) : p_(o.p_ ? o.p_->clone() : nullptr) {}
    BehaviourPtr(BehaviourPtr&&) noexcept = default;
    BehaviourPtr& operator=(const BehaviourPtr& o) {
        if (this != &o) p_ = o.p_ ? o.p_->clone() : nullptr;
        return *this;
    }
    BehaviourPtr& operator=(BehaviourPtr&&) noexcept = default;

    Behaviour* get() const { return p_.get(); }
    Behaviour* operator->() const { return p_.get(); }
    Behaviour& operator*() const { return *p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    std::unique_ptr<Behaviour> p_;
};

// A behaviour whose type isn't registered (its game module isn't loaded, failed to build, or no longer has it). It
// keeps the type name and the saved fields, so scenes still load and save without losing anything, and does
// nothing when played. reattachBehaviours() (scene/serialization.h) turns it back into the real thing.
class MissingBehaviour : public Behaviour {
public:
    MissingBehaviour(std::string type, std::string fieldsJson) : type_(std::move(type)), json_(std::move(fieldsJson)) {}
    const char* typeName() const override { return type_.c_str(); }
    std::unique_ptr<Behaviour> clone() const override { return std::make_unique<MissingBehaviour>(*this); }
    const std::string& fieldsJson() const { return json_; }   // the saved fields, as a JSON object
private:
    std::string type_, json_;
};

// ---- Registry: behaviour types by name (for the inspector's "Add component" menu and for loading scenes)
using BehaviourFactory = std::unique_ptr<Behaviour> (*)();
struct BehaviourType {
    std::string name;
    std::string description;
    BehaviourFactory create;
    std::string module;   // the game module that registered it ("" for the engine's own)
};

void registerBehaviourType(const char* name, const char* description, BehaviourFactory create);
// Removes every type a module registered; built-in types it had replaced come back
void unregisterModuleBehaviours(const std::string& module);
// Types registered while this is set belong to that module (GameModule sets it around the module's register call)
void setRegisteringModule(const std::string& module);
template <class T>
void registerBehaviour(const char* description) {
    registerBehaviourType(T::kTypeName, description, []() -> std::unique_ptr<Behaviour> { return std::make_unique<T>(); });
}
const std::vector<BehaviourType>& behaviourTypes();
std::unique_ptr<Behaviour> createBehaviour(const std::string& typeName);   // nullptr if unknown

// Set a field by name through fields(); false if the behaviour has no such field of that type
bool setField(Behaviour& b, const std::string& name, float value);
bool setField(Behaviour& b, const std::string& name, const std::string& value);

// The engine's own behaviours (game/builtin_behaviours.cpp); registered automatically on first use of the registry.
void registerBuiltinBehaviours();
