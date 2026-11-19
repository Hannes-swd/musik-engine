#pragma once
#include <string>

// Basis für alle Instrumente. Ein neues Instrument implementiert diese
// Schnittstelle und meldet sich in der InstrumentRegistry an.
class IInstrument {
public:
    virtual ~IInstrument() = default;

    virtual std::string name() const = 0;
    // General-MIDI-Programmnummer für den MIDI-Export (0 = Acoustic Grand Piano)
    virtual int midiProgram() const { return 0; }
    // Schlagzeug: jede Tonhöhe ist ein anderes Instrument (GM-Belegung, MIDI-Kanal 10)
    virtual bool isDrumKit() const { return false; }
    // Beschriftung einer Tonhöhe in der Piano-Roll (z.B. "Snare"), nullptr = normal
    virtual const char* noteLabel(int /*pitch*/) const { return nullptr; }
    // Klingt der Ton, solange die Taste gehalten wird (Orgel, Streicher)? Sonst klingt er ab (Klavier).
    virtual bool sustains() const { return false; }
    virtual void setSampleRate(float sampleRate) = 0;

    // pitch = MIDI-Notennummer (60 = C4), velocity = 0..1
    virtual void noteOn(int pitch, float velocity) = 0;
    virtual void noteOff(int pitch) = 0;
    virtual void allNotesOff() = 0;

    // Addiert frameCount Stereo-Frames (interleaved L/R) in out.
    virtual void render(float* out, int frameCount) = 0;
};
