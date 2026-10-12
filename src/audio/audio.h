// Sound playback (miniaudio): one-shot and looping sounds, positioned in 3D, heard from a listener.
//
// A clip is a name: one of the built-in synthesised sounds (builtinClips(): "Pickup", "Shot", "Bounce", ...) or the
// path of a WAV / MP3 / FLAC file (decoded once, then cached; or streamed: decoded as it plays, for music).
//
// Sounds play on a bus (effects, music, UI, ambience), each with its own volume; a bus can be ducked (lowered for a
// while, as under dialogue). Effects and ambience go through a room reverb (setReverb), off until a game turns it
// up. Moving spatial sounds and listeners get a Doppler shift from their velocities.
//
// Spatial sounds go through a little filter of their own: occlusion (setOcclusion: muffled and quieter, as behind a
// wall; the game sets it from rays through the scene) and the panning, either for speakers (equal-power) or, with
// setHeadphones(true), binaural: a spherical-head model (the far ear hears it up to 0.66 ms later and muffled by
// the head; sounds behind are a little duller), not measured HRTFs, but enough to tell left from right and front
// from back on headphones.
//
// Music can be layered (stems started together, faded in and out on their own: setMusicLayerVolume) and have
// stingers (short phrases) played over it on its next beat or bar.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/vecmath.h"

enum class AudioBus { Effects, Music, UI, Ambience, Count };
const char* audioBusName(AudioBus bus);   // "effects", "music", ...
AudioBus audioBusFromName(const std::string& name);

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
        AudioBus bus = AudioBus::Effects;
        bool stream = false;         // a file decoded as it plays (long music, ambience) instead of all up front
        Vec3 velocity;               // spatial: m/s, for the Doppler shift
        float doppler = 1.f;         // how strong the Doppler shift is (0: none)
        float fadeIn = 0.f;          // seconds
        float occlusion = 0.f;       // spatial: how muffled it starts (0..1; setOcclusion changes it)
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
    void stop(Sound sound, float fadeSeconds = 0.f);
    void stopAll();
    bool playing(Sound sound) const;
    void setPosition(Sound sound, Vec3 position);
    void setVelocity(Sound sound, Vec3 velocity);
    void setListener(Vec3 position, Vec3 forward, Vec3 velocity = {});
    void setMasterVolume(float volume);
    // Sound files' import settings ("volume": a gain on every play) are read once per file: after changing them
    void forgetImportSettings();
    bool streaming(Sound sound) const;   // decoded as it plays

    // Mixer
    void setBusVolume(AudioBus bus, float volume);
    // The player's own volume for a bus (a settings menu), on top of the game's setBusVolume
    void setBusUserVolume(AudioBus bus, float volume);
    float busVolume(AudioBus bus) const;
    // Lowers the bus to `volume` (times its own) for `seconds`, easing down and back up over `fade` seconds
    void duck(AudioBus bus, float volume, float seconds, float fade = 0.25f);
    float busLevel(AudioBus bus) const;   // its volume now, with any ducking

    // Music: one track at a time, streamed and looping on the music bus, cross-fading into the next
    Sound playMusic(const std::string& clip, float fadeSeconds = 1.f, float volume = 1.f);
    void stopMusic(float fadeSeconds = 1.f);
    Sound music() const { return music_; }
    // A layered track: the clips (stems of the same length) start on the same sample and loop together; volumes[i]
    // is layer i's (missing: 1). bpm / beatsPerBar: its tempo, for stingers (0: they play at once).
    Sound playMusicLayers(const std::vector<std::string>& clips, const std::vector<float>& volumes, float fadeSeconds = 1.f,
                          float bpm = 0.f, int beatsPerBar = 4);
    void setMusicLayerVolume(int layer, float volume, float fadeSeconds = 1.f);
    float musicLayerVolume(int layer) const;   // now (mid-fade: part way)
    int musicLayers() const;
    void setMusicTempo(float bpm, int beatsPerBar = 4);
    float musicPosition() const;   // seconds into the current track (0: none)
    // A stinger: a one-shot on the music bus, started on the music's next beat or bar (at once without a tempo or
    // music), with the music ducked under it. Returns the sound; startsIn (if given) says how long until it plays.
    enum class Sync { Now, Beat, Bar };
    Sound playStinger(const std::string& clip, Sync sync = Sync::Beat, float volume = 1.f, float duckMusic = 0.5f,
                      float* startsIn = nullptr);

    // Spatial sounds: muffling by what's between them and the listener (0: clear .. 1: behind a wall)
    void setOcclusion(Sound sound, float amount);
    float occlusion(Sound sound) const;     // what it was set to (-1: not a spatial sound)
    struct SpatialSound {
        Sound sound;
        Vec3 position;
    };
    std::vector<SpatialSound> spatialSounds() const;   // playing ones, for working out their occlusion
    // Headphones: binaural panning (see above) for every spatial sound, playing ones too; off: speakers
    void setHeadphones(bool on);
    bool headphones() const;

    // The room the listener is in: how much reverb effects and ambience get (0: dry), how big and how muffled
    void setReverb(float wet, float roomSize = 0.7f, float damping = 0.5f);
    float reverbWet() const;

    void update(float dt = -1.f);   // once per frame: fades, ducking, releases finished sounds (dt < 0: measured)

    size_t activeSounds() const;
    size_t decodedClips() const;   // clips decoded whole and cached (built-ins included)
    unsigned long long playCount() const { return playCount_; }   // sounds started so far (for tests)
    bool readFrames(float* stereoOut, uint32_t frames);           // without a device: mix the next frames

    static const std::vector<std::string>& builtinClips();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
    unsigned long long playCount_ = 0;
    Sound music_ = 0;
};
