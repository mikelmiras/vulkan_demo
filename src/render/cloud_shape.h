// Volumetric clouds' shape (shaders/sky.frag's cloudAt): the density at a height through the cloud slab from the
// flat layer's coverage there: a flat base, and a dome over it as tall as the cover is dense (thin cover stays low
// and wide; the densest reaches the top). A plain function, tested; the shader repeats it.
#pragma once

namespace clouds {

// `cover` 0..1 (cloudDensity's), `h` 0..1 through the slab (base to top)
float density(float cover, float h);

}  // namespace clouds
