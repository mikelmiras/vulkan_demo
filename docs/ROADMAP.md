# Engine roadmap

Working list for turning the renderer demo into a small game engine. Each task is one commit.

| # | Task | Status |
|---|------|--------|
| 4 | Stress scene + frame stats; id index, physics broad-phase, frustum culling, instancing | done |
| 1 | Game modules: project code as a shared library loaded by editor and runtime | done |
| 2 | Runtime scene flow: loadScene, quit, persistent game state, save/load | done |
| 3 | Game UI API for behaviours (HUD text, bars, images, menus) | done |
| 5 | Skeletal animation: skinning, glTF animation import, Animator component | done |
| 6 | Rendering: PBR materials, normal maps, emissive, HDR + tonemapping, bloom | todo |
| 7 | Physics: rotational dynamics (or Jolt behind PhysicsWorld) | todo |
| 8 | Export game: distributable folder (vkgame + shaders + assets + module) | todo |
| 9 | Hygiene: lavapipe smoke test in CI, split large files, scene-format migration tests | todo |

## Notes

- Task 4 (stress scene, 4000 statics / 300 bodies / 200 triggers, `engine_bench`): fixed update 20.4 ms -> 4.7 ms,
  1000 raycasts 83 ms -> 1.3 ms, draw list 9.7 ms -> 0.2 ms; simulation checksum unchanged. Remaining cost is
  mostly the physics world being rebuilt twice per update (world matrices + tree): an incremental update would be
  the next step. Mesh colliders still test every triangle (no per-mesh tree).
- Rendering can be checked headless: `xvfb-run -a vkworld --screenshot out.png` (lavapipe).
- Task 1: the engine is a shared library now (ImGui inside it, WINDOWS_EXPORT_ALL_SYMBOLS on Windows) so modules
  share its registry and ImGui context. Modules are loaded from a temp copy (rebuildable while loaded). Unknown
  behaviour types load as MissingBehaviour placeholders (no more load failures). Windows/macOS module loading
  is untested here (no CI access from this machine).
- Task 2: ctx.loadScene / quit / state() / saveGame / loadGame; Behaviour::state() for runtime state in saves;
  built-ins SceneExit and SaveLoadKeys; scene format 9 (quicksave / quickload actions added to older files).
  The sample game has two levels with the score carried over.
- Task 3: Behaviour::drawUI + GameUI (canvas 1080 high; text, rect, border, bar, image, button; own hit-testing);
  ctx.setPaused / paused; InputMap::pressedThisFrame; HudText / HudBar / PauseMenu built-ins; GameState::format;
  scene format 10 (pause action); module API 3. Images use the texture's descriptor set as the ImTextureID.
- Task 5: GPU skinning (SkinVertex stream at binding 1, joints SSBO at set 0 binding 3, instance params.w = first
  joint); Skin / Animator components (joints and targets by name under the model root); clips in
  MeshLibrary::animations, re-imported from "file#animN"; cross-fades; scene format 11; module API 4. Skinned meshes
  aren't frustum-culled. Animation runs at the fixed 60 Hz (no render interpolation). No editor preview outside Play.
