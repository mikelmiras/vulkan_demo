#include "scene/serialization.h"

#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

#include "core/json.h"

namespace {

constexpr const char* kFormat = "vkworld-scene";
constexpr int kVersion = 3;

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
            int id = importObj(meshes, source, importError);
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
json::Value entityToJson(const Entity& e, const MeshLibrary& meshes) {
    json::Value v = json::Value::object();
    v.set("id", e.id);
    v.set("name", e.name);
    if (e.parent != kNoEntity) v.set("parent", e.parent);
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
        if (e.meshRenderer->checker) c.set("checker", true);
    }
    if (e.camera) {
        json::Value& c = v.set("camera", json::Value::object());
        c.set("fov", e.camera->fovY / DEG2RAD);
        c.set("near", e.camera->zNear);
        c.set("far", e.camera->zFar);
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

bool entityFromJson(const json::Value& v, const std::map<std::string, int>& meshIds, Entity& out, std::string& error) {
    if (!v.isObject()) { error = "entity is not an object"; return false; }
    Entity e;
    e.id = (EntityId)v["id"].asNumber(0);
    e.name = v["name"].asString();
    e.parent = (EntityId)v["parent"].asNumber(0);
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
        e.meshRenderer = mr;
    }
    if (const json::Value& c = v["camera"]; c.isObject()) {
        Camera cam;
        cam.fovY = c["fov"].asFloat(cam.fovY / DEG2RAD) * DEG2RAD;
        cam.zNear = c["near"].asFloat(cam.zNear);
        cam.zFar = c["far"].asFloat(cam.zFar);
        e.camera = cam;
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
// Scenes
// ---------------------------------------------------------------------------------------------
bool saveScene(const std::string& path, const Scene& scene, const MeshLibrary& meshes, std::string& error) {
    json::Value doc = json::Value::object();
    doc.set("format", kFormat);
    doc.set("version", kVersion);

    std::set<int> used;
    for (const Entity& e : scene.entities)
        if (e.meshRenderer && e.meshRenderer->mesh >= (int)MeshKind::Count) used.insert(e.meshRenderer->mesh);
    json::Value& list = doc.set("meshes", json::Value::array());
    for (int id : used) {
        json::Value m = json::Value::object();
        m.set("name", meshes.name(id));
        m.set("source", meshes.get(id).source);
        list.push(std::move(m));
    }
    json::Value& ents = doc.set("entities", json::Value::array());
    for (const Entity& e : scene.entities) ents.push(entityToJson(e, meshes));

    std::ofstream f(path, std::ios::binary);
    if (!f) { error = "cannot write " + path; return false; }
    f << json::write(doc);
    if (!f) { error = "write failed for " + path; return false; }
    return true;
}

bool loadScene(const std::string& path, Scene& scene, MeshLibrary& meshes, std::string& error) {
    std::string text;
    if (!readFile(path, text)) { error = "cannot open " + path; return false; }
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || text[first] != '{') return loadLegacy(path, text, scene, meshes, error);

    json::Value doc;
    if (!json::parse(text, doc, error)) { error = path + ": " + error; return false; }
    if (doc["format"].asString() != kFormat) { error = path + ": not a vkworld scene"; return false; }
    if (doc["version"].asInt() > kVersion) {
        error = path + ": made by a newer version (scene format " + std::to_string(doc["version"].asInt()) + ")";
        return false;
    }

    std::map<std::string, int> meshIds = builtinMeshIds(meshes);
    for (const json::Value& m : doc["meshes"].elements()) {
        std::string importError;
        int id = importObj(meshes, m["source"].asString(), importError);
        if (id < 0) { error = path + ": " + importError; return false; }
        meshIds[m["name"].asString()] = id;
    }

    Scene result;
    for (const json::Value& v : doc["entities"].elements()) {
        Entity e;
        if (!entityFromJson(v, meshIds, e, error)) { error = path + ": " + error; return false; }
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
    scene = std::move(result);
    return true;
}
