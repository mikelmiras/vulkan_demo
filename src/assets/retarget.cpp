#include "assets/retarget.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace retarget {

namespace {

Skeleton::Bone boneOf(const Entity& e, int parent) {
    Skeleton::Bone b;
    b.name = e.name;
    b.parent = parent;
    Mat4 m = e.transform.matrix();
    b.translation = {m.m[12], m.m[13], m.m[14]};
    b.rotation = normalizeQuat(rotationOf(m));
    b.scale = {e.transform.scale[0], e.transform.scale[1], e.transform.scale[2]};
    return b;
}

// Rest rotations and positions in the skeleton's space (parents first)
void restWorld(const Skeleton& s, std::vector<Quat>& rot, std::vector<Vec3>& pos) {
    rot.resize(s.bones.size());
    pos.resize(s.bones.size());
    for (size_t i = 0; i < s.bones.size(); ++i) {
        const Skeleton::Bone& b = s.bones[i];
        if (b.parent < 0) {
            rot[i] = b.rotation;
            pos[i] = b.translation;
        } else {
            rot[i] = normalizeQuat(rot[(size_t)b.parent] * b.rotation);
            pos[i] = pos[(size_t)b.parent] + rotate(rot[(size_t)b.parent], b.translation);
        }
    }
}

}  // namespace

Skeleton skeletonOf(const Scene& scene, EntityId root) {
    Skeleton s;
    std::vector<std::pair<EntityId, int>> todo{{root, -1}};   // (depth first: parents before children)
    while (!todo.empty()) {
        auto [id, parent] = todo.back();
        todo.pop_back();
        const Entity* e = scene.find(id);
        if (!e) continue;
        s.bones.push_back(boneOf(*e, parent));
        int me = (int)s.bones.size() - 1;
        std::vector<EntityId> kids = scene.childrenOf(id);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) todo.push_back({*it, me});
    }
    return s;
}

Skeleton skeletonOf(const Prefab& prefab) {
    Skeleton s;
    std::map<EntityId, int> index;
    for (size_t pass = 0; pass < prefab.entities.size() && index.size() < prefab.entities.size(); ++pass)
        for (const Entity& e : prefab.entities) {   // (parents first, whatever order they're stored in)
            if (index.count(e.id)) continue;
            bool root = e.parent == kNoEntity || std::none_of(prefab.entities.begin(), prefab.entities.end(),
                                                              [&](const Entity& o) { return o.id == e.parent; });
            if (!root && !index.count(e.parent)) continue;
            s.bones.push_back(boneOf(e, root ? -1 : index[e.parent]));
            index[e.id] = (int)s.bones.size() - 1;
        }
    return s;
}

std::string boneKey(const std::string& raw) {
    std::string name = raw;
    for (char sep : {':', '|'})   // "mixamorig:Hips", "Armature|Hips"
        if (size_t p = name.rfind(sep); p != std::string::npos) name = name.substr(p + 1);
    // Words: split on separators and camel case, lower case
    std::vector<std::string> words;
    std::string w;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        bool sep = c == '_' || c == '.' || c == ' ' || c == '-';
        bool camel = i > 0 && std::isupper((unsigned char)c) && std::islower((unsigned char)name[i - 1]);
        bool digit = i > 0 && std::isdigit((unsigned char)c) && !std::isdigit((unsigned char)name[i - 1]);
        if ((sep || camel || digit) && !w.empty()) {
            words.push_back(w);
            w.clear();
        }
        if (!sep) w += (char)std::tolower((unsigned char)c);
    }
    if (!w.empty()) words.push_back(w);
    // The side
    std::string side;
    for (auto it = words.begin(); it != words.end();) {
        if (*it == "left" || *it == "l") { side = "l"; it = words.erase(it); }
        else if (*it == "right" || *it == "r") { side = "r"; it = words.erase(it); }
        else if (*it == "mixamorig" || *it == "bip" || *it == "def" || *it == "armature" || *it == "jnt" || *it == "bone") it = words.erase(it);
        else ++it;
    }
    std::string joined;
    for (const std::string& x : words) joined += x;
    // The body part
    static const std::vector<std::pair<std::vector<const char*>, const char*>> parts = {
        {{"hips", "pelvis", "hip"}, "hips"},
        {{"spine", "spine0"}, "spine"},
        {{"spine1", "chest"}, "chest"},
        {{"spine2", "upperchest", "spine3"}, "upperchest"},
        {{"neck", "neck1"}, "neck"},
        {{"head"}, "head"},
        {{"shoulder", "clavicle", "collar"}, "shoulder"},
        {{"arm", "upperarm"}, "upperarm"},
        {{"forearm", "lowerarm", "elbow"}, "forearm"},
        {{"hand", "wrist"}, "hand"},
        {{"upleg", "upperleg", "thigh"}, "thigh"},
        {{"leg", "lowerleg", "calf", "shin", "knee"}, "calf"},
        {{"foot", "ankle"}, "foot"},
        {{"toe", "toebase", "toes", "ball"}, "toe"},
    };
    std::string part;
    for (const auto& [names, key] : parts)
        for (const char* n : names)
            if (joined == n) part = key;
    for (const char* finger : {"thumb", "index", "middle", "ring", "pinky"})   // fingers keep their joint number
        if (joined.rfind(finger, 0) == 0) part = joined;
    if (part.empty()) return {};
    return side.empty() ? part : part + "." + side;
}

std::vector<std::pair<std::string, std::string>> mapBones(const Skeleton& from, const Skeleton& to) {
    std::vector<std::pair<std::string, std::string>> map;
    std::set<std::string> used;
    for (const Skeleton::Bone& t : to.bones) {
        std::string key = boneKey(t.name), match;
        for (const Skeleton::Bone& s : from.bones) {
            if (used.count(s.name)) continue;
            if ((!key.empty() && boneKey(s.name) == key) || s.name == t.name) {
                match = s.name;
                break;
            }
        }
        if (!match.empty()) {
            map.emplace_back(t.name, match);
            used.insert(match);
        }
    }
    return map;
}

AnimationClip retargetClip(const AnimationClip& clip, const Skeleton& from, const Skeleton& to,
                           const std::vector<std::pair<std::string, std::string>>& map, const std::string& name) {
    AnimationClip out;
    out.name = name;
    out.duration = clip.duration;
    auto recipe = std::make_shared<RetargetRecipe>();
    recipe->clip = clip.name;
    recipe->from = from;
    recipe->to = to;
    recipe->map = map;
    out.retarget = recipe;
    std::vector<Quat> fromRot, toRot;
    std::vector<Vec3> fromPos, toPos;
    restWorld(from, fromRot, fromPos);
    restWorld(to, toRot, toPos);
    // The hips: the paired bone nearest the root (its travel is carried, scaled by how high the hips stand)
    int hipsT = -1, hipsDepth = 1 << 30;
    for (const auto& [t, s] : map) {
        int i = to.find(t), d = 0;
        for (int p = i; p >= 0 && to.bones[(size_t)p].parent >= 0; p = to.bones[(size_t)p].parent) ++d;
        if (i >= 0 && d < hipsDepth && from.find(s) >= 0) { hipsDepth = d; hipsT = i; }
    }
    const int samples = std::max(2, (int)std::ceil(clip.duration * 30.f) + 1);
    for (const auto& [targetName, sourceName] : map) {
        int ti = to.find(targetName), si = from.find(sourceName);
        if (ti < 0 || si < 0) continue;
        const AnimationTrack* src = nullptr;
        for (const AnimationTrack& t : clip.tracks)
            if (t.target == sourceName) src = &t;
        if (!src) continue;
        const Skeleton::Bone &sb = from.bones[(size_t)si], &tb = to.bones[(size_t)ti];
        Quat parentS = sb.parent >= 0 ? fromRot[(size_t)sb.parent] : Quat{};
        Quat parentT = tb.parent >= 0 ? toRot[(size_t)tb.parent] : Quat{};
        AnimationTrack track;
        track.target = targetName;
        bool rotates = !src->rotation.empty(), moves = ti == hipsT && !src->translation.empty();
        float ratio = 1.f;
        if (moves) {
            // (above the model's root; the hips' own height when they are the root)
            float hs = si == 0 ? fromPos[0].y : fromPos[(size_t)si].y - fromPos[0].y;
            float ht = ti == 0 ? toPos[0].y : toPos[(size_t)ti].y - toPos[0].y;
            ratio = hs > 0.01f && ht > 0.01f ? ht / hs : 1.f;
        }
        for (int k = 0; k < samples; ++k) {
            float t = std::min((float)k / 30.f, clip.duration);
            AnimationPose p = sampleTrack(*src, t);
            if (rotates) {
                // How far it turned from rest (in its parent's frame), seen in the world, applied to the target's rest
                Quat delta = normalizeQuat(p.rotation * conjugate(sb.rotation));
                Quat world = parentS * delta * conjugate(parentS);
                Quat local = normalizeQuat(conjugate(parentT) * world * parentT * tb.rotation);
                track.rotation.times.push_back(t);
                track.rotation.values.insert(track.rotation.values.end(), {local.x, local.y, local.z, local.w});
            }
            if (moves) {
                Vec3 world = rotate(parentS, p.translation - sb.translation) * ratio;
                Vec3 local = tb.translation + rotate(conjugate(parentT), world);
                track.translation.times.push_back(t);
                track.translation.values.insert(track.translation.values.end(), {local.x, local.y, local.z});
            }
        }
        track.rotation.width = 4;
        track.translation.width = 3;
        if (!track.rotation.empty() || !track.translation.empty()) out.tracks.push_back(std::move(track));
    }
    compressClip(out);
    return out;
}

json::Value skeletonToJson(const Skeleton& s) {
    json::Value a = json::Value::array();
    for (const Skeleton::Bone& b : s.bones) {
        json::Value v = json::Value::object();
        v.set("name", b.name);
        v.set("parent", b.parent);
        json::Value t = json::Value::array();
        for (float f : {b.translation.x, b.translation.y, b.translation.z, b.rotation.x, b.rotation.y, b.rotation.z,
                        b.rotation.w, b.scale.x, b.scale.y, b.scale.z})
            t.push(f);
        v.set("rest", t);
        a.push(std::move(v));
    }
    return a;
}

Skeleton skeletonFromJson(const json::Value& v) {
    Skeleton s;
    for (const json::Value& b : v.elements()) {
        Skeleton::Bone bone;
        bone.name = b["name"].asString();
        bone.parent = b["parent"].asInt(-1);
        const json::Value& r = b["rest"];
        if (r.size() == 10) {
            bone.translation = {r[0].asFloat(), r[1].asFloat(), r[2].asFloat()};
            bone.rotation = normalizeQuat({r[3].asFloat(), r[4].asFloat(), r[5].asFloat(), r[6].asFloat(1.f)});
            bone.scale = {r[7].asFloat(1.f), r[8].asFloat(1.f), r[9].asFloat(1.f)};
        }
        if (bone.parent >= (int)s.bones.size()) bone.parent = -1;   // (parents come first)
        s.bones.push_back(bone);
    }
    return s;
}

}  // namespace retarget
