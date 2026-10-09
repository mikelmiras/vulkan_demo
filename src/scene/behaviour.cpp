#include "scene/behaviour.h"

namespace {

std::vector<BehaviourType>& registry() {
    static std::vector<BehaviourType> types;
    static bool builtinsDone = false;
    if (!builtinsDone) {   // explicit, not static-initialiser registration: the linker drops unreferenced objects
        builtinsDone = true;
        registerBuiltinBehaviours();
    }
    return types;
}

}  // namespace

void registerBehaviourType(const char* name, const char* description, BehaviourFactory create) {
    std::vector<BehaviourType>& types = registry();
    for (BehaviourType& t : types)
        if (t.name == name) {   // re-registering replaces (lets a game override a built-in)
            t = {name, description, create};
            return;
        }
    types.push_back({name, description, create});
}

const std::vector<BehaviourType>& behaviourTypes() { return registry(); }

std::unique_ptr<Behaviour> createBehaviour(const std::string& typeName) {
    for (const BehaviourType& t : registry())
        if (t.name == typeName) return t.create();
    return nullptr;
}

bool setField(Behaviour& b, const std::string& name, float value) {
    struct Setter : FieldVisitor {
        const std::string& name;
        float value;
        bool found = false;
        Setter(const std::string& n, float v) : name(n), value(v) {}
        void field(const char* n, float& v, float, float, float) override {
            if (name == n) { v = value; found = true; }
        }
        void field(const char*, bool&) override {}
        void field(const char*, Vec3&, float) override {}
        void field(const char*, std::string&) override {}
    } setter(name, value);
    b.fields(setter);
    return setter.found;
}
