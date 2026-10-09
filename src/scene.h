// Editable scene objects, default scene, and a small text file format for save/load.
#pragma once

#include <string>
#include <vector>

#include "geometry.h"
#include "vecmath.h"

struct SceneObject {
    std::string name;
    int mesh = (int)MeshKind::Cube;   // id in the MeshLibrary
    float position[3] = {0, 0, 0};
    float rotation[3] = {0, 0, 0};   // Euler degrees (ImGuizmo convention)
    float scale[3] = {1, 1, 1};
    Vec3 color{0.8f, 0.8f, 0.8f};    // sRGB, as shown in the colour picker
    bool checker = false;            // procedural checkerboard + grid (used by the ground)
    bool visible = true;
    bool locked = false;             // not pickable by clicking in the viewport
    float spin = 0.f;                // rad/s around world Y (when animation plays)
    float bob = 0.f;                 // vertical bobbing amplitude
    float phase = 0.f;               // animation phase offset (radians)

    Vec3 pos() const { return {position[0], position[1], position[2]}; }
    Mat4 baseMatrix() const;               // the editable transform (what the gizmo moves)
    Mat4 animatedMatrix(float t) const;    // baseMatrix plus spin/bob animation
};

std::vector<SceneObject> buildDefaultScene();

// Imported meshes are saved as their OBJ paths and re-imported into `meshes` on load.
bool saveScene(const std::string& path, const std::vector<SceneObject>& objects, const MeshLibrary& meshes,
               std::string& error);
bool loadScene(const std::string& path, std::vector<SceneObject>& objects, MeshLibrary& meshes, std::string& error);
