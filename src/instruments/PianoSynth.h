#pragma once
#include "IInstrument.h"
#include <array>
#include <vector>

// Additiver Konzertflügel:
//  - bis zu 3 Saiten pro Ton (Chor), leicht gegeneinander verstimmt -> das typische lebendige Schweben
//  - 16 leicht inharmonische Obertöne (Saitensteifigkeit), Helligkeit hängt vom Anschlag ab
//  - zweistufiges Ausklingen wie bei echten Saiten (schneller "Prompt"-Anteil + langer Nachklang)
//  - Hammergeräusch beim Anschlag, Dämpfergeräusch beim Loslassen
//  Sinus-Oszillatoren als rotierende Zeiger (schnell, ohne sin()/exp() pro Sample).
class PianoSynth : public IInstrument {
public:
    PianoSynth();

    std::string name() const override { return "Piano"; }
    void setSampleRate(float sampleRate) override { sampleRate_ = sampleRate; }
    void noteOn(int pitch, float velocity) override;
    void noteOff(int pitch) override;
    void allNotesOff() override;
    void render(float* out, int frameCount) override;

private:
    static constexpr int kHarmonics = 16;
    static constexpr int kStrings = 3;
    static constexpr int kMaxVoices = 32;

    struct Partial {
        float x = 1.f, y = 0.f;       // rotierender Zeiger (cos, sin)
        float c = 1.f, s = 0.f;       // Drehung pro Sample
        float fast = 0.f, slow = 0.f; // Amplituden: schneller + langsamer Abklinganteil
        float kFast = 1.f, kSlow = 1.f;
    };

    struct Voice {
        bool active = false;
        bool released = false;
        int pitch = 0;
        float velocity = 0;
        float freq = 0;
        float time = 0;              // Sekunden seit noteOn
        float releaseGain = 1;       // fällt nach noteOff gegen 0
        float releaseMul = 1;
        float pan = 0.5f;
        int partials = 0;
        int strings = 1;
        std::array<Partial, kHarmonics * kStrings> p{};
        unsigned noise = 12345;
        float hammerLp = 0.f;        // gefiltertes Hammergeräusch
        float thump = 0.f;           // Dämpfergeräusch
        int renorm = 0;
    };

    std::vector<Voice> voices_;
    float sampleRate_ = 48000.f;
};
