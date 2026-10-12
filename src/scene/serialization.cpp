#include "scene/serialization.h"
#include "assets/asset_db.h"
#include "scene/terrain.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

#include "core/vfs.h"
#include "assets/gltf.h"
#include "core/json.h"
#include "core/project.h"

namespace {

constexpr const char* kFormat = "vkworld-scene";
constexpr int kVersion = 36;  // 36: soft bodies, fracture, buoyancy; 35: network relevance; 34: AI brains; 33: terrain holes and foliage; 32: light cookies; 31: audio occlusion; 30: vehicles; 29: replication, network settings; 28: UI elements; 27: 2D blend spaces, additive layers, IK; 26: reflection probes, sky images; 25: terrains; 24: asset ids next to file references (assets/asset_db.h); 23: audio buses, streaming, Doppler, reverb zones; 22: ambient occlusion settings; 21: navigation links; 20: blend trees, animation layers, root motion; 19: joints, kinematic bodies; 18: tags, layers; 17: navigation settings; 16: animation events and state machines; 15: alpha modes; 14: spot lights, sky colours; 13: particle emitters; 12: PBR materials, render settings; 11: skins, animators; 10: pause action; 9: quicksave / quickload actions; 8: audio sources; 7: rigid bodies; 6: linked prefabs; 5: textures, lights; 4: prefabs; 3: entities

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
    void field(const char* name, int& v, int, int) override { out.set(name, v); }
    void choice(const char* name, int& v, const char* const* options, int count) override {
        if (v >= 0 && v < count) out.set(name, options[v]);   // by name: options may be reordered or added to
        else out.set(name, v);
    }
    void field(const char* name, std::string& v, FieldRef kind) override {
        out.set(name, v);
        if (kind != FieldRef::Scene && kind != FieldRef::Sound) return;
        std::string id = assetdb::idOfStored(v);   // (so the reference survives the file being moved)
        if (!id.empty()) refs.set(name, id);
    }
    json::Value refs = json::Value::object();   // field -> the id of the file it names ("$refs")
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
    void field(const char* name, int& v, int, int) override {
        // (rounded: an int field may have been a float before)
        if (in[name].type() == json::Value::Type::Number) v = (int)std::lround(in[name].asNumber());
    }
    void choice(const char* name, int& v, const char* const* options, int count) override {
        const json::Value& j = in[name];
        if (j.type() == json::Value::Type::String) {
            for (int i = 0; i < count; ++i)
                if (j.asString() == options[i]) v = i;
        } else if (j.type() == json::Value::Type::Number && j.asInt() >= 0 && j.asInt() < count) {
            v = j.asInt();
        }
    }
    void field(const char* name, std::string& v, FieldRef kind) override {
        field(name, v);
        if (kind == FieldRef::Scene || kind == FieldRef::Sound) v = assetdb::heal(v, in["$refs"][name].asString());
    }
};

bool readFile(const std::string& path, std::string& text) { return vfs::read(std::filesystem::u8path(path), text); }

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
            int id = importMesh(meshes, project::resolve(source), importError);
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

json::Value behaviourToJson(const Behaviour& b) {
    json::Value bv = json::Value::object();
    bv.set("type", b.typeName());
    if (auto* missing = dynamic_cast<const MissingBehaviour*>(&b)) {
        json::Value kept;
        std::string error;
        if (json::parse(missing->fieldsJson(), kept, error))
            for (const auto& [key, value] : kept.members()) bv.set(key, value);
        return bv;
    }
    FieldWriter writer(bv);
    const_cast<Behaviour&>(b).fields(writer);   // fields() only reads here
    if (writer.refs.size()) bv.set("$refs", writer.refs);
    return bv;
}

std::unique_ptr<Behaviour> behaviourFromJson(const json::Value& bv) {
    std::string type = bv["type"].asString();
    if (std::unique_ptr<Behaviour> b = createBehaviour(type)) {
        FieldReader reader(bv);
        b->fields(reader);
        return b;
    }
    json::Value kept = json::Value::object();   // unknown type: keep everything but the type for later
    for (const auto& [key, value] : bv.members())
        if (key != "type") kept.set(key, value);
    return std::make_unique<MissingBehaviour>(type, json::write(kept));
}

json::Value behaviourStateToJson(Behaviour& b) {
    json::Value v = json::Value::object();
    FieldWriter writer(v);
    b.state(writer);
    return v;
}

void readBehaviourFields(Behaviour& b, const json::Value& fields) {
    FieldReader reader(fields);
    b.fields(reader);
}

void behaviourStateFromJson(Behaviour& b, const json::Value& v) {
    FieldReader reader(v);
    b.state(reader);
}

namespace {

template <class F> void forEachBehaviour(Scene& scene, F&& f) {
    for (Entity& e : scene.entities)
        for (BehaviourPtr& b : e.behaviours) f(b);
    for (Prefab& p : scene.prefabs)
        for (Entity& e : p.entities)
            for (BehaviourPtr& b : e.behaviours) f(b);
}

}  // namespace

int detachBehaviours(Scene& scene, const std::vector<std::string>& types) {
    int count = 0;
    forEachBehaviour(scene, [&](BehaviourPtr& b) {
        if (!b || dynamic_cast<MissingBehaviour*>(b.get())) return;
        if (std::find(types.begin(), types.end(), b->typeName()) == types.end()) return;
        json::Value bv = behaviourToJson(*b);
        json::Value kept = json::Value::object();
        for (const auto& [key, value] : bv.members())
            if (key != "type") kept.set(key, value);
        b = BehaviourPtr(std::make_unique<MissingBehaviour>(b->typeName(), json::write(kept)));
        ++count;
    });
    return count;
}

int reattachBehaviours(Scene& scene) {
    int count = 0;
    forEachBehaviour(scene, [&](BehaviourPtr& b) {
        auto* missing = b ? dynamic_cast<MissingBehaviour*>(b.get()) : nullptr;
        if (!missing || !createBehaviour(missing->typeName())) return;
        json::Value bv = behaviourToJson(*missing);   // type + kept fields
        b = BehaviourPtr(behaviourFromJson(bv));
        ++count;
    });
    return count;
}

int countMissingBehaviours(const Scene& scene) {
    int count = 0;
    forEachBehaviour(const_cast<Scene&>(scene), [&](BehaviourPtr& b) { count += b && dynamic_cast<MissingBehaviour*>(b.get()); });
    return count;
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
    if (e.nestedPrefab) {
        json::Value& link = v.set("nestedPrefab", json::Value::object());
        link.set("id", e.nestedPrefab->prefab);
        link.set("source", e.nestedPrefab->source);
    }
    if (!e.visible) v.set("visible", false);
    if (e.locked) v.set("locked", true);
    if (!e.tags.empty()) {
        json::Value& tags = v.set("tags", json::Value::array());
        for (const std::string& t : e.tags) tags.push(t);
    }
    if (e.layer) v.set("layer", (int)e.layer);

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
        const MeshRenderer& mr = *e.meshRenderer;
        auto tex = [&](const char* key, int id) {
            if (id >= 0 && id < textures.count()) c.set(key, textures.name(id));
        };
        if (mr.metallic != 0.f) c.set("metallic", mr.metallic);
        if (mr.roughness != 0.5f) c.set("roughness", mr.roughness);
        tex("metalRoughTexture", mr.metalRoughTexture);
        tex("normalTexture", mr.normalTexture);
        if (mr.normalTexture >= 0 && mr.normalScale != 1.f) c.set("normalScale", mr.normalScale);
        if (mr.emissive.x > 0.f || mr.emissive.y > 0.f || mr.emissive.z > 0.f) {
            c.set("emissive", vec3(mr.emissive));
            if (mr.emissiveStrength != 1.f) c.set("emissiveStrength", mr.emissiveStrength);
        }
        tex("emissiveTexture", mr.emissiveTexture);
        if (mr.alphaMode != MeshRenderer::AlphaMode::Opaque) {
            c.set("alphaMode", alphaModeName(mr.alphaMode));
            c.set("opacity", mr.opacity);
            c.set("alphaCutoff", mr.alphaCutoff);
        }
    }
    if (e.camera) {
        json::Value& c = v.set("camera", json::Value::object());
        c.set("fov", e.camera->fovY / DEG2RAD);
        c.set("near", e.camera->zNear);
        c.set("far", e.camera->zFar);
    }
    if (e.audioSource) {
        const AudioSource& a = *e.audioSource;
        json::Value& c = v.set("audioSource", json::Value::object());
        c.set("clip", a.clip);
        if (std::string id = assetdb::idOfStored(a.clip); !id.empty()) c.set("clipId", id);
        c.set("volume", a.volume);
        c.set("loop", a.loop);
        c.set("playOnStart", a.playOnStart);
        c.set("playOnImpact", a.playOnImpact);
        c.set("spatial", a.spatial);
        c.set("minDistance", a.minDistance);
        c.set("maxDistance", a.maxDistance);
        if (a.bus != "effects") c.set("bus", a.bus);
        if (a.stream) c.set("stream", true);
        if (a.doppler != 1.f) c.set("doppler", a.doppler);
        if (a.occlusion != 1.f) c.set("occlusion", a.occlusion);
    }
    if (e.reverbZone) {
        const ReverbZone& z = *e.reverbZone;
        json::Value& c = v.set("reverbZone", json::Value::object());
        c.set("radius", z.radius);
        c.set("blend", z.blend);
        c.set("wet", z.wet);
        c.set("roomSize", z.roomSize);
        c.set("damping", z.damping);
    }
    if (e.skin) {
        json::Value& c = v.set("skin", json::Value::object());
        json::Value& names = c.set("joints", json::Value::array());
        for (const std::string& j : e.skin->joints) names.push(j);
        json::Value& ibms = c.set("inverseBind", json::Value::array());
        for (const Mat4& m : e.skin->inverseBind) {
            json::Value a = json::Value::array();
            for (float f : m.m) a.push(f);
            ibms.push(std::move(a));
        }
    }
    if (e.animator) {
        const Animator& a = *e.animator;
        json::Value& c = v.set("animator", json::Value::object());
        c.set("clip", a.clip);
        c.set("speed", a.speed);
        c.set("loop", a.loop);
        c.set("playOnStart", a.playOnStart);
        if (!a.events.empty()) {
            json::Value& list = c.set("events", json::Value::array());
            for (const AnimationEvent& ev : a.events) {
                json::Value o = json::Value::object();
                o.set("clip", ev.clip);
                o.set("time", ev.time);
                o.set("name", ev.name);
                list.push(std::move(o));
            }
        }
        if (!a.states.empty()) {
            json::Value& list = c.set("states", json::Value::array());
            for (const AnimatorState& s : a.states) {
                json::Value o = json::Value::object();
                o.set("name", s.name);
                o.set("clip", s.clip);
                o.set("speed", s.speed);
                o.set("loop", s.loop);
                if (!s.blend.empty()) {
                    o.set("blendParameter", s.blendParameter);
                    if (!s.blendParameterY.empty()) o.set("blendParameterY", s.blendParameterY);
                    json::Value points = json::Value::array();
                    for (const BlendClip& b : s.blend) {
                        json::Value bp = json::Value::object();
                        bp.set("clip", b.clip);
                        bp.set("at", b.at);
                        if (!s.blendParameterY.empty()) bp.set("atY", b.atY);
                        points.push(std::move(bp));
                    }
                    o.set("blend", std::move(points));
                }
                list.push(std::move(o));
            }
            json::Value& tl = c.set("transitions", json::Value::array());
            for (const AnimatorTransition& tr : a.transitions) {
                json::Value o = json::Value::object();
                o.set("from", tr.from);
                o.set("to", tr.to);
                o.set("parameter", tr.parameter);
                o.set("when", animatorConditionName(tr.when));
                o.set("value", tr.value);
                o.set("fade", tr.fade);
                tl.push(std::move(o));
            }
            if (!a.startState.empty()) c.set("startState", a.startState);
        }
        if (!a.layers.empty()) {
            json::Value list = json::Value::array();
            for (const AnimatorLayer& l : a.layers) {
                json::Value o = json::Value::object();
                o.set("name", l.name);
                o.set("clip", l.clip);
                if (!l.mask.empty()) o.set("mask", l.mask);
                o.set("weight", l.weight);
                o.set("speed", l.speed);
                o.set("loop", l.loop);
                if (l.additive) o.set("additive", true);
                if (a.playing && l.playing) {
                    o.set("playing", true);
                    o.set("time", l.time);
                }
                list.push(std::move(o));
            }
            c.set("layers", std::move(list));
        }
        if (!a.ik.empty()) {
            json::Value list = json::Value::array();
            for (const AnimatorIK& k : a.ik) {
                json::Value o = json::Value::object();
                o.set("name", k.name);
                o.set("type", ikTypeName(k.type));
                o.set("bone", k.bone);
                if (!k.targetEntity.empty()) o.set("targetEntity", k.targetEntity);
                if (k.type == AnimatorIK::Type::TwoBone) o.set("pole", vec3(k.pole));
                else {
                    o.set("aimAxis", vec3(k.aimAxis));
                    o.set("maxAngle", k.maxAngle);
                }
                o.set("weight", k.weight);
                list.push(std::move(o));
            }
            c.set("ik", std::move(list));
        }
        if (a.rootMotion) {
            c.set("rootMotion", true);
            c.set("rootBone", a.rootBone);
        }
        if (a.playing) {   // runtime state: only save games have it
            c.set("playing", true);
            if (!a.fadeFromState.empty()) c.set("fadeFromState", a.fadeFromState);
            c.set("time", a.time);
            if (!a.state.empty()) c.set("state", a.state);
            if (!a.parameters.empty()) {
                json::Value& ps = c.set("parameters", json::Value::object());
                for (const auto& p : a.parameters) ps.set(p.first, p.second);
            }
            if (!a.fadeFrom.empty()) {
                c.set("fadeFrom", a.fadeFrom);
                c.set("fadeFromTime", a.fadeFromTime);
                c.set("fade", a.fade);
                c.set("fadeLength", a.fadeLength);
            }
        }
    }
    if (e.terrain && e.terrain->resolution >= 2) {
        const Terrain& t = *e.terrain;
        json::Value& c = v.set("terrain", json::Value::object());
        c.set("resolution", t.resolution);
        c.set("size", t.size);
        float lowest = 0.f, highest = 0.f;
        std::string heights = terrain::encodeHeights(t, lowest, highest);
        c.set("lowest", lowest);
        c.set("highest", highest);
        c.set("heights", heights);   // (16-bit steps from lowest to highest, compressed, base64)
        c.set("weights", terrain::encodeWeights(t));
        json::Value& layers = c.set("layers", json::Value::array());
        for (const Terrain::Layer& l : t.layers) {
            json::Value lv = json::Value::object();
            if (l.texture >= 0 && l.texture < textures.count()) lv.set("texture", textures.name(l.texture));
            lv.set("color", vec3(l.color));
            lv.set("tiling", l.tiling);
            layers.push(std::move(lv));
        }
        if (std::any_of(t.holes.begin(), t.holes.end(), [](uint8_t h) { return h != 0; }))
            c.set("holes", terrain::encodeBytes(t.holes));
        if (!t.foliage.empty()) {
            json::Value& fl = c.set("foliage", json::Value::array());
            for (const Terrain::Foliage& f : t.foliage) {
                json::Value fv = json::Value::object();
                fv.set("mesh", f.mesh);
                fv.set("color", vec3(f.color));
                fv.set("density", f.density);
                fv.set("minScale", f.minScale);
                fv.set("maxScale", f.maxScale);
                fv.set("wind", f.wind);
                fv.set("fadeDistance", f.fadeDistance);
                fl.push(std::move(fv));
            }
            if (t.foliageDensity.size() == t.heights.size() * Terrain::kFoliageLayers)
                c.set("foliageDensity", terrain::encodeBytes(t.foliageDensity));
        }
    }
    if (e.softBody) {
        const SoftBody& sb = *e.softBody;
        json::Value& c = v.set("softBody", json::Value::object());
        c.set("shape", sb.shape == SoftBody::Shape::Cloth ? "cloth" : "mesh");
        c.set("width", sb.width);
        c.set("height", sb.height);
        c.set("resolution", sb.resolution);
        c.set("pinned", sb.pinned);
        c.set("mass", sb.mass);
        c.set("stiffness", sb.stiffness);
        c.set("bendiness", sb.bendiness);
        c.set("pressure", sb.pressure);
        c.set("damping", sb.damping);
        c.set("friction", sb.friction);
    }
    if (e.fracture) {
        const Fracture& f = *e.fracture;
        json::Value& c = v.set("fracture", json::Value::object());
        c.set("pieces", f.pieces);
        c.set("seed", (double)f.seed);
        c.set("breakSpeed", f.breakSpeed);
        c.set("burst", f.burst);
        c.set("pieceLife", f.pieceLife);
    }
    if (e.brain) {
        const AIBrain& b = *e.brain;
        json::Value& c = v.set("brain", json::Value::object());
        json::Value& nodes = c.set("nodes", json::Value::array());
        for (const AIBrain::Node& n : b.nodes) {
            json::Value nv = json::Value::object();
            nv.set("type", n.type);
            if (!n.name.empty()) nv.set("name", n.name);
            if (!n.children.empty()) {
                json::Value& kids = nv.set("children", json::Value::array());
                for (int k : n.children) kids.push(k);
            }
            if (!n.key.empty()) nv.set("key", n.key);
            if (!n.text.empty()) nv.set("text", n.text);
            if (n.number != 0.f) nv.set("number", n.number);
            if (n.number2 != 0.f) nv.set("number2", n.number2);
            nodes.push(std::move(nv));
        }
        c.set("blackboard", b.blackboard.isObject() ? b.blackboard : json::Value::object());
        c.set("perceives", b.perceives);
        c.set("sightRange", b.sightRange);
        c.set("sightAngle", b.sightAngle);
        c.set("eyeHeight", b.eyeHeight);
        c.set("hearingRange", b.hearingRange);
        c.set("memory", b.memory);
    }
    if (e.vehicle) {
        const Vehicle& ve = *e.vehicle;
        json::Value& c = v.set("vehicle", json::Value::object());
        json::Value& wl = c.set("wheels", json::Value::array());
        for (const Vehicle::Wheel& w : ve.wheels) {
            json::Value o = json::Value::object();
            o.set("position", vec3(w.position));
            o.set("radius", w.radius);
            o.set("width", w.width);
            o.set("steering", w.steering);
            o.set("driven", w.driven);
            if (!w.entity.empty()) o.set("entity", w.entity);
            wl.push(std::move(o));
        }
        c.set("engineTorque", ve.engineTorque);
        c.set("maxRpm", ve.maxRpm);
        c.set("maxSteer", ve.maxSteer);
        c.set("suspensionLength", ve.suspensionLength);
        c.set("suspensionFrequency", ve.suspensionFrequency);
        c.set("suspensionDamping", ve.suspensionDamping);
    }
    if (e.replicated) {
        json::Value& c = v.set("replicated", json::Value::object());
        c.set("fields", e.replicated->fields);
    }
    if (e.ui) {
        const UIElement& u = *e.ui;
        json::Value& c = v.set("ui", json::Value::object());
        c.set("kind", uiKindName(u.kind));
        auto pair = [](const float p[2]) {
            json::Value a = json::Value::array();
            a.push(p[0]);
            a.push(p[1]);
            return a;
        };
        c.set("anchorMin", pair(u.anchorMin));
        c.set("anchorMax", pair(u.anchorMax));
        c.set("offsetMin", pair(u.offsetMin));
        c.set("offsetMax", pair(u.offsetMax));
        c.set("color", vec3(u.color));
        c.set("opacity", u.opacity);
        c.set("rounding", u.rounding);
        if (!u.text.empty()) c.set("text", u.text);
        c.set("textSize", u.textSize);
        c.set("textColor", vec3(u.textColor));
        c.set("align", u.align == 0 ? "left" : u.align == 2 ? "right" : "center");
        if (!u.font.empty()) c.set("font", u.font);
        if (!u.image.empty()) c.set("image", u.image);
        if (!u.action.empty()) c.set("action", u.action);
        if (!u.target.empty()) c.set("target", u.target);
        if (!u.value.empty()) c.set("value", u.value);
        if (u.kind == UIElement::Kind::Slider) {
            c.set("min", u.min);
            c.set("max", u.max);
        }
        if (u.kind == UIElement::Kind::List) c.set("spacing", u.spacing);
    }
    if (e.reflectionProbe) {
        json::Value& c = v.set("reflectionProbe", json::Value::object());
        c.set("size", vec3(e.reflectionProbe->size));
        c.set("resolution", e.reflectionProbe->resolution);
        c.set("boxProjection", e.reflectionProbe->boxProjection);
    }
    if (e.particleEmitter) {
        const ParticleEmitter& p = *e.particleEmitter;
        json::Value& c = v.set("particles", json::Value::object());
        c.set("rate", p.rate);
        c.set("burst", p.burst);
        c.set("playOnStart", p.playOnStart);
        c.set("duration", p.duration);
        c.set("lifetime", p.lifetime);
        c.set("lifetimeRandom", p.lifetimeRandom);
        c.set("speed", p.speed);
        c.set("speedRandom", p.speedRandom);
        c.set("spread", p.spread);
        c.set("radius", p.radius);
        c.set("gravity", p.gravity);
        c.set("drag", p.drag);
        c.set("startSize", p.startSize);
        c.set("endSize", p.endSize);
        c.set("startColor", vec3(p.startColor));
        c.set("endColor", vec3(p.endColor));
        c.set("startAlpha", p.startAlpha);
        c.set("endAlpha", p.endAlpha);
        c.set("intensity", p.intensity);
        c.set("additive", p.additive);
        c.set("maxParticles", p.maxParticles);
    }
    if (e.rigidBody) {
        const RigidBody& rb = *e.rigidBody;
        json::Value& c = v.set("rigidBody", json::Value::object());
        c.set("mass", rb.mass);
        c.set("useGravity", rb.useGravity);
        c.set("bounciness", rb.bounciness);
        if (rb.buoyancy != 1.2f) c.set("buoyancy", rb.buoyancy);
        c.set("friction", rb.friction);
        c.set("damping", rb.damping);
        if (length(rb.velocity) > 0.f) c.set("velocity", vec3(rb.velocity));
        if (length(rb.angularVelocity) > 0.f) c.set("angularVelocity", vec3(rb.angularVelocity));
        if (rb.lockRotation) c.set("lockRotation", true);
        if (rb.kinematic) c.set("kinematic", true);
    }
    if (e.navLink) {
        json::Value& c = v.set("navLink", json::Value::object());
        c.set("end", vec3(e.navLink->end));
        c.set("bothWays", e.navLink->bothWays);
        c.set("cost", e.navLink->cost);
    }
    if (e.joint) {
        const Joint& j = *e.joint;
        json::Value& c = v.set("joint", json::Value::object());
        c.set("type", jointTypeName(j.type));
        if (!j.connectedTo.empty()) c.set("connectedTo", j.connectedTo);
        c.set("anchor", vec3(j.anchor));
        c.set("axis", vec3(j.axis));
        c.set("minLimit", j.minLimit);
        c.set("maxLimit", j.maxLimit);
        c.set("coneAngle", j.coneAngle);
        if (j.motorStrength > 0.f) {
            c.set("motorSpeed", j.motorSpeed);
            c.set("motorStrength", j.motorStrength);
        }
        if (j.breakForce > 0.f) c.set("breakForce", j.breakForce);
        if (j.collideConnected) c.set("collideConnected", true);
        if (j.broken) c.set("broken", true);
    }
    if (e.light) {
        const Light& l = *e.light;
        json::Value& c = v.set("light", json::Value::object());
        c.set("type", lightTypeKey(l.type));
        c.set("color", vec3(l.color));
        c.set("intensity", l.intensity);
        if (l.type != Light::Type::Directional) c.set("range", l.range);
        if (l.type == Light::Type::Spot) {
            c.set("spotAngle", l.spotAngle);
            c.set("spotSoftness", l.spotSoftness);
            if (!l.cookie.empty()) {
                c.set("cookie", l.cookie);
                if (std::string id = assetdb::idOfStored(l.cookie); !id.empty()) c.set("cookieId", id);
            }
        }
        c.set("castShadows", l.castShadows);
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
        for (const BehaviourPtr& b : e.behaviours) list.push(behaviourToJson(*b));
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
    if (const json::Value& link = v["nestedPrefab"]; link.isObject())
        e.nestedPrefab = PrefabLink{(uint32_t)link["id"].asNumber(0), (EntityId)link["source"].asNumber(0)};
    e.visible = v["visible"].asBool(true);
    e.locked = v["locked"].asBool(false);
    for (const json::Value& t : v["tags"].elements())
        if (!t.asString().empty()) e.tags.push_back(t.asString());
    e.layer = (uint8_t)(v["layer"].asInt(0) & 31);

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
        auto texture = [&](const char* key, int& out) {
            const std::string& tex = c[key].asString();
            if (tex.empty()) return true;
            auto t = ids.textures.find(tex);
            if (t == ids.textures.end()) { error = "entity '" + e.name + "': unknown texture '" + tex + "'"; return false; }
            out = t->second;
            return true;
        };
        if (!texture("texture", mr.texture) || !texture("metalRoughTexture", mr.metalRoughTexture) ||
            !texture("normalTexture", mr.normalTexture) || !texture("emissiveTexture", mr.emissiveTexture))
            return false;
        mr.metallic = c["metallic"].asFloat(mr.metallic);
        mr.roughness = c["roughness"].asFloat(mr.roughness);
        mr.normalScale = c["normalScale"].asFloat(mr.normalScale);
        mr.emissive = readVec3(c["emissive"], mr.emissive);
        mr.emissiveStrength = c["emissiveStrength"].asFloat(mr.emissiveStrength);
        std::string alpha = c["alphaMode"].asString();
        mr.alphaMode = alpha == "mask" ? MeshRenderer::AlphaMode::Mask : alpha == "blend" ? MeshRenderer::AlphaMode::Blend
                                                                                           : MeshRenderer::AlphaMode::Opaque;
        mr.opacity = c["opacity"].asFloat(mr.opacity);
        mr.alphaCutoff = c["alphaCutoff"].asFloat(mr.alphaCutoff);
        e.meshRenderer = mr;
    }
    if (const json::Value& c = v["camera"]; c.isObject()) {
        Camera cam;
        cam.fovY = c["fov"].asFloat(cam.fovY / DEG2RAD) * DEG2RAD;
        cam.zNear = c["near"].asFloat(cam.zNear);
        cam.zFar = c["far"].asFloat(cam.zFar);
        e.camera = cam;
    }
    if (const json::Value& c = v["audioSource"]; c.isObject()) {
        AudioSource a;
        a.clip = assetdb::heal(c["clip"].asString(), c["clipId"].asString());
        a.volume = c["volume"].asFloat(a.volume);
        a.loop = c["loop"].asBool(a.loop);
        a.playOnStart = c["playOnStart"].asBool(a.playOnStart);
        a.playOnImpact = c["playOnImpact"].asBool(a.playOnImpact);
        a.spatial = c["spatial"].asBool(a.spatial);
        a.minDistance = c["minDistance"].asFloat(a.minDistance);
        a.maxDistance = c["maxDistance"].asFloat(a.maxDistance);
        if (!c["bus"].asString().empty()) a.bus = c["bus"].asString();
        a.stream = c["stream"].asBool(false);
        a.doppler = c["doppler"].asFloat(1.f);
        a.occlusion = c["occlusion"].asFloat(1.f);
        e.audioSource = a;
    }
    if (const json::Value& c = v["reverbZone"]; c.isObject()) {
        ReverbZone z;
        z.radius = c["radius"].asFloat(z.radius);
        z.blend = c["blend"].asFloat(z.blend);
        z.wet = c["wet"].asFloat(z.wet);
        z.roomSize = c["roomSize"].asFloat(z.roomSize);
        z.damping = c["damping"].asFloat(z.damping);
        e.reverbZone = z;
    }
    if (const json::Value& c = v["skin"]; c.isObject()) {
        Skin skin;
        for (const json::Value& j : c["joints"].elements()) skin.joints.push_back(j.asString());
        for (const json::Value& m : c["inverseBind"].elements()) {
            Mat4 ibm;
            for (size_t k = 0; k < 16 && k < m.elements().size(); ++k) ibm.m[k] = m.elements()[k].asFloat(ibm.m[k]);
            skin.inverseBind.push_back(ibm);
        }
        e.skin = std::move(skin);
    }
    if (const json::Value& c = v["animator"]; c.isObject()) {
        Animator a;
        auto clipName = [&](const std::string& n) {   // the file's name for it -> the library's
            auto it = ids.animations.find(n);
            return it != ids.animations.end() ? it->second : n;
        };
        a.clip = clipName(c["clip"].asString());
        a.speed = c["speed"].asFloat(a.speed);
        a.loop = c["loop"].asBool(a.loop);
        a.playOnStart = c["playOnStart"].asBool(a.playOnStart);
        a.playing = c["playing"].asBool(false);
        a.time = c["time"].asFloat(0.f);
        a.fadeFrom = c["fadeFrom"].asString().empty() ? "" : clipName(c["fadeFrom"].asString());
        a.fadeFromTime = c["fadeFromTime"].asFloat(0.f);
        a.fade = c["fade"].asFloat(0.f);
        a.fadeLength = c["fadeLength"].asFloat(0.f);
        for (const json::Value& o : c["events"].elements())
            a.events.push_back({clipName(o["clip"].asString()), o["time"].asFloat(0.f), o["name"].asString()});
        for (const json::Value& o : c["states"].elements()) {
            AnimatorState s;
            s.name = o["name"].asString();
            s.clip = clipName(o["clip"].asString());
            s.speed = o["speed"].asFloat(s.speed);
            s.loop = o["loop"].asBool(s.loop);
            s.blendParameter = o["blendParameter"].asString();
            s.blendParameterY = o["blendParameterY"].asString();
            for (const json::Value& bp : o["blend"].elements())
                s.blend.push_back({clipName(bp["clip"].asString()), bp["at"].asFloat(0.f), bp["atY"].asFloat(0.f)});
            a.states.push_back(std::move(s));
        }
        for (const json::Value& o : c["transitions"].elements()) {
            AnimatorTransition tr;
            if (!o["from"].asString().empty()) tr.from = o["from"].asString();
            tr.to = o["to"].asString();
            tr.parameter = o["parameter"].asString();
            tr.when = animatorConditionFromName(o["when"].asString());
            tr.value = o["value"].asFloat(tr.value);
            tr.fade = o["fade"].asFloat(tr.fade);
            a.transitions.push_back(std::move(tr));
        }
        a.startState = c["startState"].asString();
        for (const json::Value& o : c["layers"].elements()) {
            AnimatorLayer l;
            l.name = o["name"].asString();
            l.clip = clipName(o["clip"].asString());
            l.mask = o["mask"].asString();
            l.weight = o["weight"].asFloat(l.weight);
            l.speed = o["speed"].asFloat(l.speed);
            l.loop = o["loop"].asBool(l.loop);
            l.additive = o["additive"].asBool(false);
            l.playing = o["playing"].asBool(false);
            l.time = o["time"].asFloat(0.f);
            a.layers.push_back(std::move(l));
        }
        for (const json::Value& o : c["ik"].elements()) {
            AnimatorIK k;
            k.name = o["name"].asString();
            k.type = o["type"].asString() == "look-at" ? AnimatorIK::Type::LookAt : AnimatorIK::Type::TwoBone;
            k.bone = o["bone"].asString();
            k.targetEntity = o["targetEntity"].asString();
            k.pole = readVec3(o["pole"], k.pole);
            k.aimAxis = readVec3(o["aimAxis"], k.aimAxis);
            k.maxAngle = o["maxAngle"].asFloat(k.maxAngle);
            k.weight = o["weight"].asFloat(k.weight);
            a.ik.push_back(std::move(k));
        }
        a.rootMotion = c["rootMotion"].asBool(false);
        a.rootBone = c["rootBone"].asString();
        a.fadeFromState = c["fadeFromState"].asString();
        a.state = c["state"].asString();
        if (const json::Value& ps = c["parameters"]; ps.isObject())
            for (const auto& p : ps.members()) a.parameters.emplace_back(p.first, p.second.asFloat(0.f));
        e.animator = a;
    }
    if (const json::Value& c = v["terrain"]; c.isObject()) {
        Terrain t;
        t.resolution = c["resolution"].asInt(0);
        t.size = c["size"].asFloat(t.size);
        if (t.resolution < 2 || t.resolution > 4096 ||
            !terrain::decodeHeights(t, c["heights"].asString(), c["lowest"].asFloat(), c["highest"].asFloat()) ||
            !terrain::decodeWeights(t, c["weights"].asString())) {
            error = "entity '" + e.name + "': the terrain's data is damaged";
            return false;
        }
        size_t k = 0;
        for (const json::Value& lv : c["layers"].elements()) {
            if (k >= (size_t)Terrain::kLayers) break;
            Terrain::Layer& l = t.layers[k++];
            const std::string& tex = lv["texture"].asString();
            if (!tex.empty()) {
                auto it = ids.textures.find(tex);
                if (it == ids.textures.end()) { error = "entity '" + e.name + "': unknown texture '" + tex + "'"; return false; }
                l.texture = it->second;
            }
            l.color = readVec3(lv["color"], l.color);
            l.tiling = lv["tiling"].asFloat(l.tiling);
        }
        size_t points = (size_t)t.resolution * (size_t)t.resolution;
        if (c.has("holes") && !terrain::decodeBytes(c["holes"].asString(), points, t.holes)) {
            error = "entity '" + e.name + "': the terrain's holes are damaged";
            return false;
        }
        for (const json::Value& fv : c["foliage"].elements()) {
            if (t.foliage.size() >= (size_t)Terrain::kFoliageLayers) break;
            Terrain::Foliage f;
            f.mesh = fv["mesh"].asString().empty() ? f.mesh : fv["mesh"].asString();
            f.color = readVec3(fv["color"], f.color);
            f.density = fv["density"].asFloat(f.density);
            f.minScale = fv["minScale"].asFloat(f.minScale);
            f.maxScale = fv["maxScale"].asFloat(f.maxScale);
            f.wind = fv["wind"].asFloat(f.wind);
            f.fadeDistance = fv["fadeDistance"].asFloat(f.fadeDistance);
            t.foliage.push_back(f);
        }
        if (c.has("foliageDensity") &&
            !terrain::decodeBytes(c["foliageDensity"].asString(), points * Terrain::kFoliageLayers, t.foliageDensity)) {
            error = "entity '" + e.name + "': the terrain's foliage is damaged";
            return false;
        }
        terrain::touch(t);
        e.terrain = std::move(t);
    }
    if (const json::Value& c = v["softBody"]; c.isObject()) {
        SoftBody sb;
        sb.shape = c["shape"].asString() == "mesh" ? SoftBody::Shape::Mesh : SoftBody::Shape::Cloth;
        sb.width = c["width"].asFloat(sb.width);
        sb.height = c["height"].asFloat(sb.height);
        sb.resolution = c["resolution"].asInt(sb.resolution);
        sb.pinned = c.has("pinned") ? c["pinned"].asString() : sb.pinned;
        sb.mass = c["mass"].asFloat(sb.mass);
        sb.stiffness = c["stiffness"].asFloat(sb.stiffness);
        sb.bendiness = c["bendiness"].asFloat(sb.bendiness);
        sb.pressure = c["pressure"].asFloat(sb.pressure);
        sb.damping = c["damping"].asFloat(sb.damping);
        sb.friction = c["friction"].asFloat(sb.friction);
        e.softBody = sb;
    }
    if (const json::Value& c = v["fracture"]; c.isObject()) {
        Fracture f;
        f.pieces = c["pieces"].asInt(f.pieces);
        f.seed = (uint32_t)c["seed"].asNumber(f.seed);
        f.breakSpeed = c["breakSpeed"].asFloat(f.breakSpeed);
        f.burst = c["burst"].asFloat(f.burst);
        f.pieceLife = c["pieceLife"].asFloat(f.pieceLife);
        e.fracture = f;
    }
    if (const json::Value& c = v["brain"]; c.isObject()) {
        AIBrain b;
        for (const json::Value& nv : c["nodes"].elements()) {
            AIBrain::Node n;
            n.type = nv["type"].asString();
            n.name = nv["name"].asString();
            for (const json::Value& k : nv["children"].elements()) n.children.push_back(k.asInt(-1));
            n.key = nv["key"].asString();
            n.text = nv["text"].asString();
            n.number = nv["number"].asFloat(0.f);
            n.number2 = nv["number2"].asFloat(0.f);
            b.nodes.push_back(n);
        }
        for (const AIBrain::Node& n : b.nodes)   // (a damaged file mustn't make a tree that points nowhere)
            for (int k : n.children)
                if (k <= 0 || k >= (int)b.nodes.size()) {
                    error = "entity '" + e.name + "': its AI brain's tree is damaged";
                    return false;
                }
        if (c["blackboard"].isObject()) b.blackboard = c["blackboard"];
        b.perceives = c["perceives"].asString();
        b.sightRange = c["sightRange"].asFloat(b.sightRange);
        b.sightAngle = c["sightAngle"].asFloat(b.sightAngle);
        b.eyeHeight = c["eyeHeight"].asFloat(b.eyeHeight);
        b.hearingRange = c["hearingRange"].asFloat(b.hearingRange);
        b.memory = c["memory"].asFloat(b.memory);
        e.brain = std::move(b);
    }
    if (const json::Value& c = v["vehicle"]; c.isObject()) {
        Vehicle ve;
        for (const json::Value& o : c["wheels"].elements()) {
            Vehicle::Wheel w;
            w.position = readVec3(o["position"], w.position);
            w.radius = o["radius"].asFloat(w.radius);
            w.width = o["width"].asFloat(w.width);
            w.steering = o["steering"].asBool(w.steering);
            w.driven = o["driven"].asBool(w.driven);
            w.entity = o["entity"].asString();
            ve.wheels.push_back(w);
        }
        ve.engineTorque = c["engineTorque"].asFloat(ve.engineTorque);
        ve.maxRpm = c["maxRpm"].asFloat(ve.maxRpm);
        ve.maxSteer = c["maxSteer"].asFloat(ve.maxSteer);
        ve.suspensionLength = c["suspensionLength"].asFloat(ve.suspensionLength);
        ve.suspensionFrequency = c["suspensionFrequency"].asFloat(ve.suspensionFrequency);
        ve.suspensionDamping = c["suspensionDamping"].asFloat(ve.suspensionDamping);
        e.vehicle = ve;
    }
    if (const json::Value& c = v["replicated"]; c.isObject()) {
        Replicated r;
        r.fields = c["fields"].asBool(true);
        e.replicated = r;
    }
    if (const json::Value& c = v["ui"]; c.isObject()) {
        UIElement u;
        u.kind = uiKindFromName(c["kind"].asString());
        auto pair = [](const json::Value& a, float out[2]) {
            if (a.isArray() && a.size() == 2) out[0] = a[0].asFloat(out[0]), out[1] = a[1].asFloat(out[1]);
        };
        pair(c["anchorMin"], u.anchorMin);
        pair(c["anchorMax"], u.anchorMax);
        pair(c["offsetMin"], u.offsetMin);
        pair(c["offsetMax"], u.offsetMax);
        u.color = readVec3(c["color"], u.color);
        u.opacity = c["opacity"].asFloat(u.opacity);
        u.rounding = c["rounding"].asFloat(u.rounding);
        u.text = c["text"].asString();
        u.textSize = c["textSize"].asFloat(u.textSize);
        u.textColor = readVec3(c["textColor"], u.textColor);
        u.align = c["align"].asString() == "left" ? 0 : c["align"].asString() == "right" ? 2 : 1;
        u.font = c["font"].asString();
        u.image = c["image"].asString();
        u.action = c["action"].asString();
        u.target = c["target"].asString();
        u.value = c["value"].asString();
        u.min = c["min"].asFloat(u.min);
        u.max = c["max"].asFloat(u.max);
        u.spacing = c["spacing"].asFloat(u.spacing);
        e.ui = u;
    }
    if (const json::Value& c = v["reflectionProbe"]; c.isObject()) {
        ReflectionProbe p;
        p.size = readVec3(c["size"], p.size);
        p.resolution = std::clamp(c["resolution"].asInt(p.resolution), 16, 1024);
        p.boxProjection = c["boxProjection"].asBool(p.boxProjection);
        e.reflectionProbe = p;
    }
    if (const json::Value& c = v["particles"]; c.isObject()) {
        ParticleEmitter p;
        p.rate = c["rate"].asFloat(p.rate);
        p.burst = c["burst"].asInt(p.burst);
        p.playOnStart = c["playOnStart"].asBool(p.playOnStart);
        p.duration = c["duration"].asFloat(p.duration);
        p.lifetime = c["lifetime"].asFloat(p.lifetime);
        p.lifetimeRandom = c["lifetimeRandom"].asFloat(p.lifetimeRandom);
        p.speed = c["speed"].asFloat(p.speed);
        p.speedRandom = c["speedRandom"].asFloat(p.speedRandom);
        p.spread = c["spread"].asFloat(p.spread);
        p.radius = c["radius"].asFloat(p.radius);
        p.gravity = c["gravity"].asFloat(p.gravity);
        p.drag = c["drag"].asFloat(p.drag);
        p.startSize = c["startSize"].asFloat(p.startSize);
        p.endSize = c["endSize"].asFloat(p.endSize);
        p.startColor = readVec3(c["startColor"], p.startColor);
        p.endColor = readVec3(c["endColor"], p.endColor);
        p.startAlpha = c["startAlpha"].asFloat(p.startAlpha);
        p.endAlpha = c["endAlpha"].asFloat(p.endAlpha);
        p.intensity = c["intensity"].asFloat(p.intensity);
        p.additive = c["additive"].asBool(p.additive);
        p.maxParticles = std::max(1, c["maxParticles"].asInt(p.maxParticles));
        e.particleEmitter = p;
    }
    if (const json::Value& c = v["rigidBody"]; c.isObject()) {
        RigidBody rb;
        rb.mass = std::max(c["mass"].asFloat(rb.mass), 0.001f);
        rb.useGravity = c["useGravity"].asBool(true);
        rb.bounciness = c["bounciness"].asFloat(rb.bounciness);
        rb.buoyancy = c["buoyancy"].asFloat(rb.buoyancy);
        rb.friction = c["friction"].asFloat(rb.friction);
        rb.damping = c["damping"].asFloat(rb.damping);
        rb.velocity = readVec3(c["velocity"], {});
        rb.angularVelocity = readVec3(c["angularVelocity"], {});
        rb.lockRotation = c["lockRotation"].asBool(false);
        rb.kinematic = c["kinematic"].asBool(false);
        e.rigidBody = rb;
    }
    if (const json::Value& c = v["navLink"]; c.isObject()) {
        NavLink l;
        l.end = readVec3(c["end"], l.end);
        l.bothWays = c["bothWays"].asBool(true);
        l.cost = c["cost"].asFloat(1.f);
        e.navLink = l;
    }
    if (const json::Value& c = v["joint"]; c.isObject()) {
        Joint j;
        j.type = jointTypeFromName(c["type"].asString());
        j.connectedTo = c["connectedTo"].asString();
        j.anchor = readVec3(c["anchor"], j.anchor);
        j.axis = readVec3(c["axis"], j.axis);
        j.minLimit = c["minLimit"].asFloat(j.minLimit);
        j.maxLimit = c["maxLimit"].asFloat(j.maxLimit);
        j.coneAngle = c["coneAngle"].asFloat(j.coneAngle);
        j.motorSpeed = c["motorSpeed"].asFloat(0.f);
        j.motorStrength = c["motorStrength"].asFloat(0.f);
        j.breakForce = c["breakForce"].asFloat(0.f);
        j.collideConnected = c["collideConnected"].asBool(false);
        j.broken = c["broken"].asBool(false);
        e.joint = j;
    }
    if (const json::Value& c = v["light"]; c.isObject()) {
        Light l;
        std::string type = c["type"].asString();
        l.type = type == "directional" ? Light::Type::Directional : type == "spot" ? Light::Type::Spot : Light::Type::Point;
        l.spotAngle = c["spotAngle"].asFloat(l.spotAngle);
        l.spotSoftness = c["spotSoftness"].asFloat(l.spotSoftness);
        l.color = readVec3(c["color"], l.color);
        l.intensity = c["intensity"].asFloat(l.intensity);
        l.range = c["range"].asFloat(l.range);
        l.castShadows = c["castShadows"].asBool(true);
        l.cookie = assetdb::heal(c["cookie"].asString(), c["cookieId"].asString());
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
        if (bv["type"].asString().empty()) { error = "entity '" + e.name + "': behaviour without a type"; return false; }
        e.behaviours.emplace_back(behaviourFromJson(bv));
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
    std::set<std::string> usedClips;
    for (const auto* list : lists)
        for (const Entity& e : *list) {
            if (e.animator) {
                if (!e.animator->clip.empty()) usedClips.insert(e.animator->clip);
                if (!e.animator->fadeFrom.empty()) usedClips.insert(e.animator->fadeFrom);
                for (const AnimatorState& s : e.animator->states) {
                    if (!s.clip.empty()) usedClips.insert(s.clip);
                    for (const BlendClip& b : s.blend)
                        if (!b.clip.empty()) usedClips.insert(b.clip);
                }
                for (const AnimatorLayer& l : e.animator->layers)
                    if (!l.clip.empty()) usedClips.insert(l.clip);
            }
            if (e.terrain)
                for (const Terrain::Layer& l : e.terrain->layers)
                    if (l.texture >= kBuiltinTextureCount) usedTextures.insert(l.texture);
            if (!e.meshRenderer) continue;
            if (e.meshRenderer->mesh >= (int)MeshKind::Count) usedMeshes.insert(e.meshRenderer->mesh);
            for (int t : {e.meshRenderer->texture, e.meshRenderer->metalRoughTexture, e.meshRenderer->normalTexture,
                          e.meshRenderer->emissiveTexture})
                if (t >= kBuiltinTextureCount) usedTextures.insert(t);
        }
    json::Value& m = doc.set("meshes", json::Value::array());
    for (int id : usedMeshes) {
        json::Value v = json::Value::object();
        v.set("name", meshes.name(id));
        v.set("source", project::toStored(meshes.get(id).source));
        if (std::string aid = assetdb::idOfStored(meshes.get(id).source); !aid.empty()) v.set("id", aid);
        m.push(std::move(v));
    }
    json::Value& t = doc.set("textures", json::Value::array());
    for (int id : usedTextures) {
        json::Value v = json::Value::object();
        v.set("name", textures.name(id));
        v.set("source", project::toStored(textures.get(id).source));
        if (std::string aid = assetdb::idOfStored(textures.get(id).source); !aid.empty()) v.set("id", aid);
        t.push(std::move(v));
    }
    json::Value& a = doc.set("animations", json::Value::array());
    for (const std::string& name : usedClips) {
        const AnimationClip* clip = meshes.findAnimation(name);
        if (!clip || clip->source.empty()) continue;
        json::Value v = json::Value::object();
        v.set("name", name);
        v.set("source", project::toStored(clip->source));
        if (std::string aid = assetdb::idOfStored(clip->source); !aid.empty()) v.set("id", aid);
        a.push(std::move(v));
    }
}

thread_local std::atomic<float>* t_progress = nullptr;
void reportProgress(float f) {
    if (t_progress) t_progress->store(f);
}

// Imports the listed meshes and textures; fills the file's names -> library ids
bool assetListsFromJson(const json::Value& doc, MeshLibrary& meshes, TextureLibrary& textures, AssetIds& ids,
                        std::string& error) {
    ids = builtinAssetIds(meshes, textures);
    // (assets are most of the work: they take progress up to 80 %)
    size_t total = doc["meshes"].elements().size() + doc["textures"].elements().size() + doc["animations"].elements().size(), done = 0;
    auto step = [&] { reportProgress(total ? 0.8f * (float)++done / (float)total : 0.8f); };
    for (const json::Value& m : doc["meshes"].elements()) {
        int id = importMesh(meshes, project::resolve(assetdb::heal(m["source"].asString(), m["id"].asString())), error);
        if (id < 0) return false;
        ids.meshes[m["name"].asString()] = id;
        step();
    }
    for (const json::Value& t : doc["textures"].elements()) {
        int id = importTexture(textures, project::resolve(assetdb::heal(t["source"].asString(), t["id"].asString())), error);
        if (id < 0) return false;
        ids.textures[t["name"].asString()] = id;
        step();
    }
    for (const json::Value& a : doc["animations"].elements()) {
        int id = importAnimationClip(meshes, project::resolve(assetdb::heal(a["source"].asString(), a["id"].asString())), error);
        if (id < 0) return false;
        ids.animations[a["name"].asString()] = meshes.animations[(size_t)id].name;
        step();
    }
    reportProgress(0.8f);
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
    return writeJson(path, sceneToJson(scene, meshes, textures), error);
}

json::Value sceneToJson(const Scene& scene, const MeshLibrary& meshes, const TextureLibrary& textures) {
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
    {
        const RenderSettings& r = scene.render;
        json::Value& rv = settings.set("render", json::Value::object());
        rv.set("sky", vec3(r.sky));
        rv.set("zenith", vec3(r.zenith));
        rv.set("ground", vec3(r.ground));
        rv.set("fogDensity", r.fogDensity);
        rv.set("ambient", r.ambient);
        rv.set("exposure", r.exposure);
        rv.set("bloomStrength", r.bloomStrength);
        rv.set("bloomThreshold", r.bloomThreshold);
        rv.set("aoStrength", r.aoStrength);
        rv.set("aoRadius", r.aoRadius);
        if (!r.skyImage.empty()) {
            rv.set("skyImage", r.skyImage);
            if (std::string id = assetdb::idOfStored(r.skyImage); !id.empty()) rv.set("skyImageId", id);
            rv.set("skyIntensity", r.skyIntensity);
            rv.set("skyRotation", r.skyRotation);
        }
    }
    {
        const NavigationSettings& n = scene.navigation;
        json::Value& nv = settings.set("navigation", json::Value::object());
        nv.set("cellSize", n.cellSize);
        nv.set("agentRadius", n.agentRadius);
        nv.set("agentHeight", n.agentHeight);
        nv.set("maxClimb", n.maxClimb);
        nv.set("maxSlope", n.maxSlope);
    }
    if (!scene.network.playerPrefab.empty() || scene.network.relevance > 0.f) {
        json::Value& nw = settings.set("network", json::Value::object());
        nw.set("playerPrefab", scene.network.playerPrefab);
        nw.set("spawnPoint", vec3(scene.network.spawnPoint));
        nw.set("spawnSpread", scene.network.spawnSpread);
        if (scene.network.relevance > 0.f) nw.set("relevance", scene.network.relevance);
    }
    {
        const LayerSettings& l = scene.layers;
        json::Value& lv = settings.set("layers", json::Value::object());
        json::Value names = json::Value::array(), collides = json::Value::array();
        for (int i = 0; i < LayerSettings::kCount; ++i) {
            names.push(l.names[i]);
            collides.push((double)l.collides[i]);
        }
        lv.set("names", std::move(names));   // (set() may move the members: no references held across it)
        lv.set("collides", std::move(collides));
    }
    json::Value& input = settings.set("input", json::Value::array());
    for (const InputAction& a : scene.inputActions) {
        json::Value av = json::Value::object();
        av.set("name", a.name);
        json::Value& keys = av.set("keys", json::Value::array());
        for (ImGuiKey k : a.keys) keys.push(keyName(k));
        if (a.mouseButton >= 0) av.set("mouseButton", a.mouseButton);
        input.push(std::move(av));
    }
    return doc;
}

void setLoadProgress(std::atomic<float>* progress) { t_progress = progress; }

bool loadScene(const std::string& path, Scene& scene, MeshLibrary& meshes, TextureLibrary& textures, std::string& error) {
    std::string text;
    if (!readFile(path, text)) { error = "cannot open " + path; return false; }
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || text[first] != '{') return loadLegacy(path, text, scene, meshes, error);

    json::Value doc;
    if (!readJson(path, kFormat, kVersion, doc, error)) return false;
    if (!sceneFromJson(doc, scene, meshes, textures, error)) { error = path + ": " + error; return false; }
    return true;
}

bool sceneFromJson(const json::Value& doc, Scene& scene, MeshLibrary& meshes, TextureLibrary& textures, std::string& error) {
    if (doc["format"].asString() != kFormat) { error = "not a vkworld scene"; return false; }
    if (doc["version"].asInt(0) > kVersion) { error = "made by a newer version (format " + std::to_string(doc["version"].asInt(0)) + ")"; return false; }
    AssetIds ids;
    if (!assetListsFromJson(doc, meshes, textures, ids, error)) return false;

    Scene result;
    for (const json::Value& v : doc["entities"].elements()) {
        Entity e;
        if (!entityFromJson(v, ids, e, error)) { return false; }
        EntityId fileId = e.id;
        if (result.add(std::move(e)).id != fileId && fileId != kNoEntity) {   // id-less entities just get a new one
            error = "duplicate entity id " + std::to_string(fileId);
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
        if (!prefabFromJson(v, ids, p, error)) { return false; }
        p.name = result.uniquePrefabName(p.name);
        result.addPrefab(std::move(p));
    }
    if (const json::Value& rv = doc["settings"]["render"]; rv.isObject()) {
        RenderSettings& r = result.render;
        r.sky = readVec3(rv["sky"], r.sky);
        r.zenith = readVec3(rv["zenith"], r.zenith);
        r.ground = readVec3(rv["ground"], r.ground);
        r.fogDensity = rv["fogDensity"].asFloat(r.fogDensity);
        r.ambient = rv["ambient"].asFloat(r.ambient);
        r.exposure = rv["exposure"].asFloat(r.exposure);
        r.bloomStrength = rv["bloomStrength"].asFloat(r.bloomStrength);
        r.bloomThreshold = rv["bloomThreshold"].asFloat(r.bloomThreshold);
        r.aoStrength = rv["aoStrength"].asFloat(r.aoStrength);
        r.aoRadius = rv["aoRadius"].asFloat(r.aoRadius);
        r.skyImage = assetdb::heal(rv["skyImage"].asString(), rv["skyImageId"].asString());
        r.skyIntensity = rv["skyIntensity"].asFloat(r.skyIntensity);
        r.skyRotation = rv["skyRotation"].asFloat(r.skyRotation);
    }
    if (const json::Value& lv = doc["settings"]["layers"]; lv.isObject()) {
        LayerSettings& l = result.layers;
        const auto& names = lv["names"].elements();
        const auto& collides = lv["collides"].elements();
        for (int i = 0; i < LayerSettings::kCount; ++i) {
            if ((size_t)i < names.size()) l.names[i] = names[(size_t)i].asString();
            if ((size_t)i < collides.size()) l.collides[i] = (uint32_t)collides[(size_t)i].asNumber(4294967295.0);
        }
    }
    if (const json::Value& nv = doc["settings"]["navigation"]; nv.isObject()) {
        NavigationSettings& n = result.navigation;
        n.cellSize = nv["cellSize"].asFloat(n.cellSize);
        n.agentRadius = nv["agentRadius"].asFloat(n.agentRadius);
        n.agentHeight = nv["agentHeight"].asFloat(n.agentHeight);
        n.maxClimb = nv["maxClimb"].asFloat(n.maxClimb);
        n.maxSlope = nv["maxSlope"].asFloat(n.maxSlope);
    }
    if (const json::Value& nw = doc["settings"]["network"]; nw.isObject()) {
        result.network.playerPrefab = nw["playerPrefab"].asString();
        result.network.spawnPoint = readVec3(nw["spawnPoint"], result.network.spawnPoint);
        result.network.spawnSpread = nw["spawnSpread"].asFloat(result.network.spawnSpread);
        result.network.relevance = nw["relevance"].asFloat(0.f);
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
        // Actions the engine added since the file was saved get their default bindings
        static const std::pair<int, const char*> added[] = {{9, "quicksave"}, {9, "quickload"}, {10, "pause"}};
        for (const InputAction& d : defaultInputActions())
            for (const auto& [version, name] : added)
                if (d.name == name && doc["version"].asInt(0) < version &&
                    std::none_of(result.inputActions.begin(), result.inputActions.end(),
                                 [&](const InputAction& a) { return a.name == d.name; }))
                    result.inputActions.push_back(d);
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
