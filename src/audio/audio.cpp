#include "audio/audio.h"

#include <cmath>
#include <cstdio>
#include <map>

#include "miniaudio.h"   // implementation compiled in miniaudio_impl.c

namespace {

constexpr uint32_t kRate = 44100;

// ---- Built-in sounds, synthesised (mono, 44.1 kHz)
float noise(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (float)(s & 0xFFFF) / 32767.5f - 1.f;
}

std::vector<float> synth(const std::string& name) {
    std::vector<float> out;
    auto length = [&](float seconds) { out.assign((size_t)(seconds * kRate), 0.f); };
    uint32_t rng = 0x9E3779B9u;
    const float tau = 2.f * PI;
    if (name == "Beep") {
        length(0.15f);
        for (size_t i = 0; i < out.size(); ++i) {
            float t = (float)i / kRate;
            out[i] = 0.4f * std::sin(tau * 880.f * t) * std::min(1.f, (0.15f - t) * 40.f);
        }
    } else if (name == "Pickup") {   // a quick rising arpeggio
        length(0.3f);
        const float notes[3] = {660.f, 880.f, 1320.f};
        for (size_t i = 0; i < out.size(); ++i) {
            float t = (float)i / kRate;
            int k = std::min(2, (int)(t / 0.1f));
            float local = t - k * 0.1f;
            out[i] = 0.35f * std::sin(tau * notes[k] * t) * std::exp(-local * 18.f);
        }
    } else if (name == "Shot") {   // noise burst plus a low thump
        length(0.25f);
        for (size_t i = 0; i < out.size(); ++i) {
            float t = (float)i / kRate;
            out[i] = 0.45f * noise(rng) * std::exp(-t * 28.f) + 0.5f * std::sin(tau * (140.f - 300.f * t) * t) * std::exp(-t * 18.f);
        }
    } else if (name == "Bounce") {   // a falling "boing"
        length(0.18f);
        for (size_t i = 0; i < out.size(); ++i) {
            float t = (float)i / kRate;
            out[i] = 0.5f * std::sin(tau * (320.f - 500.f * t) * t) * std::exp(-t * 20.f);
        }
    } else if (name == "Thud") {   // dull knock
        length(0.2f);
        float lp = 0.f;
        for (size_t i = 0; i < out.size(); ++i) {
            float t = (float)i / kRate;
            lp += (noise(rng) - lp) * 0.08f;   // low-passed noise
            out[i] = (0.6f * std::sin(tau * 90.f * t) + 1.2f * lp) * std::exp(-t * 25.f);
        }
    } else if (name == "Break") {   // crunchy decay
        length(0.45f);
        float lp = 0.f;
        for (size_t i = 0; i < out.size(); ++i) {
            float t = (float)i / kRate;
            lp += (noise(rng) - lp) * 0.3f;
            out[i] = 0.7f * lp * std::exp(-t * 9.f);
        }
    } else if (name == "Jump") {   // short rising sweep
        length(0.14f);
        for (size_t i = 0; i < out.size(); ++i) {
            float t = (float)i / kRate;
            out[i] = 0.3f * std::sin(tau * (280.f + 1400.f * t) * t) * std::exp(-t * 14.f);
        }
    } else if (name == "Hum") {   // 2 s seamless loop: whole numbers of cycles
        length(2.f);
        for (size_t i = 0; i < out.size(); ++i) {
            float t = (float)i / kRate;
            out[i] = 0.25f * std::sin(tau * 110.f * t) + 0.12f * std::sin(tau * 165.f * t) +
                     0.06f * std::sin(tau * 0.5f * t) * std::sin(tau * 220.f * t);
        }
    }
    return out;
}

}  // namespace

struct AudioEngine::Impl {
    ma_engine engine{};
    bool ready = false;
    struct Playing {
        ma_sound sound{};
        ma_audio_buffer_ref ref{};   // built-in clips: a read cursor over the shared samples
        bool usesRef = false;
        bool loop = false;
    };
    std::map<Sound, std::unique_ptr<Playing>> sounds;   // pointers stay put: miniaudio holds on to them
    std::map<std::string, std::vector<float>> builtin;
    Sound next = 1;

    void release(std::map<Sound, std::unique_ptr<Playing>>::iterator it) {
        ma_sound_uninit(&it->second->sound);
        if (it->second->usesRef) ma_audio_buffer_ref_uninit(&it->second->ref);
        sounds.erase(it);
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() { shutdown(); }

const std::vector<std::string>& AudioEngine::builtinClips() {
    static const std::vector<std::string> names = {"Pickup", "Shot", "Bounce", "Thud", "Break", "Jump", "Hum", "Beep"};
    return names;
}

bool AudioEngine::init(bool withDevice) {
    shutdown();
    ma_engine_config config = ma_engine_config_init();
    if (!withDevice) {
        config.noDevice = MA_TRUE;
        config.channels = 2;
        config.sampleRate = kRate;
    }
    ma_result r = ma_engine_init(&config, &impl_->engine);
    if (r != MA_SUCCESS) {
        error_ = std::string("audio disabled: ") + ma_result_description(r);
        return false;
    }
    impl_->ready = true;
    for (const std::string& name : builtinClips()) impl_->builtin[name] = synth(name);
    return true;
}

void AudioEngine::shutdown() {
    if (!impl_->ready) return;
    stopAll();
    ma_engine_uninit(&impl_->engine);
    impl_->ready = false;
}

bool AudioEngine::ready() const { return impl_->ready; }

AudioEngine::Sound AudioEngine::play(const std::string& clip, const PlayParams& p) {
    if (!impl_->ready || clip.empty()) return 0;
    auto s = std::make_unique<Impl::Playing>();
    ma_result r;
    auto b = impl_->builtin.find(clip);
    if (b != impl_->builtin.end()) {
        r = ma_audio_buffer_ref_init(ma_format_f32, 1, b->second.data(), b->second.size(), &s->ref);
        if (r != MA_SUCCESS) return 0;
        s->usesRef = true;
        r = ma_sound_init_from_data_source(&impl_->engine, &s->ref, 0, nullptr, &s->sound);
    } else {
        // Files: decoded once by miniaudio's resource manager, then shared
        r = ma_sound_init_from_file(&impl_->engine, clip.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, &s->sound);
    }
    if (r != MA_SUCCESS) {
        if (s->usesRef) ma_audio_buffer_ref_uninit(&s->ref);
        error_ = clip + ": " + ma_result_description(r);
        return 0;
    }
    ma_sound_set_volume(&s->sound, p.volume);
    ma_sound_set_looping(&s->sound, p.loop);
    s->loop = p.loop;
    ma_sound_set_spatialization_enabled(&s->sound, p.spatial);
    if (p.spatial) {
        ma_sound_set_position(&s->sound, p.position.x, p.position.y, p.position.z);
        ma_sound_set_attenuation_model(&s->sound, ma_attenuation_model_inverse);
        ma_sound_set_min_distance(&s->sound, p.minDistance);
        ma_sound_set_max_distance(&s->sound, p.maxDistance);
    }
    ma_sound_start(&s->sound);
    Sound id = impl_->next++;
    impl_->sounds[id] = std::move(s);
    ++playCount_;
    return id;
}

void AudioEngine::stop(Sound sound) {
    auto it = impl_->sounds.find(sound);
    if (it != impl_->sounds.end()) impl_->release(it);
}

void AudioEngine::stopAll() {
    while (!impl_->sounds.empty()) impl_->release(impl_->sounds.begin());
}

bool AudioEngine::playing(Sound sound) const {
    auto it = impl_->sounds.find(sound);
    return it != impl_->sounds.end() && !ma_sound_at_end(&it->second->sound);
}

void AudioEngine::setPosition(Sound sound, Vec3 p) {
    auto it = impl_->sounds.find(sound);
    if (it != impl_->sounds.end()) ma_sound_set_position(&it->second->sound, p.x, p.y, p.z);
}

void AudioEngine::setListener(Vec3 position, Vec3 forward) {
    if (!impl_->ready) return;
    ma_engine_listener_set_position(&impl_->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
}

void AudioEngine::setMasterVolume(float volume) {
    if (impl_->ready) ma_engine_set_volume(&impl_->engine, volume);
}

void AudioEngine::update() {
    for (auto it = impl_->sounds.begin(); it != impl_->sounds.end();) {
        auto next = std::next(it);
        if (!it->second->loop && ma_sound_at_end(&it->second->sound)) impl_->release(it);
        it = next;
    }
}

size_t AudioEngine::activeSounds() const { return impl_->sounds.size(); }

bool AudioEngine::readFrames(float* stereoOut, uint32_t frames) {
    if (!impl_->ready) return false;
    ma_uint64 read = 0;
    return ma_engine_read_pcm_frames(&impl_->engine, stereoOut, frames, &read) == MA_SUCCESS;
}
