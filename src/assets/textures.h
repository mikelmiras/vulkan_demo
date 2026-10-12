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
    // From the image's import settings (assets/asset_db.h): {"maxSize": 1024, "filter": "nearest", "mipmaps": false}
    bool nearest = false;        // sampled without filtering (pixel art)
    bool mipmaps = true;         // smaller copies for distant surfaces
    uint32_t revision = 0;       // bumped when reloaded in place (reloadTexture): the renderer uploads it again
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
// A small copy of an image file (at most maxSize pixels across, box-filtered) added under `name`, for previews
// (the editor's asset thumbnails). -1 with `error` if it can't be read.
int addThumbnail(TextureLibrary& lib, const std::string& name, const std::string& path, uint32_t maxSize, std::string& error);
int loadTexture(TextureLibrary& lib, const std::string& path, std::string& error);
int loadTextureFromMemory(TextureLibrary& lib, const std::string& name, const std::string& source, const uint8_t* data,
                          size_t size, std::string& error);
// Reads an image file again (after its import settings changed), keeping its id; -1 with `error` on failure
int reloadTexture(TextureLibrary& lib, int id, std::string& error);
// Re-imports a texture from its source: an image path or a glTF image ("model.glb#image2")
int importTexture(TextureLibrary& lib, const std::string& source, std::string& error);
