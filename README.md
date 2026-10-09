# Vulkan World Editor

A small Vulkan 1.3 3D scene with a Blender-style editing UI, in C++17.

- Runs on **Linux** (X11 / XWayland, via raw XCB), **Windows** (Win32) and **macOS** (Cocoa + MoltenVK). No GLFW/SDL needed.
- UI: [Dear ImGui](https://github.com/ocornut/imgui) v1.92.9b + [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo),
  vendored in `third_party/` and compiled into the binary.

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

### Validation layer

Set `VKWORLD_VALIDATION=1` to enable the Khronos validation layer (on by default in Debug builds,
e.g. Visual Studio's default Debug configuration). Messages go to the console.

## Layout

- **Menu bar**: File (save/load scene, import OBJ, reset), Edit, Add, Game (play/pause/step), View.
- **Importing models**: File / Add > Import OBJ... loads a Wavefront `.obj` (triangles or polygons, with or without
  normals; materials/textures ignored). It's centred and scaled to fit a unit cube, then shows up in the Add menu and
  the Inspector's mesh list. Saved scenes reference the `.obj` by absolute path and re-import it on load.
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
| Mesh Renderer   | Draws a mesh (built-in or imported) with a colour / checker pattern  |
| Camera          | The game renders through the first entity that has one               |
| Animator        | Spin / bob motion while playing                                      |

An entity without components is an **empty**: a handle for grouping others (Add > Empty). Cameras and empties
show as icons in the viewport and can be clicked like meshes.

Scenes are saved as readable **JSON** (`scene.vkw`). Files from earlier versions (the line-based text format)
still load.

## Play mode

**Play** (Ctrl+P) copies the scene and runs the game on the copy; **Stop** throws the copy away and restores
the scene exactly as it was, so anything changed while playing is temporary (and not undoable).

- The game updates at a fixed **60 Hz**, independent of the frame rate (several updates per frame on a slow
  machine, none on some frames of a fast one; a long hitch is capped at 8 updates).
  **Pause** freezes it and **Step** advances one update.
- It renders through the first **camera object** in the scene (Add > Camera creates one at the current view;
  any entity can get a Camera component in the Inspector). Without one, the editor view is used.
- Game code reads **named input actions** (`move_forward`, `jump`, `look`, ...; see `src/game/input.cpp`), never
  raw keys. Key presses are latched until the next update reads them, so a quick tap is never missed. The keyboard
  is ignored while typing in a text field, and the mouse while it's over a panel.
- Game > Maximize viewport on play hides the side panels while playing.

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

## Code

The code is split into an **engine** static library (everything a game needs at runtime) and the **editor**
application (`vkworld`) built on top of it. Includes are relative to `src/`.

| Folder / file              | What                                                            |
|----------------------------|-----------------------------------------------------------------|
| `src/main.cpp`             | ImGui setup, main loop                                          |
| `src/core/`                | `vecmath.h` vector/matrix helpers, `json.*` JSON reader/writer  |
| `src/platform/`            | OS layer: `platform.h` interface; XCB (Linux), Win32, Cocoa (macOS) implementations; ImGui XCB backend |
| `src/render/`              | Vulkan renderer: swapchain, 4x MSAA, depth/stencil, scene + outline + UI passes |
| `src/assets/`              | Mesh library: procedural meshes, OBJ import, ray/triangle picking |
| `src/scene/`               | Entities, components and hierarchy (`scene.*`); JSON save/load (`serialization.*`) |
| `src/game/`                | Game runtime: fixed-timestep loop, play-mode camera, named input actions |
| `src/editor/`              | Editor app: `editor.cpp` frame/play/operations/undo, `editor_panels.cpp` UI panels, `editor_viewport.cpp` navigation/gizmos/picking |
| `shaders/scene.*`          | Lit objects, checker ground, fog                                |
| `shaders/outline.*`        | Selection outline (screen-space extruded hull + stencil)        |
