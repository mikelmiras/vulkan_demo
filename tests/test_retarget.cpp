// Animation: retargeting between skeletons (assets/retarget.h), clip compression, foot placement (FootIK)
#include "test.h"

#include <cmath>

#include "assets/retarget.h"
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
