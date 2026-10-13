#include "game/text_shaping.h"

#include <algorithm>

#include "hb.h"
#include "hb-ot.h"

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
    case 0x202A: return B::LRE;
    case 0x202B: return B::RLE;
    case 0x202C: return B::PDF;
    case 0x202D: return B::LRO;
    case 0x202E: return B::RLO;
    case 0x2066: return B::LRI;
    case 0x2067: return B::RLI;
    case 0x2068: return B::FSI;
    case 0x2069: return B::PDI;
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

namespace {

bool isolateInitiator(BidiClass c) { return c == BidiClass::LRI || c == BidiClass::RLI || c == BidiClass::FSI; }
bool removedByX9(BidiClass c) {   // (embeddings, overrides, their ends and boundary neutrals: ignored by the rules after)
    return c == BidiClass::LRE || c == BidiClass::RLE || c == BidiClass::LRO || c == BidiClass::RLO || c == BidiClass::PDF ||
           c == BidiClass::BN;
}

// BD9: each isolate initiator's matching PDI (or n)
std::vector<size_t> matchingPdis(const std::vector<BidiClass>& t) {
    const size_t n = t.size();
    std::vector<size_t> match(n, n);
    std::vector<size_t> open;
    for (size_t i = 0; i < n; ++i) {
        if (isolateInitiator(t[i])) open.push_back(i);
        else if (t[i] == BidiClass::PDI && !open.empty()) match[open.back()] = i, open.pop_back();
        else if (t[i] == BidiClass::B) open.clear();
    }
    return match;
}

// W1-W7, N1-N2, I1-I2 over one isolating run sequence (its characters' indices, in order), sos and eos its ends
void resolveSequence(const std::vector<size_t>& seq, BidiClass sos, BidiClass eos, uint8_t level, std::vector<BidiClass>& types,
                     std::vector<uint8_t>& levels) {
    using B = BidiClass;
    const size_t n = seq.size();
    std::vector<B> t(n);
    for (size_t i = 0; i < n; ++i) t[i] = types[seq[i]];
    auto ni = [](B c) { return c == B::LRI || c == B::RLI || c == B::FSI || c == B::PDI; };
    for (size_t i = 0; i < n; ++i)   // W1: marks take the type before them (after an isolate's ends: neutral)
        if (t[i] == B::NSM) t[i] = i == 0 ? sos : ni(t[i - 1]) ? B::ON : t[i - 1];
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
    // N1 / N2: neutrals (isolates' ends among them) between two of the same direction take it (numbers count as
    // right to left), else the embedding's
    auto neutral = [&](B c) { return c == B::B || c == B::S || c == B::WS || c == B::ON || c == B::BN || ni(c); };
    auto dir = [](B c) { return c == B::L ? B::L : B::R; };
    const B embedding = level & 1 ? B::R : B::L;
    for (size_t i = 0; i < n;) {
        if (!neutral(t[i])) { ++i; continue; }
        size_t j = i;
        while (j < n && neutral(t[j])) ++j;
        B before = i == 0 ? sos : dir(t[i - 1]), after = j == n ? eos : dir(t[j]);
        B take = before == after ? before : embedding;
        for (size_t k = i; k < j; ++k) t[k] = take;
        i = j;
    }
    for (size_t i = 0; i < n; ++i) {   // I1 / I2
        uint8_t& l = levels[seq[i]];
        if (!(l & 1)) l = (uint8_t)(l + (t[i] == B::R ? 1 : t[i] == B::AN || t[i] == B::EN ? 2 : 0));
        else l = (uint8_t)(l + (t[i] == B::L || t[i] == B::EN || t[i] == B::AN ? 1 : 0));
        types[seq[i]] = t[i];
    }
}

}  // namespace

int paragraphLevel(const std::vector<uint32_t>& text, size_t from, size_t to) {
    // P2 / P3: the first strong character, skipping what's inside isolates
    int depth = 0;
    for (size_t i = from; i < std::min(to, text.size()); ++i) {
        BidiClass c = bidiClass(text[i]);
        if (isolateInitiator(c)) {
            ++depth;
            continue;
        }
        if (c == BidiClass::PDI) {
            if (depth > 0) --depth;
            continue;
        }
        if (c == BidiClass::B) break;
        if (depth > 0) continue;
        if (c == BidiClass::L) return 0;
        if (c == BidiClass::R || c == BidiClass::AL) return 1;
    }
    return 0;
}

std::vector<uint8_t> bidiLevels(const std::vector<uint32_t>& text, int paragraph) {
    using B = BidiClass;
    const size_t n = text.size();
    std::vector<B> t(n);
    for (size_t i = 0; i < n; ++i) t[i] = bidiClass(text[i]);
    const std::vector<size_t> match = matchingPdis(t);
    std::vector<uint8_t> levels(n, (uint8_t)paragraph);

    // X1-X8: the directional status stack (levels up to 125; past that, embeddings and isolates are counted, not
    // pushed, so their ends still pair up)
    const int kMaxDepth = 125;
    struct Status {
        uint8_t level;
        B override;   // ON: none, else L or R
        bool isolate;
    };
    std::vector<Status> stack{{(uint8_t)paragraph, B::ON, false}};
    int overflowIsolates = 0, overflowEmbeddings = 0, validIsolates = 0;
    for (size_t i = 0; i < n; ++i) {
        const Status top = stack.back();
        B c = t[i];
        auto next = [&](bool rtl) { return (int)top.level + 1 + ((top.level & 1) == (rtl ? 1 : 0) ? 1 : 0); };
        if (c == B::RLE || c == B::LRE || c == B::RLO || c == B::LRO) {
            bool rtl = c == B::RLE || c == B::RLO;
            int level = next(rtl);
            levels[i] = top.level;
            if (level <= kMaxDepth && overflowIsolates == 0 && overflowEmbeddings == 0)
                stack.push_back({(uint8_t)level, c == B::RLO ? B::R : c == B::LRO ? B::L : B::ON, false});
            else if (overflowIsolates == 0)
                ++overflowEmbeddings;
        } else if (isolateInitiator(c)) {
            levels[i] = top.level;
            if (top.override != B::ON) t[i] = top.override;
            bool rtl = c == B::RLI || (c == B::FSI && paragraphLevel(text, i + 1, match[i]) == 1);
            int level = next(rtl);
            if (level <= kMaxDepth && overflowIsolates == 0 && overflowEmbeddings == 0) {
                ++validIsolates;
                stack.push_back({(uint8_t)level, B::ON, true});
            } else {
                ++overflowIsolates;
            }
        } else if (c == B::PDI) {
            if (overflowIsolates > 0) {
                --overflowIsolates;
            } else if (validIsolates > 0) {
                overflowEmbeddings = 0;
                while (!stack.back().isolate) stack.pop_back();
                stack.pop_back();
                --validIsolates;
            }
            levels[i] = stack.back().level;
            if (stack.back().override != B::ON) t[i] = stack.back().override;
        } else if (c == B::PDF) {
            levels[i] = top.level;
            if (overflowIsolates > 0) {
            } else if (overflowEmbeddings > 0) {
                --overflowEmbeddings;
            } else if (!top.isolate && stack.size() >= 2) {
                stack.pop_back();
            }
        } else if (c == B::B) {
            levels[i] = (uint8_t)paragraph;
        } else {
            levels[i] = top.level;
            if (c != B::BN && top.override != B::ON) t[i] = top.override;
        }
    }

    // X9 / X10: without the removed characters, level runs joined across isolates into isolating run sequences
    std::vector<std::vector<size_t>> runs;   // level runs: indices
    for (size_t i = 0; i < n; ++i) {
        if (removedByX9(t[i])) continue;
        if (runs.empty() || levels[runs.back().back()] != levels[i]) runs.push_back({});
        runs.back().push_back(i);
    }
    std::vector<int> runOf(n, -1);
    for (size_t r = 0; r < runs.size(); ++r)
        for (size_t i : runs[r]) runOf[i] = (int)r;
    std::vector<bool> used(runs.size(), false);
    for (size_t r = 0; r < runs.size(); ++r) {
        if (used[r]) continue;
        // (a run starting with a PDI matched to an earlier initiator joined that one's sequence)
        std::vector<size_t> seq;
        size_t at = r;
        while (true) {
            used[at] = true;
            seq.insert(seq.end(), runs[at].begin(), runs[at].end());
            size_t last = runs[at].back();
            if (!isolateInitiator(t[last]) || match[last] >= n || runOf[match[last]] < 0) break;
            at = (size_t)runOf[match[last]];
        }
        uint8_t level = levels[seq.front()];
        // sos / eos: the higher of this level and the one beside the sequence (the paragraph's at the ends, and
        // after an isolate initiator left open)
        auto beside = [&](size_t i, int step) {
            for (long k = (long)i + step; k >= 0 && k < (long)n; k += step)
                if (!removedByX9(t[(size_t)k])) return (int)levels[(size_t)k];
            return paragraph;
        };
        int before = beside(seq.front(), -1);
        size_t last = seq.back();
        int after = isolateInitiator(t[last]) && match[last] >= n ? paragraph : beside(last, 1);
        B sos = std::max<int>(level, before) & 1 ? B::R : B::L, eos = std::max<int>(level, after) & 1 ? B::R : B::L;
        resolveSequence(seq, sos, eos, level, t, levels);
    }
    // The removed characters: the level beside them (for reordering only)
    for (size_t i = 0; i < n; ++i)
        if (removedByX9(t[i])) levels[i] = i > 0 ? levels[i - 1] : (uint8_t)paragraph;
    // L1: separators, and whitespace and isolate controls before them or at the end, at the paragraph's level
    auto resets = [](B c) {
        return c == B::WS || c == B::BN || c == B::LRI || c == B::RLI || c == B::FSI || c == B::PDI || removedByX9(c);
    };
    bool trailing = true;
    for (size_t i = n; i-- > 0;) {
        B c = bidiClass(text[i]);   // (the original class: the rules above changed t)
        if (c == B::S || c == B::B) {
            levels[i] = (uint8_t)paragraph;
            trailing = true;
        } else if (trailing && resets(c)) {
            levels[i] = (uint8_t)paragraph;
        } else {
            trailing = false;
        }
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
        if (b == 0xE2 && i + 2 < utf8.size()) {   // U+2000..20BF: punctuation and currency (as they are), but the
            uint8_t b1 = (uint8_t)utf8[i + 1], b2 = (uint8_t)utf8[i + 2];   // bidi controls need the bidi pass
            bool controls = (b1 == 0x80 && ((b2 >= 0x8E && b2 <= 0x8F) || (b2 >= 0xAA && b2 <= 0xAE))) ||
                            (b1 == 0x81 && b2 >= 0xA6 && b2 <= 0xA9);
            if (controls) return true;
            if (b1 <= 0x82) continue;
        }
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
    bool color = false;   // emoji (CBDT / sbix / COLR): only joiners and modifiers stay in its runs
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
    f->color = hb_ot_color_has_png(f->face) || hb_ot_color_has_layers(f->face);
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
        // Spaces and marks stay in the run's font (an emoji font's spaces are as wide as an emoji: those don't)
        if (cur && (joins(cp) || (hasGlyph(cur->face, cp) && !faces_[(size_t)cur->face]->color))) face = cur->face;
        // An emoji, or a character asked for as one (followed by U+FE0F): a colour font first
        bool emoji = cp >= 0x1F000 || (i + 1 < text.size() && text[i + 1] == 0xFE0F);
        for (int pass = emoji ? 0 : 1; pass < 2 && face < 0; ++pass)
            for (int f : faces)
                if ((pass == 1 || faces_[(size_t)f]->color) && hasGlyph(f, cp)) {
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
            gl.start = pen;
            gl.advance = (float)pos[g].x_advance / f.upem;
            pen += gl.advance;
            out.glyphs.push_back(gl);
        }
    }
    hb_buffer_destroy(buf);
    out.width = pen;
    return out;
}

std::vector<float> caretStops(const std::vector<uint32_t>& text, const Shaped& shaped) {
    const size_t n = text.size();
    std::vector<float> stops(n + 1, 0.f);
    if (n == 0) return stops;
    std::vector<uint8_t> levels = bidiLevels(text, paragraphLevel(text));
    // Each character's box on the line: its cluster's glyphs' advances, split evenly among the cluster's characters
    std::vector<float> left(n, -1.f), right(n, -1.f);
    std::vector<size_t> clusterOf(n);
    std::vector<uint32_t> starts;
    for (const Glyph& g : shaped.glyphs) starts.push_back(g.cluster);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    for (size_t c = 0; c < starts.size(); ++c) {
        size_t from = starts[c], to = c + 1 < starts.size() ? starts[c + 1] : n;
        float lo = 1e30f, hi = -1e30f;
        for (const Glyph& g : shaped.glyphs)
            if (g.cluster == starts[c]) lo = std::min(lo, g.start), hi = std::max(hi, g.start + g.advance);
        if (lo > hi || from >= n) continue;
        to = std::min(to, n);
        const float share = (hi - lo) / (float)std::max<size_t>(to - from, 1);
        const bool rtl = levels[from] & 1;
        for (size_t i = from; i < to; ++i) {   // (in a right-to-left cluster, the first character is the rightmost)
            size_t k = i - from;
            left[i] = rtl ? hi - share * (float)(k + 1) : lo + share * (float)k;
            right[i] = left[i] + share;
        }
    }
    float lastEdge = 0.f;
    for (size_t i = 0; i < n; ++i) {
        if (left[i] < 0.f) {   // (a character with no glyph of its own: where the one before ended)
            stops[i] = lastEdge;
            continue;
        }
        stops[i] = levels[i] & 1 ? right[i] : left[i];
        lastEdge = levels[i] & 1 ? left[i] : right[i];
    }
    stops[n] = lastEdge;
    return stops;
}

}  // namespace shaping
