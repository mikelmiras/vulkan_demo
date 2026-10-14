// Gamepads one by one: binding strings, pad kinds and Linux's codes, button prompts (glyphs), local players each
// on their own pad (or joining with Start), prompts following the device used last, rebinding saved in the
// player's settings (a stick as a whole too), binding conflicts, and HID pads' reports (Windows)
#include "test.h"

#include "game/game.h"
#include "game/prompts.h"
#include "game/ui.h"
#include "imgui.h"
#include "platform/hid_pads.h"
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

// Joining with Start: pads join as they press it (the first plays with player 1), an unplugged pad's seat waits
// for the next press, and the game state says who plays with what
TEST(players_join_with_start) {
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
    g.start(s, meshes());
    g.setGamepads({pad(PadType::Xbox, ImGuiKey_GamepadStart), pad(PadType::PlayStation)});
    g.setPadJoining(true);   // (a Start held already doesn't join)
    for (int i = 0; i < 2; ++i) frame(g);
    CHECK(g.playerPad(0) == -1 && g.playerPad(1) == -1);
    CHECK(g.state().number("input.players") == 1.f && g.localPlayers() == 1);
    g.setGamepads({pad(PadType::Xbox), pad(PadType::PlayStation, ImGuiKey_GamepadStart)});   // the second pad first
    frame(g);
    CHECK(g.playerPad(0) == 1 && g.playerPad(1) == -1);
    CHECK(g.state().text("input.player.1.pad") == "playstation");
    g.setGamepads({pad(PadType::Xbox, ImGuiKey_GamepadStart), pad(PadType::PlayStation)});
    frame(g);
    CHECK(g.playerPad(1) == 0 && g.localPlayers() == 2);
    CHECK(g.state().text("input.player.2.pad") == "xbox");
    g.setGamepads({pad(PadType::Xbox, ImGuiKey_GamepadFaceDown), pad(PadType::PlayStation)});
    for (int i = 0; i < 3; ++i) frame(g);
    CHECK(g.state().flag("P2.jump") && !g.state().flag("P1.jump"));
    // Unplugged: the seat waits (still two players), a new pad's Start takes it
    g.setGamepads({GamepadState{}, pad(PadType::PlayStation)});
    frame(g);
    CHECK(g.playerPad(1) == -1 && g.localPlayers() == 2);
    CHECK(g.state().text("input.player.2.pad").empty());
    g.setGamepads({GamepadState{}, pad(PadType::PlayStation), pad(PadType::Switch, ImGuiKey_GamepadStart)});
    frame(g);
    CHECK(g.playerPad(1) == 2 && g.playerPad(0) == 1);
    CHECK(g.state().text("input.player.2.pad") == "switch");
    g.resetPlayers();
    frame(g);
    CHECK(g.localPlayers() == 1 && g.playerPad(0) == -1);
    g.setPadJoining(false);
    frame(g);
    CHECK(!g.padJoining() && g.playerPad(0) == 1 && g.playerPad(1) == 2);   // (pads in order again)
    g.stop();
}

// A stick rebound as a whole: the four actions get the stick pushed (other presses ignored), the other device's
// bindings kept; the keyboard takes a cluster of keys the same way; the clash with the look actions shows
TEST(rebinding_a_stick_whole) {
    test::ImGuiContextGuard imgui;
    Scene s;
    s.create("Anything");
    Game g;
    g.start(s, meshes());
    g.resetBindings();
    frame(g);
    CHECK(g.state().number("input.conflicts") == 0.f);
    CHECK(g.state().text("input.conflict.move_left").empty());
    g.rebindStick({"move_forward", "move_back", "nothing", "move_right"}, prompts::Device::Pad);   // (no such action)
    CHECK(!g.rebinding());
    g.rebindStick({"move_forward", "move_back", "move_left", "move_right"}, prompts::Device::Pad);
    CHECK(g.state().text("input.rebinding") == "move_forward,move_back,move_left,move_right");
    frame(g);
    g.setGamepads({pad(PadType::Xbox, ImGuiKey_GamepadFaceDown)});   // (a button: not a stick)
    frame(g);
    CHECK(g.rebinding());
    g.setGamepads({pad(PadType::Xbox)});
    frame(g);
    g.setGamepads({pad(PadType::Xbox, ImGuiKey_GamepadRStickLeft)});
    frame(g);
    CHECK(!g.rebinding());
    CHECK(g.state().text("settings.bind.move_forward") == "GamepadRStickUp,W,UpArrow");
    CHECK(g.state().text("settings.bind.move_left") == "GamepadRStickLeft,A,LeftArrow");
    CHECK(g.state().text("settings.bind.move_right") == "GamepadRStickRight,D,RightArrow");
    CHECK(g.state().text("input.conflict.move_left") == "look_left");   // (the right stick looks around too)
    CHECK(g.state().text("input.conflict.look_up") == "move_forward");
    CHECK(g.state().number("input.conflicts") == 4.f);
    g.setGamepads({pad(PadType::Xbox)});
    frame(g);
    // The keyboard: a key of a cluster (J of IJKL) is the whole cluster; the pad's stick stays
    g.rebindStick({"move_forward", "move_back", "move_left", "move_right"}, prompts::Device::Keyboard);
    frame(g);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_J, true);
    frame(g);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_J, false);
    frame(g);
    CHECK(g.state().text("settings.bind.move_forward") == "I,GamepadRStickUp");
    CHECK(g.state().text("settings.bind.move_back") == "K,GamepadRStickDown");
    g.resetBindings();
    frame(g);
    CHECK(g.state().number("input.conflicts") == 0.f);
    g.stop();
}

TEST(binding_conflicts) {
    CHECK(bindingConflicts(defaultInputActions()).empty());
    std::vector<InputAction> actions = {{"jump", {ImGuiKey_Space, ImGuiKey_GamepadFaceDown}},
                                        {"use", {ImGuiKey_E, ImGuiKey_GamepadFaceDown}, ImGuiMouseButton_Left},
                                        {"fire", {}, ImGuiMouseButton_Left}};
    std::vector<BindingConflict> c = bindingConflicts(actions);
    REQUIRE(c.size() == 2);
    CHECK(c[0].first == "jump" && c[0].second == "use" && c[0].control == "GamepadFaceDown");
    CHECK(c[1].first == "use" && c[1].second == "fire" && c[1].control == "Mouse0");
    ImGuiKey g[4];
    REQUIRE(directionGroup(ImGuiKey_GamepadDpadLeft, g));
    CHECK(g[0] == ImGuiKey_GamepadDpadUp && g[3] == ImGuiKey_GamepadDpadRight);
    CHECK(!directionGroup(ImGuiKey_Space, g));
}

// Sony's and Nintendo's reports (as the pads send them over USB and Bluetooth) into pad states
TEST(hid_pad_reports) {
    using hidpad::Model;
    CHECK(hidpad::modelOf(0x054C, 0x0CE6) == Model::DualSense);
    CHECK(hidpad::modelOf(0x054C, 0x09CC) == Model::DualShock4);
    CHECK(hidpad::modelOf(0x057E, 0x2009) == Model::SwitchPro);
    CHECK(hidpad::modelOf(0x045E, 0x028E) == Model::None);
    // DualShock 4 over USB: left stick right and up, cross, the hat up, options, L2 pulled
    uint8_t ds4[64] = {0x01, 255, 0, 128, 128, 0x20 | 0, 0x20, 0, 255, 0};
    GamepadState s;
    REQUIRE(hidpad::parseReport(Model::DualShock4, ds4, sizeof(ds4), s));
    CHECK(s.connected && s.type == PadType::PlayStation && s.name == "DualShock 4");
    CHECK(s.value(ImGuiKey_GamepadLStickRight) == 1.f && s.value(ImGuiKey_GamepadLStickUp) == 1.f);
    CHECK(s.value(ImGuiKey_GamepadLStickLeft) == 0.f && s.value(ImGuiKey_GamepadRStickUp) == 0.f);   // (centred: none)
    CHECK(s.value(ImGuiKey_GamepadFaceDown) == 1.f && s.value(ImGuiKey_GamepadDpadUp) == 1.f);
    CHECK(s.value(ImGuiKey_GamepadDpadLeft) == 0.f && s.value(ImGuiKey_GamepadStart) == 1.f);
    CHECK(s.value(ImGuiKey_GamepadL2) == 1.f && s.value(ImGuiKey_GamepadR2) == 0.f);
    // ... over Bluetooth (0x11: two bytes on), the hat centred (8), square and R1
    uint8_t ds4bt[78] = {0x11, 0xC0, 0x00, 128, 128, 128, 128, 0x10 | 8, 0x02, 0, 0, 0};
    GamepadState b;
    REQUIRE(hidpad::parseReport(Model::DualShock4, ds4bt, sizeof(ds4bt), b));
    CHECK(b.value(ImGuiKey_GamepadFaceLeft) == 1.f && b.value(ImGuiKey_GamepadR1) == 1.f);
    CHECK(b.value(ImGuiKey_GamepadDpadUp) == 0.f && b.value(ImGuiKey_GamepadDpadDown) == 0.f);
    // DualSense over USB: triggers before the buttons; circle with the hat right, L1, R2 half way
    uint8_t ds[64] = {0x01, 128, 128, 0, 128, 0, 128, 7, 0x40 | 2, 0x01};
    GamepadState d;
    REQUIRE(hidpad::parseReport(Model::DualSense, ds, sizeof(ds), d));
    CHECK(d.value(ImGuiKey_GamepadFaceRight) == 1.f && d.value(ImGuiKey_GamepadDpadRight) == 1.f);
    CHECK(d.value(ImGuiKey_GamepadDpadUp) == 0.f && d.value(ImGuiKey_GamepadL1) == 1.f);
    CHECK_NEAR(d.value(ImGuiKey_GamepadR2), 0.5f, 0.01f);
    CHECK(d.value(ImGuiKey_GamepadRStickLeft) == 1.f);
    // ... over Bluetooth (0x31: one byte on), and its short report (the DualShock 4's layout)
    uint8_t dsbt[78] = {0x31, 0x00, 128, 128, 128, 128, 0, 0, 0, 0x80 | 8, 0x20};
    GamepadState e;
    REQUIRE(hidpad::parseReport(Model::DualSense, dsbt, sizeof(dsbt), e));
    CHECK(e.value(ImGuiKey_GamepadFaceUp) == 1.f && e.value(ImGuiKey_GamepadStart) == 1.f);
    uint8_t dsshort[10] = {0x01, 128, 128, 128, 128, 0x20 | 4, 0, 0, 0, 0};
    GamepadState f;
    REQUIRE(hidpad::parseReport(Model::DualSense, dsshort, sizeof(dsshort), f));
    CHECK(f.value(ImGuiKey_GamepadFaceDown) == 1.f && f.value(ImGuiKey_GamepadDpadDown) == 1.f);
    // Switch Pro Controller, full report: B (the bottom one), plus, ZL, the left stick all the way right
    uint8_t sw[64] = {0x30, 0, 0x8E, 0x04, 0x02, 0x80, 0xFF, 0x0F, 0x80, 0x00, 0x08, 0x80};
    GamepadState w;
    REQUIRE(hidpad::parseReport(Model::SwitchPro, sw, sizeof(sw), w));
    CHECK(w.type == PadType::Switch);
    CHECK(w.value(ImGuiKey_GamepadFaceDown) == 1.f && w.value(ImGuiKey_GamepadFaceRight) == 0.f);
    CHECK(w.value(ImGuiKey_GamepadStart) == 1.f && w.value(ImGuiKey_GamepadL2) == 1.f);
    CHECK(w.value(ImGuiKey_GamepadLStickRight) == 1.f && w.value(ImGuiKey_GamepadLStickUp) == 0.f);
    CHECK(w.value(ImGuiKey_GamepadRStickUp) == 0.f && w.value(ImGuiKey_GamepadRStickDown) == 0.f);
    uint8_t other[49] = {0x21};   // (a reply to a subcommand: no controls)
    CHECK(!hidpad::parseReport(Model::SwitchPro, other, sizeof(other), w));
    // Output: the light bar and motors where the drivers put them; the Switch's start-up
    const float light[3] = {1.f, 0.f, 0.5f};
    std::vector<uint8_t> o = hidpad::outputReport(Model::DualSense, 1.f, 0.f, light);
    REQUIRE(o.size() == 48);
    CHECK(o[0] == 0x02 && o[3] == 0 && o[4] == 255 && o[45] == 255 && o[46] == 0 && o[47] == 128);
    o = hidpad::outputReport(Model::DualShock4, 0.f, 1.f, light);
    REQUIRE(o.size() == 32);
    CHECK(o[0] == 0x05 && o[4] == 255 && o[5] == 0 && o[6] == 255 && o[8] == 128);
    CHECK(hidpad::outputReport(Model::SwitchPro, 1.f, 1.f, light).empty());
    std::vector<std::vector<uint8_t>> start = hidpad::switchStartup(true, 64);
    REQUIRE(start.size() == 6);
    CHECK(start[0][0] == 0x80 && start[0].size() == 64);
    CHECK(start[4][0] == 0x01 && start[4][10] == 0x03 && start[4][11] == 0x30);
    CHECK(hidpad::switchStartup(false, 49).size() == 2);
}
