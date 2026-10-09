# Vulkan World Editor

A small Vulkan 1.3 3D scene with a Blender-style editing UI, in C++17.

- Runs on **Linux** (X11 / XWayland, via raw XCB), **Windows** (Win32) and **macOS** (Cocoa + MoltenVK). No GLFW/SDL needed.
- UI: [Dear ImGui](https://github.com/ocornut/imgui) v1.92.9b + [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo);
  images: [stb_image](https://github.com/nothings/stb) v2.30; glTF: [cgltf](https://github.com/jkuhlmann/cgltf) v1.14.
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
```

The mouse is captured for the game (hidden, raw movement): Esc releases it, a click takes it back, F11 toggles
fullscreen. Ship a game by copying `vkgame`, the `shaders/` folder and your scene (plus any OBJ files it uses).

Both programs also take `--screenshot out.png [--frames N]`: render N frames (default 30), save the last one and
quit, which is handy for automated checks. The editor also takes a scene path and `--play`.

### Tests

The engine has unit tests (`tests/`, no external framework): JSON, OBJ import, scene hierarchy and
serialization, collision and character movement, prefabs and spawning, the game loop, input actions and the
built-in behaviours. They run headless, without a GPU.

```sh
make engine_tests && make test     # or: ctest, or ./engine_tests <name filter> for detailed output
```

Turn them off with `-DVKWORLD_BUILD_TESTS=OFF`.

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

## Scenes: entities and components

A scene is a flat list of **entities**. Each has a name, an id, an optional **parent** (transforms are relative to
it; hiding, deleting or duplicating a parent includes its children) and a **transform**, plus optional components:

| Component       | What it does                                                         |
|-----------------|----------------------------------------------------------------------|
| Mesh Renderer   | Draws a mesh (built-in or imported) with a colour and optional texture |
| Light           | Directional (the sun, with shadows) or point light                   |
| Camera          | The game renders through the first entity that has one               |
| Collider        | Box, sphere, capsule or mesh shape; solid or trigger (see Collision) |
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
break, and cubes leave a gem behind.

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

- **Materials**: a Mesh Renderer has a colour and optionally a texture (PNG, JPEG, TGA, BMP via Inspector > Load
  texture..., or the built-in procedural Checker / Bricks / Tiles), with a tiling factor. Built-in meshes have
  box-mapped texture coordinates; imported ones use their own. Textures get mipmaps and anisotropic filtering.
- **Lights**: Light components, directional (shines along the entity's -Z) or point (colour, intensity, range).
  The first directional light is the sun and casts the shadows; up to 8 point lights (nearest the camera) light
  each frame. Scenes without a directional light get a default sun.
- **Shadows**: one 2048x2048 shadow map for the sun, covering about 90 m around the camera, filtered 3x3, snapped
  to texels so edges don't shimmer.
- 4x MSAA, distance fog, the selection outline, and a little dithering against banding.

## Collision

Entities collide through their **Collider** component: a box, sphere, capsule or the entity's mesh, in local
space and scaled with the entity. Inspector > Add component > Collider fits one to the mesh; View > Show
colliders draws them all (blue = solid, green = trigger).

- **Solid** colliders block characters and rays. **Triggers** block nothing and instead tell the behaviours on
  both entities when something enters or leaves them (`onTriggerEnter` / `onTriggerExit`).
- Everything is kinematic for now: colliders go wherever their entities are (spinning, bobbing, moved by
  behaviours); there are no rigid-body dynamics.
- Game code can `raycast`, `overlapSphere` and `moveCharacter` (move a capsule, sliding along walls, landing on
  floors, sub-stepped so fast moves can't skip through thin walls) through `GameContext`.
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
| Spawner          | Spawns a prefab every few seconds, up to a maximum alive at once             |

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
| Ctrl+Z / Ctrl+Shift+Z (Ctrl+Y) | Undo / redo                                    |
| Ctrl+S / Ctrl+O                | Save / load the scene file (path in File menu, default `scene.vkw`) |
| F1                             | Controls help window                           |
| F11                            | Fullscreen                                     |
| Click / Esc while playing      | Capture / release the mouse for the game       |

## Code

The code is split into an **engine** static library (everything a game needs at runtime) and the **editor**
application (`vkworld`) built on top of it. Includes are relative to `src/`.

| Folder / file              | What                                                            |
|----------------------------|-----------------------------------------------------------------|
| `src/main.cpp`             | The editor's entry point                                        |
| `src/runtime/main.cpp`     | The standalone game (`vkgame`)                                  |
| `src/app/`                 | Window + renderer + ImGui setup and frame loop shared by both   |
| `src/core/`                | `vecmath.h` vector/matrix helpers, `json.*` JSON reader/writer, `png.*` screenshot writer |
| `src/platform/`            | OS layer: `platform.h` interface; XCB (Linux), Win32, Cocoa (macOS) implementations; ImGui XCB backend |
| `src/render/`              | Vulkan renderer: swapchain, 4x MSAA, depth/stencil, scene + outline + UI passes |
| `src/assets/`              | Mesh library (procedural meshes, OBJ import, picking), texture library (`textures.*`), glTF import (`gltf.*`) |
| `src/scene/`               | Entities, components and hierarchy (`scene.*`); behaviour base class and registry (`behaviour.*`); JSON save/load (`serialization.*`) |
| `src/physics/`             | Collision shapes and contact tests (`collision.*`), the per-update physics world: raycasts, overlaps, character movement, triggers (`physics_world.*`) |
| `src/game/`                | Game runtime: fixed-timestep loop and GameContext (`game.*`), input actions (`input.*`), built-in behaviours |
| `src/editor/`              | Editor app: `editor.cpp` frame/play/operations/undo, `editor_panels.cpp` UI panels, `editor_viewport.cpp` navigation/gizmos/picking |
| `tests/`                   | Engine unit tests (`test.h` is the tiny framework)              |
| `shaders/common.glsl`      | Frame data (camera, sun, point lights, shadow camera) shared by the shaders |
| `shaders/scene.*`          | Lit, textured, shadowed objects; checker ground; fog            |
| `shaders/shadow.vert`      | Shadow map (depth only)                                         |
| `shaders/outline.*`        | Selection outline (screen-space extruded hull + stencil)        |
