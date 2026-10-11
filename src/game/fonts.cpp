#include "game/fonts.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "core/project.h"
#include "core/vfs.h"
#include "imgui.h"

extern const unsigned char kDefaultGameFont[];
extern const std::size_t kDefaultGameFontSize;

namespace gamefonts {
namespace {

struct State {
    ImFontAtlas* atlas = nullptr;   // the fonts below belong to this one (ImGui contexts come and go in tests)
    ImFont* fallback = nullptr;
    std::map<std::string, ImFont*> loaded;
    std::set<std::string> pending, failed;
};

State& state() {
    static State s;
    return s;
}

// Fonts are made for the current ImGui context's atlas; a new context starts over
State& current() {
    State& s = state();
    ImFontAtlas* atlas = ImGui::GetCurrentContext() ? ImGui::GetIO().Fonts : nullptr;
    if (s.atlas != atlas) s = State{atlas, nullptr, {}, {}, {}};
    return s;
}

ImFont* addFromMemory(const void* data, size_t size, const std::string& name) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;   // (a copy, freed with the atlas)
    std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", name.c_str());
    void* copy = IM_ALLOC(size);
    std::memcpy(copy, data, size);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(copy, (int)size, 0.f, &cfg);
}

}  // namespace

ImFont* font(const std::string& file) {
    State& s = current();
    if (!s.atlas) return nullptr;
    if (file.empty()) {
        if (!s.fallback) s.pending.insert("");   // (made before the next frame)
        return s.fallback ? s.fallback : ImGui::GetFont();
    }
    auto it = s.loaded.find(file);
    if (it != s.loaded.end()) return it->second;
    if (!s.failed.count(file)) s.pending.insert(file);
    return s.fallback ? s.fallback : ImGui::GetFont();
}

void loadPending() {
    State& s = current();
    if (!s.atlas || s.pending.empty()) return;
    std::set<std::string> todo;
    todo.swap(s.pending);
    for (const std::string& file : todo) {
        if (file.empty()) {
            if (!s.fallback) s.fallback = addFromMemory(kDefaultGameFont, kDefaultGameFontSize, "Roboto (game)");
            continue;
        }
        std::string bytes;
        ImFont* f = nullptr;
        if (vfs::read(std::filesystem::u8path(project::resolve(file)), bytes) && !bytes.empty())
            f = addFromMemory(bytes.data(), bytes.size(), file);
        if (f) s.loaded[file] = f;
        else {
            s.failed.insert(file);
            std::fprintf(stderr, "font %s: cannot load\n", file.c_str());
        }
    }
}

}  // namespace gamefonts
