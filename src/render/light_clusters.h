// Clustered lighting and the shadow atlas, the parts that need no GPU (so they can be tested on their own).
//
// Clusters: the view is cut into tilesX x tilesY screen tiles and `slices` depth slices (spaced logarithmically
// between near and far, like the view's own precision). Each light goes into every cluster its sphere may touch
// (conservatively: the screen box around its bounding box, and the slices its depth range covers); a pixel then
// shades only with the lights of its cluster. scene.frag finds the cluster the same way (clusterOf).
//
// Atlas: shadow maps of spot and point lights share one square depth image, cut into power-of-two tiles.
#pragma once

#include <cstdint>
#include <vector>

#include "core/vecmath.h"

struct ClusterSetup {
    Mat4 viewProj;
    Vec3 camPos, camForward;
    float near = 0.1f, far = 1000.f;
    int tilesX = 16, tilesY = 9, slices = 24;
    int clusters() const { return tilesX * tilesY * slices; }
};

struct ClusterLight {
    Vec3 position;
    float range = 1.f;
};

// The cluster a world point falls in (-1: off screen or behind the camera)
int clusterOf(const ClusterSetup& s, Vec3 p);
int clusterSlice(const ClusterSetup& s, float depth);   // the slice a distance along the view falls in

// out: for each cluster (offset, count) into the index list that follows them (offsets count from the list's
// start), then the light indices. At most maxIndices are listed (false if some were left out: nearest lights
// first, as given).
bool buildLightClusters(const ClusterSetup& s, const std::vector<ClusterLight>& lights, std::vector<uint32_t>& out,
                        size_t maxIndices);

struct AtlasTile {
    int x = 0, y = 0, size = 0;   // in texels; size 0: didn't fit
};
// Places square power-of-two tiles in an atlas of `atlasSize` (a power of two), biggest first; a request that
// doesn't fit gets size 0. Tiles come back in the order asked.
std::vector<AtlasTile> allocateAtlas(int atlasSize, const std::vector<int>& sizes);
