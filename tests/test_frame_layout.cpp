// The frame's uniforms, the objects' and the decals' data: FrameUBO, InstanceData and GpuDecal
// (render/renderer_internal.h) must match shaders/common.glsl's Frame block and Instance struct, and scene.frag's
// GpuDecal, field for field (names, order, sizes); both files are read
// and their declarations compared, so a field added to one alone fails. And packColor's colours.
#include "test.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "render/renderer_internal.h"

using namespace render_detail;

namespace {
struct Field {
    std::string name;
    int floats;
};

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// An array size: numbers and named constants multiplied ("4 * 9", "kShadowViews")
int arraySize(const std::string& expr) {
    int n = 1;
    std::stringstream ss(expr);
    std::string tok;
    while (std::getline(ss, tok, '*')) {
        tok.erase(0, tok.find_first_not_of(' '));
        tok.erase(tok.find_last_not_of(' ') + 1);
        n *= tok == "kShadowViews" ? kShadowViews : std::atoi(tok.c_str());
    }
    return n;
}

// The fields between `open` and the closing "}" of the block: one declaration a line ("type a[2], b;"), comments
// dropped; each field's size in floats (from its type's and its array dimensions)
std::vector<Field> fields(const std::string& text, const std::string& open, int (*typeFloats)(const std::string&)) {
    std::vector<Field> out;
    size_t at = text.find(open);
    if (at == std::string::npos) return out;
    std::stringstream lines(text.substr(at + open.size()));
    std::string line;
    while (std::getline(lines, line)) {
        line = line.substr(0, line.find("//"));
        size_t start = line.find_first_not_of(' ');
        if (start == std::string::npos) continue;
        if (line[start] == '}') break;
        size_t semi = line.find(';');
        if (semi == std::string::npos) continue;
        size_t space = line.find(' ', start);
        int base = typeFloats(line.substr(start, space - start));
        std::stringstream decls(line.substr(space + 1, semi - space - 1));
        std::string d;
        while (std::getline(decls, d, ',')) {
            Field f;
            int n = base;
            size_t b = d.find('[');
            f.name = d.substr(0, b);
            f.name.erase(0, f.name.find_first_not_of(' '));
            f.name.erase(f.name.find_last_not_of(' ') + 1);
            while (b != std::string::npos) {
                size_t e = d.find(']', b);
                n *= arraySize(d.substr(b + 1, e - b - 1));
                b = d.find('[', e);
            }
            f.floats = n;
            out.push_back(f);
        }
    }
    return out;
}

int glslFloats(const std::string& type) { return type == "mat4" ? 16 : type == "vec4" ? 4 : -1000; }   // (vec4s alone: std140 has no padding then)
int cppFloats(const std::string& type) { return type == "Mat4" ? 16 : type == "float" ? 1 : -1000; }


// Each field of the shaders' block against the C++ struct's; the struct's size the fields' sum (no padding: vec4s and
// mat4s alone, std140's and std430's layouts alike then)
void compareLayouts(const char* glslOpen, const char* cppOpen, size_t cppSize, const char* shader = "common.glsl") {
    std::string root = std::string(VKWORLD_TEST_ASSETS) + "/../../";
    std::vector<Field> glsl = fields(readFile(root + "shaders/" + shader), glslOpen, glslFloats);
    std::vector<Field> cpp = fields(readFile(root + "src/render/renderer_internal.h"), cppOpen, cppFloats);
    REQUIRE(glsl.size() > 5);
    CHECK(glsl.size() == cpp.size());
    int total = 0;
    for (size_t i = 0; i < glsl.size() && i < cpp.size(); ++i) {
        if (glsl[i].name != cpp[i].name || glsl[i].floats != cpp[i].floats) {
            test::fail(__FILE__, __LINE__, std::string(cppOpen) + " field " + std::to_string(i) + ": " + glsl[i].name + " (" +
                                               std::to_string(glsl[i].floats) + ") in the shaders, " + cpp[i].name + " (" +
                                               std::to_string(cpp[i].floats) + ") in C++");
            break;
        }
        CHECK(glsl[i].floats > 0);
        total += glsl[i].floats;
    }
    CHECK(cppSize == size_t(total) * sizeof(float));
}
}  // namespace

TEST(frame_uniforms_match_the_shaders) {
    compareLayouts("uniform Frame {", "struct FrameUBO {", sizeof(FrameUBO));
    compareLayouts("struct Instance {", "struct InstanceData {", sizeof(InstanceData));
    compareLayouts("struct GpuDecal {", "struct GpuDecal {", sizeof(GpuDecal), "scene.frag");
}

// The post passes' push constants (renderer.cpp's post layout: 96 bytes, six vec4s) hold every post shader's block
TEST(post_push_constants_fit) {
    std::string root = std::string(VKWORLD_TEST_ASSETS) + "/../../shaders/";
    for (const char* shader : {"ssao.glsl", "composite.frag", "shafts.glsl", "camera_fx.glsl", "bloom_down.frag", "bloom_up.frag",
                               "luminance.frag", "adapt.frag", "ssr_capture.glsl"}) {
        std::string text = readFile(root + shader);
        size_t at = text.find("layout(push_constant) uniform ");
        REQUIRE(at != std::string::npos);
        size_t open = text.find('{', at);
        std::vector<Field> f = fields(text, text.substr(at, open - at + 1), glslFloats);
        int floats = 0;
        for (const Field& x : f) floats += x.floats;
        if (floats <= 0 || floats > 24) test::fail(__FILE__, __LINE__, std::string(shader) + ": " + std::to_string(floats) + " floats");
    }
}

TEST(packed_colours) {
    CHECK(packColor({0.f, 0.f, 0.f}) == 0.f);
    CHECK(packColor({1.f, 1.f, 1.f}) == 16777215.f);   // (2^24 - 1: whole in a float)
    CHECK(packColor({1.f, 0.f, 0.f}) == 255.f * 65536.f);
    CHECK(packColor({0.f, 1.f, 0.f}) == 255.f * 256.f);
    CHECK(packColor({2.f, -1.f, 0.5f}) == 255.f * 65536.f + 128.f);   // (clamped; rounded)
    // unpackColor's sums (common.glsl), back to the bytes
    float p = packColor({1.f, 0.35f, 0.2f});
    float r = std::floor(p / 65536.f), g = std::fmod(std::floor(p / 256.f), 256.f), b = std::fmod(p, 256.f);
    CHECK(r == 255.f && g == 89.f && b == 51.f);
}
