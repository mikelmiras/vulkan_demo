// Editor inspector: the selection's components and behaviours, the scene's look, renderer info.
#include "editor/editor.h"

#include <cctype>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <functional>

#include "misc/cpp/imgui_stdlib.h"
#include "platform/platform.h"
#include "scene/prefab_sync.h"
#include "editor/editor_ui.h"

using namespace editor_ui;

void Editor::track() {
    if (ImGui::IsItemActivated()) editBegan_ = true;
}

// ---------------------------------------------------------------------------------------------
// Inspector
// ---------------------------------------------------------------------------------------------
void Editor::drawInspector(const std::string& gpuName, const RenderStats& stats) {
    ImGuiIO& io = ImGui::GetIO();
    float w = io.DisplaySize.x - vx_ - vw_;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - w, topH_));
    ImGui::SetNextWindowSize(ImVec2(w, io.DisplaySize.y - topH_ - statusH_));
    ImGui::Begin("Inspector", nullptr, kPanelFlags);
    ImGui::PushItemWidth(-px(110.f));
    if (playing()) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "Playing: changes are lost on Stop");

    if (Entity* sel = selection()) {
        Entity& e = *sel;
        ImGui::InputText("Name", &e.name);
        track();
        ImGui::Checkbox("Visible", &e.visible);
        track();
        ImGui::SameLine();
        ImGui::Checkbox("Locked", &e.locked);
        track();
        tooltip("Locked objects ignore viewport clicks (select them in the outliner)");
        if (const Entity* parent = scene_.find(e.parent)) {
            ImGui::TextDisabled("Child of %s", parent->name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Unparent")) pendingReparent_ = e.id, pendingParent_ = kNoEntity;
        }

        ImGui::SeparatorText(e.parent != kNoEntity ? "Transform (relative to parent)" : "Transform");
        Transform& t = e.transform;
        ImGui::DragFloat3("Position", t.position, 0.05f, 0.f, 0.f, "%.2f");
        track();
        ImGui::DragFloat3("Rotation", t.rotation, 0.5f, 0.f, 0.f, "%.1f");
        track();
        ImGui::DragFloat3("Scale", t.scale, 0.01f, 0.001f, 10000.f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        track();
        float uniform = (t.scale[0] + t.scale[1] + t.scale[2]) / 3.f, before = uniform;
        if (ImGui::DragFloat("Uniform scale", &uniform, 0.01f, 0.001f, 10000.f, "%.3f", ImGuiSliderFlags_AlwaysClamp) &&
            before > 0.f)
            for (float& s : t.scale) s = std::max(s * uniform / before, 0.001f);
        track();
        if (ImGui::Button("Reset rot")) { pushUndo(); t.rotation[0] = t.rotation[1] = t.rotation[2] = 0.f; }
        ImGui::SameLine();
        if (ImGui::Button("Reset scale")) { pushUndo(); t.scale[0] = t.scale[1] = t.scale[2] = 1.f; }
        ImGui::SameLine();
        ImGui::BeginDisabled(!e.meshRenderer);
        if (ImGui::Button("Drop to ground")) dropSelectedToGround();
        ImGui::EndDisabled();

        // Prefab instance: where it comes from, what it changed, and the instance operations
        if (e.prefab) {
            const Prefab* pf = scene_.findPrefab(e.prefab->prefab);
            EntityId root = instanceRoot(scene_, e.id);
            ImGui::SeparatorText("Prefab instance");
            if (!pf || root == kNoEntity) {
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "Its prefab is gone");
                if (ImGui::SmallButton("Unlink")) { pushUndo(); e.prefab.reset(); }
            } else {
                ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.f, 1.f), "Of prefab \"%s\"%s", pf->name.c_str(),
                                   root == e.id ? "" : " (part of it)");
                std::vector<std::string> overrides = instanceOverrides(scene_, e.id, meshes_, textures_);
                if (overrides.empty()) {
                    ImGui::TextDisabled("Same as the prefab");
                } else {
                    std::string list;
                    for (const std::string& o : overrides) list += (list.empty() ? "" : ", ") + o;
                    ImGui::TextWrapped("Overrides: %s", list.c_str());
                }
                if (root != e.id && ImGui::SmallButton("Select root")) select(root);
                if (root != e.id) ImGui::SameLine();
                int index = (int)(pf - scene_.prefabs.data());
                if (ImGui::SmallButton("Apply")) {
                    select(root);
                    updatePrefabFromSelection(index);
                }
                tooltip("Make the prefab like this instance; the other instances follow");
                ImGui::SameLine();
                if (ImGui::SmallButton("Revert")) {
                    pushUndo();
                    revertInstance(scene_, root, meshes_, textures_);
                    setStatus("Reverted to prefab " + pf->name);
                }
                tooltip("Drop this instance's overrides (it stays where it is)");
                ImGui::SameLine();
                if (ImGui::SmallButton("Unlink")) {
                    pushUndo();
                    unlinkInstance(scene_, root);
                    setStatus("Unlinked: ordinary entities now");
                }
                tooltip("Stop following the prefab");
            }
        }

        // (Apply / Revert above can add entities, which may move them in memory: look the selection up again)
        Entity& cur = *selection();
        drawComponents(cur);

        ImGui::SeparatorText("Info");
        ImGui::TextDisabled("Entity id %u", cur.id);
        Vec3 wp = transformPoint(worldOf(cur.id), {0, 0, 0});
        ImGui::Text("World position: %.2f %.2f %.2f", wp.x, wp.y, wp.z);
        ImGui::Text("Distance to camera: %.2f", length(wp - cam_.pos));
    } else {
        ImGui::TextWrapped("Nothing selected.");
        ImGui::TextDisabled("Click an object in the viewport\nor pick one in the outliner.");
        ImGui::SeparatorText("Scene look");
        RenderSettings& r = scene_.render;
        ImGui::ColorEdit3("Sky (overhead)", &r.zenith.x);
        track();
        ImGui::ColorEdit3("Horizon / fog", &r.sky.x);
        track();
        ImGui::ColorEdit3("Ground", &r.ground.x);
        track();
        ImGui::DragFloat("Fog", &r.fogDensity, 0.0002f, 0.f, 0.1f, "%.4f");
        track();
        ImGui::SliderFloat("Ambient", &r.ambient, 0.f, 2.f, "%.2f");
        track();
        ImGui::SliderFloat("Exposure", &r.exposure, 0.1f, 4.f, "%.2f");
        track();
        ImGui::SliderFloat("Bloom", &r.bloomStrength, 0.f, 0.5f, "%.3f");
        track();
        tooltip("How much bright parts (emissive materials, strong highlights) glow");
        ImGui::SliderFloat("Bloom threshold", &r.bloomThreshold, 0.f, 4.f, "%.2f");
        track();

        ImGui::SeparatorText("Navigation");
        NavigationSettings& n = scene_.navigation;
        ImGui::DragFloat("Cell size", &n.cellSize, 0.01f, 0.1f, 4.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
        track();
        tooltip("The walkable grid's resolution: smaller is finer (and slower to bake). Walls thinner than this "
                "may be walked through.");
        ImGui::DragFloat("Agent radius", &n.agentRadius, 0.01f, 0.f, 10.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
        track();
        ImGui::DragFloat("Agent height", &n.agentHeight, 0.01f, 0.1f, 20.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
        track();
        ImGui::DragFloat("Max climb", &n.maxClimb, 0.01f, 0.f, 5.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
        track();
        ImGui::DragFloat("Max slope", &n.maxSlope, 0.5f, 0.f, 89.f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
        track();
        if (ImGui::Button(showNavigation_ ? "Bake again" : "Bake and show")) {
            showNavigation_ = true;
            bakeNavigationPreview();
        }
        tooltip("Agents walk on the static colliders: the solid ones on entities without a rigid body or behaviours. "
                "The game bakes this itself when it first needs a path.");
        if (showNavigation_) {
            ImGui::SameLine();
            if (ImGui::Button("Hide")) showNavigation_ = false;
            ImGui::TextDisabled("%zu walkable cells, baked in %.0f ms", navPreview_.cellCount(), navPreview_.bakeMs());
        }
    }

    ImGui::SeparatorText("Editor camera");
    ImGui::DragFloat3("Cam position", &cam_.pos.x, 0.1f, 0.f, 0.f, "%.2f");
    float yawDeg = cam_.yaw / DEG2RAD, pitchDeg = cam_.pitch / DEG2RAD;
    if (ImGui::DragFloat("Yaw", &yawDeg, 0.5f, 0.f, 0.f, "%.1f")) cam_.yaw = yawDeg * DEG2RAD;
    if (ImGui::DragFloat("Pitch", &pitchDeg, 0.5f, -89.f, 89.f, "%.1f")) cam_.pitch = pitchDeg * DEG2RAD;
    ImGui::SliderAngle("FOV", &cam_.fovY, 20.f, 120.f);

    ImGui::SeparatorText("Renderer");
    ImGui::Text("GPU: %s", gpuName.c_str());
    ImGui::Text("%.0f FPS (%.2f ms)", fps_, fps_ > 0.f ? 1000.f / fps_ : 0.f);
    ImGui::Text("Draw calls: %u   Triangles: %u", stats.drawCalls, stats.triangles);
    ImGui::Text("Objects: %u drawn, %u culled, %u cast shadows", stats.objects, stats.culled, stats.shadowCasters);
    if (playing()) ImGui::Text("Game update: %.2f ms (fixed %.0f Hz)", game_.updateMs(), 1.f / Game::kFixedDt);
    ImGui::PopItemWidth();
    ImGui::End();
}

void Editor::drawComponents(Entity& e) {
    ImGui::Spacing();
    bool open = false;
    if (e.meshRenderer) {
        if (!componentHeader("Mesh Renderer", open)) { pushUndo(); e.meshRenderer.reset(); }
        else if (open) {
            MeshRenderer& mr = *e.meshRenderer;
            ImGui::Combo("Mesh", &mr.mesh, [](void* lib, int i) { return ((const MeshLibrary*)lib)->name(i); },
                         &meshes_, meshes_.count());
            track();
            ImGui::ColorEdit3("Color", &mr.color.x);
            track();
            int tex = mr.texture + 1;   // item 0 = none
            if (ImGui::Combo(
                    "Texture", &tex,
                    [](void* lib, int i) { return i == 0 ? "(none)" : ((const TextureLibrary*)lib)->name(i - 1); },
                    &textures_, textures_.count() + 1)) {
                pushUndo();
                mr.texture = tex - 1;
            }
            if (std::filesystem::path dropped; acceptAssetDrop(AssetKind::Texture, dropped)) assignTexture(e.id, dropped);
            tooltip("Drop a texture from the Assets panel here");
            if (ImGui::SmallButton("Load texture...")) {
                std::filesystem::path path = platform::openFileDialog("Load texture", "png,jpg,jpeg,tga,bmp");
                ImGui::GetIO().ClearInputKeys();
                ImGui::GetIO().ClearInputMouse();
                std::string error;
                int id = path.empty() ? -1 : loadTexture(textures_, path.u8string(), error);
                if (id >= 0) {
                    pushUndo();
                    mr.texture = id;
                    setStatus("Loaded texture " + textures_.get(id).name);
                } else if (!path.empty()) {
                    setStatus("Texture failed: " + error);
                }
            }
            if (mr.texture >= 0) {
                ImGui::DragFloat("Tiling", &mr.tiling, 0.01f, 0.01f, 1000.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                track();
            }
            ImGui::Checkbox("Checker pattern", &mr.checker);
            track();
            ImGui::SeparatorText("Material");
            ImGui::SliderFloat("Metallic", &mr.metallic, 0.f, 1.f, "%.2f");
            track();
            ImGui::SliderFloat("Roughness", &mr.roughness, 0.f, 1.f, "%.2f");
            track();
            auto texturePicker = [&](const char* label, int& id, const char* help) {
                int item = id + 1;   // 0 = none
                if (ImGui::Combo(
                        label, &item,
                        [](void* lib, int i) { return i == 0 ? "(none)" : ((const TextureLibrary*)lib)->name(i - 1); },
                        &textures_, textures_.count() + 1)) {
                    pushUndo();
                    id = item - 1;
                }
                tooltip(help);
                if (std::filesystem::path dropped; acceptAssetDrop(AssetKind::Texture, dropped)) {
                    std::string error;
                    int t = loadTexture(textures_, dropped.u8string(), error);
                    if (t >= 0) { pushUndo(); id = t; }
                    else setStatus("Texture failed: " + error);
                }
            };
            texturePicker("Metal/rough map", mr.metalRoughTexture, "glTF layout: green = roughness, blue = metallic");
            texturePicker("Normal map", mr.normalTexture, "Tangent space; drop a texture from the Assets panel here");
            if (mr.normalTexture >= 0) {
                ImGui::SliderFloat("Normal strength", &mr.normalScale, 0.f, 2.f, "%.2f");
                track();
            }
            ImGui::ColorEdit3("Emissive", &mr.emissive.x);
            track();
            ImGui::DragFloat("Emissive strength", &mr.emissiveStrength, 0.05f, 0.f, 100.f, "%.2f");
            track();
            tooltip("Light it gives off itself; above 1 it glows");
            texturePicker("Emissive map", mr.emissiveTexture, "Times the emissive colour");
            int alphaMode = (int)mr.alphaMode;
            if (ImGui::Combo("Transparency", &alphaMode, "Opaque\0Cut-out (mask)\0See-through (blend)\0")) {
                pushUndo();
                mr.alphaMode = (MeshRenderer::AlphaMode)alphaMode;
            }
            tooltip("Cut-out: holes where the texture's alpha is below the cutoff (leaves, grates). See-through: "
                    "glass, water, ghosts");
            if (mr.alphaMode == MeshRenderer::AlphaMode::Mask) {
                ImGui::SliderFloat("Alpha cutoff", &mr.alphaCutoff, 0.f, 1.f, "%.2f");
                track();
            } else if (mr.alphaMode == MeshRenderer::AlphaMode::Blend) {
                ImGui::SliderFloat("Opacity", &mr.opacity, 0.f, 1.f, "%.2f");
                track();
            }
            ImGui::TextDisabled("%u triangles", meshes_.get(mr.mesh).indexCount / 3);
        }
    }
    if (e.camera) {
        if (!componentHeader("Camera", open)) { pushUndo(); e.camera.reset(); }
        else if (open) {
            Camera& c = *e.camera;
            ImGui::SliderAngle("Field of view", &c.fovY, 20.f, 120.f);
            track();
            ImGui::DragFloat("Near", &c.zNear, 0.01f, 0.01f, 10.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::DragFloat("Far", &c.zFar, 10.f, 10.f, 100000.f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            track();
            const Entity* first = nullptr;
            for (const Entity& o : scene_.entities)
                if (o.camera) { first = &o; break; }
            if (first == &e) ImGui::TextDisabled("Active: the game renders through this camera");
            else ImGui::TextDisabled("Inactive: %s comes first", first ? first->name.c_str() : "?");
        }
    }
    if (e.animator) {
        if (!componentHeader("Animator", open)) { pushUndo(); e.animator.reset(); }
        else if (open) {
            drawAnimatorInspector(e);
        }
    }
    if (e.particleEmitter) {
        if (!componentHeader("Particle Emitter", open)) { pushUndo(); e.particleEmitter.reset(); }
        else if (open) {
            ParticleEmitter& p = *e.particleEmitter;
            ImGui::DragFloat("Rate", &p.rate, 0.5f, 0.f, 10000.f, "%.1f /s");
            track();
            ImGui::DragInt("Burst", &p.burst, 0.5f, 0, 10000);
            track();
            ImGui::SameLine();
            if (ImGui::SmallButton("Preview burst")) previewParticles_.burst(scene_, e.id);
            ImGui::Checkbox("Play on start", &p.playOnStart);
            track();
            ImGui::SameLine();
            ImGui::Checkbox("Additive", &p.additive);
            track();
            tooltip("Light adds up (fire, sparks, magic); off: see-through (smoke, dust)");
            ImGui::DragFloat("Duration", &p.duration, 0.05f, 0.f, 1000.f, p.duration > 0.f ? "%.2f s" : "for ever");
            track();
            ImGui::DragFloat("Lifetime", &p.lifetime, 0.01f, 0.05f, 60.f, "%.2f s");
            track();
            ImGui::DragFloat("Speed", &p.speed, 0.05f, 0.f, 200.f, "%.2f m/s");
            track();
            ImGui::SliderFloat("Spread", &p.spread, 0.f, 180.f, "%.0f deg");
            track();
            ImGui::DragFloat("Radius", &p.radius, 0.01f, 0.f, 100.f, "%.2f m");
            track();
            ImGui::DragFloat("Gravity", &p.gravity, 0.05f, -100.f, 100.f, "%.2f");
            track();
            ImGui::DragFloat("Drag", &p.drag, 0.01f, 0.f, 20.f, "%.2f");
            track();
            ImGui::DragFloat2("Size (start, end)", &p.startSize, 0.005f, 0.f, 50.f, "%.3f");
            track();
            ImGui::ColorEdit3("Start colour", &p.startColor.x);
            track();
            ImGui::ColorEdit3("End colour", &p.endColor.x);
            track();
            ImGui::DragFloat2("Opacity (start, end)", &p.startAlpha, 0.01f, 0.f, 1.f, "%.2f");
            track();
            ImGui::DragFloat("Brightness", &p.intensity, 0.05f, 0.f, 100.f, "%.2f");
            track();
            tooltip("Above 1 they glow (bloom)");
            ImGui::DragInt("Max particles", &p.maxParticles, 1.f, 1, 100000);
            track();
        }
    }
    if (e.skin) {
        if (!componentHeader("Skin", open)) { pushUndo(); e.skin.reset(); }
        else if (open) {
            ImGui::TextWrapped("Deforms the mesh with %zu joints, found by name under %s.", e.skin->joints.size(),
                               scene_.find(scene_.modelRoot(e.id)) ? scene_.find(scene_.modelRoot(e.id))->name.c_str() : "?");
            if (e.meshRenderer && !meshes_.get(e.meshRenderer->mesh).skinned)
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Its mesh has no joint weights: drawn as is");
        }
    }
    if (e.audioSource) {
        if (!componentHeader("Audio Source", open)) { pushUndo(); e.audioSource.reset(); }
        else if (open) {
            AudioSource& a = *e.audioSource;
            if (ImGui::BeginCombo("Clip", a.clip.c_str())) {
                for (const std::string& name : AudioEngine::builtinClips())
                    if (ImGui::Selectable(name.c_str(), a.clip == name)) { pushUndo(); a.clip = name; }
                ImGui::EndCombo();
            }
            ImGui::InputText("Clip file", &a.clip);
            track();
            if (std::filesystem::path dropped; acceptAssetDrop(AssetKind::Sound, dropped)) {
                pushUndo();
                a.clip = project::toStored(dropped.u8string());
            }
            if (ImGui::SmallButton("Browse...")) {
                std::filesystem::path path = platform::openFileDialog("Sound file", "wav,mp3,flac");
                ImGui::GetIO().ClearInputKeys();
                ImGui::GetIO().ClearInputMouse();
                if (!path.empty()) { pushUndo(); a.clip = project::toStored(path.u8string()); }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Preview") && audio_) {
                AudioEngine::PlayParams p;
                p.volume = a.volume;
                if (!audio_->play(a.clip, p)) setStatus("Can't play " + a.clip + ": " + audio_->error());
            }
            ImGui::SliderFloat("Volume", &a.volume, 0.f, 2.f, "%.2f");
            track();
            ImGui::Checkbox("Loop", &a.loop);
            track();
            ImGui::SameLine();
            ImGui::Checkbox("3D", &a.spatial);
            track();
            tooltip("Fades with distance from the camera and pans left/right");
            ImGui::Checkbox("Play on start", &a.playOnStart);
            track();
            ImGui::SameLine();
            ImGui::Checkbox("Play on impact", &a.playOnImpact);
            track();
            tooltip("When its rigid body hits something; louder for harder hits");
            if (a.spatial) {
                ImGui::DragFloatRange2("Distance", &a.minDistance, &a.maxDistance, 0.1f, 0.f, 1000.f, "%.1f", "%.1f");
                track();
            }
            if (!audio_ || !audio_->ready()) ImGui::TextDisabled("(no audio device: silent)");
        }
    }
    if (e.rigidBody) {
        if (!componentHeader("Rigid Body", open)) { pushUndo(); e.rigidBody.reset(); }
        else if (open) {
            RigidBody& rb = *e.rigidBody;
            ImGui::DragFloat("Mass", &rb.mass, 0.05f, 0.01f, 10000.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::Checkbox("Gravity", &rb.useGravity);
            track();
            ImGui::SameLine();
            ImGui::Checkbox("Lock rotation", &rb.lockRotation);
            track();
            tooltip("Moves but never turns: tall things that shouldn't topple, upright characters");
            ImGui::SliderFloat("Bounciness", &rb.bounciness, 0.f, 1.f, "%.2f");
            track();
            ImGui::SliderFloat("Friction", &rb.friction, 0.f, 2.f, "%.2f");
            track();
            ImGui::SliderFloat("Damping", &rb.damping, 0.f, 5.f, "%.2f");
            track();
            ImGui::DragFloat3(playing() ? "Velocity" : "Start velocity", &rb.velocity.x, 0.05f, 0.f, 0.f, "%.2f");
            track();
            ImGui::DragFloat3(playing() ? "Spin" : "Start spin", &rb.angularVelocity.x, 0.05f, 0.f, 0.f, "%.2f");
            track();
            tooltip("Angular velocity, radians per second around each axis");
            if (!e.collider || e.collider->trigger)
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "Needs a solid collider to be simulated");
        }
    }
    if (e.light) {
        if (!componentHeader("Light", open)) { pushUndo(); e.light.reset(); }
        else if (open) {
            Light& l = *e.light;
            int type = (int)l.type;
            if (ImGui::Combo("Type", &type, "Directional\0Point\0Spot\0")) {
                pushUndo();
                l.type = (Light::Type)type;
            }
            ImGui::ColorEdit3("Light color", &l.color.x);
            track();
            ImGui::DragFloat("Intensity", &l.intensity, 0.01f, 0.f, 100.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            track();
            if (l.type != Light::Type::Directional) {
                ImGui::DragFloat("Range", &l.range, 0.1f, 0.1f, 1000.f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                track();
            }
            if (l.type == Light::Type::Spot) {
                ImGui::SliderFloat("Cone angle", &l.spotAngle, 1.f, 89.f, "%.0f deg");
                track();
                ImGui::SliderFloat("Edge softness", &l.spotSoftness, 0.f, 1.f, "%.2f");
                track();
            }
            if (l.type != Light::Type::Point) {
                ImGui::Checkbox("Cast shadows", &l.castShadows);
                track();
            }
            if (l.type == Light::Type::Directional)
                ImGui::TextDisabled("Shines along the entity's -Z (rotate it).\nThe first directional light is the sun.");
            else if (l.type == Light::Type::Spot)
                ImGui::TextDisabled("Shines along the entity's -Z. Up to 4 spot\nlights cast shadows (the nearest).");
        }
    }
    if (e.collider) {
        if (!componentHeader("Collider", open)) { pushUndo(); e.collider.reset(); }
        else if (open) {
            Collider& c = *e.collider;
            int shape = (int)c.shape;
            if (ImGui::Combo("Shape", &shape, "Box\0Sphere\0Capsule\0Mesh\0")) {
                pushUndo();
                c.shape = (Collider::Shape)shape;
            }
            ImGui::DragFloat3("Center", &c.center.x, 0.01f, 0.f, 0.f, "%.2f");
            track();
            if (c.shape == Collider::Shape::Box) {
                ImGui::DragFloat3("Size", &c.size.x, 0.01f, 0.f, 10000.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                track();
            }
            if (c.shape == Collider::Shape::Sphere || c.shape == Collider::Shape::Capsule) {
                ImGui::DragFloat("Radius", &c.radius, 0.01f, 0.f, 10000.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                track();
            }
            if (c.shape == Collider::Shape::Capsule) {
                ImGui::DragFloat("Height", &c.height, 0.01f, 0.f, 10000.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                track();
            }
            if (c.shape == Collider::Shape::Mesh && !e.meshRenderer)
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "Needs a Mesh Renderer to collide with");
            ImGui::Checkbox("Trigger", &c.trigger);
            track();
            tooltip("Triggers don't block anything: they tell behaviours when something enters or leaves them");
            if (e.meshRenderer) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Fit to mesh")) {
                    pushUndo();
                    bool trigger = c.trigger;
                    c = fitCollider(meshes_, e.meshRenderer->mesh);
                    c.trigger = trigger;
                }
            }
        }
    }
    int removeBehaviour = -1;
    FieldInspector inspector([this] { track(); });
    inspector.changed = [this] { editBegan_ = true; };
    inspector.pickReference = [this](const char* label, std::string& v, FieldRef kind) { pickReference(label, v, kind); };
    for (size_t i = 0; i < e.behaviours.size(); ++i) {
        Behaviour& b = *e.behaviours[i];
        ImGui::PushID((int)i);
        auto* missing = dynamic_cast<MissingBehaviour*>(&b);
        std::string label = std::string(b.typeName()) + (missing ? "  (code not loaded)" : "  (behaviour)");
        if (!componentHeader(label.c_str(), open)) removeBehaviour = (int)i;
        else if (open && missing) {
            ImGui::TextWrapped("This behaviour's code isn't loaded: the project's game module is missing, failed to "
                               "build, or no longer has it. Its settings are kept as they are:");
            ImGui::TextDisabled("%s", missing->fieldsJson().c_str());
        } else if (open) b.fields(inspector);
        ImGui::PopID();
    }
    if (removeBehaviour >= 0) {
        pushUndo();
        e.behaviours.erase(e.behaviours.begin() + removeBehaviour);
    }

    ImGui::Spacing();
    if (ImGui::Button("Add component", ImVec2(-FLT_MIN, 0))) ImGui::OpenPopup("##addcomp");
    if (ImGui::BeginPopup("##addcomp")) {
        if (ImGui::MenuItem("Mesh Renderer", nullptr, false, !e.meshRenderer)) { pushUndo(); e.meshRenderer = MeshRenderer{}; }
        if (ImGui::MenuItem("Camera", nullptr, false, !e.camera)) { pushUndo(); e.camera = Camera{}; }
        if (ImGui::MenuItem("Light", nullptr, false, !e.light)) { pushUndo(); e.light = Light{}; }
        if (ImGui::MenuItem("Audio Source", nullptr, false, !e.audioSource)) { pushUndo(); e.audioSource = AudioSource{}; }
        if (ImGui::MenuItem("Animator", nullptr, false, !e.animator)) { pushUndo(); e.animator = Animator{}; }
        if (ImGui::MenuItem("Particle Emitter", nullptr, false, !e.particleEmitter)) { pushUndo(); e.particleEmitter = ParticleEmitter{}; }
        if (ImGui::MenuItem("Rigid Body", nullptr, false, !e.rigidBody)) {
            pushUndo();
            e.rigidBody = RigidBody{};
            if (!e.collider) e.collider = e.meshRenderer ? fitCollider(meshes_, e.meshRenderer->mesh) : Collider{};
        }
        if (ImGui::MenuItem("Collider", nullptr, false, !e.collider)) {
            pushUndo();
            e.collider = e.meshRenderer ? fitCollider(meshes_, e.meshRenderer->mesh) : Collider{};
        }
        ImGui::SeparatorText("Behaviours");
        for (const BehaviourType& t : behaviourTypes()) {
            if (ImGui::MenuItem(t.name.c_str())) {
                pushUndo();
                e.behaviours.emplace_back(t.create());
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", t.description.c_str());
        }
        ImGui::EndPopup();
    }
}

// A behaviour field naming something: a text box (anything goes, e.g. an entity spawned later) with a list to
// pick from, and drops from the outliner (entities) or the asset browser (scenes, sounds, prefab files)
void Editor::pickReference(const char* label, std::string& value, FieldRef kind) {
    ImGui::PushID(label);
    float button = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - button - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::InputText("##text", &value);
    track();
    auto set = [&](const std::string& v) {
        if (v == value) return;
        value = v;
        editBegan_ = true;   // compared with the frame's start: one undo step
    };
    // Drops
    if (ImGui::BeginDragDropTarget()) {
        if (kind == FieldRef::Entity)
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload))
                if (const Entity* dropped = scene_.find(*(const EntityId*)p->Data)) set(dropped->name);
        ImGui::EndDragDropTarget();
    }
    std::filesystem::path dropped;
    if (kind == FieldRef::Scene && acceptAssetDrop(AssetKind::Scene, dropped)) set(project::toStored(dropped.u8string()));
    if (kind == FieldRef::Sound && acceptAssetDrop(AssetKind::Sound, dropped)) set(project::toStored(dropped.u8string()));

    // The list
    ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::BeginCombo("##pick", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft |
                                                 ImGuiComboFlags_HeightLarge)) {
        std::vector<std::string> options;
        switch (kind) {
        case FieldRef::Entity:
            for (const Entity& e : scene_.entities)
                if (std::find(options.begin(), options.end(), e.name) == options.end()) options.push_back(e.name);
            for (const Prefab& p : scene_.prefabs)   // what prefabs spawn has these names too
                for (const Entity& e : p.entities)
                    if (std::find(options.begin(), options.end(), e.name) == options.end()) options.push_back(e.name);
            break;
        case FieldRef::Prefab:
            for (const Prefab& p : scene_.prefabs) options.push_back(p.name);
            break;
        case FieldRef::Scene:
        case FieldRef::Sound: {
            if (kind == FieldRef::Sound) options = AudioEngine::builtinClips();
            std::error_code ec;
            AssetKind want = kind == FieldRef::Scene ? AssetKind::Scene : AssetKind::Sound;
            int seen = 0;
            for (auto it = std::filesystem::recursive_directory_iterator(
                     project::root(), std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator() && seen < 5000; it.increment(ec), ++seen) {
                std::string name = it->path().filename().u8string();
                if (it->is_directory(ec) && (name.empty() || name[0] == '.' || name == "CMakeFiles" || name == "build")) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (assetKind(it->path()) == want) options.push_back(project::toStored(it->path().u8string()));
            }
            break;
        }
        case FieldRef::AnimationClip:
            for (const AnimationClip& c : meshes_.animations) options.push_back(c.name);
            break;
        case FieldRef::InputAction:
            for (const InputAction& a : scene_.inputActions) options.push_back(a.name);
            break;
        }
        if (options.empty()) ImGui::TextDisabled("Nothing to pick in this scene / project");
        for (const std::string& o : options)
            if (ImGui::Selectable(o.c_str(), o == value)) set(o);
        ImGui::EndCombo();
    }
    ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    // A name that matches nothing is worth a warning (spawned entities can still match later)
    bool known = value.empty();
    switch (kind) {
    case FieldRef::Entity:
        known = known || scene_.findByName(value);
        for (const Prefab& p : scene_.prefabs)
            for (const Entity& e : p.entities) known = known || e.name == value;
        break;
    case FieldRef::Prefab:
        for (const Prefab& p : scene_.prefabs) known = known || p.name == value;
        break;
    case FieldRef::AnimationClip: known = known || meshes_.findAnimation(value); break;
    case FieldRef::InputAction:
        for (const InputAction& a : scene_.inputActions) known = known || a.name == value;
        break;
    default: known = true; break;
    }
    if (!known) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "(?)");
        tooltip("Nothing in this scene has that name");
    }
    ImGui::PopID();
}
