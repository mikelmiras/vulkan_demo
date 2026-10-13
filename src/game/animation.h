// Plays Animator components: advances their clips and poses the entities they drive (by name, see Animator).
#pragma once

#include "assets/meshes.h"
#include "scene/scene.h"

#include <utility>
#include <vector>

// An AnimationEvent that playback passed
struct FiredAnimationEvent {
    EntityId entity;   // the Animator's
    std::string name;
    std::string sound, bone;   // (AnimationEvent's: played by the Game)
    float volume = 1.f;
};

// Runs the Animators' state machines, advances every playing Animator by dt seconds (times its speed) and poses
// its entities; dt = 0 just poses them. Events passed are appended to `fired` when given.
void updateAnimators(Scene& scene, const MeshLibrary& meshes, float dt, std::vector<FiredAnimationEvent>* fired = nullptr);

// Poses the entities under `root` as `clip` has them at `time`, without touching the Animator (editor preview)
void poseClip(Scene& scene, const MeshLibrary& meshes, EntityId root, const std::string& clip, float time);

// Enters a state of the Animator's state machine (cross-fading `fade` seconds); false if there's no such state
bool enterAnimatorState(Animator& a, const std::string& state, float fade);

// 2D blend space weights (gradient band interpolation) of `points` at (x, y): one per point, adding up to 1; exactly 1
// on a point; the nearest point's when (x, y) is outside them all
std::vector<float> blendWeights2D(const std::vector<std::pair<float, float>>& points, float x, float y);
