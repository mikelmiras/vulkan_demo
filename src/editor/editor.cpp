#include "editor/editor.h"

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <set>

#include "ImGuizmo.h"
#include "misc/cpp/imgui_stdlib.h"
#include "platform/platform.h"

namespace {

constexpr float kLeftPanelW = 250.f;
constexpr float kRightPanelW = 330.f;
constexpr size_t kMaxUndo = 200;

constexpr ImGuiWindowFlags kPanelFlags =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
constexpr ImGuiWindowFlags kBarFlags = kPanelFlags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;

void verticalSeparator() {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
}

void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

bool keyPressed(ImGuiKey key) { return ImGui::IsKeyPressed(key, false); }

}  // namespace

Editor::Editor(MeshLibrary& meshes, float uiScale)
    : meshes_(meshes), uiScale_(uiScale), objects_(buildDefaultScene()) {}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------
void Editor::frame(float dt, const std::string& gpuName, const RenderStats& stats) {
    ImGuiIO& io = ImGui::GetIO();
    frameStart_ = {objects_, selected_};
    editBegan_ = false;
    if (playing_) animTime_ += dt;
    if (statusTimer_ > 0.f) statusTimer_ -= dt;
    fps_ = fps_ * 0.95f + (dt > 0.f ? 1.f / dt : 0.f) * 0.05f;

    // Must come first: creates the full-screen pass-through window the gizmos draw into,
    // so it stays underneath every panel.
    ImGuizmo::BeginFrame();

    // Layout: menu bar + toolbar on top, status bar at the bottom, panels left/right, viewport in the middle
    const ImGuiStyle& style = ImGui::GetStyle();
    float menuH = ImGui::GetFrameHeight();
    float toolbarH = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.f;
    topH_ = menuH + toolbarH;
    statusH_ = ImGui::GetTextLineHeight() + style.WindowPadding.y * 2.f;
    float leftW = showOutliner_ ? px(kLeftPanelW) : 0.f, rightW = showInspector_ ? px(kRightPanelW) : 0.f;
    vx_ = leftW;
    vy_ = topH_;
    vw_ = std::max(io.DisplaySize.x - leftW - rightW, 1.f);
    vh_ = std::max(io.DisplaySize.y - topH_ - statusH_, 1.f);

    drawMenuBar();
    drawToolbar();
    if (showOutliner_) drawOutliner();
    if (showInspector_) drawInspector(gpuName, stats);
    drawStatusBar(stats);
    if (showHelp_) drawHelp();
    drawAddPopup();
    drawImportPopup();

    handleNavigation(dt);
    handleGizmo();
    handlePicking();
    handleShortcuts();
    drawViewportOverlay();

    // Deferred actions requested from inside list iteration (context menus)
    if (pendingFocus_ >= 0) { select(pendingFocus_); focusSelected(); }
    if (pendingDuplicate_ >= 0) { select(pendingDuplicate_); duplicateSelected(); }
    if (pendingDelete_ >= 0) { select(pendingDelete_); deleteSelected(); }
    pendingFocus_ = pendingDuplicate_ = pendingDelete_ = -1;

    if (editBegan_) {
        undo_.push_back(std::move(frameStart_));
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
        redo_.clear();
    }
    if (!hasSelection()) selected_ = -1;
}

SceneRenderInfo Editor::renderInfo() const {
    SceneRenderInfo info;
    info.objects = &objects_;
    info.selected = selected_;
    info.time = animTime_;
    info.viewProj = perspective(cam_.fovY, aspect(), 0.1f, 2000.f, true) * cam_.view();
    info.camPos = cam_.pos;
    info.viewportX = vx_;
    info.viewportY = vy_;
    info.viewportW = vw_;
    info.viewportH = vh_;
    return info;
}

void Editor::track() {
    if (ImGui::IsItemActivated()) editBegan_ = true;
}

// ---------------------------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------------------------
void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        ImGui::SetNextItemWidth(px(220.f));
        ImGui::InputText("Scene file", scenePath_, sizeof(scenePath_));
        if (ImGui::MenuItem("Save", "Ctrl+S")) saveToFile();
        if (ImGui::MenuItem("Load", "Ctrl+O")) loadFromFile();
        if (ImGui::MenuItem("Import OBJ...")) openImport_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("New (ground only)")) {
            pushUndo();
            objects_.resize(1);
            objects_[0] = buildDefaultScene()[0];
            selected_ = -1;
        }
        if (ImGui::MenuItem("Reset to demo scene")) {
            pushUndo();
            objects_ = buildDefaultScene();
            selected_ = -1;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_.empty())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, !redo_.empty())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Shift+D", false, hasSelection())) duplicateSelected();
        if (ImGui::MenuItem("Delete", "X / Del", false, hasSelection())) deleteSelected();
        if (ImGui::MenuItem("Drop to ground", "End", false, hasSelection())) dropSelectedToGround();
        ImGui::Separator();
        if (ImGui::MenuItem("Hide / show selected", "H", false, hasSelection())) toggleHideSelected();
        if (ImGui::MenuItem("Unhide all", "Alt+H")) unhideAll();
        if (ImGui::MenuItem("Deselect", "Esc", false, hasSelection())) selected_ = -1;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        drawAddMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Outliner", nullptr, &showOutliner_);
        ImGui::MenuItem("Inspector", nullptr, &showInspector_);
        ImGui::Separator();
        if (ImGui::MenuItem("Focus selected", "F", false, hasSelection())) focusSelected();
        if (ImGui::MenuItem("Reset camera")) cam_ = EditorCamera{};
        ImGui::MenuItem("Play animation", "Space", &playing_);
        ImGui::Separator();
        ImGui::MenuItem("Controls help", "F1", &showHelp_);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Editor::drawToolbar() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetFrameHeight()));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, topH_ - ImGui::GetFrameHeight()));
    ImGui::Begin("##toolbar", nullptr, kBarFlags);

    auto toolButton = [&](const char* label, Tool t, const char* tip) {
        bool active = tool_ == t;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label)) tool_ = t;
        if (active) ImGui::PopStyleColor();
        tooltip(tip);
        ImGui::SameLine();
    };
    toolButton("Select", Tool::Select, "Select only, no gizmo (Q)");
    toolButton("Move", Tool::Move, "Translate gizmo (G)");
    toolButton("Rotate", Tool::Rotate, "Rotate gizmo (R)");
    toolButton("Scale", Tool::Scale, "Scale gizmo (S)");

    verticalSeparator();
    if (ImGui::Button(localSpace_ ? "Local" : "World")) localSpace_ = !localSpace_;
    tooltip("Gizmo orientation: world or object axes (L)");
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &snap_);
    tooltip("Snap gizmo moves to increments. Holding Ctrl while dragging inverts this.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(70.f));
    if (tool_ == Tool::Rotate) ImGui::DragFloat("##snap", &snapRotate_, 0.5f, 1.f, 180.f, "%.0f deg");
    else if (tool_ == Tool::Scale) ImGui::DragFloat("##snap", &snapScale_, 0.01f, 0.01f, 10.f, "%.2f x");
    else ImGui::DragFloat("##snap", &snapMove_, 0.05f, 0.05f, 100.f, "%.2f m");
    tooltip("Snap increment for the current tool");

    verticalSeparator();
    if (ImGui::Button("Add...")) openAddMenu_ = true;
    tooltip("Add a new object in front of the camera (Shift+A)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSelection());
    if (ImGui::Button("Duplicate")) duplicateSelected();
    tooltip("Shift+D");
    ImGui::SameLine();
    if (ImGui::Button("Delete")) deleteSelected();
    tooltip("X / Delete");
    ImGui::SameLine();
    if (ImGui::Button("Focus")) focusSelected();
    tooltip("Frame the selected object (F)");
    ImGui::EndDisabled();

    verticalSeparator();
    ImGui::BeginDisabled(undo_.empty());
    if (ImGui::Button("Undo")) undo();
    tooltip("Ctrl+Z");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redo_.empty());
    if (ImGui::Button("Redo")) redo();
    tooltip("Ctrl+Shift+Z / Ctrl+Y");
    ImGui::EndDisabled();

    verticalSeparator();
    if (ImGui::Button(playing_ ? "Pause" : "Play")) playing_ = !playing_;
    tooltip("Toggle spin/bob animation (Space)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(140.f));
    ImGui::SliderFloat("Fly speed", &cam_.speed, 1.f, 200.f, "%.0f", ImGuiSliderFlags_Logarithmic);
    ImGui::End();
}

void Editor::drawOutliner() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, topH_));
    ImGui::SetNextWindowSize(ImVec2(px(kLeftPanelW), io.DisplaySize.y - topH_ - statusH_));
    ImGui::Begin("Outliner", nullptr, kPanelFlags);

    filter_.Draw("Filter", -px(50.f));
    ImGui::TextDisabled("%zu objects", objects_.size());
    ImGui::Separator();

    ImGui::BeginChild("##list");
    for (int i = 0; i < (int)objects_.size(); ++i) {
        SceneObject& o = objects_[i];
        if (!filter_.PassFilter(o.name.c_str())) continue;
        ImGui::PushID(i);
        ImGui::Checkbox("##visible", &o.visible);
        track();
        tooltip("Visible");
        ImGui::SameLine();
        if (!o.visible) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(o.name.c_str(), selected_ == i)) select(i);
        if (!o.visible) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) pendingFocus_ = i;
        if (ImGui::BeginPopupContextItem()) {
            ImGui::TextDisabled("%s (%s)", o.name.c_str(), meshes_.name(o.mesh));
            if (ImGui::MenuItem("Focus")) pendingFocus_ = i;
            if (ImGui::MenuItem("Duplicate")) pendingDuplicate_ = i;
            if (ImGui::MenuItem("Delete")) pendingDelete_ = i;
            ImGui::EndPopup();
        }
        if (scrollToSelected_ && selected_ == i) ImGui::SetScrollHereY(0.5f);
        ImGui::PopID();
    }
    scrollToSelected_ = false;
    ImGui::EndChild();
    ImGui::End();
}

void Editor::drawInspector(const std::string& gpuName, const RenderStats& stats) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - px(kRightPanelW), topH_));
    ImGui::SetNextWindowSize(ImVec2(px(kRightPanelW), io.DisplaySize.y - topH_ - statusH_));
    ImGui::Begin("Inspector", nullptr, kPanelFlags);
    ImGui::PushItemWidth(-px(110.f));

    if (!hasSelection()) {
        ImGui::TextWrapped("Nothing selected.");
        ImGui::TextDisabled("Click an object in the viewport\nor pick one in the outliner.");
    } else {
        SceneObject& o = objects_[selected_];
        ImGui::InputText("Name", &o.name);
        track();
        ImGui::Combo(
            "Mesh", &o.mesh, [](void* lib, int i) { return ((const MeshLibrary*)lib)->name(i); }, &meshes_,
            meshes_.count());
        track();

        ImGui::SeparatorText("Transform");
        ImGui::DragFloat3("Position", o.position, 0.05f, 0.f, 0.f, "%.2f");
        track();
        ImGui::DragFloat3("Rotation", o.rotation, 0.5f, 0.f, 0.f, "%.1f");
        track();
        ImGui::DragFloat3("Scale", o.scale, 0.01f, 0.001f, 10000.f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        track();
        float uniform = (o.scale[0] + o.scale[1] + o.scale[2]) / 3.f, before = uniform;
        if (ImGui::DragFloat("Uniform scale", &uniform, 0.01f, 0.001f, 10000.f, "%.3f", ImGuiSliderFlags_AlwaysClamp) &&
            before > 0.f)
            for (float& s : o.scale) s = std::max(s * uniform / before, 0.001f);
        track();
        if (ImGui::Button("Reset rot")) { pushUndo(); o.rotation[0] = o.rotation[1] = o.rotation[2] = 0.f; }
        ImGui::SameLine();
        if (ImGui::Button("Reset scale")) { pushUndo(); o.scale[0] = o.scale[1] = o.scale[2] = 1.f; }
        ImGui::SameLine();
        if (ImGui::Button("Drop to ground")) dropSelectedToGround();

        ImGui::SeparatorText("Material");
        ImGui::ColorEdit3("Color", &o.color.x);
        track();
        ImGui::Checkbox("Checker pattern", &o.checker);
        track();

        ImGui::SeparatorText("Flags");
        ImGui::Checkbox("Visible", &o.visible);
        track();
        ImGui::SameLine();
        ImGui::Checkbox("Locked", &o.locked);
        track();
        tooltip("Locked objects ignore viewport clicks (select them in the outliner)");

        ImGui::SeparatorText("Animation");
        ImGui::DragFloat("Spin (rad/s)", &o.spin, 0.01f, -20.f, 20.f, "%.2f");
        track();
        ImGui::DragFloat("Bob height", &o.bob, 0.01f, 0.f, 50.f, "%.2f");
        track();
        ImGui::SliderAngle("Phase", &o.phase, 0.f, 360.f);
        track();

        ImGui::SeparatorText("Info");
        const Mesh& mesh = meshes_.get(o.mesh);
        ImGui::Text("Triangles: %u", mesh.indexCount / 3);
        ImGui::Text("Distance to camera: %.2f", length(o.pos() - cam_.pos));
        if (ImGui::TreeNode("World matrix")) {
            Mat4 m = o.animatedMatrix(animTime_);
            for (int r = 0; r < 4; ++r)
                ImGui::Text("%8.3f %8.3f %8.3f %8.3f", m.m[r], m.m[4 + r], m.m[8 + r], m.m[12 + r]);
            ImGui::TreePop();
        }
    }

    ImGui::SeparatorText("Camera");
    ImGui::DragFloat3("Cam position", &cam_.pos.x, 0.1f, 0.f, 0.f, "%.2f");
    float yawDeg = cam_.yaw / DEG2RAD, pitchDeg = cam_.pitch / DEG2RAD;
    if (ImGui::DragFloat("Yaw", &yawDeg, 0.5f, 0.f, 0.f, "%.1f")) cam_.yaw = yawDeg * DEG2RAD;
    if (ImGui::DragFloat("Pitch", &pitchDeg, 0.5f, -89.f, 89.f, "%.1f")) cam_.pitch = pitchDeg * DEG2RAD;
    ImGui::SliderAngle("FOV", &cam_.fovY, 20.f, 120.f);

    ImGui::SeparatorText("Renderer");
    ImGui::Text("GPU: %s", gpuName.c_str());
    ImGui::Text("%.0f FPS (%.2f ms)", fps_, fps_ > 0.f ? 1000.f / fps_ : 0.f);
    ImGui::Text("Draw calls: %u   Triangles: %u", stats.drawCalls, stats.triangles);
    ImGui::PopItemWidth();
    ImGui::End();
}

void Editor::drawStatusBar(const RenderStats& stats) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - statusH_));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, statusH_));
    ImGui::Begin("##status", nullptr, kBarFlags);
    if (statusTimer_ > 0.f) {
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", status_.c_str());
    } else {
        const char* hint = nav_ == Nav::Fly     ? "Flying: WASD move, E/Q up/down, Shift fast, wheel = speed"
                           : nav_ == Nav::Orbit ? "Orbiting (Shift+MMB to pan)"
                           : nav_ == Nav::Pan   ? "Panning"
                                                : "LMB select | RMB+WASD fly | MMB orbit | Shift+MMB pan | "
                                                  "Wheel zoom | G/R/S tools | F focus | Shift+A add | F1 help";
        ImGui::TextDisabled("%s", hint);
    }
    char right[128];
    std::snprintf(right, sizeof(right), "%zu objects | %u draws | %u tris | %.0f FPS", objects_.size(),
                  stats.drawCalls, stats.triangles, fps_);
    float w = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(std::max(ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x, 0.f));
    ImGui::TextUnformatted(right);
    ImGui::End();
}

void Editor::drawHelp() {
    ImGui::SetNextWindowSize(ImVec2(px(470.f), 0), ImGuiCond_FirstUseEver);
    ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Controls", &showHelp_, ImGuiWindowFlags_NoSavedSettings)) { ImGui::End(); return; }
    static const char* rows[][2] = {
        {"Left click", "Select object (click empty space to deselect)"},
        {"Right mouse + WASD", "Fly: mouse look, E/Q up/down, Shift faster, wheel = speed"},
        {"Middle mouse drag", "Orbit around the point in front of the camera"},
        {"Shift + middle drag", "Pan"},
        {"Mouse wheel", "Zoom (dolly)"},
        {"View cube (top-right)", "Click a face/edge to snap the view, drag to rotate"},
        {"Q / G / R / S", "Select / Move / Rotate / Scale tool"},
        {"L", "Toggle world/local gizmo axes"},
        {"Ctrl while dragging", "Toggle snapping"},
        {"F  (or double-click)", "Focus selected object"},
        {"Shift+A", "Add object menu"},
        {"Shift+D", "Duplicate selected"},
        {"X / Delete", "Delete selected"},
        {"End", "Drop selected to the ground"},
        {"H / Alt+H", "Hide selected / unhide all"},
        {"Esc", "Deselect"},
        {"Space", "Play / pause animation"},
        {"Ctrl+Z / Ctrl+Shift+Z", "Undo / redo"},
        {"Ctrl+S / Ctrl+O", "Save / load scene file"},
    };
    if (ImGui::BeginTable("help", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        for (auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[0]);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r[1]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Editor::drawAddPopup() {
    if (openAddMenu_) {
        ImGui::OpenPopup("##add");
        openAddMenu_ = false;
    }
    if (ImGui::BeginPopup("##add")) {
        ImGui::TextDisabled("Add object");
        ImGui::Separator();
        drawAddMenuItems();
        ImGui::EndPopup();
    }
}

// Built-in meshes, then imported ones, then the import dialog
void Editor::drawAddMenuItems() {
    for (int k = 0; k < meshes_.count(); ++k) {
        if (k == (int)MeshKind::Count) ImGui::Separator();
        if (ImGui::MenuItem(meshes_.name(k))) addObject(k);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Import OBJ...")) openImport_ = true;
}

void Editor::drawImportPopup() {
    namespace fs = std::filesystem;
    if (openImport_) {
        ImGui::OpenPopup("Import OBJ");
        importError_.clear();
        openImport_ = false;
    }
    ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(px(520.f), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Import OBJ", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    ImGui::TextWrapped("Browse for a Wavefront .obj file or type its path. The mesh is centred and scaled to fit a unit cube; "
                       "materials and textures are ignored.");
    bool submit = false;
    if (ImGui::Button("Browse...")) {
        std::filesystem::path picked = platform::openFileDialog("Import OBJ model", "obj");
        // The dialog ran its own event loop: drop key/button state ImGui may have missed the release of
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        if (!picked.empty()) {
            importPath_ = picked.u8string();
            submit = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    submit |= ImGui::InputTextWithHint("##path", "path, e.g. ~/Downloads/model.obj", &importPath_,
                                       ImGuiInputTextFlags_EnterReturnsTrue);

    // .obj files in the working directory, for convenience
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(fs::current_path(ec), ec))
        if (e.path().extension() == ".obj" || e.path().extension() == ".OBJ") found.push_back(e.path().filename());
    if (!found.empty()) {
        ImGui::TextDisabled("In %s:", fs::current_path(ec).u8string().c_str());
        if (ImGui::BeginListBox("##found", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * std::min<float>(found.size(), 6.f) + px(8.f)))) {
            for (const auto& name : found) {
                std::string n = name.u8string();
                if (ImGui::Selectable(n.c_str(), importPath_ == n, ImGuiSelectableFlags_AllowDoubleClick)) {
                    importPath_ = n;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) submit = true;
                }
            }
            ImGui::EndListBox();
        }
    }
    if (!importError_.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", importError_.c_str());

    ImGui::Separator();
    ImGui::BeginDisabled(importPath_.empty());
    if (ImGui::Button("Import", ImVec2(px(100.f), 0))) submit = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(px(100.f), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();
    if (submit && !importPath_.empty()) {
        importMesh();
        if (importError_.empty()) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Editor::drawViewportOverlay() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    static const char* toolNames[] = {"Select", "Move", "Rotate", "Scale"};
    char line[160];
    std::snprintf(line, sizeof(line), "Perspective  |  %s%s%s  |  %s", toolNames[(int)tool_],
                  tool_ == Tool::Select ? "" : (localSpace_ ? " (local)" : " (world)"), snap_ ? "  snap" : "",
                  playing_ ? "animating" : "paused");
    ImVec2 p(vx_ + px(10.f), vy_ + px(8.f));
    dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 120), line);
    dl->AddText(p, IM_COL32(255, 255, 255, 230), line);
    if (hasSelection()) {
        const SceneObject& o = objects_[selected_];
        std::snprintf(line, sizeof(line), "%s  (%s)", o.name.c_str(), meshes_.name(o.mesh));
        p.y += ImGui::GetTextLineHeightWithSpacing();
        dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 120), line);
        dl->AddText(p, IM_COL32(255, 170, 60, 255), line);
    }
}

// ---------------------------------------------------------------------------------------------
// Viewport interaction
// ---------------------------------------------------------------------------------------------
void Editor::handleNavigation(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mp = io.MousePos;
    bool inViewport = mp.x >= vx_ && mp.x < vx_ + vw_ && mp.y >= vy_ && mp.y < vy_ + vh_;
    bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemActive();
    bool viewportHovered = inViewport && !overUi;

    if (nav_ == Nav::None && viewportHovered && !ImGuizmo::IsUsingAny()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) nav_ = Nav::Fly;
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) nav_ = io.KeyShift ? Nav::Pan : Nav::Orbit;
    }
    if ((nav_ == Nav::Fly && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) ||
        ((nav_ == Nav::Orbit || nav_ == Nav::Pan) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)))
        nav_ = Nav::None;

    const float lookSens = 0.0035f;
    ImVec2 d = io.MouseDelta;
    switch (nav_) {
        case Nav::Fly: {
            cam_.yaw += d.x * lookSens;
            cam_.pitch = std::clamp(cam_.pitch - d.y * lookSens, -1.55f, 1.55f);
            Vec3 move{};
            if (ImGui::IsKeyDown(ImGuiKey_W)) move += cam_.forward();
            if (ImGui::IsKeyDown(ImGuiKey_S)) move += cam_.forward() * -1.f;
            if (ImGui::IsKeyDown(ImGuiKey_D)) move += cam_.right();
            if (ImGui::IsKeyDown(ImGuiKey_A)) move += cam_.right() * -1.f;
            if (ImGui::IsKeyDown(ImGuiKey_E) || ImGui::IsKeyDown(ImGuiKey_Space)) move += Vec3{0, 1, 0};
            if (ImGui::IsKeyDown(ImGuiKey_Q) || ImGui::IsKeyDown(ImGuiKey_C)) move += Vec3{0, -1, 0};
            if (length(move) > 0.f)
                cam_.pos += normalize(move) * (cam_.speed * (io.KeyShift ? 4.f : 1.f) * dt);
            if (io.MouseWheel != 0.f)
                cam_.speed = std::clamp(cam_.speed * std::pow(1.2f, io.MouseWheel), 1.f, 200.f);
            break;
        }
        case Nav::Orbit: {
            Vec3 pivot = cam_.pos + cam_.forward() * cam_.orbitDistance;
            cam_.yaw += d.x * lookSens * 1.5f;
            cam_.pitch = std::clamp(cam_.pitch - d.y * lookSens * 1.5f, -1.55f, 1.55f);
            cam_.pos = pivot - cam_.forward() * cam_.orbitDistance;
            break;
        }
        case Nav::Pan: {
            float k = 2.f * std::tan(cam_.fovY * 0.5f) * cam_.orbitDistance / vh_;   // world units per pixel at pivot
            cam_.pos += cam_.right() * (-d.x * k) + cam_.up() * (d.y * k);
            break;
        }
        case Nav::None:
            if (viewportHovered && io.MouseWheel != 0.f) {
                float step = std::max(cam_.orbitDistance * 0.15f, 0.3f) * io.MouseWheel;
                cam_.pos += cam_.forward() * step;
                cam_.orbitDistance = std::max(cam_.orbitDistance - step, 1.f);
            }
            break;
    }
}

void Editor::handleGizmo() {
    ImGuiIO& io = ImGui::GetIO();
    Mat4 view = cam_.view();
    Mat4 proj = perspective(cam_.fovY, aspect(), 0.1f, 2000.f, false);   // ImGuizmo expects GL conventions
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(vx_, vy_, vw_, vh_);

    gizmoDrawn_ = false;
    if (hasSelection() && tool_ != Tool::Select && objects_[selected_].visible) {
        SceneObject& o = objects_[selected_];
        ImGuizmo::OPERATION op = tool_ == Tool::Move     ? ImGuizmo::TRANSLATE
                                 : tool_ == Tool::Rotate ? ImGuizmo::ROTATE
                                                         : ImGuizmo::SCALE;
        float snapValue = tool_ == Tool::Move ? snapMove_ : tool_ == Tool::Rotate ? snapRotate_ : snapScale_;
        float snapValues[3] = {snapValue, snapValue, snapValue};
        bool useSnap = snap_ != io.KeyCtrl;

        ImGuizmo::Enable(nav_ == Nav::None);
        Mat4 m = o.baseMatrix();
        if (ImGuizmo::Manipulate(view.m, proj.m, op, localSpace_ ? ImGuizmo::LOCAL : ImGuizmo::WORLD, m.m, nullptr,
                                 useSnap ? snapValues : nullptr)) {
            ImGuizmo::DecomposeMatrixToComponents(m.m, o.position, o.rotation, o.scale);
            for (float& s : o.scale) s = std::max(s, 0.001f);
        }
        gizmoDrawn_ = true;
    }
    bool using_ = ImGuizmo::IsUsing();
    if (using_ && !gizmoWasUsing_) editBegan_ = true;
    gizmoWasUsing_ = using_;

    // Orientation cube in the top-right corner of the viewport
    const float cube = px(110.f);
    // (overload taking the projection, so it also works when no Manipulate() ran this frame)
    Mat4 newView = view, identity;
    ImGuizmo::ViewManipulate(newView.m, proj.m, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, identity.m,
                             cam_.orbitDistance, ImVec2(vx_ + vw_ - cube - px(8.f), vy_ + px(8.f)), ImVec2(cube, cube),
                             IM_COL32(0, 0, 0, 40));
    if (std::memcmp(newView.m, view.m, sizeof(view.m)) != 0) setCameraFromView(newView);
}

void Editor::handlePicking() {
    ImGuiIO& io = ImGui::GetIO();
    if (nav_ != Nav::None || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    ImVec2 mp = io.MousePos;
    bool inViewport = mp.x >= vx_ && mp.x < vx_ + vw_ && mp.y >= vy_ && mp.y < vy_ + vh_;
    if (!inViewport || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemActive()) return;
    if (ImGuizmo::IsUsingAny() || ImGuizmo::IsViewManipulateHovered() || (gizmoDrawn_ && ImGuizmo::IsOver())) return;

    // Camera ray through the mouse position
    float nx = (mp.x - vx_) / vw_ * 2.f - 1.f, ny = 1.f - (mp.y - vy_) / vh_ * 2.f;
    float th = std::tan(cam_.fovY * 0.5f);
    Vec3 dir = normalize(cam_.forward() + cam_.right() * (nx * th * aspect()) + cam_.up() * (ny * th));

    int best = -1;
    float bestT = FLT_MAX;
    for (int i = 0; i < (int)objects_.size(); ++i) {
        const SceneObject& o = objects_[i];
        if (!o.visible || o.locked) continue;
        // Transform the ray into object space; t stays comparable because dir isn't renormalised
        Mat4 inv = inverseAffine(o.animatedMatrix(animTime_));
        float t = intersectMesh(meshes_, o.mesh, transformPoint(inv, cam_.pos), transformDir(inv, dir));
        if (t > 0.f && t < bestT) { bestT = t; best = i; }
    }
    if (best >= 0) select(best);
    else selected_ = -1;
}

void Editor::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive() || nav_ == Nav::Fly) return;
    bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;

    if (ctrl) {
        if (keyPressed(ImGuiKey_Z)) shift ? redo() : undo();
        if (keyPressed(ImGuiKey_Y)) redo();
        if (keyPressed(ImGuiKey_S)) saveToFile();
        if (keyPressed(ImGuiKey_O)) loadFromFile();
        if (keyPressed(ImGuiKey_Q)) quit_ = true;
        if (keyPressed(ImGuiKey_D)) duplicateSelected();
        return;
    }
    if (shift) {
        if (keyPressed(ImGuiKey_A)) openAddMenu_ = true;
        if (keyPressed(ImGuiKey_D)) duplicateSelected();
        return;
    }
    if (alt) {
        if (keyPressed(ImGuiKey_H)) unhideAll();
        return;
    }
    if (keyPressed(ImGuiKey_Q)) tool_ = Tool::Select;
    if (keyPressed(ImGuiKey_G) || keyPressed(ImGuiKey_W)) tool_ = Tool::Move;
    if (keyPressed(ImGuiKey_R) || keyPressed(ImGuiKey_E)) tool_ = Tool::Rotate;
    if (keyPressed(ImGuiKey_S)) tool_ = Tool::Scale;
    if (keyPressed(ImGuiKey_L)) localSpace_ = !localSpace_;
    if (keyPressed(ImGuiKey_F) || keyPressed(ImGuiKey_KeypadDecimal)) focusSelected();
    if (keyPressed(ImGuiKey_X) || keyPressed(ImGuiKey_Delete)) deleteSelected();
    if (keyPressed(ImGuiKey_H)) toggleHideSelected();
    if (keyPressed(ImGuiKey_End)) dropSelectedToGround();
    if (keyPressed(ImGuiKey_Escape)) selected_ = -1;
    if (keyPressed(ImGuiKey_Space)) playing_ = !playing_;
    if (keyPressed(ImGuiKey_F1)) showHelp_ = !showHelp_;
}

void Editor::setCameraFromView(const Mat4& v) {
    // Rows of the view rotation are the camera's right / up / back axes
    Vec3 right{v.m[0], v.m[4], v.m[8]}, up{v.m[1], v.m[5], v.m[9]}, back{v.m[2], v.m[6], v.m[10]};
    Vec3 t{v.m[12], v.m[13], v.m[14]};
    cam_.pos = (right * t.x + up * t.y + back * t.z) * -1.f;
    Vec3 fwd = back * -1.f;
    cam_.pitch = std::clamp(std::asin(std::clamp(fwd.y, -1.f, 1.f)), -1.55f, 1.55f);
    if (std::fabs(right.x) + std::fabs(right.z) > 1e-4f) cam_.yaw = std::atan2(-right.x, right.z);
}

// ---------------------------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------------------------
void Editor::select(int index) {
    selected_ = index;
    scrollToSelected_ = true;
}

std::string Editor::uniqueName(const std::string& wanted) const {
    std::set<std::string> names;
    for (const auto& o : objects_) names.insert(o.name);
    if (!names.count(wanted)) return wanted;
    // Strip a trailing ".NNN" and find the next free number, Blender style
    std::string base = wanted;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < base.size() &&
        base.find_first_not_of("0123456789", dot + 1) == std::string::npos)
        base = base.substr(0, dot);
    for (int n = 1;; ++n) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), ".%03d", n);
        if (!names.count(base + buf)) return base + buf;
    }
}

void Editor::addObject(int mesh) {
    pushUndo();
    SceneObject o;
    o.mesh = mesh;
    o.name = uniqueName(std::string(meshes_.name(mesh)) + ".001");
    float size = mesh == (int)MeshKind::Plane ? 6.f : 2.f;
    for (float& s : o.scale) s = size;

    // Drop it where the view ray hits the ground (if close enough), otherwise 10 units ahead
    Vec3 fwd = cam_.forward(), p = cam_.pos + fwd * 10.f;
    if (fwd.y < -0.05f) {
        float t = -cam_.pos.y / fwd.y;
        if (t > 0.f && t < 60.f) p = cam_.pos + fwd * t;
    }
    p.y = std::max(p.y, std::max(-meshes_.get(mesh).boundsMin.y * size, 0.f) + 0.01f);   // rest on the ground
    o.position[0] = p.x; o.position[1] = p.y; o.position[2] = p.z;

    static const Vec3 palette[] = {{0.90f, 0.45f, 0.35f}, {0.40f, 0.70f, 0.95f}, {0.55f, 0.85f, 0.45f},
                                   {0.95f, 0.80f, 0.35f}, {0.75f, 0.50f, 0.90f}};
    o.color = palette[objects_.size() % 5];
    objects_.push_back(o);
    select((int)objects_.size() - 1);
    setStatus("Added " + o.name);
}

void Editor::duplicateSelected() {
    if (!hasSelection()) return;
    pushUndo();
    SceneObject copy = objects_[selected_];
    copy.name = uniqueName(copy.name);
    Vec3 offset = cam_.right() * std::max(copy.scale[0], 1.f);
    copy.position[0] += offset.x;
    copy.position[2] += offset.z;
    objects_.insert(objects_.begin() + selected_ + 1, copy);
    select(selected_ + 1);
    setStatus("Duplicated as " + copy.name);
}

void Editor::deleteSelected() {
    if (!hasSelection()) return;
    pushUndo();
    setStatus("Deleted " + objects_[selected_].name);
    objects_.erase(objects_.begin() + selected_);
    selected_ = -1;
}

void Editor::toggleHideSelected() {
    if (!hasSelection()) return;
    pushUndo();
    objects_[selected_].visible = !objects_[selected_].visible;
}

void Editor::unhideAll() {
    pushUndo();
    for (auto& o : objects_) o.visible = true;
}

void Editor::focusSelected() {
    if (!hasSelection()) return;
    const SceneObject& o = objects_[selected_];
    Mat4 m = o.animatedMatrix(animTime_);
    const Mesh& mesh = meshes_.get(o.mesh);
    Vec3 center = transformPoint(m, (mesh.boundsMin + mesh.boundsMax) * 0.5f);
    float radius = 0.75f * std::max({std::fabs(o.scale[0]), std::fabs(o.scale[1]), std::fabs(o.scale[2])});
    float dist = std::max(radius / std::tan(cam_.fovY * 0.5f) * 1.3f, 1.f);
    cam_.pos = center - cam_.forward() * dist;
    cam_.orbitDistance = dist;
}

void Editor::dropSelectedToGround() {
    if (!hasSelection()) return;
    pushUndo();
    SceneObject& o = objects_[selected_];
    // Lowest point of the mesh under the current rotation/scale
    Mat4 m = o.baseMatrix();
    const Mesh& mesh = meshes_.get(o.mesh);
    float minY = FLT_MAX;
    for (uint32_t i = 0; i < mesh.indexCount; ++i)
        minY = std::min(minY, transformPoint(m, meshes_.vertices[mesh.vertexOffset + meshes_.indices[mesh.firstIndex + i]].pos).y);
    o.position[1] -= minY;
}

void Editor::saveToFile() {
    std::string error;
    if (saveScene(scenePath_, objects_, meshes_, error)) setStatus(std::string("Saved ") + scenePath_);
    else setStatus("Save failed: " + error);
}

void Editor::loadFromFile() {
    std::vector<SceneObject> loaded;
    std::string error;
    if (!loadScene(scenePath_, loaded, meshes_, error)) { setStatus("Load failed: " + error); return; }
    pushUndo();
    objects_ = std::move(loaded);
    selected_ = -1;
    setStatus(std::string("Loaded ") + scenePath_);
}

void Editor::importMesh() {
    std::string path = importPath_;
    if (path.size() >= 2 && path[0] == '~' && (path[1] == '/' || path[1] == '\\'))
        if (const char* home = std::getenv("HOME")) path = home + path.substr(1);
    int before = meshes_.count();
    importError_.clear();
    int id = importObj(meshes_, path, importError_);
    if (id < 0) return;
    addObject(id);
    const Mesh& m = meshes_.get(id);
    setStatus((meshes_.count() > before ? "Imported " : "Already imported: ") + m.name + " (" +
              std::to_string(m.indexCount / 3) + " triangles)");
}

void Editor::setStatus(const std::string& msg) {
    status_ = msg;
    statusTimer_ = 4.f;
}

// ---------------------------------------------------------------------------------------------
// Undo / redo (whole-scene snapshots; the scene is tiny)
// ---------------------------------------------------------------------------------------------
void Editor::pushUndo() {
    undo_.push_back({objects_, selected_});
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
}

void Editor::undo() {
    if (undo_.empty()) return;
    redo_.push_back({objects_, selected_});
    objects_ = std::move(undo_.back().objects);
    selected_ = undo_.back().selected;
    undo_.pop_back();
    editBegan_ = false;
    setStatus("Undo");
}

void Editor::redo() {
    if (redo_.empty()) return;
    undo_.push_back({objects_, selected_});
    objects_ = std::move(redo_.back().objects);
    selected_ = redo_.back().selected;
    redo_.pop_back();
    editBegan_ = false;
    setStatus("Redo");
}
