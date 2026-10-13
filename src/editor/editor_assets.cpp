// Editor: the project and the asset browser (scenes, prefabs, models, textures, sounds), with drag & drop.
#include "editor/editor.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "app/export.h"
#include "assets/asset_db.h"
#include "script/lua_script.h"
#include "editor/editor_ui.h"
#include "platform/platform.h"
#include "scene/serialization.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kAssetPayload = "VKW_ASSET";   // an absolute path, NUL terminated

double now() { return ImGui::GetTime(); }

const char* kindLabel(int kind) {
    static const char* labels[] = {"folder", "scene", "prefab", "model", "texture", "sound", "script", "file"};
    return labels[kind];
}

ImVec4 kindColor(int kind) {
    static const ImVec4 colors[] = {{0.85f, 0.75f, 0.45f, 1}, {0.55f, 0.85f, 0.55f, 1}, {0.55f, 0.75f, 1.f, 1},
                                    {0.95f, 0.6f, 0.4f, 1},   {0.85f, 0.55f, 0.9f, 1},  {0.5f, 0.85f, 0.85f, 1},
                                    {0.95f, 0.85f, 0.5f, 1},  {0.6f, 0.6f, 0.6f, 1}};
    return colors[kind];
}


// Line drawings for the kinds of asset without a picture of their own (folder, scene, prefab, model, image, sound)
void drawAssetIcon(ImDrawList* dl, int kind, ImVec2 o, float s, ImU32 col) {
    auto P = [&](float x, float y) { return ImVec2(o.x + x * s, o.y + y * s); };
    const float t = std::max(1.5f, s * 0.035f);
    switch (kind) {
    case 0:   // folder
        dl->AddRectFilled(P(0.1f, 0.25f), P(0.45f, 0.35f), col, s * 0.04f);
        dl->AddRectFilled(P(0.1f, 0.32f), P(0.9f, 0.8f), col, s * 0.05f);
        break;
    case 1:   // scene: a landscape with a sun
        dl->AddRect(P(0.12f, 0.18f), P(0.88f, 0.82f), col, s * 0.05f, 0, t);
        dl->AddTriangleFilled(P(0.18f, 0.76f), P(0.42f, 0.42f), P(0.62f, 0.76f), col);
        dl->AddTriangleFilled(P(0.48f, 0.76f), P(0.66f, 0.52f), P(0.82f, 0.76f), col);
        dl->AddCircleFilled(P(0.7f, 0.34f), s * 0.07f, col);
        break;
    case 2:   // prefab: a cube with a plus (a template)
    case 3: {  // model: a wire cube
        ImVec2 f0 = P(0.18f, 0.38f), f1 = P(0.62f, 0.38f), f2 = P(0.62f, 0.82f), f3 = P(0.18f, 0.82f);
        ImVec2 d = ImVec2(s * 0.2f, -s * 0.2f);
        auto off = [&](ImVec2 v) { return ImVec2(v.x + d.x, v.y + d.y); };
        dl->AddQuad(f0, f1, f2, f3, col, t);
        dl->AddLine(f0, off(f0), col, t);
        dl->AddLine(f1, off(f1), col, t);
        dl->AddLine(f2, off(f2), col, t);
        dl->AddLine(off(f0), off(f1), col, t);
        dl->AddLine(off(f1), off(f2), col, t);
        if (kind == 2) {
            dl->AddLine(P(0.4f, 0.5f), P(0.4f, 0.7f), col, t);
            dl->AddLine(P(0.3f, 0.6f), P(0.5f, 0.6f), col, t);
        }
        break;
    }
    case 5:   // sound: a waveform
        for (int i = 0; i < 9; ++i) {
            float x = 0.14f + i * 0.09f, hgt = 0.08f + 0.3f * std::fabs(std::sin(i * 1.7f + 0.4f));
            dl->AddRectFilled(P(x, 0.5f - hgt), P(x + 0.05f, 0.5f + hgt), col, s * 0.02f);
        }
        break;
    case 6:   // script: a page of code
        dl->AddRect(P(0.22f, 0.12f), P(0.78f, 0.88f), col, s * 0.04f, 0, t);
        for (int i = 0; i < 5; ++i) {
            float indent = (i == 1 || i == 2) ? 0.1f : 0.f, w = 0.18f + 0.06f * (float)((i * 7) % 4);
            dl->AddLine(P(0.32f + indent, 0.26f + i * 0.12f), P(0.32f + indent + w, 0.26f + i * 0.12f), col, t);
        }
        break;
    default:   // an image not shown yet, or anything else: a page
        dl->AddRect(P(0.22f, 0.12f), P(0.78f, 0.88f), col, s * 0.04f, 0, t);
        break;
    }
}

}  // namespace

Editor::AssetKind Editor::assetKind(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) return AssetKind::Folder;
    std::string ext = path.extension().u8string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".vkw") return AssetKind::Scene;
    if (ext == ".prefab") return AssetKind::Prefab;
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb") return AssetKind::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr")
        return AssetKind::Texture;
    if (ext == ".wav" || ext == ".mp3" || ext == ".flac") return AssetKind::Sound;
    if (ext == ".lua") return AssetKind::Script;
    return AssetKind::Other;
}

// ---------------------------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------------------------
void Editor::openProject(const fs::path& dir) {
    if (playing()) stopPlay();
    project::setRoot(dir);
    assetDir_ = project::root();
    assetsScannedAt_ = -1.0;
    std::string error;
    project::Settings settings;
    if (project::exists(dir)) {
        if (!project::load(dir, settings, error)) { setStatus(error); return; }
    } else {   // a new project: remember it with a project.json
        settings.name = project::root().filename().u8string();
        if (!project::save(dir, settings, error)) { setStatus(error); return; }
    }
    project_ = settings;
    editor_prefs::addRecent(recentProjects_, project::root().u8string());
    assetdb::open(project::root(), true);
    selectedAsset_.clear();
    loadScripts();
    bool sameModule = module_.loaded() && !project_.module.empty() && !module_.changedOnDisk() &&
                      module_.library() == GameModule::libraryPath(fs::u8path(project::resolve(project_.module)));
    if (!sameModule) loadGameModule();   // (the editor may have loaded it already, for the starting project)
    std::error_code ec;
    if (fs::exists(project::resolve(project_.startScene), ec)) {
        openScene(project_.startScene);
    } else {
        std::snprintf(scenePath_, sizeof(scenePath_), "%s", project_.startScene.c_str());
        setStatus("Opened project " + project_.name + " (" + project::root().u8string() + ")");
    }
}

void Editor::exportGameTo(const fs::path& dir, const ExportOptions& options) {
    ExportResult result;
    std::string error;
    if (!exportGame(project::root(), dir, platform::executableDir(), result, error, options)) {
        setStatus("Export failed: " + error);
        return;
    }
    for (const std::string& w : result.warnings) std::fprintf(stderr, "export: %s\n", w.c_str());
    setStatus("Exported " + std::to_string(result.files) + " files: run " + result.executable.u8string() +
              (result.package.empty() ? "" : "; packaged as " + result.package.filename().u8string()) +
              (result.updatePack.empty() ? "" : "; update pack " + result.updatePack.filename().u8string()) +
              (result.warnings.empty() ? "" : " (" + std::to_string(result.warnings.size()) + " warnings: see the console)"));
}

// ---------------------------------------------------------------------------------------------
// Game module
// ---------------------------------------------------------------------------------------------
void Editor::loadGameModule() {
    if (playing()) stopPlay();
    // The old code's behaviours become their saved fields (the code is about to go away), then come back as
    // the new code's
    std::vector<std::string> types = module_.behaviourTypes();
    forEachHeldScene([&](Scene& s) { detachBehaviours(s, types); });
    module_.unload();
    std::string error;
    bool ok = true;
    if (!project_.module.empty()) {
        fs::path lib = GameModule::libraryPath(fs::u8path(project::resolve(project_.module)));
        ok = module_.load(lib, error);
    }
    int restored = 0;
    forEachHeldScene([&](Scene& s) { restored += reattachBehaviours(s); });
    (void)restored;
    if (project_.module.empty()) return;
    if (!ok) {
        setStatus("Game module not loaded: " + error);
        std::fprintf(stderr, "%s\n", error.c_str());
        return;
    }
    std::printf("[module] loaded %s: %zu behaviours\n", module_.library().u8string().c_str(), module_.behaviourTypes().size());
    std::fflush(stdout);
    int missing = countMissingBehaviours(scene_);
    setStatus("Game module loaded: " + std::to_string(module_.behaviourTypes().size()) + " behaviours" +
              (missing ? ", " + std::to_string(missing) + " in the scene still have no code" : ""));
}

void Editor::rebuildGameModule() {
    if (moduleBuild_.valid()) return;   // already building
    if (project_.moduleBuild.empty()) {
        loadGameModule();
        return;
    }
    std::string command = project_.moduleBuild;
#ifdef VKWORLD_BUILD_DIR
    for (size_t at; (at = command.find("{engineBuild}")) != std::string::npos;)
        command.replace(at, 13, "\"" VKWORLD_BUILD_DIR "\"");
#endif
    fs::path dir = project::root();
    setStatus("Building the game module...");
    moduleBuild_ = std::async(std::launch::async, [command, dir] {
        std::string cd = "cd \"" + dir.u8string() + "\" && " + command;
        std::printf("[module] %s\n", cd.c_str());
        std::fflush(stdout);
        return std::system(cd.c_str());
    });
}

void Editor::pollGameModule() {
    if (moduleBuild_.valid() && moduleBuild_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        int code = moduleBuild_.get();
        if (code != 0) setStatus("Game module build failed (exit code " + std::to_string(code) + "): see the console");
        else loadGameModule();
        return;
    }
    if (moduleBuild_.valid() || project_.module.empty()) return;
    double t = now();
    if (t - moduleCheckedAt_ < 1.0) return;
    moduleCheckedAt_ = t;
    bool changed = module_.loaded() ? module_.changedOnDisk()
                                     : fs::exists(GameModule::libraryPath(fs::u8path(project::resolve(project_.module))));
    if (!changed) { moduleChangedWhilePlaying_ = false; return; }
    if (playing()) {   // swapping code under a running game: wait for Stop
        if (!moduleChangedWhilePlaying_) setStatus("The game module was rebuilt: it reloads when you stop");
        moduleChangedWhilePlaying_ = true;
        return;
    }
    moduleChangedWhilePlaying_ = false;
    if (!module_.loaded()) {   // it failed before: only retry when the file changes
        static fs::file_time_type lastTried{};
        std::error_code ec;
        fs::file_time_type mt = fs::last_write_time(GameModule::libraryPath(fs::u8path(project::resolve(project_.module))), ec);
        if (ec || mt == lastTried) return;
        lastTried = mt;
    }
    loadGameModule();
}

// ---------------------------------------------------------------------------------------------
// Browser
// ---------------------------------------------------------------------------------------------
void Editor::scanAssets() {
    assets_.clear();
    std::error_code ec;
    if (!fs::is_directory(assetDir_, ec)) assetDir_ = project::root();
    for (const auto& entry : fs::directory_iterator(assetDir_, fs::directory_options::skip_permission_denied, ec)) {
        std::string name = entry.path().filename().u8string();
        if (name.empty() || name[0] == '.' || name == "CMakeFiles") continue;   // hidden and build clutter
        AssetKind kind = assetKind(entry.path());
        if (kind == AssetKind::Other) continue;
        if (kind != AssetKind::Folder) assetdb::idOf(entry.path());   // (its .meta: moved along, it keeps references)
        assets_.push_back({name, entry.path(), kind});
    }
    std::sort(assets_.begin(), assets_.end(), [](const AssetEntry& a, const AssetEntry& b) {
        if ((a.kind == AssetKind::Folder) != (b.kind == AssetKind::Folder)) return a.kind == AssetKind::Folder;
        return a.name < b.name;
    });
    assetsScannedAt_ = now();
}

void Editor::drawAssetBrowser() {
    if (assetsScannedAt_ < 0.0 || now() - assetsScannedAt_ > 2.0) scanAssets();   // picks up new files on its own
    ImGui::SetNextWindowSize(ImVec2(px(700.f), px(180.f)), ImGuiCond_FirstUseEver);
    if (!editor_ui::beginPanel("Assets", &showAssets_)) {
        ImGui::End();
        return;
    }

    // Header: where we are in the project
    fs::path rel = assetDir_.lexically_relative(project::root());
    bool atRoot = rel.empty() || rel == ".";
    ImGui::BeginDisabled(atRoot);
    if (ImGui::SmallButton("Up")) {
        assetDir_ = assetDir_.parent_path();
        scanAssets();
    }
    if (!atRoot && ImGui::BeginDragDropTarget()) {   // dropped on Up: moved to the folder above
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            fs::path from = fs::u8path((const char*)pl->Data);
            pendingMove_ = {from, assetDir_.parent_path() / from.filename()};
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s/%s", project_.name.c_str(), atRoot ? "" : rel.generic_u8string().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) scanAssets();
    ImGui::SameLine();
    if (ImGui::SmallButton("New folder")) {
        std::error_code ec;
        fs::path dir = assetDir_ / "New folder";
        for (int i = 2; fs::exists(dir, ec); ++i) dir = assetDir_ / ("New folder " + std::to_string(i));
        fs::create_directory(dir, ec);
        scanAssets();
        renaming_ = dir;
        renameBuffer_ = dir.filename().u8string();
        openRename_ = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("New script")) newScript();
    editor_ui::tooltip("A Lua behaviour in scripts/, from a template");
    ImGui::SameLine();
    ImGui::TextDisabled("| click: inspect, double-click: open, right-click: rename / delete, drag onto a folder to move");

    ImGui::BeginChild("##asset-grid");
    const float cell = px(112.f), icon = px(64.f), h = icon + ImGui::GetTextLineHeightWithSpacing() * 2.f + px(10.f);
    int thumbBudget = 2;   // images decoded per frame (the rest next frames)
    int columns = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (cell + ImGui::GetStyle().ItemSpacing.x)));
    if (assets_.empty()) ImGui::TextDisabled("No scenes, prefabs, models, textures or sounds here.");
    AssetEntry open;
    bool doOpen = false;
    for (size_t i = 0; i < assets_.size(); ++i) {
        const AssetEntry& a = assets_[i];
        if (i % (size_t)columns) ImGui::SameLine();
        ImGui::PushID((int)i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##cell", a.path == selectedAsset_, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cell, h))) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                open = a;
                doOpen = true;
            } else {   // one click: in the Inspector
                selectedAsset_ = a.path;
                select(kNoEntity);
            }
        }
        if (ImGui::BeginPopupContextItem("##asset-menu")) {
            if (ImGui::MenuItem("Open")) { open = a; doOpen = true; }
            if (ImGui::MenuItem("Show in Inspector")) { selectedAsset_ = a.path; select(kNoEntity); }
            if (ImGui::MenuItem("Rename / move...")) {
                renaming_ = a.path;
                renameBuffer_ = a.path.filename().u8string();
                openRename_ = true;
            }
            if (ImGui::MenuItem("Delete...")) {
                deleting_ = a.path;
                openDelete_ = true;
            }
            ImGui::EndPopup();
        }
        if (a.kind == AssetKind::Folder && ImGui::BeginDragDropTarget()) {   // an asset dropped on a folder: moved in
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                fs::path from = fs::u8path((const char*)pl->Data);
                pendingMove_ = {from, a.path / from.filename()};
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s\n%s", project::toStored(a.path.u8string()).c_str(), kindLabel((int)a.kind));
        if (a.kind != AssetKind::Folder && ImGui::BeginDragDropSource()) {
            std::string path = a.path.u8string();
            ImGui::SetDragDropPayload(kAssetPayload, path.c_str(), path.size() + 1);
            ImGui::Text("%s %s", kindLabel((int)a.kind), a.name.c_str());
            ImGui::EndDragDropSource();
        }
        // The thumbnail (images) or an icon for the kind, then the kind (coloured) and the name (clipped)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 i0(p.x + (cell - icon) * 0.5f, p.y + px(3.f)), i1(i0.x + icon, i0.y + icon);
        ImU32 kc = ImGui::ColorConvertFloat4ToU32(kindColor((int)a.kind));
        float aspect = 1.f;
        if (ImTextureID tex = thumbnail(a, thumbBudget, aspect)) {   // fitted in the square, its shape kept
            dl->AddRectFilled(i0, i1, IM_COL32(40, 40, 46, 255));
            float w = aspect >= 1.f ? icon : icon * aspect, hh = aspect >= 1.f ? icon / aspect : icon;
            ImVec2 c((i0.x + i1.x) * 0.5f, (i0.y + i1.y) * 0.5f);
            dl->AddImage(tex, ImVec2(c.x - w * 0.5f, c.y - hh * 0.5f), ImVec2(c.x + w * 0.5f, c.y + hh * 0.5f));
        } else {
            drawAssetIcon(dl, (int)a.kind, i0, icon, kc);
        }
        float ty = i1.y + px(3.f);
        dl->AddText(ImVec2(p.x + px(4.f), ty), kc, kindLabel((int)a.kind));
        ImVec4 clip(p.x, p.y, p.x + cell - px(4.f), p.y + h);
        dl->AddText(nullptr, 0.f, ImVec2(p.x + px(4.f), ty + ImGui::GetTextLineHeightWithSpacing()),
                    ImGui::GetColorU32(ImGuiCol_Text), a.name.c_str(), nullptr, 0.f, &clip);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
    if (doOpen) openAsset(open);
    if (!pendingMove_.first.empty()) {
        auto move = pendingMove_;
        pendingMove_ = {};
        if (move.first != move.second) moveAsset(move.first, move.second);
    }
}

// An image's thumbnail, made the first time it's shown (a few per frame)
ImTextureID Editor::thumbnail(const AssetEntry& a, int& budget, float& aspect) {
    if (a.kind != AssetKind::Texture || !uiTextures_) return (ImTextureID)0;
    std::string key = a.path.u8string();
    auto it = thumbnails_.find(key);
    if (it == thumbnails_.end()) {
        if (budget <= 0) return (ImTextureID)0;
        --budget;
        std::string error;
        it = thumbnails_.emplace(key, addThumbnail(textures_, "thumbnail: " + key, key, 96, error)).first;
    }
    if (it->second < 0) return (ImTextureID)0;
    const Texture& t = textures_.get(it->second);
    aspect = t.height ? (float)t.width / (float)t.height : 1.f;
    return uiTextures_(t.name);
}

void Editor::assignTexture(EntityId id, const fs::path& file) {
    Entity* e = scene_.find(id);
    if (!e || !e->meshRenderer) { setStatus("Textures go on objects with a mesh"); return; }
    std::string error;
    int tex = loadTexture(textures_, file.u8string(), error);
    if (tex < 0) { setStatus("Texture failed: " + error); return; }
    pushUndo();
    scene_.find(id)->meshRenderer->texture = tex;
    setStatus("Textured " + e->name + " with " + textures_.get(tex).name);
}

void Editor::openAsset(const AssetEntry& a) {
    switch (a.kind) {
        case AssetKind::Folder:
            assetDir_ = a.path;
            scanAssets();
            break;
        case AssetKind::Scene:
            if (playing()) stopPlay();
            openScene(a.path.u8string());
            break;
        case AssetKind::Prefab: importPrefabFile(a.path); break;
        case AssetKind::Model:
            importModel(a.path.u8string());
            if (!importError_.empty()) setStatus("Import failed: " + importError_);
            break;
        case AssetKind::Texture:
            if (selection()) assignTexture(selected_, a.path);
            else setStatus("Select an object first, or drag the texture onto one");
            break;
        case AssetKind::Sound:
            if (audio_) {
                AudioEngine::PlayParams p;
                if (!audio_->play(a.path.u8string(), p)) setStatus("Can't play: " + audio_->error());
            }
            break;
        case AssetKind::Script:   // in the system's editor for .lua files; saving it reloads the script
            if (!platform::openWithSystem(a.path)) setStatus("Can't open " + a.path.u8string() + " (no program for it?)");
            break;
        case AssetKind::Other: break;
    }
}

// ---------------------------------------------------------------------------------------------
// Lua scripts
// ---------------------------------------------------------------------------------------------
void Editor::loadScripts() {
    if (playing()) stopPlay();
    std::vector<std::string> types;   // the old project's script behaviours: kept as their fields
    for (const BehaviourType& t : behaviourTypes())
        if (t.module == "lua") types.push_back(t.name);
    forEachHeldScene([&](Scene& s) { detachBehaviours(s, types); });
    int n = scripting::loadProject(project::root());
    forEachHeldScene([&](Scene& s) { reattachBehaviours(s); });
    scriptErrorsShown_ = scripting::errors().size();
    if (n) std::printf("[scripts] %d Lua behaviours\n", n);
    if (!scripting::errors().empty()) setStatus("Script error: " + scripting::errors().front());
}

void Editor::pollScripts() {
    std::vector<std::string> loaded = scripting::poll();
    std::vector<std::string> errors = scripting::errors();
    if (!loaded.empty()) {
        forEachHeldScene([&](Scene& s) { reattachBehaviours(s); });   // (new scripts: their behaviours come back)
        std::string names;
        for (const std::string& n : loaded) names += (names.empty() ? "" : ", ") + n;
        setStatus("Reloaded script" + std::string(loaded.size() > 1 ? "s " : " ") + names +
                  (playing() ? " (running instances keep their state)" : ""));
    }
    if (errors.size() > scriptErrorsShown_ || (!loaded.empty() && !errors.empty()))
        setStatus("Script error: " + errors.back());
    scriptErrorsShown_ = errors.size();
}

void Editor::newScript() {
    fs::path dir = project::root() / "scripts";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string name;
    for (int i = 1; i < 1000; ++i) {
        name = "NewScript" + (i > 1 ? std::to_string(i) : std::string());
        if (!fs::exists(dir / (name + ".lua"), ec) && !createBehaviour(name)) break;
    }
    fs::path file = dir / (name + ".lua");
    std::ofstream out(file, std::ios::binary);
    out << scripting::templateSource(name);
    out.close();
    if (!out) { setStatus("Can't write " + file.u8string()); return; }
    scripting::poll(true);
    assetDir_ = dir;
    scanAssets();
    setStatus("Created scripts/" + name + ".lua (rename the file to rename the behaviour; double-click to edit)");
}

// ---------------------------------------------------------------------------------------------
// Drag & drop
// ---------------------------------------------------------------------------------------------
// The viewport isn't an ImGui item, so drops on it are handled by hand: an asset payload released over it
void Editor::handleViewportDrop() {
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (!payload || !payload->IsDataType(kAssetPayload) || !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
    ImVec2 mp = ImGui::GetIO().MousePos;
    if (mp.x < vx_ || mp.x >= vx_ + vw_ || mp.y < vy_ || mp.y >= vy_ + vh_ || viewingGameCamera()) return;
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) return;   // over a panel or popup
    fs::path file = fs::u8path((const char*)payload->Data);
    AssetKind kind = assetKind(file);

    // Where it lands: the surface under the mouse, else the ground plane (or 10 m away)
    EntityId target = pickAt(mp);
    Vec3 origin, dir;
    mouseRay(mp, origin, dir);
    Vec3 at = origin + dir * 10.f;
    if (dir.y < -0.01f) {
        float t = -origin.y / dir.y;
        if (t > 0.f && t < 200.f) at = origin + dir * t;
    }
    switch (kind) {
        case AssetKind::Model:
            importModel(file.u8string(), &at);
            if (!importError_.empty()) setStatus("Import failed: " + importError_);
            break;
        case AssetKind::Prefab: importPrefabFile(file, &at); break;
        case AssetKind::Scene: openScene(file.u8string()); break;
        case AssetKind::Texture:
            if (target) assignTexture(target, file);
            else setStatus("Drop textures on an object");
            break;
        case AssetKind::Sound:
            if (target) {   // the object gets (or changes) its Audio Source
                pushUndo();
                Entity* e = scene_.find(target);
                if (!e->audioSource) e->audioSource = AudioSource{};
                e->audioSource->clip = project::toStored(file.u8string());
                e->audioSource->stream = assetdb::importSettings(file)["stream"].asBool(e->audioSource->stream);
                select(target);
                setStatus(e->name + " now plays " + file.filename().u8string());
            }
            break;
        default: break;
    }
}

bool Editor::acceptAssetDrop(AssetKind kind, fs::path& out) {
    bool accepted = false;
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* peek = ImGui::GetDragDropPayload();
        if (peek && peek->IsDataType(kAssetPayload) && assetKind(fs::u8path((const char*)peek->Data)) == kind)
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                out = fs::u8path((const char*)p->Data);
                accepted = true;
            }
        ImGui::EndDragDropTarget();
    }
    return accepted;
}
