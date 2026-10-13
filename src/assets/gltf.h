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
        float metallic = 1.f, roughness = 1.f;   // (glTF's defaults)
        int metalRoughTexture = -1;
        int normalTexture = -1;
        float normalScale = 1.f;
        Vec3 emissive{0, 0, 0};                  // sRGB
        float emissiveStrength = 1.f;
        int emissiveTexture = -1;
        int alphaMode = 0;                       // 0 opaque, 1 mask, 2 blend (MeshRenderer::AlphaMode)
        float opacity = 1.f, alphaCutoff = 0.5f;
    };
    struct Skin {
        std::vector<std::string> joints;     // node names (the joint entities)
        std::vector<Mat4> inverseBind;       // per joint
    };
    struct Node {
        std::string name;                    // unique within the model
        int parent = -1;                     // index into nodes
        Mat4 local;                          // relative to the parent
        std::vector<int> meshes;             // MeshLibrary ids, one per primitive
        std::vector<int> materials;          // index into materials per primitive, or -1
        int skin = -1;                       // index into skins: its meshes are deformed by that skeleton
    };
    std::vector<Material> materials;
    std::vector<Skin> skins;
    std::vector<Node> nodes;                 // parents before children
    std::vector<std::string> animations;     // clip names in the MeshLibrary
};

// Everything: meshes (sources "file#mesh.primitive"), images ("file#imageN"), animations ("file#animN", into
// meshes.animations), materials, skins and the node tree
bool importGltf(const std::string& path, MeshLibrary& meshes, TextureLibrary& textures, GltfModel& out,
                std::string& error);
// Only the meshes / only the images (re-importing from a scene file)
bool importGltfMeshes(MeshLibrary& meshes, const std::string& path, std::string& error);
bool importGltfImages(TextureLibrary& textures, const std::string& path, std::string& error);
bool importGltfAnimations(MeshLibrary& meshes, const std::string& path, std::string& error);
// Each image's own file when it has one next to the model ("" for one inside it: a .glb's buffer, a data: URI)
bool gltfImageFiles(const std::string& path, std::vector<std::string>& files, std::string& error);
// A clip from its source ("model.glb#anim1"): its index in meshes.animations, or -1 with `error`
int importAnimationClip(MeshLibrary& meshes, const std::string& source, std::string& error);
