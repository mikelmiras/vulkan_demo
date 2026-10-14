// Editor: the Animator inspector. Clip, preview and scrubbing outside Play (events firing as it passes them),
// events, the state machine, retargeting and mirroring clips.
#include "editor/editor.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "misc/cpp/imgui_stdlib.h"
#include "assets/retarget.h"
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

void Editor::firePreviewEvents(const Entity& e, const AnimationClip& clip, float from, float to, bool wrapped) {
    const Animator& a = *e.animator;
    for (size_t i = 0; i < a.events.size(); ++i) {
        const AnimationEvent& ev = a.events[i];
        if (ev.clip != clip.name) continue;
        bool hit = wrapped ? ev.time > from || ev.time <= to : ev.time > from && ev.time <= to;
        if (!hit) continue;
        eventFlash_[{e.id, i}] = ImGui::GetTime();
        EntityId at = e.id;   // (its bone, as posed now)
        if (!ev.bone.empty())
            for (const Entity& b : scene_.entities)
                if (b.name == ev.bone && scene_.isAncestor(e.id, b.id)) at = b.id;
        if (!ev.sound.empty() && audio_) {
            AudioEngine::PlayParams p;
            p.volume = ev.volume;
            audio_->play(ev.sound, p);
        }
        if (ev.particles != 0) {
            const Entity* bone = scene_.find(at);
            Mat4 w = scene_.worldMatrix(at);
            if (bone && bone->particleEmitter) previewParticles_.burst(scene_, at, ev.particles);
            else if (e.particleEmitter)
                previewParticles_.emit(*e.particleEmitter, {w.m[12], w.m[13], w.m[14]},
                                       ev.particles < 0 ? e.particleEmitter->burst : ev.particles);
        }
    }
}

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
            if (!mine) animPreview_ = {e.id, 0.f, true, 0.f};
            else animPreview_.playing = !animPreview_.playing;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!mine);
        if (ImGui::SmallButton("Stop")) animPreview_ = {};
        ImGui::EndDisabled();
        float t = mine ? animPreview_.time : 0.f;
        ImGui::SetNextItemWidth(-px(110.f));
        if (ImGui::SliderFloat("Time", &t, 0.f, clip->duration, "%.2f s")) {
            if (mine) animPreview_.time = t, animPreview_.playing = false;   // scrubbing pauses
            else animPreview_ = {e.id, t, false, t};
        }
        tooltip("Scrub through the clip: events passed going forwards fire (their sounds, particles; a flash on the "
                "track). The preview only changes what's drawn, not the scene.");
    } else if (animPreview_.entity == e.id) {
        animPreview_ = {};
    }

    if (ImGui::TreeNodeEx("Events", ImGuiTreeNodeFlags_SpanAvailWidth, "Events (%zu)", a.events.size())) {
        tooltip("When playback passes an event's time in its clip, the entity's behaviours get "
                "onAnimationEvent(name), and its sound plays (at its bone)");
        // The track: the clip's length with its events as marks (drag to move, double-click to add), the preview
        // time as a line (click to set it)
        if (clip && clip->duration > 0.f) {
            ImVec2 at = ImGui::GetCursorScreenPos();
            float w = std::max(ImGui::GetContentRegionAvail().x, 1.f), h = px(28.f);
            ImGui::InvisibleButton("##eventtrack", ImVec2(w, h));
            bool hovered = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(30, 33, 40, 255), 3.f);
            for (float s = 0.f; s <= clip->duration + 1e-4f; s += 0.5f) {   // (a tick every half second)
                float x = at.x + s / clip->duration * w;
                dl->AddLine(ImVec2(x, at.y + h * 0.7f), ImVec2(x, at.y + h), IM_COL32(90, 95, 110, 255));
            }
            float mouseT = std::clamp((ImGui::GetIO().MousePos.x - at.x) / w, 0.f, 1.f) * clip->duration;
            int under = -1;
            for (size_t i = 0; i < a.events.size(); ++i) {
                const AnimationEvent& ev = a.events[i];
                if (ev.clip != a.clip) continue;
                float x = at.x + ev.time / clip->duration * w;
                bool near = hovered && std::fabs(ImGui::GetIO().MousePos.x - x) < px(6.f);
                if (near) under = (int)i;
                auto flash = eventFlash_.find({e.id, i});
                float lit = flash == eventFlash_.end() ? 0.f : std::max(0.f, 1.f - (float)(ImGui::GetTime() - flash->second) / 0.4f);
                ImU32 col = (int)i == eventDrag_ || near ? IM_COL32(255, 200, 90, 255) : IM_COL32(230, 150, 60, 255);
                if (lit > 0.f) {   // (just fired)
                    col = IM_COL32(255, 255, 220, 255);
                    dl->AddCircleFilled(ImVec2(x, at.y + h * 0.3f), px(4.f) + px(6.f) * lit, IM_COL32(255, 240, 160, (int)(160 * lit)));
                }
                dl->AddTriangleFilled(ImVec2(x - px(5.f), at.y + 2.f), ImVec2(x + px(5.f), at.y + 2.f),
                                      ImVec2(x, at.y + h * 0.55f), col);
                dl->AddLine(ImVec2(x, at.y + 2.f), ImVec2(x, at.y + h), col);
            }
            if (animPreview_.entity == e.id) {
                float x = at.x + animPreview_.time / clip->duration * w;
                dl->AddLine(ImVec2(x, at.y), ImVec2(x, at.y + h), IM_COL32(120, 200, 255, 255), 2.f);
            }
            if (playing() && a.playing) {   // in Play: where it is, its events flashing as it passes them
                float now = std::fmod(std::max(a.time, 0.f), clip->duration);
                auto seen = playSeen_.find(e.id);
                if (seen != playSeen_.end() && seen->second != now)
                    for (size_t i = 0; i < a.events.size(); ++i) {
                        float t = a.events[i].time, was = seen->second;
                        bool hit = now < was ? t > was || t <= now : t > was && t <= now;
                        if (a.events[i].clip == a.clip && hit) eventFlash_[{e.id, i}] = ImGui::GetTime();
                    }
                playSeen_[e.id] = now;
                float x = at.x + now / clip->duration * w;
                dl->AddLine(ImVec2(x, at.y), ImVec2(x, at.y + h), IM_COL32(120, 255, 160, 255), 2.f);
            }
            if (ImGui::IsItemClicked()) {
                if (under >= 0) {
                    pushUndo();
                    eventDrag_ = under;
                } else if (!playing()) {
                    animPreview_ = {e.id, mouseT, false, mouseT};   // (the preview there: a jump, nothing fires)
                }
            }
            if (eventDrag_ >= 0 && eventDrag_ < (int)a.events.size()) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) a.events[(size_t)eventDrag_].time = mouseT;
                else eventDrag_ = -1;
            }
            if (hovered && under < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                pushUndo();
                a.events.push_back({a.clip, mouseT, "Event"});
            }
            if (under >= 0) ImGui::SetTooltip("%s at %.2f s", a.events[(size_t)under].name.c_str(), a.events[(size_t)under].time);
        }
        // The bones it could sound at: the entities under it
        std::vector<std::string> bones;
        for (const Entity& b : scene_.entities)
            if (scene_.isAncestor(e.id, b.id)) bones.push_back(b.name);
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
            // Its own sound (a built-in name or a file in the project), at a bone
            ImGui::Indent(px(12.f));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
            ImGui::InputTextWithHint("##sound", "sound", &ev.sound);
            track();
            tooltip("Played when the event fires: a built-in sound (Pickup, Thud, ...) or a sound file");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
            if (ImGui::BeginCombo("##bone", ev.bone.empty() ? "(the entity)" : ev.bone.c_str())) {
                if (ImGui::Selectable("(the entity)", ev.bone.empty())) { pushUndo(); ev.bone.clear(); }
                for (const std::string& b : bones)
                    if (ImGui::Selectable(b.c_str(), b == ev.bone)) { pushUndo(); ev.bone = b; }
                ImGui::EndCombo();
            }
            tooltip("Where it's heard: a bone (a foot for footsteps)");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.f);
            ImGui::SliderFloat("##volume", &ev.volume, 0.f, 2.f, "%.2f");
            track();
            // ... particles and a camera shake there
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
            ImGui::DragInt("##particles", &ev.particles, 0.2f, -1, 500, ev.particles == 0 ? "no particles"
                           : ev.particles < 0 ? "particles: its burst" : "%d particles");
            track();
            tooltip("A burst from the bone's Particle Emitter (else this entity's, at the bone): how many (-1: the "
                    "emitter's burst size)");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.f);
            ImGui::SliderFloat("##shake", &ev.shake, 0.f, 1.f, ev.shake > 0.f ? "shake %.2f" : "no shake");
            track();
            tooltip("Shakes the camera (0.3: a heavy footstep, 1: a slam), less the further it is (none past 25 m)");
            ImGui::Unindent(px(12.f));
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
            ImGui::Checkbox("Additive", &l.additive);
            track();
            tooltip("Adds the clip's change from its first frame on top of the pose (breathing, a recoil) instead of "
                    "replacing it");
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

    // Inverse kinematics
    if (ImGui::TreeNodeEx("IK", ImGuiTreeNodeFlags_SpanAvailWidth, "IK (%zu)", a.ik.size())) {
        tooltip("After the animation: a two-bone chain (an arm, a leg) reaching for a point, or a bone turning to "
                "look at one. The target is an entity, or a point game code sets with animator->setIK(name, point, "
                "weight)");
        std::unordered_map<std::string, EntityId> names = scene_.namesUnder(e.id);
        std::vector<std::string> sorted;
        for (const auto& [n, id] : names) sorted.push_back(n);
        std::sort(sorted.begin(), sorted.end());
        int remove = -1;
        for (size_t i = 0; i < a.ik.size(); ++i) {
            AnimatorIK& k = a.ik[i];
            ImGui::PushID((int)i + 3000);
            ImGui::SeparatorText(k.name.empty() ? "(unnamed)" : k.name.c_str());
            ImGui::InputText("Name", &k.name);
            track();
            int type = (int)k.type;
            const char* types[] = {"Two-bone (reach)", "Look at"};
            if (ImGui::Combo("Type", &type, types, 2)) { pushUndo(); k.type = (AnimatorIK::Type)type; }
            if (ImGui::BeginCombo("Bone", k.bone.empty() ? "(pick one)" : k.bone.c_str())) {
                for (const std::string& n : sorted)
                    if (ImGui::Selectable(n.c_str(), n == k.bone)) { pushUndo(); k.bone = n; }
                ImGui::EndCombo();
            }
            tooltip(k.type == AnimatorIK::Type::TwoBone ? "The end of the chain (a hand, a foot): it, its parent and "
                                                         "its grandparent bend to reach" : "The bone that turns");
            ImGui::InputText("Target entity", &k.targetEntity);
            track();
            tooltip("Reach for / look at this entity (by name); empty: the point game code sets");
            if (k.type == AnimatorIK::Type::TwoBone) {
                ImGui::DragFloat3("Bend towards", &k.pole.x, 0.01f, -1.f, 1.f, "%.2f");
                track();
                tooltip("Which way the middle joint bends, in the model's space (knees: forward, elbows: back)");
            } else {
                ImGui::DragFloat3("Aim axis", &k.aimAxis.x, 0.01f, -1.f, 1.f, "%.2f");
                track();
                tooltip("The bone's own axis that should point at the target");
                ImGui::SliderFloat("Max angle", &k.maxAngle, 0.f, 180.f, "%.0f deg");
                track();
            }
            ImGui::SliderFloat("Weight", &k.weight, 0.f, 1.f, "%.2f");
            track();
            if (ImGui::SmallButton("Remove")) remove = (int)i;
            ImGui::PopID();
        }
        if (remove >= 0) { pushUndo(); a.ik.erase(a.ik.begin() + remove); }
        if (ImGui::SmallButton("Add IK chain")) {
            pushUndo();
            AnimatorIK k;
            k.name = "IK " + std::to_string(a.ik.size() + 1);
            a.ik.push_back(k);
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
                bool twoD = !s.blendParameterY.empty();
                if (ImGui::Checkbox("2D", &twoD)) {
                    pushUndo();
                    s.blendParameterY = twoD ? "y" : "";
                }
                tooltip("A 2D blend space: each clip at a point of two parameters (strafing: sideways and forward "
                        "speed); the parameters' values mix the clips around them");
                if (twoD) {
                    ImGui::InputText("Parameter Y", &s.blendParameterY);
                    track();
                    // The points, and where the parameters are now (while playing)
                    float size = std::min(ImGui::GetContentRegionAvail().x, px(160.f));
                    ImVec2 o = ImGui::GetCursorScreenPos();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(o, ImVec2(o.x + size, o.y + size), IM_COL32(30, 32, 38, 255), 4.f);
                    float lo = 1e9f, hi = -1e9f;
                    for (const BlendClip& b : s.blend) lo = std::min({lo, b.at, b.atY}), hi = std::max({hi, b.at, b.atY});
                    if (hi - lo < 1e-3f) lo -= 1.f, hi += 1.f;
                    float pad = (hi - lo) * 0.15f;
                    lo -= pad, hi += pad;
                    auto at = [&](float x, float y) {
                        return ImVec2(o.x + (x - lo) / (hi - lo) * size, o.y + size - (y - lo) / (hi - lo) * size);
                    };
                    for (const BlendClip& b : s.blend) dl->AddCircleFilled(at(b.at, b.atY), px(4.f), IM_COL32(120, 180, 255, 255));
                    if (playing() && a.state == s.name)
                        dl->AddCircle(at(a.parameter(s.blendParameter), a.parameter(s.blendParameterY)), px(6.f),
                                      IM_COL32(255, 200, 80, 255), 0, 2.f);
                    ImGui::Dummy(ImVec2(size, size));
                }
                int removePoint = -1;
                for (size_t k = 0; k < s.blend.size(); ++k) {
                    ImGui::PushID((int)k + 100);
                    ImGui::SetNextItemWidth(px(60.f));
                    ImGui::DragFloat("##at", &s.blend[k].at, 0.01f, 0.f, 0.f, "%.2f");
                    track();
                    ImGui::SameLine();
                    if (twoD) {
                        ImGui::SetNextItemWidth(px(60.f));
                        ImGui::DragFloat("##atY", &s.blend[k].atY, 0.01f, 0.f, 0.f, "%.2f");
                        track();
                        ImGui::SameLine();
                    }
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
    // Retargeting: another model's clip for this one's skeleton (assets/retarget.h)
    if (ImGui::TreeNode("Retarget a clip")) {
        static std::string clip, fromPrefab;
        clipCombo("Clip", clip, meshes_);
        if (ImGui::BeginCombo("Its model", fromPrefab.empty() ? "(a prefab)" : fromPrefab.c_str())) {
            for (const Prefab& p : scene_.prefabs)
                if (ImGui::Selectable(p.name.c_str(), p.name == fromPrefab)) fromPrefab = p.name;
            ImGui::EndCombo();
        }
        tooltip("The model the clip was made for (its prefab: its skeleton in its rest pose)");
        const Prefab* from = scene_.findPrefab(fromPrefab);
        const AnimationClip* source = meshes_.findAnimation(clip);
        if (from && source) {
            Skeleton fromSkeleton = retarget::skeletonOf(*from), toSkeleton = retarget::skeletonOf(scene_, e.id);
            // The bone map: the automatic pairing to start with, then as edited (kept while the same three are picked)
            std::string key = clip + "|" + fromPrefab + "|" + std::to_string(e.id);
            if (key != retargetKey_) {
                retargetKey_ = key;
                retargetMap_ = retarget::mapBones(fromSkeleton, toSkeleton);
            }
            auto sourceOf = [&](const std::string& target) -> std::string {
                for (const auto& [t, s] : retargetMap_)
                    if (t == target) return s;
                return {};
            };
            std::vector<std::pair<std::string, std::string>> map;
            for (const auto& pair : retargetMap_)
                if (!pair.second.empty()) map.push_back(pair);
            ImGui::TextDisabled("%d of this model's %d bones paired", (int)map.size(), (int)toSkeleton.bones.size());
            if (ImGui::TreeNode("Bone map")) {
                if (ImGui::SmallButton("Automatic")) retargetMap_ = retarget::mapBones(fromSkeleton, toSkeleton);
                tooltip("Pairs them by name again (rig prefixes, sides and humanoid names understood)");
                for (const Skeleton::Bone& tb : toSkeleton.bones) {
                    ImGui::PushID(tb.name.c_str());
                    std::string current = sourceOf(tb.name);
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
                    if (ImGui::BeginCombo(tb.name.c_str(), current.empty() ? "(none)" : current.c_str())) {
                        auto set = [&](const std::string& s) {
                            bool found = false;
                            for (auto& [t, src] : retargetMap_)
                                if (t == tb.name) src = s, found = true;
                            if (!found) retargetMap_.emplace_back(tb.name, s);
                        };
                        if (ImGui::Selectable("(none)", current.empty())) set("");
                        for (const Skeleton::Bone& sb : fromSkeleton.bones)
                            if (ImGui::Selectable(sb.name.c_str(), sb.name == current)) set(sb.name);
                        ImGui::EndCombo();
                    }
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::Checkbox("Match rest poses", &retargetMatchRest_);
            tooltip("For skeletons standing differently at rest (an A-pose model and a T-pose one): each bone is first "
                    "lined up with its source's rest direction, so the clip looks the same on both");
            static bool plantFeet = true;
            ImGui::Checkbox("Plant the feet", &plantFeet);
            tooltip("Legs of other lengths put their feet where the source's are (scaled to this model's height) with "
                    "two-bone IK, so feet that stand still don't slide");
            std::string shortName = clip.substr(clip.rfind('/') == std::string::npos ? 0 : clip.rfind('/') + 1);
            std::string name = e.name + "/" + shortName;
            if (ImGui::Button("Retarget") && !map.empty()) {
                for (int n = 2; meshes_.findAnimation(name); ++n) name = e.name + "/" + shortName + " " + std::to_string(n);
                meshes_.animations.push_back(retarget::retargetClip(*source, fromSkeleton, toSkeleton, map, name,
                                                                    retargetMatchRest_, plantFeet));
                pushUndo();
                a.clip = name;
                setStatus("Made " + name + " (" + std::to_string(map.size()) + " bones)");
            }
            tooltip("Makes the clip for this model (saved with the scene as how it was made) and plays it");
        }
        ImGui::TreePop();
    }
    // Mirroring: the clip played on the other side (a left-handed swing from a right-handed one)
    if (clip && ImGui::SmallButton("Mirror the clip")) {
        Skeleton skeleton = retarget::skeletonOf(scene_, e.id);
        std::string name = clip->name + " (mirrored)";
        for (int n = 2; meshes_.findAnimation(name); ++n) name = clip->name + " (mirrored " + std::to_string(n) + ")";
        std::vector<int> other = retarget::mirrorBones(skeleton);
        int pairs = 0;
        for (size_t i = 0; i < other.size(); ++i) pairs += other[i] != (int)i;
        meshes_.animations.push_back(retarget::mirrorClip(*clip, skeleton, name));
        pushUndo();
        a.clip = name;
        setStatus("Made " + name + " (" + std::to_string(pairs / 2) + " left and right bone pairs swapped)");
    }
    tooltip("Makes the clip mirrored left to right for this model (each bone moving as its other side did; the rest "
            "pose should be symmetric) and plays it; saved with the scene as how it was made");
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
