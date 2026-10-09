// Scene model: entities with a transform, an optional parent, and optional components.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "assets/meshes.h"
#include "assets/textures.h"
#include "core/vecmath.h"
#include "scene/behaviour.h"
#include "scene/input_bindings.h"

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
    Vec3 color{0.8f, 0.8f, 0.8f};     // sRGB, as shown in the colour picker; multiplies the texture
    int texture = -1;                 // id in the TextureLibrary, or -1 for plain colour
    float tiling = 1.f;               // texture repeats per unit of texture coordinate
    bool checker = false;             // procedural checkerboard + grid (used by the ground)
};

// Lights the scene. Without any directional light the renderer uses a default sun.
struct Light {
    enum class Type { Directional, Point };
    Type type = Type::Point;
    Vec3 color{1.f, 0.95f, 0.85f};   // sRGB
    float intensity = 1.f;
    float range = 15.f;               // point lights fade to nothing at this distance
    bool castShadows = true;          // directional lights (the first one casts the shadow map)
};
const char* lightTypeName(Light::Type type);

// The game renders through the first entity with a camera, looking down its local -Z axis.
struct Camera {
    float fovY = 60.f * DEG2RAD;   // vertical field of view, radians
    float zNear = 0.1f, zFar = 2000.f;
};

// Collision shape in the entity's local space (scaled with it). Solid colliders block characters and rays;
// triggers only report overlaps (Behaviour::onTriggerEnter / onTriggerExit).
struct Collider {
    enum class Shape { Box, Sphere, Capsule, Mesh };
    Shape shape = Shape::Box;
    Vec3 center{0, 0, 0};
    Vec3 size{1, 1, 1};     // box: full size
    float radius = 0.5f;    // sphere, capsule
    float height = 2.f;     // capsule: total height along local Y, including the caps
    bool trigger = false;
};
const char* colliderShapeName(Collider::Shape shape);
// Box for cubes and planes (a slab under the plane, so things can't fall through it), sphere for spheres,
// mesh for everything else
Collider fitCollider(const MeshLibrary& meshes, int mesh);

// Marks an entity as part of a copy ("instance") of a prefab
struct PrefabLink {
    uint32_t prefab = 0;           // Prefab::id
    EntityId source = kNoEntity;   // the entity in the prefab this one was copied from
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
    std::optional<Collider> collider;
    std::optional<Light> light;
    std::optional<PrefabLink> prefab;
    std::vector<BehaviourPtr> behaviours;   // game logic, run in play mode (see scene/behaviour.h)

    template <class T> T* behaviour() {   // first behaviour of type T, or nullptr
        for (BehaviourPtr& b : behaviours)
            if (auto* t = dynamic_cast<T*>(b.get())) return t;
        return nullptr;
    }
};

// A reusable entity tree: a template the editor and game code (GameContext::spawn) make copies of.
// entities[0] is the root (no parent); parents of the others refer to ids inside the prefab.
struct Prefab {
    uint32_t id = 0;   // stable (names can change): what instances link to
    std::string name;
    std::vector<Entity> entities;
};

class Scene {
public:
    std::vector<Entity> entities;   // flat; the hierarchy is given by Entity::parent
    std::vector<Prefab> prefabs;    // saved with the scene
    std::vector<InputAction> inputActions = defaultInputActions();   // the game's controls

    Entity& create(const std::string& name, EntityId parent = kNoEntity);   // appended with a fresh id
    Entity& add(Entity e);   // keeps e.id if it's free, otherwise assigns a new one

    int indexOf(EntityId id) const;   // -1 if missing
    Entity* find(EntityId id);
    const Entity* find(EntityId id) const;
    Entity* findByName(const std::string& name);
    const Entity* findByName(const std::string& name) const;

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

    // ---- Prefabs
    Prefab& addPrefab(Prefab p);   // keeps p.id if it's free, otherwise assigns one
    Prefab* findPrefab(const std::string& name);
    const Prefab* findPrefab(const std::string& name) const;
    Prefab* findPrefab(uint32_t id);
    const Prefab* findPrefab(uint32_t id) const;
    // Copies the subtree under `root` into a prefab (the root's position is zeroed: instances are placed anew).
    // The copies lose any prefab links of their own.
    Prefab makePrefab(EntityId root, const std::string& name) const;
    // Copies of the prefab's entities with fresh ids, linked to the prefab, the root under `parent`. Not added to
    // the scene: pass them to add() (in order). Ids are reserved, so they stay unique even if added later.
    std::vector<Entity> instantiate(const Prefab& prefab, EntityId parent = kNoEntity);
    EntityId reserveId() { return nextId_++; }
    std::string uniquePrefabName(const std::string& wanted) const;

private:
    EntityId nextId_ = 1;
    uint32_t nextPrefabId_ = 1;
};

Scene buildDefaultScene();

// A glTF model as a prefab: an empty root named after the file, then its node tree with mesh renderers,
// materials and textures. `warning` gets non-fatal problems (images that couldn't be loaded).
bool importGltfPrefab(const std::string& path, MeshLibrary& meshes, TextureLibrary& textures, Prefab& out,
                      std::string& error, std::string& warning);

// Spin (rad/s around the parent's Y) and Bob (amplitude, phase in radians) behaviours: what the old Animator
// component did. Used by the default scene and to load files from before behaviours existed.
std::vector<BehaviourPtr> motionBehaviours(float spin, float bob, float phase);
