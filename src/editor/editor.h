// Scene editor: camera navigation, picking, transform gizmos, undo/redo, play mode and the ImGui panels.
//   editor.cpp           frame loop, view, play mode, operations, undo
//   editor_panels.cpp    menu bar, toolbar, outliner, inspector, status bar, dialogs
//   editor_viewport.cpp  navigation, gizmos, picking, shortcuts, overlays
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
    // meshes: imports are appended to it. uiScale: DPI scale for fixed pixel sizes (1.0 at 96 DPI).
    Editor(MeshLibrary& meshes, TextureLibrary& textures, float uiScale);

    // Builds the UI and handles viewport input. Call between ImGui::NewFrame() and ImGui::Render().
    void frame(float dt, const std::string& gpuName, const RenderStats& stats);
    SceneRenderInfo renderInfo() const;   // valid after frame()
    bool quitRequested() const { return quit_; }
    void openScene(const std::string& path);   // like File > Load
    void play() { startPlay(); }
    bool selectByName(const std::string& name);

private:
    enum class Tool { Select, Move, Rotate, Scale };
    enum class Nav { None, Fly, Orbit, Pan };
    struct Snapshot {
        Scene scene;
        EntityId selected = kNoEntity;
    };

    // ---- Panels (editor_panels.cpp)
    void drawMenuBar();
    void drawToolbar();
    void drawOutliner();
    void drawPrefabs();
    void drawOutlinerNode(EntityId id, const std::vector<std::vector<EntityId>>& children,
                          const std::vector<int>& indexOf);
    void drawInspector(const std::string& gpuName, const RenderStats& stats);
    void drawComponents(Entity& e);
    void drawStatusBar(const RenderStats& stats);
    void drawHelp();
    void drawAddPopup();
    void drawAddMenuItems();
    void drawImportPopup();
    void drawInputBindings();
    void track();   // call after an editing widget: records an undo step when the edit starts

    // ---- Viewport (editor_viewport.cpp)
    void handleNavigation(float dt);
    void handleGizmo();
    void handlePicking();
    void handleShortcuts();
    void drawViewportOverlay();
    void drawEntityIcons();   // cameras and empties have no mesh: draw something to see and click
    void drawColliders();     // wireframes of all colliders (View > Show colliders) or just the selected one
    void setCameraFromView(const Mat4& view);
    bool worldToScreen(Vec3 p, ImVec2& out) const;   // through the current view; false if behind the camera
    bool viewportHovered() const;

    // ---- Frame / view (editor.cpp)
    void updateView();   // picks the editor or game camera for this frame
    void buildDrawItems();
    float aspect() const { return vh_ > 0.f ? vw_ / vh_ : 1.f; }
    float px(float v) const { return v * uiScale_; }
    // World matrices of what's on screen: the animated play pose while playing, else the edit pose
    const std::vector<Mat4>& worlds() const { return worlds_; }
    const Mat4& worldOf(EntityId id) const;

    // ---- Play mode: the scene is copied on Play and restored on Stop
    bool playing() const { return game_.running(); }
    bool viewingGameCamera() const { return playing() && gameCameraActive_; }
    void startPlay();
    void stopPlay();
    void togglePause();

    // ---- Operations
    Entity* selection() { return scene_.find(selected_); }
    void select(EntityId id);
    void addObject(int mesh);
    void addEmpty();
    void addCamera();
    void addLight(Light::Type type);
    void importModel();   // the path in the import dialog: .obj -> one object, .gltf / .glb -> a prefab + copy
    // Prefabs (index into scene_.prefabs)
    void createPrefabFromSelection();
    void instantiatePrefab(int index);
    void updatePrefabFromSelection(int index);
    void deletePrefab(int index);
    void exportPrefab(int index);
    void importPrefab();
    void duplicateSelected();
    void deleteSelected();
    void reparent(EntityId id, EntityId newParent);
    void toggleHideSelected();
    void unhideAll();
    void focusSelected();
    void dropSelectedToGround();
    void newScene(bool demo);
    void saveToFile();
    void loadFromFile();
    void setStatus(const std::string& msg);
    Vec3 spawnPoint(float lift) const;   // where new objects go: on the ground in front of the camera

    // ---- Undo (whole-scene snapshots; the scene is small)
    void pushUndo();
    void undo();
    void redo();

    MeshLibrary& meshes_;
    TextureLibrary& textures_;
    float uiScale_ = 1.f;
    Scene scene_;
    EntityId selected_ = kNoEntity;
    EditorCamera cam_;

    Tool tool_ = Tool::Move;
    Nav nav_ = Nav::None;
    bool localSpace_ = false;
    bool snap_ = false;
    float snapMove_ = 0.5f, snapRotate_ = 15.f, snapScale_ = 0.1f;

    Game game_;
    Scene editScene_;   // the scene as it was when Play was pressed
    EntityId editSelected_ = kNoEntity;
    bool maximizeOnPlay_ = false;
    bool captureOnClick_ = true;      // clicking the game view captures the mouse (Esc releases it)
    bool showInputBindings_ = false;
    int bindingCapture_ = -1;         // action waiting for a key press in the bindings window
    bool keyTakenByBindings_ = false; // that key press mustn't also trigger a shortcut
    bool gameCameraActive_ = false;

    // Current view (editor or game camera) and what to draw, rebuilt every frame
    Mat4 viewProj_;
    Vec3 viewPos_, viewForward_;
    std::vector<LightInfo> lights_;
    std::vector<Mat4> worlds_;
    std::vector<DrawItem> drawItems_;

    std::vector<Snapshot> undo_, redo_;
    Snapshot frameStart_;
    bool editBegan_ = false;
    bool gizmoWasUsing_ = false;
    bool gizmoDrawn_ = false;

    bool showOutliner_ = true, showInspector_ = true, showHelp_ = false, showColliders_ = false;
    bool openAddMenu_ = false, scrollToSelected_ = false;
    EntityId pendingDelete_ = kNoEntity, pendingDuplicate_ = kNoEntity, pendingFocus_ = kNoEntity;
    EntityId pendingReparent_ = kNoEntity, pendingParent_ = kNoEntity;   // drag & drop in the outliner
    EntityId pendingPrefab_ = kNoEntity;
    ImGuiTextFilter filter_;
    int selectedPrefab_ = -1;
    bool showPrefabsTab_ = false;   // switch the outliner to the Prefabs tab next frame
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
