#include "assets/animation.h"

#include <algorithm>
#include <cmath>

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

namespace {

// How far apart two values of a channel are: an angle (degrees) for rotations, a distance otherwise
float keyError(const float* a, const float* b, int width) {
    if (width == 4) {
        float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        return 2.f * std::acos(std::min(d, 1.f)) / DEG2RAD;
    }
    float s = 0.f;
    for (int c = 0; c < width; ++c) s += (a[c] - b[c]) * (a[c] - b[c]);
    return std::sqrt(s);
}

size_t compressKeys(AnimationKeys& k, float tolerance) {
    using I = AnimationKeys::Interpolation;
    size_t n = k.times.size();
    if (n < 3 || k.interpolation == I::CubicSpline || k.values.size() != n * (size_t)k.width) return 0;
    std::vector<size_t> kept{0};
    const int w = k.width;
    auto at = [&](size_t i) { return &k.values[i * (size_t)w]; };
    if (k.interpolation == I::Step) {   // a key the same as the one before it changes nothing
        for (size_t i = 1; i + 1 < n; ++i)
            if (keyError(at(i), at(kept.back()), w) > tolerance) kept.push_back(i);
    } else {
        // From each kept key, reach as far as a straight run (interpolated from it to the far key) still passes within
        // the tolerance of every key in between
        size_t i = 0;
        while (i + 1 < n) {
            size_t j = i + 1;
            while (j + 1 < n) {
                size_t far = j + 1;
                bool ok = true;
                for (size_t m = i + 1; m < far && ok; ++m) {
                    float span = k.times[far] - k.times[i], u = span > 0.f ? (k.times[m] - k.times[i]) / span : 0.f;
                    float v[4];
                    if (w == 4) {
                        Quat q = slerp({at(i)[0], at(i)[1], at(i)[2], at(i)[3]}, {at(far)[0], at(far)[1], at(far)[2], at(far)[3]}, u);
                        v[0] = q.x; v[1] = q.y; v[2] = q.z; v[3] = q.w;
                    } else {
                        for (int c = 0; c < w; ++c) v[c] = at(i)[c] + (at(far)[c] - at(i)[c]) * u;
                    }
                    ok = keyError(v, at(m), w) <= tolerance;
                }
                if (!ok) break;
                j = far;
            }
            kept.push_back(j);
            i = j;
        }
    }
    if (kept.back() != n - 1) kept.push_back(n - 1);
    std::vector<float> times, values;
    for (size_t i : kept) {
        times.push_back(k.times[i]);
        values.insert(values.end(), at(i), at(i) + w);
    }
    size_t removed = n - kept.size();
    k.times = std::move(times);
    k.values = std::move(values);
    return removed;
}

}  // namespace

size_t compressClip(AnimationClip& clip, float rotationDegrees, float translation, float scale) {
    size_t removed = 0;
    for (AnimationTrack& t : clip.tracks) {
        removed += compressKeys(t.rotation, rotationDegrees);
        removed += compressKeys(t.translation, translation);
        removed += compressKeys(t.scale, scale);
    }
    return removed;
}

size_t keyCount(const AnimationClip& clip) {
    size_t n = 0;
    for (const AnimationTrack& t : clip.tracks) n += t.rotation.times.size() + t.translation.times.size() + t.scale.times.size();
    return n;
}
