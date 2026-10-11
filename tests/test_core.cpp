// core/: JSON and vector maths
#include "test.h"

#include <fstream>
#include <thread>

#include "core/log.h"
#include "core/profiler.h"

#include "core/json.h"

TEST(json_roundtrip_escapes_and_unicode) {
    json::Value v;
    std::string err;
    // A plain string (not raw) outside the macro: MSVC's legacy preprocessor can't stringize raw strings
    const char* text = "{\"s\":\"a\\\"b\\\\c\\n\\u00e9\\ud83d\\ude00\",\"n\":-1.5e2,\"t\":true,\"z\":null,\"a\":[]}";
    REQUIRE(json::parse(text, v, err));
    CHECK(v["s"].asString() == "a\"b\\c\n\xc3\xa9\xf0\x9f\x98\x80");
    CHECK_NEAR(v["n"].asNumber(), -150.0, 1e-9);
    CHECK(v["t"].asBool());
    CHECK(v["z"].isNull());
    CHECK(v["a"].isArray() && v["a"].size() == 0);
    json::Value back;
    REQUIRE(json::parse(json::write(v), back, err));
    CHECK(json::write(back) == json::write(v));
}

TEST(json_missing_keys_give_defaults) {
    json::Value v;
    std::string err;
    REQUIRE(json::parse("{\"x\": 1}", v, err));
    CHECK(v["nope"].isNull());
    CHECK(v["nope"]["deeper"].asInt(7) == 7);
    CHECK(v["x"].asString().empty());   // wrong type -> default
    float vec[3] = {1, 2, 3};
    v["x"].readVec3(vec);                // not an array -> unchanged
    CHECK(vec[0] == 1 && vec[2] == 3);
}

TEST(json_rejects_malformed_input) {
    json::Value v;
    std::string err;
    CHECK(!json::parse("{\"a\": [1, 2,", v, err));
    CHECK(!err.empty());
    CHECK(!json::parse("{\"a\": 1} trailing", v, err));
    CHECK(!json::parse("{'a': 1}", v, err));
    CHECK(!json::parse("", v, err));
}

TEST(json_numbers_keep_precision) {
    json::Value v = json::Value::object();
    v.set("big", 123456789.0);
    v.set("small", 0.1f);
    v.set("int", 42);
    json::Value back;
    std::string err;
    REQUIRE(json::parse(json::write(v), back, err));
    CHECK_NEAR(back["big"].asNumber(), 123456789.0, 1e-6);
    CHECK_NEAR(back["small"].asFloat(), 0.1f, 1e-7);
    CHECK(back["int"].asInt() == 42);
}

TEST(vecmath_inverse_affine) {
    Mat4 m = translate({1, 2, 3}) * rotateY(0.7f);
    m.m[0] *= 2.f; m.m[1] *= 2.f; m.m[2] *= 2.f;   // scale X
    Mat4 id = inverseAffine(m) * m;
    for (int i = 0; i < 16; ++i) CHECK_NEAR(id.m[i], (i % 5 == 0) ? 1.f : 0.f, 1e-5);
}

// The frame profiler: nested scopes per frame, summarised as a tree; nothing outside frames or off the frame thread
TEST(profiler_scopes) {
    { PROFILE_SCOPE("Ignored"); }   // no frame yet
    for (int f = 0; f < 3; ++f) {
        profiler::beginFrame();
        {
            PROFILE_SCOPE("Update");
            { PROFILE_SCOPE("Physics"); }
            if (f == 2) { PROFILE_SCOPE("Rare"); }
            std::thread other([] { PROFILE_SCOPE("Other thread"); });
            other.join();
        }
        { PROFILE_SCOPE("Render"); { PROFILE_SCOPE("Physics"); } }   // same name, another place
        profiler::endFrame();
    }
    const profiler::Frame& last = profiler::history().back();
    REQUIRE(last.scopes.size() == 5);
    CHECK(std::string(last.scopes[0].name) == "Update" && last.scopes[0].depth == 0);
    CHECK(std::string(last.scopes[1].name) == "Physics" && last.scopes[1].depth == 1);
    CHECK(last.scopes[0].ms >= last.scopes[1].ms);
    std::vector<profiler::Summary> s = profiler::summarize();
    std::vector<std::string> order;
    for (const profiler::Summary& x : s) order.push_back(std::to_string(x.depth) + x.name);
    // Children right after their parents, even ones first seen in a later frame
    std::vector<std::string> expected = {"0Update", "1Physics", "1Rare", "0Render", "1Physics"};
    CHECK(order == expected);
    for (const profiler::Summary& x : s) CHECK(x.name != "Ignored" && x.name != "Other thread");
}

// The log file: lines with the time, flushed as written; a new run keeps the previous log as .1
TEST(log_file) {
    auto path = test::tempPath("logs/run.log");
    REQUIRE(logfile::open(path));
    logfile::write("first run");
    logLine("an error: %d", 42);   // (to stderr as well)
    auto read = [](const std::filesystem::path& p) {
        std::ifstream in(p);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    CHECK(read(path).find("first run") != std::string::npos);   // there already: no need to close first
    CHECK(read(path).find("an error: 42") != std::string::npos);
    REQUIRE(logfile::open(path));
    logfile::write("second run");
    std::filesystem::path old = path;
    old += ".1";
    CHECK(read(path).find("first run") == std::string::npos);
    CHECK(read(old).find("first run") != std::string::npos);
    logfile::close();
    logfile::write("not written");
    CHECK(read(path).find("not written") == std::string::npos);
}
