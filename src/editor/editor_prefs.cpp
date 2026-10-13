#include "editor/editor_prefs.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "core/json.h"
#include "core/project.h"
#include "core/vfs.h"

namespace fs = std::filesystem;

json::Value EditorPrefs::toJson() const {
    json::Value v = json::Value::object();
    v.set("format", "vkworld-editor-prefs");
    v.set("tool", tool);
    v.set("localSpace", localSpace);
    v.set("snap", snap);
    v.set("snapMove", snapMove);
    v.set("snapRotate", snapRotate);
    v.set("snapScale", snapScale);
    v.set("showOutliner", showOutliner);
    v.set("showInspector", showInspector);
    v.set("showAssets", showAssets);
    v.set("showColliders", showColliders);
    v.set("showProfiler", showProfiler);
    v.set("previewParticles", previewParticles);
    v.set("cameraSpeed", cameraSpeed);
    v.set("cameraFov", cameraFov);
    v.set("antialiasing", antialiasing);
    v.set("realtimeViewport", realtimeViewport);
    v.set("frameCap", frameCap);
    v.set("throttleInBackground", throttleInBackground);
    v.set("maximizeOnPlay", maximizeOnPlay);
    v.set("captureOnClick", captureOnClick);
    json::Value recent = json::Value::array();
    for (const std::string& p : recentProjects) recent.push(p);
    v.set("recentProjects", std::move(recent));
    return v;
}

EditorPrefs EditorPrefs::fromJson(const json::Value& v) {
    EditorPrefs p;
    auto positive = [](float x, float def) { return std::isfinite(x) && x > 0.f ? x : def; };
    p.tool = std::clamp(v["tool"].asInt(p.tool), 0, 3);
    p.localSpace = v["localSpace"].asBool(p.localSpace);
    p.snap = v["snap"].asBool(p.snap);
    p.snapMove = positive(v["snapMove"].asFloat(p.snapMove), p.snapMove);
    p.snapRotate = positive(v["snapRotate"].asFloat(p.snapRotate), p.snapRotate);
    p.snapScale = positive(v["snapScale"].asFloat(p.snapScale), p.snapScale);
    p.showOutliner = v["showOutliner"].asBool(p.showOutliner);
    p.showInspector = v["showInspector"].asBool(p.showInspector);
    p.showAssets = v["showAssets"].asBool(p.showAssets);
    p.showColliders = v["showColliders"].asBool(p.showColliders);
    p.showProfiler = v["showProfiler"].asBool(p.showProfiler);
    p.previewParticles = v["previewParticles"].asBool(p.previewParticles);
    p.cameraSpeed = std::clamp(positive(v["cameraSpeed"].asFloat(p.cameraSpeed), p.cameraSpeed), 0.1f, 1000.f);
    p.cameraFov = std::clamp(v["cameraFov"].asFloat(p.cameraFov), 20.f, 120.f);
    p.antialiasing = std::clamp(v["antialiasing"].asInt(p.antialiasing), -1, 2);
    p.realtimeViewport = v["realtimeViewport"].asBool(p.realtimeViewport);
    p.frameCap = std::clamp(v["frameCap"].asInt(p.frameCap), 0, 500);
    p.throttleInBackground = v["throttleInBackground"].asBool(p.throttleInBackground);
    p.maximizeOnPlay = v["maximizeOnPlay"].asBool(p.maximizeOnPlay);
    p.captureOnClick = v["captureOnClick"].asBool(p.captureOnClick);
    for (const json::Value& r : v["recentProjects"].elements())
        if (!r.asString().empty() && p.recentProjects.size() < kMaxRecent) p.recentProjects.push_back(r.asString());
    return p;
}

bool EditorPrefs::operator==(const EditorPrefs& o) const {
    return tool == o.tool && localSpace == o.localSpace && snap == o.snap && snapMove == o.snapMove &&
           snapRotate == o.snapRotate && snapScale == o.snapScale && showOutliner == o.showOutliner &&
           showInspector == o.showInspector && showAssets == o.showAssets && showColliders == o.showColliders &&
           showProfiler == o.showProfiler && previewParticles == o.previewParticles && cameraSpeed == o.cameraSpeed &&
           cameraFov == o.cameraFov && antialiasing == o.antialiasing && realtimeViewport == o.realtimeViewport &&
           frameCap == o.frameCap && throttleInBackground == o.throttleInBackground && maximizeOnPlay == o.maximizeOnPlay && captureOnClick == o.captureOnClick &&
           recentProjects == o.recentProjects;
}

namespace editor_prefs {

fs::path folder() { return project::saveDir("Editor").parent_path(); }

bool load(const fs::path& file, EditorPrefs& out) {
    std::string text, error;
    json::Value doc;
    if (!vfs::read(file, text) || !json::parse(text, doc, error)) return false;
    if (doc["format"].asString() != "vkworld-editor-prefs") return false;
    out = EditorPrefs::fromJson(doc);
    return true;
}

bool save(const fs::path& file, const EditorPrefs& prefs) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << json::write(prefs.toJson());
        if (!f) return false;
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

void addRecent(std::vector<std::string>& list, const std::string& dir) {
    if (dir.empty()) return;
    list.erase(std::remove(list.begin(), list.end(), dir), list.end());
    list.insert(list.begin(), dir);
    if (list.size() > EditorPrefs::kMaxRecent) list.resize(EditorPrefs::kMaxRecent);
}

}  // namespace editor_prefs
