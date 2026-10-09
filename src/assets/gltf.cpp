#include "assets/gltf.h"

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

    std::vector<Vertex> verts(pos->count);
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

}  // namespace

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

    GltfModel model;
    for (size_t i = 0; i < data->materials_count; ++i) {
        const cgltf_material& m = data->materials[i];
        GltfModel::Material mat;
        mat.name = m.name ? m.name : "";
        if (m.has_pbr_metallic_roughness) {
            const float* c = m.pbr_metallic_roughness.base_color_factor;
            mat.color = {std::pow(c[0], 1.f / 2.2f), std::pow(c[1], 1.f / 2.2f), std::pow(c[2], 1.f / 2.2f)};
            const cgltf_texture* tex = m.pbr_metallic_roughness.base_color_texture.texture;
            if (tex && tex->image) mat.texture = imageIds[(size_t)(tex->image - data->images)];
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
        n.name = p.node->name && *p.node->name ? p.node->name : "Node " + std::to_string(model.nodes.size());
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
