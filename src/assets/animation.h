// Animation clips: keyframed translation / rotation / scale of named nodes (the entities of an imported model),
// as glTF stores them. Clips live in the MeshLibrary next to the meshes they move.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/vecmath.h"

struct AnimationKeys {
    enum class Interpolation { Linear, Step, CubicSpline };
    std::vector<float> times;    // seconds, ascending
    std::vector<float> values;   // `width` floats per key (3 for translation and scale, 4 for a rotation quaternion);
                                 // cubic splines store in-tangent, value, out-tangent per key
    int width = 3;
    Interpolation interpolation = Interpolation::Linear;
    bool empty() const { return times.empty(); }
};

struct AnimationTrack {
    std::string target;   // the node's (entity's) name
    AnimationKeys translation, rotation, scale;
};

// A skeleton in its rest pose: bones by name, each with its parent (-1: a root) and its local transform at rest
struct Skeleton {
    struct Bone {
        std::string name;
        int parent = -1;
        Vec3 translation;
        Quat rotation;
        Vec3 scale{1, 1, 1};
    };
    std::vector<Bone> bones;   // parents before their children
    int find(const std::string& name) const {
        for (size_t i = 0; i < bones.size(); ++i)
            if (bones[i].name == name) return (int)i;
        return -1;
    }
};

// How a clip was made from another skeleton's (assets/retarget.h): kept with it, so scenes save the recipe and
// loading makes it again
struct RetargetRecipe {
    std::string clip;      // the source clip's name
    Skeleton from, to;
    std::vector<std::pair<std::string, std::string>> map;   // (target bone, source bone)
    bool matchRestPose = false;   // rest poses told apart (an A-pose clip on a T-pose rig): retarget.h
};

struct AnimationClip {
    std::string name;     // "Fox/Run"
    std::string source;   // "model.glb#anim2" (re-imported from there when a scene is loaded)
    float duration = 0.f;
    std::vector<AnimationTrack> tracks;
    std::shared_ptr<const RetargetRecipe> retarget;   // made from another skeleton's clip (no source of its own)
};

// A node's local transform as a track sets it; the has* flags say which parts the track animates
struct AnimationPose {
    Vec3 translation;
    Quat rotation;
    Vec3 scale{1, 1, 1};
    bool hasTranslation = false, hasRotation = false, hasScale = false;
};

// The track's pose at `time` (clamped to the keys)
AnimationPose sampleTrack(const AnimationTrack& track, float time);
// Blends b into a by `t` (0 = a, 1 = b); parts only one side has come from that side
AnimationPose blendPoses(const AnimationPose& a, const AnimationPose& b, float t);

// Compression: drops the keys that interpolating their neighbours reproduces within a tolerance (rotations in
// degrees, translations in metres, scales as factors). Linear and step channels (cubic splines are left as they
// are). Returns how many keys went.
size_t compressClip(AnimationClip& clip, float rotationDegrees = 0.05f, float translation = 0.0005f, float scale = 0.0005f);
size_t keyCount(const AnimationClip& clip);
