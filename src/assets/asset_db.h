// The project's asset database: every asset file has a stable id, kept next to it in a sidecar "<file>.meta" (JSON:
// the id and the asset's import settings). Scenes and prefabs save the id along with each file they reference, so a
// file renamed or moved (with its .meta: the editor's asset browser moves both; in a file manager or git, move both)
// is found again when the scene loads ("healed"), and the editor's rename / move updates the references in the
// project's files as well.
//
// The editor opens the database writable: it gives files ids (writes .meta files) as they're referenced or browsed.
// Games open it read-only (only existing .meta files: a shipped game never writes into its install folder).
// Thread-safe: scenes load on worker threads.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/json.h"

namespace assetdb {

// Scans the project's .meta files (on disk or in the mounted pack). Replaces the previous project's.
void open(const std::filesystem::path& projectRoot, bool writable);
void close();
bool isOpen();
bool writable();
std::filesystem::path metaPath(const std::filesystem::path& file);   // "<file>.meta"

// The id of a file in the project ("" outside it, for built-ins, or with no .meta when not writable). When
// writable, a file without a .meta gets one (a new id).
std::string idOf(const std::filesystem::path& file);
// The same for a path as scenes store it ("textures/a.png", "models/m.glb#mesh2": the part before '#'); "" for
// names of built-ins ("Hum", "Bricks") and files that don't exist
std::string idOfStored(const std::string& stored);
// Where the asset with this id is now (absolute), or "" if unknown (one rescan is tried first)
std::filesystem::path pathOf(const std::string& id);
// A reference as stored, maybe stale -> the one to use: `stored` itself if its file exists or `id` is unknown, else
// the id's file now, stored the same way (relative to the project), with the same "#fragment". `healed` tells.
std::string heal(const std::string& stored, const std::string& id, bool* healed = nullptr);

// Import settings: the .meta's "import" object (an empty object when there are none)
json::Value importSettings(const std::filesystem::path& file);
bool setImportSettings(const std::filesystem::path& file, const json::Value& settings, std::string& error);

// Renames or moves a file or a folder (into an existing folder, under the new name), its .meta(s) along
bool move(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error);
// Deletes a file (or a folder and everything in it) and its .meta(s)
bool remove(const std::filesystem::path& file, std::string& error);

// References in the project's scene, prefab and project files to `from` (a file, or a folder: everything under
// it) are pointed at `to` instead; returns how many references changed. (After a move: references are paths.)
int retargetProjectFiles(const std::filesystem::path& from, const std::filesystem::path& to,
                         std::vector<std::filesystem::path>* changedFiles = nullptr);
// The same rewrite for one JSON document (a scene, a prefab, project.json): every string value naming `from` (or
// something under it, or a "#fragment" of it), as stored (relative to the project) or absolute
int retargetJson(json::Value& doc, const std::filesystem::path& from, const std::filesystem::path& to);
// One reference: the new stored form if it names `from` (or under it), else unchanged; `changed` tells
std::string retarget(const std::string& ref, const std::filesystem::path& from, const std::filesystem::path& to,
                     bool* changed = nullptr);

}  // namespace assetdb
