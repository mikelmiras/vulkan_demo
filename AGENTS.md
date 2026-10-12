# Working on this repository

Instructions for coding agents (and people) changing this engine. `README.md` describes what the engine does;
`docs/ROADMAP.md` lists the planned work, what's done, and notes on how each finished task was built. Read the
roadmap's notes for the area you're about to touch: they record design decisions and known limits ("Not done: ...").

## Building

C++17, CMake, Vulkan 1.3 (dynamic rendering). Shaders are GLSL compiled by `glslc` at build time.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j1          # see the note on memory below
./build/engine_tests             # all tests; ./build/engine_tests <substring> runs the matching ones
```

- **Memory**: some translation units (Jolt, `physics.cpp`, the renderer, Dear ImGui) are heavy. On small machines
  (a few GB of RAM) build with `-j1` or `-j2`; a high `-j` can run the machine out of memory. Long builds are
  best run in the background with output to a log file, then grepped for `error`.
- Targets: `engine` (shared library), `vkworld` (the editor), `vkgame` (the game runtime), `engine_tests`,
  `engine_bench`, and the sample game's module (`examples/sample_game`; `VKWORLD_BUILD_EXAMPLES`, on by default).
  The module tests load the sample's module: rebuild everything (not just `engine_tests`) after changing engine
  headers.
- New source files go into the lists in `CMakeLists.txt` (`ENGINE_SOURCES`, the `engine_tests` list).
  New shaders go into `SHADER_SOURCES`; a new shared include (`*.glsl`) must also be added to the shaders'
  `DEPENDS`, or edits to it won't recompile them.

## Testing

- Every change comes with tests in `tests/` (framework: `tests/test.h`: `TEST(name)`, `CHECK`, `CHECK_NEAR`,
  `REQUIRE`). Tests run headless, without a GPU: keep logic that can be tested (scattering, clustering,
  allocation, encoding, simulation) in plain functions apart from Vulkan code, as `render/light_clusters.*` and
  `assets/mesh_lod.*` do.
- Tests needing input use `test::ImGuiContextGuard`; audio tests use `AudioEngine::init(false)` (no device) and
  read the mix with `readFrames`. Files go under `test::tempPath(...)`. The runner points `VKWORLD_SAVE_DIR` at a
  temporary folder: never read or write the real user's data folder from a test.
- Run the full suite before committing; commit only when it passes.

### Rendering checks (no GPU needed)

Rendering changes are checked for real on a software driver (Mesa's lavapipe) under a virtual X server, with the
Khronos validation layer, synchronisation checks included (CI does the same on Linux):

```sh
VKWORLD_VALIDATION=1 VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true \
  xvfb-run -a -s '-screen 0 1280x720x24' ./build/vkgame --screenshot shot.png --frames 20
python3 tools/check_screenshot.py shot.png
```

- `--screenshot` exits with 1 if validation reported anything: treat any validation message as a bug.
- Try the paths a change can affect: `VKWORLD_AA=msaa|taa|off`, `VKWORLD_GPU_CULLING=0`, `VKWORLD_OCCLUSION=0`,
  the editor (`./build/vkworld --stress --screenshot ...` shows stats in the status bar), the sample game.
- Look at the screenshots; comparing two of them pixel by pixel (e.g. a feature on and off) is a good way to show a
  change doesn't alter the picture.
- If the validation layer isn't installed system-wide, unpack `vulkan-validationlayers` locally and point
  `VK_ADD_LAYER_PATH` and `LD_LIBRARY_PATH` at it.
- Throwaway scenes and tools for checks go in a scratch folder outside the repository, never in it.
- Native Wayland (Linux): run a headless compositor, `weston --backend=headless --renderer=pixman --socket=wl-test`
  (unpacked locally like the validation layer if need be), then `WAYLAND_DISPLAY=wl-test VKWORLD_PLATFORM=wayland
  ./build/vkgame --screenshot ...`. Without `VKWORLD_PLATFORM` a compositor that doesn't decorate windows (weston's
  kiosk shell, GNOME) gets XWayland when `DISPLAY` is set.

### Performance

`engine_bench --json out.json` times the CPU side of a frame on the stress scene. CI's `tools/check_perf.py`
compares it with `tools/perf_baseline.json` per platform (fails past 2x + 1 ms). After a change meant to change
performance, update the baseline from the numbers CI prints.

## Conventions

- **Code style**: match the surrounding code: 4-space indentation, `camelCase` functions and members, trailing
  underscore for private members, `k` prefix for constants. Comments explain why and how (short sentences,
  British spelling), at the density of the code around them. Each file starts with a comment on what it's for.
- **Includes** are relative to `src/`. Third-party code lives in `third_party/` (vendored, with versions in the
  README); don't modify it.
- **Scene file format**: anything saved in scenes bumps `kVersion` in `src/scene/serialization.cpp` (with a note in
  its comment list), loads older files with sensible defaults, and is covered by a save / load test. Old scene
  fixtures in `tests/assets/formats` must keep loading.
- **Components** (`src/scene/scene.h`): a new field needs serialization, `eq()` in `src/scene/scene_diff.cpp` (or
  undo misses edits), the editor's inspector, and runtime-only fields marked as such (not saved).
- **Module API**: game modules are compiled against engine headers. Changing the layout of `Behaviour`,
  `GameContext` or any scene type (components, `Mesh`, ...) means bumping `VKWORLD_MODULE_API_VERSION` in
  `src/game/module.h`.
- **Renderer**: per-frame resources are indexed by `frame_` (`MAX_FRAMES` in flight) and may only be rewritten after
  that frame's fence; a descriptor set in use by a pending frame must not be updated. Every new GPU access needs its
  barrier (sync validation will tell you). Uniform layouts in `src/render/renderer_internal.h` must match
  `shaders/common.glsl` exactly.
- **Cross-platform**: Linux, Windows (MSVC) and macOS (Clang) all build in CI. Guard OS-specific code with
  `#if defined(_WIN32)` / `__APPLE__`, and watch for headers MSVC doesn't pull in implicitly.
- **Environment switches** for features are named `VKWORLD_<FEATURE>` and documented in the README.

## Finishing a change

1. Tests pass; rendering changes checked as above.
2. Documentation: the README section for the feature (what it does, how to use it, its limits), and for a
   roadmap task: mark it `(done)` in `docs/ROADMAP.md` and add its entry to the round's notes (how it works, what
   was checked, "Not done: ...").
3. One commit per task, with a message saying what changed and why.
4. After pushing, check CI (`gh run list -b main -L 1`, `gh run watch <id> --exit-status`) and fix whatever breaks
   on the other platforms.
