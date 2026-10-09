#include "editor/editor.h"

#include <cfloat>
#include <cstdlib>

#include "ImGuizmo.h"
#include "scene/serialization.h"

namespace {
constexpr size_t kMaxUndo = 200;
}

Editor::Editor(MeshLibrary& meshes, float uiScale)
    : meshes_(meshes), uiScale_(uiScale), scene_(buildDefaultScene()) {}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------
void Editor::frame(float dt, const std::string& gpuName, const RenderStats& stats) {
    ImGuiIO& io = ImGui::GetIO();
    // The game sees the keyboard unless a text field has it, and the mouse only over the viewport
    game_.frame(dt, !io.WantTextInput, viewportHovered());
    frameStart_ = {scene_, selected_};
    editBegan_ = false;
    if (statusTimer_ > 0.f) statusTimer_ -= dt;
    fps_ = fps_ * 0.95f + (dt > 0.f ? 1.f / dt : 0.f) * 0.05f;

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
    float leftW = panels && showOutliner_ ? px(250.f) : 0.f, rightW = panels && showInspector_ ? px(340.f) : 0.f;
    vx_ = leftW;
    vy_ = topH_;
    vw_ = std::max(io.DisplaySize.x - leftW - rightW, 1.f);
    vh_ = std::max(io.DisplaySize.y - topH_ - statusH_, 1.f);
    updateView();

    drawMenuBar();
    drawToolbar();
    if (leftW > 0.f) drawOutliner();
    if (rightW > 0.f) drawInspector(gpuName, stats);
    drawStatusBar(stats);
    if (showHelp_) drawHelp();
    drawAddPopup();
    drawImportPopup();

    handleNavigation(dt);
    handleGizmo();
    handlePicking();
    handleShortcuts();
    drawEntityIcons();
    drawColliders();
    drawViewportOverlay();

    // Deferred actions requested from inside list iteration (context menus, drag & drop)
    if (pendingFocus_) { select(pendingFocus_); focusSelected(); }
    if (pendingDuplicate_) { select(pendingDuplicate_); duplicateSelected(); }
    if (pendingDelete_) { select(pendingDelete_); deleteSelected(); }
    if (pendingReparent_) reparent(pendingReparent_, pendingParent_);
    pendingFocus_ = pendingDuplicate_ = pendingDelete_ = pendingReparent_ = pendingParent_ = kNoEntity;

    if (editBegan_ && !playing()) {   // play-mode edits are thrown away on Stop: no undo
        undo_.push_back(std::move(frameStart_));
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
        redo_.clear();
    }
    if (!scene_.find(selected_)) selected_ = kNoEntity;
    updateView();   // the scene or the camera may have changed during this frame
    buildDrawItems();
}

void Editor::updateView() {
    worlds_ = playing() ? game_.worldMatrices() : scene_.worldMatrices();
    CameraView gc;
    gameCameraActive_ = playing() && game_.activeCamera(gc);
    if (gameCameraActive_) {
        viewProj_ = perspective(gc.fovY, aspect(), gc.zNear, gc.zFar, true) * gc.view;
        viewPos_ = gc.position;
    } else {
        viewProj_ = perspective(cam_.fovY, aspect(), 0.1f, 2000.f, true) * cam_.view();
        viewPos_ = cam_.pos;
    }
}

const Mat4& Editor::worldOf(EntityId id) const {
    static const Mat4 identity;
    int i = scene_.indexOf(id);
    return i >= 0 && i < (int)worlds_.size() ? worlds_[(size_t)i] : identity;
}

void Editor::buildDrawItems() {
    drawItems_.clear();
    for (size_t i = 0; i < scene_.entities.size(); ++i) {
        const Entity& e = scene_.entities[i];
        if (!e.meshRenderer || !scene_.visibleInHierarchy(e.id)) continue;
        DrawItem d;
        d.model = worlds_[i];
        d.mesh = e.meshRenderer->mesh;
        d.color = e.meshRenderer->color;
        d.checker = e.meshRenderer->checker;
        d.outlined = e.id == selected_ && !viewingGameCamera();
        drawItems_.push_back(d);
    }
}

SceneRenderInfo Editor::renderInfo() const {
    SceneRenderInfo info;
    info.items = &drawItems_;
    info.viewProj = viewProj_;
    info.camPos = viewPos_;
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
    game_.start(scene_, meshes_);
    setStatus("Playing. Ctrl+P stops and restores the scene.");
}

void Editor::stopPlay() {
    if (!playing()) return;
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
    scrollToSelected_ = true;
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

void Editor::importMesh() {
    std::string path = importPath_;
    if (path.size() >= 2 && path[0] == '~' && (path[1] == '/' || path[1] == '\\'))
        if (const char* home = std::getenv("HOME")) path = home + path.substr(1);
    int before = meshes_.count();
    importError_.clear();
    int id = importObj(meshes_, path, importError_);
    if (id < 0) return;
    addObject(id);
    const Mesh& m = meshes_.get(id);
    setStatus((meshes_.count() > before ? "Imported " : "Already imported: ") + m.name + " (" +
              std::to_string(m.indexCount / 3) + " triangles)");
}

void Editor::duplicateSelected() {
    Entity* e = selection();
    if (!e) return;
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

void Editor::saveToFile() {
    std::string error;
    if (saveScene(scenePath_, scene_, meshes_, error)) setStatus(std::string("Saved ") + scenePath_);
    else setStatus("Save failed: " + error);
}

void Editor::loadFromFile() {
    Scene loaded;
    std::string error;
    if (!loadScene(scenePath_, loaded, meshes_, error)) { setStatus("Load failed: " + error); return; }
    pushUndo();
    scene_ = std::move(loaded);
    selected_ = kNoEntity;
    setStatus(std::string("Loaded ") + scenePath_);
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
    undo_.push_back({scene_, selected_});
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
}

void Editor::undo() {
    if (undo_.empty() || playing()) return;
    redo_.push_back({scene_, selected_});
    scene_ = std::move(undo_.back().scene);
    selected_ = undo_.back().selected;
    undo_.pop_back();
    editBegan_ = false;
    setStatus("Undo");
}

void Editor::redo() {
    if (redo_.empty() || playing()) return;
    undo_.push_back({scene_, selected_});
    scene_ = std::move(redo_.back().scene);
    selected_ = redo_.back().selected;
    redo_.pop_back();
    editBegan_ = false;
    setStatus("Redo");
}
