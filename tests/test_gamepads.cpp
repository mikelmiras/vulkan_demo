// Gamepads one by one: binding strings, pad kinds and Linux's codes, button prompts (glyphs), local players each
// on their own pad, prompts following the device used last, and rebinding saved in the player's settings
#include "test.h"

#include "game/game.h"
#include "game/prompts.h"
#include "game/ui.h"
#include "imgui.h"
#include "scene/input_bindings.h"

namespace {

const MeshLibrary& meshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}

void frame(Game& g) {
    ImGui::GetIO().DeltaTime = 1.f / 60.f;
    ImGui::NewFrame();
    g.frame(1.f / 60.f, true, true);
    ImGui::EndFrame();
}

// Notes in the game state whether its input says "jump" is down, and rumbles when it's pressed
struct JumpWatch : BehaviourBase<JumpWatch> {
    static constexpr const char* kTypeName = "TestJumpWatch";
    void update(GameContext& ctx, Entity& self) override {
        ctx.state().set(self.name + ".jump", json::Value(ctx.input().down("jump")));
        if (ctx.input().pressed("jump")) ctx.rumble(0.5f, 0.5f, 0.2f);
    }
};

GamepadState pad(PadType type, ImGuiKey held = ImGuiKey_None) {
    GamepadState p;
    p.connected = true;
    p.type = type;
    if (held != ImGuiKey_None) p.set(held, 1.f);
    return p;
}

}  // namespace

TEST(input_binding_strings_and_pad_kinds) {
    InputAction a{"fire", {ImGuiKey_F, ImGuiKey_GamepadR2}, ImGuiMouseButton_Left};
    CHECK(bindingString(a) == "F,GamepadR2,Mouse0");
    InputAction b{"fire"};
    REQUIRE(parseBinding("Space, GamepadFaceDown ,Mouse1", b));
    CHECK(b.keys.size() == 2 && b.keys[0] == ImGuiKey_Space && b.keys[1] == ImGuiKey_GamepadFaceDown && b.mouseButton == 1);
    CHECK(!parseBinding("Nothing,AtAll", b));
    CHECK(b.keys.size() == 2);   // (unchanged)
    CHECK(padTypeOf(0x054c, "") == PadType::PlayStation);
    CHECK(padTypeOf(0x057e, "") == PadType::Switch);
    CHECK(padTypeOf(0x045e, "") == PadType::Xbox);
    CHECK(padTypeOf(0, "Sony Interactive Entertainment Wireless Controller") == PadType::PlayStation);
    CHECK(padTypeOf(0, "DualSense Wireless Controller") == PadType::PlayStation);
    CHECK(padTypeOf(0, "Nintendo Switch Pro Controller") == PadType::Switch);
    CHECK(padTypeOf(0, "Microsoft X-Box 360 pad") == PadType::Xbox);
    CHECK(padTypeOf(0x1234, "Some Pad") == PadType::Generic);
    // Linux codes: xpad's BTN_X / BTN_Y are its left and top buttons, Sony's and Nintendo's top and left
    CHECK(linuxPadButton(0x130, PadType::Xbox) == ImGuiKey_GamepadFaceDown);
    CHECK(linuxPadButton(0x133, PadType::Xbox) == ImGuiKey_GamepadFaceLeft);
    CHECK(linuxPadButton(0x134, PadType::Xbox) == ImGuiKey_GamepadFaceUp);
    CHECK(linuxPadButton(0x133, PadType::PlayStation) == ImGuiKey_GamepadFaceUp);
    CHECK(linuxPadButton(0x134, PadType::Switch) == ImGuiKey_GamepadFaceLeft);
    CHECK(linuxPadButton(0x13b, PadType::Generic) == ImGuiKey_GamepadStart);
    CHECK(linuxPadButton(0x13c, PadType::Xbox) == ImGuiKey_None);   // (the guide button)
    ImGuiKey n, p, t;
    REQUIRE(linuxPadAxis(0x01, n, p, t));
    CHECK(n == ImGuiKey_GamepadLStickUp && p == ImGuiKey_GamepadLStickDown && t == ImGuiKey_None);
    REQUIRE(linuxPadAxis(0x05, n, p, t));
    CHECK(t == ImGuiKey_GamepadR2);
    REQUIRE(linuxPadAxis(0x10, n, p, t));
    CHECK(n == ImGuiKey_GamepadDpadLeft);
    CHECK(!linuxPadAxis(0x28, n, p, t));
}

TEST(button_prompt_glyphs) {
    test::ImGuiContextGuard imgui;
    std::string cross = prompts::padGlyph(PadType::PlayStation, ImGuiKey_GamepadFaceDown);
    CHECK(cross.size() == 3);
    std::vector<prompts::Piece> pieces = prompts::split("Press " + cross + " or " + prompts::keyCap("Space") + "!");
    REQUIRE(pieces.size() == 5);
    CHECK(pieces[0].kind == prompts::Piece::Text && pieces[0].text == "Press ");
    CHECK(pieces[1].kind == prompts::Piece::Pad && pieces[1].type == PadType::PlayStation && pieces[1].key == ImGuiKey_GamepadFaceDown);
    CHECK(pieces[3].kind == prompts::Piece::Key && pieces[3].text == "Space");
    CHECK(pieces[4].text == "!");
    CHECK(!prompts::hasGlyphs("plain text, ñ and € too"));
    CHECK(prompts::split("ñ €").size() == 1);
    // Labels per kind of pad
    CHECK(prompts::padLabel(PadType::PlayStation, ImGuiKey_GamepadFaceDown) == "Cross");
    CHECK(prompts::padLabel(PadType::Xbox, ImGuiKey_GamepadFaceDown) == "A");
    CHECK(prompts::padLabel(PadType::Switch, ImGuiKey_GamepadFaceDown) == "B");   // (Nintendo's A is on the right)
    CHECK(prompts::padLabel(PadType::Switch, ImGuiKey_GamepadFaceRight) == "A");
    CHECK(prompts::padLabel(PadType::Xbox, ImGuiKey_GamepadL2) == "LT");
    CHECK(prompts::padLabel(PadType::PlayStation, ImGuiKey_GamepadR1) == "R1");
    CHECK(prompts::padLabel(PadType::Switch, ImGuiKey_GamepadL2) == "ZL");
    CHECK(prompts::padLabel(PadType::PlayStation, ImGuiKey_GamepadStart) == "Options");
    CHECK(prompts::padLabel(PadType::Xbox, ImGuiKey_GamepadLStickUp) == "Left stick up");
    CHECK(prompts::keyLabel(ImGuiKey_LeftShift) == "Left Shift");
    CHECK(prompts::keyLabel(ImGuiKey_UpArrow) == "Up");
    CHECK(prompts::keyLabel(ImGuiKey_Escape) == "Esc");
    InputAction jump{"jump", {ImGuiKey_Space, ImGuiKey_GamepadFaceDown}};
    CHECK(prompts::forAction(jump, prompts::Device::Keyboard, PadType::Xbox) == prompts::keyCap("Space"));
    CHECK(prompts::forAction(jump, prompts::Device::Pad, PadType::PlayStation) == cross);
    CHECK(prompts::wordsForAction(jump, prompts::Device::Pad, PadType::PlayStation) == "Cross");
    InputAction fire{"fire", {ImGuiKey_GamepadR2}, ImGuiMouseButton_Left};
    CHECK(prompts::wordsForAction(fire, prompts::Device::Keyboard, PadType::Xbox) == "Left click");
    CHECK(prompts::forAction(InputAction{"none"}, prompts::Device::Pad, PadType::Xbox).empty());
    // Drawn inside text: they take room (and drawing them doesn't trip over anything)
    GameUI ui;
    ImGui::NewFrame();
    ui.begin(ImGui::GetBackgroundDrawList(), 0.f, 0.f, 1920.f, 1080.f, false);
    float plain = ui.textWidth("Jump"), with = ui.textWidth("Jump " + cross);
    CHECK(with > plain + 20.f);
    CHECK(ui.textWidth(prompts::keyCap("Space")) > ui.textWidth("Space") * 0.6f);
    for (PadType type : {PadType::Generic, PadType::Xbox, PadType::PlayStation, PadType::Switch})
        for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; ++k)
            ui.text(10.f, 10.f, "x " + prompts::padGlyph(type, (ImGuiKey)k) + " y", 36.f);
    ui.end();
    ImGui::EndFrame();
}

// Two player cameras: player 1 plays with the first pad (and the keyboard), player 2 with the second; each one's
// behaviours read their own; rumble goes to the asking player's pad
TEST(local_players_on_their_own_pads) {
    test::ImGuiContextGuard imgui;
    Scene s;
    for (int n = 1; n <= 2; ++n) {
        Entity& root = s.create("P" + std::to_string(n));
        root.behaviours.emplace_back(std::make_unique<JumpWatch>());
        Entity& cam = s.create("Cam" + std::to_string(n), root.id);
        cam.camera = Camera{};
        cam.camera->player = n;
    }
    Game g;
    std::vector<Game::Rumble> rumbles;
    g.setGamepadOutput({[&](int padSlot, float low, float high, float seconds) { rumbles.push_back({low, high, seconds, padSlot}); }, nullptr});
    g.start(s, meshes());
    // (slot 0 empty: the pads connected, in order, are players 1 and 2)
    g.setGamepads({GamepadState{}, pad(PadType::Xbox), pad(PadType::PlayStation, ImGuiKey_GamepadFaceDown)});
    for (int i = 0; i < 3; ++i) frame(g);
    CHECK(g.playerPad(0) == 1 && g.playerPad(1) == 2);
    CHECK(!g.state().flag("P1.jump"));
    CHECK(g.state().flag("P2.jump"));
    REQUIRE(!rumbles.empty());
    CHECK(rumbles.back().pad == 2);
    CHECK(g.lastRumble().pad == 2);
    g.setGamepads({GamepadState{}, pad(PadType::Xbox, ImGuiKey_GamepadFaceDown), pad(PadType::PlayStation)});
    for (int i = 0; i < 3; ++i) frame(g);
    CHECK(g.state().flag("P1.jump"));
    CHECK(!g.state().flag("P2.jump"));
    CHECK(rumbles.back().pad == 1);
    // Player 1 has the keyboard too, player 2 doesn't
    g.setGamepads({pad(PadType::Xbox), pad(PadType::Xbox)});
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, true);
    for (int i = 0; i < 3; ++i) frame(g);
    CHECK(g.state().flag("P1.jump"));
    CHECK(!g.state().flag("P2.jump"));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, false);
    frame(g);
    g.stop();
    // One player (no player cameras): any pad, through Dear ImGui, as before
    Scene one;
    one.create("Solo").behaviours.emplace_back(std::make_unique<JumpWatch>());
    Game g1;
    g1.start(one, meshes());
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;   // (else Dear ImGui drops pad input)
    ImGui::GetIO().AddKeyAnalogEvent(ImGuiKey_GamepadFaceDown, true, 1.f);
    for (int i = 0; i < 3; ++i) frame(g1);
    CHECK(g1.state().flag("Solo.jump"));
    ImGui::GetIO().AddKeyAnalogEvent(ImGuiKey_GamepadFaceDown, false, 0.f);
    frame(g1);
    g1.stop();
}

// "{button:jump}": the glyph of the device used last (keys, or that kind of pad); forced with :pad / :keyboard
TEST(prompts_follow_the_device) {
    test::ImGuiContextGuard imgui;
    Scene s;
    s.create("Anything");
    Game g;
    g.start(s, meshes());
    frame(g);
    CHECK(g.context().format("Jump: {button:jump}") == "Jump: " + prompts::keyCap("Space"));
    g.setGamepads({pad(PadType::PlayStation, ImGuiKey_GamepadFaceDown)});
    frame(g);
    CHECK(g.lastDevice() == prompts::Device::Pad && g.lastPadType() == PadType::PlayStation);
    CHECK(g.context().format("{button:jump}") == prompts::padGlyph(PadType::PlayStation, ImGuiKey_GamepadFaceDown));
    CHECK(g.context().format("{button:jump:keyboard}") == prompts::keyCap("Space"));
    CHECK(g.context().prompt("fire") == prompts::padGlyph(PadType::PlayStation, ImGuiKey_GamepadR2));
    g.setGamepads({pad(PadType::PlayStation)});
    ImGui::GetIO().AddKeyEvent(ImGuiKey_W, true);
    frame(g);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_W, false);
    frame(g);
    CHECK(g.lastDevice() == prompts::Device::Keyboard);
    CHECK(g.context().format("{button:fire}") == prompts::keyCap("Left click"));
    CHECK(g.context().format("{button:jump:pad}") == prompts::padGlyph(PadType::PlayStation, ImGuiKey_GamepadFaceDown));
    CHECK(g.context().format("{button:nothing}").empty());
    g.stop();
}

// Rebinding: the next press on that device (after letting go) becomes the binding there, the other device's stay;
// saved in the player's settings and back in the next game; Esc cancels; reset brings the scene's back
TEST(rebinding_controls) {
    test::ImGuiContextGuard imgui;
    Scene s;
    s.create("Watcher").behaviours.emplace_back(std::make_unique<JumpWatch>());
    {
        Game g;
        g.start(s, meshes());
        g.resetBindings();   // (from an earlier run)
        frame(g);
        g.rebind("jump", prompts::Device::Keyboard);
        CHECK(g.rebinding());
        CHECK(g.state().text("input.rebinding") == "jump");
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, true);   // (still held from the press that asked: not this one)
        frame(g);
        CHECK(g.rebinding());
        CHECK(!g.state().flag("Watcher.jump"));   // (the game doesn't see presses while it waits)
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, false);
        frame(g);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_K, true);
        frame(g);
        CHECK(!g.rebinding());
        CHECK(g.state().text("settings.bind.jump") == "K,GamepadFaceDown");
        frame(g);
        CHECK(g.state().flag("Watcher.jump"));   // K jumps now
        ImGui::GetIO().AddKeyEvent(ImGuiKey_K, false);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, true);
        frame(g);
        frame(g);
        CHECK(!g.state().flag("Watcher.jump"));  // ... Space doesn't
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, false);
        frame(g);
        // A pad's: R1 instead of A, the keyboard's K kept
        g.rebind("jump", prompts::Device::Pad);
        frame(g);
        g.setGamepads({pad(PadType::Xbox, ImGuiKey_GamepadR1)});
        frame(g);
        CHECK(g.state().text("settings.bind.jump") == "K,GamepadR1");
        CHECK(g.prompt("jump", prompts::Device::Pad) == prompts::padGlyph(PadType::Xbox, ImGuiKey_GamepadR1));
        g.setGamepads({pad(PadType::Xbox)});
        frame(g);
        // Esc: nothing changes
        g.rebind("jump", prompts::Device::Keyboard);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        frame(g);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        frame(g);
        CHECK(!g.rebinding());
        CHECK(g.state().text("settings.bind.jump") == "K,GamepadR1");
        g.stop();   // (saves the settings)
    }
    {
        Game again;   // the player's bindings came back
        again.start(s, meshes());
        frame(again);
        CHECK(again.state().text("settings.bind.jump") == "K,GamepadR1");
        CHECK(again.prompt("jump", prompts::Device::Keyboard) == prompts::keyCap("K"));
        again.resetBindings("jump");
        CHECK(again.prompt("jump", prompts::Device::Keyboard) == prompts::keyCap("Space"));
        CHECK(again.state().text("settings.bind.jump").empty());
        again.stop();
    }
}

// A player's own values (settings, bindings, input.*, net.*) stay out of save games and multiplayer snapshots, and
// loading one keeps them
TEST(game_state_keeps_local_values) {
    GameState st;
    st.set("score", json::Value(5.0));
    st.set("settings.bind.jump", json::Value("K"));
    st.set("input.rebinding", json::Value("jump"));
    st.set("net.status", json::Value("Offline"));
    json::Value saved = st.toJson();
    CHECK(saved.has("score") && !saved.has("settings.bind.jump") && !saved.has("input.rebinding") && !saved.has("net.status"));
    json::Value other = json::Value::object();
    other.set("score", json::Value(9.0));
    other.set("settings.bind.jump", json::Value("L"));   // (someone else's: ignored)
    st.fromJson(other);
    CHECK(st.number("score") == 9.f);
    CHECK(st.text("settings.bind.jump") == "K");
    CHECK(st.text("input.rebinding") == "jump");
}
