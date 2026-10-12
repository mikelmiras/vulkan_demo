#include "assets/textures.h"
#include "assets/asset_db.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "core/vfs.h"
#include "assets/gltf.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

int TextureLibrary::find(const std::string& name) const {
    for (int i = 0; i < count(); ++i)
        if (textures[(size_t)i].name == name) return i;
    return -1;
}

namespace {

// Cheap deterministic per-cell variation for the procedural textures
float hash(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)(h & 0xFFFF) / 65535.f;
}

template <class F>
Texture procedural(const char* name, uint32_t size, F colorAt) {
    Texture t;
    t.name = name;
    t.width = t.height = size;
    t.rgba.resize((size_t)size * size * 4);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
            float rgb[3];
            colorAt((float)x / size, (float)y / size, rgb);
            uint8_t* p = &t.rgba[((size_t)y * size + x) * 4];
            for (int c = 0; c < 3; ++c) p[c] = (uint8_t)std::lround(std::fmin(std::fmax(rgb[c], 0.f), 1.f) * 255.f);
            p[3] = 255;
        }
    return t;
}

}  // namespace

TextureLibrary buildTextureLibrary() {
    TextureLibrary lib;
    // 8x8 light/dark squares
    lib.textures.push_back(procedural("Checker", 256, [](float u, float v, float* c) {
        bool odd = ((int)(u * 8) + (int)(v * 8)) & 1;
        c[0] = c[1] = c[2] = odd ? 0.55f : 0.95f;
    }));
    // Running-bond bricks with mortar; colour varies a little per brick
    lib.textures.push_back(procedural("Bricks", 256, [](float u, float v, float* c) {
        const int rows = 8;
        int row = (int)(v * rows);
        float bu = u * 4.f + (row & 1) * 0.5f;
        int col = (int)std::floor(bu);
        float fu = bu - (float)col, fv = v * rows - (float)row;
        bool mortar = fu < 0.04f || fv < 0.08f;
        float n = hash(col, row) * 0.25f + hash((int)(u * 256), (int)(v * 256)) * 0.08f;
        if (mortar) { c[0] = 0.78f; c[1] = 0.76f; c[2] = 0.72f; }
        else { c[0] = 0.62f + n; c[1] = 0.30f + n * 0.5f; c[2] = 0.22f + n * 0.3f; }
    }));
    // Square floor tiles with grout
    lib.textures.push_back(procedural("Tiles", 256, [](float u, float v, float* c) {
        float fu = u * 4.f - std::floor(u * 4.f), fv = v * 4.f - std::floor(v * 4.f);
        bool grout = fu < 0.05f || fv < 0.05f;
        float n = hash((int)(u * 4), (int)(v * 4)) * 0.12f;
        if (grout) { c[0] = c[1] = c[2] = 0.35f; }
        else { c[0] = 0.80f + n; c[1] = 0.82f + n; c[2] = 0.86f + n; }
    }));
    // A normal map for Bricks: bevelled bricks standing out of recessed mortar (tangent space, +Y up the image)
    lib.textures.push_back(procedural("Bricks Normal", 256, [](float u, float v, float* c) {
        auto height = [](float u, float v) {
            const int rows = 8;
            float wv = v - std::floor(v), wu = u - std::floor(u);
            int row = (int)(wv * rows);
            float bu = wu * 4.f + (row & 1) * 0.5f;
            float fu = bu - std::floor(bu), fv = wv * rows - (float)row;
            // Distance to the brick's edge, in brick heights (a brick is 1/8 high, 1/4 wide)
            float du = std::fmin(fu, 1.f - fu) * 2.f, dv = std::fmin(fv, 1.f - fv);
            float d = std::fmin(du, dv);
            return std::fmin(std::fmax((d - 0.04f) / 0.12f, 0.f), 1.f);   // mortar 0, bevel, brick top 1
        };
        const float e = 1.f / 256.f, k = 0.7f;   // bevels lean about 35 degrees
        float h = height(u, v), hu = height(u + e, v), hv = height(u, v + e);
        Vec3 n = normalize(Vec3{-(hu - h) / e * k / 64.f, (hv - h) / e * k / 64.f, 1.f});
        c[0] = n.x * 0.5f + 0.5f;
        c[1] = n.y * 0.5f + 0.5f;
        c[2] = n.z * 0.5f + 0.5f;
    }));
    // A metal grate: bars with see-through holes (alpha 0) between them, for cut-out materials
    {
        Texture grate = procedural("Grate", 256, [](float u, float v, float* c) {
            float fu = u * 8.f - std::floor(u * 8.f), fv = v * 8.f - std::floor(v * 8.f);
            float edge = std::fmin(std::fmin(fu, 1.f - fu), std::fmin(fv, 1.f - fv));
            c[0] = c[1] = c[2] = 0.55f + edge * 0.4f;
        });
        for (uint32_t y = 0; y < grate.height; ++y)
            for (uint32_t x = 0; x < grate.width; ++x) {
                float fu = x * 8.f / grate.width, fv = y * 8.f / grate.height;
                fu -= std::floor(fu);
                fv -= std::floor(fv);
                bool bar = fu < 0.18f || fv < 0.18f;
                grate.rgba[((size_t)y * grate.width + x) * 4 + 3] = bar ? 255 : 0;
            }
        lib.textures.push_back(std::move(grate));
    }
    return lib;
}

int addTexture(TextureLibrary& lib, const std::string& name, const std::string& source, uint32_t width, uint32_t height,
               std::vector<uint8_t> rgba) {
    Texture t;
    t.name = name.empty() ? "Texture" : name;
    for (int n = 2; lib.find(t.name) >= 0; ++n) t.name = (name.empty() ? "Texture" : name) + " " + std::to_string(n);
    t.source = source;
    t.width = width;
    t.height = height;
    t.rgba = std::move(rgba);
    lib.textures.push_back(std::move(t));
    ++lib.version;
    return lib.count() - 1;
}

int loadTextureFromMemory(TextureLibrary& lib, const std::string& name, const std::string& source, const uint8_t* data,
                          size_t size, std::string& error) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
    if (!pixels) {
        error = source + ": " + (stbi_failure_reason() ? stbi_failure_reason() : "cannot decode image");
        return -1;
    }
    std::vector<uint8_t> rgba(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
    return addTexture(lib, name, source, (uint32_t)w, (uint32_t)h, std::move(rgba));
}

namespace {

// Box-filters RGBA pixels down by whole steps until neither side is over maxSize
void shrink(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h, uint32_t maxSize) {
    if (maxSize == 0 || std::max(w, h) <= maxSize) return;
    uint32_t step = (std::max(w, h) + maxSize - 1) / maxSize;
    uint32_t tw = std::max(1u, w / step), th = std::max(1u, h / step);
    std::vector<uint8_t> out((size_t)tw * th * 4);
    for (uint32_t y = 0; y < th; ++y)
        for (uint32_t x = 0; x < tw; ++x) {
            uint32_t sum[4] = {0, 0, 0, 0}, n = 0;
            for (uint32_t sy = y * step; sy < std::min((y + 1) * step, h); ++sy)
                for (uint32_t sx = x * step; sx < std::min((x + 1) * step, w); ++sx, ++n)
                    for (int c = 0; c < 4; ++c) sum[c] += rgba[((size_t)sy * w + sx) * 4 + c];
            for (int c = 0; c < 4; ++c) out[((size_t)y * tw + x) * 4 + c] = (uint8_t)(sum[c] / std::max(n, 1u));
        }
    rgba = std::move(out);
    w = tw;
    h = th;
}

// An image file decoded, with its import settings applied
bool decodeImageFile(const std::string& path, Texture& t, std::string& error) {
    int w = 0, h = 0, channels = 0;
    std::string bytes;
    stbi_uc* pixels = vfs::read(path, bytes) ? stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w,
                                                                     &h, &channels, 4)
                                             : nullptr;
    if (!pixels) {
        error = path + ": " + (stbi_failure_reason() ? stbi_failure_reason() : "cannot load image");
        return false;
    }
    t.rgba.assign(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
    t.width = (uint32_t)w;
    t.height = (uint32_t)h;
    json::Value s = assetdb::importSettings(std::filesystem::u8path(path));
    shrink(t.rgba, t.width, t.height, (uint32_t)std::max(0, s["maxSize"].asInt(0)));
    t.nearest = s["filter"].asString() == "nearest";
    t.mipmaps = s["mipmaps"].asBool(true);
    return true;
}

}  // namespace

int loadTexture(TextureLibrary& lib, const std::string& path, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(path, ec), ec);
    std::string source = (ec ? fs::path(path) : abs).u8string();
    for (int i = 0; i < lib.count(); ++i)
        if (lib.textures[(size_t)i].source == source) return i;
    Texture t;
    if (!decodeImageFile(path, t, error)) return -1;
    int id = addTexture(lib, fs::path(path).stem().u8string(), source, t.width, t.height, std::move(t.rgba));
    lib.textures[(size_t)id].nearest = t.nearest;
    lib.textures[(size_t)id].mipmaps = t.mipmaps;
    return id;
}

int reloadTexture(TextureLibrary& lib, int id, std::string& error) {
    if (id < 0 || id >= lib.count()) { error = "no such texture"; return -1; }
    Texture& t = lib.textures[(size_t)id];
    if (t.source.empty() || t.source.find('#') != std::string::npos) { error = t.name + ": not from an image file"; return -1; }
    Texture fresh;
    if (!decodeImageFile(t.source, fresh, error)) return -1;
    t.rgba = std::move(fresh.rgba);
    t.width = fresh.width;
    t.height = fresh.height;
    t.nearest = fresh.nearest;
    t.mipmaps = fresh.mipmaps;
    ++t.revision;
    ++lib.version;
    return id;
}

int addThumbnail(TextureLibrary& lib, const std::string& name, const std::string& path, uint32_t maxSize, std::string& error) {
    if (int have = lib.find(name); have >= 0) return have;
    std::string bytes;
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = vfs::read(path, bytes) ? stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w,
                                                                     &h, &channels, 4)
                                             : nullptr;
    if (!pixels) {
        error = path + ": " + (stbi_failure_reason() ? stbi_failure_reason() : "cannot load image");
        return -1;
    }
    // Every thumbnail pixel averages the block of source pixels it covers
    std::vector<uint8_t> rgba(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
    uint32_t tw = (uint32_t)w, th = (uint32_t)h;
    shrink(rgba, tw, th, std::max(maxSize, 1u));
    return addTexture(lib, name, "", tw, th, std::move(rgba));
}

int importTexture(TextureLibrary& lib, const std::string& source, std::string& error) {
    for (int i = 0; i < lib.count(); ++i)
        if (lib.textures[(size_t)i].source == source) return i;
    size_t hash = source.rfind('#');
    if (hash == std::string::npos) return loadTexture(lib, source, error);
    if (!importGltfImages(lib, source.substr(0, hash), error)) return -1;
    for (int i = 0; i < lib.count(); ++i)
        if (lib.textures[(size_t)i].source == source) return i;
    error = source + ": no such image in the file";
    return -1;
}
