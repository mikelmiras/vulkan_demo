#include "scene/behaviour.h"

#include <type_traits>

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

namespace {

// Assigns `value` to the field called `name` if it has type T
template <class T>
struct FieldSetter : FieldVisitor {
    const std::string& name;
    const T& value;
    bool found = false;
    FieldSetter(const std::string& n, const T& v) : name(n), value(v) {}
    template <class U>
    void set(const char* n, U& v) {
        if constexpr (std::is_same_v<T, U>)
            if (name == n) { v = value; found = true; }
    }
    void field(const char* n, float& v, float, float, float) override { set(n, v); }
    void field(const char* n, bool& v) override { set(n, v); }
    void field(const char* n, Vec3& v, float) override { set(n, v); }
    void field(const char* n, std::string& v) override { set(n, v); }
};

}  // namespace

bool setField(Behaviour& b, const std::string& name, float value) {
    FieldSetter<float> setter(name, value);
    b.fields(setter);
    return setter.found;
}

bool setField(Behaviour& b, const std::string& name, const std::string& value) {
    FieldSetter<std::string> setter(name, value);
    b.fields(setter);
    return setter.found;
}
