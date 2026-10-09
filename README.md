# Vulkan World Editor

A small Vulkan 1.3 3D scene with a Blender-style editing UI, in C++17.

- Runs on **Linux** (X11 / XWayland, via raw XCB) and **Windows** (Win32). No GLFW/SDL needed.
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

### Validation layer

Set `VKWORLD_VALIDATION=1` to enable the Khronos validation layer (on by default in Debug builds,
e.g. Visual Studio's default Debug configuration). Messages go to the console.

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
| `src/main.cpp`             | ImGui setup, main loop                                          |
| `src/platform.h`           | OS layer interface: window, input, Vulkan surface, paths        |
| `src/platform_xcb.cpp`     | Linux implementation (XCB)                                      |
| `src/platform_win32.cpp`   | Windows implementation (Win32 + ImGui's Win32 backend)          |
| `src/editor.*`             | Camera navigation, picking, gizmos, undo/redo, all UI panels    |
| `src/renderer.*`           | Vulkan: swapchain, 4x MSAA, depth/stencil, scene + outline + UI passes |
| `src/scene.*`              | Scene objects, default scene, save/load text format             |
| `src/geometry.*`           | Procedural meshes and ray/triangle picking                      |
| `src/imgui_impl_xcb.*`     | Dear ImGui platform backend for XCB (Linux only)                |
| `src/vecmath.h`            | Small vector/matrix helpers                                     |
| `shaders/scene.*`          | Lit objects, checker ground, fog                                |
| `shaders/outline.*`        | Selection outline (screen-space extruded hull + stencil)        |
