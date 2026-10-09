# Vulkan World Editor

A small Vulkan 1.3 3D scene with a Blender-style editing UI, in C++17.

- Window/input: raw XCB (works on X11 and XWayland). No GLFW/SDL needed.
- UI: [Dear ImGui](https://github.com/ocornut/imgui) v1.92.9b + [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo),
  vendored in `third_party/` and compiled into the binary.

## Build & run

```sh
cmake -S . -B build
cmake --build build -j
./build/vkworld
```

Requires the Vulkan SDK (`VULKAN_SDK` set, provides `glslc`) and libxcb.
Set `VKWORLD_VALIDATION=1` to enable the Khronos validation layer (on by default in Debug builds).

## Layout

- **Menu bar**: File (save/load scene, reset), Edit, Add, View.
- **Toolbar**: Select/Move/Rotate/Scale tools, world/local axes, snapping, add/duplicate/delete/focus, undo/redo,
  play/pause animation, fly speed.
- **Outliner** (left): every object, with visibility toggles, filter, right-click menu, double-click to focus.
- **Inspector** (right): name, mesh, position/rotation/scale, colour, flags, animation, camera and renderer stats.
- **Viewport**: selected object gets an orange outline (visible through other objects) plus the gizmo.
  The cube in the top-right corner snaps/rotates the view.

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
| Space                          | Play / pause animation                         |
| Ctrl+Z / Ctrl+Shift+Z (Ctrl+Y) | Undo / redo                                    |
| Ctrl+S / Ctrl+O                | Save / load the scene file (path in File menu, default `scene.vkw`) |
| F1                             | Controls help window                           |

## Code

| File                       | What                                                            |
|----------------------------|-----------------------------------------------------------------|
| `src/main.cpp`             | XCB window, ImGui setup, main loop                              |
| `src/editor.*`             | Camera navigation, picking, gizmos, undo/redo, all UI panels    |
| `src/renderer.*`           | Vulkan: swapchain, 4x MSAA, depth/stencil, scene + outline + UI passes |
| `src/scene.*`              | Scene objects, default scene, save/load text format             |
| `src/geometry.*`           | Procedural meshes and ray/triangle picking                      |
| `src/imgui_impl_xcb.*`     | Dear ImGui platform backend for XCB (keyboard map, text input, mouse) |
| `src/math.h`               | Small vector/matrix helpers                                     |
| `shaders/scene.*`          | Lit objects, checker ground, fog                                |
| `shaders/outline.*`        | Selection outline (screen-space extruded hull + stencil)        |
