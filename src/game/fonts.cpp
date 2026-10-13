#include "game/fonts.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "core/project.h"
#include "core/vfs.h"
#include "game/glyph_font.h"
#include "imgui.h"
#include "imgui_internal.h"

extern const unsigned char kDefaultGameFont[];
extern const std::size_t kDefaultGameFontSize;

namespace gamefonts {
namespace {

struct Fallback {
    std::filesystem::path file;
    int index = 0;
};

struct State {
    ImFontAtlas* atlas = nullptr;   // the fonts below belong to this one (ImGui contexts come and go in tests)
    ImFont* fallback = nullptr;
    std::map<std::string, ImFont*> loaded;
    std::set<std::string> pending, failed;
    // Shaping
    std::unique_ptr<shaping::Shaper> shaper;
    std::map<std::string, int> faceOf;      // font file ("": the default) -> its face
    std::vector<int> fallbacks;             // the fallback faces, in the order they're tried
    std::vector<ImFont*> glyphFonts;        // per face (null until made)
    std::vector<std::shared_ptr<const std::string>> faceData;
    std::vector<int> faceIndex;
    std::set<int> pendingGlyphFonts;
    std::vector<Fallback> pendingFallbacks;  // (addFallback, the project's)
    std::set<std::string> scriptsWanted, scriptsTried;   // the system's fallbacks, by script
    std::set<std::string> filesTried;
    bool projectFallbacksQueued = false;
    std::unordered_map<std::string, shaping::Shaped> cache;
};

State& state() {
    static State s;
    return s;
}

// Fonts are made for the current ImGui context's atlas; a new context starts over (also one whose atlas happens to
// be where the last one's was: the fonts made are no longer in it)
State& current() {
    State& s = state();
    ImFontAtlas* atlas = ImGui::GetCurrentContext() ? ImGui::GetIO().Fonts : nullptr;
    auto owned = [&](ImFont* f) {
        if (!f || !atlas) return true;
        for (ImFont* g : atlas->Fonts)
            if (g == f) return true;
        return false;
    };
    ImFont* someGlyphFont = nullptr;
    for (ImFont* f : s.glyphFonts)
        if (f) someGlyphFont = f;
    if (s.atlas != atlas || !owned(s.fallback) || !owned(someGlyphFont) || (!s.loaded.empty() && !owned(s.loaded.begin()->second))) {
        s = State{};
        s.atlas = atlas;
    }
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

// ---- Shaping faces
int addFace(State& s, std::shared_ptr<const std::string> data, int index) {
    if (!s.shaper) s.shaper = std::make_unique<shaping::Shaper>();
    int face = s.shaper->addFace(data, index);
    if (face < 0) return -1;
    s.faceData.resize((size_t)face + 1);
    s.faceIndex.resize((size_t)face + 1);
    s.glyphFonts.resize((size_t)face + 1, nullptr);
    s.faceData[(size_t)face] = std::move(data);
    s.faceIndex[(size_t)face] = index;
    s.pendingGlyphFonts.insert(face);
    s.cache.clear();   // (a new face: lines may come out differently)
    return face;
}

int faceFor(State& s, const std::string& file) {
    auto it = s.faceOf.find(file);
    if (it != s.faceOf.end()) return it->second;
    auto data = std::make_shared<std::string>();
    if (file.empty()) {
        data->assign((const char*)kDefaultGameFont, kDefaultGameFontSize);
        glyphfont::setBaseline(glyphfont::ascentOf(data->data()));   // (shaped text sits where Roboto's does,
        glyphfont::setEmScale(glyphfont::emScaleOf(data->data()));   //  as big)
    } else if (!vfs::read(std::filesystem::u8path(project::resolve(file)), *data)) {
        data->clear();
    }
    int face = data->empty() ? -1 : addFace(s, data, 0);
    if (face < 0 && !file.empty()) face = faceFor(s, "");   // (unreadable: the default)
    s.faceOf[file] = face;
    return face;
}

bool readFile(const std::filesystem::path& file, std::string& out) {
    if (vfs::read(file, out) && !out.empty()) return true;   // (the pack first: a game's own fallbacks)
    FILE* f = std::fopen(file.u8string().c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t)size : 0);
    bool ok = size > 0 && std::fread(&out[0], 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

void loadFallback(State& s, const Fallback& f) {
    std::string key = f.file.u8string() + "#" + std::to_string(f.index);
    if (!s.filesTried.insert(key).second) return;
    auto data = std::make_shared<std::string>();
    if (!readFile(f.file, *data)) return;
    int face = addFace(s, data, f.index);
    if (face >= 0) s.fallbacks.push_back(face);
}

// Which group of fallbacks a character wants (its script, emoji, or "" for anything else)
std::string scriptOf(uint32_t cp) {
    if ((cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF)) return "emoji";
    if (cp >= 0x0590 && cp <= 0x05FF) return "he";
    if ((cp >= 0x0600 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF)) return "ar";
    if (cp >= 0x0900 && cp <= 0x0DFF) return "hi";   // (the Indic scripts)
    if (cp >= 0x0E00 && cp <= 0x0EFF) return "th";
    if ((cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x3130 && cp <= 0x318F)) return "ko";
    if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x3FFFF) ||
        (cp >= 0xFF00 && cp <= 0xFFEF))
        return "ja";   // (CJK: Han, kana, full-width forms; a Japanese font has them all)
    return "";
}

// The system's fonts for a group, best first
std::vector<Fallback> systemFallbacks(const std::string& script, uint32_t example) {
    std::vector<Fallback> out;
#if defined(_WIN32)
    const char* windir = std::getenv("WINDIR");
    std::filesystem::path fonts = std::filesystem::u8path(windir ? windir : "C:\\Windows") / "Fonts";
    std::vector<const char*> names;
    if (script == "ar") names = {"segoeui.ttf", "tahoma.ttf", "arial.ttf"};
    else if (script == "he") names = {"segoeui.ttf", "arial.ttf", "david.ttf"};
    else if (script == "hi") names = {"Nirmala.ttc", "Nirmala.ttf", "mangal.ttf"};
    else if (script == "th") names = {"leelawui.ttf", "tahoma.ttf"};
    else if (script == "ko") names = {"malgun.ttf", "gulim.ttc"};
    else if (script == "ja") names = {"YuGothM.ttc", "msgothic.ttc", "msyh.ttc", "simsun.ttc"};
    else if (script == "emoji") names = {"seguiemj.ttf", "seguisym.ttf"};
    else names = {"segoeui.ttf", "seguisym.ttf", "arialuni.ttf"};
    for (const char* n : names) out.push_back({fonts / n, 0});
    (void)example;
#elif defined(__APPLE__)
    const std::filesystem::path sys = "/System/Library/Fonts", sup = "/System/Library/Fonts/Supplemental";
    std::vector<std::filesystem::path> files;
    if (script == "ar") files = {sys / "GeezaPro.ttc", sup / "GeezaPro.ttc", sup / "Arial Unicode.ttf"};
    else if (script == "he") files = {sys / "ArialHB.ttc", sup / "Arial Hebrew.ttc", sup / "Arial Unicode.ttf"};
    else if (script == "hi") files = {sys / "Kohinoor.ttc", sup / "Devanagari Sangam MN.ttc", sup / "Arial Unicode.ttf"};
    else if (script == "th") files = {sys / "Thonburi.ttc", sup / "Thonburi.ttc"};
    else if (script == "ko") files = {sys / "AppleSDGothicNeo.ttc", sup / "AppleGothic.ttf"};
    else if (script == "ja") files = {sys / "Hiragino Sans GB.ttc", sys / "STHeiti Light.ttc", sys / "PingFang.ttc", sup / "Arial Unicode.ttf"};
    else if (script == "emoji") files = {sys / "Apple Symbols.ttf", sup / "Arial Unicode.ttf"};   // (Apple Color Emoji: bitmaps only)
    else files = {sup / "Arial Unicode.ttf", sys / "Apple Symbols.ttf"};
    for (const auto& f : files) out.push_back({f, 0});
    (void)example;
#else
    // fontconfig: the best fonts with that character (a few, best first)
    char cmd[160];
    std::snprintf(cmd, sizeof(cmd), "fc-match -s -f '%%{file}|%%{index}\\n' 'sans-serif:charset=%x' 2>/dev/null", example);
    if (FILE* p = popen(cmd, "r")) {
        char line[1024];
        while (out.size() < 3 && std::fgets(line, sizeof(line), p)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            size_t bar = l.rfind('|');
            if (bar == std::string::npos) continue;
            out.push_back({std::filesystem::u8path(l.substr(0, bar)), std::atoi(l.c_str() + bar + 1)});
        }
        pclose(p);
    }
    (void)script;
#endif
    return out;
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

const shaping::Shaped* shaped(const std::string& text, const std::string& file) {
    State& s = current();
    if (!s.atlas) return nullptr;
    std::string key = file + '\x1f' + text;
    if (auto it = s.cache.find(key); it != s.cache.end()) return &it->second;
    if (!s.projectFallbacksQueued) {   // (the game's own fallbacks, once: before the next frame)
        s.projectFallbacksQueued = true;
        std::vector<std::filesystem::path> files;
        for (const char* ext : {".ttf", ".otf", ".ttc"})
            for (const auto& f : vfs::list(project::root() / "fonts" / "fallback", ext)) files.push_back(f);
        std::sort(files.begin(), files.end());
        for (const auto& f : files) s.pendingFallbacks.push_back({f, 0});
    }
    int main = faceFor(s, file);
    if (main < 0) return nullptr;
    std::vector<int> chain{main};
    for (int f : s.fallbacks)
        if (f != main) chain.push_back(f);
    shaping::Shaped result = s.shaper->shape(text, chain);
    for (uint32_t cp : result.missing) {   // (looked for before the next frame)
        std::string script = scriptOf(cp);
        if (!s.scriptsTried.count(script)) s.scriptsWanted.insert(script + ":" + std::to_string(cp));
    }
    if (s.cache.size() > 4000) s.cache.clear();   // (texts that change every frame: a timer)
    return &(s.cache[key] = std::move(result));
}

ImFont* glyphFont(int face) {
    State& s = current();
    if (face < 0 || face >= (int)s.glyphFonts.size()) return nullptr;
    return s.glyphFonts[(size_t)face];
}

void addFallback(const std::filesystem::path& file, int index) { current().pendingFallbacks.push_back({file, index}); }

void loadPending() {
    State& s = current();
    if (!s.atlas) return;
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
    // Fallbacks: the ones asked for, then the system's for scripts the loaded fonts lack
    std::vector<Fallback> fallbacks;
    fallbacks.swap(s.pendingFallbacks);
    for (const Fallback& f : fallbacks) loadFallback(s, f);
    std::set<std::string> wanted;
    wanted.swap(s.scriptsWanted);
    for (const std::string& w : wanted) {
        size_t colon = w.rfind(':');
        std::string script = w.substr(0, colon);
        uint32_t cp = (uint32_t)std::strtoul(w.c_str() + colon + 1, nullptr, 10);
        if (!s.scriptsTried.insert(script).second) continue;
        bool covered = false;   // (one font is enough: the first of the system's that has it)
        for (const Fallback& f : systemFallbacks(script, cp)) {
            if (covered) break;
            size_t before = s.fallbacks.size();
            loadFallback(s, f);
            if (s.fallbacks.size() > before && s.shaper->hasGlyph(s.fallbacks.back(), cp)) covered = true;
        }
    }
    // Glyph fonts for new faces (shaped text's glyphs are drawn through them)
    std::set<int> glyphs;
    glyphs.swap(s.pendingGlyphFonts);
    for (int face : glyphs) {
        if (face < 0 || face >= (int)s.faceData.size() || s.glyphFonts[(size_t)face]) continue;
        const std::string& data = *s.faceData[(size_t)face];
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = true;
        void* copy = IM_ALLOC(data.size());
        std::memcpy(copy, data.data(), data.size());
        cfg.FontData = copy;
        cfg.FontDataSize = (int)data.size();
        cfg.FontNo = s.faceIndex[(size_t)face];
        cfg.FontLoader = glyphfont::loader();
        std::snprintf(cfg.Name, sizeof(cfg.Name), "glyphs of face %d", face);
        s.glyphFonts[(size_t)face] = ImGui::GetIO().Fonts->AddFont(&cfg);
    }
}

}  // namespace gamefonts
