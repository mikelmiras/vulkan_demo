#include "editor/editor.h"
#include "scene/stress_scene.h"

#include <cctype>
#include <cfloat>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "ImGuizmo.h"
#include "app/profiler_view.h"
#include "core/log.h"
#include "core/profiler.h"
#include "game/animation.h"
#include "render/scene_draw.h"
#include "scene/prefab_sync.h"
#include "platform/platform.h"
#include "scene/serialization.h"

namespace {
constexpr size_t kMaxUndo = 200;
}

Editor::Editor(MeshLibrary& meshes, TextureLibrary& textures, AudioEngine* audio, float uiScale)
    : meshes_(meshes), textures_(textures), audio_(audio), uiScale_(uiScale), scene_(buildDefaultScene()) {
    game_.setAudio(audio);
    assetDir_ = project::root();
    std::string error;
    if (project::exists(project::root())) project::load(project::root(), project_, error);
    else project_.name = project::root().filename().u8string();
    if (!project_.module.empty()) loadGameModule();
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
    game_.frame(dt, !io.WantTextInput, captured || viewportHovered(), captured ? ImVec2(mdx, mdy) : io.MouseDelta);
    uiWantsCursor_ = false;
    if (playing()) {   // the game's HUD and menus, over the viewport (last frame's layout)
        gameUi_.begin(ImGui::GetBackgroundDrawList(), vx_, vy_, vw_, vh_, !captured && viewportHovered());
        game_.drawUI(gameUi_);
        gameUi_.end();
        uiWantsCursor_ = gameUi_.wantsCursor();
    }
    keyTakenByBindings_ = false;
    if (audio_ && !playing()) audio_->update();   // (while playing, the game does it)
    pollGameModule();
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

    // Layout: menu bar + toolbar on top, status bar at the bottom, panels left/right, viewport in the middle
    const ImGuiStyle& style = ImGui::GetStyle();
    float menuH = ImGui::GetFrameHeight();
    float toolbarH = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.f;
    topH_ = menuH + toolbarH;
    statusH_ = ImGui::GetTextLineHeight() + style.WindowPadding.y * 2.f;
    bool panels = !(playing() && maximizeOnPlay_);
    // (panel sizes: dragged by their edges, in unscaled units; kept so the viewport has some room)
    if (outlinerW_ <= 0.f) outlinerW_ = 250.f, inspectorW_ = 340.f, assetsPanelH_ = 160.f;
    float minView = px(200.f);
    float leftW = panels && showOutliner_ ? std::clamp(px(outlinerW_), px(140.f), std::max(io.DisplaySize.x * 0.45f, px(140.f))) : 0.f;
    float rightW = panels && showInspector_ ? std::clamp(px(inspectorW_), px(220.f), std::max(io.DisplaySize.x * 0.5f, px(220.f))) : 0.f;
    if (io.DisplaySize.x - leftW - rightW < minView) {   // a small window: the panels give way
        float over = minView - (io.DisplaySize.x - leftW - rightW);
        float l = std::min(over * 0.5f, std::max(leftW - px(140.f), 0.f));
        leftW -= l;
        rightW = std::max(rightW - (over - l), 0.f);
    }
    vx_ = leftW;
    vy_ = topH_;
    vw_ = std::max(io.DisplaySize.x - leftW - rightW, 1.f);
    assetsH_ = panels && showAssets_ ? std::clamp(px(assetsPanelH_), px(70.f), std::max(io.DisplaySize.y * 0.6f, px(70.f))) : 0.f;
    vh_ = std::max(io.DisplaySize.y - topH_ - statusH_ - assetsH_, 1.f);
    updateView();

    drawMenuBar();
    drawToolbar();
    if (leftW > 0.f) drawOutliner();
    if (rightW > 0.f) drawInspector(gpuName, stats);
    drawStatusBar(stats);
    if (showHelp_) drawHelp();
    if (assetsH_ > 0.f) drawAssetBrowser();
    drawAddPopup();
    drawImportPopup();
    if (showInputBindings_) drawInputBindings();
    if (animGraphEntity_ != kNoEntity) drawAnimatorGraph();
    if (showProfiler_) drawProfilerWindow(&showProfiler_);
    // Splitters on the panels' inner edges: drag to resize
    if (leftW > 0.f) drawSplitter("##split-left", ImVec2(vx_ - px(3.f), vy_), ImVec2(px(6.f), io.DisplaySize.y - vy_ - statusH_), false, outlinerW_, 1.f);
    if (rightW > 0.f) drawSplitter("##split-right", ImVec2(vx_ + vw_ - px(3.f), vy_), ImVec2(px(6.f), io.DisplaySize.y - vy_ - statusH_), false, inspectorW_, -1.f);
    if (assetsH_ > 0.f) drawSplitter("##split-assets", ImVec2(vx_, vy_ + vh_ - px(3.f)), ImVec2(vw_, px(6.f)), true, assetsPanelH_, -1.f);

    handleNavigation(dt);
    handleGizmo();
    handlePicking();
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
    if (!scene_.find(selected_)) selected_ = kNoEntity;
    if (selected_ == kNoEntity) extraSelected_.clear();
    extraSelected_.erase(std::remove_if(extraSelected_.begin(), extraSelected_.end(),
                                        [&](EntityId id) { return !scene_.find(id) || id == selected_; }),
                         extraSelected_.end());
    updateView();   // the scene or the camera may have changed during this frame
    buildDrawItems();
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
        drawItems_ = collectDrawItems(scene_, scene_.worldMatrices(), viewingGameCamera() ? kNoEntity : selected_,
                                      &joints_, &meshes_, viewingGameCamera() ? nullptr : &extraSelected_);
        for (size_t i = 0; i < saved.size(); ++i) scene_.entities[i].transform = saved[i];
    } else {
        if (playing()) animPreview_ = {};
        drawItems_ = collectDrawItems(scene_, worlds_, viewingGameCamera() ? kNoEntity : selected_, &joints_, &meshes_,
                                      viewingGameCamera() ? nullptr : &extraSelected_);
    }
    lights_ = collectLights(scene_, worlds_);
    particles_.clear();
    if (playing()) {
        previewParticles_.clear();
        game_.particles().collect(particles_);
    } else if (previewParticles_on_) {   // emitters run in the editor too, so their look can be tuned
        previewParticles_.update(scene_, worlds_, std::min(frameDt_, 0.1f));
        previewParticles_.collect(particles_);
    } else {
        previewParticles_.clear();
    }
}

SceneRenderInfo Editor::renderInfo() const {
    SceneRenderInfo info;
    info.items = &drawItems_;
    info.lights = &lights_;
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
    info.zNear = viewNear_;
    info.zFar = viewFar_;
    info.viewProj = viewProj_;
    info.camPos = viewPos_;
    info.camForward = viewForward_;
    info.viewportX = vx_;
    info.viewportY = vy_;
    info.viewportW = vw_;
    info.viewportH = vh_;
    return info;
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
}

void Editor::stopPlay() {
    if (!playing()) return;
    platform::setCursorCaptured(false);
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
void Editor::drawSplitter(const char* id, ImVec2 pos, ImVec2 size, bool horizontal, float& value, float sign) {
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
    ImGui::Begin(id, nullptr, flags);
    ImGui::InvisibleButton("##handle", size);
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(horizontal ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        value += (horizontal ? d.y : d.x) * sign / std::max(uiScale_, 0.01f);
    }
    if (hot) {   // a highlight line along the edge
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
        if (horizontal) dl->AddLine(ImVec2(pos.x, c.y), ImVec2(pos.x + size.x, c.y), IM_COL32(90, 150, 230, 255), 2.f);
        else dl->AddLine(ImVec2(c.x, pos.y), ImVec2(c.x, pos.y + size.y), IM_COL32(90, 150, 230, 255), 2.f);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
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
    Scene now = scene_;
    applyDelta(scene_, redo_.back().delta);
    undo_.push_back({diffScenes(now, scene_), selected_});
    selected_ = redo_.back().selected;
    extraSelected_.clear();
    redo_.pop_back();
    ++editVersion_;
    setStatus("Redo");
}
