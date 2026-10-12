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

// The clip for `to` (named `name`), sampled 30 times a second and compressed; it keeps its recipe
AnimationClip retargetClip(const AnimationClip& clip, const Skeleton& from, const Skeleton& to,
                           const std::vector<std::pair<std::string, std::string>>& map, const std::string& name);

// Skeletons in scene files
json::Value skeletonToJson(const Skeleton& s);
Skeleton skeletonFromJson(const json::Value& v);

}  // namespace retarget
