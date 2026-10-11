#include "game/animation.h"

#include <cmath>

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

}  // namespace

void updateAnimators(Scene& scene, const MeshLibrary& meshes, float dt) {
    for (size_t i = 0; i < scene.entities.size(); ++i) {
        if (!scene.entities[i].animator || !scene.entities[i].animator->playing) continue;
        EntityId rootId = scene.entities[i].id;
        Animator& a = *scene.entities[i].animator;
        const AnimationClip* clip = meshes.findAnimation(a.clip);
        if (!clip) continue;
        a.time = advance(a.time, dt * a.speed, clip->duration, a.loop);
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
