// The decals drawn and their texture slots (decals.h)
#include "render/decals.h"

#include <algorithm>
#include <numeric>

DecalPick pickDecals(const std::vector<DecalInfo>& decals, Vec3 camPos, size_t maxDecals, size_t maxTextures) {
    DecalPick pick;
    std::vector<int> order(decals.size());
    std::iota(order.begin(), order.end(), 0);
    auto reach = [&](int i) { return std::max(length(decals[(size_t)i].center - camPos) - decals[(size_t)i].radius, 0.f); };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return reach(a) < reach(b); });
    if (order.size() > maxDecals) order.resize(maxDecals);
    for (int i : order) {
        const DecalInfo& d = decals[(size_t)i];
        int slot = -1;
        if (d.texture >= 0) {
            auto it = std::find(pick.textures.begin(), pick.textures.end(), d.texture);
            if (it != pick.textures.end()) slot = (int)(it - pick.textures.begin());
            else if (pick.textures.size() < maxTextures) {   // (the nearer decals' textures first)
                slot = (int)pick.textures.size();
                pick.textures.push_back(d.texture);
            }
        }
        pick.decals.push_back(i);
        pick.slots.push_back(slot);
    }
    return pick;
}
