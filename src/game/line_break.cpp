// UAX #14's pair rules (LB4-LB31, without the tailorings for Hebrew letters and the dictionary scripts) over classes
// from ranges, then greedy wrapping. See line_break.h.
#include "game/line_break.h"

#include <initializer_list>

#include "game/text_shaping.h"
#include "hb.h"

namespace linebreak {

Class classOf(uint32_t cp) {
    using C = Class;
    switch (cp) {
    case 0x0B: case 0x0C: case 0x2028: case 0x2029: return C::BK;
    case 0x0D: return C::CR;
    case 0x0A: return C::LF;
    case 0x85: return C::NL;
    case ' ': return C::SP;
    case 0x200B: return C::ZW;
    case 0x200D: return C::ZWJ;
    case 0xA0: case 0x202F: case 0x2007: case 0x2011: case 0x180E: case 0x034F: return C::GL;
    case 0x2060: case 0xFEFF: return C::WJ;
    case '(': case '[': case '{': case 0xA1: case 0xBF: case 0x2045: case 0x207D: case 0x208D: return C::OP;
    case ')': case ']': return C::CP;
    case '}': case 0x2046: case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFE50: case 0xFE52: return C::CL;
    case '"': case '\'': case 0xAB: case 0xBB: case 0x2018: case 0x2019: case 0x201B: case 0x201C: case 0x201D:
    case 0x201F: case 0x2039: case 0x203A: return C::QU;
    case ',': case '.': case ':': case ';': case 0x037E: case 0x0589: case 0x060C: case 0x060D: case 0x07F8: case 0x2044:
    case 0xFE10: case 0xFE13: case 0xFE14: return C::IS;
    case '$': case '+': case '\\': case 0xA3: case 0xA4: case 0xA5: case 0xB1: case 0x20AC: case 0x2116: return C::PR;
    case '%': case 0xA2: case 0xB0: case 0x2030: case 0x2031: case 0x2032: case 0x2033: case 0x2103: case 0x2109: return C::PO;
    case '/': return C::SY;
    case '-': return C::HY;
    case '\t': case 0xAD: case 0x058A: case 0x2010: case 0x2012: case 0x2013: case 0x05BE: case 0x0F0B: case 0x1361:
    case 0x17D8: case 0x17DA: case '|': case 0x2027: case 0x205F: case 0x3000: return C::BA;
    case 0xB4: case 0x02C8: case 0x02CC: case 0x02DF: return C::BB;
    case 0x2014: return C::B2;
    case '!': case '?': case 0x05C6: case 0x061B: case 0x061E: case 0x061F: case 0x06D4: case 0x07F9: case 0x0F0D:
    case 0xFF01: case 0xFF1F: return C::EX;
    case 0x2024: case 0x2025: case 0x2026: case 0x22EF: case 0xFE19: return C::IN;
    case 0x3005: case 0x303B: case 0x309B: case 0x309C: case 0x309D: case 0x309E: case 0x30A0: case 0x30FB: case 0x30FD:
    case 0x30FE: case 0x30FC: return C::NS;
    default: break;
    }
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return C::CM;   // (other controls attach, as LB9 has them)
    if (cp >= 0x2000 && cp <= 0x200A && cp != 0x2007) return C::BA;   // (spaces of other widths)
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return C::RI;
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return C::EM;
    // Hangul: syllables with or without a final (H2 / H3), the jamo
    if (cp >= 0xAC00 && cp <= 0xD7A3) return (cp - 0xAC00) % 28 == 0 ? C::H2 : C::H3;
    if (cp >= 0x1100 && cp <= 0x115F) return C::JL;
    if (cp >= 0x1160 && cp <= 0x11A7) return C::JV;
    if (cp >= 0x11A8 && cp <= 0x11FF) return C::JT;
    // Small kana and the like: not at a line's start (CJ, taken as NS)
    if (cp == 0x3041 || cp == 0x3043 || cp == 0x3045 || cp == 0x3047 || cp == 0x3049 || cp == 0x3063 || cp == 0x3083 ||
        cp == 0x3085 || cp == 0x3087 || cp == 0x308E || cp == 0x30A1 || cp == 0x30A3 || cp == 0x30A5 || cp == 0x30A7 ||
        cp == 0x30A9 || cp == 0x30C3 || cp == 0x30E3 || cp == 0x30E5 || cp == 0x30E7 || cp == 0x30EE)
        return C::NS;
    // CJK brackets
    if (cp == 0x3008 || cp == 0x300A || cp == 0x300C || cp == 0x300E || cp == 0x3010 || cp == 0x3014 || cp == 0xFF08 ||
        cp == 0xFF3B || cp == 0xFF5B)
        return C::OP;
    if (cp == 0x3009 || cp == 0x300B || cp == 0x300D || cp == 0x300F || cp == 0x3011 || cp == 0x3015 || cp == 0xFF3D || cp == 0xFF5D)
        return C::CL;
    if (cp == 0xFF09) return C::CP;
    // Ideographs, kana, fullwidth forms, emoji: a break may go between any two
    if ((cp >= 0x2E80 && cp <= 0x2FFF) || (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3100 && cp <= 0x31FF) ||
        (cp >= 0x3200 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF01 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD))
        return C::ID;
    if ((cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x1F000 && cp <= 0x1F2FF))
        return C::ID;   // (emoji; the people ones also take skin tones: EB, folded into ID here with LB30b below)
    hb_unicode_general_category_t gc = hb_unicode_general_category(hb_unicode_funcs_get_default(), cp);
    switch (gc) {
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK: case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK: case HB_UNICODE_GENERAL_CATEGORY_FORMAT:
        return C::CM;
    case HB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER: return C::NU;
    case HB_UNICODE_GENERAL_CATEGORY_OPEN_PUNCTUATION: return C::OP;
    case HB_UNICODE_GENERAL_CATEGORY_CLOSE_PUNCTUATION: return C::CL;
    case HB_UNICODE_GENERAL_CATEGORY_INITIAL_PUNCTUATION: case HB_UNICODE_GENERAL_CATEGORY_FINAL_PUNCTUATION: return C::QU;
    case HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR: return C::BA;
    case HB_UNICODE_GENERAL_CATEGORY_CURRENCY_SYMBOL: return C::PR;
    default: return C::AL;   // (letters, symbols; Thai, Lao, Khmer, Myanmar too: they'd need a dictionary)
    }
}

std::vector<Break> opportunities(const std::vector<uint32_t>& text) {
    using C = Class;
    const size_t n = text.size();
    std::vector<Break> out(n, Break::None);
    if (n == 0) return out;
    std::vector<C> cls(n);
    std::vector<bool> zwj(n, false);   // (the character itself is a ZWJ: LB8a looks at it before LB9 folds it in)
    for (size_t i = 0; i < n; ++i) {
        cls[i] = classOf(text[i]);
        zwj[i] = cls[i] == C::ZWJ;
    }
    // LB9 / LB10: marks and ZWJ take the class of what they're on (no break before them); with nothing to be on, AL
    for (size_t i = 0; i < n; ++i) {
        if (cls[i] != C::CM && cls[i] != C::ZWJ) continue;
        C base = i > 0 ? cls[i - 1] : C::BK;
        bool attaches = base != C::BK && base != C::CR && base != C::LF && base != C::NL && base != C::SP && base != C::ZW;
        cls[i] = attaches ? base : C::AL;
    }
    auto isOneOf = [](C c, std::initializer_list<C> set) {
        for (C s : set)
            if (c == s) return true;
        return false;
    };
    C beforeSpaces = cls[0];   // the class of the last character that isn't a space (for the "X SP* ×" rules)
    int riRun = cls[0] == C::RI ? 1 : 0;   // regional indicators in a row before this one
    for (size_t i = 1; i < n; ++i) {
        const C a = cls[i - 1], b = cls[i];
        const uint32_t rawA = text[i - 1];
        if (a != C::SP) beforeSpaces = a;
        Break k = Break::Allowed;
        C rawClassB = classOf(text[i]);
        if (rawA != 0 && (classOf(rawA) == C::BK || classOf(rawA) == C::LF || classOf(rawA) == C::NL)) k = Break::Mandatory;  // LB4, LB5
        else if (classOf(rawA) == C::CR) k = rawClassB == C::LF ? Break::None : Break::Mandatory;
        else if (isOneOf(rawClassB, {C::BK, C::CR, C::LF, C::NL})) k = Break::None;   // LB6
        else if (b == C::SP || rawClassB == C::ZW) k = Break::None;                    // LB7
        else if (beforeSpaces == C::ZW && (a == C::SP || a == C::ZW || classOf(rawA) == C::ZW)) k = Break::Allowed;   // LB8
        else if (zwj[i - 1]) k = Break::None;                                           // LB8a
        else if (rawClassB == C::CM || rawClassB == C::ZWJ) k = Break::None;            // LB9 (attached)
        else if (a == C::WJ || b == C::WJ) k = Break::None;                             // LB11
        else if (a == C::GL) k = Break::None;                                           // LB12
        else if (b == C::GL && !isOneOf(a, {C::SP, C::BA, C::HY})) k = Break::None;     // LB12a
        else if (isOneOf(b, {C::CL, C::CP, C::EX, C::IS, C::SY})) k = Break::None;      // LB13
        else if (beforeSpaces == C::OP) k = Break::None;                                // LB14
        else if (beforeSpaces == C::QU && b == C::OP) k = Break::None;                  // LB15
        else if (isOneOf(beforeSpaces, {C::CL, C::CP}) && b == C::NS) k = Break::None;  // LB16
        else if (beforeSpaces == C::B2 && b == C::B2) k = Break::None;                  // LB17
        else if (a == C::SP) k = Break::Allowed;                                        // LB18
        else if (a == C::QU || b == C::QU) k = Break::None;                             // LB19
        else if (isOneOf(b, {C::BA, C::HY, C::NS}) || a == C::BB) k = Break::None;      // LB21
        else if (b == C::IN) k = Break::None;                                           // LB22
        else if ((a == C::AL && b == C::NU) || (a == C::NU && b == C::AL)) k = Break::None;                // LB23
        else if ((a == C::PR && isOneOf(b, {C::ID, C::EB, C::EM})) || (isOneOf(a, {C::ID, C::EB, C::EM}) && b == C::PO))
            k = Break::None;                                                            // LB23a
        else if ((isOneOf(a, {C::PR, C::PO}) && b == C::AL) || (a == C::AL && isOneOf(b, {C::PR, C::PO}))) k = Break::None;  // LB24
        else if ((isOneOf(a, {C::CL, C::CP, C::NU}) && isOneOf(b, {C::PO, C::PR})) ||
                 (isOneOf(a, {C::PO, C::PR}) && isOneOf(b, {C::OP, C::NU})) ||
                 (isOneOf(a, {C::HY, C::IS, C::NU, C::SY}) && b == C::NU) || (a == C::OP && b == C::NU))
            k = Break::None;                                                            // LB25 (numbers)
        else if ((a == C::JL && isOneOf(b, {C::JL, C::JV, C::H2, C::H3})) || (isOneOf(a, {C::JV, C::H2}) && isOneOf(b, {C::JV, C::JT})) ||
                 (isOneOf(a, {C::JT, C::H3}) && b == C::JT))
            k = Break::None;                                                            // LB26
        else if ((isOneOf(a, {C::JL, C::JV, C::JT, C::H2, C::H3}) && b == C::PO) || (a == C::PR && isOneOf(b, {C::JL, C::JV, C::JT, C::H2, C::H3})))
            k = Break::None;                                                            // LB27
        else if (a == C::AL && b == C::AL) k = Break::None;                             // LB28
        else if (a == C::IS && b == C::AL) k = Break::None;                             // LB29
        else if ((isOneOf(a, {C::AL, C::NU}) && b == C::OP) || (a == C::CP && isOneOf(b, {C::AL, C::NU}))) k = Break::None;  // LB30
        else if (a == C::RI && b == C::RI && riRun % 2 == 1) k = Break::None;           // LB30a: flags in pairs
        else if (isOneOf(a, {C::EB, C::ID}) && b == C::EM) k = Break::None;             // LB30b
        out[i] = k;
        riRun = b == C::RI ? (a == C::RI ? riRun + 1 : 1) : 0;
    }
    return out;
}

std::vector<Line> wrap(const std::string& utf8, float maxWidth, const std::function<float(const std::string&)>& measure) {
    std::vector<Line> lines;
    std::vector<uint32_t> text = shaping::decodeUtf8(utf8);
    std::vector<Break> breaks = opportunities(text);
    // Each character's byte offset (as decodeUtf8 read it: one character per sequence, or per bad byte)
    std::vector<size_t> at(text.size() + 1, utf8.size());
    for (size_t i = 0, b = 0; i < text.size() && b < utf8.size(); ++i) {
        at[i] = b;
        uint8_t c = (uint8_t)utf8[b];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        bool whole = b + len <= utf8.size();
        for (size_t k = 1; k < len && whole; ++k) whole = ((uint8_t)utf8[b + k] & 0xC0) == 0x80;
        b += whole && text[i] != 0xFFFD ? len : (text[i] == 0xFFFD && whole && len > 1 ? len : 1);
    }
    auto isSpace = [&](size_t i) {
        Class c = classOf(text[i]);
        return c == Class::SP || c == Class::BK || c == Class::CR || c == Class::LF || c == Class::NL;
    };
    // [s, e) without its trailing spaces and line breaks
    auto trimmed = [&](size_t s, size_t e) {
        while (e > s && isSpace(e - 1)) --e;
        return e;
    };
    auto width = [&](size_t s, size_t e) { return measure(utf8.substr(at[s], at[e] - at[s])); };
    size_t start = 0;
    while (start < text.size()) {
        size_t lastFit = start, end = text.size();
        bool mandatory = false;
        for (size_t i = start + 1; i <= text.size(); ++i) {
            bool candidate = i == text.size() || breaks[i] != Break::None;
            if (!candidate) continue;
            size_t e = trimmed(start, i);
            if (e > start && width(start, e) > maxWidth) break;   // (this one doesn't fit: the last that did)
            lastFit = i;
            if (i < text.size() && breaks[i] == Break::Mandatory) {
                mandatory = true;
                break;
            }
        }
        if (lastFit == start) {   // not even the first word fits: as many characters as do (one at least)
            size_t i = start + 1;
            while (i < text.size() && (classOf(text[i]) == Class::CM || classOf(text[i]) == Class::ZWJ || classOf(text[i - 1]) == Class::ZWJ))
                ++i;   // (marks and joined emoji stay with their character)
            size_t fit = i;
            for (size_t j = i + 1; j <= text.size(); ++j) {
                if (j < text.size() && (classOf(text[j]) == Class::CM || classOf(text[j]) == Class::ZWJ || classOf(text[j - 1]) == Class::ZWJ)) continue;
                if (width(start, j) > maxWidth) break;
                fit = j;
            }
            lastFit = fit;
        }
        end = lastFit;
        lines.push_back({at[start], at[trimmed(start, end)]});
        start = end;
        // The spaces the line ended at go with it (not the next line's start; after a line break, they're indents)
        if (!mandatory)
            while (start < text.size() && classOf(text[start]) == Class::SP) ++start;
    }
    if (lines.empty()) lines.push_back({0, 0});
    return lines;
}

}  // namespace linebreak
