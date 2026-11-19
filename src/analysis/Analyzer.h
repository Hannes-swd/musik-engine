#pragma once
#include "core/Song.h"
#include "theory/Theory.h"
#include <vector>

// Analyse von gespielten/aufgenommenen Noten.

struct KeyGuess {
    int key = 0;                          // Grundton 0..11
    ScaleType scale = ScaleType::Major;   // inkl. Kirchentonart-Erkennung
    float confidence = 0;                 // Korrelation 0..1
};

// Tonart-Erkennung nach Krumhansl-Schmuckler (Tonhöhen-Profile, nach Dauer gewichtet),
// danach Feinbestimmung von Dorisch/Mixolydisch/Lydisch/Harmonisch Moll anhand typischer Töne.
KeyGuess detectKey(const std::vector<Note>& notes);

// Quantisieren: Startzeiten (und optional Enden) Richtung Raster ziehen. strength 0..1
void quantizeNotes(std::vector<Note>& notes, Tick grid, float strength, bool ends = true);

// Leiterfremde Töne auf betonten Zählzeiten/langen Noten zum nächsten Skalenton schieben
int correctToScale(std::vector<Note>& notes, int key, ScaleType scale, const Song& song);

struct Feel {
    float notesPerBeat = 0;     // Dichte
    float offbeatRatio = 0;     // Anteil Off-Beat-Einsätze (Synkopen)
    float meanVelocity = 0.7f;
    float meanPitch = 64;
    int lowest = 60, highest = 72;
    bool harmonic = false;      // überwiegend Akkorde gespielt
};
Feel analyzeFeel(const std::vector<Note>& notes, const Song& song, int bars);

// Noten einer Spur im Bereich [from, to), auf from verschoben
std::vector<Note> extractRange(const Track& track, Tick from, Tick to);

// Akkorderkennung (für das Akkordblatt): pro Zeitfenster der Akkord, der am besten zu allen klingenden
// Tönen passt (ohne Schlagzeug). Grundton bevorzugt, wenn er auch der tiefste Ton ist.
struct ChordLabel {
    Tick start = 0, length = 0;
    int root = -1;          // -1 = Pause / nichts erkennbar
    int quality = 0;        // Index in chordQualityName()
    std::string symbol;     // z.B. "Am7"
};
std::vector<ChordLabel> detectChords(const Song& song, Tick step);
const char* chordQualityName(int quality);   // "", "m", "7", "maj7", "m7", "dim", "m7b5", "sus4", "sus2", "aug"
bool chordQualityMinor(int quality);
