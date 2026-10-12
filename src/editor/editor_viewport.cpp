// Editor viewport: camera navigation, transform gizmos, picking, keyboard shortcuts and overlays.
#include "editor/editor.h"

#include <cfloat>
#include <cstdio>
#include <cstring>

#include "ImGuizmo.h"
#include "platform/platform.h"
#include "physics/physics.h"

namespace {
bool keyPressed(ImGuiKey key) { return ImGui::IsKeyPressed(key, false); }
}

bool Editor::viewportHovered() const {
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool inViewport = mp.x >= vx_ && mp.x < vx_ + vw_ && mp.y >= vy_ && mp.y < vy_ + vh_;
    return inViewport && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && !ImGui::IsAnyItemActive();
}

bool Editor::worldToScreen(Vec3 p, ImVec2& out) const {
    const float* m = viewProj_.m;
    float x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
    float y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
    float w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    if (w < 1e-4f) return false;
    out = ImVec2(vx_ + (x / w * 0.5f + 0.5f) * vw_, vy_ + (y / w * 0.5f + 0.5f) * vh_);   // Vulkan clip: Y down
    return true;
}

// ---------------------------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------------------------
void Editor::handleNavigation(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    if (viewingGameCamera()) { nav_ = Nav::None; return; }   // the game owns the view (and the mouse)
    bool hovered = viewportHovered();

    if (nav_ == Nav::None && hovered && !ImGuizmo::IsUsingAny()) {
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
            if (hovered && io.MouseWheel != 0.f) {
                float step = std::max(cam_.orbitDistance * 0.15f, 0.3f) * io.MouseWheel;
                cam_.pos += cam_.forward() * step;
                cam_.orbitDistance = std::max(cam_.orbitDistance - step, 1.f);
            }
            break;
    }
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
// Gizmo: edits the world matrix, written back relative to the parent
// ---------------------------------------------------------------------------------------------
void Editor::handleGizmo() {
    ImGuiIO& io = ImGui::GetIO();
    gizmoDrawn_ = false;
    if (viewingGameCamera()) return;
    if (const Entity* e = selection(); e && e->ui) return;   // (UI elements are placed by anchors: dragged in the preview)
    Mat4 view = cam_.view();
    Mat4 proj = perspective(cam_.fovY, aspect(), 0.1f, 2000.f, false);   // ImGuizmo expects GL conventions
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(vx_, vy_, vw_, vh_);

    Entity* e = selection();
    if (e && tool_ != Tool::Select && (scene_.visibleInHierarchy(e->id) || !e->meshRenderer)) {
        ImGuizmo::OPERATION op = tool_ == Tool::Move     ? ImGuizmo::TRANSLATE
                                 : tool_ == Tool::Rotate ? ImGuizmo::ROTATE
                                                         : ImGuizmo::SCALE;
        float snapValue = tool_ == Tool::Move ? snapMove_ : tool_ == Tool::Rotate ? snapRotate_ : snapScale_;
        float snapValues[3] = {snapValue, snapValue, snapValue};
        bool useSnap = snap_ != io.KeyCtrl;

        ImGuizmo::Enable(nav_ == Nav::None);
        Mat4 parentWorld = scene_.parentWorldMatrix(e->id);
        Mat4 m = parentWorld * e->transform.matrix();
        Mat4 before = m;
        std::vector<EntityId> group = topLevelSelection();
        if (ImGuizmo::Manipulate(view.m, proj.m, op, localSpace_ ? ImGuizmo::LOCAL : ImGuizmo::WORLD, m.m, nullptr,
                                 useSnap ? snapValues : nullptr)) {
            // The others move with it, as if attached: the pivot's change, applied to each one's world
            Mat4 delta = m * inverseAffine(before);
            for (EntityId id : group) {
                if (id == e->id || scene_.isAncestor(id, e->id)) continue;
                Entity* o = scene_.find(id);
                Mat4 pw = scene_.parentWorldMatrix(id);
                o->transform.setMatrix(inverseAffine(pw) * (delta * (pw * o->transform.matrix())));
                for (float& s : o->transform.scale) s = std::max(s, 0.001f);
            }
            if (std::find(group.begin(), group.end(), e->id) != group.end() || group.empty()) {
                e->transform.setMatrix(inverseAffine(parentWorld) * m);
                for (float& s : e->transform.scale) s = std::max(s, 0.001f);
            }
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

// ---------------------------------------------------------------------------------------------
// Picking: meshes by ray, mesh-less entities (cameras, empties) by their icon
// ---------------------------------------------------------------------------------------------
void Editor::handlePicking() {
    if (viewingGameCamera() || nav_ != Nav::None || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    if (!viewportHovered()) return;
    if (ImGuizmo::IsUsingAny() || ImGuizmo::IsViewManipulateHovered() || (gizmoDrawn_ && ImGuizmo::IsOver())) return;

    EntityId best = pickAt(ImGui::GetIO().MousePos);
    if (ImGui::GetIO().KeyCtrl) {   // add to / take from the selection
        if (best) toggleSelect(best);
        return;
    }
    if (best) select(best);
    else selected_ = kNoEntity;
}

void Editor::mouseRay(ImVec2 mp, Vec3& origin, Vec3& dir) const {
    float nx = (mp.x - vx_) / vw_ * 2.f - 1.f, ny = 1.f - (mp.y - vy_) / vh_ * 2.f;
    float th = std::tan(cam_.fovY * 0.5f);
    origin = cam_.pos;
    dir = normalize(cam_.forward() + cam_.right() * (nx * th * aspect()) + cam_.up() * (ny * th));
}

EntityId Editor::pickAt(ImVec2 mp) const {
    Vec3 origin, dir;
    mouseRay(mp, origin, dir);
    EntityId best = kNoEntity;
    float bestT = FLT_MAX;
    for (size_t i = 0; i < scene_.entities.size(); ++i) {
        const Entity& e = scene_.entities[i];
        if (e.locked) continue;
        const Mat4& world = worlds_[i];
        if (e.terrain && e.terrain->mesh >= 0 && e.terrain->mesh < meshes_.count() && scene_.visibleInHierarchy(e.id)) {
            Mat4 inv = inverseAffine(world);
            float t = intersectMesh(meshes_, e.terrain->mesh, transformPoint(inv, origin), transformDir(inv, dir));
            if (t > 0.f && t < bestT) { bestT = t; best = e.id; }
            continue;
        }
        if (!e.meshRenderer) {
            Vec3 pos{world.m[12], world.m[13], world.m[14]};
            ImVec2 sp;
            if (!worldToScreen(pos, sp)) continue;
            float dx = sp.x - mp.x, dy = sp.y - mp.y, t = length(pos - cam_.pos);
            if (dx * dx + dy * dy < px(14.f) * px(14.f) && t < bestT) { bestT = t; best = e.id; }
            continue;
        }
        if (!scene_.visibleInHierarchy(e.id)) continue;
        // Transform the ray into object space; t stays comparable because dir isn't renormalised
        Mat4 inv = inverseAffine(world);
        float t = intersectMesh(meshes_, e.meshRenderer->mesh, transformPoint(inv, origin), transformDir(inv, dir));
        if (t > 0.f && t < bestT) { bestT = t; best = e.id; }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------
// Shortcuts
// ---------------------------------------------------------------------------------------------
void Editor::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive() || nav_ == Nav::Fly || bindingCapture_ >= 0 ||
        keyTakenByBindings_)
        return;
    bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;

    if (ctrl && keyPressed(ImGuiKey_P)) {
        if (shift) togglePause();
        else playing() ? stopPlay() : startPlay();
        return;
    }
    if (keyPressed(ImGuiKey_F1)) showHelp_ = !showHelp_;
    if (keyPressed(ImGuiKey_F3)) showProfiler_ = !showProfiler_;
    if (playing()) return;   // everything else belongs to the game while it runs

    if (ctrl) {
        if (keyPressed(ImGuiKey_Z)) shift ? redo() : undo();
        if (keyPressed(ImGuiKey_Y)) redo();
        if (keyPressed(ImGuiKey_S)) saveToFile();
        if (keyPressed(ImGuiKey_O)) loadFromFile();
        if (keyPressed(ImGuiKey_Q)) quit_ = true;
        if (keyPressed(ImGuiKey_D)) duplicateSelected();
        if (keyPressed(ImGuiKey_A)) selectAllRoots();
        if (keyPressed(ImGuiKey_R) && !project_.module.empty()) rebuildGameModule();
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
    if (keyPressed(ImGuiKey_T))
        if (const Entity* e = selection(); e && e->terrain) tool_ = Tool::Terrain;
    if (keyPressed(ImGuiKey_L)) localSpace_ = !localSpace_;
    if (keyPressed(ImGuiKey_F) || keyPressed(ImGuiKey_KeypadDecimal)) focusSelected();
    if (keyPressed(ImGuiKey_X) || keyPressed(ImGuiKey_Delete)) deleteSelected();
    if (keyPressed(ImGuiKey_H)) toggleHideSelected();
    if (keyPressed(ImGuiKey_End)) dropSelectedToGround();
    if (keyPressed(ImGuiKey_Escape)) selected_ = kNoEntity;
}

// ---------------------------------------------------------------------------------------------
// Overlays
// ---------------------------------------------------------------------------------------------
void Editor::drawViewportOverlay() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    static const char* toolNames[] = {"Select", "Move", "Rotate", "Scale", "Terrain"};
    char line[200];
    if (playing()) {
        const char* mouse = !gameCameraActive_         ? "  |  no camera in the scene: editor view"
                            : platform::cursorCaptured() ? "  |  Esc releases the mouse"
                            : captureOnClick_           ? "  |  click to capture the mouse"
                                                        : "";
        std::snprintf(line, sizeof(line), "%s  |  t = %.2f s  |  tick %llu%s", game_.paused() ? "PAUSED" : "PLAYING",
                      game_.time(), game_.tick(), mouse);
    } else {
        std::snprintf(line, sizeof(line), "Perspective  |  %s%s%s", toolNames[(int)tool_],
                      tool_ == Tool::Select ? "" : (localSpace_ ? " (local)" : " (world)"), snap_ ? "  snap" : "");
    }
    ImVec2 p(vx_ + px(10.f), vy_ + px(8.f));
    dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 120), line);
    dl->AddText(p, IM_COL32(255, 255, 255, 230), line);
    if (playing()) {   // coloured frame around the viewport while playing
        dl->AddRect(ImVec2(vx_ + 1, vy_ + 1), ImVec2(vx_ + vw_ - 1, vy_ + vh_ - 1),
                    game_.paused() ? IM_COL32(240, 190, 60, 200) : IM_COL32(220, 70, 50, 200), 0.f, 0, px(2.f));
    }
    // Recent game log messages (GameContext::log), fading out after a few seconds
    if (playing()) {
        float y = vy_ + vh_ - px(10.f);
        const auto& log = game_.log();
        int shown = 0;
        for (auto it = log.rbegin(); it != log.rend() && shown < 6; ++it, ++shown) {
            float age = game_.time() - it->first;
            if (age > 6.f) break;
            int alpha = (int)(255.f * std::clamp(6.f - age, 0.f, 1.f));
            y -= ImGui::GetTextLineHeightWithSpacing();
            ImVec2 tp(vx_ + px(10.f), y);
            dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, alpha / 2), it->second.c_str());
            dl->AddText(tp, IM_COL32(255, 235, 140, alpha), it->second.c_str());
        }
    }
    const Entity* e = scene_.find(selected_);
    if (e && !viewingGameCamera()) {
        const char* kind = e->meshRenderer ? meshes_.name(e->meshRenderer->mesh) : e->camera ? "camera"
                           : e->terrain ? "terrain" : e->ui ? "ui" : "empty";
        std::snprintf(line, sizeof(line), "%s  (%s)", e->name.c_str(), kind);
        p.y += ImGui::GetTextLineHeightWithSpacing();
        dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 120), line);
        dl->AddText(p, IM_COL32(255, 170, 60, 255), line);
    }
}

// Mesh-less entities: cameras get their view frustum, empties a small axis cross; both get a dot to click
void Editor::drawEntityIcons() {
    if (viewingGameCamera()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vx_, vy_), ImVec2(vx_ + vw_, vy_ + vh_), true);
    for (size_t i = 0; i < scene_.entities.size(); ++i) {
        const Entity& e = scene_.entities[i];
        if (e.meshRenderer) continue;
        const Mat4& m = worlds_[i];
        Vec3 x = normalize(transformDir(m, {1, 0, 0})), y = normalize(transformDir(m, {0, 1, 0}));
        Vec3 z = normalize(cross(x, y)), pos{m.m[12], m.m[13], m.m[14]};
        ImU32 col = e.id == selected_ ? IM_COL32(255, 170, 60, 255) : IM_COL32(220, 220, 230, 200);
        ImVec2 sp;
        if (!worldToScreen(pos, sp)) continue;
        if (e.light && !e.camera) {
            if (e.light->type == Light::Type::Directional) {   // a sun with rays, and its direction
                dl->AddCircle(sp, px(9.f), col, 0, px(1.5f));
                for (int k = 0; k < 8; ++k) {
                    float a = k * PI / 4.f;
                    ImVec2 dir(std::cos(a), std::sin(a));
                    dl->AddLine(ImVec2(sp.x + dir.x * px(12.f), sp.y + dir.y * px(12.f)),
                                ImVec2(sp.x + dir.x * px(17.f), sp.y + dir.y * px(17.f)), col, px(1.5f));
                }
                ImVec2 tip;
                if (worldToScreen(pos - z * 4.f, tip)) dl->AddLine(sp, tip, col, px(1.5f));
            } else {   // a bulb; its range when selected
                dl->AddCircle(sp, px(8.f), col, 0, px(1.5f));
                if (e.id == selected_) {
                    const int n = 48;
                    const Vec3 planes[3][2] = {{{1, 0, 0}, {0, 0, 1}}, {{1, 0, 0}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}}};
                    for (const auto& plane : planes) {
                        Vec3 u = plane[0], v = plane[1];
                        for (int k = 0; k < n; ++k) {
                            float a0 = 2.f * PI * k / n, a1 = 2.f * PI * (k + 1) / n;
                            ImVec2 p0, p1;
                            if (worldToScreen(pos + (u * std::cos(a0) + v * std::sin(a0)) * e.light->range, p0) &&
                                worldToScreen(pos + (u * std::cos(a1) + v * std::sin(a1)) * e.light->range, p1))
                                dl->AddLine(p0, p1, IM_COL32(255, 220, 120, 90), px(1.f));
                        }
                    }
                }
            }
        } else if (e.camera) {
            float d = 1.5f, h = std::tan(e.camera->fovY * 0.5f) * d, w = h * 16.f / 9.f;
            Vec3 c[4] = {pos - z * d - x * w - y * h, pos - z * d + x * w - y * h, pos - z * d + x * w + y * h,
                         pos - z * d - x * w + y * h};
            ImVec2 sc[4];
            bool ok = true;
            for (int k = 0; k < 4; ++k) ok = ok && worldToScreen(c[k], sc[k]);
            if (ok) {
                for (int k = 0; k < 4; ++k) {
                    dl->AddLine(sp, sc[k], col, px(1.5f));
                    dl->AddLine(sc[k], sc[(k + 1) % 4], col, px(1.5f));
                }
                ImVec2 up;   // little triangle marks the top edge
                if (worldToScreen(pos - z * d + y * (h * 1.4f), up)) dl->AddTriangleFilled(sc[2], sc[3], up, col);
            }
        } else {
            const Vec3 axes[3] = {x, y, z * -1.f};
            const ImU32 axisCol[3] = {IM_COL32(230, 80, 80, 220), IM_COL32(110, 210, 90, 220), IM_COL32(90, 140, 240, 220)};
            for (int k = 0; k < 3; ++k) {
                ImVec2 a, b;
                if (worldToScreen(pos - axes[k] * 0.7f, a) && worldToScreen(pos + axes[k] * 0.7f, b))
                    dl->AddLine(a, b, e.id == selected_ ? col : axisCol[k], px(1.5f));
            }
        }
        dl->AddCircleFilled(sp, px(4.5f), col);
    }
    dl->PopClipRect();
}

// Collider wireframes: blue = solid, green = trigger. All of them with View > Show colliders, else the selection's
void Editor::drawColliders() {
    if (viewingGameCamera()) return;
    if (const Entity* e = scene_.find(selected_); e && e->reflectionProbe) {   // the selected probe's box
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->PushClipRect(ImVec2(vx_, vy_), ImVec2(vx_ + vw_, vy_ + vh_), true);
        const Mat4& w = worldOf(e->id);
        Vec3 c{w.m[12], w.m[13], w.m[14]}, h = e->reflectionProbe->size * 0.5f;
        auto corner = [&](int i) { return c + Vec3{i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z}; };
        for (int i = 0; i < 8; ++i)
            for (int bit : {1, 2, 4})
                if (!(i & bit)) {
                    ImVec2 a, b;
                    if (worldToScreen(corner(i), a) && worldToScreen(corner(i | bit), b))
                        dl->AddLine(a, b, IM_COL32(120, 200, 255, 200), px(1.2f));
                }
        dl->PopClipRect();
    }
    if (showContacts_ && playing()) {   // the last physics step's contacts: a dot and its normal
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->PushClipRect(ImVec2(vx_, vy_), ImVec2(vx_ + vw_, vy_ + vh_), true);
        for (const Physics::ContactPoint& c : game_.physics().contacts()) {
            ImVec2 a, b;
            if (!worldToScreen(c.position, a)) continue;
            dl->AddCircleFilled(a, px(3.f), IM_COL32(255, 80, 60, 230));
            if (worldToScreen(c.position + c.normal * 0.3f, b)) dl->AddLine(a, b, IM_COL32(255, 220, 60, 230), px(1.5f));
        }
        dl->PopClipRect();
    }
    if (const Entity* e = scene_.find(selected_); e && e->vehicle) {   // the selected vehicle's wheels
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->PushClipRect(ImVec2(vx_, vy_), ImVec2(vx_ + vw_, vy_ + vh_), true);
        const Mat4& m = worldOf(e->id);
        Vec3 side = normalize(transformDir(m, {1, 0, 0})), up = normalize(transformDir(m, {0, 1, 0})),
             fwd = normalize(transformDir(m, {0, 0, 1}));
        for (const Vehicle::Wheel& w : e->vehicle->wheels) {
            Vec3 c = transformPoint(m, w.position) - up * e->vehicle->suspensionLength;   // (hanging down)
            ImU32 col = w.driven ? IM_COL32(255, 170, 60, 220) : IM_COL32(200, 200, 200, 200);
            for (float s : {-0.5f, 0.5f}) {
                Vec3 o = c + side * (w.width * s);
                const int n = 20;
                for (int i = 0; i < n; ++i) {
                    float a0 = 2.f * PI * i / n, a1 = 2.f * PI * (i + 1) / n;
                    ImVec2 sa, sb;
                    if (worldToScreen(o + (up * std::cos(a0) + fwd * std::sin(a0)) * w.radius, sa) &&
                        worldToScreen(o + (up * std::cos(a1) + fwd * std::sin(a1)) * w.radius, sb))
                        dl->AddLine(sa, sb, col, px(1.2f));
                }
            }
            ImVec2 top, bottom;
            if (worldToScreen(transformPoint(m, w.position), top) && worldToScreen(c, bottom))
                dl->AddLine(top, bottom, col, px(1.f));
        }
        dl->PopClipRect();
    }
    bool any = showColliders_;
    if (!any)
        if (const Entity* e = scene_.find(selected_)) any = e->collider.has_value();
    if (!any) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vx_, vy_), ImVec2(vx_ + vw_, vy_ + vh_), true);
    auto line = [&](Vec3 a, Vec3 b, ImU32 col) {
        ImVec2 sa, sb;
        if (worldToScreen(a, sa) && worldToScreen(b, sb)) dl->AddLine(sa, sb, col, px(1.2f));
    };
    auto circle = [&](Vec3 c, Vec3 u, Vec3 v, float r, ImU32 col) {   // in the plane of unit vectors u, v
        const int n = 24;
        for (int i = 0; i < n; ++i) {
            float a0 = 2.f * PI * i / n, a1 = 2.f * PI * (i + 1) / n;
            line(c + (u * std::cos(a0) + v * std::sin(a0)) * r, c + (u * std::cos(a1) + v * std::sin(a1)) * r, col);
        }
    };
    for (const Entity& e : scene_.entities) {
        if (!e.collider) continue;
        bool selected = e.id == selected_;
        if (!showColliders_ && !selected) continue;
        if (e.collider->shape == Collider::Shape::Mesh && !e.meshRenderer) continue;
        const Collider& c = *e.collider;
        const Mat4& m = worldOf(e.id);
        Vec3 cx = transformDir(m, {1, 0, 0}), cy = transformDir(m, {0, 1, 0}), cz = transformDir(m, {0, 0, 1});
        float sx = length(cx), sy = length(cy), sz = length(cz);
        ImU32 col = c.trigger ? IM_COL32(90, 230, 120, selected ? 255 : 170) : IM_COL32(90, 190, 255, selected ? 255 : 150);
        switch (c.shape) {
            case Collider::Shape::Box:
            case Collider::Shape::Mesh: {   // meshes: their bounds
                Vec3 center = c.center, size = c.size;
                if (c.shape == Collider::Shape::Mesh) {
                    const Mesh& mesh = meshes_.get(e.meshRenderer->mesh);
                    center = (mesh.boundsMin + mesh.boundsMax) * 0.5f;
                    size = mesh.boundsMax - mesh.boundsMin;
                }
                Vec3 mid = transformPoint(m, center);
                Vec3 ax[3] = {cx * (size.x * 0.5f), cy * (size.y * 0.5f), cz * (size.z * 0.5f)};
                for (int i = 0; i < 3; ++i) {   // 4 edges parallel to each axis
                    Vec3 u = ax[(i + 1) % 3], v = ax[(i + 2) % 3];
                    for (float su : {-1.f, 1.f})
                        for (float sv : {-1.f, 1.f}) {
                            Vec3 corner = mid + u * su + v * sv;
                            line(corner - ax[i], corner + ax[i], col);
                        }
                }
                break;
            }
            case Collider::Shape::Sphere: {
                Vec3 mid = transformPoint(m, c.center);
                float r = c.radius * std::max({sx, sy, sz});
                circle(mid, {1, 0, 0}, {0, 1, 0}, r, col);
                circle(mid, {0, 1, 0}, {0, 0, 1}, r, col);
                circle(mid, {1, 0, 0}, {0, 0, 1}, r, col);
                break;
            }
            case Collider::Shape::Capsule: {
                Vec3 mid = transformPoint(m, c.center);
                Vec3 up = normalize(cy), u = normalize(cx), v = normalize(cz);
                float r = c.radius * std::max(sx, sz);
                float halfSeg = std::max(c.height * 0.5f * sy - r, 0.f);
                Vec3 a = mid - up * halfSeg, b = mid + up * halfSeg;
                circle(a, u, v, r, col);
                circle(b, u, v, r, col);
                for (Vec3 side : {u, u * -1.f, v, v * -1.f}) line(a + side * r, b + side * r, col);
                for (Vec3 side : {u, v}) {   // end caps: half circles
                    const int n = 12;
                    for (int i = 0; i < n; ++i) {
                        float a0 = PI * i / n, a1 = PI * (i + 1) / n;
                        line(b + (side * std::cos(a0) + up * std::sin(a0)) * r, b + (side * std::cos(a1) + up * std::sin(a1)) * r, col);
                        line(a + (side * std::cos(a0) - up * std::sin(a0)) * r, a + (side * std::cos(a1) - up * std::sin(a1)) * r, col);
                    }
                }
                break;
            }
        }
    }
    dl->PopClipRect();
}

void Editor::bakeNavigationPreview() {
    navLinks_.clear();
    if (!navPreview_.bake(scene_, meshes_, scene_.navigation)) setStatus("Nothing to walk on: no static colliders");
    else setStatus("Navigation: " + std::to_string(navPreview_.cellCount()) + " cells");
}

// The walkable grid as lines between linked cells, near the camera (a big level has millions)
void Editor::drawNavigation() {
    if (!showNavigation_ || navPreview_.empty() || viewingGameCamera()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(vx_, vy_), ImVec2(vx_ + vw_, vy_ + vh_), true);
    const float reach = 40.f;
    const Vec3 lift{0, 0.03f, 0};
    int drawn = 0;
    const std::vector<Vec3>& links = navLinks_.empty() ? (navLinks_ = navPreview_.links()) : navLinks_;
    for (size_t i = 0; i + 1 < links.size() && drawn < 60000; i += 2) {
        Vec3 d = links[i] - viewPos_;
        if (d.x * d.x + d.z * d.z > reach * reach || std::fabs(d.y) > reach) continue;
        ImVec2 a, b;
        if (worldToScreen(links[i] + lift, a) && worldToScreen(links[i + 1] + lift, b)) {
            dl->AddLine(a, b, IM_COL32(60, 220, 255, 110), px(1.f));
            ++drawn;
        }
    }
    for (const auto& [from, to] : navPreview_.offMeshLinks()) {   // links: arcs
        Vec3 prev = from;
        float height = 0.3f + 0.2f * length(to - from);
        for (int k = 1; k <= 12; ++k) {
            float u = (float)k / 12.f;
            Vec3 p = from + (to - from) * u + Vec3{0.f, 4.f * height * u * (1.f - u), 0.f};
            ImVec2 a, b;
            if (worldToScreen(prev + lift, a) && worldToScreen(p + lift, b)) dl->AddLine(a, b, IM_COL32(255, 170, 60, 230), px(2.f));
            prev = p;
        }
    }
    dl->PopClipRect();
}
