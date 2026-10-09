// Scene model: entities with a transform, an optional parent, and optional components.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "assets/meshes.h"
#include "core/vecmath.h"
#include "scene/behaviour.h"

using EntityId = uint32_t;
constexpr EntityId kNoEntity = 0;

// Position / rotation / scale relative to the parent (or the world for root entities)
struct Transform {
    float position[3] = {0, 0, 0};
    float rotation[3] = {0, 0, 0};   // Euler degrees (ImGuizmo convention)
    float scale[3] = {1, 1, 1};

    Vec3 pos() const { return {position[0], position[1], position[2]}; }
    Mat4 matrix() const;
    void setMatrix(const Mat4& m);   // decomposes (shear from non-uniform parent scale is lost)
};

// ---- Components -----------------------------------------------------------------------------
struct MeshRenderer {
    int mesh = (int)MeshKind::Cube;   // id in the MeshLibrary
    Vec3 color{0.8f, 0.8f, 0.8f};     // sRGB, as shown in the colour picker
    bool checker = false;             // procedural checkerboard + grid (used by the ground)
};

// The game renders through the first entity with a camera, looking down its local -Z axis.
struct Camera {
    float fovY = 60.f * DEG2RAD;   // vertical field of view, radians
    float zNear = 0.1f, zFar = 2000.f;
};

struct Entity {
    EntityId id = kNoEntity;
    std::string name;
    EntityId parent = kNoEntity;
    bool visible = true;    // also hides the children
    bool locked = false;    // not pickable by clicking in the viewport

    Transform transform;
    std::optional<MeshRenderer> meshRenderer;
    std::optional<Camera> camera;
    std::vector<BehaviourPtr> behaviours;   // game logic, run in play mode (see scene/behaviour.h)

    template <class T> T* behaviour() {   // first behaviour of type T, or nullptr
        for (BehaviourPtr& b : behaviours)
            if (auto* t = dynamic_cast<T*>(b.get())) return t;
        return nullptr;
    }
};

class Scene {
public:
    std::vector<Entity> entities;   // flat; the hierarchy is given by Entity::parent

    Entity& create(const std::string& name, EntityId parent = kNoEntity);   // appended with a fresh id
    Entity& add(Entity e);   // keeps e.id if it's free, otherwise assigns a new one

    int indexOf(EntityId id) const;   // -1 if missing
    Entity* find(EntityId id);
    const Entity* find(EntityId id) const;
    Entity* findByName(const std::string& name);

    std::vector<Mat4> worldMatrices() const;      // for every entity, aligned with `entities`
    Mat4 worldMatrix(EntityId id) const;
    Mat4 parentWorldMatrix(EntityId id) const;    // identity for roots

    bool isAncestor(EntityId ancestor, EntityId id) const;   // true if `ancestor` is above `id` in the tree
    bool visibleInHierarchy(EntityId id) const;              // own flag and all ancestors' flags
    std::vector<EntityId> childrenOf(EntityId parent) const; // in vector order; kNoEntity = roots
    std::vector<EntityId> subtree(EntityId root) const;      // root first, then descendants (depth first)

    // Reparents; keepWorld keeps the entity where it is on screen. Refuses cycles. Returns false if refused.
    bool setParent(EntityId id, EntityId newParent, bool keepWorld = true);
    void remove(EntityId id);           // with all descendants
    EntityId duplicate(EntityId id);    // deep copy of the subtree with fresh ids, same parent; returns the copy

    std::string uniqueName(const std::string& wanted) const;   // "Cube.001" -> "Cube.002" if taken, Blender style

private:
    EntityId nextId_ = 1;
};

Scene buildDefaultScene();

// Spin (rad/s around the parent's Y) and Bob (amplitude, phase in radians) behaviours: what the old Animator
// component did. Used by the default scene and to load files from before behaviours existed.
std::vector<BehaviourPtr> motionBehaviours(float spin, float bob, float phase);
