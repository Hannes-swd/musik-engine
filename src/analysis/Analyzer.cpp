#include "Analyzer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include "instruments/InstrumentRegistry.h"

namespace {
// Krumhansl-Kessler-Profile (empirisch ermittelte "Passung" jeder Stufe zur Tonart)
const std::array<float, 12> kMajor = {6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f};
const std::array<float, 12> kMinor = {6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f};

float correlation(const std::array<float, 12>& a, const std::array<float, 12>& b, int rot) {
    float ma = 0, mb = 0;
    for (int i = 0; i < 12; ++i) { ma += a[i]; mb += b[i]; }
    ma /= 12; mb /= 12;
    float num = 0, da = 0, db = 0;
    for (int i = 0; i < 12; ++i) {
        float x = a[(i + rot) % 12] - ma, y = b[i] - mb;
        num += x * y; da += x * x; db += y * y;
    }
    return (da > 0 && db > 0) ? num / std::sqrt(da * db) : 0.f;
}
}

KeyGuess detectKey(const std::vector<Note>& notes) {
    std::array<float, 12> hist{};
    for (auto& n : notes) hist[size_t(mod12(n.pitch))] += float(std::min<Tick>(n.length, 1920)) * (0.5f + n.velocity);
    KeyGuess best;
    best.confidence = -2;
    for (int k = 0; k < 12; ++k) {
        float cM = correlation(hist, kMajor, k), cm = correlation(hist, kMinor, k);
        if (cM > best.confidence) best = {k, ScaleType::Major, cM};
        if (cm > best.confidence) best = {k, ScaleType::NaturalMinor, cm};
    }
    // Kirchentonarten: charakteristischer Ton deutlich häufiger als der "normale"
    auto w = [&](int rel) { return hist[size_t(mod12(best.key + rel))]; };
    float total = 0;
    for (float h : hist) total += h;
    auto clearly = [&](int characteristic, int normal) { return w(characteristic) > 0.04f * total && w(characteristic) > 1.5f * w(normal); };
    if (best.scale == ScaleType::NaturalMinor) {
        if (clearly(11, 10)) best.scale = ScaleType::HarmonicMinor;
        else if (clearly(9, 8)) best.scale = ScaleType::Dorian;
        else if (clearly(1, 2)) best.scale = ScaleType::Phrygian;
    } else {
        if (clearly(10, 11)) best.scale = ScaleType::Mixolydian;
        else if (clearly(6, 5)) best.scale = ScaleType::Lydian;
    }
    best.confidence = std::clamp(best.confidence, 0.f, 1.f);
    return best;
}

void quantizeNotes(std::vector<Note>& notes, Tick grid, float strength, bool ends) {
    if (grid <= 0 || strength <= 0) return;
    auto q = [&](Tick t) {
        Tick target = Tick(std::llround(double(t) / double(grid))) * grid;
        return t + Tick(std::llround(double(target - t) * strength));
    };
    for (auto& n : notes) {
        Tick end = n.end();
        n.start = std::max<Tick>(0, q(n.start));
        if (ends) end = q(end);
        n.length = std::max<Tick>(ends ? Tick(grid / 2) : 1, end - n.start);
    }
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
}

int correctToScale(std::vector<Note>& notes, int key, ScaleType scale, const Song& song) {
    Scale sc(key, scale);
    int changed = 0;
    for (auto& n : notes) {
        if (sc.contains(n.pitch)) continue;
        const bool strong = n.start % song.ticksPerBeat() == 0;
        const bool longNote = n.length >= song.ticksPerBeat();
        if (!strong && !longNote) continue;   // kurze Durchgangstöne dürfen chromatisch sein
        n.pitch = sc.pitch(sc.nearestDegree(n.pitch));
        ++changed;
    }
    return changed;
}

Feel analyzeFeel(const std::vector<Note>& notes, const Song& song, int bars) {
    Feel f;
    if (notes.empty()) return f;
    // Einsätze gruppieren (gleichzeitige Töne = ein Einsatz)
    std::vector<Tick> onsets;
    int simultaneous = 0;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (!onsets.empty() && std::llabs(notes[i].start - onsets.back()) < song.ppq / 16) { ++simultaneous; continue; }
        onsets.push_back(notes[i].start);
    }
    const Tick beat = song.ticksPerBeat();
    int off = 0;
    for (Tick t : onsets) if (t % beat > beat / 8 && t % beat < beat - beat / 8) ++off;
    f.notesPerBeat = float(onsets.size()) / float(std::max(1, bars * song.beatsPerBar));
    f.offbeatRatio = float(off) / float(onsets.size());
    f.harmonic = simultaneous > int(onsets.size()) / 2;
    float vel = 0, pitch = 0;
    f.lowest = 127; f.highest = 0;
    for (auto& n : notes) {
        vel += n.velocity; pitch += float(n.pitch);
        f.lowest = std::min(f.lowest, n.pitch); f.highest = std::max(f.highest, n.pitch);
    }
    f.meanVelocity = vel / float(notes.size());
    f.meanPitch = pitch / float(notes.size());
    return f;
}

std::vector<Note> extractRange(const Track& track, Tick from, Tick to) {
    std::vector<Note> out;
    for (auto n : track.notes) {
        if (n.start < from || n.start >= to) continue;
        n.start -= from;
        n.length = std::min(n.length, to - from - n.start);
        out.push_back(n);
    }
    std::sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    return out;
}

// ---------------------------------------------------------------------
//  Akkorderkennung (Schablonen-Vergleich über Tonklassen-Gewichte)
// ---------------------------------------------------------------------
namespace {
struct ChordTemplate { const char* name; std::vector<int> iv; bool minor; float bias; };
const std::vector<ChordTemplate>& chordTemplates() {
    static const std::vector<ChordTemplate> t = {
        {"", {0, 4, 7}, false, 0.10f},     {"m", {0, 3, 7}, true, 0.10f},      {"7", {0, 4, 7, 10}, false, 0.f},
        {"maj7", {0, 4, 7, 11}, false, 0.f}, {"m7", {0, 3, 7, 10}, true, 0.f},  {"dim", {0, 3, 6}, true, -0.05f},
        {"m7b5", {0, 3, 6, 10}, true, -0.05f}, {"sus4", {0, 5, 7}, false, -0.05f}, {"sus2", {0, 2, 7}, false, -0.08f},
        {"aug", {0, 4, 8}, false, -0.12f},
    };
    return t;
}
}

const char* chordQualityName(int q) {
    const auto& t = chordTemplates();
    return (q >= 0 && q < int(t.size())) ? t[size_t(q)].name : "";
}
bool chordQualityMinor(int q) {
    const auto& t = chordTemplates();
    return q >= 0 && q < int(t.size()) && t[size_t(q)].minor;
}

std::vector<ChordLabel> detectChords(const Song& song, Tick step) {
    std::vector<ChordLabel> out;
    if (step <= 0) return out;
    const Tick len = song.length();
    std::vector<bool> drum(song.tracks.size(), false);
    for (size_t i = 0; i < song.tracks.size(); ++i) {
        auto inst = InstrumentRegistry::instance().create(song.tracks[i].instrument);
        drum[i] = (inst && inst->isDrumKit()) || song.tracks[i].muted;
    }
    const auto& tpl = chordTemplates();
    for (Tick a = 0; a < len; a += step) {
        const Tick b = a + step;
        std::array<float, 12> w{};
        int lowest = 999;
        float lowestW = 0;
        float total = 0;
        for (size_t ti = 0; ti < song.tracks.size(); ++ti) {
            if (drum[ti]) continue;
            for (auto& n : song.tracks[ti].notes) {
                const Tick ov = std::min(b, n.end()) - std::max(a, n.start);
                if (ov <= 0) continue;
                const float x = float(ov) / float(step) * (0.4f + n.velocity);
                w[size_t(((n.pitch % 12) + 12) % 12)] += x;
                total += x;
                if (n.pitch < lowest && ov >= step / 4) { lowest = n.pitch; lowestW = x; }
            }
        }
        ChordLabel cl{a, step, -1, 0, ""};
        if (total > 0.05f) {
            float best = -1e9f;
            for (int r = 0; r < 12; ++r)
                for (size_t q = 0; q < tpl.size(); ++q) {
                    float in = 0;
                    for (int x : tpl[q].iv) in += w[size_t((r + x) % 12)];
                    const float outside = total - in;
                    // Vierklänge nur, wenn die Septime wirklich klingt
                    float sc = in - 0.9f * outside + tpl[q].bias * total;
                    if (tpl[q].iv.size() == 4 && w[size_t((r + tpl[q].iv[3]) % 12)] < 0.12f * total) sc -= 0.3f * total;
                    if (w[size_t(r)] < 0.05f * total) sc -= 0.4f * total;   // Grundton muss vorkommen
                    if (lowest < 999 && (lowest % 12) == r) sc += 0.25f * total + 0.1f * lowestW;
                    if (sc > best) { best = sc; cl.root = r; cl.quality = int(q); }
                }
            cl.symbol = std::string(noteName(cl.root)) + tpl[size_t(cl.quality)].name;
        }
        out.push_back(cl);
    }
    return out;
}
