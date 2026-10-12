// audio/: the engine without an output device (mixed into a buffer), and the game's use of it
#include "test.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

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

namespace {

// A mono 16-bit WAV of a sine
std::string writeTone(const std::string& name, float hz, float seconds) {
    const uint32_t rate = 44100, n = (uint32_t)(seconds * rate);
    std::string path = test::tempPath(name).string();
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + n * 2);
    out.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    out.write("data", 4);
    u32(n * 2);
    for (uint32_t i = 0; i < n; ++i) u16((uint16_t)(int16_t)(12000.f * std::sin(2.f * PI * hz * (float)i / rate)));
    return path;
}

int zeroCrossings(AudioEngine& a, uint32_t frames) {
    std::vector<float> buf(frames * 2);
    a.readFrames(buf.data(), frames);
    int n = 0;
    for (uint32_t i = 1; i < frames; ++i) n += (buf[i * 2] >= 0.f) != (buf[(i - 1) * 2] >= 0.f);
    return n;
}

}  // namespace

// Buses have their own volumes; ducking lowers one for a while and brings it back
TEST(audio_buses_and_ducking) {
    AudioEngine a;
    REQUIRE(a.init(false));
    AudioEngine::PlayParams loop;
    loop.loop = true;
    a.play("Hum", loop);
    float full = level(a, 4096);
    a.setBusVolume(AudioBus::Effects, 0.5f);
    a.update(0.f);
    CHECK_NEAR(level(a, 4096) / full, 0.5, 0.05);
    a.setBusVolume(AudioBus::Music, 0.f);   // another bus: no change here
    CHECK_NEAR(level(a, 4096) / full, 0.5, 0.05);
    a.setBusVolume(AudioBus::Effects, 1.f);
    a.duck(AudioBus::Effects, 0.2f, 1.f, 0.1f);
    a.update(0.5f);   // well into it
    CHECK_NEAR(a.busLevel(AudioBus::Effects), 0.2, 1e-4);
    CHECK_NEAR(level(a, 4096) / full, 0.2, 0.05);
    a.update(0.6f);   // over: easing back
    a.update(0.2f);
    CHECK_NEAR(a.busLevel(AudioBus::Effects), 1.0, 1e-4);
}

// Music streams (decoded as it plays, not cached) and cross-fades into the next track
TEST(audio_music_streams_and_crossfades) {
    AudioEngine a;
    REQUIRE(a.init(false));
    std::string one = writeTone("tune1.wav", 440.f, 1.f), two = writeTone("tune2.wav", 660.f, 1.f);
    size_t cached = a.decodedClips();
    AudioEngine::Sound m1 = a.playMusic(one, 0.f);
    REQUIRE(m1);
    CHECK(a.streaming(m1));
    CHECK(a.decodedClips() == cached);
    CHECK(level(a, 4096) > 0.05f);
    level(a, 44100 * 2);   // longer than the file: it loops
    a.update(0.f);
    CHECK(a.playing(m1));
    AudioEngine::Sound m2 = a.playMusic(two, 0.5f);
    CHECK(a.music() == m2 && m2 != m1);
    CHECK(!a.playing(m1));   // fading out (no longer counted as playing) ...
    level(a, 44100);
    a.update(1.f);
    CHECK(a.activeSounds() == 1);   // ... and gone after its fade
    a.stopMusic(0.f);
    CHECK(a.activeSounds() == 0);
    // A one-off sound can stream too
    AudioEngine::PlayParams p;
    p.stream = true;
    AudioEngine::Sound s = a.play(one, p);
    CHECK(a.streaming(s));
    CHECK(level(a, 2048) > 0.05f);
}

// The reverb leaves a tail after a short sound; Doppler raises the pitch of something coming closer
TEST(audio_reverb_and_doppler) {
    auto tail = [](float wet) {
        AudioEngine a;
        a.init(false);
        a.setReverb(wet, 0.8f, 0.3f);
        a.play("Beep", {});   // 0.15 s
        level(a, 44100 / 4);
        return level(a, 44100 / 4);   // 0.25..0.5 s: after it
    };
    CHECK(tail(0.f) < 1e-5f);
    CHECK(tail(0.6f) > 1e-3f);

    std::string tone = writeTone("tone.wav", 440.f, 2.f);
    auto crossings = [&](float speed) {
        AudioEngine a;
        a.init(false);
        a.setListener({0, 0, 0}, {0, 0, -1});
        AudioEngine::PlayParams p;
        p.spatial = true;
        p.loop = true;
        p.position = {0, 0, -20};
        p.velocity = {0, 0, speed};   // towards the listener
        p.minDistance = 1.f;
        p.maxDistance = 100.f;
        a.play(tone, p);
        zeroCrossings(a, 4410);
        return zeroCrossings(a, 44100 / 2);
    };
    int still = crossings(0.f), closing = crossings(120.f);
    CHECK_NEAR(still, 440, 10);   // 440 Hz: 2 crossings a cycle over half a second
    CHECK((float)closing / (float)still > 1.3f);
}

// In the game: reverb zones around the listener, sources on their buses, saved with the scene
TEST(audio_zones_and_buses_in_game) {
    test::ImGuiContextGuard imgui;
    MeshLibrary meshes = buildMeshLibrary();
    AudioEngine audio;
    REQUIRE(audio.init(false));
    Scene s;
    Entity& cam = s.create("Camera");
    cam.camera = Camera{};
    Entity& hall = s.create("Hall");
    hall.transform.position[0] = 100.f;
    hall.reverbZone = ReverbZone{};
    hall.reverbZone->radius = 10.f;
    hall.reverbZone->blend = 2.f;
    hall.reverbZone->wet = 0.5f;
    Entity& radio = s.create("Radio");
    radio.audioSource = AudioSource{};
    radio.audioSource->clip = "Hum";
    radio.audioSource->bus = "ambience";
    radio.audioSource->spatial = false;   // (heard the same everywhere)
    Game g;
    g.setAudio(&audio);
    g.start(s, meshes);
    auto frame = [&] {
        ImGui::NewFrame();
        g.frame(1.f / 60.f, true, true);
        ImGui::EndFrame();
    };
    frame();
    CHECK_NEAR(audio.reverbWet(), 0.0, 1e-6);   // outside the hall
    s.findByName("Camera")->transform.position[0] = 91.f;   // 9 m in: halfway through its 2 m edge
    frame();
    CHECK_NEAR(audio.reverbWet(), 0.25, 0.02);
    s.findByName("Camera")->transform.position[0] = 100.f;
    frame();
    CHECK_NEAR(audio.reverbWet(), 0.5, 1e-4);
    float on = level(audio, 4096);
    audio.setBusVolume(AudioBus::Ambience, 0.f);
    audio.update(0.f);
    CHECK(on > 0.01f);
    level(audio, 44100 * 3);                 // (the hall's reverb tail rings on for a while)
    CHECK(level(audio, 4096) < on * 0.05f);   // the radio was on the ambience bus

    std::string error, path = test::tempPath("audio_zones.vkw").string();
    REQUIRE(saveScene(path, s, meshes, textures(), error));
    Scene loaded;
    REQUIRE(loadScene(path, loaded, meshes, textures(), error));
    REQUIRE(loaded.findByName("Hall")->reverbZone);
    CHECK_NEAR(loaded.findByName("Hall")->reverbZone->wet, 0.5, 1e-6);
    CHECK(loaded.findByName("Radio")->audioSource->bus == "ambience");
}

namespace {

// The next frames, as separate left / right channels
void channels(AudioEngine& a, uint32_t frames, std::vector<float>& l, std::vector<float>& r) {
    std::vector<float> buf(frames * 2);
    a.readFrames(buf.data(), frames);
    l.resize(frames);
    r.resize(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        l[i] = buf[i * 2];
        r[i] = buf[i * 2 + 1];
    }
}
float rms(const std::vector<float>& v) {
    double s = 0;
    for (float x : v) s += (double)x * x;
    return (float)std::sqrt(s / std::max<size_t>(v.size(), 1));
}
float brightness(const std::vector<float>& v) {   // how much of it is high frequencies: RMS of the differences
    double s = 0;
    for (size_t i = 1; i < v.size(); ++i) s += (double)(v[i] - v[i - 1]) * (v[i] - v[i - 1]);
    return (float)std::sqrt(s / std::max<size_t>(v.size(), 1)) / std::max(rms(v), 1e-9f);
}

}  // namespace

// Occlusion muffles (less of the highs) and quietens a spatial sound
TEST(audio_occlusion_muffles) {
    auto listen = [](float occlusion, std::vector<float>& mono) {
        AudioEngine a;
        a.init(false);
        a.setListener({0, 0, 0}, {0, 0, -1});
        AudioEngine::PlayParams p;
        p.spatial = true;
        p.loop = true;
        p.position = {0, 0, -2};
        p.occlusion = occlusion;
        AudioEngine::Sound s = a.play("Break", p);   // (noise: every frequency)
        CHECK(a.occlusion(s) == occlusion);
        std::vector<float> l, r;
        channels(a, 8192, l, r);
        mono = l;
    };
    std::vector<float> clear, walled;
    listen(0.f, clear);
    listen(1.f, walled);
    CHECK(rms(walled) < rms(clear) * 0.5f);
    CHECK(brightness(walled) < brightness(clear) * 0.5f);
    AudioEngine a;
    a.init(false);
    CHECK(a.occlusion(a.play("Beep", {})) == -1.f);   // (not spatial: no occlusion)
}

// Headphones: a sound on the right reaches the left ear too, later and duller; speakers pan it right only.
// Behind sounds duller than in front.
TEST(audio_headphones_binaural) {
    AudioEngine a;
    REQUIRE(a.init(false));
    a.setListener({0, 0, 0}, {0, 0, -1});
    AudioEngine::PlayParams p;
    p.spatial = true;
    p.loop = true;
    p.position = {3, 0, 0};   // right (+X, facing -Z)
    AudioEngine::Sound s = a.play("Break", p);
    std::vector<float> l, r;
    channels(a, 8192, l, r);
    CHECK(rms(r) > 0.01f);
    CHECK(rms(l) < rms(r) * 0.01f);   // speakers: all on the right

    a.setHeadphones(true);
    CHECK(a.headphones());
    channels(a, 4096, l, r);   // (easing into it)
    channels(a, 8192, l, r);
    CHECK(rms(l) > rms(r) * 0.2f && rms(l) < rms(r) * 0.8f);   // the far ear: quieter ...
    CHECK(brightness(l) < brightness(r) * 0.8f);               // ... muffled by the head ...
    int bestLag = 0;                                           // ... and later: about 0.66 ms (29 samples)
    double best = -1e30;
    for (int lag = 0; lag <= 60; ++lag) {
        double c = 0;
        for (size_t i = 100; i + lag < l.size(); ++i) c += (double)r[i] * l[i + (size_t)lag];
        if (c > best) { best = c; bestLag = lag; }
    }
    CHECK(bestLag >= 24 && bestLag <= 36);

    // In front and behind: about the same level, behind duller
    a.setPosition(s, {0, 0, -3});
    channels(a, 4096, l, r);
    channels(a, 8192, l, r);
    float front = brightness(l);
    CHECK(std::fabs(rms(l) - rms(r)) < rms(l) * 0.05f);   // centred
    a.setPosition(s, {0, 0, 3});
    channels(a, 4096, l, r);
    channels(a, 8192, l, r);
    CHECK(brightness(l) < front * 0.8f);
}

// Layered music starts in step, layers fade on their own; stingers wait for the next beat or bar
TEST(audio_music_layers_and_stingers) {
    AudioEngine a;
    REQUIRE(a.init(false));
    float wait = -1.f;
    a.playStinger("Beep", AudioEngine::Sync::Beat, 1.f, 0.5f, &wait);
    CHECK(wait == 0.f);   // no music: at once
    a.stopAll();
    AudioEngine::Sound m = a.playMusicLayers({"Hum", "Hum"}, {1.f, 0.f}, 0.f, 120.f, 4);
    REQUIRE(m);
    CHECK(a.music() == m);
    CHECK(a.musicLayers() == 2);
    CHECK(a.musicLayerVolume(0) == 1.f && a.musicLayerVolume(1) == 0.f);
    a.setMusicLayerVolume(1, 1.f, 0.5f);
    a.update(0.25f);
    CHECK_NEAR(a.musicLayerVolume(1), 0.5, 0.01);
    a.update(0.5f);
    CHECK(a.musicLayerVolume(1) == 1.f);
    a.setMusicLayerVolume(0, 0.2f, 0.f);
    CHECK(a.musicLayerVolume(0) == 0.2f);

    level(a, (uint32_t)(44100 * 0.3f));   // 0.3 s in (it started 20 ms after it was asked: in step)
    float at = a.musicPosition();
    CHECK(at > 0.25f && at < 0.31f);
    a.playStinger("Beep", AudioEngine::Sync::Beat, 1.f, 0.5f, &wait);   // beats every 0.5 s at 120 bpm
    CHECK_NEAR(wait, 0.5f - at, 0.002);
    a.playStinger("Beep", AudioEngine::Sync::Bar, 1.f, 0.5f, &wait);    // bars every 2 s
    CHECK_NEAR(wait, 2.f - at, 0.002);
    a.playStinger("Beep", AudioEngine::Sync::Now, 1.f, 0.5f, &wait);
    CHECK(wait == 0.f);
    a.stopMusic(0.f);
    CHECK(a.musicLayers() == 0 && a.music() == 0);
}

// The player's settings: in the game state as settings.*, applied to the mixer, saved and read back
TEST(audio_settings_saved_and_applied) {
    test::ImGuiContextGuard imgui;
    AudioEngine audio;
    REQUIRE(audio.init(false));
    MeshLibrary meshes = buildMeshLibrary();
    std::filesystem::path dir = test::tempPath("settings_game");
    auto run = [&](Game& g, int frames) {
        for (int i = 0; i < frames; ++i) {
            ImGui::NewFrame();
            g.frame(1.f / 60.f, true, true);
            ImGui::EndFrame();
        }
    };
    {
        Scene s;
        s.create("Camera").camera = Camera{};
        Game g;
        g.setAudio(&audio);
        g.setSaveDirectory(dir);
        g.start(s, meshes);
        CHECK(g.state().number("settings.musicVolume") == 1.f);
        CHECK(!g.state().flag("settings.headphones"));
        g.state().setNumber("settings.musicVolume", 0.3f);
        g.state().setFlag("settings.headphones", true);
        g.state().setNumber("settings.effectsVolume", 7.f);   // (clamped)
        run(g, 2);
        CHECK_NEAR(audio.busLevel(AudioBus::Music), 0.3, 1e-4);
        CHECK_NEAR(audio.busLevel(AudioBus::Effects), 1.0, 1e-4);
        CHECK(audio.headphones());
        CHECK(!std::filesystem::exists(dir / "settings.json"));   // (saved a second later ...)
        run(g, 70);
        CHECK(std::filesystem::exists(dir / "settings.json"));
        g.state().setNumber("settings.uiVolume", 0.5f);
        run(g, 1);
        g.stop();   // (... or when the game ends)
    }
    Scene s;
    Game g;
    g.setAudio(&audio);
    g.setSaveDirectory(dir);
    g.start(s, meshes);
    CHECK_NEAR(g.state().number("settings.musicVolume"), 0.3, 1e-4);
    CHECK_NEAR(g.state().number("settings.uiVolume"), 0.5, 1e-4);
    CHECK(g.state().number("settings.effectsVolume") == 1.f);
    CHECK(g.state().flag("settings.headphones"));
    g.stop();
    audio.setHeadphones(false);
}

// In a game: a wall between the camera and a sound muffles it (as much as its AudioSource says)
TEST(audio_occlusion_in_game) {
    test::ImGuiContextGuard imgui;
    AudioEngine audio;
    REQUIRE(audio.init(false));
    MeshLibrary meshes = buildMeshLibrary();
    Scene s;
    s.create("Camera").camera = Camera{};   // at the origin, looking down -Z
    Entity& src = s.create("Speaker");
    src.transform.position[2] = -10.f;
    src.audioSource = AudioSource{};
    src.collider = Collider{};   // (its own collider doesn't count)
    Entity& wall = s.create("Wall");
    wall.transform.position[2] = -5.f;
    wall.collider = Collider{};
    wall.collider->size = {10, 10, 0.5f};
    Game g;
    g.setAudio(&audio);
    g.start(s, meshes);
    auto frame = [&] {
        ImGui::NewFrame();
        g.frame(1.f / 60.f, true, true);
        ImGui::EndFrame();
    };
    frame();
    REQUIRE(audio.spatialSounds().size() == 1);
    AudioEngine::Sound sound = audio.spatialSounds()[0].sound;
    CHECK(audio.occlusion(sound) == 1.f);
    s.remove(s.findByName("Wall")->id);
    frame();
    CHECK(audio.occlusion(sound) == 0.f);
    Entity& wall2 = s.create("Wall");
    wall2.transform.position[2] = -5.f;
    wall2.collider = Collider{};
    wall2.collider->size = {10, 10, 0.5f};
    s.findByName("Speaker")->audioSource->occlusion = 0.4f;
    frame();
    CHECK_NEAR(audio.occlusion(sound), 0.4, 1e-5);
    g.stop();
}
