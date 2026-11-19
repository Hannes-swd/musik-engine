#pragma once
// Kleine DSP-Bausteine für die Synthesizer-Instrumente.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <atomic>

namespace dsp {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

inline float midiToFreq(float pitch) { return 440.f * std::pow(2.f, (pitch - 69.f) / 12.f); }
inline float centsToRatio(float cents) { return std::pow(2.f, cents / 1200.f); }

// Schnelles Rauschen (-1..1)
struct Noise {
    uint32_t s = 0x1234567u;
    void seed(uint32_t v) { s = v ? v : 1u; }
    float next() {
        s = s * 1664525u + 1013904223u;
        return float(int32_t(s)) * (1.f / 2147483648.f);
    }
};

// PolyBLEP-Korrektur gegen Aliasing bei Sägezahn/Rechteck
inline float polyBlep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
}

enum class Wave { Sine, Saw, Square, Triangle };

struct Osc {
    float phase = 0.f;   // 0..1
    float tri = 0.f;     // Integrator für Dreieck
    float next(float freq, float sr, Wave w, float pw = 0.5f) {
        const float dt = std::min(freq / sr, 0.49f);
        float out;
        switch (w) {
        case Wave::Sine: out = std::sin(kTwoPi * phase); break;
        case Wave::Saw: out = 2.f * phase - 1.f - polyBlep(phase, dt); break;
        case Wave::Square: {
            out = phase < pw ? 1.f : -1.f;
            out += polyBlep(phase, dt);
            float t2 = phase - pw;
            if (t2 < 0.f) t2 += 1.f;
            out -= polyBlep(t2, dt);
            break;
        }
        default: {   // Dreieck = integriertes Rechteck
            float sq = phase < 0.5f ? 1.f : -1.f;
            sq += polyBlep(phase, dt);
            float t2 = phase + 0.5f;
            if (t2 >= 1.f) t2 -= 1.f;
            sq -= polyBlep(t2, dt);
            tri = dt * 4.f * sq + (1.f - dt * 0.5f) * tri;
            out = tri;
            break;
        }
        }
        phase += dt;
        if (phase >= 1.f) phase -= 1.f;
        return out;
    }
};

// ADSR-Hüllkurve (Zeiten in Sekunden, Sustain 0..1)
struct ADSR {
    float attack = 0.01f, decay = 0.2f, sustain = 0.7f, release = 0.2f;
    enum Stage { Idle, Attack, Decay, Sustain, Release } stage = Idle;
    float value = 0.f;

    void start(float sr) {
        sr_ = sr;
        attInc_ = 1.f / std::max(1.f, attack * sr);
        decCoef_ = coef(decay);
        relCoef_ = coef(release);
        stage = Attack;
    }
    void noteOff() { if (stage != Idle) stage = Release; }
    void setRelease(float sec) { release = sec; relCoef_ = coef(sec); }
    bool idle() const { return stage == Idle; }

    float next() {
        switch (stage) {
        case Attack:
            value += attInc_;
            if (value >= 1.f) { value = 1.f; stage = Decay; }
            break;
        case Decay:
            value = sustain + (value - sustain) * decCoef_;
            if (value - sustain < 1e-4f) stage = sustain <= 0.f ? Idle : Sustain;   // ohne Sustain: ausgeklungen
            break;
        case Sustain: value = sustain; break;
        case Release:
            value *= relCoef_;
            if (value < 1e-4f) { value = 0.f; stage = Idle; }
            break;
        default: break;
        }
        return value;
    }

private:
    float coef(float sec) const { return std::exp(-1.f / std::max(1.f, sec * sr_ / 4.6f)); }   // ~-40 dB nach 'sec'
    float sr_ = 48000.f, attInc_ = 0.f, decCoef_ = 0.f, relCoef_ = 0.f;
};

// Exponentieller Abfall (für perkussive Klänge)
struct Decay {
    float value = 0.f, coef = 0.f;
    void start(float level, float seconds, float sr) {
        value = level;
        coef = std::exp(-1.f / std::max(1.f, seconds * sr / 4.6f));
    }
    float next() { float v = value; value *= coef; return v; }
};

// State-Variable-Filter (TPT): Tief-, Band- und Hochpass gleichzeitig
struct SVF {
    float ic1 = 0.f, ic2 = 0.f;
    float a1 = 0.f, a2 = 0.f, a3 = 0.f, k = 1.f;
    float lp = 0.f, bp = 0.f, hp = 0.f;

    void set(float cutoff, float resonance, float sr) {
        cutoff = std::clamp(cutoff, 20.f, sr * 0.45f);
        const float g = std::tan(kPi * cutoff / sr);
        k = 2.f - 2.f * std::clamp(resonance, 0.f, 0.98f);
        a1 = 1.f / (1.f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    float process(float x) {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.f * v1 - ic1;
        ic2 = 2.f * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = x - k * v1 - v2;
        return lp;
    }
};

// Sanfte Sättigung
inline float softClip(float x) { return x / (1.f + std::abs(x)); }

// Jede Note etwas anders – wie bei echten Musikern: Vibrato-Tempo und -Phase sowie eine winzige
// Verstimmung werden pro Anschlag leicht gewürfelt (sonst schwingen alle Töne exakt im Gleichschritt).
struct Humanize {
    float rate = 1.f, phase = 0.f, cents = 0.f;
    void init(int pitch, float maxCents = 3.f) {
        static std::atomic<uint32_t> counter{1};
        Noise n;
        n.seed(uint32_t(pitch) * 2654435761u + counter.fetch_add(7919u));
        n.next();
        rate = 1.f + n.next() * 0.09f;
        phase = (n.next() + 1.f) * kPi;
        cents = n.next() * maxCents;
    }
};

}   // namespace dsp
