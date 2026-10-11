// Loading scenes without stalling the game: a scene file is read, parsed and its assets imported (models decoded,
// images unpacked) on a worker thread, into asset libraries of its own; when it's done, the main thread merges
// those into the game's libraries (reusing what's already loaded) and takes the scene, its asset ids remapped.
#pragma once

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>

#include "assets/meshes.h"
#include "assets/textures.h"
#include "scene/scene.h"

class SceneLoader {
public:
    SceneLoader() = default;
    ~SceneLoader();   // waits for a load in progress
    SceneLoader(const SceneLoader&) = delete;
    SceneLoader& operator=(const SceneLoader&) = delete;

    // Starts loading the scene file (a path as loadScene takes it); a load in progress is abandoned
    void start(const std::string& path);
    bool busy() const { return result_.valid(); }   // started and not taken yet
    bool ready() const;                              // done: finish() won't wait
    // Waits if needed, then merges the assets into the libraries and hands over the scene. False (with error) if
    // loading failed. After this the loader is idle again.
    bool finish(Scene& out, MeshLibrary& meshes, TextureLibrary& textures, std::string& error);
    const std::string& path() const { return path_; }
    float seconds() const;   // since start()

private:
    struct Loaded;
    std::future<std::shared_ptr<Loaded>> result_;
    std::string path_;
    std::chrono::steady_clock::time_point started_;
};

// Moves the meshes, textures and animation clips `scene` uses from the libraries it was loaded with into others
// (assets with the same source already there are reused) and remaps the scene's ids and clip names to match
void mergeSceneAssets(Scene& scene, const MeshLibrary& fromMeshes, const TextureLibrary& fromTextures,
                      MeshLibrary& meshes, TextureLibrary& textures);
