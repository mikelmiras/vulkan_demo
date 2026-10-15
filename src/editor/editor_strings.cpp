// Editor: the string table window (every key of the project's strings/*.json against every language, missing
// translations marked, a key's text previewed as the game draws it: shaped, right to left, CJK, in the game's font),
// and shaped text in the editor's own panels (GameUI's text drawn into a panel: also button prompts' glyphs).
#include "editor/editor.h"

#include <algorithm>
#include <cctype>

#include "core/project.h"
#include "game/text_shaping.h"
#include "editor/editor_ui.h"
#include "misc/cpp/imgui_stdlib.h"

using namespace editor_ui;

float Editor::drawShapedTextAt(ImVec2 at, const std::string& text, float pixels, float width, const std::string& font,
                               bool rightToLeft) {
    // A canvas scaled so its 36-unit text is `pixels` high, from `at`
    float scale = pixels / 36.f;
    float canvasWidth = std::max(width, 1.f) / scale;
    float h = gameUi_.textBoxHeight(canvasWidth, text, 36.f, font) * scale;
    gameUi_.begin(ImGui::GetWindowDrawList(), at.x, at.y, std::max(width, 1.f), 1080.f * scale, false);
    gameUi_.textBox(0.f, 0.f, canvasWidth, text, 36.f, {}, rightToLeft ? GameUI::Align::Right : GameUI::Align::Left, font);
    gameUi_.end();
    return h;
}

float Editor::drawShapedText(const std::string& text, float pixels, float width, const std::string& font, bool rightToLeft) {
    float h = drawShapedTextAt(ImGui::GetCursorScreenPos(), text, pixels, width, font, rightToLeft);
    ImGui::Dummy(ImVec2(width, std::max(h, pixels)));   // (the space it takes in the panel)
    return h;
}

void Editor::drawStringTables() {
    ImGui::SetNextWindowSize(ImVec2(px(760.f), px(520.f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("String tables", &showStringTables_)) {
        ImGui::End();
        return;
    }
    StringEditor& se = strings_;
    if (!se.loaded) {
        se.tables.load(project::root());
        if (se.tables.languages.empty()) se.tables.addLanguage("en");
        se.loaded = true;
        se.dirty = false;
    }
    StringTables& t = se.tables;
    ImGui::TextWrapped("Texts in each language (strings/<language>.json): UI elements and code show them with {@key}. "
                       "Empty cells fall back to English.");
    // The toolbar: filter, a key or a language to add, saving
    ImGui::SetNextItemWidth(px(160.f));
    ImGui::InputTextWithHint("##filter", "filter keys and texts", &se.filter);
    ImGui::SameLine();
    ImGui::Checkbox("Missing only", &se.missingOnly);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(140.f));
    bool addKey = ImGui::InputTextWithHint("##newkey", "new.key", &se.newKey, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    addKey |= ImGui::SmallButton("Add key");
    if (addKey && !se.newKey.empty() && !t.text.count(se.newKey)) {
        t.text[se.newKey][t.languages.empty() ? "en" : t.languages[0]] = "";
        se.selectedKey = se.newKey;
        se.newKey.clear();
        se.dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(px(60.f));
    bool addLang = ImGui::InputTextWithHint("##newlang", "es", &se.newLanguage, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    addLang |= ImGui::SmallButton("Add language");
    tooltip("A language code: es, fr, ar, he, ja, zh, ... (a new strings/<code>.json)");
    if (addLang && !se.newLanguage.empty()) {
        std::string code = se.newLanguage;
        for (char& c : code) c = (char)std::tolower((unsigned char)c);
        t.addLanguage(code);
        se.newLanguage.clear();
        se.dirty = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!se.dirty);
    if (ImGui::Button("Save")) {
        std::string error;
        if (t.save(project::root(), error)) {
            se.dirty = false;
            previewStringsAt_ = -1.0;   // (the UI preview picks them up)
            setStatus("Saved the string tables (" + std::to_string(t.languages.size()) + " languages)");
        } else {
            setStatus("String tables: " + error);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload")) se.loaded = false;
    // Missing per language
    std::string counts;
    for (const std::string& l : t.languages) {
        size_t m = t.missing(l).size();
        if (m) counts += (counts.empty() ? "" : ", ") + l + ": " + std::to_string(m) + " missing";
    }
    if (!counts.empty()) ImGui::TextColored(ImVec4(1.f, 0.65f, 0.3f, 1.f), "%s", counts.c_str());

    // The table: a row per key, a column per language
    float previewHeight = px(110.f);
    std::string removeKey;
    int columns = (int)t.languages.size() + 2;
    if (columns <= 64 && ImGui::BeginTable("##strings", columns,
                                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInner | ImGuiTableFlags_ScrollY |
                                               ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable,
                                           ImVec2(0, -previewHeight))) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, px(150.f));
        for (const std::string& l : t.languages) ImGui::TableSetupColumn(l.c_str(), ImGuiTableColumnFlags_WidthFixed, px(180.f));
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, px(24.f));
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();
        std::string filter = se.filter;
        for (char& c : filter) c = (char)std::tolower((unsigned char)c);
        for (auto& [key, langs] : t.text) {
            bool missing = false;
            std::string all = key;
            for (const std::string& l : t.languages) {
                auto it = langs.find(l);
                missing = missing || it == langs.end() || it->second.empty();
                if (it != langs.end()) all += " " + it->second;
            }
            if (se.missingOnly && !missing) continue;
            if (!filter.empty()) {
                for (char& c : all) c = (char)std::tolower((unsigned char)c);
                if (all.find(filter) == std::string::npos) continue;
            }
            ImGui::PushID(key.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(key.c_str(), se.selectedKey == key)) se.selectedKey = key;
            for (const std::string& l : t.languages) {
                ImGui::TableNextColumn();
                std::string& value = langs[l];
                bool empty = value.empty();
                if (empty) ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(110, 45, 25, 255));   // (missing here)
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText(("##" + l).c_str(), &value)) se.dirty = true;
                if (empty) ImGui::PopStyleColor();
                if (ImGui::IsItemActive() || ImGui::IsItemFocused()) {
                    se.selectedKey = key;
                    se.selectedLanguage = l;
                }
                if (!ImGui::IsItemActive() && shaping::needsShaping(value)) {   // (as the game draws it, but while typing)
                    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg));
                    float pad = ImGui::GetStyle().FramePadding.x, line = ImGui::GetTextLineHeight();
                    ImGui::PushClipRect(a, b, true);
                    drawShapedTextAt(ImVec2(a.x + pad, a.y + (b.y - a.y - line * 1.2f) * 0.5f), value, line,
                                     b.x - a.x - 2.f * pad, "", StringTables::rightToLeft(value));
                    ImGui::PopClipRect();
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) removeKey = key;
            tooltip("Remove this key (from every language)");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!removeKey.empty()) {
        t.text.erase(removeKey);
        se.dirty = true;
    }
    // The selected text as the game draws it (shaping, direction, fallback fonts), in a font of the project's
    ImGui::Separator();
    auto key = t.text.find(se.selectedKey);
    if (key == t.text.end()) {
        ImGui::TextDisabled("Select a key (or click into a text) to preview it as the game shows it");
    } else {
        std::string lang = se.selectedLanguage.empty() ? (t.languages.empty() ? "en" : t.languages[0]) : se.selectedLanguage;
        const std::string& text = key->second[lang];
        bool rtl = StringTables::rightToLeft(text);
        ImGui::Text("%s  [%s]%s", key->first.c_str(), lang.c_str(), rtl ? "  right to left" : "");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(px(200.f));
        if (ImGui::BeginCombo("##font", se.font.empty() ? "(the game's font)" : se.font.c_str())) {
            if (ImGui::Selectable("(the game's font)", se.font.empty())) se.font.clear();
            std::error_code ec;   // (the project's fonts/ folder)
            for (const auto& entry : std::filesystem::directory_iterator(project::root() / "fonts", ec)) {
                std::string ext = entry.path().extension().u8string();
                for (char& c : ext) c = (char)std::tolower((unsigned char)c);
                if (ext != ".ttf" && ext != ".otf") continue;
                std::string rel = "fonts/" + entry.path().filename().u8string();
                if (ImGui::Selectable(rel.c_str(), rel == se.font)) se.font = rel;
            }
            ImGui::EndCombo();
        }
        tooltip("Letters a font lacks come from the project's fonts/fallback/ and then the system's (Arabic, Hebrew, CJK, ...)");
        if (text.empty()) ImGui::TextDisabled("(no text in %s: the game shows English's)", lang.c_str());
        else drawShapedText(text, px(26.f), ImGui::GetContentRegionAvail().x, se.font, rtl);
    }
    ImGui::End();
}
