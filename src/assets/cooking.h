// Cooked assets: what an export ships in place of source images and glTF models, ready for the GPU and read
// without decoding or parsing. A cooked texture (<image>.vktex) holds its mip levels already made and block
// compressed (assets/bc_codec.h); a cooked model (<model>.glb.vkmodel) holds the meshes the glTF importer makes
// (with their LODs), its images cooked and its animation clips, as arrays copied straight in.
//
// Loading looks for the cooked file first: loadTexture("t.png") reads "t.png.vktex" if there is one, and the glTF
// importers read "m.glb.vkmodel", so scenes keep naming their sources. app/export.cpp cooks (ExportOptions::cook).
#pragma once

#include <string>

#include "assets/bc_codec.h"
#include "assets/meshes.h"
#include "assets/textures.h"

namespace cook {

// How textures are compressed: plain RGBA8 (mips baked only), small (BC1, or BC3 with alpha: 4 / 8 bits a pixel),
// quality (BC7: 8 bits a pixel)
enum class Compression { None, Small, Quality };
Compression compressionFromName(const std::string& name);   // "none", "bc1", "bc7" (anything else: Quality)
const char* compressionName(Compression c);

std::string texturePath(const std::string& source);   // source + ".vktex"
std::string modelPath(const std::string& source);     // source + ".vkmodel"

// The format a texture is cooked in (BC1 / BC3 by whether any pixel isn't opaque)
bc::Format formatFor(const Texture& t, Compression c);

// A texture (its rgba) cooked: every level made (box filtered in linear light, as streaming does), then encoded
std::string cookTexture(const Texture& t, Compression c);
// ... read back: width, height, format, levels, nearest, mipmaps (rgba stays empty)
bool readTexture(const std::string& bytes, Texture& out, std::string& error);
// An image file cooked, its import settings applied (assets/asset_db.h)
bool cookImageFile(const std::string& path, Compression c, std::string& out, std::string& error);

// A glTF file cooked: everything importGltfMeshes / Images / Animations would bring in
bool cookModel(const std::string& path, Compression c, std::string& out, std::string& error);
// A cooked model's meshes, images and clips (`clips`: its animations), each skipped when its library is null,
// named as the glTF importer names them after `file` ("file#0.0", "file#image1", "file#anim0"); ones already in
// the libraries are kept
bool readModel(const std::string& bytes, const std::string& file, MeshLibrary* meshes, TextureLibrary* textures,
               MeshLibrary* clips, std::string& error);

}  // namespace cook
