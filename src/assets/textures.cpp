#include "assets/textures.h"

#include <cmath>
#include <filesystem>

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

int loadTexture(TextureLibrary& lib, const std::string& path, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(path, ec), ec);
    std::string source = (ec ? fs::path(path) : abs).u8string();
    for (int i = 0; i < lib.count(); ++i)
        if (lib.textures[(size_t)i].source == source) return i;
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        error = path + ": " + (stbi_failure_reason() ? stbi_failure_reason() : "cannot load image");
        return -1;
    }
    std::vector<uint8_t> rgba(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
    return addTexture(lib, fs::path(path).stem().u8string(), source, (uint32_t)w, (uint32_t)h, std::move(rgba));
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
