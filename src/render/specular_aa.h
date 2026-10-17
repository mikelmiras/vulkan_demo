// Specular anti-aliasing (shaders/scene.frag): a highlight widened by how much the normal varies within a pixel, so
// small shiny bumps and distant glossy or wet ground don't sparkle and crawl. Two sources: the geometry's and the
// normal's change from pixel to pixel (screen-space derivatives: Tokuyoshi & Kaplanyan), and the normal map's bumps
// smaller than a pixel, which its mip levels average into a shorter normal (Toksvig: the shorter, the more they
// vary). Plain functions, tested; the shader repeats them.
#pragma once

namespace specaa {

// The variance of the normals a normal map's filtered (mipmapped) sample averages, from its length (1: flat or all
// alike; shorter, the more they spread), scaled by the map's strength squared
float normalMapVariance(float filteredLength, float strength);

// The roughness (perceptual: GGX alpha = roughness^2) widened by the variances: the derivatives' (`screen`) and the
// normal maps' (`texture`), each limited (0.18 in alpha squared: a mirror can't turn chalk-matte)
float widenedRoughness(float roughness, float screen, float texture);

}  // namespace specaa
