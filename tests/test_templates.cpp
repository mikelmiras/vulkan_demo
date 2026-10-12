// scene/project_templates: new projects that open and play
#include "test.h"

#include "core/project.h"
#include "game/game.h"
#include "imgui.h"
#include "scene/project_templates.h"
#include "scene/serialization.h"
#include "script/lua_script.h"

namespace fs = std::filesystem;
using project_templates::Template;

namespace {

void play(Game& g, int frames) {
    for (int i = 0; i < frames; ++i) {
        ImGui::GetIO().DeltaTime = 1.f / 60.f;
        ImGui::NewFrame();
        g.frame(1.f / 60.f, true, false);
        ImGui::EndFrame();
    }
}

}  // namespace

// Each template: a project with its start scene, which loads
TEST(templates_create) {
    for (int i = 0; i < (int)Template::Count; ++i) {
        Template t = (Template)i;
        fs::path dir = test::tempPath("template_" + std::to_string(i));
        fs::remove_all(dir);
        std::string error;
        REQUIRE(project_templates::create(dir, t, "", error));
        project::Settings settings;
        REQUIRE(project::load(dir, settings, error));
        CHECK(settings.name == dir.filename().u8string());
        CHECK(settings.startScene == "scenes/main.vkw");
        Scene s;
        MeshLibrary lib = buildMeshLibrary();
        REQUIRE(loadScene((dir / "scenes" / "main.vkw").u8string(), s, lib, textures(), error));
        CHECK(s.findByName("Sun") && s.findByName("Ground"));
        int cameras = 0;
        for (const Entity& e : s.entities) cameras += e.camera ? 1 : 0;
        CHECK(cameras == 1);
        CHECK(!project_templates::create(dir, t, "", error));   // (not over a project)
    }
}

// The top-down one plays: its script moves the player with the move keys, and the camera follows
TEST(templates_top_down_plays) {
    test::ImGuiContextGuard imgui;
    fs::path dir = test::tempPath("template_topdown");
    fs::remove_all(dir);
    std::string error;
    REQUIRE(project_templates::create(dir, Template::TopDown, "Top", error));
    scripting::reset();
    CHECK(scripting::loadProject(dir) == 1);
    CHECK(scripting::errors().empty());
    Scene s;
    MeshLibrary lib = buildMeshLibrary();
    REQUIRE(loadScene((dir / "scenes" / "main.vkw").u8string(), s, lib, textures(), error));
    Game g;
    g.start(s, lib);
    play(g, 30);   // (lands)
    Vec3 start = g.context().find(s.findByName("Player")->id)->transform.pos();
    float camToPlayer = g.context().find(s.findByName("Camera")->id)->transform.position[2] - start.z;
    ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
    play(g, 60);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
    play(g, 2);
    Vec3 now = g.context().find(s.findByName("Player")->id)->transform.pos();
    CHECK(now.z < start.z - 3.f);   // walked forward (-Z, up the screen)
    CHECK(std::fabs(now.x - start.x) < 0.1f);
    CHECK(std::fabs(now.y - start.y) < 0.2f);
    CHECK(scripting::errors().empty());
    const Entity* cam = g.context().find(s.findByName("Camera")->id);
    CHECK_NEAR(cam->transform.position[2] - now.z, camToPlayer, 0.05);
    const Entity* body = g.context().find(s.findByName("Player Body")->id);
    CHECK_NEAR(body->transform.rotation[1], 0.0, 1.0);   // facing where it went
    g.stop();
    scripting::reset();
}
