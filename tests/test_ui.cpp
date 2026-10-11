// game/ui: the canvas, buttons, the HUD built-ins and the pause menu
#include "test.h"

#include "game/game.h"
#include "game/ui.h"
#include "imgui.h"

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

// One rendered frame of the game: update, then its UI over an 800 x 600 view at (100, 50)
void frame(Game& g, GameUI& ui, bool mouse = true) {
    ImGui::GetIO().DeltaTime = 1.f / 60.f;
    ImGui::NewFrame();
    g.frame(1.f / 60.f, true, true);
    ui.begin(ImGui::GetBackgroundDrawList(), 100.f, 50.f, 800.f, 600.f, mouse);
    g.drawUI(ui);
    ui.end();
    ImGui::EndFrame();
}

void click(float x, float y) {   // window pixels
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
}

void release() { ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false); }

}  // namespace

TEST(ui_canvas_maps_to_the_view) {
    test::ImGuiContextGuard imgui;
    ImGui::NewFrame();
    GameUI ui;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ui.begin(dl, 100.f, 50.f, 800.f, 600.f, true);
    CHECK_NEAR(ui.height(), 1080.0, 0.0);
    CHECK_NEAR(ui.width(), 800.0 * 1080.0 / 600.0, 1e-3);   // follows the aspect ratio
    int before = dl->VtxBuffer.Size;
    ui.rect(0.f, 0.f, ui.width(), 540.f, {1, 0, 0, 1});      // the top half of the view
    REQUIRE(dl->VtxBuffer.Size >= before + 4);
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (int i = before; i < dl->VtxBuffer.Size; ++i) {
        minX = std::min(minX, dl->VtxBuffer[i].pos.x);
        maxX = std::max(maxX, dl->VtxBuffer[i].pos.x);
        minY = std::min(minY, dl->VtxBuffer[i].pos.y);
        maxY = std::max(maxY, dl->VtxBuffer[i].pos.y);
    }
    CHECK_NEAR(minX, 100.0, 0.5);
    CHECK_NEAR(maxX, 900.0, 0.5);
    CHECK_NEAR(minY, 50.0, 0.5);
    CHECK_NEAR(maxY, 350.0, 0.5);
    CHECK(ui.textWidth("Hello", 72.f) > ui.textWidth("Hello", 36.f));
    ui.end();
    ImGui::EndFrame();
}

TEST(ui_buttons_click_only_inside_with_a_free_mouse) {
    test::ImGuiContextGuard imgui;
    GameUI ui;
    auto clickAt = [&](float x, float y, bool mouse) {
        click(x, y);
        ImGui::NewFrame();
        ui.begin(ImGui::GetBackgroundDrawList(), 100.f, 50.f, 800.f, 600.f, mouse);
        bool hit = ui.button(0.f, 0.f, 540.f, 540.f, "OK");   // the view's top-left quarter (300 x 300 px)
        ui.end();
        ImGui::EndFrame();
        release();
        ImGui::NewFrame();
        ImGui::EndFrame();
        return hit;
    };
    CHECK(clickAt(250.f, 200.f, true));
    CHECK(!clickAt(500.f, 500.f, true));    // outside
    CHECK(!clickAt(250.f, 200.f, false));   // the pointer isn't the game's (captured or over a panel)
}

TEST(ui_state_format) {
    GameState st;
    st.setNumber("score", 12.f);
    st.setNumber("speed", 2.5f);
    st.setText("name", "Ada");
    st.setFlag("door", true);
    CHECK(st.format("Score: {score}") == "Score: 12");
    CHECK(st.format("{name} at {speed} m/s, door open: {door}") == "Ada at 2.5 m/s, door open: yes");
    CHECK(st.format("{missing}|{unclosed") == "|{unclosed");
}

// P opens the pause menu (the game stops), a click on Resume closes it; presses made while paused don't leak
TEST(ui_pause_menu) {
    test::ImGuiContextGuard imgui;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigInputTrickleEventQueue = false;   // key, move and click events all apply in the next frame
    Scene s = buildDefaultScene();
    s.create("Menu").behaviours.emplace_back(createBehaviour("PauseMenu"));
    Game g;
    GameUI ui;
    g.start(s, meshes());
    frame(g, ui);
    io.AddKeyEvent(ImGuiKey_P, true);
    frame(g, ui);
    io.AddKeyEvent(ImGuiKey_P, false);
    CHECK(g.paused());
    CHECK(ui.wantsCursor());
    float t = g.time();
    io.AddKeyEvent(ImGuiKey_Space, true);   // jump while paused: must not fire on resume
    frame(g, ui);
    io.AddKeyEvent(ImGuiKey_Space, false);
    frame(g, ui);
    CHECK_NEAR(g.time(), t, 0.0);           // nothing ran
    // Resume: the first button, centred, at canvas y 330..402 -> window pixels (scale 600/1080, view at y 50)
    float scale = 600.f / 1080.f;
    click(100.f + 400.f, 50.f + 360.f * scale);
    frame(g, ui);
    release();
    CHECK(!g.paused());
    CHECK(!g.input().pressed("jump"));
    frame(g, ui);
    CHECK(!ui.wantsCursor());
    CHECK(g.time() > t);
    // Quit: the fourth button
    io.AddKeyEvent(ImGuiKey_P, true);
    frame(g, ui);
    io.AddKeyEvent(ImGuiKey_P, false);
    click(100.f + 400.f, 50.f + (330.f + 3 * 94.f + 36.f) * scale);
    frame(g, ui);
    release();
    CHECK(g.quitRequested());
}

TEST(ui_hidden_entities_draw_nothing) {
    test::ImGuiContextGuard imgui;
    Scene s;
    Entity& hud = s.create("HUD");
    hud.behaviours.emplace_back(createBehaviour("HudText"));
    Game g;
    GameUI ui;
    g.start(s, meshes());
    g.state().setNumber("score", 3.f);
    auto vertices = [&] {
        ImGui::NewFrame();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ui.begin(dl, 0.f, 0.f, 800.f, 600.f, true);
        g.drawUI(ui);
        ui.end();
        int n = dl->VtxBuffer.Size;
        ImGui::EndFrame();
        return n;
    };
    CHECK(vertices() > 0);
    s.findByName("HUD")->visible = false;
    CHECK(vertices() == 0);
}
