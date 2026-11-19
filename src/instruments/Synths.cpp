// Synthesizer-Instrumente. Jedes Instrument = eine Voice-Struktur + eine kleine Klasse.
// Neues Instrument hinzufügen: Voice schreiben, Klasse mit DEFINE_INSTRUMENT anlegen,
// in registerSynths() eintragen. Fertig.
#include "Synths.h"
#include "DSP.h"
#include "InstrumentRegistry.h"
#include "PolyInstrument.h"

#include <array>
#include <cmath>
#include <memory>

using namespace dsp;

namespace {

// Hilfsmakro: Instrument-Klasse aus einer Voice erzeugen
#define DEFINE_INSTRUMENT(Class, VoiceT, Name, Program, Voices, Gain, Sustains)        \
    class Class : public PolyInstrument<VoiceT> {                                     \
    public:                                                                           \
        Class() : PolyInstrument<VoiceT>(Voices, Gain) {}                             \
        std::string name() const override { return Name; }                            \
        int midiProgram() const override { return Program; }                          \
        bool sustains() const override { return Sustains; }                           \
    };

// --------------------------------------------------------------------
// E-Piano: 2-Operator-FM mit glockigem "Tine"-Anschlag (Rhodes-artig)
struct EPianoVoice {
    float sr = 48000, f = 440, t = 0, vel = 1, pc = 0, pm = 0, pt = 0;
    ADSR env;
    void start(int pitch, float v, float s) {
        sr = s; f = midiToFreq(float(pitch)); vel = v; t = 0; pc = pm = pt = 0;
        env.attack = 0.002f;
        env.decay = std::clamp(1.6f + (72 - pitch) * 0.05f, 0.5f, 4.f);
        env.sustain = 0.f;
        env.release = 0.25f;
        env.start(sr);
    }
    void release() { env.noteOff(); }
    float next() {
        const float e = env.next();
        const float idx = (0.6f + vel * 2.0f) * std::exp(-t * 4.f) + 0.2f * vel;
        const float tine = std::sin(kTwoPi * pt) * 0.22f * vel * std::exp(-t * 28.f);
        const float out = std::sin(kTwoPi * pc + idx * std::sin(kTwoPi * pm)) + tine;
        const float d = f / sr;
        pc += d; pm += d; pt += d * 14.f;
        pc -= std::floor(pc); pm -= std::floor(pm); pt -= std::floor(pt);
        t += 1.f / sr;
        return out * e * (0.45f + 0.55f * vel);
    }
    bool finished() const { return env.idle(); }
};
DEFINE_INSTRUMENT(EPiano, EPianoVoice, "E-Piano", 4, 24, 0.28f, false)

// --------------------------------------------------------------------
// Orgel: Zugriegel (16', 8', 5 1/3', 4', 2 2/3', 2'), Tastenklick, Leslie-Tremolo
struct OrganVoice {
    float sr = 48000, f = 440, t = 0, vel = 1;
    Humanize hz;   // pro Anschlag leicht anders
    std::array<float, 6> ph{};
    ADSR env;
    Noise noise;
    void start(int pitch, float v, float s) {
        sr = s; f = midiToFreq(float(pitch)); vel = v; t = 0; ph.fill(0);
        env.attack = 0.006f; env.decay = 0.1f; env.sustain = 1.f; env.release = 0.07f;
        env.start(sr);
        noise.seed(uint32_t(pitch * 7919 + 1));
    }
    void release() { env.noteOff(); }
    float next() {
        static const float ratio[6] = {0.5f, 1.f, 1.5f, 2.f, 3.f, 4.f};
        static const float amp[6] = {0.5f, 1.f, 0.35f, 0.55f, 0.3f, 0.22f};
        float s = 0;
        for (int i = 0; i < 6; ++i) {
            if (f * ratio[i] > sr * 0.45f) continue;
            s += std::sin(kTwoPi * ph[i]) * amp[i];
            ph[i] += f * ratio[i] / sr;
            ph[i] -= std::floor(ph[i]);
        }
        s *= 0.35f;
        if (t < 0.004f) s += noise.next() * 0.15f * (1.f - t / 0.004f);   // Klick
        const float leslie = 1.f + 0.12f * std::sin(kTwoPi * 6.3f * t);
        t += 1.f / sr;
        return s * env.next() * leslie * (0.6f + 0.4f * vel);
    }
    bool finished() const { return env.idle(); }
};
DEFINE_INSTRUMENT(Organ, OrganVoice, "Orgel", 16, 24, 0.21f, true)

// --------------------------------------------------------------------
// Streicher: drei verstimmte Sägezähne, verzögertes Vibrato, Tiefpass
struct StringsVoice {
    float sr = 48000, f = 440, t = 0, vel = 1;
    Humanize hz;   // pro Anschlag leicht anders
    std::array<Osc, 3> osc;
    SVF filt;
    ADSR env;
    void start(int pitch, float v, float s) {
        sr = s; f = midiToFreq(float(pitch)); vel = v; t = 0; hz.init(pitch); f *= centsToRatio(hz.cents);
        osc[0].phase = 0.f; osc[1].phase = 0.33f; osc[2].phase = 0.71f;
        env.attack = 0.35f - 0.2f * vel; env.decay = 0.5f; env.sustain = 0.85f; env.release = 0.55f;
        env.start(sr);
        filt = SVF{};
        filt.set(std::min(f * 5.f + 1200.f + 2000.f * vel, 9000.f), 0.12f, sr);
    }
    void release() { env.noteOff(); }
    float next() {
        const float vibDepth = std::clamp((t - 0.3f) * 2.f, 0.f, 1.f) * 12.f;   // Cent
        const float vib = centsToRatio(vibDepth * std::sin(kTwoPi * 5.3f * hz.rate * t + hz.phase));
        const float s = osc[0].next(f * vib * 0.9954f, sr, Wave::Saw) + osc[1].next(f * vib, sr, Wave::Saw) +
                        osc[2].next(f * vib * 1.0041f, sr, Wave::Saw);
        t += 1.f / sr;
        return filt.process(s * 0.33f) * env.next();
    }
    bool finished() const { return env.idle(); }
};
DEFINE_INSTRUMENT(Strings, StringsVoice, "Streicher", 48, 24, 0.26f, true)

// --------------------------------------------------------------------
// Pad: breite, weiche Fläche mit langsam wanderndem Filter
struct PadVoice {
    float sr = 48000, f = 440, t = 0, vel = 1;
    Humanize hz;   // pro Anschlag leicht anders
    std::array<Osc, 5> osc;
    SVF filt;
    ADSR env;
    int counter = 0;
    void start(int pitch, float v, float s) {
        sr = s; f = midiToFreq(float(pitch)); vel = v; t = 0; counter = 0;
        for (int i = 0; i < 5; ++i) osc[size_t(i)].phase = float(i) * 0.21f;
        env.attack = 0.8f; env.decay = 1.f; env.sustain = 0.8f; env.release = 1.6f;
        env.start(sr);
        filt = SVF{};
    }
    void release() { env.noteOff(); }
    float next() {
        if ((counter++ & 31) == 0) {
            const float sweep = 0.5f + 0.5f * std::sin(kTwoPi * 0.13f * t);
            filt.set(600.f + 1600.f * sweep + 1200.f * vel, 0.25f, sr);
        }
        static const float det[4] = {-12.f, -5.f, 5.f, 12.f};
        float s = 0;
        for (int i = 0; i < 4; ++i) s += osc[size_t(i)].next(f * centsToRatio(det[i]), sr, Wave::Saw);
        s += osc[4].next(f * 0.5f, sr, Wave::Triangle) * 0.6f;
        t += 1.f / sr;
        return filt.process(s * 0.25f) * env.next();
    }
    bool finished() const { return env.idle(); }
};
DEFINE_INSTRUMENT(Pad, PadVoice, "Pad", 89, 24, 0.36f, true)

// --------------------------------------------------------------------
// Synth-Bass: Sägezahn + Rechteck + Sub, resonanter Filter mit Hüllkurve
struct SynthBassVoice {
    float sr = 48000, f = 110, vel = 1;
    Osc saw, sq, sub;
    SVF filt;
    ADSR env;
    Decay fenv;
    int counter = 0;
    void start(int pitch, float v, float s) {
        sr = s; f = midiToFreq(float(pitch)); vel = v; counter = 0;
        saw.phase = sq.phase = sub.phase = 0;
        env.attack = 0.002f; env.decay = 0.35f; env.sustain = 0.65f; env.release = 0.07f;
        env.start(sr);
        fenv.start(1.f, 0.25f, sr);
        filt = SVF{};
    }
    void release() { env.noteOff(); }
    float next() {
        const float fe = fenv.next();
        if ((counter++ & 7) == 0) filt.set(120.f + f * 1.5f + fe * 2600.f * vel, 0.5f, sr);
        const float s = saw.next(f, sr, Wave::Saw) * 0.6f + sq.next(f, sr, Wave::Square, 0.3f) * 0.35f;
        const float x = filt.process(s) + sub.next(f * 0.5f, sr, Wave::Sine) * 0.5f;
        return softClip(x * 1.4f) * env.next() * (0.55f + 0.45f * vel);
    }
    bool finished() const { return env.idle(); }
};
DEFINE_INSTRUMENT(SynthBass, SynthBassVoice, "Synth-Bass", 38, 12, 0.37f, false)

// --------------------------------------------------------------------
// Gezupfte Saite (Karplus-Strong mit Allpass-Feinstimmung): Gitarre und Kontrabass
template <int Kind>   // 0 = Gitarre, 1 = Kontrabass, 2 = Harfe
struct PluckVoice {
    static constexpr bool Bass = Kind == 1, Harp = Kind == 2, Pizz = Kind == 3;
    std::array<float, 4096> buf{};
    int len = 100, idx = 0;
    float sr = 48000, g = 0.99f, apC = 0, apX1 = 0, apY1 = 0, prevX = 0, vel = 1, period = 100;
    float life = 0, maxLife = 4;
    bool released = false;
    SVF body;
    Noise noise;
    void start(int pitch, float v, float s) {
        sr = s; vel = v; released = false; life = 0; idx = 0; apX1 = apY1 = prevX = 0;
        const float f = midiToFreq(float(pitch));
        period = std::clamp(sr / f, 4.f, 4000.f);
        float d = period - 0.5f;   // Mittelwert-Filter verzögert um eine halbe Probe
        len = int(d);
        float frac = d - float(len);
        if (frac < 0.1f) { len -= 1; frac += 1.f; }
        apC = (1.f - frac) / (1.f + frac);
        // Anregung: gefiltertes Rauschen (Anschlag weicher/härter je nach Velocity)
        noise.seed(uint32_t(pitch * 92821 + int(v * 1000)));
        const float bright = Bass ? 0.2f + 0.3f * v : Harp ? 0.55f + 0.35f * v : Pizz ? 0.3f + 0.35f * v : 0.35f + 0.55f * v;
        float y = 0, mean = 0;
        for (int i = 0; i < len; ++i) { y += bright * (noise.next() - y); buf[size_t(i)] = y; mean += y; }
        mean /= float(len);
        for (int i = 0; i < len; ++i) buf[size_t(i)] -= mean;
        const float t60 = Bass ? 2.2f : Pizz ? std::clamp(0.9f - float(pitch - 40) * 0.008f, 0.35f, 0.9f) : Harp ? std::clamp(7.f - float(pitch - 40) * 0.07f, 1.5f, 7.f) : std::clamp(4.5f - float(pitch - 40) * 0.05f, 1.f, 4.5f);
        g = std::pow(10.f, -3.f * period / (t60 * sr));
        maxLife = t60 * 1.1f;
        body = SVF{};
        body.set(Bass ? 900.f : Harp ? 7000.f : Pizz ? 3200.f : 5000.f, 0.1f, sr);
    }
    void release() {
        released = true;
        g = std::pow(10.f, -3.f * period / ((Bass ? 0.25f : Harp ? 1.2f : 0.18f) * sr));   // abdämpfen (Harfe klingt nach)
        maxLife = life + (Harp ? 1.5f : 0.35f);
    }
    float next() {
        const float x = buf[size_t(idx)];
        const float avg = 0.5f * (x + prevX);
        prevX = x;
        const float ap = apC * avg + apX1 - apC * apY1;
        apX1 = avg;
        apY1 = ap;
        buf[size_t(idx)] = ap * g;
        if (++idx >= len) idx = 0;
        life += 1.f / sr;
        return body.process(x) * (Bass ? 2.2f : 1.6f) * (0.5f + 0.5f * vel);
    }
    bool finished() const { return life > maxLife; }
};
using GuitarVoice = PluckVoice<0>;
using UprightVoice = PluckVoice<1>;
using HarpVoice = PluckVoice<2>;
using PizzVoice = PluckVoice<3>;
DEFINE_INSTRUMENT(Guitar, GuitarVoice, "Gitarre", 24, 16, 0.7f, false)
DEFINE_INSTRUMENT(UprightBass, UprightVoice, "Kontrabass", 32, 8, 1.15f, false)
DEFINE_INSTRUMENT(Harp, HarpVoice, "Harfe", 46, 24, 0.6f, false)
DEFINE_INSTRUMENT(Pizzicato, PizzVoice, "Pizzicato", 45, 24, 1.1f, false)

// --------------------------------------------------------------------
// Flöte: Sinus mit Obertönen, Atemrauschen, verzögertes Vibrato
struct FluteVoice {
    float sr = 48000, f = 440, t = 0, vel = 1, ph = 0;
    Humanize hz;   // pro Anschlag leicht anders
    ADSR env;
    SVF breath;
    Noise noise;
    void start(int pitch, float v, float s) {
        sr = s; f = midiToFreq(float(pitch)); vel = v; t = 0; ph = 0; hz.init(pitch); f *= centsToRatio(hz.cents);
        env.attack = 0.07f; env.decay = 0.2f; env.sustain = 0.85f; env.release = 0.14f;
        env.start(sr);
        breath = SVF{};
        breath.set(std::min(f * 2.f, 8000.f), 0.6f, sr);
        noise.seed(uint32_t(pitch * 31337 + 7));
    }
    void release() { env.noteOff(); }
    float next() {
        const float vibDepth = std::clamp((t - 0.25f) * 2.f, 0.f, 1.f) * 9.f;
        const float fr = f * centsToRatio(vibDepth * std::sin(kTwoPi * 5.f * hz.rate * t + hz.phase));
        const float tone = std::sin(kTwoPi * ph) + 0.22f * std::sin(2 * kTwoPi * ph) + 0.07f * std::sin(3 * kTwoPi * ph);
        ph += fr / sr;
        ph -= std::floor(ph);
        breath.process(noise.next());
        const float chiff = t < 0.06f ? (1.f - t / 0.06f) * 0.25f : 0.f;
        t += 1.f / sr;
        const float e = env.next();
        return (tone * 0.75f + breath.bp * (0.12f + chiff)) * e * (0.5f + 0.5f * vel);
    }
    bool finished() const { return env.idle(); }
};
DEFINE_INSTRUMENT(Flute, FluteVoice, "Flöte", 73, 12, 0.17f, true)

// --------------------------------------------------------------------
// Lead-Synth: Rechteck + Sägezahn, Filter, Vibrato
struct LeadVoice {
    float sr = 48000, f = 440, t = 0, vel = 1;
    Humanize hz;   // pro Anschlag leicht anders
    Osc a, b;
    SVF filt;
    ADSR env;
    void start(int pitch, float v, float s) {
        sr = s; f = midiToFreq(float(pitch)); vel = v; t = 0; a.phase = 0; b.phase = 0.5f; hz.init(pitch); f *= centsToRatio(hz.cents);
        env.attack = 0.008f; env.decay = 0.3f; env.sustain = 0.75f; env.release = 0.15f;
        env.start(sr);
        filt = SVF{};
        filt.set(std::min(f * 4.f + 2200.f * v, 10000.f), 0.3f, sr);
    }
    void release() { env.noteOff(); }
    float next() {
        const float vibDepth = std::clamp((t - 0.35f) * 2.f, 0.f, 1.f) * 15.f;
        const float fr = f * centsToRatio(vibDepth * std::sin(kTwoPi * 5.5f * hz.rate * t + hz.phase));
        const float s = a.next(fr, sr, Wave::Square) * 0.5f + b.next(fr * 1.003f, sr, Wave::Saw) * 0.4f;
        t += 1.f / sr;
        return filt.process(s) * env.next() * (0.55f + 0.45f * vel);
    }
    bool finished() const { return env.idle(); }
};
DEFINE_INSTRUMENT(Lead, LeadVoice, "Lead", 80, 12, 0.195f, true)

// --------------------------------------------------------------------
// Mallets: Marimba (Holz, kurze Obertöne) und Glocken (FM, lang ausklingend)
template <bool Bell>
struct MalletVoice {
    float sr = 48000, f = 440, vel = 1, p1 = 0, p2 = 0, p3 = 0, t = 0;
    Decay main, over, click;
    void start(int pitch, float v, float s) {
        sr = s; f = midiToFreq(float(pitch)); vel = v; p1 = p2 = p3 = 0; t = 0;
        const float T = Bell ? std::clamp(4.f - float(pitch - 60) * 0.05f, 1.2f, 5.f)
                             : std::clamp(1.3f - float(pitch - 60) * 0.02f, 0.3f, 1.8f);
        main.start(1.f, T, sr);
        over.start(Bell ? 1.f : 0.45f * v, Bell ? T * 0.4f : T * 0.12f, sr);
        click.start(0.2f * v, 0.015f, sr);
    }
    void release() { main.coef = std::min(main.coef, std::exp(-1.f / (0.6f * sr / 4.6f))); }
    float next() {
        float s;
        if (Bell) {   // FM-Glocke: unharmonisches Verhältnis 3.5
            const float idx = (1.f + 3.f * vel) * over.next() + 0.3f;
            s = std::sin(kTwoPi * p1 + idx * std::sin(kTwoPi * p2)) * main.next();
            p1 += f / sr; p2 += f * 3.5f / sr;
        } else {
            s = std::sin(kTwoPi * p1) * main.next() + std::sin(kTwoPi * p2) * over.next() +
                std::sin(kTwoPi * p3) * click.next();
            p1 += f / sr; p2 += f * 4.f / sr; p3 += f * 10.f / sr;
        }
        p1 -= std::floor(p1); p2 -= std::floor(p2); p3 -= std::floor(p3);
        t += 1.f / sr;
        return s * (0.4f + 0.6f * vel);
    }
    bool finished() const { return main.value < 1e-4f; }
};
using MarimbaVoice = MalletVoice<false>;
using BellVoice = MalletVoice<true>;
DEFINE_INSTRUMENT(Marimba, MarimbaVoice, "Marimba", 12, 16, 0.29f, false)
DEFINE_INSTRUMENT(Bells, BellVoice, "Glocken", 14, 16, 0.19f, false)

// ====================================================================
//  ORCHESTER
// ====================================================================

// Streicher-Register: Violinen, Bratschen, Celli, Kontrabässe
struct StringParams { float attack, release, cutMul, cutAdd, vibCents, vibRate, detune; int saws; float sub; };
template <int Kind>
struct SectionStringVoice {
    static constexpr StringParams P[4] = {
        {0.10f, 0.35f, 9.f, 2400.f, 14.f, 5.6f, 9.f, 4, 0.f},    // Violinen
        {0.12f, 0.40f, 7.f, 1700.f, 12.f, 5.3f, 8.f, 3, 0.f},    // Bratschen
        {0.13f, 0.45f, 6.f, 1100.f, 11.f, 5.0f, 7.f, 3, 0.f},    // Celli
        {0.12f, 0.40f, 5.f, 550.f, 7.f, 4.6f, 6.f, 3, 0.35f},    // Kontrabässe
    };
    float sr = 48000, f = 440, t = 0, vel = 1;
    Humanize hz;   // pro Anschlag leicht anders
    std::array<Osc, 5> osc;
    Osc sub;
    SVF filt, filt2;
    ADSR env;
    Noise noise;
    void start(int pitch, float v, float s) {
        const auto& p = P[Kind];
        sr = s; f = midiToFreq(float(pitch)); vel = v; t = 0; hz.init(pitch); f *= centsToRatio(hz.cents);
        for (int i = 0; i < 5; ++i) osc[size_t(i)].phase = float(i) * 0.23f;
        env.attack = p.attack * (1.6f - 0.8f * v); env.decay = 0.4f; env.sustain = 0.9f; env.release = p.release;
        env.start(sr);
        filt = SVF{}; filt2 = SVF{};
        const float cut = std::min(f * p.cutMul + p.cutAdd * (0.6f + 0.4f * v), 10000.f);
        filt.set(cut, 0.1f, sr);
        filt2.set(cut * 1.3f, 0.05f, sr);
        noise.seed(uint32_t(pitch * 131 + 5));
    }
    void release() { env.noteOff(); }
    float next() {
        const auto& p = P[Kind];
        const float vibDepth = std::clamp((t - 0.2f) * 2.5f, 0.f, 1.f) * p.vibCents;
        const float vib = centsToRatio(vibDepth * std::sin(kTwoPi * p.vibRate * hz.rate * t + hz.phase));
        float s = 0;
        for (int i = 0; i < p.saws; ++i) {
            const float det = p.saws > 1 ? (float(i) / float(p.saws - 1) - 0.5f) * 2.f * p.detune : 0.f;
            s += osc[size_t(i)].next(f * vib * centsToRatio(det), sr, Wave::Saw);
        }
        s /= float(p.saws);
        s += sub.next(f * 0.5f, sr, Wave::Sine) * p.sub;
        s += noise.next() * 0.02f;   // Bogengeräusch
        t += 1.f / sr;
        return filt2.process(filt.process(s)) * env.next();
    }
    bool finished() const { return env.idle(); }
};
using ViolinVoice = SectionStringVoice<0>;
using ViolaVoice = SectionStringVoice<1>;
using CelloVoice = SectionStringVoice<2>;
using ContrabassVoice = SectionStringVoice<3>;
DEFINE_INSTRUMENT(Violins, ViolinVoice, "Violinen", 48, 24, 0.336f, true)
DEFINE_INSTRUMENT(Violas, ViolaVoice, "Bratschen", 48, 24, 0.26f, true)
DEFINE_INSTRUMENT(Cellos, CelloVoice, "Celli", 42, 16, 0.28f, true)
DEFINE_INSTRUMENT(Contrabasses, ContrabassVoice, "Kontrabässe", 43, 12, 0.28f, true)

// Blechbläser als Satz (3 Spieler): je lauter, desto heller und "schmetternder"; jeder Spieler setzt
// minimal versetzt ein, ist leicht verstimmt und rutscht beim Ansatz von unten in den Ton. Dazu Atem beim
// Ansatz und die Schall-Resonanz des Instruments (Horn dunkel-rund, Trompete hell, Posaune kernig).
struct BrassParams { float attack, release, brightMul, brightAdd, scoopCents, vibCents, formant, formantQ, spreadCents; };
template <int Kind>
struct BrassVoice {
    static constexpr BrassParams P[3] = {
        {0.07f, 0.30f, 2.5f, 450.f, -20.f, 4.f, 480.f, 0.55f, 5.f},     // Hörner
        {0.025f, 0.16f, 6.f, 1100.f, -35.f, 7.f, 1350.f, 0.5f, 4.f},    // Trompeten
        {0.04f, 0.24f, 4.f, 420.f, -25.f, 4.f, 620.f, 0.5f, 5.f},       // Posaunen
    };
    static constexpr int kPlayers = 3;
    float sr = 48000, f = 440, t = 0, vel = 1;
    Humanize hz;   // pro Anschlag leicht anders
    std::array<Osc, kPlayers> a, b;
    std::array<float, kPlayers> det{}, delay{};
    SVF filt, bell, breathF;
    ADSR env;
    Noise noise;
    int counter = 0;
    void start(int pitch, float v, float s) {
        const auto& p = P[Kind];
        sr = s; f = midiToFreq(float(pitch)); vel = v; t = 0; counter = 0; hz.init(pitch); f *= centsToRatio(hz.cents);
        for (int i = 0; i < kPlayers; ++i) {
            Humanize h;
            h.init(pitch + 17 * i, p.spreadCents);
            det[size_t(i)] = h.cents;
            delay[size_t(i)] = float(i) * (0.006f + 0.004f * h.rate);   // Einsätze minimal versetzt
            a[size_t(i)].phase = 0.13f * float(i);
            b[size_t(i)].phase = 0.4f + 0.21f * float(i);
        }
        env.attack = p.attack; env.decay = 0.3f; env.sustain = 0.8f; env.release = p.release;
        env.start(sr);
        filt = SVF{}; bell = SVF{}; breathF = SVF{};
        bell.set(p.formant, p.formantQ, sr);
        breathF.set(std::min(f * 3.f, 6000.f), 0.6f, sr);
        noise.seed(uint32_t(pitch * 4099 + Kind));
    }
    void release() { env.noteOff(); }
    float next() {
        const auto& p = P[Kind];
        const float e = env.next();
        if ((counter++ & 15) == 0) filt.set(p.brightAdd + f * (1.f + p.brightMul * vel * e * std::sqrt(e)), 0.2f, sr);
        const float scoop = p.scoopCents * std::exp(-t * 35.f);
        const float vibDepth = std::clamp((t - 0.3f) * 2.f, 0.f, 1.f) * p.vibCents;
        const float vib = vibDepth * std::sin(kTwoPi * 5.2f * hz.rate * t + hz.phase);
        float s = 0.f;
        for (int i = 0; i < kPlayers; ++i) {
            const float on = std::clamp((t - delay[size_t(i)]) / 0.01f, 0.f, 1.f);   // versetzter Einsatz
            const float fr = f * centsToRatio(scoop * (1.f + 0.2f * float(i)) + vib + det[size_t(i)]);
            s += (a[size_t(i)].next(fr, sr, Wave::Saw) * 0.7f + b[size_t(i)].next(fr * 1.002f, sr, Wave::Saw) * 0.3f) * on;
        }
        s *= 1.f / float(kPlayers);
        // Atem/Lippen beim Ansatz
        breathF.process(noise.next());
        const float puff = std::exp(-t * 25.f) * 0.25f * (0.4f + 0.6f * vel);
        t += 1.f / sr;
        float y = filt.process(s);
        bell.process(y);
        y = y * 0.75f + bell.bp * 0.6f;                         // Schall-Resonanz
        const float drive = 1.3f + 1.4f * vel * vel;            // forte: schmetternd
        return (softClip(y * drive) / std::sqrt(drive) * 1.2f + breathF.bp * puff) * e;
    }
    bool finished() const { return env.idle(); }
};
using HornVoice = BrassVoice<0>;
using TrumpetVoice = BrassVoice<1>;
using TromboneVoice = BrassVoice<2>;
DEFINE_INSTRUMENT(Horns, HornVoice, "Hörner", 60, 16, 0.44f, true)
DEFINE_INSTRUMENT(Trumpets, TrumpetVoice, "Trompeten", 56, 12, 0.441f, true)
DEFINE_INSTRUMENT(Trombones, TromboneVoice, "Posaunen", 57, 12, 0.398f, true)

// Holzbläser: Oboe (schmaler Puls, nasal), Klarinette (nur ungerade Obertöne), Fagott
struct WindParams { float pw, cutMul, cutAdd, formant, vibCents, attack; };
template <int Kind>
struct WoodwindVoice {
    static constexpr WindParams P[3] = {
        {0.16f, 5.f, 800.f, 1400.f, 10.f, 0.04f},    // Oboe
        {0.5f, 4.f, 500.f, 0.f, 3.f, 0.05f},         // Klarinette
        {0.25f, 5.f, 300.f, 500.f, 6.f, 0.05f},      // Fagott
    };
    float sr = 48000, f = 440, t = 0, vel = 1;
    Humanize hz;   // pro Anschlag leicht anders
    Osc osc;
    SVF filt, form, breath;
    ADSR env;
    Noise noise;
    void start(int pitch, float v, float s) {
        const auto& p = P[Kind];
        sr = s; f = midiToFreq(float(pitch)); vel = v; t = 0; osc.phase = 0; hz.init(pitch); f *= centsToRatio(hz.cents);
        env.attack = p.attack; env.decay = 0.2f; env.sustain = 0.85f; env.release = 0.12f;
        env.start(sr);
        filt = SVF{}; form = SVF{}; breath = SVF{};
        filt.set(std::min(f * p.cutMul + p.cutAdd * (0.6f + 0.6f * v), 9000.f), 0.15f, sr);
        if (p.formant > 0) form.set(p.formant, 0.6f, sr);
        breath.set(std::min(f * 2.5f, 7000.f), 0.5f, sr);
        noise.seed(uint32_t(pitch * 977 + Kind));
    }
    void release() { env.noteOff(); }
    float next() {
        const auto& p = P[Kind];
        const float vibDepth = std::clamp((t - 0.25f) * 2.f, 0.f, 1.f) * p.vibCents;
        const float fr = f * centsToRatio(vibDepth * std::sin(kTwoPi * 5.f * hz.rate * t + hz.phase));
        float s = filt.process(osc.next(fr, sr, Wave::Square, p.pw));
        if (p.formant > 0) { form.process(s); s = s * 0.6f + form.bp * 0.8f; }
        breath.process(noise.next());
        s += breath.bp * 0.06f;
        t += 1.f / sr;
        return s * env.next() * (0.55f + 0.45f * vel);
    }
    bool finished() const { return env.idle(); }
};
using OboeVoice = WoodwindVoice<0>;
using ClarinetVoice = WoodwindVoice<1>;
using BassoonVoice = WoodwindVoice<2>;
DEFINE_INSTRUMENT(Oboe, OboeVoice, "Oboe", 68, 8, 0.093f, true)
DEFINE_INSTRUMENT(Clarinet, ClarinetVoice, "Klarinette", 71, 8, 0.10f, true)
DEFINE_INSTRUMENT(Bassoon, BassoonVoice, "Fagott", 70, 8, 0.083f, true)

// Pauken: gestimmte Membran (unharmonische Schwingungsmoden) + Schlaggeräusch
struct TimpaniVoice {
    float sr = 48000, f = 100, vel = 1;
    std::array<float, 5> ph{};
    std::array<Decay, 5> modes;
    Decay strike;
    SVF filt;
    Noise noise;
    void start(int pitch, float v, float s) {
        static const float dec[5] = {2.6f, 1.6f, 1.2f, 0.9f, 0.6f};
        static const float amp[5] = {1.f, 0.5f, 0.35f, 0.2f, 0.1f};
        sr = s; f = midiToFreq(float(pitch)); vel = v; ph.fill(0);
        for (int i = 0; i < 5; ++i) modes[size_t(i)].start(amp[i] * (0.3f + 0.7f * v), dec[i] * (0.6f + 0.4f * v), sr);
        strike.start(0.6f * v, 0.04f, sr);
        filt = SVF{};
        filt.set(700.f, 0.2f, sr);
        noise.seed(uint32_t(pitch * 4243 + int(v * 100)));
    }
    void release() {}
    float next() {
        static const float ratio[5] = {1.f, 1.5f, 1.98f, 2.44f, 2.94f};
        float s = 0;
        for (int i = 0; i < 5; ++i) {
            s += std::sin(kTwoPi * ph[size_t(i)]) * modes[size_t(i)].next();
            ph[size_t(i)] += f * ratio[i] / sr;
            ph[size_t(i)] -= std::floor(ph[size_t(i)]);
        }
        s += filt.process(noise.next()) * strike.next();
        return s;
    }
    bool finished() const { return modes[0].value < 1e-4f; }
};
class Timpani : public PolyInstrument<TimpaniVoice> {
public:
    Timpani() : PolyInstrument<TimpaniVoice>(24, 0.17f, 0.2f) { stackSamePitch_ = true; }   // Wirbel überlappen
    std::string name() const override { return "Pauken"; }
    int midiProgram() const override { return 47; }
};

// --------------------------------------------------------------------
// Chor ("Aah"): vier leicht verstimmte Stimmen durch Vokal-Formantfilter
struct ChoirVoice {
    float sr = 48000, f = 220, t = 0, vel = 1;
    Humanize hz;   // pro Anschlag leicht anders
    std::array<Osc, 4> osc;
    std::array<float, 4> det{}, vibPh{};
    SVF f1, f2, f3;
    ADSR env;
    Noise noise;
    void start(int pitch, float v, float s) {
        sr = s; f = midiToFreq(float(pitch)); vel = v; t = 0; hz.init(pitch); f *= centsToRatio(hz.cents);
        static const float d[4] = {-9.f, -3.f, 4.f, 10.f};
        for (int i = 0; i < 4; ++i) { det[size_t(i)] = d[i]; vibPh[size_t(i)] = float(i) * 0.37f; osc[size_t(i)].phase = float(i) * 0.21f; }
        env.attack = 0.45f - 0.25f * v; env.decay = 0.5f; env.sustain = 0.9f; env.release = 0.7f;
        env.start(sr);
        // Vokal "a": tiefe Stimmen (Männer) / hohe Stimmen (Frauen)
        const bool high = pitch >= 60;
        f1 = SVF{}; f2 = SVF{}; f3 = SVF{};
        f1.set(high ? 850.f : 730.f, 0.8f, sr);
        f2.set(high ? 1220.f : 1090.f, 0.8f, sr);
        f3.set(high ? 2810.f : 2440.f, 0.75f, sr);
        noise.seed(uint32_t(pitch * 613 + 11));
    }
    void release() { env.noteOff(); }
    float next() {
        float s = 0;
        for (int i = 0; i < 4; ++i) {
            const float vib = 14.f * std::sin(kTwoPi * (5.1f + 0.23f * float(i)) * hz.rate * t + vibPh[size_t(i)] * kTwoPi + hz.phase);
            s += osc[size_t(i)].next(f * centsToRatio(det[size_t(i)] + vib), sr, Wave::Saw);
        }
        s = s * 0.25f + noise.next() * 0.04f;   // etwas Atem
        f1.process(s); f2.process(s); f3.process(s);
        t += 1.f / sr;
        return (f1.bp + 0.6f * f2.bp + 0.3f * f3.bp) * env.next() * (0.55f + 0.45f * vel);
    }
    bool finished() const { return env.idle(); }
};
DEFINE_INSTRUMENT(Choir, ChoirVoice, "Chor", 52, 24, 0.22f, true)

// --------------------------------------------------------------------
// Taiko: große japanische Trommeln (Ensemble) – Odaiko, Nagado, Shime, Rand-Schlag
struct TaikoVoice {
    float sr = 48000, f0 = 60, t = 0, vel = 1, ph = 0;
    Decay amp, slap;
    SVF lp;
    Noise noise;
    bool rim = false;
    void start(int pitch, float v, float s) {
        sr = s; t = 0; ph = 0; vel = v; rim = pitch == 41;
        float decay = 0.9f;
        switch (pitch) {
        case 36: f0 = 46.f; decay = 1.8f; break;    // Odaiko
        case 38: f0 = 72.f; decay = 1.0f; break;    // Nagado
        case 40: f0 = 140.f; decay = 0.35f; break;  // Shime
        case 41: f0 = 900.f; decay = 0.05f; break;  // Ka (Rand)
        default: f0 = 60.f + float(pitch - 36) * 6.f; break;
        }
        const float lvl = 0.25f + 0.75f * v * v;
        amp.start(lvl, decay, sr);
        slap.start(lvl, rim ? 0.03f : 0.05f, sr);
        lp = SVF{};
        lp.set(rim ? 2500.f : 700.f, 0.2f, sr);
        noise.seed(uint32_t(pitch * 271 + int(v * 1000)));
    }
    void release() {}
    float next() {
        const float fr = f0 * (1.f + 0.7f * std::exp(-t * 22.f));   // Fell gibt nach -> Ton fällt
        ph += fr / sr;
        ph -= std::floor(ph);
        lp.process(noise.next());
        const float body = rim ? 0.f : std::sin(kTwoPi * ph) * amp.next();
        const float s = body * 1.5f + (rim ? lp.bp * 2.f * amp.next() : lp.lp * 1.2f) * slap.next();
        t += 1.f / sr;
        return softClip(s * 1.4f);
    }
    bool finished() const { return amp.value < 1e-4f; }
};
class Taiko : public PolyInstrument<TaikoVoice> {
public:
    Taiko() : PolyInstrument<TaikoVoice>(32, 0.3f, 0.4f) { stackSamePitch_ = true; }
    std::string name() const override { return "Taiko"; }
    int midiProgram() const override { return 116; }   // GM "Taiko Drum"
    bool isDrumKit() const override { return true; }
    const char* noteLabel(int pitch) const override {
        switch (pitch) {
        case 36: return "Odaiko (groß)";
        case 38: return "Nagado (mittel)";
        case 40: return "Shime (klein)";
        case 41: return "Ka (Rand)";
        default: return nullptr;
        }
    }
};

// --------------------------------------------------------------------
// Schlagzeug: synthetisch, General-MIDI-Belegung
enum class Drum { Kick, Snare, Clap, Rim, HatClosed, HatPedal, HatOpen, Tom, Crash, Ride, RideBell, Cowbell, Tambourine, Shaker, Click };

Drum drumFor(int pitch) {
    switch (pitch) {
    case 33: case 34: return Drum::Click;   // Metronom
    case 35: case 36: return Drum::Kick;
    case 37: return Drum::Rim;
    case 38: case 40: return Drum::Snare;
    case 39: return Drum::Clap;
    case 42: return Drum::HatClosed;
    case 44: return Drum::HatPedal;
    case 46: return Drum::HatOpen;
    case 49: case 52: case 55: case 57: return Drum::Crash;
    case 51: case 59: return Drum::Ride;
    case 53: return Drum::RideBell;
    case 54: return Drum::Tambourine;
    case 56: return Drum::Cowbell;
    case 69: case 70: case 82: return Drum::Shaker;
    default: return Drum::Tom;
    }
}

struct DrumVoice {
    Drum kind = Drum::Kick;
    float sr = 48000, t = 0, vel = 1, f0 = 100, ph = 0, ph2 = 0;
    Decay amp, aux;
    SVF filt;
    Noise noise;
    std::array<Osc, 6> metal;

    float metallic() {   // sechs unharmonische Rechtecke (klassischer Becken-Trick)
        static const float fr[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.f, 800.f};
        float s = 0;
        for (int i = 0; i < 6; ++i) s += metal[size_t(i)].next(fr[i] * 1.7f, sr, Wave::Square);
        return s / 6.f;
    }

    void start(int pitch, float v, float s) {
        sr = s; t = 0; vel = v; ph = ph2 = 0; kind = drumFor(pitch);
        noise.seed(uint32_t(pitch * 7717 + int(v * 977) + 3));
        for (int i = 0; i < 6; ++i) metal[size_t(i)].phase = float(i) * 0.17f;
        filt = SVF{};
        const float lvl = 0.25f + 0.75f * v * v;
        switch (kind) {
        case Drum::Kick: f0 = pitch == 35 ? 50.f : 56.f; amp.start(lvl, 0.45f, sr); aux.start(1.f, 0.006f, sr); break;
        case Drum::Snare: amp.start(lvl, 0.14f, sr); aux.start(lvl, 0.22f, sr); filt.set(2800.f, 0.2f, sr); break;
        case Drum::Clap: amp.start(lvl, 0.2f, sr); filt.set(1150.f, 0.55f, sr); break;
        case Drum::Rim: amp.start(lvl, 0.035f, sr); filt.set(1700.f, 0.7f, sr); break;
        case Drum::HatClosed: amp.start(lvl, 0.05f, sr); filt.set(7500.f, 0.2f, sr); break;
        case Drum::HatPedal: amp.start(lvl * 0.8f, 0.035f, sr); filt.set(7000.f, 0.2f, sr); break;
        case Drum::HatOpen: amp.start(lvl, 0.38f, sr); filt.set(7000.f, 0.2f, sr); break;
        case Drum::Crash: amp.start(lvl, pitch == 57 ? 1.4f : 1.8f, sr); filt.set(pitch == 57 ? 5200.f : 4500.f, 0.1f, sr); break;
        case Drum::Ride: amp.start(lvl, 0.9f, sr); filt.set(3800.f, 0.3f, sr); break;
        case Drum::RideBell: amp.start(lvl, 1.2f, sr); filt.set(2600.f, 0.5f, sr); break;
        case Drum::Cowbell: amp.start(lvl, 0.28f, sr); filt.set(900.f, 0.6f, sr); break;
        case Drum::Tambourine: amp.start(lvl, 0.14f, sr); filt.set(8000.f, 0.3f, sr); break;
        case Drum::Shaker: amp.start(lvl * 0.7f, 0.07f, sr); filt.set(6500.f, 0.2f, sr); break;
        case Drum::Click: f0 = pitch == 34 ? 2500.f : 1600.f; amp.start(lvl * 0.8f, 0.04f, sr); break;
        case Drum::Tom: {
            static const std::array<std::pair<int, float>, 6> toms = {{{41, 82.f}, {43, 98.f}, {45, 110.f}, {47, 131.f}, {48, 147.f}, {50, 175.f}}};
            f0 = midiToFreq(float(pitch)) * 0.5f;
            for (auto& [p, fq] : toms) if (p == pitch) f0 = fq;
            amp.start(lvl, 0.35f, sr);
            break;
        }
        }
    }
    void release() {}   // Schlagzeug klingt immer natürlich aus
    void choke() { amp.coef = std::min(amp.coef, std::exp(-1.f / (0.03f * sr / 4.6f))); }

    float next() {
        float s = 0;
        const float n = noise.next();
        switch (kind) {
        case Drum::Kick: {
            const float fr = f0 + 120.f * std::exp(-t * 38.f);
            ph += fr / sr;
            s = std::sin(kTwoPi * ph) * amp.next() * 1.6f + n * aux.next() * 0.4f * vel;
            s = softClip(s * 1.3f);
            break;
        }
        case Drum::Snare: {
            ph += 185.f / sr; ph2 += 330.f / sr;
            const float body = (std::sin(kTwoPi * ph) + 0.5f * std::sin(kTwoPi * ph2)) * amp.next() * 0.6f;
            filt.process(n);
            s = body + (filt.hp * 0.6f + filt.bp * 0.5f) * aux.next();
            break;
        }
        case Drum::Clap: {
            float e = 0;   // drei kurze Schläge + Hall-Fahne
            for (float off : {0.f, 0.009f, 0.018f})
                if (t >= off && t < off + 0.008f) e = std::max(e, 1.f - (t - off) / 0.008f);
            const float av = amp.next();
            const float tail = t > 0.022f ? av : 0.f;
            filt.process(n);
            s = filt.bp * (e * vel + tail) * 1.6f;
            break;
        }
        case Drum::Rim: {
            ph += 820.f / sr;
            filt.process(n);
            s = (filt.bp * 0.7f + std::sin(kTwoPi * ph) * 0.5f) * amp.next() * 1.4f;
            break;
        }
        case Drum::HatClosed: case Drum::HatPedal: case Drum::HatOpen: case Drum::Tambourine: case Drum::Shaker: {
            const float m = (kind == Drum::Tambourine || kind == Drum::Shaker) ? 0.f : metallic();
            filt.process(m * 0.6f + n * 0.5f);
            s = filt.hp * amp.next() * 0.9f;
            break;
        }
        case Drum::Crash: {
            filt.process(metallic() * 0.5f + n * 0.7f);
            s = filt.hp * amp.next() * 0.7f;
            break;
        }
        case Drum::Ride: case Drum::RideBell: {
            const float bell = kind == Drum::RideBell ? 1.f : 0.35f;
            filt.process(metallic() * (0.8f + bell) + n * 0.12f);
            s = filt.bp * amp.next() * 0.9f;
            break;
        }
        case Drum::Cowbell: {
            filt.process(metal[0].next(540.f, sr, Wave::Square) + metal[1].next(800.f, sr, Wave::Square));
            s = filt.bp * amp.next() * 0.6f;
            break;
        }
        case Drum::Click:
            ph += f0 / sr;
            s = std::sin(kTwoPi * ph) * amp.next();
            break;
        case Drum::Tom: {
            const float fr = f0 * (1.f + 0.5f * std::exp(-t * 18.f));
            ph += fr / sr;
            s = (std::sin(kTwoPi * ph) + n * 0.05f) * amp.next() * 1.1f;
            break;
        }
        }
        t += 1.f / sr;
        return s;
    }
    bool finished() const { return amp.value < 1e-4f; }
};

class DrumKit : public PolyInstrument<DrumVoice> {
public:
    DrumKit() : PolyInstrument<DrumVoice>(32, 0.63f, 0.35f) {}
    std::string name() const override { return "Schlagzeug"; }
    bool isDrumKit() const override { return true; }
    const char* noteLabel(int pitch) const override {
        switch (pitch) {
        case 33: return "Metronom";     case 34: return "Metronom 1";
        case 35: return "Kick 2";       case 36: return "Kick";        case 37: return "Rimshot";
        case 38: return "Snare";        case 39: return "Clap";        case 40: return "Snare 2";
        case 41: return "Tom tief 2";   case 42: return "HiHat zu";    case 43: return "Tom tief";
        case 44: return "HiHat Pedal";  case 45: return "Tom mittel 2"; case 46: return "HiHat offen";
        case 47: return "Tom mittel";   case 48: return "Tom hoch 2";  case 49: return "Crash";
        case 50: return "Tom hoch";     case 51: return "Ride";        case 53: return "Ride Glocke";
        case 54: return "Tamburin";     case 56: return "Kuhglocke";   case 57: return "Crash 2";
        case 59: return "Ride 2";       case 70: return "Shaker";
        default: return nullptr;
        }
    }

protected:
    void onNoteOn(int pitch) override {   // geschlossene Hi-Hat würgt die offene ab
        if (pitch != 42 && pitch != 44) return;
        for (auto& s : slots_)
            if (s.active && s.pitch == 46) s.voice.choke();
    }
};

#undef DEFINE_INSTRUMENT
}   // namespace

void registerSynths(InstrumentRegistry& reg) {
    reg.registerInstrument("E-Piano", [] { return std::make_unique<EPiano>(); });
    reg.registerInstrument("Orgel", [] { return std::make_unique<Organ>(); });
    reg.registerInstrument("Streicher", [] { return std::make_unique<Strings>(); });
    reg.registerInstrument("Pad", [] { return std::make_unique<Pad>(); });
    reg.registerInstrument("Synth-Bass", [] { return std::make_unique<SynthBass>(); });
    reg.registerInstrument("Kontrabass", [] { return std::make_unique<UprightBass>(); });
    reg.registerInstrument("Gitarre", [] { return std::make_unique<Guitar>(); });
    reg.registerInstrument("Flöte", [] { return std::make_unique<Flute>(); });
    reg.registerInstrument("Lead", [] { return std::make_unique<Lead>(); });
    reg.registerInstrument("Marimba", [] { return std::make_unique<Marimba>(); });
    reg.registerInstrument("Glocken", [] { return std::make_unique<Bells>(); });
    reg.registerInstrument("Schlagzeug", [] { return std::make_unique<DrumKit>(); });
    // Orchester
    reg.registerInstrument("Harfe", [] { return std::make_unique<Harp>(); });
    reg.registerInstrument("Pizzicato", [] { return std::make_unique<Pizzicato>(); });
    reg.registerInstrument("Violinen", [] { return std::make_unique<Violins>(); });
    reg.registerInstrument("Bratschen", [] { return std::make_unique<Violas>(); });
    reg.registerInstrument("Celli", [] { return std::make_unique<Cellos>(); });
    reg.registerInstrument("Kontrabässe", [] { return std::make_unique<Contrabasses>(); });
    reg.registerInstrument("Hörner", [] { return std::make_unique<Horns>(); });
    reg.registerInstrument("Trompeten", [] { return std::make_unique<Trumpets>(); });
    reg.registerInstrument("Posaunen", [] { return std::make_unique<Trombones>(); });
    reg.registerInstrument("Oboe", [] { return std::make_unique<Oboe>(); });
    reg.registerInstrument("Klarinette", [] { return std::make_unique<Clarinet>(); });
    reg.registerInstrument("Fagott", [] { return std::make_unique<Bassoon>(); });
    reg.registerInstrument("Pauken", [] { return std::make_unique<Timpani>(); });
    reg.registerInstrument("Chor", [] { return std::make_unique<Choir>(); });
    reg.registerInstrument("Taiko", [] { return std::make_unique<Taiko>(); });
}
