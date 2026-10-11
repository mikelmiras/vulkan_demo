// Scene model: entities with a transform, an optional parent, and optional components.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
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
    // Physically based material (glTF metallic-roughness)
    float metallic = 0.f;             // 0 = non-metal (plastic, wood, stone), 1 = metal
    float roughness = 0.5f;           // 0 = mirror smooth, 1 = matte
    int metalRoughTexture = -1;       // glTF layout: green = roughness, blue = metallic (times the values above)
    int normalTexture = -1;           // tangent-space normal map
    float normalScale = 1.f;          // how strongly it bends the surface
    Vec3 emissive{0, 0, 0};           // sRGB colour of the light it gives off (black: none)
    float emissiveStrength = 1.f;     // above 1 it glows (bloom)
    int emissiveTexture = -1;         // times `emissive`
    // Transparency (glTF alpha modes): opacity times the texture's alpha
    enum class AlphaMode { Opaque, Mask, Blend };
    AlphaMode alphaMode = AlphaMode::Opaque;
    float opacity = 1.f;              // Blend: how much it hides what's behind (0..1)
    float alphaCutoff = 0.5f;         // Mask: below this it's a hole
};
const char* alphaModeName(MeshRenderer::AlphaMode mode);

// Lights the scene. Without any directional light the renderer uses a default sun.
struct Light {
    enum class Type { Directional, Point, Spot };
    Type type = Type::Point;
    Vec3 color{1.f, 0.95f, 0.85f};   // sRGB
    float intensity = 1.f;
    float range = 15.f;               // point and spot lights fade to nothing at this distance
    float spotAngle = 30.f;           // spot lights: half-angle of the cone, degrees (along the entity's -Z)
    float spotSoftness = 0.2f;        // spot lights: how much of the cone's edge fades out (0..1)
    bool castShadows = true;          // the sun (the first directional light) and up to 4 spot lights
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

// Simulated motion for an entity with a (solid) collider (Jolt Physics, see physics/physics.h): gravity,
// bouncing, sliding, tumbling, stacking, being pushed and knocked.
struct RigidBody {
    float mass = 1.f;
    bool lockRotation = false;   // moves but never turns (upright characters, say)
    bool useGravity = true;
    float bounciness = 0.3f;   // 0 = dead stop, 1 = keeps all its speed
    float friction = 0.4f;
    float damping = 0.05f;     // air drag: fraction of the speed lost per second
    Vec3 velocity;             // the starting velocity; changes while playing
    Vec3 angularVelocity;      // radians per second around each axis (world space)
};
// Box for cubes and planes (a slab under the plane, so things can't fall through it), sphere for spheres,
// mesh for everything else
Collider fitCollider(const MeshLibrary& meshes, int mesh);

// Deforms the entity's (skinned) mesh with a skeleton: the joint entities, found by name under the model's root
// (the nearest ancestor with an Animator, else the topmost one). Imported with glTF models.
struct Skin {
    std::vector<std::string> joints;
    std::vector<Mat4> inverseBind;   // per joint: from the mesh's space to the joint's, in the bind pose
};

// A named moment in a clip: when playback passes it, the behaviours on the Animator's entity get
// onAnimationEvent(name) (footsteps, the frame a sword hits)
struct AnimationEvent {
    std::string clip;
    float time = 0.f;   // seconds into the clip
    std::string name;
};

// A state machine for an Animator: states play clips; a transition moves to another state when a parameter (set
// by game code with setParameter) meets its condition, or when a clip that doesn't loop has finished.
struct AnimatorState {
    std::string name, clip;
    float speed = 1.f;
    bool loop = true;
};
struct AnimatorTransition {
    enum class When { Greater, Less, IsTrue, IsFalse, ClipEnded };
    std::string from = "*";   // a state, or "*" for any
    std::string to;
    std::string parameter;
    When when = When::Greater;
    float value = 0.f;
    float fade = 0.2f;        // seconds of cross-fade
};

const char* animatorConditionName(AnimatorTransition::When when);   // "greater", "clip ended", ...
AnimatorTransition::When animatorConditionFromName(const std::string& name);

// Plays animation clips (MeshLibrary::animations) on the entities under it, matched by name: a model's skeleton
// or any named children. Clips cross-fade when play() switches them. With states, a state machine picks the clip.
struct Animator {
    std::string clip;            // what plays (by name)
    float speed = 1.f;
    bool loop = true;
    bool playOnStart = true;     // starts with Play; otherwise it waits for play()
    std::vector<AnimationEvent> events;
    std::vector<AnimatorState> states;          // empty: no state machine, just `clip`
    std::vector<AnimatorTransition> transitions;
    std::string startState;                     // empty: the first state
    // Runtime (saved in save games, not scene files)
    bool playing = false;
    float time = 0.f;
    std::string fadeFrom;        // the clip being faded out, if any
    float fadeFromTime = 0.f, fade = 0.f, fadeLength = 0.f;
    std::string state;           // the state machine's current state
    std::vector<std::pair<std::string, float>> parameters;   // set by game code (bools are 0 / 1)

    void setParameter(const std::string& name, float value) {
        for (auto& p : parameters)
            if (p.first == name) { p.second = value; return; }
        parameters.emplace_back(name, value);
    }
    float parameter(const std::string& name) const {
        for (const auto& p : parameters)
            if (p.first == name) return p.second;
        return 0.f;
    }

    void play(const std::string& name, float fadeSeconds = 0.2f, bool looping = true) {
        if (playing && name == clip) return;
        if (playing && fadeSeconds > 0.f && !clip.empty()) {
            fadeFrom = clip;
            fadeFromTime = time;
            fade = 0.f;
            fadeLength = fadeSeconds;
        } else {
            fadeFrom.clear();
        }
        clip = name;
        time = 0.f;
        loop = looping;
        playing = true;
    }
    void stop() { playing = false; fadeFrom.clear(); }
};

// Emits particles (sparks, smoke, embers, sparkles): camera-facing glowing or see-through dots that rise, fall and
// fade. They go out along the entity's +Y axis (in a cone) and stay where they were emitted.
struct ParticleEmitter {
    float rate = 20.f;              // particles per second (0: only bursts)
    int burst = 0;                  // all at once when it starts, and on ctx.emitParticles(id)
    bool playOnStart = true;        // emits from Play (otherwise only when game code asks)
    float duration = 0.f;           // seconds of emitting (0: for ever)
    float lifetime = 1.5f;          // seconds a particle lasts
    float lifetimeRandom = 0.3f;    // +- this fraction
    float speed = 2.f;              // metres per second, out along +Y
    float speedRandom = 0.5f;       // +- this fraction
    float spread = 25.f;            // half-angle of the cone, degrees (180: every direction)
    float radius = 0.f;             // emitted from anywhere inside a sphere this big
    float gravity = 0.f;            // pulls them down, m/s^2 (negative: they rise faster)
    float drag = 0.f;               // fraction of the speed lost per second
    float startSize = 0.2f, endSize = 0.05f;      // metres, over its life
    Vec3 startColor{1.f, 0.8f, 0.3f};             // sRGB, over its life
    Vec3 endColor{1.f, 0.25f, 0.05f};
    float startAlpha = 1.f, endAlpha = 0.f;
    float intensity = 1.f;          // brightness: above 1 glows (bloom)
    bool additive = true;           // light adds up (fire, sparks); else see-through (smoke, dust)
    int maxParticles = 500;
};

// Marks an entity as part of a copy ("instance") of a prefab
struct PrefabLink {
    uint32_t prefab = 0;           // Prefab::id
    EntityId source = kNoEntity;   // the entity in the prefab this one was copied from
};

// Plays a sound clip from the entity: a built-in name ("Hum", "Bounce", ...) or a sound file path
struct AudioSource {
    std::string clip = "Hum";
    float volume = 1.f;
    bool loop = true;
    bool playOnStart = true;      // when Play is pressed (a looping one follows the entity around)
    bool playOnImpact = false;    // when its rigid body hits something (louder for harder hits)
    bool spatial = true;          // fades with distance from the listener and pans
    float minDistance = 2.f, maxDistance = 40.f;
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
    std::optional<RigidBody> rigidBody;
    std::optional<AudioSource> audioSource;
    std::optional<Skin> skin;
    std::optional<Animator> animator;
    std::optional<ParticleEmitter> particleEmitter;
    std::optional<PrefabLink> prefab;
    std::vector<BehaviourPtr> behaviours;   // game logic, run in play mode (see scene/behaviour.h)

    template <class T> T* behaviour() {   // first behaviour of type T, or nullptr
        for (BehaviourPtr& b : behaviours)
            if (auto* t = dynamic_cast<T*>(b.get())) return t;
        return nullptr;
    }
};

// Where agents can walk (game/navigation.h): baked from the scene's static colliders for an agent of this size
struct NavigationSettings {
    float cellSize = 0.4f;      // the grid's resolution, m: walls thinner than this may be walked through
    float agentRadius = 0.4f;   // keeps paths this far from walls and edges
    float agentHeight = 1.8f;   // headroom needed
    float maxClimb = 0.45f;     // highest step up (or down) between neighbouring cells
    float maxSlope = 45.f;      // steepest walkable surface, degrees
};

// How the scene looks: the renderer's per-scene settings (saved with the scene)
struct RenderSettings {
    Vec3 sky{0.62f, 0.75f, 0.90f};    // sRGB: the sky at the horizon, and the fog
    Vec3 zenith{0.36f, 0.56f, 0.88f}; // sRGB: the sky overhead
    Vec3 ground{0.34f, 0.35f, 0.31f}; // sRGB: below the horizon (and light bounced up from the ground)
    float fogDensity = 0.006f;
    float ambient = 0.45f;            // light that comes from everywhere
    float exposure = 1.f;             // brightness before tonemapping
    float bloomStrength = 0.06f;      // how much bright parts glow (0: off)
    float bloomThreshold = 1.f;       // brightness (linear) where the glow starts
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
    RenderSettings render;
    NavigationSettings navigation;

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
    void remove(const std::vector<EntityId>& ids);   // several at once (with descendants), in one pass
    EntityId duplicate(EntityId id);    // deep copy of the subtree with fresh ids, same parent; returns the copy

    std::string uniqueName(const std::string& wanted) const;   // "Cube.001" -> "Cube.002" if taken, Blender style
    // Where a Skin's joints and an Animator's targets are looked up: the nearest ancestor (or the entity itself)
    // with an Animator, else the topmost ancestor
    EntityId modelRoot(EntityId id) const;
    // Name -> entity for the root and everything under it (the first one wins for repeated names)
    std::unordered_map<std::string, EntityId> namesUnder(EntityId root) const;

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
    // id -> position in `entities`. `entities` is public and may be changed directly, so this is only a hint:
    // every hit is checked, and a stale entry (or a miss that a scan finds) rebuilds it.
    mutable std::unordered_map<EntityId, uint32_t> index_;
    void rebuildIndex() const;

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
