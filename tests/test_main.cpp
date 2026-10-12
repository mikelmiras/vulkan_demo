#include "test.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "imgui.h"

namespace test {

namespace {
int g_failures = 0;
std::filesystem::path g_tempDir;
}  // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void fail(const char* file, int line, const std::string& what) {
    ++g_failures;
    std::fprintf(stderr, "    %s:%d: %s\n", std::filesystem::path(file).filename().string().c_str(), line, what.c_str());
}

std::filesystem::path tempPath(const std::string& file) {
    if (g_tempDir.empty()) {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        g_tempDir = std::filesystem::temp_directory_path() / ("vkworld_tests_" + std::to_string(stamp));
        std::filesystem::create_directories(g_tempDir);
    }
    return g_tempDir / file;
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    f << text;
}

ImGuiContextGuard::ImGuiContextGuard() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.IniFilename = nullptr;
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);   // NewFrame() needs a built font atlas
}

ImGuiContextGuard::~ImGuiContextGuard() { ImGui::DestroyContext(); }

}  // namespace test

TextureLibrary& textures() {
    static TextureLibrary lib = buildTextureLibrary();
    return lib;
}

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    {   // (games save settings and saves under the user's data folder: never the real one from tests)
        std::string saves = test::tempPath("saves").u8string();
#if defined(_WIN32)
        _putenv_s("VKWORLD_SAVE_DIR", saves.c_str());
#else
        setenv("VKWORLD_SAVE_DIR", saves.c_str(), 1);
#endif
    }
    int run = 0, failed = 0;
    for (const test::Case& c : test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        ++run;
        int before = test::g_failures;
        std::printf("%-48s", c.name);
        std::fflush(stdout);
        try {
            c.fn();
        } catch (const test::Abort&) {
        } catch (const std::exception& e) {
            test::fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        bool ok = test::g_failures == before;
        failed += !ok;
        std::printf("%s\n", ok ? "ok" : "FAILED");
    }
    std::error_code ec;
    if (!test::g_tempDir.empty()) std::filesystem::remove_all(test::g_tempDir, ec);
    std::printf("\n%d tests, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
