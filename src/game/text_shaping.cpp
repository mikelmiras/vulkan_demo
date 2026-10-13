#include "game/text_shaping.h"

#include <algorithm>

#include "hb.h"

namespace shaping {

std::vector<uint32_t> decodeUtf8(const std::string& text) {
    std::vector<uint32_t> out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        uint8_t b = (uint8_t)text[i];
        int n = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + (size_t)n >= text.size() + (n ? 0 : 1)) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        uint32_t cp = n == 0 ? b : b & (0x3F >> n);
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            uint8_t c = (uint8_t)text[i + (size_t)k];
            if ((c & 0xC0) != 0x80) ok = false;
            cp = (cp << 6) | (c & 0x3F);
        }
        out.push_back(ok ? cp : 0xFFFD);
        i += ok ? (size_t)n + 1 : 1;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Bidi
// ---------------------------------------------------------------------------------------------
BidiClass bidiClass(uint32_t cp) {
    using B = BidiClass;
    if (cp >= '0' && cp <= '9') return B::EN;
    switch (cp) {
    case '+': case '-': return B::ES;
    case '#': case '$': case '%': case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xB0: case 0xB1: return B::ET;
    case ',': case '.': case '/': case ':': case 0xA0: return B::CS;
    case '\t': case 0x1F: return B::S;
    case '\n': case '\r': case 0x1C: case 0x1D: case 0x1E: case 0x85: case 0x2029: return B::B;
    case ' ': case 0x1680: case 0x2028: case 0x205F: case 0x3000: case 0x0C: return B::WS;
    case 0x200E: return B::L;    // LRM
    case 0x200F: return B::R;    // RLM
    case 0x061C: return B::AL;   // ALM
    case 0xFEFF: case 0x200B: case 0x200C: case 0x200D: return B::BN;
    default: break;
    }
    if (cp >= 0x2000 && cp <= 0x200A) return B::WS;
    if ((cp >= 0x2030 && cp <= 0x2034) || (cp >= 0x20A0 && cp <= 0x20CF)) return B::ET;
    if (cp >= 0x0660 && cp <= 0x0669) return B::AN;   // Arabic-Indic digits
    if (cp >= 0x06F0 && cp <= 0x06F9) return B::EN;   // (Persian ones: European-like)
    if (cp == 0x066B || cp == 0x066C) return B::AN;
    hb_unicode_funcs_t* u = hb_unicode_funcs_get_default();
    hb_unicode_general_category_t gc = hb_unicode_general_category(u, cp);
    if (gc == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK || gc == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK) return B::NSM;
    // Right-to-left blocks: Hebrew, NKo, Samaritan, Mandaic (R); Arabic, Syriac, Thaana and their extensions (AL)
    if ((cp >= 0x0590 && cp <= 0x05FF) || (cp >= 0x07C0 && cp <= 0x085F) || (cp >= 0xFB1D && cp <= 0xFB4F) ||
        (cp >= 0x10800 && cp <= 0x10CFF) || (cp >= 0x1E800 && cp <= 0x1EDFF))
        return B::R;
    if ((cp >= 0x0600 && cp <= 0x07BF) || (cp >= 0x0860 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
        (cp >= 0xFE70 && cp <= 0xFEFC) || (cp >= 0x10D00 && cp <= 0x10D3F) || (cp >= 0x1EE00 && cp <= 0x1EEFF))
        return B::AL;
    switch (gc) {
    case HB_UNICODE_GENERAL_CATEGORY_FORMAT: return B::BN;
    case HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR: return B::WS;
    case HB_UNICODE_GENERAL_CATEGORY_CONNECT_PUNCTUATION: case HB_UNICODE_GENERAL_CATEGORY_DASH_PUNCTUATION:
    case HB_UNICODE_GENERAL_CATEGORY_CLOSE_PUNCTUATION: case HB_UNICODE_GENERAL_CATEGORY_FINAL_PUNCTUATION:
    case HB_UNICODE_GENERAL_CATEGORY_INITIAL_PUNCTUATION: case HB_UNICODE_GENERAL_CATEGORY_OTHER_PUNCTUATION:
    case HB_UNICODE_GENERAL_CATEGORY_OPEN_PUNCTUATION: case HB_UNICODE_GENERAL_CATEGORY_CURRENCY_SYMBOL:
    case HB_UNICODE_GENERAL_CATEGORY_MODIFIER_SYMBOL: case HB_UNICODE_GENERAL_CATEGORY_MATH_SYMBOL:
    case HB_UNICODE_GENERAL_CATEGORY_OTHER_SYMBOL: case HB_UNICODE_GENERAL_CATEGORY_CONTROL:
        return B::ON;
    default: return B::L;   // (letters, spacing marks and digits of the other scripts)
    }
}

int paragraphLevel(const std::vector<uint32_t>& text) {
    for (uint32_t cp : text) {
        BidiClass c = bidiClass(cp);
        if (c == BidiClass::L) return 0;
        if (c == BidiClass::R || c == BidiClass::AL) return 1;
    }
    return 0;
}

std::vector<uint8_t> bidiLevels(const std::vector<uint32_t>& text, int paragraph) {
    using B = BidiClass;
    const size_t n = text.size();
    const B sos = paragraph & 1 ? B::R : B::L, eos = sos;
    std::vector<B> t(n);
    for (size_t i = 0; i < n; ++i) t[i] = bidiClass(text[i]);
    for (size_t i = 0; i < n; ++i)   // W1: marks take the type before them
        if (t[i] == B::NSM) t[i] = i == 0 ? sos : t[i - 1];
    B strong = sos;   // W2: numbers after Arabic letters are Arabic numbers
    for (size_t i = 0; i < n; ++i) {
        if (t[i] == B::L || t[i] == B::R || t[i] == B::AL) strong = t[i];
        else if (t[i] == B::EN && strong == B::AL) t[i] = B::AN;
    }
    for (B& c : t)   // W3
        if (c == B::AL) c = B::R;
    for (size_t i = 1; i + 1 < n; ++i) {   // W4: one separator between numbers of a kind
        if (t[i] == B::ES && t[i - 1] == B::EN && t[i + 1] == B::EN) t[i] = B::EN;
        else if (t[i] == B::CS && t[i - 1] == t[i + 1] && (t[i - 1] == B::EN || t[i - 1] == B::AN)) t[i] = t[i - 1];
    }
    for (size_t i = 0; i < n;) {   // W5: terminators next to European numbers join them
        if (t[i] != B::ET) { ++i; continue; }
        size_t j = i;
        while (j < n && t[j] == B::ET) ++j;
        if ((i > 0 && t[i - 1] == B::EN) || (j < n && t[j] == B::EN))
            for (size_t k = i; k < j; ++k) t[k] = B::EN;
        i = j;
    }
    for (B& c : t)   // W6
        if (c == B::ES || c == B::ET || c == B::CS) c = B::ON;
    strong = sos;   // W7: European numbers after left-to-right text are left to right
    for (size_t i = 0; i < n; ++i) {
        if (t[i] == B::L || t[i] == B::R) strong = t[i];
        else if (t[i] == B::EN && strong == B::L) t[i] = B::L;
    }
    // N1 / N2: neutrals between two of the same direction take it (numbers count as right to left), else the
    // paragraph's
    auto neutral = [](B c) { return c == B::B || c == B::S || c == B::WS || c == B::ON || c == B::BN; };
    auto dir = [](B c) { return c == B::L ? B::L : B::R; };
    for (size_t i = 0; i < n;) {
        if (!neutral(t[i])) { ++i; continue; }
        size_t j = i;
        while (j < n && neutral(t[j])) ++j;
        B before = i == 0 ? sos : dir(t[i - 1]), after = j == n ? eos : dir(t[j]);
        B take = before == after ? before : sos;
        for (size_t k = i; k < j; ++k) t[k] = take;
        i = j;
    }
    std::vector<uint8_t> levels(n, (uint8_t)paragraph);
    for (size_t i = 0; i < n; ++i) {   // I1 / I2
        if (!(paragraph & 1)) levels[i] = (uint8_t)(paragraph + (t[i] == B::R ? 1 : t[i] == B::AN || t[i] == B::EN ? 2 : 0));
        else levels[i] = (uint8_t)(paragraph + (t[i] == B::L || t[i] == B::EN || t[i] == B::AN ? 1 : 0));
    }
    for (size_t i = n; i-- > 0;) {   // L1: trailing whitespace at the paragraph's level
        B c = bidiClass(text[i]);
        if (c != B::WS && c != B::S && c != B::B && c != B::BN) break;
        levels[i] = (uint8_t)paragraph;
    }
    return levels;
}

namespace {

// L2: from the highest level down to the lowest odd one, every stretch at that level or above is reversed
template <class T, class LevelOf> void reorder(std::vector<T>& items, LevelOf levelOf) {
    if (items.empty()) return;
    int highest = 0, lowestOdd = 255;
    for (const T& it : items) {
        int l = levelOf(it);
        highest = std::max(highest, l);
        if (l & 1) lowestOdd = std::min(lowestOdd, l);
    }
    for (int level = highest; level >= lowestOdd && level > 0; --level)
        for (size_t i = 0; i < items.size();) {
            if (levelOf(items[i]) < level) { ++i; continue; }
            size_t j = i;
            while (j < items.size() && levelOf(items[j]) >= level) ++j;
            std::reverse(items.begin() + (long)i, items.begin() + (long)j);
            i = j;
        }
}

}  // namespace

std::vector<Run> visualRuns(const std::vector<uint8_t>& levels) {
    std::vector<Run> runs;
    for (size_t i = 0; i < levels.size();) {
        size_t j = i;
        while (j < levels.size() && levels[j] == levels[i]) ++j;
        runs.push_back({i, j, levels[i]});
        i = j;
    }
    reorder(runs, [](const Run& r) { return (int)r.level; });
    return runs;
}

bool needsShaping(const std::string& utf8) {
    for (size_t i = 0; i < utf8.size(); ++i) {
        uint8_t b = (uint8_t)utf8[i];
        if (b < 0xCC) continue;   // (ASCII, Latin, Greek, Cyrillic: drawn as they are)
        if (b == 0xCC || b == 0xCD) return true;          // U+0300..037F: combining marks
        if (b >= 0xCE && b < 0xD6) continue;              // (Greek, Cyrillic, Armenian)
        if (b == 0xE2 && i + 1 < utf8.size() && (uint8_t)utf8[i + 1] <= 0x82) continue;   // (U+2000..20BF: punctuation, currency)
        if (b >= 0xD6) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Shaping
// ---------------------------------------------------------------------------------------------
struct Shaper::Face {
    std::shared_ptr<const std::string> data;
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* font = nullptr;
    float upem = 1000.f;
    ~Face() {
        if (font) hb_font_destroy(font);
        if (face) hb_face_destroy(face);
        if (blob) hb_blob_destroy(blob);
    }
};

Shaper::Shaper() = default;
Shaper::~Shaper() = default;

int Shaper::addFace(std::shared_ptr<const std::string> data, int index) {
    if (!data || data->empty()) return -1;
    auto f = std::make_unique<Face>();
    f->data = std::move(data);
    f->blob = hb_blob_create(f->data->data(), (unsigned)f->data->size(), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    f->face = hb_face_create(f->blob, (unsigned)std::max(index, 0));
    if (hb_face_get_glyph_count(f->face) == 0) return -1;   // (not a font)
    f->upem = (float)std::max(hb_face_get_upem(f->face), 1u);
    f->font = hb_font_create(f->face);
    hb_font_set_scale(f->font, (int)f->upem, (int)f->upem);   // (positions in font units)
    faces_.push_back(std::move(f));
    return (int)faces_.size() - 1;
}

int Shaper::faceCount() const { return (int)faces_.size(); }

bool Shaper::hasGlyph(int face, uint32_t cp) const {
    if (face < 0 || face >= (int)faces_.size()) return false;
    hb_codepoint_t g = 0;
    return hb_font_get_nominal_glyph(faces_[(size_t)face]->font, cp, &g) && g != 0;
}

float Shaper::ascent(int face) const {
    if (face < 0 || face >= (int)faces_.size()) return 0.8f;
    hb_font_extents_t e{};
    hb_font_get_h_extents(faces_[(size_t)face]->font, &e);
    return (float)e.ascender / faces_[(size_t)face]->upem;
}

Shaped Shaper::shape(const std::string& utf8, const std::vector<int>& chain) const {
    Shaped out;
    std::vector<uint32_t> text = decodeUtf8(utf8);
    std::vector<int> faces;   // (the chain's usable faces)
    for (int f : chain)
        if (f >= 0 && f < (int)faces_.size()) faces.push_back(f);
    if (text.empty() || faces.empty()) return out;
    std::vector<uint8_t> levels = bidiLevels(text, paragraphLevel(text));
    hb_unicode_funcs_t* u = hb_unicode_funcs_get_default();
    // Itemising: a run per level, script and face
    struct Item {
        size_t start, end;
        uint8_t level;
        hb_script_t script;
        int face;
    };
    std::vector<Item> items;
    auto joins = [&](uint32_t cp) {   // (marks, joiners, variation selectors: with what they're on)
        hb_unicode_general_category_t gc = hb_unicode_general_category(u, cp);
        return gc == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK || gc == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK ||
               gc == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK || cp == 0x200C || cp == 0x200D ||
               (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF) || (cp >= 0x1F3FB && cp <= 0x1F3FF);
    };
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t cp = text[i];
        hb_script_t script = hb_unicode_script(u, cp);
        bool weak = script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED || script == HB_SCRIPT_UNKNOWN;
        Item* cur = items.empty() ? nullptr : &items.back();
        int face = -1;
        if (cur && (hasGlyph(cur->face, cp) || joins(cp))) face = cur->face;   // (spaces, marks: stay in the run's font)
        if (face < 0)
            for (int f : faces)
                if (hasGlyph(f, cp)) {
                    face = f;
                    break;
                }
        if (face < 0) {   // (nobody has it: the first face's missing-glyph box)
            face = cur ? cur->face : faces[0];
            if (cp > 0x20 && std::find(out.missing.begin(), out.missing.end(), cp) == out.missing.end()) out.missing.push_back(cp);
        }
        bool sameScript = weak || !cur || cur->script == HB_SCRIPT_COMMON || cur->script == script;
        if (cur && cur->level == levels[i] && cur->face == face && sameScript) {
            cur->end = i + 1;
            if (!weak && cur->script == HB_SCRIPT_COMMON) cur->script = script;
            continue;
        }
        items.push_back({i, i + 1, levels[i], weak ? HB_SCRIPT_COMMON : script, face});
    }
    reorder(items, [](const Item& it) { return (int)it.level; });
    // Shaping each run (the whole line as its context), laid out left to right
    hb_buffer_t* buf = hb_buffer_create();
    float pen = 0.f;
    for (const Item& it : items) {
        const Face& f = *faces_[(size_t)it.face];
        hb_buffer_clear_contents(buf);
        hb_buffer_add_codepoints(buf, text.data(), (int)text.size(), (unsigned)it.start, (int)(it.end - it.start));
        hb_buffer_set_direction(buf, it.level & 1 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        if (it.script != HB_SCRIPT_COMMON) hb_buffer_set_script(buf, it.script);
        hb_buffer_guess_segment_properties(buf);
        hb_shape(f.font, buf, nullptr, 0);
        unsigned count = 0;
        const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
        const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &count);
        for (unsigned g = 0; g < count; ++g) {   // (right-to-left runs come back in visual order already)
            Glyph gl;
            gl.face = it.face;
            gl.glyph = info[g].codepoint;
            gl.cluster = info[g].cluster;
            gl.x = pen + (float)pos[g].x_offset / f.upem;
            gl.y = -(float)pos[g].y_offset / f.upem;
            pen += (float)pos[g].x_advance / f.upem;
            out.glyphs.push_back(gl);
        }
    }
    hb_buffer_destroy(buf);
    out.width = pen;
    return out;
}

}  // namespace shaping
