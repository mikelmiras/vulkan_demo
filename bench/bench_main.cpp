// Engine benchmark: builds the stress scene and times the CPU side of a frame (game updates, physics, draw lists).
//   engine_bench [statics] [bodies] [triggers] [ticks] [--json results.json]
// --json also writes the times by name, for tools/check_perf.py (CI's performance regression check)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <thread>

#include "imgui.h"
#include "physics/physics.h"
#include "game/game.h"
#include "game/navigation.h"
#include "render/scene_draw.h"
#include "scene/scene_diff.h"
#include "scene/serialization.h"
#include "scene/stress_scene.h"

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

template <class F> double timeMs(int reps, F&& f) {
    auto t = Clock::now();
    for (int i = 0; i < reps; ++i) f();
    return msSince(t) / reps;
}

}  // namespace

int main(int argc, char** argv) {
    std::string jsonPath;
    {
        int out = 1;
        for (int i = 1; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--json") && i + 1 < argc) jsonPath = argv[++i];
            else argv[out++] = argv[i];
        }
        argc = out;
    }
    StressSettings st;
    if (argc > 1) st.statics = std::atoi(argv[1]);
    if (argc > 2) st.bodies = std::atoi(argv[2]);
    if (argc > 3) st.triggers = std::atoi(argv[3]);
    int ticks = argc > 4 ? std::atoi(argv[4]) : 120;

    ImGui::CreateContext();   // the game reads input through Dear ImGui
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.Fonts->Build();

    MeshLibrary meshes = buildMeshLibrary();
    Scene edit = buildStressScene(st);
    std::printf("stress scene: %zu entities (%d statics, %d bodies, %d triggers), %d ticks\n", edit.entities.size(),
                st.statics, st.bodies, st.triggers, ticks);

    double world = timeMs(20, [&] { volatile size_t n = edit.worldMatrices().size(); (void)n; });
    std::vector<Mat4> worlds = edit.worldMatrices();
    double draw = timeMs(20, [&] { volatile size_t n = collectDrawItems(edit, worlds).size(); (void)n; });
    double copy = timeMs(20, [&] { Scene c = edit; volatile size_t n = c.entities.size(); (void)n; });
    Scene moved = edit;
    moved.entities[moved.entities.size() / 2].transform.position[0] += 1.f;
    double diff = timeMs(5, [&] { volatile size_t n = diffScenes(edit, moved).entities.size(); (void)n; });
    Physics pw;
    auto made = Clock::now();
    pw.step(edit, meshes, 0.f);   // every body made (what Game::start does)
    double build = msSince(made);
    double sync = timeMs(20, [&] { pw.step(edit, meshes, 0.f); });
    double find = timeMs(5, [&] {
        for (const Entity& e : edit.entities) { volatile const Entity* f = edit.find(e.parent); (void)f; }
    });
    RaycastHit hit;
    double rays = timeMs(5, [&] {
        for (int i = 0; i < 1000; ++i) pw.raycast({(float)(i % 100) - 50.f, 1.f, -60.f}, {0.f, -0.05f, 1.f}, 200.f, hit);
    });

    NavGrid nav;
    auto navT = Clock::now();
    nav.bake(edit, meshes, edit.navigation);
    double navBake = msSince(navT);
    std::vector<Vec3> path;
    int found = 0;
    double paths = timeMs(1, [&] {
        for (int i = 0; i < 100; ++i)
            found += nav.findPath({(float)(i % 10) * 8.f - 40.f, 1.f, -40.f}, {(float)(i / 10) * 8.f - 40.f, 1.f, 40.f}, path);
    });

    Scene play = edit;
    Game game;
    auto t0 = Clock::now();
    game.start(play, meshes);
    double start = msSince(t0);
    double total = 0, worst = 0;
    for (int i = 0; i < ticks; ++i) {
        ImGui::NewFrame();
        auto t = Clock::now();
        game.step();
        double ms = msSince(t);
        total += ms;
        if (ms > worst) worst = ms;
        ImGui::EndFrame();
    }

    // Where everything ended up: compare between engine versions to check the simulation didn't change
    double checksum = 0;
    for (const Entity& e : play.entities)
        checksum += (e.transform.position[0] * 1.3 + e.transform.position[1] * 2.7 + e.transform.position[2] * 0.7) * (e.id % 97 + 1);
    std::printf("  state checksum       %.6f (%zu entities)\n", checksum, play.entities.size());
    std::printf("  Scene copy           %8.3f ms\n", copy);
    std::printf("  diffScenes (1 edit)  %8.3f ms\n", diff);
    std::printf("  worldMatrices        %8.3f ms\n", world);
    std::printf("  collectDrawItems     %8.3f ms\n", draw);
    std::printf("  physics: make bodies %8.3f ms\n", build);
    std::printf("  physics: sync scene  %8.3f ms\n", sync);
    std::printf("  find(parent) x all   %8.3f ms\n", find);
    std::printf("  1000 raycasts        %8.3f ms\n", rays);
    std::printf("  Game::start          %8.3f ms\n", start);
    // Changing to the stress scene from a file: all on the main thread, or loaded in the background (then the cost
    // left on the main thread is the frame that takes the loaded scene)
    double syncChange = 0, asyncFrame = 0, asyncTotal = 0;
    {
        TextureLibrary textures = buildTextureLibrary();
        std::string file = (std::filesystem::temp_directory_path() / "vkworld_bench_stress.vkw").string(), err;
        saveScene(file, edit, meshes, textures, err);
        for (bool async : {false, true}) {
            Scene s;
            Game g;
            g.setAssetLibraries(meshes, textures);
            g.setAsyncLoading(async);
            g.start(s, meshes);
            auto t = Clock::now();
            g.loadScene(file);
            if (!async) { syncChange = msSince(t); continue; }
            while (g.loading() && g.loadingProgress() < 1.f) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));   // (done: progress hits 1 just before)
            auto f = Clock::now();
            ImGui::NewFrame();
            g.frame(0.f, false, false);
            ImGui::EndFrame();
            asyncFrame = msSince(f);
            asyncTotal = msSince(t);
        }
    }
    std::printf("  scene change, sync   %8.3f ms\n", syncChange);
    std::printf("  scene change, async  %8.3f ms in the frame that takes it (%.0f ms in all)\n", asyncFrame, asyncTotal);
    std::printf("  navigation bake      %8.3f ms (%zu cells)\n", navBake, nav.cellCount());
    std::printf("  100 paths            %8.3f ms (%d found)\n", paths, found);
    std::printf("  fixed update         %8.3f ms avg, %.3f ms worst (budget at 60 Hz: 16.7 ms)\n", total / ticks, worst);
    if (!jsonPath.empty()) {
        const std::pair<const char*, double> metrics[] = {
            {"scene_copy", copy}, {"diff_scenes", diff}, {"world_matrices", world}, {"draw_items", draw},
            {"physics_make_bodies", build}, {"physics_sync", sync}, {"raycasts_1000", rays}, {"game_start", start},
            {"scene_change_sync", syncChange}, {"navigation_bake", navBake}, {"paths_100", paths},
            {"fixed_update_avg", total / ticks}};
        std::ofstream out(jsonPath, std::ios::trunc);
        out << "{\n  \"entities\": " << play.entities.size() << ",\n  \"ms\": {";
        for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); ++i)
            out << (i ? "," : "") << "\n    \"" << metrics[i].first << "\": " << metrics[i].second;
        out << "\n  }\n}\n";
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", jsonPath.c_str());
            return 1;
        }
    }
    ImGui::DestroyContext();
    return 0;
}
