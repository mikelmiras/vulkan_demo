// Small widgets and constants shared by the editor's panel files (editor_panels / outliner / inspector.cpp)
#pragma once

#include <cctype>
#include <functional>
#include <string>

#include "imgui.h"
#include "scene/behaviour.h"

namespace editor_ui {

constexpr ImGuiWindowFlags kPanelFlags =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
constexpr ImGuiWindowFlags kBarFlags = kPanelFlags | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;
constexpr const char* kEntityPayload = "VKW_ENTITY";

inline void verticalSeparator() {
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
}

inline void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

// "moveSpeed" -> "Move speed"
inline std::string prettyName(const char* key) {
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
    // Draws the picker for a reference (FieldRef); without one they're text boxes
    std::function<void(const char* label, std::string& v, FieldRef kind)> pickReference;
    std::function<void()> changed;   // a value was set outright (picked from a list): record it for undo
    explicit FieldInspector(std::function<void()> fn) : afterWidget(std::move(fn)) {}
    void field(const char* name, int& v, int min, int max) override {
        ImGui::DragInt(prettyName(name).c_str(), &v, 0.2f, min, max, "%d", min < max ? ImGuiSliderFlags_AlwaysClamp : 0);
        afterWidget();
    }
    void choice(const char* name, int& v, const char* const* options, int count) override {
        if (ImGui::Combo(prettyName(name).c_str(), &v, options, count) && changed) changed();
    }
    void field(const char* name, std::string& v, FieldRef kind) override {
        if (pickReference) pickReference(prettyName(name).c_str(), v, kind);
        else field(name, v);
    }
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
inline bool componentHeader(const char* label, bool& open) {
    bool keep = true;
    open = ImGui::CollapsingHeader(label, &keep, ImGuiTreeNodeFlags_DefaultOpen);
    return keep;
}

}  // namespace editor_ui
