# Vulkan World Editor

A small Vulkan 1.3 3D scene with a Blender-style editing UI, in C++17.

- Runs on **Linux** (X11 / XWayland, via raw XCB), **Windows** (Win32) and **macOS** (Cocoa + MoltenVK). No GLFW/SDL needed.
- UI: [Dear ImGui](https://github.com/ocornut/imgui) v1.92.9b + [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo);
  images: [stb_image](https://github.com/nothings/stb) v2.30; glTF: [cgltf](https://github.com/jkuhlmann/cgltf) v1.14;
  audio: [miniaudio](https://miniaud.io) v0.11.21.
  All vendored in `third_party/` and compiled into the binary.

## Build & run

Requires the [Vulkan SDK](https://vulkan.lunarg.com/) (sets `VULKAN_SDK`, provides `glslc`) and CMake 3.20+.

### Linux

Also needs libxcb (installed on any X11/XWayland desktop).

```sh
cmake -S . -B build
cmake --build build -j
./build/vkworld
```

### Windows

Install the Vulkan SDK and Visual Studio 2019+ with the "Desktop development with C++" workload, then from a
Developer Command Prompt (or any shell with CMake on the PATH):

```bat
cmake -S . -B build
cmake --build build --config Release
build\Release\vkworld.exe
```

Or open the folder in Visual Studio / VS Code (CMake Tools) and run the `vkworld` target.
MinGW-w64 and Ninja also work (`cmake -S . -B build -G Ninja`; the exe is then `build\vkworld.exe`).

### macOS

Install the Vulkan SDK (it provides MoltenVK and `glslc`), then build as on Linux:

```sh
cmake -S . -B build
cmake --build build -j
./build/vkworld
```

The editor renders at one pixel per point (no Retina scaling). Shortcuts listed with Ctrl use Cmd.

### Running the game without the editor

`vkgame` plays a scene full-window, without the editor:

```sh
./vkgame                 # scene.vkw from the working directory or next to the executable, else the demo
./vkgame my_level.vkw --fullscreen
./vkgame ../examples/sample_game   # a project folder: its start scene, with its game module
```

The mouse is captured for the game (hidden, raw movement): Esc releases it, a click takes it back, F11 toggles
fullscreen.

### Shipping a game

**File > Export game** (or `vkworld <project folder> --export <folder> [--pack | --single-file]`, without a window)
makes something that runs on its own, in one of three layouts:

- **Folder**: the game (`vkgame` named after the project), the engine library, the shaders and the project's files.
- **Pack** (`--pack`): the game, the engine library, and everything else in one `game.pak`.
- **Single file** (`--single-file`): the data appended to the game's executable.

Configure with `-DVKWORLD_STANDALONE_GAMES=ON` and each game module is also built into a **standalone executable**
(`<project>/bin/<module>_game`: the engine linked statically, the module's code compiled in). Exports use it when
it's there: no engine or module library to ship, so a single-file export is the whole game in one file.

Source code, build files and hidden files stay behind; scenes that use files from outside the project get a
warning (the game won't have them). With an `"icon"` (a PNG) in `project.json`, Linux exports get a `.desktop`
entry; on macOS the game is wrapped in an `.app` bundle. Players need a Vulkan driver (and X11 / XWayland on
Linux); everything else is inside. An export is for the platform it's made on: export on each OS you ship for.

### Tests

The engine has unit tests (`tests/`, no external framework): JSON, OBJ import, scene hierarchy and
serialization, collision and character movement, prefabs and spawning, the game loop, input actions and the
built-in behaviours. They run headless, without a GPU.

```sh
make engine_tests && make test     # or: ctest, or ./engine_tests <name filter> for detailed output
```

Turn them off with `-DVKWORLD_BUILD_TESTS=OFF`.

`tests/assets/formats` holds a scene file of every format the engine has ever written, each saved by the engine
of its time: the format tests load them all, so old projects keep opening.

### Smoke test (rendering for real)

CI also runs the programs on Linux with Mesa's software Vulkan driver (lavapipe) under Xvfb, with the validation
layer: the game, the editor on the stress scene, and an exported sample game each render a few frames and save a
screenshot, which `tools/check_screenshot.py` checks isn't blank. Programs run with `--screenshot` exit with 1 if
the screenshot failed or the validation layer reported an error. Locally:

```sh
VKWORLD_VALIDATION=1 xvfb-run -a ./vkgame --screenshot shot.png --frames 30 && python3 ../tools/check_screenshot.py shot.png
```

### Performance

`engine_bench [statics] [bodies] [triggers] [ticks]` builds a stress scene (the demo plus, by default, 4000 static
objects, 300 rigid bodies and 200 triggers) and times the CPU side of a frame: world matrices, draw lists, the
physics world, raycasts and the fixed update. It also prints a checksum of where everything ended up, to check
that an optimisation didn't change the simulation. `vkworld --stress` / `vkgame --stress` (or File > New stress
test scene) open the same scene; the editor's Inspector shows drawn / culled objects and the game update time.

### Validation layer

Set `VKWORLD_VALIDATION=1` to enable the Khronos validation layer (on by default in Debug builds,
e.g. Visual Studio's default Debug configuration). Messages go to the console.

## Layout

- **Menu bar**: File (save/load scene, import OBJ, reset), Edit, Add, Game (play/pause/step), View.
- **Importing models**: File / Add > Import model... loads **glTF** (`.gltf` with its files, or `.glb`) or a
  Wavefront **OBJ**. A glTF model keeps its node hierarchy, real size, materials (base colour + texture) and
  textures: it becomes a new prefab, and a copy is placed in the scene. An OBJ is a single mesh (with its texture
  coordinates), centred and scaled to fit a unit cube. Saved scenes reference model files by absolute path and
  re-import them on load.
- **Toolbar**: Select/Move/Rotate/Scale tools, world/local axes, snapping, add/duplicate/delete/focus, undo/redo,
  Play/Pause/Step, fly speed.
- **Outliner** (left): the entity tree, with visibility toggles, filter, right-click menu, double-click to focus.
  Drag an entity onto another to make it a child (it keeps its place in the world); drop it on the empty area
  below the tree to move it back to the root.
- **Inspector** (right): name, flags, transform (relative to the parent), the entity's components (add with
  "Add component", remove with the x on their header), editor camera and renderer stats.
- **Viewport**: selected object gets an orange outline (visible through other objects) plus the gizmo.
  The cube in the top-right corner snaps/rotates the view.

## Projects and the asset browser

A **project** is a folder with your game's scenes and assets and a `project.json` (its name and start scene).
Files inside it are saved with paths relative to the folder, so a project can be moved, zipped or put in git;
files outside it keep absolute paths.

```sh
./vkworld path/to/MyGame     # open a project (a folder without project.json becomes one)
./vkgame path/to/MyGame      # play its start scene
```

Without an argument both use the working directory. In the editor: File > Open project folder..., and
File > Use this scene at startup sets the start scene.

The **Assets** panel (under the viewport; View > Assets) shows the project's folders, scenes, prefabs, models
(OBJ / glTF), textures and sounds, and refreshes on its own:

- **Double-click**: open a scene, import a model or prefab, put a texture on the selected object, play a sound.
- **Drag onto the viewport**: models and prefabs are placed where you drop them; a texture goes on the object
  under the mouse; a sound becomes that object's Audio Source.
- **Drag onto the Inspector**: textures onto Mesh Renderer > Texture, sounds onto Audio Source > Clip file.

## Scenes: entities and components

A scene is a flat list of **entities**. Each has a name, an id, an optional **parent** (transforms are relative to
it; hiding, deleting or duplicating a parent includes its children) and a **transform**, plus optional components:

| Component       | What it does                                                         |
|-----------------|----------------------------------------------------------------------|
| Mesh Renderer   | Draws a mesh (built-in or imported) with a colour and optional texture |
| Light           | Directional (the sun, with shadows) or point light                   |
| Camera          | The game renders through the first entity that has one               |
| Collider        | Box, sphere, capsule or mesh shape; solid or trigger (see Collision) |
| Rigid Body      | Falls, bounces, slides, gets pushed and knocked (see Collision)      |
| Audio Source    | Plays a sound: on start (looping ones follow the entity), on impact  |
| Behaviours      | Game logic (any number per entity), see below                       |

An entity without components is an **empty**: a handle for grouping others (Add > Empty). Cameras and empties
show as icons in the viewport and can be clicked like meshes.

Scenes are saved as readable **JSON** (`scene.vkw`). Files from earlier versions (the line-based text format)
still load.

## Play mode

**Play** (Ctrl+P) copies the scene and runs the game on the copy; **Stop** throws the copy away and restores
the scene exactly as it was, so anything changed while playing is temporary (and not undoable).

The demo scene is a tiny game: you are the **Player** (a capsule body with a PlayerController and a camera child).
Walk with WASD / arrow keys or the left stick, look with the mouse (click the view to capture it, or hold the right
button) or the right stick, Space / A to jump, Shift to sprint, and collect the
8 floating gems. You bump into the pillars and can jump onto things. Left click shoots: the scattered objects
break, and cubes leave a gem behind. On the left there's a stack of crates to push over or shoot, and balls keep
dropping on the right.

- The game updates at a fixed **60 Hz**, independent of the frame rate (several updates per frame on a slow
  machine, none on some frames of a fast one; a long hitch is capped at 8 updates).
  **Pause** freezes it and **Step** advances one update.
- It renders through the first entity with a **Camera** component (Add > Camera creates one at the current
  view). Without one, the editor view is used.
- Game code reads **named input actions** (`move_forward`, `jump`, `look`, ...), never raw keys. Each action is
  triggered by keys, a mouse button and gamepad controls (sticks and triggers are analog: `ctx.input().value()` /
  `axis()` go from 0 to 1). Edit them in **Game > Input bindings** (click "+ key", then press a key or gamepad
  button); they're saved with the scene. Presses are latched until the next update reads them, so a quick tap is
  never missed. The keyboard is ignored while typing in a text field, and the mouse while it's over a panel.
- **Click the game view to capture the mouse** for mouse look without holding a button; Esc releases it.
- Gamepads work through Dear ImGui's backends (XInput on Windows, GameController on macOS) and the Linux joystick
  API (`/dev/input/js0`, Xbox-style layout).
- Messages logged by behaviours show at the bottom of the viewport (and on stdout).
- Game > Maximize viewport on play hides the side panels while playing.

## Rendering

- **Materials**: physically based (glTF metallic-roughness, GGX). A Mesh Renderer has a colour and texture (PNG,
  JPEG, TGA, BMP via Inspector > Load texture..., or the built-in procedural Checker / Bricks / Tiles), metallic
  and roughness (optionally from a metal/rough map), a normal map (tangents come from screen-space derivatives,
  so meshes don't need them; built-in "Bricks Normal") and an emissive colour, strength and map. glTF materials
  import all of these. Built-in meshes have box-mapped texture coordinates; imported ones use their own. Textures
  get mipmaps and anisotropic filtering, and are read as sRGB colour or as plain data, whichever the slot needs.
  The demo scene's "Materials" row shows them.
- **Lights**: Light components, directional (shines along the entity's -Z), point (colour, intensity, range) or
  spot (a cone along -Z, with a soft edge). The first directional light is the sun; up to 8 point and spot lights
  (nearest the camera) light each frame. Scenes without a directional light get a default sun.
- **Shadows**: the sun's are 4 cascades (2048x2048 each) covering 160 m of the view, closer ones sharper, each
  snapped to texels so edges don't shimmer; up to 4 spot lights cast shadows too (1024x1024). Filtered 3x3. Point
  lights don't cast shadows.
- **Sky**: a gradient from the horizon (also the fog colour) to the zenith, the ground below, and the sun's disc
  and glow. It also lights the scene: ambient light and reflections come from it (blurrier on rougher surfaces).
- **Transparency** (glTF alpha modes): cut-out materials (Mask) have holes wherever their texture's alpha is below a
  cutoff (the built-in Grate texture has them); see-through ones (Blend) are drawn after everything solid, sorted
  back to front, with their opacity. Shadows ignore transparency.
- **HDR**: the scene renders into a floating-point image; **bloom** (a 13-tap down / tent up mip chain, with a
  soft threshold) makes bright parts glow, and the final pass applies exposure and the Khronos **PBR Neutral**
  tonemapper (colours below about 0.8 come out as they are). Sky colour, fog, ambient, exposure and bloom are
  per-scene settings (Inspector, with nothing selected).
- 4x MSAA, distance fog, the selection outline (kept out of tonemapping), and a little dithering against banding.
- **Skeletal animation**: glTF skins are imported with their joint weights; skinned meshes are deformed on the GPU
  (joint matrices in a storage buffer, up to 4 joints per vertex) in every pass, shadows and outline included.
- **Batching**: objects outside the camera's (or, for the shadow pass, the sun's) frustum are culled; the rest are
  drawn instanced, one draw call per mesh + texture, with per-object data in a storage buffer.

## Animation

Importing a glTF model brings in its **skins** and **animation clips** (translation / rotation / scale, linear,
step or cubic spline; morph targets aren't supported). The model's root gets an **Animator** component, which plays
a clip on the entities under it, matched by name; skinned meshes get a **Skin** component that finds its joints the
same way. Clips are referenced by name ("tentacle/Bend") and re-imported from their files with the scene.

In play mode the Animator advances with the fixed update (before physics, so colliders on animated parts follow).
Game code switches clips with a cross-fade:

```cpp
if (self.animator) self.animator->play(running ? "Fox/Run" : "Fox/Survey", 0.3f);   // no-op if already playing
```

The sample game's tentacles (`examples/sample_game/models/tentacle.gltf`) curl, and stretch towards the player
when it comes close (its `Lurker` behaviour).

## Particles

A **Particle Emitter** component sends out camera-facing dots: a rate per second and/or bursts, lifetime, speed,
a cone around the entity's +Y, an emitting sphere, gravity, drag, size, colour and opacity over life, brightness
(above 1 they glow through the bloom), and **additive** (fire, sparks, magic) or see-through (smoke, dust)
blending. Particles stay where they were emitted and outlive their entity. The editor previews emitters outside
Play (View > Preview particles; Inspector > Preview burst). Game code bursts an entity's emitter or emits any look
anywhere:

```cpp
ctx.emitParticles(self.id);                        // its emitter's burst
ctx.emitParticles(sparkleLook, position, 24);      // a ParticleEmitter as a description, no component needed
```

Pickups burst their emitter when collected (the demo's gems sparkle), Destructibles throw dust, the centrepiece
gives off embers.

## Audio

Sound goes through [miniaudio](https://miniaud.io) (vendored v0.11.21). A clip is either one of the built-in
synthesised sounds (Pickup, Shot, Bounce, Thud, Break, Jump, Hum, Beep: no sound files needed) or the path of a
WAV / MP3 / FLAC file.

- **Audio Source** component: clip, volume, loop, 3D (fades with distance from the camera and pans) with a
  distance range, play on start, play on impact (when its rigid body hits something, louder for harder hits).
  Inspector > Preview plays it.
- Game code: `ctx.playSound("Shot", position)` / `ctx.playSound2D("Beep")`. The built-in behaviours make noise:
  pickups, shots, things breaking, jumping. In the demo the centrepiece hums, crates thud and balls bounce.
- The listener is the game's camera. Without an audio device everything just stays silent.

## Collision

Entities collide through their **Collider** component: a box, sphere, capsule or the entity's mesh, in local
space and scaled with the entity. Inspector > Add component > Collider fits one to the mesh; View > Show
colliders draws them all (blue = solid, green = trigger).

- **Solid** colliders block characters and rays. **Triggers** block nothing and instead tell the behaviours on
  both entities when something enters or leaves them (`onTriggerEnter` / `onTriggerExit`).
- Entities with a **Rigid Body** (and a solid collider) are simulated by [Jolt Physics](https://github.com/jrouwe/JoltPhysics)
  (v5.6, vendored in `third_party/JoltPhysics`, MIT): gravity, bounciness, friction, damping, mass, and rotation:
  they tumble, roll, stack and come to rest, get pushed by the player and knocked by projectiles. "Lock rotation"
  keeps one upright. Moving meshes collide as their convex hull. Every other solid collider is static, or
  kinematic once something moves it (spinning, bobbing, moved by behaviours), so it pushes the bodies it moves
  into. Game code changes a body's `velocity` / `angularVelocity` or moves its entity, and the simulation picks
  that up.
- Game code can `raycast`, `overlapSphere` and `moveCharacter` through `GameContext`. Characters (a capsule or
  sphere collider moved with `moveCharacter`) are Jolt `CharacterVirtual`s: they slide along walls, climb slopes
  up to 50 degrees and steps up to 40 cm, stick to the floor going down, ride moving platforms and push rigid
  bodies. Triggers are sensors: they block nothing.
- Everything (queries, triggers, characters, bodies) is one Jolt world, kept up to date with the scene each update.
- Mesh colliders test real triangles, so they're the most expensive: prefer boxes and spheres for big scenes.

## Prefabs

A **prefab** is a saved entity tree used as a template: build something in the scene (say a gem with its
collider and behaviours), then Outliner > **Prefabs** tab > **From selection** (or right-click > Create prefab).
Prefabs are saved in the scene file.

- **Add to scene** (or double-click) places a copy in front of the camera. Copies (*instances*) stay **linked**
  to their prefab: they show in blue in the outliner, and the Inspector lists what an instance changed (its
  *overrides*: position, colour, a behaviour field, a removed component...).
- **Apply** (Inspector, or Prefabs tab > Apply selection to prefab) makes the prefab match an instance; every other
  instance follows, **keeping its own overrides**. Entities added to or removed from the prefab are added to or
  removed from all instances. **Revert** drops an instance's overrides (it stays where it was placed);
  **Unlink** turns it into ordinary entities.
- **Export** writes `<name>.prefab` to the working directory and **Import...** loads one, to share between scenes.
- Game code creates copies while playing with `ctx.spawn("Bullet", position, forward)`. The copy joins the scene
  after the current update (so nothing moves under other behaviours' feet) and its behaviours start then; the
  returned id works with `ctx.find()` straight away.

The demo scene has two: **Bullet** (fired by the Shooter on the player's camera) and **Gem** (the gems are made
from it, and destroyed cubes drop one).

## Behaviours

Behaviours are C++ classes attached to entities as components (Inspector > Add component). Each has editable
**fields**, saved with the scene, and `start()` / `update()` callbacks that run in play mode. Built in:

| Behaviour        | What it does                                                                 |
|------------------|------------------------------------------------------------------------------|
| Spin             | Rotates around the parent's Y axis                                           |
| Bob              | Floats up and down around its starting height                                |
| Patrol           | Moves back and forth between its start and start + offset                    |
| LookAt           | Turns to face another entity (by name)                                       |
| PlayerController | First-person walking, mouse look, jumping; collides through its capsule collider |
| Pickup           | Collected when the collector touches its trigger collider; logs how many are left |
| Shooter          | Fires a prefab along its -Z while the fire action (left mouse) is held      |
| Projectile       | Flies forward, stops at the first solid collider and damages Destructibles   |
| Destructible     | Destroyed after enough hits, optionally leaving a prefab behind              |
| Spawner          | Spawns a prefab every few seconds (with a little random spread), up to a maximum alive at once |
| SceneExit        | Goes to another scene when the collector touches its trigger collider        |
| SaveLoadKeys     | Quicksave / quickload on the `quicksave` (F5) / `quickload` (F9) actions      |
| HudText          | Text over the game view; `{key}` shows a game state value ("Score: {score}")  |
| HudBar           | A bar showing a game state number out of a maximum                           |
| PauseMenu        | Pauses on the `pause` action (P / gamepad Start): Resume, Save, Load, Quit    |

Writing one (see `src/game/builtin_behaviours.cpp` for complete examples):

```cpp
struct Hover : BehaviourBase<Hover> {
    static constexpr const char* kTypeName = "Hover";
    float height = 1.f;                                    // a field: inspector + scene file
    float baseY = 0.f;                                     // runtime state: fresh on every Play

    void fields(FieldVisitor& v) override { v.field("height", height); }
    void start(GameContext&, Entity& self) override { baseY = self.transform.position[1]; }
    void update(GameContext& ctx, Entity& self) override {
        if (ctx.input().pressed("jump")) ctx.log(self.name + " jumped");
        self.transform.position[1] = baseY + height * std::sin(ctx.time());
    }
};

registerBehaviour<Hover>("Floats up and down");           // at startup, before loading scenes
```

`GameContext` gives access to the scene, input actions, time, entity lookup by name or id, world positions,
collision queries, deferred `spawn()` / `destroy()` and `log()`. Behaviours also get `onTriggerEnter` / `onTriggerExit`.

### Scene flow and save games

Game code moves between scenes and keeps progress through the `GameContext`:

```cpp
ctx.loadScene("scenes/level2.vkw");    // replaces the scene at the end of the update (path relative to the project)
ctx.state().add("score", 10.f);        // GameState: numbers, texts and flags that survive scene changes
ctx.saveGame("slot1");                 // the scene as it is, the game state and the clock
ctx.loadGame("slot1");
ctx.quit();                            // vkgame exits; in the editor, play mode stops
```

Save games go to the user's data folder (`~/.local/share/vkworld/<project>/saves`, `%APPDATA%\vkworld\...`,
`~/Library/Application Support/vkworld/...`; `VKWORLD_SAVE_DIR` overrides it). They hold the whole scene
(entities, behaviours' fields, rigid-body velocities) and each behaviour's **runtime state**: what a behaviour
builds up while playing goes in an optional `state(FieldVisitor&)` next to `fields()`, and is restored after
`start()` when a save is loaded. In the editor, Stop still restores the scene you were editing.

### Game UI

Behaviours draw the game's interface in `drawUI(GameContext&, Entity& self, GameUI& ui)`, called once per rendered
frame (also while paused, so a pause menu can resume). The canvas is 1080 units high and `ui.width()` wide, so a
layout looks the same at any resolution; it's drawn over the game view, under the editor's panels:

```cpp
void drawUI(GameContext& ctx, Entity&, GameUI& ui) override {
    ui.text(40, 30, ctx.state().format("Score {score}"), 48);
    ui.bar(40, 90, 400, 24, ctx.state().number("health") / 100.f, {0.3f, 0.9f, 0.4f, 1});
    ui.image("icons/key.png", ui.width() - 120, 30, 80, 80);   // a texture by name or file
    if (paused) {
        ui.showCursor();                                       // frees the mouse pointer for menus
        if (ui.button(ui.width() / 2 - 200, 500, 400, 72, "Resume")) ctx.setPaused(false);
    }
}
```

`ctx.input().pressedThisFrame("action")` reads actions per rendered frame for UI code. Hiding an entity hides
its UI. For HUDs without code there are the HudText, HudBar and PauseMenu behaviours.

### Game modules: a project's own code

A game's behaviours don't go into the engine: they live in the project, built as a **game module** (a shared
library) that the editor and `vkgame` load when they open the project. `examples/sample_game` is a complete
project with one (a coin hunt: `Coin`, `ScoreKeeper`, `Chaser`):

```sh
./vkworld ../examples/sample_game      # the editor, with the sample's behaviours in Add component
./vkgame ../examples/sample_game       # play it
```

The module's source registers its behaviours in one entry point:

```cpp
#include "game/module.h"
// ... behaviour classes as above ...
VKWORLD_GAME_MODULE() {
    registerBehaviour<Hover>("Floats up and down");
}
```

and the project's `CMakeLists.txt` builds it with `vkworld_add_game_module(name PROJECT_DIR <folder> SOURCES ...)`,
into `<folder>/bin`. Build projects outside this repository along with the engine by listing their folders in
`-DVKWORLD_GAME_PROJECTS="/path/to/game;..."`. `project.json` says where the library is and how to build it:

```json
"module": "bin/sample_game",
"moduleBuild": "cmake --build {engineBuild} --target sample_game"
```

**Hot reload**: the editor reloads the module when the library changes on disk (rebuilt from an IDE, say), or on
Game > Rebuild & reload game module (Ctrl+R), which runs `moduleBuild` first (`{engineBuild}` is the editor's
own build folder). The scene, prefabs and undo history keep their behaviours across the swap; while playing, the
reload waits for Stop. A behaviour whose code isn't loaded (no module, a failed build, a renamed class) is kept as
it was saved and shown as *code not loaded*; it does nothing when played, and comes back once the code does.

A module must be built with the same compiler and engine headers as the programs that load it (they share the
engine library: `libengine.so` / `engine.dll` / `libengine.dylib`).

## Controls

| Input                          | Action                                         |
|--------------------------------|------------------------------------------------|
| Left click                     | Select (empty space deselects)                 |
| Right mouse + WASD             | Fly: mouse look, E/Q up/down, Shift faster, wheel = speed |
| Middle mouse drag              | Orbit                                          |
| Shift + middle drag            | Pan                                            |
| Mouse wheel                    | Zoom                                           |
| Q / G / R / S                  | Select / Move / Rotate / Scale tool (W/E also work) |
| L                              | Toggle world/local gizmo axes                  |
| Ctrl while dragging gizmo      | Toggle snapping                                |
| F (or double-click in outliner)| Focus selected                                 |
| Shift+A                        | Add object menu                                |
| Shift+D                        | Duplicate                                      |
| X / Delete                     | Delete                                         |
| End                            | Drop selected to the ground                    |
| H / Alt+H                      | Hide selected / unhide all                     |
| Esc                            | Deselect                                       |
| Ctrl+P / Ctrl+Shift+P          | Play-stop / pause the game                     |
| Ctrl+R                         | Rebuild and reload the project's game module  |
| Ctrl+Z / Ctrl+Shift+Z (Ctrl+Y) | Undo / redo                                    |
| Ctrl+S / Ctrl+O                | Save / load the scene file (path in File menu, default `scene.vkw`) |
| F1                             | Controls help window                           |
| F11                            | Fullscreen                                     |
| Click / Esc while playing      | Capture / release the mouse for the game       |

## Code

The code is split into an **engine** shared library (everything a game needs at runtime, Dear ImGui included) and
the **editor** application (`vkworld`) built on top of it; games add their own code as modules (`examples/`).
Includes are relative to `src/`.

| Folder / file              | What                                                            |
|----------------------------|-----------------------------------------------------------------|
| `src/main.cpp`             | The editor's entry point                                        |
| `src/runtime/main.cpp`     | The standalone game (`vkgame`)                                  |
| `src/app/`                 | Window + renderer + ImGui setup and frame loop shared by both   |
| `src/core/`                | `vecmath.h` vector/matrix helpers, `json.*` JSON reader/writer, `png.*` screenshot writer, `project.*` project folder and paths |
| `src/platform/`            | OS layer: `platform.h` interface; XCB (Linux), Win32, Cocoa (macOS) implementations; ImGui XCB backend |
| `src/render/`              | Vulkan renderer: device, swapchain, pipelines, frame (`renderer.cpp`); textures and materials (`renderer_textures.cpp`); culling, batching, skinning data (`renderer_instances.cpp`); HDR, bloom, tonemapping (`renderer_post.cpp`); scene -> draw list (`scene_draw.*`) |
| `src/assets/`              | Mesh library (procedural meshes, OBJ import, picking), texture library (`textures.*`), glTF import (`gltf.*`) |
| `src/scene/`               | Entities, components and hierarchy (`scene.*`); behaviour base class and registry (`behaviour.*`); JSON save/load (`serialization.*`) |
| `src/audio/`               | Sound playback (miniaudio), built-in synthesised clips          |
| `src/physics/`             | `physics.*`: the scene's physics on Jolt Physics: rigid bodies, raycasts, overlaps, triggers, characters |
| `src/game/`                | Game runtime: fixed-timestep loop and GameContext (`game.*`), input actions (`input.*`), built-in behaviours, game module loading (`module.*`) |
| `src/editor/`              | Editor app: `editor.cpp` frame/play/operations/undo, `editor_panels.cpp` menus, toolbar, status bar and dialogs, `editor_outliner.cpp`, `editor_inspector.cpp`, `editor_viewport.cpp` navigation/gizmos/picking, `editor_assets.cpp` project, asset browser, game module and export |
| `tests/`                   | Engine unit tests (`test.h` is the tiny framework)              |
| `examples/sample_game/`    | A project with its own game module (`src/`), scene and `project.json` |
| `tools/`                   | `check_screenshot.py` (CI smoke test)                           |
| `bench/`                   | `engine_bench`: CPU timings on the stress scene (`scene/stress_scene.*`) |
| `shaders/common.glsl`      | Frame data (camera, sun, point lights, shadow camera) and per-object instance data shared by the shaders |
| `shaders/scene.*`          | Lit, textured, shadowed objects; checker ground; fog            |
| `shaders/shadow.vert`      | Shadow map (depth only)                                         |
| `shaders/post.vert`, `bloom_*.frag`, `composite.frag` | Bloom down / up passes; tonemapping and gamma onto the swapchain |
| `shaders/outline.*`        | Selection outline (screen-space extruded hull + stencil)        |
