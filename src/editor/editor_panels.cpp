// Editor panels: menu bar, toolbar, outliner, inspector, status bar, help and dialogs.
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
        if (ImGui::MenuItem("Import model...")) openImport_ = true;
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
        ImGui::MenuItem("Capture mouse on click", nullptr, &captureOnClick_);
        ImGui::Separator();
        ImGui::MenuItem("Input bindings...", nullptr, &showInputBindings_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Outliner", nullptr, &showOutliner_);
        ImGui::MenuItem("Inspector", nullptr, &showInspector_);
        ImGui::MenuItem("Show colliders", nullptr, &showColliders_);
        ImGui::Separator();
        if (ImGui::MenuItem("Focus selected", "F", false, selection() != nullptr)) focusSelected();
        if (ImGui::MenuItem("Reset camera")) cam_ = EditorCamera{};
        bool fullscreen = platform::isFullscreen();
        if (ImGui::MenuItem("Fullscreen", "F11", &fullscreen)) platform::setFullscreen(fullscreen);
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
    int instances = 0;
    for (const Entity& e : scene_.entities)
        instances += e.prefab && e.prefab->prefab == p.id && !p.entities.empty() && e.prefab->source == p.entities[0].id;
    ImGui::TextDisabled("%zu entities, %d instance(s) in the scene", p.entities.size(), instances);
    float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button("Add to scene", ImVec2(w, 0))) instantiatePrefab(selectedPrefab_);
    ImGui::BeginDisabled(selection() == nullptr);
    if (ImGui::Button("Apply selection to prefab", ImVec2(w, 0))) updatePrefabFromSelection(selectedPrefab_);
    tooltip("Make the prefab match the selected instance (or entity tree). Every other instance follows, keeping "
            "its own changes (overrides).");
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
    bool linked = e.prefab.has_value();
    if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    else if (linked) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.75f, 1.f, 1.f));   // prefab instances: blue
    const char* tag = linked && instanceRoot(scene_, e.id) == e.id ? "  [prefab]"
                      : e.camera                                  ? "  [cam]"
                      : e.light                                   ? "  [light]"
                      : (!e.meshRenderer && kids.empty())         ? "  [empty]"
                                                                  : "";
    ImGui::PopID();   // an open tree node pushes its own ID: keep the ID stack balanced around it
    if (scrollToSelected_ && scene_.isAncestor(e.id, selected_)) ImGui::SetNextItemOpen(true);   // reveal selection
    bool open = ImGui::TreeNodeEx((void*)(uintptr_t)e.id, flags, "%s%s", e.name.c_str(), tag);
    if (dim || linked) ImGui::PopStyleColor();

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
            int tex = mr.texture + 1;   // item 0 = none
            if (ImGui::Combo(
                    "Texture", &tex,
                    [](void* lib, int i) { return i == 0 ? "(none)" : ((const TextureLibrary*)lib)->name(i - 1); },
                    &textures_, textures_.count() + 1)) {
                pushUndo();
                mr.texture = tex - 1;
            }
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
    if (e.light) {
        if (!componentHeader("Light", open)) { pushUndo(); e.light.reset(); }
        else if (open) {
            Light& l = *e.light;
            int type = (int)l.type;
            if (ImGui::Combo("Type", &type, "Directional\0Point\0")) {
                pushUndo();
                l.type = (Light::Type)type;
            }
            ImGui::ColorEdit3("Light color", &l.color.x);
            track();
            ImGui::DragFloat("Intensity", &l.intensity, 0.01f, 0.f, 100.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            track();
            if (l.type == Light::Type::Point) {
                ImGui::DragFloat("Range", &l.range, 0.1f, 0.1f, 1000.f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                track();
            } else {
                ImGui::Checkbox("Cast shadows", &l.castShadows);
                track();
                ImGui::TextDisabled("Shines along the entity's -Z (rotate it).\nThe first directional light is the sun.");
            }
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
        if (ImGui::MenuItem("Light", nullptr, false, !e.light)) { pushUndo(); e.light = Light{}; }
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
        {"Click / Esc (playing)", "Capture / release the mouse for the game"},
        {"F11", "Fullscreen"},
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
    if (ImGui::MenuItem("Point light")) addLight(Light::Type::Point);
    if (ImGui::MenuItem("Directional light")) addLight(Light::Type::Directional);
    ImGui::Separator();
    if (ImGui::MenuItem("Import model...")) openImport_ = true;
}

void Editor::drawImportPopup() {
    namespace fs = std::filesystem;
    if (openImport_) {
        ImGui::OpenPopup("Import model");
        importError_.clear();
        openImport_ = false;
    }
    ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(px(520.f), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Import model", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    ImGui::TextWrapped("glTF (.gltf / .glb): the whole model with its hierarchy, materials and textures, as a new "
                       "prefab plus a copy in the scene. OBJ: a single mesh, centred and scaled to fit a unit cube.");
    bool submit = false;
    if (ImGui::Button("Browse...")) {
        std::filesystem::path picked = platform::openFileDialog("Import model", "gltf,glb,obj");
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
    submit |= ImGui::InputTextWithHint("##path", "path, e.g. ~/Downloads/model.glb", &importPath_,
                                       ImGuiInputTextFlags_EnterReturnsTrue);

    // Model files in the working directory, for convenience
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(fs::current_path(ec), ec)) {
        std::string ext = entry.path().extension().u8string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".obj" || ext == ".gltf" || ext == ".glb") found.push_back(entry.path().filename());
    }
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
        importModel();
        if (importError_.empty()) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------
// Input bindings: the scene's named actions and what triggers them
// ---------------------------------------------------------------------------------------------
void Editor::drawInputBindings() {
    ImGui::SetNextWindowSize(ImVec2(px(560.f), px(420.f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Input bindings", &showInputBindings_, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    std::vector<InputAction>& actions = scene_.inputActions;
    ImGui::TextWrapped("Game code reads these actions by name (ctx.input().down(\"jump\")). Saved with the scene; "
                       "changes apply from the next Play.");

    // Waiting for a key: the first key or gamepad button pressed is bound (Esc cancels)
    if (bindingCapture_ >= (int)actions.size()) bindingCapture_ = -1;
    if (bindingCapture_ >= 0) {
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            ImGuiKey key = (ImGuiKey)k;
            if ((key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) || !ImGui::IsKeyPressed(key, false)) continue;
            if (key != ImGuiKey_Escape) {
                pushUndo();
                std::vector<ImGuiKey>& keys = actions[(size_t)bindingCapture_].keys;
                if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
            }
            bindingCapture_ = -1;
            keyTakenByBindings_ = true;
            break;
        }
    }

    int removeAction = -1;
    if (ImGui::BeginTable("##bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, px(130.f));
        ImGui::TableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, px(24.f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        static const char* mouseNames[] = {"Mouse Left", "Mouse Right", "Mouse Middle"};
        for (int i = 0; i < (int)actions.size(); ++i) {
            InputAction& a = actions[(size_t)i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##name", &a.name);
            track();
            ImGui::TableNextColumn();
            int removeKey = -1;
            for (int k = 0; k < (int)a.keys.size(); ++k) {
                ImGui::PushID(k);
                std::string label = std::string(keyName(a.keys[(size_t)k])) + "  x";
                if (ImGui::SmallButton(label.c_str())) removeKey = k;
                tooltip("Click to remove");
                ImGui::SameLine();
                ImGui::PopID();
            }
            if (removeKey >= 0) {
                pushUndo();
                a.keys.erase(a.keys.begin() + removeKey);
            }
            if (a.mouseButton >= 0 && a.mouseButton < 3) {
                std::string label = std::string(mouseNames[a.mouseButton]) + "  x";
                if (ImGui::SmallButton(label.c_str())) { pushUndo(); a.mouseButton = -1; }
                ImGui::SameLine();
            }
            if (bindingCapture_ == i) {
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "press a key or button... (Esc cancels)");
            } else {
                if (ImGui::SmallButton("+ key")) bindingCapture_ = i;
                tooltip("Then press a key or gamepad button");
                ImGui::SameLine();
                if (ImGui::SmallButton("+ mouse")) ImGui::OpenPopup("##mouse");
                if (ImGui::BeginPopup("##mouse")) {
                    for (int m = 0; m < 3; ++m)
                        if (ImGui::MenuItem(mouseNames[m])) { pushUndo(); a.mouseButton = m; }
                    ImGui::EndPopup();
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) removeAction = i;
            tooltip("Delete this action");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeAction >= 0) {
        pushUndo();
        actions.erase(actions.begin() + removeAction);
        bindingCapture_ = -1;
    }
    if (ImGui::Button("Add action")) {
        pushUndo();
        actions.push_back({"new_action", {}, -1});
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) {
        pushUndo();
        actions = defaultInputActions();
        bindingCapture_ = -1;
    }
    ImGui::End();
}
