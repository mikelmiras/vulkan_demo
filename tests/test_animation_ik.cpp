// Animation, round 4: 2D blend spaces, additive layers, two-bone and look-at IK, events on blend trees
#include "test.h"

#include "core/json.h"
#include "game/animation.h"
#include "scene/serialization.h"

namespace {

// A clip moving one node from `a` to `b` (translation) over a second
AnimationClip moveClip(const std::string& name, const std::string& target, Vec3 a, Vec3 b, float duration = 1.f) {
    AnimationClip c;
    c.name = name;
    c.duration = duration;
    AnimationTrack t;
    t.target = target;
    t.translation.times = {0.f, duration};
    t.translation.values = {a.x, a.y, a.z, b.x, b.y, b.z};
    t.translation.width = 3;
    c.tracks.push_back(t);
    return c;
}

// A clip turning a node about Y from 0 to `degrees`
AnimationClip turnClip(const std::string& name, const std::string& target, float degrees) {
    AnimationClip c;
    c.name = name;
    c.duration = 1.f;
    AnimationTrack t;
    t.target = target;
    Quat q = axisAngle({0, 1, 0}, degrees * DEG2RAD);
    t.rotation.times = {0.f, 1.f};
    t.rotation.values = {0, 0, 0, 1, q.x, q.y, q.z, q.w};
    t.rotation.width = 4;
    c.tracks.push_back(t);
    return c;
}

Vec3 worldPos(const Scene& s, const char* name) {
    Mat4 m = s.worldMatrix(s.findByName(name)->id);
    return {m.m[12], m.m[13], m.m[14]};
}

}  // namespace

TEST(blend_space_2d_weights) {
    std::vector<std::pair<float, float>> pts = {{0, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    std::vector<float> w = blendWeights2D(pts, 1.f, 0.f);   // on a point: all of it
    CHECK_NEAR(w[1], 1.f, 1e-5);
    w = blendWeights2D(pts, 0.5f, 0.f);   // halfway from the centre to the right
    CHECK_NEAR(w[0], 0.5f, 1e-5);
    CHECK_NEAR(w[1], 0.5f, 1e-5);
    w = blendWeights2D(pts, 0.3f, 0.4f);
    float sum = 0.f;
    for (float v : w) sum += v;
    CHECK_NEAR(sum, 1.f, 1e-5);
    CHECK(w[3] == 0.f && w[4] == 0.f);   // (the far side gets nothing)
    CHECK(w[1] > 0.f && w[2] > w[1]);
    w = blendWeights2D({{0, 0}}, 5.f, 5.f);   // one point: it
    CHECK(w[0] == 1.f);
}

// A 2D blend state: the node goes where the clips around the parameters put it
TEST(blend_space_2d_animator) {
    MeshLibrary meshes = buildMeshLibrary();
    meshes.animations.push_back(moveClip("idle", "Body", {0, 0, 0}, {0, 0, 0}));
    meshes.animations.push_back(moveClip("fwd", "Body", {0, 0, -2}, {0, 0, -2}));
    meshes.animations.push_back(moveClip("right", "Body", {2, 0, 0}, {2, 0, 0}));
    meshes.animations.push_back(moveClip("left", "Body", {-2, 0, 0}, {-2, 0, 0}));
    Scene s;
    Entity& root = s.create("Root");
    s.create("Body", root.id);
    Animator a;
    AnimatorState st;
    st.name = "Move";
    st.blendParameter = "x";
    st.blendParameterY = "y";
    st.blend = {{"idle", 0, 0}, {"fwd", 0, 1}, {"right", 1, 0}, {"left", -1, 0}};
    a.states.push_back(st);
    REQUIRE(enterAnimatorState(a, "Move", 0.f));
    a.setParameter("x", 1.f);
    s.findByName("Root")->animator = a;
    updateAnimators(s, meshes, 0.1f);
    CHECK_NEAR(worldPos(s, "Body").x, 2.f, 1e-4);
    s.findByName("Root")->animator->setParameter("x", 0.5f);
    s.findByName("Root")->animator->setParameter("y", 0.5f);
    updateAnimators(s, meshes, 0.1f);
    Vec3 p = worldPos(s, "Body");
    CHECK(p.x > 0.2f && p.z < -0.2f);   // between right and forward
}

// An additive layer adds its turn on top of the pose underneath
TEST(animation_additive_layer) {
    MeshLibrary meshes = buildMeshLibrary();
    meshes.animations.push_back(turnClip("base", "Head", 30.f));
    meshes.animations.push_back(turnClip("nod", "Head", 20.f));
    Scene s;
    Entity& root = s.create("Root");
    s.create("Head", root.id);
    Animator a;
    a.clip = "base";
    a.playing = true;
    a.loop = false;
    AnimatorLayer l;
    l.clip = "nod";
    l.additive = true;
    l.playing = true;
    l.loop = false;
    a.layers.push_back(l);
    s.findByName("Root")->animator = a;
    updateAnimators(s, meshes, 1.f);   // both at their ends: 30 + 20
    Quat q = rotationOf(s.findByName("Head")->transform.matrix());
    CHECK_NEAR(quatAngle(q) / DEG2RAD, 50.f, 0.1);
    s.findByName("Root")->animator->layers[0].weight = 0.5f;
    updateAnimators(s, meshes, 0.f);
    q = rotationOf(s.findByName("Head")->transform.matrix());
    CHECK_NEAR(quatAngle(q) / DEG2RAD, 40.f, 0.1);
    // Not additive: it replaces
    s.findByName("Root")->animator->layers[0].additive = false;
    s.findByName("Root")->animator->layers[0].weight = 1.f;
    updateAnimators(s, meshes, 0.f);
    q = rotationOf(s.findByName("Head")->transform.matrix());
    CHECK_NEAR(quatAngle(q) / DEG2RAD, 20.f, 0.1);
}

// Two-bone IK: an arm reaches a point in reach exactly (the elbow bending towards the pole), and points at one out
// of reach; look-at IK turns a head towards a point, within its limit
TEST(animation_ik) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    Entity& root = s.create("Root");
    Entity& shoulder = s.create("Shoulder", root.id);
    shoulder.transform.position[1] = 2.f;
    Entity& elbow = s.create("Elbow", s.findByName("Shoulder")->id);
    elbow.transform.position[0] = 1.f;   // a straight arm along +X
    Entity& hand = s.create("Hand", s.findByName("Elbow")->id);
    hand.transform.position[0] = 1.f;
    Entity& head = s.create("Head", s.findByName("Root")->id);
    head.transform.position[1] = 3.f;
    Animator a;
    AnimatorIK arm;
    arm.name = "arm";
    arm.bone = "Hand";
    arm.pole = {0, 0, -1};
    a.ik.push_back(arm);
    AnimatorIK look;
    look.name = "look";
    look.type = AnimatorIK::Type::LookAt;
    look.bone = "Head";
    look.aimAxis = {0, 0, -1};
    look.maxAngle = 45.f;
    a.ik.push_back(look);
    s.findByName("Root")->animator = a;

    Animator* an = &*s.findByName("Root")->animator;
    REQUIRE(an->setIK("arm", {1.2f, 2.5f, 0.5f}, 1.f));
    CHECK(!an->setIK("nope", {}, 1.f));
    updateAnimators(s, meshes, 0.f);
    Vec3 h = worldPos(s, "Hand");
    CHECK_NEAR(h.x, 1.2f, 1e-3);
    CHECK_NEAR(h.y, 2.5f, 1e-3);
    CHECK_NEAR(h.z, 0.5f, 1e-3);
    CHECK_NEAR(length(worldPos(s, "Elbow") - Vec3{0, 2, 0}), 1.f, 1e-3);   // bones keep their length
    CHECK(worldPos(s, "Elbow").z < 0.2f);                                  // bent towards the pole (-Z)
    // Out of reach: as far as it goes, towards it
    an->setIK("arm", {0.f, 2.f, 10.f}, 1.f);
    updateAnimators(s, meshes, 0.f);
    h = worldPos(s, "Hand");
    CHECK_NEAR(length(h - Vec3{0, 2, 0}), 2.f, 0.01);
    CHECK(h.z > 1.9f);
    // Look-at: within the limit it points straight at it; beyond, it stops at the limit
    an->setIK("arm", {}, 0.f);
    an->setIK("look", {3.f, 3.f, -3.f}, 1.f);   // 45 degrees to the right of -Z
    updateAnimators(s, meshes, 0.f);
    Vec3 aim = normalize(transformDir(s.worldMatrix(s.findByName("Head")->id), {0, 0, -1}));
    CHECK_NEAR(dot(aim, normalize(Vec3{1, 0, -1})), 1.f, 1e-3);
    s.findByName("Head")->transform = Transform{};
    s.findByName("Head")->transform.position[1] = 3.f;
    an->setIK("look", {3.f, 3.f, 3.f}, 1.f);   // behind: 135 degrees away, limited to 45
    updateAnimators(s, meshes, 0.f);
    aim = normalize(transformDir(s.worldMatrix(s.findByName("Head")->id), {0, 0, -1}));
    CHECK_NEAR(std::acos(std::clamp(dot(aim, Vec3{0, 0, -1}), -1.f, 1.f)) / DEG2RAD, 45.f, 0.5);
    // Saved and loaded
    json::Value v = entityToJson(*s.findByName("Root"), meshes, textures());
    Entity back;
    std::string error;
    REQUIRE(entityFromJson(v, builtinAssetIds(meshes, textures()), back, error));
    REQUIRE(back.animator && back.animator->ik.size() == 2);
    CHECK(back.animator->ik[1].type == AnimatorIK::Type::LookAt && back.animator->ik[1].maxAngle == 45.f);
    CHECK(back.animator->ik[0].pole.z == -1.f);
}

// Events fire on blend trees too (the clip that weighs most, by phase), and on layers' clips
TEST(animation_events_blend_trees_and_layers) {
    MeshLibrary meshes = buildMeshLibrary();
    meshes.animations.push_back(moveClip("walk", "Body", {0, 0, 0}, {0, 0, 1}, 1.f));
    meshes.animations.push_back(moveClip("run", "Body", {0, 0, 0}, {0, 0, 2}, 0.5f));
    meshes.animations.push_back(moveClip("wave", "Arm", {0, 0, 0}, {0, 1, 0}, 1.f));
    Scene s;
    Entity& root = s.create("Root");
    s.create("Body", root.id);
    s.create("Arm", s.findByName("Root")->id);
    Animator a;
    AnimatorState st;
    st.name = "Locomotion";
    st.blendParameter = "speed";
    st.blend = {{"walk", 0.f}, {"run", 1.f}};
    a.states.push_back(st);
    a.events = {{"walk", 0.5f, "step"}, {"run", 0.25f, "stride"}, {"wave", 0.5f, "hi"}};
    REQUIRE(enterAnimatorState(a, "Locomotion", 0.f));
    a.setParameter("speed", 0.2f);   // mostly walking
    AnimatorLayer l;
    l.clip = "wave";
    l.playing = true;
    a.layers.push_back(l);
    s.findByName("Root")->animator = a;
    std::vector<FiredAnimationEvent> fired;
    for (int i = 0; i < 10; ++i) updateAnimators(s, meshes, 0.1f, &fired);   // one walk cycle (0.9 s long here)
    int steps = 0, strides = 0, his = 0;
    for (const auto& f : fired) {
        steps += f.name == "step";
        strides += f.name == "stride";
        his += f.name == "hi";
    }
    CHECK(steps == 1);
    CHECK(strides == 0);
    CHECK(his == 1);
}
