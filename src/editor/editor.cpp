#include "editor/editor.h"
#include "script/lua_debugger.h"
#include "physics/fracture.h"
#include "scene/water.h"
#include "physics/soft_body.h"
#include "scene/stress_scene.h"

#include <cctype>
#include <cfloat>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "ImGuizmo.h"
#include "render/environment.h"
#include "assets/asset_db.h"
#include "imgui_internal.h"   // (DockBuilder: the default layout)
#include "app/profiler_view.h"
#include "core/log.h"
#include "core/profiler.h"
#include "game/animation.h"
#include "render/scene_draw.h"
#include "scene/prefab_sync.h"
#include "platform/platform.h"
#include "scene/serialization.h"

Editor::Editor(MeshLibrary& meshes, TextureLibrary& textures, AudioEngine* audio, float uiScale, bool persistent)
    : meshes_(meshes), textures_(textures), audio_(audio), uiScale_(uiScale), scene_(buildDefaultScene()) {
    game_.setAudio(audio);
    if (persistent)   // (a real editor: screenshot runs and tests don't rumble pads)
        game_.setGamepadOutput({[](int pad, float low, float high, float seconds) { platform::gamepadRumble(pad, low, high, seconds); },
                           [](Vec3 c) { platform::setGamepadLight(c.x, c.y, c.z); }});
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    persistent_ = persistent;
    if (persistent_) {
        prefsFile_ = editor_prefs::folder() / "editor_prefs.json";
        EditorPrefs p;
        if (editor_prefs::load(prefsFile_, p)) applyPrefs(p);
        savedPrefs_ = currentPrefs();
        layoutIni_ = (editor_prefs::folder() / "editor_layout.ini").u8string();
        std::error_code ec;
        std::filesystem::create_directories(editor_prefs::folder(), ec);
        ImGui::GetIO().IniFilename = layoutIni_.c_str();
        ImGui::LoadIniSettingsFromDisk(layoutIni_.c_str());
    }
    assetDir_ = project::root();
    std::string error;
    if (project::exists(project::root())) project::load(project::root(), project_, error);
    else project_.name = project::root().filename().u8string();
    assetdb::open(project::root(), true);
    if (!project_.module.empty()) loadGameModule();
    loadScripts();
}

Editor::~Editor() {
    if (!trashDir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(trashDir_, ec);
    }
    if (!persistent_) return;
    if (currentPrefs() != savedPrefs_) editor_prefs::save(prefsFile_, currentPrefs());
    if (ImGui::GetCurrentContext() && ImGui::GetIO().IniFilename == layoutIni_.c_str()) {
        ImGui::SaveIniSettingsToDisk(layoutIni_.c_str());
        ImGui::GetIO().IniFilename = nullptr;   // (the string goes with us)
    }
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------
void Editor::frame(float dt, const std::string& gpuName, const RenderStats& stats) {
    PROFILE_SCOPE("Editor");
    ImGuiIO& io = ImGui::GetIO();
    autosaveTimer_ += dt;
    if (autosaveTimer_ >= 60.f) {   // unsaved edits, not yet autosaved: write them where a crash won't lose them
        autosaveTimer_ = 0.f;
        if (!playing() && unsaved() && editVersion_ != autosavedVersion_) autosave();
    }
    // Mouse capture in play mode: a click on the game view takes the mouse, Esc gives it back
    if (platform::cursorCaptured() &&
        (!viewingGameCamera() || uiWantsCursor_ || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        platform::setCursorCaptured(false);
    else if (captureOnClick_ && viewingGameCamera() && !uiWantsCursor_ && !platform::cursorCaptured() &&
             viewportHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        platform::setCursorCaptured(true);
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) platform::setFullscreen(!platform::isFullscreen());

    // The game sees the keyboard unless a text field has it, and the mouse only over the viewport (or captured)
    bool captured = platform::cursorCaptured();
    float mdx = 0.f, mdy = 0.f;
    platform::takeMouseDelta(mdx, mdy);
    game_.input().setMouseCaptured(captured);
    game_.setRecordContacts(showContacts_);
    scripting::debugger::poll();   // (an attached editor's requests: breakpoints set while the game runs)
    bool gameMouse = gameViewShown() ? gameViewHovered_ : viewportHovered();   // (over whichever shows the game)
    if (persistent_) game_.setGamepads(platform::gamepads());   // (local players: a pad each; not in screenshot runs)
    game_.frame(dt, !io.WantTextInput, captured || gameMouse, captured ? ImVec2(mdx, mdy) : io.MouseDelta);
    net_.update(dt);
    uiWantsCursor_ = false;
    gameUiCaptured_ = captured;
    if (playing() && !gameViewShown()) {   // the game's HUD and menus, over the viewport (the Game view draws its own)
        gameUi_.begin(ImGui::GetBackgroundDrawList(), vx_, vy_, vw_, vh_, !captured && viewportHovered());
        game_.drawUI(gameUi_);
        gameUi_.end();
        uiWantsCursor_ = gameUi_.wantsCursor();
    }
    keyTakenByBindings_ = false;
    if (audio_ && !playing()) audio_->update();   // (while playing, the game does it)
    pollGameModule();
    pollScripts();
    if (playing() && game_.quitRequested()) stopPlay();   // the game asked to quit: back to editing
    frameSelected_ = selected_;
    frameEntity_.reset();
    if (int i = scene_.indexOf(selected_); i >= 0) frameEntity_.emplace((size_t)i, scene_.entities[(size_t)i]);
    frameGroup_.clear();
    for (EntityId id : extraSelected_)
        if (int i = scene_.indexOf(id); i >= 0) frameGroup_.emplace_back((size_t)i, scene_.entities[(size_t)i]);
    frameRender_ = scene_.render;
    frameNav_ = scene_.navigation;
    frameLayers_ = scene_.layers;
    frameInput_ = scene_.inputActions;
    editBegan_ = false;
    if (statusTimer_ > 0.f) statusTimer_ -= dt;
    fps_ = fps_ * 0.95f + (dt > 0.f ? 1.f / dt : 0.f) * 0.05f;
    frameDt_ = dt;

    // Must come first: creates the full-screen pass-through window the gizmos draw into,
    // so it stays underneath every panel.
    ImGuizmo::BeginFrame();

    // Layout: menu bar + toolbar on top, status bar at the bottom, the panels docked around the viewport between them
    const ImGuiStyle& style = ImGui::GetStyle();
    float menuH = ImGui::GetFrameHeight();
    float toolbarH = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.f;
    topH_ = menuH + toolbarH;
    statusH_ = ImGui::GetTextLineHeight() + style.WindowPadding.y * 2.f;
    bool panels = !(playing() && maximizeOnPlay_) && !showLauncher_;   // (the launcher: nothing else until a choice)
    drawDockspace(panels);
    savePrefsIfChanged(dt);
    updateView();

    drawMenuBar();
    drawToolbar();
    if (panels && showOutliner_) drawOutliner();
    if (panels && showInspector_) drawInspector(gpuName, stats);
    drawStatusBar(stats);
    if (showHelp_) drawHelp();
    drawAbout();
    if (panels && showAssets_) drawAssetBrowser();
    drawAddPopup();
    drawImportPopup();
    if (showInputBindings_) drawInputBindings();
    if (animGraphEntity_ != kNoEntity) drawAnimatorGraph();
    drawGameView();
    if (showProfiler_) drawProfilerWindow(&showProfiler_, project::saveDir("Editor").parent_path() / "traces");
    drawAssetDialogs();
    drawProjectSettings();
    drawBuildSettings();
    drawLauncher();   // (last: over everything)

    handleNavigation(dt);
    bool brushing = handleTerrainTool(dt);
    bool uiTook = !brushing && drawGameUIPreview();   // (the game's UI, over the view while editing)
    if (tool_ != Tool::Terrain && !uiDragging_) handleGizmo();
    if (!brushing && !uiTook) handlePicking();
    handleViewportDrop();
    handleShortcuts();
    drawEntityIcons();
    drawColliders();
    drawNavigation();
    drawViewportOverlay();

    // Deferred actions requested from inside list iteration (context menus, drag & drop)
    if (pendingFocus_) { select(pendingFocus_); focusSelected(); }
    if (pendingDuplicate_) { select(pendingDuplicate_); duplicateSelected(); }
    if (pendingDelete_) { select(pendingDelete_); deleteSelected(); }
    if (pendingReparent_) reparent(pendingReparent_, pendingParent_);
    if (pendingPrefab_) { select(pendingPrefab_); createPrefabFromSelection(); }
    pendingFocus_ = pendingDuplicate_ = pendingDelete_ = pendingReparent_ = pendingParent_ = pendingPrefab_ = kNoEntity;

    commitUndo();
    if (selected_ != kNoEntity && selected_ != frameSelected_) selectedAsset_.clear();   // (an entity picked: it's shown)
    if (!scene_.find(selected_)) selected_ = kNoEntity;
    if (selected_ == kNoEntity) extraSelected_.clear();
    extraSelected_.erase(std::remove_if(extraSelected_.begin(), extraSelected_.end(),
                                        [&](EntityId id) { return !scene_.find(id) || id == selected_; }),
                         extraSelected_.end());
    updateView();   // the scene or the camera may have changed during this frame
    buildDrawItems();
}

void Editor::setRenderer(Renderer* renderer) {
    renderer_ = renderer;
    if (renderer_ && antialiasing_ >= 0) renderer_->setAntialiasing((Renderer::Antialiasing)antialiasing_);
    else if (renderer_ && !std::getenv("VKWORLD_AA") && !project_.antialiasing.empty())   // (none chosen: the project's)
        renderer_->setAntialiasing(project_.antialiasing == "taa" ? Renderer::Antialiasing::Temporal
                                   : project_.antialiasing == "off" ? Renderer::Antialiasing::Off : Renderer::Antialiasing::Msaa);
}

void Editor::updateView() {
    worlds_ = playing() ? game_.worldMatrices() : scene_.worldMatrices();
    CameraView gc;
    gameCameraActive_ = playing() && game_.activeCamera(gc);
    if (gameCameraActive_) {
        viewProj_ = perspective(gc.fovY, aspect(), gc.zNear, gc.zFar, true) * gc.view;
        viewNear_ = gc.zNear;
        viewFar_ = gc.zFar;
        viewFov_ = gc.fovY;
        viewPos_ = gc.position;
        viewForward_ = normalize(transformDir(inverseAffine(gc.view), {0, 0, -1}));
    } else {
        viewProj_ = perspective(cam_.fovY, aspect(), 0.1f, 2000.f, true) * cam_.view();
        viewNear_ = 0.1f;
        viewFar_ = 2000.f;
        viewFov_ = cam_.fovY;
        viewPos_ = cam_.pos;
        viewForward_ = cam_.forward();
    }
}

const Mat4& Editor::worldOf(EntityId id) const {
    static const Mat4 identity;
    int i = scene_.indexOf(id);
    return i >= 0 && i < (int)worlds_.size() ? worlds_[(size_t)i] : identity;
}

void Editor::buildDrawItems() {
    PROFILE_SCOPE("Draw lists");
    terrain::syncMeshes(scene_, meshes_);   // (terrains changed since: their meshes again)
    softbody::syncMeshes(scene_, meshes_);  // (soft bodies: as their particles moved)
    fracture::syncMeshes(scene_, meshes_);  // (pre-fractured: their pieces, ready to break)
    water::syncMeshes(scene_, meshes_);     // (water: its drawn grid)
    const Entity* previewed = playing() ? nullptr : scene_.find(animPreview_.entity);
    const AnimationClip* clip = previewed && previewed->animator ? meshes_.findAnimation(previewed->animator->clip) : nullptr;
    if (clip) {   // pose the clip just for drawing, then put the transforms back
        if (animPreview_.playing && clip->duration > 0.f)
            animPreview_.time = std::fmod(animPreview_.time + std::min(frameDt_, 0.1f) * previewed->animator->speed +
                                              clip->duration, clip->duration);
        std::vector<Transform> saved;
        saved.reserve(scene_.entities.size());
        for (const Entity& o : scene_.entities) saved.push_back(o.transform);
        poseClip(scene_, meshes_, animPreview_.entity, clip->name, animPreview_.time);
        float from = animPreview_.fired, to = animPreview_.time;   // events passed (while posed: at their bones)
        if (from >= 0.f && from != to) {
            bool wrapped = animPreview_.playing && to < from;
            if (wrapped || (to > from && to - from < clip->duration * 0.5f)) firePreviewEvents(*previewed, *clip, from, to, wrapped);
        }
        animPreview_.fired = to;
        drawItems_ = collectDrawItems(scene_, scene_.worldMatrices(), viewingGameCamera() ? kNoEntity : selected_,
                                      &joints_, &meshes_, viewingGameCamera() ? nullptr : &extraSelected_);
        for (size_t i = 0; i < saved.size(); ++i) scene_.entities[i].transform = saved[i];
    } else {
        if (playing()) animPreview_ = {};
        drawItems_ = collectDrawItems(scene_, worlds_, viewingGameCamera() ? kNoEntity : selected_, &joints_, &meshes_,
                                      viewingGameCamera() ? nullptr : &extraSelected_);
    }
    lights_ = collectLights(scene_, worlds_);
    plants_ = collectPlants(scene_, worlds_, meshes_);
    probes_ = collectProbes(scene_, worlds_, probeBakes_);
    particles_.clear();
    if (playing()) {
        previewParticles_.clear();
        game_.particles().collect(particles_);
    } else if (previewParticles_on_) {   // emitters run in the editor too, so their look can be tuned
        // (with the viewport not realtime, only while one is selected: else they'd keep the editor drawing)
        if (realtimeViewport_ || selectionHasEmitter()) previewParticles_.update(scene_, worlds_, std::min(frameDt_, 0.1f));
        previewParticles_.collect(particles_);
    } else {
        previewParticles_.clear();
    }
}

SceneRenderInfo Editor::renderInfo() const {
    SceneRenderInfo info;
    info.items = &drawItems_;
    info.plants = &plants_;
    info.lights = &lights_;
    info.probes = &probes_;
    info.joints = &joints_;
    info.particles = &particles_;
    const RenderSettings& r = scene_.render;
    info.sky = r.sky;
    info.zenith = r.zenith;
    info.ground = r.ground;
    info.fovY = viewFov_;
    info.aspect = aspect();
    info.fogDensity = r.fogDensity;
    info.ambient = r.ambient;
    info.exposure = r.exposure;
    info.bloomStrength = r.bloomStrength;
    info.bloomThreshold = r.bloomThreshold;
    info.aoStrength = r.aoStrength;
    info.aoRadius = r.aoRadius;
    info.gi = r.gi;
    info.giSpacing = r.giSpacing;
    info.giIntensity = r.giIntensity;
    info.reflections = r.reflections;
    info.shadowSoftness = r.shadowSoftness;
    info.volumetricFog = r.volumetricFog;
    info.fogLight = r.fogLight;
    info.fogAnisotropy = r.fogAnisotropy;
    info.fogHeightFalloff = r.fogHeightFalloff;
    info.tonemapper = r.tonemapper;
    info.contrast = r.contrast;
    info.saturation = r.saturation;
    info.vignette = r.vignette;
    // The waves: as the game's physics has them while playing; moving on the clock in the editor
    info.waterTime = playing() ? game_.physics().time() : (float)ImGui::GetTime();
    if (playing()) info.colorFilter = game_.colorFilter();   // (the player's setting, as the game will look)
    if (!r.skyImage.empty()) {   // (loaded and prefiltered once, then cached)
        std::string skyError;
        info.environment = environment::load(project::resolve(r.skyImage), skyError);
    }
    info.skyIntensity = r.skyIntensity;
    info.skyRotation = r.skyRotation * DEG2RAD;
    info.zNear = viewNear_;
    info.zFar = viewFar_;
    info.viewProj = viewProj_;
    info.camPos = viewPos_;
    info.camForward = viewForward_;
    info.viewportX = vx_;
    info.viewportY = vy_;
    info.viewportW = vw_;
    info.viewportH = vh_;
    CameraView gc;
    gameViews_.clear();
    if (gameViewShown() && game_.activeCamera(gc)) {   // the game's camera: drawn into the Game view's image
        SceneRenderInfo g = info;
        g.views = nullptr;
        g.colorFilter = game_.colorFilter();
        g.aspect = gw_ / gh_;
        g.fovY = gc.fovY;
        g.zNear = gc.zNear;
        g.zFar = gc.zFar;
        g.viewProj = perspective(gc.fovY, g.aspect, gc.zNear, gc.zFar, true) * gc.view;
        g.camPos = gc.position;
        g.camForward = normalize(transformDir(inverseAffine(gc.view), {0, 0, -1}));
        g.viewportX = g.viewportY = 0.f;
        g.viewportW = gw_;
        g.viewportH = gh_;
        gameViews_.push_back(g);
        info.views = &gameViews_;
        info.colorFilter = 0;   // (the editor's own view: as it is)
    }
    return info;
}

// The Game view: a window with a see-through background, so the game drawn under it shows through; its rectangle (from the
// last frame) is where the renderer draws the game's camera
void Editor::drawGameView() {
    if (!playing() || !showGameView_) {
        gw_ = gh_ = 0.f;
        gameViewHovered_ = false;
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(480.f, 300.f), ImGuiCond_FirstUseEver);   // (undocked; docked at first: above)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));   // (the view fills it)
    bool open = ImGui::Begin("Game", &showGameView_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (open) {
        ImVec2 at = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
        gx_ = at.x;
        gy_ = at.y;
        gw_ = std::max(size.x, 0.f);
        gh_ = std::max(size.y, 0.f);
        // The game's camera, rendered into an image of its own (renderInfo() asks for it at this size), then the
        // game's HUD and menus over it
        ImTextureID image = renderer_ && gw_ >= 1.f && gh_ >= 1.f ? renderer_->viewTexture(0, (uint32_t)gw_, (uint32_t)gh_) : (ImTextureID)0;
        ImGui::InvisibleButton("##game", ImVec2(std::max(size.x, 1.f), std::max(size.y, 1.f)));   // (it takes the mouse)
        gameViewHovered_ = ImGui::IsItemHovered();
        if (image) ImGui::GetWindowDrawList()->AddImage(image, at, ImVec2(at.x + gw_, at.y + gh_));
        gameUi_.begin(ImGui::GetWindowDrawList(), gx_, gy_, gw_, gh_, !gameUiCaptured_ && gameViewHovered_);
        game_.drawUI(gameUi_);
        gameUi_.end();
        uiWantsCursor_ = gameUi_.wantsCursor();
    } else {
        gw_ = gh_ = 0.f;
        gameViewHovered_ = false;
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Play mode
// ---------------------------------------------------------------------------------------------
void Editor::startPlay() {
    if (playing()) return;
    editScene_ = scene_;
    editSelected_ = selected_;
    nav_ = Nav::None;
    game_.setAssetLibraries(meshes_, textures_);
    game_.setLanguage(project_.language);
    game_.setAsyncLoading(true);   // scene changes load in the background, behind a loading screen
    game_.setScenePath(scenePath_);
    game_.setSaveDirectory(project::saveDir(project_.name));
    game_.start(scene_, meshes_);
    setStatus("Playing. Ctrl+P stops and restores the scene.");
    if (hostMultiplayer_) hostGame();
}

// Hosting while playing: on the default port, and in a lobby on the project's relay if it names one
void Editor::hostGame() {
    std::string error;
    if (!net_.host(game_, NetSession::kDefaultPort, error)) {
        setStatus("Can't host: " + error);
        return;
    }
    if (!project_.relay.empty() && !net_.advertise(project_.relay, project_.name, project_.name + " (editor)", 16, error))
        setStatus("Can't list the game on the relay: " + error);
}

void Editor::stopPlay() {
    if (!playing()) return;
    platform::setCursorCaptured(false);
    net_.stop();
    game_.stop();
    scene_ = std::move(editScene_);
    editScene_ = Scene{};
    selected_ = editSelected_;
    gizmoWasUsing_ = false;
    setStatus("Stopped: scene restored");
}

void Editor::togglePause() {
    if (playing()) game_.setPaused(!game_.paused());
}

// ---------------------------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------------------------
void Editor::select(EntityId id) {
    selected_ = id;
    extraSelected_.clear();
    scrollToSelected_ = true;
}

std::vector<EntityId> Editor::selectionIds() const {
    std::vector<EntityId> out;
    if (scene_.find(selected_)) out.push_back(selected_);
    for (EntityId id : extraSelected_)
        if (scene_.find(id) && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    return out;
}

std::vector<EntityId> Editor::topLevelSelection() const {
    std::vector<EntityId> all = selectionIds(), out;
    for (EntityId id : all) {
        bool under = false;
        for (EntityId other : all) under = under || (other != id && scene_.isAncestor(other, id));
        if (!under) out.push_back(id);
    }
    return out;
}

bool Editor::isSelected(EntityId id) const {
    return id != kNoEntity && (id == selected_ || std::find(extraSelected_.begin(), extraSelected_.end(), id) != extraSelected_.end());
}

void Editor::toggleSelect(EntityId id) {
    if (!scene_.find(id)) return;
    if (id == selected_) {   // the primary goes; the next one takes over
        selected_ = extraSelected_.empty() ? kNoEntity : extraSelected_.front();
        if (!extraSelected_.empty()) extraSelected_.erase(extraSelected_.begin());
    } else if (auto it = std::find(extraSelected_.begin(), extraSelected_.end(), id); it != extraSelected_.end()) {
        extraSelected_.erase(it);
    } else if (selected_ == kNoEntity) {
        selected_ = id;
    } else {
        extraSelected_.push_back(id);
    }
}

void Editor::makePrefabVariant(int index) {
    if (index < 0 || index >= (int)scene_.prefabs.size()) return;
    pushUndo();
    Prefab base = scene_.prefabs[(size_t)index];
    // An instance of the base, made a prefab of its own (so its root stays linked to the base), then removed
    std::vector<Entity> made = scene_.instantiate(base);
    EntityId root = made[0].id;
    for (Entity& e : made) scene_.add(std::move(e));
    std::string name = scene_.uniquePrefabName(base.name + " Variant");
    scene_.addPrefab(scene_.makePrefab(root, name));
    scene_.remove(root);
    selectedPrefab_ = (int)scene_.prefabs.size() - 1;
    setStatus("Made " + name + ": change it (an instance, then Apply) and it keeps following " + base.name);
}

// A thin handle over a panel's edge: dragging it changes `size` (unscaled units) by the mouse movement times `sign`
// ---------------------------------------------------------------------------------------------
// Layout and preferences
// ---------------------------------------------------------------------------------------------
void Editor::drawDockspace(bool panels) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 pos(0.f, topH_), size(io.DisplaySize.x, std::max(io.DisplaySize.y - topH_ - statusH_, 1.f));
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    // A window under everything else, holding the dockspace; the central node is left see-through: the 3D view
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##dock-host", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGuiID id = ImGui::GetID("##editor-dockspace");
    if (resetLayout_ || !ImGui::DockBuilderGetNode(id)) {   // first run (no saved layout) or View > Reset layout
        buildDefaultLayout(id);
        resetLayout_ = false;
    }
    // The Game view, the first time it shows (nothing saved about it): docked beside the scene view, which keeps the
    // dockspace's central node
    if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(id); central && playing() && showGameView_ &&
        !ImGui::FindWindowByName("Game") && !ImGui::FindWindowSettingsByID(ImHashStr("Game"))) {
        ImGuiID right = ImGui::DockBuilderSplitNode(central->ID, ImGuiDir_Right, 0.45f, nullptr, nullptr);
        ImGui::DockBuilderDockWindow("Game", right);
        ImGui::DockBuilderFinish(id);
    }
    // While playing maximised the panels aren't drawn: the dockspace is only kept alive, the view takes it all
    ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode;
    ImGui::DockSpace(id, ImVec2(0.f, 0.f), dockFlags | (panels ? 0 : ImGuiDockNodeFlags_KeepAliveOnly));
    ImGui::End();

    vx_ = pos.x, vy_ = pos.y, vw_ = size.x, vh_ = size.y;
    if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(id); panels && central && central->IsVisible) {
        vx_ = central->Pos.x, vy_ = central->Pos.y;
        vw_ = std::max(central->Size.x, 1.f), vh_ = std::max(central->Size.y, 1.f);
    }
}

void Editor::buildDefaultLayout(ImGuiID dockspace) {
    // Outliner on the left, Inspector on the right, Assets under the view
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 size(std::max(io.DisplaySize.x, 1.f), std::max(io.DisplaySize.y - topH_ - statusH_, 1.f));
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, size);
    ImGuiID center = dockspace;
    float leftW = std::clamp(px(250.f), size.x * 0.1f, size.x * 0.35f);
    float rightW = std::clamp(px(340.f), size.x * 0.1f, size.x * 0.4f);
    float assetsH = std::clamp(px(160.f), size.y * 0.1f, size.y * 0.4f);
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, leftW / size.x, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, rightW / std::max(size.x - leftW, 1.f), nullptr,
                                                &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, assetsH / size.y, nullptr, &center);
    ImGui::DockBuilderDockWindow("Outliner", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Assets", bottom);
    ImGui::DockBuilderFinish(dockspace);
}

void Editor::applyPrefs(const EditorPrefs& p) {
    tool_ = (Tool)p.tool;
    localSpace_ = p.localSpace;
    snap_ = p.snap;
    snapMove_ = p.snapMove, snapRotate_ = p.snapRotate, snapScale_ = p.snapScale;
    showOutliner_ = p.showOutliner, showInspector_ = p.showInspector, showAssets_ = p.showAssets;
    showColliders_ = p.showColliders, showProfiler_ = p.showProfiler;
    previewParticles_on_ = p.previewParticles;
    realtimeViewport_ = p.realtimeViewport, frameCap_ = p.frameCap, throttleInBackground_ = p.throttleInBackground;
    cam_.speed = p.cameraSpeed;
    cam_.fovY = p.cameraFov * DEG2RAD;
    antialiasing_ = p.antialiasing;
    maximizeOnPlay_ = p.maximizeOnPlay;
    captureOnClick_ = p.captureOnClick;
    recentProjects_ = p.recentProjects;
}

EditorPrefs Editor::currentPrefs() const {
    EditorPrefs p;
    p.tool = (int)tool_;
    p.localSpace = localSpace_;
    p.snap = snap_;
    p.snapMove = snapMove_, p.snapRotate = snapRotate_, p.snapScale = snapScale_;
    p.showOutliner = showOutliner_, p.showInspector = showInspector_, p.showAssets = showAssets_;
    p.showColliders = showColliders_, p.showProfiler = showProfiler_;
    p.previewParticles = previewParticles_on_;
    p.realtimeViewport = realtimeViewport_, p.frameCap = frameCap_, p.throttleInBackground = throttleInBackground_;
    p.cameraSpeed = cam_.speed;
    p.cameraFov = std::round(cam_.fovY / DEG2RAD * 10.f) / 10.f;
    p.antialiasing = antialiasing_;
    p.maximizeOnPlay = maximizeOnPlay_;
    p.captureOnClick = captureOnClick_;
    p.recentProjects = recentProjects_;
    return p;
}

void Editor::savePrefsIfChanged(float dt) {
    if (!persistent_ || (prefsTimer_ += dt) < 2.f) return;   // (checked every couple of seconds)
    prefsTimer_ = 0.f;
    EditorPrefs p = currentPrefs();
    if (p != savedPrefs_ && editor_prefs::save(prefsFile_, p)) savedPrefs_ = p;
}

void Editor::selectAllRoots() {
    extraSelected_.clear();
    selected_ = kNoEntity;
    for (const Entity& e : scene_.entities)
        if (e.parent == kNoEntity) {
            if (selected_ == kNoEntity) selected_ = e.id;
            else extraSelected_.push_back(e.id);
        }
}

void Editor::selectRange(EntityId id) {
    int a = scene_.indexOf(selected_), b = scene_.indexOf(id);
    if (a < 0 || b < 0) { select(id); return; }
    extraSelected_.clear();
    for (int i = std::min(a, b); i <= std::max(a, b); ++i)
        if (scene_.entities[(size_t)i].id != selected_) extraSelected_.push_back(scene_.entities[(size_t)i].id);
}

Vec3 Editor::spawnPoint(float lift) const {
    // Where the view ray hits the ground (if close enough), otherwise 10 units ahead
    Vec3 fwd = cam_.forward(), p = cam_.pos + fwd * 10.f;
    if (fwd.y < -0.05f) {
        float t = -cam_.pos.y / fwd.y;
        if (t > 0.f && t < 60.f) p = cam_.pos + fwd * t;
    }
    p.y = std::max(p.y, lift);
    return p;
}

void Editor::addObject(int mesh) {
    pushUndo();
    Entity& e = scene_.create(scene_.uniqueName(std::string(meshes_.name(mesh)) + ".001"));
    MeshRenderer mr;
    mr.mesh = mesh;
    static const Vec3 palette[] = {{0.90f, 0.45f, 0.35f}, {0.40f, 0.70f, 0.95f}, {0.55f, 0.85f, 0.45f},
                                   {0.95f, 0.80f, 0.35f}, {0.75f, 0.50f, 0.90f}};
    mr.color = palette[scene_.entities.size() % 5];
    e.meshRenderer = mr;
    float size = mesh == (int)MeshKind::Plane ? 6.f : 2.f;
    for (float& s : e.transform.scale) s = size;
    // rest on the ground
    Vec3 p = spawnPoint(std::max(-meshes_.get(mesh).boundsMin.y * size, 0.f) + 0.01f);
    e.transform.position[0] = p.x; e.transform.position[1] = p.y; e.transform.position[2] = p.z;
    select(e.id);
    setStatus("Added " + e.name);
}

void Editor::addEmpty() {
    pushUndo();
    Entity& e = scene_.create(scene_.uniqueName("Empty.001"));
    Vec3 p = spawnPoint(0.f);
    e.transform.position[0] = p.x; e.transform.position[1] = p.y; e.transform.position[2] = p.z;
    select(e.id);
    setStatus("Added " + e.name + " (an empty: group objects under it in the outliner)");
}

void Editor::addWater() {
    pushUndo();
    Entity& e = scene_.create(scene_.uniqueName("Water"));
    Vec3 p = spawnPoint(0.f);
    e.transform.position[0] = p.x; e.transform.position[1] = p.y; e.transform.position[2] = p.z;
    e.water = Water{};
    select(e.id);
    setStatus("Added water: 20 x 20 m, its surface at the entity's height; waves, current and look in the inspector");
}

void Editor::addReflectionProbe() {
    pushUndo();
    Entity& e = scene_.create(scene_.uniqueName("Reflection Probe"));
    Vec3 p = spawnPoint(0.f);
    e.transform.position[0] = p.x; e.transform.position[1] = p.y + 1.5f; e.transform.position[2] = p.z;
    e.reflectionProbe = ReflectionProbe{};
    select(e.id);
    setStatus("Added a reflection probe: shiny things inside its box reflect what it sees from its centre");
}

void Editor::addCamera() {
    pushUndo();
    Entity& e = scene_.create(scene_.uniqueName("Camera.001"));
    Camera cam;
    cam.fovY = cam_.fovY;
    e.camera = cam;
    // Start where the editor camera is, looking the same way
    e.transform.setMatrix(inverseAffine(cam_.view()));
    e.transform.scale[0] = e.transform.scale[1] = e.transform.scale[2] = 1.f;
    select(e.id);
    setStatus("Added " + e.name + " at the current view");
}

void Editor::addLight(Light::Type type) {
    pushUndo();
    bool sun = type == Light::Type::Directional;
    Entity& e = scene_.create(scene_.uniqueName(sun ? "Sun.001" : "Light.001"));
    Light l;
    l.type = type;
    e.light = l;
    const float unit[3] = {1, 1, 1};
    Vec3 p = sun ? cam_.pos + cam_.forward() * 10.f : spawnPoint(0.f) + Vec3{0, 2.5f, 0};
    e.transform.setMatrix(lookRotation(p, sun ? normalize(Vec3{-0.45f, -1.f, -0.3f}) : Vec3{0, 0, -1}, unit));
    select(e.id);
    setStatus("Added " + e.name);
}

void Editor::importModel(const std::string& rawPath, const Vec3* at) {
    std::string path = rawPath;
    if (path.size() >= 2 && path[0] == '~' && (path[1] == '/' || path[1] == '\\'))
        if (const char* home = std::getenv("HOME")) path = home + path.substr(1);
    importError_.clear();
    std::string ext = std::filesystem::path(path).extension().u8string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".gltf" || ext == ".glb") {
        // The whole model: its node tree becomes a prefab, and a copy goes into the scene
        Prefab p;
        std::string warning;
        if (!importGltfPrefab(path, meshes_, textures_, p, importError_, warning)) return;
        // The model's import setting "scale" (assets/asset_db.h): on the prefab's root
        float scale = assetdb::importSettings(std::filesystem::u8path(path))["scale"].asFloat(1.f);
        if (scale > 0.f && scale != 1.f && std::isfinite(scale) && !p.entities.empty())
            for (float& c : p.entities[0].transform.scale) c *= scale;
        pushUndo();
        p.name = scene_.uniquePrefabName(p.name);
        scene_.addPrefab(std::move(p));
        instantiatePrefab((int)scene_.prefabs.size() - 1);
        if (at)
            if (Entity* root = selection()) {
                root->transform.position[0] = at->x; root->transform.position[1] = at->y; root->transform.position[2] = at->z;
            }
        setStatus("Imported " + scene_.prefabs.back().name + " as a prefab (" +
                  std::to_string(scene_.prefabs.back().entities.size()) + " entities)" +
                  (warning.empty() ? "" : ". Warning: " + warning));
        return;
    }
    int before = meshes_.count();
    int id = importObj(meshes_, path, importError_);
    if (id < 0) return;
    addObject(id);
    if (at)
        if (Entity* e = selection()) {   // resting on the drop point
            e->transform.position[0] = at->x;
            e->transform.position[1] = at->y - meshes_.get(id).boundsMin.y * e->transform.scale[1];
            e->transform.position[2] = at->z;
        }
    const Mesh& m = meshes_.get(id);
    setStatus((meshes_.count() > before ? "Imported " : "Already imported: ") + m.name + " (" +
              std::to_string(m.indexCount / 3) + " triangles)");
}

// ---------------------------------------------------------------------------------------------
// Prefabs
// ---------------------------------------------------------------------------------------------
void Editor::createPrefabFromSelection() {
    Entity* e = selection();
    if (!e) return;
    pushUndo();
    std::string name = scene_.uniquePrefabName(e->name);
    scene_.addPrefab(scene_.makePrefab(e->id, name));
    selectedPrefab_ = (int)scene_.prefabs.size() - 1;
    showPrefabsTab_ = true;
    setStatus("Created prefab " + name + " (" + std::to_string(scene_.prefabs.back().entities.size()) + " entities)");
}

void Editor::instantiatePrefab(int index) {
    if (index < 0 || index >= (int)scene_.prefabs.size()) return;
    pushUndo();
    const Prefab& p = scene_.prefabs[(size_t)index];
    std::vector<Entity> copies = scene_.instantiate(p);
    Transform& t = copies[0].transform;
    float lift = 0.01f;   // rest a mesh root on the ground
    if (copies[0].meshRenderer) lift += std::max(-meshes_.get(copies[0].meshRenderer->mesh).boundsMin.y * t.scale[1], 0.f);
    Vec3 at = spawnPoint(lift);
    t.position[0] = at.x; t.position[1] = at.y; t.position[2] = at.z;
    EntityId root = copies[0].id;
    for (Entity& e : copies) scene_.add(std::move(e));
    select(root);
    setStatus("Added " + scene_.find(root)->name + " from prefab " + p.name);
}

void Editor::updatePrefabFromSelection(int index) {
    Entity* e = selection();
    if (!e || index < 0 || index >= (int)scene_.prefabs.size()) return;
    pushUndo();
    const Prefab& p = scene_.prefabs[(size_t)index];
    std::string name = p.name, from = e->name;
    // From an instance: apply its changes. From anything else: it becomes the prefab (and an instance of it).
    EntityId root = instanceRoot(scene_, e->id);
    if (root == kNoEntity || scene_.find(root)->prefab->prefab != p.id) root = e->id;
    int updated = applyToPrefab(scene_, root, p.id, meshes_, textures_);
    setStatus("Prefab " + name + " updated from " + from + "; " + std::to_string(std::max(updated, 0)) +
              " other instance(s) followed (their own changes were kept)");
}

void Editor::deletePrefab(int index) {
    if (index < 0 || index >= (int)scene_.prefabs.size()) return;
    pushUndo();
    setStatus("Deleted prefab " + scene_.prefabs[(size_t)index].name + " (its instances are now ordinary entities)");
    uint32_t id = scene_.prefabs[(size_t)index].id;
    for (Entity& e : scene_.entities)
        if (e.prefab && e.prefab->prefab == id) e.prefab.reset();
    scene_.prefabs.erase(scene_.prefabs.begin() + index);
    selectedPrefab_ = -1;
}

void Editor::exportPrefab(int index) {
    if (index < 0 || index >= (int)scene_.prefabs.size()) return;
    const Prefab& p = scene_.prefabs[(size_t)index];
    std::string path = (project::root() / (p.name + ".prefab")).u8string(), error;
    if (savePrefab(path, p, meshes_, textures_, error)) {
        setStatus("Exported " + project::toStored(path) + " (in the project folder)");
        assetsScannedAt_ = -1.0;   // show it in the browser
    }
    else setStatus("Export failed: " + error);
}

void Editor::importPrefab() {
    std::filesystem::path path = platform::openFileDialog("Import prefab", "prefab");
    ImGui::GetIO().ClearInputKeys();   // the dialog ran its own event loop
    ImGui::GetIO().ClearInputMouse();
    if (!path.empty()) importPrefabFile(path);
}

void Editor::importPrefabFile(const std::filesystem::path& path, const Vec3* placeAt) {
    Prefab p;
    std::string error;
    if (!loadPrefab(path.u8string(), p, meshes_, textures_, error)) { setStatus("Import failed: " + error); return; }
    pushUndo();
    p.name = scene_.uniquePrefabName(p.name);
    scene_.addPrefab(std::move(p));
    selectedPrefab_ = (int)scene_.prefabs.size() - 1;
    showPrefabsTab_ = true;
    setStatus("Imported prefab " + scene_.prefabs.back().name);
    if (placeAt) {   // dropped on the viewport: also put a copy there
        instantiatePrefab(selectedPrefab_);
        if (Entity* root = selection()) {
            root->transform.position[0] = placeAt->x; root->transform.position[1] = placeAt->y;
            root->transform.position[2] = placeAt->z;
        }
    }
}

void Editor::duplicateSelected() {
    Entity* e = selection();
    if (!e) return;
    if (!extraSelected_.empty()) {   // a group: each top-level one, the copies selected
        pushUndo();
        std::vector<EntityId> copies;
        for (EntityId id : topLevelSelection()) {
            EntityId copy = scene_.duplicate(id);
            Entity* c = scene_.find(copy);
            Vec3 offset = transformDir(inverseAffine(scene_.parentWorldMatrix(copy)), cam_.right());
            c->transform.position[0] += offset.x;
            c->transform.position[2] += offset.z;
            copies.push_back(copy);
        }
        select(copies.front());
        extraSelected_.assign(copies.begin() + 1, copies.end());
        setStatus("Duplicated " + std::to_string(copies.size()) + " objects");
        return;
    }
    pushUndo();
    float offsetBy = std::max(e->transform.scale[0], 1.f);
    EntityId copy = scene_.duplicate(e->id);
    Entity* c = scene_.find(copy);
    // Shift it sideways (in the parent's space) so it doesn't sit exactly on the original
    Vec3 offset = transformDir(inverseAffine(scene_.parentWorldMatrix(copy)), cam_.right() * offsetBy);
    c->transform.position[0] += offset.x;
    c->transform.position[2] += offset.z;
    select(copy);
    setStatus("Duplicated as " + c->name);
}

void Editor::deleteSelected() {
    Entity* e = selection();
    if (!e) return;
    if (!extraSelected_.empty()) {
        pushUndo();
        std::vector<EntityId> gone = topLevelSelection();
        scene_.remove(gone);
        setStatus("Deleted " + std::to_string(gone.size()) + " objects");
        selected_ = kNoEntity;
        extraSelected_.clear();
        return;
    }
    pushUndo();
    size_t count = scene_.subtree(e->id).size();
    setStatus("Deleted " + e->name + (count > 1 ? " and " + std::to_string(count - 1) + " children" : ""));
    scene_.remove(e->id);
    selected_ = kNoEntity;
}

void Editor::reparent(EntityId id, EntityId newParent) {
    Entity* e = scene_.find(id);
    if (!e || e->parent == newParent) return;
    if (scene_.isAncestor(id, newParent) || id == newParent) {
        setStatus("Can't parent an object to itself or to one of its children");
        return;
    }
    pushUndo();
    scene_.setParent(id, newParent, true);
    const Entity* p = scene_.find(newParent);
    setStatus(e->name + (p ? " is now a child of " + p->name : " is now at the root"));
}

void Editor::toggleHideSelected() {
    Entity* e = selection();
    if (!e) return;
    pushUndo();
    e->visible = !e->visible;
}

void Editor::unhideAll() {
    pushUndo();
    for (auto& e : scene_.entities) e.visible = true;
}

void Editor::focusSelected() {
    Entity* e = selection();
    if (!e) return;
    const Mat4& m = worldOf(e->id);
    Vec3 center = transformPoint(m, {0, 0, 0});
    float radius = 1.f;
    if (e->meshRenderer) {
        const Mesh& mesh = meshes_.get(e->meshRenderer->mesh);
        center = transformPoint(m, (mesh.boundsMin + mesh.boundsMax) * 0.5f);
        float sx = length(transformDir(m, {1, 0, 0})), sy = length(transformDir(m, {0, 1, 0})),
              sz = length(transformDir(m, {0, 0, 1}));
        radius = 0.75f * std::max({sx, sy, sz});
    }
    float dist = std::max(radius / std::tan(cam_.fovY * 0.5f) * 1.3f, 1.f);
    cam_.pos = center - cam_.forward() * dist;
    cam_.orbitDistance = dist;
}

void Editor::dropSelectedToGround() {
    Entity* e = selection();
    if (!e || !e->meshRenderer) return;
    pushUndo();
    // Lowest point of the mesh in the world, then move the entity (in its parent's space) by that much
    Mat4 world = scene_.worldMatrix(e->id);
    const Mesh& mesh = meshes_.get(e->meshRenderer->mesh);
    float minY = FLT_MAX;
    for (uint32_t i = 0; i < mesh.indexCount; ++i)
        minY = std::min(minY, transformPoint(world, meshes_.vertices[mesh.vertexOffset + meshes_.indices[mesh.firstIndex + i]].pos).y);
    Mat4 moved = translate({0, -minY, 0}) * world;
    e->transform.setMatrix(inverseAffine(scene_.parentWorldMatrix(e->id)) * moved);
}

void Editor::newScene(bool demo) {
    pushUndo();
    if (demo) {
        scene_ = buildDefaultScene();
    } else {   // ground + player (with its camera)
        Scene fresh = buildDefaultScene(), keep;
        for (const char* name : {"Ground", "Player"})
            if (const Entity* root = fresh.findByName(name))
                for (EntityId id : fresh.subtree(root->id)) keep.add(*fresh.find(id));
        scene_ = std::move(keep);
    }
    selected_ = kNoEntity;
}

void Editor::newStressScene() {
    pushUndo();
    scene_ = buildStressScene();
    selected_ = kNoEntity;
    setStatus("Stress test scene: " + std::to_string(scene_.entities.size()) + " entities");
}

void Editor::saveToFile() {
    std::string error;
    if (saveScene(project::resolve(scenePath_), scene_, meshes_, textures_, error)) {
        setStatus(std::string("Saved ") + scenePath_);
        assetsScannedAt_ = -1.0;
        savedVersion_ = autosavedVersion_ = editVersion_;
        std::error_code ec;
        std::filesystem::remove(autosavePath(), ec);   // (saved for real: nothing to recover)
        recoverable_ = false;
    }
    else setStatus("Save failed: " + error);
}

bool Editor::selectByName(const std::string& names) {
    // (several, separated by commas: the first is the primary)
    bool all = true, first = true;
    size_t at = 0;
    while (at <= names.size()) {
        size_t comma = names.find(',', at);
        std::string name = names.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        at = comma == std::string::npos ? names.size() + 1 : comma + 1;
        const Entity* e = scene_.findByName(name);
        if (!e) { all = false; continue; }
        if (first) select(e->id);
        else toggleSelect(e->id);
        first = false;
    }
    return all;
}

void Editor::openScene(const std::string& path) {
    std::snprintf(scenePath_, sizeof(scenePath_), "%s", project::toStored(project::resolve(path)).c_str());
    loadFromFile();
}

void Editor::loadFromFile() {
    Scene loaded;
    std::string error;
    if (!loadScene(project::resolve(scenePath_), loaded, meshes_, textures_, error)) {
        setStatus("Load failed: " + error);
        return;
    }
    pushUndo();
    scene_ = std::move(loaded);
    selected_ = kNoEntity;
    setStatus(std::string("Loaded ") + scenePath_);
    commitUndo();   // (the load itself is a step: undoable)
    savedVersion_ = autosavedVersion_ = editVersion_;
    // Unsaved work from a session that ended without saving (a crash, say)?
    std::error_code ec;
    std::filesystem::path auto_ = autosavePath(), file = std::filesystem::u8path(project::resolve(scenePath_));
    recoverable_ = std::filesystem::exists(auto_, ec) &&
                   std::filesystem::last_write_time(auto_, ec) > std::filesystem::last_write_time(file, ec);
    if (recoverable_) setStatus("There are unsaved changes from an earlier session: File > Recover autosave");
}

std::filesystem::path Editor::autosavePath() const {
    std::string file;
    for (char c : std::string(scenePath_)) file += (std::isalnum((unsigned char)c) || c == '-' || c == '.') ? c : '_';
    return project::saveDir(project_.name.empty() ? "Untitled" : project_.name).parent_path() / "autosave" / file;
}

void Editor::autosave() {
    std::filesystem::path path = autosavePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::string error;
    if (saveScene(path.u8string(), scene_, meshes_, textures_, error)) {
        autosavedVersion_ = editVersion_;
        logfile::write("Autosaved " + path.u8string());
    } else {
        logLine("Autosave failed: %s", error.c_str());
    }
}

void Editor::recoverAutosave() {
    Scene loaded;
    std::string error;
    if (!loadScene(autosavePath().u8string(), loaded, meshes_, textures_, error)) {
        setStatus("Can't recover: " + error);
        return;
    }
    pushUndo();
    scene_ = std::move(loaded);
    selected_ = kNoEntity;
    recoverable_ = false;
    commitUndo();   // (unsaved now: Save writes it to the scene file)
    setStatus("Recovered the autosave: Save to keep it");
}

void Editor::setStatus(const std::string& msg) {
    status_ = msg;
    statusTimer_ = 4.f;
}

// ---------------------------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------------------------
void Editor::pushUndo() {
    if (playing()) return;   // play-mode edits are discarded on Stop
    if (!undoBefore_) {      // (several operations in one frame make one step)
        undoBefore_ = scene_;
        undoBeforeSelected_ = selected_;
    }
}

void Editor::commitUndo() {
    UndoStep step;
    if (undoBefore_) {
        step = {diffScenes(*undoBefore_, scene_), undoBeforeSelected_};
        undoBefore_.reset();
    } else if (editBegan_ && !playing()) {   // a widget edit: only what widgets can change, as it was this frame
        if (frameEntity_) {
            const Entity* now = scene_.find(frameEntity_->second.id);
            if (now && !sameEntity(*now, frameEntity_->second)) step.delta.entities.push_back(*frameEntity_);
        }
        for (const auto& g : frameGroup_) {   // a group moved with the gizmo
            const Entity* now = scene_.find(g.second.id);
            if (now && !sameEntity(*now, g.second)) step.delta.entities.push_back(g);
        }
        if (!sameRenderSettings(frameRender_, scene_.render)) step.delta.render = frameRender_;
        if (!sameNavigationSettings(frameNav_, scene_.navigation)) step.delta.navigation = frameNav_;
        if (!sameLayerSettings(frameLayers_, scene_.layers)) step.delta.layers = frameLayers_;
        if (!sameInputActions(frameInput_, scene_.inputActions)) step.delta.inputActions = frameInput_;
        step.selected = frameSelected_;
    }
    editBegan_ = false;
    if (step.delta.empty() || playing()) return;
    undo_.push_back(std::move(step));
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    ++editVersion_;
}

void Editor::undo() {
    commitUndo();   // (an operation earlier this frame)
    if (undo_.empty() || playing()) return;
    if (undo_.back().file.any()) {
        UndoStep step = std::move(undo_.back());
        undo_.pop_back();
        if (reverseFileOp(step.file, false)) redo_.push_back(std::move(step));
        return;
    }
    Scene now = scene_;
    applyDelta(scene_, undo_.back().delta);
    redo_.push_back({diffScenes(now, scene_), selected_});
    selected_ = undo_.back().selected;
    extraSelected_.clear();
    ++editVersion_;
    undo_.pop_back();
    setStatus("Undo");
}

void Editor::redo() {
    if (redo_.empty() || playing()) return;
    if (redo_.back().file.any()) {
        UndoStep step = std::move(redo_.back());
        redo_.pop_back();
        if (reverseFileOp(step.file, true)) undo_.push_back(std::move(step));
        return;
    }
    Scene now = scene_;
    applyDelta(scene_, redo_.back().delta);
    undo_.push_back({diffScenes(now, scene_), selected_});
    selected_ = redo_.back().selected;
    extraSelected_.clear();
    redo_.pop_back();
    ++editVersion_;
    setStatus("Redo");
}

void Editor::pacing(bool& lazy, float& maxFps, float& unfocusedFps) const {
    lazy = !realtimeViewport_ && !(playing() && !game_.paused());
    maxFps = (float)frameCap_;
    unfocusedFps = throttleInBackground_ ? 10.f : 0.f;
}

bool Editor::busy() const {
    if (playing() && !game_.paused()) return true;
    if (game_.loading() || net_.active() || animPreview_.playing) return true;
    if (previewParticles_on_ && !playing() && selectionHasEmitter()) return true;   // (emitters animate in the editor too)
    return false;
}

bool Editor::selectionHasEmitter() const {
    for (const Entity& e : scene_.entities) {
        if (!e.particleEmitter) continue;
        for (EntityId p = e.id; p != kNoEntity;) {
            if (isSelected(p)) return true;
            const Entity* pe = scene_.find(p);
            p = pe ? pe->parent : kNoEntity;
        }
    }
    return false;
}
