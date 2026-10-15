// Differences between two states of a scene, for undo: what an edit changed, kept small (the entities it touched,
// not the whole scene).
#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "scene/scene.h"

// Every field, components and behaviours (their fields and state) included; floats compared exactly
bool sameEntity(const Entity& a, const Entity& b);
bool sameInputActions(const std::vector<InputAction>& a, const std::vector<InputAction>& b);
bool sameRenderSettings(const RenderSettings& a, const RenderSettings& b);
bool sameNavigationSettings(const NavigationSettings& a, const NavigationSettings& b);
bool sameLayerSettings(const LayerSettings& a, const LayerSettings& b);

// How to turn one state of a scene back into another: apply it to the later state to get the earlier one
struct SceneDelta {
    std::vector<std::pair<size_t, Entity>> entities;   // changed or removed entities as they were, with their index
    std::vector<EntityId> added;                       // entities that didn't exist (removed when applied)
    std::optional<std::vector<EntityId>> order;        // the entity order, when it changed
    std::optional<std::vector<Prefab>> prefabs;        // when any prefab changed
    std::optional<std::vector<InputAction>> inputActions;
    std::optional<RenderSettings> render;
    std::optional<NavigationSettings> navigation;
    std::optional<LayerSettings> layers;
    std::optional<AudioSettings> audio;

    bool empty() const {
        return entities.empty() && added.empty() && !order && !prefabs && !inputActions && !render && !navigation &&
               !layers && !audio;
    }
    size_t entityCount() const { return entities.size() + added.size(); }
};

// The delta that turns `after` back into `before`
SceneDelta diffScenes(const Scene& before, const Scene& after);
void applyDelta(Scene& scene, const SceneDelta& delta);
