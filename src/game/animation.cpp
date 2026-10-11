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

// What an Animator plays: one clip, or two mixed by `weight` (a blend tree), sampled at the same phase (0..1)
struct Source {
    const AnimationClip* a = nullptr;
    const AnimationClip* b = nullptr;
    float weight = 0.f;   // 0: all a, 1: all b
    bool valid() const { return a != nullptr; }
    float duration() const {
        if (!a) return 0.f;
        return b ? a->duration + (b->duration - a->duration) * weight : a->duration;
    }
};

const AnimatorState* findState(const Animator& an, const std::string& name) {
    if (name.empty()) return nullptr;
    for (const AnimatorState& s : an.states)
        if (s.name == name) return &s;
    return nullptr;
}

// The state's blend tree at the parameter's current value, or its (or the given) clip
Source sourceFor(const Animator& an, const std::string& stateName, const std::string& clip, const MeshLibrary& meshes) {
    Source src;
    const AnimatorState* st = findState(an, stateName);
    if (st && !st->blend.empty()) {
        std::vector<BlendClip> points = st->blend;
        std::sort(points.begin(), points.end(), [](const BlendClip& x, const BlendClip& y) { return x.at < y.at; });
        float v = an.parameter(st->blendParameter);
        size_t k = 0;
        while (k + 1 < points.size() && points[k + 1].at <= v) ++k;
        src.a = meshes.findAnimation(points[k].clip);
        if (k + 1 < points.size() && v > points[k].at) {
            src.b = meshes.findAnimation(points[k + 1].clip);
            src.weight = std::clamp((v - points[k].at) / std::max(points[k + 1].at - points[k].at, 1e-6f), 0.f, 1.f);
        }
        if (!src.a) { src.a = src.b; src.b = nullptr; src.weight = 0.f; }
        if (!src.b) src.weight = 0.f;
        return src;
    }
    src.a = meshes.findAnimation(clip);
    return src;
}

// One node's pose in a source at a phase; false if neither clip moves it
bool samplePose(const Source& src, const std::string& target, float phase, AnimationPose& out) {
    const AnimationTrack* ta = trackFor(src.a, target);
    const AnimationTrack* tb = src.b ? trackFor(src.b, target) : nullptr;
    if (!ta && !tb) return false;
    if (ta && tb) out = blendPoses(sampleTrack(*ta, phase * src.a->duration), sampleTrack(*tb, phase * src.b->duration), src.weight);
    else if (ta) out = sampleTrack(*ta, phase * src.a->duration);
    else out = sampleTrack(*tb, phase * src.b->duration);
    return true;
}

void addTargets(const Source& src, std::vector<std::string>& targets) {
    for (const AnimationClip* c : {src.a, src.b})
        if (c)
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

// Events of `clip` in the stretch of time playback moved over (from, to], wrapping around a loop
void collectEvents(const Animator& a, EntityId id, float from, float to, float duration, bool wrapped,
                   std::vector<FiredAnimationEvent>& fired) {
    for (const AnimationEvent& ev : a.events) {
        if (ev.clip != a.clip) continue;
        bool hit = wrapped ? (ev.time > from && ev.time <= duration) || ev.time <= to : ev.time > from && ev.time <= to;
        if (hit) fired.push_back({id, ev.name});
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
        if (!a.playing) continue;
        EntityId rootId = scene.entities[i].id;
        Source cur = sourceFor(a, a.state, a.clip, meshes);
        if (!cur.valid()) continue;
        const bool blendTree = cur.b != nullptr || (findState(a, a.state) && !findState(a, a.state)->blend.empty());
        float duration = cur.duration();
        float before = a.time, step = dt * a.speed;
        a.time = advance(a.time, step, duration, a.loop);
        bool wrapped = a.loop && step > 0.f && (a.time < before || step >= duration);
        if (fired && !a.events.empty() && step > 0.f && !blendTree)
            collectEvents(a, rootId, before, a.time, duration, wrapped, *fired);
        float phase = duration > 0.f ? a.time / duration : 0.f;

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
            l.time = advance(l.time, dt * l.speed, lc->duration, l.loop);
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
                poses[track.target] = blendPoses(under, sampleTrack(track, l.time), std::min(l.weight, 1.f));
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
