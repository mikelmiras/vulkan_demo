// A sky image for image-based lighting: an equirectangular panorama (an .hdr, or an ordinary image taken as sRGB),
// with what the renderer needs from it worked out once on the CPU:
//   - the sky itself (at most 2048 x 1024), drawn behind everything and seen in mirror-like reflections
//   - radiance: 256 x 128 and five smaller levels, each blurred like a GGX highlight of roughness level / 5, for
//     reflections on rougher surfaces (the renderer samples between levels by roughness)
//   - irradiance: what a matte surface facing each way gathers, as spherical harmonics (9 RGB coefficients)
// Directions map to the image as u = atan2(x, -z) / 2pi + 0.5 (-Z at the centre), v = acos(y) / pi (up at the top).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/vecmath.h"

struct Environment {
    struct Level {
        uint32_t width = 0, height = 0;
        std::vector<float> rgb;   // linear, 3 floats per pixel, rows top to bottom
    };
    std::string source;
    Level sky;
    std::vector<Level> radiance;   // 6 levels, each half the size of the one before
    float sh[9][3] = {};           // irradiance (already convolved with the cosine lobe; divide by pi for a matte white)
    uint32_t id = 0;               // unique per loaded environment (the renderer uploads a new one)
};

namespace environment {

constexpr int kRadianceLevels = 6;

// Loads and prefilters a panorama, cached by path (later calls return the same one; failures are remembered too:
// nullptr and the error again). Thread-safe; the first call for a file takes a moment (a few tenths of a second).
const Environment* load(const std::string& path, std::string& error);
// The same from pixels in memory (linear RGB, 3 floats per pixel), not cached: for tests and generated skies
Environment build(const std::string& source, uint32_t width, uint32_t height, std::vector<float> rgb);

Vec3 directionAt(float u, float v);   // image coordinates (0..1) -> direction
void uvOf(Vec3 dir, float& u, float& v);
// Evaluates the irradiance at a normal (what the shader does): radiance-weighted cosine sum, divided by pi
Vec3 irradiance(const Environment& env, Vec3 n);
Vec3 sample(const Environment::Level& level, Vec3 dir);   // bilinear

}  // namespace environment
