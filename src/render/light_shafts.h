// Light shafts (god rays) on screen (shaders/shafts.glsl): where the sun is in the picture and how much of its
// shafts show (fading as it goes off screen, none behind the camera). A plain function, tested; the renderer's post
// pass uses it (renderer_post.cpp).
#pragma once

#include "core/vecmath.h"

namespace shafts {

struct SunOnScreen {
    float x = 0.f, y = 0.f;   // pixels (may be off the viewport's edge)
    float fade = 0.f;         // 0..1: 1 with the sun in the viewport, down to 0 a third of it beyond, and behind
};

// For a view (viewProj; its viewport in pixels) and the sun shining along `sunDir` (normalized: from the sun)
SunOnScreen sunOnScreen(const Mat4& viewProj, Vec3 camPos, Vec3 sunDir, float vx, float vy, float vw, float vh);

}  // namespace shafts
