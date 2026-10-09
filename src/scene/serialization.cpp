#include "scene/serialization.h"

#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

#include "core/json.h"

namespace {

constexpr const char* kFormat = "vkworld-scene";
constexpr int kVersion = 7;   // 7: rigid bodies; 6: linked prefabs; 5: textures, lights; 4: prefabs; 3: entities

json::Value vec3(Vec3 v) {
    float a[3] = {v.x, v.y, v.z};
    return json::Value::vec3(a);
}

Vec3 readVec3(const json::Value& v, Vec3 def) {
    float a[3] = {def.x, def.y, def.z};
    v.readVec3(a);
    return {a[0], a[1], a[2]};
}

// Built-in meshes by their own names, so files don't depend on library ids
std::map<std::string, int> builtinMeshIds(const MeshLibrary& meshes) {
    std::map<std::string, int> ids;
    for (int i = 0; i < (int)MeshKind::Count; ++i) ids[meshes.name(i)] = i;
    return ids;
}

const char* lightTypeKey(Light::Type t) { return lightTypeName(t); }

// Behaviour fields -> JSON members
struct FieldWriter : FieldVisitor {
    json::Value& out;
    explicit FieldWriter(json::Value& o) : out(o) {}
    void field(const char* name, float& v, float, float, float) override { out.set(name, v); }
    void field(const char* name, bool& v) override { out.set(name, v); }
    void field(const char* name, Vec3& v, float) override { out.set(name, vec3(v)); }
    void field(const char* name, std::string& v) override { out.set(name, v); }
};

// JSON members -> behaviour fields (missing members keep the behaviour's defaults)
struct FieldReader : FieldVisitor {
    const json::Value& in;
    explicit FieldReader(const json::Value& i) : in(i) {}
    void field(const char* name, float& v, float, float, float) override { v = in[name].asFloat(v); }
    void field(const char* name, bool& v) override { v = in[name].asBool(v); }
    void field(const char* name, Vec3& v, float) override { v = readVec3(in[name], v); }
    void field(const char* name, std::string& v) override {
        if (in[name].type() == json::Value::Type::String) v = in[name].asString();
    }
};

bool readFile(const std::string& path, std::string& text) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}

// ---------------------------------------------------------------------------------------------
// Legacy text format (v1/v2, plus the camera extension): one object per line, all at the root
// ---------------------------------------------------------------------------------------------
bool loadLegacy(const std::string& path, const std::string& text, Scene& scene, MeshLibrary& meshes,
                std::string& error) {
    Scene result;
    std::map<std::string, int> meshIds = builtinMeshIds(meshes);
    std::istringstream file(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(file, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream in(line);
        auto fail = [&](const std::string& msg) { error = path + ":" + std::to_string(lineNo) + ": " + msg; return false; };
        if (line.compare(0, 5, "mesh ") == 0) {
            std::string keyword, name, source;
            in >> keyword >> std::quoted(name) >> std::quoted(source);
            if (!in) return fail("malformed mesh line");
            std::string importError;
            int id = importMesh(meshes, source, importError);
            if (id < 0) return fail(importError);
            meshIds[name] = id;
            continue;
        }
        Entity e;
        std::string mesh;
        MeshRenderer mr;
        float spin = 0.f, bob = 0.f, phase = 0.f;
        in >> std::quoted(e.name) >> std::quoted(mesh);
        for (float& v : e.transform.position) in >> v;
        for (float& v : e.transform.rotation) in >> v;
        for (float& v : e.transform.scale) in >> v;
        in >> mr.color.x >> mr.color.y >> mr.color.z >> mr.checker >> e.visible >> e.locked >> spin >> bob >> phase;
        if (!in) return fail("malformed line");
        std::string extra;
        if (in >> extra && extra == "camera") {
            Camera cam;
            in >> cam.fovY;
            e.camera = cam;
        }
        if (mesh != "None") {
            auto it = meshIds.find(mesh);
            if (it == meshIds.end()) return fail("unknown mesh '" + mesh + "'");
            mr.mesh = it->second;
            e.meshRenderer = mr;
        }
        e.behaviours = motionBehaviours(spin, bob, phase);
        result.add(std::move(e));
    }
    scene = std::move(result);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------------------------
AssetIds builtinAssetIds(const MeshLibrary& meshes, const TextureLibrary& textures) {
    AssetIds ids;
    ids.meshes = builtinMeshIds(meshes);
    for (int i = 0; i < kBuiltinTextureCount && i < textures.count(); ++i) ids.textures[textures.name(i)] = i;
    return ids;
}

json::Value entityToJson(const Entity& e, const MeshLibrary& meshes, const TextureLibrary& textures) {
    json::Value v = json::Value::object();
    v.set("id", e.id);
    v.set("name", e.name);
    if (e.parent != kNoEntity) v.set("parent", e.parent);
    if (e.prefab) {
        json::Value& link = v.set("prefab", json::Value::object());
        link.set("id", e.prefab->prefab);
        link.set("source", e.prefab->source);
    }
    if (!e.visible) v.set("visible", false);
    if (e.locked) v.set("locked", true);

    json::Value& t = v.set("transform", json::Value::object());
    t.set("position", json::Value::vec3(e.transform.position));
    t.set("rotation", json::Value::vec3(e.transform.rotation));
    t.set("scale", json::Value::vec3(e.transform.scale));

    if (e.meshRenderer) {
        json::Value& c = v.set("meshRenderer", json::Value::object());
        c.set("mesh", meshes.name(e.meshRenderer->mesh));
        c.set("color", vec3(e.meshRenderer->color));
        if (e.meshRenderer->texture >= 0 && e.meshRenderer->texture < textures.count())
            c.set("texture", textures.name(e.meshRenderer->texture));
        if (e.meshRenderer->tiling != 1.f) c.set("tiling", e.meshRenderer->tiling);
        if (e.meshRenderer->checker) c.set("checker", true);
    }
    if (e.camera) {
        json::Value& c = v.set("camera", json::Value::object());
        c.set("fov", e.camera->fovY / DEG2RAD);
        c.set("near", e.camera->zNear);
        c.set("far", e.camera->zFar);
    }
    if (e.rigidBody) {
        const RigidBody& rb = *e.rigidBody;
        json::Value& c = v.set("rigidBody", json::Value::object());
        c.set("mass", rb.mass);
        c.set("useGravity", rb.useGravity);
        c.set("bounciness", rb.bounciness);
        c.set("friction", rb.friction);
        c.set("damping", rb.damping);
        if (length(rb.velocity) > 0.f) c.set("velocity", vec3(rb.velocity));
    }
    if (e.light) {
        const Light& l = *e.light;
        json::Value& c = v.set("light", json::Value::object());
        c.set("type", lightTypeKey(l.type));
        c.set("color", vec3(l.color));
        c.set("intensity", l.intensity);
        if (l.type == Light::Type::Point) c.set("range", l.range);
        else c.set("castShadows", l.castShadows);
    }
    if (e.collider) {
        const Collider& col = *e.collider;
        json::Value& c = v.set("collider", json::Value::object());
        c.set("shape", colliderShapeName(col.shape));
        c.set("center", vec3(col.center));
        if (col.shape == Collider::Shape::Box) c.set("size", vec3(col.size));
        if (col.shape == Collider::Shape::Sphere || col.shape == Collider::Shape::Capsule) c.set("radius", col.radius);
        if (col.shape == Collider::Shape::Capsule) c.set("height", col.height);
        if (col.trigger) c.set("trigger", true);
    }
    if (!e.behaviours.empty()) {
        json::Value& list = v.set("behaviours", json::Value::array());
        for (const BehaviourPtr& b : e.behaviours) {
            json::Value bv = json::Value::object();
            bv.set("type", b->typeName());
            FieldWriter writer(bv);
            b->fields(writer);
            list.push(std::move(bv));
        }
    }
    return v;
}

bool entityFromJson(const json::Value& v, const AssetIds& ids, Entity& out, std::string& error) {
    const std::map<std::string, int>& meshIds = ids.meshes;
    if (!v.isObject()) { error = "entity is not an object"; return false; }
    Entity e;
    e.id = (EntityId)v["id"].asNumber(0);
    e.name = v["name"].asString();
    e.parent = (EntityId)v["parent"].asNumber(0);
    if (const json::Value& link = v["prefab"]; link.isObject())
        e.prefab = PrefabLink{(uint32_t)link["id"].asNumber(0), (EntityId)link["source"].asNumber(0)};
    e.visible = v["visible"].asBool(true);
    e.locked = v["locked"].asBool(false);

    const json::Value& t = v["transform"];
    t["position"].readVec3(e.transform.position);
    t["rotation"].readVec3(e.transform.rotation);
    t["scale"].readVec3(e.transform.scale);

    if (const json::Value& c = v["meshRenderer"]; c.isObject()) {
        MeshRenderer mr;
        auto it = meshIds.find(c["mesh"].asString());
        if (it == meshIds.end()) { error = "entity '" + e.name + "': unknown mesh '" + c["mesh"].asString() + "'"; return false; }
        mr.mesh = it->second;
        mr.color = readVec3(c["color"], mr.color);
        mr.checker = c["checker"].asBool(false);
        mr.tiling = c["tiling"].asFloat(1.f);
        if (const std::string& tex = c["texture"].asString(); !tex.empty()) {
            auto t = ids.textures.find(tex);
            if (t == ids.textures.end()) { error = "entity '" + e.name + "': unknown texture '" + tex + "'"; return false; }
            mr.texture = t->second;
        }
        e.meshRenderer = mr;
    }
    if (const json::Value& c = v["camera"]; c.isObject()) {
        Camera cam;
        cam.fovY = c["fov"].asFloat(cam.fovY / DEG2RAD) * DEG2RAD;
        cam.zNear = c["near"].asFloat(cam.zNear);
        cam.zFar = c["far"].asFloat(cam.zFar);
        e.camera = cam;
    }
    if (const json::Value& c = v["rigidBody"]; c.isObject()) {
        RigidBody rb;
        rb.mass = std::max(c["mass"].asFloat(rb.mass), 0.001f);
        rb.useGravity = c["useGravity"].asBool(true);
        rb.bounciness = c["bounciness"].asFloat(rb.bounciness);
        rb.friction = c["friction"].asFloat(rb.friction);
        rb.damping = c["damping"].asFloat(rb.damping);
        rb.velocity = readVec3(c["velocity"], {});
        e.rigidBody = rb;
    }
    if (const json::Value& c = v["light"]; c.isObject()) {
        Light l;
        l.type = c["type"].asString() == "directional" ? Light::Type::Directional : Light::Type::Point;
        l.color = readVec3(c["color"], l.color);
        l.intensity = c["intensity"].asFloat(l.intensity);
        l.range = c["range"].asFloat(l.range);
        l.castShadows = c["castShadows"].asBool(true);
        e.light = l;
    }
    if (const json::Value& c = v["collider"]; c.isObject()) {
        Collider col;
        std::string shape = c["shape"].asString();
        bool known = false;
        for (int i = 0; i <= (int)Collider::Shape::Mesh; ++i)
            if (shape == colliderShapeName((Collider::Shape)i)) { col.shape = (Collider::Shape)i; known = true; }
        if (!known) { error = "entity '" + e.name + "': unknown collider shape '" + shape + "'"; return false; }
        col.center = readVec3(c["center"], col.center);
        col.size = readVec3(c["size"], col.size);
        col.radius = c["radius"].asFloat(col.radius);
        col.height = c["height"].asFloat(col.height);
        col.trigger = c["trigger"].asBool(false);
        e.collider = col;
    }
    if (const json::Value& c = v["animator"]; c.isObject())   // scene format 3 before behaviours existed
        e.behaviours = motionBehaviours(c["spin"].asFloat(0.f), c["bob"].asFloat(0.f), c["phase"].asFloat(0.f));
    for (const json::Value& bv : v["behaviours"].elements()) {
        std::unique_ptr<Behaviour> b = createBehaviour(bv["type"].asString());
        if (!b) {
            error = "entity '" + e.name + "': unknown behaviour type '" + bv["type"].asString() + "'";
            return false;
        }
        FieldReader reader(bv);
        b->fields(reader);
        e.behaviours.emplace_back(std::move(b));
    }
    out = std::move(e);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Shared by scene and prefab files
// ---------------------------------------------------------------------------------------------
namespace {

// "meshes" and "textures": the imported ones the entities use, by name and source file
void assetListsToJson(json::Value& doc, const std::vector<const std::vector<Entity>*>& lists, const MeshLibrary& meshes,
                      const TextureLibrary& textures) {
    std::set<int> usedMeshes, usedTextures;
    for (const auto* list : lists)
        for (const Entity& e : *list) {
            if (!e.meshRenderer) continue;
            if (e.meshRenderer->mesh >= (int)MeshKind::Count) usedMeshes.insert(e.meshRenderer->mesh);
            if (e.meshRenderer->texture >= kBuiltinTextureCount) usedTextures.insert(e.meshRenderer->texture);
        }
    json::Value& m = doc.set("meshes", json::Value::array());
    for (int id : usedMeshes) {
        json::Value v = json::Value::object();
        v.set("name", meshes.name(id));
        v.set("source", meshes.get(id).source);
        m.push(std::move(v));
    }
    json::Value& t = doc.set("textures", json::Value::array());
    for (int id : usedTextures) {
        json::Value v = json::Value::object();
        v.set("name", textures.name(id));
        v.set("source", textures.get(id).source);
        t.push(std::move(v));
    }
}

// Imports the listed meshes and textures; fills the file's names -> library ids
bool assetListsFromJson(const json::Value& doc, MeshLibrary& meshes, TextureLibrary& textures, AssetIds& ids,
                        std::string& error) {
    ids = builtinAssetIds(meshes, textures);
    for (const json::Value& m : doc["meshes"].elements()) {
        int id = importMesh(meshes, m["source"].asString(), error);
        if (id < 0) return false;
        ids.meshes[m["name"].asString()] = id;
    }
    for (const json::Value& t : doc["textures"].elements()) {
        int id = importTexture(textures, t["source"].asString(), error);
        if (id < 0) return false;
        ids.textures[t["name"].asString()] = id;
    }
    return true;
}

json::Value prefabToJson(const Prefab& p, const MeshLibrary& meshes, const TextureLibrary& textures) {
    json::Value v = json::Value::object();
    v.set("id", p.id);
    v.set("name", p.name);
    json::Value& ents = v.set("entities", json::Value::array());
    for (const Entity& e : p.entities) ents.push(entityToJson(e, meshes, textures));
    return v;
}

bool prefabFromJson(const json::Value& v, const AssetIds& ids, Prefab& out, std::string& error) {
    Prefab p;
    p.id = (uint32_t)v["id"].asNumber(0);
    p.name = v["name"].asString();
    for (const json::Value& ev : v["entities"].elements()) {
        Entity e;
        if (!entityFromJson(ev, ids, e, error)) { error = "prefab '" + p.name + "': " + error; return false; }
        p.entities.push_back(std::move(e));
    }
    if (p.entities.empty()) { error = "prefab '" + p.name + "' has no entities"; return false; }
    p.entities[0].parent = kNoEntity;
    out = std::move(p);
    return true;
}

bool writeJson(const std::string& path, const json::Value& doc, std::string& error) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { error = "cannot write " + path; return false; }
    f << json::write(doc);
    if (!f) { error = "write failed for " + path; return false; }
    return true;
}

bool readJson(const std::string& path, const char* format, int maxVersion, json::Value& doc, std::string& error) {
    std::string text;
    if (!readFile(path, text)) { error = "cannot open " + path; return false; }
    if (!json::parse(text, doc, error)) { error = path + ": " + error; return false; }
    if (doc["format"].asString() != format) { error = path + ": not a " + std::string(format) + " file"; return false; }
    if (doc["version"].asInt() > maxVersion) {
        error = path + ": made by a newer version (format " + std::to_string(doc["version"].asInt()) + ")";
        return false;
    }
    return true;
}

constexpr const char* kPrefabFormat = "vkworld-prefab";
constexpr int kPrefabVersion = 1;

}  // namespace

// ---------------------------------------------------------------------------------------------
// Scenes
// ---------------------------------------------------------------------------------------------
bool saveScene(const std::string& path, const Scene& scene, const MeshLibrary& meshes, const TextureLibrary& textures,
               std::string& error) {
    json::Value doc = json::Value::object();
    doc.set("format", kFormat);
    doc.set("version", kVersion);
    std::vector<const std::vector<Entity>*> lists = {&scene.entities};
    for (const Prefab& p : scene.prefabs) lists.push_back(&p.entities);
    assetListsToJson(doc, lists, meshes, textures);
    json::Value& ents = doc.set("entities", json::Value::array());
    for (const Entity& e : scene.entities) ents.push(entityToJson(e, meshes, textures));
    if (!scene.prefabs.empty()) {
        json::Value& prefabs = doc.set("prefabs", json::Value::array());
        for (const Prefab& p : scene.prefabs) prefabs.push(prefabToJson(p, meshes, textures));
    }
    json::Value& settings = doc.set("settings", json::Value::object());
    json::Value& input = settings.set("input", json::Value::array());
    for (const InputAction& a : scene.inputActions) {
        json::Value av = json::Value::object();
        av.set("name", a.name);
        json::Value& keys = av.set("keys", json::Value::array());
        for (ImGuiKey k : a.keys) keys.push(keyName(k));
        if (a.mouseButton >= 0) av.set("mouseButton", a.mouseButton);
        input.push(std::move(av));
    }
    return writeJson(path, doc, error);
}

bool loadScene(const std::string& path, Scene& scene, MeshLibrary& meshes, TextureLibrary& textures, std::string& error) {
    std::string text;
    if (!readFile(path, text)) { error = "cannot open " + path; return false; }
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || text[first] != '{') return loadLegacy(path, text, scene, meshes, error);

    json::Value doc;
    if (!readJson(path, kFormat, kVersion, doc, error)) return false;
    AssetIds ids;
    if (!assetListsFromJson(doc, meshes, textures, ids, error)) { error = path + ": " + error; return false; }

    Scene result;
    for (const json::Value& v : doc["entities"].elements()) {
        Entity e;
        if (!entityFromJson(v, ids, e, error)) { error = path + ": " + error; return false; }
        EntityId fileId = e.id;
        if (result.add(std::move(e)).id != fileId && fileId != kNoEntity) {   // id-less entities just get a new one
            error = path + ": duplicate entity id " + std::to_string(fileId);
            return false;
        }
    }
    // Repair references a hand-edited file might break: missing parents become roots, cycles are cut
    for (Entity& e : result.entities) {
        if (e.parent != kNoEntity && !result.find(e.parent)) e.parent = kNoEntity;
        if (e.parent != kNoEntity && result.isAncestor(e.id, e.parent)) e.parent = kNoEntity;
    }
    for (const json::Value& v : doc["prefabs"].elements()) {
        Prefab p;
        if (!prefabFromJson(v, ids, p, error)) { error = path + ": " + error; return false; }
        p.name = result.uniquePrefabName(p.name);
        result.addPrefab(std::move(p));
    }
    if (const json::Value& input = doc["settings"]["input"]; input.isArray()) {   // else: the defaults
        result.inputActions.clear();
        for (const json::Value& av : input.elements()) {
            InputAction a;
            a.name = av["name"].asString();
            for (const json::Value& k : av["keys"].elements())
                if (ImGuiKey key = keyFromName(k.asString()); key != ImGuiKey_None) a.keys.push_back(key);
            a.mouseButton = av["mouseButton"].asInt(-1);
            if (!a.name.empty()) result.inputActions.push_back(std::move(a));
        }
    }
    scene = std::move(result);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Prefab files
// ---------------------------------------------------------------------------------------------
bool savePrefab(const std::string& path, const Prefab& prefab, const MeshLibrary& meshes, const TextureLibrary& textures,
                std::string& error) {
    json::Value doc = json::Value::object();
    doc.set("format", kPrefabFormat);
    doc.set("version", kPrefabVersion);
    assetListsToJson(doc, {&prefab.entities}, meshes, textures);
    doc.set("prefab", prefabToJson(prefab, meshes, textures));
    return writeJson(path, doc, error);
}

bool loadPrefab(const std::string& path, Prefab& prefab, MeshLibrary& meshes, TextureLibrary& textures,
                std::string& error) {
    json::Value doc;
    if (!readJson(path, kPrefabFormat, kPrefabVersion, doc, error)) return false;
    AssetIds ids;
    if (!assetListsFromJson(doc, meshes, textures, ids, error)) { error = path + ": " + error; return false; }
    if (!prefabFromJson(doc["prefab"], ids, prefab, error)) { error = path + ": " + error; return false; }
    return true;
}
