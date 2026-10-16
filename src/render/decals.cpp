// The decals drawn and their texture slots (decals.h)
#include "render/decals.h"

#include <algorithm>
#include <numeric>

namespace {
int slotOf(std::vector<int>& slots, int texture, size_t max) {   // (its slot, a new one while there are any left)
    if (texture < 0) return -1;
    auto it = std::find(slots.begin(), slots.end(), texture);
    if (it != slots.end()) return (int)(it - slots.begin());
    if (slots.size() >= max) return -1;
    slots.push_back(texture);
    return (int)slots.size() - 1;
}
}  // namespace

DecalPick pickDecals(const std::vector<DecalInfo>& decals, Vec3 camPos, size_t maxDecals, size_t maxTextures,
                     size_t maxNormalTextures) {
    DecalPick pick;
    std::vector<int> order(decals.size());
    std::iota(order.begin(), order.end(), 0);
    auto reach = [&](int i) { return std::max(length(decals[(size_t)i].center - camPos) - decals[(size_t)i].radius, 0.f); };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return reach(a) < reach(b); });
    if (order.size() > maxDecals) order.resize(maxDecals);
    for (int i : order) {
        const DecalInfo& d = decals[(size_t)i];   // (the nearer decals' textures get the slots first)
        pick.decals.push_back(i);
        pick.slots.push_back(slotOf(pick.textures, d.texture, maxTextures));
        pick.normalSlots.push_back(slotOf(pick.normalTextures, d.normalTexture, maxNormalTextures));
    }
    return pick;
}
