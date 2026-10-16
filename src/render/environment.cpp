#include "render/environment.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "core/vfs.h"
#include "stb_image.h"

namespace environment {

namespace {

constexpr float kPi = 3.14159265358979f;
std::atomic<uint32_t> g_nextId{1};

float srgbToLinear1(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// Box-filtered copy at another size (each output pixel averages the source pixels it covers)
Environment::Level resample(const Environment::Level& src, uint32_t w, uint32_t h) {
    Environment::Level out;
    out.width = w;
    out.height = h;
    out.rgb.assign((size_t)w * h * 3, 0.f);
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t y0 = y * src.height / h, y1 = std::max(y0 + 1, (y + 1) * src.height / h);
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t x0 = x * src.width / w, x1 = std::max(x0 + 1, (x + 1) * src.width / w);
            float sum[3] = {0, 0, 0};
            uint32_t n = 0;
            for (uint32_t sy = y0; sy < y1 && sy < src.height; ++sy)
                for (uint32_t sx = x0; sx < x1 && sx < src.width; ++sx, ++n)
                    for (int c = 0; c < 3; ++c) sum[c] += src.rgb[((size_t)sy * src.width + sx) * 3 + (size_t)c];
            for (int c = 0; c < 3; ++c) out.rgb[((size_t)y * w + x) * 3 + (size_t)c] = sum[c] / (float)std::max(n, 1u);
        }
    }
    return out;
}

// Directions and solid angles of a level's pixels
struct Texels {
    std::vector<Vec3> dir;
    std::vector<float> solidAngle;
};
Texels texels(const Environment::Level& l) {
    Texels t;
    t.dir.resize((size_t)l.width * l.height);
    t.solidAngle.resize(t.dir.size());
    for (uint32_t y = 0; y < l.height; ++y)
        for (uint32_t x = 0; x < l.width; ++x) {
            float v = ((float)y + 0.5f) / (float)l.height;
            size_t i = (size_t)y * l.width + x;
            t.dir[i] = directionAt(((float)x + 0.5f) / (float)l.width, v);
            t.solidAngle[i] = (2.f * kPi / (float)l.width) * (kPi / (float)l.height) * std::sin(v * kPi);
        }
    return t;
}

// Reflections off a surface of this roughness: the source weighted by a GGX lobe around each direction
Environment::Level prefilter(const Environment::Level& src, uint32_t w, uint32_t h, float roughness) {
    Texels in = texels(src);
    Environment::Level out;
    out.width = w;
    out.height = h;
    out.rgb.assign((size_t)w * h * 3, 0.f);
    float a = std::max(roughness * roughness, 0.002f), a2 = a * a;
    std::atomic<uint32_t> nextRow{0};
    auto rows = [&] {
        for (uint32_t y = nextRow++; y < h; y = nextRow++)
            for (uint32_t x = 0; x < w; ++x) {
                Vec3 n = directionAt(((float)x + 0.5f) / (float)w, ((float)y + 0.5f) / (float)h);
                double sum[3] = {0, 0, 0}, weight = 0;
                for (size_t i = 0; i < in.dir.size(); ++i) {
                    float nl = dot(n, in.dir[i]);
                    if (nl <= 0.f) continue;
                    float nh2 = 0.5f + 0.5f * nl;   // (cos of half the angle, squared: H halfway between N and L)
                    float d = nh2 * (a2 - 1.f) + 1.f;
                    float wgt = a2 / (kPi * d * d) * nl * in.solidAngle[i];
                    for (int c = 0; c < 3; ++c) sum[c] += (double)(src.rgb[i * 3 + (size_t)c] * wgt);
                    weight += wgt;
                }
                for (int c = 0; c < 3; ++c)
                    out.rgb[((size_t)y * w + x) * 3 + (size_t)c] = weight > 0 ? (float)(sum[c] / weight) : 0.f;
            }
    };
    unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < threads; ++i) pool.emplace_back(rows);
    rows();
    for (std::thread& t : pool) t.join();
    return out;
}

void shBasis(Vec3 d, float y[9]) {
    y[0] = 0.282095f;
    y[1] = 0.488603f * d.y;
    y[2] = 0.488603f * d.z;
    y[3] = 0.488603f * d.x;
    y[4] = 1.092548f * d.x * d.y;
    y[5] = 1.092548f * d.y * d.z;
    y[6] = 0.315392f * (3.f * d.z * d.z - 1.f);
    y[7] = 1.092548f * d.x * d.z;
    y[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

}  // namespace

Vec3 directionAt(float u, float v) {
    float phi = (u - 0.5f) * 2.f * kPi, theta = v * kPi;
    float r = std::sin(theta);
    return {r * std::sin(phi), std::cos(theta), -r * std::cos(phi)};
}

void uvOf(Vec3 d, float& u, float& v) {
    u = std::atan2(d.x, -d.z) / (2.f * kPi) + 0.5f;
    v = std::acos(std::clamp(d.y, -1.f, 1.f)) / kPi;
}

Vec3 sample(const Environment::Level& l, Vec3 dir) {
    float u, v;
    uvOf(normalize(dir), u, v);
    float fx = u * (float)l.width - 0.5f, fy = std::clamp(v * (float)l.height - 0.5f, 0.f, (float)l.height - 1.f);
    int x0 = (int)std::floor(fx), y0 = (int)fy;
    float tx = fx - (float)x0, ty = fy - (float)y0;
    auto at = [&](int x, int y) {
        x = ((x % (int)l.width) + (int)l.width) % (int)l.width;   // (wraps around)
        y = std::clamp(y, 0, (int)l.height - 1);
        const float* p = &l.rgb[((size_t)y * l.width + (size_t)x) * 3];
        return Vec3{p[0], p[1], p[2]};
    };
    Vec3 top = at(x0, y0) * (1.f - tx) + at(x0 + 1, y0) * tx, bottom = at(x0, y0 + 1) * (1.f - tx) + at(x0 + 1, y0 + 1) * tx;
    return top * (1.f - ty) + bottom * ty;
}

Vec3 irradiance(const Environment& env, Vec3 n) {
    float y[9];
    shBasis(normalize(n), y);
    Vec3 e;
    for (int i = 0; i < 9; ++i) e = e + Vec3{env.sh[i][0], env.sh[i][1], env.sh[i][2]} * y[i];
    return e * (1.f / kPi);
}

const Vec3 kProbeFaceForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
const Vec3 kProbeFaceUp[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};

namespace {
float fromHalf(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) bits = sign;                                     // (zero; denormals too: tiny)
    else if (exp == 31) bits = sign | 0x7F800000u | (mant << 13);
    else bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
}  // namespace

Environment fromProbeFaces(const std::vector<uint16_t>& px, uint32_t size) {
    const uint32_t pw = 512, ph = 256;
    std::vector<float> rgb((size_t)pw * ph * 3);
    if (size == 0 || px.size() < (size_t)size * size * 4 * 6) return build("probe", pw, ph, std::move(rgb));
    for (uint32_t y = 0; y < ph; ++y)
        for (uint32_t x = 0; x < pw; ++x) {
            Vec3 d = directionAt(((float)x + 0.5f) / (float)pw, ((float)y + 0.5f) / (float)ph);
            int best = 0;
            float bestDot = -2.f;
            for (int f = 0; f < 6; ++f)
                if (float k = dot(d, kProbeFaceForward[f]); k > bestDot) bestDot = k, best = f;
            Vec3 fw = kProbeFaceForward[best], up = kProbeFaceUp[best], right = normalize(cross(fw, up));
            up = cross(right, fw);
            float sx = dot(d, right) / bestDot, sy = -dot(d, up) / bestDot;   // -1..1 across the view (y down)
            uint32_t ix = std::min(size - 1, (uint32_t)((sx * 0.5f + 0.5f) * (float)size));
            uint32_t iy = std::min(size - 1, (uint32_t)((sy * 0.5f + 0.5f) * (float)size));
            const uint16_t* p = px.data() + ((size_t)best * size * size + (size_t)iy * size + ix) * 4;
            for (int c = 0; c < 3; ++c) rgb[((size_t)y * pw + x) * 3 + (size_t)c] = fromHalf(p[c]);
        }
    return build("probe", pw, ph, std::move(rgb));
}

Environment build(const std::string& source, uint32_t width, uint32_t height, std::vector<float> rgb) {
    Environment env;
    env.source = source;
    env.id = g_nextId++;
    Environment::Level full;
    full.width = width;
    full.height = height;
    full.rgb = std::move(rgb);
    for (float& c : full.rgb) c = std::isfinite(c) ? std::clamp(c, 0.f, 65000.f) : 0.f;   // (fits half floats)
    if (width > 2048) env.sky = resample(full, 2048, std::max(1u, (uint32_t)((uint64_t)height * 2048 / width)));
    else env.sky = full;

    // Radiance: sharp at 256 x 128, then a GGX blur per level from smaller copies (the wide lobes don't need more)
    env.radiance.push_back(resample(env.sky, 256, 128));
    Environment::Level fine = resample(env.radiance[0], 128, 64), coarse = resample(env.radiance[0], 64, 32);
    for (int k = 1; k < kRadianceLevels; ++k) {
        uint32_t w = 256u >> k, h = 128u >> k;
        env.radiance.push_back(prefilter(k == 1 ? fine : coarse, w, h, (float)k / (float)(kRadianceLevels - 1)));
    }

    // Irradiance: the sky projected on the first nine spherical harmonics, convolved with the cosine lobe
    Texels t = texels(coarse);
    double c[9][3] = {};
    for (size_t i = 0; i < t.dir.size(); ++i) {
        float y[9];
        shBasis(t.dir[i], y);
        for (int k = 0; k < 9; ++k)
            for (int ch = 0; ch < 3; ++ch) c[k][ch] += (double)(coarse.rgb[i * 3 + (size_t)ch] * y[k] * t.solidAngle[i]);
    }
    const float band[9] = {kPi, 2.f * kPi / 3.f, 2.f * kPi / 3.f, 2.f * kPi / 3.f, kPi / 4.f, kPi / 4.f, kPi / 4.f,
                           kPi / 4.f, kPi / 4.f};
    for (int k = 0; k < 9; ++k)
        for (int ch = 0; ch < 3; ++ch) env.sh[k][ch] = (float)c[k][ch] * band[k];
    return env;
}

const Environment* load(const std::string& path, std::string& error) {
    static std::mutex m;
    static std::map<std::string, std::unique_ptr<Environment>> cache;
    static std::map<std::string, std::string> failed;
    std::lock_guard<std::mutex> lock(m);
    if (auto it = cache.find(path); it != cache.end()) return it->second.get();
    if (auto it = failed.find(path); it != failed.end()) {
        error = it->second;
        return nullptr;
    }
    std::string bytes;
    int w = 0, h = 0, channels = 0;
    std::vector<float> rgb;
    if (!vfs::read(std::filesystem::u8path(path), bytes)) {
        error = "cannot open " + path;
    } else if (stbi_is_hdr_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size())) {
        if (float* p = stbi_loadf_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, &channels, 3)) {
            rgb.assign(p, p + (size_t)w * h * 3);
            stbi_image_free(p);
        }
    } else if (stbi_uc* p = stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, &channels, 3)) {
        rgb.resize((size_t)w * h * 3);
        for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = srgbToLinear1(p[i] / 255.f);
        stbi_image_free(p);
    }
    if (rgb.empty()) {
        if (error.empty()) error = path + ": " + (stbi_failure_reason() ? stbi_failure_reason() : "cannot decode the image");
        failed[path] = error;
        return nullptr;
    }
    auto env = std::make_unique<Environment>(build(path, (uint32_t)w, (uint32_t)h, std::move(rgb)));
    return (cache[path] = std::move(env)).get();
}

}  // namespace environment
