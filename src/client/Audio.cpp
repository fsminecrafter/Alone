#include "client/Audio.h"
#include "raylib.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace alone {

namespace {

constexpr int kRate = 22050;

// --- ambience, generated on the audio thread ---------------------------------
std::atomic<float> g_rain{0}, g_wind{0}, g_fire{0}, g_master{0};
AudioStream g_stream{};
uint32_t g_seed = 12345;

inline float noise() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return (float)((g_seed >> 9) & 0xFFFF) / 32768.0f - 1.0f;
}

void ambienceCallback(void* buffer, unsigned int frames) {
    static float rainLp = 0, windBrown = 0, windLp = 0, lfo = 0, crackle = 0, rumble = 0;
    short* out = (short*)buffer;
    float rain = g_rain, wind = g_wind, fire = g_fire, master = g_master;
    for (unsigned int i = 0; i < frames; ++i) {
        float n = noise();
        // rain: lightly filtered hiss with patter
        rainLp += 0.55f * (n - rainLp);
        float r = rainLp * 0.6f;
        if (noise() > 0.9993f) r += noise() * 0.8f;
        // wind: brown noise, slowly gusting
        windBrown = windBrown * 0.997f + noise() * 0.03f;
        windLp += 0.02f * (windBrown - windLp);
        lfo += 1.0f / kRate * 0.15f;
        float gust = 0.6f + 0.4f * std::sin(lfo * 6.283f) * std::sin(lfo * 2.7f);
        float w = windLp * 6.0f * gust;
        // fire: pops and a low rumble
        if (noise() > 0.9985f) crackle = noise() * 0.9f;
        crackle *= 0.965f;
        rumble += 0.01f * (noise() - rumble);
        float f = crackle + rumble * 1.5f;
        float v = (r * rain * 0.5f + w * wind * 0.55f + f * fire * 0.6f) * master;
        v = std::fmax(-1.0f, std::fmin(1.0f, v));
        out[i] = (short)(v * 30000);
    }
}

// --- one-shot effects -----------------------------------------------------
Sound makeSound(const std::vector<float>& samples) {
    Wave w{};
    w.frameCount = (unsigned)samples.size();
    w.sampleRate = kRate;
    w.sampleSize = 16;
    w.channels = 1;
    short* data = (short*)MemAlloc((unsigned)(samples.size() * sizeof(short)));
    for (size_t i = 0; i < samples.size(); ++i) data[i] = (short)(std::fmax(-1.0f, std::fmin(1.0f, samples[i])) * 30000);
    w.data = data;
    Sound s = LoadSoundFromWave(w);
    UnloadWave(w);
    return s;
}

std::vector<float> synth(Sfx type, int variant) {
    std::vector<float> s;
    auto len = [&](float sec) { s.assign((size_t)(sec * kRate), 0.0f); };
    float lp = 0, ph = 0;
    g_seed = 777u + (uint32_t)type * 97u + (uint32_t)variant * 13u;
    switch (type) {
        case Sfx::Footstep: {
            len(0.11f);
            float cutoff = 0.12f + 0.05f * variant;
            for (size_t i = 0; i < s.size(); ++i) {
                float t = i / (float)kRate;
                lp += cutoff * (noise() - lp);
                s[i] = lp * std::exp(-t * 45.0f) * 0.9f;
            }
            break;
        }
        case Sfx::Gunshot: {
            len(1.4f);
            for (size_t i = 0; i < s.size(); ++i) {
                float t = i / (float)kRate;
                lp += 0.35f * (noise() - lp);
                float crack = noise() * std::exp(-t * 60.0f);
                float boom = std::sin(t * 2 * 3.14159f * (70.0f - 30.0f * t)) * std::exp(-t * 8.0f);
                float tail = lp * std::exp(-t * 3.5f) * 0.4f;
                s[i] = crack * 1.0f + boom * 0.8f + tail;
            }
            break;
        }
        case Sfx::Swing: {
            len(0.22f);
            for (size_t i = 0; i < s.size(); ++i) {
                float t = i / (float)s.size();
                lp += (0.05f + 0.25f * t) * (noise() - lp);
                s[i] = lp * std::sin(t * 3.14159f) * 0.7f;
            }
            break;
        }
        case Sfx::Hit: case Sfx::Chop: {
            len(0.16f);
            float f = type == Sfx::Chop ? 190.0f : 110.0f;
            for (size_t i = 0; i < s.size(); ++i) {
                float t = i / (float)kRate;
                ph += f * (1.0f - t * 2) / kRate;
                s[i] = std::sin(ph * 6.283f) * std::exp(-t * 30.0f) * 0.9f + noise() * std::exp(-t * 90.0f) * 0.5f;
            }
            break;
        }
        case Sfx::Door: {
            len(0.45f);
            for (size_t i = 0; i < s.size(); ++i) {
                float t = i / (float)kRate;
                float creakF = 260.0f - 120.0f * t + 15.0f * std::sin(t * 40.0f);
                ph += creakF / kRate;
                float saw = 2.0f * (ph - std::floor(ph)) - 1.0f;
                lp += 0.1f * (saw - lp);
                float creak = lp * (t < 0.35f ? std::sin(t / 0.35f * 3.14159f) : 0.0f) * 0.4f;
                float thump = t > 0.33f ? std::sin((t - 0.33f) * 2 * 3.14159f * 80.0f) * std::exp(-(t - 0.33f) * 40.0f) * 0.8f : 0.0f;
                s[i] = creak + thump;
            }
            break;
        }
        case Sfx::Click: {
            len(0.04f);
            for (size_t i = 0; i < s.size(); ++i) s[i] = noise() * std::exp(-i / (float)kRate * 150.0f) * 0.6f;
            break;
        }
        case Sfx::Eat: {
            len(0.5f);
            for (size_t i = 0; i < s.size(); ++i) {
                float t = i / (float)kRate;
                float bite = std::fmod(t, 0.16f);
                lp += 0.3f * (noise() - lp);
                s[i] = lp * std::exp(-bite * 35.0f) * 0.6f;
            }
            break;
        }
        case Sfx::Drink: {
            len(0.6f);
            for (size_t i = 0; i < s.size(); ++i) {
                float t = i / (float)kRate;
                float g = std::fmod(t, 0.2f);
                ph += (180.0f + 300.0f * g) / kRate;
                s[i] = std::sin(ph * 6.283f) * std::exp(-g * 25.0f) * 0.45f;
            }
            break;
        }
        case Sfx::Hurt: {
            len(0.35f);
            for (size_t i = 0; i < s.size(); ++i) {
                float t = i / (float)kRate;
                ph += (150.0f - 60.0f * t) / kRate;
                float saw = 2.0f * (ph - std::floor(ph)) - 1.0f;
                lp += 0.08f * (saw - lp);
                s[i] = lp * std::sin(std::fmin(1.0f, t / 0.35f) * 3.14159f) * 1.2f;
            }
            break;
        }
        case Sfx::Glass: {
            // a crack, then shards tinkling down
            len(0.9f);
            for (size_t i = 0; i < s.size(); ++i) {
                float t = i / (float)kRate;
                float crack = noise() * std::exp(-t * 40.0f);
                float tinkle = 0;
                for (int k = 0; k < 5; ++k) {
                    float t0 = 0.05f + 0.13f * k + 0.03f * variant;
                    if (t > t0) tinkle += std::sin((t - t0) * 6.283f * (2600.0f + 700.0f * k)) * std::exp(-(t - t0) * 30.0f) * 0.25f;
                }
                s[i] = crack * 0.9f + tinkle;
            }
            break;
        }
        case Sfx::Engine: {
            // the starter grinding, then the engine catching and idling
            len(1.6f);
            for (size_t i = 0; i < s.size(); ++i) {
                float t = i / (float)kRate;
                float f = t < 0.5f ? 14.0f + 4.0f * std::sin(t * 30.0f) : 28.0f + 10.0f * std::exp(-(t - 0.5f) * 4.0f);
                ph += f / kRate;
                float pulse = std::pow(std::fabs(std::sin(ph * 3.14159f)), 6.0f);
                lp += 0.15f * (noise() - lp);
                float env = std::fmin(1.0f, t * 8.0f) * (t > 1.3f ? std::fmax(0.0f, 1.0f - (t - 1.3f) / 0.3f) : 1.0f);
                s[i] = (pulse * 0.7f + lp * 0.5f) * env;
            }
            break;
        }
        case Sfx::Bowshot: {
            // the string's twang and the arrow's hiss
            len(0.35f);
            for (size_t i = 0; i < s.size(); ++i) {
                float t = i / (float)kRate;
                ph += (95.0f + 10.0f * variant) / kRate;
                lp += 0.4f * (noise() - lp);
                s[i] = std::sin(ph * 6.283f) * std::exp(-t * 18.0f) * 0.6f + lp * std::exp(-t * 9.0f) * 0.25f;
            }
            break;
        }
    }
    return s;
}

} // namespace

void Audio::init() {
    if (!IsAudioDeviceReady()) return;
    SetAudioStreamBufferSizeDefault(2048);
    g_stream = LoadAudioStream(kRate, 16, 1);
    SetAudioStreamCallback(g_stream, ambienceCallback);
    PlayAudioStream(g_stream);
    for (int t = 0; t < kEffects; ++t)
        for (int v = 0; v < 3; ++v) {
            effects_[t][v].sound = makeSound(synth((Sfx)t, v));
            effects_[t][v].loaded = true;
        }
    ready_ = true;
}

void Audio::shutdown() {
    if (!ready_) return;
    for (auto& t : effects_)
        for (auto& e : t)
            if (e.loaded) UnloadSound(e.sound);
    UnloadAudioStream(g_stream);
    ready_ = false;
}

void Audio::update(Game& game, float volume) {
    if (!ready_ || !game.running()) return;
    World& w = game.world();
    bool inside = w.indoors(game.player().pos);
    const Weather& wx = w.clock().weather();
    float rain = wx.rain * (inside ? 0.35f : 1.0f);
    float wind = std::fmin(1.0f, wx.wind / 9.0f) * (inside ? 0.3f : 1.0f);
    float fire = 0;
    for (const auto& f : w.campfires())
        if (f.lit) fire = std::fmax(fire, 1.0f - distance(f.pos, game.player().pos) / 15.0f);
    g_rain = rain;
    g_wind = wind * 0.7f;
    g_fire = std::fmax(0.0f, fire);
    g_master = volume;
}

void Audio::play(const SoundEvent& e, Vec2 listener, float volume) {
    if (!ready_) return;
    int t = (int)e.type;
    if (t < 0 || t >= kEffects) return;
    float d = distance(e.pos, listener);
    float vol = volume / (1.0f + d / 12.0f);
    if (e.type == Sfx::Footstep) vol *= 0.35f;
    if (e.type == Sfx::Gunshot) vol = volume / (1.0f + d / 60.0f);   // carries far
    Sound& s = effects_[t][GetRandomValue(0, 2)].sound;
    SetSoundVolume(s, vol);
    SetSoundPitch(s, 0.92f + GetRandomValue(0, 16) / 100.0f);
    PlaySound(s);
}

} // namespace alone
