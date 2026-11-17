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
  `VK_ADD_LAYER_PATH` and `LD_LIBRARY_PATH` at it. CI's Ubuntu runner has 1.3.275, whose synchronisation checks
  catch hazards newer layers don't report: for changes to barriers, storage images or buffers, run the checks under
  that version too (`dpkg-deb -x` the noble package
  `vulkan-validationlayers_1.3.275.0-1_amd64.deb` into a scratch folder).
- Check rendering changes on Night Drive as well as the demo scene (`./build/vkgame examples/night_drive`: lamps,
  rain, fog, reflections, traffic), and with a moving camera: a still screenshot hides history (TAA, reflections,
  clouds) and caching bugs.
- Lavapipe takes about a second per Night Drive frame, so each frame is a long time step: the simulation runs at
  most 8 fixed steps (`kMaxStepsPerFrame`, about 0.13 s) and drops the rest, so `--frames 20` is only a few
  seconds of play, while anything animated by the frame's real time (rain, clouds) jumps far between frames.
- Throwaway scenes and tools for checks go in a scratch folder outside the repository, never in it.
- Native Wayland (Linux): run a headless compositor, `weston --backend=headless --renderer=pixman --socket=wl-test`
  (unpacked locally like the validation layer if need be), then `WAYLAND_DISPLAY=wl-test VKWORLD_PLATFORM=wayland
  ./build/vkgame --screenshot ...`. Without `VKWORLD_PLATFORM` a compositor that doesn't decorate windows (weston's
  kiosk shell, GNOME) gets XWayland when `DISPLAY` is set.

### Performance

`engine_bench --json out.json` times the CPU side of a frame on the stress scene. CI's `tools/check_perf.py`
compares it with `tools/perf_baseline.json` per platform (fails past 2x + 1 ms). After a change meant to change
performance, update the baseline from the numbers CI prints.

The GPU's side: `tools/check_gpu_bench.py --game build/vkgame` (under `xvfb-run` here: lavapipe) times each pass
with `vkgame --benchmark` on Night Drive and the stress scene, against `tools/gpu_baselines.json` for that GPU;
`--write` records a machine's numbers (lavapipe's are in; real GPUs' are added from the machines that have them).

- `vkgame --benchmark [frames]` prints each pass's median GPU time. It turns the camera a full circle, so anything
  cached while the camera is still (the sun's cascades, task 153) never shows: `VKWORLD_BENCHMARK_STILL=1` holds it
  still. Lavapipe's numbers move by 10-20 ms between runs: compare a change with its baseline in the same session,
  not with numbers in the roadmap.
- To find what a pass spends its time on, measure before optimising (a guess was wrong more than once): switch
  one part of the shader off at a time in a scratch copy, compile it with `glslc` over the build's `.spv` (e.g.
  `build/shaders/scene_early.frag.spv`, the lit pass's shader), benchmark, then rebuild to put the real one back.
- On lavapipe a full-screen pass costs tens of milliseconds however little it does: prefer folding work into a pass
  that already runs, or running it at half or quarter size.
- A new pass's timestamp needs room in `kMaxTimestamps` (`src/render/renderer.h`).

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
  `shaders/common.glsl` exactly. The frame set's bindings are listed in `frameBindings` in `src/render/renderer.cpp`
  (an array sized by hand: grow it with a new binding).
- **Textures read as data** (normal maps, metallic-roughness, masks) are filtered and mipmapped as numbers, not as
  sRGB colour: a new material or decal slot that reads one must mark it (`Renderer::markData`, as
  `Renderer::materialSet` does for the normal, metal-roughness, height and detail slots).
- **Generated examples**: Night Drive's scene and models are written by scripts in `examples/night_drive/tools/`
  (`make_city.py` → `scenes/city.vkw`, `make_car.py` → `models/car/*.glb`, `make_window_map.py`). Change the script
  and run it again rather than editing the outputs; give a new random choice its own generator (`SEED + n`) so
  the rest of the city stays as it was. The Night Drive tests (`tests/test_project.cpp`) load the result.
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
   on the other platforms. Let a run finish before pushing again: a new push cancels the one in progress, and its
   commit is then never checked on its own.

### Known flaky checks

- macOS runners: the loopback network and relay tests can time out under load (they sleep 1 ms per poll since
  0bfc056), and `engine_bench`'s `fixed_update_avg` varies a lot (its baseline was raised to 3.0 ms). Rerun once
  before chasing a failure in these; raise a baseline only after repeated failures with no related change.
- A rare one-off failure of the full suite locally has passed on rerun; if it comes back, find which test it is
  (`./build/engine_tests <substring>`) rather than rerunning until it passes.

## Open follow-ups

The work is planned in rounds in `docs/ROADMAP.md` (rounds 1-16 done, tasks 1-158). The maintainer chooses each
new round's direction: don't plan or start one unasked. Each finished task's notes end with "Not done: ..."; the
larger ones still open:

- **Rendering cost** (Night Drive on lavapipe: a frame about 1.2 s, the opaque pass about 450-475 ms): tighter light
  clusters (cones for spots), the sun's shadow taps scaled by quality, scrolling cascades for a moving camera (the
  still-caster cache only helps a still one), the shadow atlas's moving lamps cached like the cascades.
- **Reflections and sky**: temporal filtering of rough reflections, roughness-sized cone traces, the sky's
  reflections and the fog's ambient dimmed under clouds as the irradiance is.
- **Night Drive**: wheels turning with the traffic's speed, a bus, doors and mirrors, curved neon letters, rain on
  the camera's lens and wipers (rain drops on glass only show close up, not from the game's camera).
- **Platforms**: CI runs the programs for real only on Linux (lavapipe); Windows and macOS are built and unit-tested
  only, so platform rendering is checked on the maintainer's machines (GTX 1050 Ti, Apple M4, Intel).

## Releases

The version lives in `CMakeLists.txt` (`project(VulkanWorld VERSION x.y.z)`; `core/version.h` reads it, with the
git commit). A release is a tag `v<that version>` pushed to GitHub: `.github/workflows/release.yml` builds, tests,
splits symbols off and packages on every platform, then publishes. Bump the version in its own commit first, and
only tag when asked to (a tag publishes a release). `Actions > Release > Run workflow` builds the packages without
publishing, to check the workflow.
