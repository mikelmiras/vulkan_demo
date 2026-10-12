// Editor: the asset inspector (an asset's id, import settings and users) and moving, renaming and deleting assets
// without breaking what refers to them (assets/asset_db.h).
#include "editor/editor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "assets/asset_db.h"
#include "core/vfs.h"
#include "editor/editor_ui.h"
#include "platform/platform.h"
#include "scene/serialization.h"
#include "script/lua_script.h"

namespace fs = std::filesystem;
using namespace editor_ui;

namespace {

// Does `ref` (a stored path, maybe with "#fragment") name `file` (or something in it, for a folder)?
bool names(const std::string& ref, const fs::path& file) {
    bool hit = false;
    assetdb::retarget(ref, file, file, &hit);
    return hit;
}

// Behaviour fields that name files (scenes, sounds) or prefabs, for retargeting
struct RefFields : FieldVisitor {
    std::function<void(std::string&, FieldRef)> fn;
    explicit RefFields(std::function<void(std::string&, FieldRef)> f) : fn(std::move(f)) {}
    void field(const char*, float&, float, float, float) override {}
    void field(const char*, bool&) override {}
    void field(const char*, Vec3&, float) override {}
    void field(const char*, std::string&) override {}
    void field(const char*, int&, int, int) override {}
    void choice(const char*, int&, const char* const*, int) override {}
    void field(const char*, std::string& v, FieldRef kind) override { fn(v, kind); }
};

// Every behaviour on the scene's entities and its prefabs' entities
template <class F> void forEachEntity(Scene& s, F&& f) {
    for (Entity& e : s.entities) f(e);
    for (Prefab& p : s.prefabs)
        for (Entity& e : p.entities) f(e);
}

// "type": from -> to for behaviours in a scene or prefab document
int retypeBehaviours(json::Value& v, const std::string& from, const std::string& to) {
    int n = 0;
    if (v.isObject()) {
        for (auto& [key, child] : v.members()) {
            if (key == "behaviours" && child.isArray()) {
                for (json::Value& b : child.elements())
                    if (b.isObject() && b["type"].asString() == from) {
                        b.set("type", to);
                        ++n;
                    }
            } else {
                n += retypeBehaviours(child, from, to);
            }
        }
    } else if (v.isArray()) {
        for (json::Value& c : v.elements()) n += retypeBehaviours(c, from, to);
    }
    return n;
}

std::string humanSize(uintmax_t bytes) {
    char buf[32];
    if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%ju bytes", bytes);
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------------------------
std::vector<EntityId> Editor::assetUsers(const fs::path& file) {
    std::vector<EntityId> out;
    for (const Entity& e : scene_.entities) {
        bool uses = false;
        if (const MeshRenderer* mr = e.meshRenderer ? &*e.meshRenderer : nullptr) {
            if (mr->mesh >= 0 && mr->mesh < meshes_.count() && names(meshes_.get(mr->mesh).source, file)) uses = true;
            for (int t : {mr->texture, mr->normalTexture, mr->metalRoughTexture, mr->emissiveTexture})
                if (t >= 0 && t < textures_.count() && names(textures_.get(t).source, file)) uses = true;
        }
        if (e.audioSource && names(e.audioSource->clip, file)) uses = true;
        if (e.animator) {
            std::vector<std::string> clips = {e.animator->clip};
            for (const AnimatorState& s : e.animator->states) clips.push_back(s.clip);
            for (const std::string& c : clips)
                if (const AnimationClip* clip = c.empty() ? nullptr : meshes_.findAnimation(c))
                    if (names(clip->source, file)) uses = true;
        }
        RefFields refs([&](std::string& v, FieldRef kind) {
            if ((kind == FieldRef::Scene || kind == FieldRef::Sound) && names(v, file)) uses = true;
        });
        for (const BehaviourPtr& b : e.behaviours)
            if (b) b->fields(refs);
        if (uses) out.push_back(e.id);
    }
    return out;
}

void Editor::retargetInMemory(const fs::path& from, const fs::path& to) {
    auto fix = [&](std::string& ref) { ref = assetdb::retarget(ref, from, to); };
    for (Mesh& m : meshes_.meshes) fix(m.source);
    for (Texture& t : textures_.textures) fix(t.source);
    for (AnimationClip& c : meshes_.animations) fix(c.source);
    RefFields refs([&](std::string& v, FieldRef kind) {
        if (kind == FieldRef::Scene || kind == FieldRef::Sound) fix(v);
    });
    forEachHeldScene([&](Scene& s) {
        forEachEntity(s, [&](Entity& e) {
            if (e.audioSource) fix(e.audioSource->clip);
            for (BehaviourPtr& b : e.behaviours)
                if (b) b->fields(refs);
        });
    });
    std::string scene = scenePath_;
    fix(scene);
    std::snprintf(scenePath_, sizeof(scenePath_), "%s", scene.c_str());
    fix(project_.startScene);
    fix(project_.icon);
    std::map<std::string, int> thumbs;   // (keyed by absolute path)
    for (auto& [key, id] : thumbnails_) thumbs[assetdb::retarget(key, from, to)] = id;
    thumbnails_ = {thumbs.begin(), thumbs.end()};
    std::string dir = assetDir_.u8string(), sel = selectedAsset_.u8string();
    fix(dir);
    fix(sel);
    assetDir_ = fs::u8path(project::resolve(dir));
    if (!selectedAsset_.empty()) selectedAsset_ = fs::u8path(project::resolve(sel));
}

int Editor::renamePrefabReferences(const std::string& from, const std::string& to) {
    int n = 0;
    RefFields refs([&](std::string& v, FieldRef kind) {
        if (kind == FieldRef::Prefab && v == from) {
            v = to;
            ++n;
        }
    });
    forEachEntity(scene_, [&](Entity& e) {
        for (BehaviourPtr& b : e.behaviours)
            if (b) b->fields(refs);
    });
    return n;
}

void Editor::renameScriptType(const std::string& from, const std::string& to) {
    // Behaviours of the old type: kept as their fields under the new name, then the scripts load again
    forEachHeldScene([&](Scene& s) {
        forEachEntity(s, [&](Entity& e) {
            for (BehaviourPtr& b : e.behaviours) {
                if (!b || b->typeName() != from) continue;
                json::Value bv = behaviourToJson(*b), kept = json::Value::object();
                for (const auto& [key, value] : bv.members())
                    if (key != "type") kept.set(key, value);
                b = BehaviourPtr(std::make_unique<MissingBehaviour>(to, json::write(kept)));
            }
        });
    });
    // ... and in the project's files
    for (const fs::path& ext : {fs::path(".vkw"), fs::path(".prefab")})
        for (const fs::path& file : vfs::list(project::root(), ext.u8string())) {
            std::string text, error;
            json::Value doc;
            if (!vfs::read(file, text) || !json::parse(text, doc, error)) continue;
            if (!retypeBehaviours(doc, from, to)) continue;
            std::ofstream(file, std::ios::binary) << json::write(doc);
        }
}

// ---------------------------------------------------------------------------------------------
// Moving, renaming, deleting
// ---------------------------------------------------------------------------------------------
bool Editor::moveAsset(const fs::path& from, const fs::path& to) {
    if (playing()) { setStatus("Stop the game before moving files"); return false; }
    std::error_code ec;
    bool folder = fs::is_directory(from, ec);
    bool script = !folder && from.extension() == ".lua";
    std::string error;
    if (!assetdb::move(from, to, error)) { setStatus("Can't move: " + error); return false; }
    retargetInMemory(from, to);
    std::vector<fs::path> files;
    int refs = assetdb::retargetProjectFiles(from, to, &files);
    if (script && from.stem() != to.stem()) renameScriptType(from.stem().u8string(), to.stem().u8string());
    if (script || folder) loadScripts();   // (scripts are types by file name)
    scanAssets();
    setStatus("Moved " + project::toStored(from.u8string()) + " to " + project::toStored(to.u8string()) +
              (refs ? ": " + std::to_string(refs) + " references updated in " + std::to_string(files.size()) + " files" : ""));
    return true;
}

void Editor::deleteAsset(const fs::path& path) {
    if (playing()) { setStatus("Stop the game before deleting files"); return; }
    size_t users = assetUsers(path).size();
    bool script = path.extension() == ".lua" || fs::is_directory(path);
    std::string error;
    if (!assetdb::remove(path, error)) { setStatus(error); return; }
    if (selectedAsset_ == path) selectedAsset_.clear();
    thumbnails_.erase(path.u8string());
    if (script) loadScripts();
    scanAssets();
    setStatus("Deleted " + project::toStored(path.u8string()) +
              (users ? " (" + std::to_string(users) + " entities in this scene used it)" : ""));
}

void Editor::drawAssetDialogs() {
    if (openRename_) { ImGui::OpenPopup("Rename asset"); openRename_ = false; }
    if (openDelete_) { ImGui::OpenPopup("Delete asset"); openDelete_ = false; }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Rename asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", project::toStored(renaming_.u8string()).c_str());
        ImGui::TextDisabled("A new name, or a path inside the project to move it (\"textures/%s\")",
                            renaming_.filename().u8string().c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##name", &renameBuffer_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (renaming_.extension() == ".lua")
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.4f, 1.f), "A script's file name is its behaviour's name: scenes are updated");
        if (ImGui::Button("Rename", ImVec2(px(100.f), 0)) || enter) {
            fs::path target = fs::u8path(renameBuffer_);
            target = renameBuffer_.find('/') != std::string::npos ? project::root() / target : renaming_.parent_path() / target;
            if (!renameBuffer_.empty() && target != renaming_) moveAsset(renaming_, target);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(px(100.f), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Delete asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::error_code ec;
        bool folder = fs::is_directory(deleting_, ec);
        ImGui::Text("Delete %s%s?", project::toStored(deleting_.u8string()).c_str(), folder ? " and everything in it" : "");
        size_t users = folder ? 0 : assetUsers(deleting_).size();
        if (users) ImGui::TextColored(ImVec4(1.f, 0.6f, 0.4f, 1.f), "%zu entities in this scene use it.", users);
        ImGui::TextDisabled("This can't be undone.");
        if (ImGui::Button("Delete", ImVec2(px(100.f), 0))) {
            deleteAsset(deleting_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(px(100.f), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------------------------
// The asset inspector
// ---------------------------------------------------------------------------------------------
void Editor::applyImportSettings() {
    std::string error;
    if (!assetdb::setImportSettings(selectedAsset_, editedSettings_, error)) { setStatus(error); return; }
    int reloaded = 0;
    std::string abs = selectedAsset_.u8string();
    for (int i = 0; i < textures_.count(); ++i)
        if (textures_.get(i).source == abs) reloaded += reloadTexture(textures_, i, error) >= 0;
    for (int i = (int)MeshKind::Count; i < meshes_.count(); ++i)
        if (meshes_.get(i).source == abs) reloaded += reloadMesh(meshes_, i, error) >= 0;
    if (audio_) audio_->forgetImportSettings();
    setStatus("Import settings saved" + (reloaded ? " and " + std::to_string(reloaded) + " assets reloaded" : std::string()) +
              (error.empty() ? "" : " (" + error + ")"));
}

void Editor::drawAssetInspector() {
    const fs::path& path = selectedAsset_;
    std::error_code ec;
    if (!fs::exists(path, ec)) { selectedAsset_.clear(); return; }
    AssetKind kind = assetKind(path);
    static const char* kinds[] = {"Folder", "Scene", "Prefab", "Model", "Texture", "Sound", "Script", "File"};
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.f, 1.f), "%s", kinds[(int)kind]);
    ImGui::SameLine();
    ImGui::TextUnformatted(path.filename().u8string().c_str());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFrameHeight());
    if (ImGui::SmallButton("x")) { selectedAsset_.clear(); return; }
    tooltip("Back to the scene");
    ImGui::TextDisabled("%s", project::toStored(path.u8string()).c_str());
    if (kind != AssetKind::Folder) {
        std::string id = assetdb::idOf(path);
        ImGui::TextDisabled("%s, id %s", humanSize(fs::file_size(path, ec)).c_str(), id.empty() ? "(none)" : id.c_str());
        tooltip("Scenes keep this id with the path: the file is found again after it's moved (with its .meta)");
    }
    if (ImGui::Button("Rename / move...")) {
        renaming_ = path;
        renameBuffer_ = path.filename().u8string();
        openRename_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete...")) {
        deleting_ = path;
        openDelete_ = true;
    }
    ImGui::SameLine();
    if (kind != AssetKind::Folder && ImGui::Button("Open")) {
        AssetEntry a{path.filename().u8string(), path, kind};
        openAsset(a);
    }

    // Import settings, by kind
    if (settingsFor_ != path) {
        editedSettings_ = assetdb::importSettings(path);
        settingsFor_ = path;
    }
    json::Value& s = editedSettings_;
    std::string ext = path.extension().u8string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    bool hasSettings = kind == AssetKind::Texture || kind == AssetKind::Model || kind == AssetKind::Sound;
    if (hasSettings) ImGui::SeparatorText("Import settings");
    if (kind == AssetKind::Texture) {
        static const int sizes[] = {0, 4096, 2048, 1024, 512, 256, 128, 64};
        static const char* labels[] = {"Original", "4096", "2048", "1024", "512", "256", "128", "64"};
        int cur = 0;
        for (int i = 0; i < 8; ++i)
            if (sizes[i] == s["maxSize"].asInt(0)) cur = i;
        if (ImGui::Combo("Max size", &cur, labels, 8)) s.set("maxSize", sizes[cur]);
        tooltip("Larger images are scaled down when loaded (less memory, faster loading)");
        int filter = s["filter"].asString() == "nearest" ? 1 : 0;
        const char* filters[] = {"Smooth (linear)", "Blocky (nearest)"};
        if (ImGui::Combo("Filter", &filter, filters, 2)) s.set("filter", filter ? "nearest" : "linear");
        tooltip("Blocky: pixel art stays crisp up close");
        bool mips = s["mipmaps"].asBool(true);
        if (ImGui::Checkbox("Mipmaps", &mips)) s.set("mipmaps", mips);
        tooltip("Smaller copies for distant surfaces (no shimmering); off for UI images seen at their own size");
    } else if (kind == AssetKind::Model) {
        float scale = s["scale"].asFloat(1.f);
        if (ImGui::DragFloat("Scale", &scale, 0.01f, 0.0001f, 10000.f, "%.4g")) s.set("scale", scale);
        if (ext == ".obj") {
            bool fit = s["fitToUnitCube"].asBool(true);
            if (ImGui::Checkbox("Fit to a unit cube", &fit)) s.set("fitToUnitCube", fit);
            tooltip("Centred and sized so its largest side is 1 (then times the scale); off: as modelled");
        } else {
            ImGui::TextDisabled("glTF: the scale goes on the prefab's root when the model is imported");
        }
    } else if (kind == AssetKind::Sound) {
        float volume = s["volume"].asFloat(1.f);
        if (ImGui::SliderFloat("Volume", &volume, 0.f, 2.f, "%.2f")) s.set("volume", volume);
        tooltip("A gain on every play of this file (on top of the Audio Source's volume)");
        bool stream = s["stream"].asBool(false);
        if (ImGui::Checkbox("Stream by default", &stream)) s.set("stream", stream);
        tooltip("Audio Sources made from it decode it as it plays (long music and ambience)");
    } else if (kind == AssetKind::Script) {
        ImGui::SeparatorText("Script");
        std::string type = path.stem().u8string();
        if (scripting::isScriptType(type)) ImGui::Text("Behaviour type: %s", type.c_str());
        else ImGui::TextDisabled("Not loaded as a behaviour (see the errors below, or it's in scripts/lib)");
        for (const std::string& e : scripting::errors())
            if (e.find("/" + path.filename().u8string() + ":") != std::string::npos)
                ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f), "%s", e.c_str());
    }
    if (hasSettings) {
        bool changed = json::write(s) != json::write(assetdb::importSettings(path));
        ImGui::BeginDisabled(!changed || playing());
        if (ImGui::Button("Apply")) applyImportSettings();
        ImGui::SameLine();
        if (ImGui::Button("Revert")) settingsFor_.clear();
        ImGui::EndDisabled();
    }

    if (kind != AssetKind::Folder && kind != AssetKind::Script) {
        ImGui::SeparatorText("Used in this scene");
        std::vector<EntityId> users = assetUsers(path);
        if (users.empty()) ImGui::TextDisabled("By nothing");
        for (EntityId id : users)
            if (const Entity* e = scene_.find(id))
                if (ImGui::Selectable((e->name + "##" + std::to_string(id)).c_str())) pendingFocus_ = id;
    }
}
