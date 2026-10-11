#include "assets/animation.h"

#include <algorithm>

namespace {

// Keys around `time`: index of the one before, and how far towards the next (0..1)
void locate(const AnimationKeys& k, float time, size_t& i, float& u) {
    const std::vector<float>& t = k.times;
    if (time <= t.front()) { i = 0; u = 0.f; return; }
    if (time >= t.back()) { i = t.size() - 1; u = 0.f; return; }
    i = (size_t)(std::upper_bound(t.begin(), t.end(), time) - t.begin()) - 1;
    float span = t[i + 1] - t[i];
    u = span > 0.f ? (time - t[i]) / span : 0.f;
}

// Value number `key` (for cubic splines: the value, not a tangent)
const float* keyValue(const AnimationKeys& k, size_t key) {
    size_t stride = k.interpolation == AnimationKeys::Interpolation::CubicSpline ? 3 : 1;
    size_t at = (key * stride + (stride == 3 ? 1 : 0)) * (size_t)k.width;
    return at + (size_t)k.width <= k.values.size() ? &k.values[at] : nullptr;
}

// Interpolated value into out[width]
bool sample(const AnimationKeys& k, float time, float out[4]) {
    if (k.empty()) return false;
    size_t i;
    float u;
    locate(k, time, i, u);
    const float* a = keyValue(k, i);
    if (!a) return false;
    const float* b = i + 1 < k.times.size() ? keyValue(k, i + 1) : nullptr;
    if (!b || u <= 0.f || k.interpolation == AnimationKeys::Interpolation::Step) {
        std::copy(a, a + k.width, out);
        return true;
    }
    if (k.interpolation == AnimationKeys::Interpolation::CubicSpline) {   // Hermite, tangents scaled by the span
        float span = k.times[i + 1] - k.times[i];
        const float* outTan = a + k.width;        // a's out-tangent
        const float* inTan = b - k.width;         // b's in-tangent
        float u2 = u * u, u3 = u2 * u;
        float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        for (int c = 0; c < k.width; ++c)
            out[c] = h00 * a[c] + h10 * span * outTan[c] + h01 * b[c] + h11 * span * inTan[c];
        return true;
    }
    if (k.width == 4) {
        Quat q = slerp({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, u);
        out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
        return true;
    }
    for (int c = 0; c < k.width; ++c) out[c] = a[c] + (b[c] - a[c]) * u;
    return true;
}

}  // namespace

AnimationPose sampleTrack(const AnimationTrack& track, float time) {
    AnimationPose p;
    float v[4];
    if ((p.hasTranslation = sample(track.translation, time, v))) p.translation = {v[0], v[1], v[2]};
    if ((p.hasRotation = sample(track.rotation, time, v))) p.rotation = normalizeQuat(Quat{v[0], v[1], v[2], v[3]});
    if ((p.hasScale = sample(track.scale, time, v))) p.scale = {v[0], v[1], v[2]};
    return p;
}

AnimationPose blendPoses(const AnimationPose& a, const AnimationPose& b, float t) {
    AnimationPose r = a;
    if (b.hasTranslation) r.translation = a.hasTranslation ? a.translation + (b.translation - a.translation) * t : b.translation;
    if (b.hasRotation) r.rotation = a.hasRotation ? slerp(a.rotation, b.rotation, t) : b.rotation;
    if (b.hasScale) r.scale = a.hasScale ? a.scale + (b.scale - a.scale) * t : b.scale;
    r.hasTranslation |= b.hasTranslation;
    r.hasRotation |= b.hasRotation;
    r.hasScale |= b.hasScale;
    return r;
}
