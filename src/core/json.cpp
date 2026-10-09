#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

const std::string& Value::asString() const {
    static const std::string empty;
    return type_ == Type::String ? string_ : empty;
}

void Value::readVec3(float out[3]) const {
    if (type_ != Type::Array || array_.size() != 3) return;
    for (int i = 0; i < 3; ++i) out[i] = array_[(size_t)i].asFloat(out[i]);
}

bool Value::has(const std::string& key) const {
    for (const auto& m : object_)
        if (m.first == key) return true;
    return false;
}

const Value& Value::operator[](const std::string& key) const {
    static const Value null;
    for (const auto& m : object_)
        if (m.first == key) return m.second;
    return null;
}

const Value& Value::operator[](size_t index) const {
    static const Value null;
    return index < array_.size() ? array_[index] : null;
}

Value& Value::set(const std::string& key, Value v) {
    type_ = Type::Object;
    for (auto& m : object_)
        if (m.first == key) return m.second = std::move(v);
    object_.emplace_back(key, std::move(v));
    return object_.back().second;
}

Value& Value::push(Value v) {
    type_ = Type::Array;
    array_.push_back(std::move(v));
    return array_.back();
}

// ---------------------------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------------------------
namespace {

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

void writeNumber(std::string& out, double n) {
    if (!std::isfinite(n)) { out += "0"; return; }
    char buf[32];
    if (n == std::floor(n) && std::fabs(n) < 1e15) std::snprintf(buf, sizeof(buf), "%.0f", n);
    else std::snprintf(buf, sizeof(buf), "%.7g", n);
    out += buf;
}

bool isFlatNumberArray(const Value& v) {
    if (!v.isArray() || v.size() > 4) return false;
    for (const Value& e : v.elements())
        if (e.type() != Value::Type::Number) return false;
    return true;
}

void writeValue(std::string& out, const Value& v, int depth) {
    auto newline = [&](int d) {
        out += '\n';
        out.append((size_t)d * 2, ' ');
    };
    switch (v.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
        case Value::Type::Number: writeNumber(out, v.asNumber()); break;
        case Value::Type::String: writeString(out, v.asString()); break;
        case Value::Type::Array: {
            if (v.size() == 0) { out += "[]"; break; }
            bool flat = isFlatNumberArray(v);
            out += '[';
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) out += flat ? ", " : ",";
                if (!flat) newline(depth + 1);
                writeValue(out, v[i], depth + 1);
            }
            if (!flat) newline(depth);
            out += ']';
            break;
        }
        case Value::Type::Object: {
            if (v.size() == 0) { out += "{}"; break; }
            out += '{';
            bool first = true;
            for (const auto& m : v.members()) {
                if (!first) out += ',';
                first = false;
                newline(depth + 1);
                writeString(out, m.first);
                out += ": ";
                writeValue(out, m.second, depth + 1);
            }
            newline(depth);
            out += '}';
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Parser (recursive descent)
// ---------------------------------------------------------------------------------------------
struct Parser {
    const char* p;
    const char* begin;
    std::string error;

    bool fail(const char* msg) {
        if (error.empty()) {
            int line = 1;
            for (const char* c = begin; c < p; ++c) line += *c == '\n';
            error = "line " + std::to_string(line) + ": " + msg;
        }
        return false;
    }
    void skipSpace() {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    }
    bool literal(const char* word) {
        size_t n = std::strlen(word);
        if (std::strncmp(p, word, n) != 0) return false;
        p += n;
        return true;
    }
    static void appendUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool parseHex4(unsigned& cp) {
        cp = 0;
        for (int i = 0; i < 4; ++i, ++p) {
            char c = *p;
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool parseString(std::string& s) {
        if (*p != '"') return fail("expected string");
        ++p;
        for (;;) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\0') return fail("unterminated string");
            if (c != '\\') { s += c; continue; }
            switch (*p++) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!parseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {   // surrogate pair
                        p += 2;
                        unsigned lo;
                        if (!parseHex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(s, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }
    bool parseValue(Value& v, int depth) {
        if (depth > 200) return fail("nested too deeply");
        skipSpace();
        if (*p == '{') {
            ++p;
            v = Value::object();
            skipSpace();
            if (*p == '}') { ++p; return true; }
            for (;;) {
                skipSpace();
                std::string key;
                if (!parseString(key)) return false;
                skipSpace();
                if (*p++ != ':') return fail("expected ':'");
                Value member;
                if (!parseValue(member, depth + 1)) return false;
                v.set(key, std::move(member));
                skipSpace();
                if (*p == ',') { ++p; continue; }
                if (*p == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (*p == '[') {
            ++p;
            v = Value::array();
            skipSpace();
            if (*p == ']') { ++p; return true; }
            for (;;) {
                Value element;
                if (!parseValue(element, depth + 1)) return false;
                v.push(std::move(element));
                skipSpace();
                if (*p == ',') { ++p; continue; }
                if (*p == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (*p == '"') {
            std::string s;
            if (!parseString(s)) return false;
            v = Value(std::move(s));
            return true;
        }
        if (literal("true")) { v = Value(true); return true; }
        if (literal("false")) { v = Value(false); return true; }
        if (literal("null")) { v = Value(); return true; }
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            char* end;
            double n = std::strtod(p, &end);
            if (end == p) return fail("bad number");
            p = end;
            v = Value(n);
            return true;
        }
        return fail("unexpected character");
    }
};

}  // namespace

std::string write(const Value& v) {
    std::string out;
    writeValue(out, v, 0);
    out += '\n';
    return out;
}

bool parse(const std::string& text, Value& out, std::string& error) {
    Parser ps{text.c_str(), text.c_str(), {}};
    Value v;
    if (!ps.parseValue(v, 0)) { error = ps.error; return false; }
    ps.skipSpace();
    if (*ps.p != '\0') { ps.fail("unexpected data after the document"); error = ps.error; return false; }
    out = std::move(v);
    return true;
}

}  // namespace json
