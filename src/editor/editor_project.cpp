// The editor's project windows: the launcher (shown at start when no project was given: recent projects, a new one
// from a template, opening a folder, or the demo scene), Project Settings (everything in project.json) and Build
// Settings (the build profiles)
#include "editor/editor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app/export.h"
#include "app/updates.h"
#include "core/project.h"
#include "core/version.h"
#include "core/vfs.h"
#include "editor/editor_ui.h"
#include "game/strings.h"
#include "imgui.h"
#include "platform/platform.h"
#include "scene/project_templates.h"

namespace fs = std::filesystem;
using namespace editor_ui;

namespace {

// A line saying how the project's engine version stands (empty when it's this engine's)
std::string engineNote(const std::string& projectEngine) {
    switch (project::engineMatch(projectEngine, version::string())) {
    case project::EngineMatch::Newer: return "made with a newer engine (" + projectEngine + "): this one may not read all of it";
    case project::EngineMatch::Older: return "made with engine " + projectEngine + " (saving it updates that)";
    case project::EngineMatch::Unknown: return "from before engines recorded their version";
    default: return "";
    }
}

}  // namespace

void Editor::showLauncher() {
    showLauncher_ = true;
    launcherInfo_.clear();   // (read again: projects may have changed)
}

void Editor::drawLauncher() {
    if (!showLauncher_) return;
    if (launcherInfo_.empty())
        for (const std::string& dir : recentProjects_) launcherInfo_.push_back(project::describe(fs::u8path(dir)));
    const ImGuiViewport* vp = ImGui::GetMainViewport();   // (over the whole window: nothing else to do until a choice)
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowBgAlpha(1.f);
    ImGui::SetNextWindowFocus();
    ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoDocking);
    const float s = uiScale_;
    ImGui::SetCursorPos(ImVec2(40.f * s, 30.f * s));
    ImGui::SetWindowFontScale(1.8f);
    ImGui::TextUnformatted("Vulkan World");
    ImGui::SetWindowFontScale(1.f);
    ImGui::SetCursorPosX(40.f * s);
    ImGui::TextDisabled("Engine %s", version::string());
    ImGui::Dummy(ImVec2(0.f, 16.f * s));

    std::string open;
    float column = std::max(vp->WorkSize.x * 0.55f, 420.f * s);
    ImGui::SetCursorPosX(40.f * s);
    ImGui::BeginChild("##recent", ImVec2(column, -60.f * s), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Recent projects");
    if (launcherInfo_.empty()) ImGui::TextDisabled("None yet: make one on the right, or open a project folder.");
    std::string forget;
    for (const project::Info& info : launcherInfo_) {
        ImGui::PushID(info.dir.u8string().c_str());
        ImGui::BeginDisabled(!info.found);
        std::string title = info.found ? info.name : info.dir.filename().u8string();
        if (ImGui::Selectable(("##" + title).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.f, ImGui::GetTextLineHeight() * 2.4f)) &&
            info.found)
            open = info.dir.u8string();
        ImGui::EndDisabled();
        ImGui::SameLine(12.f * s);
        ImGui::BeginGroup();
        ImGui::TextUnformatted(title.c_str());
        if (info.found) {
            std::string note = engineNote(info.engineVersion);
            ImGui::TextDisabled("%s   version %s%s%s", info.dir.u8string().c_str(), info.gameVersion.c_str(), note.empty() ? "" : "   ",
                                note.c_str());
        } else {
            ImGui::TextDisabled("%s: %s", info.dir.u8string().c_str(), info.error.c_str());
        }
        ImGui::EndGroup();
        if (!info.found) {
            ImGui::SameLine(column - 90.f * s);
            if (ImGui::SmallButton("Forget")) forget = info.dir.u8string();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (!forget.empty()) {
        recentProjects_.erase(std::remove(recentProjects_.begin(), recentProjects_.end(), forget), recentProjects_.end());
        launcherInfo_.clear();
    }

    ImGui::SameLine();
    ImGui::BeginChild("##new", ImVec2(-40.f * s, -60.f * s), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("New project");
    ImGui::InputTextWithHint("Name", "My Game", &newProjectName_);
    for (int i = 0; i < (int)project_templates::Template::Count; ++i) {
        auto t = (project_templates::Template)i;
        ImGui::RadioButton(project_templates::name(t), &newProjectTemplate_, i);
        ImGui::Indent();
        ImGui::TextDisabled("%s", project_templates::description(t));
        ImGui::Unindent();
    }
    ImGui::Spacing();
    if (ImGui::Button("Create in a folder...")) {
        fs::path dir = platform::openFolderDialog("New project: an empty folder");
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        std::string error;
        if (!dir.empty()) {
            if (!newProjectName_.empty() && project::exists(dir) == false && fs::is_directory(dir) && !fs::is_empty(dir))
                dir /= newProjectName_;   // (a folder with things in it: the project in a new folder inside)
            if (project_templates::create(dir, (project_templates::Template)newProjectTemplate_, newProjectName_, error)) open = dir.u8string();
            else setStatus("Can't make the project: " + error);
        }
    }
    tooltip("Pick an empty folder, or one to make the project's folder in (named after the project)");
    ImGui::EndChild();

    ImGui::SetCursorPosX(40.f * s);
    if (ImGui::Button("Open a project folder...")) {
        fs::path dir = platform::openFolderDialog("Open project folder");
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        if (!dir.empty()) open = dir.u8string();
    }
    ImGui::SameLine();
    if (ImGui::Button("Just the demo scene")) showLauncher_ = false;
    tooltip("The built-in demo, no project (File > Project launcher brings this back)");
    ImGui::End();
    if (!open.empty()) {
        showLauncher_ = false;
        openProject(fs::u8path(open));
    }
}

void Editor::showProjectSettings() {
    projectDraft_ = project_;
    showProjectSettings_ = true;
}

void Editor::drawProjectSettings() {
    if (!showProjectSettings_) return;
    ImGui::SetNextWindowSize(ImVec2(620.f * uiScale_, 560.f * uiScale_), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project Settings", &showProjectSettings_)) {
        ImGui::End();
        return;
    }
    project::Settings& d = projectDraft_;
    ImGui::TextDisabled("%s", (project::root() / "project.json").u8string().c_str());
    if (std::string note = engineNote(project_.engineVersion); !note.empty()) ImGui::TextColored(ImVec4(1.f, 0.8f, 0.4f, 1.f), "%s", note.c_str());
    ImGui::SeparatorText("Game");
    ImGui::InputText("Name", &d.name);
    ImGui::InputText("Version", &d.gameVersion);
    ImGui::InputTextWithHint("Publisher", "Your name or studio", &d.publisher);
    tooltip("Who makes it: exported executables say so (Windows file properties, the macOS copyright)");
    ImGui::InputTextWithHint("Identifier", "com.example.mygame", &d.identifier);
    tooltip("Reverse-DNS, unique to the game (the macOS bundle identifier); empty: made from the name");
    tooltip("The game's own version (exports, update checks): numbers with dots, 1.2.0");
    // Scenes: a combo of the project's
    if (ImGui::BeginCombo("Start scene", d.startScene.c_str())) {
        for (const fs::path& f : vfs::list(project::root(), ".vkw")) {
            std::string stored = project::toStored(f.u8string());
            if (ImGui::Selectable(stored.c_str(), stored == d.startScene)) d.startScene = stored;
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo("Icon", d.icon.empty() ? "(none)" : d.icon.c_str())) {
        if (ImGui::Selectable("(none)", d.icon.empty())) d.icon.clear();
        for (const fs::path& f : vfs::list(project::root(), ".png")) {
            std::string stored = project::toStored(f.u8string());
            if (ImGui::Selectable(stored.c_str(), stored == d.icon)) d.icon = stored;
        }
        ImGui::EndCombo();
    }
    tooltip("A PNG in the project: the window's and the exported game's icon");
    std::vector<std::string> langs = StringTable::languages(project::root());
    if (ImGui::BeginCombo("Language", d.language.c_str())) {
        if (std::find(langs.begin(), langs.end(), "en") == langs.end()) langs.insert(langs.begin(), "en");
        for (const std::string& l : langs)
            if (ImGui::Selectable(l.c_str(), l == d.language)) d.language = l;
        ImGui::EndCombo();
    }
    tooltip("The strings/<language>.json the game starts in");
    ImGui::SeparatorText("Code");
    ImGui::InputText("Game module", &d.module);
    tooltip("The project's own C++ behaviours: the library's path without its prefix and extension (bin/game)");
    ImGui::InputText("Module build", &d.moduleBuild);
    tooltip("The command Game > Reload game module runs in the project folder ({engineBuild}: the editor's build folder)");
    ImGui::SeparatorText("Online");
    ImGui::InputTextWithHint("Update URL", "https://example.com/game/latest.json", &d.updateUrl);
    tooltip("Where the game asks whether there's a newer version (and gets the update packs)");
    ImGui::InputTextWithHint("Relay", "relay.example.com", &d.relay);
    tooltip("The relay server multiplayer lobbies are on (vkrelay)");
    ImGui::InputTextWithHint("Crash reports URL", "https://example.com/crash", &d.crashReportUrl);
    tooltip("Where crash reports go when a player agrees to send them (a POST of the report's text)");

    // What's wrong with it, before saving
    std::vector<std::string> problems;
    if (d.name.empty()) problems.push_back("the game needs a name");
    bool versionOk = !d.gameVersion.empty() && d.gameVersion.find_first_not_of("0123456789.") == std::string::npos &&
                     d.gameVersion.front() != '.' && d.gameVersion.back() != '.';
    if (!versionOk) problems.push_back("the version is numbers with dots (1.2.0)");
    if (!vfs::exists(fs::u8path(project::resolve(d.startScene)))) problems.push_back("the start scene isn't there: " + d.startScene);
    for (const std::string* url : {&d.updateUrl, &d.crashReportUrl})
        if (!url->empty() && url->rfind("http://", 0) != 0 && url->rfind("https://", 0) != 0) problems.push_back(*url + " isn't an http(s) URL");
    ImGui::Spacing();
    for (const std::string& p : problems) ImGui::TextColored(ImVec4(1.f, 0.55f, 0.45f, 1.f), "%s", p.c_str());
    ImGui::BeginDisabled(!problems.empty());
    if (ImGui::Button("Save")) {
        std::string error;
        if (project::save(project::root(), d, error)) {
            project_ = d;
            project_.engineVersion = version::string();
            setStatus("Saved the project settings");
        } else {
            setStatus("Can't save project.json: " + error);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Revert")) projectDraft_ = project_;
    ImGui::End();
}

void Editor::showBuildSettings() {
    if (project_.builds.empty()) project_.builds.push_back(project::Settings::Build{});   // (a Release profile to start from)
    buildSelected_ = std::clamp(buildSelected_, 0, (int)project_.builds.size() - 1);
    showBuildSettings_ = true;
}

void Editor::drawBuildSettings() {
    if (!showBuildSettings_) return;
    ImGui::SetNextWindowSize(ImVec2(640.f * uiScale_, 420.f * uiScale_), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Build Settings", &showBuildSettings_)) {
        ImGui::End();
        return;
    }
    std::vector<project::Settings::Build>& builds = project_.builds;
    bool changed = false;
    // The profiles on the left, the chosen one's settings on the right
    ImGui::BeginChild("##profiles", ImVec2(170.f * uiScale_, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
    for (int i = 0; i < (int)builds.size(); ++i)
        if (ImGui::Selectable((builds[(size_t)i].name + "##" + std::to_string(i)).c_str(), buildSelected_ == i)) buildSelected_ = i;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##profile", ImVec2(0.f, -ImGui::GetFrameHeightWithSpacing()));
    if (buildSelected_ >= 0 && buildSelected_ < (int)builds.size()) {
        project::Settings::Build& b = builds[(size_t)buildSelected_];
        ImGui::TextDisabled("For this platform (%s)", updates::platformKey());
        changed |= ImGui::InputText("Name", &b.name);
        int layout = b.layout == "folder" ? 0 : b.layout == "single" ? 2 : 1;
        if (ImGui::Combo("Layout", &layout, "A folder of files\0Data packed (game.pak)\0A single file\0")) {
            b.layout = layout == 0 ? "folder" : layout == 2 ? "single" : "pack";
            changed = true;
        }
        changed |= ImGui::Checkbox("Shipping", &b.shipping);
        tooltip("Without the developer tools: no F3 profiler or frame counter, no Lua debugger, traces or validation layer");
        changed |= ImGui::Checkbox("Package (AppImage / .dmg / installer)", &b.package);
        changed |= ImGui::Checkbox("Update pack", &b.updatePack);
        tooltip("The whole build as one .vkwupdate with its SHA-256, for games already out to install");
        changed |= ImGui::InputText("Folder", &b.out);
        tooltip("Where it's built: relative to the project, or a full path");
        ImGui::SameLine();
        if (ImGui::SmallButton("...")) {
            fs::path dir = platform::openFolderDialog("Build into");
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
            if (!dir.empty()) {
                std::error_code ec;
                fs::path rel = fs::relative(dir, project::root(), ec);
                b.out = !ec && !rel.empty() && rel.begin()->u8string() != ".." ? rel.generic_u8string() : dir.u8string();
                changed = true;
            }
        }
        ImGui::TextDisabled("%s", buildFolder(project::root(), b).u8string().c_str());
        ImGui::Spacing();
        if (ImGui::Button("Build")) {
            exportGameTo(buildFolder(project::root(), b), optionsFor(b));
        }
        tooltip("Builds what's saved (save the scene first)");
    }
    ImGui::EndChild();
    if (ImGui::Button("Add")) {
        project::Settings::Build b;
        b.name = "Build " + std::to_string(builds.size() + 1);
        b.out = "builds/" + b.name;
        builds.push_back(b);
        buildSelected_ = (int)builds.size() - 1;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate") && buildSelected_ >= 0 && buildSelected_ < (int)builds.size()) {
        project::Settings::Build b = builds[(size_t)buildSelected_];
        b.name += " copy";
        b.out += " copy";
        builds.push_back(b);
        buildSelected_ = (int)builds.size() - 1;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(builds.size() <= 1);
    if (ImGui::Button("Remove") && buildSelected_ >= 0 && buildSelected_ < (int)builds.size()) {
        builds.erase(builds.begin() + buildSelected_);
        buildSelected_ = std::max(buildSelected_ - 1, 0);
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::End();
    if (changed && project::exists(project::root())) {   // (kept in project.json at once)
        std::string error;
        if (!project::save(project::root(), project_, error)) setStatus("Can't save project.json: " + error);
    }
}
