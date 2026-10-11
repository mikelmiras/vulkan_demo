// A large scene for measuring the engine: the demo scene plus a field of static objects, rigid bodies and triggers.
#pragma once

#include "scene/scene.h"

struct StressSettings {
    int statics = 4000;     // boxes, spheres and pyramids with solid colliders, a quarter of them spinning
    int bodies = 300;       // crates and balls dropped onto the field
    int triggers = 200;     // bobbing trigger spheres
};

Scene buildStressScene(const StressSettings& settings = {});
