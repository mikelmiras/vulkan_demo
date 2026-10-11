// Editor outliner: the entity tree (drag and drop parenting) and the Prefabs tab.
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
