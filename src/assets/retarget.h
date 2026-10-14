// Retargeting: a clip made for one skeleton played by another (a different rig: other bone names, proportions and
// bone axes). Bones are paired by name (rig prefixes, sides and the usual humanoid names understood: "mixamorig:
// LeftUpLeg" is "thigh.l", like "Thigh_L"), and each paired bone turns as much as its source turns away from its
// rest pose, measured in the world (so bones whose local axes differ still bend the same way, as long as both rest
// poses are alike, both T-poses say). The hips also carry the source's travel, scaled by how tall the hips stand.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "assets/animation.h"
#include "scene/scene.h"

namespace retarget {

// A model's skeleton: the entity `root` and everything under it, as they stand now (their rest pose); or a prefab's
Skeleton skeletonOf(const Scene& scene, EntityId root);
Skeleton skeletonOf(const Prefab& prefab);

// A bone's humanoid key ("upperarm.l", "spine", "head") or "" when the name says nothing known
std::string boneKey(const std::string& name);
// Target bone -> source bone, for the bones whose keys (or else exact names) match
std::vector<std::pair<std::string, std::string>> mapBones(const Skeleton& from, const Skeleton& to);

// The clip for `to` (named `name`), sampled 30 times a second and compressed; it keeps its recipe.
// `matchRestPose`: the skeletons' rest poses differ (an A-pose rig and a T-pose one): each bone is first turned
// from its rest direction (towards its first paired child) onto its source's, then follows the source's turns in
// the world (the whole source pose, each frame), so the clip looks the same on both
// `plantFeet`: legs of other proportions put their feet where the source's are, scaled like the hips' travel (each
// leg, thigh -> calf -> foot, bent by two-bone IK towards its knee), so planted feet don't slide
AnimationClip retargetClip(const AnimationClip& clip, const Skeleton& from, const Skeleton& to,
                           const std::vector<std::pair<std::string, std::string>>& map, const std::string& name,
                           bool matchRestPose = false, bool plantFeet = false);

// The clip mirrored left to right (across the skeleton's x = 0 plane): each bone moves as its other side did ("thigh.l"
// as "thigh.r"; bones without a side as themselves), whatever its local axes, as long as the rest pose is symmetric.
// It keeps its recipe (RetargetRecipe::mirror).
AnimationClip mirrorClip(const AnimationClip& clip, const Skeleton& skeleton, const std::string& name);
// Each bone's other side in a skeleton (by humanoid key, else by name: Left / Right, .L / .R, _l / _r), or itself
std::vector<int> mirrorBones(const Skeleton& skeleton);

// Skeletons in scene files
json::Value skeletonToJson(const Skeleton& s);
Skeleton skeletonFromJson(const json::Value& v);

}  // namespace retarget
