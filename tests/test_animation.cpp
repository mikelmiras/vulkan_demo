// Skeletal animation: glTF skins and clips, sampling, the Animator in play mode, skinning matrices, saving
#include "test.h"

#include "assets/gltf.h"
#include "core/project.h"
#include "game/animation.h"
#include "game/game.h"
#include "imgui.h"
#include "render/scene_draw.h"
#include "scene/serialization.h"

#ifdef VKWORLD_TEST_ASSETS

namespace {

std::string barPath() { return std::string(VKWORLD_TEST_ASSETS) + "/bending_bar.gltf"; }

Vec3 worldPos(const Scene& s, const char* name) {
    const Entity* e = s.findByName(name);
    Mat4 m = s.worldMatrix(e ? e->id : kNoEntity);
    return {m.m[12], m.m[13], m.m[14]};
}

// The bar as a scene: its prefab instantiated at the origin
bool barScene(MeshLibrary& meshes, Scene& s) {
    Prefab p;
    std::string error, warning;
    if (!importGltfPrefab(barPath(), meshes, textures(), p, error, warning)) {
        std::printf("    %s\n", error.c_str());
        return false;
    }
    for (Entity& e : s.instantiate(p)) s.add(std::move(e));
    return true;
}

void steps(Game& g, int n) {
    for (int i = 0; i < n; ++i) {
        ImGui::NewFrame();
        g.step();
        ImGui::EndFrame();
    }
}

}  // namespace

TEST(animation_gltf_import) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    REQUIRE(barScene(meshes, s));
    REQUIRE(meshes.animations.size() == 2);
    const AnimationClip* bend = meshes.findAnimation("bending_bar/Bend");
    REQUIRE(bend);
    CHECK_NEAR(bend->duration, 1.0, 1e-6);
    CHECK(bend->tracks.size() == 2);
    CHECK(bend->source.find("#anim0") != std::string::npos);
    const Entity* root = s.findByName("bending_bar");
    REQUIRE(root && root->animator);
    CHECK(root->animator->clip == "bending_bar/Bend");
    const Entity* body = s.findByName("Body");
    REQUIRE(body && body->skin && body->meshRenderer);
    CHECK(meshes.get(body->meshRenderer->mesh).skinned);
    REQUIRE(body->skin->joints.size() == 2);
    CHECK(body->skin->joints[1] == "Bone1");
    CHECK_NEAR(body->skin->inverseBind[1].m[13], -1.0, 1e-6);   // bone 1 sits 1 m up in the bind pose
    CHECK(s.modelRoot(body->id) == root->id);
    // Importing again reuses the clips
    std::string error;
    CHECK(importAnimationClip(meshes, bend->source, error) == 0);
    CHECK(meshes.animations.size() == 2);
}

TEST(animation_sampling) {
    AnimationTrack t;
    t.target = "X";
    t.translation = {{0.f, 2.f}, {0, 0, 0, 4, 0, 0}, 3, AnimationKeys::Interpolation::Linear};
    float s = std::sin(PI / 4), c = std::cos(PI / 4);
    t.rotation = {{0.f, 1.f}, {0, 0, 0, 1, 0, 0, s, c}, 4, AnimationKeys::Interpolation::Linear};   // 0 -> 90 deg about Z
    AnimationPose p = sampleTrack(t, 1.f);
    CHECK(p.hasTranslation && p.hasRotation && !p.hasScale);
    CHECK_VEC(p.translation, test::v3(2, 0, 0), 1e-5f);
    Vec3 x = transformDir(composeTRS({}, p.rotation, {1, 1, 1}), {1, 0, 0});
    CHECK_VEC(x, test::v3(0, 1, 0), 1e-5f);
    p = sampleTrack(t, 0.5f);   // slerp: 45 degrees
    x = transformDir(composeTRS({}, p.rotation, {1, 1, 1}), {1, 0, 0});
    CHECK_VEC(x, test::v3(std::sqrt(0.5f), std::sqrt(0.5f), 0), 1e-4f);
    CHECK_VEC(sampleTrack(t, 9.f).translation, test::v3(4, 0, 0), 1e-6f);   // clamped
    t.translation.interpolation = AnimationKeys::Interpolation::Step;
    CHECK_VEC(sampleTrack(t, 1.9f).translation, test::v3(0, 0, 0), 1e-6f);
    // Rotation round trip through a matrix
    Quat q = rotationOf(composeTRS({1, 2, 3}, normalizeQuat({0.1f, 0.7f, -0.2f, 0.6f}), {2, 2, 2}));
    Quat r = normalizeQuat({0.1f, 0.7f, -0.2f, 0.6f});
    CHECK_NEAR(std::fabs(q.x * r.x + q.y * r.y + q.z * r.z + q.w * r.w), 1.0, 1e-5);
}

// Played: each joint bends 90 degrees by 0.5 s, and the skinning matrices put the bar's tip where they should
TEST(animation_plays_and_skins) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    REQUIRE(barScene(meshes, s));
    Game g;
    g.start(s, meshes);
    CHECK_VEC(worldPos(s, "Bone1"), test::v3(0, 1, 0), 1e-4f);
    steps(g, 30);   // 0.5 s
    CHECK_VEC(worldPos(s, "Bone1"), test::v3(-1, 0, 0), 1e-3f);

    std::vector<Mat4> joints;
    std::vector<DrawItem> items = collectDrawItems(s, s.worldMatrices(), kNoEntity, &joints, &meshes);
    const DrawItem* body = nullptr;
    for (const DrawItem& d : items)
        if (d.jointOffset >= 0) body = &d;
    REQUIRE(body);
    REQUIRE(joints.size() == (size_t)body->jointOffset + 2);
    // The tip (y = 2, all on joint 1): model * joint matrix * vertex
    Vec3 tip = transformPoint(body->model * joints[(size_t)body->jointOffset + 1], {0, 2, 0});
    CHECK_VEC(tip, test::v3(-1, -1, 0), 1e-3f);
    // The base (all on joint 0, which only rotates in place) stays at the origin
    CHECK_VEC(transformPoint(body->model * joints[(size_t)body->jointOffset], {0, 0, 0}), test::v3(0, 0, 0), 1e-4f);
    // Without a palette, skinned meshes are drawn plain
    items = collectDrawItems(s, s.worldMatrices());
    for (const DrawItem& d : items) CHECK(d.jointOffset == -1);
}

TEST(animation_crossfade) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    REQUIRE(barScene(meshes, s));
    Game g;
    g.start(s, meshes);
    Animator& a = *s.findByName("bending_bar")->animator;
    a.play("bending_bar/Stretch", 0.5f);
    CHECK(a.fadeFrom == "bending_bar/Bend");
    steps(g, 15);   // 0.25 s into a 0.5 s fade
    CHECK(!s.findByName("bending_bar")->animator->fadeFrom.empty());
    steps(g, 20);
    CHECK(s.findByName("bending_bar")->animator->fadeFrom.empty());   // done
    // Stretch alone now: at 0.5833 s the top bone is on its way back from 2 m away (at 0.5 s) to 1 m. (Bone 0 keeps
    // the bend the fade left it with: Stretch doesn't move it.)
    CHECK_NEAR(length(worldPos(s, "Bone1") - worldPos(s, "Bone0")), 2.0 - 0.0833 / 0.5, 0.02);
    a = *s.findByName("bending_bar")->animator;
    s.findByName("bending_bar")->animator->play("bending_bar/Stretch");   // already playing: no restart
    CHECK(s.findByName("bending_bar")->animator->time > 0.5f);
}

// Skins and animators survive a scene file round trip, clips re-imported into a fresh library
TEST(animation_saved_with_scene) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    REQUIRE(barScene(meshes, s));
    s.findByName("bending_bar")->animator->speed = 1.5f;
    std::string error;
    auto path = test::tempPath("animated.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    MeshLibrary fresh = buildMeshLibrary();
    Scene loaded;
    REQUIRE(loadScene(path, loaded, fresh, textures(), error));
    CHECK(fresh.animations.size() == 2);
    const Entity* root = loaded.findByName("bending_bar");
    REQUIRE(root && root->animator);
    CHECK(fresh.findAnimation(root->animator->clip) != nullptr);
    CHECK_NEAR(root->animator->speed, 1.5, 1e-6);
    CHECK(!root->animator->playing);   // runtime state stays out of scene files
    const Entity* body = loaded.findByName("Body");
    REQUIRE(body && body->skin && body->skin->joints.size() == 2);
    CHECK_NEAR(body->skin->inverseBind[1].m[13], -1.0, 1e-6);
    CHECK(fresh.get(body->meshRenderer->mesh).skinned);
}

TEST(animation_save_game_keeps_the_pose) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    REQUIRE(barScene(meshes, s));
    Game g;
    g.setAssetLibraries(meshes, textures());
    g.setSaveDirectory(test::tempPath("anim_saves"));
    g.start(s, meshes);
    steps(g, 20);
    Vec3 bone = worldPos(s, "Bone1");
    float t = s.findByName("bending_bar")->animator->time;
    std::string error;
    REQUIRE(g.saveGame("a", error));
    steps(g, 20);
    REQUIRE(g.loadGame("a", error));
    CHECK_NEAR(s.findByName("bending_bar")->animator->time, t, 1e-5);
    CHECK_VEC(worldPos(s, "Bone1"), bone, 1e-3f);
}

namespace {

struct EventLog : BehaviourBase<EventLog> {
    static constexpr const char* kTypeName = "EventLog";
    std::vector<std::pair<float, std::string>> heard;   // (game time, event)
    void onAnimationEvent(GameContext& ctx, Entity&, const std::string& event) override { heard.emplace_back(ctx.time(), event); }
};

}  // namespace

// Events fire once each time playback passes them, also across the loop's wrap, and reach the behaviours
TEST(animation_events) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    REQUIRE(barScene(meshes, s));
    Entity& root = *s.findByName("bending_bar");
    root.animator->events = {{"bending_bar/Bend", 0.25f, "quarter"}, {"bending_bar/Bend", 0.f, "start"},
                             {"bending_bar/Stretch", 0.5f, "other clip"}};
    root.behaviours.emplace_back(std::make_unique<EventLog>());
    Game g;
    g.start(s, meshes);
    steps(g, 150);   // 2.5 s of a 1 s looping clip
    const auto& heard = static_cast<EventLog&>(*s.findByName("bending_bar")->behaviours[0]).heard;
    int quarters = 0, starts = 0;
    for (const auto& h : heard) {
        quarters += h.second == "quarter";
        starts += h.second == "start";
        CHECK(h.second != "other clip");
    }
    CHECK(quarters == 3);   // at 0.25, 1.25, 2.25 s
    CHECK(starts == 2);     // the wraps at 1 and 2 s
    REQUIRE(!heard.empty());
    CHECK_NEAR(heard[0].first, 0.25, 1.0 / 60.0 + 1e-4);

    // Plain updateAnimators reports them too
    std::vector<FiredAnimationEvent> fired;
    updateAnimators(s, meshes, 1.f, &fired);
    CHECK(fired.size() == 2);
}

// A state machine: parameters and clip ends move it between states, with cross-fades
TEST(animation_state_machine) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    REQUIRE(barScene(meshes, s));
    Animator& a = *s.findByName("bending_bar")->animator;
    a.states = {{"Idle", "bending_bar/Bend", 1.f, true}, {"Run", "bending_bar/Stretch", 2.f, true},
                {"Jump", "bending_bar/Bend", 1.f, false}};
    AnimatorTransition toRun;
    toRun.from = "Idle";
    toRun.to = "Run";
    toRun.parameter = "speed";
    toRun.value = 0.5f;
    toRun.fade = 0.25f;
    AnimatorTransition toIdle = toRun;
    toIdle.from = "Run";
    toIdle.to = "Idle";
    toIdle.when = AnimatorTransition::When::Less;
    AnimatorTransition jump;   // from any state
    jump.to = "Jump";
    jump.parameter = "jump";
    jump.when = AnimatorTransition::When::IsTrue;
    jump.fade = 0.f;
    AnimatorTransition landed;
    landed.from = "Jump";
    landed.to = "Idle";
    landed.when = AnimatorTransition::When::ClipEnded;
    a.transitions = {toRun, toIdle, jump, landed};
    a.startState = "Idle";
    Game g;
    g.start(s, meshes);
    auto anim = [&]() -> Animator& { return *s.findByName("bending_bar")->animator; };
    CHECK(anim().state == "Idle");
    CHECK(anim().clip == "bending_bar/Bend");
    steps(g, 10);
    CHECK(anim().state == "Idle");
    anim().setParameter("speed", 1.f);
    steps(g, 1);
    CHECK(anim().state == "Run");
    CHECK(anim().clip == "bending_bar/Stretch");
    CHECK(anim().fadeFrom == "bending_bar/Bend");
    CHECK_NEAR(anim().speed, 2.0, 1e-6);
    anim().setParameter("speed", 0.f);
    steps(g, 1);
    CHECK(anim().state == "Idle");
    anim().setParameter("jump", 1.f);
    steps(g, 1);
    CHECK(anim().state == "Jump");
    CHECK(!anim().loop);
    CHECK(anim().fadeFrom.empty());   // no fade asked for
    anim().setParameter("jump", 0.f);
    steps(g, 30);
    CHECK(anim().state == "Jump");   // half way through the 1 s clip
    steps(g, 35);
    CHECK(anim().state == "Idle");   // ended: back to idle
    CHECK(anim().loop);

    // The machine is saved with the scene; its runtime state too, as the game is running (a save game)
    std::string error;
    auto path = test::tempPath("state_machine.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    MeshLibrary fresh = buildMeshLibrary();
    Scene loaded;
    REQUIRE(loadScene(path, loaded, fresh, textures(), error));
    const Animator& b = *loaded.findByName("bending_bar")->animator;
    REQUIRE(b.states.size() == 3);
    CHECK(b.states[1].name == "Run" && fresh.findAnimation(b.states[1].clip) && b.states[1].speed == 2.f);
    CHECK(!b.states[2].loop);
    REQUIRE(b.transitions.size() == 4);
    CHECK(b.transitions[1].from == "Run" && b.transitions[1].when == AnimatorTransition::When::Less);
    CHECK(b.transitions[2].from == "*" && b.transitions[2].when == AnimatorTransition::When::IsTrue);
    CHECK(b.transitions[3].when == AnimatorTransition::When::ClipEnded);
    CHECK_NEAR(b.transitions[0].fade, 0.25, 1e-6);
    CHECK(b.startState == "Idle");
    CHECK(b.state == "Idle");
    CHECK(b.parameter("speed") == 0.f && b.parameters.size() == 2);
}

// The editor's preview: a clip posed at a time
TEST(animation_pose_clip) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    REQUIRE(barScene(meshes, s));
    EntityId root = s.findByName("bending_bar")->id;
    poseClip(s, meshes, root, "bending_bar/Bend", 0.5f);
    CHECK_VEC(worldPos(s, "Bone1"), test::v3(-1, 0, 0), 1e-3f);
    CHECK(!s.findByName("bending_bar")->animator->playing);
}

#endif

#ifdef VKWORLD_TEST_ASSETS
TEST(gltf_material_values) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    REQUIRE(barScene(meshes, s));
    const Entity* body = s.findByName("Body");
    REQUIRE(body && body->meshRenderer);
    CHECK_NEAR(body->meshRenderer->metallic, 0.0, 1e-6);
    CHECK_NEAR(body->meshRenderer->roughness, 0.6, 1e-6);
}
#endif
