// Editor panels: menu bar, toolbar, status bar, help, dialogs and the input bindings window.
#include "editor/editor.h"
#include "scene/project_templates.h"
#include "script/lua_debugger.h"
#include "game/strings.h"

#include <cctype>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <functional>

#include "misc/cpp/imgui_stdlib.h"
#include "platform/platform.h"
#include "scene/prefab_sync.h"
#include "editor/editor_ui.h"

using namespace editor_ui;

// ---------------------------------------------------------------------------------------------
// Menu bar / toolbar
// ---------------------------------------------------------------------------------------------
void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        ImGui::BeginDisabled(playing());   // stop first: the scene on screen is the throwaway play copy
        ImGui::TextDisabled("Project: %s", project_.name.c_str());
        if (ImGui::BeginMenu("New project")) {
            for (int i = 0; i < (int)project_templates::Template::Count; ++i) {
                auto t = (project_templates::Template)i;
                bool chosen = ImGui::MenuItem((std::string(project_templates::name(t)) + "...").c_str());
                tooltip(project_templates::description(t));
                if (!chosen) continue;
                std::filesystem::path dir = platform::openFolderDialog("New project: an empty folder");
                ImGui::GetIO().ClearInputKeys();
                ImGui::GetIO().ClearInputMouse();
                std::string error;
                if (dir.empty()) continue;
                if (project_templates::create(dir, t, "", error)) openProject(dir);
                else setStatus("Can't make the project: " + error);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Open project folder...")) {
            std::filesystem::path dir = platform::openFolderDialog("Open project folder");
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
            if (!dir.empty()) openProject(dir);
        }
        if (ImGui::BeginMenu("Recent projects", recentProjects_.size() > 1)) {
            std::string open;
            for (const std::string& dir : recentProjects_)
                if (ImGui::MenuItem(dir.c_str(), nullptr, dir == project::root().u8string())) open = dir;
            ImGui::EndMenu();
            std::error_code ec;
            if (!open.empty() && std::filesystem::is_directory(std::filesystem::u8path(open), ec)) openProject(std::filesystem::u8path(open));
            else if (!open.empty()) {
                recentProjects_.erase(std::remove(recentProjects_.begin(), recentProjects_.end(), open), recentProjects_.end());
                setStatus("Not there any more: " + open);
            }
        }
        if (ImGui::BeginMenu("Export game", project::exists(project::root()))) {
            const char* labels[3] = {"As a folder of files...", "With its data packed (game.pak)...",
                                     "As a single file..."};
            const char* help[3] = {"The game, the engine, the shaders and the project's files, as they are",
                                   "The game and the engine, and everything else in one game.pak",
                                   "Everything in the game's executable (with a standalone build, one file in all: "
                                   "configure with -DVKWORLD_STANDALONE_GAMES=ON)"};
            for (int i = 0; i < 3; ++i) {
                if (ImGui::MenuItem(labels[i])) {
                    std::filesystem::path dir = platform::openFolderDialog("Export the game into");
                    ImGui::GetIO().ClearInputKeys();
                    ImGui::GetIO().ClearInputMouse();
                    ExportOptions o;
                    o.layout = (ExportOptions::Layout)i;
                    if (!dir.empty()) exportGameTo(dir, o);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s (save the scene first)", help[i]);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Use this scene at startup")) {
            project_.startScene = scenePath_;
            std::string error;
            if (project::save(project::root(), project_, error)) setStatus("vkgame and the editor now start with " + project_.startScene);
            else setStatus(error);
        }
        tooltip("Saved in project.json: the scene vkgame plays");
        ImGui::Separator();
        ImGui::SetNextItemWidth(px(220.f));
        ImGui::InputText("Scene file", scenePath_, sizeof(scenePath_));
        if (ImGui::MenuItem("Save", "Ctrl+S")) saveToFile();
        if (ImGui::MenuItem("Recover autosave", nullptr, false, recoverable_)) recoverAutosave();
        if (ImGui::MenuItem("Load", "Ctrl+O")) loadFromFile();
        if (ImGui::MenuItem("Import model...")) openImport_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("New (ground + player)")) newScene(false);
        if (ImGui::MenuItem("Reset to demo scene")) newScene(true);
        if (ImGui::MenuItem("New stress test scene")) newStressScene();
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        bool sel = selection() != nullptr;
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_.empty() && !playing())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, !redo_.empty() && !playing())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Shift+D", false, sel)) duplicateSelected();
        if (ImGui::MenuItem("Delete", "X / Del", false, sel)) deleteSelected();
        if (ImGui::MenuItem("Drop to ground", "End", false, sel)) dropSelectedToGround();
        if (ImGui::MenuItem("Unparent", nullptr, false, sel && selection()->parent != kNoEntity))
            reparent(selected_, kNoEntity);
        if (ImGui::MenuItem("Create prefab from selection", nullptr, false, sel)) createPrefabFromSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Hide / show selected", "H", false, sel)) toggleHideSelected();
        if (ImGui::MenuItem("Unhide all", "Alt+H")) unhideAll();
        if (ImGui::MenuItem("Select all", "Ctrl+A")) selectAllRoots();
        if (ImGui::MenuItem("Deselect", "Esc", false, sel)) selected_ = kNoEntity;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        drawAddMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Game")) {
        if (ImGui::MenuItem(playing() ? "Stop" : "Play", "Ctrl+P")) playing() ? stopPlay() : startPlay();
        if (ImGui::MenuItem("Pause", "Ctrl+Shift+P", playing() && game_.paused(), playing())) togglePause();
        if (ImGui::MenuItem("Step one tick", nullptr, false, playing() && game_.paused())) game_.step();
        ImGui::Separator();
        if (ImGui::MenuItem("Host a multiplayer game", nullptr, &hostMultiplayer_)) {
            if (playing() && hostMultiplayer_ && !net_.active()) {
                std::string error;
                if (!net_.host(game_, NetSession::kDefaultPort, error)) setStatus("Can't host: " + error);
            } else if (!hostMultiplayer_) {
                net_.stop();
            }
        }
        tooltip("While playing, others can join with vkgame --join <this machine>:27015 (the scene's Network "
                "settings say what player they get)");
        if (ImGui::BeginMenu("Language")) {   // the project's: what the game starts in (strings/<language>.json)
            std::vector<std::string> langs = StringTable::languages(project::root());
            if (langs.empty()) ImGui::TextDisabled("No string tables (strings/<language>.json)");
            for (const std::string& l : langs)
                if (ImGui::MenuItem(l.c_str(), nullptr, project_.language == l)) {
                    project_.language = l;
                    std::string error;
                    if (project::exists(project::root()) && !project::save(project::root(), project_, error))
                        setStatus("Can't save project.json: " + error);
                    else setStatus("The game starts in " + l + " (Play to try it)");
                }
            ImGui::EndMenu();
        }
        ImGui::MenuItem("Maximize viewport on play", nullptr, &maximizeOnPlay_);
        ImGui::MenuItem("Capture mouse on click", nullptr, &captureOnClick_);
        ImGui::Separator();
        ImGui::MenuItem("Input bindings...", nullptr, &showInputBindings_);
        bool debugging = scripting::debugger::listening();
        if (ImGui::MenuItem("Lua debugger (port 4711)", nullptr, debugging)) {
            if (debugging) {
                scripting::debugger::stop();
                setStatus("Lua debugger off");
            } else {
                std::string error;
                if (scripting::debugger::listen(4711, error)) setStatus("Lua debugger: attach from VS Code (port 4711)");
                else setStatus("Lua debugger: " + error);
            }
        }
        tooltip("Breakpoints, stepping and watches in Lua scripts from VS Code (tools/vscode-vkworld-lua)");
        ImGui::Separator();
        bool building = moduleBuild_.valid();
        if (ImGui::MenuItem(project_.moduleBuild.empty() ? "Reload game module" : "Rebuild && reload game module",
                            "Ctrl+R", false, !project_.module.empty() && !building))
            rebuildGameModule();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip(project_.module.empty()
                                  ? "This project has no game module (\"module\" in project.json)"
                                  : "%s", module_.loaded() ? module_.library().u8string().c_str() : "not loaded");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Outliner", nullptr, &showOutliner_);
        ImGui::MenuItem("Inspector", nullptr, &showInspector_);
        ImGui::MenuItem("Assets", nullptr, &showAssets_);
        ImGui::MenuItem("Show colliders", nullptr, &showColliders_);
        ImGui::MenuItem("Show contacts", nullptr, &showContacts_);
        ImGui::MenuItem("Game view", nullptr, &showGameView_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("While playing: the game's camera in a window of its own (dock it beside the scene), the\n"
                              "viewport staying with yours");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("While playing: where bodies touch (dots) and which way they push (lines)");
        ImGui::MenuItem("Profiler", "F3", &showProfiler_);
        if (renderer_ && ImGui::BeginMenu("Anti-aliasing")) {
            const char* names[] = {"MSAA (4x)", "Temporal", "Off"};
            const char* tips[] = {"Smooth edges from four samples per pixel", "Jittered frames blended over time: "
                                  "also smooths shading and thin lines; softer, and can trail behind fast motion",
                                  "Jagged edges, fastest"};
            for (int i = 0; i < 3; ++i) {
                auto mode = (Renderer::Antialiasing)i;
                if (ImGui::MenuItem(names[i], nullptr, renderer_->antialiasing() == mode, renderer_->supports(mode))) {
                    renderer_->setAntialiasing(mode);
                    antialiasing_ = i;
                }
                tooltip(tips[i]);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reset layout")) {   // the panels back where they started (and shown)
            resetLayout_ = true;
            showOutliner_ = showInspector_ = showAssets_ = true;
        }
        if (ImGui::MenuItem("Show navigation", nullptr, &showNavigation_) && showNavigation_) bakeNavigationPreview();
        ImGui::MenuItem("Preview particles", nullptr, &previewParticles_on_);
        ImGui::MenuItem("Show game UI", nullptr, &showGameUi_);
        ImGui::Separator();
        if (ImGui::MenuItem("Focus selected", "F", false, selection() != nullptr)) focusSelected();
        if (ImGui::MenuItem("Reset camera")) cam_ = EditorCamera{};
        bool fullscreen = platform::isFullscreen();
        if (ImGui::MenuItem("Fullscreen", "F11", &fullscreen)) platform::setFullscreen(fullscreen);
        ImGui::Separator();
        ImGui::MenuItem("Controls help", "F1", &showHelp_);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Editor::drawToolbar() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetFrameHeight()));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, topH_ - ImGui::GetFrameHeight()));
    ImGui::Begin("##toolbar", nullptr, kBarFlags);

    auto toolButton = [&](const char* label, Tool t, const char* tip) {
        bool active = tool_ == t;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label)) tool_ = t;
        if (active) ImGui::PopStyleColor();
        tooltip(tip);
        ImGui::SameLine();
    };
    toolButton("Select", Tool::Select, "Select only, no gizmo (Q)");
    toolButton("Move", Tool::Move, "Translate gizmo (G)");
    toolButton("Rotate", Tool::Rotate, "Rotate gizmo (R)");
    toolButton("Scale", Tool::Scale, "Scale gizmo (S)");
    if (const Entity* sel = selection(); sel && sel->terrain) toolButton("Terrain", Tool::Terrain, "Sculpt and paint the terrain (T)");
    else if (tool_ == Tool::Terrain) tool_ = Tool::Move;

    verticalSeparator();
    if (ImGui::Button(localSpace_ ? "Local" : "World")) localSpace_ = !localSpace_;
    tooltip("Gizmo orientation: world or object axes (L)");
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &snap_);
    tooltip("Snap gizmo moves to increments. Holding Ctrl while dragging inverts this.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(70.f));
    if (tool_ == Tool::Rotate) ImGui::DragFloat("##snap", &snapRotate_, 0.5f, 1.f, 180.f, "%.0f deg");
    else if (tool_ == Tool::Scale) ImGui::DragFloat("##snap", &snapScale_, 0.01f, 0.01f, 10.f, "%.2f x");
    else ImGui::DragFloat("##snap", &snapMove_, 0.05f, 0.05f, 100.f, "%.2f m");
    tooltip("Snap increment for the current tool");

    verticalSeparator();
    if (ImGui::Button("Add...")) openAddMenu_ = true;
    tooltip("Add a new object in front of the camera (Shift+A)");
    ImGui::SameLine();
    ImGui::BeginDisabled(selection() == nullptr);
    if (ImGui::Button("Duplicate")) duplicateSelected();
    tooltip("Shift+D (with its children)");
    ImGui::SameLine();
    if (ImGui::Button("Delete")) deleteSelected();
    tooltip("X / Delete (with its children)");
    ImGui::SameLine();
    if (ImGui::Button("Focus")) focusSelected();
    tooltip("Frame the selected object (F)");
    ImGui::EndDisabled();

    verticalSeparator();
    ImGui::BeginDisabled(undo_.empty() || playing());
    if (ImGui::Button("Undo")) undo();
    tooltip("Ctrl+Z");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redo_.empty() || playing());
    if (ImGui::Button("Redo")) redo();
    tooltip("Ctrl+Shift+Z / Ctrl+Y");
    ImGui::EndDisabled();

    verticalSeparator();
    bool p = playing();
    ImGui::PushStyleColor(ImGuiCol_Button, p ? ImVec4(0.75f, 0.25f, 0.2f, 1.f) : ImVec4(0.2f, 0.55f, 0.25f, 1.f));
    if (ImGui::Button(p ? "Stop" : "Play")) p ? stopPlay() : startPlay();
    ImGui::PopStyleColor();
    tooltip(p ? "Stop and restore the scene (Ctrl+P)" : "Run the game from this scene (Ctrl+P)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!p);
    bool paused = p && game_.paused();
    if (paused) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Pause")) togglePause();
    if (paused) ImGui::PopStyleColor();
    tooltip("Ctrl+Shift+P");
    ImGui::SameLine();
    ImGui::BeginDisabled(!paused);
    if (ImGui::Button("Step")) game_.step();
    tooltip("Advance one fixed update (1/60 s) while paused");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(140.f));
    ImGui::SliderFloat("Fly speed", &cam_.speed, 1.f, 200.f, "%.0f", ImGuiSliderFlags_Logarithmic);
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Status bar, help, popups
// ---------------------------------------------------------------------------------------------
void Editor::drawStatusBar(const RenderStats& stats) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - statusH_));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, statusH_));
    ImGui::Begin("##status", nullptr, kBarFlags);
    if (statusTimer_ > 0.f) {
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", status_.c_str());
    } else if (net_.active()) {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 1.f, 1.f), "Multiplayer: %s", net_.status().c_str());
    } else {
        const char* hint = nav_ == Nav::Fly     ? "Flying: WASD move, E/Q up/down, Shift fast, wheel = speed"
                           : nav_ == Nav::Orbit ? "Orbiting (Shift+MMB to pan)"
                           : nav_ == Nav::Pan   ? "Panning"
                           : playing()          ? "Playing: Ctrl+P stops and restores the scene, Ctrl+Shift+P pauses"
                                                : "LMB select | RMB+WASD fly | MMB orbit | Shift+MMB pan | "
                                                  "Wheel zoom | G/R/S tools | F focus | Shift+A add | Ctrl+P play";
        ImGui::TextDisabled("%s", hint);
    }
    char right[160], update[32] = "";
    if (playing()) std::snprintf(update, sizeof(update), "update %.2f ms | ", game_.updateMs());
    std::snprintf(right, sizeof(right), "%s%zu entities | %u draws | %u tris | %s%.0f FPS", unsaved() ? "unsaved | " : "",
                  scene_.entities.size(), stats.drawCalls, stats.triangles, update, fps_);
    float w = ImGui::CalcTextSize(right).x;
    ImGui::SameLine(std::max(ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x, 0.f));
    ImGui::TextUnformatted(right);
    ImGui::End();
}

void Editor::drawHelp() {
    ImGui::SetNextWindowSize(ImVec2(px(470.f), 0), ImGuiCond_FirstUseEver);
    ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Controls", &showHelp_, ImGuiWindowFlags_NoSavedSettings)) { ImGui::End(); return; }
    static const char* rows[][2] = {
        {"Left click", "Select object (click empty space to deselect)"},
        {"Right mouse + WASD", "Fly: mouse look, E/Q up/down, Shift faster, wheel = speed"},
        {"Middle mouse drag", "Orbit around the point in front of the camera"},
        {"Shift + middle drag", "Pan"},
        {"Mouse wheel", "Zoom (dolly)"},
        {"View cube (top-right)", "Click a face/edge to snap the view, drag to rotate"},
        {"Q / G / R / S", "Select / Move / Rotate / Scale tool"},
        {"L", "Toggle world/local gizmo axes"},
        {"Ctrl while dragging", "Toggle snapping"},
        {"F  (or double-click)", "Focus selected object"},
        {"Outliner drag & drop", "Drop an object on another to parent it, on empty space to unparent"},
        {"Shift+A", "Add object menu"},
        {"Shift+D", "Duplicate selected (with children)"},
        {"X / Delete", "Delete selected (with children)"},
        {"End", "Drop selected to the ground"},
        {"H / Alt+H", "Hide selected / unhide all"},
        {"Esc", "Deselect"},
        {"Ctrl+click", "Add to / take from the selection (Shift+click in the outliner: a range)"},
        {"Ctrl+A", "Select every top-level object"},
        {"Ctrl+P", "Play / stop the game (the scene is restored on stop)"},
        {"Ctrl+Shift+P", "Pause / resume the game"},
        {"Click / Esc (playing)", "Capture / release the mouse for the game"},
        {"F11", "Fullscreen"},
        {"Ctrl+Z / Ctrl+Shift+Z", "Undo / redo"},
        {"Ctrl+S / Ctrl+O", "Save / load scene file"},
    };
    if (ImGui::BeginTable("help", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        for (auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r[0]);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r[1]);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Editor::drawAddPopup() {
    if (openAddMenu_) {
        ImGui::OpenPopup("##add");
        openAddMenu_ = false;
    }
    if (ImGui::BeginPopup("##add")) {
        ImGui::TextDisabled("Add object");
        ImGui::Separator();
        drawAddMenuItems();
        ImGui::EndPopup();
    }
}

// Built-in meshes, then imported ones, then the other kinds of entity
void Editor::drawAddMenuItems() {
    for (int k = 0; k < meshes_.count(); ++k) {
        if (k == (int)MeshKind::Count) ImGui::Separator();
        if (ImGui::MenuItem(meshes_.name(k))) addObject(k);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Empty")) addEmpty();
    if (ImGui::MenuItem("Camera")) addCamera();
    if (ImGui::MenuItem("Terrain")) addTerrain();
    if (ImGui::MenuItem("Reflection probe")) addReflectionProbe();
    if (ImGui::BeginMenu("UI")) {
        const char* names[] = {"Panel", "Text", "Image", "Button", "Slider", "Toggle", "Text field", "List"};
        for (int i = 0; i < 8; ++i)
            if (ImGui::MenuItem(names[i])) addUIElement((UIElement::Kind)i);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Point light")) addLight(Light::Type::Point);
    if (ImGui::MenuItem("Spot light")) addLight(Light::Type::Spot);
    if (ImGui::MenuItem("Directional light")) addLight(Light::Type::Directional);
    ImGui::Separator();
    if (ImGui::MenuItem("Import model...")) openImport_ = true;
}

void Editor::drawImportPopup() {
    namespace fs = std::filesystem;
    if (openImport_) {
        ImGui::OpenPopup("Import model");
        importError_.clear();
        openImport_ = false;
    }
    ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(px(520.f), 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Import model", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    ImGui::TextWrapped("glTF (.gltf / .glb): the whole model with its hierarchy, materials and textures, as a new "
                       "prefab plus a copy in the scene. OBJ: a single mesh, centred and scaled to fit a unit cube.");
    bool submit = false;
    if (ImGui::Button("Browse...")) {
        std::filesystem::path picked = platform::openFileDialog("Import model", "gltf,glb,obj");
        // The dialog ran its own event loop: drop key/button state ImGui may have missed the release of
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        if (!picked.empty()) {
            importPath_ = picked.u8string();
            submit = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    submit |= ImGui::InputTextWithHint("##path", "path, e.g. ~/Downloads/model.glb", &importPath_,
                                       ImGuiInputTextFlags_EnterReturnsTrue);

    // Model files in the working directory, for convenience
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(fs::current_path(ec), ec)) {
        std::string ext = entry.path().extension().u8string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".obj" || ext == ".gltf" || ext == ".glb") found.push_back(entry.path().filename());
    }
    if (!found.empty()) {
        ImGui::TextDisabled("In %s:", fs::current_path(ec).u8string().c_str());
        float rows = std::min<float>((float)found.size(), 6.f);
        if (ImGui::BeginListBox("##found", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * rows + px(8.f)))) {
            for (const auto& name : found) {
                std::string n = name.u8string();
                if (ImGui::Selectable(n.c_str(), importPath_ == n, ImGuiSelectableFlags_AllowDoubleClick)) {
                    importPath_ = n;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) submit = true;
                }
            }
            ImGui::EndListBox();
        }
    }
    if (!importError_.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", importError_.c_str());

    ImGui::Separator();
    ImGui::BeginDisabled(importPath_.empty());
    if (ImGui::Button("Import", ImVec2(px(100.f), 0))) submit = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(px(100.f), 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        ImGui::CloseCurrentPopup();
    if (submit && !importPath_.empty()) {
        importModel(importPath_);
        if (importError_.empty()) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------
// Input bindings: the scene's named actions and what triggers them
// ---------------------------------------------------------------------------------------------
void Editor::drawInputBindings() {
    ImGui::SetNextWindowSize(ImVec2(px(560.f), px(420.f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Input bindings", &showInputBindings_, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    std::vector<InputAction>& actions = scene_.inputActions;
    ImGui::TextWrapped("Game code reads these actions by name (ctx.input().down(\"jump\")). Saved with the scene; "
                       "changes apply from the next Play.");

    // Waiting for a key: the first key or gamepad button pressed is bound (Esc cancels)
    if (bindingCapture_ >= (int)actions.size()) bindingCapture_ = -1;
    if (bindingCapture_ >= 0) {
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            ImGuiKey key = (ImGuiKey)k;
            if ((key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) || !ImGui::IsKeyPressed(key, false)) continue;
            if (key != ImGuiKey_Escape) {
                pushUndo();
                std::vector<ImGuiKey>& keys = actions[(size_t)bindingCapture_].keys;
                if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
            }
            bindingCapture_ = -1;
            keyTakenByBindings_ = true;
            break;
        }
    }

    int removeAction = -1;
    if (ImGui::BeginTable("##bindings", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                          ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, px(130.f));
        ImGui::TableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, px(24.f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        static const char* mouseNames[] = {"Mouse Left", "Mouse Right", "Mouse Middle"};
        for (int i = 0; i < (int)actions.size(); ++i) {
            InputAction& a = actions[(size_t)i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##name", &a.name);
            track();
            ImGui::TableNextColumn();
            int removeKey = -1;
            for (int k = 0; k < (int)a.keys.size(); ++k) {
                ImGui::PushID(k);
                std::string label = std::string(keyName(a.keys[(size_t)k])) + "  x";
                if (ImGui::SmallButton(label.c_str())) removeKey = k;
                tooltip("Click to remove");
                ImGui::SameLine();
                ImGui::PopID();
            }
            if (removeKey >= 0) {
                pushUndo();
                a.keys.erase(a.keys.begin() + removeKey);
            }
            if (a.mouseButton >= 0 && a.mouseButton < 3) {
                std::string label = std::string(mouseNames[a.mouseButton]) + "  x";
                if (ImGui::SmallButton(label.c_str())) { pushUndo(); a.mouseButton = -1; }
                ImGui::SameLine();
            }
            if (bindingCapture_ == i) {
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "press a key or button... (Esc cancels)");
            } else {
                if (ImGui::SmallButton("+ key")) bindingCapture_ = i;
                tooltip("Then press a key or gamepad button");
                ImGui::SameLine();
                if (ImGui::SmallButton("+ mouse")) ImGui::OpenPopup("##mouse");
                if (ImGui::BeginPopup("##mouse")) {
                    for (int m = 0; m < 3; ++m)
                        if (ImGui::MenuItem(mouseNames[m])) { pushUndo(); a.mouseButton = m; }
                    ImGui::EndPopup();
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) removeAction = i;
            tooltip("Delete this action");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeAction >= 0) {
        pushUndo();
        actions.erase(actions.begin() + removeAction);
        bindingCapture_ = -1;
    }
    if (ImGui::Button("Add action")) {
        pushUndo();
        actions.push_back({"new_action", {}, -1});
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to defaults")) {
        pushUndo();
        actions = defaultInputActions();
        bindingCapture_ = -1;
    }
    ImGui::End();
}
