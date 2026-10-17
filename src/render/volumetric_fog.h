// Volumetric fog's maths, as shaders/fog.glsl and fog_integrate.comp do it on the GPU (this is their reference, for
// tests): the cells' slices along the view (logarithmic, from near to far), where a distance falls in the integrated
// volume, the Henyey-Greenstein phase function, and summing a column of cells from the camera out.
#pragma once

#include <vector>

#include "core/vecmath.h"

namespace fog {

constexpr int kCellsX = 160, kCellsY = 90, kCellsZ = 64;

struct Range {
    float near = 0.5f, far = 200.f;
    float slicesPerLog() const;   // fogRange.y: slices / log(far / near)
};
float sliceDepth(const Range& r, float slice);   // where slice `slice` (fractional) starts, metres along the view
float volumeZ(const Range& r, float depth);      // the integrated volume's z coordinate (0..1) for a distance

// How much of light going along `travel` turns towards a camera looking along `view` (times 4 pi: 1 for g = 0)
float phase(Vec3 travel, Vec3 view, float g);

// One cell: the light it scatters towards the camera times its density, and its density
struct Cell {
    Vec3 scattered;
    float density = 0.f;
};
// The column summed: for each slice, the light gathered up to its far end and how much gets through it (`stretch`:
// metres of ray per metre along the view)
struct Gathered {
    Vec3 light;
    float through = 1.f;
};
std::vector<Gathered> integrate(const Range& r, float stretch, const std::vector<Cell>& column);

// Whether a cell of slice `slice` can be seen (fog_inject.comp's seen, the same sums): `reach` the farthest pixel
// of the nine columns round it (0: unknown, every cell is); two slices of margin for the volume's filtering
bool cellSeen(const Range& r, int slice, float reach);

}  // namespace fog
