// Game runtime: runs the scene's behaviours with a fixed timestep, independent of the frame rate.
#pragma once

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <initializer_list>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>
#include <vector>

#include "audio/audio.h"
#include "core/json.h"
#include "core/vecmath.h"
#include "game/behaviour_tree.h"
#include "game/input.h"
#include "game/prompts.h"
#include "game/navigation.h"
#include "game/scene_loader.h"
#include "game/strings.h"
#include "game/particles.h"
#include "game/ui.h"
#include "game/ui_layout.h"
#include "physics/physics.h"
#include "scene/scene.h"

// What the game is looking through: the first entity with a Camera component.
struct CameraView {
    Mat4 view;
    Vec3 position;
    float fovY = 60.f * DEG2RAD;
    float zNear = 0.1f, zFar = 2000.f;
    EntityId entity = kNoEntity;   // the camera's entity
    int player = 0;                // (split screen) its player, and its part of the screen: x, y, w, h (0..1)
    float rect[4] = {0.f, 0.f, 1.f, 1.f};
};

class Game;
class GameContext;

// A chain of timed steps: ctx.sequence(self.id).wait(1.f).then(openDoor).wait(2.f).then(closeDoor). Each wait()
// adds to the time from now; then() runs at the time reached so far. Dropped with its owner entity.
class Sequence {
public:
    Sequence& wait(float seconds) {
        at_ += seconds;
        return *this;
    }
    Sequence& then(std::function<void(GameContext&)> step);

private:
    friend class GameContext;
    Sequence(GameContext& ctx, uint32_t owner) : ctx_(&ctx), owner_(owner) {}
    GameContext* ctx_;
    uint32_t owner_;
    float at_ = 0.f;
};

// Values that outlive a scene (score, lives, inventory, progress): kept when ctx.loadScene() changes the scene,
// saved in save games, empty when the game starts
class GameState {
public:
    float number(const std::string& key, float fallback = 0.f) const;
    std::string text(const std::string& key, const std::string& fallback = "") const;
    bool flag(const std::string& key, bool fallback = false) const;
    void setNumber(const std::string& key, float value) { values_[key] = value; }
    void setText(const std::string& key, const std::string& value) { values_[key] = value; }
    void setFlag(const std::string& key, bool value) { values_[key] = value; }
    void set(const std::string& key, json::Value value) { values_[key] = std::move(value); }
    float add(const std::string& key, float amount) {   // number += amount; returns the new value
        setNumber(key, number(key) + amount);
        return number(key);
    }
    bool has(const std::string& key) const { return values_.count(key) > 0; }
    const json::Value* get(const std::string& key) const {   // the value whatever its type (nullptr: none)
        auto it = values_.find(key);
        return it == values_.end() ? nullptr : &it->second;
    }
    // "Score: {score}" -> "Score: 12": {key} becomes the value (whole numbers without decimals; unknown keys: "")
    std::string format(const std::string& text) const;
    void erase(const std::string& key) { values_.erase(key); }
    void clear() { values_.clear(); }
    // The game's values, for save games and multiplayer snapshots: without this machine's own (isLocal: the
    // player's settings.*, input.*, net.*, update.*, crash.*), which loading keeps as they are
    json::Value toJson() const;
    void fromJson(const json::Value& v);
    static bool isLocal(const std::string& key);

private:
    std::map<std::string, json::Value> values_;
};

// Where this machine is in a multiplayer game, and a message for another machine (GameContext::sendToServer)
enum class NetRole : uint8_t { Offline, Server, DedicatedServer, Client };
struct NetMessage {
    EntityId to = kNoEntity;
    Message message;
    bool toServer = false;
    uint16_t client = 0;   // (to clients: 0 all, else that one)
};

// Everything a behaviour can reach while it runs
class GameContext {
public:
    Scene& scene() { return *scene_; }
    const InputMap& input() const { return *input_; }
    float time() const { return time_; }   // seconds since Play
    float dt() const { return dt_; }       // length of this update (Game::kFixedDt)

    // Also finds entities spawned during this update; a pointer to one of those lasts until the next spawn()
    Entity* find(EntityId id);
    Entity* find(const std::string& name);
    Vec3 worldPosition(EntityId id) const;

    // Collision (see physics/physics.h). Queries see the world as of the last physics step (plus characters moved
    // since), not moves other behaviours make during this update.
    const Physics& physics() const { return *physics_; }
    bool raycast(Vec3 origin, Vec3 dir, float maxDistance, RaycastHit& hit, EntityId ignore = kNoEntity) const {
        return physics_->raycast(origin, dir, maxDistance, hit, ignore);
    }
    std::vector<EntityId> overlapSphere(Vec3 center, float radius) const { return physics_->overlapSphere(center, radius); }
    // The same, seeing only some layers: ctx.layerMask({"Enemy", "Default"}) (unknown names add nothing)
    uint32_t layerMask(std::initializer_list<const char*> layerNames) const;
    bool raycast(Vec3 origin, Vec3 dir, float maxDistance, RaycastHit& hit, EntityId ignore, uint32_t layers) const {
        return physics_->raycast(origin, dir, maxDistance, hit, ignore, layers);
    }
    std::vector<EntityId> overlapSphere(Vec3 center, float radius, uint32_t layers) const {
        return physics_->overlapSphere(center, radius, true, kNoEntity, layers);
    }

    // Tags (Entity::tags): every entity with one, in scene order (spawned ones included), or the first
    std::vector<EntityId> findWithTag(const std::string& tag);
    Entity* findFirstWithTag(const std::string& tag);

    // Messages: delivered to the entity's behaviours (onMessage) once this update's behaviours have run (or the
    // trigger callbacks, if sent from those). Messages sent while delivering go out in the same round, up to a limit.
    void send(EntityId to, const Message& message);
    void sendToTag(const std::string& tag, const Message& message);
    void broadcast(const Message& message);   // to every entity with behaviours
    // Multiplayer (net/net_session.h): to the same entity on the other machines, reliably and in order, delivered
    // there as send() does. sendToServer: from a client to the server (where Message::client says which client sent
    // it); sendToClients: from the server to every client, or one (its id). Where this machine is the server (and
    // plays too) or plays alone they're delivered here as well, so a game works the same offline.
    void sendToServer(EntityId to, const Message& message);
    void sendToClients(EntityId to, const Message& message, uint16_t client = 0);
    bool isServer() const { return netRole_ != NetRole::Client; }   // (hosting, or not in a multiplayer game)
    bool isClient() const { return netRole_ == NetRole::Client; }
    // Local players joining with their pads' Start (Game::setPadJoining), and how many play
    void setPadJoining(bool on);
    int localPlayers() const;

    // Timers, in game time (paused with the game), run after the update they come due in. With an owner they're
    // dropped when it's destroyed. Not kept in save games or across scene changes.
    using TimerId = uint32_t;
    TimerId after(float seconds, std::function<void(GameContext&)> fn, EntityId owner = kNoEntity);
    TimerId every(float seconds, std::function<void(GameContext&)> fn, EntityId owner = kNoEntity);
    void cancel(TimerId timer);
    Sequence sequence(EntityId owner = kNoEntity) { return Sequence(*this, owner); }
    // Moves `self` by `delta` (world space) as a character: with a capsule or sphere collider it slides along walls,
    // climbs slopes and steps, rides moving platforms and pushes rigid bodies. Without one it just moves.
    CharacterMove moveCharacter(Entity& self, Vec3 delta);

    // A ragdoll (game/ragdoll.h) under `root` goes limp (simulated, its Animator stopped) or back to animation
    void setRagdoll(EntityId root, bool limp);

    // Navigation (see game/navigation.h): a walking path around walls and over steps, as corner points on the
    // ground from `from` to (nearest reachable spot to) `to`; false if there's no way. The walkable grid comes
    // from a saved bake next to the scene file (<scene>.vkw.nav, the editor's "Bake and save") when it matches the
    // level, else it's baked: on a worker thread from the start when the scene has NavAgents or NavLinks, or the
    // first time it's needed. Rigid bodies (and entities tagged "NavObstacle") block the cells under them.
    // `jumps`, when given, marks the off-mesh links along it (see NavGrid::findPath).
    bool findPath(Vec3 from, Vec3 to, std::vector<Vec3>& path, std::vector<uint8_t>* jumps = nullptr);
    const NavGrid& navigation();
    void rebuildNavigation();   // after the level itself changed (static colliders added, moved or removed)

    // Creates a copy of the scene's prefab at `position` (world space), turned so its -Z faces `forward` (on top of
    // the prefab's own rotation and scale), under `parent` if given. It joins the scene after this update, when
    // its behaviours' start() runs; the returned id works with find() right away. kNoEntity if there's no prefab.
    EntityId spawn(const std::string& prefab, Vec3 position, Vec3 forward = {0, 0, -1}, EntityId parent = kNoEntity);

    // Sounds (no-ops without an audio engine): a clip name or file, at a place in the world or in stereo
    void playSound(const std::string& clip, Vec3 position, float volume = 1.f);
    void playSound2D(const std::string& clip, float volume = 1.f);
    // A noise AI brains can hear (without a sound: footsteps, a thrown stone); playSound makes one too
    void makeNoise(Vec3 position, float loudness = 1.f, EntityId source = kNoEntity);
    // Gamepad feedback: rumble (the heavy and light motors 0..1, for `seconds`; 0 stops it; nothing while the
    // player's settings.rumble is off) and the colour of a light bar (DualShock 4, DualSense; sRGB 0..1)
    // (local multiplayer: the pad of the player whose behaviour asks)
    void rumble(float low, float high, float seconds);
    // What to press for an action, as a prompt for GameUI's text (glyphs: game/prompts.h), on the device the player
    // used last; texts get the same from "{button:jump}" ("{button:jump:pad}", "{button:jump:keyboard}": that one)
    std::string prompt(const std::string& action) const;
    void setGamepadLight(Vec3 color);
    // An AI brain's blackboard (nullptr: no AIBrain on it)
    json::Value* blackboard(EntityId id);
    // Music: one track, streamed and looping on the music bus, cross-fading from the one before
    void playMusic(const std::string& clip, float fadeSeconds = 1.f, float volume = 1.f);
    void stopMusic(float fadeSeconds = 1.f);
    // Layered music: stems started together (same length), each with a volume to fade (adaptive music: add drums
    // in a fight); bpm / beatsPerBar time stingers, short phrases played over the music on its next beat or bar
    void playMusicLayers(const std::vector<std::string>& clips, const std::vector<float>& volumes, float fadeSeconds = 1.f,
                         float bpm = 0.f, int beatsPerBar = 4);
    void setMusicLayer(int layer, float volume, float fadeSeconds = 1.f);
    void playStinger(const std::string& clip, const std::string& sync = "beat", float volume = 1.f);   // "now", "beat", "bar"
    // The mixer's buses ("effects", "music", "ui", "ambience"): a volume, or lowered for a while (under a voice line)
    void setBusVolume(const std::string& bus, float volume);

    // Text in the player's language (game/strings.h): a key's text (or `fallback`), text with {@key}s and
    // {state values} filled in, and the language (loads strings/<language>.json)
    std::string text(const std::string& key, const std::string& fallback = "") const;
    std::string format(const std::string& text) const;
    void setLanguage(const std::string& language);
    const std::string& language() const;
    void duck(const std::string& bus, float volume, float seconds);

    // Camera shake (game/camera_shake.h): trauma (0..1) that decays over a second or so; from a place in the world,
    // less the further the camera is (all of it within 3 m, none past `radius`)
    void shakeCamera(float amount);
    void shakeCamera(float amount, Vec3 position, float radius = 25.f);

    // Particles: a burst from the entity's ParticleEmitter (`count`, or its burst size), or any look anywhere
    void emitParticles(EntityId id, int count = -1);
    void emitParticles(const ParticleEmitter& look, Vec3 position, int count, Vec3 up = {0, 1, 0});

    // Removes the entity (and its children) once every behaviour has run this update, so references stay valid
    void destroy(EntityId id);
    // Breaks a pre-fractured entity (a Fracture component) into its pieces: rigid bodies flying apart from where it
    // was (cleared away after its pieceLife), or held together by joints if it's set to; without one (or its pieces
    // not made yet), just destroys it
    void shatter(EntityId id);
    bool isDestroyed(EntityId id) const;   // queued this update
    void log(const std::string& message);  // shown in the viewport and printed to stdout

    // ---- Scene flow. These take effect at the end of the update (after every behaviour has run).
    // Replaces the scene with another scene file (relative to the project). state() and the clock carry on.
    void loadScene(const std::string& path);
    void quit();                                       // vkgame exits; the editor stops playing
    GameState& state();                                // what survives scene changes
    // Save games: the scene as it is now (entities, behaviours' fields and state(), rigid-body velocities), state()
    // and the clock, in the project's save folder (project::saveDir). Loading replaces the scene with the saved one.
    void saveGame(const std::string& slot = "quicksave");
    void loadGame(const std::string& slot = "quicksave");
    bool hasSaveGame(const std::string& slot = "quicksave") const;
    const std::string& scenePath() const;              // the current scene's file ("" for a scene built in code)
    // Loading: with asynchronous loading on (Game::setAsyncLoading, as vkgame and the editor do), loadScene()
    // reads the next scene in the background. The current one stays on screen, frozen (no updates; drawUI() still
    // runs, so a behaviour can draw its own loading screen while loading() is true), under a default loading screen
    // unless showLoadingScreen(false).
    bool loading() const;
    void showLoadingScreen(bool show);

    // Streaming: adds another scene file's entities to this scene, under a new root entity placed at `offset`
    // (loaded in the background with asynchronous loading, else at the end of this update). Their behaviours
    // start() as they arrive. Returns that root's id: streamedIn() tells when it's there, streamOut() removes it all
    // (or cancels the load). The file's prefabs join the scene's (by name); its settings are ignored.
    EntityId streamIn(const std::string& path, Vec3 offset = {});
    bool streamedIn(EntityId chunk) const;
    void streamOut(EntityId chunk);

    // Pausing stops update() (and physics); drawUI() still runs, so a pause menu can resume
    void setPaused(bool paused);
    bool paused() const;

private:
    friend class Game;
    Game* game_ = nullptr;
    Scene* scene_ = nullptr;
    const InputMap* input_ = nullptr;
    Physics* physics_ = nullptr;
    const MeshLibrary* meshes_ = nullptr;
    AudioEngine* audio_ = nullptr;
    float time_ = 0.f, dt_ = 0.f;
    std::vector<EntityId> destroyQueue_;
    std::vector<Entity> spawnQueue_;
    std::vector<std::pair<EntityId, Message>> messages_;
    NetRole netRole_ = NetRole::Offline;
    std::vector<NetMessage> netOutbox_;
    std::vector<std::pair<float, std::string>>* log_ = nullptr;
};

class Game {
public:
    static constexpr float kFixedDt = 1.f / 60.f;
    static constexpr int kMaxStepsPerFrame = 8;   // after a long hitch, drop time instead of spiralling

    // Takes over `scene` (the play-mode copy) until stop(), and calls every behaviour's start().
    // `meshes` is needed for mesh colliders.
    void start(Scene& scene, const MeshLibrary& meshes);
    void stop();
    bool running() const { return scene_ != nullptr; }

    // Once per rendered frame: latches input, then runs as many fixed updates as the elapsed time calls for.
    // keyboard/mouse = false while the UI owns that device. mouseDelta: this frame's mouse movement (raw while
    // the cursor is captured); the overload without it uses Dear ImGui's.
    void frame(float dt, bool keyboard, bool mouse, ImVec2 mouseDelta);
    void frame(float dt, bool keyboard, bool mouse) { frame(dt, keyboard, mouse, ImGui::GetIO().MouseDelta); }
    void setPaused(bool paused) { paused_ = paused; }
    // Optional: with an audio engine, AudioSources play and ctx.playSound() is heard. Set before start().
    void setAudio(AudioEngine* audio) { audio_ = audio; }
    // Optional: where gamepad feedback goes (the host's platform::gamepadRumble / setGamepadLight). Requests are
    // remembered either way (tests): the last rumble asked for, and the light's colour.
    struct GamepadOutput {
        // `pad`: the slot in gamepads() (a player's own pad), or -1: the first one
        std::function<void(int pad, float low, float high, float seconds)> rumble;
        std::function<void(Vec3 color)> light;
    };
    void setGamepadOutput(GamepadOutput output) { gamepadOut_ = std::move(output); }
    // Optional: opens a web page (the UI action "@open <url>"; the host's platform::openWithSystem)
    void setUrlOpener(std::function<void(const std::string& url)> open) { urlOpener_ = std::move(open); }
    // Multiplayer from the game's menus: the UI actions "@host [port]", "@join address", "@joinlobby code",
    // "@lobbies" (list the relay's) and "@leave" go to whatever runs the session (vkgame), which reports back in the
    // game state (net.status, net.code, net.lobbies, net.lobby.<i>.name / code / players: see the README)
    void setNetworkHandler(std::function<void(const std::string& verb, const std::string& arg)> handler) {
        networkHandler_ = std::move(handler);
    }
    // Updates and crash reports from the game's menus: "@update" (download and install the update that's out),
    // "@restart" (into it), "@sendcrashreport", "@dismisscrashreport" go to the host (vkgame), which reports in the
    // game state (update.*, crash.*: see the README)
    void setHostHandler(std::function<void(const std::string& verb, const std::string& arg)> handler) {
        hostHandler_ = std::move(handler);
    }
    struct Rumble {
        float low = 0.f, high = 0.f, seconds = 0.f;
        int pad = -1;   // (the asking behaviour's player's pad, or -1: the first)
    };
    const Rumble& lastRumble() const { return lastRumble_; }
    const Vec3* gamepadLight() const { return hasLight_ ? &light_ : nullptr; }
    // For ctx.loadScene() / loadGame(): the libraries scenes load into (imports are appended). Without them, scene
    // changes fail (logged). The meshes must be the ones given to start().
    void setAssetLibraries(MeshLibrary& meshes, TextureLibrary& textures) { meshLib_ = &meshes; texLib_ = &textures; }
    // Scene changes and streaming load on a worker thread (off by default: everything happens within the update
    // that asked, which keeps tests and tools deterministic). Needs setAssetLibraries().
    void setAsyncLoading(bool on) { async_ = on; }
    // The language the game starts in (strings/<language>.json in the project); ctx.setLanguage changes it
    void setLanguage(const std::string& language);
    const StringTable& strings() const { return strings_; }
    GameContext& context() { return ctx_; }   // what behaviours get (for hosts and tests)
    bool loading() const { return loader_.busy(); }
    void finishLoading();   // waits for loads in progress and applies them
    float loadingProgress() const { return loader_.progress(); }
    const std::string& loadError() const { return loadError_; }   // why the last scene change failed ("" if it didn't)
    // Changes scene as ctx.loadScene() does, from outside the game (vkgame starts its first scene this way, so it
    // loads behind the loading screen too)
    void loadScene(const std::string& path);
    void setScenePath(const std::string& path) { scenePath_ = path; }   // the scene's file, recorded in save games
    const std::string& scenePath() const { return scenePath_; }
    void setSaveDirectory(const std::filesystem::path& dir) { saveDir_ = dir; }
    const std::filesystem::path& saveDirectory() const { return saveDir_; }
    bool quitRequested() const { return quit_; }
    GameState& state() { return state_; }

    // The player's settings: settings.json in the save folder, read at start(), and in the game state as
    // settings.<name> (kSettings: volumes 0..1, headphones, rumble), so menus (UI sliders and toggles bound to them) and
    // scripts change them; applied every frame, saved a second after a change and at stop()
    static const std::vector<std::pair<std::string, json::Value>>& defaultSettings();
    // settings.colorBlind as SceneRenderInfo::colorFilter (0 none, 1 protanopia, 2 deuteranopia, 3 tritanopia)
    int colorFilter() const;
    void saveSettings();

    // The same as the GameContext requests, done right away (between updates)
    bool changeScene(const std::string& path, std::string& error);
    bool saveGame(const std::string& slot, std::string& error);
    bool loadGame(const std::string& slot, std::string& error);
    std::filesystem::path saveGamePath(const std::string& slot) const;
    bool paused() const { return paused_; }
    void step();   // run exactly one fixed update (frame stepping while paused)
    // Once per rendered frame, after frame(): the behaviours' drawUI(), then the scene's UI layouts (UIElement
    // components, game/ui_layout.h), between ui.begin() and ui.end()
    void drawUI(GameUI& ui);
    const UIController& uiController() const { return uiController_; }

    // ---- Local players (split screen: cameras with Camera::player 1..4) and gamepads
    // The pads connected now (the host's platform::gamepads(), each frame before frame()). With two or more player
    // cameras, player 1 plays with the keyboard, the mouse and the first pad, player n with the n-th pad, and the
    // behaviours under a player camera's root (its topmost ancestor) read that player's input as ctx.input().
    // Otherwise (and without pads) every pad is everyone's, through Dear ImGui.
    // Joining with Start (setPadJoining(true), the UI action "@padjoin on"): instead of pads given out in order,
    // a pad joins when its Start is pressed: the first one plays with player 1 (the keyboard's), the next ones are
    // players 2, 3, 4. A pad unplugged leaves its seat waiting: the next Start takes it. The game state says how many
    // play (input.players) and with what (input.player.<n>.pad: "xbox", "playstation", ...; "" none yet).
    void setPadJoining(bool on);
    bool padJoining() const { return padJoining_; }
    void resetPlayers();   // everyone out but player 1 (with no pad)
    int localPlayers() const;
    void setGamepads(std::vector<GamepadState> pads) { pads_ = std::move(pads); }
    const std::vector<GamepadState>& gamepads() const { return pads_; }
    const InputMap& playerInput(int player) const { return players_[std::clamp(player, 0, kMaxPlayers - 1)]; }
    int playerPad(int player) const;   // the slot (in gamepads()) of the pad player n (0-based) plays with; -1: none
    // What was used last (button prompts show it): the keyboard and mouse, or a pad, and which kind
    prompts::Device lastDevice() const { return lastDevice_; }
    PadType lastPadType() const { return lastPad_; }
    // An action's binding as a prompt (glyphs: game/prompts.h) on the device used last, or on `device`
    std::string prompt(const std::string& action) const { return prompt(action, lastDevice_); }
    std::string prompt(const std::string& action, prompts::Device device) const;
    // Rebinding (the UI action "@rebind jump"): the next press on `device` (keys and mouse buttons, or a pad's
    // buttons and sticks) becomes the action's binding there, saved in the player's settings (settings.bind.<action>:
    // game/input_bindings.h's format); Esc or 10 s cancels. The game state's input.rebinding names the action
    // while it waits (menus say "Press a button..."); the UI doesn't take input meanwhile.
    void rebind(const std::string& action, prompts::Device device);
    // A stick as a whole ("@rebindstick move_forward,move_back,move_left,move_right": the up, down, left and right
    // actions): the next stick or d-pad pushed (or key of a cluster: WASD, the arrows, ...) becomes all four
    void rebindStick(const std::vector<std::string>& actions, prompts::Device device);
    bool rebinding() const { return !rebind_.action.empty(); }
    void resetBindings(const std::string& action = "");   // the scene's bindings back: all, or one action's
    const std::vector<InputAction>& bindings() const { return actions_; }   // the scene's, with the player's own
    static constexpr int kMaxPlayers = 4;

    // ---- Multiplayer (net/net_session.h)
    // The behaviours of `root` and everything under it read `input` (a remote player's) instead of this machine's;
    // null: back to this machine's
    void setInputSource(EntityId root, const InputMap* input);
    // Not simulated here: its behaviours don't run (another machine runs it and sends where it is)
    void setSimulated(EntityId id, bool simulated);
    bool simulated(EntityId id) const { return !notSimulated_.count(id); }
    // This machine's player: the game looks through a camera under it (rather than the first one in the scene)
    // Multiplayer messages: what the machine is (NetSession sets it), and the ones behaviours sent since last asked
    void setNetRole(NetRole role) { ctx_.netRole_ = role; }
    std::vector<NetMessage> takeNetMessages() { return std::move(ctx_.netOutbox_); }
    void setLocalPlayer(EntityId id) { localPlayer_ = id; }
    EntityId localPlayer() const { return localPlayer_; }
    // Called just before and after every fixed update (feeding remote inputs, sending this machine's)
    void setTickHooks(std::function<void()> before, std::function<void()> after) {
        beforeTick_ = std::move(before);
        afterTick_ = std::move(after);
    }
    // Lag compensation: called around the update of each entity a remote player's input drives (begin gets that
    // player's root), so the server can put the world back where that player saw it
    void setRewindHooks(std::function<void(EntityId)> begin, std::function<void()> end) {
        rewindBegin_ = std::move(begin);
        rewindEnd_ = std::move(end);
    }
    void syncPhysics();   // the physics world brought up to date with entities moved by hand (no time passes)
    Scene* scene() { return scene_; }
    // Entities joining the scene at the next update, their behaviours started then (like spawns; their ids kept)
    void addEntities(std::vector<Entity> entities) {
        for (Entity& e : entities) ctx_.spawnQueue_.push_back(std::move(e));
    }

    float time() const { return time_; }               // simulated seconds since start()
    float updateMs() const { return updateMs_; }       // CPU time of a fixed update (smoothed), milliseconds
    unsigned long long tick() const { return tick_; }  // fixed updates since start()
    InputMap& input() { return input_; }
    // Messages from GameContext::log(), with the game time they were logged at (latest last)
    const std::vector<std::pair<float, std::string>>& log() const { return log_; }

    std::vector<Mat4> worldMatrices() const;      // aligned with scene.entities
    const NavGrid& navigation();                  // baked on first use (see GameContext::findPath)
    const Physics& physics() const { return physics_; }
    void setRecordContacts(bool on) { physics_.setRecordContacts(on); }   // (Physics::contacts(): for showing them)
    const ParticleSystem& particles() const { return particles_; }
    bool activeCamera(CameraView& out) const;     // the first entity with a camera; false if none
    float trauma() const { return trauma_; }      // the camera's shake now (GameContext::shakeCamera)
    // What the screen shows: the split-screen player cameras (Camera::player) when there are two or more, by player,
    // each with its part of the screen (2: left and right halves; 3-4: quarters, player order); else just
    // activeCamera()'s, all of it. Empty if there's no camera.
    std::vector<CameraView> cameras() const;

private:
    CameraView cameraAt(size_t entityIndex) const;

public:
    // Render interpolation (on by default): worldMatrices() and activeCamera() blend the last two fixed updates
    // by how far the frame is into the next one, so motion is smooth when frames and updates don't line up
    void setInterpolation(bool on) { interpolate_ = on; }
    bool interpolation() const { return interpolate_; }

private:
    friend class GameContext;
    void begin(bool fresh = true, bool keepPhysics = false);   // starts the current scene: physics, sounds, animators (if fresh), start()
    void stopSounds();
    void applyRequests();   // scene changes, saves and quitting asked for during the update
    void fixedUpdate(float dt);
    void prepareContext(float dt);
    void flushDestroyed();
    void flushSpawned();
    void flushQueues();
    void deliverMessages();   // one round
    void runTimers();
    void updateTriggers();
    void startAudio();
    void updateAudio(float dt);
    void playImpacts(const std::vector<struct Impact>& impacts);
    void savePreviousWorlds();
    void pollLoads(bool wait);   // applies finished loads (all of them, waiting, if `wait`)
    void completeSceneChange();
    void addChunk(EntityId root, const std::string& path, Vec3 offset, Scene& chunk);
    void drawLoadingScreen(GameUI& ui);
    void runUIAction(const UIAction& a);   // a button pressed in a UI layout (game/ui_layout.h)
    UIController uiController_;
    std::unordered_map<EntityId, const InputMap*> inputSources_;
    std::unordered_set<EntityId> notSimulated_;
    EntityId localPlayer_ = kNoEntity;
    std::function<void()> beforeTick_, afterTick_;
    const InputMap* inputFor(const Entity& e) const;
    EntityId inputRoot(const Entity& e) const;   // the root whose input source drives it (kNoEntity: this machine's)
    std::function<void(EntityId)> rewindBegin_;
    std::function<void()> rewindEnd_;
    bool keyboard_ = true;   // the last frame's: whether the UI may read keys

    Scene* scene_ = nullptr;
    const MeshLibrary* meshes_ = nullptr;
    InputMap input_;
    Physics physics_;   // (Jolt) bodies persist between updates: made again on a new scene
    ParticleSystem particles_;
    StringTable strings_;
    bool stringsLoaded_ = false;
    NavGrid nav_;
    bool navBaked_ = false;                            // nav_ is up to date (or navJob_ will make it so)
    std::future<std::shared_ptr<NavGrid>> navJob_;     // a bake on a worker thread, started with the scene
    void startNavigation();
    void updateNavObstacles();
    bool async_ = false, loadingScreen_ = true;
    SceneLoader loader_;   // the next scene, when changing scenes asynchronously
    std::string loadingScene_, loadError_;
    struct Chunk {   // a scene being streamed in
        EntityId root = kNoEntity;
        std::string path;
        Vec3 offset;
        std::unique_ptr<SceneLoader> loader;
        bool cancelled = false;
    };
    std::vector<Chunk> chunks_;
    struct Timer {
        GameContext::TimerId id;
        double at;
        float interval;   // 0: once
        EntityId owner;
        std::function<void(GameContext&)> fn;
        bool done = false;
    };
    std::vector<Timer> timers_;
    GameContext::TimerId nextTimer_ = 1;
    std::vector<std::pair<EntityId, EntityId>> triggerPairs_;   // overlapping after the previous update
    AudioEngine* audio_ = nullptr;
    GamepadOutput gamepadOut_;
    std::function<void(const std::string&)> urlOpener_;
    std::function<void(const std::string&, const std::string&)> networkHandler_, hostHandler_;
    // Local players and pads
    std::vector<GamepadState> pads_;
    InputMap players_[kMaxPlayers];
    std::vector<EntityId> playerRoots_;   // (2+ player cameras) each player's camera's root
    std::vector<int> playerPads_;         // ... and the pad slot each plays with
    std::vector<InputAction> actions_;    // the scene's bindings with the player's own
    prompts::Device lastDevice_ = prompts::Device::Keyboard;
    PadType lastPad_ = PadType::Generic;
    struct Rebind {
        std::string action;
        prompts::Device device = prompts::Device::Keyboard;
        float waited = 0.f;
        bool released = false;   // (everything let go since it was asked: the press that asked doesn't count)
        std::vector<std::string> stick;   // (a stick's four actions)
    } rebind_;
    float trauma_ = 0.f;   // camera shake
    bool padJoining_ = false;
    int seats_[kMaxPlayers] = {-1, -1, -1, -1};   // the pad slot each local player plays with
    int joined_ = 1;
    std::vector<float> startWas_;   // each pad's Start last frame (presses join)
    void updateSeats();
    void publishSeats();
    void publishConflicts();
    void applyBindings();        // actions_ from the scene and settings, into every InputMap
    void beginInput(bool keyboard, bool mouse, ImVec2 mouseDelta);
    void endInputStep();
    void trackDevice();
    void updateRebind(float dt);
    Rumble lastRumble_;
    Vec3 light_;
    bool hasLight_ = false;
    std::vector<std::pair<EntityId, AudioEngine::Sound>> attached_;   // sounds following their entity
    std::unordered_map<AudioEngine::Sound, Vec3> soundPos_;           // ... where they were last frame
    Vec3 listener_;
    bool haveListener_ = false;
    std::map<std::string, json::Value> settings_;
    std::unordered_map<EntityId, ai::BrainState> brains_;   // AI brains' memory (game/behaviour_tree.h)
    std::vector<ai::Noise> noises_;                          // heard by them (the last two seconds)
    std::vector<std::pair<EntityId, float>> debris_;         // shattered pieces, and when they're cleared away
    uint64_t noiseSerial_ = 0;
    void updateBrains();
    float settingsSaveIn_ = -1.f;   // (>= 0: changed, saved when it runs out)
    void loadSettings();
    void putSettings();               // into the game state (at start, and over a loaded save game's)
    void syncSettings(float dt);      // the state's edits taken, applied to the audio
    void occludeSounds(EntityId listenerEntity);
    GameContext ctx_;
    std::vector<std::pair<float, std::string>> log_;
    float accumulator_ = 0.f, time_ = 0.f, updateMs_ = 0.f;
    unsigned long long tick_ = 0;
    bool paused_ = false;
    bool interpolate_ = true;
    std::vector<Mat4> prevWorlds_;   // world matrices before the last fixed update (render interpolation)
    std::vector<EntityId> prevIds_;

    MeshLibrary* meshLib_ = nullptr;
    TextureLibrary* texLib_ = nullptr;
    std::string scenePath_;
    std::filesystem::path saveDir_;
    GameState state_;
    std::string pendingScene_, pendingSave_, pendingLoad_;
    bool quit_ = false;
};
