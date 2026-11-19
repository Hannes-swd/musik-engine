#include "Theory.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {
struct ScaleDef {
    const char* name;
    std::vector<int> steps;
    bool minor;
    ScaleType harmony;
};

const ScaleDef& def(ScaleType t) {
    static const ScaleDef defs[] = {
        {"Dur", {0, 2, 4, 5, 7, 9, 11}, false, ScaleType::Major},
        {"Moll", {0, 2, 3, 5, 7, 8, 10}, true, ScaleType::NaturalMinor},
        {"Harmonisch Moll", {0, 2, 3, 5, 7, 8, 11}, true, ScaleType::NaturalMinor},
        {"Dorisch", {0, 2, 3, 5, 7, 9, 10}, true, ScaleType::Dorian},
        {"Phrygisch", {0, 1, 3, 5, 7, 8, 10}, true, ScaleType::Phrygian},
        {"Lydisch", {0, 2, 4, 6, 7, 9, 11}, false, ScaleType::Lydian},
        {"Mixolydisch", {0, 2, 4, 5, 7, 9, 10}, false, ScaleType::Mixolydian},
        {"Dur-Pentatonik", {0, 2, 4, 7, 9}, false, ScaleType::Major},
        {"Moll-Pentatonik", {0, 3, 5, 7, 10}, true, ScaleType::NaturalMinor},
    };
    return defs[std::clamp(int(t), 0, int(ScaleType::Count) - 1)];
}

std::string qualitySuffix(const std::vector<int>& iv) {
    struct Q { std::vector<int> iv; const char* s; };
    static const Q qs[] = {
        {{0, 4, 7}, ""},       {{0, 3, 7}, "m"},        {{0, 3, 6}, "dim"},     {{0, 4, 8}, "aug"},
        {{0, 4, 7, 11}, "maj7"}, {{0, 4, 7, 10}, "7"},  {{0, 3, 7, 10}, "m7"},  {{0, 3, 6, 10}, "m7b5"},
        {{0, 3, 6, 9}, "dim7"},  {{0, 3, 7, 11}, "m(maj7)"}, {{0, 5, 7}, "sus4"},
    };
    for (auto& q : qs)
        if (q.iv == iv) return q.s;
    return "";
}
}

int mod12(int a) { int x = a % 12; return x < 0 ? x + 12 : x; }
int floorDiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }

const char* scaleName(ScaleType t) { return def(t).name; }
bool scaleIsMinor(ScaleType t) { return def(t).minor; }
ScaleType harmonyScaleFor(ScaleType t) { return def(t).harmony; }

const char* noteName(int pc) {
    static const char* n[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "H"};
    return n[mod12(pc)];
}

Scale::Scale(int root, ScaleType type) : root_(mod12(root)), type_(type), steps_(def(type).steps) {}

int Scale::pitch(int degree) const {
    int n = size();
    int oct = floorDiv(degree, n);
    int idx = degree - oct * n;
    return 60 + root_ + oct * 12 + steps_[idx];
}

int Scale::nearestDegree(int p) const {
    int guess = floorDiv((p - 60 - root_) * size(), 12);
    int best = guess, bestDist = 1000;
    for (int d = guess - 3; d <= guess + 3; ++d) {
        int dist = std::abs(pitch(d) - p);
        if (dist < bestDist) { bestDist = dist; best = d; }
    }
    return best;
}

bool Scale::contains(int p) const {
    int rel = mod12(p - root_);
    return std::find(steps_.begin(), steps_.end(), rel) != steps_.end();
}

bool Chord::hasPc(int pc) const {
    pc = mod12(pc);
    for (int i : intervals)
        if (mod12(root + i) == pc) return true;
    return false;
}

std::vector<int> Chord::pcs() const {
    std::vector<int> out;
    for (int i : intervals) out.push_back(mod12(root + i));
    return out;
}

Chord makeChord(int rootPc, const std::vector<int>& intervals, const std::string& roman) {
    Chord c;
    c.root = mod12(rootPc);
    c.intervals = intervals;
    c.symbol = std::string(noteName(c.root)) + qualitySuffix(intervals);
    c.roman = roman;
    return c;
}

Chord diatonicChord(const Scale& harmony, int degree, bool seventh) {
    const auto& s = harmony.steps();
    const int n = int(s.size());
    degree = ((degree % n) + n) % n;
    std::vector<int> iv;
    for (int k : {0, 2, 4, 6}) {
        if (k == 6 && !seventh) break;
        int idx = degree + k;
        int semis = s[idx % n] + 12 * (idx / n);
        iv.push_back(semis - s[degree]);
    }
    static const char* numerals[] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    std::string roman = numerals[degree % 7];
    bool minorThird = iv.size() > 1 && iv[1] == 3;
    bool dim = iv.size() > 2 && iv[2] == 6;
    if (minorThird) for (auto& ch : roman) ch = char(std::tolower(ch));
    if (dim) roman += "°";
    if (seventh) roman += (iv[3] == 11 ? "maj7" : "7");
    return makeChord(harmony.root() + s[degree], iv, roman);
}
