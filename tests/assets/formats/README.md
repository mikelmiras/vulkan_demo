Scene files in every format the engine has written, each saved by the engine of its time (its demo scene, from
the commit that introduced the format; see tests/test_formats.cpp). They must keep loading: don't edit them.

| File            | Format | Saved by commit                                        |
|-----------------|--------|--------------------------------------------------------|
| scene_v2.txt    | 2 (text) | 879252a Add play mode with a fixed-timestep game loop |
| scene_v3.vkw    | 3      | 2e3d07c Replace scene objects with entities, components and a hierarchy |
| scene_v4.vkw    | 4      | 35243d3 Add prefabs and runtime spawning               |
| scene_v5.vkw    | 5      | 524d003 Add textures, materials, lights, shadow mapping and glTF import |
| scene_v6.vkw    | 6      | 64b8fd9 Link prefab instances to their prefab, with overrides |
| scene_v7.vkw    | 7      | 1616f21 Add rigid-body dynamics                        |
| scene_v8.vkw    | 8      | 24732d0 Add audio: miniaudio playback, audio sources and game sounds |
| scene_v9.vkw    | 9      | 1de4354 Add scene flow: scene changes, game state, save games, quitting |
| scene_v10.vkw   | 10     | 237550f Add a game UI API: HUD and menus drawn by behaviours |
| scene_v11.vkw   | 11     | 65d8cf8 Add skeletal animation: glTF skins and clips, GPU skinning, Animator |
