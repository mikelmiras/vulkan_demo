// Cooked textures and models: little-endian binary files, a magic word and a version first. See cooking.h.
#include "assets/cooking.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "assets/gltf.h"
#include "render/texture_streaming.h"

namespace cook {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTextureMagic = 0x58544B56;   // "VKTX"
constexpr uint32_t kModelMagic = 0x444D4B56;     // "VKMD"
constexpr uint32_t kVersion = 1;

static_assert(sizeof(Vertex) == 32 && sizeof(SkinVertex) == 24, "cooked models copy vertices as they are in memory");

struct Writer {
    std::string out;
    void raw(const void* p, size_t n) { out.append((const char*)p, n); }
    void u8(uint8_t v) { raw(&v, 1); }
    void u32(uint32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void str(const std::string& s) {
        u32((uint32_t)s.size());
        raw(s.data(), s.size());
    }
    template <class T>
    void array(const std::vector<T>& v) {
        u32((uint32_t)v.size());
        raw(v.data(), v.size() * sizeof(T));
    }
};

// Reads with bounds checks: a short or damaged file sets `bad` and reads zeros
struct Reader {
    const std::string& in;
    size_t at = 0;
    bool bad = false;
    bool raw(void* p, size_t n) {
        if (bad || n > in.size() - at) {
            bad = true;
            std::memset(p, 0, n);
            return false;
        }
        std::memcpy(p, in.data() + at, n);
        at += n;
        return true;
    }
    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    float f32() { float v = 0; raw(&v, 4); return v; }
    std::string str() {
        uint32_t n = u32();
        if (bad || n > in.size() - at) { bad = true; return {}; }
        std::string s = in.substr(at, n);
        at += n;
        return s;
    }
    template <class T>
    std::vector<T> array() {
        uint32_t n = u32();
        if (bad || (uint64_t)n * sizeof(T) > in.size() - at) { bad = true; return {}; }
        std::vector<T> v(n);
        raw(v.data(), (size_t)n * sizeof(T));
        return v;
    }
};

std::string suffix(const std::string& source) {
    size_t hash = source.rfind('#');
    return hash == std::string::npos ? std::string() : source.substr(hash);
}

void writeKeys(Writer& w, const AnimationKeys& k) {
    w.u8((uint8_t)k.interpolation);
    w.u32((uint32_t)k.width);
    w.array(k.times);
    w.array(k.values);
}

AnimationKeys readKeys(Reader& r) {
    AnimationKeys k;
    uint8_t interpolation = r.u8();
    k.interpolation = interpolation <= 2 ? (AnimationKeys::Interpolation)interpolation : AnimationKeys::Interpolation::Linear;
    k.width = (int)r.u32();
    k.times = r.array<float>();
    k.values = r.array<float>();
    return k;
}

}  // namespace

Compression compressionFromName(const std::string& name) {
    if (name == "none") return Compression::None;
    if (name == "bc1") return Compression::Small;
    return Compression::Quality;
}

const char* compressionName(Compression c) {
    return c == Compression::None ? "none" : c == Compression::Small ? "bc1" : "bc7";
}

std::string texturePath(const std::string& source) { return source + ".vktex"; }
std::string modelPath(const std::string& source) { return source + ".vkmodel"; }

bc::Format formatFor(const Texture& t, Compression c) {
    if (c == Compression::None) return bc::Format::RGBA8;
    if (c == Compression::Quality) return bc::Format::BC7;
    for (size_t i = 3; i < t.rgba.size(); i += 4)
        if (t.rgba[i] != 255) return bc::Format::BC3;
    return bc::Format::BC1;
}

std::string cookTexture(const Texture& t, Compression c, bool data) {
    bc::Format format = formatFor(t, c);
    uint32_t levels = t.mipmaps ? streaming::mipCount(t.width, t.height) : 1;
    Writer w;
    w.u32(kTextureMagic);
    w.u32(kVersion);
    w.u32((uint32_t)format);
    w.u32(t.width);
    w.u32(t.height);
    w.u32(levels);
    w.u32((t.nearest ? 1u : 0u) | (t.mipmaps ? 2u : 0u));
    for (uint32_t level = 0; level < levels; ++level) {
        uint32_t lw = t.width, lh = t.height;
        std::vector<uint8_t> pixels = level == 0 ? t.rgba : streaming::downsample(t.rgba.data(), t.width, t.height, level, lw, lh, data);
        w.array(bc::encode(format, pixels.data(), lw, lh));
    }
    return w.out;
}

bool readTexture(const std::string& bytes, Texture& out, std::string& error) {
    Reader r{bytes};
    if (r.u32() != kTextureMagic || r.u32() != kVersion) { error = "not a cooked texture (or from another engine version)"; return false; }
    uint32_t format = r.u32();
    out.width = r.u32();
    out.height = r.u32();
    uint32_t levels = r.u32(), flags = r.u32();
    if (r.bad || format > (uint32_t)bc::Format::BC7 || out.width == 0 || out.height == 0 || levels == 0 || levels > 32) {
        error = "damaged cooked texture";
        return false;
    }
    out.format = (bc::Format)format;
    out.nearest = (flags & 1) != 0;
    out.mipmaps = (flags & 2) != 0;
    out.rgba.clear();
    out.levels.clear();
    for (uint32_t level = 0; level < levels; ++level) {
        out.levels.push_back(r.array<uint8_t>());
        uint32_t lw = std::max(out.width >> level, 1u), lh = std::max(out.height >> level, 1u);
        if (r.bad || out.levels.back().size() != bc::imageBytes(out.format, lw, lh)) {
            error = "damaged cooked texture";
            return false;
        }
    }
    return true;
}

bool cookImageFile(const std::string& path, Compression c, std::string& out, std::string& error, bool data) {
    TextureLibrary lib;
    int id = loadTexture(lib, path, error);
    if (id < 0) return false;
    if (lib.get(id).cooked()) { error = path + ": already cooked"; return false; }
    out = cookTexture(lib.get(id), c, data);
    return true;
}

bool cookModel(const std::string& path, Compression c, std::string& out, std::string& error) {
    MeshLibrary meshes;   // (empty: the file's vertices start at 0)
    TextureLibrary images;
    if (!importGltfMeshes(meshes, path, error) || !importGltfImages(images, path, error) ||
        !importGltfAnimations(meshes, path, error))
        return false;
    bool skinned = false;
    for (const Mesh& m : meshes.meshes) skinned |= m.skinned;
    Writer w;
    w.u32(kModelMagic);
    w.u32(kVersion);
    w.array(meshes.vertices);
    w.array(skinned ? meshes.skin : std::vector<SkinVertex>{});
    w.array(meshes.indices);
    w.u32((uint32_t)meshes.meshes.size());
    for (const Mesh& m : meshes.meshes) {
        w.str(m.name);
        w.str(suffix(m.source));
        w.u32(m.firstIndex);
        w.u32(m.indexCount);
        w.u32((uint32_t)m.vertexOffset);
        for (float f : {m.boundsMin.x, m.boundsMin.y, m.boundsMin.z, m.boundsMax.x, m.boundsMax.y, m.boundsMax.z}) w.f32(f);
        w.u8(m.skinned ? 1 : 0);
        w.array(m.lods);
    }
    // Images: cooked in, unless they're files of their own (shared by models, maybe cooked themselves): named then
    std::vector<std::string> files;
    std::vector<bool> data;   // (normal, metal / rough and occlusion maps: mips averaged as numbers)
    if (!gltfImageFiles(path, files, error, &data)) return false;
    w.u32((uint32_t)images.count());
    for (const Texture& t : images.textures) {
        w.str(t.name);
        std::string at = suffix(t.source);
        w.str(at);
        size_t image = (size_t)std::atoi(at.c_str() + 6);   // ("#imageN")
        std::string file = image < files.size() ? files[image] : std::string();
        w.u8(file.empty() ? 0 : 1);
        w.str(file.empty() ? cookTexture(t, c, image < data.size() && data[image]) : fs::relative(fs::u8path(file), fs::u8path(path).parent_path()).generic_u8string());
    }
    w.u32((uint32_t)meshes.animations.size());
    for (const AnimationClip& clip : meshes.animations) {
        w.str(clip.name);
        w.str(suffix(clip.source));
        w.f32(clip.duration);
        w.u32((uint32_t)clip.tracks.size());
        for (const AnimationTrack& t : clip.tracks) {
            w.str(t.target);
            writeKeys(w, t.translation);
            writeKeys(w, t.rotation);
            writeKeys(w, t.scale);
        }
    }
    out = std::move(w.out);
    return true;
}

bool readModel(const std::string& bytes, const std::string& file, MeshLibrary* meshes, TextureLibrary* textures,
               MeshLibrary* clips, std::string& error) {
    Reader r{bytes};
    if (r.u32() != kModelMagic || r.u32() != kVersion) { error = file + ": not a cooked model (or from another engine version)"; return false; }
    std::vector<Vertex> vertices = r.array<Vertex>();
    std::vector<SkinVertex> skin = r.array<SkinVertex>();
    std::vector<uint32_t> indices = r.array<uint32_t>();
    uint32_t meshCount = r.u32();
    std::vector<Mesh> list;
    for (uint32_t i = 0; i < meshCount && !r.bad; ++i) {
        Mesh m;
        m.name = r.str();
        m.source = file + r.str();
        m.firstIndex = r.u32();
        m.indexCount = r.u32();
        m.vertexOffset = (int32_t)r.u32();
        m.boundsMin = {r.f32(), r.f32(), r.f32()};
        m.boundsMax = {r.f32(), r.f32(), r.f32()};
        m.skinned = r.u8() != 0;
        m.lods = r.array<MeshLod>();
        // Every index (relative to its mesh's or LOD's first vertex) inside the file's vertices
        auto inside = [&](uint32_t first, uint32_t count, int32_t offset) {
            if ((uint64_t)first + count > indices.size() || offset < 0) return false;
            for (uint32_t k = first; k < first + count; ++k)
                if ((uint64_t)(uint32_t)offset + indices[k] >= vertices.size()) return false;
            return true;
        };
        bool ok = inside(m.firstIndex, m.indexCount, m.vertexOffset);
        for (const MeshLod& l : m.lods) ok = ok && inside(l.firstIndex, l.indexCount, l.vertexOffset);
        if (!ok) r.bad = true;
        list.push_back(std::move(m));
    }
    if (!skin.empty() && skin.size() != vertices.size()) r.bad = true;
    if (r.bad) { error = file + ": damaged cooked model"; return false; }

    if (meshes) {
        auto loaded = [&](const std::string& source) {
            for (const Mesh& have : meshes->meshes)
                if (have.source == source) return true;
            return false;
        };
        bool missing = false;
        for (const Mesh& m : list) missing |= !loaded(m.source);
        if (missing) {
            // The file's arrays go on the end of the library's; its meshes and LODs move with them
            uint32_t baseIndex = (uint32_t)meshes->indices.size(), baseVertex = (uint32_t)meshes->vertices.size();
            meshes->skin.resize(meshes->vertices.size());
            meshes->vertices.insert(meshes->vertices.end(), vertices.begin(), vertices.end());
            if (skin.empty()) meshes->skin.resize(meshes->vertices.size());
            else meshes->skin.insert(meshes->skin.end(), skin.begin(), skin.end());
            meshes->indices.insert(meshes->indices.end(), indices.begin(), indices.end());
            for (Mesh& m : list) {
                if (loaded(m.source)) continue;
                m.firstIndex += baseIndex;
                m.vertexOffset += (int32_t)baseVertex;
                for (MeshLod& l : m.lods) {
                    l.firstIndex += baseIndex;
                    l.vertexOffset += (int32_t)baseVertex;
                }
                std::string unique = m.name;
                for (int n = 2; meshes->find(unique) >= 0; ++n) unique = m.name + " " + std::to_string(n);
                m.name = unique;
                meshes->meshes.push_back(std::move(m));
            }
            ++meshes->version;
        }
    }

    uint32_t imageCount = r.u32();
    for (uint32_t i = 0; i < imageCount && !r.bad; ++i) {
        std::string name = r.str(), source = file + r.str();
        bool own = r.u8() != 0;
        std::string data = r.str();   // (the cooked image, or its file's path from the model's folder)
        if (!textures || r.bad) continue;
        bool have = false;
        for (const Texture& t : textures->textures) have |= t.source == source;
        if (have) continue;
        Texture t;
        if (own) {   // (loaded as any image file is: cooked if it was)
            TextureLibrary one;
            std::string image = (fs::u8path(file).parent_path() / fs::u8path(data)).u8string();
            std::string why;
            int id = loadTexture(one, image, why);
            if (id < 0) continue;   // (as the importer does: untextured)
            t = std::move(one.textures[(size_t)id]);
        } else if (!readTexture(data, t, error)) {
            error = file + ": " + error;
            return false;
        }
        int id = addTexture(*textures, name, source, t.width, t.height, std::move(t.rgba));
        Texture& added = textures->textures[(size_t)id];
        added.format = t.format;
        added.levels = std::move(t.levels);
        added.nearest = t.nearest;
        added.mipmaps = t.mipmaps;
    }

    uint32_t clipCount = r.u32();
    for (uint32_t i = 0; i < clipCount && !r.bad; ++i) {
        AnimationClip clip;
        clip.name = r.str();
        clip.source = file + r.str();
        clip.duration = r.f32();
        uint32_t tracks = r.u32();
        for (uint32_t k = 0; k < tracks && !r.bad; ++k) {
            AnimationTrack t;
            t.target = r.str();
            t.translation = readKeys(r);
            t.rotation = readKeys(r);
            t.scale = readKeys(r);
            clip.tracks.push_back(std::move(t));
        }
        if (!clips || r.bad) continue;
        bool have = false;
        for (const AnimationClip& c : clips->animations) have |= c.source == clip.source;
        if (have) continue;
        std::string base = clip.name;
        for (int n = 2; clips->findAnimation(clip.name); ++n) clip.name = base + " " + std::to_string(n);
        clips->animations.push_back(std::move(clip));
    }
    if (r.bad) { error = file + ": damaged cooked model"; return false; }
    return true;
}

}  // namespace cook
