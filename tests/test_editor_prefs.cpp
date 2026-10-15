// editor/editor_prefs: the editor's settings kept between sessions
#include "test.h"

#include <fstream>

#include "core/json.h"
#include "editor/editor_prefs.h"

TEST(editor_prefs_roundtrip) {
    EditorPrefs p;
    p.tool = 3;
    p.snap = true;
    p.snapRotate = 45.f;
    p.showAssets = false;
    p.cameraSpeed = 30.f;
    p.maximizeOnPlay = true;
    p.theme = 1;   // (high contrast)
    p.recentProjects = {"/games/a", "/games/b"};
    auto file = test::tempPath("prefs/editor_prefs.json");
    REQUIRE(editor_prefs::save(file, p));
    EditorPrefs back;
    REQUIRE(editor_prefs::load(file, back));
    CHECK(back == p);
    CHECK(back.theme == 1);
    CHECK(back.tool == 3 && back.snapRotate == 45.f && !back.showAssets && back.recentProjects.size() == 2);
    CHECK(!std::filesystem::exists(file.string() + ".tmp"));

    EditorPrefs untouched;
    CHECK(!editor_prefs::load(test::tempPath("prefs/missing.json"), untouched));
    CHECK(untouched == EditorPrefs{});
}

// A hand-edited or older file: unknown keys ignored, missing or nonsense values back to defaults
TEST(editor_prefs_bad_values) {
    json::Value v;
    std::string err;
    REQUIRE(json::parse("{\"tool\": 9, \"snapMove\": -1, \"cameraFov\": 500, \"showOutliner\": \"yes\", "
                        "\"recentProjects\": [\"/x\", 4, \"\"], \"somethingNew\": 1}", v, err));
    EditorPrefs p = EditorPrefs::fromJson(v);
    CHECK(p.tool == 3);
    CHECK(p.snapMove == EditorPrefs{}.snapMove);
    CHECK(p.cameraFov == 120.f);
    CHECK(p.showOutliner);
    CHECK(p.recentProjects == std::vector<std::string>{"/x"});

    auto file = test::tempPath("prefs/other.json");
    std::ofstream(file) << "{\"format\": \"something-else\"}";
    CHECK(!editor_prefs::load(file, p));
}

TEST(editor_prefs_recent_list) {
    std::vector<std::string> list;
    for (int i = 0; i < 12; ++i) editor_prefs::addRecent(list, "/p" + std::to_string(i));
    CHECK(list.size() == EditorPrefs::kMaxRecent);
    CHECK(list.front() == "/p11");
    editor_prefs::addRecent(list, "/p8");   // already there: moved to the front, not doubled
    CHECK(list.front() == "/p8" && list[1] == "/p11" && list.size() == EditorPrefs::kMaxRecent);
    editor_prefs::addRecent(list, "");
    CHECK(list.front() == "/p8");
}
