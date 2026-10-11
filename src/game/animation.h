// Plays Animator components: advances their clips and poses the entities they drive (by name, see Animator).
#pragma once

#include "assets/meshes.h"
#include "scene/scene.h"

// Advances every playing Animator by dt seconds (times its speed) and poses its entities; dt = 0 just poses them
void updateAnimators(Scene& scene, const MeshLibrary& meshes, float dt);
