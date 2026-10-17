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
particles from the manholes. It's raining: the weather settings (Scene look: wetness, puddles, rain) make the
streets wet and glossy, with standing water in the hollows and rings on it. Puddle decals in the gutters under
about half the lamps mirror them. Rain falls round the taxi as lit, streaked particles that show where there's light.
The cars are painted metal: a metallic base under a clear coat (the lamps and neon run along their bonnets), glass all
round the cabin under a painted roof, chrome bumpers and tyres (`car_body` in `tools/make_city.py`); the taxi is
yellow with a chequered band along each side.
Traffic drives the grid: a few cars (`scripts/TrafficCar.lua`) in the right-hand lanes, choosing at each crossroads
to go on or turn (never out of the city), easing to a stop behind whatever's ahead (another car, the taxi: it can
bump into them, they aren't pushed) and pulling away again, with headlights, tail lamps and their glow on the wet
road. Wet tyres throw spray behind them, the taxi's too: GPU particles whose rate follows each car's speed
(`scripts/SprayBySpeed.lua`). `TRAFFIC` and `LANE` in `tools/make_city.py` set how many and where. Every crossroads
has traffic lights (`scripts/TrafficLight.lua`): two poles, one for each way through, cycling green, amber and red
(each lamp's glow on the wet road), the crossroads' cycles offset from their neighbours'; the traffic stops at the
line before the zebra on red, and on amber unless it's too close to stop. Each cycle (36 seconds) ends with a walk
phase, every way held on red: pedestrians (`scripts/Pedestrian.lua`, three round each block, some under umbrellas)
walk the sidewalks and now and then cross at a zebra to the next block, waiting at the kerb for the walk signal
(poles at both ends of every crossing: a white figure, or the red hand). Their colliders stop the traffic too.
The cycle is worked out from the clock the same way by the lights, the cars and the pedestrians, so nothing is
shared between them.

The neon signs over the doors light the street as tubes (point lights with a *Tube length*), their colour
stretched along the wall and the wet road mirroring a streak; about one in eight is on the blink
(`scripts/NeonFlicker.lua`: a burst of quick flickers every few seconds, the sign's tube and light together).

## Models

`models/city/` (City Kit Commercial) and `models/roads/` (City Kit Roads) are by Kenney (www.kenney.nl), CC0: see the
`License.txt` in each. The generator lays the buildings round each block facing the streets, tinted for the night,
with `models/city/Textures/night-windows.png` (made by `tools/make_window_map.py`) lighting their windows; the road
tiles (straights, zebra crossings, crossroads) are stretched to the street's width, their kerbs raised to the
sidewalks' height.
