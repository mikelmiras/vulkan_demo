// The editor's own settings, kept per user between sessions: editor_prefs.json in the user's data folder, next to
// editor.log. (The panel layout is Dear ImGui's editor_layout.ini, beside it.)
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace json { class Value; }

struct EditorPrefs {
    // Tools
    int tool = 1;   // Editor::Tool: select, move, rotate, scale
    bool localSpace = false, snap = false;
    float snapMove = 0.5f, snapRotate = 15.f, snapScale = 0.1f;
    // View
    bool showOutliner = true, showInspector = true, showAssets = true, showColliders = false, showProfiler = false;
    bool previewParticles = true;
    float cameraSpeed = 12.f, cameraFov = 60.f;   // fly speed (units/s), degrees
    int antialiasing = -1;   // Renderer::Antialiasing (-1: the renderer's default)
    // Power: the view drawn only when something changes (realtime: every frame, as games are), at most frameCap
    // frames a second (0: the display's rate), slowed to 10 in the background
    bool realtimeViewport = false;
    int frameCap = 60;
    bool throttleInBackground = true;
    // Play
    bool maximizeOnPlay = false, captureOnClick = true;
    std::vector<std::string> recentProjects;   // folders, newest first

    static constexpr size_t kMaxRecent = 8;

    json::Value toJson() const;
    static EditorPrefs fromJson(const json::Value& v);   // missing or wrong-typed keys keep their defaults
    bool operator==(const EditorPrefs& o) const;
    bool operator!=(const EditorPrefs& o) const { return !(*this == o); }
};

namespace editor_prefs {
std::filesystem::path folder();   // <user data>/vkworld/Editor (VKWORLD_SAVE_DIR's parent when that's set)
bool load(const std::filesystem::path& file, EditorPrefs& out);   // false (out unchanged) when missing or unreadable
bool save(const std::filesystem::path& file, const EditorPrefs& prefs);   // whole or not at all (temp file + rename)
// Puts `dir` at the front of a recent list (once), dropping the oldest past EditorPrefs::kMaxRecent
void addRecent(std::vector<std::string>& list, const std::string& dir);
}  // namespace editor_prefs
