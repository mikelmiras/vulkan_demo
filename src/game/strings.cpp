#include "game/strings.h"

#include <algorithm>

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

std::vector<std::string> StringTable::languages(const std::filesystem::path& root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root / "strings", ec))
        if (entry.path().extension() == ".json") out.push_back(entry.path().stem().u8string());
    std::sort(out.begin(), out.end());
    return out;
}
