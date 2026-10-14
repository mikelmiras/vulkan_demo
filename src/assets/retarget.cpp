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

// retargetClip with matchRestPose: whole poses in the world, each target bone first turned onto its source's rest
// direction
void retargetMatched(const AnimationClip& clip, const Skeleton& from, const Skeleton& to,
                     const std::vector<std::pair<std::string, std::string>>& map, const std::vector<Quat>& fromRot,
                     const std::vector<Vec3>& fromPos, const std::vector<Quat>& toRot, const std::vector<Vec3>& toPos,
                     AnimationClip& out) {
    std::vector<int> sourceOf(to.bones.size(), -1);
    for (const auto& [t, s] : map) {
        int ti = to.find(t), si = from.find(s);
        if (ti >= 0 && si >= 0) sourceOf[(size_t)ti] = si;
    }
    // Rest corrections: the turn taking the target bone's rest direction (to its first paired child) onto its
    // source's, in the world
    std::vector<Quat> correction(to.bones.size());
    for (size_t ti = 0; ti < to.bones.size(); ++ti) {
        if (sourceOf[ti] < 0) continue;
        for (size_t c = 0; c < to.bones.size(); ++c) {
            if (to.bones[c].parent != (int)ti || sourceOf[c] < 0) continue;
            Vec3 dt = toPos[c] - toPos[ti], ds = fromPos[(size_t)sourceOf[c]] - fromPos[(size_t)sourceOf[ti]];
            if (length(dt) > 1e-4f && length(ds) > 1e-4f) correction[ti] = rotationBetween(dt, ds);
            break;
        }
    }
    std::vector<const AnimationTrack*> trackOf(from.bones.size(), nullptr);
    for (const AnimationTrack& t : clip.tracks)
        if (int i = from.find(t.target); i >= 0) trackOf[(size_t)i] = &t;
    // The hips: the paired bone nearest the root, carrying the source's travel scaled by how high they stand
    int hipsT = -1, hipsDepth = 1 << 30;
    for (size_t i = 0; i < to.bones.size(); ++i) {
        if (sourceOf[i] < 0) continue;
        int d = 0;
        for (int p = (int)i; p >= 0 && to.bones[(size_t)p].parent >= 0; p = to.bones[(size_t)p].parent) ++d;
        if (d < hipsDepth) hipsDepth = d, hipsT = (int)i;
    }
    std::vector<AnimationTrack> tracks(to.bones.size());
    const int samples = std::max(2, (int)std::ceil(clip.duration * 30.f) + 1);
    std::vector<Quat> ws(from.bones.size()), wt(to.bones.size());
    for (int k = 0; k < samples; ++k) {
        float time = std::min((float)k / 30.f, clip.duration);
        for (size_t i = 0; i < from.bones.size(); ++i) {   // the source's pose, in the world
            const Skeleton::Bone& b = from.bones[i];
            Quat local = trackOf[i] && !trackOf[i]->rotation.empty() ? sampleTrack(*trackOf[i], time).rotation : b.rotation;
            ws[i] = b.parent >= 0 ? normalizeQuat(ws[(size_t)b.parent] * local) : local;
        }
        for (size_t i = 0; i < to.bones.size(); ++i) {   // the target's: paired bones turn like theirs
            const Skeleton::Bone& b = to.bones[i];
            Quat parent = b.parent >= 0 ? wt[(size_t)b.parent] : Quat{};
            if (sourceOf[i] < 0) {
                wt[i] = normalizeQuat(parent * b.rotation);
                continue;
            }
            int s = sourceOf[i];
            Quat turned = ws[(size_t)s] * conjugate(fromRot[(size_t)s]);
            wt[i] = normalizeQuat(turned * correction[i] * toRot[i]);
            Quat local = normalizeQuat(conjugate(parent) * wt[i]);
            AnimationTrack& tr = tracks[i];
            tr.rotation.times.push_back(time);
            tr.rotation.values.insert(tr.rotation.values.end(), {local.x, local.y, local.z, local.w});
            if ((int)i == hipsT && trackOf[(size_t)s] && !trackOf[(size_t)s]->translation.empty()) {
                const Skeleton::Bone& sb = from.bones[(size_t)s];
                Quat parentS = sb.parent >= 0 ? fromRot[(size_t)sb.parent] : Quat{};
                Quat parentT = b.parent >= 0 ? toRot[(size_t)b.parent] : Quat{};
                float hs = s == 0 ? fromPos[0].y : fromPos[(size_t)s].y - fromPos[0].y;
                float ht = i == 0 ? toPos[0].y : toPos[i].y - toPos[0].y;
                float ratio = hs > 0.01f && ht > 0.01f ? ht / hs : 1.f;
                Vec3 world = rotate(parentS, sampleTrack(*trackOf[(size_t)s], time).translation - sb.translation) * ratio;
                Vec3 local = b.translation + rotate(conjugate(parentT), world);
                tr.translation.times.push_back(time);
                tr.translation.values.insert(tr.translation.values.end(), {local.x, local.y, local.z});
            }
        }
    }
    for (size_t i = 0; i < to.bones.size(); ++i) {
        AnimationTrack& tr = tracks[i];
        if (tr.rotation.empty() && tr.translation.empty()) continue;
        tr.target = to.bones[i].name;
        tr.rotation.width = 4;
        tr.translation.width = 3;
        out.tracks.push_back(std::move(tr));
    }
}

// The tracks of a clip by bone (null: none)
std::vector<const AnimationTrack*> tracksFor(const Skeleton& s, const AnimationClip& clip) {
    std::vector<const AnimationTrack*> out(s.bones.size(), nullptr);
    for (const AnimationTrack& t : clip.tracks)
        if (int i = s.find(t.target); i >= 0) out[(size_t)i] = &t;
    return out;
}

// A skeleton posed by a clip at `time`, in its own space: world rotations and positions (untracked parts at rest)
void posed(const Skeleton& s, const std::vector<const AnimationTrack*>& tracks, float time, std::vector<Quat>& rot,
           std::vector<Vec3>& pos) {
    rot.resize(s.bones.size());
    pos.resize(s.bones.size());
    for (size_t i = 0; i < s.bones.size(); ++i) {
        const Skeleton::Bone& b = s.bones[i];
        Quat r = b.rotation;
        Vec3 t = b.translation;
        if (tracks[i]) {
            AnimationPose p = sampleTrack(*tracks[i], time);
            if (p.hasRotation) r = p.rotation;
            if (p.hasTranslation) t = p.translation;
        }
        if (b.parent < 0) {
            rot[i] = normalizeQuat(r);
            pos[i] = t;
        } else {
            rot[i] = normalizeQuat(rot[(size_t)b.parent] * r);
            pos[i] = pos[(size_t)b.parent] + rotate(rot[(size_t)b.parent], t);
        }
    }
}

// How much taller the target stands: its hips' height over the source's (the paired bone nearest the root)
float heightRatio(const Skeleton& from, const Skeleton& to, const std::vector<std::pair<std::string, std::string>>& map,
                  const std::vector<Vec3>& fromPos, const std::vector<Vec3>& toPos) {
    int hipsT = -1, hipsS = -1, best = 1 << 30;
    for (const auto& [t, s] : map) {
        int i = to.find(t), si = from.find(s), d = 0;
        if (i < 0 || si < 0) continue;
        for (int p = i; p >= 0 && to.bones[(size_t)p].parent >= 0; p = to.bones[(size_t)p].parent) ++d;
        if (d < best) best = d, hipsT = i, hipsS = si;
    }
    if (hipsT < 0) return 1.f;
    float hs = hipsS == 0 ? fromPos[0].y : fromPos[(size_t)hipsS].y - fromPos[0].y;
    float ht = hipsT == 0 ? toPos[0].y : toPos[(size_t)hipsT].y - toPos[0].y;
    return hs > 0.01f && ht > 0.01f ? ht / hs : 1.f;
}

// Planting the feet: every 30th of a second, each leg (thigh -> calf -> foot, by humanoid keys) of the retargeted
// clip bent by two-bone IK so its foot is where the source's is, scaled about the skeletons' roots by their heights;
// the foot keeps its turn in the world, the knee bends the way it did
void plantFeet(const AnimationClip& clip, const Skeleton& from, const Skeleton& to,
               const std::vector<std::pair<std::string, std::string>>& map, const std::vector<Vec3>& fromPos,
               const std::vector<Vec3>& toPos, AnimationClip& out) {
    struct Leg {
        int thigh, calf, foot, source;
        std::vector<Quat> thighKeys, calfKeys, footKeys;
    };
    std::vector<Leg> legs;
    for (const char* side : {"l", "r"}) {
        int thigh = -1, calf = -1, foot = -1;
        for (size_t i = 0; i < to.bones.size(); ++i) {
            std::string k = boneKey(to.bones[i].name);
            if (k == std::string("thigh.") + side) thigh = (int)i;
            if (k == std::string("calf.") + side) calf = (int)i;
            if (k == std::string("foot.") + side) foot = (int)i;
        }
        if (thigh < 0 || calf < 0 || foot < 0 || to.bones[(size_t)calf].parent != thigh || to.bones[(size_t)foot].parent != calf)
            continue;
        int source = -1;
        for (const auto& [t, s] : map)
            if (t == to.bones[(size_t)foot].name) source = from.find(s);
        if (source >= 0) legs.push_back({thigh, calf, foot, source, {}, {}, {}});
    }
    if (legs.empty()) return;
    float ratio = heightRatio(from, to, map, fromPos, toPos);
    std::vector<const AnimationTrack*> sourceTracks = tracksFor(from, clip), targetTracks = tracksFor(to, out);
    const int samples = std::max(2, (int)std::ceil(clip.duration * 30.f) + 1);
    std::vector<Quat> rs, rt;
    std::vector<Vec3> ps, pt;
    for (int k = 0; k < samples; ++k) {
        float time = std::min((float)k / 30.f, clip.duration);
        posed(from, sourceTracks, time, rs, ps);
        posed(to, targetTracks, time, rt, pt);
        for (Leg& leg : legs) {
            Quat parent = to.bones[(size_t)leg.thigh].parent >= 0 ? rt[(size_t)to.bones[(size_t)leg.thigh].parent] : Quat{};
            Quat thighW = rt[(size_t)leg.thigh], calfW = rt[(size_t)leg.calf], footW = rt[(size_t)leg.foot];
            Vec3 A = pt[(size_t)leg.thigh], B = pt[(size_t)leg.calf], C = pt[(size_t)leg.foot];
            Vec3 goal = toPos[0] + (ps[(size_t)leg.source] - fromPos[0]) * ratio;
            float ab = length(B - A), bc = length(C - B);
            Vec3 at = goal - A;
            Vec3 bend = (B - A) - normalize(at) * dot(B - A, normalize(at));   // (the knee's way, as it is)
            if (ab > 1e-5f && bc > 1e-5f && length(at) > 1e-5f && length(bend) > 1e-5f) {
                float reach = std::clamp(length(at), std::fabs(ab - bc) + 1e-4f, ab + bc - 1e-4f);
                Vec3 dir = normalize(at), perp = normalize(bend);
                float cosA = std::clamp((ab * ab + reach * reach - bc * bc) / (2.f * ab * reach), -1.f, 1.f);
                Vec3 knee = A + dir * (cosA * ab) + perp * (std::sqrt(std::max(0.f, 1.f - cosA * cosA)) * ab);
                Quat q1 = rotationBetween(B - A, knee - A);
                thighW = normalizeQuat(q1 * thighW);
                calfW = normalizeQuat(q1 * calfW);
                Vec3 end = knee + rotate(q1, C - B);
                calfW = normalizeQuat(rotationBetween(end - knee, A + dir * reach - knee) * calfW);
            }
            leg.thighKeys.push_back(normalizeQuat(conjugate(parent) * thighW));
            leg.calfKeys.push_back(normalizeQuat(conjugate(thighW) * calfW));
            leg.footKeys.push_back(normalizeQuat(conjugate(calfW) * footW));
        }
    }
    auto setKeys = [&](int bone, const std::vector<Quat>& keys) {
        AnimationTrack* track = nullptr;
        for (AnimationTrack& t : out.tracks)
            if (t.target == to.bones[(size_t)bone].name) track = &t;
        if (!track) {
            out.tracks.push_back({});
            track = &out.tracks.back();
            track->target = to.bones[(size_t)bone].name;
            track->translation.width = 3;
        }
        track->rotation = {};
        track->rotation.width = 4;
        for (size_t k = 0; k < keys.size(); ++k) {
            track->rotation.times.push_back(std::min((float)k / 30.f, clip.duration));
            track->rotation.values.insert(track->rotation.values.end(), {keys[k].x, keys[k].y, keys[k].z, keys[k].w});
        }
    };
    for (const Leg& leg : legs) {
        setKeys(leg.thigh, leg.thighKeys);
        setKeys(leg.calf, leg.calfKeys);
        setKeys(leg.foot, leg.footKeys);
    }
}

// A name's other side: "Left" <-> "Right", ".L" <-> ".R", "_l" <-> "_r" (and the like); "" if it has none
std::string otherSide(const std::string& name) {
    static const std::vector<std::pair<std::string, std::string>> swaps = {
        {"Left", "Right"}, {"left", "right"}, {"LEFT", "RIGHT"}, {".L", ".R"}, {".l", ".r"}, {"_L", "_R"},
        {"_l", "_r"}, {"L_", "R_"}, {"l_", "r_"}, {" L", " R"}, {" l", " r"}};
    for (const auto& [a, b] : swaps) {
        for (const auto& [x, y] : {std::make_pair(a, b), std::make_pair(b, a)}) {
            size_t at = name.find(x);
            if (at == std::string::npos) continue;
            bool prefix = x.back() == '_', suffix = x.front() == '.' || x.front() == '_' || x.front() == ' ';
            if (prefix && at != 0) continue;                                  // (L_Arm: at the start)
            if (suffix && !prefix && at + x.size() != name.size()) continue;   // (Arm.L: at the end)
            return name.substr(0, at) + y + name.substr(at + x.size());
        }
    }
    return {};
}

}  // namespace

std::vector<int> mirrorBones(const Skeleton& s) {
    std::vector<int> out(s.bones.size());
    for (size_t i = 0; i < s.bones.size(); ++i) {
        out[i] = (int)i;
        std::string key = boneKey(s.bones[i].name);
        size_t dot = key.rfind('.');
        if (dot != std::string::npos) {   // a humanoid bone with a side: the one with the same part, other side
            std::string want = key.substr(0, dot + 1) + (key.substr(dot + 1) == "l" ? "r" : "l");
            for (size_t j = 0; j < s.bones.size(); ++j)
                if (boneKey(s.bones[j].name) == want) out[i] = (int)j;
            if (out[i] != (int)i) continue;
        }
        std::string other = otherSide(s.bones[i].name);
        if (int j = other.empty() ? -1 : s.find(other); j >= 0) out[i] = j;
    }
    return out;
}

AnimationClip mirrorClip(const AnimationClip& clip, const Skeleton& skeleton, const std::string& name) {
    AnimationClip out;
    out.name = name;
    out.duration = clip.duration;
    auto recipe = std::make_shared<RetargetRecipe>();
    recipe->clip = clip.name;
    recipe->from = recipe->to = skeleton;
    recipe->mirror = true;
    out.retarget = recipe;
    std::vector<int> other = mirrorBones(skeleton);
    std::vector<const AnimationTrack*> tracks = tracksFor(skeleton, clip);
    std::vector<Quat> restRot;
    std::vector<Vec3> restPos;
    restWorld(skeleton, restRot, restPos);
    // Mirroring across x = 0: a point's x flips; a turn's axis does too, so its quaternion is (x, -y, -z, w)
    auto mirrorQ = [](Quat q) { return Quat{q.x, -q.y, -q.z, q.w}; };
    auto mirrorP = [](Vec3 p) { return Vec3{-p.x, p.y, p.z}; };
    const size_t n = skeleton.bones.size();
    std::vector<AnimationTrack> made(n);
    std::vector<bool> turns(n), moves(n);
    for (size_t i = 0; i < n; ++i) {
        const AnimationTrack* t = tracks[(size_t)other[i]];
        turns[i] = (t && !t->rotation.empty()) || (tracks[i] && !tracks[i]->rotation.empty());
        moves[i] = t && !t->translation.empty();
        if (t && !t->scale.empty()) made[i].scale = t->scale;   // (scales are the same on both sides)
    }
    const int samples = std::max(2, (int)std::ceil(clip.duration * 30.f) + 1);
    std::vector<Quat> rot, mrot(n);
    std::vector<Vec3> pos, mpos(n);
    for (int k = 0; k < samples; ++k) {
        float time = std::min((float)k / 30.f, clip.duration);
        posed(skeleton, tracks, time, rot, pos);
        for (size_t i = 0; i < n; ++i) {   // (parents first)
            size_t j = (size_t)other[i];
            const Skeleton::Bone& b = skeleton.bones[i];
            // Its other side's turn from rest, in the world, mirrored, applied to this bone's own rest
            Quat delta = normalizeQuat(rot[j] * conjugate(restRot[j]));
            mrot[i] = normalizeQuat(mirrorQ(delta) * restRot[i]);
            Quat parent = b.parent >= 0 ? mrot[(size_t)b.parent] : Quat{};
            Vec3 parentPos = b.parent >= 0 ? mpos[(size_t)b.parent] : Vec3{};
            Vec3 local = b.translation;
            if (moves[i]) local = rotate(conjugate(parent), mirrorP(pos[j]) - parentPos);
            mpos[i] = b.parent >= 0 ? parentPos + rotate(parent, local) : local;
            AnimationTrack& tr = made[i];
            if (turns[i]) {
                Quat l = normalizeQuat(conjugate(parent) * mrot[i]);
                tr.rotation.times.push_back(time);
                tr.rotation.values.insert(tr.rotation.values.end(), {l.x, l.y, l.z, l.w});
            }
            if (moves[i]) {
                tr.translation.times.push_back(time);
                tr.translation.values.insert(tr.translation.values.end(), {local.x, local.y, local.z});
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        AnimationTrack& tr = made[i];
        if (tr.rotation.empty() && tr.translation.empty() && tr.scale.empty()) continue;
        tr.target = skeleton.bones[i].name;
        tr.rotation.width = 4;
        tr.translation.width = 3;
        out.tracks.push_back(std::move(tr));
    }
    compressClip(out);
    return out;
}

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
                           const std::vector<std::pair<std::string, std::string>>& map, const std::string& name,
                           bool matchRestPose, bool feet) {
    AnimationClip out;
    out.name = name;
    out.duration = clip.duration;
    auto recipe = std::make_shared<RetargetRecipe>();
    recipe->clip = clip.name;
    recipe->from = from;
    recipe->to = to;
    recipe->map = map;
    recipe->matchRestPose = matchRestPose;
    recipe->plantFeet = feet;
    out.retarget = recipe;
    std::vector<Quat> fromRot, toRot;
    std::vector<Vec3> fromPos, toPos;
    restWorld(from, fromRot, fromPos);
    restWorld(to, toRot, toPos);
    if (matchRestPose) {
        retargetMatched(clip, from, to, map, fromRot, fromPos, toRot, toPos, out);
        if (feet) plantFeet(clip, from, to, map, fromPos, toPos, out);
        compressClip(out);
        return out;
    }
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
    if (feet) plantFeet(clip, from, to, map, fromPos, toPos, out);
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
