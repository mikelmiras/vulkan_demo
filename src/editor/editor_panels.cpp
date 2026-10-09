// Editor panels: menu bar, toolbar, outliner, inspector, status bar, help and dialogs.
#include "editor/editor.h"

#include <cctype>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <functional>

#include "misc/cpp/imgui_stdlib.h"
#include "platform/platform.h"

namespace {

constexpr ImGuiWindowFlags kPanelFlags =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
constexpr ImGuiWindowFlags kBarFlags = kPanelFlags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;
constexpr const char* kEntityPayload = "VKW_ENTITY";

void verticalSeparator() {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
}

void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

// "moveSpeed" -> "Move speed"
std::string prettyName(const char* key) {
    std::string out;
    for (const char* c = key; *c; ++c) {
        if (c == key) out += (char)std::toupper((unsigned char)*c);
        else if (std::isupper((unsigned char)*c)) { out += ' '; out += (char)std::tolower((unsigned char)*c); }
        else out += *c;
    }
    return out;
}

// Behaviour fields as inspector widgets; afterWidget runs after each one (undo tracking)
struct FieldInspector : FieldVisitor {
    std::function<void()> afterWidget;
    explicit FieldInspector(std::function<void()> fn) : afterWidget(std::move(fn)) {}
    void field(const char* name, float& v, float speed, float min, float max) override {
        ImGui::DragFloat(prettyName(name).c_str(), &v, speed, min, max, "%.3g",
                         min < max ? ImGuiSliderFlags_AlwaysClamp : 0);
        afterWidget();
    }
    void field(const char* name, bool& v) override {
        ImGui::Checkbox(prettyName(name).c_str(), &v);
        afterWidget();
    }
    void field(const char* name, Vec3& v, float speed) override {
        ImGui::DragFloat3(prettyName(name).c_str(), &v.x, speed, 0.f, 0.f, "%.2f");
        afterWidget();
    }
    void field(const char* name, std::string& v) override {
        ImGui::InputText(prettyName(name).c_str(), &v);
        afterWidget();
    }
};

// Collapsing header with a remove button; returns false when the component should be removed
bool componentHeader(const char* label, bool& open) {
    bool keep = true;
    open = ImGui::CollapsingHeader(label, &keep, ImGuiTreeNodeFlags_DefaultOpen);
    return keep;
}

}  // namespace

void Editor::track() {
    if (ImGui::IsItemActivated()) editBegan_ = true;
}

// ---------------------------------------------------------------------------------------------
// Menu bar / toolbar
// ---------------------------------------------------------------------------------------------
void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        ImGui::BeginDisabled(playing());   // stop first: the scene on screen is the throwaway play copy
        ImGui::SetNextItemWidth(px(220.f));
        ImGui::InputText("Scene file", scenePath_, sizeof(scenePath_));
        if (ImGui::MenuItem("Save", "Ctrl+S")) saveToFile();
        if (ImGui::MenuItem("Load", "Ctrl+O")) loadFromFile();
        if (ImGui::MenuItem("Import OBJ...")) openImport_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("New (ground + player)")) newScene(false);
        if (ImGui::MenuItem("Reset to demo scene")) newScene(true);
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        bool sel = selection() != nullptr;
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_.empty() && !playing())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, !redo_.empty() && !playing())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Shift+D", false, sel)) duplicateSelected();
        if (ImGui::MenuItem("Delete", "X / Del", false, sel)) deleteSelected();
        if (ImGui::MenuItem("Drop to ground", "End", false, sel)) dropSelectedToGround();
        if (ImGui::MenuItem("Unparent", nullptr, false, sel && selection()->parent != kNoEntity))
            reparent(selected_, kNoEntity);
        if (ImGui::MenuItem("Create prefab from selection", nullptr, false, sel)) createPrefabFromSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Hide / show selected", "H", false, sel)) toggleHideSelected();
        if (ImGui::MenuItem("Unhide all", "Alt+H")) unhideAll();
        if (ImGui::MenuItem("Deselect", "Esc", false, sel)) selected_ = kNoEntity;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        drawAddMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Game")) {
        if (ImGui::MenuItem(playing() ? "Stop" : "Play", "Ctrl+P")) playing() ? stopPlay() : startPlay();
        if (ImGui::MenuItem("Pause", "Ctrl+Shift+P", playing() && game_.paused(), playing())) togglePause();
        if (ImGui::MenuItem("Step one tick", nullptr, false, playing() && game_.paused())) game_.step();
        ImGui::Separator();
        ImGui::MenuItem("Maximize viewport on play", nullptr, &maximizeOnPlay_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Outliner", nullptr, &showOutliner_);
        ImGui::MenuItem("Inspector", nullptr, &showInspector_);
        ImGui::MenuItem("Show colliders", nullptr, &showColliders_);
        ImGui::Separator();
        if (ImGui::MenuItem("Focus selected", "F", false, selection() != nullptr)) focusSelected();
        if (ImGui::MenuItem("Reset camera")) cam_ = EditorCamera{};
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
    ImGui::BeginDisabled(selection() == nullptr);
    if (ImGui::Button("Duplicate")) duplicateSelected();
    tooltip("Shift+D (with its children)");
    ImGui::SameLine();
    if (ImGui::Button("Delete")) deleteSelected();
    tooltip("X / Delete (with its children)");
    ImGui::SameLine();
    if (ImGui::Button("Focus")) focusSelected();
    tooltip("Frame the selected object (F)");
    ImGui::EndDisabled();

    verticalSeparator();
    ImGui::BeginDisabled(undo_.empty() || playing());
    if (ImGui::Button("Undo")) undo();
    tooltip("Ctrl+Z");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redo_.empty() || playing());
    if (ImGui::Button("Redo")) redo();
    tooltip("Ctrl+Shift+Z / Ctrl+Y");
    ImGui::EndDisabled();

    verticalSeparator();
    bool p = playing();
    ImGui::PushStyleColor(ImGuiCol_Button, p ? ImVec4(0.75f, 0.25f, 0.2f, 1.f) : ImVec4(0.2f, 0.55f, 0.25f, 1.f));
    if (ImGui::Button(p ? "Stop" : "Play")) p ? stopPlay() : startPlay();
    ImGui::PopStyleColor();
    tooltip(p ? "Stop and restore the scene (Ctrl+P)" : "Run the game from this scene (Ctrl+P)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!p);
    bool paused = p && game_.paused();
    if (paused) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Pause")) togglePause();
    if (paused) ImGui::PopStyleColor();
    tooltip("Ctrl+Shift+P");
    ImGui::SameLine();
    ImGui::BeginDisabled(!paused);
    if (ImGui::Button("Step")) game_.step();
    tooltip("Advance one fixed update (1/60 s) while paused");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(140.f));
    ImGui::SliderFloat("Fly speed", &cam_.speed, 1.f, 200.f, "%.0f", ImGuiSliderFlags_Logarithmic);
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Outliner: the entity tree. Drag an entity onto another to parent it, onto the empty area to unparent.
// ---------------------------------------------------------------------------------------------
void Editor::drawOutliner() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, topH_));
    ImGui::SetNextWindowSize(ImVec2(vx_, io.DisplaySize.y - topH_ - statusH_));
    ImGui::Begin("Outliner", nullptr, kPanelFlags);
    if (!ImGui::BeginTabBar("##outliner-tabs")) { ImGui::End(); return; }
    if (ImGui::BeginTabItem("Prefabs", nullptr, showPrefabsTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
        drawPrefabs();
        ImGui::EndTabItem();
    }
    showPrefabsTab_ = false;
    if (!ImGui::BeginTabItem("Scene", nullptr, ImGuiTabItemFlags_Leading)) {
        ImGui::EndTabBar();
        ImGui::End();
        return;
    }

    filter_.Draw("Filter", -px(50.f));
    ImGui::TextDisabled("%zu entities", scene_.entities.size());
    ImGui::Separator();

    ImGui::BeginChild("##tree");
    // children[i] = ids of entity i's children; roots go in the extra last slot
    const size_t n = scene_.entities.size();
    std::vector<int> indexOf;   // not a map: ids are small and dense enough, and this runs every frame
    EntityId maxId = 0;
    for (const Entity& e : scene_.entities) maxId = std::max(maxId, e.id);
    indexOf.assign((size_t)maxId + 1, -1);
    for (size_t i = 0; i < n; ++i) indexOf[scene_.entities[i].id] = (int)i;
    std::vector<std::vector<EntityId>> children(n + 1);
    for (const Entity& e : scene_.entities) {
        int p = e.parent != kNoEntity && e.parent <= maxId ? indexOf[e.parent] : -1;
        children[p >= 0 ? (size_t)p : n].push_back(e.id);
    }

    if (filter_.IsActive()) {   // flat list of matches
        for (const Entity& e : scene_.entities)
            if (filter_.PassFilter(e.name.c_str())) {
                ImGui::PushID((int)e.id);
                if (ImGui::Selectable(e.name.c_str(), e.id == selected_)) select(e.id);
                ImGui::PopID();
            }
    } else {
        for (EntityId root : children[n]) drawOutlinerNode(root, children, indexOf);
        // The rest of the panel is a drop target meaning "make it a root"
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##root-drop", ImVec2(std::max(avail.x, 1.f), std::max(avail.y, px(40.f))));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                pendingReparent_ = *(const EntityId*)pl->Data;
                pendingParent_ = kNoEntity;
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemClicked()) selected_ = kNoEntity;
    }
    scrollToSelected_ = false;
    ImGui::EndChild();
    ImGui::EndTabItem();
    ImGui::EndTabBar();
    ImGui::End();
}

// Prefabs: templates of entity trees, saved with the scene; game code spawns them by name
void Editor::drawPrefabs() {
    if (ImGui::Button("From selection")) createPrefabFromSelection();
    tooltip("Make a prefab from the selected entity and its children");
    ImGui::SameLine();
    if (ImGui::Button("Import...")) importPrefab();
    tooltip("Load a .prefab file");
    ImGui::Separator();

    ImGui::BeginChild("##prefab-list", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f), ImGuiChildFlags_Borders);
    if (scene_.prefabs.empty()) ImGui::TextDisabled("No prefabs yet.\nSelect an entity and press\n\"From selection\".");
    for (int i = 0; i < (int)scene_.prefabs.size(); ++i) {
        const Prefab& p = scene_.prefabs[(size_t)i];
        ImGui::PushID(i);
        if (ImGui::Selectable(p.name.c_str(), selectedPrefab_ == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            selectedPrefab_ = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) instantiatePrefab(i);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%zu entities. Double-click to add one to the scene.", p.entities.size());
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (selectedPrefab_ < 0 || selectedPrefab_ >= (int)scene_.prefabs.size()) {
        selectedPrefab_ = -1;
        return;
    }
    Prefab& p = scene_.prefabs[(size_t)selectedPrefab_];
    ImGui::SetNextItemWidth(-FLT_MIN);
    std::string name = p.name;
    if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && name != p.name) {
        pushUndo();
        scene_.prefabs[(size_t)selectedPrefab_].name = scene_.uniquePrefabName(name);
    }
    tooltip("Rename (Enter). Behaviours refer to prefabs by this name, e.g. Shooter's \"prefab\" field.");
    ImGui::TextDisabled("%zu entities, root: %s", p.entities.size(), p.entities.empty() ? "-" : p.entities[0].name.c_str());
    float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Add to scene", ImVec2(w, 0))) instantiatePrefab(selectedPrefab_);
    ImGui::BeginDisabled(selection() == nullptr);
    if (ImGui::Button("Update from selection", ImVec2(w, 0))) updatePrefabFromSelection(selectedPrefab_);
    tooltip("Replace this prefab with the selected entity and its children");
    ImGui::EndDisabled();
    if (ImGui::Button("Export", ImVec2(w * 0.5f - 2.f, 0))) exportPrefab(selectedPrefab_);
    tooltip("Save as <name>.prefab in the working directory");
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(-FLT_MIN, 0))) deletePrefab(selectedPrefab_);
}

void Editor::drawOutlinerNode(EntityId id, const std::vector<std::vector<EntityId>>& children,
                              const std::vector<int>& indexOf) {
    int index = indexOf[id];
    Entity& e = scene_.entities[(size_t)index];
    const std::vector<EntityId>& kids = children[(size_t)index];

    ImGui::PushID((int)e.id);
    ImGui::Checkbox("##visible", &e.visible);
    track();
    tooltip("Visible (hides the children too)");
    ImGui::SameLine();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (e.id == selected_) flags |= ImGuiTreeNodeFlags_Selected;
    bool dim = !scene_.visibleInHierarchy(e.id);
    if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const char* tag = e.camera ? "  [cam]" : (!e.meshRenderer && kids.empty()) ? "  [empty]" : "";
    ImGui::PopID();   // an open tree node pushes its own ID: keep the ID stack balanced around it
    if (scrollToSelected_ && scene_.isAncestor(e.id, selected_)) ImGui::SetNextItemOpen(true);   // reveal selection
    bool open = ImGui::TreeNodeEx((void*)(uintptr_t)e.id, flags, "%s%s", e.name.c_str(), tag);
    if (dim) ImGui::PopStyleColor();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) select(e.id);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) pendingFocus_ = e.id;

    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kEntityPayload, &e.id, sizeof(EntityId));
        ImGui::Text("Move %s", e.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kEntityPayload)) {
            pendingReparent_ = *(const EntityId*)pl->Data;
            pendingParent_ = e.id;
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem()) {   // keyed on the tree node
        ImGui::TextDisabled("%s", e.name.c_str());
        if (ImGui::MenuItem("Focus")) pendingFocus_ = e.id;
        if (ImGui::MenuItem("Duplicate")) pendingDuplicate_ = e.id;
        if (ImGui::MenuItem("Delete")) pendingDelete_ = e.id;
        if (ImGui::MenuItem("Unparent", nullptr, false, e.parent != kNoEntity)) {
            pendingReparent_ = e.id;
            pendingParent_ = kNoEntity;
        }
        if (ImGui::MenuItem("Create prefab")) pendingPrefab_ = e.id;
        ImGui::EndPopup();
    }
    if (scrollToSelected_ && selected_ == e.id) ImGui::SetScrollHereY(0.5f);

    if (open && !kids.empty()) {
        for (EntityId child : kids) drawOutlinerNode(child, children, indexOf);
        ImGui::TreePop();
    }
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

        drawComponents(e);

        ImGui::SeparatorText("Info");
        ImGui::TextDisabled("Entity id %u", e.id);
        Vec3 wp = transformPoint(worldOf(e.id), {0, 0, 0});
        ImGui::Text("World position: %.2f %.2f %.2f", wp.x, wp.y, wp.z);
        ImGui::Text("Distance to camera: %.2f", length(wp - cam_.pos));
    } else {
        ImGui::TextWrapped("Nothing selected.");
        ImGui::TextDisabled("Click an object in the viewport\nor pick one in the outliner.");
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
            ImGui::Checkbox("Checker pattern", &mr.checker);
            track();
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
    for (size_t i = 0; i < e.behaviours.size(); ++i) {
        Behaviour& b = *e.behaviours[i];
        ImGui::PushID((int)i);
        std::string label = std::string(b.typeName()) + "  (behaviour)";
        if (!componentHeader(label.c_str(), open)) removeBehaviour = (int)i;
        else if (open) b.fields(inspector);
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

// ---------------------------------------------------------------------------------------------
// Status bar, help, popups
// ---------------------------------------------------------------------------------------------
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
                           : playing()          ? "Playing: Ctrl+P stops and restores the scene, Ctrl+Shift+P pauses"
                                                : "LMB select | RMB+WASD fly | MMB orbit | Shift+MMB pan | "
                                                  "Wheel zoom | G/R/S tools | F focus | Shift+A add | Ctrl+P play";
        ImGui::TextDisabled("%s", hint);
    }
    char right[128];
    std::snprintf(right, sizeof(right), "%zu entities | %u draws | %u tris | %.0f FPS", scene_.entities.size(),
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
        {"Outliner drag & drop", "Drop an object on another to parent it, on empty space to unparent"},
        {"Shift+A", "Add object menu"},
        {"Shift+D", "Duplicate selected (with children)"},
        {"X / Delete", "Delete selected (with children)"},
        {"End", "Drop selected to the ground"},
        {"H / Alt+H", "Hide selected / unhide all"},
        {"Esc", "Deselect"},
        {"Ctrl+P", "Play / stop the game (the scene is restored on stop)"},
        {"Ctrl+Shift+P", "Pause / resume the game"},
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

// Built-in meshes, then imported ones, then the other kinds of entity
void Editor::drawAddMenuItems() {
    for (int k = 0; k < meshes_.count(); ++k) {
        if (k == (int)MeshKind::Count) ImGui::Separator();
        if (ImGui::MenuItem(meshes_.name(k))) addObject(k);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Empty")) addEmpty();
    if (ImGui::MenuItem("Camera")) addCamera();
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

    ImGui::TextWrapped("Browse for a Wavefront .obj file or type its path. The mesh is centred and scaled to fit a "
                       "unit cube; materials and textures are ignored.");
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
    for (const auto& entry : fs::directory_iterator(fs::current_path(ec), ec))
        if (entry.path().extension() == ".obj" || entry.path().extension() == ".OBJ")
            found.push_back(entry.path().filename());
    if (!found.empty()) {
        ImGui::TextDisabled("In %s:", fs::current_path(ec).u8string().c_str());
        float rows = std::min<float>((float)found.size(), 6.f);
        if (ImGui::BeginListBox("##found", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * rows + px(8.f)))) {
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
