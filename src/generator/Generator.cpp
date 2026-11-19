// =====================================================================
//  Song-Generator
//
//  Ablauf:
//   1. Grundparameter festlegen (Tonart, Skala, Tempo, Form)
//   2. Pro Abschnitts-Typ "Material" erfinden: Akkordfolge (Markov-Kette +
//      Bewertung), Begleit- und Bass-Stil, Melodie-Form (Periode, Satz …)
//   3. Abschnitt für Abschnitt komponieren:
//        Akkorde -> Basslinie (Viterbi, Stimmführung) -> Bass-Pattern
//        -> Melodie (Kandidaten erzeugen, vom Kritiker bewerten,
//           mit Simulated Annealing verfeinern) -> Begleitung (Stimmführung)
//   4. Mehrere komplette Songs komponieren und den besten behalten.
//
//  Der Kritiker (scoreMotif) kennt Regeln aus Kontrapunkt (Fux), Formenlehre
//  (Schönberg: Satz/Periode), Melodielehre (Höhepunkt, Sprungausgleich,
//  Leitton, Vorhalte, Durchgänge) und Songwriting (Hook, Kontrast
//  Strophe/Refrain, Register).
// =====================================================================
#include "Generator.h"
#include "analysis/Analyzer.h"
#include "instruments/InstrumentRegistry.h"
#include "audio/AudioEngine.h"
#include "audio/Effects.h"
#include "theory/Theory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>

namespace {

using Rng = std::mt19937;

float frand(Rng& r, float a = 0.f, float b = 1.f) { return std::uniform_real_distribution<float>(a, b)(r); }
int irand(Rng& r, int a, int b) { return a >= b ? a : std::uniform_int_distribution<int>(a, b)(r); }
bool chance(Rng& r, float p) { return frand(r) < p; }
int sgn(int x) { return (x > 0) - (x < 0); }

int pickWeighted(Rng& r, const std::vector<float>& w) {
    float sum = 0;
    for (float x : w) sum += std::max(0.f, x);
    if (sum <= 0.f) return irand(r, 0, int(w.size()) - 1);
    float t = frand(r, 0.f, sum);
    for (size_t i = 0; i < w.size(); ++i) {
        t -= std::max(0.f, w[i]);
        if (t <= 0.f) return int(i);
    }
    return int(w.size()) - 1;
}

template <class T>
T pickWeighted(Rng& r, const std::vector<std::pair<T, float>>& opts) {
    std::vector<float> w;
    for (auto& o : opts) w.push_back(o.second);
    return opts[pickWeighted(r, w)].first;
}

// ===================== Form =====================
enum class Sec { Intro, Verse, Pre, Chorus, Bridge, Outro };
struct SecDef {
    Sec type;
    int bars;
    float energy = 0.f;   // Energie-Abweichung dieses Abschnitts (Form-Text: "C8+" lauter, "V8-" leiser)
    bool solo = false;    // Instrumental-Solo über die Strophen-Akkorde (Form-Text: "L8")
    bool stripped = false; // Breakdown: abgespeckt, ohne Schlagzeug, nur Flächen (Form-Text: "C8~")
    bool fade = false;    // wird ausgeblendet (Form-Text: "O4>")
};

const char* secName(Sec t) {
    static const char* n[] = {"Intro", "Strophe", "Pre-Refrain", "Refrain", "Bridge", "Outro"};
    return n[int(t)];
}
char secLetter(Sec t) { return "IVPCBO"[int(t)]; }
float secBaseEnergy(Sec t) {
    static const float e[] = {0.38f, 0.52f, 0.64f, 0.84f, 0.50f, 0.42f};
    return e[int(t)];
}

std::vector<SecDef> parseStructure(const std::string& text) {
    std::vector<SecDef> out;
    std::istringstream in(text);
    std::string tok;
    while (in >> tok) {
        char c = char(std::toupper(static_cast<unsigned char>(tok[0])));
        Sec t;
        int bars;
        bool solo = false;
        switch (c) {
        case 'L': t = Sec::Verse; bars = 8; solo = true; break;
        case 'I': t = Sec::Intro; bars = 4; break;
        case 'V': case 'S': t = Sec::Verse; bars = 8; break;
        case 'P': t = Sec::Pre; bars = 4; break;
        case 'C': case 'R': t = Sec::Chorus; bars = 8; break;
        case 'B': t = Sec::Bridge; bars = 8; break;
        case 'O': t = Sec::Outro; bars = 4; break;
        default: continue;
        }
        size_t p = 1;
        while (p < tok.size() && std::isdigit(static_cast<unsigned char>(tok[p]))) ++p;
        if (p > 1) {
            try { bars = std::stoi(tok.substr(1, p - 1)); } catch (...) {}
        }
        float energy = 0.f;
        bool stripped = false, fade = false;
        for (; p < tok.size(); ++p) {
            if (tok[p] == '+') energy += 0.15f;
            else if (tok[p] == '-') energy -= 0.15f;
            else if (tok[p] == '~') stripped = true;
            else if (tok[p] == '>') fade = true;
        }
        out.push_back({t, std::clamp(bars, 1, 64), std::clamp(energy, -0.6f, 0.6f), solo, stripped, fade});
    }
    return out;
}

std::string structureText(const std::vector<SecDef>& st) {
    std::string out;
    for (auto& sd : st) {
        out += sd.solo ? 'L' : secLetter(sd.type);
        out += std::to_string(sd.bars);
        const int marks = int(std::lround(sd.energy / 0.15f));
        out += std::string(size_t(std::abs(marks)), marks > 0 ? '+' : '-');
        if (sd.stripped) out += '~';
        if (sd.fade) out += '>';
        out += ' ';
    }
    return out;
}

// ---------------------------------------------------------------------
//  Songformen: echte, unterschiedliche Bauweisen von Songs (Pop, AABA, Strophenlied, Dance …).
//  Die Vorlage wird danach an die gewünschte Länge angepasst.
//  Zeichen: I Intro (mitten im Song = Zwischenspiel), V Strophe, P Pre-Refrain, C Refrain, B Bridge,
//  O Outro, L Solo;  + / - Energie, ~ Breakdown (abgespeckt), > ausblenden
// ---------------------------------------------------------------------
struct FormDef { const char* name; const char* hint; const char* text; };
const std::vector<FormDef>& formDefs() {
    static const std::vector<FormDef> f = {
        {"Klassisch", "Strophe/Refrain im Wechsel mit Bridge - die bewährte Standardform.", ""},
        {"Pop mit Pre-Refrain", "Jede Strophe baut über einen Pre-Refrain Spannung zum Refrain auf.",
         "I4 V8 P4 C8 V8 P4 C8 B8 C8 C8+ O4"},
        {"Refrain zuerst", "Startet direkt mit dem Hook - kein Intro, sofort der Ohrwurm.", "C8 V8 C8 V8 C8 B8 C8+ O2"},
        {"Kurz & knackig", "Ohne Umwege: Strophe, Refrain, halbe Strophe, doppelter Schlussrefrain.", "V8 C8 V4 C8 C8+"},
        {"Doppelstrophe", "Zwei Strophen erzählen, bevor der erste Refrain kommt.", "I2 V8 V8 C8 V8 C8 C8+ O2"},
        {"Halbe zweite Strophe", "Die zweite Strophe ist nur halb so lang - schneller zurück zum Refrain.", "I4 V8 C8 V4 C8 B8 C8+ O4"},
        {"Mit Zwischenspiel", "Ein instrumentales Zwischenspiel trennt die beiden Hälften.", "I4 V8 C8 I4 V8 C8 B8 C8+ O4"},
        {"Breakdown vor dem Schluss", "Der vorletzte Refrain ist abgespeckt (ohne Schlagzeug), dann volle Wucht.",
         "I4 V8 C8 V8 C8 B8 C8~ C8++ O2"},
        {"Leise Anfang, großes Finale", "Beginnt zart und steigert sich Abschnitt für Abschnitt bis zum Finale.",
         "I8- V8- V8 P4 C8 B8- P4 C8++ C8++ O4"},
        {"Ballade", "Ruhige Strophen, gefühlvolle Refrains, am Ende ausgeblendet.", "I4 V8- V8 C8 V8 C8 B8- C8+ O4>"},
        {"Rock mit Solo", "Nach dem zweiten Refrain übernimmt ein Instrumental-Solo.", "I4 V8 C8 V8 C8 L8+ C8 C8+ O2"},
        {"Dance: Build-up & Drop", "Aufbau, Drop, Breakdown, wieder Aufbau und Drop - wie im Club.",
         "I8- P8 C8++ V8 P8 C8++ B8~ P8 C8++ O8->"},
        {"AABA (Jazz-Standard)", "Zwei gleiche Teile (A), eine Bridge (B), wieder A - ohne Refrain, mit Solo-Chorus.",
         "I4 V8 V8 B8 V8 L8 V8 B8 V8 O4"},
        {"Strophenlied", "Wie ein Volkslied: Strophe auf Strophe, mit kurzem Zwischenspiel, ausgeblendet.",
         "I4 V8 V8 I2 V8 V8- O4>"},
        {"Rondo (A-B-A-C-A)", "Der Refrain kehrt immer wieder, dazwischen wechselnde Teile.", "C8 V8 C8 B8 C8+ O4"},
        {"Durchkomponiert (Film)", "Erzählt eine Entwicklung: jeder Teil anders, großer Höhepunkt, leises Ende.",
         "I8- V8 P4 C8 B8- V8 C8++ O4>"},
    };
    return f;
}

// Form an die Ziellänge (in Takten) anpassen: verlängern durch längere Teile / zusätzlichen Schlussrefrain /
// Zwischenspiel, kürzen durch Weglassen von Wiederholungen
void fitForm(std::vector<SecDef>& st, int target) {
    auto total = [&] { int t = 0; for (auto& s : st) t += s.bars; return t; };
    // Sehr lange Songs: Strophen/Refrains/Soli verdoppeln (16 Takte) statt endlos wiederholen
    if (total() * 2 <= target * 1.15 && total() > 0)
        for (auto& s : st)
            if ((s.type == Sec::Verse || s.type == Sec::Chorus) && s.bars <= 8) s.bars *= 2;
            else if (s.type == Sec::Pre && s.bars <= 4) s.bars *= 2;
    // Die Kennzeichen einer Form (Zwischenspiel, Breakdown, Solo, Bridge, Ausblenden) bleiben erhalten.
    // Länge darf etwas abweichen (0,72 … 1,4 x Ziel), damit die Form nicht verstümmelt wird.
    auto special = [](const SecDef& s) { return s.stripped || s.solo || s.fade || s.type == Sec::Bridge; };
    auto same = [](const SecDef& a, const SecDef& b) {
        return a.type == b.type && a.bars == b.bars && a.stripped == b.stripped && a.solo == b.solo && a.fade == b.fade &&
               std::abs(a.energy - b.energy) < 0.01f;
    };
    for (int guard = 0; guard < 4 && total() < target * 0.72; ++guard) {
        // Verlängern: einen weiteren Durchgang Strophe (+Pre) + Refrain vor die Schluss-Refrains setzen
        int fv = -1, fc = -1;
        for (int i = 0; i < int(st.size()) && fc < 0; ++i) {
            const auto& s = st[size_t(i)];
            if (fv < 0 && s.type == Sec::Verse && !s.solo) fv = i;
            else if (fv >= 0 && s.type == Sec::Chorus) fc = i;
        }
        if (fv >= 0 && fc > fv) {
            std::vector<SecDef> cycle(st.begin() + fv, st.begin() + fc + 1);
            for (auto& c : cycle) { c.stripped = c.fade = false; c.energy = std::max(c.energy, 0.f); }
            // direkt hinter den ersten Durchgang (die Besonderheiten der Form bleiben weiter hinten)
            st.insert(st.begin() + fc + 1, cycle.begin(), cycle.end());
        } else {
            // Form ohne Refrain (AABA, Strophenlied): eine Strophe mehr vor der letzten
            int lv = -1;
            for (int i = int(st.size()) - 1; i >= 0 && lv < 0; --i)
                if (st[size_t(i)].type == Sec::Verse && !st[size_t(i)].solo) lv = i;
            if (lv < 0) break;
            SecDef extra = st[size_t(lv)];
            extra.fade = extra.stripped = false;
            extra.energy = 0.f;
            st.insert(st.begin() + lv, extra);
        }
    }
    for (int guard = 0; guard < 12 && total() > target * 1.4 && st.size() > 3; ++guard) {
        bool done = false;
        // 1. lange Intros/Outros kürzen
        for (auto& s : st)
            if (!done && (s.type == Sec::Intro || s.type == Sec::Outro) && s.bars > 2) { s.bars /= 2; done = true; }
        // 2. Teile mit 16 Takten halbieren
        for (auto& s : st)
            if (!done && s.bars >= 16) { s.bars /= 2; done = true; }
        // 3. zwei völlig gleiche Refrains direkt hintereinander -> einer
        for (size_t i = 1; i < st.size() && !done; ++i)
            if (st[i].type == Sec::Chorus && same(st[i], st[i - 1])) { st.erase(st.begin() + std::ptrdiff_t(i)); done = true; }
        // 4. einen gewöhnlichen Durchgang Strophe (+Pre) + Refrain entfernen (nicht den ersten, nichts Besonderes)
        for (size_t i = 1; i + 1 < st.size() && !done; ++i) {
            if (st[i].type != Sec::Verse || special(st[i]) || st[i - 1].type == Sec::Intro) continue;   // nach Zwischenspiel bleibt
            size_t j = i + 1;
            if (j < st.size() && st[j].type == Sec::Pre) ++j;
            if (j + 1 < st.size() && st[j].type == Sec::Chorus && !special(st[j])) {
                bool earlierChorus = false;
                for (size_t q = 0; q < i; ++q) earlierChorus = earlierChorus || st[q].type == Sec::Chorus;
                if (earlierChorus) { st.erase(st.begin() + std::ptrdiff_t(i), st.begin() + std::ptrdiff_t(j + 1)); done = true; }
            }
        }
        // 5. Form ohne Refrain: eine von mehreren gleichen Strophen weg
        int verses = 0;
        for (auto& s : st) verses += s.type == Sec::Verse && !s.solo;
        for (size_t i = 1; i < st.size() && !done && verses > 3; ++i)
            if (st[i].type == Sec::Verse && !special(st[i]) && same(st[i], st[i - 1])) { st.erase(st.begin() + std::ptrdiff_t(i)); done = true; }
        if (!done) break;
    }
}

std::vector<SecDef> autoStructure(Rng& r, int target, const GeneratorSettings& gs);

// Songform wählen: fest eingestellt oder passend zur Stimmung gewürfelt
std::vector<SecDef> chooseForm(Rng& r, int target, const GeneratorSettings& gs, int& formIndex) {
    const auto& defs = formDefs();
    int idx = gs.form;
    if (idx < 0 || idx >= int(defs.size())) {
        std::vector<float> w(defs.size(), 1.f);
        auto mul = [&](const char* name, float f) { for (size_t i = 0; i < defs.size(); ++i) if (std::string(defs[i].name) == name) w[i] *= f; };
        mul("Klassisch", 1.6f);
        const bool orch = gs.ensemble == Ensemble::Orchestra;
        if (gs.drums == DrumStyle::FourOnFloor) { mul("Dance: Build-up & Drop", 8.f); mul("Breakdown vor dem Schluss", 2.f); }
        else mul("Dance: Build-up & Drop", gs.energy > 0.7f && !orch ? 0.6f : 0.1f);
        if (gs.drums == DrumStyle::Jazz || gs.swing > 0.4f) { mul("AABA (Jazz-Standard)", 6.f); mul("Rock mit Solo", 1.5f); }
        else mul("AABA (Jazz-Standard)", 0.35f);
        if (!gs.withDrums || gs.energy < 0.35f) { mul("Strophenlied", 3.f); mul("Ballade", 2.5f); mul("Rock mit Solo", 0.3f); }
        else mul("Strophenlied", 0.4f);
        if (gs.energy > 0.7f) { mul("Rock mit Solo", 1.8f); mul("Breakdown vor dem Schluss", 2.f); mul("Leise Anfang, großes Finale", 1.8f); }
        if (gs.energy < 0.5f) mul("Ballade", 2.f);
        if (orch) {
            mul("Durchkomponiert (Film)", 3.f); mul("Rondo (A-B-A-C-A)", 2.f); mul("Leise Anfang, großes Finale", 2.5f);
            mul("Kurz & knackig", 0.4f); mul("Rock mit Solo", 0.5f);
        }
        if (gs.drums == DrumStyle::Epic) mul("Leise Anfang, großes Finale", 3.f);
        if (gs.repetition > 0.65f) { mul("Refrain zuerst", 2.f); mul("Rondo (A-B-A-C-A)", 1.8f); }
        if (target < 36) { mul("Kurz & knackig", 3.f); mul("Leise Anfang, großes Finale", 0.4f); }
        idx = int(pickWeighted(r, w));
    }
    formIndex = idx;
    std::vector<SecDef> st = defs[size_t(idx)].text[0] ? parseStructure(defs[size_t(idx)].text) : autoStructure(r, target, gs);
    if (defs[size_t(idx)].text[0]) fitForm(st, target);
    // Abschnitte, die nicht erlaubt sind, entfernen
    std::vector<SecDef> out;
    for (size_t i = 0; i < st.size(); ++i) {
        const Sec t = st[i].type;
        if (t == Sec::Intro && !gs.useIntro) continue;
        if (t == Sec::Outro && !gs.useOutro) continue;
        if (t == Sec::Pre && !gs.usePre) continue;
        if (t == Sec::Bridge && !gs.useBridge) continue;
        out.push_back(st[i]);
    }
    if (out.empty()) out = {{Sec::Verse, 8}, {Sec::Chorus, 8}};
    return out;
}

std::vector<SecDef> autoStructure(Rng& r, int target, const GeneratorSettings& gs) {
    std::vector<SecDef> out;
    if (target < 16) {
        int h = std::max(2, (target / 2 + 1) / 2 * 2);
        out = {{Sec::Verse, h}, {Sec::Chorus, std::max(2, target - h)}};
        return out;
    }
    const bool intro = gs.useIntro && target >= 20 && chance(r, 0.85f);
    const bool outro = gs.useOutro && target >= 28 && chance(r, 0.8f);
    const bool pre = gs.usePre && target >= 44 && chance(r, 0.4f);
    const bool bridge = gs.useBridge && target >= 40 && chance(r, 0.8f);
    const int introBars = (target >= 32 && chance(r, 0.7f)) ? 4 : 2;
    const int outroBars = chance(r, 0.5f) ? 4 : 2;
    const int bridgeBars = chance(r, 0.5f) ? 8 : 4;
    // Sehr lange Songs (schnelles Tempo, 3/4): doppelt so lange Abschnitte statt endloser Wiederholungen
    const int U = target >= 88 ? 16 : 8;
    const int preBars = U == 16 ? 8 : 4;
    const int cycle = U + (pre ? preBars : 0) + U;
    const int fixed = (intro ? introBars : 0) + (outro ? outroBars : 0) + (bridge ? bridgeBars + U : 0);
    const int cycles = std::max(1, int(std::lround(double(target - fixed) / cycle)));

    if (intro) out.push_back({Sec::Intro, introBars});
    for (int c = 0; c < cycles; ++c) {
        out.push_back({Sec::Verse, U});
        if (pre) out.push_back({Sec::Pre, preBars});
        out.push_back({Sec::Chorus, U});
    }
    if (bridge) {
        out.push_back({Sec::Bridge, bridgeBars});
        out.push_back({Sec::Chorus, U});
    }
    int total = outro ? outroBars : 0;
    for (auto& sd : out) total += sd.bars;
    if (target - total >= U * 3 / 4) out.push_back({Sec::Chorus, U});   // Schluss-Refrain doppelt
    if (outro) out.push_back({Sec::Outro, outroBars});
    return out;
}

// ===================== Harmonik =====================
// Übergangswahrscheinlichkeiten zwischen Stufen (Funktionsharmonik)
using Table = std::array<std::array<float, 7>, 7>;
const Table kMajorT = {{
    {0.03f, 0.14f, 0.08f, 0.28f, 0.22f, 0.23f, 0.02f},   // I
    {0.08f, 0.00f, 0.04f, 0.12f, 0.60f, 0.08f, 0.08f},   // ii
    {0.04f, 0.12f, 0.00f, 0.34f, 0.08f, 0.42f, 0.00f},   // iii
    {0.26f, 0.12f, 0.06f, 0.00f, 0.36f, 0.16f, 0.04f},   // IV
    {0.52f, 0.04f, 0.06f, 0.14f, 0.00f, 0.24f, 0.00f},   // V
    {0.06f, 0.26f, 0.10f, 0.34f, 0.22f, 0.00f, 0.02f},   // vi
    {0.70f, 0.00f, 0.20f, 0.00f, 0.05f, 0.05f, 0.00f},   // vii°
}};
const Table kMinorT = {{
    {0.02f, 0.05f, 0.15f, 0.25f, 0.13f, 0.22f, 0.18f},   // i
    {0.10f, 0.00f, 0.05f, 0.05f, 0.65f, 0.05f, 0.10f},   // ii°
    {0.10f, 0.03f, 0.00f, 0.30f, 0.07f, 0.30f, 0.20f},   // III
    {0.25f, 0.05f, 0.07f, 0.00f, 0.28f, 0.15f, 0.20f},   // iv
    {0.55f, 0.00f, 0.05f, 0.10f, 0.00f, 0.25f, 0.05f},   // v
    {0.15f, 0.10f, 0.20f, 0.20f, 0.05f, 0.00f, 0.30f},   // VI
    {0.35f, 0.00f, 0.40f, 0.05f, 0.05f, 0.15f, 0.00f},   // VII
}};

// Bewährte Pop-Loops (Stufen)
const std::vector<std::vector<int>> kMajorLoops = {
    {0, 4, 5, 3}, {5, 3, 0, 4}, {0, 5, 3, 4}, {0, 3, 5, 4}, {3, 0, 4, 5}, {0, 3, 0, 4}, {0, 2, 3, 4}, {5, 4, 3, 4},
};
const std::vector<std::vector<int>> kMinorLoops = {
    {0, 5, 2, 6}, {0, 6, 5, 6}, {0, 3, 6, 2}, {0, 5, 3, 4}, {0, 3, 4, 0}, {5, 6, 0, 0}, {0, 6, 5, 4},
};

enum class End { Loop, Half, Tonic };

// ===================== Datenstrukturen =====================
struct TChord {
    Chord chord;
    Tick start = 0;
    Tick len = 0;
    int bass = -1;   // gewählte Basstonhöhe (Umkehrungen möglich)
};

struct MNote {
    Tick start;
    Tick len;
    int deg;   // Stufe in der Melodie-Skala
};
using Motif = std::vector<MNote>;

// Orchester-Stimmen (Partitur-Reihenfolge)
enum Orch { FL, OB, CL, FG, HN, TP, TB, TI, PC, TK, GL, HP, CH, V1, V2, VA, VC, KB, PZ, TU, PI, CB, OCOUNT };

// Orchester-Techniken
enum class Tex { Pad, Ostinato8, Ostinato16, Arpeggio, Tremolo, Pizz, Stabs, Chorale, Waltz, March, Count };
enum class MelT { Solo, Octave, Unison, Thirds, CallResponse, Canon, Count };
enum class BassT { Sustain, Pedal, LowOstinato, Walking, Pizz, Count };
enum class PercT { None, TimpHits, TimpOstinato, EpicHits, SnareMarch, Count };

// Was ein Abschnitt orchestral macht
struct OrchPlan {
    Tex tex = Tex::Pad;
    MelT mel = MelT::Solo;
    BassT bass = BassT::Sustain;
    PercT perc = PercT::None;
    int lead = V1, second = FL;     // Melodie-Instrument und Partner (Verdopplung, Terzen, Antwort, Kanon)
    bool counter = false;           // Gegenstimme
    int counterInst = HN;
    bool sparkle = false;           // Glockenspiel im Refrain
    bool gliss = false;             // Harfen-Glissando in den nächsten Refrain
    bool crescendo = false;
    // epische Mittel
    bool choir = false;             // Chor ("Aah")
    int choirMode = 0;              // 0 = Akkordfläche, 1 = singt die Melodie mit
    bool braams = false;            // tiefes Blech fortissimo
    bool taiko = false;             // Taiko-Ostinato
    bool brassChorale = false;      // Blechbläser-Choral (Posaunen, Hörner, Trompeten als Akkord)
    bool fanfare = false;           // Trompeten-Fanfare
    // Spannung aufbauen / Ausrufezeichen
    bool timpRoll = false;          // Paukenwirbel mit Crescendo in den Refrain
    bool tremoloBuild = false;      // Streicher-Tremolo schwillt im letzten Takt an
    bool brassSwell = false;        // Blech: leise ansetzen und anschwellen (fp<)
    bool snareRoll = false;         // Trommelwirbel (Marsch) vor dem Refrain
    bool accentHit = false;         // "Schicksalsschlag": Pauke + große Trommel + Becken auf der Eins
};

// ---------------------------------------------------------------------
//  Kompositions-Systeme: verschiedene, erprobte Bauweisen für Harmonie, Rhythmus und Melodie.
//  Jeder Song würfelt sich eine kleine Palette davon, jeder Abschnitt nimmt sich daraus eins.
// ---------------------------------------------------------------------
enum class HarmSys { Classic, Loop, Fifths, Vamp, Pedal, Andalusian, LineCliche, Mediant, Plagal, Canon, Count };
enum class RhySys { Straight, Tresillo, Clave, Offbeat, Bossa, Driving, Count };
enum class MelSys { Motif, PentaHook, Riff, Arpeggio, Chorale, Count };
const char* harmSysName(HarmSys h) {
    static const char* n[] = {"Funktionsharmonik", "Pop-Loop", "Quintfall", "Modaler Vamp", "Orgelpunkt",
                              "Andalusische Kadenz", "Line Cliché", "Chromatische Mediante", "Plagal (Gospel)", "Kanon-Folge"};
    return n[int(h)];
}
const char* rhySysName(RhySys r) {
    static const char* n[] = {"Gerade", "Tresillo (3-3-2)", "Clave (3-2)", "Offbeat (Reggae)", "Bossa", "Treibende Achtel"};
    return n[int(r)];
}
const char* melSysName(MelSys m) {
    static const char* n[] = {"Motivisch", "Pentatonik-Hook", "Riff", "Arpeggio-Thema", "Choral"};
    return n[int(m)];
}

struct Material {
    Sec type{};
    std::vector<int> degrees;
    std::vector<Chord> chords;
    double barsPerChord = 1.0;
    AccompStyle acc = AccompStyle::Pad;
    BassStyle bass = BassStyle::Whole;
    int rhythmPattern = 0;
    bool arp16 = false, bassOctaves = false;
    bool melody = true;
    float density = 0.5f;
    int centerOffset = 0;
    int contour = -1;            // -1 = frei, 0 Bogen, 1 steigend, 2 fallend, 3 Tal
    std::string form = "ABAC";   // Motiv-Folge pro 2-Takt-Einheit
    bool ownMotifs = true;
    Sec motifOwner{};
    std::map<char, Motif> motifs;
    int occurrences = 0;

    // Schlagzeug
    bool drumsOn = true;
    bool drumsLight = false;      // nur Hi-Hat + Kick (Intro)
    DrumStyle drumStyle = DrumStyle::Pop;
    int kickVariant = 0;
    bool openHats = false, ride = false;
    float ghost = 0.f;            // Geisternoten auf der Snare

    // Eigene Idee (eingespielt)
    bool hasIdea = false, ideaHarmonic = false;
    std::vector<Note> ideaNotes;
    int ideaBars = 0;

    // Orchester: Plan, welche Techniken dieser Abschnitt nutzt
    OrchPlan orch;
    int orchMelody = V1;

    // Kompositions-Systeme dieses Abschnitts
    HarmSys harmSys = HarmSys::Classic;
    RhySys rhySys = RhySys::Straight;
    MelSys melSys = MelSys::Motif;
    bool pedal = false;            // Orgelpunkt: Bass bleibt auf dem Grundton
    float variationMul = 1.f;      // Riff: kaum Veränderung
};

// ---------------------------------------------------------------------
//  Orchester-Stile: jeder Stil gewichtet die Techniken anders.
//  Pro Song wird ein Stil gewürfelt, pro Abschnitt die Techniken (passend zur Energie).
// ---------------------------------------------------------------------
struct OrchProfile {
    const char* name;
    float tex[int(Tex::Count)];      // Pad, Ost8, Ost16, Arpeggio, Tremolo, Pizz, Stabs, Choral, Walzer, Marsch
    float mel[int(MelT::Count)];     // Solo, Oktave, Unisono, Terzen, Frage-Antwort, Kanon
    float bass[int(BassT::Count)];   // gehalten, Orgelpunkt, tiefes Ostinato, Walking, Pizzicato
    float perc[int(PercT::Count)];   // keins, Pauken-Schläge, Pauken-Ostinato, epische Schläge, Marsch-Snare
    std::vector<int> leads;          // bevorzugte Melodie-Instrumente
    float counter;                   // Chance auf Gegenstimme
    float sparkle;                   // Chance auf Glockenspiel / Harfen-Glissando
    bool brass;                      // Blechbläser erlaubt
};

const std::vector<OrchProfile>& orchProfiles() {
    static const std::vector<OrchProfile> p = {
        {"Episch (Film)",
         {0.3f, 0.6f, 0.8f, 0.2f, 0.5f, 0.f, 0.4f, 0.2f, 0.f, 0.f}, {0.3f, 0.6f, 0.7f, 0.2f, 0.1f, 0.1f},
         {0.3f, 0.3f, 0.8f, 0.f, 0.f}, {0.1f, 0.5f, 0.7f, 0.6f, 0.1f}, {V1, HN, TP, VC}, 0.5f, 0.4f, true},
        {"Romantisch",
         {0.8f, 0.1f, 0.f, 0.7f, 0.2f, 0.1f, 0.f, 0.3f, 0.1f, 0.f}, {0.6f, 0.5f, 0.1f, 0.4f, 0.4f, 0.2f},
         {0.7f, 0.3f, 0.f, 0.2f, 0.1f}, {0.5f, 0.5f, 0.f, 0.f, 0.f}, {V1, VC, OB, CL, HN, FL}, 0.6f, 0.4f, true},
        {"Pastoral",
         {0.3f, 0.2f, 0.f, 0.5f, 0.f, 0.8f, 0.1f, 0.2f, 0.2f, 0.f}, {0.7f, 0.2f, 0.f, 0.6f, 0.6f, 0.3f},
         {0.3f, 0.3f, 0.f, 0.3f, 0.8f}, {0.8f, 0.2f, 0.f, 0.f, 0.f}, {FL, OB, CL, V1, HN}, 0.4f, 0.5f, true},   // Pastoral: Hörner-Rufe gehören dazu
        {"Marsch / Heroisch",
         {0.2f, 0.3f, 0.f, 0.f, 0.f, 0.f, 0.6f, 0.3f, 0.f, 0.9f}, {0.3f, 0.6f, 0.5f, 0.4f, 0.2f, 0.f},
         {0.4f, 0.1f, 0.3f, 0.2f, 0.2f}, {0.f, 0.5f, 0.2f, 0.3f, 0.9f}, {TP, HN, V1, FL}, 0.4f, 0.3f, true},
        {"Walzer",
         {0.2f, 0.f, 0.f, 0.2f, 0.f, 0.3f, 0.f, 0.f, 1.f, 0.f}, {0.5f, 0.4f, 0.f, 0.5f, 0.3f, 0.1f},
         {0.3f, 0.1f, 0.f, 0.f, 0.5f}, {0.6f, 0.3f, 0.f, 0.f, 0.f}, {V1, FL, OB, CL}, 0.4f, 0.4f, true},
        {"Mysteriös",
         {0.5f, 0.3f, 0.f, 0.3f, 0.9f, 0.3f, 0.f, 0.f, 0.f, 0.f}, {0.8f, 0.2f, 0.f, 0.f, 0.4f, 0.4f},
         {0.5f, 0.8f, 0.3f, 0.f, 0.f}, {0.5f, 0.3f, 0.2f, 0.f, 0.f}, {CL, OB, VA, VC, FL, HN}, 0.5f, 0.3f, true},
        {"Barock / Kammer",
         {0.2f, 0.4f, 0.f, 0.5f, 0.f, 0.f, 0.f, 0.4f, 0.f, 0.f}, {0.4f, 0.f, 0.f, 0.6f, 0.5f, 0.8f},
         {0.2f, 0.f, 0.f, 0.9f, 0.f}, {0.9f, 0.1f, 0.f, 0.f, 0.f}, {V1, OB, FL, TP}, 0.7f, 0.1f, true},        // Barock: Barocktrompete
        {"Minimal",
         {0.4f, 0.6f, 0.5f, 0.9f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f}, {0.5f, 0.f, 0.f, 0.f, 0.4f, 0.5f},
         {0.5f, 0.6f, 0.f, 0.f, 0.f}, {0.7f, 0.f, 0.2f, 0.f, 0.f}, {CL, FL, VC, V1}, 0.3f, 0.3f, false},
    };
    return p;
}

const char* texName(Tex t) {
    static const char* n[] = {"Streicher-Flächen", "Ostinato (Achtel)", "Ostinato (16tel)", "Arpeggien", "Tremolo",
                              "Pizzicato", "Akzent-Stöße", "Choralsatz", "Walzer-Begleitung", "Marsch-Begleitung"};
    return n[int(t)];
}
const char* melTName(MelT t) {
    static const char* n[] = {"Solo", "Oktav-Verdopplung", "Unisono-Tutti", "Terzen-Satz", "Frage-Antwort", "Kanon"};
    return n[int(t)];
}
const char* bassTName(BassT t) {
    static const char* n[] = {"gehaltener Bass", "Orgelpunkt", "tiefes Ostinato", "Walking Bass", "Pizzicato-Bass"};
    return n[int(t)];
}
const char* percTName(PercT t) {
    static const char* n[] = {"", "Pauken-Schläge", "Pauken-Ostinato", "Große Trommel + Becken", "Marsch-Snare"};
    return n[int(t)];
}
const char* orchInstName(int i) {
    static const char* n[] = {"Flöte", "Oboe", "Klarinette", "Fagott", "Hörner", "Trompeten", "Posaunen", "Pauken",
                              "Schlagwerk", "Taiko", "Glockenspiel", "Harfe", "Chor", "Violinen I", "Violinen II",
                              "Bratschen", "Celli", "Kontrabässe", "Streicher pizz.", "Tuba", "Piccolo", "Cembalo"};
    return n[std::clamp(i, 0, int(OCOUNT) - 1)];
}

struct MelEnv {
    const std::vector<TChord>* chords = nullptr;
    Tick uStart = 0, uLen = 0, secStart = 0;
    int role = 0;                // 0 = keine Kadenz, 1 = offen (Halbschluss), 2 = geschlossen
    int lo = 0, hi = 0, center = 0;
    int contour = -1;
    Sec type = Sec::Verse;
    float density = 0.5f;
    const std::vector<Note>* bass = nullptr;
    std::vector<std::pair<const Motif*, float>> simTargets;   // gewünschte Ähnlichkeit zu anderen Motiven
    const Motif* registerRef = nullptr;                        // soll höher liegen als dieses Motiv
    float registerGap = 0;
    int prevPitch = -1;                                        // letzter Ton davor (für saubere Anschlüsse)
};

struct Resolved {
    int key = 0;
    ScaleType scale = ScaleType::Major;
    double bpm = 110;
    int bpb = 4;
    std::vector<SecDef> structure;
    int keyChangeAt = -1;
    int keyChangeSemis = 0;
    DrumStyle drumStyle = DrumStyle::Pop;   // Grund-Stil des Songs
    bool hasIdea = false;
    Sec ideaType = Sec::Verse;
    int ideaBars = 0;
    int orchStyle = 0;
    int formIndex = -1;                     // gewählte Songform (-1 = eigene Form aus dem Text)
};

// ===================== Builder =====================
class Builder {
public:
    Builder(const GeneratorSettings& s, const Resolved& rs, uint32_t seed, std::atomic<float>* progress,
            float progBase, float progSpan)
        : s_(s), rs_(rs), r_(seed), mel_(rs.key, rs.scale), harm_(rs.key, harmonyScaleFor(rs.scale)),
          keyRoot_(rs.key), minorKey_(scaleIsMinor(rs.scale)), progress_(progress), progBase_(progBase),
          progSpan_(progSpan) {}

    Song build();
    float score() const { return scoreCount_ ? scoreSum_ / scoreCount_ : -1e9f; }
    int evaluated() const { return evaluated_; }
    std::vector<std::string> progressionInfo() const;
    std::vector<std::string> orchInfo() const;   // eingesetzte Orchester-Techniken pro Abschnitt
    std::vector<std::string> systemsInfo() const;   // Kompositions-Systeme pro Abschnitt

private:
    // --- Helfer ---
    int effort() const { return std::clamp(s_.quality, 1, 10); }
    int strengthAt(Tick t) const;
    const Chord& chordAt(const std::vector<TChord>& tc, Tick t) const;
    int realize(int deg, const Chord& c) const;
    bool isCT(int deg, const Chord& c) const { return c.hasPc(mod12(realize(deg, c))); }
    float roleWeight(int deg, int role) const;
    float energyOf(Sec t) const { return std::clamp(secBaseEnergy(t) + (s_.energy - 0.5f) * 0.5f, 0.f, 1.f); }
    // Energie eines konkreten Abschnitts (inkl. Abweichung aus dem Form-Text und Solo-Schub)
    float secEnergy(size_t k) const {
        const SecDef& sd = rs_.structure[std::min(k, rs_.structure.size() - 1)];
        return std::clamp(energyOf(sd.type) + sd.energy + (sd.stripped ? -0.25f : 0.f), 0.f, 1.f);
    }
    Chord dominant();
    Chord tonic() { return diatonicChord(harm_, 0, false); }

    // --- Harmonik ---
    Table table() const;
    std::vector<int> genProgression(int n, const std::vector<int>& starts, End end,
                                    const std::vector<std::vector<int>>& avoid);
    std::vector<Chord> colorize(const std::vector<int>& degs);
    void makeMaterials();
    void makeHarmony(Material& m);
    std::vector<TChord> tile(const Material& m, int bars) const;

    // --- Bass ---
    void assignBassLine(std::vector<TChord>& tc);
    int fifthAbove(const TChord& tc) const;
    void genBass(const Material& m, const std::vector<TChord>& tc, Tick secStart, float vel, std::vector<Note>& out);

    // --- Eigene Idee ---
    void applyIdea(Material& m);
    std::vector<Chord> harmonizeIdea(const std::vector<Note>& notes, int bars);
    std::vector<Chord> recognizeChords(const std::vector<Note>& notes, int bars);

    // --- Orchester ---
    void planOrchestra(Material& m, Sec t, float e);
    void orchestrate(const Material& m, const SecDef& sd, size_t k, const std::vector<TChord>& tc, Tick secStart,
                     int transpose, float vel, const std::vector<Note>& mel, const std::vector<Note>& acc,
                     const std::vector<Note>& bass);

    // --- Schlagzeug ---
    void genDrums(const Material& m, const SecDef& sd, size_t k, Tick secStart, float vel, std::vector<Note>& out);
    bool bigFinale_ = false;   // Orchester: Paukenwirbel + Schluss-Schlag am Ende
    static bool isInst(const std::string& a, std::initializer_list<const char*> l) {
        for (const char* x : l) if (a == x) return true;
        return false;
    }
    bool islandSong() const {   // Insel-/Latin-Klangfarben -> Hand-Percussion passt immer
        return isInst(s_.melodyInstrument, {"Steel Drum", "Marimba", "Ukulele", "Kalimba", "Panflöte"}) ||
               isInst(s_.chordInstrument, {"Ukulele", "Steel Drum", "Marimba"});
    }
    bool handPerc(const Material& m) const {
        return bpb_ == 4 && (islandSong() || m.rhySys == RhySys::Tresillo || m.rhySys == RhySys::Clave ||
                             m.rhySys == RhySys::Bossa || m.rhySys == RhySys::Offbeat);
    }
    bool sparkleSong() const {
        return isInst(s_.melodyInstrument, {"Glocken", "Celesta", "Kalimba", "Harfe", "Vibraphon"}) ||
               isInst(s_.chordInstrument, {"Harfe", "Celesta", "Glocken"});
    }

    // --- Begleitung ---
    std::vector<int> voice(const Chord& c);
    void genAccomp(const Material& m, const std::vector<TChord>& tc, Tick secStart, float vel, std::vector<Note>& out);

    // --- Melodie ---
    std::vector<std::pair<Tick, Tick>> genRhythm(Tick unitTicks, float density);
    float contourAt(int shape, float amp, float p) const;
    float noteWeight(int d, int prev, int prev2, const Chord& c, int strength, Tick len, float target) const;
    Motif pitchesFor(const std::vector<std::pair<Tick, Tick>>& rhythm, const MelEnv& e);
    Motif randomMotif(const MelEnv& e);
    Motif derivedMotif(const Motif& rel, const MelEnv& e, int mode);
    Motif mutate(const Motif& m, const MelEnv& e);
    Motif snapToChords(Motif m, const MelEnv& e) const;
    Motif fitToUnit(const Motif& m, Tick uLen) const;
    float scoreMotif(const Motif& m, const MelEnv& e);
    float similarity(const Motif& a, const Motif& b) const;
    float barRhythmSimilarity(const Motif& m) const;
    int bassAt(const MelEnv& e, Tick relT) const;
    Motif composeMotif(const MelEnv& e, const Motif* related);
    Motif varyMotif(const Motif& src, const MelEnv& e, float variation);
    // Improvisiertes Instrumental-Solo über die Akkorde eines Abschnitts
    void genSolo(const std::vector<TChord>& tc, Tick secStart, int bars, float vel, std::vector<Note>& out);
    void genMelody(Material& m, const SecDef& sd, const std::vector<TChord>& tc, Tick secStart, bool lastSec,
                   float vel, const std::vector<Note>* bass, std::vector<Note>& out);

    void addNotes(Track& t, const std::vector<Note>& notes, int transpose);
    void resolveClashes();
    void applyDynamics();
    std::vector<TChord> chordLine_;   // Akkordverlauf des ganzen Songs (absolut, transponiert)
    std::set<std::tuple<std::string, Tick, int>> melodyKeys_, bassKeys_;   // (Spur, Start, Ton) fester Noten
    std::set<std::tuple<std::string, Tick, int>> partnerKeys_;             // abgeleitete 2. Stimmen (Terzen, Kanon)
    void finish();

    const GeneratorSettings& s_;
    const Resolved& rs_;
    Rng r_;
    Scale mel_, harm_;
    int keyRoot_;
    bool minorKey_;
    std::atomic<float>* progress_;
    float progBase_, progSpan_;

    Song song_;
    Tick bar_ = 1920, beat_ = 480, six_ = 120;
    int bpb_ = 4;
    std::map<Sec, Material> mats_;
    std::vector<Sec> matOrder_;
    std::vector<int> prevVoicing_;
    int prevBass_ = -1;
    int lastMelDeg_ = INT_MIN;
    bool chordSustains_ = false;   // Akkord-Instrument hält Töne (Streicher, Orgel …)
    bool epic_ = false;            // epischer Stil (Orchester "Episch" oder Band-Schlagzeug "Episch")
    // Kompositions-Systeme: Palette des Songs + Melodie-Einstellungen des gerade komponierten Abschnitts
    std::vector<HarmSys> harmPal_;
    std::vector<RhySys> rhyPal_;
    std::vector<MelSys> melPal_;
    float melCtBias_ = 1.f, melLeapMul_ = 1.f;
    bool melPenta_ = false;
    void choosePalette();
    bool systemHarmony(Material& m);   // true = Akkorde vom System gesetzt
    int baseCenterPitch_ = 70;     // mittlere Melodielage (bei eigener Idee: deren Lage)
    std::vector<Track> orch_;      // Orchester-Stimmen
    float scoreSum_ = 0;
    int scoreCount_ = 0;
    int evaluated_ = 0;
};

// ---------------------------------------------------------------------
int Builder::strengthAt(Tick t) const {
    Tick pos = t % bar_;
    if (pos == 0) return 3;                                // Takt-Eins
    if (pos % beat_ == 0) return (bpb_ == 4 && pos == 2 * beat_) ? 2 : 1;
    return 0;                                              // Off-Beat
}

const Chord& Builder::chordAt(const std::vector<TChord>& tc, Tick t) const {
    for (auto it = tc.rbegin(); it != tc.rend(); ++it)
        if (t >= it->start) return it->chord;
    return tc.front().chord;
}

// Skalenton passend zum Akkord: Ist ein Akkordton nicht in der Melodie-Skala
// (z.B. Leitton bei V in Moll), wird der benachbarte Skalenton chromatisch angepasst.
int Builder::realize(int deg, const Chord& c) const {
    int p = mel_.pitch(deg);
    int pc = mod12(p);
    if (c.hasPc(pc)) return p;
    for (int d : {1, -1}) {
        int q = mod12(pc + d);
        if (c.hasPc(q) && !mel_.contains(q)) return p + d;
    }
    return p;
}

// Wie gut eignet sich ein Ton als Schlusston einer Phrase?
float Builder::roleWeight(int deg, int role) const {
    int pc = mod12(mel_.pitch(deg) - keyRoot_);
    if (role == 2) return pc == 0 ? 5.f : (pc == 3 || pc == 4) ? 2.f : pc == 7 ? 1.f : 0.3f;   // Grundton
    if (role == 1) return pc == 7 ? 3.f : pc == 2 ? 2.5f : (pc == 11 || pc == 5) ? 1.4f : pc == 0 ? 0.5f : 1.f;
    return 1.f;
}

// Feinheiten: Anteil Septakkorde / Farbakkorde (-1 = aus Kreativität)
static float seventhChance(const GeneratorSettings& s) { return s.jazzChords >= 0.f ? s.jazzChords * 0.85f : s.creativity * 0.4f; }
static float colorFactor(const GeneratorSettings& s) { return s.colorChords >= 0.f ? s.colorChords * 2.2f : s.creativity; }

Chord Builder::dominant() {
    bool seventh = chance(r_, 0.3f + seventhChance(s_) * 0.75f);
    if (harm_.type() == ScaleType::Major) return diatonicChord(harm_, 4, seventh);
    return makeChord(keyRoot_ + 7, seventh ? std::vector<int>{0, 4, 7, 10} : std::vector<int>{0, 4, 7},
                     seventh ? "V7" : "V");
}

// ===================== Harmonik =====================
Table Builder::table() const {
    Table T = minorKey_ ? kMinorT : kMajorT;
    int flavor = -1;   // charakteristische Stufe der Kirchentonart betonen
    switch (harm_.type()) {
    case ScaleType::Dorian: flavor = 3; break;       // IV-Dur
    case ScaleType::Mixolydian: flavor = 6; break;   // bVII
    case ScaleType::Lydian: flavor = 1; break;       // II-Dur
    case ScaleType::Phrygian: flavor = 1; break;     // bII
    default: break;
    }
    if (flavor >= 0)
        for (int i = 0; i < 7; ++i)
            if (i != flavor) T[i][flavor] += 0.15f;
    return T;
}

std::vector<int> Builder::genProgression(int n, const std::vector<int>& starts, End end,
                                         const std::vector<std::vector<int>>& avoid) {
    const Table T = table();
    // Verminderte Akkorde (z.B. vi° in Dorisch) sind schwache Anfangs-/Schlussakkorde
    std::array<bool, 7> dim{};
    for (int d = 0; d < 7; ++d) {
        Chord c = diatonicChord(harm_, d, false);
        dim[d] = c.intervals.size() >= 3 && c.intervals[2] == 6;
    }
    std::vector<int> okStarts;
    for (int st : starts) if (!dim[st]) okStarts.push_back(st);
    if (okStarts.empty()) okStarts = {0};

    std::vector<int> best;
    float bestScore = -1e9f;
    const int tries = 300 + effort() * 150;
    for (int t = 0; t < tries; ++t) {
        std::vector<int> p(n);
        p[0] = okStarts[irand(r_, 0, int(okStarts.size()) - 1)];
        for (int i = 1; i < n; ++i) {
            std::vector<float> w(7);
            for (int j = 0; j < 7; ++j) w[j] = T[p[i - 1]][j] + (j != p[i - 1] ? s_.creativity * 0.05f : 0.f);
            p[i] = pickWeighted(r_, w);
        }
        float sc = 0;
        for (int i = 1; i < n; ++i) sc += std::log(T[p[i - 1]][p[i]] + 0.01f);
        // Grundton-Bewegung: Quartsprung aufwärts / Terz abwärts sind besonders "funktional"
        for (int i = 1; i < n; ++i) {
            int mv = ((p[i] - p[i - 1]) % 7 + 7) % 7;
            if (mv == 3) sc += 0.4f;
            else if (mv == 5) sc += 0.25f;
        }
        if (end == End::Loop) sc += std::log(T[p.back()][p[0]] + 0.01f);
        if (end == End::Half) sc += p.back() == 4 ? 2.f : p.back() == 3 ? 0.4f : -1.5f;
        if (end == End::Tonic) sc += (p.back() == 0 ? 3.f : -3.f) + ((n >= 2 && (p[n - 2] == 4 || p[n - 2] == 3)) ? 1.5f : 0.f);
        std::set<int> distinct(p.begin(), p.end());
        if (int(distinct.size()) < std::min(n, 3)) sc -= 3.f;
        // Tonika soll vorkommen (Kirchentonarten brauchen sie besonders, sonst kippt das Zentrum)
        const bool modal = harm_.type() != ScaleType::Major && harm_.type() != ScaleType::NaturalMinor;
        if (std::find(p.begin(), p.end(), 0) != p.end()) sc += modal ? 2.f : 1.f;
        else if (!modal && std::find(p.begin(), p.end(), 5) != p.end()) sc += 0.4f;
        for (int i = 0; i < n; ++i)
            if (dim[p[i]]) sc -= (i == 0 || i == n - 1) ? 4.f : 1.5f;
        for (auto& a : avoid)
            if (a == p) sc -= 8.f;
        sc += frand(r_, 0.f, 1.5f + s_.creativity * 2.f);   // Zufall: immer etwas anderes
        if (sc > bestScore) { bestScore = sc; best = p; }
    }
    return best;
}

std::vector<Chord> Builder::colorize(const std::vector<int>& degs) {
    const bool majorLike = harm_.type() == ScaleType::Major;
    const bool natMinor = harm_.type() == ScaleType::NaturalMinor;
    const float seventh = seventhChance(s_);
    std::vector<Chord> out;
    for (size_t i = 0; i < degs.size(); ++i) {
        const int d = degs[i];
        Chord c = diatonicChord(harm_, d, chance(r_, seventh));
        const float vChance = rs_.scale == ScaleType::HarmonicMinor ? 1.f : 0.65f;
        if (natMinor && d == 4 && chance(r_, vChance)) {
            // Dur-Dominante in Moll (Leitton!)
            bool s7 = chance(r_, seventh + 0.2f);
            c = makeChord(keyRoot_ + 7, s7 ? std::vector<int>{0, 4, 7, 10} : std::vector<int>{0, 4, 7}, s7 ? "V7" : "V");
        } else if (chance(r_, colorFactor(s_) * 0.15f)) {
            // Geliehene Akkorde (Modal Interchange)
            if (majorLike && d == 3) c = makeChord(keyRoot_ + 5, {0, 3, 7}, "iv");
            else if (majorLike && d == 6) c = makeChord(keyRoot_ + 10, {0, 4, 7}, "bVII");
            else if (majorLike && d == 5 && chance(r_, 0.4f)) c = makeChord(keyRoot_ + 8, {0, 4, 7}, "bVI");
            else if (natMinor && d == 3 && chance(r_, 0.5f)) c = makeChord(keyRoot_ + 5, {0, 4, 7}, "IV");
        }
        // Orchester: große Septime (maj7) weglassen – als lange Fläche in Chor/Blech reibt sie hörbar
        if (s_.ensemble == Ensemble::Orchestra && c.intervals.size() > 3 && c.intervals[3] == 11)
            c = makeChord(c.root, {c.intervals[0], c.intervals[1], c.intervals[2]},
                          c.roman.size() > 4 && c.roman.substr(c.roman.size() - 4) == "maj7" ? c.roman.substr(0, c.roman.size() - 4) : c.roman);
        out.push_back(c);
    }
    // Zwischendominanten: V7 des folgenden Akkords
    for (size_t i = 2; i < out.size(); ++i) {
        const Chord& target = out[i];
        bool dim = target.intervals.size() >= 3 && target.intervals[2] == 6;
        if (!dim && mod12(target.root - keyRoot_) != 0 && chance(r_, colorFactor(s_) * 0.18f))
            out[i - 1] = makeChord(target.root + 7, {0, 4, 7, 10}, "V7/" + target.roman);
    }
    return out;
}

// ===================== Kompositions-Systeme =====================
// Palette des Songs: aus jedem Bereich 1-3 Systeme, gewichtet nach Tonleiter, Tempo, Energie und Besetzung
void Builder::choosePalette() {
    harmPal_.clear();
    rhyPal_.clear();
    melPal_.clear();
    if (!s_.mixSystems) return;
    const float v = std::clamp(s_.systemVariety, 0.f, 1.f);
    const bool orch = s_.ensemble == Ensemble::Orchestra;
    const ScaleType sc = rs_.scale;
    const bool modal = sc == ScaleType::Dorian || sc == ScaleType::Mixolydian || sc == ScaleType::Phrygian || sc == ScaleType::Lydian;
    const bool loopsOk = harm_.type() == ScaleType::Major || harm_.type() == ScaleType::NaturalMinor;
    auto pickN = [&](auto opts, int n, auto& out) {
        for (int k = 0; k < n; ++k) {
            float total = 0;
            for (auto& o : opts) total += o.second;
            if (total <= 0.f) break;
            auto chosen = pickWeighted(r_, opts);
            out.push_back(chosen);
            for (auto& o : opts) if (o.first == chosen) o.second = 0.f;
        }
    };
    using H = HarmSys;
    std::vector<std::pair<H, float>> h = {
        {H::Classic, 1.2f - v * 0.6f},
        {H::Loop, loopsOk ? 0.9f : 0.f},
        {H::Fifths, 0.6f + (seventhChance(s_) > 0.35f ? 1.f : 0.f)},
        {H::Vamp, modal ? 2.5f : 0.5f},
        {H::Pedal, (orch || epic_) ? 0.8f : 0.35f},
        {H::Andalusian, minorKey_ ? 0.7f : 0.f},
        {H::LineCliche, orch ? 0.f : (minorKey_ ? 0.6f : 0.35f)},
        {H::Mediant, (orch || epic_) ? 0.9f : 0.3f},
        {H::Plagal, minorKey_ ? 0.f : 0.5f},
        {H::Canon, minorKey_ ? 0.35f : 0.6f},
    };
    pickN(h, 1 + int(std::lround(v * 2.f)), harmPal_);

    using R = RhySys;
    if (!orch && bpb_ == 4) {
        const double bpm = rs_.bpm;
        std::vector<std::pair<R, float>> r = {
            {R::Straight, 1.4f - v * 0.6f},
            {R::Tresillo, 0.6f + s_.syncopation},
            {R::Clave, 0.25f + s_.syncopation * 0.6f},
            {R::Offbeat, (bpm >= 66 && bpm <= 112) ? 0.55f : 0.15f},
            {R::Bossa, (bpm <= 135 && s_.energy < 0.7f && rs_.drumStyle != DrumStyle::Rock) ? 0.5f : 0.05f},
            {R::Driving, s_.energy > 0.6f ? 0.9f : 0.2f},
        };
        if (rs_.drumStyle == DrumStyle::Jazz) for (auto& o : r) if (o.first == R::Offbeat || o.first == R::Driving) o.second *= 0.2f;
        if (rs_.drumStyle == DrumStyle::FourOnFloor || rs_.drumStyle == DrumStyle::Epic)
            for (auto& o : r) if (o.first == R::Bossa || o.first == R::Offbeat) o.second *= 0.2f;
        pickN(r, 1 + int(std::lround(v * 1.5f)), rhyPal_);
    }

    using M = MelSys;
    std::vector<std::pair<M, float>> m = {
        {M::Motif, 1.4f - v * 0.5f},
        {M::PentaHook, 0.6f + s_.repetition * 0.6f},
        {M::Riff, (s_.energy > 0.55f ? 0.6f : 0.25f) + s_.repetition * 0.3f},
        {M::Arpeggio, 0.4f + s_.leaps * 0.6f},
        {M::Chorale, (s_.energy < 0.45f || orch) ? 0.6f : 0.15f},
    };
    pickN(m, 1 + int(std::lround(v * 2.f)), melPal_);
}

// Akkorde nach dem gewählten Harmonie-System (alle aus bewährten Mustern, passend zur Tonart)
bool Builder::systemHarmony(Material& m) {
    if (m.harmSys == HarmSys::Classic) return false;
    const bool orch = s_.ensemble == Ensemble::Orchestra;
    const float sev = orch ? 0.f : seventhChance(s_);
    const bool natMinor = harm_.type() == ScaleType::NaturalMinor;
    auto D = [&](int deg, float sevChance) { return diatonicChord(harm_, deg, chance(r_, sevChance)); };
    auto C = [&](int semis, std::vector<int> iv, const char* roman) { return makeChord(keyRoot_ + semis, iv, roman); };
    const std::vector<int> maj = {0, 4, 7}, min = {0, 3, 7};
    std::vector<Chord> ch;
    std::vector<int> dg;
    auto fromDegrees = [&](std::vector<int> d, float s7) {
        dg = d;
        for (int x : d) ch.push_back(D(x, s7));
    };
    switch (m.harmSys) {
    case HarmSys::Loop: {
        const auto& loops = minorKey_ ? kMinorLoops : kMajorLoops;
        dg = loops[size_t(irand(r_, 0, int(loops.size()) - 1))];
        ch = colorize(dg);
        break;
    }
    case HarmSys::Fifths: {   // Quintfall: jeder Akkord fällt eine Quinte zum nächsten (Jazz, Barock, Pop-Balladen)
        static const std::vector<std::vector<int>> ma = {{5, 1, 4, 0}, {2, 5, 1, 4}, {0, 5, 1, 4}, {0, 3, 6, 2}};
        static const std::vector<std::vector<int>> mi = {{0, 3, 6, 2}, {5, 1, 4, 0}, {3, 6, 2, 5}};
        const auto& o = minorKey_ ? mi : ma;
        fromDegrees(o[size_t(irand(r_, 0, int(o.size()) - 1))], std::max(sev, orch ? 0.f : 0.5f));
        if (minorKey_)
            for (size_t i = 0; i < dg.size(); ++i)
                if (dg[i] == 4) ch[i] = C(7, orch ? maj : std::vector<int>{0, 4, 7, 10}, orch ? "V" : "V7");
        break;
    }
    case HarmSys::Vamp: {     // zwei Akkorde im Wechsel – typisch für Kirchentonarten (Dorisch i-IV, Mixolydisch I-bVII …)
        int x = 3;
        switch (rs_.scale) {
        case ScaleType::Dorian: x = 3; break;
        case ScaleType::Mixolydian: x = 6; break;
        case ScaleType::Phrygian: case ScaleType::Lydian: x = 1; break;
        case ScaleType::NaturalMinor: case ScaleType::MinorPentatonic: x = chance(r_, 0.5f) ? 5 : 3; break;
        default: x = chance(r_, 0.6f) ? 3 : 5; break;
        }
        fromDegrees({0, x, 0, x}, sev);
        break;
    }
    case HarmSys::Pedal: {    // Orgelpunkt: Akkorde wandern, der Bass bleibt auf dem Grundton
        static const std::vector<std::vector<int>> ma = {{0, 3, 1, 0}, {0, 5, 3, 0}, {0, 1, 3, 0}};
        static const std::vector<std::vector<int>> mi = {{0, 3, 5, 6}, {0, 5, 3, 0}, {0, 6, 5, 6}};
        const auto& o = minorKey_ ? mi : ma;
        fromDegrees(o[size_t(irand(r_, 0, int(o.size()) - 1))], 0.f);
        m.pedal = true;
        break;
    }
    case HarmSys::Andalusian: {   // i - VII - VI - V (Flamenco, Film, "Hit the road")
        if (!minorKey_) return false;
        dg = {0, 6, 5, 4};
        ch = {D(0, 0.f), C(10, maj, "VII"), C(8, maj, "VI"), C(7, maj, "V")};
        break;
    }
    case HarmSys::LineCliche: {   // gleicher Grundton, eine Stimme wandert chromatisch abwärts (Bond-Thema, Balladen)
        const std::vector<int> b = minorKey_ ? min : maj;
        const char* r0 = minorKey_ ? "i" : "I";
        dg = {0, 0, 0, 0};
        ch = {C(0, b, r0), C(0, {b[0], b[1], b[2], 11}, minorKey_ ? "i(maj7)" : "Imaj7"),
              C(0, {b[0], b[1], b[2], 10}, minorKey_ ? "i7" : "I7"), C(0, {b[0], b[1], b[2], 9}, minorKey_ ? "i6" : "I6")};
        break;
    }
    case HarmSys::Mediant: {      // chromatische Medianten: der typische Kino-"Wow"-Wechsel
        if (minorKey_) {
            if (chance(r_, 0.5f)) { dg = {0, 5, 2, 4}; ch = {D(0, 0.f), C(8, maj, "bVI"), C(3, maj, "bIII"), C(7, maj, "V")}; }
            else { dg = {0, 1, 5, 4}; ch = {D(0, 0.f), C(1, maj, "bII"), C(8, maj, "bVI"), C(7, maj, "V")}; }
        } else {
            const int o = irand(r_, 0, 2);
            if (o == 0) { dg = {0, 5, 3, 0}; ch = {D(0, 0.f), C(8, maj, "bVI"), D(3, 0.f), D(0, 0.f)}; }
            else if (o == 1) { dg = {0, 2, 3, 3}; ch = {D(0, 0.f), C(4, maj, "III"), D(3, 0.f), C(5, min, "iv")}; }
            else { dg = {0, 2, 3, 0}; ch = {D(0, 0.f), C(3, maj, "bIII"), D(3, 0.f), D(0, 0.f)}; }
        }
        break;
    }
    case HarmSys::Plagal: {       // Gospel/Hymne: IV - I statt V - I, dazu die geliehene Moll-Subdominante
        if (minorKey_) return false;
        if (chance(r_, 0.5f)) { dg = {0, 3, 3, 0}; ch = {D(0, sev), D(3, sev), C(5, min, "iv"), D(0, 0.f)}; }
        else { dg = {0, 5, 3, 3}; ch = {D(0, sev), D(5, sev), D(3, sev), C(5, min, "iv")}; }
        break;
    }
    case HarmSys::Canon: {        // Pachelbel-Folge (8 Akkorde)
        fromDegrees({0, 4, 5, 2, 3, 0, 3, 4}, sev * 0.5f);
        if (minorKey_ || natMinor) ch.back() = C(7, maj, "V");
        break;
    }
    default: return false;
    }
    if (ch.empty()) return false;
    m.degrees = dg;
    m.chords = ch;
    return true;
}

std::vector<std::string> Builder::systemsInfo() const {
    std::vector<std::string> out;
    if (!s_.mixSystems) return out;
    for (Sec t : matOrder_) {
        auto it = mats_.find(t);
        if (it == mats_.end()) continue;
        const Material& m = it->second;
        std::string line = std::string(secName(t)) + ": " + harmSysName(m.harmSys);
        if (s_.ensemble == Ensemble::Band && bpb_ == 4) line += " · " + std::string(rhySysName(m.rhySys));
        if (m.melody && m.ownMotifs) line += " · " + std::string(melSysName(m.melSys));
        out.push_back(line);
    }
    return out;
}

void Builder::makeHarmony(Material& m) {
    // Eigene Akkordfolge des Benutzers (wie ein Akkordblatt)
    {
        const std::string* user = m.type == Sec::Verse ? &s_.chordsVerse : m.type == Sec::Chorus ? &s_.chordsChorus
                                : m.type == Sec::Pre ? &s_.chordsPre : m.type == Sec::Bridge ? &s_.chordsBridge : nullptr;
        if (user && !user->empty()) {
            std::istringstream in(*user);
            std::string tok;
            std::vector<Chord> chords;
            std::vector<int> degs;
            while (in >> tok) {
                if (tok == "|" || tok == "-") continue;
                int root = 0;
                std::vector<int> iv;
                if (!parseChordSymbol(tok, root, iv)) continue;
                Chord c = makeChord(root, iv);
                // Stufe für die Statistik/Vermeidung (nächster Skalenton des Grundtons)
                int d = 0;
                for (int j = 0; j < harm_.size(); ++j) if (mod12(harm_.pitch(j)) == root) d = j;
                const char* romans[] = {"I", "II", "III", "IV", "V", "VI", "VII"};
                c.roman = romans[std::clamp(d, 0, 6)];
                if (iv.size() > 1 && iv[1] == 3) {   // Moll/vermindert: kleine römische Ziffer
                    for (char& ch : c.roman) ch = char(std::tolower(static_cast<unsigned char>(ch)));
                    if (iv.size() > 2 && iv[2] == 6) c.roman += "°";
                }
                chords.push_back(c);
                degs.push_back(d);
            }
            if (!chords.empty()) {
                m.chords = chords;
                m.degrees = degs;
                // Ein Akkord pro Takt (oder wie unter "Akkordwechsel" eingestellt)
                m.barsPerChord = s_.harmonicRhythm == 1 ? 2.0 : (s_.harmonicRhythm == 3 && bpb_ != 3) ? 0.5 : 1.0;
                return;
            }
        }
    }
    const bool loopsOk = harm_.type() == ScaleType::Major || harm_.type() == ScaleType::NaturalMinor;
    auto copyFrom = [&](Sec src) {
        auto it = mats_.find(src);
        if (it == mats_.end() || it->second.degrees.empty()) return false;
        m.degrees = it->second.degrees;
        m.chords = it->second.chords;
        return true;
    };
    if (m.type == Sec::Intro) {
        if ((chance(r_, 0.6f) && copyFrom(Sec::Chorus)) || copyFrom(Sec::Verse) || copyFrom(Sec::Chorus)) return;
    }
    if (m.type == Sec::Outro) {
        if (copyFrom(Sec::Chorus) || copyFrom(Sec::Verse)) return;
    }
    if (systemHarmony(m)) return;   // Kompositions-System dieses Abschnitts (Vamp, Quintfall, Andalusisch …)

    std::vector<int> starts;
    End end = End::Loop;
    std::vector<std::vector<int>> avoid;
    auto avoidOf = [&](Sec t) { if (mats_.count(t)) avoid.push_back(mats_[t].degrees); };
    switch (m.type) {
    case Sec::Chorus:
        starts = minorKey_ ? std::vector<int>{5, 0, 3, 2} : std::vector<int>{3, 0, 5, 0};
        avoidOf(Sec::Verse);
        break;
    case Sec::Pre:
        starts = minorKey_ ? std::vector<int>{3, 5, 1} : std::vector<int>{1, 3, 5};
        end = End::Half;
        avoidOf(Sec::Verse); avoidOf(Sec::Chorus);
        break;
    case Sec::Bridge:
        starts = minorKey_ ? std::vector<int>{5, 2, 3, 6} : std::vector<int>{5, 3, 1, 2};
        end = End::Half;
        avoidOf(Sec::Verse); avoidOf(Sec::Chorus);
        break;
    default:   // Strophe (und Intro/Outro ohne Vorlage)
        starts = {0, 0, 5, 3};
        break;
    }

    // Episch: typische Kino-Folgen (i-VI-III-VII, VI-VII-i …) – in Moll und Dur
    if (epic_ && (m.type == Sec::Verse || m.type == Sec::Chorus || m.type == Sec::Bridge) && chance(r_, 0.75f)) {
        static const std::vector<std::vector<int>> epicMinor = {{0, 5, 2, 6}, {0, 5, 6, 0}, {5, 6, 0, 0}, {0, 3, 5, 4},
                                                                {0, 5, 3, 6}, {0, 6, 5, 6}, {5, 3, 0, 4}, {0, 2, 6, 5}};
        static const std::vector<std::vector<int>> epicMajor = {{0, 4, 5, 3}, {3, 0, 4, 5}, {5, 3, 0, 4}, {0, 5, 3, 4}, {3, 4, 5, 5}};
        const auto& loops = minorKey_ ? epicMinor : epicMajor;
        for (int t = 0; t < 12; ++t) {
            auto cand = loops[size_t(irand(r_, 0, int(loops.size()) - 1))];
            if (std::find(avoid.begin(), avoid.end(), cand) == avoid.end()) { m.degrees = cand; break; }
        }
    }
    if (m.degrees.empty() && (m.type == Sec::Verse || m.type == Sec::Chorus) && loopsOk && chance(r_, (1.f - s_.creativity) * 0.3f)) {
        const auto& loops = minorKey_ ? kMinorLoops : kMajorLoops;
        for (int t = 0; t < 10; ++t) {
            auto cand = loops[irand(r_, 0, int(loops.size()) - 1)];
            if (std::find(avoid.begin(), avoid.end(), cand) == avoid.end()) { m.degrees = cand; break; }
        }
    }
    if (m.degrees.empty()) m.degrees = genProgression(4, starts, end, avoid);
    m.chords = colorize(m.degrees);
}

void Builder::makeMaterials() {
    std::set<Sec> present;
    for (auto& sd : rs_.structure) present.insert(sd.type);
    const Sec order[] = {Sec::Verse, Sec::Chorus, Sec::Pre, Sec::Bridge, Sec::Intro, Sec::Outro};
    choosePalette();
    const float variety = std::clamp(s_.systemVariety, 0.f, 1.f);
    // Welches System aus der Palette passt zu welchem Abschnitt? (+ Kontrast Refrain gegen Strophe)
    auto pickHarm = [&](Sec t) {
        if (harmPal_.empty() || t == Sec::Intro || t == Sec::Outro) return HarmSys::Classic;
        if (!chance(r_, 0.45f + 0.5f * variety)) return HarmSys::Classic;
        std::vector<std::pair<HarmSys, float>> o;
        for (HarmSys h : harmPal_) {
            float w = 1.f;
            switch (h) {
            case HarmSys::Pedal: w = (t == Sec::Pre || t == Sec::Bridge) ? 2.f : t == Sec::Chorus ? 0.4f : 1.f; break;
            case HarmSys::Vamp: case HarmSys::LineCliche: w = t == Sec::Verse ? 1.6f : t == Sec::Chorus ? 0.6f : 1.f; break;
            case HarmSys::Fifths: w = (t == Sec::Pre || t == Sec::Bridge) ? 1.6f : 1.f; break;
            case HarmSys::Canon: case HarmSys::Plagal: case HarmSys::Loop: w = t == Sec::Chorus ? 1.6f : 0.8f; break;
            case HarmSys::Mediant: w = (t == Sec::Bridge || t == Sec::Chorus) ? 1.5f : 0.7f; break;
            default: break;
            }
            if (t == Sec::Chorus && mats_.count(Sec::Verse) && mats_[Sec::Verse].harmSys == h) w *= 0.35f;
            o.push_back({h, w});
        }
        return pickWeighted(r_, o);
    };
    auto pickRhy = [&](Sec t) {
        if (rhyPal_.empty() || t == Sec::Intro || t == Sec::Outro) return RhySys::Straight;
        if (t == Sec::Bridge && chance(r_, 0.5f)) return RhySys::Straight;
        std::vector<std::pair<RhySys, float>> o;
        for (RhySys r : rhyPal_) {
            float w = 1.f;
            if (t == Sec::Chorus && mats_.count(Sec::Verse) && mats_[Sec::Verse].rhySys == r && rhyPal_.size() > 1) w *= 0.5f;
            if (r == RhySys::Driving) w *= t == Sec::Chorus || t == Sec::Pre ? 1.5f : 0.7f;
            o.push_back({r, w});
        }
        return pickWeighted(r_, o);
    };
    auto pickMel = [&](Sec t) {
        if (melPal_.empty() || t == Sec::Intro || t == Sec::Outro) return MelSys::Motif;
        std::vector<std::pair<MelSys, float>> o;
        for (MelSys ms : melPal_) {
            float w = 1.f;
            if (ms == MelSys::PentaHook || ms == MelSys::Riff) w *= t == Sec::Chorus ? 1.7f : 0.8f;
            if (ms == MelSys::Chorale) w *= (t == Sec::Bridge || t == Sec::Verse) ? 1.4f : 0.6f;
            o.push_back({ms, w});
        }
        return pickWeighted(r_, o);
    };

    for (Sec t : order) {
        if (!present.count(t)) continue;
        Material& m = mats_[t];
        m.type = t;
        const float e = energyOf(t);
        m.harmSys = pickHarm(t);

        // Harmonischer Rhythmus
        if ((t == Sec::Intro || t == Sec::Outro || t == Sec::Bridge) && chance(r_, 0.35f)) m.barsPerChord = 2;
        if ((t == Sec::Chorus || t == Sec::Pre) && chance(r_, 0.1f + s_.melodyDensity * 0.2f)) m.barsPerChord = 0.5;
        // Fest eingestelltes Akkordwechsel-Tempo (Intro/Outro dürfen etwas ruhiger bleiben)
        if (s_.harmonicRhythm == 1) m.barsPerChord = 2;
        else if (s_.harmonicRhythm == 2) m.barsPerChord = (t == Sec::Intro || t == Sec::Outro) && m.barsPerChord > 1 ? 2 : 1;
        else if (s_.harmonicRhythm == 3) m.barsPerChord = (t == Sec::Intro || t == Sec::Outro) ? 1 : 0.5;
        if (rs_.bpb == 3 && m.barsPerChord < 1) m.barsPerChord = 1;   // 3/4: kein Akkordwechsel nach 1,5 Schlägen
        makeHarmony(m);

        // Begleitung
        using A = AccompStyle;
        if (s_.accompaniment != A::Auto) m.acc = s_.accompaniment;
        else if (chordSustains_) {
            // Haltende Instrumente (Streicher, Orgel, Pad) klingen als Flächen am besten
            std::vector<std::pair<A, float>> o = {{A::Pad, 0.6f}, {A::Rhythm, t == Sec::Chorus ? 0.35f : 0.15f}, {A::Arpeggio, 0.2f}};
            m.acc = pickWeighted(r_, o);
        } else {
            std::vector<std::pair<A, float>> o;
            switch (t) {
            case Sec::Intro: o = {{A::Pad, 0.5f}, {A::Arpeggio, 0.5f}}; break;
            case Sec::Verse: o = {{A::Arpeggio, 0.35f}, {A::Alberti, 0.2f}, {A::Broken, 0.25f}, {A::Pad, 0.25f * (1 - e)}}; break;
            case Sec::Pre: o = {{A::Rhythm, 0.4f}, {A::Arpeggio, 0.4f}, {A::Broken, 0.2f}}; break;
            case Sec::Chorus: o = {{A::Rhythm, 0.5f}, {A::Arpeggio, 0.3f}, {A::Broken, 0.2f}}; break;
            case Sec::Bridge: o = {{A::Pad, 0.45f}, {A::Arpeggio, 0.35f}, {A::Alberti, 0.2f}}; break;
            case Sec::Outro: o = {{A::Pad, 0.6f}, {A::Arpeggio, 0.4f}}; break;
            }
            m.acc = pickWeighted(r_, o);
            // Kontrast: Refrain anders begleiten als Strophe
            if (t == Sec::Chorus && mats_.count(Sec::Verse) && mats_[Sec::Verse].acc == m.acc && chance(r_, 0.7f))
                m.acc = pickWeighted(r_, o);
        }
        m.rhythmPattern = irand(r_, 0, 2);
        m.arp16 = e > 0.7f && chance(r_, 0.5f);

        // Bass
        using B = BassStyle;
        if (s_.bass != B::Auto) m.bass = s_.bass;
        else {
            std::vector<std::pair<B, float>> o;
            switch (t) {
            case Sec::Intro: case Sec::Outro: o = {{B::Whole, 0.8f}, {B::RootFifth, 0.2f}}; break;
            case Sec::Verse: o = {{B::RootFifth, 0.4f}, {B::Syncopated, 0.3f}, {B::Whole, 0.3f}, {B::Walking, s_.creativity * 0.5f}}; break;
            case Sec::Pre: o = {{B::Eighths, 0.5f}, {B::RootFifth, 0.5f}}; break;
            case Sec::Chorus: o = {{B::Eighths, 0.4f}, {B::Syncopated, 0.4f}, {B::RootFifth, 0.2f}}; break;
            case Sec::Bridge: o = {{B::Whole, 0.5f}, {B::RootFifth, 0.5f}, {B::Walking, s_.creativity * 0.5f}}; break;
            }
            m.bass = pickWeighted(r_, o);
        }
        m.bassOctaves = chance(r_, 0.35f);

        // Schlagzeug: Grund-Stil des Songs, pro Abschnitt abgewandelt (Kontrast!)
        using D = DrumStyle;
        m.drumStyle = rs_.drumStyle;
        m.kickVariant = irand(r_, 0, 4);
        m.ghost = s_.creativity * 0.2f + s_.syncopation * 0.15f;
        switch (t) {
        case Sec::Intro:
            m.drumsOn = chance(r_, 0.5f);
            m.drumsLight = true;
            break;
        case Sec::Verse: break;
        case Sec::Pre: m.openHats = chance(r_, 0.5f); break;
        case Sec::Chorus:
            m.openHats = true;
            m.ride = rs_.drumStyle == D::Rock && chance(r_, 0.5f);
            break;
        case Sec::Bridge:
            if (chance(r_, 0.25f)) m.drumsOn = false;   // "Breakdown"
            else if (rs_.drumStyle != D::Jazz && chance(r_, 0.6f)) m.drumStyle = chance(r_, 0.5f) ? D::HalfTime : D::Ballad;
            break;
        case Sec::Outro:
            m.drumsOn = chance(r_, 0.7f);
            if (rs_.drumStyle != D::Jazz) m.drumStyle = D::HalfTime;
            break;
        }

        // Orchester: Begleitung als gehaltene Streicher / Harfen-Arpeggien, ruhigere Bässe
        if (s_.ensemble == Ensemble::Orchestra) planOrchestra(m, t, e);

        // Melodie
        m.melody = t == Sec::Intro ? chance(r_, 0.25f) : t == Sec::Outro ? chance(r_, 0.55f) : true;
        float dOff = t == Sec::Chorus ? 0.05f : t == Sec::Verse ? 0.f : t == Sec::Bridge ? -0.1f : t == Sec::Pre ? 0.05f : -0.2f;
        m.density = std::clamp(s_.melodyDensity + dOff, 0.05f, 1.f);
        const int chorusLift = mel_.size() >= 7 ? 3 : 2;
        m.centerOffset = t == Sec::Verse ? 0 : t == Sec::Pre ? 1 : t == Sec::Bridge ? irand(r_, -1, 2) : chorusLift;
        m.contour = t == Sec::Pre ? 1 : (t == Sec::Chorus && chance(r_, 0.6f)) ? 0 : -1;

        const float cr = s_.creativity;
        std::vector<std::pair<std::string, float>> forms;
        switch (t) {
        case Sec::Verse: forms = {{"ABAC", 1.f}, {"AABC", 1.f}, {"AABA", 0.7f - cr * 0.4f}, {"ABAB", 0.5f}}; break;
        case Sec::Chorus: forms = {{"ABAB", 1.f}, {"AABA", 0.8f}, {"AAAB", 0.3f}, {"ABAC", 0.4f + cr * 0.5f}}; break;
        case Sec::Pre: forms = {{"AB", 1.f}, {"AA", 0.6f}}; break;
        case Sec::Bridge: forms = {{"ABAC", 0.5f}, {"ABCD", 0.3f + cr * 0.5f}, {"AB", 0.5f}}; break;
        default: forms = {{"AB", 1.f}}; break;
        }
        // Wiedererkennung: wiederholungsreiche Formen (AABA, ABAB …) bevorzugen oder meiden
        for (auto& fw : forms) {
            const size_t distinct = std::set<char>(fw.first.begin(), fw.first.end()).size();
            fw.second *= distinct <= 2 ? 0.4f + 1.2f * s_.repetition : 1.6f - 1.2f * s_.repetition;
            fw.second = std::max(fw.second, 0.02f);
        }
        m.form = pickWeighted(r_, forms);

        // ---- Kompositions-Systeme: Rhythmus und Melodie, Anpassungen ----
        m.rhySys = pickRhy(t);
        m.melSys = pickMel(t);
        const bool userAcc = s_.accompaniment != AccompStyle::Auto, userBass = s_.bass != BassStyle::Auto;
        if (m.pedal) {   // Orgelpunkt: gehaltener Grundton im Bass, ruhige Akkordwechsel
            if (!userBass) m.bass = BassStyle::Whole;
            m.barsPerChord = std::max(1.0, m.barsPerChord);
        }
        switch (m.rhySys) {
        case RhySys::Straight: break;
        case RhySys::Tresillo:
            if (!userAcc && m.acc != AccompStyle::Arpeggio) m.acc = AccompStyle::Rhythm;
            if (!userBass && !m.pedal) m.bass = BassStyle::Syncopated;
            break;
        case RhySys::Driving:
            if (!userAcc) m.acc = AccompStyle::Rhythm;
            if (!userBass && !m.pedal) m.bass = BassStyle::Eighths;
            break;
        default:   // Clave, Offbeat, Bossa: eigene Begleit- und Bassmuster
            if (!userAcc) m.acc = AccompStyle::Rhythm;
            break;
        }
        switch (m.melSys) {
        case MelSys::Riff:      // kurzes Motiv, das fast unverändert wiederkehrt
            m.form = chance(r_, 0.6f) ? "AAAB" : "AABA";
            if (t == Sec::Pre) m.form = "AA";
            m.variationMul = 0.35f;
            m.density = std::clamp(m.density + 0.08f, 0.05f, 1.f);
            break;
        case MelSys::PentaHook: // nur Pentatonik-Töne: klingt immer, eingängig
            if (t == Sec::Chorus) m.form = chance(r_, 0.5f) ? "ABAB" : "AABA";
            break;
        case MelSys::Chorale:   // ruhige, lange Töne in Schritten (Hymne)
            m.density = std::min(m.density, 0.25f);
            m.contour = 0;
            break;
        default: break;
        }

        // Intro/Outro zitieren den Refrain
        if ((t == Sec::Intro || t == Sec::Outro) && present.count(Sec::Chorus)) {
            m.ownMotifs = false;
            m.motifOwner = Sec::Chorus;
        }
        if (rs_.hasIdea && t == rs_.ideaType) applyIdea(m);   // eigene Idee übernimmt diesen Abschnitt
        matOrder_.push_back(t);
    }
}

std::vector<TChord> Builder::tile(const Material& m, int bars) const {
    std::vector<TChord> tc;
    const Tick slot = std::max<Tick>(beat_, Tick(double(bar_) * m.barsPerChord));
    const Tick total = Tick(bars) * bar_;
    size_t i = 0;
    for (Tick t = 0; t < total; t += slot, ++i)
        tc.push_back({m.chords[i % m.chords.size()], t, std::min(slot, total - t)});
    return tc;
}

// ===================== Eigene Idee =====================
void Builder::applyIdea(Material& m) {
    const MusicalIdea& id = *s_.idea;
    m.hasIdea = true;
    m.pedal = false;                 // eigene Idee: keine System-Harmonie
    m.harmSys = HarmSys::Classic;
    m.ideaHarmonic = id.harmonic;
    m.ideaNotes = id.notes;
    m.ideaBars = rs_.ideaBars;
    m.barsPerChord = 1;
    m.degrees.clear();
    m.chords = id.harmonic ? recognizeChords(id.notes, rs_.ideaBars) : harmonizeIdea(id.notes, rs_.ideaBars);
    if (id.harmonic) return;
    // Die Idee in 2-Takt-Motive zerlegen: so kann der Generator sie später variieren
    m.motifs.clear();
    m.form.clear();
    m.ownMotifs = true;
    m.melody = true;
    const Tick unit = 2 * bar_;
    const int U = (rs_.ideaBars + 1) / 2;
    for (int u = 0; u < U; ++u) {
        const char L = char('A' + u);
        Motif mo;
        for (auto& n : id.notes)
            if (n.start >= u * unit && n.start < (u + 1) * unit)
                mo.push_back({n.start - u * unit, n.length, mel_.nearestDegree(n.pitch)});
        m.motifs[L] = mo;
        m.form += L;
    }
}

// Harmonisierung: Viterbi über die leitereigenen Akkorde. Bewertet pro Takt, wie gut ein Akkord
// zu den Melodietönen passt (nach Dauer und Betonung gewichtet), plus wie gut die Folge "läuft".
std::vector<Chord> Builder::harmonizeIdea(const std::vector<Note>& notes, int bars) {
    const Table T = table();
    std::vector<Chord> cands;
    std::vector<int> candDeg;
    for (int d = 0; d < 7; ++d) { cands.push_back(diatonicChord(harm_, d, false)); candDeg.push_back(d); }
    if (minorKey_) { cands.push_back(makeChord(keyRoot_ + 7, {0, 4, 7}, "V")); candDeg.push_back(4); }
    const int C = int(cands.size());
    static const float strengthW[4] = {0.6f, 1.f, 1.3f, 1.7f};

    std::vector<std::vector<float>> em(size_t(bars), std::vector<float>(size_t(C), 0.f));
    for (int b = 0; b < bars; ++b) {
        const Tick b0 = Tick(b) * bar_, b1 = b0 + bar_;
        for (auto& n : notes) {
            const Tick ov = std::min(n.end(), b1) - std::max(n.start, b0);
            if (ov <= 0) continue;
            const float w = float(ov) / float(beat_) * (n.start >= b0 ? strengthW[strengthAt(n.start - b0)] : 1.f);
            for (int c = 0; c < C; ++c) {
                if (cands[size_t(c)].hasPc(n.pitch)) em[size_t(b)][size_t(c)] += w;
                else em[size_t(b)][size_t(c)] -= (mel_.contains(n.pitch) ? 0.55f : 0.25f) * w;
            }
        }
        for (int c = 0; c < C; ++c)
            if (cands[size_t(c)].intervals.size() >= 3 && cands[size_t(c)].intervals[2] == 6) em[size_t(b)][size_t(c)] -= 1.f;
    }
    em[0][0] += 1.f;                                  // Anfang auf der Tonika
    em[size_t(bars - 1)][0] += 0.5f;
    em[size_t(bars - 1)][4] += 0.4f;

    auto trans = [&](int a, int b) {
        const int da = candDeg[size_t(a)], db = candDeg[size_t(b)];
        return a == b ? std::log(0.12f) : std::log(T[size_t(da)][size_t(db)] + 0.03f) * 0.9f;
    };
    std::vector<std::vector<float>> cost(size_t(bars), std::vector<float>(size_t(C), -1e9f));
    std::vector<std::vector<int>> from(size_t(bars), std::vector<int>(size_t(C), 0));
    for (int c = 0; c < C; ++c) cost[0][size_t(c)] = em[0][size_t(c)];
    for (int b = 1; b < bars; ++b)
        for (int c = 0; c < C; ++c) {
            for (int p = 0; p < C; ++p) {
                const float v = cost[size_t(b - 1)][size_t(p)] + trans(p, c);
                if (v > cost[size_t(b)][size_t(c)]) { cost[size_t(b)][size_t(c)] = v; from[size_t(b)][size_t(c)] = p; }
            }
            cost[size_t(b)][size_t(c)] += em[size_t(b)][size_t(c)];
        }
    int c = int(std::max_element(cost.back().begin(), cost.back().end()) - cost.back().begin());
    std::vector<Chord> out(static_cast<size_t>(bars));
    for (int b = bars - 1; b >= 0; --b) {
        out[size_t(b)] = cands[size_t(c)];
        c = from[size_t(b)][size_t(c)];
    }
    return out;
}

// Gespielte Akkorde erkennen: pro Takt die Akkord-Schablone mit der besten Übereinstimmung
std::vector<Chord> Builder::recognizeChords(const std::vector<Note>& notes, int bars) {
    static const std::vector<std::vector<int>> templates = {
        {0, 4, 7}, {0, 3, 7}, {0, 3, 6}, {0, 4, 7, 10}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 5, 7},
    };
    std::vector<Chord> out;
    Chord prev = diatonicChord(harm_, 0, false);
    for (int b = 0; b < bars; ++b) {
        const Tick b0 = Tick(b) * bar_, b1 = b0 + bar_;
        std::array<float, 12> hist{};
        float total = 0;
        int lowest = 999;
        for (auto& n : notes) {
            const Tick ov = std::min(n.end(), b1) - std::max(n.start, b0);
            if (ov <= 0) continue;
            hist[size_t(mod12(n.pitch))] += float(ov);
            total += float(ov);
            lowest = std::min(lowest, n.pitch);
        }
        if (total <= 0) { out.push_back(prev); continue; }
        float best = -1e9f;
        Chord bestC = prev;
        for (int root = 0; root < 12; ++root)
            for (auto& tp : templates) {
                Chord c = makeChord(root, tp);
                float sc = 0;
                for (int pc = 0; pc < 12; ++pc) sc += c.hasPc(pc) ? hist[size_t(pc)] : -0.6f * hist[size_t(pc)];
                if (lowest < 999 && mod12(lowest) == root) sc += 0.35f * total;   // Basston = Grundton
                if (tp.size() == 4) sc -= 0.12f * total;                         // einfache Akkorde bevorzugen
                bool inKey = true;
                for (int pc : c.pcs()) inKey = inKey && harm_.contains(pc);
                if (inKey) sc += 0.15f * total;
                if (sc > best) { best = sc; bestC = c; }
            }
        out.push_back(bestC);
        prev = bestC;
    }
    return out;
}

// ===================== Orchester =====================
// Pro Abschnitt Techniken würfeln – gewichtet nach Stil-Profil und Energie des Abschnitts.
void Builder::planOrchestra(Material& m, Sec t, float e) {
    const OrchProfile& P = orchProfiles()[size_t(rs_.orchStyle)];
    OrchPlan& o = m.orch;
    auto pick = [&](const float* w, int n, auto mod) {
        std::vector<float> ws(static_cast<size_t>(n));
        float sum = 0;
        for (int i = 0; i < n; ++i) { ws[size_t(i)] = std::max(0.f, w[i] * mod(i)); sum += ws[size_t(i)]; }
        return sum > 0 ? pickWeighted(r_, ws) : 0;
    };
    const bool loud = t == Sec::Chorus || (t == Sec::Pre && e > 0.6f);
    const bool quiet = t == Sec::Intro || t == Sec::Outro || (t == Sec::Bridge && e < 0.55f);

    // Begleit-Textur: kräftige Texturen in lauten, leichte in ruhigen Teilen – und Kontrast zur Strophe
    const Tex verseTex = (t != Sec::Verse && mats_.count(Sec::Verse)) ? mats_[Sec::Verse].orch.tex : Tex::Count;
    o.tex = Tex(pick(P.tex, int(Tex::Count), [&](int i) {
        const Tex x = Tex(i);
        const bool heavy = x == Tex::Ostinato16 || x == Tex::Stabs || x == Tex::March || x == Tex::Ostinato8;
        const bool light = x == Tex::Pad || x == Tex::Arpeggio || x == Tex::Tremolo || x == Tex::Pizz;
        float w = 1.f;
        if (heavy) w *= quiet ? 0.15f : 0.3f + e * 1.4f;
        if (light) w *= loud ? 0.6f : 1.4f - e * 0.6f;
        if (x == verseTex && (t == Sec::Chorus || t == Sec::Bridge)) w *= 0.15f;
        if (!P.brass && x == Tex::Stabs) w *= 0.5f;
        return w;
    }));
    if (rs_.bpb == 3 && o.tex == Tex::March) o.tex = Tex::Waltz;

    o.mel = MelT(pick(P.mel, int(MelT::Count), [&](int i) {
        const MelT x = MelT(i);
        if (loud) return (x == MelT::Octave || x == MelT::Unison) ? 1.6f : x == MelT::Solo ? 0.5f : 1.f;
        if (quiet) return x == MelT::Unison ? 0.1f : x == MelT::Solo ? 1.5f : 1.f;
        return x == MelT::Unison ? 0.4f : 1.f;
    }));
    o.bass = BassT(pick(P.bass, int(BassT::Count), [&](int i) {
        const BassT x = BassT(i);
        if (x == BassT::LowOstinato) return quiet ? 0.1f : 0.4f + e;
        if (x == BassT::Pedal) return (t == Sec::Intro || t == Sec::Bridge || t == Sec::Outro) ? 2.f : 0.6f;
        return 1.f;
    }));
    o.perc = PercT(pick(P.perc, int(PercT::Count), [&](int i) {
        if (PercT(i) == PercT::None) return quiet ? 3.f : loud ? 0.4f : 1.2f;
        return quiet ? 0.2f : 0.4f + e;
    }));
    if (!P.brass && o.perc == PercT::SnareMarch) o.perc = PercT::TimpHits;

    // Melodie-Instrument; die Bridge bekommt bewusst eine andere Farbe als Strophe und Refrain
    std::vector<int> leads = P.leads;
    if (!P.brass) leads.erase(std::remove_if(leads.begin(), leads.end(), [](int x) { return x == HN || x == TP; }), leads.end());
    if (leads.empty()) leads = {V1};
    o.lead = leads[size_t(irand(r_, 0, int(leads.size()) - 1))];
    if (t == Sec::Chorus && chance(r_, 0.5f)) o.lead = V1;
    if (t == Sec::Bridge || t == Sec::Pre) {
        static const std::vector<int> colors = {VC, OB, CL, HN, FL, VA};
        for (int tries = 0; tries < 8; ++tries) {
            const int cand = colors[size_t(irand(r_, 0, int(colors.size()) - 1))];
            if (!P.brass && cand == HN) continue;
            bool used = false;
            for (Sec s : {Sec::Verse, Sec::Chorus}) used = used || (mats_.count(s) && mats_[s].orch.lead == cand);
            if (!used) { o.lead = cand; break; }
        }
    }
    std::vector<int> partners = {FL, OB, CL, V2, VA, VC, HN};
    if (!P.brass) partners.pop_back();
    for (int tries = 0; tries < 10; ++tries) {
        o.second = partners[size_t(irand(r_, 0, int(partners.size()) - 1))];
        if (o.second != o.lead) break;
    }
    o.counter = t != Sec::Intro && chance(r_, P.counter * (0.4f + e));
    std::vector<int> counters = {VC, VA, CL, FG, HN};
    if (!P.brass) counters.pop_back();
    for (int tries = 0; tries < 10; ++tries) {
        o.counterInst = counters[size_t(irand(r_, 0, int(counters.size()) - 1))];
        if (o.counterInst != o.lead && o.counterInst != o.second) break;
    }
    o.sparkle = t == Sec::Chorus && chance(r_, P.sparkle);
    o.gliss = chance(r_, P.sparkle);
    o.crescendo = t == Sec::Pre || chance(r_, 0.25f);
    if (P.brass) {
        // Blech bekommt eigene Rollen: Hörner als warme Gegenstimme, Choral in Bridge/Schluss, Fanfaren im Refrain
        if (o.counter && o.lead != HN && o.second != HN && chance(r_, 0.45f)) o.counterInst = HN;
        o.brassChorale = (t == Sec::Bridge && chance(r_, 0.4f)) || (t == Sec::Outro && chance(r_, 0.35f)) ||
                         (t == Sec::Chorus && e >= 0.72f && chance(r_, 0.3f));
        o.fanfare = (t == Sec::Chorus || t == Sec::Intro) && e >= 0.7f && chance(r_, 0.35f);
    }
    {
        // Spannungs-Techniken: je dramatischer der Stil, desto öfter
        const std::string nm = P.name;
        const bool dramatic = nm == "Episch (Film)" || nm == "Marsch / Heroisch" || nm == "Mysteriös" || epic_;
        const bool builds = t == Sec::Pre || t == Sec::Bridge || t == Sec::Intro || t == Sec::Verse;
        o.timpRoll = builds && chance(r_, dramatic ? 0.65f : nm == "Romantisch" ? 0.4f : 0.22f);
        o.tremoloBuild = builds && nm != "Barock / Kammer" && chance(r_, dramatic ? 0.5f : 0.3f);
        o.brassSwell = P.brass && (t == Sec::Pre || t == Sec::Bridge) && chance(r_, dramatic ? 0.5f : 0.3f);
        o.snareRoll = nm == "Marsch / Heroisch" && builds && chance(r_, 0.6f);
        o.accentHit = t == Sec::Chorus && chance(r_, dramatic ? 0.85f : nm == "Minimal" || nm == "Pastoral" ? 0.25f : 0.5f);
    }

    // Epische Mittel: Chor, Braams (tiefes Blech), Taiko – je nach Stil unterschiedlich wahrscheinlich
    const std::string sname = P.name;
    const float ep = sname == "Episch (Film)" ? 1.f : sname == "Mysteriös" ? 0.5f : sname == "Marsch / Heroisch" ? 0.4f
                   : sname == "Romantisch" ? 0.25f : sname == "Minimal" ? 0.3f : 0.f;
    const bool big = t == Sec::Chorus || (t == Sec::Bridge && e > 0.5f) || t == Sec::Outro;
    o.choir = big ? chance(r_, ep * 0.9f) : (t == Sec::Pre || t == Sec::Bridge) ? chance(r_, ep * 0.45f) : false;
    o.choirMode = (t == Sec::Chorus && chance(r_, 0.35f)) ? 1 : 0;
    o.braams = P.brass && (t == Sec::Chorus || (t == Sec::Bridge && e > 0.5f)) && chance(r_, ep * 0.8f);
    o.taiko = (t != Sec::Outro) && chance(r_, ep * (sname == "Episch (Film)" ? 0.5f + e : 0.3f + e * 0.5f));

    // Grundgerüst passend dazu: Akkorde kommen aus der Textur, Bass aus der Bass-Technik
    m.acc = AccompStyle::Pad;
    m.bass = o.bass == BassT::Walking ? BassStyle::Walking : (t == Sec::Chorus && e > 0.7f) ? BassStyle::RootFifth : BassStyle::Whole;
    if (o.tex == Tex::Waltz || o.tex == Tex::March) m.bass = BassStyle::RootFifth;
    m.orchMelody = o.lead;
}

std::vector<std::string> Builder::orchInfo() const {
    std::vector<std::string> out;
    for (Sec t : matOrder_) {
        auto it = mats_.find(t);
        if (it == mats_.end()) continue;
        const OrchPlan& o = it->second.orch;
        std::string s = std::string(secName(t)) + ": ";
        if (it->second.melody) s += std::string(melTName(o.mel)) + " (" + orchInstName(o.lead) + "), ";
        s += std::string(texName(o.tex)) + ", " + bassTName(o.bass);
        if (o.perc != PercT::None) s += std::string(", ") + percTName(o.perc);
        if (o.counter) s += std::string(", Gegenstimme (") + orchInstName(o.counterInst) + ")";
        if (o.sparkle) s += ", Glockenspiel";
        if (o.choir) s += o.choirMode == 1 ? ", Chor singt Melodie" : ", Chor-Fläche";
        if (o.braams) s += ", Braams (tiefes Blech)";
        if (o.taiko) s += ", Taiko-Ostinato";
        if (o.brassChorale) s += ", Blechbläser-Choral";
        if (o.fanfare) s += ", Trompeten-Fanfare";
        if (o.timpRoll) s += ", Paukenwirbel";
        if (o.tremoloBuild) s += ", Tremolo-Crescendo";
        if (o.brassSwell) s += ", Blech-Anschwellen";
        if (o.snareRoll) s += ", Trommelwirbel";
        if (o.accentHit) s += ", Schicksalsschlag";
        out.push_back(s);
    }
    return out;
}

void Builder::orchestrate(const Material& m, const SecDef& sd, size_t k, const std::vector<TChord>& tc, Tick secStart,
                          int tr, float vel, const std::vector<Note>& mel, const std::vector<Note>& /*acc*/,
                          const std::vector<Note>& bass) {
    // Spielbare Tonumfänge der Register (MIDI)
    static const int range[OCOUNT][2] = {{60, 96}, {58, 91}, {50, 89}, {34, 72}, {41, 77}, {55, 82}, {40, 72},
                                         {38, 55}, {0, 127}, {0, 127}, {72, 105}, {24, 100}, {48, 81}, {55, 100},
                                         {55, 93}, {48, 84}, {36, 76}, {28, 60}, {36, 88}, {26, 58}, {74, 105}, {41, 89}};
    const OrchPlan& P = m.orch;
    const OrchProfile& prof = orchProfiles()[size_t(rs_.orchStyle)];
    const std::string style = prof.name;
    std::array<size_t, OCOUNT> before{};
    for (int i = 0; i < OCOUNT; ++i) before[size_t(i)] = orch_[size_t(i)].notes.size();

    auto put = [&](int t, Note n, int shift, float vm) {
        if (t != PC && t != TK) {
            n.pitch += tr + shift;
            while (n.pitch < range[t][0]) n.pitch += 12;
            while (n.pitch > range[t][1]) n.pitch -= 12;
        }
        n.length = std::max<Tick>(six_ / 2, n.length);
        n.velocity = std::clamp(n.velocity * vm, 0.05f, 1.f);
        orch_[size_t(t)].notes.push_back(n);
    };
    auto add = [&](int t, int pitch, Tick start, Tick len, float v) { put(t, Note{pitch, start, len, v}, 0, 1.f); };
    const float e = secEnergy(k);
    const auto& st = rs_.structure;
    const bool lastSec = k + 1 == st.size();
    const bool nextChorus = k + 1 < st.size() && st[k + 1].type == Sec::Chorus;
    const Tick secLen = Tick(sd.bars) * bar_;
    const Tick eighth = beat_ / 2;
    const bool brass = prof.brass;

    // Steigerung: ein späterer Refrain bekommt mehr Schichten
    MelT melT = P.mel;
    bool counter = P.counter;
    if (sd.type == Sec::Chorus && m.occurrences > 0) {
        if (melT == MelT::Solo || melT == MelT::CallResponse) melT = MelT::Octave;
        counter = true;
    }

    // ---------- 1. Melodie ----------
    const int lead = P.lead, second = P.second;
    const int octShift = (range[second][0] + range[second][1]) > (range[lead][0] + range[lead][1]) ? 12 : -12;
    std::vector<Note> melAll(mel.begin(), mel.end());   // Melodie + ihre zweiten Stimmen (Terzen, Kanon)
    switch (melT) {
    case MelT::Octave:   // Verdopplung in der Oktave
        for (auto& n : mel) { put(lead, n, 0, 1.f); put(second, n, octShift, 0.75f); }
        break;
    case MelT::Unison:   // breites Unisono über mehrere Oktaven (filmisch)
        for (auto& n : mel) {
            put(V1, n, 0, 1.f);
            put(V2, n, 0, 0.8f);
            // Tiefe Verdopplungen nur mit Akkordtönen: Durchgangs-/Vorhaltstöne reiben tief unten am Bass
            bool chordTone = true;   // über die ganze Notenlänge (auch über Akkordwechsel hinweg) ein Akkordton?
            for (const auto& c : tc)
                if (secStart + c.start < n.end() && secStart + c.start + c.len > n.start) chordTone = chordTone && c.chord.hasPc(n.pitch);
            if (chordTone || n.length < eighth) {
                put(VA, n, -12, 0.75f);
                put(VC, n, -12, 0.75f);
                if (brass) put(HN, n, -12, 0.6f);
            }
        }
        break;
    case MelT::Thirds:   // zweite Stimme eine leitereigene Terz darunter
        for (auto& n : mel) {
            put(lead, n, 0, 1.f);
            Note h = n;
            h.pitch = realize(mel_.nearestDegree(n.pitch) - 2, chordAt(tc, n.start - secStart));
            put(second, h, 0, 0.75f);
            partnerKeys_.insert({orch_[size_t(second)].name, orch_[size_t(second)].notes.back().start, orch_[size_t(second)].notes.back().pitch});
            melAll.push_back(h);
        }
        break;
    case MelT::CallResponse:   // 2-Takt-Phrasen wechseln zwischen zwei Instrumenten
        for (auto& n : mel) put(((n.start - secStart) / (2 * bar_)) % 2 == 0 ? lead : second, n, 0, 1.f);
        break;
    case MelT::Canon: {   // zweite Stimme setzt einen Harmonie-Durchlauf später ein -> passt immer zu den Akkorden
        const Tick cycle = Tick(double(m.chords.size()) * m.barsPerChord * double(bar_));
        for (auto& n : mel) {
            put(lead, n, 0, 1.f);
            if (cycle > 0 && cycle < secLen && n.start + cycle < secStart + secLen) {
                Note c = n;
                c.start += cycle;
                put(second, c, 0, 0.7f);
                partnerKeys_.insert({orch_[size_t(second)].name, orch_[size_t(second)].notes.back().start, orch_[size_t(second)].notes.back().pitch});
                melAll.push_back(c);
            }
        }
        break;
    }
    default:
        for (auto& n : mel) put(lead, n, 0, 1.f);
        break;
    }

    // Alle eben geschriebenen Melodie-Noten (inkl. Verdopplungen) merken: sie gelten beim Bereinigen als fest
    auto markNotes = [&](std::set<std::tuple<std::string, Tick, int>>& keys, const std::array<size_t, OCOUNT>& from) {
        for (int i = 0; i < OCOUNT; ++i)
            for (size_t j = from[size_t(i)]; j < orch_[size_t(i)].notes.size(); ++j)
                keys.insert({orch_[size_t(i)].name, orch_[size_t(i)].notes[j].start, orch_[size_t(i)].notes[j].pitch});
    };
    markNotes(melodyKeys_, before);
    std::array<size_t, OCOUNT> afterMelody{};
    for (int i = 0; i < OCOUNT; ++i) afterMelody[size_t(i)] = orch_[size_t(i)].notes.size();
    (void)afterMelody;

    // ---------- Begleitung macht der Melodie Platz ----------
    // Reibt ein Begleitton im Halbtonabstand an einem längeren Melodieton (z.B. Vorhalt), weicht er auf den
    // nächsten passenden Akkordton aus – oder pausiert.
    auto rubs = [&](int p, Tick a, Tick b) {
        for (auto& n : melAll) {
            if (n.length < eighth || n.end() <= a || n.start >= b) continue;
            const int d = mod12(n.pitch - p);
            if (d == 1 || d == 11) return true;
        }
        return false;
    };
    auto makeRoom = [&](const TChord& c, std::vector<int> v) {
        const Tick a = secStart + c.start, b = a + c.len;
        for (auto& p : v) {
            if (!rubs(p, a, b)) continue;
            int repl = -1;
            for (int d = 1; d <= 7 && repl < 0; ++d)
                for (int sgn2 : {-1, 1})
                    if (c.chord.hasPc(p + sgn2 * d) && !rubs(p + sgn2 * d, a, b)) { repl = p + sgn2 * d; break; }
            p = repl;
        }
        v.erase(std::remove(v.begin(), v.end(), -1), v.end());
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        if (v.size() < 2) {   // zu wenig übrig: Grundton (und Quinte) in mittlerer Lage
            int r = 48;
            while (mod12(r) != c.chord.root) ++r;
            v = {r, r + (c.chord.intervals.size() > 2 ? c.chord.intervals[2] : 7)};
        }
        return v;
    };

    // Welche Streicherstimmen sind frei für die Begleitung? (Melodie/Partner/Gegenstimme nicht doppelt belegen)
    auto taken = [&](int x) { return x == lead || (melT != MelT::Solo && x == second) || (counter && x == P.counterInst); };
    const int upT = !taken(V2) ? V2 : !taken(VA) ? VA : CL;
    const int loT = !taken(VA) ? VA : !taken(V2) ? V2 : FG;
    // ---------- 2. Begleit-Textur ----------
    for (size_t ci = 0; ci < tc.size(); ++ci) {
        const TChord& c = tc[ci];
        const Tick t0 = secStart + c.start;
        const auto v = makeRoom(c, voice(c.chord));   // 3-4 Töne, eng um C4, ohne Reibung an der Melodie
        const int top = v.back(), low = v.front();
        auto upper = [&](int p) { return p >= 60 ? upT : loT; };
        switch (P.tex) {
        case Tex::Pad:
            for (int p : v) add(upper(p), p, t0, c.len - six_ / 2, vel * 0.6f);
            if (e >= 0.6f && brass && sd.type != Sec::Intro)
                for (size_t i = 1; i < v.size() && i < 3; ++i) add(HN, v[i], t0, c.len - six_ / 2, vel * 0.5f);
            break;
        case Tex::Ostinato8:
        case Tex::Ostinato16: {   // repetierte Akkordtöne, Akzent auf den Schlägen
            const Tick step = P.tex == Tex::Ostinato16 ? six_ : eighth;
            int i = 0;
            for (Tick t = 0; t < c.len; t += step, ++i) {
                // Episch: Akzente im 3+3+2-Raster (treibt nach vorne), sonst auf den Schlägen
                const int pos16 = int((t / six_) % 8);
                const float acc = style == "Episch (Film)" ? ((pos16 == 0 || pos16 == 3 || pos16 == 6) ? 1.f : 0.65f)
                                                          : ((t % beat_ == 0) ? 1.f : 0.8f);
                add(upT, (i % 2 == 0) ? top : v[v.size() - 2], t0 + t, step - six_ / 4, vel * 0.55f * acc);
                add(loT, low, t0 + t, step - six_ / 4, vel * 0.5f * acc);
            }
            break;
        }
        case Tex::Arpeggio: {   // gebrochene Akkorde: Harfe – im Minimal-/Barock-Stil Klarinette
            const bool minimal = style == "Minimal";
            const int inst = (minimal || style == "Barock / Kammer") ? CL : HP;
            const Tick step = minimal ? six_ : eighth;
            std::vector<int> seq = v;
            seq.push_back(v[0] + 12);
            std::vector<int> order;
            if (minimal) order = {0, 1, 2, 1};   // kreisende Figur
            else {
                for (int i = 0; i < int(seq.size()); ++i) order.push_back(i);
                for (int i = int(seq.size()) - 2; i >= 1; --i) order.push_back(i);
            }
            int i = 0;
            for (Tick t = 0; t < c.len; t += step, ++i)
                add(inst, seq[size_t(order[size_t(i) % order.size()]) % seq.size()], t0 + t, step * 2, vel * 0.6f);
            for (int p : v) add(upper(p), p, t0, c.len - six_ / 2, vel * 0.35f);   // leise Fläche darunter
            break;
        }
        case Tex::Tremolo:   // schnelle Tonwiederholung, schwellend
            for (Tick t = 0; t < c.len; t += six_) {
                const float sw = 0.35f + 0.3f * float(t) / float(c.len);
                for (int p : v) add(upper(p), p, t0 + t, six_ - six_ / 8, vel * sw);
            }
            break;
        case Tex::Pizz:   // gezupfte Akkorde auf den Nebenzählzeiten
            for (Tick bo = 0; bo < c.len; bo += bar_)
                for (int b = 1; b < bpb_; ++b)
                    if (bo + b * beat_ < c.len)
                        for (size_t i = 1; i < v.size(); ++i) add(PZ, v[i], t0 + bo + b * beat_, eighth, vel * 0.7f);
            break;
        case Tex::Stabs: {   // kurze, synkopierte Akkordschläge
            const std::vector<int> pat = bpb_ == 3 ? std::vector<int>{0, 2, 4} : std::vector<int>{0, 3, 6};
            for (Tick bo = 0; bo < c.len; bo += bar_)
                for (int x : pat) {
                    const Tick t = bo + x * eighth;
                    if (t >= c.len) continue;
                    for (int p : v) {
                        if (brass) add(p >= 60 ? TP : HN, p, t0 + t, eighth, vel * 0.7f);
                        add(upper(p), p, t0 + t, eighth, vel * 0.6f);
                    }
                }
            for (int p : v) add(upper(p), p, t0, c.len - six_ / 2, vel * 0.3f);
            break;
        }
        case Tex::Chorale: {   // die Melodie wird mehrstimmig mitgesetzt (homophon)
            bool any = false;
            for (auto& n : mel) {
                if (n.start < t0 || n.start >= t0 + c.len || n.length < eighth) continue;
                any = true;
                int found = 0;
                for (int p = n.pitch - 3; p > n.pitch - 15 && found < 2; --p)
                    if (c.chord.hasPc(p)) { add(found == 0 ? upT : loT, p, n.start, n.length, vel * 0.55f); ++found; p -= 2; }
            }
            if (!any) for (int p : v) add(upper(p), p, t0, c.len - six_ / 2, vel * 0.5f);
            break;
        }
        case Tex::Waltz:   // "Umm-pa-pa": Akkord auf den Nebenschlägen
        case Tex::March:   // "Umm-pa": Akkord auf 2 und 4, im Marsch mit Hörnern
            for (Tick bo = 0; bo < c.len; bo += bar_)
                for (int b = 1; b < bpb_; ++b) {
                    if (bpb_ == 4 && b % 2 == 0) continue;
                    const Tick t = bo + b * beat_;
                    if (t >= c.len) continue;
                    for (size_t i = 0; i < v.size(); ++i) {
                        add(upper(v[i]), v[i], t0 + t, eighth, vel * 0.6f);
                        if (P.tex == Tex::March && brass && i > 0) add(HN, v[i], t0 + t, eighth, vel * 0.5f);
                    }
                }
            break;
        default: break;
        }
    }

    // ---------- 3. Gegenstimme: Terzen/Septimen der Akkorde, schrittweise verbunden ----------
    if (counter) {
        const int ci = P.counterInst;
        const int lo = range[ci][0] + 3 - tr, hi = std::min(range[ci][1] - 3, range[ci][0] + 19) - tr;
        auto guide = [&](const Chord& ch, int ref, Tick spanA, Tick spanB) {
            std::vector<int> pcs;
            if (ch.intervals.size() > 1) pcs.push_back(mod12(ch.root + ch.intervals[1]));
            const int other = ch.intervals.size() > 3 ? ch.intervals[3] : ch.intervals.size() > 2 ? ch.intervals[2] : 0;
            pcs.push_back(mod12(ch.root + other));
            pcs.push_back(mod12(ch.root));   // Notlösung, falls Terz und Septime/Quinte an der Melodie reiben
            int best = (lo + hi) / 2, bd = 999;
            for (int p = lo; p <= hi; ++p) {
                const auto it = std::find(pcs.begin(), pcs.end(), mod12(p));
                if (it == pcs.end() || rubs(p, spanA, spanB)) continue;
                const int d = std::abs(p - (ref < 0 ? (lo + hi) / 2 : ref)) + (it == pcs.end() - 1 ? 4 : 0);   // Grundton nur notfalls
                if (d < bd) { bd = d; best = p; }
            }
            return best;
        };
        int prev = -1;
        for (size_t i = 0; i < tc.size(); ++i) {
            const TChord& c = tc[i];
            const Tick t0 = secStart + c.start;
            const bool split = c.len >= bar_ && i + 1 < tc.size();
            const int p = guide(c.chord, prev, t0, t0 + (split ? c.len / 2 : c.len));
            if (split) {
                const Tick nt0 = secStart + tc[i + 1].start;
                const int nx = guide(tc[i + 1].chord, p, nt0, nt0 + tc[i + 1].len);
                add(ci, p, t0, c.len / 2, vel * 0.55f);
                int pass = std::abs(nx - p) >= 3 ? mel_.pitch(mel_.nearestDegree((p + nx) / 2)) : p;
                if (rubs(pass, t0 + c.len / 2, t0 + c.len)) pass = p;
                add(ci, pass, t0 + c.len / 2, c.len / 2 - six_ / 2, vel * 0.5f);
            } else {
                add(ci, p, t0, c.len - six_ / 2, vel * 0.55f);
            }
            prev = p;
        }
    }

    // ---------- 4. Bass ----------
    std::array<size_t, OCOUNT> beforeBass{};
    for (int i = 0; i < OCOUNT; ++i) beforeBass[size_t(i)] = orch_[size_t(i)].notes.size();
    // Spielen die Celli gerade die Melodie, übernehmen sie keine Bassstimme (sonst reibt beides in einer Spur)
    const bool vcMelody = lead == VC || melT == MelT::Unison || (melT != MelT::Solo && second == VC) || (counter && P.counterInst == VC);
    switch (P.bass) {
    case BassT::Pedal: {   // Orgelpunkt: Grundton (im Pre-Refrain die Dominante) liegt durch
        const int pc = sd.type == Sec::Pre ? keyRoot_ + 7 : keyRoot_;
        int p = 28;
        while (mod12(p) != mod12(pc)) ++p;
        // Liegt ein Akkordton einen Halbton neben dem Orgelpunkt (z.B. Leitton), weicht der Bass für diesen Akkord aus
        for (auto& c : tc) {
            const bool clash = c.chord.hasPc(p + 1) || c.chord.hasPc(p - 1);
            add(KB, clash ? c.bass : p, secStart + c.start, c.len - six_ / 2, vel * 0.7f);
        }
        if (!vcMelody) for (auto& n : bass) put(VC, n, n.pitch + 12 <= 64 ? 12 : 0, 0.8f);
        break;
    }
    case BassT::LowOstinato: {   // tiefe Streicher in Achteln/16teln, Oktaven im Wechsel
        const Tick step = e > 0.75f ? six_ : eighth;
        for (auto& c : tc) {
            int i = 0;
            for (Tick t = 0; t < c.len; t += step, ++i) {
                if (!vcMelody) add(VC, c.bass + ((i % 2) ? 12 : 0), secStart + c.start + t, step - six_ / 4, vel * ((t % beat_ == 0) ? 0.75f : 0.6f));
                add(KB, c.bass, secStart + c.start + t, step - six_ / 4, vel * 0.6f);
            }
        }
        break;
    }
    case BassT::Pizz:
        for (auto& c : tc)
            for (Tick bo = 0; bo < c.len; bo += bar_) {
                add(PZ, c.bass, secStart + c.start + bo, eighth, vel * 0.8f);
                add(KB, c.bass, secStart + c.start + bo, eighth, vel * 0.6f);
                if (bpb_ == 4 && bo + 2 * beat_ < c.len) add(PZ, fifthAbove(c), secStart + c.start + bo + 2 * beat_, eighth, vel * 0.7f);
            }
        break;
    default: {   // gehalten / Walking: Basslinie des Grundgerüsts
        // Spielen die Celli gerade die Melodie, übernimmt das Fagott ihre Bassverdopplung
        for (auto& n : bass) {
            put(KB, n, 0, 1.f);
            if (!vcMelody) put(VC, n, n.pitch + 12 <= 64 ? 12 : 0, 0.9f);
            else if (lead != FG && second != FG) put(FG, n, 12, 0.7f);
            if (!vcMelody && e < 0.6f && (sd.type == Sec::Verse || sd.type == Sec::Bridge)) put(FG, n, 12, 0.6f);
        }
        break;
    }
    }
    markNotes(bassKeys_, beforeBass);   // Bassnoten gelten beim Bereinigen ebenfalls als fest
    if (brass && sd.type == Sec::Chorus && e > 0.7f && P.tex != Tex::Stabs && !P.braams && !P.brassChorale)   // Posaunen: Grundton + Quinte
        for (auto& c : tc) {
            int p = 43;
            while (mod12(p) != c.chord.root) ++p;
            const int fifth = c.chord.intervals.size() > 2 ? c.chord.intervals[2] : 7;
            add(TB, p, secStart + c.start, c.len - six_ / 2, vel * 0.6f);
            add(TB, p + fifth, secStart + c.start, c.len - six_ / 2, vel * 0.5f);
        }
    if (brass && (sd.type == Sec::Chorus || (P.fanfare && sd.type == Sec::Intro)) &&
        (P.perc == PercT::EpicHits || P.perc == PercT::SnareMarch || P.fanfare))   // Trompeten-Fanfare
        for (auto& c : tc) {
            int p = 65;
            while (!c.chord.hasPc(p)) ++p;
            for (Tick bo = 0; bo < c.len; bo += bar_) {
                const Tick t0 = secStart + c.start + bo;
                add(TP, p, t0, beat_ - six_, vel * 0.75f);
                if (bpb_ == 4) {
                    add(TP, p, t0 + 2 * beat_, eighth - six_ / 2, vel * 0.65f);
                    add(TP, p, t0 + 2 * beat_ + eighth, eighth - six_ / 2, vel * 0.65f);
                    add(TP, p, t0 + 3 * beat_, beat_ - six_, vel * 0.7f);
                }
            }
        }
    // Blechbläser-Choral: Posaunen (Grundton + Quinte), Hörner (Terz + Quinte), Trompete oben – mit Stimmführung
    if (brass && P.brassChorale) {
        const bool soft = sd.type == Sec::Bridge || sd.type == Sec::Outro;
        const float cv = vel * (soft ? 0.5f : 0.64f);
        int prevTop = 70;
        for (auto& c : tc) {
            const Tick t0 = secStart + c.start, len = c.len - six_ / 2;
            const int third = c.chord.intervals.size() > 1 ? c.chord.intervals[1] : 4;
            const int fifth = c.chord.intervals.size() > 2 ? c.chord.intervals[2] : 7;
            int root = 40;
            while (mod12(root) != c.chord.root) ++root;
            add(TB, root, t0, len, cv * 0.95f);
            add(TB, root + fifth, t0, len, cv * 0.85f);
            int h1 = 53;
            while (mod12(h1) != mod12(c.chord.root + third)) ++h1;
            int h2 = h1 + 1;
            while (mod12(h2) != mod12(c.chord.root + fifth)) ++h2;
            add(HN, h1, t0, len, cv * 0.85f);
            add(HN, h2, t0, len, cv * 0.8f);
            if (!soft) {
                int best = -1;
                for (int p = 64; p <= 76; ++p)
                    if (c.chord.hasPc(p) && (best < 0 || std::abs(p - prevTop) < std::abs(best - prevTop))) best = p;
                if (best >= 0) { add(TP, best, t0, len, cv * 0.72f); prevTop = best; }
            }
        }
    }

    // ---------- 5. Schlagwerk ----------
    auto timp = [&](int pc) { int p = 41; while (mod12(p) != mod12(pc + tr)) ++p; return p; };
    auto timpHit = [&](int pitch, Tick t, float v) { orch_[TI].notes.push_back({pitch, t, beat_, std::clamp(v, 0.05f, 1.f)}); };
    auto perc = [&](int pitch, Tick t, float v) { orch_[PC].notes.push_back({pitch, t, six_, std::clamp(v, 0.05f, 1.f)}); };
    switch (P.perc) {
    case PercT::TimpHits:
        timpHit(timp(tc.front().chord.root), secStart, vel * 0.9f);
        if (sd.type == Sec::Chorus)
            for (int b = 2; b < sd.bars; b += 2) timpHit(timp(chordAt(tc, Tick(b) * bar_).root), secStart + Tick(b) * bar_, vel * 0.7f);
        break;
    case PercT::TimpOstinato:   // treibendes Pauken-Muster (Grundton, zuletzt Quinte)
        for (int b = 0; b < sd.bars; ++b) {
            const Tick bo = secStart + Tick(b) * bar_;
            const std::vector<int> pat = bpb_ == 3 ? std::vector<int>{0, 2, 4} : std::vector<int>{0, 2, 3, 5};
            for (size_t i = 0; i < pat.size(); ++i) {
                // Akkord zum Zeitpunkt des Schlags (Harmonie kann mitten im Takt wechseln); Quinte nur, wenn sie rein ist
                const Chord& ch = chordAt(tc, Tick(b) * bar_ + pat[i] * eighth);
                const bool pureFifth = ch.intervals.size() > 2 && ch.intervals[2] == 7;
                timpHit(i + 1 == pat.size() && pureFifth ? timp(ch.root + 7) : timp(ch.root), bo + pat[i] * eighth, vel * (i == 0 ? 0.85f : 0.6f));
            }
        }
        break;
    case PercT::EpicHits:   // große Trommel + Becken + Pauke auf den großen Zeiten
        for (int b = 0; b < sd.bars; b += (sd.type == Sec::Chorus ? 2 : 4)) {
            const Tick t = secStart + Tick(b) * bar_;
            perc(36, t, vel);
            if (b % 4 == 0) perc(49, t, vel * 0.85f);
            timpHit(timp(chordAt(tc, Tick(b) * bar_).root), t, vel * 0.85f);
        }
        break;
    case PercT::SnareMarch: {   // Marsch-Trommel
        const std::vector<int> pat = bpb_ == 3 ? std::vector<int>{0, 2, 3, 4, 6, 8, 10} : std::vector<int>{0, 2, 3, 4, 6, 8, 10, 11, 12, 14};
        for (int b = 0; b < sd.bars; ++b) {
            const Tick bo = secStart + Tick(b) * bar_;
            for (int x : pat) perc(38, bo + x * six_, vel * ((x % 4 == 0) ? 0.75f : 0.45f));
            perc(36, bo, vel * 0.7f);
        }
        perc(49, secStart, vel * 0.8f);
        break;
    }
    default: break;
    }
    // Übergang in den Refrain / zum Schluss: Wirbel
    if ((nextChorus || lastSec) && sd.type != Sec::Chorus && P.perc != PercT::None) {
        const Tick r0 = secStart + secLen - bar_;
        const int p = timp(keyRoot_ + 7);
        for (Tick t = 0; t < bar_; t += six_ / 2) timpHit(p, r0 + t, vel * (0.25f + 0.6f * float(t) / float(bar_)));
        if (P.perc == PercT::SnareMarch || P.perc == PercT::EpicHits)
            for (Tick t = 0; t < bar_; t += six_ / 2) perc(38, r0 + t, vel * (0.2f + 0.6f * float(t) / float(bar_)));
    }

    // ---------- 6. Glanz ----------
    if (P.sparkle)   // Glockenspiel verdoppelt die betonten Melodietöne
        for (auto& n : mel)
            if (strengthAt(n.start - secStart) >= 2) put(GL, n, 12, 0.5f);
    if (P.gliss && nextChorus && sd.type != Sec::Chorus) {   // Harfen-Glissando hinein in den Refrain
        const int n = 16;
        const Tick step = (2 * beat_) / n;
        const int d0 = mel_.nearestDegree(55);
        for (int i = 0; i < n; ++i)
            add(HP, mel_.pitch(d0 + i), secStart + secLen - 2 * beat_ + i * step, step * 2, vel * (0.4f + 0.4f * float(i) / float(n)));
    }

    // ---------- 6a. Spannung aufbauen (vor dem Refrain) und Ausrufezeichen ----------
    {
        const Tick secEnd = secStart + secLen;
        if (nextChorus && P.timpRoll) {   // Paukenwirbel: immer dichter und lauter hinein in den Refrain
            const Tick len = (sd.bars >= 4 && chance(r_, 0.5f) ? 2 : 1) * bar_;
            const Tick st0 = std::max(secStart, secEnd - len);
            const int pitch = timp(chordAt(tc, secLen - six_).root);
            for (Tick tt = st0; tt < secEnd; tt += six_ / 2) {   // 32tel-Schläge, abwechselnd links/rechts
                const float prog = float(tt - st0) / float(std::max<Tick>(1, secEnd - st0));
                const float alt = ((tt - st0) / (six_ / 2)) % 2 ? 0.92f : 1.f;
                timpHit(pitch, tt, vel * (0.1f + 0.7f * prog * prog) * alt);
            }
        }
        if (nextChorus && P.snareRoll) {   // Trommelwirbel (Marsch): Snare in 32teln, Crescendo
            const Tick st0 = std::max(secStart, secEnd - bar_);
            for (Tick tt = st0; tt < secEnd; tt += six_ / 2) {
                const float prog = float(tt - st0) / float(std::max<Tick>(1, secEnd - st0));
                perc(38, tt, vel * (0.15f + 0.6f * prog));
            }
        }
        if (nextChorus && P.tremoloBuild) {   // Streicher-Tremolo schwillt im letzten Takt an
            const Tick st0 = std::max(secStart, secEnd - bar_);
            for (Tick tt = st0; tt < secEnd; tt += six_) {
                const float prog = float(tt - st0) / float(std::max<Tick>(1, secEnd - st0));
                const auto v = voice(chordAt(tc, tt - secStart));
                const int tracks3[3] = {VA, V2, V1};
                for (size_t j = 0; j < v.size() && j < 3; ++j)
                    add(tracks3[j], v[j] + (j == 2 ? 12 : 0), tt, six_ - six_ / 8, vel * (0.15f + 0.65f * prog));
            }
        }
        if (nextChorus && brass && P.brassSwell) {   // Blech: leise ansetzen, anschwellen (Hörner + Posaunen)
            const Tick st0 = std::max(secStart, secEnd - bar_);
            const Chord& ch = chordAt(tc, secLen - six_);
            int root = 40;
            while (mod12(root) != ch.root) ++root;
            const int fifth = ch.intervals.size() > 2 ? ch.intervals[2] : 7;
            const int third = ch.intervals.size() > 1 ? ch.intervals[1] : 4;
            // als Folge immer lauterer, überlappender Töne (die Lautstärke-Automation glättet das)
            for (int step = 0; step < 4; ++step) {
                const Tick tt = st0 + (secEnd - st0) * step / 4;
                const float v = vel * (0.2f + 0.18f * float(step));
                add(TB, root, tt, (secEnd - st0) / 4, v);
                add(HN, root + 12 + third, tt, (secEnd - st0) / 4, v);
                add(HN, root + 12 + fifth, tt, (secEnd - st0) / 4, v);
            }
        }
        if (sd.type == Sec::Chorus && P.accentHit) {   // "Schicksalsschlag": alles auf die Eins
            const int root = tc.front().chord.root;
            timpHit(timp(root), secStart, std::min(1.f, vel * 1.3f));
            perc(35, secStart, std::min(1.f, vel * 1.2f));   // große Trommel
            perc(49, secStart, vel * 0.85f);                 // Becken
        }
        // Großes Finale: Paukenwirbel über den letzten Takt (meist die Dominante) – der große Schlag mit
        // Pauke, großer Trommel und Becken fällt dann auf die Eins des Schlussakkords (siehe "Schlusstakt")
        if (k + 1 == st.size() && sd.bars >= 2 && bpb_ >= 3 && !sd.fade &&
            (epic_ || style == "Episch (Film)" || style == "Marsch / Heroisch" || style == "Romantisch" || style == "Mysteriös")) {
            const Tick st0 = std::max(secStart, secEnd - bar_);
            const int pitch = timp(chordAt(tc, secLen - six_).root);
            for (Tick tt = st0; tt < secEnd; tt += six_ / 2) {
                const float prog = float(tt - st0) / float(std::max<Tick>(1, secEnd - st0));
                timpHit(pitch, tt, vel * (0.15f + 0.65f * prog * prog) * (((tt - st0) / (six_ / 2)) % 2 ? 0.92f : 1.f));
            }
            bigFinale_ = true;
        }
    }

    // ---------- 6b. Weitere Farben: Triangel, Piccolo, Tuba, Cembalo, Gong ----------
    bool lastCh = sd.type == Sec::Chorus;
    for (size_t j = k + 1; j < st.size() && lastCh; ++j) if (st[j].type == Sec::Chorus) lastCh = false;
    if (P.sparkle && sd.type == Sec::Chorus)   // Triangel: helles "Ting" auf den Takt-Einsen
        for (int b = 0; b < sd.bars; ++b) {
            perc(81, secStart + Tick(b) * bar_, vel * 0.5f);
            if (bpb_ == 4 && b % 2 == 1) perc(80, secStart + Tick(b) * bar_ + 2 * beat_, vel * 0.35f);
        }
    if (sd.type == Sec::Chorus && e >= 0.78f && (style == "Episch (Film)" || style == "Marsch / Heroisch"))   // Piccolo: Melodie eine Oktave höher
        for (auto& n : mel)
            if (n.length >= 2 * six_) put(PI, n, 12, 0.45f);
    if (brass && (P.brassChorale || (sd.type == Sec::Chorus && e >= 0.72f)))   // Tuba: Fundament unter dem Blech
        for (auto& n : bass) put(TU, n, n.pitch >= 46 ? -12 : 0, 0.7f);
    if (style == "Barock / Kammer" && sd.type != Sec::Outro)   // Cembalo: Generalbass-Akkorde (Continuo) auf jedem Schlag
        for (auto& c : tc) {
            const auto v = voice(c.chord);
            for (Tick bo = 0; bo < c.len; bo += beat_)
                for (int p : v) add(CB, p, secStart + c.start + bo, eighth, vel * ((c.start + bo) % bar_ == 0 ? 0.55f : 0.42f));
        }
    if ((epic_ || style == "Mysteriös") && lastCh && k > 0) {   // Becken-Wirbel hinein, Gong auf die Eins des letzten Refrains
        const Tick roll = Tick(1.8 * rs_.bpm / 60.0 * double(song_.ppq));
        if (secStart > roll) perc(55, secStart - roll, vel * 0.8f);
        perc(52, secStart, vel * 0.9f);
    }
    if (P.sparkle && (sd.type == Sec::Bridge || (sd.type == Sec::Intro && k == 0)) && !epic_)   // Windspiel: Glitzern zu Beginn
        perc(84, secStart, vel * 0.6f);

    // ---------- 7. Epische Mittel ----------
    if (P.braams)   // tiefes Blech fortissimo – mit jedem Akkord neu, nur Akkordtöne (Grundton, Quinte, Oktave)
        for (auto& c : tc) {
            int root = 36;
            while (mod12(root) != c.chord.root) ++root;   // C2..H2
            const int fifth = c.chord.intervals.size() > 2 ? c.chord.intervals[2] : 7;
            const Tick t0 = secStart + c.start, len = c.len - six_ / 2;
            for (int p : {root, root + fifth, root + 12}) add(TB, p, t0, len, vel * 0.95f);
            for (int p : {root + 12, root + 12 + fifth}) add(HN, p, t0, len, vel * 0.85f);
        }
    if (P.choir) {
        if (P.choirMode == 1)   // Chor singt die Melodie mit (eine Oktave tiefer, wenn sie hoch liegt)
            for (auto& n : mel) put(CH, n, n.pitch > 76 ? -12 : 0, 0.85f);
        else                    // "Aah"-Fläche über den Akkorden
            for (auto& c : tc)
                for (int p : makeRoom(c, voice(c.chord))) add(CH, p + 7 <= 72 ? p + 12 : p, secStart + c.start, c.len - six_ / 2, vel * 0.7f);
    }
    if (P.taiko) {   // Taiko-Ensemble: 3+3+2-Muster, in ruhigeren Teilen nur die großen Schläge
        const bool full = sd.type == Sec::Chorus || e > 0.65f;
        for (int b = 0; b < sd.bars; ++b) {
            const Tick bo = secStart + Tick(b) * bar_;
            auto tk = [&](int pitch, int s16, float v) { if (Tick(s16) * six_ < bar_) orch_[TK].notes.push_back({pitch, bo + Tick(s16) * six_, six_, std::clamp(vel * v, 0.05f, 1.f)}); };
            if (bpb_ == 3) { tk(36, 0, 1.f); if (full) { tk(38, 4, 0.7f); tk(38, 8, 0.7f); tk(40, 10, 0.5f); } continue; }
            tk(36, 0, 1.f);
            tk(36, 6, full ? 0.9f : 0.6f);
            if (full) {
                tk(36, 12, 0.85f);
                tk(38, 3, 0.65f);
                tk(38, 9, 0.65f);
                tk(41, 14, 0.5f);
                if (b % 2 == 1) for (int s16 = 12; s16 < 16; ++s16) tk(40, s16, 0.4f + 0.1f * float(s16 - 12));
            }
        }
    }

    // ---------- 8. Überleitung in den nächsten Abschnitt (je nach Stil) ----------
    if (k + 1 < st.size()) {
        const Tick last = secStart + secLen - bar_;   // letzter Takt
        auto run = [&](int inst, bool up) {   // Tonleiter-Lauf in 16teln über die letzten zwei Schläge
            const int d0 = mel_.nearestDegree(up ? 64 : 84);
            for (int i = 0; i < 8; ++i)
                add(inst, mel_.pitch(up ? d0 + i : d0 - i), secStart + secLen - 2 * beat_ + i * (beat_ / 4), beat_ / 4,
                    vel * (0.45f + 0.04f * float(i)));
        };
        if (style == "Episch (Film)" && nextChorus) {
            // Riser: Streicher-Tremolo auf der Dominante schwillt an, Becken rollt dazu
            int d = 67;
            while (mod12(d) != mod12(keyRoot_ + 7)) ++d;
            for (Tick t = 0; t < bar_; t += six_) {
                const float v = vel * (0.15f + 0.75f * float(t) / float(bar_));
                add(V1, d + 12, last + t, six_, v);
                add(V2, d, last + t, six_, v);
            }
            for (Tick t = 0; t < bar_; t += six_ / 2) orch_[PC].notes.push_back({49, last + t, six_ / 2, std::clamp(vel * (0.05f + 0.6f * float(t) / float(bar_)), 0.05f, 1.f)});
            if (P.taiko) for (Tick t = 0; t < bar_; t += six_) orch_[TK].notes.push_back({t < bar_ / 2 ? 38 : 40, last + t, six_, std::clamp(vel * (0.3f + 0.6f * float(t) / float(bar_)), 0.05f, 1.f)});
        } else if (style == "Pastoral" || style == "Barock / Kammer") {
            run(style == "Pastoral" ? FL : OB, true);
        } else if ((style == "Walzer" || style == "Romantisch") && !P.gliss) {
            run(nextChorus ? FL : CL, nextChorus);
        } else if (style == "Mysteriös") {   // tiefe Streicher schwellen im Tremolo an
            int d = 48;
            while (mod12(d) != mod12(keyRoot_ + 7)) ++d;
            for (Tick t = 0; t < bar_; t += six_) add(VC, d, last + t, six_, vel * (0.15f + 0.5f * float(t) / float(bar_)));
        }
    }

    // ---------- 9. Gestaffelte Einsätze: Schichten kommen nacheinander dazu statt alle auf einmal ----------
    {
        auto delayTrack = [&](int tr2, Tick from) {
            if (tr2 == P.lead) return;   // die Melodie bleibt
            auto& notes = orch_[size_t(tr2)].notes;
            notes.erase(std::remove_if(notes.begin() + long(before[size_t(tr2)]), notes.end(),
                                       [&](const Note& n) { return n.start < secStart + from; }),
                        notes.end());
        };
        const Tick half = Tick(std::max(1, sd.bars / 2)) * bar_;
        const Tick quarter = Tick(std::max(1, sd.bars / 4)) * bar_;
        const bool buildUp = sd.type == Sec::Intro || sd.type == Sec::Pre || (sd.type == Sec::Bridge && nextChorus);
        if (buildUp) {
            for (int t2 : {int(TI), int(PC), int(TK)}) delayTrack(t2, half);
            if (sd.type == Sec::Intro) {
                for (int t2 : {int(HN), int(TP), int(TB), int(CH)}) delayTrack(t2, half);
                if (counter) delayTrack(P.counterInst, quarter);
            }
        }
        if (sd.type == Sec::Verse && m.occurrences == 0 && counter) delayTrack(P.counterInst, half);   // Gegenstimme kommt später dazu
    }

    // ---------- 10. Dynamik: Crescendo (Aufbau) / Decrescendo (Outro) ----------
    const bool cresc = P.crescendo || sd.type == Sec::Pre;
    const bool decresc = sd.type == Sec::Outro;
    if (cresc || decresc)
        for (int i = 0; i < OCOUNT; ++i)
            for (size_t j = before[size_t(i)]; j < orch_[size_t(i)].notes.size(); ++j) {
                Note& n = orch_[size_t(i)].notes[j];
                const float prog = std::clamp(float(n.start - secStart) / float(secLen), 0.f, 1.f);
                n.velocity = std::clamp(n.velocity * (cresc ? 0.65f + 0.4f * prog : 1.f - 0.45f * prog), 0.05f, 1.f);
            }
}

// ===================== Bass =====================
// Viterbi-Suche: Basstöne aus Grundton/Terz/Quinte so wählen, dass die Linie
// melodisch fließt (Schritte bevorzugt) und meist auf dem Grundton steht.
void Builder::assignBassLine(std::vector<TChord>& tc) {
    struct Cand { int pitch; float cost; };
    const size_t n = tc.size();
    std::vector<std::vector<Cand>> cands(n);
    for (size_t i = 0; i < n; ++i) {
        auto pcs = tc[i].chord.pcs();
        for (size_t k = 0; k < pcs.size(); ++k)
            for (int p = 33; p <= 47; ++p) {
                if (mod12(p) != pcs[k]) continue;
                float c = k == 0 ? 0.f : k == 1 ? 1.5f - s_.creativity * 0.7f : k == 2 ? 2.6f : 3.5f;
                if ((i == 0 || i + 1 == n) && k != 0) c += 1.5f;
                c += std::abs(p - 40) * 0.06f + frand(r_, 0.f, 0.3f);
                cands[i].push_back({p, c});
            }
    }
    auto trans = [](int a, int b) {
        int d = std::abs(a - b);
        if (d == 0) return 0.35f;
        if (d <= 2) return 0.f;
        if (d <= 4) return 0.3f;
        if (d == 5 || d == 7) return 0.35f;
        return 0.5f + d * 0.12f;
    };
    std::vector<std::vector<float>> cost(n);
    std::vector<std::vector<int>> from(n);
    for (auto& c : cands[0]) cost[0].push_back(c.cost + (prevBass_ >= 0 ? trans(prevBass_, c.pitch) * 0.5f : 0.f));
    from[0].assign(cands[0].size(), -1);
    for (size_t i = 1; i < n; ++i) {
        for (auto& c : cands[i]) {
            float best = 1e9f;
            int arg = 0;
            for (size_t k = 0; k < cands[i - 1].size(); ++k) {
                float v = cost[i - 1][k] + trans(cands[i - 1][k].pitch, c.pitch);
                if (v < best) { best = v; arg = int(k); }
            }
            cost[i].push_back(best + c.cost);
            from[i].push_back(arg);
        }
    }
    int j = int(std::min_element(cost[n - 1].begin(), cost[n - 1].end()) - cost[n - 1].begin());
    for (size_t i = n; i-- > 0;) {
        tc[i].bass = cands[i][j].pitch;
        j = from[i][j];
    }
    prevBass_ = tc.back().bass;
}

int Builder::fifthAbove(const TChord& tc) const {
    int best = tc.bass + 7, bestD = 99;
    for (int p = tc.bass + 3; p <= tc.bass + 9; ++p)
        if (tc.chord.hasPc(p) && mod12(p) != mod12(tc.bass) && std::abs(p - (tc.bass + 7)) < bestD) {
            bestD = std::abs(p - (tc.bass + 7));
            best = p;
        }
    return best;
}

void Builder::genBass(const Material& m, const std::vector<TChord>& tc, Tick secStart, float vel, std::vector<Note>& out) {
    const Tick eighth = beat_ / 2;
    for (size_t ci = 0; ci < tc.size(); ++ci) {
        const TChord& c = tc[ci];
        const int b = c.bass;
        const int fifth = fifthAbove(c);
        const int next = ci + 1 < tc.size() ? tc[ci + 1].bass : b;
        auto add = [&](int p, Tick st, Tick l, float v) {
            if (st >= c.len) return;
            l = std::min(l, c.len - st);
            out.push_back({p, secStart + c.start + st, std::max<Tick>(l, six_), std::clamp(v, 0.05f, 1.f)});
        };
        for (Tick bo = 0; bo < c.len; bo += bar_) {
            // Rhythmus-System: typische Basslinien (Clave, Reggae, Bossa) – nicht beim Orgelpunkt / festen Bass-Stil
            if (bpb_ == 4 && !m.pedal && s_.bass == BassStyle::Auto &&
                (m.rhySys == RhySys::Clave || m.rhySys == RhySys::Offbeat || m.rhySys == RhySys::Bossa)) {
                const bool odd = ((c.start + bo) / bar_) % 2 == 1;
                struct BN { int at, len; bool fifth; float v; };
                std::vector<BN> pat;
                if (m.rhySys == RhySys::Clave) pat = odd ? std::vector<BN>{{0, 7, false, 0.9f}, {8, 6, true, 0.8f}}
                                                         : std::vector<BN>{{0, 5, false, 0.95f}, {6, 5, true, 0.82f}, {12, 3, false, 0.8f}};
                else if (m.rhySys == RhySys::Offbeat) pat = {{0, 6, false, 0.95f}, {8, 3, true, 0.8f}, {12, 3, false, 0.78f}};
                else pat = {{0, 6, false, 0.9f}, {6, 2, false, 0.7f}, {8, 6, true, 0.85f}, {14, 2, true, 0.68f}};   // Bossa
                for (auto& n : pat) add(n.fifth ? fifth : b, bo + n.at * six_, Tick(n.len) * six_ - six_ / 4, vel * n.v);
                continue;
            }
            switch (m.bass) {
            case BassStyle::Auto:
            case BassStyle::Whole: add(b, bo, bar_ - six_ / 2, vel); break;
            case BassStyle::RootFifth:
                add(b, bo, 2 * beat_ - six_ / 2, vel);
                if (bpb_ >= 3) add(fifth, bo + 2 * beat_, Tick(bpb_ - 2) * beat_ - six_ / 2, vel * 0.85f);
                break;
            case BassStyle::Eighths: {
                const int n = int(bar_ / eighth);
                for (int i = 0; i < n; ++i)
                    add((m.bassOctaves && i % 2 == 1) ? b + 12 : b, bo + i * eighth, Tick(eighth * 0.8), vel * (i % 2 == 0 ? 0.9f : 0.72f));
                break;
            }
            case BassStyle::Walking:
                for (int i = 0; i < bpb_; ++i) {
                    Tick t = bo + i * beat_;
                    int p;
                    if (i == 0) p = b;
                    else if (t + beat_ >= c.len) {
                        // Annäherung an den nächsten Grundton: Halbton nur, wenn er leiterleigen ist und nicht am Akkord reibt
                        p = next;
                        for (int d : {-1, 1, -2, 2}) {
                            const int q = next + (chance(r_, 0.5f) ? d : -d);
                            if (!harm_.contains(q)) continue;
                            bool rub = false;
                            for (int cp : c.chord.pcs()) rub = rub || mod12(q - cp) == 1 || mod12(cp - q) == 1;
                            if (!rub) { p = q; break; }
                        }
                    }
                    else {
                        auto pcs = c.chord.pcs();
                        int pc = pcs[irand(r_, 1, int(pcs.size()) - 1)];
                        p = b;
                        while (mod12(p) != pc) ++p;
                    }
                    add(p, t, beat_ - six_ / 2, vel * (i == 0 ? 0.9f : 0.78f));
                }
                break;
            case BassStyle::Syncopated:
                if (bpb_ == 4) {
                    add(b, bo, 3 * eighth - six_ / 2, vel);
                    add(b, bo + 3 * eighth, 3 * eighth - six_ / 2, vel * 0.82f);
                    add(m.bassOctaves ? b + 12 : fifth, bo + 6 * eighth, 2 * eighth - six_ / 2, vel * 0.8f);
                } else {
                    add(b, bo, 3 * eighth - six_ / 2, vel);
                    add(fifth, bo + 3 * eighth, 3 * eighth - six_ / 2, vel * 0.8f);
                }
                break;
            default: break;
            }
        }
    }
}

// ===================== Schlagzeug =====================
void Builder::genDrums(const Material& m, const SecDef& sd, size_t k, Tick secStart, float vel, std::vector<Note>& out) {
    if (!m.drumsOn) return;
    using D = DrumStyle;
    enum { KICK = 36, RIM = 37, SNARE = 38, CLAP = 39, HHC = 42, HHP = 44, HHO = 46, CRASH = 49, RIDE = 51 };
    static const int toms[6] = {50, 48, 47, 45, 43, 41};   // hoch -> tief

    const auto& st = rs_.structure;
    const bool hasNext = k + 1 < st.size();
    const bool nextChorus = hasNext && st[k + 1].type == Sec::Chorus;
    const int steps = bpb_ * 4;   // 16tel pro Takt
    // Offbeat (Reggae): "One Drop" – Bassdrum und Snare zusammen auf Schlag 3
    const D style = (bpb_ == 4 && m.rhySys == RhySys::Offbeat && m.drumStyle != D::Jazz) ? D::HalfTime : m.drumStyle;
    const float e = secEnergy(k);
    auto hit = [&](int pitch, Tick t, float v) { out.push_back({pitch, t, six_, std::clamp(v, 0.05f, 1.f)}); };

    // Bassdrum-Muster; bei synkopiertem Bass spielt die Kick mit (3+3+2)
    std::vector<int> kick;
    if (bpb_ == 4) {
        static const std::vector<std::vector<int>> pop = {{0, 8}, {0, 8, 10}, {0, 6, 8}, {0, 8, 11}, {0, 3, 8, 10}};
        switch (style) {
        case D::FourOnFloor: kick = {0, 4, 8, 12}; break;
        case D::HalfTime: kick = m.kickVariant % 2 ? std::vector<int>{0, 11} : std::vector<int>{0, 3, 10}; break;
        case D::Ballad: kick = {0, 10}; break;
        case D::Rock: kick = m.kickVariant % 2 ? std::vector<int>{0, 8, 10} : std::vector<int>{0, 2, 8, 10}; break;
        default: kick = pop[size_t(m.kickVariant) % pop.size()]; break;
        }
        if (m.bass == BassStyle::Syncopated && style != D::FourOnFloor && style != D::HalfTime) kick = {0, 6, 12};
        // Rhythmus-Systeme: Bassdrum passend zum Muster
        switch (m.rhySys) {
        case RhySys::Tresillo: if (style != D::FourOnFloor) kick = {0, 6, 12}; break;
        case RhySys::Clave: kick = {0, 8}; break;
        case RhySys::Offbeat: kick = {8}; break;
        case RhySys::Bossa: kick = {0, 6, 8, 14}; break;
        default: break;
        }
    } else {
        kick = {0};
        if (bpb_ >= 5) kick.push_back((bpb_ / 2) * 4);
    }
    auto isKick = [&](int s) { return std::find(kick.begin(), kick.end(), s) != kick.end(); };
    // Clave / Bossa: Rimshot spielt das 2-taktige Clave-Muster
    if (bpb_ == 4 && !m.drumsLight && (m.rhySys == RhySys::Clave || m.rhySys == RhySys::Bossa)) {
        for (int b = 0; b < sd.bars; ++b) {
            const bool odd = b % 2 == 1;
            const std::vector<int> pos = m.rhySys == RhySys::Clave ? (odd ? std::vector<int>{4, 8} : std::vector<int>{0, 6, 12})
                                                                   : (odd ? std::vector<int>{4, 10} : std::vector<int>{0, 6, 12});
            for (int p : pos) hit(RIM, secStart + Tick(b) * bar_ + Tick(p) * six_, vel * 0.62f);
        }
    }
    auto isBackbeat = [&](int s) {
        if (style == D::HalfTime) return bpb_ == 4 ? s == 8 : s == (bpb_ - 1) * 4;
        if (bpb_ == 4) return s == 4 || s == 12;
        if (bpb_ == 3) return s == 4 || s == 8;   // Walzer: "bum - tschak - tschak"
        return s % 8 == 4;
    };

    const bool fill = hasNext && !m.drumsLight && chance(r_, nextChorus ? 0.9f : 0.5f);
    const int fillLen = (steps >= 8 && chance(r_, 0.5f)) ? 8 : 4;
    const int fillType = irand(r_, 0, 2);
    const float hatVel = 0.3f + e * 0.25f;

    for (int b = 0; b < sd.bars; ++b) {
        const Tick bo = secStart + Tick(b) * bar_;
        const bool lastBar = b == sd.bars - 1;
        const int fillFrom = (lastBar && fill) ? steps - fillLen : steps;

        if (b == 0 && k > 0 && sd.type != Sec::Intro) hit(CRASH, bo, vel * (sd.type == Sec::Chorus ? 1.f : 0.85f));

        if (style == D::Jazz && !m.drumsLight) {
            // Swing-Ride "ding - ding-ga - ding", Hi-Hat-Pedal auf 2 und 4, leise Bassdrum
            for (int bt = 0; bt < bpb_; ++bt) {
                const Tick t = bo + Tick(bt) * beat_;
                if (bt * 4 >= fillFrom) break;
                hit(RIDE, t, vel * (bt % 2 ? 0.6f : 0.5f));
                if (bt % 2 == 1) {
                    hit(RIDE, t + beat_ * 2 / 3, vel * 0.38f);
                    hit(HHP, t, vel * 0.45f);
                }
                hit(KICK, t, vel * 0.18f);
                if (chance(r_, 0.22f)) hit(SNARE, t + beat_ * 2 / 3, vel * 0.3f);   // Comping
            }
        } else {
            for (int s = 0; s < fillFrom; ++s) {
                const Tick t = bo + Tick(s) * six_;
                const bool onBeat = s % 4 == 0, on8 = s % 2 == 0;
                if (style == D::Epic) {   // Kino-Groove: Halftime-Snare, Tom-Ostinato 3+3+2, schwere Bassdrum
                    const int s16 = s % 16;
                    const bool big = sd.type == Sec::Chorus || e > 0.7f;
                    if (s16 == 0 || s16 == 10 || (big && s16 == 6)) hit(KICK, t, vel * (s16 == 0 ? 1.f : 0.85f));
                    if (!m.drumsLight && s16 == 8) { hit(SNARE, t, vel * 0.95f); hit(CLAP, t, vel * 0.6f); }
                    if (s16 == 0 || s16 == 6 || s16 == 12) hit(41, t, vel * (m.drumsLight ? 0.45f : 0.8f));
                    if (!m.drumsLight && (s16 == 3 || s16 == 9)) hit(43, t, vel * 0.6f);
                    if (big && s16 == 14) hit(45, t, vel * 0.6f);
                    if (big && s16 == 0 && b % 2 == 0) hit(CRASH, t, vel * 0.8f);
                    continue;
                }
                if (isKick(s) && (!m.drumsLight || s == 0)) hit(KICK, t, vel * (s == 0 ? 1.f : 0.85f));
                if (!m.drumsLight && isBackbeat(s)) {
                    const bool soft = bpb_ == 3 || (style == D::Ballad && sd.type != Sec::Chorus);
                    if (soft) hit(style == D::Ballad ? RIM : SNARE, t, vel * 0.55f);
                    else {
                        hit(SNARE, t, vel * 0.9f);
                        if (style == D::FourOnFloor) hit(CLAP, t, vel * 0.7f);
                    }
                }
                // Becken / Hi-Hat
                if (style == D::FourOnFloor && !m.drumsLight) {
                    if (s % 4 == 2) hit(HHO, t, vel * hatVel * 1.2f);
                    else if (s % 2 == 1) hit(HHC, t, vel * hatVel * 0.6f);
                } else if (style == D::Ballad) {
                    if (onBeat) hit(HHC, t, vel * hatVel * 0.9f);
                } else if (m.ride && !m.drumsLight) {
                    if (on8) hit(RIDE, t, vel * (onBeat ? 0.6f : 0.45f));
                } else {
                    const bool sixteenths = style == D::HalfTime && e > 0.7f;
                    if (on8 || sixteenths) {
                        const bool open = m.openHats && bpb_ == 4 && s == 14;
                        hit(open ? HHO : HHC, t, vel * hatVel * (onBeat ? 1.3f : on8 ? 1.f : 0.7f));
                    }
                }
                // Geisternoten
                if (!m.drumsLight && !on8 && !isBackbeat(s - 1) && !isBackbeat(s + 1) && chance(r_, m.ghost * 0.5f))
                    hit(SNARE, t, vel * 0.18f);
            }
        }

        // Hand-Percussion: bei lateinamerikanischen/karibischen Rhythmen (oder Insel-Instrumenten)
        // spielen Congas (Tumbao), Bongos (Martillo), Shaker und im Refrain eine Agogo-Glocke mit
        if (handPerc(m) && style != D::Jazz && style != D::Epic) {
            const bool full = sd.type == Sec::Chorus || e > 0.65f;
            const bool soft = sd.type == Sec::Intro || sd.type == Sec::Outro || m.drumsLight;
            const float pv = vel * (soft ? 0.6f : 1.f);
            for (int s = 0; s < fillFrom; ++s) {
                const Tick t = bo + Tick(s) * six_;
                const int s16 = s % 16;
                if (s % 2 == 0 || full) hit(70, t, pv * (s % 4 == 2 ? 0.42f : s % 2 ? 0.2f : 0.28f));   // Shaker
                if (soft) continue;
                if (s16 == 4) hit(62, t, pv * 0.62f);                                   // Conga-Slap
                if (s16 == 7) hit(63, t, pv * 0.4f);
                if (s16 == 12 || s16 == 14) hit(s16 == 12 ? 63 : 64, t, pv * 0.6f);   // offene Töne
                if (full && s % 2 == 0) hit(s16 == 6 || s16 == 14 ? 61 : 60, t, pv * (s16 % 4 == 0 ? 0.42f : 0.3f));   // Bongos
                if (full && e > 0.6f && (s16 == 0 || s16 == 3 || s16 == 6 || s16 == 10 || s16 == 12))
                    hit(s16 == 3 || s16 == 10 ? 68 : 67, t, pv * 0.35f);              // Agogo
            }
        }
        // Windspiel als Glitzer zu Beginn ruhiger Teile, wenn die Melodie "funkelt"
        if (b == 0 && (sd.type == Sec::Intro || sd.type == Sec::Bridge) && sparkleSong())
            hit(84, bo, vel * 0.55f);

        // Steigerung im Pre-Refrain: Snare-Achtel mit Crescendo
        if (sd.type == Sec::Pre && lastBar && fillFrom == steps)
            for (int s = 0; s < steps; s += 2) hit(SNARE, bo + Tick(s) * six_, vel * (0.35f + 0.6f * float(s) / float(steps)));

        // Episch: Becken schwillt im letzten Takt vor dem Refrain an
        if (style == D::Epic && lastBar && nextChorus)
            for (Tick t = 0; t < bar_; t += six_ / 2) hit(CRASH, bo + t, vel * (0.08f + 0.6f * float(t) / float(bar_)));

        // Fill vor dem nächsten Abschnitt
        if (fillFrom < steps) {
            hit(KICK, bo + Tick(fillFrom) * six_, vel * 0.8f);
            for (int s = fillFrom; s < steps; ++s) {
                if (style == D::Jazz && s % 2) continue;
                const float v = vel * (0.5f + 0.5f * float(s - fillFrom + 1) / float(steps - fillFrom));
                const Tick t = bo + Tick(s) * six_;
                const int tom = toms[std::min(5, (s - fillFrom) * 6 / (steps - fillFrom))];
                if (fillType == 0) hit(SNARE, t, v);
                else if (fillType == 1) hit(tom, t, v);
                else hit(s % 2 == 0 ? SNARE : tom, t, v);
            }
        }
    }
}

// ===================== Begleitung =====================
// Stimmführung: die Umkehrung wählen, die am wenigsten Bewegung zum vorigen Akkord braucht.
std::vector<int> Builder::voice(const Chord& c) {
    auto pcs = c.pcs();
    const int k = int(pcs.size());
    std::vector<int> best;
    float bestCost = 1e9f;
    for (int inv = 0; inv < k; ++inv)
        for (int base = 48; base < 60; ++base) {
            if (mod12(base) != pcs[inv]) continue;
            std::vector<int> v{base};
            for (int j = 1; j < k; ++j) {
                int pc = pcs[(inv + j) % k];
                int p = v.back() + 1;
                while (mod12(p) != pc) ++p;
                v.push_back(p);
            }
            if (v.back() > 72) continue;
            float avg = 0;
            for (int p : v) avg += float(p);
            avg /= float(k);
            float cost = std::abs(avg - 60.f) * 0.4f;
            for (int p : v) {
                int md = 99;
                for (int q : prevVoicing_) md = std::min(md, std::abs(p - q));
                if (!prevVoicing_.empty()) cost += float(md);
            }
            cost += frand(r_, 0.f, 0.8f);
            if (cost < bestCost) { bestCost = cost; best = v; }
        }
    if (best.empty()) best = {48 + pcs[0], 52 + pcs[0], 55 + pcs[0]};
    prevVoicing_ = best;
    return best;
}

void Builder::genAccomp(const Material& m, const std::vector<TChord>& tc, Tick secStart, float vel, std::vector<Note>& out) {
    const Tick eighth = beat_ / 2;
    // Eingespielte Akkorde: das eigene Begleitmuster wird beibehalten (bei längeren Teilen wiederholt)
    if (m.hasIdea && m.ideaHarmonic && !m.ideaNotes.empty()) {
        Tick secLen = 0;
        for (auto& c : tc) secLen = std::max(secLen, c.start + c.len);
        const Tick period = Tick(m.ideaBars) * bar_;
        for (Tick off = 0; off < secLen; off += period)
            for (const Note& n : m.ideaNotes)
                if (off + n.start < secLen)
                    out.push_back({n.pitch, secStart + off + n.start, std::min(n.length, secLen - off - n.start), n.velocity});
        return;
    }
    for (const TChord& c : tc) {
        auto v = voice(c.chord);
        auto add = [&](int p, Tick st, Tick l, float ve) {
            if (st >= c.len) return;
            l = std::min(l, c.len - st);
            out.push_back({p, secStart + c.start + st, std::max<Tick>(l, six_), std::clamp(ve, 0.05f, 1.f)});
        };
        for (Tick bo = 0; bo < c.len; bo += bar_) {
            switch (m.acc) {
            case AccompStyle::Auto:
            case AccompStyle::Pad:
                if (chordSustains_) {   // haltendes Instrument: einmal pro Akkord, durchklingen lassen
                    if (bo == 0) for (int p : v) add(p, 0, c.len - six_ / 2, vel * 0.8f);
                } else {
                    for (int p : v) add(p, bo, bar_ - six_ / 2, vel * 0.8f);
                }
                break;
            case AccompStyle::Rhythm: {
                // Rhythmus-System: eigene Anschlagsmuster in 16teln (2-taktig, wo nötig)
                if (bpb_ == 4 && m.rhySys != RhySys::Straight) {
                    const bool odd = ((c.start + bo) / bar_) % 2 == 1;
                    std::vector<int> hits;
                    Tick len16 = 0;   // 0 = bis zum nächsten Anschlag
                    float accentVel = 0.86f, otherVel = 0.72f;
                    switch (m.rhySys) {
                    case RhySys::Tresillo: hits = {0, 6, 12}; break;
                    case RhySys::Clave: hits = odd ? std::vector<int>{4, 8} : std::vector<int>{0, 6, 12}; break;
                    case RhySys::Offbeat: hits = {2, 6, 10, 14}; len16 = 1; accentVel = otherVel = 0.7f; break;   // kurze "Skanks"
                    case RhySys::Bossa: hits = odd ? std::vector<int>{2, 8, 12} : std::vector<int>{0, 6, 10}; break;
                    case RhySys::Driving: hits = {0, 2, 4, 6, 8, 10, 12, 14}; len16 = 1; break;
                    default: hits = {0, 8}; break;
                    }
                    for (size_t i = 0; i < hits.size(); ++i) {
                        const Tick st = bo + hits[i] * six_;
                        const Tick nx = i + 1 < hits.size() ? bo + hits[i + 1] * six_ : bo + bar_;
                        const Tick l = len16 > 0 ? Tick(len16) * six_ + six_ / 2 : Tick(double(nx - st) * 0.85);
                        const bool onBeat = hits[i] % 4 == 0;
                        for (int p : v) add(p, st, l, vel * (onBeat ? accentVel : otherVel));
                    }
                    break;
                }
                static const std::vector<std::vector<int>> p4 = {{0, 3, 6}, {0, 2, 4, 6}, {0, 3, 4, 6}};
                static const std::vector<std::vector<int>> p3 = {{0, 2, 4}, {0, 3, 4}, {0, 2, 3, 4}};
                std::vector<int> generic;
                for (int b = 0; b < bpb_; ++b) generic.push_back(b * 2);
                const auto& pat = bpb_ == 4 ? p4[m.rhythmPattern % 3] : bpb_ == 3 ? p3[m.rhythmPattern % 3] : generic;
                for (size_t i = 0; i < pat.size(); ++i) {
                    Tick st = bo + pat[i] * eighth;
                    Tick nx = i + 1 < pat.size() ? bo + pat[i + 1] * eighth : bo + bar_;
                    for (int p : v) add(p, st, Tick((nx - st) * 0.85), vel * (i == 0 ? 0.88f : 0.74f));
                }
                break;
            }
            case AccompStyle::Arpeggio: {
                std::vector<int> seq = v;
                seq.push_back(v[0] + 12);
                std::vector<int> order;
                for (int i = 0; i < int(seq.size()); ++i) order.push_back(i);
                for (int i = int(seq.size()) - 2; i >= 1; --i) order.push_back(i);
                const Tick step = m.arp16 ? six_ : eighth;
                const int n = int(bar_ / step);
                for (int i = 0; i < n; ++i)
                    add(seq[order[i % order.size()]], bo + i * step, step * 2, vel * (i == 0 ? 0.8f : 0.64f));
                break;
            }
            case AccompStyle::Alberti: {
                int lo = v[0], mid = v[std::min<size_t>(1, v.size() - 1)], hi = v[std::min<size_t>(2, v.size() - 1)];
                const int pat[4] = {lo, hi, mid, hi};
                const int n = int(bar_ / eighth);
                for (int i = 0; i < n; ++i) add(pat[i % 4], bo + i * eighth, eighth, vel * (i % 2 == 0 ? 0.72f : 0.6f));
                break;
            }
            case AccompStyle::Broken:
                if (!s_.withBass) add(v[0] - 12, bo, beat_, vel * 0.85f);
                for (int b = 1; b < bpb_; ++b)
                    for (int p : v) add(p, bo + b * beat_, beat_ / 2, vel * 0.68f);
                break;
            default: break;
            }
        }
    }
}

// ===================== Melodie =====================
std::vector<std::pair<Tick, Tick>> Builder::genRhythm(Tick unitTicks, float density) {
    struct Cell {
        int beats;
        std::vector<std::pair<int, int>> n;   // (Start, Länge) in Sechzehnteln
        float base, dens, sync, sparse;
        bool notFirst;
    };
    static const std::vector<Cell> cells = {
        {1, {{0, 4}}, 0.6f, 0.f, 0.f, 1.2f, false},                         // Viertel
        {2, {{0, 8}}, 0.05f, 0.f, 0.f, 0.9f, false},                        // Halbe
        {1, {{0, 2}, {2, 2}}, 0.4f, 1.4f, 0.f, 0.f, false},                 // 2 Achtel
        {1, {{0, 3}, {3, 1}}, 0.15f, 0.35f, 0.f, 0.f, false},               // punktiert
        {1, {{0, 1}, {1, 1}, {2, 2}}, 0.f, 0.5f, 0.f, 0.f, false},
        {1, {{0, 2}, {2, 1}, {3, 1}}, 0.f, 0.5f, 0.f, 0.f, false},
        {1, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}, -0.15f, 0.35f, 0.f, 0.f, false},
        {1, {{2, 2}}, 0.f, 0.f, 0.7f, 0.2f, true},                          // Off-Beat
        {1, {{0, 1}, {1, 2}, {3, 1}}, 0.f, 0.f, 0.5f, 0.f, false},          // synkopiert
        {2, {{0, 6}, {6, 2}}, 0.25f, 0.f, 0.3f, 0.f, false},                // punktierte Viertel
        {1, {}, 0.05f, 0.f, 0.f, 0.25f, true},                              // Pause
    };
    const int beats = int(unitTicks / beat_);
    std::vector<std::pair<Tick, Tick>> out;
    int b = 0;
    while (b < beats) {
        const Tick bs = b * beat_;
        if (beats >= 2 && b == beats - 2) {   // Phrasenende: langer Ton, evtl. Atempause
            if (chance(r_, 0.55f)) out.push_back({bs, 2 * beat_});
            else {
                out.push_back({bs, beat_});
                if (chance(r_, 0.5f)) out.push_back({bs + beat_, beat_});
            }
            b += 2;
            continue;
        }
        if (b == beats - 1) { out.push_back({bs, beat_}); ++b; continue; }
        std::vector<float> w;
        for (auto& c : cells) {
            float wt = c.base + c.dens * density + c.sync * s_.syncopation + c.sparse * (1.f - density);
            if (c.notFirst && b == 0) wt = 0;
            if (b + c.beats > beats - 2) wt = 0;
            w.push_back(std::max(0.f, wt));
        }
        const Cell& c = cells[pickWeighted(r_, w)];
        for (auto [o, l] : c.n) out.push_back({bs + o * six_, l * six_});
        b += c.beats;
    }
    // Antizipation: Ton vor die Takt-Eins ziehen (typisch Pop)
    for (size_t i = 1; i < out.size(); ++i) {
        if (out[i].first % bar_ == 0 && out[i].first > 0 && chance(r_, s_.syncopation * 0.35f)) {
            Tick ns = out[i].first - 2 * six_;
            auto& pv = out[i - 1];
            if (ns <= pv.first) continue;
            if (pv.first + pv.second > ns) pv.second = ns - pv.first;
            out[i].second += 2 * six_;
            out[i].first = ns;
        }
    }
    if (out.empty()) out.push_back({0, beat_});
    return out;
}

float Builder::contourAt(int shape, float amp, float p) const {
    const float pi = 3.14159265f;
    switch (shape) {
    case 0: return amp * std::sin(pi * p) - amp * 0.3f;          // Bogen
    case 1: return amp * (p - 0.5f) * 1.6f;                      // steigend
    case 2: return amp * (0.5f - p) * 1.6f;                      // fallend
    default: return -amp * std::sin(pi * p) + amp * 0.3f;        // Tal
    }
}

float Builder::noteWeight(int d, int prev, int prev2, const Chord& c, int strength, Tick len, float target) const {
    const bool ct = isCT(d, c);
    float w;
    if (prev == INT_MIN) w = ct ? 3.f : 0.2f;
    else {
        const int iv = std::abs(d - prev);
        static const float stepW[] = {0.3f, 1.f, 0.6f, 0.3f, 0.18f, 0.1f, 0.06f, 0.04f};
        if (iv > 7) return 0.f;
        w = stepW[iv];
        if (iv >= 3) w *= (0.6f + s_.creativity * 0.8f) * (0.35f + s_.leaps * 1.3f) * melLeapMul_;   // Sprungfreude
        if (prev2 != INT_MIN) {
            const int last = prev - prev2;
            if (std::abs(last) >= 3) {
                const int mv = d - prev;
                if (mv * sgn(last) < 0 && std::abs(mv) <= 2) w *= 2.5f;   // Sprung ausgleichen
                else if (mv * sgn(last) > 0) w *= 0.35f;
            }
            if (d == prev && prev == prev2) w *= 0.1f;
        }
        if (!ct && strength == 0 && iv != 1) w *= 0.2f;
    }
    static const float ctW[] = {1.2f, 1.8f, 3.f, 4.f};
    if (melPenta_) {   // Pentatonik-Hook: Halbton-Stufen (4./7. bzw. 2./6.) fast nie
        const int pc = mod12(realize(d, c) - keyRoot_);
        const bool ok = minorKey_ ? (pc == 0 || pc == 3 || pc == 5 || pc == 7 || pc == 10)
                                  : (pc == 0 || pc == 2 || pc == 4 || pc == 7 || pc == 9);
        if (!ok) w *= 0.08f;
    }
    if (ct) w *= ctW[strength] * melCtBias_;
    else {
        const float ncW[] = {0.9f, 0.5f, 0.12f + s_.creativity * 0.15f, 0.08f + s_.creativity * 0.15f};
        w *= ncW[strength];
        if (len >= beat_) w *= 0.4f;
    }
    const float sigma = 2.2f + s_.creativity * 2.f;
    const float dz = float(d) - target;
    w *= std::exp(-dz * dz / (2 * sigma * sigma));
    return w;
}

Motif Builder::pitchesFor(const std::vector<std::pair<Tick, Tick>>& rhythm, const MelEnv& e) {
    Motif m;
    const int shape = e.contour >= 0 ? e.contour : irand(r_, 0, 3);
    const float amp = frand(r_, 1.f, 3.f);
    const float bias = frand(r_, -1.f, 1.5f);
    int prev = INT_MIN, prev2 = INT_MIN;
    if (e.prevPitch >= 0) prev = mel_.nearestDegree(e.prevPitch);   // nahtlos an die vorige Phrase anschließen
    for (size_t i = 0; i < rhythm.size(); ++i) {
        auto [st, len] = rhythm[i];
        const float p = rhythm.size() > 1 ? float(i) / float(rhythm.size() - 1) : 0.f;
        const float target = float(e.center) + bias + contourAt(shape, amp, p);
        const Chord& c = chordAt(*e.chords, e.uStart + st);
        const int strength = strengthAt(st);
        const bool last = i + 1 == rhythm.size();
        std::vector<float> w;
        for (int d = e.lo; d <= e.hi; ++d) {
            float wt = noteWeight(d, prev, prev2, c, strength, len, target);
            if (last && e.role > 0) wt *= isCT(d, c) ? roleWeight(d, e.role) : 0.02f;
            w.push_back(wt);
        }
        const int d = e.lo + pickWeighted(r_, w);
        m.push_back({st, len, d});
        prev2 = prev;
        prev = d;
    }
    return m;
}

Motif Builder::randomMotif(const MelEnv& e) { return pitchesFor(genRhythm(e.uLen, e.density), e); }

// Abgeleitete Kandidaten: verwandt mit einem anderen Motiv (Zusammenhalt)
Motif Builder::derivedMotif(const Motif& rel, const MelEnv& e, int mode) {
    std::vector<std::pair<Tick, Tick>> rhythm;
    switch (mode) {
    case 1:   // gleicher Rhythmus, neue Töne
        for (auto& n : rel) if (n.start < e.uLen) rhythm.push_back({n.start, std::min(n.len, e.uLen - n.start)});
        return pitchesFor(rhythm.empty() ? genRhythm(e.uLen, e.density) : rhythm, e);
    case 2: {  // Ruf & Antwort: 1. Takt übernehmen, 2. Takt neu
        Motif m;
        for (auto& n : rel) if (n.start < bar_) m.push_back({n.start, std::min(n.len, bar_ - n.start), n.deg});
        auto r2 = genRhythm(e.uLen, e.density);
        std::vector<std::pair<Tick, Tick>> second;
        for (auto& p : r2) if (p.first >= bar_) second.push_back(p);
        MelEnv e2 = e;
        Motif tail = pitchesFor(second, e2);
        if (!m.empty() && !tail.empty()) {   // Anschluss: Anfang des 2. Takts nahe am Ende des 1.
            int shift = m.back().deg - tail.front().deg;
            shift = std::clamp(shift, -2, 2);
            for (auto& n : tail) n.deg += shift;
        }
        m.insert(m.end(), tail.begin(), tail.end());
        return snapToChords(m, e);
    }
    default: {  // Abspaltung + Sequenz: 1. Takt, im 2. Takt versetzt wiederholt
        Motif m;
        static const int seqs[] = {-2, -1, 1, 2};
        const int shift = seqs[irand(r_, 0, 3)];
        const Motif* src = &rel;
        for (auto& n : *src) if (n.start < bar_) m.push_back({n.start, std::min(n.len, bar_ - n.start), n.deg});
        const size_t first = m.size();
        for (size_t i = 0; i < first; ++i)
            if (m[i].start + bar_ < e.uLen) m.push_back({m[i].start + bar_, m[i].len, m[i].deg + shift});
        if (m.empty()) return randomMotif(e);
        return snapToChords(m, e);
    }
    }
}

Motif Builder::snapToChords(Motif m, const MelEnv& e) const {
    for (auto& n : m) {
        n.deg = std::clamp(n.deg, e.lo, e.hi);
        const Chord& c = chordAt(*e.chords, e.uStart + n.start);
        if ((strengthAt(n.start) >= 2 || n.len >= beat_) && !isCT(n.deg, c)) {
            for (int k : {1, -1, 2, -2, 3, -3}) {
                int d = n.deg + k;
                if (d >= e.lo && d <= e.hi && isCT(d, c)) { n.deg = d; break; }
            }
        }
    }
    return m;
}

Motif Builder::fitToUnit(const Motif& src, Tick uLen) const {
    Motif m;
    for (auto n : src) {
        if (n.start >= uLen) break;
        n.len = std::min(n.len, uLen - n.start);
        m.push_back(n);
    }
    return m;
}

Motif Builder::mutate(const Motif& m, const MelEnv& e) {
    Motif x = m;
    const int n = int(x.size());
    if (n == 0) return x;
    static const int deltas[] = {-2, -1, 1, 2};
    const int op = irand(r_, 0, 9);
    if (op <= 3) x[irand(r_, 0, n - 1)].deg += deltas[irand(r_, 0, 3)];
    else if (op == 4 && n >= 2) {
        int i = irand(r_, 0, n - 2), d = chance(r_, 0.5f) ? 1 : -1;
        x[i].deg += d;
        x[i + 1].deg += d;
    } else if (op == 5) {
        int d = chance(r_, 0.5f) ? 1 : -1;
        for (auto& nn : x) nn.deg += d;
    } else if (op == 6) x.back().deg += deltas[irand(r_, 0, 3)];
    else if (op == 7 && n >= 3) {   // zwei Töne verschmelzen
        int i = irand(r_, 0, n - 2);
        if (x[i].start + x[i].len == x[i + 1].start) {
            x[i].len += x[i + 1].len;
            x.erase(x.begin() + i + 1);
        }
    } else if (op == 8) {           // Ton teilen
        int i = irand(r_, 0, n - 1);
        if (x[i].len >= beat_) {
            Tick h = (x[i].len / 2) / six_ * six_;
            MNote b{x[i].start + h, x[i].len - h, x[i].deg + (chance(r_, 0.5f) ? 1 : -1)};
            x[i].len = h;
            x.insert(x.begin() + i + 1, b);
        }
    } else if (n >= 2) {            // Nachbarton
        int i = irand(r_, 1, n - 1);
        x[i].deg = x[i - 1].deg + (chance(r_, 0.5f) ? 1 : -1);
    }
    for (auto& nn : x) nn.deg = std::clamp(nn.deg, e.lo, e.hi);
    return x;
}

int Builder::bassAt(const MelEnv& e, Tick relT) const {
    if (!e.bass) return -1;
    const Tick t = e.secStart + relT;
    int found = -1;
    for (const Note& n : *e.bass) {
        if (n.start > t) break;
        if (n.end() > t) found = n.pitch;
    }
    return found;
}

float Builder::similarity(const Motif& a, const Motif& b) const {
    if (a.empty() || b.empty()) return 0.f;
    std::map<Tick, int> bm;
    for (auto& n : b) bm[n.start] = n.deg;
    int common = 0, samePitch = 0;
    for (auto& n : a) {
        auto it = bm.find(n.start);
        if (it != bm.end()) { ++common; if (it->second == n.deg) ++samePitch; }
    }
    const float uni = float(a.size() + b.size() - common);
    const float jacc = uni > 0 ? float(common) / uni : 0.f;
    const float pm = float(samePitch) / float(std::max(a.size(), b.size()));
    return 0.5f * jacc + 0.5f * pm;
}

float Builder::barRhythmSimilarity(const Motif& m) const {
    std::set<Tick> a, b;
    for (auto& n : m) (n.start < bar_ ? a : b).insert(n.start % bar_);
    if (a.empty() || b.empty()) return 0.f;
    int inter = 0;
    for (Tick t : a) inter += int(b.count(t));
    return float(inter) / float(a.size() + b.size() - inter);
}

// ---------------------------------------------------------------------
//  Der Kritiker: bewertet ein Motiv in seinem Umfeld (höher = besser)
// ---------------------------------------------------------------------
float Builder::scoreMotif(const Motif& m, const MelEnv& e) {
    ++evaluated_;
    const int n = int(m.size());
    if (n == 0) return -100.f;
    std::vector<int> p(n), pc(n), st(n);
    std::vector<char> ct(n);
    std::vector<const Chord*> ch(n);
    for (int i = 0; i < n; ++i) {
        ch[i] = &chordAt(*e.chords, e.uStart + m[i].start);
        p[i] = realize(m[i].deg, *ch[i]);
        pc[i] = mod12(p[i]);
        ct[i] = ch[i]->hasPc(pc[i]);
        st[i] = strengthAt(m[i].start);
    }
    float sc = 0;

    // 1. Harmonie: Akkordtöne auf Schwerpunkten, Fremdtöne richtig behandelt
    for (int i = 0; i < n; ++i) {
        const bool stepIn = i > 0 && p[i] != p[i - 1] && std::abs(p[i] - p[i - 1]) <= 2;
        const bool stepOut = i + 1 < n && p[i + 1] != p[i] && std::abs(p[i + 1] - p[i]) <= 2;
        if (ct[i]) sc += st[i] >= 2 ? 2.f : st[i] == 1 ? 0.8f : 0.3f;
        else {
            if (st[i] >= 2) {   // Vorhalt: muss schrittweise abwärts auflösen
                const bool resolves = i + 1 < n && ct[i + 1] && p[i + 1] < p[i] && p[i] - p[i + 1] <= 2;
                sc += resolves ? -0.6f + s_.creativity : -3.5f;
            } else if (st[i] == 1) sc += (stepIn && stepOut) ? 0.f : stepOut ? -0.6f : -1.6f;
            else sc += (stepIn && stepOut) ? 0.3f : (stepIn || stepOut) ? -0.5f : -2.f;   // Durchgang/Wechselnote
            if (m[i].len >= beat_) sc -= 1.f;
            for (int cp : ch[i]->pcs())
                if (mod12(pc[i] - cp) == 1 || (mod12(cp - pc[i]) == 1 && st[i] >= 1)) { sc -= 1.5f; break; }   // Reibung
        }
    }

    // 2. Intervalle und Sprungbehandlung
    int steps = 0, dirChanges = 0, lastDir = 0, sameRun = 0, maxSameRun = 0, repRun = 0;
    for (int i = 1; i < n; ++i) {
        const int iv = p[i] - p[i - 1], a = std::abs(iv);
        if (a == 0) { sc -= 0.25f; if (++repRun >= 2) sc -= 1.2f; }
        else repRun = 0;
        if (a >= 1 && a <= 2) { sc += 0.5f; ++steps; }
        else if (a <= 4) sc += 0.15f;
        else if (a == 5 || a == 7) sc -= 0.1f;
        else if (a == 6) sc -= 2.5f;                      // Tritonus
        else if (a == 8 || a == 9) sc -= 0.9f;
        else if (a == 10 || a == 11) sc -= 2.8f;          // Septime
        else if (a == 12) sc -= 0.6f;
        else sc -= 4.f;                                   // größer als Oktave
        if (a >= 5) {
            const bool arp = ct[i] && ct[i - 1] && ch[i] == ch[i - 1];   // Akkordbrechung ist ok
            if (i + 1 < n) {
                const int nx = p[i + 1] - p[i];
                if (sgn(nx) == -sgn(iv) && std::abs(nx) <= 2) sc += 1.f;   // Sprung ausgeglichen
                else if (sgn(nx) == sgn(iv)) sc -= arp ? 0.4f : 1.6f;
            }
            if (i >= 2 && sgn(p[i - 1] - p[i - 2]) == sgn(iv) && std::abs(p[i - 1] - p[i - 2]) >= 5) sc -= arp ? 0.5f : 2.f;
        }
        const int dir = sgn(iv);
        if (dir != 0) {
            if (lastDir != 0 && dir != lastDir) ++dirChanges;
            sameRun = (dir == lastDir) ? sameRun + 1 : 1;
            maxSameRun = std::max(maxSameRun, sameRun);
            lastDir = dir;
        }
    }
    if (n >= 3) {
        sc -= std::abs(float(steps) / float(n - 1) - (0.8f - s_.leaps * 0.3f)) * 6.f;   // Ziel-Anteil Schritte
        sc -= std::abs(float(dirChanges) / float(n - 1) - 0.42f) * 3.f;
    }
    if (maxSameRun > 5) sc -= float(maxSameRun - 5);

    // 3. Ambitus & ein klarer Höhepunkt
    const int mx = *std::max_element(p.begin(), p.end()), mn = *std::min_element(p.begin(), p.end());
    const int range = mx - mn;
    if (range < 5) sc -= float(5 - range) * 0.8f;
    if (range > 14) sc -= float(range - 14) * 1.2f;
    const int peaks = int(std::count(p.begin(), p.end(), mx));
    sc += peaks == 1 ? 1.5f : -0.6f * float(peaks - 1);
    const int firstPeak = int(std::find(p.begin(), p.end(), mx) - p.begin());
    const float peakPos = n > 1 ? float(firstPeak) / float(n - 1) : 0.f;
    if (peakPos > 0.3f && peakPos < 0.85f) sc += 0.8f;

    // 4. Tonale Strebetendenzen: Leitton nach oben auflösen
    for (int i = 0; i + 1 < n; ++i) {
        const int rel = mod12(p[i] - keyRoot_), rel2 = mod12(p[i + 1] - keyRoot_);
        if (rel == 11) sc += (rel2 == 0 && p[i + 1] > p[i]) ? 0.8f : (st[i] >= 1 ? -0.6f : 0.f);
    }

    // 5. Phrasenschluss
    if (e.role > 0) {
        sc += std::log(roleWeight(m.back().deg, e.role)) * 1.5f;
        if (!ct[n - 1]) sc -= 3.f;
        sc += m.back().len >= beat_ ? 1.f : -0.8f;
    }

    // 6. Rhythmus
    std::set<Tick> lens;
    int offbeats = 0;
    for (int i = 0; i < n; ++i) { lens.insert(m[i].len); if (st[i] == 0) ++offbeats; }
    sc += (lens.size() >= 2 && lens.size() <= 4) ? 0.6f : -0.4f;
    if (e.uLen >= 2 * bar_) {   // rhythmischer "Reim" zwischen den beiden Takten
        const float want = e.type == Sec::Chorus ? 0.75f : 0.55f;
        sc -= std::abs(barRhythmSimilarity(m) - want) * 2.5f;
    }
    sc -= std::abs(float(offbeats) / float(n) - (0.12f + s_.syncopation * 0.45f)) * 2.f;
    const double secs = song_.ticksToSeconds(double(m.back().start + m.back().len - m.front().start));
    if (secs > 0) {
        const double nps = n / secs;   // singbar bleiben
        if (nps > 6.5) sc -= float(nps - 6.5) * 1.5f;
    }

    // 7. Kontrapunkt mit dem Bass (Fux): keine Quint-/Oktavparallelen, Gegenbewegung bevorzugt
    if (e.bass) {
        int prevM = -1, prevB = -1;
        for (int i = 0; i < n; ++i) {
            if (st[i] < 1) continue;
            const int b = bassAt(e, e.uStart + m[i].start);
            if (b < 0) continue;
            const int ivl = mod12(p[i] - b);
            if (ivl == 1) sc -= 2.f;   // kleine None gegen den Bass
            if (prevM >= 0) {
                const int dm = p[i] - prevM, db = b - prevB;
                const int pIv = mod12(prevM - prevB);
                if (dm != 0 && db != 0 && sgn(dm) == sgn(db) && (ivl == 0 || ivl == 7) && ivl == pIv) sc -= 1.5f;
                if (dm != 0 && db != 0 && sgn(dm) == -sgn(db)) sc += 0.3f;
            }
            prevM = p[i];
            prevB = b;
        }
    }

    // 8. Verwandtschaft / Kontrast zu anderen Motiven
    for (auto& [ref, want] : e.simTargets) sc -= std::abs(similarity(m, *ref) - want) * 4.f;
    if (e.registerRef && !e.registerRef->empty()) {
        float a = 0, b = 0;
        for (auto& x : m) a += float(x.deg);
        for (auto& x : *e.registerRef) b += float(x.deg);
        a /= float(m.size());
        b /= float(e.registerRef->size());
        sc += std::clamp((a - b) - e.registerGap, -3.f, 0.5f);
        // Rhythmischer Kontrast (Notendichte) zwischen Refrain und Strophe
        const float da = float(m.size()), db = float(e.registerRef->size());
        sc += std::min(std::abs(da - db) / std::max(da, db), 0.4f) * 3.f;
    }

    // 9. Anschluss an die vorige Phrase
    if (e.prevPitch >= 0) {
        const int a = std::abs(p[0] - e.prevPitch);
        if (a == 0) sc += 0.1f;
        else if (a <= 2) sc += 0.6f;
        else if (a <= 4) sc += 0.3f;
        else if (a == 5 || a == 7) sc += 0.f;
        else if (a == 6) sc -= 2.f;
        else sc -= float(a - 7) * 0.5f + 0.5f;
    }

    // 10. Lage
    float mean = 0;
    for (int x : p) mean += float(x);
    mean /= float(n);
    sc -= std::abs(mean - float(mel_.pitch(e.center))) * 0.15f;
    return sc;
}

// Viele Kandidaten erzeugen, bewerten, die besten verfeinern (Simulated Annealing)
Motif Builder::composeMotif(const MelEnv& e, const Motif* related) {
    const int nCand = 100 + effort() * 120;
    const int nTop = 3 + effort();
    const int nClimb = 100 + effort() * 120;

    std::vector<std::pair<float, Motif>> pool;
    pool.reserve(nCand);
    for (int k = 0; k < nCand; ++k) {
        Motif c = (related && chance(r_, 0.5f)) ? derivedMotif(*related, e, irand(r_, 1, 3)) : randomMotif(e);
        pool.push_back({scoreMotif(c, e), std::move(c)});
    }
    std::sort(pool.begin(), pool.end(), [](auto& a, auto& b) { return a.first > b.first; });

    Motif best = pool.front().second;
    float bestSc = pool.front().first;
    for (int t = 0; t < std::min(nTop, int(pool.size())); ++t) {
        Motif cur = pool[t].second;
        float curSc = pool[t].first;
        float T = 1.f;
        for (int it = 0; it < nClimb; ++it) {
            Motif c = mutate(cur, e);
            float cs = scoreMotif(c, e);
            if (cs > curSc || chance(r_, std::exp((cs - curSc) / std::max(T, 0.01f)))) {
                cur = std::move(c);
                curSc = cs;
                if (curSc > bestSc) { bestSc = curSc; best = cur; }
            }
            T *= std::pow(0.01f, 1.f / float(nClimb));   // Temperatur sinkt auf 1 %
        }
    }
    scoreSum_ += bestSc;
    ++scoreCount_;
    return best;
}

// Wiederholung mit Variation: soll gut klingen UND wiedererkennbar bleiben
Motif Builder::varyMotif(const Motif& src, const MelEnv& e, float variation) {
    Motif base = snapToChords(fitToUnit(src, e.uLen), e);
    if (base.empty()) return base;
    const float targetSim = 1.f - variation * 0.6f;
    auto eval = [&](const Motif& c) { return scoreMotif(c, e) - std::abs(similarity(c, src) - targetSim) * 8.f; };
    Motif best = base;
    float bestSc = eval(base);
    const int n = 60 + effort() * 60;
    for (int k = 0; k < n; ++k) {
        Motif c = base;
        const int ops = 1 + irand(r_, 0, int(variation * 5.f));
        for (int o = 0; o < ops; ++o) c = mutate(c, e);
        if (variation > 0.2f && chance(r_, variation * 0.3f)) {   // Sequenz: ganze Phrase versetzen
            int d = chance(r_, 0.5f) ? 1 : -1;
            for (auto& x : c) x.deg = std::clamp(x.deg + d, e.lo, e.hi);
            c = snapToChords(c, e);
        }
        float cs = eval(c);
        if (cs > bestSc) { bestSc = cs; best = std::move(c); }
    }
    return best;
}

void Builder::genMelody(Material& m, const SecDef& sd, const std::vector<TChord>& tc, Tick secStart, bool lastSec,
                        float vel, const std::vector<Note>* bass, std::vector<Note>& out) {
    // Melodie-System dieses Abschnitts (gilt nur während dieser Melodie, danach zurücksetzen)
    struct MelSysGuard {
        Builder& b;
        ~MelSysGuard() { b.melCtBias_ = 1.f; b.melLeapMul_ = 1.f; b.melPenta_ = false; }
    } melGuard{*this};
    switch (m.melSys) {
    case MelSys::PentaHook: melPenta_ = true; melCtBias_ = 1.2f; break;
    case MelSys::Arpeggio: melCtBias_ = 2.0f; melLeapMul_ = 1.8f; break;   // Thema aus Akkordtönen, mit Sprüngen
    case MelSys::Chorale: melCtBias_ = 1.3f; melLeapMul_ = 0.4f; break;    // ruhige Schritte
    case MelSys::Riff: melCtBias_ = 1.2f; break;
    default: break;
    }
    // Eigene Idee beim ersten Mal unverändert übernehmen – sie ist der Kern des Songs.
    // Ist der Abschnitt länger als die Idee, geht es danach mit Variationen ihrer Motive weiter.
    int firstUnit = 0;
    std::map<char, int> used;
    if (m.hasIdea && !m.ideaHarmonic && m.occurrences == 0) {
        for (Note n : m.ideaNotes) {
            if (n.start >= Tick(sd.bars) * bar_) continue;
            n.start += secStart;
            out.push_back(n);
        }
        if (!m.ideaNotes.empty()) lastMelDeg_ = mel_.nearestDegree(m.ideaNotes.back().pitch);
        firstUnit = (m.ideaBars + 1) / 2;
        for (int u = 0; u < firstUnit; ++u) used[m.form[size_t(u) % m.form.size()]]++;
        if (Tick(firstUnit) * 2 * bar_ >= Tick(sd.bars) * bar_) return;
    }
    Material& owner = m.ownMotifs ? m : mats_[m.motifOwner];
    const int center = mel_.nearestDegree(baseCenterPitch_ + 12 * s_.melodyOctaveShift) + m.centerOffset;
    const double amb = 0.55 + double(s_.ambitus) * 0.9;   // Tonumfang (0.5 = wie bisher)
    const int lo = center - int(std::lround(mel_.size() * 0.8 * amb)), hi = center + int(std::lround(mel_.size() * 0.9 * amb));
    const Tick unit = 2 * bar_, secLen = Tick(sd.bars) * bar_;
    const int U = int((secLen + unit - 1) / unit);

    for (int u = firstUnit; u < U; ++u) {
        MelEnv e;
        e.chords = &tc;
        e.uStart = u * unit;
        e.uLen = std::min(unit, secLen - e.uStart);
        e.secStart = secStart;
        e.lo = lo; e.hi = hi; e.center = center;
        e.contour = m.contour;
        e.type = m.type;
        e.density = m.density;
        e.bass = bass;
        if (!out.empty()) e.prevPitch = out.back().pitch;
        else if (lastMelDeg_ != INT_MIN) e.prevPitch = mel_.pitch(lastMelDeg_);

        // Kadenz-Rolle: Vordersatz offen, Nachsatz geschlossen
        if (u == U - 1) {
            if (lastSec) e.role = 1;
            else e.role = (m.type == Sec::Chorus || m.type == Sec::Outro) ? 2 : 1;
        } else if (u % 2 == 1) e.role = (u % 4 == 3) ? 2 : 1;

        const char L = m.form[u % m.form.size()];
        Motif result;
        auto it = owner.motifs.find(L);
        if (it == owner.motifs.end()) {
            const Motif* related = nullptr;
            if (L != 'A' && owner.motifs.count('A')) {
                related = &owner.motifs['A'];
                e.simTargets.push_back({related, 0.35f});
            }
            auto motifA = [&](Sec t) -> const Motif* {
                auto mi = mats_.find(t);
                if (mi == mats_.end()) return nullptr;
                auto ai = mi->second.motifs.find('A');
                return ai == mi->second.motifs.end() ? nullptr : &ai->second;
            };
            if (L == 'A') {
                const Sec type = owner.type;
                if (type == Sec::Chorus) {
                    if (auto v = motifA(Sec::Verse)) { e.simTargets.push_back({v, 0.15f}); e.registerRef = v; e.registerGap = 2.f; }
                } else if (type == Sec::Pre) {
                    if (auto v = motifA(Sec::Verse)) { e.simTargets.push_back({v, 0.2f}); e.registerRef = v; e.registerGap = 1.f; }
                } else if (type == Sec::Bridge) {
                    if (auto v = motifA(Sec::Verse)) e.simTargets.push_back({v, 0.1f});
                    if (auto c = motifA(Sec::Chorus)) e.simTargets.push_back({c, 0.1f});
                }
            }
            result = composeMotif(e, related);
            owner.motifs[L] = result;
        } else {
            float variation;
            if (used[L] > 0) variation = 0.12f + s_.creativity * 0.3f;
            else if (m.occurrences > 0) variation = m.type == Sec::Chorus ? s_.creativity * 0.12f : 0.15f + s_.creativity * 0.35f;
            else variation = 0.05f + s_.creativity * 0.15f;
            if (u >= 4) variation += 0.08f;
            variation *= 1.6f - s_.repetition * 1.2f;   // Wiedererkennung (0.5 = wie bisher)
            variation *= m.variationMul;                 // Riff: kehrt fast unverändert wieder
            result = varyMotif(it->second, e, variation);
        }
        used[L]++;

        for (auto& n : result) {
            const Chord& c = chordAt(tc, e.uStart + n.start);
            const int p = realize(n.deg, c);
            float v = vel + 0.06f + (strengthAt(n.start) >= 2 ? 0.05f : 0.f) + float(p - mel_.pitch(center)) * 0.004f;
            const Tick len = std::max<Tick>(six_ / 2, n.len - six_ / 4);
            out.push_back({p, secStart + e.uStart + n.start, len, std::clamp(v, 0.05f, 1.f)});
            lastMelDeg_ = n.deg;
        }
    }
}

// ---------------------------------------------------------------------
//  Instrumental-Solo (wie ein Improvisator): Phrasen à 2 Takte mit Atempausen, Steigerung zum
//  Höhepunkt, Akkordtöne auf den Zählzeiten, Durchgangs- und chromatische Annäherungstöne dazwischen.
// ---------------------------------------------------------------------
void Builder::genSolo(const std::vector<TChord>& tc, Tick secStart, int bars, float vel, std::vector<Note>& out) {
    if (tc.empty()) return;
    const Tick secLen = Tick(bars) * bar_, phraseLen = 2 * bar_;
    const int sz = mel_.size();
    const int center = mel_.nearestDegree(baseCenterPitch_ + 12 * s_.melodyOctaveShift) + sz / 3;   // etwas über der Gesangslage
    const int lo = center - sz, hi = center + sz + sz / 2;
    const int phrases = std::max(1, int(secLen / phraseLen));
    // Rhythmus-Zellen pro Schlag in 16teln: (Beginn, Länge)
    using Cell = std::vector<std::pair<int, int>>;
    static const Cell cells[] = {
        {{0, 2}, {2, 2}},                   // zwei Achtel
        {{0, 1}, {1, 1}, {2, 1}, {3, 1}},   // vier 16tel
        {{0, 4}},                           // Viertel
        {{0, 3}, {3, 1}},                   // punktiert
        {{2, 2}},                           // Pause + Achtel (Off-Beat)
        {{0, 1}, {1, 1}, {2, 2}},           // 16-16-8
        {{0, 2}, {2, 1}, {3, 1}},           // 8-16-16
    };
    int deg = center;
    int dir = 1;
    for (int ph = 0; ph < phrases; ++ph) {
        const float build = phrases > 1 ? float(ph) / float(phrases - 1) : 1.f;   // 0 … 1: Steigerung
        const bool last = ph == phrases - 1;
        const Tick p0 = Tick(ph) * phraseLen;
        const Tick pEnd = std::min(secLen, p0 + phraseLen - (last ? beat_ : beat_ * irand(r_, 1, 2)));
        const int goal = std::clamp(center + int(std::lround((build * 1.2f - 0.1f) * float(sz))) + irand(r_, -1, 2), lo + 1, hi);
        // Rhythmus der Phrase
        std::vector<std::pair<Tick, Tick>> rh;
        const float busy = std::clamp(0.25f + build * 0.45f + (s_.melodyDensity - 0.5f) * 0.4f, 0.1f, 0.95f);
        for (Tick b = p0; b + beat_ <= pEnd; b += beat_) {
            std::vector<float> w = {1.2f, 0.4f + busy * 2.2f, 1.2f - busy, 0.6f, 0.5f + s_.syncopation, 0.3f + busy, 0.3f + busy};
            if (b == p0 && ph > 0 && chance(r_, 0.4f)) w = {0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f};   // Auftakt nach Pause
            const Cell& c = cells[pickWeighted(r_, w)];
            for (auto& [st, ln] : c) rh.push_back({b + Tick(st) * six_, Tick(ln) * six_});
        }
        if (rh.empty()) continue;
        // Letzter Ton der Phrase klingt bis zum Phrasenende
        rh.back().second = std::max(rh.back().second, pEnd - rh.back().first);
        // Tonhöhen: Schrittbewegung zum Ziel, gelegentlich Sprünge, Akkordtöne auf den Zählzeiten
        std::vector<int> degs;
        for (size_t i = 0; i < rh.size(); ++i) {
            const Tick t = rh[i].first;
            const Chord& ch = chordAt(tc, t);
            if (deg < goal - 1) dir = 1;
            else if (deg > goal + 1) dir = -1;
            else if (chance(r_, 0.3f)) dir = -dir;
            int step = dir;
            if (chance(r_, 0.12f + s_.leaps * 0.25f)) step = dir * irand(r_, 2, 3);   // Sprung
            else if (chance(r_, 0.1f)) step = 0;                                      // Tonwiederholung
            int d = std::clamp(deg + step, lo, hi);
            const bool onBeat = (t % beat_) == 0;
            const bool lastNote = i + 1 == rh.size();
            if ((onBeat || lastNote || rh[i].second >= beat_) && !isCT(d, ch)) {
                // auf den nächsten Akkordton in Bewegungsrichtung (sonst Gegenrichtung)
                int best = d;
                for (int k = 1; k <= 3; ++k) {
                    if (isCT(d + dir * k, ch) && d + dir * k >= lo && d + dir * k <= hi) { best = d + dir * k; break; }
                    if (isCT(d - dir * k, ch) && d - dir * k >= lo && d - dir * k <= hi) { best = d - dir * k; break; }
                }
                d = best;
            }
            degs.push_back(d);
            deg = d;
        }
        // Ausgeben + chromatische Annäherung von unten an Zählzeit-Töne
        for (size_t i = 0; i < rh.size(); ++i) {
            const Chord& ch = chordAt(tc, rh[i].first);
            int pitch = realize(degs[i], ch);
            if (i + 1 < rh.size() && rh[i].second <= six_ && (rh[i + 1].first % beat_) == 0 &&
                chance(r_, 0.15f + s_.creativity * 0.35f)) {
                const int target = realize(degs[i + 1], chordAt(tc, rh[i + 1].first));
                if (target - 1 != pitch) pitch = target - 1;
            }
            const bool onBeat = (rh[i].first % beat_) == 0;
            float v = vel * 0.92f + (onBeat ? 0.06f : 0.f) + build * 0.08f + float(degs[i] - center) * 0.006f;
            out.push_back({pitch, secStart + rh[i].first, std::max<Tick>(six_ / 2, rh[i].second - six_ / 8),
                           std::clamp(v, 0.2f, 1.f)});
        }
    }
}

// ===================== Zusammenbau =====================
void Builder::addNotes(Track& t, const std::vector<Note>& notes, int transpose) {
    for (Note n : notes) {
        n.pitch = std::clamp(n.pitch + transpose, 21, 108);
        t.notes.push_back(n);
    }
}

Song Builder::build() {
    song_ = Song{};
    song_.bpm = rs_.bpm;
    song_.beatsPerBar = rs_.bpb;
    bpb_ = rs_.bpb;
    bar_ = song_.ticksPerBar();
    beat_ = song_.ticksPerBeat();
    six_ = song_.ppq / 4;

    Track melT{"Melodie", s_.melodyInstrument};
    Track chordT{"Begleitung", s_.chordInstrument};
    Track bassT{"Bass", s_.bassInstrument};
    Track drumT{"Schlagzeug", s_.drumInstrument};
    Track soloT{"Solo", s_.soloInstrument};
    Track voice2T{"Zweite Stimme", s_.melodyInstrument};
    Track stringT{"Streicher-Linie", "Streicher"};
    voice2T.volume = 0.5f;
    voice2T.pan = -0.3f;
    stringT.volume = 0.42f;
    stringT.pan = 0.3f;
    soloT.volume = 0.8f;
    chordT.volume = 0.55f;
    bassT.volume = 0.8f;
    drumT.volume = 0.75f;
    if (auto inst = InstrumentRegistry::instance().create(s_.chordInstrument)) chordSustains_ = inst->sustains();

    const bool orch = s_.ensemble == Ensemble::Orchestra;
    epic_ = orch ? std::string(orchProfiles()[size_t(rs_.orchStyle)].name) == "Episch (Film)"
                 : (s_.withDrums && rs_.drumStyle == DrumStyle::Epic);
    if (orch) {
        static const char* inst[OCOUNT] = {"Flöte", "Oboe", "Klarinette", "Fagott", "Hörner", "Trompeten", "Posaunen",
                                           "Pauken", "Schlagzeug", "Taiko", "Glocken", "Harfe", "Chor", "Violinen", "Violinen",
                                           "Bratschen", "Celli", "Kontrabässe", "Pizzicato", "Tuba", "Piccolo", "Cembalo"};
        static const float vol[OCOUNT] = {0.7f, 0.7f, 0.7f, 0.6f, 0.6f, 0.55f, 0.55f, 0.8f, 0.6f, 0.75f,
                                          0.45f, 0.65f, 0.6f, 0.85f, 0.6f, 0.55f, 0.7f, 0.7f, 0.65f, 0.6f, 0.45f, 0.55f};
        chordSustains_ = true;
        orch_.clear();
        for (int i = 0; i < OCOUNT; ++i) {
            orch_.push_back({orchInstName(i), inst[i]});
            orch_.back().volume = vol[i];
        }
    }
    // Lage der eigenen Melodie bestimmt die Lage des ganzen Songs
    if (rs_.hasIdea && !s_.idea->harmonic && !s_.idea->notes.empty()) {
        float mean = 0;
        for (auto& n : s_.idea->notes) mean += float(n.pitch);
        mean /= float(s_.idea->notes.size());
        baseCenterPitch_ = int(std::lround(mean)) - (rs_.ideaType == Sec::Chorus ? 4 : 0) - 12 * s_.melodyOctaveShift;
    }

    makeMaterials();

    const auto& st = rs_.structure;
    std::map<Sec, int> counter;
    Tick cursor = 0;
    for (size_t k = 0; k < st.size(); ++k) {
        const SecDef& sd = st[k];
        Material& m = mats_[sd.type];
        const bool lastSec = k + 1 == st.size();
        const int transpose = (rs_.keyChangeAt >= 0 && int(k) >= rs_.keyChangeAt) ? rs_.keyChangeSemis : 0;

        auto tc = tile(m, sd.bars);
        // Kadenzen an den Abschnittsgrenzen
        const bool nextChorus = k + 1 < st.size() && st[k + 1].type == Sec::Chorus;
        if (nextChorus && sd.type != Sec::Chorus && sd.type != Sec::Pre && chance(r_, 0.6f)) tc.back().chord = dominant();
        if (lastSec) tc.back().chord = chance(r_, 0.75f) ? dominant() : diatonicChord(harm_, 3, false);
        if (sd.type == Sec::Verse && m.form == "ABAC" && tc.size() == 8 && chance(r_, 0.35f)) tc[3].chord = dominant();

        bool lastChorus = true;
        for (size_t j = k + 1; j < st.size(); ++j) if (st[j].type == Sec::Chorus) lastChorus = false;
        float e = secEnergy(k) + ((sd.type == Sec::Chorus && lastChorus) ? 0.06f : 0.f);
        const float vel = 0.42f + e * 0.45f;

        assignBassLine(tc);
        if (m.pedal) {   // Orgelpunkt: Bass bleibt auf dem Grundton (gleiche Lage)
            int pb = 36 + mod12(keyRoot_);
            if (pb > 45) pb -= 12;
            for (auto& c : tc) c.bass = pb;
        }
        for (auto& c : tc)   // Akkordverlauf merken (für den Bereinigungsdurchgang am Ende)
            chordLine_.push_back({makeChord(c.chord.root + transpose, c.chord.intervals), cursor + c.start, c.len, c.bass + transpose});
        // Breakdown: abgespeckt (Flächen statt Rhythmus, ruhiger Bass, kein Schlagzeug) – danach zurücksetzen
        const AccompStyle accSave = m.acc;
        const BassStyle bassSave = m.bass;
        const bool drumsSave = m.drumsOn;
        const OrchPlan orchSave = m.orch;
        if (sd.stripped) {
            m.acc = AccompStyle::Pad;
            m.bass = BassStyle::Whole;
            m.drumsOn = false;
            m.orch.tex = Tex::Pad;
            m.orch.perc = PercT::None;
            m.orch.braams = m.orch.taiko = m.orch.counter = m.orch.sparkle = false;
        }
        std::vector<Note> bassNotes, melNotes, accNotes;
        genBass(m, tc, cursor, vel * 0.95f, bassNotes);
        if (sd.solo) {
            // Instrumental-Solo statt Gesangsmelodie
            if (s_.withMelody) genSolo(tc, cursor, sd.bars, vel, melNotes);
        } else if (s_.withMelody && m.melody)
            genMelody(m, sd, tc, cursor, lastSec, vel, s_.withBass ? &bassNotes : nullptr, melNotes);
        if (s_.withChords) genAccomp(m, tc, cursor, vel * 0.9f, accNotes);

        if (orch) {
            orchestrate(m, sd, k, tc, cursor, transpose, vel, melNotes, accNotes,
                        s_.withBass ? bassNotes : std::vector<Note>{});
        } else {
            if (s_.withBass) addNotes(bassT, bassNotes, transpose);
            addNotes(sd.solo ? soloT : melT, melNotes, transpose);
            // Ab dem 2. Refrain: zweite Stimme (Terz/Sexte unter der Melodie, wie ein Backgroundchor)
            if (sd.type == Sec::Chorus && !sd.stripped && s_.withMelody && counter[Sec::Chorus] >= 1) {
                std::vector<Note> harm;
                for (const Note& n : melNotes) {
                    if (n.length < 2 * six_) continue;
                    const Chord& ch = chordAt(tc, n.start - cursor);
                    for (int d : {3, 4, 8, 9}) {
                        const int q = n.pitch - d;
                        if (q >= 50 && ch.hasPc(q)) { harm.push_back({q, n.start, n.length, n.velocity * 0.72f}); break; }
                    }
                }
                addNotes(voice2T, harm, transpose);
            }
            // Letzter Refrain: warme Streicher-Gegenlinie aus Leittönen (Terz/Septime), wenn die Begleitung nicht ohnehin hält
            if (sd.type == Sec::Chorus && lastChorus && !sd.stripped && !chordSustains_ && counter[Sec::Chorus] >= 1) {
                std::vector<Note> line;
                int prev = 66;
                for (const TChord& c : tc) {
                    std::vector<int> pcs = {c.chord.root + (c.chord.intervals.size() > 1 ? c.chord.intervals[1] : 4)};
                    if (c.chord.intervals.size() > 3) pcs.push_back(c.chord.root + c.chord.intervals[3]);
                    int best = -1;
                    for (int p = 60; p <= 74; ++p)
                        for (int pc : pcs)
                            if (mod12(p - pc) == 0 && (best < 0 || std::abs(p - prev) < std::abs(best - prev))) best = p;
                    if (best < 0) continue;
                    line.push_back({best, cursor + c.start, c.len - six_ / 2, vel * 0.55f});
                    prev = best;
                }
                addNotes(stringT, line, transpose);
            }
            addNotes(chordT, accNotes, transpose);
            if (s_.withDrums) {
                std::vector<Note> drumNotes;
                genDrums(m, sd, k, cursor, vel, drumNotes);
                addNotes(drumT, drumNotes, 0);   // Schlagzeug wird nie transponiert
            }
        }

        m.acc = accSave;
        m.bass = bassSave;
        m.drumsOn = drumsSave;
        m.orch = orchSave;

        if (sd.solo) {   // Solo-Abschnitt: eigener Name, zählt nicht als Strophe
            song_.sections.push_back({"Solo", cursor, Tick(sd.bars) * bar_});
            cursor += Tick(sd.bars) * bar_;
            if (progress_) *progress_ = progBase_ + progSpan_ * float(k + 1) / float(st.size() + 1);
            continue;
        }
        std::string name;
        if (sd.type == Sec::Intro && k > 0) {
            name = "Zwischenspiel";   // Intro-Material mitten im Song
        } else {
            int num = ++counter[sd.type];
            int total = 0;
            for (size_t j = 0; j < st.size(); ++j)
                if (st[j].type == sd.type && !st[j].solo && !(st[j].type == Sec::Intro && j > 0)) ++total;
            name = secName(sd.type);
            if (total > 1) name += " " + std::to_string(num);
        }
        if (sd.stripped) name += " (Breakdown)";
        song_.sections.push_back({name, cursor, Tick(sd.bars) * bar_});

        m.occurrences++;
        cursor += Tick(sd.bars) * bar_;
        if (progress_) *progress_ = progBase_ + progSpan_ * float(k + 1) / float(st.size() + 1);
    }

    // Schlusstakt: Tonika
    {
        const int transpose = rs_.keyChangeAt >= 0 ? rs_.keyChangeSemis : 0;
        Chord t = tonic();
        if (minorKey_ && chance(r_, s_.creativity * 0.3f)) t = makeChord(keyRoot_, {0, 4, 7}, "I");   // picardische Terz
        const Tick len = 2 * bar_;
        chordLine_.push_back({makeChord(t.root + transpose, t.intervals), cursor, len, -1});
        std::vector<Note> fin;
        if (orch) {   // Tutti-Schlussakkord
            for (int p : voice(t)) {
                orch_[p >= 62 ? V2 : VA].notes.push_back({p + transpose, cursor, len, 0.6f});
                if (p >= 53 && p <= 70) orch_[HN].notes.push_back({p + transpose, cursor, len, 0.5f});
            }
            int b = 36 + mod12(t.root);
            if (b > 45) b -= 12;
            orch_[KB].notes.push_back({b + transpose, cursor, len, 0.65f});
            orch_[VC].notes.push_back({b + 12 + transpose, cursor, len, 0.6f});
            orch_[TB].notes.push_back({b + 12 + transpose, cursor, len, 0.5f});
            int tp = 41;
            while (mod12(tp) != mod12(t.root + transpose)) ++tp;
            orch_[TI].notes.push_back({tp, cursor, beat_, bigFinale_ ? 1.f : 0.9f});
            if (bigFinale_) {   // der letzte große Schlag: große Trommel + Becken mit der Pauke
                orch_[PC].notes.push_back({35, cursor, six_, 1.f});
                orch_[PC].notes.push_back({49, cursor, six_, 0.9f});
            }
            if (s_.withMelody && lastMelDeg_ != INT_MIN) {
                int best = lastMelDeg_, bestD = 99;
                for (int d = lastMelDeg_ - 4; d <= lastMelDeg_ + 4; ++d)
                    if (mod12(mel_.pitch(d) - keyRoot_) == 0 && std::abs(d - lastMelDeg_) < bestD) { bestD = std::abs(d - lastMelDeg_); best = d; }
                orch_[V1].notes.push_back({mel_.pitch(best) + transpose, cursor, len, 0.65f});
                orch_[FL].notes.push_back({mel_.pitch(best) + 12 + transpose, cursor, len, 0.5f});
            }
        } else if (s_.withChords)
            for (int p : voice(t)) fin.push_back({p + transpose, cursor, len, 0.55f});
        if (s_.withBass && !orch) {
            int b = 36 + mod12(t.root);
            if (b > 45) b -= 12;
            bassT.notes.push_back({b + transpose, cursor, len, 0.6f});
        }
        if (s_.withMelody && !orch && lastMelDeg_ != INT_MIN) {
            int best = lastMelDeg_, bestD = 99;
            for (int d = lastMelDeg_ - 4; d <= lastMelDeg_ + 4; ++d)
                if (mod12(mel_.pitch(d) - keyRoot_) == 0 && std::abs(d - lastMelDeg_) < bestD) { bestD = std::abs(d - lastMelDeg_); best = d; }
            melT.notes.push_back({mel_.pitch(best) + transpose, cursor, len, 0.6f});
        }
        addNotes(chordT, fin, 0);
        if (s_.withDrums && !orch) {   // Schlussschlag
            drumT.notes.push_back({49, cursor, six_, 0.85f});
            drumT.notes.push_back({36, cursor, six_, 0.9f});
        }
        if (!song_.sections.empty()) song_.sections.back().length += bar_;
    }

    if (orch) {
        song_.tracks.clear();
        for (auto& t : orch_)   // nur Stimmen, die wirklich mitspielen (nicht nur im Schlussakkord)
            if (t.notes.size() >= 4) song_.tracks.push_back(t);
        if (song_.tracks.empty()) song_.tracks.push_back(orch_[V1]);
    } else {
        song_.tracks = {melT, chordT, bassT};
        if (!soloT.notes.empty()) song_.tracks.insert(song_.tracks.begin() + 1, soloT);
        if (!voice2T.notes.empty()) song_.tracks.push_back(voice2T);
        if (!stringT.notes.empty()) song_.tracks.push_back(stringT);
        if (s_.withDrums) song_.tracks.push_back(drumT);
        if (!s_.withMelody) song_.tracks[0].notes.clear();
    }
    resolveClashes();
    applyDynamics();
    finish();
    return song_;
}

// ---------------------------------------------------------------------
//  Bereinigungsdurchgang ("Korrekturlesen" des Arrangements)
//  Melodie und Bass stehen fest. Jede andere längere Note, die im Halbtonabstand (kleine Sekunde,
//  große Septime, kleine None) an etwas gleichzeitig Klingendem reibt, rückt auf den nächsten
//  Akkordton, der nirgends reibt – oder pausiert. Kurze Durchgangstöne bleiben unangetastet.
// ---------------------------------------------------------------------
void Builder::resolveClashes() {
    if (chordLine_.empty() || song_.sections.empty()) return;
    const bool orch = s_.ensemble == Ensemble::Orchestra;
    const auto& st = rs_.structure;
    const Tick minLen = beat_ / 2;   // ab einer Achtel zählt ein Ton als "liegend"
    auto secOf = [&](Tick t) {
        size_t k = 0;
        while (k + 1 < song_.sections.size() && t >= song_.sections[k + 1].start) ++k;
        return std::min(k, st.size() - 1);
    };
    auto chordAtAbs = [&](Tick t) -> const Chord& {
        for (auto it = chordLine_.rbegin(); it != chordLine_.rend(); ++it)
            if (t >= it->start) return it->chord;
        return chordLine_.front().chord;
    };
    auto orchIdx = [&](const std::string& name) {
        for (int i = 0; i < OCOUNT; ++i) if (name == orchInstName(i)) return i;
        return -1;
    };

    // Rang je Note: 0 = Melodie, 1 = Bass (beide fest), 2 = Melodie-Partner, 3 = Begleitung
    struct Item { size_t tr, idx; int rank; };
    std::vector<Item> items;
    for (size_t ti = 0; ti < song_.tracks.size(); ++ti) {
        const Track& tr = song_.tracks[ti];
        auto inst = InstrumentRegistry::instance().create(tr.instrument);
        if (inst && inst->isDrumKit()) continue;   // Schlagzeug/Taiko haben keine Tonhöhe
        const int oi = orch ? orchIdx(tr.name) : -1;
        for (size_t i = 0; i < tr.notes.size(); ++i) {
            const size_t k = secOf(tr.notes[i].start);
            const Material& mat = mats_[st[k].type];
            int rank = 3;
            if (orch) {
                const auto key = std::make_tuple(tr.name, tr.notes[i].start, tr.notes[i].pitch);
                rank = partnerKeys_.count(key) ? 2 : melodyKeys_.count(key) ? 0 : bassKeys_.count(key) ? 1 : 3;
                (void)mat;
                (void)oi;
            } else {
                rank = (tr.name == "Melodie" || tr.name == "Solo") ? 0 : tr.name == "Bass" ? 1 : 3;
                if (rs_.hasIdea && s_.idea->harmonic && st[k].type == rs_.ideaType && tr.name == "Begleitung") rank = 0;   // eigene Akkorde bleiben
            }
            items.push_back({ti, i, rank});
        }
    }
    // Melodie-Ton, der in einen neuen Basston hineinklingt und dort reibt (z.B. Leitton der Zwischendominante
    // über dem nächsten Akkord): am Basswechsel enden lassen. Beide sind "fest", darum vorab.
    {
        // Akkordfremder Bass-Durchgangston (z.B. Walking-Bass-Annäherung), der an der Melodie reibt:
        // auf den nächsten Akkordton ohne Reibung setzen
        std::vector<const Note*> melNotes;
        for (const auto& it : items)
            if (it.rank == 0) melNotes.push_back(&song_.tracks[it.tr].notes[it.idx]);
        auto firstRub = [&](int pitch, Tick s, Tick e) {
            Tick first = -1;
            for (const Note* m : melNotes) {
                if (std::min(e, m->end()) - std::max(s, m->start) < six_) continue;
                const int ic = mod12(pitch - m->pitch);
                if (ic != 1 && ic != 11) continue;
                const Tick at = std::max(s, m->start);
                if (first < 0 || at < first) first = at;
            }
            return first;
        };
        auto rubsMelody = [&](int pitch, Tick s, Tick e) { return firstRub(pitch, s, e) >= 0; };
        // Wie lange reibt ein akkordfremder Melodieton insgesamt am Bass? (Akkordtöne wie die große Septime
        // eines maj7-Akkords sind gewollte Farbe und zählen nicht)
        auto rubLength = [&](int pitch, Tick s, Tick e) {
            Tick sum = 0;
            for (const Note* m : melNotes) {
                const Tick ov = std::min(e, m->end()) - std::max(s, m->start);
                const int ic = mod12(pitch - m->pitch);
                if (ov <= 0 || (ic != 1 && ic != 11)) continue;
                // gewollte Farbe nur: Bass auf dem Grundton, Melodie auf einem Akkordton (z.B. maj7 oben)
                const Chord& c = chordAtAbs(m->start);
                const bool colour = c.hasPc(m->pitch) && mod12(pitch - c.root) == 0;
                if (!colour && ov >= six_) sum = std::max(sum, m->length);   // zählt: wie lang der reibende Melodieton ist
            }
            return sum;
        };
        for (const auto& it : items) {
            if (it.rank != 1) continue;
            Note& b = song_.tracks[it.tr].notes[it.idx];
            if (b.length < minLen) continue;
            const Tick fr = firstRub(b.pitch, b.start, b.end());
            if (fr < 0) continue;
            // Gehaltener Basston klingt in einen Akkord hinein, zu dem er nicht passt: dort enden
            if (fr - b.start >= minLen && !chordAtAbs(fr).hasPc(b.pitch)) {
                b.length = fr - b.start;
                continue;
            }
            const Chord& ch = chordAtAbs(b.start);
            // Akkordton im Bass: ein kurzer Vorhalt der Melodie darf reiben, eine lange Reibung (ab 1 Schlag)
            // löst der Bass durch eine Umkehrung (anderer Akkordton) auf
            if (ch.hasPc(b.pitch) && rubLength(b.pitch, b.start, b.end()) < beat_ * 5 / 8) continue;
            for (int d = 1; d <= 7; ++d) {
                int q = -1;
                for (int sg : {-1, 1})
                    if (q < 0 && ch.hasPc(b.pitch + sg * d) && !rubsMelody(b.pitch + sg * d, b.start, b.end())) q = b.pitch + sg * d;
                if (q >= 0) { b.pitch = q; break; }
            }
        }
        std::vector<const Note*> bassNotes;
        for (const auto& it : items)
            if (it.rank == 1) bassNotes.push_back(&song_.tracks[it.tr].notes[it.idx]);
        for (const auto& it : items) {
            if (it.rank != 0) continue;
            Note& n = song_.tracks[it.tr].notes[it.idx];
            Tick cut = n.end();
            for (const Note* b : bassNotes) {
                if (b->start <= n.start + minLen || b->start >= cut - six_) continue;
                if (std::min(n.end(), b->end()) - b->start < six_) continue;
                const int ic = mod12(n.pitch - b->pitch);
                if (ic == 1 || ic == 11) cut = b->start;
            }
            if (cut < n.end()) n.length = std::max<Tick>(six_, cut - n.start);
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.rank < b.rank; });

    struct Placed { Tick s, e; int pitch; };
    std::vector<Placed> placed;
    placed.reserve(items.size());
    // Frühester Zeitpunkt, ab dem dieser Ton an etwas Festem reibt (Überlappung ab einer Sechzehntel), sonst -1
    auto firstClash = [&](int pitch, Tick s, Tick e) {
        Tick first = -1;
        for (const auto& p : placed) {
            if (std::min(e, p.e) - std::max(s, p.s) < six_) continue;
            const int ic = mod12(pitch - p.pitch);
            if (ic != 1 && ic != 11) continue;
            const Tick at = std::max(s, p.s);
            if (first < 0 || at < first) first = at;
        }
        return first;
    };
    auto clashes = [&](int pitch, Tick s, Tick e) { return firstClash(pitch, s, e) >= 0; };
    std::vector<std::vector<char>> del(song_.tracks.size());
    for (size_t ti = 0; ti < song_.tracks.size(); ++ti) del[ti].assign(song_.tracks[ti].notes.size(), 0);
    for (const auto& it : items) {
        Note& n = song_.tracks[it.tr].notes[it.idx];
        const Tick fc = (it.rank <= 1 || n.length < minLen) ? -1 : firstClash(n.pitch, n.start, n.end());
        if (fc < 0) {
            placed.push_back({n.start, n.end(), n.pitch});
            continue;
        }
        // Reibt der Ton erst am Ende (Ausklang in den nächsten Akkord): dort kürzen statt verschieben
        if (fc - n.start >= minLen) {
            n.length = std::max<Tick>(six_ / 2, fc - n.start - six_ / 4);
            placed.push_back({n.start, n.end(), n.pitch});
            continue;
        }
        const Chord& ch = chordAtAbs(n.start);
        int best = -1;
        for (int d = 1; d <= 7 && best < 0; ++d)
            for (int sg : {-1, 1}) {
                const int q = n.pitch + sg * d;
                if (ch.hasPc(q) && !clashes(q, n.start, n.end())) { best = q; break; }
            }
        if (best >= 0) {
            n.pitch = best;
            placed.push_back({n.start, n.end(), n.pitch});
        } else {
            del[it.tr][it.idx] = 1;   // kein passender Ton -> pausieren
        }
    }
    for (size_t ti = 0; ti < song_.tracks.size(); ++ti) {
        auto& ns = song_.tracks[ti].notes;
        size_t w = 0;
        for (size_t i = 0; i < ns.size(); ++i)
            if (!del[ti][i]) ns[w++] = ns[i];
        ns.resize(w);
    }
}

// ---------------------------------------------------------------------
//  Dynamik & Übergänge (regelbasiert, nicht zufällig)
//   1. Spannungsbogen über den ganzen Song (Intensität am Anfang/Ende jedes Abschnitts)
//   2. Generalpause vor dem letzten Refrain + Akzent auf dem Einsatz
//   3. Lautstärke-Automation pro Spur: Bogen × Einblenden neuer Stimmen × Ausblenden endender
//      Stimmen × "Atmen" gehaltener Flächen
//   4. Phrasierung im Anschlag: Taktschwerpunkte, höhere Töne einer Phrase etwas lauter
//   5. Dynamikzeichen für die Partitur
// ---------------------------------------------------------------------
void Builder::applyDynamics() {
    const auto& st = rs_.structure;
    const size_t n = st.size();
    if (n == 0 || song_.sections.size() != n) return;

    // ---- 1. Spannungsbogen ----
    std::vector<float> i0(n), i1(n);
    int chorusTotal = 0, chorusSeen = 0, verseSeen = 0, lastChorus = -1;
    for (size_t k = 0; k < n; ++k)
        if (st[k].type == Sec::Chorus) { ++chorusTotal; lastChorus = int(k); }
    for (size_t k = 0; k < n; ++k) {
        const bool nextChorus = k + 1 < n && st[k + 1].type == Sec::Chorus;
        switch (st[k].type) {
        case Sec::Intro: i0[k] = 0.28f; i1[k] = nextChorus ? 0.6f : 0.42f; break;
        case Sec::Verse: i0[k] = i1[k] = 0.46f + 0.05f * float(verseSeen++); if (nextChorus) i1[k] += 0.08f; break;
        case Sec::Pre: i0[k] = 0.52f; i1[k] = 0.84f; break;   // Aufbau
        case Sec::Chorus:
            i0[k] = i1[k] = (int(k) == lastChorus && chorusTotal > 1) ? 0.96f : 0.8f + 0.05f * float(chorusSeen);
            ++chorusSeen;
            break;
        case Sec::Bridge: i0[k] = 0.32f; i1[k] = nextChorus ? 0.74f : 0.42f; break;   // subito piano, dann Aufbau
        case Sec::Outro: i0[k] = 0.58f; i1[k] = k + 1 == n ? 0.16f : 0.4f; break;     // verklingen
        }
    }
    for (size_t k = 0; k < n; ++k) {   // Varianten: Zwischenspiel, Breakdown
        const bool nextChorus = k + 1 < n && st[k + 1].type == Sec::Chorus;
        if (st[k].type == Sec::Intro && k > 0) { i0[k] = 0.45f; i1[k] = nextChorus ? 0.66f : 0.5f; }
        if (st[k].stripped) { i0[k] -= 0.22f; i1[k] -= 0.1f; }
    }
    for (size_t k = 0; k < n; ++k) {   // eingestellte Energie pro Abschnitt ("C8+", Solo)
        i0[k] += st[k].energy * 0.6f;
        i1[k] += st[k].energy * 0.6f;
    }
    const float scale = 0.8f + 0.4f * s_.energy;
    const float spread = 0.4f + 1.2f * std::clamp(s_.dynamicRange, 0.f, 1.f);   // Dynamik-Umfang (0.5 = wie bisher)
    for (size_t k = 0; k < n; ++k) {
        i0[k] = std::clamp((0.6f + (i0[k] - 0.6f) * spread) * scale, 0.1f, 1.08f);
        i1[k] = std::clamp((0.6f + (i1[k] - 0.6f) * spread) * scale, 0.1f, 1.08f);
    }
    for (size_t k = 0; k < n; ++k)   // Ausblenden: bis fast zur Stille
        if (st[k].fade) i1[k] = 0.03f;
    if (bigFinale_ && n > 0 && !st[n - 1].fade) {   // großes Finale: zum Schluss anschwellen statt verklingen
        i0[n - 1] = std::max(i0[n - 1], 0.6f);
        i1[n - 1] = std::max(i1[n - 1], 0.98f);
    }
    auto secOf = [&](Tick t) {
        size_t k = 0;
        while (k + 1 < n && t >= song_.sections[k + 1].start) ++k;
        return k;
    };
    auto intensity = [&](Tick t) {
        const size_t k = secOf(t);
        const auto& sec = song_.sections[k];
        const float prog = std::clamp(float(t - sec.start) / float(std::max<Tick>(1, sec.length)), 0.f, 1.f);
        return i0[k] + (i1[k] - i0[k]) * prog;
    };

    // ---- 2. Generalpause vor dem letzten Refrain (bei kraftvoller Musik) ----
    if (s_.generalPause && lastChorus > 0 && chorusTotal > 1 && (epic_ || s_.energy > 0.6f)) {
        const Tick S = song_.sections[size_t(lastChorus)].start, cut = S - beat_;
        for (auto& tr : song_.tracks) {
            auto& ns = tr.notes;
            ns.erase(std::remove_if(ns.begin(), ns.end(), [&](const Note& x) { return x.start >= cut && x.start < S; }), ns.end());
            for (auto& x : ns)
                if (x.start < cut && x.end() > cut) x.length = std::max<Tick>(six_ / 2, cut - x.start);
        }
    }
    // Akzent (sforzato) auf dem Einsatz jedes Refrains
    for (size_t k = 0; k < n; ++k) {
        if (st[k].type != Sec::Chorus) continue;
        const Tick S = song_.sections[k].start;
        const float acc = int(k) == lastChorus ? 1.15f : 1.08f;
        for (auto& tr : song_.tracks)
            for (auto& x : tr.notes)
                if (x.start >= S && x.start < S + six_) x.velocity = std::clamp(x.velocity * acc, 0.05f, 1.f);
    }

    // ---- 3. Lautstärke-Automation pro Spur – jede Stimme nach ihrer Rolle im Abschnitt ----
    const Tick songEnd = song_.length();
    const bool orch = s_.ensemble == Ensemble::Orchestra;
    enum Role { RMel, RDouble, RCounter, RHarm, RBass, RPerc, RSparkle };
    auto orchIndex = [&](const std::string& name) {
        for (int i = 0; i < OCOUNT; ++i) if (name == orchInstName(i)) return i;
        return -1;
    };
    // Wer trägt in welchem Abschnitt die Melodie? Und wann klingt sie (für Ausweichen/Antworten)?
    std::vector<int> leadOf(n, -1);
    std::vector<std::vector<std::pair<Tick, Tick>>> melSpans(n);
    for (size_t k = 0; k < n; ++k) {
        const Material& mat = mats_[st[k].type];
        if (!mat.melody) continue;
        leadOf[k] = orch ? mat.orch.lead : -1;
        const std::string leadName = orch ? orchInstName(mat.orch.lead) : (st[k].solo ? "Solo" : "Melodie");
        const auto& sec = song_.sections[k];
        for (auto& tr : song_.tracks) {
            if (tr.name != leadName) continue;
            for (auto& x : tr.notes)
                if (x.start >= sec.start && x.start < sec.start + sec.length) {
                    if (!melSpans[k].empty() && x.start - melSpans[k].back().second < beat_ / 2)
                        melSpans[k].back().second = std::max(melSpans[k].back().second, x.end());
                    else melSpans[k].push_back({x.start, x.end()});
                }
        }
    }
    auto melodySounds = [&](size_t k, Tick t) {
        for (auto& sp : melSpans[k]) if (t >= sp.first && t < sp.second + beat_ / 2) return true;
        return false;
    };
    for (auto& tr : song_.tracks) {
        auto inst = InstrumentRegistry::instance().create(tr.instrument);
        const bool drum = inst && inst->isDrumKit();
        const bool sustaining = inst && inst->sustains();
        const int oi = orch ? orchIndex(tr.name) : -1;
        const bool brassHarm = oi == HN || oi == TP || oi == TB;
        // Rolle dieser Stimme in jedem Abschnitt
        std::vector<Role> role(n, RHarm);
        for (size_t k = 0; k < n; ++k) {
            if (drum || oi == TI) { role[k] = RPerc; continue; }
            if (!orch) {
                role[k] = (tr.name == "Melodie" || tr.name == "Solo") ? RMel : tr.name == "Bass" ? RBass
                        : tr.name == "Zweite Stimme" ? RCounter : RHarm;
                continue;
            }
            const OrchPlan& o = mats_[st[k].type].orch;
            const bool hasMel = mats_[st[k].type].melody;
            if (oi == GL) { role[k] = RSparkle; continue; }   // Glockenspiel: Glanz, nicht Hauptsache
            if (oi == PI) { role[k] = RDouble; continue; }    // Piccolo verdoppelt die Melodie
            if (oi == TU) { role[k] = RBass; continue; }
            if (hasMel && oi == o.lead) role[k] = RMel;
            else if (hasMel && o.mel == MelT::CallResponse && oi == o.second) role[k] = RMel;
            else if (hasMel && o.mel == MelT::Unison && (oi == V1 || oi == V2 || oi == VA || oi == VC || oi == HN)) role[k] = RDouble;
            else if (hasMel && o.mel != MelT::Solo && oi == o.second) role[k] = RDouble;
            else if (hasMel && o.choir && o.choirMode == 1 && oi == CH) role[k] = RDouble;
            else if (o.counter && oi == o.counterInst) role[k] = RCounter;
            else if (oi == KB || ((oi == VC || oi == FG) && o.bass != BassT::LowOstinato)) role[k] = RBass;
            else role[k] = RHarm;
        }
        // Wo spielt die Stimme? (erster Ton je Abschnitt)
        std::vector<char> act(n, 0);
        std::vector<Tick> first(n, 0);
        for (auto& x : tr.notes) {
            const size_t k = secOf(x.start);
            if (!act[k] || x.start < first[k]) first[k] = x.start;
            act[k] = 1;
        }
        std::vector<std::pair<Tick, float>> pts;
        for (Tick t = 0; t <= songEnd + beat_; t += beat_) {
            const size_t k = secOf(t);
            const auto& sec = song_.sections[k];
            const float I = intensity(t);
            // Eigene Kurve je Rolle: Melodie vorne, Begleitung dahinter, Bass gleichmäßiger, Schlagwerk mit großem Umfang
            float g;
            switch (role[k]) {
            case RMel: g = 0.45f + 0.65f * I; break;
            case RDouble: g = 0.36f + 0.62f * I; break;
            case RCounter: g = 0.3f + 0.65f * I; break;
            case RBass: g = 0.42f + 0.58f * I; break;
            case RPerc: g = 0.32f + 0.88f * I; break;
            case RSparkle: g = 0.12f + 0.5f * I; break;   // leise in ruhigen Teilen, präsenter an Höhepunkten
            default: g = 0.26f + 0.76f * I; break;
            }
            if (brassHarm && role[k] == RHarm && I < 0.65f) g *= 0.8f;   // Blech hält sich bis zum Höhepunkt zurück
            if (orch) {   // Rollen-Mix: die Melodie führt, Verdopplungen/Gegenstimmen dahinter, Begleitung und Glanz weiter hinten
                static const float roleGain[] = {1.f, 0.62f, 0.55f, 0.45f, 0.6f, 0.7f, 0.5f};
                g *= roleGain[int(role[k])] * 1.45f;   // 1,45: Gesamtlautstärke ungefähr wie vorher
            }
            if (!melSpans[k].empty()) {
                const bool sounding = melodySounds(k, t);
                if (role[k] == RHarm) g *= sounding ? 0.88f : 1.06f;        // der Melodie ausweichen, Lücken füllen
                if (role[k] == RCounter) g *= sounding ? 0.9f : 1.18f;      // Gegenstimme antwortet in den Pausen
            }
            // Rampenlicht: neues Melodie-Instrument in den ersten 2 Takten hervorheben, Rest tritt zurück
            const bool newLead = orch && leadOf[k] >= 0 && (k == 0 || leadOf[k] != leadOf[k - 1]) && st[k].type != Sec::Chorus;
            if (newLead && t < sec.start + 2 * bar_) {
                if (role[k] == RMel) g *= 1.12f;
                else if (role[k] == RHarm || role[k] == RCounter) g *= 0.9f;
            }
            // Einblenden: Stimme setzt (neu) ein -> über bis zu 2 Takte hochfahren (Refrain-Einsätze dürfen knallen)
            if (act[k] && (k == 0 || !act[k - 1]) && st[k].type != Sec::Chorus && !drum) {
                const Tick e0 = first[k], L = std::min<Tick>(2 * bar_, std::max<Tick>(bar_, sec.length / 2));
                if (t < e0) g *= 0.3f;
                else if (t < e0 + L) g *= 0.3f + 0.7f * float(t - e0) / float(L);
            }
            // Ausblenden: Stimme hört nach diesem Abschnitt auf -> letzter Takt abfallend
            if (act[k] && k + 1 < n && !act[k + 1]) {
                const Tick x0 = sec.start + sec.length - bar_;
                if (t > x0) g *= 1.f - 0.5f * float(t - x0) / float(bar_);
            }
            // Gehaltene Flächen "atmen" in ruhigen Teilen (sanftes An- und Abschwellen über 2 Takte)
            if (sustaining && !drum && I < 0.6f)
                g *= 0.9f + 0.1f * std::sin(3.14159265f * float(t % (2 * bar_)) / float(2 * bar_));
            if (st[k].fade) {   // Ausblenden: über den ganzen Abschnitt bis fast zur Stille
                const float prog = std::clamp(float(t - sec.start) / float(std::max<Tick>(1, sec.length)), 0.f, 1.f);
                g *= std::max(0.02f, std::pow(1.f - prog, 1.6f));
            }
            if (pts.empty() || std::abs(pts.back().second - g) > 0.004f || t > songEnd) pts.push_back({t, g});
        }
        tr.expression = std::move(pts);

        // ---- 4. Phrasierung im Anschlag ----
        if (drum || tr.notes.empty()) continue;
        std::map<Tick, std::pair<int, int>> range;   // je 2 Takte: tiefster/höchster Ton
        for (auto& x : tr.notes) {
            auto& r = range[x.start / (2 * bar_)];
            if (r.first == 0 && r.second == 0) r = {x.pitch, x.pitch};
            r.first = std::min(r.first, x.pitch);
            r.second = std::max(r.second, x.pitch);
        }
        for (auto& x : tr.notes) {
            const Tick pos = x.start % bar_;
            float mul = pos == 0 ? 1.06f : (bpb_ == 4 && pos == 2 * beat_) ? 1.03f : (pos % beat_ == 0) ? 1.f : 0.94f;
            const auto& r = range[x.start / (2 * bar_)];
            if (r.second > r.first) mul *= 0.93f + 0.14f * float(x.pitch - r.first) / float(r.second - r.first);
            x.velocity = std::clamp(x.velocity * mul, 0.05f, 1.f);
        }
    }

    // ---- 5. Dynamikzeichen für die Partitur ----
    auto level = [](float I) { return I < 0.3f ? 0 : I < 0.42f ? 1 : I < 0.55f ? 2 : I < 0.7f ? 3 : I < 0.88f ? 4 : 5; };
    song_.dynamics.clear();
    int lastLevel = -1;
    for (size_t k = 0; k < n; ++k) {
        const auto& sec = song_.sections[k];
        const int lv = level(i0[k]);
        if (lv != lastLevel) song_.dynamics.push_back({DynamicMark::Level, sec.start, sec.start, lv});
        lastLevel = lv;
        if (i1[k] - i0[k] > 0.1f || i0[k] - i1[k] > 0.1f) {
            const Tick a = sec.start + std::min<Tick>(bar_, sec.length / 4);
            song_.dynamics.push_back({i1[k] > i0[k] ? DynamicMark::Crescendo : DynamicMark::Diminuendo, a, sec.start + sec.length - six_, 0});
            lastLevel = level(i1[k]);
            if (k + 1 == n) song_.dynamics.push_back({DynamicMark::Level, sec.start + sec.length - bar_, sec.start + sec.length, lastLevel});
        }
    }
}

// Humanisierung und Aufräumen (Überlappungen gleicher Töne verhindern)
void Builder::finish() {
    // Swing: Zeit innerhalb jedes Schlags verbiegen – die Off-Beat-Achtel rutscht Richtung Triole
    const float sw = std::clamp(s_.swing, 0.f, 1.f);
    if (sw > 0.01f) {
        const double half = double(beat_) / 2.0, d = double(sw) * double(beat_) / 6.0;
        auto warp = [&](Tick t) {
            const Tick b0 = (t >= 0 ? t / beat_ : (t - beat_ + 1) / beat_) * beat_;
            const double p = double(t - b0);
            const double q = p <= half ? p * (half + d) / half : half + d + (p - half) * (half - d) / half;
            return b0 + Tick(std::lround(q));
        };
        for (auto& t : song_.tracks)
            for (auto& n : t.notes) {
                const Tick s0 = warp(n.start), e0 = warp(n.end());
                n.start = s0;
                n.length = std::max<Tick>(1, e0 - s0);
            }
        for (auto& t : song_.tracks)
            for (auto& pt : t.expression) pt.first = warp(pt.first);
    }

    // Groove-Gefühl (Mikrotiming wie bei echten Drummern/Bassisten), nur Band-Besetzung
    if (s_.groove > 0 && s_.ensemble == Ensemble::Band) {
        const float g = std::clamp(s_.grooveAmount, 0.f, 1.f);
        const bool laid = s_.groove == 1;
        auto shift = [&](Note& n, float ticks) { n.start = std::max<Tick>(0, n.start + Tick(std::lround(ticks * g))); };
        for (auto& t : song_.tracks) {
            const bool drums = t.name == "Schlagzeug";
            for (auto& n : t.notes) {
                if (drums) {
                    const bool snare = n.pitch == 38 || n.pitch == 40 || n.pitch == 39;
                    const bool hat = n.pitch == 42 || n.pitch == 44 || n.pitch == 46 || n.pitch == 51;
                    if (snare) shift(n, laid ? 22.f : -4.f);
                    else if (hat) shift(n, laid ? 6.f : -10.f);
                    if (hat) {   // Betonung: Achtel-Zählzeit kräftig, 16tel dazwischen leiser
                        const Tick pos = n.start % beat_;
                        const float acc = pos < six_ / 2 ? 1.f + 0.12f * g : (std::abs(pos - beat_ / 2) < six_ / 2 ? 1.f - 0.12f * g : 1.f - 0.25f * g);
                        n.velocity = std::clamp(n.velocity * acc, 0.05f, 1.f);
                    }
                } else if (t.name == "Bass") {
                    shift(n, laid ? 12.f : -3.f);
                } else if (t.name == "Begleitung") {
                    shift(n, laid ? 6.f : 0.f);
                } else if (t.name == "Melodie" || t.name == "Solo") {
                    shift(n, laid ? 10.f : -2.f);
                }
            }
        }
    }

    const float h = std::clamp(s_.humanize, 0.f, 1.f);
    const Tick jitter = Tick(h * 14.f);
    for (auto& t : song_.tracks) {
        for (auto& n : t.notes) {
            if (jitter > 0 && n.start > 0) n.start = std::max<Tick>(0, n.start + irand(r_, -int(jitter), int(jitter)));
            n.velocity = std::clamp(n.velocity * (1.f + frand(r_, -h * 0.12f, h * 0.12f)), 0.05f, 1.f);
        }
        std::sort(t.notes.begin(), t.notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
        std::map<int, size_t> lastOf;
        for (size_t i = 0; i < t.notes.size(); ++i) {
            auto it = lastOf.find(t.notes[i].pitch);
            if (it != lastOf.end()) {
                Note& prev = t.notes[it->second];
                if (prev.end() > t.notes[i].start) prev.length = std::max<Tick>(1, t.notes[i].start - prev.start);
            }
            lastOf[t.notes[i].pitch] = i;
        }
    }
    // Melodie ist einstimmig (Band-Besetzung: Spur 0)
    if (s_.ensemble == Ensemble::Orchestra || song_.tracks.empty()) return;
    auto& mel = song_.tracks[0].notes;
    for (size_t i = 0; i + 1 < mel.size(); ++i)
        if (mel[i].end() > mel[i + 1].start) mel[i].length = std::max<Tick>(six_ / 2, mel[i + 1].start - mel[i].start);
}

std::vector<std::string> Builder::progressionInfo() const {
    std::vector<std::string> out;
    for (Sec t : matOrder_) {
        auto it = mats_.find(t);
        if (it == mats_.end()) continue;
        std::string line = std::string(secName(t)) + ":  ";
        std::string roman;
        for (auto& c : it->second.chords) { line += c.symbol + "  "; roman += c.roman + " "; }
        line += " (" + roman.substr(0, roman.empty() ? 0 : roman.size() - 1) + ")";
        out.push_back(line);
    }
    return out;
}

// ===================== Grundparameter =====================
Resolved resolve(const GeneratorSettings& s, Rng& r) {
    Resolved rs;
    KeyGuess guess;
    if (s.idea && !s.idea->notes.empty()) {
        rs.hasIdea = true;
        rs.ideaBars = std::clamp((s.idea->bars + 1) / 2 * 2, 2, 16);
        rs.ideaType = s.idea->asChorus ? Sec::Chorus : Sec::Verse;
        guess = detectKey(s.idea->notes);   // Tonart aus der eigenen Idee
    }
    // Orchester-Stil (beeinflusst Skala und Takt)
    const int nStyles = int(orchProfiles().size());
    rs.orchStyle = (s.orchStyle >= 0 && s.orchStyle < nStyles) ? s.orchStyle : irand(r, 0, nStyles - 1);
    const bool orch = s.ensemble == Ensemble::Orchestra;
    const std::string styleName = orchProfiles()[size_t(rs.orchStyle)].name;

    // Eigene Akkordfolge: Tonart daraus ableiten (erster Akkord zählt doppelt – meist die Tonika)
    bool chordGuess = false;
    if (!rs.hasIdea) {
        std::istringstream in(s.chordsVerse + " " + s.chordsChorus + " " + s.chordsPre + " " + s.chordsBridge);
        std::string tok;
        std::vector<Note> notes;
        Tick t = 0;
        while (in >> tok) {
            int root = 0;
            std::vector<int> iv;
            if (!parseChordSymbol(tok, root, iv)) continue;
            const int reps = notes.empty() ? 2 : 1;
            for (int k = 0; k < reps; ++k) {
                for (int x : iv) notes.push_back({60 + mod12(root + x), t, 480, 0.7f});
                notes.push_back({48 + root, t, 960, 0.7f});
                t += 960;
            }
        }
        if (!notes.empty()) { guess = detectKey(notes); chordGuess = true; }
    }
    rs.key = s.key >= 0 ? s.key % 12 : (rs.hasIdea || chordGuess) ? guess.key : irand(r, 0, 11);
    if (s.scale >= 0 && s.scale < int(ScaleType::Count)) rs.scale = ScaleType(s.scale);
    else if (rs.hasIdea || chordGuess) rs.scale = guess.scale;
    else if (orch && styleName == "Episch (Film)") {   // Kino-Klang: meist Moll
        using S = ScaleType;
        rs.scale = pickWeighted<S>(r, {{S::NaturalMinor, 0.45f}, {S::HarmonicMinor, 0.15f}, {S::Dorian, 0.2f}, {S::Major, 0.2f}});
    } else if (orch && styleName == "Mysteriös") {
        using S = ScaleType;
        rs.scale = pickWeighted<S>(r, {{S::NaturalMinor, 0.3f}, {S::HarmonicMinor, 0.3f}, {S::Phrygian, 0.2f}, {S::Dorian, 0.2f}});
    } else {
        using S = ScaleType;
        rs.scale = pickWeighted<S>(r, {{S::Major, 0.3f}, {S::NaturalMinor, 0.3f}, {S::Dorian, 0.08f}, {S::Mixolydian, 0.07f},
                                       {S::Lydian, 0.04f}, {S::HarmonicMinor, 0.06f}, {S::Phrygian, 0.02f},
                                       {S::MajorPentatonic, 0.07f}, {S::MinorPentatonic, 0.06f}});
    }
    rs.bpm = s.bpm > 0 ? s.bpm : (scaleIsMinor(rs.scale) ? irand(r, 70, 112) : irand(r, 90, 132));
    rs.bpb = std::clamp(s.beatsPerBar, 2, 7);
    if (orch && !rs.hasIdea && styleName == "Walzer") {   // Walzer = 3/4, schwungvolles Tempo
        rs.bpb = 3;
        if (s.bpm <= 0) rs.bpm = irand(r, 132, 176);
    }
    if (orch && !rs.hasIdea && s.bpm <= 0 && styleName == "Marsch / Heroisch") rs.bpm = irand(r, 104, 122);
    rs.structure = parseStructure(s.structure);
    // Auto-Form immer würfeln (gleicher Zufallsverlauf, auch wenn eine Form vorgegeben ist)
    const int target = std::max(4, int(std::lround(s.lengthSeconds * rs.bpm / 60.0 / rs.bpb)));
    auto autoForm = autoStructure(r, target, s);
    (void)autoForm;
    if (rs.structure.empty()) {
        // Songform aus der Bibliothek (Pop, Refrain zuerst, AABA, Strophenlied, Dance …)
        rs.structure = chooseForm(r, target, s, rs.formIndex);
        // Episch: langes Aufbau-Intro, in dem die Schichten nacheinander einsetzen
        if (orch && styleName == "Episch (Film)" && target >= 40 && rs.structure.front().type == Sec::Intro)
            rs.structure.front().bars = std::max(rs.structure.front().bars, 8);
        bool hasSolo = false, hasChorus = false;
        for (auto& sd : rs.structure) { hasSolo = hasSolo || sd.solo; hasChorus = hasChorus || sd.type == Sec::Chorus; }
        // Form ohne Refrain (AABA, Strophenlied): Solo vor die letzte Strophe
        if (s.solo && !s.idea && !hasSolo && !hasChorus) {
            for (size_t i = rs.structure.size(); i-- > 1;)
                if (rs.structure[i].type == Sec::Verse) {
                    rs.structure.insert(rs.structure.begin() + std::ptrdiff_t(i), SecDef{Sec::Verse, rs.structure[i].bars, 0.1f, true});
                    break;
                }
            hasSolo = true;
        }
        // Instrumental-Solo nach dem zweiten Refrain (bzw. vor dem letzten)
        if (s.solo && !s.idea && !hasSolo) {
            int chorusSeen = 0;
            size_t at = rs.structure.size();
            for (size_t i = 0; i < rs.structure.size(); ++i)
                if (rs.structure[i].type == Sec::Chorus && ++chorusSeen == 2) { at = i + 1; break; }
            if (at == rs.structure.size()) {   // nur ein Refrain: vor den letzten
                for (size_t i = rs.structure.size(); i-- > 0;)
                    if (rs.structure[i].type == Sec::Chorus) { at = i; break; }
            }
            int verseBars = 8;
            for (auto& sd : rs.structure) if (sd.type == Sec::Verse) { verseBars = sd.bars; break; }
            SecDef solo{Sec::Verse, verseBars, 0.1f, true};
            rs.structure.insert(rs.structure.begin() + std::ptrdiff_t(std::min(at, rs.structure.size())), solo);
            // Das Solo ersetzt die Bridge-Funktion: danach folgt immer ein Refrain
            const size_t si = std::min(at, rs.structure.size() - 1);
            if (si + 1 < rs.structure.size() && rs.structure[si + 1].type == Sec::Bridge)
                rs.structure.erase(rs.structure.begin() + std::ptrdiff_t(si + 1));
            if (si + 1 >= rs.structure.size() || rs.structure[si + 1].type != Sec::Chorus) {
                int chorusBars = 8;
                for (auto& sd : rs.structure) if (sd.type == Sec::Chorus) { chorusBars = sd.bars; break; }
                rs.structure.insert(rs.structure.begin() + std::ptrdiff_t(si + 1), SecDef{Sec::Chorus, chorusBars});
            }
        }
    }
    if (rs.hasIdea) {
        // Abschnitt der Idee: mindestens 8 Takte – kurze Ideen werden danach variiert fortgesetzt.
        // Als Strophe beginnt der Song direkt mit der Idee.
        const int secBars = rs.ideaBars >= 8 ? rs.ideaBars : 8;
        for (auto& sd : rs.structure) if (sd.type == rs.ideaType) sd.bars = secBars;
        if (rs.ideaType == Sec::Verse) {
            while (!rs.structure.empty() && rs.structure.front().type == Sec::Intro) rs.structure.erase(rs.structure.begin());
            if (rs.structure.empty() || rs.structure.front().type != Sec::Verse)
                rs.structure.insert(rs.structure.begin(), {Sec::Verse, secBars});
        } else {
            bool has = false;
            for (auto& sd : rs.structure) has = has || sd.type == Sec::Chorus;
            if (!has) rs.structure.push_back({Sec::Chorus, secBars});
        }
    }
    // Rückung: letzter Refrain einen Ganz- oder Halbton höher
    int choruses = 0, lastChorus = -1;
    for (size_t i = 0; i < rs.structure.size(); ++i)
        if (rs.structure[i].type == Sec::Chorus) { ++choruses; lastChorus = int(i); }
    const bool kcRoll = chance(r, 0.12f + s.creativity * 0.35f);
    const bool kcBig = chance(r, 0.6f);
    if (s.forceKeyChangeAt >= -1) {   // vom Original übernommen ("Abschnitt neu komponieren")
        rs.keyChangeAt = s.forceKeyChangeAt < int(rs.structure.size()) ? s.forceKeyChangeAt : -1;
        rs.keyChangeSemis = rs.keyChangeAt >= 0 ? s.forceKeyChangeSemis : 0;
    } else if (s.allowKeyChange && choruses >= 3 && kcRoll) {
        rs.keyChangeAt = lastChorus;
        // bei doppeltem Schlussrefrain schon beim ersten der beiden rücken
        if (lastChorus > 0 && rs.structure[lastChorus - 1].type == Sec::Chorus) rs.keyChangeAt = lastChorus - 1;
        rs.keyChangeSemis = kcBig ? 2 : 1;
    }
    // Schlagzeug-Stil passend zu Tempo, Energie und Charakter
    if (s.drums != DrumStyle::Auto) rs.drumStyle = s.drums;
    else {
        using D = DrumStyle;
        std::vector<std::pair<D, float>> o;
        if (rs.bpm < 82) o = {{D::Ballad, 0.5f}, {D::HalfTime, 0.3f}, {D::Pop, 0.2f}};
        else if (s.energy > 0.7f)
            o = {{D::Rock, 0.4f}, {D::Pop, 0.35f}, {D::FourOnFloor, (rs.bpm >= 110 && rs.bpm <= 135) ? 0.3f : 0.f}, {D::HalfTime, 0.1f}};
        else o = {{D::Pop, 0.5f}, {D::Rock, 0.15f}, {D::HalfTime, 0.12f}, {D::Ballad, 0.1f}, {D::FourOnFloor, 0.08f}};
        if (s.bass == BassStyle::Walking || (s.creativity > 0.6f && s.syncopation > 0.45f)) o.push_back({D::Jazz, 0.8f});
        if (s.energy > 0.7f && scaleIsMinor(rs.scale)) o.push_back({D::Epic, 0.45f});
        rs.drumStyle = pickWeighted(r, o);
    }
    return rs;
}

}   // namespace

const char* accompStyleName(AccompStyle s) {
    static const char* n[] = {"Automatisch", "Flächen", "Akkord-Rhythmus", "Arpeggio", "Alberti-Bass", "Afterbeat"};
    return n[std::clamp(int(s), 0, int(AccompStyle::Count) - 1)];
}

int orchStyleCount() { return int(orchProfiles().size()); }
int songFormCount() { return int(formDefs().size()); }
const char* songFormName(int f) { return (f >= 0 && f < songFormCount()) ? formDefs()[size_t(f)].name : "Zufall (passend)"; }
const char* songFormHint(int f) {
    return (f >= 0 && f < songFormCount()) ? formDefs()[size_t(f)].hint : "Wählt bei jedem Song eine passende Form - zur Stimmung, Energie und Besetzung.";
}
const char* orchStyleName(int style) {
    if (style < 0 || style >= orchStyleCount()) return "Zufall";
    return orchProfiles()[size_t(style)].name;
}

const char* drumStyleName(DrumStyle s) {
    static const char* n[] = {"Automatisch", "Pop", "Rock", "Halftime", "Four-on-the-floor", "Ballade", "Jazz (Swing)", "Episch (Toms)"};
    return n[std::clamp(int(s), 0, int(DrumStyle::Count) - 1)];
}

const char* bassStyleName(BassStyle s) {
    static const char* n[] = {"Automatisch", "Ganze Noten", "Grundton-Quinte", "Achtel", "Walking", "Synkopiert"};
    return n[std::clamp(int(s), 0, int(BassStyle::Count) - 1)];
}

// ===================== Raumklang (Hall & Echo) =====================
namespace {
struct RoomDef { const char* name; float room, damp, level, pre; int delayNote; float delayFb, delayLevel; };
const std::vector<RoomDef>& roomDefs() {
    static const std::vector<RoomDef> r = {
        {"Passend zur Musik", 0.6f, 0.45f, 0.8f, 0.02f, 1, 0.35f, 0.7f},   // Platzhalter (nur Generator)
        {"Trocken", 0.2f, 0.65f, 0.25f, 0.005f, 1, 0.25f, 0.5f},
        {"Kleiner Raum", 0.36f, 0.55f, 0.55f, 0.01f, 1, 0.3f, 0.6f},
        {"Studio", 0.55f, 0.5f, 0.72f, 0.02f, 1, 0.35f, 0.7f},
        {"Konzertsaal", 0.8f, 0.35f, 0.95f, 0.03f, 0, 0.3f, 0.6f},
        {"Kathedrale", 0.93f, 0.25f, 1.1f, 0.045f, 0, 0.35f, 0.6f},
        {"Weltraum", 0.97f, 0.15f, 1.2f, 0.06f, 2, 0.55f, 0.9f},
    };
    return r;
}

// ---------------------------------------------------------------------
//  Automatischer Pegelabgleich (Band) – wie ein Toningenieur:
//  hört sich jede Spur im letzten Refrain einzeln an (wahrgenommene Lautheit, K-gewichtet) und stellt ihre
//  Lautstärke so ein, dass jede Rolle ihren festen Abstand zur Melodie hat (Bass -4,5 dB, Begleitung -7 dB …).
//  Die Instrumente selbst sind auf gleiche wahrgenommene Lautheit kalibriert.
// ---------------------------------------------------------------------
// Wahrgenommene Lautheit eines Stereo-Puffers (K-Gewichtung wie bei LUFS), in dB
double perceivedLevel(const std::vector<float>& s, float sr) {
    Biquad hp, sh;
    hp.highpass(60.f, 0.5f, sr);
    sh.shelf(true, 1500.f, 4.f, sr);
    double sum = 0;
    size_t n = 0;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        float l = s[i], r = s[i + 1];
        hp.process(l, r);
        sh.process(l, r);
        sum += double(l) * l + double(r) * r;
        ++n;
    }
    return 10.0 * std::log10(std::max(1e-12, sum / double(std::max<size_t>(1, n))));
}

// Eine Spur allein (mit ihrer Lautstärke und Dynamik-Automation) in einem Zeitfenster anhören und messen
double trackLevel(const Song& song, size_t track, Tick a, Tick b) {
    Song solo;
    solo.bpm = song.bpm;
    solo.ppq = song.ppq;
    solo.beatsPerBar = song.beatsPerBar;
    solo.beatUnit = song.beatUnit;
    solo.fx.on = false;
    solo.fx.mastering = false;
    Track t = song.tracks[track];
    t.muted = false;
    t.solo = false;
    t.notes.erase(std::remove_if(t.notes.begin(), t.notes.end(), [&](const Note& n) { return n.end() <= a || n.start >= b; }),
                  t.notes.end());
    if (t.notes.empty()) return -200;
    solo.tracks.push_back(t);
    AudioEngine e;
    e.loadSong(solo);
    const float sr = 32000.f;   // reicht für eine Lautheitsmessung und spart Zeit
    return perceivedLevel(e.renderRange(double(a), double(b), int(sr)), sr);
}

void balanceBand(Song& song) {
    if (song.tracks.empty() || song.sections.empty()) return;
    // Messfenster: der letzte Refrain (dort spielen fast alle Stimmen zusammen), sonst der Abschnitt mit den meisten Spuren
    auto active = [&](size_t i, Tick a, Tick b) {
        for (auto& n : song.tracks[i].notes) if (n.start < b && n.end() > a) return true;
        return false;
    };
    int win = -1, bestCount = -1;
    for (int k = int(song.sections.size()) - 1; k >= 0; --k) {
        const auto& sec = song.sections[size_t(k)];
        int cnt = 0;
        for (size_t i = 0; i < song.tracks.size(); ++i) cnt += active(i, sec.start, sec.start + sec.length);
        const bool chorus = sec.name.rfind("Refrain", 0) == 0 && sec.name.find("Breakdown") == std::string::npos;
        if (chorus && win < 0) { win = k; bestCount = cnt; break; }
        if (cnt > bestCount) { bestCount = cnt; win = k; }
    }
    if (win < 0) return;
    const Tick wa = song.sections[size_t(win)].start, wb = wa + std::min<Tick>(song.sections[size_t(win)].length, 16 * song.ticksPerBar());
    size_t melIdx = song.tracks.size();
    for (size_t i = 0; i < song.tracks.size(); ++i) if (song.tracks[i].name == "Melodie") melIdx = i;
    if (melIdx == song.tracks.size()) return;
    std::map<std::pair<Tick, Tick>, double> melCache;
    auto melodyIn = [&](Tick a, Tick b) {
        auto key = std::make_pair(a, b);
        auto it = melCache.find(key);
        if (it != melCache.end()) return it->second;
        const double v = trackLevel(song, melIdx, a, b);
        melCache[key] = v;
        return v;
    };
    const double melRef = melodyIn(wa, wb);
    if (melRef < -150) return;
    for (size_t i = 0; i < song.tracks.size(); ++i) {
        Track& t = song.tracks[i];
        if (i == melIdx || t.notes.empty()) continue;
        auto inst = InstrumentRegistry::instance().create(t.instrument);
        const bool sus = inst && inst->sustains();
        float target;   // gewünschter Abstand zur Melodie (dB), wie in einer guten Popmischung
        if (t.name == "Solo") target = 0.f;
        else if (t.name == "Bass") target = -4.5f;
        else if (t.name == "Begleitung") target = sus ? -8.f : -7.f;
        else if (t.name == "Schlagzeug") target = -3.5f;
        else if (t.name == "Zweite Stimme") target = -9.f;
        else if (t.name == "Streicher-Linie") target = -10.f;
        else target = -8.f;
        // im Fenster messen; spielt die Spur dort nicht, dann in ihrem eigenen ersten Abschnitt
        Tick a = wa, b = wb;
        if (!active(i, wa, wb)) {
            const Tick first = t.notes.front().start;
            for (auto& sec : song.sections)
                if (first >= sec.start && first < sec.start + sec.length) {
                    a = sec.start;
                    b = sec.start + std::min<Tick>(sec.length, 16 * song.ticksPerBar());
                }
        }
        const double lvl = trackLevel(song, i, a, b);
        if (lvl < -150) continue;
        double ref = melodyIn(a, b);
        if (ref < -150) ref = melRef;   // Melodie pausiert dort (z.B. beim Solo): mit ihrem Refrain-Pegel vergleichen
        const double diff = lvl - ref;
        t.volume = std::clamp(t.volume * std::pow(10.f, (target - float(diff)) / 20.f), 0.05f, 1.4f);
    }
}

// Raum passend zur Musik wählen und jeder Spur ihren Hall-/Echo-Anteil nach ihrer Rolle geben
void applyMix(Song& song, const GeneratorSettings& s, const Resolved& rs) {
    const bool orch = s.ensemble == Ensemble::Orchestra;
    const std::string style = orch ? orchProfiles()[size_t(rs.orchStyle)].name : "";
    int room = s.room;
    bool leadEcho = false;
    if (room <= 0 || room >= int(roomDefs().size())) {
        if (orch) {
            room = (style == "Episch (Film)" || style == "Mysteriös") ? 5 : style == "Barock / Kammer" ? 3 : 4;
        } else if (s.melodyInstrument == "Kirchenorgel" || s.chordInstrument == "Kirchenorgel") {
            room = 5;   // Orgel gehört in die Kathedrale
        } else if (!s.withDrums && s.accompaniment == AccompStyle::Pad && rs.bpm < 85) {
            room = 6;   // schwebend, Weltraum
        } else if (rs.drumStyle == DrumStyle::Jazz || (s.groove == 1 && s.swing > 0.3f)) {
            room = 2;   // Jazzclub / Lofi
        } else if (rs.drumStyle == DrumStyle::Epic) {
            room = 5;
        } else if (!s.withDrums || s.energy < 0.38f || rs.drumStyle == DrumStyle::Ballad) {
            room = 4;
            leadEcho = true;
        } else {
            room = 3;
            leadEcho = rs.drumStyle == DrumStyle::FourOnFloor;
        }
    }
    applyRoomPreset(song.fx, room);
    if (rs.drumStyle == DrumStyle::FourOnFloor && !orch) song.fx.delayNote = 2;   // punktierte Achtel (Dance-Klassiker)
    if (room == 6) leadEcho = true;
    for (auto& t : song.tracks) {
        const std::string& n = t.name;
        float rev = 0.2f, del = 0.f;
        if (!orch) {
            if (n == "Melodie") { rev = 0.22f; del = leadEcho ? 0.18f : 0.f; }
            else if (n == "Solo") { rev = 0.2f; del = 0.2f; }
            else if (n == "Zweite Stimme") { rev = 0.28f; del = leadEcho ? 0.12f : 0.f; }
            else if (n == "Streicher-Linie") rev = 0.36f;
            else if (n == "Begleitung") {
                auto inst = InstrumentRegistry::instance().create(t.instrument);
                rev = (inst && inst->sustains()) ? 0.3f : 0.22f;
            } else if (n == "Bass") rev = t.instrument == "Kirchenorgel" ? 0.3f : 0.03f;
            else if (n == "Schlagzeug") rev = 0.1f;
        } else {
            auto has = [&](const char* p) { return n.find(p) != std::string::npos; };
            if (has("Kontrab")) rev = 0.12f;
            else if (has("Violinen") || has("Bratschen") || has("Celli")) rev = 0.3f;
            else if (has("pizz")) rev = 0.25f;
            else if (has("Flöte") || has("Oboe") || has("Klarinette") || has("Fagott")) rev = 0.28f;
            else if (has("Hörner")) rev = 0.32f;
            else if (has("Trompeten") || has("Posaunen")) rev = 0.25f;
            else if (has("Pauken")) rev = 0.25f;
            else if (has("Schlagwerk")) rev = 0.2f;
            else if (has("Taiko")) rev = 0.22f;
            else if (has("Glocken")) rev = 0.4f;
            else if (has("Harfe")) rev = 0.38f;
            else if (has("Chor")) rev = 0.42f;
            else if (has("Tuba")) rev = 0.18f;
            else if (has("Piccolo")) rev = 0.3f;
            else if (has("Cembalo")) rev = 0.22f;
        }
        t.reverb = rev;
        t.delay = del;
    }
    if (!orch) balanceBand(song);
}
}

int roomPresetCount() { return int(roomDefs().size()); }
const char* roomPresetName(int p) { return (p >= 0 && p < roomPresetCount()) ? roomDefs()[size_t(p)].name : "?"; }
void applyRoomPreset(Effects& fx, int p) {
    if (p <= 0 || p >= roomPresetCount()) return;
    const RoomDef& d = roomDefs()[size_t(p)];
    fx.on = true;
    fx.roomSize = d.room;
    fx.damping = d.damp;
    fx.reverbLevel = d.level;
    fx.preDelay = d.pre;
    fx.delayNote = d.delayNote;
    fx.delayFeedback = d.delayFb;
    fx.delayLevel = d.delayLevel;
}

Song generateSong(const GeneratorSettings& s, GenerationInfo* info, std::atomic<float>* progress) {
    uint32_t seed = s.seed;
    if (seed == 0) {
        std::random_device rd;
        seed = rd();
        if (seed == 0) seed = 1;
    }
    Rng r(seed);
    const Resolved rs = resolve(s, r);

    // Mehrere komplette Songs komponieren, den besten behalten
    const int tries = 1 + std::clamp(s.quality, 1, 10) / 2;
    Song best;
    float bestScore = -1e30f;
    int evaluated = 0;
    std::vector<std::string> bestProg, bestTech, bestSys;
    int bestTry = 0;
    for (int t = 0; t < tries; ++t) {
        if (s.onlyTry >= 0 && t != std::min(s.onlyTry, tries - 1)) continue;   // nur der gewünschte Durchgang
        Builder b(s, rs, seed * 2654435761u + uint32_t(t) * 40503u + 17u, progress, float(t) / float(tries),
                  1.f / float(tries));
        Song song = b.build();
        evaluated += b.evaluated();
        if (b.score() > bestScore) {
            bestScore = b.score();
            bestTry = t;
            best = std::move(song);
            bestProg = b.progressionInfo();
            bestSys = b.systemsInfo();
            bestTech = s.ensemble == Ensemble::Orchestra ? b.orchInfo() : std::vector<std::string>{};
        }
    }

    std::string keyName = std::string(noteName(rs.key)) + " " + scaleName(rs.scale);
    best.title = "Song " + std::to_string(seed) + " (" + keyName + ")";
    best.key = rs.key;
    best.scale = int(rs.scale);
    applyMix(best, s, rs);   // Raumklang: Hall & Echo passend zur Musik
    if (progress) *progress = 1.f;

    if (info) {
        info->seed = seed;
        info->key = keyName;
        info->bpm = rs.bpm;
        info->structure = structureText(rs.structure);
        info->orchStyleIndex = rs.orchStyle;
        info->formName = rs.formIndex >= 0 ? formDefs()[size_t(rs.formIndex)].name : "Eigene Form";
        info->drumStyleIndex = int(rs.drumStyle);
        info->keyChangeAt = rs.keyChangeAt;
        info->keyChangeSemis = rs.keyChangeSemis;
        info->bestTry = bestTry;
        info->progressions = bestProg;
        info->systems = bestSys;
        info->drums = s.withDrums ? drumStyleName(rs.drumStyle) : "aus";
        info->orchStyle = s.ensemble == Ensemble::Orchestra ? orchProfiles()[size_t(rs.orchStyle)].name : "";
        info->techniques = bestTech;
        info->notes.clear();
        if (rs.keyChangeAt >= 0)
            info->notes = "Rückung um " + std::to_string(rs.keyChangeSemis) + " Halbton/-töne im letzten Refrain";
        info->score = bestScore;
        info->candidatesEvaluated = evaluated;
    }
    return best;
}

// ---------------------------------------------------------------------
//  Akkordsymbole lesen ("Am", "F#m7", "Bbmaj7", "Gsus4", "Hdim", "C/E")
//  Deutsche Schreibweise: H = h, B = b (Bb). Englisch "Bb" geht auch.
// ---------------------------------------------------------------------
bool parseChordSymbol(const std::string& text, int& rootPc, std::vector<int>& intervals) {
    if (text.empty()) return false;
    static const int base[] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
    const char c0 = char(std::toupper(static_cast<unsigned char>(text[0])));
    int root;
    if (c0 == 'H') root = 11;
    else if (c0 >= 'A' && c0 <= 'G') root = (c0 == 'B') ? 10 : base[c0 - 'A'];
    else return false;
    size_t p = 1;
    if (p < text.size() && (text[p] == '#' || text[p] == 'b')) {
        // "Bb" (englisch) = B (deutsch) – nicht doppelt erniedrigen
        if (!(c0 == 'B' && text[p] == 'b')) root += text[p] == '#' ? 1 : -1;
        ++p;
    } else if (p + 1 < text.size() && (text.compare(p, 2, "is") == 0 || text.compare(p, 2, "es") == 0)) {
        root += text[p] == 'i' ? 1 : -1;   // deutsch: Fis, Es …
        p += 2;
    }
    std::string q = text.substr(p);
    const size_t slash = q.find('/');
    if (slash != std::string::npos) q = q.substr(0, slash);   // Basston ignorieren (wählt der Generator)
    std::string ql;
    for (char ch : q) ql += char(std::tolower(static_cast<unsigned char>(ch)));
    auto has = [&](const char* s) { return ql.find(s) != std::string::npos; };
    std::vector<int> iv = {0, 4, 7};
    const bool maj = has("maj") || has("j7") || q.find('M') != std::string::npos;
    const bool minor = !maj && (ql.rfind("m", 0) == 0 || ql.rfind("-", 0) == 0) && ql.rfind("maj", 0) != 0;
    if (minor) iv = {0, 3, 7};
    if (has("dim") || has("°")) iv = has("7") ? std::vector<int>{0, 3, 6, 9} : std::vector<int>{0, 3, 6};
    else if (has("m7b5") || has("ø")) iv = {0, 3, 6, 10};
    else if (has("aug") || has("+")) iv = {0, 4, 8};
    else if (has("sus2")) iv = {0, 2, 7};
    else if (has("sus")) iv = {0, 5, 7};
    if (!has("dim") && !has("m7b5") && !has("ø")) {
        if (maj && (has("7") || has("maj"))) iv.push_back(11);
        else if (has("7") || has("9")) iv.push_back(10);
        else if (has("6")) iv.push_back(9);
    }
    rootPc = mod12(root);
    intervals = iv;
    return true;
}

// ---------------------------------------------------------------------
//  Abschnitte austauschen ("Abschnitt neu komponieren", "Energie ändern")
// ---------------------------------------------------------------------
void spliceSections(Song& target, const Song& source, const std::vector<int>& sections) {
    const Tick tol = target.ppq / 8;   // gespielte Noten dürfen etwas vor der Grenze beginnen (Humanize)
    for (int si : sections) {
        if (si < 0 || si >= int(target.sections.size()) || si >= int(source.sections.size())) continue;
        const Tick a = target.sections[size_t(si)].start;
        const Tick b = a + target.sections[size_t(si)].length;
        const bool lastSec = si + 1 == int(target.sections.size());
        const Tick wa = a - tol, wb = lastSec ? INT64_MAX / 4 : b - tol;
        for (auto& tr : target.tracks) {
            auto& ns = tr.notes;
            ns.erase(std::remove_if(ns.begin(), ns.end(), [&](const Note& n) { return n.start >= wa && n.start < wb; }), ns.end());
            for (auto& n : ns)
                if (n.start < wa && n.end() > a) n.length = std::max<Tick>(1, a - n.start);
            // Lautstärke-Verlauf: alten Wert an der hinteren Grenze merken, Bereich leeren
            auto& ex = tr.expression;
            if (!ex.empty()) {
                const float atA = tr.expressionAt(double(a - 1)), atB = tr.expressionAt(double(b));
                ex.erase(std::remove_if(ex.begin(), ex.end(), [&](const std::pair<Tick, float>& p) { return p.first >= a - 1 && p.first <= b; }), ex.end());
                if (a > 0) ex.push_back({a - 1, atA});
                if (!lastSec) ex.push_back({b, atB});
            }
        }
        for (const auto& st : source.tracks) {
            Track* tt = nullptr;
            for (auto& tr : target.tracks) if (tr.name == st.name) { tt = &tr; break; }
            if (!tt) {
                Track nt{st.name, st.instrument};
                nt.volume = st.volume;
                target.tracks.push_back(nt);
                tt = &target.tracks.back();
            }
            for (const auto& n : st.notes)
                if (n.start >= wa && n.start < wb) {
                    Note x = n;
                    if (!lastSec && x.end() > b) x.length = std::max<Tick>(1, b - x.start);
                    tt->notes.push_back(x);
                }
            if (!st.expression.empty() || !tt->expression.empty()) {
                if (tt->expression.empty()) {   // bisher gleichbleibend: außerhalb des Abschnitts bei 1 bleiben
                    if (a > 0) tt->expression.push_back({a - 1, 1.f});
                    if (!lastSec) tt->expression.push_back({b, 1.f});
                }
                tt->expression.push_back({a, st.expressionAt(double(a))});
                for (const auto& p : st.expression)
                    if (p.first > a && p.first < b) tt->expression.push_back(p);
                if (!lastSec) tt->expression.push_back({b - 1, st.expressionAt(double(b - 1))});
            }
        }
        // Dynamikzeichen des Abschnitts übernehmen
        auto& dm = target.dynamics;
        dm.erase(std::remove_if(dm.begin(), dm.end(), [&](const DynamicMark& d) { return d.start >= a && d.start < b; }), dm.end());
        for (const auto& d : source.dynamics)
            if (d.start >= a && d.start < b) dm.push_back(d);
    }
    for (auto& tr : target.tracks) {
        std::sort(tr.notes.begin(), tr.notes.end(), [](const Note& x, const Note& y) { return x.start < y.start; });
        std::stable_sort(tr.expression.begin(), tr.expression.end(),
                         [](const std::pair<Tick, float>& x, const std::pair<Tick, float>& y) { return x.first < y.first; });
    }
    std::sort(target.dynamics.begin(), target.dynamics.end(), [](const DynamicMark& x, const DynamicMark& y) { return x.start < y.start; });
    // Spuren ohne Noten entfernen
    target.tracks.erase(std::remove_if(target.tracks.begin(), target.tracks.end(), [](const Track& t) { return t.notes.empty(); }),
                        target.tracks.end());
}
