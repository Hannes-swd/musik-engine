#include "PianoSynth.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
float midiToFreq(int pitch) { return 440.f * std::pow(2.f, (pitch - 69) / 12.f); }
}

PianoSynth::PianoSynth() : voices_(kMaxVoices) {}

void PianoSynth::noteOn(int pitch, float velocity) {
    // Gleicher Ton: neu anschlagen; sonst freie Stimme, sonst die älteste (bevorzugt losgelassene)
    Voice* v = nullptr;
    for (auto& cand : voices_) if (cand.active && cand.pitch == pitch) { v = &cand; break; }
    if (!v) for (auto& cand : voices_) if (!cand.active) { v = &cand; break; }
    if (!v) v = &*std::max_element(voices_.begin(), voices_.end(), [](const Voice& a, const Voice& b) {
            if (a.released != b.released) return !a.released;
            return a.time < b.time;
        });

    *v = Voice{};
    v->active = true;
    v->pitch = pitch;
    v->velocity = std::clamp(velocity, 0.f, 1.f);
    v->freq = midiToFreq(pitch);
    v->pan = std::clamp(0.5f + (pitch - 60) / 80.f, 0.15f, 0.85f);
    v->noise = 0x9E3779B9u ^ static_cast<unsigned>(pitch * 7919 + int(velocity * 977.f));

    const float sr = sampleRate_;
    const float vel = v->velocity;
    // Saiten pro Ton: Bass 1, Mitte 2, ab dem Tenor 3 – leicht verstimmt (mehr in der Höhe)
    v->strings = pitch < 34 ? 1 : pitch < 48 ? 2 : 3;
    const float detuneCents = 0.6f + 0.02f * float(std::max(0, pitch - 40));
    const float B = 0.00008f * std::pow(2.f, float(pitch - 21) / 24.f);   // Inharmonizität (höher = steifer)
    const float bright = 0.35f + 0.6f * vel;                                // kräftiger Anschlag = heller
    // Ausklingen: tiefe Töne lang, hohe kurz; zwei Phasen (Prompt + Nachklang)
    const float sustainT = std::clamp(14.f * std::pow(0.5f, float(pitch - 33) / 14.f), 0.9f, 16.f);
    int n = 0;
    for (int s = 0; s < v->strings; ++s) {
        const float det = v->strings == 1 ? 0.f : (float(s) / float(v->strings - 1) - 0.5f) * 2.f * detuneCents;
        const float detRatio = std::pow(2.f, det / 1200.f);
        for (int h = 0; h < kHarmonics; ++h) {
            const float k = float(h + 1);
            const float f = v->freq * k * std::sqrt(1.f + B * k * k) * detRatio;
            if (f > sr * 0.45f) break;   // kein Aliasing
            Partial& P = v->p[size_t(n++)];
            const float w = kTwoPi * f / sr;
            P.c = std::cos(w);
            P.s = std::sin(w);
            P.x = std::cos(float(s) * 1.7f + float(h) * 0.9f);   // verschiedene Startphasen
            P.y = std::sin(float(s) * 1.7f + float(h) * 0.9f);
            // Spektrum: fällt mit der Ordnung (anschlagabhängig); Hammer bei 1/8 der Saite -> 8. Oberton schwach
            float a = std::pow(bright, float(h) * 0.85f) / std::pow(k, 0.9f);
            if ((h + 1) % 8 == 0) a *= 0.25f;
            a /= float(v->strings);
            const float tSlow = sustainT / (1.f + 0.35f * float(h));
            const float tFast = std::max(0.08f, tSlow * 0.12f);
            P.fast = a * 0.55f;
            P.slow = a * 0.45f;
            P.kFast = std::exp(-1.f / (tFast * sr));
            P.kSlow = std::exp(-1.f / (tSlow * sr));
        }
    }
    v->partials = n;
    v->releaseMul = std::exp(-1.f / ((pitch > 88 ? 0.5f : 0.11f) * sr));   // ganz oben keine Dämpfer
}

void PianoSynth::noteOff(int pitch) {
    for (auto& v : voices_)
        if (v.active && !v.released && v.pitch == pitch) {
            v.released = true;
            v.thump = 0.02f * v.velocity;   // Dämpfer legt sich auf die Saite
        }
}

void PianoSynth::allNotesOff() {
    for (auto& v : voices_) v.released = true;
}

void PianoSynth::render(float* out, int frameCount) {
    const float dt = 1.f / sampleRate_;
    const float hammerA = std::min(1.f, 2.f * kTwoPi * 3000.f / sampleRate_);
    for (auto& v : voices_) {
        if (!v.active) continue;
        const float gain = (0.45f + 0.55f * v.velocity) * 1.21f;   // kalibriert auf ca. -23 dB RMS wie alle Instrumente
        for (int i = 0; i < frameCount; ++i) {
            float s = 0;
            for (int k = 0; k < v.partials; ++k) {
                Partial& P = v.p[size_t(k)];
                const float nx = P.x * P.c - P.y * P.s;
                P.y = P.x * P.s + P.y * P.c;
                P.x = nx;
                P.fast *= P.kFast;
                P.slow *= P.kSlow;
                s += P.y * (P.fast + P.slow);
            }
            // Hammer: kurzer gefilterter Rauschimpuls (heller bei kräftigem Anschlag)
            if (v.time < 0.015f) {
                v.noise = v.noise * 1664525u + 1013904223u;
                const float nz = (float(v.noise >> 9) / 4194304.f) - 1.f;
                v.hammerLp += (nz - v.hammerLp) * hammerA * (0.4f + 0.6f * v.velocity);
                s += v.hammerLp * 0.12f * v.velocity * (1.f - v.time / 0.015f);
            }
            // Dämpfer-Geräusch beim Loslassen
            if (v.thump > 1e-5f) {
                v.noise = v.noise * 1664525u + 1013904223u;
                s += ((float(v.noise >> 9) / 4194304.f) - 1.f) * v.thump;
                v.thump *= 0.995f;
            }
            const float attack = std::min(1.f, v.time / 0.0015f);
            if (v.released) v.releaseGain *= v.releaseMul;
            s *= attack * v.releaseGain * gain;
            out[2 * i] += s * (1.f - v.pan);
            out[2 * i + 1] += s * v.pan;
            v.time += dt;
        }
        // Zeiger gelegentlich auf Länge 1 bringen (Rundungsfehler)
        if (++v.renorm >= 8) {
            v.renorm = 0;
            for (int k = 0; k < v.partials; ++k) {
                Partial& P = v.p[size_t(k)];
                const float len = std::sqrt(P.x * P.x + P.y * P.y);
                if (len > 0.f) { P.x /= len; P.y /= len; }
            }
        }
        float level = 0;
        for (int k = 0; k < v.partials; ++k) level += v.p[size_t(k)].fast + v.p[size_t(k)].slow;
        if (v.releaseGain < 0.0005f || level < 0.0002f || v.time > 20.f) v.active = false;
    }
}
