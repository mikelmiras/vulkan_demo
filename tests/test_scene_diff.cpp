// scene/scene_diff: undo steps as diffs. Applying diffScenes(before, after) to `after` must give back `before`.
#include "test.h"

#include "scene/scene_diff.h"

namespace {

bool sameScene(const Scene& a, const Scene& b) {
    if (a.entities.size() != b.entities.size() || a.prefabs.size() != b.prefabs.size()) return false;
    for (size_t i = 0; i < a.entities.size(); ++i)
        if (!sameEntity(a.entities[i], b.entities[i])) return false;
    for (size_t i = 0; i < a.prefabs.size(); ++i)
        if (a.prefabs[i].name != b.prefabs[i].name || a.prefabs[i].entities.size() != b.prefabs[i].entities.size()) return false;
    return sameInputActions(a.inputActions, b.inputActions) && sameRenderSettings(a.render, b.render);
}

// Undo, then redo, the way the editor does
void checkRoundTrip(const Scene& before, const Scene& after) {
    SceneDelta undo = diffScenes(before, after);
    Scene s = after;
    applyDelta(s, undo);
    CHECK(sameScene(s, before));
    SceneDelta redo = diffScenes(after, s);
    applyDelta(s, redo);
    CHECK(sameScene(s, after));
}

}  // namespace

TEST(scene_diff_kinds_of_change) {
    Scene before = buildDefaultScene();
    {   // one entity moved: the delta holds just it
        Scene after = before;
        after.findByName("Gold")->transform.position[0] += 1.f;
        SceneDelta d = diffScenes(before, after);
        CHECK(d.entities.size() == 1 && d.added.empty() && !d.order && !d.prefabs);
        checkRoundTrip(before, after);
    }
    {   // a subtree deleted, then one duplicated
        Scene after = before;
        after.remove(after.findByName("Centrepiece")->id);
        after.duplicate(after.findByName("Materials")->id);
        checkRoundTrip(before, after);
    }
    {   // reparented (order and parent change), a behaviour field edited, a component added
        Scene after = before;
        after.setParent(after.findByName("Steel")->id, kNoEntity);
        Entity* spinner = nullptr;
        for (Entity& e : after.entities)
            if (!spinner && !e.behaviours.empty() && std::string(e.behaviours[0]->typeName()) == "Spin") spinner = &e;
        REQUIRE(spinner);
        REQUIRE(setField(*spinner->behaviours[0], "speed", 123.f));
        after.findByName("Gold")->rigidBody = RigidBody{};
        SceneDelta d = diffScenes(before, after);
        CHECK(d.entities.size() == 3);
        checkRoundTrip(before, after);
    }
    {   // scene-level: prefabs, the look, the bindings
        Scene after = before;
        after.prefabs.erase(after.prefabs.begin());
        after.render.exposure = 2.f;
        after.inputActions.pop_back();
        SceneDelta d = diffScenes(before, after);
        CHECK(d.prefabs && d.render && d.inputActions && d.entities.empty());
        checkRoundTrip(before, after);
    }
    CHECK(diffScenes(before, before).empty());
}

// Random edits (moves, renames, deletions, additions, reparenting, duplicates) against full copies
TEST(scene_diff_random_edits) {
    uint32_t seed = 7;
    auto rnd = [&](uint32_t n) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return seed % n;
    };
    Scene s = buildDefaultScene();
    for (int round = 0; round < 60; ++round) {
        Scene before = s;
        int edits = 1 + (int)rnd(4);
        for (int k = 0; k < edits && !s.entities.empty(); ++k) {
            Entity& e = s.entities[rnd((uint32_t)s.entities.size())];
            switch (rnd(6)) {
                case 0: e.transform.position[1] += 0.5f; break;
                case 1: e.name += "*"; break;
                case 2: s.remove(e.id); break;
                case 3: s.create("New " + std::to_string(round), rnd(2) ? e.id : kNoEntity); break;
                case 4: s.setParent(e.id, s.entities[rnd((uint32_t)s.entities.size())].id); break;
                case 5: s.duplicate(e.id); break;
            }
        }
        checkRoundTrip(before, s);
    }
}
