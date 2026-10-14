// Animation: retargeting between skeletons (assets/retarget.h; planting feet on legs of other lengths), mirrored
// clips, clip compression, foot placement (FootIK), camera shake
#include "test.h"

#include <cmath>

#include "assets/retarget.h"
#include "game/camera_shake.h"
#include "game/game.h"
#include "imgui.h"
#include "scene/serialization.h"

namespace {

Skeleton::Bone bone(const char* name, int parent, Vec3 t, Quat r = {}) {
    Skeleton::Bone b;
    b.name = name;
    b.parent = parent;
    b.translation = t;
    b.rotation = r;
    return b;
}

// The bone's rotation in the skeleton's space with `local` in place of its own rest rotation
Quat worldRotation(const Skeleton& s, int bone, Quat local) {
    Quat q = local;
    for (int p = s.bones[(size_t)bone].parent; p >= 0; p = s.bones[(size_t)p].parent) q = s.bones[(size_t)p].rotation * q;
    return q;
}

Quat restWorld(const Skeleton& s, int bone) { return worldRotation(s, bone, s.bones[(size_t)bone].rotation); }

}  // namespace

TEST(retarget_bone_names) {
    CHECK(retarget::boneKey("mixamorig:LeftUpLeg") == "thigh.l");
    CHECK(retarget::boneKey("Thigh_L") == "thigh.l");
    CHECK(retarget::boneKey("RightArm") == "upperarm.r");
    CHECK(retarget::boneKey("upperarm.R") == "upperarm.r");
    CHECK(retarget::boneKey("Spine1") == "chest");
    CHECK(retarget::boneKey("pelvis") == "hips");
    CHECK(retarget::boneKey("Armature|Head") == "head");
    CHECK(retarget::boneKey("LeftFoot") == "foot.l");
    CHECK(retarget::boneKey("Tail").empty());
    Skeleton a, b;
    a.bones = {bone("mixamorig:Hips", -1, {0, 1, 0}), bone("mixamorig:LeftUpLeg", 0, {0.1f, 0, 0}), bone("Tail", 0, {0, 0, -0.3f})};
    b.bones = {bone("pelvis", -1, {0, 2, 0}), bone("Thigh_L", 0, {0.2f, 0, 0}), bone("Thigh_R", 0, {-0.2f, 0, 0}), bone("Tail", 0, {})};
    auto map = retarget::mapBones(a, b);
    REQUIRE(map.size() == 3);   // (the right thigh has no partner; the tail pairs by its exact name)
    CHECK(map[0] == std::make_pair(std::string("pelvis"), std::string("mixamorig:Hips")));
    CHECK(map[1] == std::make_pair(std::string("Thigh_L"), std::string("mixamorig:LeftUpLeg")));
    CHECK(map[2].first == "Tail");
}

// A leg kicking 45 degrees about Z on a skeleton whose thigh's local axes are turned 90 degrees (and that stands
// twice as tall): the retargeted thigh still turns 45 degrees about Z in the world; the hips travel twice as far
TEST(retarget_turns_the_same_way_in_the_world) {
    Skeleton from, to;
    from.bones = {bone("Hips", -1, {0, 1, 0}), bone("Thigh", 0, {0.1f, 0, 0}), bone("Calf", 1, {0, -0.5f, 0})};
    Quat twist = axisAngle({1, 0, 0}, 90.f * DEG2RAD);
    to.bones = {bone("Hips", -1, {0, 2, 0}), bone("Thigh", 0, {0.2f, 0, 0}, twist), bone("Calf", 1, {0, 0, 1.f}, conjugate(twist))};
    AnimationClip clip;
    clip.name = "Kick";
    clip.duration = 1.f;
    AnimationTrack thigh{"Thigh", {}, {}, {}}, hips{"Hips", {}, {}, {}};
    for (int k = 0; k <= 10; ++k) {
        float t = (float)k / 10.f;
        Quat q = axisAngle({0, 0, 1}, 45.f * DEG2RAD * t);
        thigh.rotation.times.push_back(t);
        thigh.rotation.values.insert(thigh.rotation.values.end(), {q.x, q.y, q.z, q.w});
        hips.translation.times.push_back(t);
        hips.translation.values.insert(hips.translation.values.end(), {t, 1.f, 0.f});   // 1 m along x
    }
    thigh.rotation.width = 4;
    hips.translation.width = 3;
    clip.tracks = {thigh, hips};
    auto map = retarget::mapBones(from, to);
    AnimationClip out = retarget::retargetClip(clip, from, to, map, "Robot/Kick");
    REQUIRE(out.retarget);
    CHECK(out.retarget->clip == "Kick");
    const AnimationTrack* t = nullptr;
    const AnimationTrack* h = nullptr;
    for (const AnimationTrack& tr : out.tracks) {
        if (tr.target == "Thigh") t = &tr;
        if (tr.target == "Hips") h = &tr;
    }
    REQUIRE(t && h);
    int ti = to.find("Thigh");
    for (float time : {0.f, 0.5f, 1.f}) {
        Quat local = sampleTrack(*t, time).rotation;
        Quat delta = worldRotation(to, ti, local) * conjugate(restWorld(to, ti));   // (turned from rest, in the world)
        Quat expected = axisAngle({0, 0, 1}, 45.f * DEG2RAD * time);
        Vec3 probe{0, -1, 0.3f};
        CHECK_VEC(rotate(delta, probe), rotate(expected, probe), 1e-3f);
    }
    CHECK_NEAR(sampleTrack(*h, 1.f).translation.x, 2.0, 1e-3);   // twice as tall: twice the travel
    CHECK_NEAR(sampleTrack(*h, 1.f).translation.y, 2.0, 1e-3);
}

// An A-pose clip on a T-pose rig: with rest poses matched, the target's arm points where the source's does (down at
// 45 degrees at rest, then raised level); without, it keeps its own rest pose as the start and overshoots
TEST(retarget_matches_rest_poses) {
    Quat aPose = axisAngle({0, 0, 1}, -45.f * DEG2RAD);
    Skeleton from, to;
    from.bones = {bone("Hips", -1, {0, 1, 0}), bone("UpperArm.L", 0, {0.2f, 0.5f, 0}, aPose), bone("Forearm.L", 1, {0.3f, 0, 0})};
    to.bones = {bone("Hips", -1, {0, 1, 0}), bone("UpperArm.L", 0, {0.2f, 0.5f, 0}), bone("Forearm.L", 1, {0.3f, 0, 0})};
    AnimationClip clip;
    clip.name = "Raise";
    clip.duration = 1.f;
    AnimationTrack arm{"UpperArm.L", {}, {}, {}};
    for (int k = 0; k <= 10; ++k) {   // from the A-pose up to level
        float t = (float)k / 10.f;
        Quat q = axisAngle({0, 0, 1}, -45.f * DEG2RAD * (1.f - t));
        arm.rotation.times.push_back(t);
        arm.rotation.values.insert(arm.rotation.values.end(), {q.x, q.y, q.z, q.w});
    }
    arm.rotation.width = 4;
    clip.tracks = {arm};
    auto map = retarget::mapBones(from, to);
    auto direction = [&](const AnimationClip& c, float t) {   // the target's upper arm, in the world
        Quat hips = to.bones[0].rotation, upper = to.bones[1].rotation;
        for (const AnimationTrack& tr : c.tracks) {
            if (tr.target == "Hips" && !tr.rotation.empty()) hips = sampleTrack(tr, t).rotation;
            if (tr.target == "UpperArm.L" && !tr.rotation.empty()) upper = sampleTrack(tr, t).rotation;
        }
        return normalize(rotate(normalizeQuat(hips * upper), Vec3{0.3f, 0, 0}));
    };
    AnimationClip matched = retarget::retargetClip(clip, from, to, map, "Rig/Raise", true);
    REQUIRE(matched.retarget && matched.retarget->matchRestPose);
    Vec3 start = direction(matched, 0.f), end = direction(matched, 1.f);
    CHECK_NEAR(start.x, 0.7071, 0.02);
    CHECK_NEAR(start.y, -0.7071, 0.02);   // (down at 45 degrees, like the source)
    CHECK_NEAR(end.x, 1.0, 0.02);
    CHECK_NEAR(end.y, 0.0, 0.02);         // (level)
    AnimationClip plain = retarget::retargetClip(clip, from, to, map, "Rig/Raise plain");
    Vec3 plainStart = direction(plain, 0.f), plainEnd = direction(plain, 1.f);
    CHECK_NEAR(plainStart.y, 0.0, 0.02);    // (its own T-pose: the turns are the source's away from its rest)
    CHECK_NEAR(plainEnd.y, 0.7071, 0.02);   // (then raised 45 degrees past level)
    // Saved as its recipe, the setting with it
    Scene s;
    Entity& e = s.create("Rig");
    e.animator = Animator{};
    e.animator->clip = "Rig/Raise";
    MeshLibrary lib = buildMeshLibrary();
    lib.animations.push_back(clip);
    lib.animations.push_back(matched);
    TextureLibrary tex;
    json::Value doc = sceneToJson(s, lib, tex);
    MeshLibrary fresh = buildMeshLibrary();
    fresh.animations.push_back(clip);
    Scene back;
    std::string error;
    REQUIRE(sceneFromJson(doc, back, fresh, tex, error));
    const AnimationClip* again = fresh.findAnimation("Rig/Raise");
    REQUIRE(again && again->retarget);
    CHECK(again->retarget->matchRestPose);
    CHECK_NEAR(direction(*again, 0.f).y, -0.7071, 0.02);
}

TEST(retargeted_clip_saved_as_its_recipe) {
    MeshLibrary lib = buildMeshLibrary();
    AnimationClip source;
    source.name = "Fox/Run";
    source.duration = 1.f;
    AnimationTrack tr{"Thigh_L", {}, {}, {}};
    for (int k = 0; k <= 4; ++k) {
        Quat q = axisAngle({1, 0, 0}, 0.3f * (float)k);
        tr.rotation.times.push_back((float)k / 4.f);
        tr.rotation.values.insert(tr.rotation.values.end(), {q.x, q.y, q.z, q.w});
    }
    tr.rotation.width = 4;
    source.tracks = {tr};
    lib.animations.push_back(source);
    Skeleton from, to;
    from.bones = {bone("Hips", -1, {0, 1, 0}), bone("Thigh_L", 0, {0.1f, 0, 0})};
    to.bones = {bone("pelvis", -1, {0, 1, 0}), bone("mixamorig:LeftUpLeg", 0, {0.1f, 0, 0})};
    lib.animations.push_back(retarget::retargetClip(source, from, to, retarget::mapBones(from, to), "Robot/Run"));
    size_t keys = keyCount(lib.animations.back());
    Scene s;
    Entity& robot = s.create("Robot");
    robot.animator = Animator{};
    robot.animator->clip = "Robot/Run";
    TextureLibrary tex;
    json::Value doc = sceneToJson(s, lib, tex);
    // Loaded where only the source clip exists: the retargeted one is made again from its recipe
    MeshLibrary fresh = buildMeshLibrary();
    fresh.animations.push_back(source);
    Scene back;
    std::string error;
    REQUIRE(sceneFromJson(doc, back, fresh, tex, error));
    const AnimationClip* made = fresh.findAnimation("Robot/Run");
    REQUIRE(made && made->retarget);
    CHECK(keyCount(*made) == keys);
    CHECK(made->tracks.size() == 1 && made->tracks[0].target == "mixamorig:LeftUpLeg");
}

TEST(animation_compression) {
    AnimationClip c;
    c.duration = 2.f;
    AnimationTrack t{"Bone", {}, {}, {}};
    for (int k = 0; k <= 100; ++k) {   // a straight line (100 keys too many) and a turn at a steady rate
        float time = (float)k / 50.f;
        t.translation.times.push_back(time);
        t.translation.values.insert(t.translation.values.end(), {time * 3.f, 1.f, -time});
        Quat q = axisAngle({0, 1, 0}, std::sin(time * 3.f));
        t.rotation.times.push_back(time);
        t.rotation.values.insert(t.rotation.values.end(), {q.x, q.y, q.z, q.w});
    }
    t.translation.width = 3;
    t.rotation.width = 4;
    c.tracks = {t};
    AnimationClip original = c;
    size_t removed = compressClip(c, 0.1f, 0.0005f, 0.0005f);
    CHECK(c.tracks[0].translation.times.size() == 2);
    CHECK(c.tracks[0].rotation.times.size() < 60);
    CHECK(removed == keyCount(original) - keyCount(c));
    for (float time = 0.f; time <= 2.f; time += 0.013f) {   // still the same motion, within the tolerance
        AnimationPose a = sampleTrack(original.tracks[0], time), b = sampleTrack(c.tracks[0], time);
        CHECK_VEC(a.translation, b.translation, 1e-3f);
        CHECK(quatAngle(a.rotation * conjugate(b.rotation)) < 0.25f * DEG2RAD);
    }
}

// Foot placement: a foot over a step goes up onto it; a foot over a dip makes the hips drop so it reaches down
TEST(foot_ik_places_feet) {
    test::ImGuiContextGuard imgui;
    MeshLibrary lib = buildMeshLibrary();
    for (float ground : {0.25f, -0.2f}) {
        Scene s;
        Entity& floor = s.create("Floor");
        floor.transform.position[1] = -0.5f;
        floor.collider = Collider{};
        floor.collider->size = {20, 1, 20};
        Entity& step = s.create("Step");   // under the left foot only
        step.transform.position[0] = 2.f;   // (from x = 0 on: under the left foot, at x = 0.15)
        step.transform.position[1] = ground - 0.5f;
        step.collider = Collider{};
        step.collider->size = {4, 1, 4};
        if (ground < 0.f) {   // (a dip: the floor there is lower)
            s.remove(s.findByName("Step")->id);
            Entity& f = *s.findByName("Floor");
            f.transform.position[0] = -10.f + 0.05f;   // the floor ends between the feet: the left one over the dip
            Entity& low = s.create("Low");
            low.transform.position[0] = 10.f + 0.05f;
            low.transform.position[1] = ground - 0.5f;
            low.collider = Collider{};
            low.collider->size = {20, 1, 20};
        }
        Entity& body = s.create("Body");
        body.animator = Animator{};
        for (const char* side : {"Left", "Right"}) {
            AnimatorIK ik;
            ik.name = std::string(side) + "Foot";
            ik.bone = std::string(side) + "Foot";
            ik.pole = {0, 0, 1};
            body.animator->ik.push_back(ik);
        }
        body.behaviours.emplace_back(createBehaviour("FootIK"));
        EntityId root = body.id;
        Entity& hips = s.create("Hips", root);
        hips.transform.position[1] = 0.9f;
        EntityId hipsId = hips.id;
        for (float x : {0.15f, -0.15f}) {
            bool left = x > 0.f;
            Entity& thigh = s.create(left ? "LeftThigh" : "RightThigh", hipsId);
            thigh.transform.position[0] = x;
            EntityId thighId = thigh.id;
            Entity& calf = s.create(left ? "LeftCalf" : "RightCalf", thighId);
            calf.transform.position[1] = -0.42f;
            calf.transform.position[2] = 0.02f;   // (a slight bend: the knee knows which way)
            EntityId calfId = calf.id;
            Entity& foot = s.create(left ? "LeftFoot" : "RightFoot", calfId);
            foot.transform.position[1] = -0.42f;
            foot.transform.position[2] = -0.02f;
        }
        Game g;
        g.start(s, lib);
        for (int i = 0; i < 120; ++i) {
            ImGui::NewFrame();
            g.frame(1.f / 60.f, true, true);
            ImGui::EndFrame();
        }
        Mat4 l = s.worldMatrix(s.findByName("LeftFoot")->id), r = s.worldMatrix(s.findByName("RightFoot")->id);
        Mat4 h = s.worldMatrix(s.findByName("Hips")->id);
        if (ground > 0.f) {
            CHECK_NEAR(l.m[13], 0.06 + ground, 0.05);   // up on the step
            CHECK_NEAR(r.m[13], 0.06, 0.05);            // the other where it was
        } else {
            CHECK_NEAR(l.m[13], 0.06 + ground, 0.05);   // down into the dip
            CHECK_NEAR(h.m[13], 0.9 + ground, 0.05);    // the hips dropped with it
            CHECK_NEAR(r.m[13], 0.06, 0.05);            // the other still on the floor (its knee bent)
        }
        g.stop();
    }
}

namespace {

// Where a bone is, in the skeleton's space, with a clip posed at `time` (bones without a track: at rest)
Vec3 posedAt(const Skeleton& s, const AnimationClip& clip, float time, const char* name) {
    std::vector<Quat> rot(s.bones.size());
    std::vector<Vec3> pos(s.bones.size());
    for (size_t i = 0; i < s.bones.size(); ++i) {
        const Skeleton::Bone& b = s.bones[i];
        Quat r = b.rotation;
        Vec3 t = b.translation;
        for (const AnimationTrack& tr : clip.tracks)
            if (tr.target == b.name) {
                AnimationPose p = sampleTrack(tr, time);
                if (p.hasRotation) r = p.rotation;
                if (p.hasTranslation) t = p.translation;
            }
        rot[i] = b.parent >= 0 ? normalizeQuat(rot[(size_t)b.parent] * r) : r;
        pos[i] = b.parent >= 0 ? pos[(size_t)b.parent] + rotate(rot[(size_t)b.parent], t) : t;
    }
    return pos[(size_t)s.find(name)];
}

void key(AnimationTrack& tr, float t, Quat q) {
    tr.rotation.times.push_back(t);
    tr.rotation.values.insert(tr.rotation.values.end(), {q.x, q.y, q.z, q.w});
    tr.rotation.width = 4;
}

}  // namespace

// A step with a planted foot: the source's hips move forward while its leg (two halves of 0.5 m) bends to keep the
// foot still. On a model 1.2 times as tall, with a long thigh and a short calf, copying the turns slides the foot;
// with the feet planted it stays where the source's is (scaled), within a few millimetres
TEST(retarget_plants_feet) {
    Skeleton from, to;
    from.bones = {bone("Hips", -1, {0, 1, 0}), bone("thigh.l", 0, {0.1f, 0, 0}), bone("calf.l", 1, {0, -0.5f, 0}),
                  bone("foot.l", 2, {0, -0.5f, 0})};
    to.bones = {bone("Hips", -1, {0, 1.2f, 0}), bone("thigh.l", 0, {0.12f, 0, 0}), bone("calf.l", 1, {0, -0.75f, 0}),
                bone("foot.l", 2, {0, -0.45f, 0})};
    AnimationClip clip;
    clip.name = "Step";
    clip.duration = 1.f;
    AnimationTrack hips{"Hips", {}, {}, {}}, thigh{"thigh.l", {}, {}, {}}, calf{"calf.l", {}, {}, {}};
    const Vec3 planted{0.1f, 0.05f, 0.15f};
    for (int k = 0; k <= 20; ++k) {   // the leg solved by hand: the foot kept at `planted` (knee bending forwards)
        float t = (float)k / 20.f, z = -0.15f + 0.3f * t;
        hips.translation.times.push_back(t);
        hips.translation.values.insert(hips.translation.values.end(), {0.f, 1.f, z});
        hips.translation.width = 3;
        float dy = planted.y - 1.f, dz = planted.z - z, d = std::sqrt(dy * dy + dz * dz);
        float knee = std::acos(std::clamp((0.25f + 0.25f - d * d) / (2.f * 0.25f), -1.f, 1.f));   // inner angle
        float toFoot = std::atan2(-dz, -dy);   // (about x: 0 straight down, + towards -z)
        float a = std::acos(std::clamp(d / 1.f, -1.f, 1.f));   // the thigh's angle off the hip-foot line
        key(thigh, t, axisAngle({1, 0, 0}, toFoot - a));
        key(calf, t, axisAngle({1, 0, 0}, PI - knee));
    }
    clip.tracks = {hips, thigh, calf};
    REQUIRE(length(posedAt(from, clip, 0.f, "foot.l") - planted) < 2e-3f);   // (the source's foot is planted)
    REQUIRE(length(posedAt(from, clip, 1.f, "foot.l") - planted) < 2e-3f);
    auto map = retarget::mapBones(from, to);
    AnimationClip copied = retarget::retargetClip(clip, from, to, map, "Tall/Step");
    AnimationClip planted2 = retarget::retargetClip(clip, from, to, map, "Tall/Step planted", false, true);
    REQUIRE(planted2.retarget && planted2.retarget->plantFeet);
    Vec3 goal = Vec3{0, 1.2f, 0} + (planted - Vec3{0, 1, 0}) * 1.2f;   // (scaled about the root by the heights)
    float slide = 0.f, off = 0.f;
    for (float t : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
        slide = std::max(slide, length(posedAt(to, copied, t, "foot.l") - posedAt(to, copied, 0.f, "foot.l")));
        off = std::max(off, length(posedAt(to, planted2, t, "foot.l") - goal));
    }
    CHECK(slide > 0.03f);   // copying the turns: the foot slides
    CHECK(off < 0.005f);    // planted
    // (both ways, with rest poses matched too)
    AnimationClip matched = retarget::retargetClip(clip, from, to, map, "Tall/Step matched", true, true);
    for (float t : {0.f, 0.5f, 1.f}) CHECK(length(posedAt(to, matched, t, "foot.l") - goal) < 0.005f);
}

// A mirrored clip: the left leg's swing played by the right one (whatever their local axes), the hips' travel and
// turn mirrored across x; bones without a side move as before, mirrored; saved as its recipe
TEST(mirrored_clips) {
    Skeleton s;
    Quat axesL = axisAngle({0, 0, 1}, 90.f * DEG2RAD), axesR = axisAngle({0, 0, 1}, -90.f * DEG2RAD);   // (mirrored axes)
    s.bones = {bone("Hips", -1, {0, 1, 0}), bone("LeftUpLeg", 0, {0.15f, 0, 0}, axesL), bone("LeftLeg", 1, {0.5f, 0, 0}),
               bone("RightUpLeg", 0, {-0.15f, 0, 0}, axesR), bone("RightLeg", 3, {-0.5f, 0, 0}), bone("Spine", 0, {0, 0.2f, 0})};
    std::vector<int> other = retarget::mirrorBones(s);
    CHECK(other[1] == 3 && other[3] == 1 && other[2] == 4 && other[0] == 0 && other[5] == 5);
    AnimationClip clip;
    clip.name = "Kick";
    clip.duration = 1.f;
    AnimationTrack hips{"Hips", {}, {}, {}}, left{"LeftUpLeg", {}, {}, {}};
    for (int k = 0; k <= 10; ++k) {
        float t = (float)k / 10.f;
        hips.translation.times.push_back(t);
        hips.translation.values.insert(hips.translation.values.end(), {0.5f * t, 1.f, 0.2f * t});
        hips.translation.width = 3;
        key(hips, t, axisAngle({0, 1, 0}, 30.f * DEG2RAD * t));
        key(left, t, normalizeQuat(axisAngle({1, 0, 0}, -45.f * DEG2RAD * t) * axesL));   // (swings, on its own axes)
    }
    clip.tracks = {hips, left};
    AnimationClip m = retarget::mirrorClip(clip, s, "Kick (mirrored)");
    REQUIRE(m.retarget && m.retarget->mirror);
    for (float t : {0.f, 0.5f, 1.f}) {
        Vec3 l = posedAt(s, clip, t, "LeftLeg"), r = posedAt(s, m, t, "RightLeg");
        CHECK_VEC(r, test::v3(-l.x, l.y, l.z), 2e-3f);   // the right foot where the left one was, across x
        Vec3 h = posedAt(s, clip, t, "Spine"), hm = posedAt(s, m, t, "Spine");
        CHECK_VEC(hm, test::v3(-h.x, h.y, h.z), 2e-3f);   // the hips (and what's on them) mirrored: travel and turn
        Vec3 l2 = posedAt(s, m, t, "LeftLeg"), r0 = posedAt(s, clip, t, "RightLeg");
        CHECK_VEC(l2, test::v3(-r0.x, r0.y, r0.z), 2e-3f);   // ... and the left one where the right one was
    }
    // Saved as its recipe, made again on loading
    MeshLibrary lib = buildMeshLibrary();
    lib.animations.push_back(clip);
    lib.animations.push_back(m);
    Scene scene;
    Entity& e = scene.create("Kicker");
    e.animator = Animator{};
    e.animator->clip = "Kick (mirrored)";
    TextureLibrary tex;
    json::Value doc = sceneToJson(scene, lib, tex);
    MeshLibrary fresh = buildMeshLibrary();
    fresh.animations.push_back(clip);
    Scene back;
    std::string error;
    REQUIRE(sceneFromJson(doc, back, fresh, tex, error));
    const AnimationClip* made = fresh.findAnimation("Kick (mirrored)");
    REQUIRE(made && made->retarget && made->retarget->mirror);
    Vec3 a = posedAt(s, *made, 1.f, "RightLeg"), b = posedAt(s, m, 1.f, "RightLeg");
    CHECK_VEC(a, b, 1e-4f);
}

TEST(camera_shake_offsets) {
    shake::Offset none = shake::at(0.f, 1.f);
    CHECK(none.yaw == 0.f && none.pitch == 0.f && length(none.move) == 0.f);
    float most = 0.f, changed = 0.f;
    for (int k = 0; k < 200; ++k) {
        shake::Offset o = shake::at(1.f, (float)k * 0.01f), again = shake::at(1.f, (float)k * 0.01f);
        CHECK(o.yaw == again.yaw && o.roll == again.roll);   // (the same at the same time)
        most = std::max({most, std::fabs(o.yaw), std::fabs(o.pitch)});
        changed = std::max(changed, std::fabs(o.yaw - shake::at(1.f, (float)k * 0.01f + 0.05f).yaw));
    }
    CHECK(most <= shake::kMaxAngle + 1e-6f && most > 0.5f * shake::kMaxAngle);
    CHECK(changed > 0.f);
    CHECK(std::fabs(shake::at(0.5f, 0.3f).yaw) <= 0.25f * shake::kMaxAngle + 1e-6f);   // (trauma squared)
    CHECK(shake::falloff(1.f, 2.f, 25.f) == 1.f);
    CHECK(shake::falloff(1.f, 30.f, 25.f) == 0.f);
    float mid = shake::falloff(1.f, 14.f, 25.f);
    CHECK(mid > 0.f && mid < 1.f);
}
