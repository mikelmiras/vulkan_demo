// Editor viewport: camera navigation, transform gizmos, picking, keyboard shortcuts and overlays.
#include "editor/editor.h"

#include <cfloat>
#include <cstdio>
#include <cstring>

#include "ImGuizmo.h"

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
        // Edit the transform, not the animated play pose (the Animator is applied on top of it)
        Mat4 parentWorld = playing() ? worldOf(e->parent) : scene_.parentWorldMatrix(e->id);
        Mat4 m = parentWorld * e->transform.matrix();
        if (ImGuizmo::Manipulate(view.m, proj.m, op, localSpace_ ? ImGuizmo::LOCAL : ImGuizmo::WORLD, m.m, nullptr,
                                 useSnap ? snapValues : nullptr)) {
            e->transform.setMatrix(inverseAffine(parentWorld) * m);
            for (float& s : e->transform.scale) s = std::max(s, 0.001f);
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

    // Camera ray through the mouse position
    ImVec2 mp = ImGui::GetIO().MousePos;
    float nx = (mp.x - vx_) / vw_ * 2.f - 1.f, ny = 1.f - (mp.y - vy_) / vh_ * 2.f;
    float th = std::tan(cam_.fovY * 0.5f);
    Vec3 dir = normalize(cam_.forward() + cam_.right() * (nx * th * aspect()) + cam_.up() * (ny * th));

    EntityId best = kNoEntity;
    float bestT = FLT_MAX;
    for (size_t i = 0; i < scene_.entities.size(); ++i) {
        const Entity& e = scene_.entities[i];
        if (e.locked) continue;
        const Mat4& world = worlds_[i];
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
        float t = intersectMesh(meshes_, e.meshRenderer->mesh, transformPoint(inv, cam_.pos), transformDir(inv, dir));
        if (t > 0.f && t < bestT) { bestT = t; best = e.id; }
    }
    if (best) select(best);
    else selected_ = kNoEntity;
}

// ---------------------------------------------------------------------------------------------
// Shortcuts
// ---------------------------------------------------------------------------------------------
void Editor::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive() || nav_ == Nav::Fly) return;
    bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;

    if (ctrl && keyPressed(ImGuiKey_P)) {
        if (shift) togglePause();
        else playing() ? stopPlay() : startPlay();
        return;
    }
    if (keyPressed(ImGuiKey_F1)) showHelp_ = !showHelp_;
    if (playing()) return;   // everything else belongs to the game while it runs

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
    if (keyPressed(ImGuiKey_Escape)) selected_ = kNoEntity;
}

// ---------------------------------------------------------------------------------------------
// Overlays
// ---------------------------------------------------------------------------------------------
void Editor::drawViewportOverlay() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    static const char* toolNames[] = {"Select", "Move", "Rotate", "Scale"};
    char line[200];
    if (playing()) {
        std::snprintf(line, sizeof(line), "%s  |  t = %.2f s  |  tick %llu%s", game_.paused() ? "PAUSED" : "PLAYING",
                      game_.time(), game_.tick(), gameCameraActive_ ? "" : "  |  no camera in the scene: editor view");
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
    const Entity* e = scene_.find(selected_);
    if (e && !viewingGameCamera()) {
        const char* kind = e->meshRenderer ? meshes_.name(e->meshRenderer->mesh) : e->camera ? "camera" : "empty";
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
        if (e.camera) {
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
