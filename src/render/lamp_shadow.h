// Lamps' soft shadows (shaders/scene.frag's lampShadow, PCSS): a blocker's depth in the lamp's shadow map turned back
// into how far along it is from the light towards the shaded point, exactly (along that ray the projection's z and w
// are linear and w is 0 at the light), and the penumbra a light `size` round leaves there. Plain functions, tested;
// the shader repeats them.
#pragma once

namespace lampshadow {

// The fraction of the way from the light to the shaded point at which a blocker of depth `d` (the map's, z / w) sits:
// the point's clip z and w, and the light's own clip z (`lightZ`: the projection's constant term along the ray)
float blockerFraction(float d, float clipZ, float clipW, float lightZ);

// The penumbra's width (m) at the point, for a light `size` round and blockers at fraction t of the way
float penumbra(float size, float t);

}  // namespace lampshadow
