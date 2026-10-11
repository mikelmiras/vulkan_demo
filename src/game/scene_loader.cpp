#include "game/scene_loader.h"

#include <map>

#include "scene/serialization.h"

struct SceneLoader::Loaded {
    Scene scene;
    MeshLibrary meshes;
    TextureLibrary textures;
    bool ok = false;
    std::string error;
};

namespace {

// The built-in assets every library starts with (made once: the procedural textures take a moment)
const MeshLibrary& builtinMeshes() {
    static const MeshLibrary lib = buildMeshLibrary();
    return lib;
}
const TextureLibrary& builtinTextures() {
    static const TextureLibrary lib = buildTextureLibrary();
    return lib;
}

void remapEntity(Entity& e, const std::vector<int>& meshMap, const std::vector<int>& texMap,
                 const std::map<std::string, std::string>& clipMap) {
    auto tex = [&](int& t) {
        if (t >= 0 && t < (int)texMap.size()) t = texMap[(size_t)t];
    };
    if (e.meshRenderer) {
        MeshRenderer& m = *e.meshRenderer;
        if (m.mesh >= 0 && m.mesh < (int)meshMap.size()) m.mesh = meshMap[(size_t)m.mesh];
        tex(m.texture);
        tex(m.metalRoughTexture);
        tex(m.normalTexture);
        tex(m.emissiveTexture);
    }
    if (e.animator) {
        auto clip = [&](std::string& name) {
            auto it = clipMap.find(name);
            if (it != clipMap.end()) name = it->second;
        };
        Animator& a = *e.animator;
        clip(a.clip);
        clip(a.fadeFrom);
        for (AnimationEvent& ev : a.events) clip(ev.clip);
        for (AnimatorState& s : a.states) clip(s.clip);
    }
}

}  // namespace

void mergeSceneAssets(Scene& scene, const MeshLibrary& fromMeshes, const TextureLibrary& fromTextures,
                      MeshLibrary& meshes, TextureLibrary& textures) {
    // Meshes: built-ins keep their ids; imported ones are found by source or copied over
    std::vector<int> meshMap((size_t)fromMeshes.count());
    for (int i = 0; i < fromMeshes.count(); ++i) {
        const Mesh& m = fromMeshes.get(i);
        meshMap[(size_t)i] = i;
        if (i < (int)MeshKind::Count) continue;
        int found = -1;
        for (int k = 0; k < meshes.count() && found < 0 && !m.source.empty(); ++k)
            if (meshes.get(k).source == m.source) found = k;
        if (found < 0) {
            size_t first = (size_t)m.vertexOffset;
            size_t end = i + 1 < fromMeshes.count() ? (size_t)fromMeshes.get(i + 1).vertexOffset : fromMeshes.vertices.size();
            std::vector<Vertex> verts(fromMeshes.vertices.begin() + (long)first, fromMeshes.vertices.begin() + (long)end);
            std::vector<uint32_t> idx(fromMeshes.indices.begin() + m.firstIndex,
                                      fromMeshes.indices.begin() + m.firstIndex + m.indexCount);
            if (m.skinned) {
                std::vector<SkinVertex> skin(fromMeshes.skin.begin() + (long)first, fromMeshes.skin.begin() + (long)end);
                found = addSkinnedMesh(meshes, m.name, m.source, verts, skin, idx);
            } else {
                found = addMesh(meshes, m.name, m.source, verts, idx);
            }
        }
        meshMap[(size_t)i] = found;
    }
    // Clips: by source too (names may differ if the game's library already had one with that name)
    std::map<std::string, std::string> clipMap;
    for (const AnimationClip& c : fromMeshes.animations) {
        const AnimationClip* have = nullptr;
        for (const AnimationClip& o : meshes.animations)
            if (!c.source.empty() && o.source == c.source) have = &o;
        if (!have) {
            AnimationClip copy = c;
            for (int n = 2; meshes.findAnimation(copy.name); ++n) copy.name = c.name + " " + std::to_string(n);
            meshes.animations.push_back(std::move(copy));
            have = &meshes.animations.back();
        }
        if (have->name != c.name) clipMap[c.name] = have->name;
    }
    // Textures: the same
    std::vector<int> texMap((size_t)fromTextures.count());
    for (int i = 0; i < fromTextures.count(); ++i) {
        texMap[(size_t)i] = i;
        if (i < kBuiltinTextureCount) continue;
        const Texture& t = fromTextures.get(i);
        int found = -1;
        for (int k = 0; k < textures.count() && found < 0 && !t.source.empty(); ++k)
            if (textures.get(k).source == t.source) found = k;
        if (found < 0) found = addTexture(textures, t.name, t.source, t.width, t.height, t.rgba);
        texMap[(size_t)i] = found;
    }
    for (Entity& e : scene.entities) remapEntity(e, meshMap, texMap, clipMap);
    for (Prefab& p : scene.prefabs)
        for (Entity& e : p.entities) remapEntity(e, meshMap, texMap, clipMap);
}

SceneLoader::~SceneLoader() {
    if (result_.valid()) result_.wait();
}

void SceneLoader::start(const std::string& path) {
    if (result_.valid()) result_.wait();   // (abandoned: its libraries are its own, so just let it finish)
    path_ = path;
    started_ = std::chrono::steady_clock::now();
    const MeshLibrary& baseMeshes = builtinMeshes();   // made here, on the main thread, the first time
    const TextureLibrary& baseTextures = builtinTextures();
    result_ = std::async(std::launch::async, [path, &baseMeshes, &baseTextures]() {
        auto loaded = std::make_shared<Loaded>();
        loaded->meshes = baseMeshes;
        loaded->textures = baseTextures;
        loaded->ok = loadScene(path, loaded->scene, loaded->meshes, loaded->textures, loaded->error);
        return loaded;
    });
}

bool SceneLoader::ready() const {
    return result_.valid() && result_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

bool SceneLoader::finish(Scene& out, MeshLibrary& meshes, TextureLibrary& textures, std::string& error) {
    if (!result_.valid()) { error = "nothing is loading"; return false; }
    std::shared_ptr<Loaded> loaded = result_.get();
    if (!loaded->ok) { error = loaded->error; return false; }
    mergeSceneAssets(loaded->scene, loaded->meshes, loaded->textures, meshes, textures);
    out = std::move(loaded->scene);
    return true;
}

float SceneLoader::seconds() const {
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - started_).count();
}
