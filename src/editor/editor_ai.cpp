// Editor: AI brains (game/behaviour_tree.h): the tree in the Inspector (each node with what its parameters mean;
// while playing, coloured by what it last answered), perception and the blackboard's starting values.
#include "editor/editor.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "editor/editor_ui.h"
#include "game/behaviour_tree.h"

using namespace editor_ui;

namespace {

// Removes node `i` and everything under it, keeping the other indices right
void removeSubtree(AIBrain& b, int i) {
    std::vector<bool> gone(b.nodes.size(), false);
    std::function<void(int)> mark = [&](int n) {
        if (n < 0 || n >= (int)b.nodes.size() || gone[(size_t)n]) return;
        gone[(size_t)n] = true;
        for (int c : b.nodes[(size_t)n].children) mark(c);
    };
    mark(i);
    std::vector<int> remap(b.nodes.size(), -1);
    std::vector<AIBrain::Node> kept;
    for (size_t n = 0; n < b.nodes.size(); ++n)
        if (!gone[n]) {
            remap[n] = (int)kept.size();
            kept.push_back(b.nodes[n]);
        }
    for (AIBrain::Node& n : kept) {
        std::vector<int> kids;
        for (int c : n.children)
            if (c >= 0 && c < (int)remap.size() && remap[(size_t)c] >= 0) kids.push_back(remap[(size_t)c]);
        n.children = kids;
    }
    b.nodes = kept;
}

ImVec4 statusColour(int8_t s) {
    switch (s) {
        case 0: return ImVec4(0.45f, 0.9f, 0.45f, 1.f);   // success
        case 1: return ImVec4(1.f, 0.45f, 0.4f, 1.f);     // failure
        case 2: return ImVec4(1.f, 0.85f, 0.3f, 1.f);     // running
        default: return ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    }
}

}  // namespace

void Editor::drawBrainInspector(Entity& e) {
    AIBrain& b = *e.brain;
    if (b.nodes.empty()) {
        ImGui::TextDisabled("No tree yet.");
        if (ImGui::Button("Start a guard tree")) {
            pushUndo();
            ai::makeGuard(b, "");
        }
        tooltip("Chase what it sees, go and look where it heard something, else patrol (set its waypoints)");
        ImGui::SameLine();
        if (ImGui::Button("Empty tree")) {
            pushUndo();
            b.nodes.push_back(AIBrain::Node{});
            b.nodes[0].type = "Selector";
        }
    }
    // The tree, depth first: a row per node
    int removeNode = -1, addTo = -1, moveUp = -1;
    std::function<void(int, int, int)> row = [&](int i, int parent, int depth) {
        if (i < 0 || i >= (int)b.nodes.size() || depth > 32) return;
        AIBrain::Node& n = b.nodes[(size_t)i];
        const ai::NodeType* t = ai::nodeType(n.type);
        ImGui::PushID(i);
        ImGui::Indent((float)depth * 12.f + 0.001f);
        int8_t status = playing() && i < (int)b.status.size() ? b.status[(size_t)i] : -1;
        ImGui::TextColored(statusColour(status), "%s", status == 2 ? ">" : status == 0 ? "+" : status == 1 ? "x" : "-");
        ImGui::SameLine();
        std::string label = (n.name.empty() ? n.type : n.name + " (" + n.type + ")") + "###node";
        bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        if (t && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t->help);
        if (open) {
            if (ImGui::BeginCombo("Type", n.type.c_str())) {
                const char* group = "";
                for (const ai::NodeType& nt : ai::nodeTypes()) {
                    if (std::strcmp(group, nt.group) != 0) {
                        ImGui::SeparatorText(nt.group);
                        group = nt.group;
                    }
                    if (ImGui::Selectable(nt.type, n.type == nt.type)) {
                        pushUndo();
                        n.type = nt.type;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", nt.help);
                }
                ImGui::EndCombo();
            }
            ImGui::InputText("Name", &n.name);
            track();
            if (t && t->key) {
                ImGui::InputText(t->key, &n.key);
                track();
            }
            if (t && t->text) {
                ImGui::InputText(t->text, &n.text);
                track();
            }
            if (t && t->number) {
                ImGui::DragFloat(t->number, &n.number, 0.05f);
                track();
            }
            if (t && t->number2) {
                ImGui::DragFloat(t->number2, &n.number2, 0.05f);
                track();
            }
            bool takesChildren = t && (t->children < 0 || (t->children == 1 && n.children.empty()));
            if (takesChildren && ImGui::SmallButton("Add child")) addTo = i;
            if (parent >= 0) {
                if (takesChildren) ImGui::SameLine();
                if (ImGui::SmallButton("Up")) moveUp = i;
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) removeNode = i;
            }
            std::vector<int> kids = n.children;   // (copied: rows may change the tree)
            for (int c : kids) row(c, i, depth + 1);
            ImGui::TreePop();
        }
        ImGui::Unindent((float)depth * 12.f + 0.001f);
        ImGui::PopID();
    };
    if (!b.nodes.empty()) row(0, -1, 0);
    if (addTo >= 0) {
        pushUndo();
        AIBrain::Node n;
        n.type = "Wait";
        n.number = 1.f;
        b.nodes.push_back(n);
        b.nodes[(size_t)addTo].children.push_back((int)b.nodes.size() - 1);
    }
    if (moveUp >= 0)   // earlier among its siblings
        for (AIBrain::Node& p : b.nodes) {
            auto it = std::find(p.children.begin(), p.children.end(), moveUp);
            if (it != p.children.end() && it != p.children.begin()) {
                pushUndo();
                std::iter_swap(it, it - 1);
                break;
            }
        }
    if (removeNode > 0) {
        pushUndo();
        removeSubtree(b, removeNode);
    }

    ImGui::SeparatorText("Perception");
    ImGui::InputText("Notices tag", &b.perceives);
    track();
    tooltip("Entities with this tag are seen and heard (e.g. Player)");
    ImGui::DragFloat("Sight range", &b.sightRange, 0.1f, 0.f, 1000.f, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
    track();
    ImGui::SliderFloat("Sight angle", &b.sightAngle, 1.f, 360.f, "%.0f deg");
    track();
    ImGui::DragFloat("Eye height", &b.eyeHeight, 0.01f, 0.f, 10.f, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
    track();
    ImGui::DragFloat("Hearing range", &b.hearingRange, 0.1f, 0.f, 1000.f, "%.1f m", ImGuiSliderFlags_AlwaysClamp);
    track();
    ImGui::DragFloat("Memory", &b.memory, 0.1f, 0.f, 600.f, "%.1f s", ImGuiSliderFlags_AlwaysClamp);
    track();
    tooltip("How long it remembers a target it can no longer see");

    ImGui::SeparatorText(playing() ? "Blackboard (live)" : "Blackboard (starting values)");
    if (!b.blackboard.isObject()) b.blackboard = json::Value::object();
    std::string removeKey;
    for (auto& [key, value] : b.blackboard.members()) {
        ImGui::PushID(key.c_str());
        std::string text = value.type() == json::Value::Type::String ? value.asString() : json::write(value);
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
        if (ImGui::InputText(key.c_str(), &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
            pushUndo();
            value = ai::parseValue(text);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeKey = key;
        ImGui::PopID();
    }
    if (!removeKey.empty()) {
        pushUndo();
        auto& m = b.blackboard.members();
        m.erase(std::remove_if(m.begin(), m.end(), [&](const auto& kv) { return kv.first == removeKey; }), m.end());
    }
    static std::string newKey;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::InputText("##newkey", &newKey);
    ImGui::SameLine();
    if (ImGui::SmallButton("Add value") && !newKey.empty()) {
        pushUndo();
        b.blackboard.set(newKey, json::Value(0.0));
        newKey.clear();
    }
    tooltip("A value its tree can read (Has, Compare) and write (Set): numbers, true / false, text, [x, y, z]");
}
