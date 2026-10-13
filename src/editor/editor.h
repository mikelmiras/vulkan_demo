// Scene editor: camera navigation, picking, transform gizmos, undo/redo, play mode and the ImGui panels.
//   editor.cpp           frame loop, view, play mode, operations, undo
//   editor_panels.cpp    menu bar, toolbar, status bar, dialogs
//   editor_outliner.cpp  entity tree, prefabs
//   editor_inspector.cpp components, behaviours, scene look
//   editor_viewport.cpp  navigation, gizmos, picking, shortcuts, overlays
#pragma once

#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include "imgui.h"
#include "assets/meshes.h"
#include "app/export.h"
#include "core/project.h"
#include "app/updates.h"
#include "editor/editor_prefs.h"
#include "game/game.h"
#include "game/module.h"
#include "render/renderer.h"
#include "scene/scene.h"
#include "scene/scene_diff.h"
#include "scene/terrain.h"
#include "net/net_session.h"

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
    // persistent: keep the panel layout and preferences between sessions (off for screenshots: always the defaults).
    Editor(MeshLibrary& meshes, TextureLibrary& textures, AudioEngine* audio, float uiScale, bool persistent = false);
    ~Editor();

    // Builds the UI and handles viewport input. Call between ImGui::NewFrame() and ImGui::Render().
    void frame(float dt, const std::string& gpuName, const RenderStats& stats);
    SceneRenderInfo renderInfo() const;   // valid after frame()
    // Power (app/application.h's pacing): draw only when something changes unless the viewport is realtime or the
    // game runs; busy(): something animates by itself now (the game, a load, particles, a clip preview, a session)
    void pacing(bool& lazy, float& maxFps, float& unfocusedFps) const;
    bool busy() const;
    void setIdle(bool idle) { idle_ = idle; }   // (this frame is the sleeping editor's heartbeat: no FPS to show)
    bool quitRequested() const { return quit_; }
    void openScene(const std::string& path);   // like File > Load
    // Makes `dir` the current project (creating its project.json if it has none) and opens its start scene
    void openProject(const std::filesystem::path& dir);
    // The project launcher (editor_project.cpp): over the whole window until a project is chosen (or the demo);
    // File > Project launcher. Project Settings: File > Project settings.
    void showLauncher();
    void showProjectSettings();
    void showBuildSettings();
    void play() { startPlay(); }
    void newStressScene();   // the demo plus thousands of objects: for measuring performance
    void setUiTextures(GameUI::TextureLookup lookup) {
        uiTextures_ = lookup;
        gameUi_.setTextureLookup(std::move(lookup));
    }
    void setGameView(bool shown) { showGameView_ = shown; }   // View > Game view
    bool selectByName(const std::string& names);   // "A" or "A,B,C" (A is the primary)
    void showStatus(const std::string& msg) { setStatus(msg); }   // (the status bar, for a while)
    // The renderer drawing the editor (for View > Anti-aliasing); the saved choice is applied to it
    void setRenderer(Renderer* renderer);

private:
    enum class Tool { Select, Move, Rotate, Scale, Terrain };
    enum class Nav { None, Fly, Orbit, Pan };
    // A file operation in the asset browser, undoable: a move (or rename) `from` -> `to`, or a delete of `from`
    // (`kept`: the copy kept to put back, in this session's trash folder)
    struct FileOp {
        std::filesystem::path from, to, kept;
        bool any() const { return !from.empty(); }
    };
    // An undo (or redo) step: how to get the scene back to how it was, and what was selected then; or a file
    // operation to reverse
    struct UndoStep {
        SceneDelta delta;
        EntityId selected = kNoEntity;
        FileOp file;
    };
    static constexpr size_t kMaxUndo = 200;

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
    void drawAbout();   // Help > About and Check for updates
    bool showAbout_ = false;
    bool exportPackage_ = false;   // File > Export game > And make an AppImage / .dmg / installer
    bool exportUpdatePack_ = false;   // ... > And make an update pack
    updates::Check updateCheck_;
    void drawAddPopup();
    void drawAddMenuItems();
    void drawImportPopup();
    void drawInputBindings();
    // ---- Project and assets (editor_assets.cpp)
    enum class AssetKind { Folder, Scene, Prefab, Model, Texture, Sound, Script, Other };
    struct AssetEntry {
        std::string name;
        std::filesystem::path path;   // absolute
        AssetKind kind = AssetKind::Other;
    };
    static AssetKind assetKind(const std::filesystem::path& path);
    void drawAssetBrowser();
    void scanAssets();
    void openAsset(const AssetEntry& asset);
    void handleViewportDrop();          // assets dragged from the browser onto the viewport
    bool acceptAssetDrop(AssetKind kind, std::filesystem::path& out);   // after an Inspector widget: dropped on it?
    void assignTexture(EntityId id, const std::filesystem::path& file);
    void drawAnimatorInspector(Entity& e);   // editor_animation.cpp
    void drawAnimatorGraph();
    void drawTagsAndLayer(Entity& e);
    void drawLayerSettings();
    void pickReference(const char* label, std::string& value, FieldRef kind);   // a behaviour's FieldRef field
    void track();   // call after an editing widget: records an undo step when the edit starts

    // ---- Viewport (editor_viewport.cpp)
    void handleNavigation(float dt);
    void handleGizmo();
    void handlePicking();
    EntityId pickAt(ImVec2 mouse) const;                 // what's under the mouse in the viewport, or kNoEntity
    void mouseRay(ImVec2 mouse, Vec3& origin, Vec3& dir) const;
    void handleShortcuts();
    void drawViewportOverlay();
    void drawEntityIcons();   // cameras and empties have no mesh: draw something to see and click
    void drawNavigation();    // the walkable grid's links near the camera (Bake and show, in the scene settings)
    void bakeNavigationPreview();
    void drawColliders();     // wireframes of all colliders (View > Show colliders) or just the selected one
    void setCameraFromView(const Mat4& view);
    bool worldToScreen(Vec3 p, ImVec2& out) const;   // through the current view; false if behind the camera
    bool viewportHovered() const;

    // ---- Frame / view (editor.cpp)
    void updateView();   // picks the editor or game camera for this frame
    void buildDrawItems();
    float aspect() const { return vh_ > 0.f ? vw_ / vh_ : 1.f; }
    float px(float v) const { return v * uiScale_; }

public:
    float uiScale() const { return uiScale_; }
    void setUiScale(float scale) { uiScale_ = scale; }   // (the display's changed: Application's uiScale())

private:
    // World matrices of what's on screen: the animated play pose while playing, else the edit pose
    const std::vector<Mat4>& worlds() const { return worlds_; }
    const Mat4& worldOf(EntityId id) const;

    // ---- Play mode: the scene is copied on Play and restored on Stop
    bool playing() const { return game_.running(); }
    bool viewingGameCamera() const { return playing() && gameCameraActive_ && !gameViewShown(); }
    // The Game view (View > Game view): while playing, the game's camera in a window of its own (docked anywhere),
    // the main viewport staying with the editor's camera
    bool gameViewShown() const { return playing() && showGameView_ && gw_ >= 1.f && gh_ >= 1.f; }
    void drawGameView();
    void startPlay();
    void stopPlay();
    void hostGame();
    void togglePause();

    // ---- Operations
    Entity* selection() { return scene_.find(selected_); }
    // Multi-selection: the primary (selected_: the inspector's, the gizmo's pivot) and others (Ctrl / Shift click)
    std::vector<EntityId> selectionIds() const;          // primary first
    std::vector<EntityId> topLevelSelection() const;     // without those under another selected entity
    bool isSelected(EntityId id) const;
    bool selectionHasEmitter() const;   // (a selected entity, or one under it, has a ParticleEmitter)
    void toggleSelect(EntityId id);
    void selectRange(EntityId id);                        // the primary to `id`, in the outliner's order
    void selectAllRoots();
    void makePrefabVariant(int prefabIndex);
    // Autosave: unsaved edits are written to the user's data folder every minute, offered back when the scene opens
    uint64_t editVersion_ = 0, savedVersion_ = 0, autosavedVersion_ = 0;   // bumped by every edit (undo step)
    float autosaveTimer_ = 0.f;
    bool recoverable_ = false;   // an autosave newer than the open scene's file exists
    std::filesystem::path autosavePath() const;
    void autosave();
    void recoverAutosave();
    bool unsaved() const { return editVersion_ != savedVersion_; }
    // Layout: the panels dock around the viewport (the dockspace's central node); Dear ImGui keeps where they are in
    // editor_layout.ini, the rest of the editor's settings are in editor_prefs.json (editor_prefs.h)
    void drawDockspace(bool panels);   // sets vx_ .. vh_ to the central node
    void buildDefaultLayout(ImGuiID dockspace);
    void applyPrefs(const EditorPrefs& p);
    EditorPrefs currentPrefs() const;
    void savePrefsIfChanged(float dt);
    bool persistent_ = false;     // reads and writes the layout and preferences (not in screenshot runs)
    bool resetLayout_ = false;    // View > Reset layout: rebuilt next frame
    std::string layoutIni_;       // (io.IniFilename points into it)
    std::filesystem::path prefsFile_;
    EditorPrefs savedPrefs_;      // as last written
    float prefsTimer_ = 0.f;
    std::vector<std::string> recentProjects_;   // File > Recent projects (newest first)
    bool showLauncher_ = false, showProjectSettings_ = false;
    std::vector<project::Info> launcherInfo_;   // (the recent projects, read when the launcher opens)
    std::string newProjectName_;
    int newProjectTemplate_ = 1;
    project::Settings projectDraft_;            // (Project Settings: the edits until saved)
    void drawLauncher();
    void drawProjectSettings();
    bool showBuildSettings_ = false;
    int buildSelected_ = 0;
    void drawBuildSettings();
    Renderer* renderer_ = nullptr;
    int antialiasing_ = -1;   // the one picked in View > Anti-aliasing (Renderer::Antialiasing; -1: the default)
    void select(EntityId id);
    void addObject(int mesh);
    void addEmpty();
    void addCamera();
    void addLight(Light::Type type);
    // .obj -> one object, .gltf / .glb -> a prefab + copy; placed at `at` if given, else in front of the camera.
    // Errors go to importError_.
    void importModel(const std::string& path, const Vec3* at = nullptr);
    // Prefabs (index into scene_.prefabs)
    void createPrefabFromSelection();
    void instantiatePrefab(int index);
    void updatePrefabFromSelection(int index);
    void deletePrefab(int index);
    void exportPrefab(int index);
    void importPrefab();                                  // asks for a .prefab file
    void importPrefabFile(const std::filesystem::path& path, const Vec3* placeAt = nullptr);
    void duplicateSelected();
    void deleteSelected();
    void reparent(EntityId id, EntityId newParent);
    void toggleHideSelected();
    void unhideAll();
    void focusSelected();
    void dropSelectedToGround();
    void newScene(bool demo);
    void exportGameTo(const std::filesystem::path& dir, const ExportOptions& options);
    void saveToFile();
    void loadFromFile();
    void setStatus(const std::string& msg);
    Vec3 spawnPoint(float lift) const;   // where new objects go: on the ground in front of the camera

    // ---- Game module (the project's own behaviours, game/module.h)
    void loadGameModule();      // (re)loads project_.module, keeping every scene's behaviours across the swap
    void rebuildGameModule();   // runs project_.moduleBuild in the background, then reloads
    void pollGameModule();      // once per frame: a finished build, or a library rebuilt outside the editor
    // ---- Game UI layouts (game/ui_layout.h, editor_ui_layout.cpp)
    void addUIElement(UIElement::Kind kind);
    void drawUIInspector(Entity& e);
    bool drawGameUIPreview();   // true when it took the mouse
    bool showGameUi_ = true, uiDragging_ = false;
    StringTable previewStrings_;       // the project's texts, for the preview
    double previewStringsAt_ = -1.0;
    // ---- Terrains (scene/terrain.h, editor_terrain.cpp)
    void addTerrain();
    void drawTerrainInspector(Entity& e);
    void drawBrainInspector(Entity& e);   // (editor_ai.cpp)
    bool handleTerrainTool(float dt);   // true while the brush has the mouse
    terrain::BrushSettings brush_;
    std::optional<std::pair<size_t, Entity>> strokeBefore_;   // the terrain at the start of a brush stroke (undo)
    // ---- Asset database (assets/asset_db.h, editor_asset_db.cpp): moves and renames keep references working
    std::filesystem::path selectedAsset_;      // shown in the Inspector instead of the scene (cleared by picking an entity)
    std::filesystem::path settingsFor_;        // whose import settings are being edited
    json::Value editedSettings_;
    std::filesystem::path renaming_, deleting_;   // the asset a Rename / Delete dialog is open for
    std::string renameBuffer_;
    bool openRename_ = false, openDelete_ = false;
    std::pair<std::filesystem::path, std::filesystem::path> pendingMove_;   // an asset dropped on a folder
    void drawAssetInspector();
    void drawAssetDialogs();
    void applyImportSettings();
    // A file or a folder; `undoable`: a step on the undo stack (not when undoing or redoing one)
    bool moveAsset(const std::filesystem::path& from, const std::filesystem::path& to, bool undoable = true);
    bool deleteAsset(const std::filesystem::path& path, bool undoable = true);
    bool reverseFileOp(FileOp& op, bool redo);   // undo (or redo) one
    void pushFileOp(const FileOp& op);
    std::filesystem::path trashDir_;   // deleted files kept for undo (emptied when the editor closes)
    std::filesystem::path lastKept_;   // (deleteAsset's copy, when not pushed as a step)
    std::vector<EntityId> assetUsers(const std::filesystem::path& file);   // entities of the scene using it
    void retargetInMemory(const std::filesystem::path& from, const std::filesystem::path& to);
    int renamePrefabReferences(const std::string& from, const std::string& to);   // behaviour fields naming a prefab
    void renameScriptType(const std::string& from, const std::string& to);       // a script file renamed
    // ---- Lua scripts (script/lua_script.h): the project's scripts/ folder, reloaded when a file changes
    void loadScripts();         // (on opening a project, before its scenes)
    void pollScripts();         // once per frame
    void newScript();           // scripts/NewScriptN.lua from a template
    size_t scriptErrorsShown_ = 0;
    template <class F> void forEachHeldScene(F&& f);   // the scene, the play copy, undo/redo snapshots

    // ---- Undo: steps are diffs (scene/scene_diff.h), so they stay small in big scenes. Operations call pushUndo()
    // before changing anything (a copy of the scene, diffed at the end of the frame); widget edits are caught by
    // track() against what the selection, the scene's look and its input bindings were at the start of the frame.
    void pushUndo();
    void commitUndo();   // end of frame: this frame's edits become an undo step
    void undo();
    void redo();

    // First member, so it's destroyed last: the scenes below hold behaviours whose code may live in it
    GameModule module_;
    MeshLibrary& meshes_;
    TextureLibrary& textures_;
    AudioEngine* audio_ = nullptr;   // may be null or not ready (no sound)
    float uiScale_ = 1.f;
    Scene scene_;
    EntityId selected_ = kNoEntity;
    std::vector<EntityId> extraSelected_;
    std::vector<std::pair<size_t, Entity>> frameGroup_;   // the other selected entities at the frame's start (undo)
    EditorCamera cam_;

    Tool tool_ = Tool::Move;
    Nav nav_ = Nav::None;
    bool localSpace_ = false;
    bool snap_ = false;
    float snapMove_ = 0.5f, snapRotate_ = 15.f, snapScale_ = 0.1f;

    Game game_;
    NetSession net_;                // (after game_: stopped before it goes) hosting while playing
    bool hostMultiplayer_ = false;
    GameUI gameUi_;                 // the game's own interface, drawn over the viewport while playing
    bool uiWantsCursor_ = false;    // last frame's game UI had a menu up: keep the mouse pointer free
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
    float viewFov_ = 1.f;
    float viewNear_ = 0.1f, viewFar_ = 2000.f;   // the clip planes of the view drawn
    std::vector<LightInfo> lights_;
    std::vector<PlantSet> plants_;
    std::vector<ProbeInfo> probes_;
    uint32_t probeBakes_ = 0;   // bumped by Bake: every reflection probe is baked again
    void addReflectionProbe();
    std::vector<Mat4> worlds_;
    std::vector<DrawItem> drawItems_;
    std::vector<Mat4> joints_;   // skinned meshes' joint matrices (see DrawItem::jointOffset)
    std::vector<ParticleDraw> particles_;
    ParticleSystem previewParticles_;   // emitters outside Play (View > Preview particles)
    bool previewParticles_on_ = true;
    bool realtimeViewport_ = false, throttleInBackground_ = true;   // (View > Power)
    int frameCap_ = 60;
    NavGrid navPreview_;
    EntityId animGraphEntity_ = kNoEntity;   // whose state machine the graph window shows
    std::string newTag_;
    std::vector<Vec3> navLinks_;   // navPreview_.links(), made when first drawn
    bool showNavigation_ = false;
    // Clip preview outside Play: the entity's clip drawn at `time` (the scene itself isn't changed)
    struct AnimationPreview {
        EntityId entity = kNoEntity;
        float time = 0.f;
        bool playing = false;
    } animPreview_;
    int eventDrag_ = -1;   // (the Animator's event track: the event being dragged)
    // Animator > Retarget a clip: the bone map being edited (for the clip, model and entity in retargetKey_)
    std::string retargetKey_;
    std::vector<std::pair<std::string, std::string>> retargetMap_;
    bool retargetMatchRest_ = false;
    float frameDt_ = 0.f;

    std::vector<UndoStep> undo_, redo_;
    std::optional<Scene> undoBefore_;     // pushUndo()'s copy, until the end of the frame
    EntityId undoBeforeSelected_ = kNoEntity;
    // The start of this frame, for widget edits: the selected entity (and its index), the look, the bindings
    std::optional<std::pair<size_t, Entity>> frameEntity_;
    RenderSettings frameRender_;
    NavigationSettings frameNav_;
    LayerSettings frameLayers_;
    std::vector<InputAction> frameInput_;
    EntityId frameSelected_ = kNoEntity;
    bool editBegan_ = false;
    bool gizmoWasUsing_ = false;
    bool gizmoDrawn_ = false;

    bool showOutliner_ = true, showInspector_ = true, showHelp_ = false, showColliders_ = false, showProfiler_ = false;
    bool showContacts_ = false;   // View > Show contacts (while playing)
    bool showGameView_ = false;
    float gx_ = 0, gy_ = 0, gw_ = 0, gh_ = 0;   // the Game view's rectangle (last frame's)
    bool gameViewHovered_ = false;
    mutable std::vector<SceneRenderInfo> gameViews_;   // (what renderInfo() points the renderer's extra views at)
    bool gameUiCaptured_ = false;                      // (this frame: the mouse is the game's)
    bool openAddMenu_ = false, scrollToSelected_ = false;
    EntityId pendingDelete_ = kNoEntity, pendingDuplicate_ = kNoEntity, pendingFocus_ = kNoEntity;
    EntityId pendingReparent_ = kNoEntity, pendingParent_ = kNoEntity;   // drag & drop in the outliner
    EntityId pendingPrefab_ = kNoEntity;
    ImGuiTextFilter filter_;
    int selectedPrefab_ = -1;
    bool showPrefabsTab_ = false;   // switch the outliner to the Prefabs tab next frame
    char scenePath_[256] = "scene.vkw";   // relative to the project (or absolute)
    project::Settings project_;
    bool showAssets_ = true;
    std::filesystem::path assetDir_;      // the folder the browser shows (absolute)
    std::vector<AssetEntry> assets_;
    GameUI::TextureLookup uiTextures_;
    std::unordered_map<std::string, int> thumbnails_;   // image path -> its thumbnail in the texture library (-1: failed)
    ImTextureID thumbnail(const AssetEntry& a, int& budget, float& aspect);   // aspect: width / height
    double assetsScannedAt_ = -1.0;
    bool openImport_ = false;
    std::string importPath_, importError_;
    std::future<int> moduleBuild_;        // the running build command's exit code
    double moduleCheckedAt_ = 0.0;
    bool moduleChangedWhilePlaying_ = false;
    std::string status_;
    float statusTimer_ = 0.f;
    float fps_ = 0.f;
    bool idle_ = false;
    bool quit_ = false;

    // Layout (window pixels), recomputed every frame
    float vx_ = 0, vy_ = 0, vw_ = 1, vh_ = 1;
    float topH_ = 0, statusH_ = 0;
};

// (in the header: editor_assets.cpp and editor_asset_db.cpp use it)
template <class F>
void Editor::forEachHeldScene(F&& f) {
    f(scene_);
    f(editScene_);
    if (undoBefore_) f(*undoBefore_);
    // Undo steps and the frame's copy of the selection hold entities (and prefabs) too: lend them to a scene
    auto lend = [&](std::vector<std::pair<size_t, Entity>>& entities, std::optional<std::vector<Prefab>>* prefabs) {
        Scene s;
        for (auto& item : entities) s.entities.push_back(std::move(item.second));
        if (prefabs && *prefabs) s.prefabs = std::move(**prefabs);
        f(s);
        for (size_t i = 0; i < entities.size(); ++i) entities[i].second = std::move(s.entities[i]);
        if (prefabs && *prefabs) **prefabs = std::move(s.prefabs);
    };
    for (std::vector<UndoStep>* steps : {&undo_, &redo_})
        for (UndoStep& step : *steps) lend(step.delta.entities, &step.delta.prefabs);
    if (frameEntity_) {
        std::vector<std::pair<size_t, Entity>> one{std::move(*frameEntity_)};
        lend(one, nullptr);
        frameEntity_ = std::move(one[0]);
    }
}
