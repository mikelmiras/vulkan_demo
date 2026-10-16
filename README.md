# Vulkan World Editor

A small Vulkan 1.3 3D scene with a Blender-style editing UI, in C++17.

- Runs on **Linux** (Wayland, X11 / XWayland via raw XCB), **Windows** (Win32) and **macOS** (Cocoa + MoltenVK). No
  GLFW/SDL needed.
- Scripting: [Lua](https://www.lua.org) 5.4.7 (`third_party/lua`, MIT)
- UI: [Dear ImGui](https://github.com/ocornut/imgui) v1.92.9b (docking branch) + [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo);
  images: [stb_image](https://github.com/nothings/stb) v2.30; glTF: [cgltf](https://github.com/jkuhlmann/cgltf) v1.14;
  audio: [miniaudio](https://miniaud.io) v0.11.21; text shaping: [HarfBuzz](https://harfbuzz.github.io) 14.6.0.
  All vendored in `third_party/` and compiled into the binary.

## Build & run

Requires the [Vulkan SDK](https://vulkan.lunarg.com/) (sets `VULKAN_SDK`, provides `glslc`) and CMake 3.20+.

### Linux

Also needs libxcb (installed on any X11/XWayland desktop), and for native Wayland windows libwayland and
libxkbcommon's headers (`sudo apt install libwayland-dev libxkbcommon-dev`; without them it builds for X11 only).
The libraries themselves are loaded when the program starts, so a build runs on machines without them too (on X11).
On a Wayland session the window is a native Wayland one when the compositor draws window decorations itself (KDE,
sway, Hyprland, ...); elsewhere (GNOME) it goes through XWayland, which gets a title bar. `VKWORLD_PLATFORM=wayland`
or `=x11` picks one. On HiDPI displays a native Wayland window is drawn in physical pixels (fractional scales too:
125 %, 150 %), as on Windows, and the UI follows the scale, also when the window moves to another display. On X11
the UI takes the desktop's scale from `Xft.dpi` (what KDE's and GNOME's scaling settings set; else `GDK_SCALE`) and
the window opens that much bigger. On a Mac's Retina screen the editor draws in the screen's pixels (sharp text and
lines); games keep one pixel per point (a quarter of the pixels to fill) unless `VKWORLD_HIDPI=1` (`=0` turns the
editor's off too). `VKWORLD_UI_SCALE=1.5` sets the UI's scale by hand anywhere. View > Theme > *High contrast* gives
the editor black panels, white text, outlined controls and yellow focus.

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

**File > Export game** (or `vkworld <project folder> --export <folder> [--pack | --single-file] [--package]`,
without a window)
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

**Projects**: a project is a folder with a `project.json` (its name, start scene, version, game module, icon,
language, update URL, relay, crash report URL, and the engine version that last saved it). Started without one,
the editor shows its **project launcher**: recent projects (with their versions; ones that moved can be forgotten),
a new project from a template, opening a folder, or just the demo scene (File > Project launcher brings it back;
`vkworld --launcher` shows it anyway). **File > Project settings** edits everything in `project.json`, with pickers
for the scene, icon and language and checks before saving. Opening a project made with a newer engine logs a
warning. New projects get a `.gitignore` for what the engine generates in them (`bin/`, `.vkworld/`, updates), so
the rest (scenes, scripts, assets and their `.meta` files) goes into version control.

**Packages to hand out** (File > Export game > *And make an AppImage / a .dmg / an installer*, or `--package`):
next to the export, `<game>-<version>` made into an **AppImage** on Linux (one file that runs on any distribution;
needs [appimagetool](https://github.com/AppImage/appimagetool) on the PATH or in `VKWORLD_APPIMAGETOOL`), a
**.dmg** on macOS (the app and a link to Applications), or an **installer** on Windows (per user, no administrator:
a Start menu entry and an uninstaller; needs [NSIS](https://nsis.sourceforge.io)'s `makensis`, or
`VKWORLD_MAKENSIS`). The version is `project.json`'s `"gameVersion"` (default 1.0.0).

**Build profiles** (File > Build settings, or `vkworld <project> --build <name>`): named ways to build the game,
kept in `project.json`: the layout, a package, an update pack, the folder, and **Shipping**, which turns off the
developer tools in the built game (the F3 profiler and the frame counter, the Lua debugger, `--trace` and the
validation layer). Exported executables say what they are: on Windows a version resource (Properties > Details:
product, publisher, version, copyright), on macOS the bundle's `Info.plist` (identifier, version, copyright), from
the project's name, version, *publisher* and *identifier* (Project Settings). Games and the editor keep the GPU's
compiled pipelines between runs (`pipelines.cache` in the user's data folder), so later starts are quicker.

**Cooked assets** (a build profile's *Cook assets*, on by default; `--cook` or `--textures bc7|bc1|none` with
`--export`): the build ships textures and models ready for the GPU instead of their sources. Every image a scene
or prefab uses as a texture becomes `<image>.vktex`: its mip levels made at build time (filtered in linear light,
its import settings applied) and **block compressed**, BC7 by default (8 bits a pixel instead of 32, close to the
original), or BC1 (4 bits; BC3 for images with alpha) for the smallest, or uncompressed with the mips baked. Every
glTF model becomes `<model>.vkmodel`: the meshes as the importer makes them, LODs included, its images cooked and
its animation clips, copied straight into memory at load with no parsing. Loading looks for the cooked file first,
so scenes still name their sources; an image also named outside a texture list (an icon, a sky, a height map, a
path in a script) is shipped as it is too. Texture streaming keeps working (the levels are uploaded from the cooked
file) and counts compressed bytes against its budget; a GPU without BC support gets the textures decoded at load
(`VKWORLD_BC=0` does that on any GPU, to try it).

**Update checks**: with an `"updateUrl"` in `project.json`, the game asks it at start (in the background) whether
there's a newer `gameVersion`. It serves `{"version": "1.1.0", "url": "https://...", "notes": "..."}`, or it's a
GitHub API `.../releases/latest` URL. When there is, the game state gets `update.available`, `update.version`,
`update.url` and `update.notes` for a menu to show, and a button with the action `@open {update.url}` opens the
page. Players can turn it off (`settings.checkUpdates`).

**Installing updates**: export the new version with *And make an update pack* (or `--update-pack`): next to the
export, `<game>-<version>-<platform>.vkwupdate` (the whole export as one pack) and its SHA-256 in a `.sha256` file.
Put the pack online and name it in the manifest per platform: `"downloads": {"linux": {"url": "https://.../
Game-1.1.0-linux.vkwupdate", "sha256": "..."}, "windows": {...}, "macos": {...}}`, or attach the packs to a GitHub
release (their names must have the platform in them; GitHub gives their digests). Then `update.installable` is true,
and a menu button with `@update` downloads it in the background (`update.stage`: `downloading`, `verifying`,
`unpacking`, `ready` or `failed`, with `update.error`), checks it against its digest (no digest, or one that
doesn't match: nothing is installed), and unpacks it; `@restart` swaps it in and starts the game again. The game's
files are moved aside (`<file>.old-update`, removed at the next start) and put back if anything fails. `vkgame
--install-update` does the same from a command line (a launcher, a script) and exits. The game must be somewhere
its player may write (a per-user install, an unpacked folder: not Program Files or a read-only image).

**Delta updates**: the export's update pack also comes as a chunked release, `update/manifest.json` and
`update/chunks/` (put the folder online as it is and point `updateUrl` at the manifest). Every file is cut into
chunks by its content (a rolling hash: an edit only changes the chunks around it) named by their SHA-256, so
installing downloads only the chunks the installed version doesn't already have, from any earlier version (a
level changed in a 2 GB pack: a few megabytes), checks each one, and replaces only the files that changed.
`update.progress` (0-100) and `update.downloaded` (bytes) follow it. **Resuming**: chunks are kept as they arrive
(`<game>/.update/chunks`), so a download cut off (a lost connection, the game closed) picks up where it stopped,
also after a restart. **AppImages** update in place: the export lists the AppImage too (`linux-appimage`), and a
game run from one rebuilds the new AppImage from the old one's chunks and puts it where the old one is, under its
name. A release for several platforms: `vkgame --merge-manifests manifest.json linux.json windows.json macos.json`.

**Signed updates**: `vkgame --update-keygen key.txt` makes a signing key (the secret in `key.txt`: keep it out of the
project and version control) and prints its public half: put it in `project.json` as `"updateKey"`. From then on
the game only takes a manifest signed with that key (Ed25519, over the whole manifest: every file's and chunk's
digest), so a compromised server or a hijacked URL can't hand players anything else. Sign with `vkgame
--sign-manifest manifest.json key.txt`, or export with `VKWORLD_UPDATE_KEY_FILE=key.txt` to sign as it's made.
GitHub releases can't be signed this way: with an `updateKey`, use your own manifest.

**Crash reports, sent**: with a `"crashReportUrl"` in `project.json`, a game whose last run crashed sets
`crash.lastRun` for a menu to ask the player, and `@sendcrashreport` sends that report (`@dismisscrashreport`: no);
players who always agree turn on `settings.sendCrashReports` (off unless they do). The report goes as a plain-text
POST with the home folder written `~` and the user's name `<user>` (state `crash.sending`, `crash.sent`,
`crash.error`), and is then renamed `*-crash-sent.txt`, so it's sent once. Nothing is sent without the player.

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

**File > New project** starts one from a template, in a folder you pick:

- *Empty*: a sun, a ground and a camera.
- *First person*: a PlayerController body with its camera at eye height, a few blocks and a ramp.
- *Top-down*: a character seen from above that collects gems, moved by `scripts/TopDownPlayer.lua` (a Lua
  behaviour to change: speed, turning, the camera following it), and a score on screen.

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
the scenes too; renaming a prefab (Outliner > Prefabs) updates the behaviour fields that name it. Moves, renames and
deletes are **undoable** (Ctrl+Z, in the same history as scene edits): a deleted file is kept, with its `.meta`, in
a trash folder in the editor's user data until the editor closes, and comes back with the same id.

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

**View > Game view** shows the game's camera in a window of its own, docked beside the scene view the first time
(drag its tab anywhere): the viewport stays with your camera, gizmos and selection while the game plays next to it,
and the game's mouse and UI go to the Game view. It's rendered whole into an image of its own (bloom, ambient
occlusion, anti-aliasing, the player's colour filter), so it can go anywhere, even under other windows.
`vkworld --play --game-view` starts that way.

**Split screen**: give two to four cameras a *Split-screen player* (1-4, Inspector > Camera) and vkgame shares the
screen between them: two side by side, three or four in quarters, by player number (`Game::cameras()` lists them
with their parts of the screen). Player 1's view is drawn as usual and the others each whole into an image of their
own; each has the sun's shadows around it (its own four cascades, in tiles of the shadow atlas: 1024 texels for
two players, 512 for three or four; the editor's Game view gets them too), and they share the lights. Each player has their own pad:
player 1 the keyboard, the mouse and the first pad connected, player 2 the second pad and so on, and the behaviours
under a player camera's root (its topmost parent: the player entity) read that player's input as `ctx.input()`
(`ctx.rumble` shakes that player's pad). The editor's Game view shows player 1.

**Joining with Start**: `ctx.setPadJoining(true)` (Lua `game.setPadJoining(true)`, the UI action `@padjoin on`)
hands pads out as players press Start instead of in order: the first pad to press it plays with player 1 (who also
has the keyboard), the next ones become players 2, 3 and 4. A pad unplugged leaves its seat waiting for the next
Start; `@resetplayers` sends everyone but player 1 out. The game state says how many play (`input.players`,
`ctx.localPlayers()`) and with what (`input.player.2.pad`: `xbox`, `playstation`, `switch`, `generic`, or `""`), so a
"Press Start to join" screen can show the seats and turn on the cameras of the players who joined.

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
- **Gamepads**: each read on its own (`platform::gamepads()`: on Windows XInput's pads, and DualShock 4s, DualSenses
  and Switch Pro Controllers read as HID (XInput doesn't see them: four more slots after XInput's four); the
  GameController framework's on macOS; the Linux joystick API's `/dev/input/js*` with each driver's own button codes,
  so Sony's and Nintendo's pads have their buttons in the right places; a pad plugged in later is found within two
  seconds). Menus answer any of them (on Windows the HID pads only while no XInput pad is plugged in). What kind each
  is (Xbox, PlayStation, Switch, other) comes from its USB vendor and name.
- **Button prompts**: `{button:jump}` in any text (UI layouts, `ctx.format`, Lua `ui.text`) shows what to press for an
  action on the device the player used last: a keycap for keys and mouse buttons, the pad's own buttons for pads
  (Xbox's coloured A B X Y, PlayStation's cross, circle, square and triangle, Switch's letters where Nintendo puts
  them, LB / L1 / L, LT / L2 / ZL, the d-pad, sticks, Menu / Options / +), drawn as shapes (any font). They switch
  as soon as the player picks up the other device. `{button:jump:pad}` / `{button:jump:keyboard}` pick one;
  `ctx.prompt("jump")` and Lua `input.prompt("jump")` give the same text.
- **Rebinding** from the game's menus: a button with the action `@rebind jump` waits for the next key, mouse button
  or pad control (on the device that pressed the button: rebinding with the pad binds a pad control) and makes it
  that device's binding for the action; the other device's stay. While it waits, the game state's `input.rebinding`
  names the action (for a "Press a key..." text) and nothing else takes the press; Esc or 10 s cancel. Players'
  bindings are saved with their settings (`settings.bind.<action>`, e.g. `"K,GamepadR1,Mouse0"`); `@resetbindings`
  (or `@resetbindings jump`) brings the scene's back.
- **A stick as a whole**: `@rebindstick move_forward,move_back,move_left,move_right` (the up, down, left and right
  actions) waits for a stick or the d-pad to be pushed (buttons are ignored) and binds its four directions to the
  four actions; on the keyboard a key of a cluster binds the whole cluster (WASD, the arrows, IJKL, the number pad's
  8 4 5 6).
- **Conflicts**: a control bound to two actions shows in the game state: `input.conflict.<action>` names the other
  actions with a control in common (`""`: none), `input.conflicts` counts the pairs, so a controls menu can mark
  them (`{input.conflict.jump}`).
- **Rumble and light bars**: `ctx.rumble(low, high, seconds)` runs the heavy and light motors (0..1; Lua
  `input.rumble`), `ctx.setGamepadLight({r, g, b})` colours a DualShock 4 / DualSense light bar (Lua
  `input.setLight`). Windows rumbles through XInput, and Sony's pads (and their light bars) through HID; macOS uses
  the pad's haptics and light;
  Linux rumbles through the pad's evdev node and writes its LEDs in sysfs (often only root may: a udev rule allows
  it). The player's `settings.rumble` turns rumble off.
- Messages logged by behaviours show at the bottom of the viewport (and on stdout).
- Game > Maximize viewport on play hides the side panels while playing.
- **Power**: the editor only draws when something changes: input, an edit, a drag, a load, the game playing, a
  selected particle emitter, global illumination still filling in. In between it sleeps, waking every 2 s (the
  status bar says `idle`). So a laptop stays cool with the editor open. View > Power has:
  - *Realtime viewport*: every frame, as before; live effects, every particle emitter running.
  - *Slow down in the background*: 10 frames a second while another window is in front.
  - A frame-rate cap: 30, 60 (the default), 120, or the display's rate.

  On macOS the editor picks the integrated GPU when there are two. `VKWORLD_GPU=integrated|discrete|<part of a
  name>` chooses for the editor and games alike (games take the fastest by default).

## Rendering

- **Materials**: physically based (glTF metallic-roughness, GGX). A Mesh Renderer has a colour and texture (PNG,
  JPEG, TGA, BMP via Inspector > Load texture..., or the built-in procedural Checker / Bricks / Tiles), metallic
  and roughness (optionally from a metal/rough map), a normal map (tangents come from screen-space derivatives,
  so meshes don't need them; built-in "Bricks Normal") and an emissive colour, strength and map. glTF materials
  import all of these. Built-in meshes have box-mapped texture coordinates; imported ones use their own. Textures
  get mipmaps and anisotropic filtering, and are read as sRGB colour or as plain data, whichever the slot needs.
  The demo scene's "Materials" row shows them.
  Over the base, layers: a **clear coat** (a varnish with its own sharp reflection over the base's: car paint,
  lacquer; its roughness apart), **sheen** (cloth's soft glow at grazing angles: velvet, felt) and **anisotropy**
  (highlights stretched across the surface along the texture's u or v: brushed metal). The specular keeps the
  light that bounces between microfacets (multiple-scattering compensation), so rough metals don't turn dull and
  dark. Light inside the material: **translucency** for thin things (leaves, paper, curtains, lampshades): light
  from behind comes through in their colour, glowing most when you look towards the light through them, and the sky
  behind lights them too; terrain foliage is translucent (0.5), so grass against a low sun glows. **Subsurface** for
  thick, soft things (skin, wax, marble, jade): the light reaches past the shadow's edge (wrapped diffuse), tinted
  by the *Subsurface colour* there (red for skin), and their shadows are softer; and the light under them spreads
  on screen (screen-space diffusion: a blur of their diffuse light across and down, up to 2 cm in the world at full
  subsurface, each channel as far as the *Subsurface colour* says: skin's red the widest, a blue one's blue; only
  over subsurface pixels at about the same depth), so lighting and shadow edges on skin and wax melt rather than
  cut, while highlights stay sharp (the lit pass keeps their specular light apart). Thin parts lit from behind by
  the sun (ears, fingers, a wax sheet) let light through by how thick they are, from the sun's shadow map
  (`VKWORLD_SSS=0`: no diffusion, to compare). Lamps' light doesn't come through yet. Materials with none of these layers
  (coat, sheen, anisotropy, translucency, subsurface) are drawn with a lean variant of the lit shader that leaves
  their code out (a specialization constant); the rest with the full one, after them (`VKWORLD_LEAN_SHADING=0`:
  everything with the full one, to compare). The inspector's *Preset* sets a kind
  of material at once (plastic, rubber, polished, rough or brushed metal, car paint, lacquered wood, cloth, velvet,
  stone, leaf, paper, skin, wax, marble).
  Surface detail: a **height map** with a **parallax depth** gives **parallax occlusion mapping**. The view ray is
  stepped down into the heights (more steps at grazing angles and at higher quality) and the material read where it
  meets them, so bricks stand out and mortar sinks in, without more geometry. A **detail map** is a fine normal map
  tiled over the others (its own tiling and strength): grain, pores, scratches that hold up close. **Sheltered**
  keeps a surface dry whatever the weather.
- **Decals** (Add > Decal, or the Decal component): an image or colour projected down the entity's box (a unit
  cube under its transform: scale it to size; the direction is its -Y) onto whatever's inside: road markings,
  dirt, cracks, signs on walls. A decal sets the surface's colour (the texture's, where its alpha is), optionally
  its roughness, and its wetness (1 on flat ground: a puddle). It fades at the box's ends and, by **Angle fade**,
  on surfaces turned away. Decals change the surface before it's lit, so they're shaded, shadowed and reflected
  like it. A decal's **normal map** bends the surface where it shows (cracks, tyre tracks, a manhole's rim; with
  opacity 0, bumps alone). A decal with **Glow** gives off light: its colour (times the texture) this bright on top
  of the lit surface, bloomed above 1 (painted neon, lit signs, glowing runes); with screen-space GI on it lights
  the surfaces near it too, through the picture the GI traces (it's no light of its own: no shadows, nothing lit
  off screen). The nearest 256 in view are drawn, with up to 16 different textures and 8 normal maps
  (further decals beyond that keep their colour alone, or go flat). They're binned into the lights' clusters, so
  each pixel only checks the decals near it.
- **Weather** (Scene look > Wetness, Puddles, Rain): surfaces facing up (and the walls, a little) get darker where
  porous and glossier, standing water collects in hollows of flat ground (as much as Puddles says, placed by noise)
  as dark, smooth mirrors, and rain rings spread on them. With screen-space reflections on, wet ground reflects
  the street, most of the cost of a wet scene.
- **Lights**: Light components, directional (shines along the entity's -Z), point (colour, intensity, range) or
  spot (a cone along -Z, with a soft edge). The first directional light is the sun; up to 1024 point and spot lights
  (nearest the camera) light each frame, through **clustered** lighting: the view is cut into 16 x 9 tiles and 24
  depth slices, each listing the lights that reach it, so a pixel only shades with the lights near it (hundreds of
  small lights cost little). A spot light can have a **cookie**: an image it shines through (a window frame,
  leaves), its brightness masking the light. Scenes without a directional light get a default sun.
- **Shadows**: the sun's are 4 cascades (2048x2048 each) covering 160 m of the view, closer ones sharper, each
  snapped to texels so edges don't shimmer. Spot and point lights cast shadows into a **shadow atlas** (one 4096 x
  4096 image cut into tiles): those in view nearest the camera get the biggest tiles (1024, then 512, then 256; a
  point light's six cube faces half that), up to 44 views a frame. **Soft**: each shadow lookup is 12 to 16 taps on
  a Poisson disk turned per pixel (temporal AA smooths the grain), each a hardware-filtered comparison; the sun's
  are **PCSS** (percentage-closer soft shadows): sharp where an object meets the ground, softer further from it,
  as wide as the sun's size says (Scene look > *Shadow softness*: 1 a slightly hazy sun, 0 as sharp as the maps
  allow), and cascades blend into each other over the last fifth of each, so where one takes over doesn't show.
  **Cached**: a shadow map is only drawn again when what it holds changes (its camera, its place in the atlas, or any
  caster in it moving, animating or changing): a still street lamp's shadows cost nothing frame after frame, and the
  sun's two far cascades catch up with a moving camera every second and fourth frame (`VKWORLD_SHADOW_CACHE=0`:
  everything every frame). The sun's cascades are **fitted to the view**: they start at the nearest thing in sight
  rather than at the camera (`VKWORLD_SHADOW_FIT=0`: from the camera), so an overhead camera's four cascades all
  fall on the ground it sees, and the shadow distance counts from there. Their **casters are culled by the view**:
  a caster is drawn into the sun's maps only if its shadow (its bounds swept along the sunlight, down to the lowest
  thing in the scene) can land in view, and on the box round what's visible; the far cascades, drawn every few
  frames, use a wider view for what the camera turns to meanwhile, and draw their casters a level of detail coarser.
  A spot or point light that moved (a headlight) culls its casters the same way (their shadows, cones away from it,
  must reach the view); a still one's map is cached with all of them (`VKWORLD_SHADOW_CULL=0`: every caster in a
  shadow camera's view, at the camera's level). The editor's stats count the casters skipped. In **volumetric fog**, spot and point lights' shadows cut their beams and halos too
  (headlights stop at the wall they light).
  Cut-outs cast shadows with their holes. **Coloured shadows**: in the sun, see-through materials under 75% opaque
  (glass, a stained window) and thin translucent ones (translucency 0.5 and over: paper, a curtain) don't block its
  light but tint it, their colour more the more opaque they are (a layer per cascade, half its size, holds what
  they let through and the nearest one's depth); more opaque see-through ones cast a plain shadow. Spot and point
  lights do the same (a tint tile beside each shadow tile, at a quarter of the atlas's size): a lamp behind
  stained glass throws its colour, and in volumetric fog the beam takes it below the glass. Frames without any leave
  the lookups out of the lit shader (a specialization: the tinted variants are made the first time a scene has
  them).
- **Ambient occlusion**: corners, creases and the ground under things darken a little (screen-space, from the
  depth, at half resolution; strength and radius in the scene's look settings). It's worked out before lighting,
  after the depth prepass, and darkens the light that comes from all round (the sky's, the probes', the bounced
  light) and the reflections in crevices, never the sun's or the lamps': a sunlit kerb isn't dirtied. Its samples
  turn every frame and it keeps a history (followed by the motion vectors when they're made: temporal AA, the
  half-size traces, motion blur; otherwise kept while the camera holds still), so it settles smooth. It also finds
  the **bent normal**, the open sky's mean direction, and the sky's and the probes' light comes from there: a wall's
  foot next to a crate takes the light from above and away rather than from the crate. Split-screen
  views and `VKWORLD_AO_LIT=0` (or VKWORLD_PREPASS=0) darken the finished picture as before.
- **Sky**: a gradient from the horizon (also the fog colour) to the zenith, the ground below, and the sun's disc
  and glow. It also lights the scene: ambient light and reflections come from it (blurrier on rougher surfaces).
- **Sky images**: a scene can use a panorama instead (Scene look > Sky image: an equirectangular image, twice as
  wide as high, ideally an `.hdr`; with a brightness and a turn). It's drawn as the sky, and it lights the scene:
  matte surfaces get its light from all around (spherical harmonics), shiny ones reflect it, sharp on smooth
  surfaces and blurred like their highlights on rough ones (six levels prefiltered with the GGX lobe when it's
  loaded). The sun is still the scene's directional light; the fog keeps the horizon colour.
- **Reflection probes** (Add > Reflection probe): a box where shiny things reflect their real surroundings instead
  of the sky. The probe renders the scene from its centre in all six directions (when it's first seen, moved or
  resized, on Bake in its Inspector, and when the sky it was baked under has changed: the sun moved by 2 degrees
  with the time of day, the clouds or the haze), and that picture is prefiltered. Baking never stalls: one probe at
  a time, a face a frame drawn with the frame itself, then prefiltered on a worker thread, the old picture (or, for
  a new probe, the sky's) in use until the new one's ready (about ten frames) like a sky image; surfaces inside the box
  reflect it (lined up with the walls of a box-shaped room with *Box projection*) and get their ambient light from
  it, blending into the sky over the last metre inside its edges. The four probes nearest the camera are used.
- **Global illumination** (Scene look > Global illumination; off by default): light bounces off surfaces, so a red
  wall tints the floor beside it and shadows aren't flat. A grid of 8 x 4 x 8 light probes follows the camera
  (*GI probe spacing* apart); each frame two of them (the nearest new ones first, then the longest unchanged) draw the
  lit scene around them into six small faces, turned into spherical harmonics on the GPU, and surfaces take their
  ambient light from the eight probes around them. The probes are lit by the grid too, so light bounces again each
  time they're redrawn; moving lights and objects show with a lag of a few seconds. *GI intensity* scales it.
  Limits: light can leak through thin walls (no visibility test per probe); probes inside walls see darkness.
- **Screen-space reflections** (Scene look > Screen-space reflections; off by default): surfaces up to roughness
  0.8 reflect what's on screen: each pixel's reflected ray is marched through the last frame's picture and
  distances (up to 120 m: where nothing in the ray's 8 x 8 tile of the picture is as near as the ray, it jumps
  across, taken from a pyramid of the picture's nearest distances; near things, it steps a couple of pixels at a
  time, so thin ones aren't stepped over), and what it meets is blended over the probe or sky reflection, fading at the picture's edges and
  handing over to the probes as the surface gets rougher. The picture is mipped: a rough surface reads it as
  blurred as its reflection spreads over the distance to what it meets. What's off screen or hidden falls back to
  the probes and the sky.
- **Screen-space global illumination** (Scene look > Screen-space GI, with a strength; off by default): light
  bounced off what's on screen nearby, so a lit wall brightens the ground beside it, a red sphere tints the floor
  and an emissive sign lights its surroundings. Each pixel marches one or two short rays (up to 4 m) over its
  hemisphere through the last frame's picture; what they meet (blurred) lights it, and where they meet nothing the
  probes' and the sky's light does. The picture holds last frame's bounces, so light keeps bouncing frame after
  frame. Reflections and GI are traced at **half size** before the lit pass (with MSAA and under a render scale
  too): the depth prepass also writes each pixel's normal and roughness, each half-size pixel traces one of its four pixels
  (a different one each frame, more GI rays than per pixel) and keeps a history of the others (followed by the
  motion vectors, dropped where the depth changed), and the lit pass brings them up to full size with a depth-aware
  filter. With MSAA the prepass draws them multisampled and averages them, and the traces read the depth's first
  sample; under a render scale they cover the drawn area alone. On see-through surfaces and water they're traced
  per pixel in the lit pass (the rays turning every frame). Off at low quality.
  Where GI is traced at half size, its rays also give **specular occlusion**: in a crevice, where they find things
  close all round, the surroundings' reflection is hidden too (more for rough surfaces).
- **Contact shadows** (Scene look > Contact shadows): a short ray towards the sun through the view's depth for
  each pixel (with the half-size traces), catching the contact the shadow maps are too
  coarse for: a tyre on the road, a box against a wall, the gaps between stacked crates. Its reach grows with
  distance (the same few pixels); about 3% of Night Drive's frame on lavapipe. The nearest point or spot light
  that casts shadows gets them too (a lamp over a table, a car's headlight on the kerb); the others don't.
- **HDR output** (View > HDR output, `VKWORLD_HDR=1`, or a game's `settings.hdr`): on a display (and driver) that
  takes it, the picture goes out as HDR10 (PQ, Rec.2020 primaries; else scRGB): highlights keep their brightness up
  to the display's peak instead of rolling off at white, and the UI sits at paper white (`VKWORLD_HDR_PAPER_WHITE`,
  200 nits) — it's drawn into a layer of its own and blended in by the composite, in linear light. The peak is the
  display's own where it says (DXGI on Windows; the EDID's HDR metadata on Linux, the brightest connected display's;
  the screen's EDR headroom on macOS), else 1000 nits; `VKWORLD_HDR_PEAK` overrides it. With VK_EXT_hdr_metadata
  the swapchain tells the display about the picture (its primaries, D65 white, the peak as its maximum content
  light) so the display's own tone mapping leaves it alone. The colour-blindness filter works in HDR too, in linear
  light (highlights over paper white keep their brightness). (`VKWORLD_UI_LAYER=1` uses the layered UI on an SDR
  display: a check.)
- **HDR screenshots**: `--screenshot shot.hdr` saves a Radiance `.hdr` of the scene's light before tone mapping
  (floating point, exposure applied; not bloom or the UI; with split screen, player 1's view), on any display.
  Usable as a sky image.
- **Texture streaming**: textures go to the GPU at most 256 pixels across at first; the scene shader reports, per
  texture, the finest mip level the screen needs (one pixel in sixteen, by its footprint), and the finer levels are
  uploaded when something needs them, a few megabytes a frame. Under a budget (512 MB; `VKWORLD_TEXTURE_BUDGET=<MB>`)
  the textures needed longest ago lose their finest levels first; ones not needed for three seconds go back to 256.
  A material's other maps follow its colour map; textures shown in the UI, terrains' layers and ones without
  mipmaps stay whole. The Inspector's Renderer section shows what's on the GPU. `VKWORLD_TEXTURE_STREAMING=0`
  uploads everything whole. (The images stay whole in memory on the CPU side; this saves GPU memory.)
- **Transparency** (glTF alpha modes): cut-out materials (Mask) have holes wherever their texture's alpha is below a
  cutoff (the built-in Grate texture has them); see-through ones (Blend) are drawn after everything solid, sorted
  back to front, with their opacity.
- **HDR**: the scene renders into a floating-point image; **bloom** (a 13-tap down / tent up mip chain, with a
  soft threshold) makes bright parts glow, and the final pass applies exposure and the Khronos **PBR Neutral**
  tonemapper (colours below about 0.8 come out as they are). Sky colour, fog, ambient, exposure and bloom are
  per-scene settings (Inspector, with nothing selected).
- **Anti-aliasing**: 4x MSAA by default; or **temporal** (each frame drawn with a different sub-pixel offset and
  blended with the frames before, found where they were and pulled towards the colours around each pixel so moving
  things don't smear: kept within their mean and spread, not just their range, so flickering and ghosting colours
  give way sooner). Where they were comes from **motion vectors**: the depth prepass also writes each pixel's
  motion since last frame, from each object's last transform, skinned joints and waves as well as the camera's,
  so a moving car or a running character keeps its detail (each pixel takes the motion of the nearest thing round
  it, so edges move with what's in front); the sky's comes from the depth. The history is read with a Catmull-Rom
  filter (a bilinear read would soften it a little more every frame it moves), and less of it is kept the faster a
  pixel moves (down to 80 %) and the further outside the colours round it the history lay, so fast motion shows
  this frame's detail. The result is sharpened (contrast-adaptive: more where the picture is soft, none where it's
  already sharp): players set how much with `settings.sharpness` (0..1, 0.3 by default; a fifth more when upscaling),
  `VKWORLD_SHARPEN=0..1` overrides it; the history left as it was so sharpening doesn't build up. It also smooths shading
  and thin lines, and costs much less than MSAA on
  slower GPUs. A project chooses what its game starts with (`"antialiasing"` in project.json, Project Settings; the
  editor uses it too until View > Anti-aliasing is set, which is remembered); players change it with the game state's
  `settings.antialiasing` (`msaa`, `taa`, `off` or `default`); `vkgame --aa msaa|taa|off` or `VKWORLD_AA=taa` override.
- **Quality** (`settings.quality` for players, `VKWORLD_QUALITY` otherwise): `high` draws everything the scene asks
  for; `medium` keeps the sun's shadows to 100 m, halves the lights' shadow maps and takes half the shadow taps,
  fewer reflection steps, ambient occlusion samples and fog shadow samples (about a quarter off the GPU's frame);
  `low` keeps shadows to 60 m with a fixed soft edge, and turns off screen-space reflections and ambient occlusion
  (about half off). F3 shows each pass's GPU time and their total. On a game's **first run** (no quality saved yet)
  vkgame times a second's worth of frames at high and picks the highest quality whose frame would fit 60 a second
  (medium at about two thirds of high's cost, low at half), and saves it (`settings.qualityChosen`; the player can
  change it after; `VKWORLD_AUTO_QUALITY=0` or `VKWORLD_QUALITY` skip it).
- **GPU benchmark**: `vkgame [scene] --benchmark [frames] [--benchmark-out results.json]` holds the scene's camera
  where it starts and turns it a whole circle over the run (after 30 frames' warm-up), then prints each pass's
  median GPU time, the GPU's frame and the frame on the clock, and exits. `tools/check_gpu_bench.py --game
  build/vkgame` runs it on Night Drive and the stress scene and compares the GPU's frame with this GPU's baseline in
  `tools/gpu_baselines.json` (by the driver's name for it; `--write` stores this machine's).
- **Render scale and upscaling**: the 3D scene can be drawn at a fraction of the viewport (50 % to 100 % of its width
  and height: down to a quarter of the pixels to shade) and brought up to full size, the UI staying sharp at full
  size. With temporal AA it's temporal upscaling: each frame's jittered pixels land on the full-size pixels nearest
  their samples and the full-size history keeps the rest, so still parts of the picture get close to native
  detail over a few frames; without, a sharpened bilinear upscale. *Dynamic resolution* picks the scale from the
  GPU's frame time to hold 60 frames a second (`VKWORLD_TARGET_FPS`), in steps of 5 %, never above the set scale.
  Players set them with `settings.renderScale` (0.5 to 1) and `settings.dynamicResolution`; the editor in View >
  Render scale; `VKWORLD_RENDER_SCALE=0.75` anywhere. Worth it on big and Retina screens (with `VKWORLD_HIDPI=1` on a
  Mac, 67 % costs about what one pixel per point did) and on slow GPUs.
- **Depth prepass**: opaque things' depth is drawn first with a cheap shader, then the lit pass shades only the
  pixel in front (a city's ground, roads, sidewalks and buildings stacked on each other are lit once, not four
  times). `VKWORLD_PREPASS=0` turns it off. Point lights are clustered over the depth range of what's in view.
- Distance fog, the selection outline (kept out of tonemapping), and a little dithering against banding.
- **Volumetric fog** (Scene look > *Volumetric fog*): the fog lit by what passes through it, shafts of sunlight
  between buildings (it looks up the sun's shadows), halos round street lamps and neon, headlight beams. A grid of
  160 x 90 x 64 cells through the view (slices spaced logarithmically over the depth of what's in view) is filled
  with each cell's density and in-scattered light (the fog's own colour, the sun, the point and spot lights of its
  light cluster, a Henyey-Greenstein phase: *forward scattering* makes glows round lights you look towards
  brighter), then summed from the camera out; surfaces and the sky are dimmed by what's in front and get its light.
  *Fog light* scales the lights' share, *height falloff* thins it with height (low-lying mist). It runs after the
  depth prepass, which gives each cell column the distance to what it sees: at the edge of a building a pixel weighs
  the columns beside it by how near their surface is to its own, so the cells inside the building (shadowed or lit
  differently) don't darken or light the street's edge. Each cell looks at a different point inside itself every frame
  and blends with what that place held the frame before (reprojected), so a beam's or a shaft's edge is smooth, not
  stepped by the cells. A few percent of a frame. The main view only (split-screen
  views and probes keep the plain fog).
- **Tone mapping and grading**: PBR Neutral (colours as they are) or **filmic** (AgX: richer contrast, highlights
  bleaching towards white as film's do), then contrast, saturation and a vignette (Scene look).
- **Auto-exposure and local tone mapping** (Scene look, off by default): with **Auto-exposure** the exposure follows
  the view's brightness (metered each frame on a 256x144 copy of the view: a histogram weighted towards the middle,
  averaged between its 50th and 95th percentiles, so a few street lights or a black corner don't swing it), easing towards it
  like an eye adjusting: three times quicker into a brighter view than into a darker one, at **Adaptation speed**,
  within an **Exposure range** (stops either side). The brightness aimed for falls with the view's own (an automatic
  key), so a night street stays a night street rather than lifted to grey; the scene's Exposure multiplies the
  result (compensation). **Local tone mapping** (0..1) lifts dark regions and holds back bright ones relative to the
  mean (by the coarse brightness around each pixel, edge-aware: neighbours of a very different brightness count
  less, so there's no halo along a dark wall against a bright sky), keeping detail in shadows and around lights.
  **Lens dirt** (0..1): smudges, specks and wipe marks on the lens that the bloom lights up round bright lights.
  **Light shafts** (0..2, off by default; without volumetric fog, which has its own): the clear air lit by the sun
  and scattering it towards you, brightest looking towards the sun, with dark streaks where something blocks it:
  each half-size pixel marches its view ray through the sun's shadow cascades, so what's in the way counts on screen
  or off. The four lamps that show most (bright, wide, near) light it too: spot lights' beams (their cones and
  shadows: headlights, a lamp's beam in the night air) and point lights' halos (worked out whole along each ray, so
  the bright core isn't missed between steps; a shadowed one's halo dimmed by what its shadows hide). The air dims
  what's behind it a little with distance (the sky's own air kept). Split-screen views get theirs, through their own
  cascades. Off at low quality.
  **Depth of field** (0..1, off by default): what's nearer or further than the focus blurs as through a lens, by
  its circle of confusion (a thin lens's: |distance - focus| / distance, at 1 up to about 1% of the view's height),
  bright spots opening into discs (bokeh); the focus is a distance (*Focus distance*) or an entity kept sharp
  wherever it goes (*Focus on*: a player, a car). The background doesn't spill over a sharp foreground, and a
  blurred foreground spreads over a sharp background (each pixel gathers as far as the largest near blur in its
  8 x 8 tile and those round it). **Motion blur** (0..1, off by default): the picture smears along each pixel's
  motion on screen (the depth prepass's motion vectors, turned on by it), at 1 over the whole frame's motion, at
  most a twentieth of the view; the sky smears with the camera's turn.
  Both run once after temporal AA, before bloom; off at low quality, the main view's alone. The main view
  meters; split-screen views use its exposure, without local tone mapping.
- **Physical sky and time of day** (Scene look > Physical sky, off by default): the sky worked out from the air,
  Rayleigh scattering (blue), Mie scattering (haze, the glow round the sun) and ozone, after Hillaire's 2020
  technique. Two small tables are made on the GPU, the light getting through the air and the sky around the viewer
  (redrawn only when the sun moves or the haze changes). The sun's light is coloured by the air it crosses (white at
  noon, red at sunset, gone below the horizon), and the sky's colours light the scene (the ambient's hemisphere, the
  fog) in place of the Sky / Horizon colours; the Ground colour stays (it's what the ground reflects). **Time of
  day** (hours) turns the scene's sun along an equinox's path at a **Latitude**, rising in the east (+x); **Sky
  turn** turns the compass, **Sky brightness** scales the sky. **Haze** whitens the sky and widens the sun's glow.
  **Aerial perspective** fades distant things into the air's colour in their direction (1: as thick as real air,
  barely visible over a small scene; 10 to 30 suit most). The sky is calibrated so a sun of intensity 1 lights the
  scene about as the default colours do. Real light makes for strong contrast: raise Ambient towards 1, or use
  auto-exposure. A sky image, if set, is drawn in place of the physical sky (the sun is still coloured by the air).
  Light scattered more than once comes from Hillaire's multiple-scattering table (the twilight's blue, the haze's
  glow), and the sky lights the scene through spherical harmonics of itself: brighter on the sun's side, blue from
  above, the ground's light from below. At night stars come out in the dark sky and the **moon** (opposite the
  sun) takes over as the light, dim and blue, its shadows too. **Clouds** (0..1 of the sky) are a layer 2 km up,
  drifting with the wind, lit by the sun through the air (red at sunset) and the sky, their shadows moving over the
  ground. They're in the sky's light too: as much of the ambient's harmonics as they cover (on average) takes their
  colour, so an overcast day's shade is grey and even rather than blue, and the ground's light is dimmed by their
  shadows on average.
- **Specular anti-aliasing**: where the normal changes within a pixel (small curved things, distant bumpy ground),
  highlights are widened to match, so they don't sparkle and crawl.
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

**Foot placement**: the built-in **FootIK** behaviour keeps a character's feet on uneven ground through its
Animator's two-bone chains (one per foot, named *LeftFoot* / *RightFoot* by default): each foot goes up or down by
how much higher or lower the ground under it is than under the character, and the hips (a bone you name) drop
when a foot needs to reach lower ground.

**Retargeting**: a clip made for one model can be played by another with a different rig. In the Animator
inspector, *Retarget a clip* takes the clip and the model it was made for (its prefab) and makes the clip for this
one: bones are paired by name (rig prefixes, sides and the usual humanoid names understood: Mixamo's
`mixamorig:LeftUpLeg` pairs with `Thigh_L`), each turns as much as its partner turns away from its rest pose, in the
world (so differently oriented bones still bend the same way, when both rest poses are alike), and the hips carry
the travel, scaled by their height. The scene saves how the clip was made and makes it again when it loads. Its
*Bone map* shows the pairing for every bone of this model and lets you change it (or pair none); *Match rest
poses* is for skeletons that stand differently at rest (an A-pose model and a T-pose one): each bone is first
lined up with its partner's rest direction, so the clip looks the same on both. *Plant the feet* (on by default)
fixes what copying turns can't: legs of other proportions (a long thigh, a short shin) put their feet somewhere else
than the source's, so planted feet slide; with it each leg (thigh, calf, foot, by humanoid names) is bent by
two-bone IK so its foot lands where the source's does, scaled to this model's height, the knee bending the way it
did and the foot keeping its turn.

**Mirroring**: *Mirror the clip* makes the clip mirrored left to right for this model (a left-handed swing from a
right-handed one, a limp on the other leg): each bone moves as its other side did (paired by humanoid names, or by
Left / Right, .L / .R, _l / _r in the name), mirrored across the model's middle, whatever the bones' own axes (the
rest pose has to be symmetric); bones without a side move as before, mirrored, and the hips' travel and turn are
mirrored too. Saved as how it was made, like a retargeted clip.

**Compression**: imported clips drop the keys their neighbours' interpolation gives back anyway (to within 0.05
degrees and half a millimetre): smaller clips, the same motion.

**Animation events** are named times in a clip (a footstep, the frame a sword hits): when playback passes one,
the entity's behaviours get `onAnimationEvent(ctx, self, "Footstep")`. In a blend tree the clip that weighs most
fires its events (at the same point of its cycle), and layers' clips fire theirs. An event can also play a
**sound** by itself (a built-in one or a file, at a volume) at a **bone** (an entity under the Animator's: a foot
for footsteps), a burst of **particles** there (from the bone's Particle Emitter, else the Animator entity's, with
its look: dust from a foot), and a **camera shake** (0.3 a heavy step, 1 a slam; weaker the further the camera is,
none past 25 m), no code needed. The Animator inspector's event track shows the clip's events as marks over its
length: drag one to move it, double-click to add one there, click to move the preview.

Outside Play, the Animator inspector previews its clip: play / pause, and a slider to scrub to any time (only
what's drawn changes, not the scene). New events are added at the preview time. Events fire as the preview passes
them (playing, or scrubbing forwards): their sounds play, their particles show, and their marks flash on the track;
in Play the track shows where the Animator is, its events flashing as they fire.

**Camera shake** from code: `ctx.shakeCamera(0.5f)` (Lua `game.shakeCamera(0.5)`), or from a place in the world,
weaker with distance: `ctx.shakeCamera(0.8f, explosion, 30.f)`. It's "trauma" (0..1, added up, fading in about two
thirds of a second from full), shaking by its square: the camera turns (a few degrees at most) and moves (a few
centimetres) by smooth noise in game time, so a paused game holds still.

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
blending. **Stretch** draws them as streaks along their motion (as long as that many seconds of it: rain, sparks
flying) instead of dots; **Lit** makes them take the scene's light (the sun, the sky's, and the point and spot
lights of their part of the view, scattered mostly forwards; no shadows) instead of glowing: rain that shows in the
headlights and under the lamps and is dark elsewhere. **On the GPU** simulates an emitter in a compute pass:
thousands of particles cost little, and drops that hit what's on screen splash there (with screen-space
reflections or GI on: they find it in the last frame's picture); see-through ones (smoke, steam, dust) are sorted
back to front on the GPU too, and drawn after the CPU's see-through particles. Game code's bursts from it still
come from the CPU. Night Drive's rain and manhole steam both run on the GPU. Particles are **soft**: they fade where they meet the surfaces
behind them instead of cutting a hard line (the solid picture is copied for it mid-frame, as for water). Particles stay where they were emitted and outlive their entity. The editor previews emitters outside
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
  Sources can stream too (long ambience). A scene can have its own **music** (Inspector with nothing selected,
  *Sound*: a track, its volume, a cross-fade): it starts with the scene and cross-fades from what was playing when
  the scene is loaded, and a next scene with the same track lets it carry on without a seam.
- **Reverb Zone** component: while the camera is inside one, effects and ambience echo (wet, room size, damping),
  easing in at its edge. **Reverb from the world** (the scene's *Sound* settings) works it out instead: 26 rays
  from the camera five times a second measure how closed in it is and how far the walls are, so corridors and halls
  ring and open ground stays dry, with *Reverb amount* to taste; zones take over where there are some.
- **Doppler**: moving 3D sounds (and a moving camera) shift in pitch.
- **Occlusion**: a solid collider between the camera and a 3D sound muffles it (fewer highs, quieter), easing as
  things move; the Audio Source's *Occlusion* says how much (0: heard through walls). Five rays go to the sound and to
  points half a metre around it, so a sound round a door's edge or over a low wall is only partly muffled.
- **Headphones**: binaural panning for 3D sounds, from a spherical-head model rather than measured HRTFs: the far
  ear hears a sound up to 0.66 ms later and muffled by the head, sounds behind are a little duller, and height comes
  through the outer ear's notch (a dip that rises in pitch, 6 to 10.5 kHz, as the sound goes from below to above;
  sounds below are duller too). It tells left from right, front from back and up from down on headphones; on
  speakers (the default) sounds pan with equal power.
- **Layered music and stingers**: `ctx.playMusicLayers({"music/base.ogg", "music/drums.ogg"}, {1.f, 0.f}, 1.f,
  120.f)` starts stems of the same length together; `ctx.setMusicLayer(1, 1.f, 2.f)` fades the drums in (a fight
  starting). `ctx.playStinger("music/hit.ogg", "beat")` plays a short phrase on the music's next beat (or "bar", or
  "now"), with the music dipping under it. MusicPlayer has `layers` (comma-separated), `layerVolume`, `bpm` and
  `beatsPerBar`; Lua has `game.playMusicLayers`, `game.setMusicLayer` and `game.playStinger`.
- **Player settings**: master, music, effects, UI and ambience volumes, headphones, rumble and the accessibility
  options live in `settings.json` in the game's save folder, read when the game starts and saved a second after a
  change. They appear in the game state as `settings.masterVolume`, `settings.musicVolume`, ..., `settings.headphones`,
  `settings.rumble`, `settings.highContrast` (UI layouts drawn with opaque black boxes, white text and a thick
  yellow outline on the focus) and `settings.colorBlind` (`none`, `protanopia`, `deuteranopia` or `tritanopia`: a
  filter over the 3D view that moves what that eye can't tell apart into differences it can), so a menu's sliders and
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
- **Soft bodies**: a **Soft Body** component makes **cloth** (a sheet of particles, pinned along its top edge, at
  its top corners or nowhere; how stiff and how freely it folds) or turns the entity's mesh into a **squashy**
  volume (jelly, a cushion; *Pressure* inflates it into a firm ball). Simulated by Jolt with everything else (it
  collides, things land on it); drawn with the Mesh Renderer's look, its mesh following the particles. A cloth or
  a mesh with *Tears at* set (1.5: an edge stretched half again its length) tears: those triangles go, the rest
  carries on (a torn balloon loses its pressure). In water a soft body's own *Buoyancy* pushes up the particles
  under the surface (1 hovers, more floats).
- **Water**: a **Water** component (Add > Water, or Add component > Water) is a body of water: its *Width*,
  *Length* and *Depth* below the entity, *Wave height*, *Wave length*, *Wave direction* and *Choppiness* (sharper
  crests, foam on them) for Gerstner waves, a *Current* (metres per second, in world space: a river) and the
  water's *Colour* and *Clarity*. It's drawn as a moving surface (the same waves on the GPU, with Fresnel
  reflections and foam) and things in it float on the waves and drift with the current and the waves' motion.
  What's under the surface shows through it, **refracted**: bent by the waves' slopes, and dimmed by the water it
  passes through towards the water's colour (the colours it lacks go first; murkier with less Clarity), so the
  shallows are clear and the deep end is coloured. **Foam** lines the shore, wherever the water meets something
  close beneath it (broken up, drifting), and sunlit surfaces under the water get moving **caustics** (fading
  with depth; under a bridge, none: they follow the sun's shadows). With screen-space reflections on, it reflects
  what's on screen. With the camera **under the surface**, everything is seen through the water, absorbed towards
  its colour with the distance (darker the deeper), and the surface from below shows what's above through Snell's
  window, the water's own colour beyond it. (To refract, the scene's pass stops once the solid things are drawn
  and copies the picture: a pass more when water or particles are in view, in split-screen views too; reflection
  probes see water see-through. See-through things behind water are hidden by it.)
- **Buoyancy**: rigid bodies in a Water volume (a Water component, or a trigger box tagged **Water**) float or sink
  by their Rigid Body's *Buoyancy* (1: neither; a cork 2, a stone 0.4), with drag in the water.
- **Fracture**: a **Fracture** component cuts the mesh into pieces ahead of time (as many as set; another *Seed*,
  other cuts). It breaks into them on an impact faster than its *Break speed*, when a Destructible on it runs out
  of hits, or on `ctx.shatter(id)`: rigid pieces (tagged Debris) flying apart, cleared away after their *Piece life*.
  Concave meshes cut cleanly too (the cut faces follow the outline, holes kept), and a concave piece collides as
  several convex parts. With *Hold together* it cracks instead: the pieces stay joined (each by a fixed joint to one
  it touches) until a force past *Hold strength* knocks them off; a piece's life starts once it's off.
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
  `*-crash-last.txt` and says so (the editor in its status bar, the game in its log). A report has the version and
  commit (`vkworld --version` / `vkgame --version` print them too), the game's name and version, the stack, and
  (Linux, macOS) the modules loaded with their build ids, so it can be read back later:
  `python3 tools/symbolicate.py game-crash.txt --symbols <folder>` puts the function, file and line after each
  frame. Releases keep their symbols aside: configure with `-DVKWORLD_RELEASE_SYMBOLS=ON`, then
  `python3 tools/symbols.py split --out symbols <binaries>` strips what ships and keeps the debug info by build id
  (macOS: `.dSYM`s; Windows: the `.pdb`s). Without symbols it still names functions from the binaries; Windows names
  them in the report itself when the `.pdb` is next to the program.
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
  `@toggle <entity>` (menus that open other menus), `@save` / `@loadgame <slot>`, `@set <key> <value>` (a game
  state value: a number, `true` / `false` or a word) and `@cycle <key> <a> <b> ...` (the next of those values:
  `@cycle settings.colorBlind none protanopia deuteranopia tritanopia` on a button showing
  `Colours: {settings.colorBlind}`).
- **Slider**, **Toggle**, **Text field**: they show and change a **game state** value (`ctx.state()`, `game.state`
  in Lua): a number in a range, a flag, a text.

Texts are formatted like `HudText`: `{@key}` from the player's language, `{name}` from the game state. Menus work with
the **keyboard and gamepad**: arrows / d-pad move to the nearest element that way, Enter / Space / A presses,
Left / Right move a slider; the mouse works too (and is freed while a menu is up). While a text field is being typed
into, the game's own controls don't see the keys. A text field opened with the gamepad (A) brings up an **on-screen
keyboard**: the d-pad picks a key, A types it, B deletes, X is a space, Y is Shift, Start or Done finishes (the
mouse can click its keys too). The sample game starts on a menu made this way
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

**Game > String tables** edits them all at once: a row per key, a column per language (*Add language* makes a
`strings/<code>.json`), empty cells marked red with a count of what each language lacks, a filter and *Missing
only*. Text in scripts the editor's own font can't show (Arabic, Hebrew, CJK, Thai...) is drawn in its cell as the
game draws it, shaped and joined, and the selected text is previewed below at size, right-aligned when it runs right
to left, in the game's font or one from `fonts/` (letters it lacks from `fonts/fallback/`, then the system's). *Save*
writes every language back (nested by the dots). The Input bindings window shows pad bindings as the glyphs games
show (Xbox, PlayStation or Switch) and marks actions that share a control with another.

HudText, HudBar and PauseMenu translate `{@key}`s (the pause menu's buttons use `pause.resume`, `pause.save`,
`pause.load`, `pause.quit`). The project's language (project.json `"language"`, Game > Language in the editor) is
what the game starts in; `vkgame --language es` overrides it. The sample game has English and Spanish.

**Every script**: text in the game's UI that needs it (Arabic, Hebrew, the Indic scripts, Thai, combining marks, CJK,
emoji) is **shaped** with HarfBuzz: letters join and take their contextual forms, ligatures form, marks sit on their
letters, and right-to-left text runs right to left (the Unicode Bidirectional Algorithm: a line's direction is its
first strong character's, numbers keep their order inside right-to-left text, and the explicit controls work:
embeddings and overrides, LRE RLE LRO RLO PDF, and isolates, LRI RLI FSI PDI, which keep a name or a number in a
sentence from taking the sentence's direction). Latin, Greek and Cyrillic text is drawn as before. Characters the
font lacks come from **fallback fonts**: the project's own first (`fonts/fallback/*.ttf`, `.otf` or `.ttc`, in name
order: ship the fonts your languages need, the safest way), then the system's for that script (fontconfig on Linux,
the usual Windows and macOS fonts), loaded the first time the script turns up. Alignment stays the game's choice
(`Align::Right` for right-to-left menus).

**Colour emoji**: emoji fonts' colour glyphs are drawn in colour, all three kinds: PNG bitmaps in CBDT (Noto Color
Emoji, Linux's and Android's), Apple's sbix (Apple Color Emoji) and COLR version 0 layers (Segoe UI Emoji), read
by the engine itself (`game/color_glyphs`) and scaled to the text's size. Skin tones, flags and joined sequences
come out as one emoji (HarfBuzz forms them); a character followed by U+FE0F (❤️) is taken from a colour font first;
an emoji's space isn't (emoji fonts' spaces are as wide as an emoji). Colour glyphs keep their colours (the text's
colour sets only their transparency) and cast no drop shadow.

**Wrapping**: Text and Panel elements wrap text that's wider than they are (or has line breaks) onto several lines,
centred in the element; `ui.textBox(x, y, w, text, size, color, align, font)` draws wrapped text from Lua (returning
its height; `ui.textBoxHeight(w, text, size, font)` measures it). Lines break where the Unicode Line Breaking
Algorithm (UAX #14) allows: after spaces and hyphens, between CJK characters, not before closing punctuation or a
full stop, not inside numbers ("-3.5", "$20"), emoji sequences or flags; a word too long for a line is cut between
characters. **Text fields** have a caret: the arrow keys move it (on screen: through right-to-left text from right
to left), typing and Backspace / Delete happen where it is, Home and End jump to the ends.

Not there: COLR version 1 emoji (gradients; their outline fallback is drawn), line breaking inside Thai, Lao, Khmer
or Myanmar words (they need a dictionary: they break at spaces), bidi per paragraph across wrapped lines (each line
is resolved on its own), shaping in the editor's own panels, vertical text.

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

In the editor, Game > Host a multiplayer game hosts while you play. A client loads the scene the host is playing.

- **Players**: the scene's Multiplayer settings (Inspector, nothing selected) name a **player prefab**: each player
  who joins gets one (spread around a spawn point), and so does the host. On the server, a player's behaviours read
  *that player's* input as their `ctx.input()` (the client sends its input actions every update), so a
  `PlayerController` works for everyone unchanged. The game looks through a camera under the local player.
- **Replication**: entities with a **Replicated** component (players always) are sent to every client 20 times a
  second: where they are, whether they're visible and (optionally) their behaviours' fields; ones spawned or
  destroyed on the server appear and go on the clients too. The **game state** (`ctx.state()`) is shared the same
  way. Clients show replicated entities where the server has them (smoothed between updates) and don't simulate
  them; everything else runs on each machine as usual (effects, UI).
- **Delta snapshots**: a client acknowledges each snapshot it gets whole, and the next ones only carry what changed
  since (an entity that didn't change costs its id); lost packets just mean the next snapshot carries a bit more.
- **Interest**: set *Relevance* in the Multiplayer settings and a client only gets the replicated entities that
  near its player (the others are hidden on it until they come near): big worlds send far less.
- **Prediction**: a client moves its own player at once with its own input. When the server says where the player
  was after input *n*, the client takes that and adds back what its later inputs (not yet seen by the server) did:
  the server's corrections (a knock back, a wall the client didn't know about) show at once, without the player
  jumping back to where it was.
- **Lag compensation**: while a remote player's behaviours run on the server, the other replicated entities with
  colliders are put back where that player saw them (its round trip, the time its input waited and the client's
  smoothing ago), so a ray it fires at what was on its screen hits it.
- **Joining late**: a player joining a game in progress gets the scene the host is in and everything destroyed
  before they came (picked-up items stay picked up).
- **Encryption**: every session is encrypted and authenticated. Joining, the client and the server exchange X25519
  keys; from then on every packet both ways is sealed with ChaCha20-Poly1305 (a key each way, a counter per packet),
  so nothing can be read on the way, and changed, forged or replayed packets are dropped.
- **Passwords**: `--password secret` on the host and on the players (or the `@password` UI action before
  `@host` / `@join`): the password goes into the session's keys, so a player with a different one can't open
  anything and is turned away ("Refused: wrong password", `net.refused` in the game state); five wrong ones from an
  address in a minute and it waits a minute.
- **Messages between machines**: `ctx.sendToServer(id, message)` on a client reaches entity `id`'s behaviours on the
  server (`onMessage`, with `message.client` saying which client sent it); `ctx.sendToClients(id, message)` on the
  server reaches it on every client (or one: `sendToClients(id, message, client)`). They're numbered, resent until
  acknowledged and delivered in order, once. On the host (and offline) they're delivered there too, so a game works
  the same alone; `ctx.isServer()` / `ctx.isClient()` say which this is. Lua: `game.sendToServer(e, name, value,
  text)`, `game.sendToClients(e, name, value, text, client)`, `game.isServer()`.

- **Dedicated servers**: `vkgame path/to/MyGame --server` (or `--server 4000`) runs the game with no window and no
  GPU (a server machine needs neither), at 60 updates a second, with no player of its own: everyone who joins is a
  client. It logs who joins and leaves; Ctrl+C (or SIGTERM) stops it, saying goodbye to the players.
- **Lobbies and NAT traversal** through a **relay**: `vkrelay` (built with the engine, in the releases; standalone,
  no GPU or engine library) is a small server for a machine with a public UDP port (27100 by default:
  `vkrelay [port]`). Name it in `project.json` (`"relay": "relay.example.com"`, or `--relay host[:port]`) and a host
  (`--host`, `--server`, the editor's Game > Host) lists its game there and gets a six-letter **lobby code**, shown in
  its status (`--lobby "name"` names the lobby, `--max-players N` says how many it takes). Players join with
  `--join-lobby K7QX2M`, list the game's lobbies with `vkgame path/to/MyGame --lobbies`, or do either from the game's
  menus (below). The relay tells host and player where each other is as the internet sees them and both punch
  through their NATs (most home routers then let the game's packets straight through); if nothing gets through
  directly within 2 s, everything between them goes through the relay instead (only between peers it introduced).
- **Private lobbies and the relay's secret**: `--private` (the `@private 1` UI action) hosts a lobby that isn't
  listed, only joined by its code; lobbies with a password say so in lists (`net.lobby.0.locked`). `vkrelay
  --secret S` (or `VKRELAY_SECRET`) only takes lobbies from games that know the secret (`"relaySecret"` in
  `project.json`, or `--relay-secret S`): the relay challenges a host with a random number, and the host answers
  with a hash keyed by the secret (never the secret itself). The relay limits each address's packets and bytes a
  second (a flood is dropped, not forwarded) and how many lobbies it hosts at once.
- **From the game's menus**: UI actions `@host [port]`, `@join address`, `@joinlobby code` (formatted like texts:
  `@joinlobby {codeField}` takes a text field's value), `@lobbies`, `@leave`, `@password text` and `@private 1`;
  vkgame reports back in the game state:
  `net.status`, `net.active`, `net.hosting`, `net.connected`, `net.code` (a host's lobby code), `net.searching`,
  `net.lobbiesError`, and the lobbies last listed: `net.lobbies` (how many), `net.lobby.0.name`, `.code`, `.players`
  ("2 / 8") and so on (a List of buttons with `@joinlobby {net.lobby.0.code}` makes a lobby browser).

```sh
./vkrelay                                                  # on a server with a public address
./vkgame MyGame --server --relay relay.example.com --lobby "Friday night"   # lists "Friday night": code K7QX2M
./vkgame MyGame --relay relay.example.com --join-lobby K7QX2M
```

Not there yet: scene changes and save games while connected (after joining), IPv6, checking who the server is (a
man in the middle who answers the key exchange himself can read a game without a password; with one he can't, but
he can try passwords offline against what he saw: use long ones), and accounts (a player is who their address is).

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
  (rigid bodies), `color` (mesh renderers), `particleRate` (particle emitters: particles a second); vectors are copies: change one and assign it back. Methods:
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
  `axis(negative, positive)`, `mouseDelta()` (returns x, y), `rumble(low, high, seconds)`, `setLight(color)`.
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

**Time limit**: a callback running for more than half a second (an endless loop, say) is stopped with an error,
like any other script error, instead of hanging the game (`scripting::setTimeLimit(seconds)`; 0: none).

**Debugging**: turn on Game > Lua debugger in the editor (or run `vkgame --lua-debug [port]`, 4711 by default) and
attach from VS Code with the small extension in `tools/vscode-vkworld-lua` (its README says how to install it):
breakpoints in `scripts/*.lua`, stepping over, in and out, pausing, the stack with each frame's locals and upvalues,
tables to expand, watches and hovers (expressions evaluated in the frame). It speaks the Debug Adapter Protocol,
so other editors with DAP support can attach too. While stopped, the game waits: its window doesn't redraw until
you continue.

- **Conditional breakpoints** stop only when their expression is true (`self.health < 10`, evaluated in the frame);
  a **hit count** says which hits stop (`5` or `>= 5`: from the fifth on, `== 5`, `% 3`: every third); a condition
  that fails to run (an error in it) stops, saying why in the debug console.
- **Logpoints** print their message without stopping, `{expressions}` filled in (`hp {self.health} at {dt}`;
  `{{` and `}}` for braces): in the editor's debug console and the game's log.
- **Script errors**: with the *Script errors* exception filter on (the default, in VS Code's Breakpoints view), the
  game stops where a script raises an error, before it's reported: the frame that raised it, its locals, the message
  (and its traceback in the exception's details). Continuing reports the error as usual.
- **Setting variables** while stopped (VS Code: *Set Value* on a variable): a local, an upvalue or a table's field
  (`self.step`) takes an expression's value, evaluated in its frame (`before // 10`); the script goes on with it.
- **Data breakpoints** (*Break on Value Change* on a table's field, `self.count`): the game stops on the line after
  the one that changed it, saying what it is now (`count changed: now 4`). Locals can't be watched (they're gone once
  their function returns): watch a field.
- **Coroutines** are threads of their own (in VS Code's Call Stack): a breakpoint inside one stops there, with its
  stack and locals; the main thread's stack shows where it was resumed; suspended coroutines can be looked into
  (their stack, where they yielded, and their locals). `coroutine.create`, `wrap` and `resume` are tracked for this.

**Profiling**: every script callback is a profiler scope named after the script and the callback (`Door.update`,
all its instances' calls together), so they appear where they run in the profiler window (View > Profiler, F3), in
saved traces, and in its *Scripts* section, slowest first with how many calls each made last frame. *Sample Lua
functions* there tells where inside the scripts the time goes: every millisecond a script runs, the function and
line running then get a sample (a thread of its own asks for it), listed by function (`Door.lua:12 open`) or, with
*By line*, by line (`Door.lua:15`), with their milliseconds and share. It's wall-clock time: a slow call into the
engine (a raycast, `string.rep` of megabytes) counts against the line that made it, for as long as it took.

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

## Releases

`.github/workflows/release.yml` builds a release when a tag `v<version>` is pushed (the version in
`CMakeLists.txt`'s `project()`; the tag must match it): optimised with debug info on Linux, Windows and macOS,
tested, the symbols split off into `*-symbols` archives, and packaged as an AppImage and `.tar.gz` (Linux), a `.dmg`
(macOS) and an installer and `.zip` (Windows), published as a GitHub release. Run by hand (Actions > Release > Run
workflow) it builds the same packages as artifacts only. Every release has a `SHA256SUMS` file. With the
repository's signing secrets set, Windows programs, libraries and the installer are signed (`WINDOWS_CERT_PFX` as
base64, `WINDOWS_CERT_PASSWORD`), and macOS binaries are signed with the hardened runtime and the `.dmg` notarized
and stapled (`APPLE_CERT_P12`, `APPLE_CERT_PASSWORD`, `APPLE_SIGNING_ID`; `APPLE_ID`, `APPLE_TEAM_ID`,
`APPLE_APP_PASSWORD` for notarizing); without them the packages go out unsigned and the run says so. The editor's
**Help > Check for updates** asks GitHub for
the latest release (only when you click it; `-DVKWORLD_UPDATE_URL=` points it elsewhere); **Help > About** shows the
version and commit.

## Code

The code is split into an **engine** shared library (everything a game needs at runtime, Dear ImGui included) and
the **editor** application (`vkworld`) built on top of it; games add their own code as modules (`examples/`).
Includes are relative to `src/`.

| Folder / file              | What                                                            |
|----------------------------|-----------------------------------------------------------------|
| `src/main.cpp`             | The editor's entry point                                        |
| `src/runtime/main.cpp`     | The standalone game (`vkgame`)                                  |
| `src/app/`                 | Window + renderer + ImGui setup and frame loop shared by both; exporting (and packaging) games (`export.*`); update checks (`updates.*`) |
| `src/core/`                | `vecmath.h` vector/matrix helpers, `json.*` JSON reader/writer, `png.*` screenshot writer, `project.*` project folder and paths |
| `src/platform/`            | OS layer: `platform.h` interface; Linux (`platform_linux.cpp`: gamepads, dialogs, and the window backend chosen at start: `platform_xcb.cpp` or `platform_wayland.cpp`), Win32, Cocoa (macOS) implementations; ImGui XCB backend |
| `src/render/`              | Vulkan renderer: device, swapchain, pipelines, frame (`renderer.cpp`); textures and materials (`renderer_textures.cpp`); culling, batching, skinning data (`renderer_instances.cpp`); HDR, bloom, tonemapping (`renderer_post.cpp`); scene -> draw list (`scene_draw.*`) |
| `src/assets/`              | Mesh library (procedural meshes, OBJ import, picking), texture library (`textures.*`), glTF import (`gltf.*`), asset ids and import settings (`asset_db.*`) |
| `src/scene/`               | Entities, components and hierarchy (`scene.*`); behaviour base class and registry (`behaviour.*`); JSON save/load (`serialization.*`); terrains (`terrain.*`); new projects from templates (`project_templates.*`) |
| `src/audio/`               | Sound playback (miniaudio), built-in synthesised clips          |
| `src/physics/`             | `physics.*`: the scene's physics on Jolt Physics: rigid bodies, raycasts, overlaps, triggers, characters |
| `src/game/`                | Game runtime: fixed-timestep loop and GameContext (`game.*`), input actions (`input.*`), button prompts (`prompts.*`), game UI and fonts (`ui.*`, `fonts.*`, text shaping and bidi in `text_shaping.*`), built-in behaviours, game module loading (`module.*`) |
| `src/script/`              | `lua_script.*`: Lua behaviours (the scripts/ folder, the Lua API, hot reload)   |
| `src/net/`                 | `udp.*` sockets, `net_session.*` multiplayer: hosting, joining, replication, remote input; `relay.*` lobbies, NAT traversal |
| `src/relay/main.cpp`       | The relay server (`vkrelay`)                                    |
| `src/editor/`              | Editor app: `editor.cpp` frame/play/operations/undo, `editor_panels.cpp` menus, toolbar, status bar and dialogs, `editor_outliner.cpp`, `editor_inspector.cpp`, `editor_viewport.cpp` navigation/gizmos/picking, `editor_assets.cpp` project, asset browser, game module and export, `editor_asset_db.cpp` asset inspector, moving / renaming assets, `editor_terrain.cpp` terrain tool, `editor_prefs.cpp` preferences saved per user |
| `tests/`                   | Engine unit tests (`test.h` is the tiny framework)              |
| `examples/sample_game/`    | A project with its own game module (`src/`), scene and `project.json` |
| `examples/night_drive/`    | A small Lua-only game: a taxi in a night city of Kenney's City Kit models |
| `tools/`                   | `check_screenshot.py` (CI smoke test), `check_perf.py`, `symbols.py` (keep a release's symbols aside), `symbolicate.py` (read crash reports), `vscode-vkworld-lua` |
| `bench/`                   | `engine_bench`: CPU timings on the stress scene (`scene/stress_scene.*`) |
| `shaders/common.glsl`      | Frame data (camera, sun, light clusters, shadow cameras) and per-object instance data shared by the shaders |
| `shaders/scene.*`          | Lit, textured, shadowed objects; checker ground; fog            |
| `shaders/shadow.vert`      | Shadow map (depth only)                                         |
| `shaders/post.vert`, `bloom_*.frag`, `composite.frag` | Bloom down / up passes; tonemapping and gamma onto the swapchain |
| `shaders/atmosphere.glsl`, `sky_transmittance.frag`, `sky_view.frag` | The physical sky's model and its two tables (`render/atmosphere.*`: the same on the CPU, `renderer_sky.cpp`) |
| `shaders/luminance.frag`, `adapt.frag` | Auto-exposure: the view's log luminance (mipped down to its mean), the exposure eased towards its target |
| `shaders/outline.*`        | Selection outline (screen-space extruded hull + stencil)        |
