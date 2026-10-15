// Text in the player's language: string tables in the project's strings/ folder, one JSON file per language
// (strings/en.json, strings/es.json, ...), each mapping keys to text. Nested objects make dotted keys:
//
//   { "menu": { "resume": "Reanudar", "quit": "Salir" }, "hud.score": "Puntos" }
//
// A key missing in the language comes from English (en.json), then the fallback the code gives (or the key).
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

class StringTable {
public:
    // Loads strings/<language>.json (and en.json under it) from the project folder; false if neither exists
    bool load(const std::filesystem::path& projectRoot, const std::string& language);
    bool loadJson(const std::string& languageJson, const std::string& englishJson = "");   // (from text: tests)
    const std::string& language() const { return language_; }
    // The text for `key`; `fallback` (or the key itself) when no table has it
    std::string get(const std::string& key, const std::string& fallback = "") const;
    bool has(const std::string& key) const { return strings_.count(key) > 0; }
    // "Score: {@hud.score}" -> "Score: Puntos": every {@key} replaced by its text
    std::string translate(const std::string& text) const;
    // The languages a project has string tables for ("en", "es", ...)
    static std::vector<std::string> languages(const std::filesystem::path& projectRoot);

private:
    std::string language_ = "en";
    std::map<std::string, std::string> strings_;
};

// Every language's string table at once, as the editor's string table window edits them: keys (dotted, flat) and
// each language's text. Saved back nested by the dots ("menu.resume" -> {"menu": {"resume": ...}}), keys sorted;
// a key that is also another's prefix ("menu" and "menu.resume") stays flat.
struct StringTables {
    std::vector<std::string> languages;   // English first, then by code
    std::map<std::string, std::map<std::string, std::string>> text;   // key -> language -> text

    bool load(const std::filesystem::path& projectRoot);   // false if there's no strings/ folder
    bool save(const std::filesystem::path& projectRoot, std::string& error) const;
    std::string toJson(const std::string& language) const;   // a language's file
    void addLanguage(const std::string& code);
    std::vector<std::string> missing(const std::string& language) const;   // keys with no text in it
    static bool rightToLeft(const std::string& text);   // has Hebrew or Arabic letters (for the preview's alignment)
};
