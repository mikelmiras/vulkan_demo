// Scene editor: camera navigation, picking, transform gizmos, undo/redo and the ImGui panels.
#pragma once

#include <string>
#include <vector>

#include "imgui.h"
#include "geometry.h"
#include "renderer.h"
#include "scene.h"

struct EditorCamera {
    Vec3 pos{0, 10, 42};
    float yaw = -PI / 2, pitch = -0.22f;
    float speed = 12.f;           // fly speed, units/s
    float orbitDistance = 40.f;   // distance to the orbit/pan pivot in front of the camera
    float fovY = 60.f * DEG2RAD;

    Vec3 forward() const {
        return {std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw)};
    }
    Vec3 right() const { return normalize(cross(forward(), {0, 1, 0})); }
    Vec3 up() const { return cross(right(), forward()); }
    Mat4 view() const { return lookAt(pos, pos + forward(), {0, 1, 0}); }
};

class Editor {
public:
    explicit Editor(const MeshLibrary& meshes);

    // Builds the UI and handles viewport input. Call between ImGui::NewFrame() and ImGui::Render().
    void frame(float dt, const std::string& gpuName, const RenderStats& stats);
    SceneRenderInfo renderInfo() const;
    bool quitRequested() const { return quit_; }

private:
    enum class Tool { Select, Move, Rotate, Scale };
    enum class Nav { None, Fly, Orbit, Pan };
    struct Snapshot {
        std::vector<SceneObject> objects;
        int selected = -1;
    };

    // UI
    void drawMenuBar();
    void drawToolbar();
    void drawOutliner();
    void drawInspector(const std::string& gpuName, const RenderStats& stats);
    void drawStatusBar(const RenderStats& stats);
    void drawHelp();
    void drawAddPopup();
    void drawViewportOverlay();
    void track();   // call after an editing widget: records an undo step when the edit starts

    // Viewport interaction
    void handleNavigation(float dt);
    void handleGizmo();
    void handlePicking();
    void handleShortcuts();
    void setCameraFromView(const Mat4& view);
    float aspect() const { return vh_ > 0.f ? vw_ / vh_ : 1.f; }

    // Operations
    void select(int index);
    bool hasSelection() const { return selected_ >= 0 && selected_ < (int)objects_.size(); }
    void addObject(MeshKind kind);
    void duplicateSelected();
    void deleteSelected();
    void toggleHideSelected();
    void unhideAll();
    void focusSelected();
    void dropSelectedToGround();
    void saveToFile();
    void loadFromFile();
    std::string uniqueName(const std::string& wanted) const;
    void setStatus(const std::string& msg);

    // Undo
    void pushUndo();
    void undo();
    void redo();

    const MeshLibrary& meshes_;
    std::vector<SceneObject> objects_;
    int selected_ = -1;
    EditorCamera cam_;

    Tool tool_ = Tool::Move;
    Nav nav_ = Nav::None;
    bool localSpace_ = false;
    bool snap_ = false;
    float snapMove_ = 0.5f, snapRotate_ = 15.f, snapScale_ = 0.1f;
    bool playing_ = true;
    float animTime_ = 0.f;

    std::vector<Snapshot> undo_, redo_;
    Snapshot frameStart_;
    bool editBegan_ = false;
    bool gizmoWasUsing_ = false;
    bool gizmoDrawn_ = false;

    bool showOutliner_ = true, showInspector_ = true, showHelp_ = false;
    bool openAddMenu_ = false, scrollToSelected_ = false;
    int pendingDelete_ = -1, pendingDuplicate_ = -1, pendingFocus_ = -1;
    ImGuiTextFilter filter_;
    char scenePath_[256] = "scene.vkw";
    std::string status_;
    float statusTimer_ = 0.f;
    float fps_ = 0.f;
    bool quit_ = false;

    // Layout (window pixels), recomputed every frame
    float vx_ = 0, vy_ = 0, vw_ = 1, vh_ = 1;
    float topH_ = 0, statusH_ = 0;
};
