// audio/: the engine without an output device (mixed into a buffer), and the game's use of it
#include "test.h"

#include <cmath>

#include "audio/audio.h"
#include "core/json.h"
#include "game/game.h"
#include "imgui.h"
#include "scene/serialization.h"

namespace {

// RMS level of the next `frames` stereo frames
float level(AudioEngine& a, uint32_t frames) {
    std::vector<float> buf(frames * 2);
    if (!a.readFrames(buf.data(), frames)) return -1.f;
    double sum = 0;
    for (float s : buf) sum += (double)s * s;
    return (float)std::sqrt(sum / buf.size());
}

}  // namespace

TEST(audio_builtin_clips_play_and_finish) {
    AudioEngine a;
    REQUIRE(a.init(false));
    CHECK(level(a, 1024) < 1e-6f);   // silence before anything plays
    for (const std::string& clip : AudioEngine::builtinClips()) {
        AudioEngine::Sound s = a.play(clip, {});
        CHECK(s != 0);
        CHECK(level(a, 2048) > 0.01f);   // something comes out
        a.stop(s);
    }
    AudioEngine::Sound beep = a.play("Beep", {});
    level(a, 44100 / 2);   // 0.5 s: past its 0.15 s
    a.update();
    CHECK(!a.playing(beep));
    CHECK(a.activeSounds() == 0);   // finished one-shots are freed
    AudioEngine::PlayParams loop;
    loop.loop = true;
    AudioEngine::Sound hum = a.play("Hum", loop);
    level(a, 44100 * 3);   // longer than the clip
    a.update();
    CHECK(a.playing(hum));   // loops keep going
    CHECK(a.play(test::tempPath("missing.wav").string(), {}) == 0);
    CHECK(!a.error().empty());
}

TEST(audio_spatial_sounds_fade_with_distance) {
    auto loudnessAt = [](float distance) {
        AudioEngine a;
        a.init(false);
        a.setListener({0, 0, 0}, {0, 0, -1});
        AudioEngine::PlayParams p;
        p.spatial = true;
        p.loop = true;
        p.position = {0, 0, -distance};
        p.minDistance = 1.f;
        p.maxDistance = 100.f;
        a.play("Hum", p);
        return level(a, 8192);
    };
    float near = loudnessAt(1.f), far = loudnessAt(30.f);
    CHECK(near > 0.01f);
    CHECK(far < near * 0.2f);
}

TEST(audio_in_game) {
    test::ImGuiContextGuard imgui;
    AudioEngine audio;
    REQUIRE(audio.init(false));
    MeshLibrary meshes = buildMeshLibrary();
    Scene s = buildDefaultScene();
    Game g;
    g.setAudio(&audio);
    g.start(s, meshes);
    CHECK(audio.activeSounds() >= 1);   // the centrepiece hum started with Play
    unsigned long long before = audio.playCount();
    // Walk into a gem: the pickup sound plays
    Entity* player = s.findByName("Player");
    Mat4 gem = s.worldMatrix(s.findByName("Gem.002")->id);
    player->transform.position[0] = gem.m[12];
    player->transform.position[2] = gem.m[14];
    for (int i = 0; i < 3; ++i) {
        ImGui::NewFrame();
        g.frame(1.f / 60.f, true, true);
        ImGui::EndFrame();
    }
    CHECK(audio.playCount() > before);
    g.stop();               // stops the looping hum
    level(audio, 44100);    // let the pickup sound finish
    audio.update();
    CHECK(audio.activeSounds() == 0);
}

TEST(audio_source_saved_with_scene) {
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    AudioSource a;
    a.clip = "Thud";
    a.volume = 0.4f;
    a.loop = false;
    a.playOnImpact = true;
    a.maxDistance = 12.f;
    s.create("Speaker").audioSource = a;
    std::string err;
    auto path = test::tempPath("audio.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), err));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, textures(), err));
    CHECK(json::write(entityToJson(loaded.entities[0], meshes, textures())) ==
          json::write(entityToJson(s.entities[0], meshes, textures())));
}
