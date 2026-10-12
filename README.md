# Vulkan World Editor

A small Vulkan 1.3 3D scene with a Blender-style editing UI, in C++17.

- Runs on **Linux** (X11 / XWayland, via raw XCB), **Windows** (Win32) and **macOS** (Cocoa + MoltenVK). No GLFW/SDL needed.
- Scripting: [Lua](https://www.lua.org) 5.4.7 (`third_party/lua`, MIT)
- UI: [Dear ImGui](https://github.com/ocornut/imgui) v1.92.9b (docking branch) + [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo);
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
warning (the game won't have them). Packs are compressed (an LZ4-style codec, file by file, where it pays: scenes,
models and shaders shrink, images and sounds are stored as they are).

With an `"icon"` (a PNG) in `project.json`, the game shows it on its window; Windows exports get it as the
executable's icon, macOS ones in the `.app` bundle, Linux ones in a `.desktop` entry. Players need a Vulkan driver
(and X11 / XWayland on Linux); everything else is inside. An export is for the platform it's made on: export on
each OS you ship for.

`"Sample Game" --check` loads an exported game (pack, project, game code, start scene) without opening a window
and reports what it found: CI runs it on every export, on Linux, Windows and macOS.

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

`engine_bench --json out.json` also writes the times by name. CI runs `tools/check_perf.py`, which takes the best
of three runs and fails when a time is more than twice its baseline (plus 1 ms) in `tools/perf_baseline.json`,
kept per platform. After a change meant to make something slower (or faster), update the baseline with the
numbers CI printed, or `check_perf.py --bench build/engine_bench --platform Linux --write` on that platform.

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
- **Docking**: the Outliner, Inspector, Assets, Profiler and other windows dock around the viewport. Drag a
  panel by its tab to move it to another side, stack it with another panel as a tab, or pull it out as a
  floating window; drag the edges between panels to resize them. Close a panel from its tab, and bring it back
  from the View menu. View > Reset layout puts everything back where it started.
- **Saved per user**: the panel layout (`editor_layout.ini`) and the editor's preferences (`editor_prefs.json`:
  tool, snapping, which panels and overlays are shown, fly speed, field of view, play options and recent
  projects) are kept in the user's data folder next to `editor.log` (`~/.local/share/vkworld/Editor` on Linux,
  `%APPDATA%\vkworld\Editor` on Windows, `~/Library/Application Support/vkworld/Editor` on macOS). File > Recent
  projects reopens a project. Screenshot runs (`--screenshot`) always use the defaults and change nothing.

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

Images in the browser show as thumbnails; other kinds of file get an icon.

**The asset database**: every asset has an id, kept next to it in a small `<file>.meta` file (made by the editor;
commit them with the project). Scenes and prefabs save the id along with each file they use (models, textures,
animations, sounds, scene and sound fields of behaviours), so a file moved or renamed together with its `.meta`
(in a file manager, or `git mv` both) is found again when the scene loads. Moving inside the editor does more:
right-click an asset (or use the Inspector's buttons) to **rename or move** it (type a path to move it), drag it onto
a folder (or onto Up) to move it, or **delete** it; the `.meta` goes along, and the references in the open scene, the
project's other scenes and prefabs and `project.json` are updated. Renaming a script renames its behaviour type in
the scenes too; renaming a prefab (Outliner > Prefabs) updates the behaviour fields that name it.

Clicking an asset shows it in the **Inspector**: its path, size and id, which entities of the open scene use it,
and its **import settings** (saved in its `.meta`; Apply reloads it in place, on screen right away):

- Textures: *Max size* (larger images are scaled down when loaded), *Filter* (smooth, or blocky for pixel art),
  *Mipmaps* (off for UI images).
- Models: *Scale*; for OBJ, *Fit to a unit cube* (on: centred with its largest side 1, as before; off: as modelled).
  A glTF model's scale goes on its prefab's root when it's imported.
- Sounds: *Volume* (a gain on every play of the file) and *Stream by default* (Audio Sources made from it decode it
  as it plays).

Selecting several objects: Ctrl+click in the viewport or the outliner (Shift+click in the outliner selects a
range, Ctrl+A every top-level object); the gizmo then moves, turns or scales them together around the first one,
and Delete / Duplicate apply to all of them.

## Scenes: entities and components

A scene is a flat list of **entities**. Each has a name, an id, an optional **parent** (transforms are relative to
it; hiding, deleting or duplicating a parent includes its children) and a **transform**, plus optional components:

| Component       | What it does                                                         |
|-----------------|----------------------------------------------------------------------|
| Mesh Renderer   | Draws a mesh (built-in or imported) with a colour and optional texture |
| Light           | Directional (the sun), point or spot light, with shadows; spots can have a cookie |
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
  spot (a cone along -Z, with a soft edge). The first directional light is the sun; up to 1024 point and spot lights
  (nearest the camera) light each frame, through **clustered** lighting: the view is cut into 16 x 9 tiles and 24
  depth slices, each listing the lights that reach it, so a pixel only shades with the lights near it (hundreds of
  small lights cost little). A spot light can have a **cookie**: an image it shines through (a window frame,
  leaves), its brightness masking the light. Scenes without a directional light get a default sun.
- **Shadows**: the sun's are 4 cascades (2048x2048 each) covering 160 m of the view, closer ones sharper, each
  snapped to texels so edges don't shimmer. Spot and point lights cast shadows into a **shadow atlas** (one 4096 x
  4096 image cut into tiles): those in view nearest the camera get the biggest tiles (1024, then 512, then 256; a
  point light's six cube faces half that), up to 48 views a frame. Filtered 3x3. Cut-outs cast shadows with their holes; see-through
  materials cast one only where they're mostly opaque (glass: none).
- **Ambient occlusion**: corners, creases and the ground under things darken a little (screen-space, from the
  depth, at half resolution; strength and radius in the scene's look settings).
- **Sky**: a gradient from the horizon (also the fog colour) to the zenith, the ground below, and the sun's disc
  and glow. It also lights the scene: ambient light and reflections come from it (blurrier on rougher surfaces).
- **Sky images**: a scene can use a panorama instead (Scene look > Sky image: an equirectangular image, twice as
  wide as high, ideally an `.hdr`; with a brightness and a turn). It's drawn as the sky, and it lights the scene:
  matte surfaces get its light from all around (spherical harmonics), shiny ones reflect it, sharp on smooth
  surfaces and blurred like their highlights on rough ones (six levels prefiltered with the GGX lobe when it's
  loaded). The sun is still the scene's directional light; the fog keeps the horizon colour.
- **Reflection probes** (Add > Reflection probe): a box where shiny things reflect their real surroundings instead
  of the sky. The probe renders the scene from its centre in all six directions (when it's first seen, moved or
  resized, or on Bake in its Inspector), and that picture is prefiltered like a sky image; surfaces inside the box
  reflect it (lined up with the walls of a box-shaped room with *Box projection*) and get their ambient light from
  it, blending into the sky over the last metre inside its edges. The four probes nearest the camera are used.
- **Transparency** (glTF alpha modes): cut-out materials (Mask) have holes wherever their texture's alpha is below a
  cutoff (the built-in Grate texture has them); see-through ones (Blend) are drawn after everything solid, sorted
  back to front, with their opacity.
- **HDR**: the scene renders into a floating-point image; **bloom** (a 13-tap down / tent up mip chain, with a
  soft threshold) makes bright parts glow, and the final pass applies exposure and the Khronos **PBR Neutral**
  tonemapper (colours below about 0.8 come out as they are). Sky colour, fog, ambient, exposure and bloom are
  per-scene settings (Inspector, with nothing selected).
- **Anti-aliasing**: 4x MSAA by default; or **temporal** (each frame drawn with a different sub-pixel offset and
  blended with the frames before, found where they were through the depth and clamped to the colours around each
  pixel so moving things don't smear): it also smooths shading and thin lines. View > Anti-aliasing in the editor
  (remembered), `vkgame --aa msaa|taa|off`, or `VKWORLD_AA=taa` for either.
- Distance fog, the selection outline (kept out of tonemapping), and a little dithering against banding.
- **Skeletal animation**: glTF skins are imported with their joint weights; skinned meshes are deformed on the GPU
  (joint matrices in a storage buffer, up to 4 joints per vertex) in every pass, shadows and outline included.
- **Batching and culling**: objects are drawn instanced, one draw call per mesh + material. Each object's data
  (transform, material numbers) stays in GPU memory between frames; only what changed is copied up each frame (a
  still scene uploads nothing). The main pass's solid objects are culled on the GPU: a compute shader tests each
  against the view frustum and counts the visible ones into each batch's indirect draw (`VKWORLD_GPU_CULLING=0`
  does it on the CPU instead); see-through ones (sorted) and the shadow passes are culled on the CPU.
- **Occlusion culling**: after each frame a depth pyramid is made from what it drew; the next frame's culling also
  drops objects hidden behind it (seen through that frame's camera). Something coming out from behind a wall may
  show up a frame late. `VKWORLD_OCCLUSION=0` turns it off.
- **Levels of detail**: imported meshes of 1500 triangles or more get two simpler versions (about a quarter and a
  sixteenth of the triangles, by vertex clustering that keeps texture seams and sharp edges apart); objects small
  on screen draw those, chosen on the GPU (or the CPU, without GPU culling). Shadows use the view's level.

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

Or give the Animator a **state machine** in the inspector: states play clips (with their own speed and looping),
and transitions move between them, from one state or from any, when a parameter is greater or less than a value,
is true or false, or when a clip that doesn't loop has ended, cross-fading for a set time. Game code only sets the
parameters:

```cpp
self.animator->setParameter("speed", length(velocity));
self.animator->setParameter("jump", jumpPressed);
```

A state can be a **blend tree** instead of one clip: points along a parameter (walk at 0, run at 3), mixing the
two either side of its value and keeping them in step. Tick *2D* for a **blend space** over two parameters
(strafing: sideways and forward speed): each clip sits at a point, and the parameters' values mix the clips around
them (gradient band interpolation: a clip plays alone on its point); the inspector plots the points and, while
playing, where the parameters are. **Layers** play other clips over the main one on part
of the skeleton (a mask bone and everything under it), by weight: `animator->playLayer("Upper", "Wave", 0.8f)`.
An *additive* layer adds its clip's change from its first frame on top of whatever pose is there (breathing, a
flinch) instead of replacing it.

**IK** (inverse kinematics) runs after the animation, in the Animator's *IK* list: a **two-bone** chain (pick its
end bone, a hand or a foot: it, its parent and grandparent bend, the middle joint towards a direction you give)
reaches for a point, and **look-at** turns a bone's axis towards one (as far as a maximum angle). The target is an
entity by name, or a point game code sets each update with a weight to fade it in and out:
`self.animator->setIK("LeftFoot", groundHit.point, 1.f)` (feet on slopes), `setIK("Head", enemyPos, 0.7f)`.
With **root motion** on, the root bone's travel across the ground moves the entity (as a character when it has a
capsule) and the bone stays put, so feet don't slide. *Open graph* shows the state machine as a diagram, the
playing state lit while the game runs.

**Animation events** are named times in a clip (a footstep, the frame a sword hits): when playback passes one,
the entity's behaviours get `onAnimationEvent(ctx, self, "Footstep")`. In a blend tree the clip that weighs most
fires its events (at the same point of its cycle), and layers' clips fire theirs.

Outside Play, the Animator inspector previews its clip: play / pause, and a slider to scrub to any time (only
what's drawn changes, not the scene). New events are added at the preview time.

Rendering is **interpolated** between the last two fixed updates (60 Hz), so motion stays smooth at any frame
rate: `Game::worldMatrices()` and the game camera blend by how far the frame is into the next update.

The sample game's tentacles (`examples/sample_game/models/tentacle.gltf`) curl, and stretch towards the player
when it comes close (its `Lurker` behaviour).

## Terrain

Add > Terrain makes a **terrain**: a ground of hills and valleys, 64 m across by default, centred on its entity
(which can be moved, turned and scaled). Select it and pick the **Terrain** tool (toolbar, or T) to work on it with
a brush (its circle follows the ground under the mouse):

- **Raise** / **Lower** (Shift swaps them), **Smooth**, **Flatten** (to the height where the stroke starts, or one
  typed in), and **Paint** a layer. Each stroke is one undo step.
- Brush radius (metres) and strength are in the Inspector, with the terrain's *Detail* (64, 128 or 256 height
  samples along each side; changing it keeps the shape), its *Width*, *Add hills* (random rolling hills) and
  *Flatten all*.
- **Layers**: four, each a texture (or none) times a tint, repeating every so many metres; the terrain starts as
  grass, with rock, dirt and sand to paint. Drop a texture from the Assets panel onto a layer.
- **Holes** cut the ground away (cave mouths, tunnels; Shift, or **Fill**, puts it back): the triangles touching a
  hole point go, from the picture and from the collision alike, and rays pass through. A hole still casts a shadow.
- **Import heightmap...** takes a greyscale image (16-bit PNGs keep the most detail) as the heights, black at 0 and
  white at the height given, stretched over the whole terrain.
- **Foliage**: up to four layers of plants, each a mesh (the built-in *Grass Blade* clump, or any mesh: rocks,
  bushes), a colour, a density (per square metre where fully painted), a scale range, how much it sways in the
  wind and how far away it fades out. Paint where they grow with the **Foliage** brush (Shift or *Erase* thins
  them). They're scattered the same way every time (from the painting, never in holes), drawn instanced and lit
  like everything else; they don't cast shadows.

Terrains are drawn in chunks of 32 x 32 cells, each culled on its own and drawn with fewer triangles (every second
or fourth point) when it's far away; skirts hanging under chunk edges hide the seams between levels (chunks with
holes keep full detail). A terrain collides as a height field (characters walk on it, things land on it, rays hit it; any Collider on the
same entity is ignored), navigation bakes over it, and it's saved in the scene (compressed: a flat terrain is a few
hundred bytes).

## Navigation

Agents find their way around walls and up stairs on a **walkable grid** baked from the scene's static level
geometry: the solid colliders of entities without a rigid body or behaviours. Every column of the grid is probed
for surfaces flat enough to stand on with enough headroom, so floors can stack (a bridge and the ground under it).
Neighbouring cells link when the step between them is low enough, and cells closer than the agent's radius to a
wall or an edge are dropped. The scene's **Navigation** settings (Inspector, with nothing selected) set the cell
size and the agent's radius, height, climb and steepest slope; **Bake and show** draws the grid in the viewport.

The game bakes it the first time something asks for a path (22 ms for the sample game, 1.3 s for the 600 m stress
scene on two cores). Paths are A* over the cells, straightened so they only turn where they must:

```cpp
std::vector<Vec3> path;   // corner points on the ground
if (ctx.findPath(ctx.worldPosition(self.id), ctx.worldPosition(target), path)) { /* walk to path[1], ... */ }
```

The built-in **NavAgent** behaviour walks to an entity that way (repathing as it moves), keeps its distance from
other agents, and hops across **Navigation Links**: components that join two spots the grid doesn't (a jump over
a gap, a ladder, a drop; one way or both), drawn as arcs over the grid. The sample game's drones chase the player
around the blocks. Rigid bodies (and entities tagged `NavObstacle`) block the cells they stand on while the game
runs, so paths go round crates.

Scenes with NavAgents or links bake on a worker thread as they start. For big levels, **Bake and save** writes
the bake next to the scene (`level.vkw.nav`, exported with the game): it's loaded instead of baking for as long as
the level's static colliders, links and settings stay the same.

## AI

An **AI Brain** component (Add component > AI Brain) gives an entity a behaviour tree, a blackboard and
perception. *Start a guard tree* in its Inspector makes one that chases what it sees, goes to look where it heard
something, and otherwise patrols (set its waypoints: entity names, comma separated).

- **The tree** is edited in the Inspector (add, delete, reorder nodes; each node's parameters are labelled with what
  they mean). Every update it's run from the root and each node answers success, failure or running. Composites:
  **Selector** (the first child that doesn't fail; earlier ones are checked again every update, so a
  higher-priority branch takes over at once), **Sequence** (children in order while they succeed; its conditions are
  checked again while an action runs), **Parallel**, **Random**. Decorators: **Inverter**, **Succeeder**,
  **Repeat**, **UntilFail**, **Cooldown**, **Timeout**. Conditions: **Has**, **Compare** (a blackboard value),
  **CanSee**, **Heard**, **Near**, **Chance**. Actions: **MoveTo** and **Flee** (walking over the scene's
  navigation), **Patrol**, **Wait**, **LookAt**, **Set** / **Clear** (blackboard values), **Message**, **Sound**,
  **Animate** (an animator parameter), **Log**. While playing, the Inspector colours each node by its last answer.
- **Targets**: a node's target names a blackboard value holding an entity's name or a position, or else is an
  entity's name itself.
- **Perception**: entities with the tag it notices (Player by default) are seen within its sight range and angle
  (from its eyes, along -Z; the cone is drawn when it's selected) unless something solid is in the way, and heard
  within its hearing range when they play a sound (`ctx.playSound`) or make a noise (`ctx.makeNoise(position,
  loudness)`). It writes `seeing`, `target`, `targetPosition`, `lastSeen`, `noise` and `heardTime` to the
  blackboard, and forgets a target out of sight after its memory time.
- **The blackboard** starts with the values set in the Inspector; game code reads and writes it with
  `ctx.blackboard(id)`, Lua with `e:blackboard(key)` / `e:setBlackboard(key, value)`.

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
- **Mixer**: every sound plays on a bus (effects, music, UI, ambience) with its own volume
  (`ctx.setBusVolume("music", 0.5f)`); `ctx.duck("music", 0.3f, 2.f)` lowers one for a while, easing back.
- **Music** streams (decoded as it plays, not all up front) and loops on the music bus, cross-fading from one
  track to the next: `ctx.playMusic("music/theme.ogg", 2.f)`, or the built-in **MusicPlayer** behaviour. Audio
  Sources can stream too (long ambience).
- **Reverb Zone** component: while the camera is inside one, effects and ambience echo (wet, room size, damping),
  easing in at its edge.
- **Doppler**: moving 3D sounds (and a moving camera) shift in pitch.
- **Occlusion**: a solid collider between the camera and a 3D sound muffles it (fewer highs, quieter), easing as
  things move; the Audio Source's *Occlusion* says how much (0: heard through walls).
- **Headphones**: binaural panning for 3D sounds, from a spherical-head model rather than measured HRTFs: the far
  ear hears a sound up to 0.66 ms later and muffled by the head, and sounds behind are a little duller. It tells
  left from right and front from back on headphones; on speakers (the default) sounds pan with equal power.
- **Layered music and stingers**: `ctx.playMusicLayers({"music/base.ogg", "music/drums.ogg"}, {1.f, 0.f}, 1.f,
  120.f)` starts stems of the same length together; `ctx.setMusicLayer(1, 1.f, 2.f)` fades the drums in (a fight
  starting). `ctx.playStinger("music/hit.ogg", "beat")` plays a short phrase on the music's next beat (or "bar", or
  "now"), with the music dipping under it. MusicPlayer has `layers` (comma-separated), `layerVolume`, `bpm` and
  `beatsPerBar`; Lua has `game.playMusicLayers`, `game.setMusicLayer` and `game.playStinger`.
- **Player settings**: master, music, effects, UI and ambience volumes and headphones live in `settings.json` in
  the game's save folder, read when the game starts and saved a second after a change. They appear in the game
  state as `settings.masterVolume`, `settings.musicVolume`, ..., `settings.headphones`, so a menu's sliders and
  toggles (UI elements bound to those values) or a script change them, and the game applies them (on top of its
  own bus volumes). The sample game's title menu has a Settings screen built that way; a button's action can now
  be several, separated by `;` (`@hide Panel; @show Settings`).

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
- A **Joint** connects a rigid body to another (by name, nearest first) or to the world: fixed, ball, hinge,
  slider, distance or cone (swing and twist), with limits, a motor (hinges and sliders), a break force (it comes
  apart when pulled harder; `joint->broken` says so) and whether the two still collide. A **kinematic** rigid
  body is moved by animation or code and pushes others without being pushed.
- **Ragdolls**: the Skin component's *Make ragdoll* gives every bone a collider, a kinematic rigid body and a cone
  joint to its parent bone. The model animates as usual until `ctx.setRagdoll(root, true)` makes it go limp (its
  Animator stops and the bones fall in one piece); `false` hands it back to the animation. Put the ragdoll on a
  layer that doesn't meet its character's own capsule.
- **Vehicles**: a **Vehicle** component on a rigid body makes it a car (Jolt's wheeled vehicle): wheels
  (position, radius, width, steering, driven, and an entity shown as the wheel, which then turns and springs with
  it), engine torque and RPM, steering lock and suspension. Add component > Vehicle puts four wheels at the
  corners of the collider (front ones steer, back ones drive) and gives the body a car's mass; the selected
  vehicle's wheels are drawn in the viewport. The **VehicleDriver** behaviour drives it with the move keys (back
  brakes, then reverses; jump is the handbrake); code sets `vehicle->throttle`, `steering`, `brake`, `handbrake`
  and reads `speed` and `gear`.
- **PlayerController** also crouches (the `crouch` action, C / Left Ctrl: the capsule shrinks from the top, the
  camera eases down, and it only stands up with room overhead), climbs entities tagged **Ladder** (forward /
  back go up and down; jump lets go) and swims in trigger volumes tagged **Water** (it floats and sinks slowly;
  jump or up swims up).
- View > Show contacts draws, while playing, where bodies touch (dots) and which way they push (lines).
- Everything (queries, triggers, characters, bodies) is one Jolt world, kept up to date with the scene each update.
- Mesh colliders test real triangles, so they're the most expensive: prefer boxes and spheres for big scenes.

## Profiling, logs and autosave

- **Profiler** (editor: View > Profiler or F3; vkgame: F3 for an overlay): frame times over the last two seconds,
  the last frame's CPU scopes as nested bars, each scope's average and worst time, and the GPU's time per pass
  (shadows, scene, AO / bloom / composite, UI) from timestamp queries. Game code can time its own parts:
  `PROFILE_SCOPE("Pathfinding");` (core/profiler.h). **Save trace** writes the last two seconds, and **Record** /
  **Stop and save** any stretch of frames, as a Chrome trace (`traces/trace-<date>-<time>.json` in the user's data
  folder): open it in `chrome://tracing`, `edge://tracing` or [ui.perfetto.dev](https://ui.perfetto.dev) to scroll
  and zoom through frames, CPU scopes nested on one track and GPU passes on another. `vkgame --trace out.json`
  records the whole run and saves it on exit.
- **Crash reports**: a crash (a bad memory access, an assertion, an illegal instruction, an exception nothing
  caught) writes what happened and the stack to the console, the log file and `game-crash.txt` /
  `editor-crash.txt` next to it, then the program ends as it would have. The next run keeps it as
  `*-crash-last.txt` and says so (the editor in its status bar, the game in its log). Linux and macOS stacks name
  library functions and give offsets into programs (`addr2line -f -C -e vkgame <offset>`, `atos` on macOS);
  Windows names functions from the `.pdb` when one is next to the program.
- **Log files**: vkgame writes `game.log`, the editor `editor.log`, in the user's data folder next to the save games
  (`~/.local/share/vkworld/<project>/`), with the game's messages, validation messages and fatal errors; the
  previous run's log stays as `*.1`.
- **Autosave**: the editor writes unsaved changes to the user's data folder once a minute (never into the project,
  so they don't ship). Opening a scene that has newer unsaved work offers File > Recover autosave. The status bar
  shows "unsaved" until you save.

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
- Prefabs **nest**: a prefab can contain instances of other prefabs, and a change to the inner prefab reaches the
  outer one and every instance of it (each keeping its overrides). **Make variant** creates a prefab based on
  another: it follows every change to its base except what it changes itself.
- **Export** writes `<name>.prefab` to the working directory and **Import...** loads one, to share between scenes
  (prefabs nested in it aren't exported with it).
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
| NavAgent         | Walks to another entity along paths around walls (the scene's navigation)    |
| PlayerController | First-person walking, mouse look, jumping, crouching, ladders and swimming; collides through its capsule collider |
| VehicleDriver    | Drives the entity's Vehicle with the move keys (jump: the handbrake)          |
| Pickup           | Collected when the collector touches its trigger collider; logs how many are left |
| Shooter          | Fires a prefab along its -Z while the fire action (left mouse) is held      |
| Projectile       | Flies forward, stops at the first solid collider and damages Destructibles   |
| Destructible     | Destroyed after enough hits, optionally leaving a prefab behind              |
| Spawner          | Spawns a prefab every few seconds (with a little random spread), up to a maximum alive at once |
| SceneExit        | Goes to another scene when the collector touches its trigger collider        |
| StreamingZone    | Streams a scene in around it while the player is near, out when it leaves    |
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

Fields come in a few kinds, each with its own inspector widget:

```cpp
v.field("speed", speed, 0.05f, 0.f, 10.f);              // float: drag speed and range
v.field("enabled", enabled);                            // bool, Vec3 and plain strings too
v.field("maxAlive", maxAlive, 0, 100);                  // int, with a range
v.field("align", align, {"left", "center", "right"});   // int choosing an option (saved by its name)
v.field("target", target, FieldRef::Entity);            // a string naming something, with a picker:
v.field("bullet", bullet, FieldRef::Prefab);            //   Entity, Prefab, Scene, Sound, AnimationClip,
v.field("onHit", sound, FieldRef::Sound);               //   InputAction
```

References are names, as before (so they also find entities spawned later), but the inspector lists what's there
to pick from, takes entities dragged from the outliner and scenes / sounds from the asset browser, and flags a name
that matches nothing.

`GameContext` gives access to the scene, input actions, time, entity lookup by name or id, world positions,
collision queries, deferred `spawn()` / `destroy()` and `log()`. Behaviours also get `onTriggerEnter` / `onTriggerExit`
and `onAnimationEvent`.

### Tags, layers, messages and timers

Entities carry **tags** (the inspector's Tags row) and sit on one of 32 named **layers** (the scene's Layers
settings, with a matrix of which layers meet: colliders on layers that don't meet pass through each other and
don't set off each other's triggers):

```cpp
for (EntityId id : ctx.findWithTag("Enemy")) ...;
ctx.raycast(eye, forward, 50.f, hit, self.id, ctx.layerMask({"Default", "Enemy"}));   // sees only those

ctx.send(other, {"hit", self.id, 10.f});        // Behaviour::onMessage(ctx, self, message) after this update
ctx.sendToTag("Enemy", {"alarm"});
ctx.broadcast({"level-start"});

ctx.after(2.f, [](GameContext& c) { c.log("two seconds later"); }, self.id);   // dropped if self is destroyed
ctx.every(0.5f, [](GameContext& c) { /* ... */ });
ctx.sequence(self.id).wait(1.f).then(openDoor).wait(3.f).then(closeDoor);
```

Timers run on game time (they stop while paused) and, holding code, aren't kept in save games or across scene
changes.

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

### Loading and streaming

In vkgame and the editor's play mode, `loadScene()` reads the next scene on a **worker thread**: the file is
parsed and its models and images decoded there, into asset libraries of its own, then merged into the game's
(reusing what's already loaded), and its physics world is made there too. Meanwhile the current scene stays on
screen, frozen, under a loading screen (a spinner and a progress bar; `ctx.showLoadingScreen(false)` and
`ctx.loading()` let the game draw its own). vkgame's first scene loads the same way, once the window is up.
`Game::setAsyncLoading` switches this on; without it (tests, tools) loads happen within the update. On the stress
scene a change costs the main thread 2.6 ms instead of 118 ms.

New meshes reach the GPU by copying just the new vertices into room the buffers keep spare, and new textures are
uploaded as part of the frame's own commands, a few megabytes per frame (drawn plain until theirs arrives), so
neither waits for the GPU to go idle.

Big worlds can come in pieces: **streaming** adds another scene file's entities to the running scene, under a
new root placed anywhere, and takes them out again:

```cpp
EntityId part = ctx.streamIn("scenes/castle.vkw", {200, 0, 0});   // in the background; its behaviours start()
if (ctx.streamedIn(part)) { /* it's there */ }
ctx.streamOut(part);                                               // removed (or the load cancelled)
```

The built-in **StreamingZone** does it by distance: its scene comes in where the zone is while the player is
within its radius, and goes again once the player is a margin further away.

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

Text is UTF-8, drawn in Roboto (compiled in) at any size, or in a font of the game's own:
`ui.text(x, y, "¡Hola!", 48, {}, GameUI::Align::Left, "fonts/Title.ttf")` (HudText has a Font field). Glyphs are
made as they're first used, so any script the font covers works.

### UI layouts

Menus and HUDs can also be laid out in the editor instead of drawn from code: **UI elements** are entities with a
UI Element component (Add > UI), nested like any entities (an element sits inside its parent's rectangle; the top one
in the screen's), drawn over the game in the outliner's order, while playing and, in the editor, over the viewport
(View > Show game UI) where clicking picks one and dragging moves it. Each is placed by **anchors** (where in the
parent: a corner, an edge, the centre, or stretched across; presets in the Inspector) and **offsets** from them, on
the same 1080-high canvas as `GameUI`. Kinds:

- **Panel**, **Text**, **Image**: a background (colour, opacity, rounding), a text, a picture.
- **List**: its children stacked top to bottom (each as tall as its offsets say), with spacing.
- **Button**: its *action* runs when it's pressed: a message (sent to the *target* entity by name, or to everyone;
  `onMessage` gets it, also while paused), or `@resume`, `@pause`, `@quit`, `@load <scene>`, `@show` / `@hide` /
  `@toggle <entity>` (menus that open other menus), `@save` / `@loadgame <slot>`.
- **Slider**, **Toggle**, **Text field**: they show and change a **game state** value (`ctx.state()`, `game.state`
  in Lua): a number in a range, a flag, a text.

Texts are formatted like `HudText`: `{@key}` from the player's language, `{name}` from the game state. Menus work with
the **keyboard and gamepad**: arrows / d-pad move to the nearest element that way, Enter / Space / A presses,
Left / Right move a slider; the mouse works too (and is freed while a menu is up). While a text field is being typed
into, the game's own controls don't see the keys. The sample game starts on a menu made this way
(`scenes/menu.vkw`: Play, a music volume slider and a language toggle applied by `scripts/MenuSettings.lua`, a name
field, Quit).

### Languages

Games speak several languages through **string tables**: `strings/<language>.json` in the project, keys to text
(nested objects make dotted keys). A key missing in a language comes from `en.json`, then from the fallback the
code gives:

```json
{ "hud": { "score": "Puntos" }, "pause": { "resume": "Continuar" } }
```

```cpp
ui.text(40, 30, ctx.format("{@hud.score}: {score}"), 48);   // {@key}: the string; {key}: a game state value
ui.button(x, y, w, h, ctx.text("pause.resume", "Resume"));
ctx.setLanguage("es");                                       // switch while playing
```

HudText, HudBar and PauseMenu translate `{@key}`s (the pause menu's buttons use `pause.resume`, `pause.save`,
`pause.load`, `pause.quit`). The project's language (project.json `"language"`, Game > Language in the editor) is
what the game starts in; `vkgame --language es` overrides it. The sample game has English and Spanish.

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

### Multiplayer

The basics of a networked game, over UDP: one machine **hosts** (the server runs the game), others **join**:

```sh
./vkgame path/to/MyGame --host            # on port 27015 (or --host 4000)
./vkgame path/to/MyGame --join 192.168.1.5    # (:port if not the default)
```

In the editor, Game > Host a multiplayer game hosts while you play. Both sides load the same scene; a client
joins once it has.

- **Players**: the scene's Multiplayer settings (Inspector, nothing selected) name a **player prefab**: each player
  who joins gets one (spread around a spawn point), and so does the host. On the server, a player's behaviours read
  *that player's* input as their `ctx.input()` (the client sends its input actions every update), so a
  `PlayerController` works for everyone unchanged. The game looks through a camera under the local player.
- **Replication**: entities with a **Replicated** component (players always) are sent to every client 20 times a
  second: where they are, whether they're visible and (optionally) their behaviours' fields; ones spawned or
  destroyed on the server appear and go on the clients too. The **game state** (`ctx.state()`) is shared the same
  way. Clients show replicated entities where the server has them (smoothed between updates) and don't simulate
  them; everything else runs on each machine as usual (effects, UI).
- **Prediction**: a client moves its own player at once with its own input, and only puts it where the server has
  it when they disagree by more than a metre.

Not there yet: scene changes and save games while connected, joining mid-game with history, lag compensation,
replaying unacknowledged inputs, encryption and NAT punch-through. Messages between behaviours stay on the machine
that sends them.

### Lua scripting

Behaviours can also be written in **Lua** (5.4, built into the engine): no compiler, and they reload while the game
runs. Every `.lua` file under a project's `scripts/` folder is a behaviour type named after the file
(`scripts/Sentry.lua` is `Sentry`), listed under *Scripts* in the inspector's Add component menu and saved in
scenes like the C++ ones. Assets > New script makes one from a template; double-clicking a script opens it in the
system's editor for `.lua` files. The sample game has two (`Sentry`, `RunTimer`) and a helper in `scripts/lib/`.

```lua
-- scripts/Door.lua
local Door = {
    description = "Opens when the player comes near",
    fields = {                                     -- edited in the inspector, saved in scenes
        {"openHeight", 3, min = 0, max = 10},       -- number (int = true: whole numbers; speed = drag speed)
        {"player", "Player", ref = "entity"},       -- string (ref: entity, prefab, scene, sound, clip, action)
        {"mode", "slide", options = {"slide", "swing"}},
        {"locked", false},                          -- boolean
        {"offset", vec3(0, 1, 0)},                  -- vector
    },
}

function Door:start()
    self.closed = self.entity.position             -- fields are members of self; so is anything else you add
end

function Door:update(dt)
    local player = game.find(self.player)
    local near = player and vec3.distance(player.worldPosition, self.closed) < 4
    local target = near and not self.locked and self.closed + vec3(0, self.openHeight, 0) or self.closed
    self.entity.position = vec3.lerp(self.entity.position, target, math.min(1, dt * 3))
end

return Door
```

Callbacks, all optional: `start()`, `update(dt)` (fixed step), `onTriggerEnter(other)`, `onTriggerExit(other)`,
`onMessage(msg)` (`msg.name`, `msg.from`, `msg.value`, `msg.text`), `onAnimationEvent(name)`, `drawUI(ui)`.

- **Entities** (`self.entity`, `game.find(...)`, ...): `id`, `name`, `exists`, `position` / `rotation` (degrees) /
  `scale` (local), `worldPosition`, `forward` / `right` / `up`, `visible`, `layer`, `tags`, `parent`, `velocity`
  (rigid bodies), `color` (mesh renderers); vectors are copies: change one and assign it back. Methods:
  `hasTag`, `addTag`, `removeTag`, `destroy`, `send(name, value, text)`, `moveCharacter(delta)` (returns
  `grounded`, `hitWall`, ...), `lookAt(point)`, `emitParticles(n)`, `playSound(clip, volume)`, `setRagdoll(limp)`,
  `script("Type")` (another Lua behaviour's `self`), `getField("Type", "name")` / `setField("Type", "name", v)`
  (any behaviour's fields, C++ ones too); animators: `setAnimParameter(name, value)`, `playAnimation(clip, fade)`,
  `playLayer(layer, clip, weight)`, `stopLayer(layer)`, `setIK(chain, point, weight)`.
- **`game`**: `time()`, `dt()`, `find(name or id)`, `findWithTag(tag)`, `spawn(prefab, pos, forward, parent)`,
  `destroy(e)`, `log(...)` (and `print`), `raycast(origin, dir, maxDistance, ignore, layers)` (a hit: `entity`,
  `point`, `normal`, `distance`), `overlapSphere(center, radius, layers)`, `findPath(from, to)`,
  `after(seconds, fn)` / `every(seconds, fn)` / `cancel(id)`, `send(e, name, value, text)`, `sendToTag`,
  `broadcast`, `playSound(clip, pos, volume)`, `playSound2D`, `playMusic(clip, fade, volume)`, `stopMusic`,
  `setBusVolume`, `duck`, `text(key)`, `format(text)`, `setLanguage`, `language()`, `loadScene(path)`,
  `scenePath()`, `quit()`, `saveGame(slot)`, `loadGame(slot)`, `hasSaveGame(slot)`, `setPaused(b)`, `paused()`,
  and `game.state.key` (numbers, strings and booleans that survive scene changes and go in save games).
- **`input`**: `down(action)`, `pressed`, `released`, `pressedThisFrame` (for `drawUI`), `value`,
  `axis(negative, positive)`, `mouseDelta()` (returns x, y).
- **`ui`** (in `drawUI`): `width()`, `height()`, `text(x, y, text, size, color, align, font)`, `textWidth`,
  `rect(x, y, w, h, color, rounding)`, `border`, `bar(x, y, w, h, fraction, fill, back)`, `image(texture, x, y, w, h)`,
  `button(x, y, w, h, label, size)` (true when clicked), `hovered(...)`, `showCursor()`; colours are
  `color(r, g, b, a)`, `{r, g, b, a}` or a vec3.
- **`vec3(x, y, z)`**: `+ - * /` (by numbers or per component), `==`, `v:length()`, `v:normalized()`, `v:copy()`,
  `vec3.dot`, `vec3.cross`, `vec3.lerp`, `vec3.distance`.
- **`require("lib.name")`** loads `scripts/lib/name.lua` (helpers shared by scripts; files in `lib/` aren't
  behaviours). The standard `string`, `table`, `math`, `utf8` and `coroutine` libraries are there; `io`, `os` and
  loading files aren't.

**Hot reload**: saving a script reloads it, in the editor and in `vkgame` (when the files are on disk, not packed):
running instances keep their members and use the new functions from their next callback, so a game can be changed
while it plays (new fields get their defaults). An error is reported with its file and line, in the status bar or
the viewport and in the log; a script that fails to load keeps its previous version, and a callback that fails
stops for that instance until the script is saved again. `vkgame --check` fails on script errors too.

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
| `src/assets/`              | Mesh library (procedural meshes, OBJ import, picking), texture library (`textures.*`), glTF import (`gltf.*`), asset ids and import settings (`asset_db.*`) |
| `src/scene/`               | Entities, components and hierarchy (`scene.*`); behaviour base class and registry (`behaviour.*`); JSON save/load (`serialization.*`); terrains (`terrain.*`) |
| `src/audio/`               | Sound playback (miniaudio), built-in synthesised clips          |
| `src/physics/`             | `physics.*`: the scene's physics on Jolt Physics: rigid bodies, raycasts, overlaps, triggers, characters |
| `src/game/`                | Game runtime: fixed-timestep loop and GameContext (`game.*`), input actions (`input.*`), built-in behaviours, game module loading (`module.*`) |
| `src/script/`              | `lua_script.*`: Lua behaviours (the scripts/ folder, the Lua API, hot reload)   |
| `src/net/`                 | `udp.*` sockets, `net_session.*` multiplayer: hosting, joining, replication, remote input |
| `src/editor/`              | Editor app: `editor.cpp` frame/play/operations/undo, `editor_panels.cpp` menus, toolbar, status bar and dialogs, `editor_outliner.cpp`, `editor_inspector.cpp`, `editor_viewport.cpp` navigation/gizmos/picking, `editor_assets.cpp` project, asset browser, game module and export, `editor_asset_db.cpp` asset inspector, moving / renaming assets, `editor_terrain.cpp` terrain tool, `editor_prefs.cpp` preferences saved per user |
| `tests/`                   | Engine unit tests (`test.h` is the tiny framework)              |
| `examples/sample_game/`    | A project with its own game module (`src/`), scene and `project.json` |
| `tools/`                   | `check_screenshot.py` (CI smoke test)                           |
| `bench/`                   | `engine_bench`: CPU timings on the stress scene (`scene/stress_scene.*`) |
| `shaders/common.glsl`      | Frame data (camera, sun, light clusters, shadow cameras) and per-object instance data shared by the shaders |
| `shaders/scene.*`          | Lit, textured, shadowed objects; checker ground; fog            |
| `shaders/shadow.vert`      | Shadow map (depth only)                                         |
| `shaders/post.vert`, `bloom_*.frag`, `composite.frag` | Bloom down / up passes; tonemapping and gamma onto the swapchain |
| `shaders/outline.*`        | Selection outline (screen-space extruded hull + stencil)        |
