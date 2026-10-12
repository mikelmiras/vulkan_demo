// Editor: terrains (scene/terrain.h): adding one, its inspector, and the Terrain tool's brushes (sculpt and paint).
#include "editor/editor.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <random>

#include "editor/editor_ui.h"
#include "platform/platform.h"

using namespace editor_ui;

void Editor::addTerrain() {
    pushUndo();
    Entity& e = scene_.create(scene_.uniqueName("Terrain"));
    Terrain t;
    terrain::init(t, 128, 64.f);
    const Vec3 colors[Terrain::kLayers] = {{0.36f, 0.52f, 0.24f}, {0.5f, 0.48f, 0.46f}, {0.47f, 0.36f, 0.25f},
                                           {0.84f, 0.78f, 0.58f}};   // grass, rock, dirt, sand
    for (int k = 0; k < Terrain::kLayers; ++k) t.layers[k].color = colors[k];
    e.terrain = std::move(t);
    select(e.id);
    tool_ = Tool::Terrain;
    setStatus("Added a terrain: drag on it to raise the ground (Shift lowers); brushes in the Inspector");
}

void Editor::drawTerrainInspector(Entity& e) {
    Terrain& t = *e.terrain;
    if (t.resolution < 2) {
        if (ImGui::Button("Make it")) {
            pushUndo();
            terrain::init(t, 128, t.size);
        }
        return;
    }
    // Brushes
    bool active = tool_ == Tool::Terrain;
    if (ImGui::Button(active ? "Sculpting (T)" : "Sculpt and paint (T)")) tool_ = active ? Tool::Move : Tool::Terrain;
    tooltip("The Terrain tool: drag on the terrain with the brush; Shift: the opposite (lower / raise)");
    const char* modes[] = {"Raise", "Lower", "Smooth", "Flatten", "Paint"};
    int mode = (int)brush_.mode;
    for (int i = 0; i < 5; ++i) {
        if (i && i != 3) ImGui::SameLine();   // (two rows: they don't fit a narrow panel on one)
        if (ImGui::RadioButton(modes[i], mode == i)) {
            brush_.mode = (terrain::Brush)i;
            tool_ = Tool::Terrain;
        }
    }
    ImGui::SliderFloat("Brush radius", &brush_.radius, 0.5f, 50.f, "%.1f m", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Strength", &brush_.strength, 0.02f, 3.f, "%.2f", ImGuiSliderFlags_Logarithmic);
    if (brush_.mode == terrain::Brush::Flatten) {
        ImGui::DragFloat("Flatten to", &brush_.flattenHeight, 0.05f, 0.f, 0.f, "%.2f m");
        tooltip("Set by where a stroke starts (or here)");
    }
    if (brush_.mode == terrain::Brush::Paint) {
        const char* names[] = {"Layer 1", "Layer 2", "Layer 3", "Layer 4"};
        ImGui::Combo("Paint layer", &brush_.layer, names, Terrain::kLayers);
    }

    ImGui::SeparatorText("Size");
    int res = 0;
    for (int i = 0; i < 3; ++i)
        if (terrain::kResolutions[i] == t.resolution) res = i;
    const char* resNames[] = {"64 x 64", "128 x 128", "256 x 256"};
    if (ImGui::Combo("Detail", &res, resNames, 3) && terrain::kResolutions[res] != t.resolution) {
        pushUndo();
        terrain::resize(t, terrain::kResolutions[res]);
    }
    tooltip("Height samples along each side (resampled: the shape is kept)");
    ImGui::DragFloat("Width", &t.size, 0.25f, 1.f, 4096.f, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
    track();
    if (ImGui::IsItemEdited()) terrain::touch(t);
    ImGui::TextDisabled("Samples %.2f m apart", terrain::cellSize(t));
    static float hills = 6.f, features = 24.f;
    ImGui::DragFloat("Hill height", &hills, 0.1f, 0.f, 500.f, "%.1f m");
    ImGui::DragFloat("Hill size", &features, 0.1f, 1.f, 1000.f, "%.1f m");
    if (ImGui::Button("Add hills")) {
        pushUndo();
        terrain::addNoise(t, hills, features, std::random_device{}());
    }
    tooltip("Random rolling hills on top of what's there");
    ImGui::SameLine();
    if (ImGui::Button("Flatten all")) {
        pushUndo();
        std::fill(t.heights.begin(), t.heights.end(), 0.f);
        terrain::touch(t);
    }

    ImGui::SeparatorText("Layers");
    for (int k = 0; k < Terrain::kLayers; ++k) {
        Terrain::Layer& l = t.layers[k];
        ImGui::PushID(k);
        std::string label = "Layer " + std::to_string(k + 1) +
                            (l.texture >= 0 && l.texture < textures_.count() ? std::string(": ") + textures_.name(l.texture) : "");
        if (ImGui::TreeNode(label.c_str())) {
            int tex = l.texture + 1;
            if (ImGui::Combo(
                    "Texture", &tex,
                    [](void* lib, int i) { return i == 0 ? "(none)" : ((const TextureLibrary*)lib)->name(i - 1); },
                    &textures_, textures_.count() + 1)) {
                pushUndo();
                l.texture = tex - 1;
            }
            if (std::filesystem::path dropped; acceptAssetDrop(AssetKind::Texture, dropped)) {
                std::string error;
                int id = loadTexture(textures_, dropped.u8string(), error);
                if (id >= 0) {
                    pushUndo();
                    l.texture = id;
                } else {
                    setStatus("Texture failed: " + error);
                }
            }
            tooltip("Drop a texture from the Assets panel here");
            ImGui::ColorEdit3("Tint", &l.color.x);
            track();
            ImGui::DragFloat("Tiling", &l.tiling, 0.005f, 0.001f, 100.f, "%.3f / m", ImGuiSliderFlags_AlwaysClamp);
            track();
            if (ImGui::SmallButton("Paint with this")) {
                brush_.mode = terrain::Brush::Paint;
                brush_.layer = k;
                tool_ = Tool::Terrain;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

// The Terrain tool: a brush where the mouse meets the selected terrain; dragging applies it. True while it has the
// mouse (no picking then).
bool Editor::handleTerrainTool(float dt) {
    Entity* e = selection();
    bool usable = tool_ == Tool::Terrain && e && e->terrain && e->terrain->resolution >= 2 && !playing() &&
                  !viewingGameCamera();
    auto endStroke = [&] {
        if (!strokeBefore_) return;
        UndoStep step;   // the whole stroke: one undo step
        step.delta.entities.push_back(std::move(*strokeBefore_));
        step.selected = selected_;
        strokeBefore_.reset();
        undo_.push_back(std::move(step));
        if (undo_.size() > 200) undo_.erase(undo_.begin());
        redo_.clear();
        ++editVersion_;
    };
    if (!usable) {
        endStroke();
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (strokeBefore_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) endStroke();
    if (!viewportHovered() || nav_ != Nav::None) return strokeBefore_.has_value();

    Terrain& t = *e->terrain;
    Mat4 world = worldOf(e->id), inv = inverseAffine(world);
    Vec3 origin, dir;
    mouseRay(io.MousePos, origin, dir);
    Vec3 lo = transformPoint(inv, origin), ld = transformDir(inv, dir);
    float len = length(ld), hit = 0.f;
    if (len < 1e-9f || !terrain::raycast(t, lo, ld * (1.f / len), 1e6f, hit)) return strokeBefore_.has_value();
    Vec3 p = lo + ld * (hit / len);

    terrain::BrushSettings b = brush_;
    float scale = std::max(length(Vec3{world.m[0], world.m[1], world.m[2]}), 1e-4f);
    b.radius = brush_.radius / scale;   // (metres in the world -> the terrain's own)
    if (io.KeyShift && b.mode == terrain::Brush::Raise) b.mode = terrain::Brush::Lower;
    else if (io.KeyShift && b.mode == terrain::Brush::Lower) b.mode = terrain::Brush::Raise;

    // The brush's outline on the ground
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const int segments = 48;
    ImVec2 pts[segments];
    int n = 0;
    for (int i = 0; i < segments; ++i) {
        float a = (float)i / segments * 2.f * PI;
        float x = p.x + std::cos(a) * b.radius, z = p.z + std::sin(a) * b.radius;
        Vec3 w = transformPoint(world, {x, terrain::heightAt(t, x, z) + 0.05f, z});
        if (worldToScreen(w, pts[n])) ++n;
    }
    ImU32 col = b.mode == terrain::Brush::Lower ? IM_COL32(255, 140, 90, 230)
                : b.mode == terrain::Brush::Paint ? IM_COL32(120, 220, 255, 230) : IM_COL32(255, 230, 120, 230);
    if (n == segments) dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, 2.f);
    ImVec2 c;
    if (worldToScreen(transformPoint(world, p), c)) dl->AddCircleFilled(c, 3.f, col);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {   // a stroke starts: the terrain as it was, for undo
        int i = scene_.indexOf(e->id);
        strokeBefore_.emplace((size_t)i, *e);
        if (brush_.mode == terrain::Brush::Flatten) brush_.flattenHeight = p.y;
        b.flattenHeight = brush_.flattenHeight;
    }
    if (strokeBefore_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) terrain::applyBrush(t, b, p, std::min(dt, 0.05f));
    return true;
}
