// Scene editor: camera navigation, picking, transform gizmos, undo/redo and the ImGui panels.
#pragma once

#include <string>
#include <vector>

#include "imgui.h"
#include "assets/meshes.h"
#include "game/game.h"
#include "render/renderer.h"
#include "scene/scene.h"

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
    // uiScale: DPI scale for fixed pixel sizes (panel widths etc.), 1.0 at 96 DPI
    Editor(MeshLibrary& meshes, float uiScale);   // meshes: imports are appended to it

    // Builds the UI and handles viewport input. Call between ImGui::NewFrame() and ImGui::Render().
    void frame(float dt, const std::string& gpuName, const RenderStats& stats);
    SceneRenderInfo renderInfo() const;   // valid after frame()
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
    void drawAddMenuItems();
    void drawImportPopup();
    void drawViewportOverlay();
    void track();   // call after an editing widget: records an undo step when the edit starts

    // Viewport interaction
    void handleNavigation(float dt);
    void handleGizmo();
    void handlePicking();
    void handleShortcuts();
    void setCameraFromView(const Mat4& view);
    float aspect() const { return vh_ > 0.f ? vw_ / vh_ : 1.f; }
    float px(float v) const { return v * uiScale_; }
    bool worldToScreen(Vec3 p, ImVec2& out) const;   // through the current view; false if behind the camera
    bool viewportHovered() const;
    void updateView();        // picks the editor or game camera for this frame
    void buildDrawItems();
    void drawCameraGizmos();

    // Play mode: the scene is copied on Play and restored on Stop
    bool playing() const { return game_.running(); }
    bool viewingGameCamera() const { return playing() && gameCameraActive_; }
    float sceneTime() const { return playing() ? game_.time() : 0.f; }
    void startPlay();
    void stopPlay();
    void togglePause();

    // Operations
    void select(int index);
    bool hasSelection() const { return selected_ >= 0 && selected_ < (int)objects_.size(); }
    void addObject(int mesh);
    void addCamera();
    void importMesh();
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

    MeshLibrary& meshes_;
    float uiScale_ = 1.f;
    std::vector<SceneObject> objects_;
    int selected_ = -1;
    EditorCamera cam_;

    Tool tool_ = Tool::Move;
    Nav nav_ = Nav::None;
    bool localSpace_ = false;
    bool snap_ = false;
    float snapMove_ = 0.5f, snapRotate_ = 15.f, snapScale_ = 0.1f;
    Game game_;
    std::vector<SceneObject> editObjects_;   // the scene as it was when Play was pressed
    int editSelected_ = -1;
    bool maximizeOnPlay_ = false;
    bool gameCameraActive_ = false;

    // Current view (editor or game camera) and what to draw, rebuilt every frame
    Mat4 viewProj_;
    Vec3 viewPos_;
    std::vector<DrawItem> drawItems_;

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
    bool openImport_ = false;
    std::string importPath_, importError_;
    std::string status_;
    float statusTimer_ = 0.f;
    float fps_ = 0.f;
    bool quit_ = false;

    // Layout (window pixels), recomputed every frame
    float vx_ = 0, vy_ = 0, vw_ = 1, vh_ = 1;
    float topH_ = 0, statusH_ = 0;
};
