#include "game/animation.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace {

// An entity's current local transform as a pose (everything set)
AnimationPose poseOf(const Transform& t) {
    AnimationPose p;
    p.translation = t.pos();
    p.rotation = rotationOf(t.matrix());
    p.scale = {t.scale[0], t.scale[1], t.scale[2]};
    p.hasTranslation = p.hasRotation = p.hasScale = true;
    return p;
}

void apply(Transform& t, const AnimationPose& p) {
    AnimationPose cur = poseOf(t);
    Vec3 tr = p.hasTranslation ? p.translation : cur.translation;
    Quat r = p.hasRotation ? p.rotation : cur.rotation;
    Vec3 s = p.hasScale ? p.scale : cur.scale;
    t.setMatrix(composeTRS(tr, r, s));
}

// Advances a clip time; looping wraps, otherwise it holds the last frame
float advance(float time, float dt, float duration, bool loop) {
    time += dt;
    if (duration <= 0.f) return 0.f;
    if (loop) {
        time = std::fmod(time, duration);
        if (time < 0.f) time += duration;
        return time;
    }
    return std::fmin(std::fmax(time, 0.f), duration);
}

const AnimationTrack* trackFor(const AnimationClip* clip, const std::string& target) {
    if (!clip) return nullptr;
    for (const AnimationTrack& t : clip->tracks)
        if (t.target == target) return &t;
    return nullptr;
}

bool conditionMet(const Animator& a, const AnimatorTransition& t, float duration) {
    float v = a.parameter(t.parameter);
    switch (t.when) {
    case AnimatorTransition::When::Greater: return v > t.value;
    case AnimatorTransition::When::Less: return v < t.value;
    case AnimatorTransition::When::IsTrue: return v != 0.f;
    case AnimatorTransition::When::IsFalse: return v == 0.f;
    case AnimatorTransition::When::ClipEnded: return !a.loop && a.time >= duration;
    }
    return false;
}

// What an Animator plays: one clip, or several mixed by weight (a blend tree), all sampled at the same phase (0..1)
struct Source {
    std::vector<std::pair<const AnimationClip*, float>> clips;   // (weights add up to 1)
    bool valid() const { return !clips.empty(); }
    float duration() const {
        float d = 0.f;
        for (const auto& [c, w] : clips) d += c->duration * w;
        return d;
    }
    const AnimationClip* main() const {   // the one that weighs most (its events fire)
        const AnimationClip* best = nullptr;
        float bw = -1.f;
        for (const auto& [c, w] : clips)
            if (w > bw) best = c, bw = w;
        return best;
    }
};

const AnimatorState* findState(const Animator& an, const std::string& name) {
    if (name.empty()) return nullptr;
    for (const AnimatorState& s : an.states)
        if (s.name == name) return &s;
    return nullptr;
}

}  // namespace

std::vector<float> blendWeights2D(const std::vector<std::pair<float, float>>& points, float x, float y) {
    // Gradient band interpolation (cartesian): each point's weight falls off across the band towards every other
    size_t n = points.size();
    std::vector<float> w(n, 0.f);
    if (n == 0) return w;
    float sum = 0.f;
    for (size_t i = 0; i < n; ++i) {
        float wi = 1.f;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            float ex = points[j].first - points[i].first, ey = points[j].second - points[i].second;
            float len2 = ex * ex + ey * ey;
            if (len2 < 1e-12f) continue;
            float h = 1.f - ((x - points[i].first) * ex + (y - points[i].second) * ey) / len2;
            wi = std::min(wi, h);
        }
        w[i] = std::max(wi, 0.f);
        sum += w[i];
    }
    if (sum <= 1e-9f) {   // (outside them all: the nearest)
        size_t best = 0;
        float bd = 1e30f;
        for (size_t i = 0; i < n; ++i) {
            float dx = x - points[i].first, dy = y - points[i].second;
            if (dx * dx + dy * dy < bd) bd = dx * dx + dy * dy, best = i;
        }
        w[best] = 1.f;
        return w;
    }
    for (float& v : w) v /= sum;
    return w;
}

namespace {

// The state's blend tree at the parameters' current values, or its (or the given) clip
Source sourceFor(const Animator& an, const std::string& stateName, const std::string& clip, const MeshLibrary& meshes) {
    Source src;
    const AnimatorState* st = findState(an, stateName);
    if (st && !st->blend.empty() && !st->blendParameterY.empty()) {   // 2D
        std::vector<std::pair<float, float>> points;
        for (const BlendClip& b : st->blend) points.emplace_back(b.at, b.atY);
        std::vector<float> w = blendWeights2D(points, an.parameter(st->blendParameter), an.parameter(st->blendParameterY));
        float total = 0.f;
        for (size_t i = 0; i < w.size(); ++i)
            if (w[i] > 1e-4f)
                if (const AnimationClip* c = meshes.findAnimation(st->blend[i].clip)) {
                    src.clips.emplace_back(c, w[i]);
                    total += w[i];
                }
        for (auto& c : src.clips) c.second /= std::max(total, 1e-6f);
        return src;
    }
    if (st && !st->blend.empty()) {   // 1D: the two clips either side
        std::vector<BlendClip> points = st->blend;
        std::sort(points.begin(), points.end(), [](const BlendClip& x, const BlendClip& y) { return x.at < y.at; });
        float v = an.parameter(st->blendParameter);
        size_t k = 0;
        while (k + 1 < points.size() && points[k + 1].at <= v) ++k;
        const AnimationClip* a = meshes.findAnimation(points[k].clip);
        const AnimationClip* b = nullptr;
        float weight = 0.f;
        if (k + 1 < points.size() && v > points[k].at) {
            b = meshes.findAnimation(points[k + 1].clip);
            weight = std::clamp((v - points[k].at) / std::max(points[k + 1].at - points[k].at, 1e-6f), 0.f, 1.f);
        }
        if (a && b) src.clips = {{a, 1.f - weight}, {b, weight}};
        else if (a || b) src.clips = {{a ? a : b, 1.f}};
        return src;
    }
    if (const AnimationClip* c = meshes.findAnimation(clip)) src.clips = {{c, 1.f}};
    return src;
}

// One node's pose in a source at a phase; false if no clip of it moves the node
bool samplePose(const Source& src, const std::string& target, float phase, AnimationPose& out) {
    float total = 0.f;
    for (const auto& [clip, w] : src.clips) {
        const AnimationTrack* t = trackFor(clip, target);
        if (!t || w <= 0.f) continue;
        AnimationPose p = sampleTrack(*t, phase * clip->duration);
        total += w;
        out = total == w ? p : blendPoses(out, p, w / total);   // (a running weighted mix)
    }
    return total > 0.f;
}

void addTargets(const Source& src, std::vector<std::string>& targets) {
    for (const auto& [c, w] : src.clips)
        for (const AnimationTrack& t : c->tracks)
            if (std::find(targets.begin(), targets.end(), t.target) == targets.end()) targets.push_back(t.target);
}

// Takes the first transition (from the current state or any) whose condition holds
void runStateMachine(Animator& a, const MeshLibrary& meshes) {
    float duration = sourceFor(a, a.state, a.clip, meshes).duration();
    for (const AnimatorTransition& t : a.transitions) {
        if (t.to == a.state || (t.from != "*" && t.from != a.state)) continue;
        if (conditionMet(a, t, duration)) {
            enterAnimatorState(a, t.to, t.fade);
            return;
        }
    }
}

// Events of `clip` in the stretch of playback moved over, as phases (from, to] (0..1), wrapping around a loop
void collectEvents(const Animator& a, EntityId id, const AnimationClip* clip, float from, float to, bool wrapped,
                   std::vector<FiredAnimationEvent>& fired) {
    if (!clip) return;
    for (const AnimationEvent& ev : a.events) {
        if (ev.clip != clip->name) continue;
        float at = clip->duration > 0.f ? ev.time / clip->duration : 0.f;
        bool hit = wrapped ? at > from || at <= to : at > from && at <= to;
        if (hit) fired.push_back({id, ev.name});
    }
}

// A world-space turn of an entity about a point (its local transform changes to match)
void turnAbout(Scene& scene, Entity& e, Vec3 pivot, Quat q) {
    Mat4 world = scene.worldMatrix(e.id);
    Mat4 turned = translate(pivot) * composeTRS({}, q, {1, 1, 1}) * translate(pivot * -1.f) * world;
    Mat4 parent = e.parent != kNoEntity ? scene.worldMatrix(e.parent) : Mat4{};
    e.transform.setMatrix(inverseAffine(parent) * turned);
}

Vec3 positionOf(const Scene& scene, EntityId id) {
    Mat4 m = scene.worldMatrix(id);
    return {m.m[12], m.m[13], m.m[14]};
}

// The Animator's IK chains, on its entities as they're posed now
void applyIK(Scene& scene, EntityId rootId, Animator& a, const std::unordered_map<std::string, EntityId>& names) {
    for (AnimatorIK& k : a.ik) {
        float w = std::clamp(k.weight, 0.f, 1.f);
        if (w <= 0.f || k.bone.empty()) continue;
        Vec3 target = k.target;
        if (!k.targetEntity.empty()) {
            const Entity* t = scene.findByName(k.targetEntity);
            if (!t) continue;
            target = positionOf(scene, t->id);
        } else if (!k.hasTarget) {
            continue;
        }
        auto it = names.find(k.bone);
        Entity* end = it != names.end() ? scene.find(it->second) : nullptr;
        if (!end) continue;
        if (k.type == AnimatorIK::Type::LookAt) {
            Mat4 wm = scene.worldMatrix(end->id);
            Vec3 p{wm.m[12], wm.m[13], wm.m[14]};
            Vec3 aim = transformDir(wm, k.aimAxis), to = target - p;
            if (length(aim) < 1e-6f || length(to) < 1e-6f) continue;
            Quat q = rotationBetween(aim, to);
            float angle = quatAngle(q), limit = std::max(k.maxAngle, 0.f) * DEG2RAD;
            if (angle > limit && angle > 1e-6f) q = slerp(Quat{}, q, limit / angle);
            turnAbout(scene, *end, p, slerp(Quat{}, q, w));
            continue;
        }
        // Two bones: top -> mid -> end (the law of cosines puts the middle joint, bent towards the pole)
        Entity* mid = scene.find(end->parent);
        Entity* top = mid ? scene.find(mid->parent) : nullptr;
        if (!top) continue;
        Vec3 A = positionOf(scene, top->id), B = positionOf(scene, mid->id), C = positionOf(scene, end->id);
        float ab = length(B - A), bc = length(C - B);
        if (ab < 1e-5f || bc < 1e-5f) continue;
        Vec3 at = target - A;
        float reach = std::clamp(length(at), std::fabs(ab - bc) + 1e-4f, ab + bc - 1e-4f);
        Vec3 dir = length(at) > 1e-6f ? normalize(at) : normalize(C - A);
        Vec3 pole = transformDir(scene.worldMatrix(rootId), k.pole);
        Vec3 perp = pole - dir * dot(pole, dir);
        if (length(perp) < 1e-5f) perp = (B - A) - dir * dot(B - A, dir);   // (along the reach: keep the bend)
        if (length(perp) < 1e-5f) continue;
        perp = normalize(perp);
        float cosA = std::clamp((ab * ab + reach * reach - bc * bc) / (2.f * ab * reach), -1.f, 1.f);
        Vec3 knee = A + dir * (cosA * ab) + perp * (std::sqrt(std::max(0.f, 1.f - cosA * cosA)) * ab);
        turnAbout(scene, *top, A, slerp(Quat{}, rotationBetween(B - A, knee - A), w));
        B = positionOf(scene, mid->id);
        C = positionOf(scene, end->id);
        Vec3 goal = A + dir * reach;   // (the target, or as near as the chain reaches)
        turnAbout(scene, *mid, B, slerp(Quat{}, rotationBetween(C - B, goal - B), w));
    }
}

}  // namespace

bool enterAnimatorState(Animator& a, const std::string& name, float fade) {
    for (const AnimatorState& s : a.states)
        if (s.name == name) {
            // Like play(), but a state always starts its clip over, even when the last state played the same one
            if (a.playing && fade > 0.f && (!a.clip.empty() || !a.state.empty())) {
                a.fadeFrom = a.clip;
                a.fadeFromState = a.state;
                a.fadeFromTime = a.time;
                a.fade = 0.f;
                a.fadeLength = fade;
            } else {
                a.fadeFrom.clear();
                a.fadeFromState.clear();
            }
            a.clip = s.blend.empty() ? s.clip : s.blend.front().clip;   // (a blend tree: its first clip, for show)
            a.time = 0.f;
            a.loop = s.loop;
            a.playing = true;
            a.speed = s.speed;
            a.state = s.name;
            return true;
        }
    return false;
}

void updateAnimators(Scene& scene, const MeshLibrary& meshes, float dt, std::vector<FiredAnimationEvent>* fired) {
    for (size_t i = 0; i < scene.entities.size(); ++i) {
        if (!scene.entities[i].animator) continue;
        Animator& a = *scene.entities[i].animator;
        a.rootDelta = {};
        if (!a.states.empty() && a.playing && dt > 0.f) runStateMachine(a, meshes);
        EntityId rootId = scene.entities[i].id;
        Source cur = a.playing ? sourceFor(a, a.state, a.clip, meshes) : Source{};
        if (!cur.valid()) {   // (not animating: IK still turns it)
            if (!a.ik.empty()) {
                std::unordered_map<std::string, EntityId> names = scene.namesUnder(rootId);
                applyIK(scene, rootId, *scene.entities[i].animator, names);
            }
            continue;
        }
        float duration = cur.duration();
        float before = a.time, step = dt * a.speed;
        a.time = advance(a.time, step, duration, a.loop);
        bool wrapped = a.loop && step > 0.f && (a.time < before || step >= duration);
        float phase = duration > 0.f ? a.time / duration : 0.f;
        if (fired && !a.events.empty() && step > 0.f)   // (a blend tree: the main clip's, by phase)
            collectEvents(a, rootId, cur.main(), duration > 0.f ? before / duration : 0.f, phase, wrapped, *fired);

        // The clip (or blend) being faded out
        Source from;
        float blend = 1.f, fromPhase = 0.f;
        if (!a.fadeFrom.empty() || !a.fadeFromState.empty()) {
            from = sourceFor(a, a.fadeFromState, a.fadeFrom, meshes);
            if (from.valid()) {
                float fd = from.duration();
                a.fadeFromTime = advance(a.fadeFromTime, step, fd, true);
                fromPhase = fd > 0.f ? a.fadeFromTime / fd : 0.f;
                a.fade += dt;
                blend = a.fadeLength > 0.f ? a.fade / a.fadeLength : 1.f;
            }
            if (!from.valid() || blend >= 1.f) {
                blend = 1.f;
                from = {};
                a.fadeFrom.clear();
                a.fadeFromState.clear();
            }
        }

        std::unordered_map<std::string, EntityId> names = scene.namesUnder(rootId);
        auto entityFor = [&](const std::string& target) -> Entity* {
            auto it = names.find(target);
            return it != names.end() ? scene.find(it->second) : nullptr;
        };
        std::vector<std::string> targets;
        addTargets(cur, targets);
        size_t curTargets = targets.size();
        addTargets(from, targets);
        std::unordered_map<std::string, AnimationPose> poses;
        for (size_t t = 0; t < targets.size(); ++t) {
            const std::string& target = targets[t];
            AnimationPose p, old;
            bool here = t < curTargets && samplePose(cur, target, phase, p);
            bool there = from.valid() && samplePose(from, target, fromPhase, old);
            if (here && there) p = blendPoses(old, p, blend);
            else if (there) {   // nodes only the old one moves ease back from it to where they are
                Entity* e = entityFor(target);
                if (!e) continue;
                p = blendPoses(old, poseOf(e->transform), blend);
            } else if (!here) {
                continue;
            }
            poses[target] = p;
        }

        // Layers over it, on their part of the skeleton
        for (AnimatorLayer& l : a.layers) {
            if (!l.playing || l.weight <= 0.f) continue;
            const AnimationClip* lc = meshes.findAnimation(l.clip);
            if (!lc) continue;
            float lBefore = l.time, lStep = dt * l.speed;
            l.time = advance(l.time, lStep, lc->duration, l.loop);
            if (fired && !a.events.empty() && lStep > 0.f && lc->duration > 0.f)   // (layers' clips have events too)
                collectEvents(a, rootId, lc, lBefore / lc->duration, l.time / lc->duration,
                              l.loop && (l.time < lBefore || lStep >= lc->duration), *fired);
            std::unordered_map<std::string, EntityId> masked;
            if (!l.mask.empty())
                if (Entity* m = entityFor(l.mask)) {
                    masked = scene.namesUnder(m->id);
                    masked[m->name] = m->id;
                }
            for (const AnimationTrack& track : lc->tracks) {
                if (!l.mask.empty() && !masked.count(track.target)) continue;
                AnimationPose under;
                auto it = poses.find(track.target);
                if (it != poses.end()) {
                    under = it->second;
                } else if (Entity* e = entityFor(track.target)) {   // its rest pose (kept the first time)
                    auto rest = std::find_if(a.layerRest.begin(), a.layerRest.end(),
                                             [&](const auto& r) { return r.first == track.target; });
                    if (rest == a.layerRest.end()) {
                        a.layerRest.emplace_back(track.target, e->transform);
                        rest = a.layerRest.end() - 1;
                    }
                    under = poseOf(rest->second);
                } else {
                    continue;
                }
                if (!l.additive) {
                    poses[track.target] = blendPoses(under, sampleTrack(track, l.time), std::min(l.weight, 1.f));
                    continue;
                }
                // Additive: the clip's change since its first frame, on top (parts `under` lacks: where the node is)
                Entity* e = entityFor(track.target);
                AnimationPose base = e ? poseOf(e->transform) : AnimationPose{};
                if (under.hasTranslation) base.translation = under.translation;
                if (under.hasRotation) base.rotation = under.rotation;
                if (under.hasScale) base.scale = under.scale;
                AnimationPose ref = sampleTrack(track, 0.f), now = sampleTrack(track, l.time);
                float w = std::min(l.weight, 1.f);
                if (now.hasTranslation) base.translation = base.translation + (now.translation - ref.translation) * w;
                if (now.hasRotation)
                    base.rotation = normalizeQuat(base.rotation * slerp(Quat{}, conjugate(ref.rotation) * now.rotation, w));
                if (now.hasScale)
                    base.scale = {base.scale.x * (1.f + (now.scale.x / std::max(ref.scale.x, 1e-6f) - 1.f) * w),
                                  base.scale.y * (1.f + (now.scale.y / std::max(ref.scale.y, 1e-6f) - 1.f) * w),
                                  base.scale.z * (1.f + (now.scale.z / std::max(ref.scale.z, 1e-6f) - 1.f) * w)};
                base.hasTranslation = base.hasRotation = base.hasScale = true;
                poses[track.target] = base;
            }
        }

        // Root motion: the root bone's travel across the ground goes to the entity; the bone stays where it starts
        if (a.rootMotion && !a.rootBone.empty() && step != 0.f) {
            auto rootAt = [&](float ph) {
                AnimationPose p;
                return samplePose(cur, a.rootBone, ph, p) && p.hasTranslation ? p.translation : Vec3{};
            };
            float p0 = duration > 0.f ? before / duration : 0.f;
            Vec3 d = wrapped ? (rootAt(1.f) - rootAt(p0)) + (rootAt(phase) - rootAt(0.f)) : rootAt(phase) - rootAt(p0);
            a.rootDelta = {d.x, 0.f, d.z};
            auto it = poses.find(a.rootBone);
            if (it != poses.end() && it->second.hasTranslation) {
                Vec3 start = rootAt(0.f);
                it->second.translation.x = start.x;
                it->second.translation.z = start.z;
            }
        }

        for (const auto& [target, p] : poses)
            if (Entity* e = entityFor(target)) apply(e->transform, p);
        if (!a.ik.empty()) applyIK(scene, rootId, a, names);
    }
}

void poseClip(Scene& scene, const MeshLibrary& meshes, EntityId root, const std::string& clipName, float time) {
    const AnimationClip* clip = meshes.findAnimation(clipName);
    if (!clip) return;
    std::unordered_map<std::string, EntityId> names = scene.namesUnder(root);
    for (const AnimationTrack& track : clip->tracks) {
        auto it = names.find(track.target);
        if (it == names.end()) continue;
        if (Entity* e = scene.find(it->second)) apply(e->transform, sampleTrack(track, time));
    }
}
