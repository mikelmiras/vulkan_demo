#include "game/strings.h"

#include <algorithm>
#include <fstream>
#include <set>

#include "core/json.h"
#include "core/vfs.h"

namespace {

void flatten(const json::Value& v, const std::string& prefix, std::map<std::string, std::string>& out) {
    for (const auto& [k, child] : v.members()) {
        std::string key = prefix.empty() ? k : prefix + "." + k;
        if (child.isObject()) flatten(child, key, out);
        else if (child.type() == json::Value::Type::String) out[key] = child.asString();
    }
}

bool parseInto(const std::string& text, std::map<std::string, std::string>& out) {
    json::Value v;
    std::string error;
    if (text.empty() || !json::parse(text, v, error) || !v.isObject()) return false;
    flatten(v, "", out);
    return true;
}

}  // namespace

bool StringTable::loadJson(const std::string& languageJson, const std::string& englishJson) {
    strings_.clear();
    bool any = parseInto(englishJson, strings_);   // English first: the language's own strings replace it
    std::map<std::string, std::string> own;
    if (parseInto(languageJson, own)) {
        any = true;
        for (auto& [k, v] : own) strings_[k] = v;
    }
    return any;
}

bool StringTable::load(const std::filesystem::path& root, const std::string& language) {
    language_ = language.empty() ? "en" : language;
    std::string lang, en;
    vfs::read(root / "strings" / (language_ + ".json"), lang);
    if (language_ != "en") vfs::read(root / "strings" / "en.json", en);
    return loadJson(lang, en);
}

std::string StringTable::get(const std::string& key, const std::string& fallback) const {
    auto it = strings_.find(key);
    if (it != strings_.end()) return it->second;
    return fallback.empty() ? key : fallback;
}

std::string StringTable::translate(const std::string& text) const {
    if (text.find("{@") == std::string::npos) return text;
    std::string out;
    size_t at = 0;
    while (at < text.size()) {
        size_t open = text.find("{@", at);
        size_t close = open == std::string::npos ? std::string::npos : text.find('}', open);
        if (close == std::string::npos) {
            out += text.substr(at);
            break;
        }
        out += text.substr(at, open - at);
        out += get(text.substr(open + 2, close - open - 2));
        at = close + 1;
    }
    return out;
}

bool StringTables::load(const std::filesystem::path& root) {
    languages.clear();
    text.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(root / "strings", ec)) return false;
    for (const std::string& lang : StringTable::languages(root)) {
        std::string json;
        std::map<std::string, std::string> flat;
        vfs::read(root / "strings" / (lang + ".json"), json);
        parseInto(json, flat);
        addLanguage(lang);
        for (auto& [k, v] : flat) text[k][lang] = v;
    }
    return true;
}

void StringTables::addLanguage(const std::string& code) {
    if (code.empty() || std::find(languages.begin(), languages.end(), code) != languages.end()) return;
    languages.push_back(code);
    std::sort(languages.begin(), languages.end(), [](const std::string& a, const std::string& b) {
        return a == "en" ? b != "en" : b == "en" ? false : a < b;
    });
}

std::string StringTables::toJson(const std::string& language) const {
    std::set<std::string> keys;
    for (const auto& [k, langs] : text)
        if (langs.count(language)) keys.insert(k);
    json::Value root = json::Value::object();
    auto prefixOfAnother = [&](const std::string& k) {
        auto it = keys.upper_bound(k);
        return it != keys.end() && it->rfind(k + ".", 0) == 0;
    };
    for (const std::string& k : keys) {
        const std::string& value = text.at(k).at(language);
        // Nested along the dots, unless a part is a text itself (or this key is another's prefix): then flat
        json::Value* at = &root;
        std::vector<std::string> parts;
        for (size_t from = 0;;) {
            size_t dot = k.find('.', from);
            parts.push_back(k.substr(from, dot == std::string::npos ? std::string::npos : dot - from));
            if (dot == std::string::npos) break;
            from = dot + 1;
        }
        bool nest = !prefixOfAnother(k) && std::none_of(parts.begin(), parts.end(), [](const std::string& p) { return p.empty(); });
        for (size_t i = 0; nest && i + 1 < parts.size(); ++i) {
            const json::Value& child = (*at)[parts[i]];
            if (!child.isNull() && !child.isObject()) nest = false;
        }
        if (!nest) {
            root.set(k, value);
            continue;
        }
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            json::Value* next = nullptr;
            for (auto& [name, child] : at->members())
                if (name == parts[i]) next = &child;
            at = next ? next : &at->set(parts[i], json::Value::object());
        }
        at->set(parts.back(), value);
    }
    return json::write(root);
}

bool StringTables::save(const std::filesystem::path& root, std::string& error) const {
    std::error_code ec;
    std::filesystem::create_directories(root / "strings", ec);
    for (const std::string& lang : languages) {
        std::filesystem::path file = root / "strings" / (lang + ".json"), part = file;
        part += ".part";
        {
            std::ofstream f(part, std::ios::binary);
            if (!(f << toJson(lang))) {
                error = "cannot write " + file.u8string();
                return false;
            }
        }
        std::filesystem::rename(part, file, ec);
        if (ec) {
            error = "cannot write " + file.u8string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

std::vector<std::string> StringTables::missing(const std::string& language) const {
    std::vector<std::string> out;
    for (const auto& [k, langs] : text) {
        auto it = langs.find(language);
        if (it == langs.end() || it->second.empty()) out.push_back(k);
    }
    return out;
}

bool StringTables::rightToLeft(const std::string& s) {
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xD6 || c == 0xD7 || c == 0xD8 || c == 0xD9 || c == 0xDA || c == 0xDB) return true;   // U+0580-06FF
    }
    return false;
}

std::vector<std::string> StringTable::languages(const std::filesystem::path& root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root / "strings", ec))
        if (entry.path().extension() == ".json") out.push_back(entry.path().stem().u8string());
    std::sort(out.begin(), out.end());
    return out;
}
