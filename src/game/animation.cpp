#include "game/animation.h"

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

bool conditionMet(const Animator& a, const AnimatorTransition& t, const AnimationClip* clip) {
    float v = a.parameter(t.parameter);
    switch (t.when) {
    case AnimatorTransition::When::Greater: return v > t.value;
    case AnimatorTransition::When::Less: return v < t.value;
    case AnimatorTransition::When::IsTrue: return v != 0.f;
    case AnimatorTransition::When::IsFalse: return v == 0.f;
    case AnimatorTransition::When::ClipEnded: return !a.loop && (!clip || a.time >= clip->duration);
    }
    return false;
}

// Takes the first transition (from the current state or any) whose condition holds
void runStateMachine(Animator& a, const MeshLibrary& meshes) {
    const AnimationClip* clip = meshes.findAnimation(a.clip);
    for (const AnimatorTransition& t : a.transitions) {
        if (t.to == a.state || (t.from != "*" && t.from != a.state)) continue;
        if (conditionMet(a, t, clip)) {
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
            if (a.playing && fade > 0.f && !a.clip.empty()) {
                a.fadeFrom = a.clip;
                a.fadeFromTime = a.time;
                a.fade = 0.f;
                a.fadeLength = fade;
            } else {
                a.fadeFrom.clear();
            }
            a.clip = s.clip;
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
        if (!a.states.empty() && a.playing && dt > 0.f) runStateMachine(a, meshes);
        if (!a.playing) continue;
        EntityId rootId = scene.entities[i].id;
        const AnimationClip* clip = meshes.findAnimation(a.clip);
        if (!clip) continue;
        float before = a.time;
        a.time = advance(a.time, dt * a.speed, clip->duration, a.loop);
        if (fired && !a.events.empty() && dt * a.speed > 0.f)
            collectEvents(a, rootId, before, a.time, clip->duration, a.loop && (a.time < before || dt * a.speed >= clip->duration), *fired);
        const AnimationClip* from = a.fadeFrom.empty() ? nullptr : meshes.findAnimation(a.fadeFrom);
        float blend = 1.f;
        if (from) {
            a.fadeFromTime = advance(a.fadeFromTime, dt * a.speed, from->duration, true);
            a.fade += dt;
            blend = a.fadeLength > 0.f ? a.fade / a.fadeLength : 1.f;
            if (blend >= 1.f) {
                blend = 1.f;
                from = nullptr;
            }
        }
        if (!from) a.fadeFrom.clear();
        float time = a.time, fromTime = a.fadeFromTime;
        std::unordered_map<std::string, EntityId> names = scene.namesUnder(rootId);
        auto pose = [&](const std::string& target, const AnimationPose& p) {
            auto it = names.find(target);
            if (it == names.end()) return;
            if (Entity* e = scene.find(it->second)) apply(e->transform, p);
        };
        for (const AnimationTrack& track : clip->tracks) {
            AnimationPose p = sampleTrack(track, time);
            if (from)
                if (const AnimationTrack* old = trackFor(from, track.target)) p = blendPoses(sampleTrack(*old, fromTime), p, blend);
            pose(track.target, p);
        }
        if (from)   // nodes only the old clip moves ease back from it to where they are
            for (const AnimationTrack& old : from->tracks) {
                if (trackFor(clip, old.target)) continue;
                auto it = names.find(old.target);
                Entity* e = it != names.end() ? scene.find(it->second) : nullptr;
                if (e) apply(e->transform, blendPoses(sampleTrack(old, fromTime), poseOf(e->transform), blend));
            }
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
