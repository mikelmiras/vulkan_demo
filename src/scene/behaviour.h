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

#include <memory>
#include <string>
#include <vector>

#include "core/vecmath.h"

struct Entity;
class GameContext;   // game/game.h

// Visits a behaviour's parameters by name: the same fields() function drives the inspector UI, saving and loading.
// Names are camelCase keys ("moveSpeed"); the inspector shows them as "Move speed".
class FieldVisitor {
public:
    virtual ~FieldVisitor() = default;
    virtual void field(const char* name, float& v, float speed = 0.05f, float min = 0.f, float max = 0.f) = 0;
    virtual void field(const char* name, bool& v) = 0;
    virtual void field(const char* name, Vec3& v, float speed = 0.05f) = 0;
    virtual void field(const char* name, std::string& v) = 0;
};

class Behaviour {
public:
    virtual ~Behaviour() = default;
    virtual const char* typeName() const = 0;
    virtual std::unique_ptr<Behaviour> clone() const = 0;

    virtual void fields(FieldVisitor&) {}
    virtual void start(GameContext&, Entity& /*self*/) {}    // when Play is pressed
    virtual void update(GameContext&, Entity& /*self*/) {}   // every fixed update; ctx.dt() is the step length
    // A trigger collider started / stopped overlapping another collider. Called on the behaviours of both entities
    // (`self` is the entity the behaviour is on), after every update() of the step.
    virtual void onTriggerEnter(GameContext&, Entity& /*self*/, Entity& /*other*/) {}
    virtual void onTriggerExit(GameContext&, Entity& /*self*/, Entity& /*other*/) {}
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

// ---- Registry: behaviour types by name (for the inspector's "Add component" menu and for loading scenes)
using BehaviourFactory = std::unique_ptr<Behaviour> (*)();
struct BehaviourType {
    std::string name;
    std::string description;
    BehaviourFactory create;
};

void registerBehaviourType(const char* name, const char* description, BehaviourFactory create);
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
