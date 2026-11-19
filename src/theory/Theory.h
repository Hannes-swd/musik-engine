#pragma once
#include <string>
#include <vector>

// Grundlegende Musiktheorie: Skalen (Tonleitern) und Akkorde.

enum class ScaleType {
    Major, NaturalMinor, HarmonicMinor, Dorian, Phrygian, Lydian, Mixolydian,
    MajorPentatonic, MinorPentatonic, Count
};

const char* scaleName(ScaleType t);
const char* noteName(int pc);             // 0 = C … 11 = H
bool scaleIsMinor(ScaleType t);           // kleine Terz?
ScaleType harmonyScaleFor(ScaleType t);   // Pentatonik/Harm. Moll -> Skala für die Akkorde

int mod12(int a);
int floorDiv(int a, int b);

class Scale {
public:
    Scale(int root = 0, ScaleType type = ScaleType::Major);

    int root() const { return root_; }
    ScaleType type() const { return type_; }
    int size() const { return int(steps_.size()); }
    const std::vector<int>& steps() const { return steps_; }

    // Stufe -> MIDI-Tonhöhe. Stufe 0 = Grundton ab C4 (60), negative/große Stufen erlaubt.
    int pitch(int degree) const;
    // Stufe des nächstgelegenen Skalentons
    int nearestDegree(int pitch) const;
    bool contains(int pitchOrPc) const;

private:
    int root_;
    ScaleType type_;
    std::vector<int> steps_;
};

struct Chord {
    int root = 0;                 // absolute Tonklasse 0..11
    std::vector<int> intervals;   // Halbtöne über dem Grundton, z.B. {0,4,7}
    std::string symbol;           // z.B. "Am7"
    std::string roman;            // z.B. "vi7"

    bool hasPc(int pc) const;
    std::vector<int> pcs() const; // Tonklassen, Grundton zuerst
};

Chord makeChord(int rootPc, const std::vector<int>& intervals, const std::string& roman = "");
// Leitereigener Akkord auf Stufe degree (0..6) einer 7-stufigen Skala
Chord diatonicChord(const Scale& harmony, int degree, bool seventh);
