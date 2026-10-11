// Ragdolls: a skinned model's skeleton as rigid bodies held together by joints. Made in the editor (the Skin
// component's "Make ragdoll") or by code; its parts start kinematic, so the Animator still moves them (and they
// push things), until setRagdollLimp() hands them to the physics.
#pragma once

#include "scene/scene.h"

// Gives every bone of the model under `root` (the joints of its Skin components) a collider (a capsule along the
// bone where it points along its Y, else a sphere), a kinematic rigid body (masses shared out by length) and a
// cone joint to its parent bone. Bones that already have a collider keep it. Returns the number of parts.
int makeRagdoll(Scene& scene, EntityId root, float totalMass = 70.f);

// Limp: every rigid body under `root` is simulated (and the model's Animator stops). Not limp: kinematic again,
// following animation.
void setRagdollLimp(Scene& scene, EntityId root, bool limp);
