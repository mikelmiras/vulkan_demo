// Engine benchmark: builds the stress scene and times the CPU side of a frame (game updates, physics, draw lists).
//   engine_bench [statics] [bodies] [triggers] [ticks]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "imgui.h"
#include "physics/dynamics.h"
#include "physics/physics_world.h"
#include "game/game.h"
#include "render/scene_draw.h"
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
    PhysicsWorld pw;
    double build = timeMs(20, [&] { pw.build(edit, meshes); });
    double find = timeMs(5, [&] {
        for (const Entity& e : edit.entities) { volatile const Entity* f = edit.find(e.parent); (void)f; }
    });
    RaycastHit hit;
    double rays = timeMs(5, [&] {
        for (int i = 0; i < 1000; ++i) pw.raycast({(float)(i % 100) - 50.f, 1.f, -60.f}, {0.f, -0.05f, 1.f}, 200.f, hit);
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
    std::printf("  worldMatrices        %8.3f ms\n", world);
    std::printf("  collectDrawItems     %8.3f ms\n", draw);
    std::printf("  PhysicsWorld::build  %8.3f ms\n", build);
    std::printf("  find(parent) x all   %8.3f ms\n", find);
    std::printf("  1000 raycasts        %8.3f ms\n", rays);
    std::printf("  Game::start          %8.3f ms\n", start);
    std::printf("  fixed update         %8.3f ms avg, %.3f ms worst (budget at 60 Hz: 16.7 ms)\n", total / ticks, worst);
    ImGui::DestroyContext();
    return 0;
}
