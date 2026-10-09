// Sound playback (miniaudio): one-shot and looping sounds, positioned in 3D, heard from a listener.
//
// A clip is a name: one of the built-in synthesised sounds (builtinClips(): "Pickup", "Shot", "Bounce", ...) or the
// path of a WAV / MP3 / FLAC file (decoded once, then cached).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/vecmath.h"

class AudioEngine {
public:
    using Sound = uint32_t;   // a playing sound; 0 = none

    struct PlayParams {
        float volume = 1.f;
        bool loop = false;
        bool spatial = false;        // positioned in the world (fades with distance, pans) or plain stereo
        Vec3 position;
        float minDistance = 1.5f;    // full volume up to here
        float maxDistance = 60.f;    // no quieter beyond here
    };

    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // With an output device (false if there's none: the program just stays silent), or without one (tests):
    // then sounds are mixed into readFrames()
    bool init(bool withDevice = true);
    void shutdown();
    bool ready() const;
    const std::string& error() const { return error_; }

    Sound play(const std::string& clip, const PlayParams& params);
    void stop(Sound sound);
    void stopAll();
    bool playing(Sound sound) const;
    void setPosition(Sound sound, Vec3 position);
    void setListener(Vec3 position, Vec3 forward);
    void setMasterVolume(float volume);
    void update();   // once per frame: releases finished sounds

    size_t activeSounds() const;
    unsigned long long playCount() const { return playCount_; }   // sounds started so far (for tests)
    bool readFrames(float* stereoOut, uint32_t frames);           // without a device: mix the next frames

    static const std::vector<std::string>& builtinClips();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
    unsigned long long playCount_ = 0;
};
