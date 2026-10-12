#include "audio/audio.h"
#include "assets/asset_db.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>

#include "core/project.h"
#include "core/vfs.h"
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

// ---- Reverb: a small Freeverb (4 combs and 2 all-passes per channel), as a node between the effect buses and the
// output. Its settings are atomics: the game sets them, the audio thread reads them.
struct ReverbNode {
    ma_node_base base;   // (first: miniaudio treats this as a ma_node)
    std::atomic<float> wet{0.f}, room{0.7f}, damp{0.5f};
    struct Comb {
        std::vector<float> buf;
        size_t at = 0;
        float store = 0.f;
        float process(float x, float feedback, float damping) {
            float y = buf[at];
            store = y * (1.f - damping) + store * damping;
            buf[at] = x + store * feedback;
            if (++at == buf.size()) at = 0;
            return y;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        size_t at = 0;
        float process(float x) {
            float b = buf[at];
            buf[at] = x + b * 0.5f;
            if (++at == buf.size()) at = 0;
            return b - x;
        }
    };
    Comb combs[2][4];
    Allpass allpasses[2][2];
    void setup(uint32_t rate) {
        const int combLen[4] = {1116, 1188, 1277, 1356}, apLen[2] = {556, 441};
        float k = (float)rate / 44100.f;
        for (int ch = 0; ch < 2; ++ch) {
            int spread = ch ? 23 : 0;   // (left and right differ a little: a wide sound)
            for (int c = 0; c < 4; ++c) combs[ch][c].buf.assign((size_t)((combLen[c] + spread) * k), 0.f);
            for (int a = 0; a < 2; ++a) allpasses[ch][a].buf.assign((size_t)((apLen[a] + spread) * k), 0.f);
        }
    }
};

void reverbProcess(ma_node* node, const float** in, ma_uint32* inCount, float** out, ma_uint32* outCount) {
    auto* r = reinterpret_cast<ReverbNode*>(node);
    ma_uint32 frames = std::min(*inCount, *outCount);
    const float* src = in[0];
    float* dst = out[0];
    float wet = r->wet.load(std::memory_order_relaxed);
    if (wet <= 0.0001f) {   // dry: just pass it on (the tails just stop)
        std::copy(src, src + frames * 2, dst);
        *outCount = frames;
        return;
    }
    float feedback = r->room.load(std::memory_order_relaxed) * 0.28f + 0.7f;
    float damping = r->damp.load(std::memory_order_relaxed) * 0.4f;
    for (ma_uint32 f = 0; f < frames; ++f) {
        float l = src[f * 2], rr = src[f * 2 + 1], input = (l + rr) * 0.03f;
        float o[2];
        for (int ch = 0; ch < 2; ++ch) {
            float acc = 0.f;
            for (auto& c : r->combs[ch]) acc += c.process(input, feedback, damping);
            for (auto& a : r->allpasses[ch]) acc = a.process(acc);
            o[ch] = acc;
        }
        dst[f * 2] = l + o[0] * wet * 3.f;
        dst[f * 2 + 1] = rr + o[1] * wet * 3.f;
    }
    *outCount = frames;
}

ma_node_vtable kReverbVtable = {reverbProcess, nullptr, 1, 1, 0};

// ---- A spatial sound's own filter (mono in, stereo out): occlusion (a low-pass and a dip in volume), then
// panning, for speakers or binaural for headphones. Targets are atomics set by the game; the audio thread eases
// towards them a block at a time (gains ramp across the block: no clicks).
struct SpatialNode {
    ma_node_base base;   // (first)
    std::atomic<float> occlusion{0.f}, pan{0.f}, behind{0.f};   // targets: 0..1, -1 (left)..1 (right), 0..1
    const std::atomic<bool>* headphones = nullptr;
    uint32_t rate = 44100;
    // audio thread
    float occ = 0.f, panNow = 0.f, back = 0.f;
    float gainL = 0.70710678f, gainR = 0.70710678f;
    float lpOcc = 0.f, lpBack = 0.f, lpFar = 0.f;
    float delayNow = 0.f;
    float ring[128] = {0};
    uint32_t at = 0;
    bool started = false;
};

// A one-pole low-pass's coefficient, from open (amount 0: passes everything, exactly) down to `cutoff` Hz (amount 1)
float onePole(float cutoff, float amount, uint32_t rate) {
    float a = 1.f - std::exp(-2.f * PI * cutoff / (float)rate);
    return 1.f + (a - 1.f) * std::clamp(amount, 0.f, 1.f);
}

void spatialProcess(ma_node* node, const float** in, ma_uint32* inCount, float** out, ma_uint32* outCount) {
    auto* n = reinterpret_cast<SpatialNode*>(node);
    ma_uint32 frames = std::min(*inCount, *outCount);
    *outCount = frames;
    const float* src = in[0];
    float* dst = out[0];
    if (frames == 0) return;
    // Ease the targets: about 80 ms for occlusion, 30 ms for direction
    float block = (float)frames / (float)n->rate;
    auto ease = [&](float& now, float target, float seconds) {
        float k = n->started ? std::min(1.f, block / seconds) : 1.f;
        now += (target - now) * k;
    };
    ease(n->occ, std::clamp(n->occlusion.load(std::memory_order_relaxed), 0.f, 1.f), 0.08f);
    ease(n->panNow, std::clamp(n->pan.load(std::memory_order_relaxed), -1.f, 1.f), 0.03f);
    ease(n->back, std::clamp(n->behind.load(std::memory_order_relaxed), 0.f, 1.f), 0.03f);
    bool first = !n->started;
    n->started = true;
    bool binaural = n->headphones && n->headphones->load(std::memory_order_relaxed);
    float side = std::fabs(n->panNow);
    // Occlusion: from open (20 kHz) down to a wall's 600 Hz, and up to 9 dB quieter
    float aOcc = onePole(600.f, std::sqrt(n->occ), n->rate);
    float occGain = 1.f - 0.65f * n->occ;
    float targetL, targetR, aBack = 1.f, aFar = 1.f, delay = 0.f;
    if (!binaural) {   // equal power
        float theta = (n->panNow + 1.f) * 0.25f * PI;
        targetL = std::cos(theta);
        targetR = std::sin(theta);
    } else {
        // The near ear a little louder, the far one quieter, later (up to 0.66 ms) and muffled by the head
        float nearG = 0.70710678f + 0.25f * side, farG = 0.70710678f * (1.f - 0.45f * side);
        targetL = n->panNow >= 0.f ? farG : nearG;
        targetR = n->panNow >= 0.f ? nearG : farG;
        aFar = onePole(1500.f, side, n->rate);
        aBack = onePole(5000.f, n->back, n->rate);
        delay = 0.00066f * side * (float)n->rate;
    }
    if (first) {   // (no ramp into where it starts)
        n->gainL = targetL;
        n->gainR = targetR;
        n->delayNow = delay;
    }
    float startL = n->gainL, startR = n->gainR, startDelay = n->delayNow;
    bool farIsLeft = n->panNow >= 0.f;
    for (ma_uint32 f = 0; f < frames; ++f) {
        float t = (float)(f + 1) / (float)frames;
        float x = src[f];
        n->lpOcc += (x - n->lpOcc) * aOcc;
        float y = n->lpOcc * occGain;
        float l, r;
        if (!binaural) {
            l = r = y;
        } else {
            n->lpBack += (y - n->lpBack) * aBack;
            y = n->lpBack;
            n->ring[n->at] = y;
            float d = startDelay + (delay - startDelay) * t;   // (fractional, read between two samples)
            float pos = (float)n->at - d;
            while (pos < 0.f) pos += 128.f;
            uint32_t i0 = (uint32_t)pos, i1 = (i0 + 1) & 127;
            float frac = pos - (float)i0;
            float delayed = n->ring[i0 & 127] * (1.f - frac) + n->ring[i1] * frac;
            n->at = (n->at + 1) & 127;
            n->lpFar += (delayed - n->lpFar) * aFar;
            l = farIsLeft ? n->lpFar : y;
            r = farIsLeft ? y : n->lpFar;
        }
        dst[f * 2] = l * (startL + (targetL - startL) * t);
        dst[f * 2 + 1] = r * (startR + (targetR - startR) * t);
    }
    n->gainL = targetL;
    n->gainR = targetR;
    n->delayNow = delay;
}

ma_node_vtable kSpatialVtable = {spatialProcess, nullptr, 1, 1, 0};

}  // namespace

const char* audioBusName(AudioBus bus) {
    switch (bus) {
    case AudioBus::Effects: return "effects";
    case AudioBus::Music: return "music";
    case AudioBus::UI: return "ui";
    case AudioBus::Ambience: return "ambience";
    case AudioBus::Count: break;
    }
    return "effects";
}

AudioBus audioBusFromName(const std::string& name) {
    for (int i = 0; i < (int)AudioBus::Count; ++i)
        if (name == audioBusName((AudioBus)i)) return (AudioBus)i;
    return AudioBus::Effects;
}

struct AudioEngine::Impl {
    ma_engine engine{};
    bool ready = false;
    struct Playing {
        ma_sound sound{};
        ma_audio_buffer_ref ref{};   // decoded clips: a read cursor over the shared samples
        bool usesRef = false;
        ma_decoder decoder{};        // streamed ones: decoding as they play ...
        std::string bytes;           // ... from the file's bytes
        bool usesDecoder = false;
        bool loop = false;
        float stopIn = -1.f;         // fading out: released after this many seconds
        std::unique_ptr<SpatialNode> spatial;   // spatial sounds: occlusion and panning
        Vec3 position;
        float volume = 1.f;          // as played
        float gain = 1.f;            // the clip's import setting
    };
    std::map<Sound, std::unique_ptr<Playing>> sounds;   // pointers stay put: miniaudio holds on to them
    struct Clip {
        std::vector<float> samples;   // interleaved
        ma_uint32 channels = 1;
    };
    std::map<std::string, Clip> clips;   // built-in ones, and files once decoded (by path)
    std::map<std::string, float> gains;  // files' "volume" import settings (assets/asset_db.h), by clip
    float gainOf(const std::string& clip) {
        auto it = gains.find(clip);
        if (it != gains.end()) return it->second;
        float g = 1.f;
        std::filesystem::path path = std::filesystem::u8path(project::resolve(clip));
        if (vfs::exists(path)) g = std::clamp(assetdb::importSettings(path)["volume"].asFloat(1.f), 0.f, 4.f);
        return gains[clip] = g;
    }
    Sound next = 1;
    ma_sound_group buses[(int)AudioBus::Count]{};
    float busVolume[(int)AudioBus::Count] = {1.f, 1.f, 1.f, 1.f};
    float userVolume[(int)AudioBus::Count] = {1.f, 1.f, 1.f, 1.f};
    struct Duck {
        float target = 1.f, level = 1.f, left = 0.f, rate = 4.f;
    } ducks[(int)AudioBus::Count];
    ReverbNode reverb{};
    std::chrono::steady_clock::time_point lastUpdate = std::chrono::steady_clock::now();

    std::atomic<bool> headphones{false};
    Vec3 listenerPos, listenerForward{0, 0, -1};
    // Music layers (layer 0 is music_ itself) and their volume fades; the tempo, for stingers
    struct Layer {
        Sound sound = 0;
        float volume = 1.f, target = 1.f, rate = 0.f;
    };
    std::vector<Layer> layers;
    float duckLayers = 1.f, duckLayersLeft = 0.f, duckLayersLevel = 1.f;   // under a stinger
    float bpm = 0.f;
    int beatsPerBar = 4;
    ma_uint64 musicStart = 0;   // engine time (frames) the music's first sample played

    void aim(Playing& p) {   // the spatial node's direction from the listener
        if (!p.spatial) return;
        Vec3 d = p.position - listenerPos;
        float len = length(d);
        Vec3 f = normalize(listenerForward);
        Vec3 right = normalize(cross(f, Vec3{0, 1, 0}));
        if (len < 1e-4f || length(right) < 0.5f) {
            p.spatial->pan.store(0.f);
            p.spatial->behind.store(0.f);
            return;
        }
        d = d * (1.f / len);
        p.spatial->pan.store(dot(d, right));
        p.spatial->behind.store(std::max(0.f, -dot(d, f)));
    }
    void release(std::map<Sound, std::unique_ptr<Playing>>::iterator it) {
        ma_sound_uninit(&it->second->sound);
        if (it->second->spatial) ma_node_uninit(it->second->spatial.get(), nullptr);
        if (it->second->usesRef) ma_audio_buffer_ref_uninit(&it->second->ref);
        if (it->second->usesDecoder) ma_decoder_uninit(&it->second->decoder);
        sounds.erase(it);
    }
    void applyBus(int b) { ma_sound_group_set_volume(&buses[b], busVolume[b] * userVolume[b] * ducks[b].level); }
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
    // The mix: effects and ambience through the reverb, music and UI straight out
    ma_uint32 channels[1] = {2};
    ma_node_config nc = ma_node_config_init();
    nc.vtable = &kReverbVtable;
    nc.pInputChannels = channels;
    nc.pOutputChannels = channels;
    impl_->reverb.setup(ma_engine_get_sample_rate(&impl_->engine));
    bool reverb = ma_engine_get_channels(&impl_->engine) == 2 &&
                  ma_node_init(ma_engine_get_node_graph(&impl_->engine), &nc, nullptr, &impl_->reverb) == MA_SUCCESS;
    if (reverb) ma_node_attach_output_bus(&impl_->reverb, 0, ma_engine_get_endpoint(&impl_->engine), 0);
    for (int b = 0; b < (int)AudioBus::Count; ++b) {
        ma_sound_group_init(&impl_->engine, 0, nullptr, &impl_->buses[b]);
        if (reverb && (b == (int)AudioBus::Effects || b == (int)AudioBus::Ambience))
            ma_node_attach_output_bus(&impl_->buses[b], 0, &impl_->reverb, 0);
        impl_->ducks[b] = {};
        impl_->applyBus(b);
    }
    impl_->ready = true;
    for (const std::string& name : builtinClips()) impl_->clips[name] = {synth(name), 1};
    return true;
}

void AudioEngine::shutdown() {
    if (!impl_->ready) return;
    stopAll();
    for (ma_sound_group& g : impl_->buses) ma_sound_group_uninit(&g);
    ma_engine_uninit(&impl_->engine);   // (stops the device: the reverb node can't be running now)
    ma_node_uninit(&impl_->reverb, nullptr);
    impl_->ready = false;
    music_ = 0;
}

bool AudioEngine::ready() const { return impl_->ready; }

AudioEngine::Sound AudioEngine::play(const std::string& clip, const PlayParams& p) {
    if (!impl_->ready || clip.empty()) return 0;
    auto s = std::make_unique<Impl::Playing>();
    ma_sound_group* group = &impl_->buses[std::clamp((int)p.bus, 0, (int)AudioBus::Count - 1)];
    ma_result r;
    // Spatial sounds: mono out of miniaudio's spatializer (distance, Doppler) into their own node (occlusion,
    // panning), then the bus. Others: straight into the bus.
    auto initSound = [&](ma_data_source* source) -> ma_result {
        if (!p.spatial) return ma_sound_init_from_data_source(&impl_->engine, source, 0, group, &s->sound);
        s->spatial = std::make_unique<SpatialNode>();
        SpatialNode& n = *s->spatial;
        n.rate = ma_engine_get_sample_rate(&impl_->engine);
        n.headphones = &impl_->headphones;
        n.occlusion.store(std::clamp(p.occlusion, 0.f, 1.f));
        ma_uint32 inChannels[1] = {1}, outChannels[1] = {2};
        ma_node_config nc = ma_node_config_init();
        nc.vtable = &kSpatialVtable;
        nc.pInputChannels = inChannels;
        nc.pOutputChannels = outChannels;
        ma_result nr = ma_node_init(ma_engine_get_node_graph(&impl_->engine), &nc, nullptr, &n);
        if (nr != MA_SUCCESS) {
            s->spatial.reset();
            return nr;
        }
        ma_node_attach_output_bus(&n, 0, group, 0);
        ma_sound_config sc = ma_sound_config_init_2(&impl_->engine);
        sc.pDataSource = source;
        sc.pInitialAttachment = &n;
        sc.channelsOut = 1;
        ma_result sr = ma_sound_init_ex(&impl_->engine, &sc, &s->sound);
        if (sr != MA_SUCCESS) {
            ma_node_uninit(&n, nullptr);
            s->spatial.reset();
        }
        return sr;
    };
    auto c = impl_->clips.find(clip);
    if (c == impl_->clips.end() && p.stream) {
        // Streamed: the file's bytes stay with the sound, decoded a little at a time on the audio thread
        std::string path = project::resolve(clip);
        if (!vfs::read(std::filesystem::u8path(path), s->bytes)) {
            error_ = clip + ": cannot open " + path;
            return 0;
        }
        ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, ma_engine_get_sample_rate(&impl_->engine));
        r = ma_decoder_init_memory(s->bytes.data(), s->bytes.size(), &dc, &s->decoder);
        if (r != MA_SUCCESS) {
            error_ = clip + ": " + ma_result_description(r);
            return 0;
        }
        s->usesDecoder = true;
        r = initSound(&s->decoder);
    } else {
        if (c == impl_->clips.end()) {
            // A file (relative to the project; from the game's pack if it has one): decoded once, then shared
            std::string path = project::resolve(clip), bytes;
            if (!vfs::read(std::filesystem::u8path(path), bytes)) {
                error_ = clip + ": cannot open " + path;
                return 0;
            }
            ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, ma_engine_get_sample_rate(&impl_->engine));
            ma_uint64 frames = 0;
            void* pcm = nullptr;
            r = ma_decode_memory(bytes.data(), bytes.size(), &dc, &frames, &pcm);
            if (r != MA_SUCCESS) {
                error_ = clip + ": " + ma_result_description(r);
                return 0;
            }
            Impl::Clip decoded;
            decoded.channels = dc.channels;
            decoded.samples.assign((const float*)pcm, (const float*)pcm + frames * dc.channels);
            ma_free(pcm, nullptr);
            c = impl_->clips.emplace(clip, std::move(decoded)).first;
        }
        r = ma_audio_buffer_ref_init(ma_format_f32, c->second.channels, c->second.samples.data(),
                                     c->second.samples.size() / c->second.channels, &s->ref);
        if (r != MA_SUCCESS) return 0;
        s->usesRef = true;
        r = initSound(&s->ref);
    }
    if (r != MA_SUCCESS) {
        if (s->usesRef) ma_audio_buffer_ref_uninit(&s->ref);
        if (s->usesDecoder) ma_decoder_uninit(&s->decoder);
        error_ = clip + ": " + ma_result_description(r);
        return 0;
    }
    s->volume = p.volume;
    s->gain = impl_->gainOf(clip);
    ma_sound_set_volume(&s->sound, s->volume * s->gain);
    s->position = p.position;
    impl_->aim(*s);
    ma_sound_set_looping(&s->sound, p.loop);
    s->loop = p.loop;
    ma_sound_set_spatialization_enabled(&s->sound, p.spatial);
    if (p.spatial) {
        ma_sound_set_position(&s->sound, p.position.x, p.position.y, p.position.z);
        ma_sound_set_velocity(&s->sound, p.velocity.x, p.velocity.y, p.velocity.z);
        ma_sound_set_doppler_factor(&s->sound, std::max(p.doppler, 0.f));
        ma_sound_set_attenuation_model(&s->sound, ma_attenuation_model_inverse);
        ma_sound_set_min_distance(&s->sound, p.minDistance);
        ma_sound_set_max_distance(&s->sound, p.maxDistance);
        if (s->spatial) ma_sound_set_directional_attenuation_factor(&s->sound, 0.f);   // (the node pans: not this)
    }
    if (p.fadeIn > 0.f) ma_sound_set_fade_in_milliseconds(&s->sound, 0.f, 1.f, (ma_uint64)(p.fadeIn * 1000.f));   // (on top of its volume)
    ma_sound_start(&s->sound);
    Sound id = impl_->next++;
    impl_->sounds[id] = std::move(s);
    ++playCount_;
    return id;
}

void AudioEngine::stop(Sound sound, float fadeSeconds) {
    auto it = impl_->sounds.find(sound);
    if (it == impl_->sounds.end()) return;
    if (fadeSeconds <= 0.f) {
        impl_->release(it);
        return;
    }
    if (it->second->stopIn < 0.f) {   // fade out, then release (in update)
        ma_sound_stop_with_fade_in_milliseconds(&it->second->sound, (ma_uint64)(fadeSeconds * 1000.f));
        it->second->stopIn = fadeSeconds + 0.05f;
    }
}

void AudioEngine::stopAll() {
    while (!impl_->sounds.empty()) impl_->release(impl_->sounds.begin());
    impl_->layers.clear();
    impl_->duckLayersLeft = 0.f;
    impl_->duckLayersLevel = 1.f;
    music_ = 0;
}

bool AudioEngine::playing(Sound sound) const {
    auto it = impl_->sounds.find(sound);
    return it != impl_->sounds.end() && it->second->stopIn < 0.f && !ma_sound_at_end(&it->second->sound);
}

bool AudioEngine::streaming(Sound sound) const {
    auto it = impl_->sounds.find(sound);
    return it != impl_->sounds.end() && it->second->usesDecoder;
}

void AudioEngine::setPosition(Sound sound, Vec3 p) {
    auto it = impl_->sounds.find(sound);
    if (it == impl_->sounds.end()) return;
    ma_sound_set_position(&it->second->sound, p.x, p.y, p.z);
    it->second->position = p;
    impl_->aim(*it->second);
}

void AudioEngine::setVelocity(Sound sound, Vec3 v) {
    auto it = impl_->sounds.find(sound);
    if (it != impl_->sounds.end()) ma_sound_set_velocity(&it->second->sound, v.x, v.y, v.z);
}

void AudioEngine::setListener(Vec3 position, Vec3 forward, Vec3 velocity) {
    if (!impl_->ready) return;
    ma_engine_listener_set_position(&impl_->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_velocity(&impl_->engine, 0, velocity.x, velocity.y, velocity.z);
    impl_->listenerPos = position;
    impl_->listenerForward = forward;
    for (auto& [id, s] : impl_->sounds) impl_->aim(*s);
}

void AudioEngine::setOcclusion(Sound sound, float amount) {
    auto it = impl_->sounds.find(sound);
    if (it != impl_->sounds.end() && it->second->spatial) it->second->spatial->occlusion.store(std::clamp(amount, 0.f, 1.f));
}

float AudioEngine::occlusion(Sound sound) const {
    auto it = impl_->sounds.find(sound);
    return it != impl_->sounds.end() && it->second->spatial ? it->second->spatial->occlusion.load() : -1.f;
}

std::vector<AudioEngine::SpatialSound> AudioEngine::spatialSounds() const {
    std::vector<SpatialSound> out;
    for (const auto& [id, s] : impl_->sounds)
        if (s->spatial && s->stopIn < 0.f) out.push_back({id, s->position});
    return out;
}

void AudioEngine::setHeadphones(bool on) { impl_->headphones.store(on); }
bool AudioEngine::headphones() const { return impl_->headphones.load(); }

AudioEngine::Sound AudioEngine::playMusicLayers(const std::vector<std::string>& clips, const std::vector<float>& volumes,
                                                float fadeSeconds, float bpm, int beatsPerBar) {
    stopMusic(fadeSeconds);
    if (!impl_->ready || clips.empty()) return 0;
    // All of them start on the same sample: scheduled a little ahead on the engine's clock
    ma_uint64 start = ma_engine_get_time_in_pcm_frames(&impl_->engine) + ma_engine_get_sample_rate(&impl_->engine) / 50;
    for (size_t i = 0; i < clips.size(); ++i) {
        PlayParams p;
        p.volume = i < volumes.size() ? volumes[i] : 1.f;
        p.loop = true;
        p.bus = AudioBus::Music;
        p.stream = true;
        p.fadeIn = fadeSeconds;
        Sound s = play(clips[i], p);
        if (s) {
            ma_sound_set_start_time_in_pcm_frames(&impl_->sounds[s]->sound, start);
        }
        Impl::Layer layer;
        layer.sound = s;
        layer.volume = layer.target = p.volume;
        impl_->layers.push_back(layer);
    }
    music_ = impl_->layers.empty() ? 0 : impl_->layers[0].sound;
    impl_->musicStart = start;
    setMusicTempo(bpm, beatsPerBar);
    return music_;
}

void AudioEngine::setMusicLayerVolume(int layer, float volume, float fadeSeconds) {
    if (layer < 0 || layer >= (int)impl_->layers.size()) return;
    Impl::Layer& l = impl_->layers[(size_t)layer];
    l.target = std::max(volume, 0.f);
    if (fadeSeconds <= 0.f) l.volume = l.target;   // (update() applies it)
    l.rate = fadeSeconds > 0.f ? std::fabs(l.target - l.volume) / fadeSeconds : 0.f;
}

float AudioEngine::musicLayerVolume(int layer) const {
    return layer >= 0 && layer < (int)impl_->layers.size() ? impl_->layers[(size_t)layer].volume : 0.f;
}

int AudioEngine::musicLayers() const { return (int)impl_->layers.size(); }

void AudioEngine::setMusicTempo(float bpm, int beatsPerBar) {
    impl_->bpm = std::max(bpm, 0.f);
    impl_->beatsPerBar = std::max(beatsPerBar, 1);
}

float AudioEngine::musicPosition() const {
    if (!impl_->ready || !music_ || !impl_->sounds.count(music_)) return 0.f;
    ma_uint64 now = ma_engine_get_time_in_pcm_frames(&impl_->engine);
    if (now <= impl_->musicStart) return 0.f;
    return (float)(now - impl_->musicStart) / (float)ma_engine_get_sample_rate(&impl_->engine);
}

AudioEngine::Sound AudioEngine::playStinger(const std::string& clip, Sync sync, float volume, float duckMusic,
                                            float* startsIn) {
    if (startsIn) *startsIn = 0.f;
    if (!impl_->ready) return 0;
    float wait = 0.f;
    if (sync != Sync::Now && impl_->bpm > 0.f && music_ && impl_->sounds.count(music_)) {
        float unit = 60.f / impl_->bpm * (sync == Sync::Bar ? (float)impl_->beatsPerBar : 1.f);
        float at = musicPosition();
        wait = std::ceil(at / unit - 1e-4f) * unit - at;   // (on a beat already: now)
        if (wait < 0.f) wait = 0.f;
    }
    PlayParams p;
    p.volume = volume;
    p.bus = AudioBus::Music;
    Sound s = play(clip, p);
    if (!s) return 0;
    if (wait > 0.f) {
        ma_sound_stop(&impl_->sounds[s]->sound);   // (from its first sample, on the beat)
        ma_sound_seek_to_pcm_frame(&impl_->sounds[s]->sound, 0);
        ma_sound_set_start_time_in_pcm_frames(&impl_->sounds[s]->sound,
                                              ma_engine_get_time_in_pcm_frames(&impl_->engine) +
                                                  (ma_uint64)(wait * (float)ma_engine_get_sample_rate(&impl_->engine)));
        ma_sound_start(&impl_->sounds[s]->sound);
    }
    // The music dips under it while it plays (the stinger itself is on the music bus too, so it goes through the
    // layers' volumes instead of the bus: just the track's sounds)
    if (duckMusic < 1.f) {
        float length = 0.f;
        ma_sound_get_length_in_seconds(&impl_->sounds[s]->sound, &length);
        impl_->duckLayers = std::clamp(duckMusic, 0.f, 1.f);
        impl_->duckLayersLeft = wait + length;
    }
    if (startsIn) *startsIn = wait;
    return s;
}

void AudioEngine::forgetImportSettings() { impl_->gains.clear(); }

void AudioEngine::setMasterVolume(float volume) {
    if (impl_->ready) ma_engine_set_volume(&impl_->engine, volume);
}

void AudioEngine::setBusVolume(AudioBus bus, float volume) {
    int b = std::clamp((int)bus, 0, (int)AudioBus::Count - 1);
    impl_->busVolume[b] = std::max(volume, 0.f);
    if (impl_->ready) impl_->applyBus(b);
}

void AudioEngine::setBusUserVolume(AudioBus bus, float volume) {
    int b = std::clamp((int)bus, 0, (int)AudioBus::Count - 1);
    impl_->userVolume[b] = std::max(volume, 0.f);
    if (impl_->ready) impl_->applyBus(b);
}

float AudioEngine::busVolume(AudioBus bus) const { return impl_->busVolume[std::clamp((int)bus, 0, (int)AudioBus::Count - 1)]; }

float AudioEngine::busLevel(AudioBus bus) const {
    int b = std::clamp((int)bus, 0, (int)AudioBus::Count - 1);
    return impl_->busVolume[b] * impl_->userVolume[b] * impl_->ducks[b].level;
}

void AudioEngine::duck(AudioBus bus, float volume, float seconds, float fade) {
    Impl::Duck& d = impl_->ducks[std::clamp((int)bus, 0, (int)AudioBus::Count - 1)];
    d.target = std::clamp(volume, 0.f, 1.f);
    d.left = std::max(seconds, 0.f);
    d.rate = fade > 0.f ? 1.f / fade : 1000.f;
}

AudioEngine::Sound AudioEngine::playMusic(const std::string& clip, float fadeSeconds, float volume) {
    return playMusicLayers({clip}, {volume}, fadeSeconds, impl_->bpm, impl_->beatsPerBar);
}

void AudioEngine::stopMusic(float fadeSeconds) {
    for (const Impl::Layer& l : impl_->layers)
        if (l.sound) stop(l.sound, fadeSeconds);
    impl_->layers.clear();
    impl_->duckLayersLeft = 0.f;
    impl_->duckLayersLevel = 1.f;
    music_ = 0;
}

void AudioEngine::setReverb(float wet, float roomSize, float damping) {
    impl_->reverb.wet.store(std::clamp(wet, 0.f, 1.f));
    impl_->reverb.room.store(std::clamp(roomSize, 0.f, 1.f));
    impl_->reverb.damp.store(std::clamp(damping, 0.f, 1.f));
}

float AudioEngine::reverbWet() const { return impl_->reverb.wet.load(); }

void AudioEngine::update(float dt) {
    auto now = std::chrono::steady_clock::now();
    if (dt < 0.f) dt = std::min(std::chrono::duration<float>(now - impl_->lastUpdate).count(), 0.25f);
    impl_->lastUpdate = now;
    if (!impl_->ready) return;
    for (int b = 0; b < (int)AudioBus::Count; ++b) {   // ducking: towards its target while it lasts, then back
        Impl::Duck& d = impl_->ducks[b];
        float goal = d.left > 0.f ? d.target : 1.f;
        d.left = std::max(d.left - dt, 0.f);
        float step = d.rate * dt;
        d.level = d.level < goal ? std::min(d.level + step, goal) : std::max(d.level - step, goal);
        impl_->applyBus(b);
    }
    // Music layers: fading towards their volumes, all dipped while a stinger plays
    impl_->duckLayersLeft = std::max(impl_->duckLayersLeft - dt, 0.f);
    float duckGoal = impl_->duckLayersLeft > 0.f ? impl_->duckLayers : 1.f;
    float duckStep = dt * 4.f;
    float& duckNow = impl_->duckLayersLevel;
    duckNow = duckNow < duckGoal ? std::min(duckNow + duckStep, duckGoal) : std::max(duckNow - duckStep, duckGoal);
    for (Impl::Layer& l : impl_->layers) {
        float step = l.rate > 0.f ? l.rate * dt : 1e9f;
        l.volume = l.volume < l.target ? std::min(l.volume + step, l.target) : std::max(l.volume - step, l.target);
        auto it = impl_->sounds.find(l.sound);
        if (it != impl_->sounds.end() && it->second->stopIn < 0.f)
            ma_sound_set_volume(&it->second->sound, l.volume * duckNow * it->second->gain);
    }
    for (auto it = impl_->sounds.begin(); it != impl_->sounds.end();) {
        auto next = std::next(it);
        Impl::Playing& s = *it->second;
        if (s.stopIn >= 0.f) {
            s.stopIn -= dt;
            if (s.stopIn <= 0.f) impl_->release(it);
        } else if (!s.loop && ma_sound_at_end(&s.sound)) {
            impl_->release(it);
        }
        it = next;
    }
}

size_t AudioEngine::activeSounds() const { return impl_->sounds.size(); }

size_t AudioEngine::decodedClips() const { return impl_->clips.size(); }

bool AudioEngine::readFrames(float* stereoOut, uint32_t frames) {
    if (!impl_->ready) return false;
    ma_uint64 read = 0;
    return ma_engine_read_pcm_frames(&impl_->engine, stereoOut, frames, &read) == MA_SUCCESS;
}
