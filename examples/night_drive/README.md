# Night Drive

A very small game for the engine: a taxi on a dark, wet 1980s New York-ish street grid at night, seen from above at
a fixed angle (the way GTA: Chinatown Wars or The Precinct look), the camera easing after it.

- Drive: W / S or the up / down arrows to accelerate and brake (then reverse), A / D or left / right to steer, Space
  for the handbrake; on a gamepad, the left stick and A.
- The city is made by `tools/make_city.py` (a fixed seed: the same city every time). Change its numbers (blocks,
  street width, building heights, lamps, colours) and run `python3 tools/make_city.py` to remake `scenes/city.vkw`.
- The camera is `scripts/ChaseCamera.lua`: distance, pitch, yaw, how quickly it follows and how far it looks ahead
  are fields you can change in the inspector while playing.

Run it with `vkgame examples/night_drive`, or open the folder in the editor and press Play. The streets, buildings and lamp posts are Kenney's City Kit
models (see below); everything else is built-in: a Jolt vehicle with the VehicleDriver behaviour, point and spot lights (street lamps, neon signs,
headlights), emissive materials (windows, signs), screen-space reflections on the wet asphalt, bloom, fog and steam
particles from the manholes.

## Models

`models/city/` (City Kit Commercial) and `models/roads/` (City Kit Roads) are by Kenney (www.kenney.nl), CC0: see the
`License.txt` in each. The generator lays the buildings round each block facing the streets, tinted for the night,
with `models/city/Textures/night-windows.png` (made by `tools/make_window_map.py`) lighting their windows; the road
tiles (straights, zebra crossings, crossroads) are stretched to the street's width, their kerbs raised to the
sidewalks' height.
