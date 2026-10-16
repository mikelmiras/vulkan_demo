// Editor inspector: the selection's components and behaviours, the scene's look, renderer info.
#include "editor/editor.h"
#include "assets/asset_db.h"
#include "render/environment.h"

#include <cctype>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <functional>

#include "misc/cpp/imgui_stdlib.h"
#include "platform/platform.h"
#include "audio/audio.h"
#include "game/ragdoll.h"
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
    ImGui::SetNextWindowSize(ImVec2(px(340.f), px(500.f)), ImGuiCond_FirstUseEver);
    if (!editor_ui::beginPanel("Inspector", &showInspector_)) {
        ImGui::End();
        return;
    }
    ImGui::PushItemWidth(-px(110.f));
    if (!selectedAsset_.empty()) {   // an asset picked in the browser
        drawAssetInspector();
        if (!selectedAsset_.empty()) {
            ImGui::PopItemWidth();
            ImGui::End();
            return;
        }
    }
    if (playing()) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "Playing: changes are lost on Stop");

    if (Entity* sel = selection()) {
        Entity& e = *sel;
        if (!extraSelected_.empty()) {
            ImGui::TextColored(ImVec4(1.f, 0.75f, 0.35f, 1.f), "%zu objects selected", selectionIds().size());
            ImGui::TextDisabled("The gizmo moves them all; below: %s", e.name.c_str());
        }
        ImGui::InputText("Name", &e.name);
        track();
        ImGui::Checkbox("Visible", &e.visible);
        track();
        ImGui::SameLine();
        ImGui::Checkbox("Locked", &e.locked);
        track();
        tooltip("Locked objects ignore viewport clicks (select them in the outliner)");
        drawTagsAndLayer(e);
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
        ImGui::SliderFloat("Ambient occlusion", &r.aoStrength, 0.f, 1.5f, "%.2f");
        track();
        tooltip("How much corners, creases and the ground under things darken (screen-space; 0: off)");
        ImGui::SliderFloat("Occlusion radius", &r.aoRadius, 0.05f, 4.f, "%.2f m");
        track();
        ImGui::Checkbox("Global illumination", &r.gi);
        track();
        tooltip("Light bouncing off surfaces (a coloured wall lights what's near it), from a grid of probes around the "
                "camera redrawn a few each frame: it follows moving lights and things with a lag. Costs some frame time.");
        if (r.gi) {
            ImGui::SliderFloat("GI probe spacing", &r.giSpacing, 0.5f, 16.f, "%.1f m");
            track();
            tooltip("Between the probes (8 x 4 x 8 of them): closer is more detailed, over less of the world");
            ImGui::SliderFloat("GI intensity", &r.giIntensity, 0.f, 3.f, "%.2f");
            track();
        }
        ImGui::Checkbox("Screen-space reflections", &r.reflections);
        track();
        tooltip("Smooth surfaces reflect what's on screen (else only reflection probes and the sky)");
        ImGui::SliderFloat("Shadow softness", &r.shadowSoftness, 0.f, 2.f, "%.2f");
        track();
        tooltip("How soft the sun's shadows are: 1 is about the real sun's (sharp where they touch, softer further from "
                "what casts them); 0 as sharp as they go");
        ImGui::Checkbox("Volumetric fog", &r.volumetricFog);
        track();
        tooltip("The fog lit by what passes through it: shafts of sunlight between buildings, halos round street lamps, "
                "headlight beams. Uses the fog density above.");
        if (r.volumetricFog) {
            ImGui::SliderFloat("Fog light", &r.fogLight, 0.f, 8.f, "%.2f");
            track();
            tooltip("How strongly the fog shows the light in it");
            ImGui::SliderFloat("Fog forward scattering", &r.fogAnisotropy, -0.9f, 0.9f, "%.2f");
            track();
            tooltip("How much the fog sends light on forwards: higher makes glows round lights you look towards brighter");
            ImGui::SliderFloat("Fog height falloff", &r.fogHeightFalloff, 0.f, 1.f, "%.3f");
            track();
            tooltip("Thinner higher up (0: just as thick everywhere): low-lying mist");
        }
        ImGui::Combo("Tone mapper", &r.tonemapper, "Neutral (PBR Neutral)\0Filmic\0");
        track();
        tooltip("Neutral keeps colours as they are; filmic gives richer contrast, highlights that bleach towards white "
                "like film's");
        ImGui::SliderFloat("Contrast", &r.contrast, 0.5f, 2.f, "%.2f");
        track();
        ImGui::SliderFloat("Saturation", &r.saturation, 0.f, 2.f, "%.2f");
        track();
        ImGui::SliderFloat("Vignette", &r.vignette, 0.f, 1.f, "%.2f");
        track();
        tooltip("Darker corners, drawing the eye to the middle");
        ImGui::Checkbox("Auto-exposure", &r.autoExposure);
        track();
        tooltip("The exposure follows how bright the view is, easing like an eye adjusting (Exposure then adds to it)");
        if (r.autoExposure) {
            ImGui::DragFloatRange2("Exposure range", &r.exposureMin, &r.exposureMax, 0.05f, -12.f, 12.f, "%.1f stops");
            track();
            ImGui::SliderFloat("Adaptation speed", &r.adaptationSpeed, 0.1f, 10.f, "%.1f", ImGuiSliderFlags_Logarithmic);
            track();
        }
        ImGui::SliderFloat("Local tone mapping", &r.localToneMapping, 0.f, 1.f, "%.2f");
        track();
        tooltip("Dark and bright parts of the view pulled towards the middle: detail in the shadows and round the lights");
        ImGui::Checkbox("Physical sky", &r.physicalSky);
        track();
        tooltip("The sky, the sun's colour and the light from the sky worked out from the air (blue by day, red at "
                "sunset); far things fade into the air");
        if (r.physicalSky) {
            bool timed = r.timeOfDay >= 0.f;
            if (ImGui::Checkbox("##timed", &timed)) {
                pushUndo();
                r.timeOfDay = timed ? 12.f : -1.f;
            }
            tooltip("Place the sun by the time of day (else where the scene's sun points)");
            ImGui::SameLine();
            ImGui::BeginDisabled(!timed);
            float hours = timed ? r.timeOfDay : 12.f;
            if (ImGui::SliderFloat("Time of day", &hours, 0.f, 24.f, "%.2f h") && timed) r.timeOfDay = std::fmod(hours, 24.f);
            track();
            ImGui::SliderFloat("Latitude", &r.latitude, -90.f, 90.f, "%.0f deg");
            track();
            ImGui::EndDisabled();
            ImGui::SliderFloat("Haze", &r.haze, 0.f, 10.f, "%.2f", ImGuiSliderFlags_Logarithmic);
            track();
            tooltip("A whiter sky, a wider glow round the sun, distance fading faster");
            ImGui::SliderFloat("Aerial perspective", &r.aerialPerspective, 0.f, 100.f, "%.1f", ImGuiSliderFlags_Logarithmic);
            track();
            tooltip("How thick the air looks over distance (1: as real; small scenes want more)");
            if (r.skyImage.empty()) {
                ImGui::SliderFloat("Sky brightness##phys", &r.skyIntensity, 0.f, 4.f, "%.2f");
                track();
                ImGui::SliderFloat("Sky turn##phys", &r.skyRotation, -180.f, 180.f, "%.0f deg");
                track();
                tooltip("Turns the compass: where the sun rises (with a time of day)");
            }
        }
        // The sky image: a panorama that's the sky, the ambient light and what shiny things reflect
        ImGui::InputText("Sky image", &r.skyImage);
        track();
        if (std::filesystem::path dropped; acceptAssetDrop(AssetKind::Texture, dropped)) {
            pushUndo();
            r.skyImage = project::toStored(dropped.u8string());
        }
        tooltip("A panorama (equirectangular: twice as wide as high), best an .hdr: drop one from the Assets panel");
        if (ImGui::SmallButton("Browse##sky")) {
            std::filesystem::path path = platform::openFileDialog("Sky image", "hdr,png,jpg,jpeg");
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
            if (!path.empty()) {
                pushUndo();
                r.skyImage = project::toStored(path.u8string());
            }
        }
        if (!r.skyImage.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove##sky")) {
                pushUndo();
                r.skyImage.clear();
            }
            std::string error;
            if (!environment::load(project::resolve(r.skyImage), error))
                ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "%s", error.c_str());
            ImGui::SliderFloat("Sky brightness", &r.skyIntensity, 0.f, 4.f, "%.2f");
            track();
            ImGui::SliderFloat("Sky turn", &r.skyRotation, -180.f, 180.f, "%.0f deg");
            track();
        }

        drawLayerSettings();

        ImGui::SeparatorText("Sound");
        {
            AudioSettings& au = scene_.audio;
            ImGui::InputTextWithHint("Music", "(none: what plays carries on)", &au.music);
            track();
            if (std::filesystem::path dropped; acceptAssetDrop(AssetKind::Sound, dropped)) {
                pushUndo();
                au.music = project::toStored(dropped.u8string());
            }
            tooltip("Played when the scene starts or is loaded, cross-fading from the music before (the same track carries "
                    "on): drop a sound file from the Assets panel");
            if (ImGui::SmallButton("Browse##music")) {
                std::filesystem::path path = platform::openFileDialog("Music", "wav,mp3,flac,ogg");
                ImGui::GetIO().ClearInputKeys();
                ImGui::GetIO().ClearInputMouse();
                if (!path.empty()) { pushUndo(); au.music = project::toStored(path.u8string()); }
            }
            if (!au.music.empty()) {
                ImGui::SliderFloat("Music volume", &au.musicVolume, 0.f, 2.f, "%.2f");
                track();
                ImGui::DragFloat("Cross-fade", &au.musicFade, 0.05f, 0.f, 10.f, "%.1f s", ImGuiSliderFlags_AlwaysClamp);
                track();
            }
            ImGui::Checkbox("Reverb from the world", &au.reverbFromWorld);
            track();
            tooltip("Rays from the listener measure how closed in it is and how big the room: corridors and halls ring, "
                    "open ground stays dry. Reverb zones take over inside them.");
            if (au.reverbFromWorld) {
                ImGui::SliderFloat("Reverb amount", &au.reverbAmount, 0.f, 2.f, "%.2f");
                track();
            }
        }

        ImGui::SeparatorText("Multiplayer");
        {
            NetworkSettings& n = scene_.network;
            std::string prefab = n.playerPrefab;
            if (ImGui::BeginCombo("Player prefab", prefab.empty() ? "(none: joiners watch)" : prefab.c_str())) {
                if (ImGui::Selectable("(none: joiners watch)", prefab.empty())) { pushUndo(); n.playerPrefab.clear(); }
                for (const Prefab& p : scene_.prefabs)
                    if (ImGui::Selectable(p.name.c_str(), p.name == prefab)) { pushUndo(); n.playerPrefab = p.name; }
                ImGui::EndCombo();
            }
            tooltip("Each player who joins a hosted game gets one of these (and the host too), driven by their input");
            ImGui::DragFloat3("Spawn point", &n.spawnPoint.x, 0.05f, 0.f, 0.f, "%.2f");
            ImGui::DragFloat("Spawn spread", &n.spawnSpread, 0.05f, 0.f, 100.f, "%.2f m");
            ImGui::DragFloat("Relevance", &n.relevance, 0.5f, 0.f, 10000.f, n.relevance > 0.f ? "%.0f m" : "everything",
                             ImGuiSliderFlags_AlwaysClamp);
            tooltip("Players only get the replicated entities this near them (less to send in big worlds; 0: all)");
        }

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
                "The game bakes this itself when it first needs a path (or from the start, with NavAgents).");
        ImGui::SameLine();
        if (ImGui::Button("Bake and save")) {
            NavGrid grid;
            std::string file = project::resolve(scenePath_) + ".nav";
            if (!grid.bake(scene_, meshes_, scene_.navigation)) setStatus("Nothing to walk on: no static colliders");
            else if (!grid.save(std::filesystem::u8path(file))) setStatus("Can't write " + file);
            else setStatus("Saved " + file + " (" + std::to_string(grid.cellCount()) + " cells): the game loads it "
                           "while the level stays as it is");
        }
        tooltip("Writes the bake next to the scene file (<scene>.vkw.nav). The game uses it instead of baking for as "
                "long as the level (static colliders, links, these settings) is unchanged; bake again after edits.");
        if (showNavigation_) {
            if (ImGui::Button("Hide")) showNavigation_ = false;
            ImGui::SameLine();
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
    if (stats.textureBudget)   // (texture streaming: what's on the GPU, against its budget)
        ImGui::Text("Textures: %.0f of %.0f MB", (double)stats.textureBytes / (1 << 20), (double)stats.textureBudget / (1 << 20));
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
            {   // presets: a starting point (the colour and textures stay)
                struct Preset {
                    const char* name;
                    float metallic, roughness, clearcoat, clearcoatRoughness, sheen, anisotropy;
                };
                static const Preset presets[] = {
                    {"Plastic", 0.f, 0.45f, 0.f, 0.05f, 0.f, 0.f},      {"Rubber", 0.f, 0.9f, 0.f, 0.05f, 0.f, 0.f},
                    {"Polished metal", 1.f, 0.15f, 0.f, 0.05f, 0.f, 0.f}, {"Rough metal", 1.f, 0.65f, 0.f, 0.05f, 0.f, 0.f},
                    {"Brushed metal", 1.f, 0.35f, 0.f, 0.05f, 0.f, 0.7f}, {"Car paint", 0.5f, 0.4f, 1.f, 0.04f, 0.f, 0.f},
                    {"Lacquered wood", 0.f, 0.6f, 0.8f, 0.08f, 0.f, 0.f}, {"Cloth", 0.f, 0.85f, 0.f, 0.05f, 1.f, 0.f},
                    {"Velvet", 0.f, 0.95f, 0.f, 0.05f, 1.f, 0.f},          {"Stone", 0.f, 0.8f, 0.f, 0.05f, 0.f, 0.f},
                };
                if (ImGui::BeginCombo("Preset", "(choose to apply)")) {
                    for (const Preset& p : presets)
                        if (ImGui::Selectable(p.name)) {
                            pushUndo();
                            mr.metallic = p.metallic;
                            mr.roughness = p.roughness;
                            mr.clearcoat = p.clearcoat;
                            mr.clearcoatRoughness = p.clearcoatRoughness;
                            mr.sheen = p.sheen;
                            mr.anisotropy = p.anisotropy;
                        }
                    ImGui::EndCombo();
                }
                tooltip("Sets metallic, roughness and the layers below to a kind of material (the colour and textures stay)");
            }
            ImGui::SliderFloat("Metallic", &mr.metallic, 0.f, 1.f, "%.2f");
            track();
            ImGui::SliderFloat("Roughness", &mr.roughness, 0.f, 1.f, "%.2f");
            track();
            ImGui::SliderFloat("Clear coat", &mr.clearcoat, 0.f, 1.f, "%.2f");
            track();
            tooltip("A clear varnish over the material with a sharp reflection of its own: car paint, lacquer, a polished floor");
            if (mr.clearcoat > 0.f) {
                ImGui::SliderFloat("Coat roughness", &mr.clearcoatRoughness, 0.f, 1.f, "%.2f");
                track();
            }
            ImGui::SliderFloat("Sheen", &mr.sheen, 0.f, 1.f, "%.2f");
            track();
            tooltip("Cloth's soft glow at grazing angles (velvet, felt, fabric), tinted by the colour");
            ImGui::SliderFloat("Anisotropy", &mr.anisotropy, -1.f, 1.f, "%.2f");
            track();
            tooltip("Highlights stretched across the surface, like brushed metal: along the texture's u (above 0) or v "
                    "(below)");
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
            const char* players[5] = {"none", "1", "2", "3", "4"};
            ImGui::Combo("Split-screen player", &c.player, players, 5);
            track();
            tooltip("With two or more player cameras, vkgame splits the screen between them (2: side by side, 3-4: in "
                    "quarters)");
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
    if (e.replicated) {
        if (!componentHeader("Replicated", open)) { pushUndo(); e.replicated.reset(); }
        else if (open) {
            ImGui::Checkbox("Behaviour fields too", &e.replicated->fields);
            track();
            ImGui::TextDisabled("In a multiplayer game, players see it where the host has it");
        }
    }
    if (e.ui) {
        if (!componentHeader("UI Element", open)) { pushUndo(); e.ui.reset(); }
        else if (open) drawUIInspector(e);
    }
    if (e.reflectionProbe) {
        if (!componentHeader("Reflection Probe", open)) { pushUndo(); e.reflectionProbe.reset(); }
        else if (open) {
            ReflectionProbe& p = *e.reflectionProbe;
            ImGui::DragFloat3("Box size", &p.size.x, 0.05f, 0.1f, 1000.f, "%.2f m");
            track();
            tooltip("Shiny things inside this box (centred on the probe) reflect what it sees");
            int res = p.resolution <= 64 ? 0 : p.resolution <= 128 ? 1 : 2;
            const char* resNames[] = {"64", "128", "256"};
            if (ImGui::Combo("Resolution", &res, resNames, 3)) {
                pushUndo();
                p.resolution = 64 << res;
            }
            ImGui::Checkbox("Box projection", &p.boxProjection);
            track();
            tooltip("Reflections lined up with the walls of a box-shaped room (off: as if seen from far away)");
            if (ImGui::Button("Bake")) ++probeBakes_;
            tooltip("Render what it sees again (it's baked when first seen, moved or resized; not when the scene "
                    "around it changes)");
        }
    }
    if (e.water) {
        if (!componentHeader("Water", open)) { pushUndo(); e.water.reset(); }
        else if (open) {
            Water& w = *e.water;
            ImGui::DragFloat("Width", &w.width, 0.1f, 0.1f, 100000.f, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::DragFloat("Length", &w.length, 0.1f, 0.1f, 100000.f, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::DragFloat("Depth", &w.depth, 0.05f, 0.f, 10000.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            track();
            tooltip("Its surface is the entity's height; things float in it down to this deep (along its x and z: width, length)");
            ImGui::DragFloat("Wave height", &w.waveHeight, 0.01f, 0.f, 50.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::DragFloat("Wave length", &w.waveLength, 0.05f, 0.1f, 1000.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            track();
            tooltip("From one crest to the next: longer waves run faster (as in deep water)");
            ImGui::SliderFloat("Wave direction", &w.waveDirection, -180.f, 180.f, "%.0f deg");
            track();
            ImGui::SliderFloat("Choppiness", &w.choppiness, 0.f, 1.f, "%.2f");
            track();
            tooltip("0: rounded swells; 1: sharp crests, with foam");
            ImGui::DragFloat3("Current", &w.current.x, 0.02f, -50.f, 50.f, "%.2f m/s");
            track();
            tooltip("The water's flow: floating things are carried along, ripples drift with it");
            ImGui::ColorEdit3("Colour", &w.color.x);
            track();
            ImGui::SliderFloat("Clarity", &w.clarity, 0.f, 1.f, "%.2f");
            track();
            tooltip("How much of what's under shows through (looking straight down; at a glancing angle it reflects)");
        }
    }
    if (e.terrain) {
        if (!componentHeader("Terrain", open)) { pushUndo(); e.terrain.reset(); }
        else if (open) drawTerrainInspector(e);
    }
    if (e.softBody) {
        if (!componentHeader("Soft Body", open)) { pushUndo(); e.softBody.reset(); }
        else if (open) {
            SoftBody& sb = *e.softBody;
            int shape = (int)sb.shape;
            if (ImGui::Combo("Shape", &shape, "Cloth\0Mesh (squashy)\0")) {
                pushUndo();
                sb.shape = (SoftBody::Shape)shape;
            }
            if (sb.shape == SoftBody::Shape::Cloth) {
                ImGui::DragFloat("Width", &sb.width, 0.05f, 0.05f, 100.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
                track();
                ImGui::DragFloat("Height", &sb.height, 0.05f, 0.05f, 100.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
                track();
                ImGui::SliderInt("Resolution", &sb.resolution, 2, 64);
                track();
                int pin = sb.pinned == "top" ? 0 : sb.pinned == "corners" ? 1 : 2;
                if (ImGui::Combo("Pinned", &pin, "Top edge\0Top corners\0Nothing\0")) {
                    pushUndo();
                    sb.pinned = pin == 0 ? "top" : pin == 1 ? "corners" : "none";
                }
                ImGui::SliderFloat("Bendiness", &sb.bendiness, 0.f, 1.f, "%.2f");
                track();
                ImGui::DragFloat("Tears at", &sb.tearStretch, 0.01f, 0.f, 10.f, sb.tearStretch > 0.f ? "%.2fx stretch" : "never",
                                 ImGuiSliderFlags_AlwaysClamp);
                track();
                tooltip("A triangle tears once one of its edges is stretched this many times its length (1.5: half again)");
            } else {
                ImGui::DragFloat("Pressure", &sb.pressure, 10.f, 0.f, 1e6f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
                track();
                tooltip("Inflates a closed mesh (a ball, a cushion): a few thousand holds it firm");
                ImGui::DragFloat("Tears at", &sb.tearStretch, 0.01f, 0.f, 10.f, sb.tearStretch > 0.f ? "%.2fx stretch" : "never",
                                 ImGuiSliderFlags_AlwaysClamp);
                track();
                tooltip("A triangle tears once one of its edges is stretched this many times its length; torn open, "
                        "it holds no pressure (a burst balloon)");
                if (!e.meshRenderer) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "Needs a mesh renderer (its mesh)");
            }
            ImGui::DragFloat("Mass", &sb.mass, 0.05f, 0.01f, 10000.f, "%.2f kg", ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::SliderFloat("Stiffness", &sb.stiffness, 0.f, 1.f, "%.2f");
            track();
            ImGui::SliderFloat("Damping", &sb.damping, 0.f, 2.f, "%.2f");
            track();
            ImGui::SliderFloat("Friction", &sb.friction, 0.f, 1.f, "%.2f");
            track();
            ImGui::DragFloat("Buoyancy", &sb.buoyancy, 0.01f, 0.f, 10.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            track();
            tooltip("In water (trigger boxes tagged Water): 1 hovers, more floats, 0 sinks as if there were none");
            ImGui::TextDisabled("Simulated while playing; drawn with the Mesh Renderer's look.");
        }
    }
    if (e.fracture) {
        if (!componentHeader("Fracture", open)) { pushUndo(); e.fracture.reset(); }
        else if (open) {
            Fracture& f = *e.fracture;
            ImGui::SliderInt("Pieces", &f.pieces, 2, 64);
            track();
            int seed = (int)f.seed;
            if (ImGui::DragInt("Seed", &seed, 1.f, 0, 1 << 30)) f.seed = (uint32_t)seed;
            track();
            tooltip("Another seed, other cuts (the same pieces every time for a seed)");
            ImGui::DragFloat("Break speed", &f.breakSpeed, 0.1f, 0.f, 200.f, f.breakSpeed > 0.f ? "%.1f m/s" : "never by impacts",
                             ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::DragFloat("Burst", &f.burst, 0.05f, 0.f, 100.f, "%.2f m/s", ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::DragFloat("Piece life", &f.pieceLife, 0.1f, 0.f, 600.f, f.pieceLife > 0.f ? "%.1f s" : "forever",
                             ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::Checkbox("Hold together", &f.holdTogether);
            track();
            tooltip("Cracks instead of breaking: the pieces stay joined until knocked apart");
            if (f.holdTogether) {
                ImGui::DragFloat("Hold strength", &f.holdStrength, 10.f, 1.f, 1e7f, "%.0f N", ImGuiSliderFlags_AlwaysClamp);
                track();
            }
            ImGui::TextDisabled("%d pieces made. Breaks on a hard impact, a Destructible\nrunning out of hits, or ctx.shatter().",
                                (int)f.pieceMeshes.size());
            if (!e.meshRenderer) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "Needs a mesh renderer (its mesh is cut)");
        }
    }
    if (e.brain) {
        if (!componentHeader("AI Brain", open)) { pushUndo(); e.brain.reset(); }
        else if (open) drawBrainInspector(e);
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
            if (ImGui::Button("Make ragdoll")) {
                pushUndo();
                int parts = makeRagdoll(scene_, scene_.modelRoot(e.id));
                setStatus("Ragdoll: " + std::to_string(parts) + " parts (kinematic until ctx.setRagdoll(root, true))");
            }
            tooltip("Gives each bone a collider, a rigid body and a joint to its parent bone. The parts follow the "
                    "animation (kinematic) until game code makes the ragdoll go limp.");
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
                a.stream = assetdb::importSettings(dropped)["stream"].asBool(a.stream);   // (its import setting)
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
                ImGui::SliderFloat("Doppler", &a.doppler, 0.f, 3.f, "%.2f");
                track();
                tooltip("How much its pitch shifts as it moves towards or away from the listener (0: none)");
                ImGui::SliderFloat("Occlusion", &a.occlusion, 0.f, 1.f, "%.2f");
                track();
                tooltip("How muffled it gets with a wall (any solid collider) between it and the listener (0: heard through)");
            }
            int bus = (int)audioBusFromName(a.bus);
            const char* buses[] = {"effects", "music", "ui", "ambience"};
            if (ImGui::Combo("Bus", &bus, buses, IM_ARRAYSIZE(buses))) { pushUndo(); a.bus = buses[bus]; }
            tooltip("The mixer bus it plays on: each has its own volume (ctx.setBusVolume) and can be ducked; effects "
                    "and ambience get the reverb of the zone the listener is in");
            ImGui::Checkbox("Stream", &a.stream);
            track();
            tooltip("Decode the file as it plays instead of all up front: for long music and ambience");
            if (!audio_ || !audio_->ready()) ImGui::TextDisabled("(no audio device: silent)");
        }
    }
    if (e.reverbZone) {
        if (!componentHeader("Reverb Zone", open)) { pushUndo(); e.reverbZone.reset(); }
        else if (open) {
            ReverbZone& z = *e.reverbZone;
            ImGui::DragFloat("Radius", &z.radius, 0.1f, 0.f, 10000.f, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::DragFloat("Blend", &z.blend, 0.05f, 0.f, 1000.f, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
            track();
            tooltip("How far in from the edge the reverb takes to reach its full amount");
            ImGui::SliderFloat("Wet", &z.wet, 0.f, 1.f, "%.2f");
            track();
            ImGui::SliderFloat("Room size", &z.roomSize, 0.f, 1.f, "%.2f");
            track();
            ImGui::SliderFloat("Damping", &z.damping, 0.f, 1.f, "%.2f");
            track();
            tooltip("Muffles the echoes: soft rooms high, stone halls low");
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
            ImGui::Checkbox("Kinematic", &rb.kinematic);
            track();
            tooltip("Moved by animation or game code instead of simulated: it pushes others but isn't pushed (a "
                    "ragdoll's parts until it goes limp, a lift)");
            ImGui::SliderFloat("Bounciness", &rb.bounciness, 0.f, 1.f, "%.2f");
            track();
            ImGui::SliderFloat("Buoyancy", &rb.buoyancy, 0.f, 3.f, "%.2f");
            tooltip("In Water volumes (trigger boxes tagged Water): 1 neither floats nor sinks, more floats, less sinks");
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
    if (e.navLink) {
        if (!componentHeader("Navigation Link", open)) { pushUndo(); e.navLink.reset(); }
        else if (open) {
            ImGui::DragFloat3("End", &e.navLink->end.x, 0.05f, 0.f, 0.f, "%.2f");
            track();
            tooltip("Where it leads, in this entity's own space (it starts where the entity is)");
            ImGui::Checkbox("Both ways", &e.navLink->bothWays);
            track();
            ImGui::DragFloat("Cost", &e.navLink->cost, 0.05f, 0.01f, 100.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            track();
            tooltip("Times its length: above 1, paths take it only when walking round is much longer");
        }
    }
    if (e.joint) {
        if (!componentHeader("Joint", open)) { pushUndo(); e.joint.reset(); }
        else if (open) {
            Joint& j = *e.joint;
            int type = (int)j.type;
            const char* types[] = {"Fixed", "Ball", "Hinge", "Slider", "Distance", "Cone"};
            if (ImGui::Combo("Type", &type, types, IM_ARRAYSIZE(types))) { pushUndo(); j.type = (Joint::Type)type; }
            pickReference("Connected to", j.connectedTo, FieldRef::Entity);
            tooltip("The other rigid body (or static collider), by name; empty: the world");
            ImGui::DragFloat3("Anchor", &j.anchor.x, 0.01f, 0.f, 0.f, "%.2f");
            track();
            tooltip("Where the two are held together, in this entity's own space");
            if (j.type == Joint::Type::Hinge || j.type == Joint::Type::Slider || j.type == Joint::Type::Cone ||
                j.type == Joint::Type::Fixed) {
                ImGui::DragFloat3("Axis", &j.axis.x, 0.01f, -1.f, 1.f, "%.2f");
                track();
            }
            bool angles = j.type == Joint::Type::Hinge || j.type == Joint::Type::Cone;
            if (j.type != Joint::Type::Fixed && j.type != Joint::Type::Ball) {
                ImGui::DragFloatRange2("Limits", &j.minLimit, &j.maxLimit, angles ? 0.5f : 0.01f, 0.f, 0.f,
                                       angles ? "%.0f deg" : "%.2f m", angles ? "%.0f deg" : "%.2f m");
                track();
                tooltip(j.type == Joint::Type::Cone ? "Twist around the axis (equal: +-20 deg)"
                                                    : "Equal: free (distance: as they start)");
            }
            if (j.type == Joint::Type::Cone) {
                ImGui::SliderFloat("Cone angle", &j.coneAngle, 0.f, 179.f, "%.0f deg");
                track();
            }
            if (j.type == Joint::Type::Hinge || j.type == Joint::Type::Slider) {
                ImGui::DragFloat("Motor speed", &j.motorSpeed, 0.5f, 0.f, 0.f, j.type == Joint::Type::Hinge ? "%.0f deg/s" : "%.2f m/s");
                track();
                ImGui::DragFloat("Motor strength", &j.motorStrength, 1.f, 0.f, 1e7f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
                track();
                tooltip("Torque (N m) or force (N) the motor drives it with; 0: no motor");
            }
            ImGui::DragFloat("Break force", &j.breakForce, 1.f, 0.f, 1e9f, "%.0f N", ImGuiSliderFlags_AlwaysClamp);
            track();
            tooltip("Comes apart when pulled harder than this (0: never)");
            ImGui::Checkbox("Collide connected", &j.collideConnected);
            track();
            if (j.broken) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "broken");
            }
            if (!e.rigidBody) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "Needs a rigid body (and a collider)");
        }
    }
    if (e.vehicle) {
        if (!componentHeader("Vehicle", open)) { pushUndo(); e.vehicle.reset(); }
        else if (open) {
            Vehicle& v = *e.vehicle;
            ImGui::DragFloat("Engine torque", &v.engineTorque, 5.f, 0.f, 1e5f, "%.0f N m", ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::DragFloat("Max RPM", &v.maxRpm, 10.f, 1000.f, 20000.f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::SliderFloat("Max steering", &v.maxSteer, 0.f, 60.f, "%.0f deg");
            track();
            ImGui::DragFloat("Suspension", &v.suspensionLength, 0.01f, 0.01f, 2.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
            track();
            tooltip("How far each wheel hangs below where it's attached");
            ImGui::DragFloat("Stiffness", &v.suspensionFrequency, 0.05f, 0.1f, 20.f, "%.2f Hz", ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::SliderFloat("Damping", &v.suspensionDamping, 0.f, 2.f, "%.2f");
            track();
            int removeWheel = -1;
            for (size_t w = 0; w < v.wheels.size(); ++w) {
                Vehicle::Wheel& wheel = v.wheels[w];
                ImGui::PushID((int)w);
                bool node = ImGui::TreeNode("wheel", "Wheel %d", (int)w + 1);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) removeWheel = (int)w;
                if (node) {
                    ImGui::DragFloat3("Position", &wheel.position.x, 0.01f, 0.f, 0.f, "%.2f");
                    track();
                    tooltip("Where its suspension is attached, in the vehicle's own space");
                    ImGui::DragFloat("Radius", &wheel.radius, 0.01f, 0.01f, 5.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
                    track();
                    ImGui::DragFloat("Width", &wheel.width, 0.01f, 0.01f, 5.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
                    track();
                    ImGui::Checkbox("Steers", &wheel.steering);
                    track();
                    ImGui::SameLine();
                    ImGui::Checkbox("Driven", &wheel.driven);
                    track();
                    pickReference("Shown by", wheel.entity, FieldRef::Entity);
                    tooltip("An entity (usually a child) turned and moved with this wheel");
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (removeWheel >= 0) { pushUndo(); v.wheels.erase(v.wheels.begin() + removeWheel); }
            if (ImGui::Button("Add wheel")) { pushUndo(); v.wheels.emplace_back(); }
            if (!e.rigidBody) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "Needs a rigid body (and a collider)");
            else if (std::find_if(e.behaviours.begin(), e.behaviours.end(), [](const BehaviourPtr& b) {
                         return std::string(b->typeName()) == "VehicleDriver";
                     }) == e.behaviours.end())
                ImGui::TextDisabled("Add a VehicleDriver behaviour to drive it with the keys");
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
            if (l.type == Light::Type::Spot) {
                ImGui::InputText("Cookie", &l.cookie);
                track();
                if (std::filesystem::path dropped; acceptAssetDrop(AssetKind::Texture, dropped)) {
                    pushUndo();
                    l.cookie = project::toStored(dropped.u8string());
                }
                tooltip("An image the light shines through (a window frame, leaves): drop one from the Assets panel");
                if (!l.cookie.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove##cookie")) {
                        pushUndo();
                        l.cookie.clear();
                    }
                }
            }
            ImGui::Checkbox("Cast shadows", &l.castShadows);
            track();
            if (l.type == Light::Type::Directional)
                ImGui::TextDisabled("Shines along the entity's -Z (rotate it).\nThe first directional light is the sun.");
            else
                ImGui::TextDisabled("%s Shadows: the nearest lights in view get\nthe sharpest (up to 48 views; a point light uses 6).",
                                    l.type == Light::Type::Spot ? "Shines along the entity's -Z." : "Shines all around.");
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
        if (ImGui::MenuItem("Reflection Probe", nullptr, false, !e.reflectionProbe)) { pushUndo(); e.reflectionProbe = ReflectionProbe{}; }
        if (ImGui::MenuItem("Water", nullptr, false, !e.water)) { pushUndo(); e.water = Water{}; }
        if (ImGui::MenuItem("UI Element", nullptr, false, !e.ui)) { pushUndo(); e.ui = UIElement{}; }
        if (ImGui::MenuItem("Replicated", nullptr, false, !e.replicated)) { pushUndo(); e.replicated = Replicated{}; }
        if (ImGui::MenuItem("Terrain", nullptr, false, !e.terrain)) {
            pushUndo();
            e.terrain = Terrain{};
            terrain::init(*e.terrain, 128, 64.f);
        }
        if (ImGui::MenuItem("Rigid Body", nullptr, false, !e.rigidBody)) {
            pushUndo();
            e.rigidBody = RigidBody{};
            if (!e.collider) e.collider = e.meshRenderer ? fitCollider(meshes_, e.meshRenderer->mesh) : Collider{};
        }
        if (ImGui::MenuItem("Navigation Link", nullptr, false, !e.navLink)) { pushUndo(); e.navLink = NavLink{}; }
        if (ImGui::MenuItem("Reverb Zone", nullptr, false, !e.reverbZone)) { pushUndo(); e.reverbZone = ReverbZone{}; }
        if (ImGui::MenuItem("Joint", nullptr, false, !e.joint)) {
            pushUndo();
            e.joint = Joint{};
            if (!e.rigidBody) e.rigidBody = RigidBody{};
            if (!e.collider) e.collider = e.meshRenderer ? fitCollider(meshes_, e.meshRenderer->mesh) : Collider{};
        }
        if (ImGui::MenuItem("Soft Body", nullptr, false, !e.softBody)) {
            pushUndo();
            e.softBody = SoftBody{};
            if (e.meshRenderer && e.meshRenderer->mesh != (int)MeshKind::Plane) e.softBody->shape = SoftBody::Shape::Mesh;
            if (!e.meshRenderer) {
                e.meshRenderer = MeshRenderer{};
                e.meshRenderer->mesh = (int)MeshKind::Plane;
            }
        }
        if (ImGui::MenuItem("Fracture", nullptr, false, !e.fracture)) {
            pushUndo();
            e.fracture = Fracture{};
            if (!e.collider) e.collider = e.meshRenderer ? fitCollider(meshes_, e.meshRenderer->mesh) : Collider{};
        }
        if (ImGui::MenuItem("AI Brain", nullptr, false, !e.brain)) {
            pushUndo();
            e.brain = AIBrain{};
        }
        if (ImGui::MenuItem("Vehicle", nullptr, false, !e.vehicle)) {
            // A car: four wheels at the corners of its collider (the front two steer, the back two drive)
            pushUndo();
            if (!e.collider) e.collider = e.meshRenderer ? fitCollider(meshes_, e.meshRenderer->mesh) : Collider{};
            if (!e.rigidBody) {
                e.rigidBody = RigidBody{};
                e.rigidBody->mass = 1200.f;
            }
            Vehicle v;
            const Collider& c = *e.collider;
            Vec3 half = c.shape == Collider::Shape::Box ? c.size * 0.5f : Vec3{c.radius, c.radius, c.radius};
            for (int i = 0; i < 4; ++i) {
                Vehicle::Wheel w;
                bool front = i < 2;
                w.position = {c.center.x + (i % 2 ? half.x : -half.x), c.center.y - half.y * 0.5f,
                              c.center.z + (front ? -half.z : half.z) * 0.7f};
                w.steering = front;
                w.driven = !front;
                v.wheels.push_back(w);
            }
            e.vehicle = v;
        }
        if (ImGui::MenuItem("Collider", nullptr, false, !e.collider)) {
            pushUndo();
            e.collider = e.meshRenderer ? fitCollider(meshes_, e.meshRenderer->mesh) : Collider{};
        }
        ImGui::SeparatorText("Behaviours");
        bool scripts = false;
        for (int pass = 0; pass < 2; ++pass)   // C++ ones, then the project's Lua scripts
            for (const BehaviourType& t : behaviourTypes()) {
                if ((t.module == "lua") != (pass == 1)) continue;
                if (pass == 1 && !scripts) {
                    ImGui::SeparatorText("Scripts");
                    scripts = true;
                }
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

namespace {

std::string layerLabel(const LayerSettings& l, int i) {
    return l.names[i].empty() ? "Layer " + std::to_string(i) : l.names[i];
}

}  // namespace

// Tags as removable chips plus a box to add one; the layer as a list of the scene's named layers
void Editor::drawTagsAndLayer(Entity& e) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Tags");
    int remove = -1;
    for (size_t i = 0; i < e.tags.size(); ++i) {
        ImGui::SameLine();
        ImGui::PushID((int)i);
        if (ImGui::SmallButton((e.tags[i] + "  x").c_str())) remove = (int)i;
        tooltip("Remove this tag");
        ImGui::PopID();
    }
    if (remove >= 0) { pushUndo(); e.tags.erase(e.tags.begin() + remove); }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x, px(60.f)));
    if (ImGui::InputTextWithHint("##newtag", "add a tag", &newTag_, ImGuiInputTextFlags_EnterReturnsTrue) &&
        !newTag_.empty() && !e.hasTag(newTag_)) {
        pushUndo();
        e.tags.push_back(newTag_);
        newTag_.clear();
    }
    tooltip("Game code finds entities by tag: ctx.findWithTag(\"Enemy\"), ctx.sendToTag(...)");

    const LayerSettings& layers = scene_.layers;
    if (ImGui::BeginCombo("Layer", layerLabel(layers, e.layer).c_str())) {
        for (int i = 0; i < LayerSettings::kCount; ++i) {
            if (layers.names[i].empty() && i != e.layer) continue;
            if (ImGui::Selectable(layerLabel(layers, i).c_str(), i == e.layer)) { pushUndo(); e.layer = (uint8_t)i; }
        }
        ImGui::EndCombo();
    }
    tooltip("Which other layers its collider meets (the scene's layer matrix) and which queries see it. Name more "
            "layers in the scene settings (Inspector with nothing selected).");
}

// The scene's layer names, and which named layers meet, as a triangle of checkboxes
void Editor::drawLayerSettings() {
    ImGui::SeparatorText("Layers");
    LayerSettings& l = scene_.layers;
    int shown = 0;
    for (int i = 0; i < LayerSettings::kCount; ++i) {
        if (l.names[i].empty() && shown > 0 && (i == 0 || l.names[i - 1].empty())) continue;   // one blank to add more
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(-px(60.f));
        ImGui::InputTextWithHint("##name", ("Layer " + std::to_string(i)).c_str(), &l.names[i]);
        track();
        ImGui::SameLine();
        ImGui::TextDisabled("%d", i);
        ImGui::PopID();
        ++shown;
    }
    std::vector<int> named;
    for (int i = 0; i < LayerSettings::kCount; ++i)
        if (!l.names[i].empty()) named.push_back(i);
    if (named.size() < 2) return;
    if (ImGui::TreeNode("Which layers meet")) {
        // Rows: each layer; columns: it and the layers before it
        for (size_t r = 0; r < named.size(); ++r) {
            ImGui::PushID((int)r);
            for (size_t c = 0; c <= r; ++c) {
                if (c) ImGui::SameLine();
                bool on = l.meet(named[r], named[c]);
                ImGui::PushID((int)c);
                if (ImGui::Checkbox("##m", &on)) { pushUndo(); l.setMeet(named[r], named[c], on); }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s and %s", l.names[named[r]].c_str(), l.names[named[c]].c_str());
                ImGui::PopID();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(l.names[named[r]].c_str());
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}
