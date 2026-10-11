// Animation clips: keyframed translation / rotation / scale of named nodes (the entities of an imported model),
// as glTF stores them. Clips live in the MeshLibrary next to the meshes they move.
#pragma once

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

struct AnimationClip {
    std::string name;     // "Fox/Run"
    std::string source;   // "model.glb#anim2" (re-imported from there when a scene is loaded)
    float duration = 0.f;
    std::vector<AnimationTrack> tracks;
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
