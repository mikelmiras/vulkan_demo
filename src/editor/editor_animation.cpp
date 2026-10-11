// Editor: the Animator inspector. Clip, preview and scrubbing outside Play, events, and the state machine.
#include "editor/editor.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <unordered_map>

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

    // Root motion
    ImGui::Checkbox("Root motion", &a.rootMotion);
    track();
    tooltip("The root bone's travel across the ground moves this entity (as a character, with a capsule or sphere "
            "collider) instead of the bone, so feet don't slide");
    if (a.rootMotion) {
        std::unordered_map<std::string, EntityId> names = scene_.namesUnder(e.id);
        if (ImGui::BeginCombo("Root bone", a.rootBone.empty() ? "(pick one)" : a.rootBone.c_str())) {
            std::vector<std::string> sorted;
            for (const auto& [n, id] : names) sorted.push_back(n);
            std::sort(sorted.begin(), sorted.end());
            for (const std::string& n : sorted)
                if (ImGui::Selectable(n.c_str(), n == a.rootBone)) { pushUndo(); a.rootBone = n; }
            ImGui::EndCombo();
        }
    }

    // Layers
    if (ImGui::TreeNodeEx("Layers", ImGuiTreeNodeFlags_SpanAvailWidth, "Layers (%zu)", a.layers.size())) {
        tooltip("Clips played over the main one on part of the skeleton (the mask bone and everything under it), "
                "mixed in by weight. Game code: animator->playLayer(\"Upper\", \"Wave\", 1.f), stopLayer(...)");
        int removeLayer = -1;
        std::unordered_map<std::string, EntityId> names = scene_.namesUnder(e.id);
        for (size_t i = 0; i < a.layers.size(); ++i) {
            AnimatorLayer& l = a.layers[i];
            ImGui::PushID((int)i + 2000);
            ImGui::SeparatorText(l.name.empty() ? "(unnamed)" : l.name.c_str());
            ImGui::InputText("Name", &l.name);
            track();
            std::string lc = l.clip;
            if (clipCombo("Clip", lc, meshes_)) { pushUndo(); l.clip = lc; }
            if (ImGui::BeginCombo("Mask", l.mask.empty() ? "(whole skeleton)" : l.mask.c_str())) {
                if (ImGui::Selectable("(whole skeleton)", l.mask.empty())) { pushUndo(); l.mask.clear(); }
                std::vector<std::string> sorted;
                for (const auto& [n, id] : names) sorted.push_back(n);
                std::sort(sorted.begin(), sorted.end());
                for (const std::string& n : sorted)
                    if (ImGui::Selectable(n.c_str(), n == l.mask)) { pushUndo(); l.mask = n; }
                ImGui::EndCombo();
            }
            ImGui::SliderFloat("Weight", &l.weight, 0.f, 1.f, "%.2f");
            track();
            ImGui::DragFloat("Speed", &l.speed, 0.01f, -10.f, 10.f, "%.2f");
            track();
            ImGui::Checkbox("Loop", &l.loop);
            track();
            ImGui::SameLine();
            ImGui::Checkbox("Playing", &l.playing);
            track();
            tooltip("Plays from the start of the game (else when game code calls playLayer)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) removeLayer = (int)i;
            ImGui::PopID();
        }
        if (removeLayer >= 0) { pushUndo(); a.layers.erase(a.layers.begin() + removeLayer); }
        if (ImGui::SmallButton("Add layer")) {
            pushUndo();
            AnimatorLayer l;
            l.name = "Layer " + std::to_string(a.layers.size() + 1);
            a.layers.push_back(l);
        }
        ImGui::TreePop();
    }

    if (!a.states.empty() && ImGui::SmallButton("Open graph")) animGraphEntity_ = e.id;
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
            bool tree = !s.blend.empty();
            if (ImGui::Checkbox("Blend tree", &tree)) {
                pushUndo();
                if (tree) s.blend = {{s.clip, 0.f}, {s.clip, 1.f}};
                else s.blend.clear();
            }
            tooltip("Mixes the clips either side of a parameter's value (walk at 0, run at 1, say), kept in step");
            if (!tree) {
                std::string sc = s.clip;
                if (clipCombo("Clip", sc, meshes_)) { pushUndo(); s.clip = sc; }
            } else {
                ImGui::InputText("Parameter", &s.blendParameter);
                track();
                int removePoint = -1;
                for (size_t k = 0; k < s.blend.size(); ++k) {
                    ImGui::PushID((int)k + 100);
                    ImGui::SetNextItemWidth(px(60.f));
                    ImGui::DragFloat("##at", &s.blend[k].at, 0.01f, 0.f, 0.f, "%.2f");
                    track();
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(28.f));
                    std::string bc = s.blend[k].clip;
                    if (clipCombo("##clip", bc, meshes_)) { pushUndo(); s.blend[k].clip = bc; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) removePoint = (int)k;
                    ImGui::PopID();
                }
                if (removePoint >= 0 && s.blend.size() > 1) { pushUndo(); s.blend.erase(s.blend.begin() + removePoint); }
                if (ImGui::SmallButton("Add blend point")) {
                    pushUndo();
                    s.blend.push_back({s.blend.back().clip, s.blend.back().at + 1.f});
                }
            }
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

// The state machine as a graph: states round a circle, arrows for transitions (their conditions on hover), the
// state playing now highlighted
void Editor::drawAnimatorGraph() {
    Entity* e = scene_.find(animGraphEntity_);
    if (!e || !e->animator || e->animator->states.empty()) { animGraphEntity_ = kNoEntity; return; }
    const Animator& a = *e->animator;
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(px(520.f), px(420.f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(1.f);
    if (!ImGui::Begin(("State machine: " + e->name + "###animgraph").c_str(), &open)) {
        ImGui::End();
        if (!open) animGraphEntity_ = kNoEntity;
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
    ImVec2 centre(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
    float radius = std::max(std::min(size.x, size.y) * 0.5f - px(60.f), px(40.f));
    size_t n = a.states.size();
    std::vector<ImVec2> at(n);
    for (size_t i = 0; i < n; ++i) {
        float ang = (float)i / (float)n * 2.f * PI - PI * 0.5f;
        at[i] = n == 1 ? centre : ImVec2(centre.x + std::cos(ang) * radius, centre.y + std::sin(ang) * radius);
    }
    auto index = [&](const std::string& name) {
        for (size_t i = 0; i < n; ++i)
            if (a.states[i].name == name) return (int)i;
        return -1;
    };
    const ImVec2 half(px(55.f), px(16.f));
    const ImU32 arrow = IM_COL32(170, 180, 200, 200);
    ImVec2 mouse = ImGui::GetIO().MousePos;
    std::string hover;
    auto drawArrow = [&](ImVec2 p0, ImVec2 p1, const AnimatorTransition& t, float bend) {
        ImVec2 d(p1.x - p0.x, p1.y - p0.y);
        float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < 1.f) return;
        ImVec2 u(d.x / len, d.y / len), nrm(-u.y * bend, u.x * bend);   // offset: two-way pairs don't overlap
        ImVec2 a0(p0.x + nrm.x + u.x * half.x, p0.y + nrm.y + u.y * half.y * 1.6f);
        ImVec2 a1(p1.x + nrm.x - u.x * half.x, p1.y + nrm.y - u.y * half.y * 1.6f);
        dl->AddLine(a0, a1, arrow, px(1.5f));
        ImVec2 l(-u.y, u.x);
        dl->AddTriangleFilled(a1, ImVec2(a1.x - u.x * px(9.f) + l.x * px(4.f), a1.y - u.y * px(9.f) + l.y * px(4.f)),
                              ImVec2(a1.x - u.x * px(9.f) - l.x * px(4.f), a1.y - u.y * px(9.f) - l.y * px(4.f)), arrow);
        ImVec2 mid((a0.x + a1.x) * 0.5f, (a0.y + a1.y) * 0.5f);
        if (std::fabs(mouse.x - mid.x) < px(12.f) && std::fabs(mouse.y - mid.y) < px(12.f)) {
            const char* names[] = {">", "<", "is true", "is false", ""};
            hover = t.when == AnimatorTransition::When::ClipEnded
                        ? "when the clip ends"
                        : t.parameter + " " + names[(int)t.when] +
                              (t.when == AnimatorTransition::When::Greater || t.when == AnimatorTransition::When::Less
                                   ? " " + std::to_string(t.value).substr(0, 5) : "");
            hover += "  (fade " + std::to_string(t.fade).substr(0, 4) + " s)";
        }
    };
    for (const AnimatorTransition& t : a.transitions) {
        int to = index(t.to);
        if (to < 0) continue;
        if (t.from == "*") {   // from any state: a short arrow from outside the circle
            ImVec2 dir(at[(size_t)to].x - centre.x, at[(size_t)to].y - centre.y);
            float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (len < 1.f) dir = ImVec2(0.f, -1.f), len = 1.f;
            ImVec2 outside(at[(size_t)to].x + dir.x / len * px(80.f), at[(size_t)to].y + dir.y / len * px(80.f));
            dl->AddText(ImVec2(outside.x - px(10.f), outside.y - px(8.f)), arrow, "any");
            drawArrow(outside, at[(size_t)to], t, 0.f);
            continue;
        }
        int from = index(t.from);
        if (from >= 0 && from != to) drawArrow(at[(size_t)from], at[(size_t)to], t, px(6.f));
    }
    for (size_t i = 0; i < n; ++i) {
        const AnimatorState& s = a.states[i];
        bool now = playing() && a.state == s.name;
        bool start = a.startState.empty() ? i == 0 : a.startState == s.name;
        ImVec2 p0(at[i].x - half.x, at[i].y - half.y), p1(at[i].x + half.x, at[i].y + half.y);
        dl->AddRectFilled(p0, p1, now ? IM_COL32(70, 140, 90, 255) : IM_COL32(45, 55, 75, 255), px(6.f));
        dl->AddRect(p0, p1, start ? IM_COL32(240, 200, 90, 255) : IM_COL32(110, 120, 140, 255), px(6.f), 0, px(1.5f));
        std::string label = s.name + (s.blend.empty() ? "" : " *");
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(at[i].x - ts.x * 0.5f, at[i].y - ts.y * 0.5f), IM_COL32(235, 240, 250, 255), label.c_str());
        if (mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y)
            hover = s.blend.empty() ? "clip " + s.clip : "blend tree on " + s.blendParameter;
    }
    if (!hover.empty()) ImGui::SetTooltip("%s", hover.c_str());
    ImGui::Dummy(size);
    ImGui::End();
    if (!open) animGraphEntity_ = kNoEntity;
}
