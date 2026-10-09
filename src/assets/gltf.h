// glTF 2.0 import (.gltf with its .bin and images, or binary .glb), via cgltf.
#pragma once

#include <string>
#include <vector>

#include "assets/meshes.h"
#include "assets/textures.h"
#include "core/vecmath.h"

// A glTF file as imported: meshes and textures go into the libraries, this describes how they're put together
struct GltfModel {
    struct Material {
        std::string name;
        Vec3 color{1, 1, 1};   // sRGB (glTF stores linear base colour; converted)
        int texture = -1;      // TextureLibrary id of the base colour texture
    };
    struct Node {
        std::string name;
        int parent = -1;                     // index into nodes
        Mat4 local;                          // relative to the parent
        std::vector<int> meshes;             // MeshLibrary ids, one per primitive
        std::vector<int> materials;          // index into materials per primitive, or -1
    };
    std::vector<Material> materials;
    std::vector<Node> nodes;                 // parents before children
};

// Everything: meshes (sources "file#mesh.primitive"), images ("file#imageN"), materials and the node tree
bool importGltf(const std::string& path, MeshLibrary& meshes, TextureLibrary& textures, GltfModel& out,
                std::string& error);
// Only the meshes / only the images (re-importing from a scene file)
bool importGltfMeshes(MeshLibrary& meshes, const std::string& path, std::string& error);
bool importGltfImages(TextureLibrary& textures, const std::string& path, std::string& error);
