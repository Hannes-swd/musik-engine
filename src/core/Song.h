#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Zeit wird in Ticks gemessen. ppq = Ticks pro Viertelnote.
using Tick = int64_t;

struct Note {
    int pitch = 60;          // MIDI-Notennummer (60 = C4)
    Tick start = 0;
    Tick length = 480;
    float velocity = 0.8f;   // 0..1

    Tick end() const { return start + length; }
};

struct Track {
    std::string name = "Track";
    std::string instrument = "Piano";   // Name aus der InstrumentRegistry
    std::vector<Note> notes;
    float volume = 1.f;
    bool muted = false;
    bool solo = false;                  // nur Solo-Spuren hörbar (wenn mindestens eine Solo ist)
    float pan = 0.f;                    // Panorama -1 = links … 0 = Mitte … 1 = rechts
    float reverb = 0.2f;                // Anteil in den Hall (Effekt-Weg) 0..1
    float delay = 0.f;                  // Anteil ins Echo 0..1
    // Lautstärke-Verlauf (Dynamik-Automation): Punkte (Tick, Faktor), dazwischen linear.
    // Leer = gleichbleibend 1. Damit können auch gehaltene Töne an- und abschwellen.
    std::vector<std::pair<Tick, float>> expression;

    float expressionAt(double t) const { return evalExpression(expression, t); }
    static float evalExpression(const std::vector<std::pair<Tick, float>>& e, double t) {
        if (e.empty()) return 1.f;
        if (t <= double(e.front().first)) return e.front().second;
        if (t >= double(e.back().first)) return e.back().second;
        auto it = std::upper_bound(e.begin(), e.end(), t, [](double v, const std::pair<Tick, float>& p) { return v < double(p.first); });
        const auto& b = *it;
        const auto& a = *(it - 1);
        const double span = double(b.first - a.first);
        const float f = span > 0 ? float((t - double(a.first)) / span) : 1.f;
        return a.second + (b.second - a.second) * f;
    }
};

// Dynamikzeichen für die Notenansicht (pp … ff, Crescendo-/Decrescendo-Gabeln)
struct DynamicMark {
    enum Kind { Level, Crescendo, Diminuendo } kind = Level;
    Tick start = 0, end = 0;
    int level = 3;   // 0 pp, 1 p, 2 mp, 3 mf, 4 f, 5 ff
};

// Abschnitt eines Songs (Intro, Strophe, Refrain …) – für die Anzeige
struct Section {
    std::string name;
    Tick start = 0;
    Tick length = 0;
};

// Raumklang des ganzen Songs (Hall + Echo als gemeinsame Effekte, jede Spur schickt ihren Anteil hinein)
struct Effects {
    bool on = true;
    float roomSize = 0.6f;      // Raumgröße / Nachhall 0..1
    float damping = 0.45f;      // Höhendämpfung 0..1 (mehr = dunklerer Raum)
    float reverbLevel = 0.8f;   // Lautstärke des Halls
    float preDelay = 0.02f;     // Sekunden bis der Hall einsetzt
    int delayNote = 1;          // Echo-Zeit: 0 = 1/4, 1 = 1/8, 2 = 1/8 punktiert, 3 = 1/8-Triole, 4 = 1/16
    float delayFeedback = 0.35f;
    float delayLevel = 0.7f;
    bool mastering = true;      // Mastering-Kette auf der Summe (Kompressor, EQ, Breite, Limiter)
    float masterAmount = 0.5f;  // wie kräftig gemastert wird 0..1
};

struct Song {
    std::string title = "Unbenannt";
    std::vector<Section> sections;
    std::vector<DynamicMark> dynamics;
    Effects fx;
    double bpm = 120.0;      // Viertelnoten pro Minute
    int ppq = 480;
    int beatsPerBar = 4;     // Taktart beatsPerBar / beatUnit
    int beatUnit = 4;
    int key = -1;            // Tonart-Grundton 0..11 (-1 = unbekannt, wird erkannt)
    int scale = -1;          // ScaleType (für Vorzeichen in der Notenansicht)
    std::vector<Track> tracks;

    Tick ticksPerBeat() const { return Tick(ppq) * 4 / beatUnit; }
    Tick ticksPerBar() const { return ticksPerBeat() * beatsPerBar; }
    Tick bar(int n) const { return ticksPerBar() * n; }

    double ticksToSeconds(double ticks) const { return ticks / ppq * 60.0 / bpm; }
    double secondsToTicks(double sec) const { return sec * bpm / 60.0 * ppq; }

    // Ende der letzten Note
    Tick length() const {
        Tick len = 0;
        for (auto& t : tracks)
            for (auto& n : t.notes) len = std::max(len, n.end());
        return len;
    }
    double durationSeconds() const { return ticksToSeconds(double(length())); }
};
