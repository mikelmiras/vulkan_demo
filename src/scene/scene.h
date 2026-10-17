// Scene model: entities with a transform, an optional parent, and optional components.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/meshes.h"
#include "assets/textures.h"
#include "core/json.h"
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
    // Layers over the base (glTF's KHR_materials_clearcoat, _sheen, _anisotropy, simplified)
    float clearcoat = 0.f;            // a clear varnish over it (car paint, lacquer): its own sharp reflection
    float clearcoatRoughness = 0.05f;
    float sheen = 0.f;                // cloth's soft rim of light at grazing angles (velvet, felt), tinted by the colour
    float anisotropy = 0.f;           // -1..1: highlights stretched across (brushed metal), along the texture's u (> 0) or v
    // Light inside the material
    float translucency = 0.f;         // thin things (leaves, paper, curtains): light from behind shows through, tinted
    float subsurface = 0.f;           // thick ones (skin, wax, marble, jade): light spreads under the surface, past the
    Vec3 subsurfaceColor{1.f, 0.35f, 0.2f};   // shadow's edge, in this colour (sRGB)
    // Surface detail
    int heightTexture = -1;           // a height map (white high) for parallax occlusion: the surface looks deep
    float parallaxDepth = 0.f;        // how deep, in texture units (0.02..0.08 suits most; 0: flat)
    int detailTexture = -1;           // a fine normal map tiled over the surface (grain, pores, scratches)
    float detailTiling = 8.f;         // its repeats per repeat of the other textures
    float detailStrength = 1.f;
    bool sheltered = false;           // the weather doesn't wet it (RenderSettings::wetness): indoors, under cover
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
    float size = 0.05f;               // point and spot lights: the bulb's radius (m): their shadows sharp where
                                      // things touch, softer the further from what casts them (0: sharp throughout);
                                      // and its highlights that much broader
    float length = 0.f;               // point lights: a tube this long (m) along the entity's x axis (a neon or
                                      // fluorescent tube: a streak of highlight); 0: a bulb
    bool castShadows = true;          // the sun (the first directional light), spot and point lights (the
                                      // most important get the biggest tiles of the shadow atlas)
    std::string cookie;               // spot lights: an image projected through the cone (its brightness)
};
const char* lightTypeName(Light::Type type);

// The game renders through the first entity with a camera, looking down its local -Z axis.
struct Camera {
    float fovY = 60.f * DEG2RAD;   // vertical field of view, radians
    float zNear = 0.1f, zFar = 2000.f;
    // Split screen: 1-4 makes it that player's camera. With two or more player cameras the screen is shared (two
    // side by side, three or four in quarters, by player number); 0: an ordinary camera.
    int player = 0;
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
    bool kinematic = false;    // moved by animation or game code, not simulated (pushes others, isn't pushed):
                               // a ragdoll's parts until it goes limp
    float buoyancy = 1.2f;     // in Water volumes: 1 neither floats nor sinks, more floats, less sinks
};

// A soft body (physics/soft_body.h): cloth, or the entity's mesh as a squashy volume (jelly, a ball, a cushion).
// Drawn with the entity's MeshRenderer look; its particles collide with everything else.
struct SoftBody {
    enum class Shape { Cloth, Mesh };
    Shape shape = Shape::Cloth;
    float width = 2.f, height = 2.f;   // cloth: a sheet in the entity's XY plane, centred on it
    int resolution = 16;               // cloth: particles along each side
    std::string pinned = "top";        // cloth: "top" edge, "corners" (top two) or "none" held where they start
    float mass = 1.f;                  // all of it, kg
    float stiffness = 0.9f;            // 0 floppy .. 1 hardly stretches
    float bendiness = 0.5f;            // cloth: how freely it folds (0 stiff .. 1 limp)
    float pressure = 0.f;              // mesh: inflation (0: none; a few thousand: a firm ball)
    float damping = 0.1f;
    float friction = 0.5f;
    float tearStretch = 0.f;           // a triangle tears once an edge is this many times its length (0: never); a
                                       // mesh torn open can't hold its pressure
    float buoyancy = 0.f;              // in water (as RigidBody's): 1 hovers, more floats, 0: the water doesn't hold it
    // Runtime (not saved): the drawn mesh, deformed in place; the particles' positions (entity space) as simulated;
    // the cloth's torn triangles (by index, in softbody::build's faces)
    int mesh = -1;
    int meshVertices = 0, meshSource = -1;
    std::vector<Vec3> positions;
    uint64_t positionsVersion = 0, meshVersion = 0;   // (the drawn mesh shows the particles of meshVersion)
    std::vector<uint32_t> torn;
};

// A body of water (scene/water.h): a level surface `width` x `length` (along the entity's x and z, its top at the
// entity's height), `depth` deep, moved by waves (three Gerstner waves from the main one) and flowing with a current.
// What floats in it (RigidBody / SoftBody buoyancy) rides its waves and is carried along; it's drawn see-through and
// reflective, with foam on the crests.
struct Water {
    float width = 20.f, length = 20.f, depth = 4.f;   // m
    float waveHeight = 0.25f;     // the main wave's height above the level, m (0: flat)
    float waveLength = 8.f;       // from one crest to the next, m
    float waveDirection = 0.f;    // where the waves run, degrees round the vertical from the entity's +x
    float choppiness = 0.5f;      // 0 rounded swells .. 1 sharp crests
    Vec3 current{0.f, 0.f, 0.f};  // the water's flow, m/s (world): floating things are carried along
    Vec3 color{0.05f, 0.2f, 0.24f};   // sRGB, its colour in the depths
    float clarity = 0.5f;         // 0 opaque .. 1 clear: how much of what's under shows through
    // Runtime (not saved): the drawn grid (water::syncMeshes)
    int mesh = -1;
};

// Pre-fractured: breaks into pieces (cut from its mesh, the same ones every time: physics/fracture.h) on a hard
// enough impact, when a Destructible on it runs out of hits, or on ctx.shatter()
struct Fracture {
    int pieces = 8;
    uint32_t seed = 1;
    float breakSpeed = 6.f;     // impacts this fast (m/s) break it (0: only ctx.shatter / Destructible)
    float burst = 2.f;          // how fast the pieces fly apart (m/s)
    float pieceLife = 8.f;      // seconds before the pieces are cleared away (0: they stay)
    // Held together: it cracks instead, its pieces joined (fixed joints, each to a touching one) until a force past
    // holdStrength (N) knocks them apart; a held piece is only cleared away once it's off
    bool holdTogether = false;
    float holdStrength = 2000.f;
    // Runtime: the pieces' meshes in the library (made by fracture::syncMeshes)
    std::vector<int> pieceMeshes;
    uint64_t piecesKey = 0;
};

// A wheeled vehicle (physics/physics.h, Jolt's): the entity's rigid body is the chassis (scale 1), its wheels hang
// under it on springs and an engine drives them through an automatic gearbox. Game code (or the built-in
// VehicleDriver behaviour) sets the throttle, steering and brakes; the physics step reads them.
struct Vehicle {
    struct Wheel {
        Vec3 position;               // where its suspension is attached, in the chassis's space
        float radius = 0.4f, width = 0.25f;
        bool steering = false;       // turns with the steering (the front ones)
        bool driven = true;          // the engine drives it
        std::string entity;          // a child that shows it, by name ("": none): moved, steered and spun to match
    };
    std::vector<Wheel> wheels;       // pairs left (x < 0) and right make the axles
    float engineTorque = 500.f;      // N·m at most
    float maxRpm = 6000.f;
    float maxSteer = 30.f;           // degrees
    float suspensionLength = 0.3f;   // travel, m
    float suspensionFrequency = 2.f, suspensionDamping = 0.5f;   // springiness (Hz), 0..1
    // Driving (runtime: set by game code)
    float throttle = 0.f;            // -1..1 (backwards below 0)
    float steering = 0.f;            // -1 left .. 1 right
    float brake = 0.f;               // 0..1
    float handbrake = 0.f;           // 0..1 (the wheels that don't steer)
    // Read back after each step
    float speed = 0.f;               // m/s along its forward (-Z)
    int gear = 0;
};

// Connects the entity's rigid body to another body, or to the world (Jolt constraints). The other body is found
// by name, nearest first (under the same parents: a ragdoll's bones find their own model's).
struct Joint {
    enum class Type { Fixed, Ball, Hinge, Slider, Distance, Cone };
    Type type = Type::Hinge;
    std::string connectedTo;          // "" = the world
    Vec3 anchor;                      // where, in this entity's local space
    Vec3 axis{1, 0, 0};               // hinge and slider axis, a cone's twist axis (local)
    float minLimit = 0.f, maxLimit = 0.f;   // hinge and cone twist: degrees; slider and distance: m (min >= max: free)
    float coneAngle = 45.f;           // cone: how far it swings off its axis, degrees
    float motorSpeed = 0.f;           // hinge: degrees / s, slider: m / s ...
    float motorStrength = 0.f;        // ... with this much torque (N m) / force (N); 0: no motor
    float breakForce = 0.f;           // N: comes apart beyond this (0: never)
    bool collideConnected = false;    // the two bodies collide with each other
    bool broken = false;              // runtime: it came apart
};
const char* jointTypeName(Joint::Type type);

// An off-mesh link for navigation: a way between two walkable spots the grid doesn't join (a jump across a gap, a
// ladder, a drop), from where this entity is to `end` (in its own space). Agents cross it in a hop.
struct NavLink {
    Vec3 end{0.f, 0.f, 2.f};
    bool bothWays = true;
    float cost = 1.f;   // times its length: how much a path avoids it
};
Joint::Type jointTypeFromName(const std::string& name);
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
    // Done by itself when it fires (no code needed: footsteps, swooshes): a sound played at a bone (an entity of
    // that name under the Animator's, e.g. "foot.l"; the entity itself if none) at a volume
    std::string sound;
    std::string bone;
    float volume = 1.f;
    // ... a burst of particles there (this many; -1: the emitter's burst size; 0: none), from the bone's own
    // ParticleEmitter, else the Animator entity's (its look, at the bone)
    int particles = 0;
    // ... and a camera shake (trauma 0..1: 0.3 a footfall, 1 a slam), less the further the camera is (none past 25 m)
    float shake = 0.f;
};

// A state machine for an Animator: states play clips; a transition moves to another state when a parameter (set
// by game code with setParameter) meets its condition, or when a clip that doesn't loop has finished.
// A point of a 1D blend: this clip when the blend parameter is at `at`
struct BlendClip {
    std::string clip;
    float at = 0.f;
    float atY = 0.f;   // 2D blend spaces: where it is along the second parameter
};
struct AnimatorState {
    std::string name, clip;
    float speed = 1.f;
    bool loop = true;
    // A blend tree instead of one clip (when `blend` has points): the two clips either side of the parameter's
    // value, mixed by where it falls between them and kept in step (walk / run by speed)
    std::string blendParameter;
    std::vector<BlendClip> blend;
    // A 2D blend space when set (strafing: speed forward and sideways): the clips are points (at, atY), mixed by how
    // near the two parameters are to each (gradient band interpolation: exactly a clip on its point)
    std::string blendParameterY;
};

// A clip played over the Animator's main one, on part of the skeleton (the `mask` bone and everything under it;
// "" = all), mixed in by `weight`: waving while walking, aiming while running
struct AnimatorLayer {
    std::string name;
    std::string clip;
    std::string mask;
    float weight = 1.f;
    float speed = 1.f;
    bool loop = true;
    // Additive: the clip's change from its first frame is added on top of what's there (a breathing or a recoil over
    // any pose), instead of replacing it
    bool additive = false;
    // runtime
    bool playing = false;
    float time = 0.f;
};

// Inverse kinematics on top of the animation (game/animation.h): a two-bone chain (an arm, a leg: the bone, its
// parent and grandparent) reaching for a point, or a bone turning towards one (a head, a gun arm). The target is an
// entity (by name) or a point game code sets (Animator::setIK), with a weight to fade it in and out.
struct AnimatorIK {
    enum class Type { TwoBone, LookAt };
    std::string name;
    Type type = Type::TwoBone;
    std::string bone;             // two-bone: the end (hand, foot); look-at: the bone that turns
    std::string targetEntity;     // follows this entity ("": the point set by setIK)
    Vec3 pole{0, 0, 1};           // two-bone: which way the middle joint bends, in the Animator's space (knees: forward)
    Vec3 aimAxis{0, 0, 1};        // look-at: the bone's own axis that should point at the target
    float maxAngle = 80.f;        // look-at: turns at most this far from the animation's pose, degrees
    float weight = 1.f;           // 0: just the animation, 1: all the way
    // runtime: the point to reach (world space), set by game code
    Vec3 target;
    bool hasTarget = false;
    Vec3 posed;                   // runtime: where the animation put the end bone (world), before any offset and IK
    bool hasPosed = false;
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
const char* ikTypeName(AnimatorIK::Type type);   // "two-bone", "look-at"
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
    std::vector<AnimatorLayer> layers;
    std::vector<AnimatorIK> ik;   // applied in this order after the animation
    // Root motion: the root bone's travel in the clip (across the ground: x and z) moves the Animator's entity
    // instead of the bone (as a character, with a capsule or sphere collider), so feet don't slide
    bool rootMotion = false;
    std::string rootBone;
    // Runtime (saved in save games, not scene files)
    bool playing = false;
    float time = 0.f;
    std::string fadeFrom;        // the clip being faded out, if any
    float fadeFromTime = 0.f, fade = 0.f, fadeLength = 0.f;
    std::string state;           // the state machine's current state
    std::string fadeFromState;   // the state being faded out (a blend tree is faded out as one)
    Vec3 rootDelta;              // root motion of the last update, in the entity's own space
    std::string offsetBone;      // runtime, set by game code (FootIK): moved by `offset` (world space) after posing,
    Vec3 offset;                 // before IK (hips lowered so legs reach lower ground)
    Vec3 offsetApplied, offsetResult;   // runtime: the last offset applied (local) and where it left the bone
    // runtime: bones IK turned last time (before, after): put back first if nothing has posed them since
    std::vector<std::pair<EntityId, std::pair<Transform, Transform>>> ikTouched;
    // Where nodes only layers move were before any layer did (what a layer's weight mixes with)
    std::vector<std::pair<std::string, Transform>> layerRest;
    std::vector<std::pair<std::string, float>> parameters;   // set by game code (bools are 0 / 1)

    void setParameter(const std::string& name, float value) {
        for (auto& p : parameters)
            if (p.first == name) { p.second = value; return; }
        parameters.emplace_back(name, value);
    }
    // An IK chain's target (world space) and weight; false if there's no chain by that name
    bool setIK(const std::string& name, Vec3 target, float weight) {
        for (AnimatorIK& k : ik)
            if (k.name == name) {
                k.target = target;
                k.hasTarget = true;
                k.weight = weight;
                return true;
            }
        return false;
    }
    float parameter(const std::string& name) const {
        for (const auto& p : parameters)
            if (p.first == name) return p.second;
        return 0.f;
    }

    void play(const std::string& name, float fadeSeconds = 0.2f, bool looping = true) {
        if (playing && name == clip && state.empty()) return;
        if (playing && fadeSeconds > 0.f && !clip.empty()) {
            fadeFrom = clip;
            fadeFromState = state;
            fadeFromTime = time;
            fade = 0.f;
            fadeLength = fadeSeconds;
        } else {
            fadeFrom.clear();
            fadeFromState.clear();
        }
        state.clear();   // (a clip played by code: the state machine's states are left)
        clip = name;
        time = 0.f;
        loop = looping;
        playing = true;
    }
    void stop() { playing = false; fadeFrom.clear(); }

    // Layers by name (made when first played)
    void playLayer(const std::string& layer, const std::string& clipName, float layerWeight = 1.f, bool looping = true) {
        AnimatorLayer* l = findLayer(layer);
        if (!l) {
            layers.push_back({});
            l = &layers.back();
            l->name = layer;
        }
        if (!(l->playing && l->clip == clipName)) l->time = 0.f;
        l->clip = clipName;
        l->weight = layerWeight;
        l->loop = looping;
        l->playing = true;
    }
    void stopLayer(const std::string& layer) {
        if (AnimatorLayer* l = findLayer(layer)) l->playing = false;
    }
    AnimatorLayer* findLayer(const std::string& layer) {
        for (AnimatorLayer& l : layers)
            if (l.name == layer) return &l;
        return nullptr;
    }
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
    float stretch = 0.f;            // streaks along their motion, as long as this many seconds of it (rain); 0: dots
    bool lit = false;               // lit by the scene's lights (rain in the headlights) instead of glowing on its own
    // Simulated on the GPU (renderer_particles.cpp): thousands cost little, drops that hit something splash, see-through
    // ones sorted back to front there; game code's bursts still come from the CPU
    bool gpu = false;
};

// Marks an entity as part of a copy ("instance") of a prefab
// Shared with the players of a multiplayer game (net/net_session.h): the server sends where it is (and whether it's
// visible, and with `fields` its behaviours' fields) to every client, which shows it there instead of simulating it
// An AI: a behaviour tree run every update (game/behaviour_tree.h), a blackboard of values it (and game code)
// reads and writes, and perception (what it sees and hears, written to the blackboard)
struct AIBrain {
    struct Node {
        std::string type = "Sequence";   // see ai::nodeTypes(): Selector, Sequence, MoveTo, Wait, CanSee...
        std::string name;                // shown in the editor ("" : the type)
        std::vector<int> children;       // indices into nodes
        std::string key, text;           // what they mean depends on the type (a blackboard key, a target...)
        float number = 0.f, number2 = 0.f;
    };
    std::vector<Node> nodes;          // nodes[0]: the root
    json::Value blackboard = json::Value::object();   // its starting values (while playing: the live ones)
    // Perception: entities with this tag are noticed within sight (a cone along -Z, from the eyes, unless
    // something solid is in the way) and their sounds within hearing
    std::string perceives = "Player";
    float sightRange = 15.f, sightAngle = 120.f;   // metres, degrees (the whole cone)
    float eyeHeight = 1.6f;
    float hearingRange = 20.f;                     // metres (louder sounds carry further)
    float memory = 5.f;                            // seconds a target out of sight is still remembered
    std::vector<int8_t> status;                    // runtime: each node's last result (-1 not run, 0 success,
                                                   // 1 failure, 2 running), for the editor
};

struct Replicated {
    bool fields = true;
};

// A piece of the game's interface, laid out in the editor (game/ui_layout.h): a rectangle placed by its anchors (where
// in the parent element's rectangle, as fractions: 0,0 top left; or the screen's, for the top one) and offsets (canvas
// units from those points: the canvas is 1080 high), nested by the entity hierarchy and drawn in its order over the
// game. Buttons, sliders, toggles and text fields react to the mouse, and to the keyboard and gamepad: arrows / d-pad
// move between them, Enter / Space / A presses, Left / Right change a slider.
struct UIElement {
    enum class Kind { Panel, Text, Image, Button, Slider, Toggle, TextField, List };
    Kind kind = Kind::Panel;
    float anchorMin[2] = {0.5f, 0.5f}, anchorMax[2] = {0.5f, 0.5f};
    float offsetMin[2] = {-150.f, -40.f}, offsetMax[2] = {150.f, 40.f};
    Vec3 color{0.12f, 0.13f, 0.16f};   // its background (sRGB)
    float opacity = 0.85f;             // 0: no background
    float rounding = 8.f;
    std::string text;                  // shown, formatted: {@key} from the strings, {name} from the game state
    float textSize = 36.f;
    Vec3 textColor{1, 1, 1};
    int align = 1;                     // 0 left, 1 centre, 2 right
    std::string font;                  // "" the game's font (game/fonts.h)
    std::string image;                 // Image: a texture (built-in name or file)
    // Button: what pressing it does: a message (its name) to `target` (an entity's name; "": everyone), or @resume,
    // @pause, @quit, @load <scene>, @show <entity>, @hide <entity>, @toggle <entity>, @save <slot>, @loadgame <slot>
    std::string action;
    std::string target;
    std::string value;                 // Slider / Toggle / TextField: the game state value it shows and changes
    float min = 0.f, max = 1.f;        // Slider's range
    float spacing = 10.f;              // List: the gap between its children, stacked top to bottom at their heights
};
const char* uiKindName(UIElement::Kind kind);   // "panel", "button", "textField", ...
UIElement::Kind uiKindFromName(const std::string& name);

// Reflections of the surroundings for shiny things inside a box: the scene is rendered from the entity's position in
// all directions (when it's first seen, when it moves or changes, and on the editor's Bake), prefiltered like a sky
// image, and surfaces inside the box reflect that (and get its ambient light) instead of the sky
struct ReflectionProbe {
    Vec3 size{10, 6, 10};         // the box, centred on the entity (not turned with it), metres
    int resolution = 128;         // pixels along each of the six views (64, 128 or 256)
    bool boxProjection = true;    // reflections lined up with a box-shaped room's walls (parallax correction)
};

// A decal: an image or a colour projected down the entity's box (a unit cube under its transform: its scale is the
// box's size in metres; down its local -y) onto whatever is inside: road markings, dirt, cracks, puddles. It changes
// the surface's colour, roughness and wetness before it's lit.
struct Decal {
    int texture = -1;             // colour (sRGB) and alpha: where it shows; -1: the colour alone, the whole box
    Vec3 color{1, 1, 1};          // sRGB, times the texture
    float opacity = 1.f;
    float roughness = -1.f;       // the surface's roughness under it (-1: as it is)
    float wetness = 0.f;          // 0..1: wet there (1: a puddle: dark, glossy, with rings in the rain)
    float angleFade = 0.5f;       // fading on surfaces turned away from its direction (0: none, 1: most)
    int normalTexture = -1;       // a normal map over the surface where it shows (cracks, tyre tracks, a manhole's rim)
    float normalStrength = 1.f;
    float emissive = 0.f;         // it glows: its colour (times the texture) as light, this bright (0: none; above 1 blooms)
};

// A heightmap ground (scene/terrain.h): a square grid of heights over size x size metres, centred on the entity (its
// transform applies on top), with up to four painted layers (a texture and a tint each; weights per grid point). It
// collides (a static height field: its Collider, if any, is ignored), navigation walks on it, and it's drawn like a
// mesh. The editor sculpts and paints it with the Terrain tool.
struct Terrain {
    static constexpr int kLayers = 4;
    struct Layer {
        int texture = -1;          // id in the TextureLibrary (-1: just the tint)
        Vec3 color{1, 1, 1};       // sRGB tint
        float tiling = 0.25f;      // texture repeats per metre
    };
    int resolution = 0;            // height samples along each side: 64, 128 or 256 (0: not made yet)
    float size = 64.f;             // metres along X and Z
    std::vector<float> heights;    // resolution^2, row by row along +Z, metres above the entity
    std::vector<uint8_t> weights;  // resolution^2 * 4: how much of each layer at each point (they add up to 255)
    Layer layers[kLayers];
    std::vector<uint8_t> holes;    // resolution^2: 1 where the ground is cut away (the triangles using that point
                                   // go, drawn and colliding); empty: none
    // Vegetation: up to four kinds of plant (or rock), each painted with how much grows where; their instances are
    // scattered from that (the same every time) by terrain::syncFoliage
    static constexpr int kFoliageLayers = 4;
    struct Foliage {
        std::string mesh = "Grass Blade";   // a mesh in the library by name ("Grass Blade": a clump of blades)
        Vec3 color{0.33f, 0.55f, 0.18f};    // sRGB
        float density = 6.f;                // per square metre where fully painted
        float minScale = 0.7f, maxScale = 1.3f;
        float wind = 0.25f;                 // how far the tips sway (0: still)
        float fadeDistance = 60.f;          // they shrink away towards this far from the camera
    };
    std::vector<Foliage> foliage;
    std::vector<uint8_t> foliageDensity;    // resolution^2 * kFoliageLayers: how much of each grows at each point
    // Runtime: a new number after every change (redrawn, collision remade); not saved
    uint64_t revision = 0;
    std::vector<int> meshes;       // its chunks' meshes in the MeshLibrary, built by terrain::syncMeshes
    uint64_t meshRevision = 0;     // the revision they show
    int meshResolution = 0;        // the resolution they were made for
    std::vector<uint32_t> chunkLods;   // per chunk: (first index, index count) of its two coarser levels
    struct Plant {                 // one scattered instance, in the terrain's space
        float pos[3];
        float yaw, scale, shade;
    };
    struct PlantRange {            // the instances of one foliage layer in one chunk (with their bounds)
        int layer = 0;
        uint32_t first = 0, count = 0;
        Vec3 lo, hi;
    };
    std::vector<Plant> plants;
    std::vector<PlantRange> plantRanges;
    uint64_t plantRevision = 0;    // the revision they were scattered from
};

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
    std::string bus = "effects";  // mixer bus: effects, music, ui, ambience (audio/audio.h)
    bool stream = false;          // decoded as it plays (long files: music, ambience) rather than all up front
    float doppler = 1.f;          // pitch shift as it moves relative to the listener (0: none)
    float occlusion = 1.f;        // spatial: how much walls between it and the listener muffle it (0: not at all)
};

// A space with its own acoustics: while the listener is inside (within `radius`, easing in over `blend` metres at
// the edge) effects and ambience get this much reverb, in a room this big (0..1) and muffled (0..1)
struct ReverbZone {
    float radius = 10.f;
    float blend = 3.f;
    float wet = 0.35f;
    float roomSize = 0.7f;
    float damping = 0.5f;
};

struct Entity {
    EntityId id = kNoEntity;
    std::string name;
    EntityId parent = kNoEntity;
    bool visible = true;    // also hides the children
    bool locked = false;    // not pickable by clicking in the viewport
    std::vector<std::string> tags;   // labels game code finds entities by (GameContext::findWithTag)
    uint8_t layer = 0;               // 0..31: which others its collider meets, and which queries see it (LayerSettings)

    bool hasTag(const std::string& tag) const {
        for (const std::string& t : tags)
            if (t == tag) return true;
        return false;
    }

    Transform transform;
    std::optional<MeshRenderer> meshRenderer;
    std::optional<Camera> camera;
    std::optional<Collider> collider;
    std::optional<Light> light;
    std::optional<RigidBody> rigidBody;
    std::optional<Joint> joint;
    std::optional<NavLink> navLink;
    std::optional<AudioSource> audioSource;
    std::optional<ReverbZone> reverbZone;
    std::optional<Skin> skin;
    std::optional<Animator> animator;
    std::optional<ParticleEmitter> particleEmitter;
    std::optional<Terrain> terrain;
    std::optional<ReflectionProbe> reflectionProbe;
    std::optional<Decal> decal;
    std::optional<Water> water;
    std::optional<UIElement> ui;
    std::optional<Replicated> replicated;
    std::optional<Vehicle> vehicle;
    std::optional<AIBrain> brain;
    std::optional<SoftBody> softBody;
    std::optional<Fracture> fracture;
    std::optional<PrefabLink> prefab;
    // In an instance of a prefab that itself contains an instance of another prefab (or is a variant of one), the
    // link the prefab's entity has to that inner prefab. Inside a prefab's own entities, `prefab` is that link.
    std::optional<PrefabLink> nestedPrefab;
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

// A scene's sound: its music (started when the scene starts or is loaded, cross-faded from what was playing; the
// same track carries on; "": whatever plays goes on), and reverb worked out from the world around the listener
// (rays: how closed in, how big the room; reverb zones take over where they are)
struct AudioSettings {
    std::string music;
    float musicVolume = 1.f;
    float musicFade = 2.f;          // seconds of cross-fade
    bool reverbFromWorld = false;
    float reverbAmount = 1.f;       // 0..2: how strong that reverb is
};

// Multiplayer (net/net_session.h): what a joining player gets
struct NetworkSettings {
    std::string playerPrefab;      // spawned for each client that joins ("": clients only watch)
    Vec3 spawnPoint{0, 1, 0};
    float spawnSpread = 2.f;       // players are spread around it, this far apart
    float relevance = 0.f;         // clients only get replicated entities this near their player (0: all of them)
};

// Layers: up to 32, named per scene. Colliders on two layers only meet (collide, or set off triggers) when the
// matrix allows it; queries can be limited to some layers (a mask of 1 << layer bits).
struct LayerSettings {
    static constexpr int kCount = 32;
    std::string names[kCount] = {"Default"};
    uint32_t collides[kCount];   // bit b of collides[a]: layers a and b meet (kept symmetric)
    LayerSettings() {
        for (uint32_t& c : collides) c = ~0u;
    }
    bool meet(int a, int b) const { return (collides[a & 31] >> (b & 31)) & 1u; }
    void setMeet(int a, int b, bool on) {
        a &= 31;
        b &= 31;
        if (on) { collides[a] |= 1u << b; collides[b] |= 1u << a; }
        else { collides[a] &= ~(1u << b); collides[b] &= ~(1u << a); }
    }
    int find(const std::string& name) const {   // -1 if no layer has that name
        for (int i = 0; i < kCount; ++i)
            if (!name.empty() && names[i] == name) return i;
        return -1;
    }
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
    float aoStrength = 0.6f;          // ambient occlusion: how much nooks and contact spots darken (0: off)
    float aoRadius = 0.6f;            // ... over how far, m
    // A sky image (an equirectangular panorama: .hdr, or an ordinary image): drawn as the sky, it also lights the
    // scene and is reflected (render/environment.h); the colours above then only colour the fog. "": none.
    std::string skyImage;
    float skyIntensity = 1.f;         // its brightness
    float skyRotation = 0.f;          // turned around the vertical, degrees
    // Global illumination: light bouncing off surfaces, from a grid of probes around the camera that are redrawn a
    // few each frame (so it follows moving lights and objects, with a lag)
    bool gi = false;
    float giSpacing = 3.f;            // between the probes, m (the grid covers 8 x 4 x 8 of them)
    float giIntensity = 1.f;          // how strong the bounced light is
    bool reflections = false;         // screen-space reflections on smooth surfaces (else probes and the sky only)
    // Screen-space global illumination: light bounced off what's on screen nearby (a lit wall beside a car), from the
    // last frame's picture, on top of the probes' and the sky's (which stand in where the rays find nothing)
    bool ssgi = false;
    float ssgiIntensity = 1.f;        // how strong (0..1: 1 as it is)
    // Shadows: how soft the sun's are (its size in the sky: 1 a slightly hazy sun's, sharp where they touch and
    // softer further from what casts them; 0: as sharp as the shadow maps allow)
    float shadowSoftness = 1.f;
    // Contact shadows: a short ray towards the sun through the view's depth for each pixel, catching the contact the
    // shadow maps are too coarse for (a tyre on the road, a cup on a table). Without MSAA, at full render scale.
    bool contactShadows = false;
    // Volumetric fog: the fog lit by the sun (with its shadows: light shafts) and the lights (halos, beams), in a grid
    // of cells through the view (render/volumetric_fog.h); else the fog is a plain colour by distance
    bool volumetricFog = false;
    float fogLight = 1.f;             // how strongly the fog shows the light passing through it
    float fogAnisotropy = 0.5f;       // -1..1: how much light keeps going forward (0: every way alike; 0.5: haze)
    float fogHeightFalloff = 0.f;     // thinner higher up: density halves every 0.69 / this metres (0: even)
    // The picture: the tone mapper (0: PBR Neutral, colours as they are; 1: filmic, richer contrast and highlights
    // that bleach like film's) and grading
    int tonemapper = 0;
    float contrast = 1.f, saturation = 1.f;
    float vignette = 0.f;             // darker corners (0: none, 1: strong)
    // Auto-exposure: the exposure follows how bright the view is (its average, eased like an eye adjusting: quicker
    // to a brighter view), between limits in stops; `exposure` is then a compensation on top
    bool autoExposure = false;
    float exposureMin = -4.f, exposureMax = 4.f;   // stops darker / brighter than the exposure for a mid-grey world
    float adaptationSpeed = 1.f;      // how quickly it settles (1: about a second into the dark, a third out of it)
    // Local tone mapping: darker and brighter parts of the view pulled towards the middle (0: off, 1: strong), so
    // a night street keeps detail both in the shadows and round the lights
    float localToneMapping = 0.f;
    float lensDirt = 0.f;             // 0..1: smudges on the lens lit up by the bloom round bright lights
    float lightShafts = 0.f;          // 0..2: the sun's rays through gaps on screen (god rays; without volumetric fog)
    // Camera effects (off at 0)
    float depthOfField = 0.f;         // 0..1: how blurred what's out of focus gets (1: about 1% of the view's height)
    float focusDistance = 10.f;       // metres from the camera that are sharp ...
    EntityId focusTarget = kNoEntity; // ... or this entity's distance, when there is one
    float motionBlur = 0.f;           // 0..1: smears along what moves on screen (1: over the whole frame's motion)
    // The physically based sky (render/atmosphere.h): the sky, the sun's colour and the light from the sky worked out
    // from the air (blue by day, red at sunset, dark at night) instead of the colours above (the ground's stays: what
    // the ground reflects); distant things fade into the air's colour (aerial perspective). A sky image wins over it.
    // skyIntensity and skyRotation brighten it and turn its compass.
    bool physicalSky = false;
    float timeOfDay = -1.f;           // hours (0..24) placing the sun (the scene's sun turned to it); -1: as placed
    float latitude = 40.f;            // degrees: how high the sun climbs (with a time of day)
    float haze = 1.f;                 // haze in the air (Mie scattering: a whiter sky, a wider glow round the sun)
    float aerialPerspective = 10.f;   // how thick the air looks over distance (1: as real; small scenes want more)
    float cloudCover = 0.f;           // 0..1: clouds over the physical sky (lit by it, drifting, shading the ground)
    // Weather: how wet the world is (surfaces facing up darker and glossier, puddles on flat ground) and the rain on
    // it (rings in the puddles); MeshRenderer::sheltered surfaces stay dry
    float wetness = 0.f;              // 0..1
    float puddles = 0.5f;             // 0..1: how much of flat ground holds standing water (when wet)
    float rain = 0.f;                 // 0..1: how hard it rains (the rings)
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
    LayerSettings layers;
    NetworkSettings network;
    AudioSettings audio;

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
    // Instances of other prefabs in it stay linked to them: nested prefabs (a variant, when `root` is one).
    Prefab makePrefab(EntityId root, const std::string& name) const;
    // Copies of the prefab's entities with fresh ids, linked to the prefab, the root under `parent`. Not added to
    // the scene: pass them to add() (in order). Ids are reserved, so they stay unique even if added later.
    std::vector<Entity> instantiate(const Prefab& prefab, EntityId parent = kNoEntity);
    EntityId reserveId() { return nextId_++; }
    void setNextId(EntityId id) { nextId_ = std::max(nextId_, id); }   // (ids below are left for another machine's)
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
