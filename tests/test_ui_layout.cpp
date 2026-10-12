// game/ui_layout: UI elements placed by anchors and offsets, lists, buttons, sliders, toggles, text fields, focus
// moved by keys, and what buttons do
#include "test.h"

#include "game/game.h"
#include "game/ui_layout.h"
#include "imgui.h"
#include "scene/serialization.h"

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

// A 1920 x 1080 view: canvas units are window pixels
void frame(Game& g, GameUI& ui) {
    ImGui::GetIO().DeltaTime = 1.f / 60.f;
    ImGui::NewFrame();
    g.frame(1.f / 60.f, true, true);
    ui.begin(ImGui::GetBackgroundDrawList(), 0.f, 0.f, 1920.f, 1080.f, true);
    g.drawUI(ui);
    ui.end();
    ImGui::EndFrame();
}

void tap(Game& g, GameUI& ui, ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true);
    frame(g, ui);
    ImGui::GetIO().AddKeyEvent(key, false);
    frame(g, ui);
}

Entity& element(Scene& s, const char* name, UIElement::Kind kind, EntityId parent = kNoEntity) {
    Entity& e = s.create(name, parent);
    e.ui = UIElement{};
    e.ui->kind = kind;
    return e;
}

void place(UIElement& u, float ax0, float ay0, float ax1, float ay1, float ox0, float oy0, float ox1, float oy1) {
    u.anchorMin[0] = ax0, u.anchorMin[1] = ay0, u.anchorMax[0] = ax1, u.anchorMax[1] = ay1;
    u.offsetMin[0] = ox0, u.offsetMin[1] = oy0, u.offsetMax[0] = ox1, u.offsetMax[1] = oy1;
}

struct Inbox : BehaviourBase<Inbox> {
    static constexpr const char* kTypeName = "TestUiInbox";
    std::vector<Message> got;
    void onMessage(GameContext&, Entity&, const Message& m) override { got.push_back(m); }
};

}  // namespace

TEST(ui_layout_anchors_nesting_lists) {
    Scene s;
    Entity& canvas = element(s, "Canvas", UIElement::Kind::Panel);
    place(*canvas.ui, 0, 0, 1, 1, 0, 0, 0, 0);   // the whole screen
    EntityId c = canvas.id;
    Entity& corner = element(s, "Corner", UIElement::Kind::Text, c);
    place(*corner.ui, 1, 0, 1, 0, -200, 20, -20, 70);   // 180 x 50, 20 in from the top right
    Entity& list = element(s, "List", UIElement::Kind::List, c);
    place(*list.ui, 0.5f, 0.5f, 0.5f, 0.5f, -100, -100, 100, 100);
    list.ui->spacing = 10.f;
    EntityId l = list.id;
    for (const char* n : {"A", "B", "C"}) {
        Entity& item = element(s, n, UIElement::Kind::Button, l);
        place(*item.ui, 0, 0, 0, 0, 0, 0, 0, 40);   // (in a list: only the height counts)
    }
    s.findByName("B")->visible = false;   // hidden: no room kept for it
    UILayout layout = layoutUI(s, 1920.f);
    UIRect r = layout.rects[s.findByName("Corner")->id];
    CHECK_NEAR(r.x, 1720.f, 1e-3);
    CHECK_NEAR(r.y, 20.f, 1e-3);
    CHECK_NEAR(r.w, 180.f, 1e-3);
    CHECK_NEAR(r.h, 50.f, 1e-3);
    UIRect a = layout.rects[s.findByName("A")->id], cc = layout.rects[s.findByName("C")->id];
    CHECK_NEAR(a.x, 860.f, 1e-3);
    CHECK_NEAR(a.y, 440.f, 1e-3);
    CHECK_NEAR(a.w, 200.f, 1e-3);
    CHECK_NEAR(cc.y, 490.f, 1e-3);   // right under A (B is hidden)
    CHECK(!layout.rects.count(s.findByName("B")->id));
    CHECK(layout.order.front() == c);
    s.findByName("Canvas")->visible = false;
    CHECK(layoutUI(s, 1920.f).order.empty());
}

// Keys move the focus between buttons and press them; buttons send messages and run built-in actions; a slider,
// a toggle and a text field change the game state
TEST(ui_layout_menu_with_keys) {
    test::ImGuiContextGuard imgui;
    ImGui::GetIO().DisplaySize = ImVec2(1920, 1080);
    Scene s;
    Entity& listener = s.create("Listener");
    listener.behaviours.emplace_back(std::make_unique<Inbox>());
    Entity& menu = element(s, "Menu", UIElement::Kind::List);
    place(*menu.ui, 0.5f, 0.5f, 0.5f, 0.5f, -200, -200, 200, 200);
    EntityId m = menu.id;
    Entity& play = element(s, "Play", UIElement::Kind::Button, m);
    place(*play.ui, 0, 0, 0, 0, 0, 0, 0, 60);
    play.ui->action = "start";
    play.ui->target = "Listener";
    Entity& volume = element(s, "Volume", UIElement::Kind::Slider, m);
    place(*volume.ui, 0, 0, 0, 0, 0, 0, 0, 60);
    volume.ui->value = "volume";
    volume.ui->min = 0.f;
    volume.ui->max = 10.f;
    Entity& music = element(s, "Music", UIElement::Kind::Toggle, m);
    place(*music.ui, 0, 0, 0, 0, 0, 0, 0, 60);
    music.ui->value = "music";
    Entity& name = element(s, "Name", UIElement::Kind::TextField, m);
    place(*name.ui, 0, 0, 0, 0, 0, 0, 0, 60);
    name.ui->value = "playerName";
    Entity& close = element(s, "Close", UIElement::Kind::Button, m);
    place(*close.ui, 0, 0, 0, 0, 0, 0, 0, 60);
    close.ui->action = "@hide Menu";

    Game g;
    g.start(s, meshes());
    GameUI ui;
    frame(g, ui);
    CHECK(g.uiController().hasMenu());
    CHECK(g.uiController().focused() == kNoEntity);
    tap(g, ui, ImGuiKey_DownArrow);   // (wakes the menu: the first one)
    CHECK(g.uiController().focused() == s.findByName("Play")->id);
    tap(g, ui, ImGuiKey_Enter);
    REQUIRE(!s.findByName("Listener")->behaviour<Inbox>()->got.empty());
    CHECK(s.findByName("Listener")->behaviour<Inbox>()->got[0].name == "start");
    CHECK(s.findByName("Listener")->behaviour<Inbox>()->got[0].from == s.findByName("Play")->id);
    tap(g, ui, ImGuiKey_DownArrow);   // the slider
    CHECK(g.uiController().focused() == s.findByName("Volume")->id);
    for (int i = 0; i < 4; ++i) tap(g, ui, ImGuiKey_RightArrow);
    CHECK_NEAR(g.state().number("volume"), 2.f, 1e-4);   // 4 steps of 5 %
    tap(g, ui, ImGuiKey_LeftArrow);
    CHECK_NEAR(g.state().number("volume"), 1.5f, 1e-4);
    tap(g, ui, ImGuiKey_DownArrow);
    tap(g, ui, ImGuiKey_Space);   // the toggle
    CHECK(g.state().flag("music"));
    tap(g, ui, ImGuiKey_DownArrow);
    tap(g, ui, ImGuiKey_Enter);   // the text field: typing
    CHECK(g.uiController().editingText());
    for (char ch : std::string("Ana")) {
        ImGui::GetIO().AddInputCharacter((unsigned)ch);
        frame(g, ui);
    }
    tap(g, ui, ImGuiKey_Backspace);
    ImGui::GetIO().AddInputCharacter(0xF1);   // n with a tilde
    frame(g, ui);
    tap(g, ui, ImGuiKey_Enter);
    CHECK(!g.uiController().editingText());
    CHECK(g.state().text("playerName") == "An\xc3\xb1");
    tap(g, ui, ImGuiKey_DownArrow);
    tap(g, ui, ImGuiKey_UpArrow);
    tap(g, ui, ImGuiKey_DownArrow);
    CHECK(g.uiController().focused() == s.findByName("Close")->id);
    tap(g, ui, ImGuiKey_Enter);
    CHECK(!s.findByName("Menu")->visible);
    frame(g, ui);
    CHECK(!g.uiController().hasMenu());
    g.stop();
}

// Clicking presses buttons; dragging a slider sets it where the mouse is
TEST(ui_layout_mouse) {
    test::ImGuiContextGuard imgui;
    ImGui::GetIO().DisplaySize = ImVec2(1920, 1080);
    Scene s;
    Entity& b = element(s, "Pause", UIElement::Kind::Button);
    place(*b.ui, 0, 0, 0, 0, 100, 100, 300, 160);
    b.ui->action = "@pause";
    Entity& sl = element(s, "Slider", UIElement::Kind::Slider);
    place(*sl.ui, 0, 0, 0, 0, 100, 300, 500, 340);
    sl.ui->value = "v";
    sl.ui->max = 4.f;
    Game g;
    g.start(s, meshes());
    GameUI ui;
    frame(g, ui);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(200, 130);
    frame(g, ui);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(g, ui);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(g, ui);
    CHECK(g.paused());
    io.AddMousePosEvent(150, 320);
    frame(g, ui);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame(g, ui);
    io.AddMousePosEvent(400, 320);   // three quarters of the way
    frame(g, ui);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame(g, ui);
    CHECK_NEAR(g.state().number("v"), 3.f, 1e-3);
    g.stop();
}

TEST(ui_layout_saved) {
    Scene s;
    Entity& e = element(s, "Button", UIElement::Kind::Slider);
    place(*e.ui, 0.1f, 0.2f, 0.3f, 0.4f, -1, -2, 3, 4);
    e.ui->text = "{@menu.volume}";
    e.ui->value = "volume";
    e.ui->max = 5.f;
    e.ui->align = 0;
    MeshLibrary lib = buildMeshLibrary();
    json::Value v = entityToJson(*s.findByName("Button"), lib, textures());
    Entity back;
    std::string error;
    REQUIRE(entityFromJson(v, builtinAssetIds(lib, textures()), back, error));
    REQUIRE(back.ui);
    CHECK(back.ui->kind == UIElement::Kind::Slider);
    CHECK(back.ui->anchorMax[1] == 0.4f && back.ui->offsetMin[1] == -2.f);
    CHECK(back.ui->text == "{@menu.volume}" && back.ui->value == "volume" && back.ui->max == 5.f && back.ui->align == 0);
}

// A text field opened with the gamepad gets the on-screen keyboard: keys picked with the d-pad, typed with A
TEST(ui_layout_on_screen_keyboard) {
    test::ImGuiContextGuard imgui;
    ImGui::GetIO().DisplaySize = ImVec2(1920, 1080);
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;   // (else ImGui drops the pad's keys)
    Scene s;
    Entity& name = element(s, "Name", UIElement::Kind::TextField);
    place(*name.ui, 0.5f, 0.2f, 0.5f, 0.2f, -200, 0, 200, 60);
    name.ui->value = "playerName";
    Game g;
    g.start(s, meshes());
    GameUI ui;
    frame(g, ui);
    tap(g, ui, ImGuiKey_GamepadDpadDown);   // (wakes the menu)
    tap(g, ui, ImGuiKey_GamepadFaceDown);   // opens the field, with the keyboard (the q row, its first key)
    REQUIRE(g.uiController().keyboardShown());
    tap(g, ui, ImGuiKey_GamepadFaceUp);     // Shift: a capital
    tap(g, ui, ImGuiKey_GamepadDpadRight);
    tap(g, ui, ImGuiKey_GamepadDpadRight);
    tap(g, ui, ImGuiKey_GamepadFaceDown);   // E
    tap(g, ui, ImGuiKey_GamepadDpadDown);   // the a row, d
    tap(g, ui, ImGuiKey_GamepadFaceDown);   // d (Shift was for one)
    tap(g, ui, ImGuiKey_GamepadFaceLeft);   // a space
    tap(g, ui, ImGuiKey_GamepadDpadUp);
    tap(g, ui, ImGuiKey_GamepadDpadUp);     // the digits, 3
    tap(g, ui, ImGuiKey_GamepadFaceDown);
    for (int i = 0; i < 3; ++i) tap(g, ui, ImGuiKey_GamepadDpadLeft);   // round the edge: 0
    tap(g, ui, ImGuiKey_GamepadFaceDown);
    CHECK(g.state().text("playerName") == "Ed 30");
    tap(g, ui, ImGuiKey_GamepadFaceRight);  // deletes it
    CHECK(g.state().text("playerName") == "Ed 3");
    // The bottom row: Done finishes
    tap(g, ui, ImGuiKey_GamepadDpadUp);     // (round the top: the bottom row, under the 0: Done)
    tap(g, ui, ImGuiKey_GamepadDpadLeft);   // Delete (a wide key: one step)
    tap(g, ui, ImGuiKey_GamepadDpadRight);  // Done
    tap(g, ui, ImGuiKey_GamepadFaceDown);
    CHECK(!g.uiController().editingText());
    CHECK(!g.uiController().keyboardShown());
    CHECK(g.state().text("playerName") == "Ed 3");
    // Opened with the keyboard (Enter): no on-screen one
    tap(g, ui, ImGuiKey_Enter);
    CHECK(g.uiController().editingText());
    CHECK(!g.uiController().keyboardShown());
    g.stop();
}

// Accessibility settings from a menu: "@cycle" steps settings.colorBlind through its modes (the render filter
// follows), "@set" turns on high contrast (the UI's drawn that way)
TEST(ui_layout_accessibility_settings) {
    test::ImGuiContextGuard imgui;
    ImGui::GetIO().DisplaySize = ImVec2(1920, 1080);
    Scene s;
    Entity& menu = element(s, "Menu", UIElement::Kind::List);
    place(*menu.ui, 0.5f, 0.5f, 0.5f, 0.5f, -200, -100, 200, 100);
    EntityId m = menu.id;
    Entity& colors = element(s, "Colours", UIElement::Kind::Button, m);
    place(*colors.ui, 0, 0, 0, 0, 0, 0, 0, 60);
    colors.ui->text = "Colours: {settings.colorBlind}";
    colors.ui->action = "@cycle settings.colorBlind none protanopia deuteranopia tritanopia";
    Entity& contrast = element(s, "Contrast", UIElement::Kind::Button, m);
    place(*contrast.ui, 0, 0, 0, 0, 0, 0, 0, 60);
    contrast.ui->action = "@set settings.highContrast true; @set difficulty 2";
    Game g;
    g.start(s, meshes());
    GameUI ui;
    frame(g, ui);
    CHECK(g.state().text("settings.colorBlind") == "none");
    CHECK(g.colorFilter() == 0);
    tap(g, ui, ImGuiKey_DownArrow);   // (wakes the menu: Colours)
    tap(g, ui, ImGuiKey_Enter);
    tap(g, ui, ImGuiKey_Enter);
    CHECK(g.state().text("settings.colorBlind") == "deuteranopia");
    CHECK(g.colorFilter() == 2);
    tap(g, ui, ImGuiKey_Enter);
    tap(g, ui, ImGuiKey_Enter);   // (round to the first)
    CHECK(g.state().text("settings.colorBlind") == "none");
    tap(g, ui, ImGuiKey_Enter);
    tap(g, ui, ImGuiKey_DownArrow);
    tap(g, ui, ImGuiKey_Enter);
    CHECK(g.state().flag("settings.highContrast"));
    CHECK(g.state().number("difficulty") == 2.f);
    frame(g, ui);   // (drawn in high contrast)
    g.stop();
    // Saved with the player's settings, and read back
    Game again;
    again.start(s, meshes());
    CHECK(again.state().text("settings.colorBlind") == "protanopia");
    CHECK(again.colorFilter() == 1);
    CHECK(again.state().flag("settings.highContrast"));
    again.state().set("settings.colorBlind", json::Value("none"));   // (the next run starts as before)
    again.state().set("settings.highContrast", json::Value(false));
    frame(again, ui);
    again.stop();
}
