#include "assets/gltf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#define CGLTF_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "cgltf.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace fs = std::filesystem;

namespace {

struct DataDeleter {
    void operator()(cgltf_data* d) const { cgltf_free(d); }
};
using Data = std::unique_ptr<cgltf_data, DataDeleter>;

std::string canonical(const std::string& path) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(path, ec), ec);
    return (ec ? fs::path(path) : abs).u8string();
}

Data load(const std::string& path, std::string& error) {
    cgltf_options options{};
    cgltf_data* raw = nullptr;
    cgltf_result r = cgltf_parse_file(&options, path.c_str(), &raw);
    Data data(raw);
    if (r != cgltf_result_success) {
        error = path + ": not a readable glTF file (cgltf error " + std::to_string((int)r) + ")";
        return nullptr;
    }
    if (cgltf_load_buffers(&options, data.get(), path.c_str()) != cgltf_result_success) {
        error = path + ": cannot load its buffers (.bin files next to it?)";
        return nullptr;
    }
    return data;
}

std::string meshSource(const std::string& file, size_t mesh, size_t prim) {
    return file + "#" + std::to_string(mesh) + "." + std::to_string(prim);
}

std::string imageSource(const std::string& file, size_t image) { return file + "#image" + std::to_string(image); }
std::string animationSource(const std::string& file, size_t anim) { return file + "#anim" + std::to_string(anim); }

// Every node's entity name: its own (made unique), or "Node <index>". The same for every import of a file, so
// skins and animation tracks can find their nodes' entities by name.
std::vector<std::string> nodeNames(const cgltf_data& data) {
    std::vector<std::string> names(data.nodes_count);
    std::vector<std::string> taken;
    for (size_t i = 0; i < data.nodes_count; ++i) {
        const char* n = data.nodes[i].name;
        names[i] = n && *n ? n : "Node " + std::to_string(i);
    }
    for (size_t i = 0; i < names.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (names[j] == names[i]) {
                names[i] += " (" + std::to_string(i) + ")";
                break;
            }
    return names;
}

const cgltf_accessor* attribute(const cgltf_primitive& p, cgltf_attribute_type type) {
    for (size_t i = 0; i < p.attributes_count; ++i)
        if (p.attributes[i].type == type && p.attributes[i].index == 0) return p.attributes[i].data;
    return nullptr;
}

// One triangle primitive -> library mesh. Missing normals are computed (smooth); missing UVs are zero.
int importPrimitive(MeshLibrary& lib, const std::string& name, const std::string& source, const cgltf_primitive& p) {
    if (p.type != cgltf_primitive_type_triangles) return -1;
    const cgltf_accessor* pos = attribute(p, cgltf_attribute_type_position);
    if (!pos || pos->count == 0) return -1;
    const cgltf_accessor* nrm = attribute(p, cgltf_attribute_type_normal);
    const cgltf_accessor* uv = attribute(p, cgltf_attribute_type_texcoord);
    const cgltf_accessor* joints = attribute(p, cgltf_attribute_type_joints);
    const cgltf_accessor* weights = attribute(p, cgltf_attribute_type_weights);

    std::vector<Vertex> verts(pos->count);
    std::vector<SkinVertex> skin;
    if (joints && weights && joints->count == pos->count && weights->count == pos->count) {
        skin.resize(pos->count);
        for (size_t i = 0; i < pos->count; ++i) {
            cgltf_uint j[4] = {0, 0, 0, 0};
            float w[4] = {0, 0, 0, 0};
            cgltf_accessor_read_uint(joints, i, j, 4);
            cgltf_accessor_read_float(weights, i, w, 4);
            float sum = w[0] + w[1] + w[2] + w[3];
            for (int k = 0; k < 4; ++k) {
                skin[i].joints[k] = (uint16_t)std::min<cgltf_uint>(j[k], 65535);
                skin[i].weights[k] = sum > 0.f ? w[k] / sum : (k == 0 ? 1.f : 0.f);
            }
        }
    }
    for (size_t i = 0; i < pos->count; ++i) {
        float v[3] = {0, 0, 0}, n[3] = {0, 0, 0}, t[2] = {0, 0};
        cgltf_accessor_read_float(pos, i, v, 3);
        if (nrm) cgltf_accessor_read_float(nrm, i, n, 3);
        if (uv) cgltf_accessor_read_float(uv, i, t, 2);
        verts[i] = {{v[0], v[1], v[2]}, {n[0], n[1], n[2]}, t[0], t[1]};
    }
    std::vector<uint32_t> indices;
    if (p.indices) {
        indices.resize(p.indices->count);
        for (size_t i = 0; i < p.indices->count; ++i) indices[i] = (uint32_t)cgltf_accessor_read_index(p.indices, i);
    } else {
        for (uint32_t i = 0; i < (uint32_t)pos->count; ++i) indices.push_back(i);
    }
    indices.resize(indices.size() / 3 * 3);
    for (uint32_t& i : indices)
        if (i >= verts.size()) i = 0;   // malformed index: degenerate rather than out of bounds
    if (!nrm) {
        for (Vertex& v : verts) v.normal = {};
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            Vertex &a = verts[indices[i]], &b = verts[indices[i + 1]], &c = verts[indices[i + 2]];
            Vec3 n = cross(b.pos - a.pos, c.pos - a.pos);   // area weighted
            a.normal += n;
            b.normal += n;
            c.normal += n;
        }
        for (Vertex& v : verts) v.normal = normalize(v.normal);
    }
    if (!skin.empty()) return addSkinnedMesh(lib, name, source, verts, skin, indices);
    return addMesh(lib, name, source, verts, indices);
}

// Image bytes, from the .glb buffer, a data: URI or a file next to the model
bool imageBytes(const cgltf_image& img, const std::string& path, std::vector<uint8_t>& out, std::string& error) {
    if (img.buffer_view) {
        const uint8_t* base = (const uint8_t*)img.buffer_view->buffer->data;
        if (!base) { error = "image buffer not loaded"; return false; }
        out.assign(base + img.buffer_view->offset, base + img.buffer_view->offset + img.buffer_view->size);
        return true;
    }
    if (!img.uri) { error = "image without data"; return false; }
    if (std::strncmp(img.uri, "data:", 5) == 0) {
        const char* comma = std::strchr(img.uri, ',');
        if (!comma || !std::strstr(img.uri, ";base64")) { error = "unsupported data URI"; return false; }
        size_t len = std::strlen(comma + 1), size = len / 4 * 3;
        while (len > 0 && comma[len] == '=') { --len; --size; }   // padding
        void* decoded = nullptr;
        cgltf_options options{};
        if (cgltf_load_buffer_base64(&options, size, comma + 1, &decoded) != cgltf_result_success) {
            error = "bad base64 image";
            return false;
        }
        out.assign((uint8_t*)decoded, (uint8_t*)decoded + size);
        std::free(decoded);
        return true;
    }
    std::string uri = img.uri;
    std::vector<char> decodedUri(uri.begin(), uri.end());
    decodedUri.push_back('\0');
    cgltf_decode_uri(decodedUri.data());   // %20 etc.
    fs::path file = fs::path(path).parent_path() / fs::u8path(decodedUri.data());
    std::ifstream f(file, std::ios::binary);
    if (!f) { error = "cannot open " + file.u8string(); return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    out.assign(s.begin(), s.end());
    return true;
}

// Imports every image; ids[i] is image i's texture id (or -1 if it couldn't be decoded)
bool importImages(TextureLibrary& lib, const cgltf_data& data, const std::string& file, std::vector<int>& ids,
                  std::string& error) {
    ids.assign(data.images_count, -1);
    for (size_t i = 0; i < data.images_count; ++i) {
        std::string source = imageSource(file, i);
        for (int t = 0; t < lib.count(); ++t)
            if (lib.get(t).source == source) ids[i] = t;
        if (ids[i] >= 0) continue;
        std::vector<uint8_t> bytes;
        std::string why;
        const char* name = data.images[i].name ? data.images[i].name : nullptr;
        std::string label = name && *name ? name : fs::path(file).stem().u8string() + " image " + std::to_string(i);
        if (imageBytes(data.images[i], file, bytes, why))
            ids[i] = loadTextureFromMemory(lib, label, source, bytes.data(), bytes.size(), why);
        if (ids[i] < 0) error += (error.empty() ? "" : "; ") + label + ": " + why;   // keep going: just untextured
    }
    return true;
}

// Imports every primitive; ids[mesh][prim] is its mesh id (or -1 for non-triangle primitives)
void importMeshes(MeshLibrary& lib, const cgltf_data& data, const std::string& file,
                  std::vector<std::vector<int>>& ids) {
    ids.assign(data.meshes_count, {});
    for (size_t m = 0; m < data.meshes_count; ++m) {
        const cgltf_mesh& mesh = data.meshes[m];
        std::string base = mesh.name && *mesh.name ? mesh.name : fs::path(file).stem().u8string() + " mesh " + std::to_string(m);
        for (size_t p = 0; p < mesh.primitives_count; ++p) {
            std::string source = meshSource(file, m, p);
            int id = -1;
            for (int i = 0; i < lib.count(); ++i)
                if (lib.get(i).source == source) id = i;
            if (id < 0)
                id = importPrimitive(lib, mesh.primitives_count > 1 ? base + " " + std::to_string(p) : base, source,
                                     mesh.primitives[p]);
            ids[m].push_back(id);
        }
    }
}

AnimationKeys::Interpolation interpolationOf(cgltf_interpolation_type t) {
    if (t == cgltf_interpolation_type_step) return AnimationKeys::Interpolation::Step;
    if (t == cgltf_interpolation_type_cubic_spline) return AnimationKeys::Interpolation::CubicSpline;
    return AnimationKeys::Interpolation::Linear;
}

// Imports every animation; names[i] is animation i's clip name in the library
void importAnimations(MeshLibrary& lib, const cgltf_data& data, const std::string& file, std::vector<std::string>& names) {
    std::vector<std::string> nodes = nodeNames(data);
    std::string stem = fs::path(file).stem().u8string();
    names.clear();
    for (size_t a = 0; a < data.animations_count; ++a) {
        const cgltf_animation& anim = data.animations[a];
        std::string source = animationSource(file, a);
        bool have = false;
        for (const AnimationClip& c : lib.animations)
            if (c.source == source) { names.push_back(c.name); have = true; }
        if (have) continue;
        AnimationClip clip;
        clip.name = stem + "/" + (anim.name && *anim.name ? std::string(anim.name) : "Animation " + std::to_string(a));
        for (int n = 2; lib.findAnimation(clip.name); ++n) clip.name += " " + std::to_string(n);
        clip.source = source;
        for (size_t c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& ch = anim.channels[c];
            if (!ch.target_node || !ch.sampler || !ch.sampler->input || !ch.sampler->output) continue;
            int width = ch.target_path == cgltf_animation_path_type_rotation ? 4 : 3;
            if (ch.target_path != cgltf_animation_path_type_translation && ch.target_path != cgltf_animation_path_type_rotation &&
                ch.target_path != cgltf_animation_path_type_scale)
                continue;   // morph target weights: not supported
            const std::string& target = nodes[(size_t)(ch.target_node - data.nodes)];
            AnimationTrack* track = nullptr;
            for (AnimationTrack& t : clip.tracks)
                if (t.target == target) track = &t;
            if (!track) {
                clip.tracks.push_back({target, {}, {}, {}});
                track = &clip.tracks.back();
            }
            AnimationKeys& keys = width == 4 ? track->rotation
                                  : ch.target_path == cgltf_animation_path_type_scale ? track->scale : track->translation;
            keys.width = width;
            keys.interpolation = interpolationOf(ch.sampler->interpolation);
            const cgltf_accessor* in = ch.sampler->input;
            const cgltf_accessor* out = ch.sampler->output;
            keys.times.resize(in->count);
            for (size_t k = 0; k < in->count; ++k) cgltf_accessor_read_float(in, k, &keys.times[k], 1);
            keys.values.resize(out->count * (size_t)width);
            for (size_t k = 0; k < out->count; ++k) cgltf_accessor_read_float(out, k, &keys.values[k * (size_t)width], (cgltf_size)width);
            if (!keys.times.empty()) clip.duration = std::max(clip.duration, keys.times.back());
        }
        names.push_back(clip.name);
        lib.animations.push_back(std::move(clip));
    }
}

}  // namespace

bool importGltfAnimations(MeshLibrary& meshes, const std::string& path, std::string& error) {
    Data data = load(path, error);
    if (!data) return false;
    std::vector<std::string> names;
    importAnimations(meshes, *data, canonical(path), names);
    return true;
}

int importAnimationClip(MeshLibrary& meshes, const std::string& source, std::string& error) {
    for (size_t i = 0; i < meshes.animations.size(); ++i)
        if (meshes.animations[i].source == source) return (int)i;
    size_t hash = source.rfind('#');
    if (hash == std::string::npos) { error = source + ": not an animation source"; return -1; }
    if (!importGltfAnimations(meshes, source.substr(0, hash), error)) return -1;
    for (size_t i = 0; i < meshes.animations.size(); ++i)
        if (meshes.animations[i].source == source) return (int)i;
    error = source + ": no such animation in the file";
    return -1;
}

bool importGltfMeshes(MeshLibrary& meshes, const std::string& path, std::string& error) {
    Data data = load(path, error);
    if (!data) return false;
    std::vector<std::vector<int>> ids;
    importMeshes(meshes, *data, canonical(path), ids);
    return true;
}

bool importGltfImages(TextureLibrary& textures, const std::string& path, std::string& error) {
    Data data = load(path, error);
    if (!data) return false;
    std::vector<int> ids;
    std::string warnings;
    return importImages(textures, *data, canonical(path), ids, warnings);
}

bool importGltf(const std::string& path, MeshLibrary& meshes, TextureLibrary& textures, GltfModel& out,
                std::string& error) {
    Data data = load(path, error);
    if (!data) return false;
    std::string file = canonical(path), warnings;
    std::vector<int> imageIds;
    importImages(textures, *data, file, imageIds, warnings);
    std::vector<std::vector<int>> meshIds;
    importMeshes(meshes, *data, file, meshIds);
    std::vector<std::string> names = nodeNames(*data);

    GltfModel model;
    importAnimations(meshes, *data, file, model.animations);
    for (size_t i = 0; i < data->skins_count; ++i) {
        const cgltf_skin& sk = data->skins[i];
        GltfModel::Skin skin;
        for (size_t j = 0; j < sk.joints_count; ++j) {
            skin.joints.push_back(names[(size_t)(sk.joints[j] - data->nodes)]);
            Mat4 ibm;   // identity when the file has none
            if (sk.inverse_bind_matrices) cgltf_accessor_read_float(sk.inverse_bind_matrices, j, ibm.m, 16);
            skin.inverseBind.push_back(ibm);
        }
        model.skins.push_back(std::move(skin));
    }
    for (size_t i = 0; i < data->materials_count; ++i) {
        const cgltf_material& m = data->materials[i];
        GltfModel::Material mat;
        mat.name = m.name ? m.name : "";
        if (m.has_pbr_metallic_roughness) {
            const float* c = m.pbr_metallic_roughness.base_color_factor;
            mat.color = {std::pow(c[0], 1.f / 2.2f), std::pow(c[1], 1.f / 2.2f), std::pow(c[2], 1.f / 2.2f)};
            const cgltf_texture* tex = m.pbr_metallic_roughness.base_color_texture.texture;
            if (tex && tex->image) mat.texture = imageIds[(size_t)(tex->image - data->images)];
            mat.opacity = c[3];
            mat.metallic = m.pbr_metallic_roughness.metallic_factor;
            mat.roughness = m.pbr_metallic_roughness.roughness_factor;
            const cgltf_texture* mr = m.pbr_metallic_roughness.metallic_roughness_texture.texture;
            if (mr && mr->image) mat.metalRoughTexture = imageIds[(size_t)(mr->image - data->images)];
        }
        if (const cgltf_texture* n = m.normal_texture.texture; n && n->image) {
            mat.normalTexture = imageIds[(size_t)(n->image - data->images)];
            mat.normalScale = m.normal_texture.scale;
        }
        mat.alphaMode = m.alpha_mode == cgltf_alpha_mode_mask ? 1 : m.alpha_mode == cgltf_alpha_mode_blend ? 2 : 0;
        mat.alphaCutoff = m.alpha_cutoff;
        const float* e = m.emissive_factor;
        mat.emissive = {std::pow(e[0], 1.f / 2.2f), std::pow(e[1], 1.f / 2.2f), std::pow(e[2], 1.f / 2.2f)};
        if (m.has_emissive_strength) mat.emissiveStrength = m.emissive_strength.emissive_strength;
        if (const cgltf_texture* et = m.emissive_texture.texture; et && et->image) {
            mat.emissiveTexture = imageIds[(size_t)(et->image - data->images)];
            if (e[0] == 0.f && e[1] == 0.f && e[2] == 0.f) mat.emissive = {1, 1, 1};   // a texture alone means "as is"
        }
        model.materials.push_back(mat);
    }

    // Nodes in parents-first order: walk the scene's trees (or every root, without a scene)
    std::vector<const cgltf_node*> roots;
    if (data->scene) roots.assign(data->scene->nodes, data->scene->nodes + data->scene->nodes_count);
    else
        for (size_t i = 0; i < data->nodes_count; ++i)
            if (!data->nodes[i].parent) roots.push_back(&data->nodes[i]);
    struct Pending {
        const cgltf_node* node;
        int parent;
    };
    std::vector<Pending> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.push_back({*it, -1});
    while (!stack.empty() && model.nodes.size() < 100000) {
        Pending p = stack.back();
        stack.pop_back();
        GltfModel::Node n;
        n.name = names[(size_t)(p.node - data->nodes)];
        if (p.node->skin) n.skin = (int)(p.node->skin - data->skins);
        n.parent = p.parent;
        cgltf_node_transform_local(p.node, n.local.m);
        if (const cgltf_mesh* mesh = p.node->mesh) {
            size_t m = (size_t)(mesh - data->meshes);
            for (size_t k = 0; k < mesh->primitives_count; ++k) {
                if (meshIds[m][k] < 0) continue;
                n.meshes.push_back(meshIds[m][k]);
                const cgltf_material* mat = mesh->primitives[k].material;
                n.materials.push_back(mat ? (int)(mat - data->materials) : -1);
            }
        }
        int index = (int)model.nodes.size();
        model.nodes.push_back(std::move(n));
        for (size_t c = p.node->children_count; c-- > 0;) stack.push_back({p.node->children[c], index});
    }
    if (model.nodes.empty()) {
        error = path + ": no nodes to import";
        return false;
    }
    out = std::move(model);
    if (!warnings.empty()) error = warnings;   // succeeded, but some images are missing
    return true;
}
