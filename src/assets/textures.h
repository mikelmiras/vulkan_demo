// Texture library: RGBA8 images in memory (the renderer uploads them), from image files, glTF models or built in.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Texture {
    std::string name;
    std::string source;   // "bricks.png", "model.glb#image2"; empty for built-ins
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;   // sRGB colour, 4 bytes per pixel
};

// Textures are only ever appended, so ids stay valid. Id -1 means "no texture" (plain colour).
struct TextureLibrary {
    std::vector<Texture> textures;
    uint32_t version = 0;   // bumped whenever textures are added (the renderer uploads the new ones)

    int count() const { return (int)textures.size(); }
    const Texture& get(int id) const { return textures[(size_t)id]; }
    const char* name(int id) const { return textures[(size_t)id].name.c_str(); }
    int find(const std::string& name) const;   // -1 if missing
};

// Built-in procedural textures ("Checker", "Bricks", "Tiles"), handy without any image files
TextureLibrary buildTextureLibrary();
constexpr int kBuiltinTextureCount = 5;   // Checker, Bricks, Tiles, Bricks Normal, Grate

int addTexture(TextureLibrary& lib, const std::string& name, const std::string& source, uint32_t width, uint32_t height,
               std::vector<uint8_t> rgba);
// PNG, JPEG, TGA, BMP, ... (stb_image). Already loaded sources return their id; -1 with `error` on failure.
int loadTexture(TextureLibrary& lib, const std::string& path, std::string& error);
int loadTextureFromMemory(TextureLibrary& lib, const std::string& name, const std::string& source, const uint8_t* data,
                          size_t size, std::string& error);
// Re-imports a texture from its source: an image path or a glTF image ("model.glb#image2")
int importTexture(TextureLibrary& lib, const std::string& source, std::string& error);
