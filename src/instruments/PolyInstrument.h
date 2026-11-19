#pragma once
// Vorlage für polyphone Instrumente.
//
// Ein neues Instrument braucht nur eine Voice-Klasse mit:
//   void start(int pitch, float velocity, float sampleRate);
//   void release();            // Taste losgelassen
//   float next();              // nächstes Mono-Sample
//   bool finished() const;     // Stimme ist ausgeklungen
// und eine kleine Klasse, die von PolyInstrument<Voice> erbt und name() liefert.
// Stimmenverwaltung (Stealing, Retrigger), Stereo-Panorama und Mischung übernimmt die Vorlage.
#include "IInstrument.h"
#include <algorithm>
#include <cstdint>
#include <vector>

template <class Voice>
class PolyInstrument : public IInstrument {
public:
    explicit PolyInstrument(int maxVoices = 24, float gain = 0.25f, float stereoWidth = 0.6f)
        : slots_(size_t(maxVoices)), gain_(gain), width_(stereoWidth) {}

    void setSampleRate(float sr) override { sr_ = sr; }

    void noteOn(int pitch, float velocity) override {
        Slot* s = nullptr;
        if (!stackSamePitch_)
            for (auto& c : slots_) if (c.active && c.pitch == pitch) { s = &c; break; }   // gleicher Ton: neu anschlagen
        if (!s) for (auto& c : slots_) if (!c.active) { s = &c; break; }
        if (!s) {   // älteste Stimme übernehmen (bevorzugt losgelassene)
            s = &*std::min_element(slots_.begin(), slots_.end(), [](const Slot& a, const Slot& b) {
                if (a.released != b.released) return a.released;
                return a.age < b.age;
            });
        }
        s->active = true;
        s->released = false;
        s->pitch = pitch;
        s->age = ++counter_;
        const float p = 0.5f + std::clamp((pitch - 64) / 48.f, -0.5f, 0.5f) * width_;
        s->panL = std::sqrt(1.f - p);
        s->panR = std::sqrt(p);
        s->voice.start(pitch, std::clamp(velocity, 0.f, 1.f), sr_);
        onNoteOn(pitch);
    }

    void noteOff(int pitch) override {
        for (auto& s : slots_)
            if (s.active && !s.released && s.pitch == pitch) { s.released = true; s.voice.release(); }
    }

    void allNotesOff() override {
        for (auto& s : slots_)
            if (s.active && !s.released) { s.released = true; s.voice.release(); }
    }

    void render(float* out, int frames) override {
        for (auto& s : slots_) {
            if (!s.active) continue;
            const float l = s.panL * gain_, r = s.panR * gain_;
            for (int i = 0; i < frames; ++i) {
                const float x = s.voice.next();
                out[2 * i] += x * l;
                out[2 * i + 1] += x * r;
            }
            if (s.voice.finished()) s.active = false;
        }
    }

protected:
    struct Slot {
        Voice voice;
        bool active = false, released = false;
        int pitch = 0;
        uint64_t age = 0;
        float panL = 0.7f, panR = 0.7f;
    };
    virtual void onNoteOn(int /*pitch*/) {}   // z.B. Hi-Hat-Choke beim Schlagzeug

    std::vector<Slot> slots_;
    float sr_ = 48000.f;
    float gain_;
    float width_;
    uint64_t counter_ = 0;
    bool stackSamePitch_ = false;   // true: gleicher Ton darf mehrfach überlappend klingen (Pauken-Wirbel)
};
