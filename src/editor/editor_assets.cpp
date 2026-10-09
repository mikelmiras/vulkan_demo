// Editor: the project and the asset browser (scenes, prefabs, models, textures, sounds), with drag & drop.
#include "editor/editor.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "platform/platform.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kAssetPayload = "VKW_ASSET";   // an absolute path, NUL terminated

double now() { return ImGui::GetTime(); }

const char* kindLabel(int kind) {
    static const char* labels[] = {"folder", "scene", "prefab", "model", "texture", "sound", "file"};
    return labels[kind];
}

ImVec4 kindColor(int kind) {
    static const ImVec4 colors[] = {{0.85f, 0.75f, 0.45f, 1}, {0.55f, 0.85f, 0.55f, 1}, {0.55f, 0.75f, 1.f, 1},
                                    {0.95f, 0.6f, 0.4f, 1},   {0.85f, 0.55f, 0.9f, 1},  {0.5f, 0.85f, 0.85f, 1},
                                    {0.6f, 0.6f, 0.6f, 1}};
    return colors[kind];
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
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") return AssetKind::Texture;
    if (ext == ".wav" || ext == ".mp3" || ext == ".flac") return AssetKind::Sound;
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
    std::error_code ec;
    if (fs::exists(project::resolve(project_.startScene), ec)) {
        openScene(project_.startScene);
    } else {
        std::snprintf(scenePath_, sizeof(scenePath_), "%s", project_.startScene.c_str());
        setStatus("Opened project " + project_.name + " (" + project::root().u8string() + ")");
    }
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
    ImGui::SetNextWindowPos(ImVec2(vx_, vy_ + vh_));
    ImGui::SetNextWindowSize(ImVec2(vw_, assetsH_));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    ImGui::Begin("##assets", nullptr, flags);

    // Header: where we are in the project
    ImGui::TextColored(kindColor(0), "Assets");
    ImGui::SameLine();
    fs::path rel = assetDir_.lexically_relative(project::root());
    bool atRoot = rel.empty() || rel == ".";
    ImGui::BeginDisabled(atRoot);
    if (ImGui::SmallButton("Up")) {
        assetDir_ = assetDir_.parent_path();
        scanAssets();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s/%s", project_.name.c_str(), atRoot ? "" : rel.generic_u8string().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) scanAssets();
    ImGui::SameLine();
    ImGui::TextDisabled("| double-click to open, drag onto the viewport or the Inspector");

    ImGui::BeginChild("##asset-grid");
    const float cell = px(112.f), h = ImGui::GetTextLineHeightWithSpacing() * 2.f + px(6.f);
    int columns = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (cell + ImGui::GetStyle().ItemSpacing.x)));
    if (assets_.empty()) ImGui::TextDisabled("No scenes, prefabs, models, textures or sounds here.");
    AssetEntry open;
    bool doOpen = false;
    for (size_t i = 0; i < assets_.size(); ++i) {
        const AssetEntry& a = assets_[i];
        if (i % (size_t)columns) ImGui::SameLine();
        ImGui::PushID((int)i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##cell", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cell, h)) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            open = a;
            doOpen = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s\n%s", project::toStored(a.path.u8string()).c_str(), kindLabel((int)a.kind));
        if (a.kind != AssetKind::Folder && ImGui::BeginDragDropSource()) {
            std::string path = a.path.u8string();
            ImGui::SetDragDropPayload(kAssetPayload, path.c_str(), path.size() + 1);
            ImGui::Text("%s %s", kindLabel((int)a.kind), a.name.c_str());
            ImGui::EndDragDropSource();
        }
        // Two lines in the cell: the kind (coloured) and the name (clipped)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(p.x + px(4.f), p.y + px(2.f)), ImGui::ColorConvertFloat4ToU32(kindColor((int)a.kind)),
                    kindLabel((int)a.kind));
        ImVec4 clip(p.x, p.y, p.x + cell - px(4.f), p.y + h);
        dl->AddText(nullptr, 0.f, ImVec2(p.x + px(4.f), p.y + px(2.f) + ImGui::GetTextLineHeightWithSpacing()),
                    ImGui::GetColorU32(ImGuiCol_Text), a.name.c_str(), nullptr, 0.f, &clip);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
    if (doOpen) openAsset(open);
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
        case AssetKind::Other: break;
    }
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
