// Editor: the Animator inspector. Clip, preview and scrubbing outside Play, events, and the state machine.
#include "editor/editor.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>

#include "misc/cpp/imgui_stdlib.h"
#include "editor/editor_ui.h"

using namespace editor_ui;

namespace {

bool clipCombo(const char* label, std::string& clip, const MeshLibrary& meshes) {
    bool changed = false;
    if (ImGui::BeginCombo(label, clip.empty() ? "(none)" : clip.c_str())) {
        if (ImGui::Selectable("(none)", clip.empty())) { clip.clear(); changed = true; }
        for (const AnimationClip& c : meshes.animations)
            if (ImGui::Selectable(c.name.c_str(), clip == c.name)) { clip = c.name; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

bool stateCombo(const char* label, std::string& value, const Animator& a, const char* anyLabel) {
    bool changed = false;
    const char* shown = value == "*" && anyLabel ? anyLabel : value.empty() ? "(first)" : value.c_str();
    if (ImGui::BeginCombo(label, shown)) {
        if (anyLabel && ImGui::Selectable(anyLabel, value == "*")) { value = "*"; changed = true; }
        for (const AnimatorState& s : a.states)
            if (ImGui::Selectable(s.name.c_str(), value == s.name)) { value = s.name; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

}  // namespace

void Editor::drawAnimatorInspector(Entity& e) {
    Animator& a = *e.animator;
    std::string clipName = a.clip;
    if (clipCombo("Clip", clipName, meshes_)) { pushUndo(); a.clip = clipName; }
    const AnimationClip* clip = meshes_.findAnimation(a.clip);
    if (clip) ImGui::TextDisabled("%.2f s, %zu animated nodes", clip->duration, clip->tracks.size());
    else if (!a.clip.empty()) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "No clip with that name is loaded");
    ImGui::DragFloat("Speed", &a.speed, 0.01f, -10.f, 10.f, "%.2f");
    track();
    ImGui::Checkbox("Loop", &a.loop);
    track();
    ImGui::SameLine();
    ImGui::Checkbox("Play on start", &a.playOnStart);
    track();
    tooltip("Plays the clip on the entities under this one, matched by name; game code switches clips with "
            "animator->play(\"Clip\", fadeSeconds), or drives the state machine with setParameter()");

    // Preview: the clip drawn at a time, outside Play (the scene keeps its rest pose)
    if (!playing() && clip) {
        bool mine = animPreview_.entity == e.id;
        if (ImGui::SmallButton(mine && animPreview_.playing ? "Pause" : "Preview")) {
            if (!mine) animPreview_ = {e.id, 0.f, true};
            else animPreview_.playing = !animPreview_.playing;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!mine);
        if (ImGui::SmallButton("Stop")) animPreview_ = {};
        ImGui::EndDisabled();
        float t = mine ? animPreview_.time : 0.f;
        ImGui::SetNextItemWidth(-px(110.f));
        if (ImGui::SliderFloat("Time", &t, 0.f, clip->duration, "%.2f s")) {
            animPreview_ = {e.id, t, false};   // scrubbing pauses
        }
        tooltip("Scrub through the clip. The preview only changes what's drawn, not the scene.");
    } else if (animPreview_.entity == e.id) {
        animPreview_ = {};
    }

    if (ImGui::TreeNodeEx("Events", ImGuiTreeNodeFlags_SpanAvailWidth, "Events (%zu)", a.events.size())) {
        tooltip("When playback passes an event's time in its clip, the entity's behaviours get "
                "onAnimationEvent(name)");
        int remove = -1;
        for (size_t i = 0; i < a.events.size(); ++i) {
            AnimationEvent& ev = a.events[i];
            ImGui::PushID((int)i);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
            ImGui::InputText("##name", &ev.name);
            track();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(28.f));
            const AnimationClip* evClip = meshes_.findAnimation(ev.clip);
            ImGui::DragFloat("##time", &ev.time, 0.01f, 0.f, evClip ? evClip->duration : 1000.f, "%.2f s",
                             ImGuiSliderFlags_AlwaysClamp);
            track();
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = (int)i;
            if (ev.clip != a.clip) ImGui::TextDisabled("  in %s", ev.clip.c_str());
            ImGui::PopID();
        }
        if (remove >= 0) { pushUndo(); a.events.erase(a.events.begin() + remove); }
        ImGui::BeginDisabled(a.clip.empty());
        if (ImGui::SmallButton("Add event")) {
            pushUndo();
            float at = animPreview_.entity == e.id ? animPreview_.time : 0.f;
            a.events.push_back({a.clip, at, "Event"});
        }
        tooltip("At the preview time, in the current clip");
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("States", ImGuiTreeNodeFlags_SpanAvailWidth, "State machine (%zu states)", a.states.size())) {
        tooltip("States play clips. Transitions move between them when a parameter (set by game code with "
                "animator->setParameter(\"name\", value)) meets the condition, or when a clip that doesn't loop ends.");
        int remove = -1;
        for (size_t i = 0; i < a.states.size(); ++i) {
            AnimatorState& s = a.states[i];
            ImGui::PushID((int)i);
            ImGui::SeparatorText(s.name.c_str());
            std::string name = s.name;
            if (ImGui::InputText("Name", &name, ImGuiInputTextFlags_EnterReturnsTrue) && !name.empty() && name != s.name) {
                pushUndo();
                for (AnimatorTransition& t : a.transitions) {   // keep the transitions pointing at it
                    if (t.from == s.name) t.from = name;
                    if (t.to == s.name) t.to = name;
                }
                if (a.startState == s.name) a.startState = name;
                s.name = name;
            }
            std::string sc = s.clip;
            if (clipCombo("Clip", sc, meshes_)) { pushUndo(); s.clip = sc; }
            ImGui::DragFloat("Speed", &s.speed, 0.01f, -10.f, 10.f, "%.2f");
            track();
            ImGui::Checkbox("Loop", &s.loop);
            track();
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove state")) remove = (int)i;
            ImGui::PopID();
        }
        if (remove >= 0) {
            pushUndo();
            std::string gone = a.states[(size_t)remove].name;
            a.states.erase(a.states.begin() + remove);
            a.transitions.erase(std::remove_if(a.transitions.begin(), a.transitions.end(),
                                               [&](const AnimatorTransition& t) { return t.from == gone || t.to == gone; }),
                                a.transitions.end());
            if (a.startState == gone) a.startState.clear();
        }
        if (ImGui::SmallButton("Add state")) {
            pushUndo();
            AnimatorState s;
            s.name = "State " + std::to_string(a.states.size() + 1);
            s.clip = a.clip;
            a.states.push_back(s);
        }

        if (!a.states.empty()) {
            ImGui::SeparatorText("Start and transitions");
            std::string start = a.startState;
            if (stateCombo("Start state", start, a, nullptr)) { pushUndo(); a.startState = start; }
            int removeT = -1;
            for (size_t i = 0; i < a.transitions.size(); ++i) {
                AnimatorTransition& t = a.transitions[i];
                ImGui::PushID(1000 + (int)i);
                ImGui::Separator();
                std::string from = t.from, to = t.to;
                if (stateCombo("From", from, a, "(any)")) { pushUndo(); t.from = from; }
                if (stateCombo("To", to, a, nullptr)) { pushUndo(); t.to = to; }
                int when = (int)t.when;
                const char* names[] = {"parameter >", "parameter <", "parameter true", "parameter false", "clip ended"};
                if (ImGui::Combo("When", &when, names, IM_ARRAYSIZE(names))) {
                    pushUndo();
                    t.when = (AnimatorTransition::When)when;
                }
                if (t.when != AnimatorTransition::When::ClipEnded) {
                    ImGui::InputText("Parameter", &t.parameter);
                    track();
                }
                if (t.when == AnimatorTransition::When::Greater || t.when == AnimatorTransition::When::Less) {
                    ImGui::DragFloat("Value", &t.value, 0.01f);
                    track();
                }
                ImGui::DragFloat("Fade", &t.fade, 0.01f, 0.f, 5.f, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
                track();
                if (ImGui::SmallButton("Remove transition")) removeT = (int)i;
                ImGui::PopID();
            }
            if (removeT >= 0) { pushUndo(); a.transitions.erase(a.transitions.begin() + removeT); }
            if (ImGui::SmallButton("Add transition")) {
                pushUndo();
                AnimatorTransition t;
                t.to = a.states.back().name;
                t.parameter = "speed";
                a.transitions.push_back(t);
            }
        }
        if (playing() && !a.state.empty()) {
            ImGui::TextDisabled("Now in %s", a.state.c_str());
            for (const auto& p : a.parameters) ImGui::TextDisabled("  %s = %.2f", p.first.c_str(), p.second);
        }
        ImGui::TreePop();
    }
}
