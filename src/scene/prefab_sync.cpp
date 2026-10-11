#include "scene/prefab_sync.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "core/json.h"
#include "scene/serialization.h"

namespace {

// Every library name -> id (entityFromJson resolves meshes and textures by name)
AssetIds allAssetIds(const MeshLibrary& meshes, const TextureLibrary& textures) {
    AssetIds ids;
    for (int i = 0; i < meshes.count(); ++i) ids.meshes[meshes.name(i)] = i;
    for (int i = 0; i < textures.count(); ++i) ids.textures[textures.name(i)] = i;
    return ids;
}

bool same(const json::Value& a, const json::Value& b) { return json::write(a) == json::write(b); }

// Three-way merge of one entity: `inst` is the instance, `before` / `after` the prefab entity before and after the
// change. Objects merge key by key; anything else is a single value: if the instance still has the prefab's old
// value it takes the new one, otherwise (an override) it keeps its own.
json::Value merge(const json::Value& inst, const json::Value& before, const json::Value& after) {
    if (!(inst.isObject() && before.isObject() && after.isObject())) return same(inst, before) ? after : inst;
    json::Value out = json::Value::object();
    std::vector<std::string> keys;
    std::set<std::string> seen;
    for (const json::Value* v : {&after, &inst, &before})
        for (const auto& m : v->members())
            if (seen.insert(m.first).second) keys.push_back(m.first);
    for (const std::string& k : keys) {
        json::Value merged = merge(inst[k], before[k], after[k]);
        if (!merged.isNull()) out.set(k, std::move(merged));
    }
    return out;
}

// Entities of the instance under `root` that are linked to `prefab` (nested instances of other prefabs aren't)
std::vector<EntityId> instanceEntities(const Scene& scene, EntityId root, uint32_t prefab) {
    std::vector<EntityId> out;
    for (EntityId id : scene.subtree(root))
        if (const Entity* e = scene.find(id); e->prefab && e->prefab->prefab == prefab) out.push_back(id);
    return out;
}

const Entity* prefabEntity(const Prefab& p, EntityId source) {
    for (const Entity& e : p.entities)
        if (e.id == source) return &e;
    return nullptr;
}

// Rewrites one instance from `before` to `after`
void syncInstance(Scene& scene, EntityId root, const Prefab& before, const Prefab& after, const MeshLibrary& meshes,
                  const TextureLibrary& textures, bool revert) {
    AssetIds ids = allAssetIds(meshes, textures);
    std::map<EntityId, EntityId> bySource;   // prefab entity -> instance entity
    for (EntityId id : instanceEntities(scene, root, after.id)) bySource[scene.find(id)->prefab->source] = id;

    std::vector<EntityId> removed;
    for (const auto& [source, id] : bySource) {
        Entity& e = *scene.find(id);
        const Entity* b = prefabEntity(before, source);
        const Entity* a = prefabEntity(after, source);
        if (!a) {   // gone from the prefab
            removed.push_back(id);
            continue;
        }
        json::Value instJson = entityToJson(e, meshes, textures);
        json::Value afterJson = entityToJson(*a, meshes, textures);
        json::Value merged;
        if (revert) {
            merged = afterJson;   // everything from the prefab, except where the instance is placed
            if (id == root) {
                merged.set("name", instJson["name"]);
                json::Value t = afterJson["transform"];
                t.set("position", instJson["transform"]["position"]);
                merged.set("transform", t);
            }
        } else {
            merged = merge(instJson, b ? entityToJson(*b, meshes, textures) : afterJson, afterJson);
        }
        Entity updated;
        std::string error;
        if (!entityFromJson(merged, ids, updated, error)) continue;
        updated.id = e.id;   // identity stays the instance's
        updated.parent = e.parent;
        updated.prefab = e.prefab;
        updated.nestedPrefab = a->prefab;
        e = std::move(updated);
    }
    for (EntityId id : removed) scene.remove(id);

    // New in the prefab: create them under the matching instance entity (parents come first in a prefab)
    for (const Entity& a : after.entities) {
        if (bySource.count(a.id)) continue;
        auto parent = bySource.find(a.parent);
        if (parent == bySource.end()) continue;
        Entity e = a;
        e.id = scene.reserveId();
        e.parent = parent->second;
        e.nestedPrefab = a.prefab;
        e.prefab = PrefabLink{after.id, a.id};
        bySource[a.id] = e.id;
        scene.add(std::move(e));
    }
}

}  // namespace

EntityId instanceRoot(const Scene& scene, EntityId id) {
    const Entity* e = scene.find(id);
    if (!e || !e->prefab) return kNoEntity;
    const Prefab* p = scene.findPrefab(e->prefab->prefab);
    if (!p || p->entities.empty()) return kNoEntity;
    // Walk up while the entities belong to the same prefab; the one linked to the prefab's root is the root
    int depth = 0;
    for (; e && depth < 256; e = scene.find(e->parent), ++depth) {
        if (!e->prefab || e->prefab->prefab != p->id) return kNoEntity;
        if (e->prefab->source == p->entities[0].id) return e->id;
    }
    return kNoEntity;
}

// The new version of `prefab` made from the tree under `root`. The root keeps the prefab root's id (instances are
// matched by it), linked entities keep their source ids, anything else gets a new id. `ids` gets scene -> prefab ids.
static Prefab prefabFromTree(const Scene& scene, EntityId root, const Prefab& prefab, std::map<EntityId, EntityId>& ids) {
    Prefab p;
    p.id = prefab.id;
    p.name = prefab.name;
    EntityId next = 1;
    for (const Entity& e : prefab.entities) next = std::max(next, e.id + 1);
    EntityId rootId = prefab.entities.empty() ? next++ : prefab.entities[0].id;
    for (EntityId id : scene.subtree(root)) {
        const Entity* e = scene.find(id);
        bool linked = id != root && e->prefab && e->prefab->prefab == prefab.id &&
                      e->prefab->source != rootId && prefabEntity(prefab, e->prefab->source);
        ids[id] = id == root ? rootId : linked ? e->prefab->source : next++;
    }
    for (EntityId id : scene.subtree(root)) {
        Entity e = *scene.find(id);
        e.id = ids[id];
        e.parent = id == root ? kNoEntity : ids[e.parent];
        // What it's an instance of inside the prefab: an inner prefab it came from, or another one it is now
        bool ours = e.prefab && e.prefab->prefab == prefab.id;
        e.prefab = ours ? e.nestedPrefab : e.prefab;
        e.nestedPrefab.reset();
        if (id == root) {   // the root's placement and name belong to the instance
            e.transform.position[0] = e.transform.position[1] = e.transform.position[2] = 0.f;
            e.name = prefab.entities.empty() ? e.name : prefab.entities[0].name;
        }
        p.entities.push_back(std::move(e));
    }
    return p;
}

int applyToPrefab(Scene& scene, EntityId root, uint32_t prefabId, const MeshLibrary& meshes,
                  const TextureLibrary& textures) {
    Prefab* prefab = scene.findPrefab(prefabId);
    if (!prefab || !scene.find(root)) return -1;
    Prefab before = *prefab;
    std::map<EntityId, EntityId> ids;
    Prefab after = prefabFromTree(scene, root, before, ids);
    for (const auto& [sceneId, prefabEntityId] : ids) {
        Entity* e = scene.find(sceneId);
        e->prefab = PrefabLink{prefabId, prefabEntityId};
        e->nestedPrefab = prefabEntity(after, prefabEntityId)->prefab;
    }
    *prefab = after;
    return propagatePrefabChange(scene, before, after, meshes, textures, root);
}

int propagatePrefabChange(Scene& scene, const Prefab& before, const Prefab& after, const MeshLibrary& meshes,
                          const TextureLibrary& textures, EntityId skipRoot, int depth) {
    int count = syncPrefabInstances(scene, before, after, meshes, textures, skipRoot);
    if (depth >= 8) return count;   // (prefabs containing each other: stop somewhere)
    // Prefabs with instances of this one inside them: update those as if each were a little scene, then their own
    // instances (and the prefabs that contain them...)
    std::vector<uint32_t> containing;
    for (const Prefab& p : scene.prefabs) {
        if (p.id == after.id || before.entities.empty()) continue;
        for (const Entity& e : p.entities)
            if (e.prefab && e.prefab->prefab == after.id && e.prefab->source == before.entities[0].id) {
                containing.push_back(p.id);
                break;
            }
    }
    for (uint32_t id : containing) {
        Prefab* outer = scene.findPrefab(id);
        if (!outer) continue;
        Prefab old = *outer;
        Scene inner;
        for (const Entity& e : old.entities) inner.add(e);
        syncPrefabInstances(inner, before, after, meshes, textures);
        outer = scene.findPrefab(id);
        outer->entities.clear();
        const Entity* root = inner.find(old.entities[0].id);
        if (root) outer->entities.push_back(*root);   // (the root first)
        for (const Entity& e : inner.entities)
            if (e.id != old.entities[0].id) outer->entities.push_back(e);
        for (Entity& e : outer->entities) e.nestedPrefab.reset();
        Prefab now = *outer;
        count += propagatePrefabChange(scene, old, now, meshes, textures, kNoEntity, depth + 1);
    }
    return count;
}

int syncPrefabInstances(Scene& scene, const Prefab& before, const Prefab& after, const MeshLibrary& meshes,
                        const TextureLibrary& textures, EntityId skipRoot) {
    std::vector<EntityId> roots;
    for (const Entity& e : scene.entities)
        if (e.prefab && e.prefab->prefab == after.id && e.id != skipRoot && !before.entities.empty() &&
            e.prefab->source == before.entities[0].id)
            roots.push_back(e.id);
    for (EntityId root : roots) syncInstance(scene, root, before, after, meshes, textures, false);
    return (int)roots.size();
}

void revertInstance(Scene& scene, EntityId root, const MeshLibrary& meshes, const TextureLibrary& textures) {
    const Entity* r = scene.find(root);
    if (!r || !r->prefab) return;
    const Prefab* p = scene.findPrefab(r->prefab->prefab);
    if (!p) return;
    Prefab copy = *p;
    syncInstance(scene, root, copy, copy, meshes, textures, true);
}

void unlinkInstance(Scene& scene, EntityId root) {
    const Entity* r = scene.find(root);
    if (!r || !r->prefab) return;
    uint32_t prefab = r->prefab->prefab;
    for (EntityId id : instanceEntities(scene, root, prefab)) scene.find(id)->prefab.reset();
}

std::vector<std::string> instanceOverrides(const Scene& scene, EntityId id, const MeshLibrary& meshes,
                                           const TextureLibrary& textures) {
    std::vector<std::string> out;
    const Entity* e = scene.find(id);
    if (!e || !e->prefab) return out;
    const Prefab* p = scene.findPrefab(e->prefab->prefab);
    const Entity* src = p ? prefabEntity(*p, e->prefab->source) : nullptr;
    if (!src) return out;
    json::Value a = entityToJson(*e, meshes, textures), b = entityToJson(*src, meshes, textures);
    bool isRoot = instanceRoot(scene, id) == id;
    // Leaf-by-leaf comparison; identity fields (and the root's name) aren't overrides
    std::function<void(const json::Value&, const json::Value&, const std::string&)> walk =
        [&](const json::Value& x, const json::Value& y, const std::string& path) {
            if (x.isObject() && y.isObject()) {
                std::set<std::string> keys;
                for (const auto& m : x.members()) keys.insert(m.first);
                for (const auto& m : y.members()) keys.insert(m.first);
                for (const std::string& k : keys) walk(x[k], y[k], path.empty() ? k : path + "." + k);
                return;
            }
            if (!same(x, y)) out.push_back(path);
        };
    walk(a, b, "");
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const std::string& p) {
                                 return p == "id" || p == "parent" || p.rfind("prefab", 0) == 0 ||
                                        p.rfind("nestedPrefab", 0) == 0 || (isRoot && p == "name");
                             }),
              out.end());
    return out;
}
