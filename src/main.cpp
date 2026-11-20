#include "audio/AudioEngine.h"
#include "core/Song.h"
#include "instruments/InstrumentRegistry.h"
#include "generator/Generator.h"
#include "analysis/Analyzer.h"
#include "io/MidiInput.h"
#include "io/SongIO.h"
#include "ui/FileDialog.h"
#include "ui/GeneratorPanel.h"
#include "ui/LivePlayer.h"
#include "ui/ScoreView.h"
#include "ui/Views.h"
#include "ui/PianoRoll.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder (Standard-Layout)
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// Kleiner Start-Song, damit man sofort etwas hört.
static Song makeDemoSong() {
    Song s;
    s.title = "Demo";
    s.bpm = 100;
    const Tick q = s.ppq, e = q / 2;

    Track melody{"Melodie", "Piano"};
    int mel[] = {64, 67, 72, 71, 69, 67, 64, 62, 60, 64, 67, 69, 67, 64, 62, 60};
    for (int i = 0; i < 16; ++i) melody.notes.push_back({mel[i], i * q, i % 4 == 3 ? q : e + e / 2, 0.75f});

    Track chords{"Begleitung", "Piano"};
    chords.volume = 0.6f;
    int prog[][4] = {{48, 55, 60, 64}, {45, 52, 57, 60}, {41, 48, 53, 57}, {43, 50, 55, 59}};
    for (int bar = 0; bar < 4; ++bar)
        for (int p : prog[bar]) chords.notes.push_back({p, s.bar(bar), s.ticksPerBar(), 0.55f});

    s.tracks = {melody, chords};
    return s;
}

// Einfache Undo/Redo-Historie über Song-Kopien
struct History {
    std::vector<Song> undo, redo;
    void push(const Song& s) {
        undo.push_back(s);
        if (undo.size() > 200) undo.erase(undo.begin());
        redo.clear();
    }
    bool doUndo(Song& s) {
        if (undo.empty()) return false;
        redo.push_back(s); s = undo.back(); undo.pop_back(); return true;
    }
    bool doRedo(Song& s) {
        if (redo.empty()) return false;
        undo.push_back(s); s = redo.back(); redo.pop_back(); return true;
    }
};

struct SnapOption { const char* label; int num, den; bool triplet; };
static const SnapOption kSnaps[] = {
    {"1/4", 1, 4, false}, {"1/8", 1, 8, false}, {"1/16", 1, 16, false}, {"1/32", 1, 32, false},
    {"1/8 Triole", 1, 8, true}, {"1/16 Triole", 1, 16, true},
};

static Tick snapToTicks(const SnapOption& o, int ppq) {
    Tick t = Tick(ppq) * 4 * o.num / o.den;
    return o.triplet ? t * 2 / 3 : t;
}

static void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.FrameRounding = 4; st.GrabRounding = 4; st.WindowRounding = 0;
    st.FramePadding = {8, 5}; st.ItemSpacing = {8, 6};
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = {0.11f, 0.115f, 0.13f, 1};
    c[ImGuiCol_FrameBg] = {0.18f, 0.19f, 0.22f, 1};
    c[ImGuiCol_Button] = {0.22f, 0.24f, 0.29f, 1};
    c[ImGuiCol_ButtonHovered] = {0.30f, 0.38f, 0.55f, 1};
    c[ImGuiCol_ButtonActive] = {0.35f, 0.47f, 0.72f, 1};
}

// Kommandozeile:  MusicEngine.exe --generate song.wav [seed] [gründlichkeit]
static int generateCli(int argc, char** argv) {
    GeneratorSettings gs;
    if (argc > 3) gs.seed = uint32_t(std::stoul(argv[3]));
    if (argc > 4) gs.quality = std::stoi(argv[4]);
    if (argc > 5 && std::string(argv[5]) == "orch") gs.ensemble = Ensemble::Orchestra;
    if (argc > 5 && std::string(argv[5]) == "epicband") { gs.drums = DrumStyle::Epic; gs.chordInstrument = "Chor"; gs.melodyInstrument = "Streicher"; gs.energy = 0.9f; }
    if (argc > 6) gs.orchStyle = std::stoi(argv[6]);
    GenerationInfo info;
    auto t0 = std::chrono::steady_clock::now();
    Song song = generateSong(gs, &info);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Seed %u | %s | %.0f BPM | %.1f s Musik\nForm: %s\n", info.seed, info.key.c_str(), info.bpm,
                song.durationSeconds(), info.structure.c_str());
    for (auto& p : info.progressions) std::printf("  %s\n", p.c_str());
    if (!info.notes.empty()) std::printf("  %s\n", info.notes.c_str());
    if (!info.orchStyle.empty()) {
        std::printf("Orchester-Stil: %s\n", info.orchStyle.c_str());
        for (auto& t : info.techniques) std::printf("  - %s\n", t.c_str());
    }
    static const char* lv[6] = {"pp", "p", "mp", "mf", "f", "ff"};
    std::printf("Dynamik:");
    for (auto& d : song.dynamics)
        std::printf(" T%lld:%s", (long long)(d.start / song.ticksPerBar() + 1),
                    d.kind == DynamicMark::Level ? lv[d.level] : d.kind == DynamicMark::Crescendo ? "cresc" : "dim");
    std::printf("\n");
    std::printf("Schlagzeug: %s\nNoten:", info.drums.c_str());
    for (auto& t : song.tracks) std::printf(" %s %zu |", t.name.c_str(), t.notes.size());
    std::printf("\n");
    std::printf("%d Kandidaten bewertet in %.2f s, Note %.2f\n", info.candidatesEvaluated, secs, info.score);
    AudioEngine offline;
    offline.loadSong(song);
    if (!offline.renderToWav(argv[2])) { std::puts("WAV-Export fehlgeschlagen"); return 1; }
    std::printf("WAV: %s\n", argv[2]);
    return 0;
}

// Anteil der Sechzehntel mit harter Halbton-Reibung (beide Töne >= Achtel, ohne Schlagzeug)
static double clashRate(const Song& song, std::map<std::string, int>* pairs = nullptr, std::vector<Tick>* where = nullptr) {
    std::vector<bool> drum(song.tracks.size());
    for (size_t i = 0; i < song.tracks.size(); ++i) {
        auto inst = InstrumentRegistry::instance().create(song.tracks[i].instrument);
        drum[i] = inst && inst->isDrumKit();
    }
    const Tick step = song.ppq / 4, minLen = song.ppq / 2;
    int steps = 0, bad = 0;
    for (Tick t = 0; t < song.length(); t += step) {
        std::vector<std::pair<int, size_t>> snd;
        for (size_t i = 0; i < song.tracks.size(); ++i) {
            if (drum[i]) continue;
            for (auto& n : song.tracks[i].notes)
                if (n.start <= t + step / 2 && n.end() > t + step / 2 && n.length >= minLen) snd.push_back({n.pitch, i});
        }
        ++steps;
        bool b = false;
        for (size_t a = 0; a < snd.size(); ++a)
            for (size_t c = a + 1; c < snd.size(); ++c) {
                const int ic = ((snd[a].first - snd[c].first) % 12 + 12) % 12;
                if (ic != 1 && ic != 11) continue;
                b = true;
                if (pairs) {
                    std::string x = song.tracks[snd[a].second].name, y = song.tracks[snd[c].second].name;
                    if (y < x) std::swap(x, y);
                    (*pairs)[x + " <-> " + y]++;
                }
            }
        if (b) { ++bad; if (where && where->size() < 12) where->push_back(t); }
    }
    return 100.0 * bad / std::max(1, steps);
}

// Alle Stimmungsvorlagen durchprobieren:  MusicEngine.exe --presets [Songs pro Vorlage]
static int presetsCli(int argc, char** argv) {
    const int per = argc > 2 ? std::max(1, std::atoi(argv[2])) : 2;
    const int only = argc > 3 ? std::atoi(argv[3]) : -1;   // nur diese Vorlage (mit Reibungs-Paaren)
    double total = 0;
    int count = 0;
    for (int p = 0; p < GeneratorPanel::presetCount(); ++p) {
        if (only >= 0 && p != only) continue;
        for (int k = 0; k < per; ++k) {
            std::mt19937 rng(uint32_t(7000 + p * 37 + k));
            GeneratorSettings gs;
            bool randomBpm = false;
            float bpm = 110;
            int meter = 0;
            GeneratorPanel::applyPresetTo(gs, p, rng, randomBpm, bpm, meter);
            gs.bpm = randomBpm ? 0.0 : double(bpm);
            gs.beatsPerBar = meter == 1 ? 3 : 4;
            gs.seed = uint32_t(100 + p * 13 + k * 7);
            gs.quality = 3;
            GenerationInfo info;
            auto t0 = std::chrono::steady_clock::now();
            Song song = generateSong(gs, &info);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            size_t notes = 0;
            for (auto& t : song.tracks) notes += t.notes.size();
            std::map<std::string, int> pairs;
            std::vector<Tick> where;
            const double cr = clashRate(song, &pairs, &where);
            total += cr;
            ++count;
            std::printf("%-14s | %-16s %3.0f BPM %d/4 | %5.0f s Musik | %5zu Noten | Reibung %4.1f %% | %.1f s | %s\n",
                        GeneratorPanel::presetName(p), info.key.c_str(), info.bpm, song.beatsPerBar, song.durationSeconds(),
                        notes, cr, secs, info.structure.c_str());
            if (only >= 0 && argc > 4) {   // Noten eines Takts ausgeben (Fehlersuche)
                const Tick b0 = song.bar(std::atoi(argv[4]) - 1), b1 = song.bar(std::atoi(argv[4]));
                for (auto& tr : song.tracks)
                    for (auto& n : tr.notes)
                        if (n.start < b1 && n.end() > b0 && n.length >= song.ppq / 2)
                            std::printf("        %-14s %s%d  Start %lld  Länge %lld\n", tr.name.c_str(), noteName(n.pitch), n.pitch / 12 - 1, (long long)(n.start - b0), (long long)n.length);
            }
            if (only >= 0) {
                std::vector<std::pair<int, std::string>> top;
                for (auto& [kk, v] : pairs) top.push_back({v, kk});
                std::sort(top.rbegin(), top.rend());
                for (size_t i = 0; i < top.size() && i < 6; ++i) std::printf("      %4d  %s\n", top[i].first, top[i].second.c_str());
                for (auto& tq : info.techniques) std::printf("      - %s\n", tq.c_str());
                for (Tick wt : where)
                    std::printf("      Reibung bei Takt %lld, Schlag %.2f\n", (long long)(wt / song.ticksPerBar() + 1),
                                double(wt % song.ticksPerBar()) / double(song.ticksPerBeat()) + 1.0);
            }
        }
    }
    std::printf("Durchschnittliche Reibung: %.2f %% (%d Songs)\n", total / std::max(1, count), count);
    return 0;
}

// Song offline rendern -> Stereo-Samples (48 kHz)
static std::vector<float> renderSamples(const Song& song, double tail) {
    AudioEngine e;
    e.loadSong(song);
    const std::string path = "selftest_render.wav";
    e.renderToWav(path, tail);
    std::vector<unsigned char> buf;
    {
        std::ifstream f(path, std::ios::binary);
        buf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::remove(path.c_str());
    std::vector<float> smp;
    size_t p = 12;
    while (p + 8 <= buf.size() && std::memcmp(&buf[p], "data", 4) != 0) {
        uint32_t sz;
        std::memcpy(&sz, &buf[p + 4], 4);
        p += 8 + sz;
    }
    for (p += 8; p + 4 <= buf.size(); p += 4) {
        float v;
        std::memcpy(&v, &buf[p], 4);
        smp.push_back(v);
    }
    return smp;
}

// Wahrgenommene Lautheit (vereinfachte K-Gewichtung wie bei LUFS: Tiefen leiser, Höhen ab 1,5 kHz +4 dB), in dB
static double perceivedDb(const std::vector<float>& s) {
    Biquad hp, sh;
    hp.highpass(60.f, 0.5f, 48000.f);
    sh.shelf(true, 1500.f, 4.f, 48000.f);
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

// Mischverhältnis messen:  MusicEngine.exe --balance
// Rendert jede Spur einzeln und zeigt, wie laut sie (wahrgenommen) im Vergleich zur Melodie ist.
static int balanceCli() {
    struct Case { const char* name; GeneratorSettings gs; };
    std::vector<Case> cases;
    {
        GeneratorSettings g; g.seed = 801; g.quality = 1; g.lengthSeconds = 60;
        g.melodyInstrument = "Glocken"; g.chordInstrument = "Piano"; g.bassInstrument = "Kontrabass";
        cases.push_back({"Band, Glocken-Melodie", g});
    }
    {
        GeneratorSettings g; g.seed = 802; g.quality = 1; g.lengthSeconds = 60;
        cases.push_back({"Band, Standard", g});
    }
    {
        GeneratorSettings g; g.seed = 803; g.quality = 1; g.lengthSeconds = 60; g.ensemble = Ensemble::Orchestra; g.energy = 0.75f;
        cases.push_back({"Orchester", g});
    }
    for (auto& c : cases) {
        Song s = generateSong(c.gs);
        std::printf("%s:\n", c.name);
        std::vector<std::pair<std::string, double>> lv;
        double ref = -999;
        for (size_t i = 0; i < s.tracks.size(); ++i) {
            if (s.tracks[i].notes.empty()) continue;
            Song solo = s;
            for (size_t j = 0; j < solo.tracks.size(); ++j) solo.tracks[j].muted = j != i;
            solo.fx.on = false;
            solo.fx.mastering = false;
            // nur dort messen, wo die Spur spielt
            Tick a = s.tracks[i].notes.front().start, b = 0;
            for (auto& n : s.tracks[i].notes) b = std::max(b, n.end());
            auto smp = renderSamples(solo, 0.2);
            const size_t ia = std::min(smp.size(), size_t(s.ticksToSeconds(double(a)) * 48000.0) * 2);
            const size_t ib = std::min(smp.size(), size_t(s.ticksToSeconds(double(b)) * 48000.0) * 2);
            std::vector<float> part(smp.begin() + std::ptrdiff_t(ia), smp.begin() + std::ptrdiff_t(std::max(ia, ib)));
            const double db = perceivedDb(part);
            lv.push_back({s.tracks[i].name + " (" + s.tracks[i].instrument + ")", db});
            if (s.tracks[i].name == "Melodie" || (ref < -900 && s.tracks[i].name.rfind("Violinen I", 0) == 0)) ref = db;
        }
        if (ref < -900 && !lv.empty()) ref = lv.front().second;
        for (auto& [n, db] : lv) std::printf("   %-36s %+6.1f dB\n", n.c_str(), db - ref);
        // Letzter Refrain: jede Spur im Verhältnis zum ganzen Mix (wer sticht heraus, wer trägt bei?)
        Tick wa = 0, wb = 0;
        for (auto& sec : s.sections)
            if (sec.name.rfind("Refrain", 0) == 0) { wa = sec.start; wb = sec.start + sec.length; }
        if (wb > wa) {
            auto window = [&](const Song& song) {
                auto smp = renderSamples(song, 0.2);
                const size_t ia = std::min(smp.size(), size_t(s.ticksToSeconds(double(wa)) * 48000.0) * 2);
                const size_t ib = std::min(smp.size(), size_t(s.ticksToSeconds(double(wb)) * 48000.0) * 2);
                return perceivedDb(std::vector<float>(smp.begin() + std::ptrdiff_t(ia), smp.begin() + std::ptrdiff_t(std::max(ia, ib))));
            };
            Song all = s;
            all.fx.on = false;
            all.fx.mastering = false;
            const double mix = window(all);
            std::vector<std::pair<double, std::string>> rows;
            for (size_t i = 0; i < s.tracks.size(); ++i) {
                Song solo = all;
                for (size_t j = 0; j < solo.tracks.size(); ++j) solo.tracks[j].muted = j != i;
                const double db = window(solo);
                if (db > -150) rows.push_back({db - mix, s.tracks[i].name});
            }
            std::sort(rows.rbegin(), rows.rend());
            std::printf("   -- letzter Refrain, relativ zum Gesamtmix (%zu Stimmen spielen):\n", rows.size());
            for (auto& [d, n] : rows) std::printf("      %-30s %+6.1f dB\n", n.c_str(), d);
        }
    }
    return 0;
}

// Mastering messen:  MusicEngine.exe --master
static int masterCli() {
    struct M { double rmsDb, peak, crestDb, shortRangeDb; };
    auto measure = [](const std::vector<float>& s) {
        double sum = 0, peak = 0;
        for (float v : s) { sum += double(v) * v; peak = std::max(peak, double(std::abs(v))); }
        const double rms = std::sqrt(sum / double(std::max<size_t>(1, s.size())));
        // Dynamik: Spanne der 400-ms-Lautheit (10. bis 90. Perzentil)
        std::vector<double> blocks;
        const size_t bl = 48000 * 2 * 4 / 10;
        for (size_t a = 0; a + bl <= s.size(); a += bl) {
            double b = 0;
            for (size_t i = a; i < a + bl; ++i) b += double(s[i]) * s[i];
            const double r = std::sqrt(b / double(bl));
            if (r > 1e-4) blocks.push_back(20 * std::log10(r));
        }
        std::sort(blocks.begin(), blocks.end());
        const double range = blocks.size() > 10 ? blocks[blocks.size() * 9 / 10] - blocks[blocks.size() / 10] : 0;
        return M{20 * std::log10(std::max(rms, 1e-9)), peak, 20 * std::log10(std::max(peak, 1e-9) / std::max(rms, 1e-9)), range};
    };
    const char* names[] = {"Band", "Orchester", "Ballade", "Dance"};
    for (int k = 0; k < 4; ++k) {
        GeneratorSettings gs;
        gs.seed = uint32_t(310 + k);
        gs.quality = 2;
        gs.lengthSeconds = 60;
        if (k == 1) gs.ensemble = Ensemble::Orchestra;
        if (k == 2) { gs.energy = 0.3f; gs.withDrums = false; }
        if (k == 3) { gs.energy = 0.85f; gs.drums = DrumStyle::FourOnFloor; }
        Song s = generateSong(gs);
        std::printf("%s:\n", names[k]);
        for (int mode = 0; mode < 3; ++mode) {
            Song t = s;
            t.fx.mastering = mode > 0;
            t.fx.masterAmount = mode == 1 ? 0.5f : 1.f;
            const M m = measure(renderSamples(t, 1.0));
            std::printf("   %-22s RMS %6.1f dB   Spitze %.2f   Crest %4.1f dB   Dynamik-Spanne %4.1f dB\n",
                        mode == 0 ? "ohne Mastering" : mode == 1 ? "Mastering 50 %" : "Mastering 100 %", m.rmsDb, m.peak, m.crestDb,
                        m.shortRangeDb);
        }
    }
    return 0;
}

// Reibung nur in einem Zeitbereich (gleiche Regeln wie clashRate)
static double clashRateRange(const Song& song, Tick from, Tick to) {
    std::vector<bool> drum(song.tracks.size());
    for (size_t i = 0; i < song.tracks.size(); ++i) {
        auto inst = InstrumentRegistry::instance().create(song.tracks[i].instrument);
        drum[i] = inst && inst->isDrumKit();
    }
    const Tick step = song.ppq / 4, minLen = song.ppq / 2;
    int steps = 0, bad = 0;
    for (Tick t = from; t < to; t += step) {
        std::vector<int> snd;
        const Tick m = t + step / 2;
        for (size_t i = 0; i < song.tracks.size(); ++i) {
            if (drum[i]) continue;
            for (auto& n : song.tracks[i].notes)
                if (n.start <= m && n.end() > m && n.length >= minLen) snd.push_back(n.pitch);
        }
        ++steps;
        bool b = false;
        for (size_t a = 0; a < snd.size() && !b; ++a)
            for (size_t c = a + 1; c < snd.size(); ++c) {
                const int ic = ((snd[a] - snd[c]) % 12 + 12) % 12;
                if (ic == 1 || ic == 11) { b = true; break; }
            }
        if (b) ++bad;
    }
    return 100.0 * bad / std::max(1, steps);
}

// Kompositions-Systeme prüfen:  MusicEngine.exe --systems
static int systemsCli() {
    struct Stat { double clash = 0; int n = 0; };
    std::map<std::string, Stat> stats;   // pro System: Reibung in den Abschnitten, die es nutzen
    std::set<std::string> combos;
    int songs = 0;
    double total = 0;
    for (int i = 0; i < 60; ++i) {
        GeneratorSettings gs;
        gs.seed = uint32_t(20000 + i * 31);
        gs.quality = 2;
        gs.lengthSeconds = 90;
        gs.systemVariety = i % 3 == 0 ? 1.f : 0.6f;
        gs.energy = 0.3f + 0.5f * float(i % 4) / 3.f;
        gs.syncopation = 0.2f + 0.5f * float(i % 5) / 4.f;
        if (i % 5 == 4) gs.ensemble = Ensemble::Orchestra;
        if (i % 7 == 3) gs.scale = int(ScaleType::Dorian);
        if (i % 7 == 5) gs.scale = int(ScaleType::Mixolydian);
        GenerationInfo info;
        Song s = generateSong(gs, &info);
        ++songs;
        total += clashRate(s);
        // Abschnittsart -> Systeme
        std::map<std::string, std::vector<std::string>> bySec;
        std::string combo;
        for (auto& line : info.systems) {
            const size_t c = line.find(": ");
            if (c == std::string::npos) continue;
            const std::string sec = line.substr(0, c);
            std::vector<std::string> parts;
            std::string rest = line.substr(c + 2);
            size_t p;
            while ((p = rest.find(" \xC2\xB7 ")) != std::string::npos) { parts.push_back(rest.substr(0, p)); rest = rest.substr(p + 4); }
            parts.push_back(rest);
            bySec[sec] = parts;
            combo += line + " | ";
        }
        combos.insert(combo);
        for (auto& sec : s.sections) {
            for (auto& [name, parts] : bySec)
                if (sec.name.rfind(name, 0) == 0) {
                    const double cr = clashRateRange(s, sec.start, sec.start + sec.length);
                    for (auto& sy : parts) { stats[sy].clash += cr; stats[sy].n++; }
                    break;
                }
        }
    }
    std::printf("%d Songs, %zu verschiedene System-Kombinationen, durchschnittliche Reibung %.2f %%\n\n", songs, combos.size(), total / songs);
    std::printf("%-26s %8s %10s\n", "System", "Abschn.", "Reibung");
    int fails = 0;
    for (auto& [name, st] : stats) {
        const double avg = st.clash / std::max(1, st.n);
        std::printf("%-26s %8d %9.2f %%%s\n", name.c_str(), st.n, avg, avg > 1.5 ? "  <- zu hoch" : "");
        if (avg > 1.5) ++fails;
    }
    const char* must[] = {"Quintfall", "Modaler Vamp", "Orgelpunkt", "Andalusische Kadenz", "Line Clich\xC3\xA9", "Chromatische Mediante",
                          "Plagal (Gospel)", "Kanon-Folge", "Tresillo (3-3-2)", "Clave (3-2)", "Offbeat (Reggae)", "Bossa",
                          "Treibende Achtel", "Pentatonik-Hook", "Riff", "Arpeggio-Thema", "Choral"};
    for (auto m : must)
        if (!stats.count(m)) { std::printf("Nie gewählt: %s\n", m); ++fails; }
    std::printf("%s\n", fails ? "Es gab Auffälligkeiten." : "Alle Systeme kommen vor und klingen sauber.");
    return fails ? 1 : 0;
}

// Songformen prüfen:  MusicEngine.exe --forms
static int formsCli() {
    int fails = 0;
    auto drumNotesIn = [](const Song& s, const Section& sec) {
        size_t n = 0;
        for (auto& t : s.tracks)
            if (t.name == "Schlagzeug")
                for (auto& x : t.notes) if (x.start >= sec.start + 60 && x.start < sec.start + sec.length - 60) ++n;
        return n;
    };
    for (int ens = 0; ens < 2; ++ens) {
        std::printf("--- %s ---\n", ens ? "Orchester" : "Band");
        for (int f = 0; f < songFormCount(); ++f) {
            GeneratorSettings gs;
            gs.seed = uint32_t(900 + f * 11 + ens);
            gs.quality = 2;
            gs.form = f;
            gs.ensemble = ens ? Ensemble::Orchestra : Ensemble::Band;
            GenerationInfo info;
            Song s = generateSong(gs, &info);
            bool ok = !s.sections.empty() && s.length() > 0;
            std::string notes;
            for (size_t k = 0; k < s.sections.size(); ++k) {
                const auto& sec = s.sections[k];
                if (sec.name.find("(Breakdown)") != std::string::npos && ens == 0) {
                    const size_t d = drumNotesIn(s, sec);
                    notes += " Breakdown-Drums=" + std::to_string(d);
                    ok = ok && d == 0;
                }
            }
            // Ausblenden: Lautstärke am Ende fast 0?
            if (info.structure.find('>') != std::string::npos) {
                float mx = 0;
                const Tick end = s.length();
                for (auto& t : s.tracks) mx = std::max(mx, t.expressionAt(double(end - s.ppq)));
                notes += " Ende-Lautstärke=" + std::to_string(mx).substr(0, 4);
                ok = ok && mx < 0.15f;
            }
            const double cr = clashRate(s);
            std::printf("%-30s %4.0f s  %-48s Reibung %.1f %%%s %s\n", songFormName(f), s.durationSeconds(),
                        info.structure.c_str(), cr, notes.c_str(), ok ? "OK" : "FEHLER");
            if (!ok) ++fails;
        }
    }
    // Vielfalt: 40 Songs mit Standard-Einstellungen
    std::map<std::string, int> forms;
    std::set<std::string> structures;
    for (int i = 0; i < 40; ++i) {
        GeneratorSettings gs;
        gs.seed = uint32_t(5000 + i * 7);
        gs.quality = 1;
        gs.energy = 0.3f + 0.5f * float(i % 5) / 4.f;
        if (i % 4 == 1) gs.drums = DrumStyle::Jazz;
        if (i % 4 == 2) gs.drums = DrumStyle::FourOnFloor;
        if (i % 6 == 0) gs.ensemble = Ensemble::Orchestra;
        GenerationInfo info;
        generateSong(gs, &info);
        forms[info.formName]++;
        structures.insert(info.structure);
    }
    std::printf("\nVielfalt bei 40 Songs: %zu verschiedene Songformen, %zu verschiedene Abläufe\n", forms.size(), structures.size());
    for (auto& [n, c] : forms) std::printf("   %2d x %s\n", c, n.c_str());
    if (forms.size() < 8) ++fails;
    std::printf("%s\n", fails ? "Es gab Fehler." : "Alle Songformen OK.");
    return fails ? 1 : 0;
}

// Neue Funktionen prüfen:  MusicEngine.exe --features
static int featuresCli() {
    int fails = 0;
    auto check = [&](bool ok, const std::string& what) {
        std::printf("%-58s %s\n", what.c_str(), ok ? "OK" : "FEHLER");
        if (!ok) ++fails;
    };
    auto notesIn = [](const Song& s, Tick a, Tick b) {
        std::vector<std::tuple<std::string, Tick, int>> v;
        for (auto& t : s.tracks)
            for (auto& n : t.notes)
                if (n.start >= a && n.start < b) v.push_back({t.name, n.start, n.pitch});
        std::sort(v.begin(), v.end());
        return v;
    };

    // 1. Akkordsymbole
    {
        int r = 0;
        std::vector<int> iv;
        bool ok = parseChordSymbol("Am", r, iv) && r == 9 && iv == std::vector<int>{0, 3, 7};
        ok = ok && parseChordSymbol("F#m7", r, iv) && r == 6 && iv == std::vector<int>{0, 3, 7, 10};
        ok = ok && parseChordSymbol("Bbmaj7", r, iv) && r == 10 && iv == std::vector<int>{0, 4, 7, 11};
        ok = ok && parseChordSymbol("H7", r, iv) && r == 11 && iv == std::vector<int>{0, 4, 7, 10};
        ok = ok && parseChordSymbol("Fis", r, iv) && r == 6 && iv == std::vector<int>{0, 4, 7};
        ok = ok && parseChordSymbol("Esus4", r, iv) && r == 4 && iv == std::vector<int>{0, 5, 7};
        ok = ok && parseChordSymbol("Hdim", r, iv) && r == 11 && iv == std::vector<int>{0, 3, 6};
        ok = ok && parseChordSymbol("C/E", r, iv) && r == 0 && iv == std::vector<int>{0, 4, 7};
        ok = ok && !parseChordSymbol("X7", r, iv);
        check(ok, "Akkordsymbole lesen (Am, F#m7, Bbmaj7, H7, Fis, Esus4 ...)");
    }
    // 2. Eigene Akkordfolge
    {
        GeneratorSettings gs;
        gs.seed = 4242; gs.quality = 2;
        gs.chordsVerse = "Am F C G";
        gs.chordsChorus = "F G Am Am";
        GenerationInfo info;
        Song s = generateSong(gs, &info);
        bool ok = info.key.find("A Moll") != std::string::npos || info.key.find("C Dur") != std::string::npos;
        bool verseOk = false;
        for (auto& p : info.progressions) if (p.find("Strophe:  Am  F  C  G") != std::string::npos) verseOk = true;
        std::printf("      Tonart erkannt: %s\n", info.key.c_str());
        for (auto& p : info.progressions) std::printf("      %s\n", p.c_str());
        check(ok && verseOk, "Eigene Akkordfolge (Strophe Am F C G)");
        std::printf("      Reibung %.2f %%\n", clashRate(s));
    }
    // 3. Instrumental-Solo
    {
        GeneratorSettings gs;
        gs.seed = 777; gs.quality = 2; gs.solo = true; gs.lengthSeconds = 150;
        GenerationInfo info;
        Song s = generateSong(gs, &info);
        size_t soloNotes = 0;
        for (auto& t : s.tracks) if (t.name == "Solo") soloNotes = t.notes.size();
        bool hasSec = false;
        for (auto& sec : s.sections) hasSec = hasSec || sec.name == "Solo";
        std::printf("      Form: %s  (%zu Solo-Noten)  Reibung %.2f %%\n", info.structure.c_str(), soloNotes, clashRate(s));
        check(soloNotes > 20 && hasSec && info.structure.find('L') != std::string::npos, "Instrumental-Solo (eigene Spur + Abschnitt)");
        GeneratorSettings go = gs;
        go.ensemble = Ensemble::Orchestra; go.seed = 778;
        Song so = generateSong(go, &info);
        bool orchSec = false;
        for (auto& sec : so.sections) orchSec = orchSec || sec.name == "Solo";
        check(orchSec, "Instrumental-Solo im Orchester");
        std::printf("      Reibung Orchester %.2f %%\n", clashRate(so));
    }
    // 4. Groove (laid-back: Snare später als bei präzise)
    {
        GeneratorSettings gs;
        gs.seed = 99; gs.quality = 1; gs.humanize = 0.f; gs.drums = DrumStyle::Pop;
        auto snareOffset = [&](const Song& s) {
            double sum = 0; int n = 0;
            for (auto& t : s.tracks)
                if (t.name == "Schlagzeug")
                    for (auto& x : t.notes)
                        if (x.pitch == 38) { Tick o = x.start % 120; if (o > 60) o -= 120; sum += double(o); ++n; }
            return n ? sum / n : 0.0;
        };
        const double a = snareOffset(generateSong(gs));
        gs.groove = 1; gs.grooveAmount = 1.f;
        const double b = snareOffset(generateSong(gs));
        gs.groove = 2;
        const double c = snareOffset(generateSong(gs));
        std::printf("      Snare-Versatz: präzise %.1f, laid-back %.1f, treibend %.1f Ticks\n", a, b, c);
        check(b > a + 10 && c < a, "Groove-Gefühl (laid-back / treibend)");
    }
    // 5. Energie pro Abschnitt: gleiche Ideen, mehr Noten/Lautstärke
    {
        GeneratorSettings gs;
        gs.seed = 1234; gs.quality = 3;
        GenerationInfo info;
        Song a = generateSong(gs, &info);
        std::istringstream in(info.structure);
        std::vector<std::string> tok;
        std::string w;
        while (in >> w) tok.push_back(w);
        int k = -1;
        for (int i = 0; i < int(tok.size()); ++i) if (tok[size_t(i)][0] == 'V') { k = i; break; }
        if (k >= 0) {
            GeneratorSettings g2 = gs;
            tok[size_t(k)] += "++";
            g2.structure.clear();
            for (auto& t : tok) g2.structure += t + " ";
            g2.key = a.key; g2.scale = a.scale; g2.bpm = a.bpm; g2.beatsPerBar = a.beatsPerBar;
            g2.drums = DrumStyle(info.drumStyleIndex); g2.orchStyle = info.orchStyleIndex;
            g2.forceKeyChangeAt = info.keyChangeAt; g2.forceKeyChangeSemis = info.keyChangeSemis;
            g2.onlyTry = info.bestTry;
            GenerationInfo i2;
            Song b = generateSong(g2, &i2);
            const Tick s0 = a.sections[size_t(k)].start, s1 = s0 + a.sections[size_t(k)].length;
            // vorherige Abschnitte identisch?
            const bool before = notesIn(a, 0, s0 - 60) == notesIn(b, 0, s0 - 60);
            // Melodie im Abschnitt: gleiche Tonhöhen-Folge?
            auto mel = [&](const Song& s) {
                std::vector<int> v;
                for (auto& t : s.tracks) if (t.name == "Melodie") for (auto& n : t.notes) if (n.start >= s0 && n.start < s1) v.push_back(n.pitch);
                return v;
            };
            float va = 0, vb = 0;
            size_t na = 0, nb = 0;
            for (auto& t : a.tracks) for (auto& n : t.notes) if (n.start >= s0 && n.start < s1) { va += n.velocity; ++na; }
            for (auto& t : b.tracks) for (auto& n : t.notes) if (n.start >= s0 && n.start < s1) { vb += n.velocity; ++nb; }
            std::printf("      %s: Noten %zu -> %zu, mittlerer Anschlag %.2f -> %.2f, Melodie gleich: %s, davor gleich: %s\n",
                        a.sections[size_t(k)].name.c_str(), na, nb, va / float(std::max<size_t>(1, na)), vb / float(std::max<size_t>(1, nb)),
                        mel(a) == mel(b) ? "ja" : "nein", before ? "ja" : "nein");
            check(before && vb / float(std::max<size_t>(1, nb)) > va / float(std::max<size_t>(1, na)), "Energie pro Abschnitt (V8++): lauter, Rest gleich");
            // Einsetzen in den Original-Song
            Song merged = a;
            spliceSections(merged, b, {k});
            const bool restSame = notesIn(merged, 0, s0 - 60) == notesIn(a, 0, s0 - 60) &&
                                  notesIn(merged, s1 + 60, a.length() + 1) == notesIn(a, s1 + 60, a.length() + 1);
            const bool secNew = notesIn(merged, s0 + 60, s1 - 60) == notesIn(b, s0 + 60, s1 - 60);
            check(restSame && secNew, "Abschnitt einsetzen (spliceSections)");
            std::printf("      Reibung nach dem Einsetzen %.2f %%\n", clashRate(merged));
        } else check(false, "Energie pro Abschnitt: keine Strophe gefunden");
    }
    // 6. Abschnitt neu komponieren (alle Refrains, neuer Seed)
    {
        GeneratorSettings gs;
        gs.seed = 555; gs.quality = 3;
        GenerationInfo info;
        Song a = generateSong(gs, &info);
        GeneratorSettings g2 = gs;
        g2.structure = info.structure; g2.key = a.key; g2.scale = a.scale; g2.bpm = a.bpm; g2.beatsPerBar = a.beatsPerBar;
        g2.drums = DrumStyle(info.drumStyleIndex); g2.forceKeyChangeAt = info.keyChangeAt; g2.forceKeyChangeSemis = info.keyChangeSemis;
        g2.seed = 999;
        GenerationInfo i2;
        Song b = generateSong(g2, &i2);
        std::vector<int> choruses;
        for (int i = 0; i < int(a.sections.size()); ++i) if (a.sections[size_t(i)].name.rfind("Refrain", 0) == 0) choruses.push_back(i);
        Song merged = a;
        spliceSections(merged, b, choruses);
        bool sameSections = b.sections.size() == a.sections.size();
        for (size_t i = 0; sameSections && i < a.sections.size(); ++i) sameSections = a.sections[i].start == b.sections[i].start;
        bool changedAll = true, restSame = true;
        for (int i = 0; i < int(a.sections.size()); ++i) {
            const Tick s0 = a.sections[size_t(i)].start + 60, s1 = a.sections[size_t(i)].start + a.sections[size_t(i)].length - 60;
            const bool isCh = std::find(choruses.begin(), choruses.end(), i) != choruses.end();
            const bool same = notesIn(merged, s0, s1) == notesIn(a, s0, s1);
            if (isCh && same) changedAll = false;
            if (!isCh && !same) restSame = false;
        }
        check(sameSections && !choruses.empty() && changedAll && restSame,
              "Neu komponiert: " + std::to_string(choruses.size()) + " Refrains neu, Rest unverändert");
        std::printf("      Reibung vorher %.2f %%, nachher %.2f %%\n", clashRate(a), clashRate(merged));
    }
    // 7. Beschreibung
    {
        const char* prompts[] = {"episches Orchester, langsam, in d-moll, 3 min",
                                 "fröhlicher Sommer-Song mit Gitarre und Marimba, 120 bpm",
                                 "lofi chill, laid-back, mit Klavier, kurz",
                                 "gruseliger Thriller ohne Schlagzeug in Fis-Moll",
                                 "jazz mit Solo und Swing, E-Piano"};
        bool ok = true;
        for (auto p : prompts) {
            GeneratorSettings gs;
            std::mt19937 rng(1);
            bool rb = true;
            float bpm = 110;
            int meter = 0;
            const std::string fb = GeneratorPanel::applyPromptTo(gs, p, rng, rb, bpm, meter);
            std::printf("      \"%s\"\n         -> %s\n", p, fb.c_str());
            ok = ok && fb.rfind("Erkannt", 0) == 0;
        }
        GeneratorSettings gs;
        std::mt19937 rng(1);
        bool rb = true;
        float bpm = 110;
        int meter = 0;
        GeneratorPanel::applyPromptTo(gs, "episches Orchester, langsam, in d-moll, 3 min", rng, rb, bpm, meter);
        ok = ok && gs.ensemble == Ensemble::Orchestra && gs.key == 2 && gs.scale == int(ScaleType::NaturalMinor) &&
             gs.lengthSeconds == 180 && !rb && bpm < 90;
        check(ok, "Beschreibung in Worten -> Einstellungen");
    }
    // 8. Ansichten: Abschnitt-Werkzeuge
    {
        GeneratorSettings gs;
        gs.seed = 31337; gs.quality = 2;
        Song a = generateSong(gs);
        auto total = [](const Song& s) { size_t n = 0; for (auto& t : s.tracks) n += t.notes.size(); return n; };
        const size_t n0 = total(a);
        const Tick len0 = a.length();
        const int k = 1;
        const Tick L = a.sections[size_t(k)].length;
        size_t inSec = 0;
        for (auto& t : a.tracks)
            for (auto& n : t.notes)
                if (n.start >= a.sections[size_t(k)].start - a.ppq / 8 && n.start < a.sections[size_t(k)].start + L - a.ppq / 8) ++inSec;
        Song d = a;
        duplicateSection(d, k);
        const bool dupOk = d.sections.size() == a.sections.size() + 1 && total(d) == n0 + inSec &&
                           d.length() == len0 + L && d.sections[size_t(k) + 1].start == a.sections[size_t(k)].start + L &&
                           notesIn(d, a.sections[size_t(k)].start + L, a.sections[size_t(k)].start + 2 * L - 60).size() ==
                               notesIn(a, a.sections[size_t(k)].start, a.sections[size_t(k)].start + L - 60).size();
        check(dupOk, "Arrangement: Abschnitt duplizieren");
        deleteSection(d, k + 1);
        check(total(d) == n0 && d.length() == len0 && d.sections.size() == a.sections.size() &&
                  notesIn(d, 0, len0 + 1) == notesIn(a, 0, len0 + 1),
              "Arrangement: Kopie wieder löschen = Original");
        Song w = a;
        swapSections(w, 0);
        swapSections(w, 0);   // zweimal tauschen = zurück? (nur bei gleich langen Abschnitten exakt)
        const bool sameLen = a.sections[0].length == a.sections[1].length;
        check(total(w) == n0 && (!sameLen || notesIn(w, 0, len0 + 1) == notesIn(a, 0, len0 + 1)) && w.length() == len0,
              "Arrangement: Abschnitte tauschen");
        std::printf("      %s  (Abschnitt %s, %zu Noten)\n", "Duplizieren/Löschen/Tauschen geprüft", a.sections[size_t(k)].name.c_str(), inSec);
    }
    // 9. Akkordblatt: erkennt die vorgegebenen Akkorde wieder
    {
        GeneratorSettings gs;
        gs.seed = 2024; gs.quality = 2; gs.withMelody = false; gs.withDrums = false;
        gs.chordsVerse = "C Am F G";
        gs.chordsChorus = "F G C C";
        gs.key = 0; gs.scale = int(ScaleType::Major);
        gs.harmonicRhythm = 2;
        GenerationInfo info;
        Song s = generateSong(gs, &info);
        const auto ch = detectChords(s, s.ticksPerBar());
        int hits = 0, tested = 0;
        const char* verse[] = {"C", "Am", "F", "G"};
        for (auto& sec : s.sections) {
            if (sec.name.rfind("Strophe", 0) != 0) continue;
            for (int b = 0; b < int(sec.length / s.ticksPerBar()) && b < 8; ++b) {
                const size_t idx = size_t(sec.start / s.ticksPerBar()) + size_t(b);
                if (idx >= ch.size()) break;
                ++tested;
                // erlaubt: gleicher Grundton und Geschlecht (ein Vierklang-Zusatz zählt auch)
                const std::string want = verse[b % 4];
                const std::string got = ch[idx].symbol;
                if (got == want || got.rfind(want, 0) == 0) ++hits;
            }
        }
        std::printf("      %d von %d Strophen-Takten richtig erkannt\n", hits, tested);
        check(tested > 0 && hits * 100 >= tested * 85, "Akkordblatt: Akkorde erkennen (mind. 85 %)");
    }
    // 10. Mischpult: Solo, Panorama und Pegel in der Engine; Speichern/Laden
    {
        GeneratorSettings gs;
        gs.seed = 5; gs.quality = 1; gs.lengthSeconds = 30;
        Song s = generateSong(gs);
        auto renderRms = [&](const Song& song, float* left, float* right) {
            AudioEngine e;
            e.loadSong(song);
            const std::string path = "selftest_mix.wav";
            e.renderToWav(path, 0.5);
            // WAV lesen (Float 32, Stereo) – Kopf überspringen, "data" suchen
            std::vector<unsigned char> buf;
            {
                std::ifstream f(path, std::ios::binary);
                if (!f) return;
                buf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
            std::remove(path.c_str());
            size_t p = 12;
            while (p + 8 <= buf.size() && std::memcmp(&buf[p], "data", 4) != 0) {
                uint32_t sz;
                std::memcpy(&sz, &buf[p + 4], 4);
                p += 8 + sz;
            }
            p += 8;
            double l = 0, r = 0;
            size_t frames = 0;
            for (; p + 8 <= buf.size(); p += 8, ++frames) {
                float a, b;
                std::memcpy(&a, &buf[p], 4);
                std::memcpy(&b, &buf[p + 4], 4);
                l += double(a) * a;
                r += double(b) * b;
            }
            *left = float(std::sqrt(l / double(std::max<size_t>(1, frames))));
            *right = float(std::sqrt(r / double(std::max<size_t>(1, frames))));
        };
        float l0 = 0, r0 = 0, l1 = 0, r1 = 0, l2 = 0, r2 = 0;
        renderRms(s, &l0, &r0);
        Song solo = s;
        solo.tracks[0].solo = true;   // nur Melodie
        renderRms(solo, &l1, &r1);
        Song panned = s;
        for (auto& t : panned.tracks) t.pan = -1.f;   // alles links (trocken messen: Hall/Mastering sind absichtlich stereo)
        panned.fx.on = false;
        panned.fx.mastering = false;
        renderRms(panned, &l2, &r2);
        std::printf("      RMS normal L %.3f R %.3f | Solo Melodie L %.3f | alles links: L %.3f R %.3f\n", l0, r0, l1, l2, r2);
        check(l1 > 0.001f && l1 < l0 * 0.9f, "Mischpult: Solo (nur eine Spur hörbar)");
        check(r2 < l2 * 0.05f && l2 > 0.001f, "Mischpult: Panorama ganz links");
        // Speichern / Laden
        panned.tracks[0].solo = true;
        std::string err;
        const std::string path = "selftest_mix.mesong";
        bool ok = saveSongFile(path, panned, nullptr, &err);
        SongFile sf;
        ok = ok && loadSongFile(path, sf, &err);
        std::remove(path.c_str());
        ok = ok && !sf.song.tracks.empty() && sf.song.tracks[0].solo && std::abs(sf.song.tracks[0].pan + 1.f) < 1e-4f;
        check(ok, "Panorama und Solo werden gespeichert");
        AudioEngine e;
        e.loadSong(s);
        std::vector<float> lv = e.trackLevels();
        check(lv.size() == s.tracks.size(), "Pegelanzeige: ein Wert pro Spur");
    }
    // 12. Hall & Echo
    {
        // Song rendern und als Stereo-Samples zurückgeben
        auto render = [](const Song& song, double tail) {
            AudioEngine e;
            e.loadSong(song);
            const std::string path = "selftest_fx.wav";
            e.renderToWav(path, tail);
            std::vector<unsigned char> buf;
            {
                std::ifstream f(path, std::ios::binary);
                buf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
            std::remove(path.c_str());
            std::vector<float> smp;
            size_t p = 12;
            while (p + 8 <= buf.size() && std::memcmp(&buf[p], "data", 4) != 0) {
                uint32_t sz;
                std::memcpy(&sz, &buf[p + 4], 4);
                p += 8 + sz;
            }
            for (p += 8; p + 4 <= buf.size(); p += 4) {
                float v;
                std::memcpy(&v, &buf[p], 4);
                smp.push_back(v);
            }
            return smp;
        };
        auto rmsRange = [](const std::vector<float>& s, double fromSec, double toSec) {
            const size_t a = std::min(s.size(), size_t(fromSec * 48000) * 2), b = std::min(s.size(), size_t(toSec * 48000) * 2);
            double sum = 0;
            for (size_t i = a; i < b; ++i) sum += double(s[i]) * s[i];
            return b > a ? std::sqrt(sum / double(b - a)) : 0.0;
        };
        // Ein kurzer Klavierton bei 120 BPM (1 Schlag = 0,5 s)
        Song one;
        one.bpm = 120;
        Track t{"Test", "Piano"};
        t.notes.push_back({60, 0, 240, 0.9f});   // Achtel = 0,25 s
        t.reverb = 0.5f;
        one.tracks.push_back(t);
        one.fx.roomSize = 0.85f;
        auto wet = render(one, 3.0);
        Song dry = one;
        dry.fx.on = false;
        auto dr = render(dry, 3.0);
        const double tailWet = rmsRange(wet, 1.5, 2.5), tailDry = rmsRange(dr, 1.5, 2.5);
        std::printf("      Nachhall 1,5-2,5 s nach dem Ton: mit Hall %.5f, ohne %.5f\n", tailWet, tailDry);
        check(tailWet > tailDry * 10 && tailWet > 1e-4, "Hall klingt nach");
        bool finite = true;
        for (float v : wet) finite = finite && std::isfinite(v) && std::abs(v) <= 1.5f;
        check(finite, "Hall: keine kaputten Werte / Übersteuerung");
        // Echo: 1/8 bei 120 BPM = 0,25 s, ohne Hall -> Wiederholungen bei 0,25 / 0,5 s
        Song echo = one;
        echo.tracks[0].reverb = 0.f;
        echo.tracks[0].delay = 1.f;
        echo.tracks[0].notes[0].length = 30;   // ganz kurzer Ton
        echo.tracks[0].instrument = "Marimba";
        echo.fx.delayNote = 1;
        echo.fx.delayFeedback = 0.4f;
        auto ec = render(echo, 2.0);
        Song noEcho = echo;
        noEcho.tracks[0].delay = 0.f;
        auto ne = render(noEcho, 2.0);
        // Nur das Echo: Differenz zur Aufnahme ohne Echo
        std::vector<float> diff(std::min(ec.size(), ne.size()));
        for (size_t i = 0; i < diff.size(); ++i) diff[i] = ec[i] - ne[i];
        double best = 0, bestT = 0;
        for (double x = 0.05; x < 0.45; x += 0.005) {
            const double v = rmsRange(diff, x, x + 0.01);
            if (v > best) { best = v; bestT = x; }
        }
        const double before = rmsRange(diff, 0.0, 0.2);
        const double second = rmsRange(diff, 0.49, 0.53);
        std::printf("      Echo: erste Wiederholung bei %.3f s (erwartet 0,25 s), davor %.5f, Spitze %.4f, zweite (0,5 s) %.4f\n",
                    bestT, before, best, second);
        check(bestT > 0.23 && bestT < 0.28 && before < best * 0.05 && second > best * 0.1, "Echo im Takt (1/8 bei 120 BPM)");
        // Verhältnis Hall zu trockenem Klang beim kurzen Ton
        const double direct = rmsRange(dr, 0.0, 0.3);
        std::printf("      Hall-Ausklang 1,5 s nach dem Ton: %.1f dB unter dem Ton\n", 20.0 * std::log10(std::max(1e-9, tailWet / direct)));
        // Ganzer Song: Lautstärke mit/ohne Effekte ähnlich (kein Matsch, kein Pegelsprung)
        GeneratorSettings gs;
        gs.seed = 77; gs.quality = 1; gs.lengthSeconds = 40;
        Song song = generateSong(gs);
        song.fx.mastering = false;   // Hall-Anteil ohne Mastering messen (der Kompressor gleicht sonst aus)
        Song noFx = song;
        noFx.fx.on = false;
        const auto a = render(song, 1.0), b = render(noFx, 1.0);
        const double ra = rmsRange(a, 0, 1e9), rb = rmsRange(b, 0, 1e9);
        std::printf("      Ganzer Song: RMS mit Effekten %.4f, ohne %.4f (x%.2f), Raum: Größe %.2f, Hall %.2f\n", ra, rb, ra / std::max(1e-9, rb),
                    song.fx.roomSize, song.fx.reverbLevel);
        {
            std::vector<float> w(std::min(a.size(), b.size()));
            for (size_t i = 0; i < w.size(); ++i) w[i] = a[i] - b[i];
            const double rw = rmsRange(w, 0, 1e9);
            std::printf("      Effekt-Anteil im Mix: %.1f dB unter dem trockenen Klang\n", 20.0 * std::log10(std::max(1e-9, rw / rb)));
        }
        {
            std::vector<float> w(std::min(a.size(), b.size()));
            for (size_t i = 0; i < w.size(); ++i) w[i] = a[i] - b[i];
            const double shareDb = 20.0 * std::log10(std::max(1e-9, rmsRange(w, 0, 1e9) / std::max(1e-9, rb)));
            // Hall hörbar (nicht unter -28 dB), aber nicht matschig (nicht über -8 dB); Gesamtlautstärke kaum verändert
            check(shareDb > -28.0 && shareDb < -8.0 && ra / std::max(1e-9, rb) > 0.97 && ra / std::max(1e-9, rb) < 1.6,
                  "Effekte: Hall-Anteil und Lautstärke im Rahmen");
        }
        // Generator wählt passende Räume
        GeneratorSettings go;
        go.seed = 3; go.quality = 1; go.lengthSeconds = 30; go.ensemble = Ensemble::Orchestra;
        Song so = generateSong(go);
        GeneratorSettings gj;
        gj.seed = 4; gj.quality = 1; gj.lengthSeconds = 30; gj.drums = DrumStyle::Jazz;
        Song sj = generateSong(gj);
        std::printf("      Raumgröße: Orchester %.2f, Jazz %.2f\n", so.fx.roomSize, sj.fx.roomSize);
        check(so.fx.roomSize >= 0.8f && sj.fx.roomSize < 0.5f, "Generator: Orchester im Saal, Jazz im kleinen Raum");
        // Speichern / Laden
        Song sv = song;
        sv.fx.roomSize = 0.33f;
        sv.fx.delayNote = 3;
        sv.tracks[0].reverb = 0.77f;
        sv.tracks[0].delay = 0.11f;
        std::string err;
        bool ok = saveSongFile("selftest_fx.mesong", sv, nullptr, &err);
        SongFile sf;
        ok = ok && loadSongFile("selftest_fx.mesong", sf, &err);
        std::remove("selftest_fx.mesong");
        ok = ok && std::abs(sf.song.fx.roomSize - 0.33f) < 1e-4f && sf.song.fx.delayNote == 3 &&
             std::abs(sf.song.tracks[0].reverb - 0.77f) < 1e-4f && std::abs(sf.song.tracks[0].delay - 0.11f) < 1e-4f;
        check(ok, "Hall & Echo werden gespeichert");
    }
    // 13. Klangqualität: zweite Stimme, Streicher-Linie, Blech, Mastering
    {
        int withVoice2 = 0, withStrings = 0, bandSongs = 0;
        double interval = 0;
        int intervals = 0;
        for (int i = 0; i < 8; ++i) {
            GeneratorSettings gs;
            gs.seed = uint32_t(4100 + i);
            gs.quality = 1;
            gs.lengthSeconds = 120;
            gs.chordInstrument = "Piano";
            GenerationInfo info;
            Song s = generateSong(gs, &info);
            ++bandSongs;
            const Track* mel = nullptr;
            const Track* v2 = nullptr;
            for (auto& t : s.tracks) {
                if (t.name == "Melodie") mel = &t;
                if (t.name == "Zweite Stimme") v2 = &t;
                if (t.name == "Streicher-Linie") ++withStrings;
            }
            if (v2) {
                ++withVoice2;
                for (auto& h : v2->notes)
                    for (auto& m : mel->notes)
                        if (std::abs(m.start - h.start) < 30) { interval += double(m.pitch - h.pitch); ++intervals; break; }
            }
        }
        std::printf("      Band: %d/%d Songs mit zweiter Stimme (Abstand im Schnitt %.1f Halbtöne), %d mit Streicher-Linie\n",
                    withVoice2, bandSongs, intervals ? interval / intervals : 0.0, withStrings);
        check(withVoice2 >= bandSongs / 2 && intervals > 0 && interval / intervals > 2.5 && interval / intervals < 10.,
              "Zweite Stimme im Refrain (Terz/Sexte darunter)");
        // Blech im Orchester
        int brassNotes = 0, total = 0;
        for (int i = 0; i < 6; ++i) {
            GeneratorSettings go;
            go.seed = uint32_t(5200 + i);
            go.quality = 1;
            go.ensemble = Ensemble::Orchestra;
            Song so = generateSong(go);
            for (auto& t : so.tracks) {
                total += int(t.notes.size());
                if (t.name == "Hörner" || t.name == "Trompeten" || t.name == "Posaunen") brassNotes += int(t.notes.size());
            }
        }
        std::printf("      Orchester: %.1f %% aller Noten im Blech\n", 100.0 * brassNotes / std::max(1, total));
        check(brassNotes * 100 >= total * 8, "Blech hat im Orchester genug zu tun (mind. 8 % der Noten)");
        // Mastering: lauter, Spitzen begrenzt
        GeneratorSettings gm;
        gm.seed = 6060; gm.quality = 1; gm.lengthSeconds = 30;
        Song sm = generateSong(gm);
        Song off = sm;
        off.fx.mastering = false;
        auto a = renderSamples(sm, 0.5), b = renderSamples(off, 0.5);
        auto rms = [](const std::vector<float>& v) { double s = 0; for (float x : v) s += double(x) * x; return std::sqrt(s / double(std::max<size_t>(1, v.size()))); };
        float pk = 0;
        for (float x : a) pk = std::max(pk, std::abs(x));
        const double gain = 20 * std::log10(rms(a) / std::max(1e-9, rms(b)));
        std::printf("      Mastering: %+.1f dB lauter, Spitze %.2f\n", gain, pk);
        check(gain > 0.5 && gain < 5.0 && pk <= 0.95f, "Mastering: etwas lauter, Spitzen begrenzt");
    }
    // 14. Neue Instrumente und Schlagwerk
    {
        // jedes neue Instrument klingt (Pegel im Rahmen, keine kaputten Werte)
        const char* insts[] = {"Saxofon", "Tenorsaxofon", "Piccolo", "Tuba", "Cembalo", "Celesta", "Xylophon", "Vibraphon",
                               "Röhrenglocken", "Akkordeon", "Mundharmonika", "E-Gitarre"};
        bool allOk = true;
        for (auto name : insts) {
            Song s;
            s.bpm = 120;
            Track t{"Test", name};
            int p = std::string(name) == "Tuba" ? 41 : std::string(name) == "Piccolo" ? 81 : 64;
            for (int i = 0; i < 4; ++i) t.notes.push_back({p + i * 2, Tick(i) * 480, 400, 0.8f});
            s.tracks.push_back(t);
            s.fx.on = false;
            s.fx.mastering = false;
            auto smp = renderSamples(s, 0.5);
            double sum = 0;
            bool fin = true;
            for (float v : smp) { sum += double(v) * v; fin = fin && std::isfinite(v); }
            const double db = 20 * std::log10(std::sqrt(sum / double(std::max<size_t>(1, smp.size()))) + 1e-12);
            if (!fin || db < -45 || db > -6) { std::printf("      %s: %.1f dB %s\n", name, db, fin ? "" : "(kaputte Werte)"); allOk = false; }
        }
        check(allOk, "Neue Instrumente klingen (Saxofon, Tuba, Cembalo, Celesta ... E-Gitarre)");
        // neue Schlagwerk-Klänge
        bool drumsOk = true;
        for (int p : {80, 81, 75, 76, 77, 52, 55}) {
            Song s;
            Track t{"Perc", "Schlagzeug"};
            t.notes.push_back({p, 0, 120, 0.9f});
            s.tracks.push_back(t);
            s.fx.on = false;
            s.fx.mastering = false;
            auto smp = renderSamples(s, 2.0);
            float pk = 0;
            for (float v : smp) pk = std::max(pk, std::abs(v));
            if (pk < 0.01f || !std::isfinite(pk)) { std::printf("      Schlagwerk %d stumm\n", p); drumsOk = false; }
        }
        check(drumsOk, "Triangel, Claves, Holzblock, Gong, Becken-Wirbel klingen");
        // Orchester nutzt die neuen Farben
        std::map<std::string, int> used;
        int gongs = 0, triangles = 0;
        for (int i = 0; i < 16; ++i) {
            GeneratorSettings go;
            go.seed = uint32_t(7300 + i);
            go.quality = 1;
            go.ensemble = Ensemble::Orchestra;
            go.orchStyle = i % 8;
            go.energy = 0.75f;
            Song so = generateSong(go);
            for (auto& t : so.tracks) {
                if (!t.notes.empty()) used[t.name]++;
                if (t.name == "Schlagwerk")
                    for (auto& n : t.notes) { gongs += n.pitch == 52; triangles += n.pitch == 81; }
            }
        }
        std::printf("      Orchester (16 Songs): Tuba %d, Piccolo %d, Cembalo %d Songs; %d Triangel-, %d Gong-Schläge\n",
                    used["Tuba"], used["Piccolo"], used["Cembalo"], triangles, gongs);
        check(used["Tuba"] > 0 && used["Piccolo"] > 0 && used["Cembalo"] > 0 && triangles > 0 && gongs > 0,
              "Orchester nutzt Tuba, Piccolo, Cembalo, Triangel und Gong");
        // Neue Vorlagen + Beschreibung
        bool presetsOk = true;
        for (int p : {20, 21}) {
            GeneratorSettings gs;
            std::mt19937 rng(3);
            bool rb = false;
            float bpm = 110;
            int meter = 0;
            GeneratorPanel::applyPresetTo(gs, p, rng, rb, bpm, meter);
            gs.bpm = bpm;
            gs.beatsPerBar = meter == 1 ? 3 : 4;
            gs.seed = 11;
            gs.quality = 1;
            Song s = generateSong(gs);
            bool hasInst = false;
            for (auto& t : s.tracks) hasInst = hasInst || t.instrument == (p == 20 ? "E-Gitarre" : "Akkordeon");
            std::printf("      Vorlage %s: %zu Spuren, %.0f s, Reibung %.2f %%\n", GeneratorPanel::presetName(p), s.tracks.size(),
                        s.durationSeconds(), clashRate(s));
            presetsOk = presetsOk && hasInst && clashRate(s) < 2.0;
        }
        GeneratorSettings gp;
        std::mt19937 rng(1);
        bool rb = true;
        float bpm = 110;
        int meter = 0;
        const std::string fb = GeneratorPanel::applyPromptTo(gp, "jazz mit Saxofon und Vibraphon", rng, rb, bpm, meter);
        std::printf("      \"jazz mit Saxofon und Vibraphon\" -> %s\n", fb.c_str());
        check(presetsOk && gp.melodyInstrument == "Saxofon" && gp.chordInstrument == "Vibraphon",
              "Vorlagen Rock + Paris, Beschreibung versteht neue Instrumente");
    }
    // 11. Spielen: Tastenbelegung und freies Instrument
    {
        const auto& km = LivePlayer::keyMap();
        std::set<int> keys, semis;
        for (auto& k : km) { keys.insert(int(k.key)); semis.insert(k.semitone); }
        bool contiguous = true;
        for (int i = 0; i <= 31; ++i) contiguous = contiguous && semis.count(i);
        check(keys.size() == km.size() && contiguous && semis.size() == 32,
              "Tastatur: " + std::to_string(km.size()) + " Tasten, 32 Halbtöne lückenlos (2 Okt. + Quinte)");
        auto rms = [&](const std::string& inst) {
            AudioEngine e;
            Song empty;
            e.loadSong(empty);
            e.setLiveInstrument(inst);
            e.noteOn(AudioEngine::kLiveTrack, 60, 0.9f);
            e.noteOn(AudioEngine::kLiveTrack, 64, 0.9f);
            const std::string path = "selftest_live.wav";
            e.renderToWav(path, 0.6);
            std::vector<unsigned char> buf;
            {
                std::ifstream f(path, std::ios::binary);
                buf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
            std::remove(path.c_str());
            double sum = 0;
            size_t n = 0;
            for (size_t p = 44; p + 4 <= buf.size(); p += 4, ++n) {
                float v;
                std::memcpy(&v, &buf[p], 4);
                if (std::isfinite(v)) sum += double(v) * v;
            }
            return std::sqrt(sum / double(std::max<size_t>(1, n)));
        };
        const double glocken = rms("Glocken"), orgel = rms("Orgel"), aus = rms("");
        std::printf("      Live-Kanal RMS: Glocken %.4f, Orgel %.4f, ohne Instrument %.4f\n", glocken, orgel, aus);
        check(glocken > 0.001 && orgel > 0.001 && aus < 1e-6, "Freies Spiel-Instrument (Live-Kanal)");
    }
    // 15. Noch mehr Instrumente, Hand-Percussion und Orchester-Spannung
    {
        auto renderOne = [&](const char* name, int pitch, Tick len, double secs) {
            Song s;
            s.bpm = 120;
            Track t{"Test", name};
            t.notes.push_back({pitch, 0, len, 0.8f});
            s.tracks.push_back(t);
            s.fx.on = false;
            s.fx.mastering = false;
            return renderSamples(s, secs);
        };
        auto rmsOf = [](const std::vector<float>& v, size_t a, size_t b) {
            double s = 0;
            b = std::min(b, v.size());
            for (size_t i = a; i < b; ++i) s += double(v[i]) * v[i];
            return std::sqrt(s / double(std::max<size_t>(1, b - a)));
        };
        bool allOk = true;
        for (auto name : {"Kirchenorgel", "Panflöte", "Ukulele", "Mandoline", "Banjo", "Steel Drum", "Kalimba"}) {
            auto smp = renderOne(name, 67, 900, 1.0);
            bool fin = true;
            for (float v : smp) fin = fin && std::isfinite(v);
            const double db = 20 * std::log10(rmsOf(smp, 0, smp.size()) + 1e-12);
            if (!fin || db < -45 || db > -6) { std::printf("      %s: %.1f dB %s\n", name, db, fin ? "" : "(kaputte Werte)"); allOk = false; }
        }
        check(allOk, "Kirchenorgel, Panflöte, Ukulele, Mandoline, Banjo, Steel Drum, Kalimba klingen");
        // Mandoline hält lange Töne mit Tremolo, die Ukulele klingt dagegen einfach aus
        {
            auto mand = renderOne("Mandoline", 67, 4 * 480, 2.0), uke = renderOne("Ukulele", 67, 4 * 480, 2.0);
            const size_t sr2 = 48000 * 2;   // Stereo-Puffer: 1 s = 2 * 48000 Werte
            const double mLate = rmsOf(mand, sr2 * 3 / 2, sr2 * 2) / std::max(1e-9, rmsOf(mand, 0, sr2 / 2));
            const double uLate = rmsOf(uke, sr2 * 3 / 2, sr2 * 2) / std::max(1e-9, rmsOf(uke, 0, sr2 / 2));
            std::printf("      nach 1,5 s noch: Mandoline %.0f %%, Ukulele %.0f %%\n", mLate * 100, uLate * 100);
            check(mLate > 0.3 && mLate > uLate * 2, "Mandolinen-Tremolo hält lange Töne");
        }
        bool drumsOk = true;
        for (int p : {60, 61, 62, 63, 64, 67, 68, 73, 74, 84}) {
            auto smp = renderOne("Schlagzeug", p, 120, 2.0);
            float pk = 0;
            for (float v : smp) pk = std::max(pk, std::abs(v));
            if (pk < 0.01f || !std::isfinite(pk)) { std::printf("      Schlagwerk %d stumm\n", p); drumsOk = false; }
        }
        check(drumsOk, "Congas, Bongos, Agogo, Guiro, Windspiel klingen");
        // Orchester: Paukenwirbel (dichte 32tel mit Crescendo) und große Einzelschläge
        int rolls = 0, bigHits = 0, finales = 0, songs = 0;
        for (int i = 0; i < 16; ++i) {
            GeneratorSettings go;
            go.seed = uint32_t(9100 + i);
            go.quality = 1;
            go.ensemble = Ensemble::Orchestra;
            go.orchStyle = i % 8;
            go.energy = 0.7f;
            Song so = generateSong(go);
            ++songs;
            const Tick th = so.ppq / 8;
            Tick songEnd = 0;
            for (auto& t : so.tracks) for (auto& n : t.notes) songEnd = std::max(songEnd, n.end());
            for (auto& t : so.tracks) {
                if (t.name != "Pauken") continue;
                auto ns = t.notes;
                std::sort(ns.begin(), ns.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
                int run = 0;
                bool counted = false;
                for (size_t j = 1; j < ns.size(); ++j) {
                    const Tick d = ns[j].start - ns[j - 1].start;   // (humanisiert: nicht exakt gleich)
                    if (d >= th / 2 && d <= th * 3 / 2) { if (++run >= 12 && !counted) { ++rolls; counted = true; } }
                    else { run = 0; counted = false; }
                }
                for (auto& n : ns) {
                    if (n.velocity >= 0.9f) ++bigHits;
                    if (n.velocity >= 0.95f && n.start + 2 * so.ticksPerBar() + so.ppq >= songEnd) ++finales;   // Schlag auf den Schlussakkord
                }
            }
        }
        std::printf("      Orchester (%d Songs): %d Paukenwirbel, %d große Schläge, %d mal Schluss-Schlag\n", songs, rolls, bigHits, finales);
        check(rolls >= 3 && bigHits >= 3 && finales >= 1, "Pauken wirbeln vor dem Refrain und schlagen laut auf die Eins");
        // Neue Vorlagen: Karibik (mit Congas), Bluegrass, Kirche (Kathedrale), Anden, Traumland
        bool presetsOk = true;
        for (int p = 22; p <= 26; ++p) {
            GeneratorSettings gs;
            std::mt19937 rng(5);
            bool rb = false;
            float bpm = 110;
            int meter = 0;
            GeneratorPanel::applyPresetTo(gs, p, rng, rb, bpm, meter);
            gs.bpm = bpm;
            gs.beatsPerBar = meter == 1 ? 3 : 4;
            gs.seed = 21;
            gs.quality = 1;
            Song s = generateSong(gs);
            static const char* want[] = {"Steel Drum", "Banjo", "Kirchenorgel", "Panflöte", "Kalimba"};
            bool hasInst = false;
            int congas = 0;
            for (auto& t : s.tracks) {
                hasInst = hasInst || t.instrument == want[p - 22];
                if (t.instrument == "Schlagzeug") for (auto& n : t.notes) congas += n.pitch >= 62 && n.pitch <= 64;
            }
            std::printf("      Vorlage %s: %zu Spuren, %.0f s, Reibung %.2f %%%s\n", GeneratorPanel::presetName(p), s.tracks.size(),
                        s.durationSeconds(), clashRate(s), p == 22 ? (" , Congas " + std::to_string(congas)).c_str() : "");
            presetsOk = presetsOk && hasInst && clashRate(s) < 2.0;
            if (p == 22) presetsOk = presetsOk && congas > 0;
            if (p == 24) presetsOk = presetsOk && s.fx.roomSize > 0.9f;   // Kathedrale
        }
        GeneratorSettings gp;
        std::mt19937 rng(1);
        bool rb = true;
        float bpm = 110;
        int meter = 0;
        const std::string fb = GeneratorPanel::applyPromptTo(gp, "karibik mit steel drum und ukulele", rng, rb, bpm, meter);
        std::printf("      \"karibik mit steel drum und ukulele\" -> %s\n", fb.c_str());
        check(presetsOk && gp.melodyInstrument == "Steel Drum" && gp.chordInstrument == "Ukulele",
              "Vorlagen Karibik, Bluegrass, Kirche, Anden, Traumland + Beschreibung");
    }
    // 16. Mehr Instrumente gleichzeitig: Orchester-Besetzungsdichte, Orgel, Band-Zusatzstimmen
    {
        // durchschnittlich gleichzeitig klingende Stimmen im Refrain
        auto simul = [&](const Song& s) {
            double sum = 0;
            int cnt = 0;
            for (auto& sec : s.sections) {
                if (sec.name.rfind("Refrain", 0) != 0) continue;
                for (Tick t = sec.start; t < sec.start + sec.length; t += s.ppq) {
                    int n = 0;
                    for (auto& tr : s.tracks) {
                        bool on = false;
                        for (auto& x : tr.notes) on = on || (x.start <= t && x.end() > t);
                        n += on;
                    }
                    sum += n;
                    ++cnt;
                }
            }
            return cnt ? sum / cnt : 0.0;
        };
        double thin = 0, full = 0;
        int organs = 0, celestas = 0, windChoirs = 0;
        for (int i = 0; i < 12; ++i) {
            GeneratorSettings go;
            go.seed = uint32_t(9500 + i);
            go.quality = 1;
            go.ensemble = Ensemble::Orchestra;
            go.orchStyle = i % 8;
            go.energy = 0.7f;
            go.orchFullness = 0.f;
            thin += simul(generateSong(go));
            go.orchFullness = 1.f;
            GenerationInfo inf;
            Song so = generateSong(go, &inf);
            full += simul(so);
            for (auto& t : so.tracks) { organs += t.name == "Orgel" && t.notes.size() > 8; celestas += t.name == "Celesta" && t.notes.size() > 8; }
            for (auto& tq : inf.techniques) windChoirs += tq.find("Holzbläser-Satz") != std::string::npos;
        }
        thin /= 12; full /= 12;
        std::printf("      Orchester-Refrain: %.1f Stimmen gleichzeitig (Kammer) -> %.1f (volles Tutti); Orgel in %d, Celesta in %d Songs, %d x Holzbläser-Satz\n",
                    thin, full, organs, celestas, windChoirs);
        check(full > thin + 1.5 && organs > 0 && celestas > 0 && windChoirs > 0, "Orchester: Besetzungsdichte, Orgel, Celesta, Holzbläser-Satz");
        // Band: verschiedene Zusatz-Instrumente pro Song
        std::set<std::string> combos, insts;
        int layered = 0;
        double clashSum = 0;
        for (int i = 0; i < 12; ++i) {
            GeneratorSettings gb;
            gb.seed = uint32_t(9600 + i);
            gb.quality = 1;
            gb.energy = 0.4f + 0.05f * float(i);
            GenerationInfo inf;
            Song sb = generateSong(gb, &inf);
            std::string c;
            for (auto& l : inf.layers) { c += l + ";"; insts.insert(l.substr(0, l.find(" ("))); }
            combos.insert(c);
            layered += inf.layers.size() >= 2;
            clashSum += clashRate(sb);
        }
        GeneratorSettings g0;
        g0.seed = 9600; g0.quality = 1; g0.extraLayers = 0;
        GenerationInfo i0;
        generateSong(g0, &i0);
        std::printf("      Band (12 Songs): %zu verschiedene Besetzungen, %zu verschiedene Zusatz-Instrumente, %d mit 2+ Zusatzstimmen, Reibung %.2f %%\n",
                    combos.size(), insts.size(), layered, clashSum / 12);
        check(combos.size() >= 9 && insts.size() >= 8 && layered >= 8 && i0.layers.empty() && clashSum / 12 < 1.0,
              "Band: jeder Song anders besetzt (Fläche, Glitzer, Bläser, Gegenmelodie)");
    }
    std::printf("%s\n", fails ? "Es gab Fehler." : "Alle neuen Funktionen OK.");
    return fails ? 1 : 0;
}

// Reibungs-Analyse:  MusicEngine.exe --analyze <seed> [band|orch] [stil]
// Sucht gleichzeitig klingende Töne im Halbtonabstand (kleine Sekunde / große Septime / kleine None),
// die beide mindestens eine Achtel lang sind – das klingt "falsch". Zeigt, welche Stimmen das verursachen.
static int analyzeCli(int argc, char** argv) {
    GeneratorSettings gs;
    gs.seed = uint32_t(std::stoul(argv[2]));
    gs.quality = 3;
    if (argc > 3 && std::string(argv[3]) == "orch") gs.ensemble = Ensemble::Orchestra;
    if (argc > 4) gs.orchStyle = std::stoi(argv[4]);
    GenerationInfo info;
    Song song = generateSong(gs, &info);
    std::vector<bool> drum(song.tracks.size());
    for (size_t i = 0; i < song.tracks.size(); ++i) {
        auto inst = InstrumentRegistry::instance().create(song.tracks[i].instrument);
        drum[i] = inst && inst->isDrumKit();
    }
    const Tick step = song.ppq / 4, minLen = song.ppq / 2;
    std::map<std::string, int> pairCount;
    std::map<std::string, int> trackCount;
    int steps = 0, badSteps = 0, examples = 0;
    bool verbose = false;   // Beispiele ausgeben (Umgebungsvariable ME_VERBOSE)
#ifdef _MSC_VER
    {
        char* v = nullptr;
        size_t len = 0;
        verbose = _dupenv_s(&v, &len, "ME_VERBOSE") == 0 && v != nullptr;
        std::free(v);
    }
#else
    verbose = std::getenv("ME_VERBOSE") != nullptr;
#endif
    for (Tick t = 0; t < song.length(); t += step) {
        struct S { int pitch; size_t track; };
        std::vector<S> snd;
        for (size_t i = 0; i < song.tracks.size(); ++i) {
            if (drum[i]) continue;
            for (auto& n : song.tracks[i].notes)
                if (n.start <= t + step / 2 && n.end() > t + step / 2 && n.length >= minLen) snd.push_back({n.pitch, i});
        }
        ++steps;
        bool bad = false;
        for (size_t a = 0; a < snd.size(); ++a)
            for (size_t b = a + 1; b < snd.size(); ++b) {
                const int ic = ((snd[a].pitch - snd[b].pitch) % 12 + 12) % 12;
                if (ic != 1 && ic != 11) continue;
                bad = true;
                std::string x = song.tracks[snd[a].track].name, y = song.tracks[snd[b].track].name;
                if (y < x) std::swap(x, y);
                pairCount[x + " <-> " + y]++;
                if (verbose && examples < 25 && (t % song.ticksPerBeat()) == 0) {
                    ++examples;
                    std::printf("   Takt %lld.%lld: %s %s%d  gegen  %s %s%d\n", (long long)(t / song.ticksPerBar() + 1),
                                (long long)((t % song.ticksPerBar()) / song.ticksPerBeat() + 1),
                                song.tracks[snd[a].track].name.c_str(), noteName(snd[a].pitch), snd[a].pitch / 12 - 1,
                                song.tracks[snd[b].track].name.c_str(), noteName(snd[b].pitch), snd[b].pitch / 12 - 1);
                }
                trackCount[song.tracks[snd[a].track].name]++;
                trackCount[song.tracks[snd[b].track].name]++;
            }
        if (bad) ++badSteps;
    }
    std::printf("%s | Stil: %s | %d von %d Sechzehnteln mit harter Reibung (%.1f %%)\n", info.key.c_str(),
                info.orchStyle.empty() ? "Band" : info.orchStyle.c_str(), badSteps, steps, 100.0 * badSteps / std::max(1, steps));
    std::vector<std::pair<int, std::string>> top;
    for (auto& [k, v] : pairCount) top.push_back({v, k});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 6; ++i) std::printf("   %5d  %s\n", top[i].first, top[i].second.c_str());
    for (auto& t : info.techniques) std::printf("     - %s\n", t.c_str());
    for (auto& t : info.layers) std::printf("     + %s\n", t.c_str());
    return 0;
}

// Instrumente vermessen:  MusicEngine.exe --instruments
// Spielt jedem Instrument dieselbe Phrase vor und misst Lautheit (RMS), Spitze und Rechenzeit.
static int instrumentTest() {
    const float sr = 48000.f;
    std::printf("%-12s %8s %8s %10s\n", "Instrument", "RMS dB", "Peak", "CPU x RT");
    for (auto& name : InstrumentRegistry::instance().names()) {
        auto inst = InstrumentRegistry::instance().create(name);
        inst->setSampleRate(sr);
        struct Ev { float time; int pitch; bool on; float vel; };
        std::vector<Ev> evs;
        if (inst->isDrumKit()) {
            for (int b = 0; b < 8; ++b) {   // 2 Takte Pop-Beat bei 120 BPM
                float t = b * 0.5f;
                evs.push_back({t, b % 2 ? 38 : 36, true, 0.8f});
                evs.push_back({t, 42, true, 0.6f});
                evs.push_back({t + 0.25f, 42, true, 0.5f});
            }
        } else {
            for (int p : {48, 60, 64, 67}) { evs.push_back({0.f, p, true, 0.8f}); evs.push_back({1.5f, p, false, 0}); }
            for (int i = 0; i < 6; ++i) {
                int p = 72 + (i % 3) * 2;
                evs.push_back({1.5f + i * 0.25f, p, true, 0.8f});
                evs.push_back({1.5f + i * 0.25f + 0.24f, p, false, 0});
            }
        }
        std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.time < b.time; });
        const int frames = int(sr * 4.f);
        std::vector<float> buf(size_t(frames) * 2, 0.f);
        size_t next = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (int pos = 0; pos < frames; pos += 64) {
            while (next < evs.size() && evs[next].time * sr <= float(pos)) {
                auto& e = evs[next++];
                if (e.on) inst->noteOn(e.pitch, e.vel); else inst->noteOff(e.pitch);
            }
            inst->render(buf.data() + size_t(pos) * 2, std::min(64, frames - pos));
        }
        double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double sum = 0;
        float peak = 0;
        for (float x : buf) { sum += double(x) * x; peak = std::max(peak, std::abs(x)); }
        double rms = std::sqrt(sum / double(buf.size()));
        std::printf("%-14s %8.1f %8.2f %10.0f   wahrgenommen %6.1f\n", name.c_str(), 20 * std::log10(rms + 1e-9), peak, 4.0 / cpu,
                    perceivedDb(buf));
    }
    return 0;
}

// Selbsttest:  MusicEngine.exe --selftest <ordner>
// Speichern/Laden (.mesong) und MIDI-Export/-Import müssen alle Noten erhalten.
static int selfTest(const std::string& dir) {
    GeneratorSettings gs;
    gs.seed = 4711;
    gs.quality = 3;
    Song a = generateSong(gs);
    const std::string mesong = dir + "/selftest_ümlaut.mesong", mid = dir + "/selftest.mid";
    std::string err;
    int fails = 0;
    auto sameNotes = [](const Song& x, const Song& y, bool exactVel) {
        size_t cx = 0, cy = 0;
        for (auto& t : x.tracks) cx += t.notes.size();
        for (auto& t : y.tracks) cy += t.notes.size();
        if (cx != cy) return false;
        for (size_t i = 0; i < x.tracks.size() && i < y.tracks.size(); ++i) {
            auto nx = x.tracks[i].notes, ny = y.tracks[i].notes;
            auto key = [](const Note& n) { return std::tuple(n.start, n.pitch, n.length); };
            std::sort(nx.begin(), nx.end(), [&](auto& p, auto& q) { return key(p) < key(q); });
            std::sort(ny.begin(), ny.end(), [&](auto& p, auto& q) { return key(p) < key(q); });
            for (size_t k = 0; k < nx.size(); ++k) {
                if (key(nx[k]) != key(ny[k])) return false;
                if (std::abs(nx[k].velocity - ny[k].velocity) > (exactVel ? 0.002f : 0.01f)) return false;
            }
        }
        return true;
    };

    SongFile f;
    bool ok = saveSongFile(mesong, a, &gs, &err) && loadSongFile(mesong, f, &err);
    ok = ok && sameNotes(a, f.song, true) && f.song.sections.size() == a.sections.size() && f.hasGenerator &&
         f.generator.seed == gs.seed && f.song.bpm == a.bpm;
    std::printf("Song-Datei speichern/laden: %s %s\n", ok ? "OK" : "FEHLER", err.c_str());
    fails += !ok;

    Song b;
    err.clear();
    ok = exportMidi(mid, a, &err) && importMidi(mid, b, &err);
    ok = ok && sameNotes(a, b, false) && std::abs(b.bpm - a.bpm) < 0.01 && b.sections.size() == a.sections.size();
    std::printf("MIDI export/import:         %s %s\n", ok ? "OK" : "FEHLER", err.c_str());
    fails += !ok;

    // Eigene Idee: 4 Takte Melodie in a-Moll (frei erfunden), muss am Songanfang unverändert stehen
    {
        Song ref;
        const Tick q = ref.ppq, e8 = q / 2;
        const int mel[][3] = {{69, 0, 2}, {72, 2, 1}, {71, 3, 1}, {69, 4, 2}, {64, 6, 2},        // in Achteln: Tonhöhe, Start, Länge
                              {65, 8, 2}, {64, 10, 1}, {62, 11, 1}, {64, 12, 4},
                              {69, 16, 2}, {72, 18, 2}, {76, 20, 3}, {74, 23, 1},
                              {72, 24, 2}, {71, 26, 2}, {69, 28, 4}};
        auto idea = std::make_shared<MusicalIdea>();
        for (auto& m : mel) idea->notes.push_back({m[0], m[1] * e8, m[2] * e8, 0.75f});
        idea->bars = 4;
        KeyGuess kg = detectKey(idea->notes);
        GeneratorSettings is;
        is.seed = 99;
        is.quality = 3;
        is.bpm = 96;
        is.idea = idea;
        GenerationInfo info;
        Song s = generateSong(is, &info);
        bool same = !s.tracks.empty();
        for (size_t i = 0; same && i < idea->notes.size(); ++i) {
            const Note& want = idea->notes[i];
            bool found = false;
            for (auto& n : s.tracks[0].notes)
                if (n.pitch == want.pitch && std::llabs(n.start - want.start) <= 20) { found = true; break; }
            same = found;
        }
        ok = same && kg.key == 9 && (kg.scale == ScaleType::NaturalMinor || kg.scale == ScaleType::HarmonicMinor) &&
             !s.sections.empty() && s.sections[0].name.rfind("Strophe", 0) == 0;
        std::printf("Idee ausbauen:              %s  (erkannt: %s %s, Form: %s)\n", ok ? "OK" : "FEHLER", noteName(kg.key),
                    scaleName(kg.scale), info.structure.c_str());
        for (auto& p : info.progressions) std::printf("      %s\n", p.c_str());
        fails += !ok;

        is.ensemble = Ensemble::Orchestra;   // gleiche Idee, diesmal als Orchester
        Song o = generateSong(is, &info);
        ok = o.tracks.size() >= 8;
        std::printf("Orchester:                  %s  (%zu Stimmen:", ok ? "OK" : "FEHLER", o.tracks.size());
        for (auto& t : o.tracks) std::printf(" %s", t.name.c_str());
        std::printf(")\n");
        fails += !ok;
    }
    return fails;
}

int main(int argc, char** argv) {
    registerBuiltinInstruments();
    if (argc > 2 && std::string(argv[1]) == "--generate") return generateCli(argc, argv);
    if (argc > 2 && std::string(argv[1]) == "--selftest") return selfTest(argv[2]);
    if (argc > 1 && std::string(argv[1]) == "--instruments") return instrumentTest();
    if (argc > 2 && std::string(argv[1]) == "--analyze") return analyzeCli(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--presets") return presetsCli(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--features") return featuresCli();
    if (argc > 1 && std::string(argv[1]) == "--forms") return formsCli();
    if (argc > 1 && std::string(argv[1]) == "--systems") return systemsCli();
    if (argc > 1 && std::string(argv[1]) == "--master") return masterCli();
    if (argc > 1 && std::string(argv[1]) == "--balance") return balanceCli();

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1400, 860, "MusicEngine", nullptr, nullptr);
    if (!window) return 1;
    glfwMaximizeWindow(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;   // Fenster andocken
    // Fenster-Layout neben der .exe speichern, damit die eigene Anordnung erhalten bleibt
    static const std::string layoutFile = executableDir() + "/musicengine_layout.ini";
    io.IniFilename = layoutFile.c_str();
    const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(fontPath)) io.Fonts->AddFontFromFileTTF(fontPath, 17.f);
    if (!ScoreView::loadFont(executableDir() + "/Bravura.otf")) std::puts("Hinweis: Bravura.otf (Notenschrift) nicht gefunden.");
    applyStyle();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    Song song = makeDemoSong();
    AudioEngine engine;
    engine.loadSong(song);
    if (!engine.start()) std::puts("Warnung: Audiogeraet konnte nicht gestartet werden.");

    History history;
    PianoRoll roll(song, engine);
    roll.beforeEdit = [&] { history.push(song); };
    GeneratorPanel generator;

    // ---------- Live spielen, Aufnahme, Notenansicht ----------
    LivePlayer live(engine);
    MidiInput midi;
    std::vector<std::string> midiDevices = MidiInput::devices();
    ScoreView score(song, engine);
    bool showRoll = true, showScore = true, resetLayout = false;
    // Zusatz-Ansichten (als Tabs neben Piano-Roll und Noten)
    bool showArrange = true, showMixer = true, showChords = true, showDrums = true, showVis = true;
    ArrangeView arrangeView;
    MixerView mixerView;
    ChordSheetView chordView;
    DrumGridView drumView;
    VisualizerView visView;
    PlayView playView;
    bool showPlay = true;
    int volumePercent = 100;   // Abspiellautstärke
    float toolbarHeight = 100.f;
    bool keyboardPlay = false, metronome = false, countIn = true;
    int recTarget = 1;          // 0 = aktuelle Spur, 1 = neue Spur
    int recQuant = 2;           // 0 aus, 1 = 1/8, 2 = 1/16, 3 = 1/8-Triole
    bool recording = false;     // Aufnahme scharf geschaltet
    int recCounter = 0, recTrack = -1, recCount = 0;
    bool recHistoryPushed = false, recNeedsReload = false;
    std::vector<Note> recNotes, recOverlay;
    std::map<int, std::pair<Tick, float>> recOpen;
    // "Idee ausbauen"
    bool ideaOpen = false;
    int ideaRange = 0, ideaRole = 0, ideaQuant = 2, ideaKey = -1, ideaScale = -1;
    bool ideaFeel = true, ideaCorrect = true;
    auto quantGrid = [&](int q) -> Tick { return q == 1 ? song.ppq / 2 : q == 2 ? song.ppq / 4 : q == 3 ? song.ppq / 3 : 0; };

    // Endlos-Modus ("Radio"): der nächste Song wird komponiert, während der aktuelle läuft
    struct EndlessSong { Song song; GenerationInfo info; std::string mood; };
    std::optional<EndlessSong> endlessNext;
    bool endlessActive = false;       // Modus läuft (Zustand des letzten Frames)
    bool endlessWaitFirst = false;    // erster Song wird noch komponiert
    bool endlessNeedLaunch = false;   // nächste Komposition starten, sobald der Generator frei ist
    bool endlessDiscard = false;      // Ergebnis einer abgebrochenen Endlos-Komposition verwerfen
    bool endlessPlayNow = false;      // nach dem Laden sofort von vorn abspielen
    double endlessEndTime = -1;       // Zeitpunkt, an dem der aktuelle Song zu Ende war

    int snapIndex = 2;
    bool loop = false;
    int loopFrom = 1, loopTo = 4;
    std::string status;

    // ---------- Datei-Zustand ----------
    std::string currentPath;          // leer = noch nie gespeichert
    bool dirty = false;               // ungespeicherte Änderungen
    std::string shownTitle;
    enum class Pending { None, New, Open, ImportMidi, Quit } pending = Pending::None;
    bool openUnsavedPopup = false;
    static const char* kSongFilter = "MusicEngine-Song (*.mesong)\0*.mesong\0Alle Dateien (*.*)\0*.*\0";
    static const char* kMidiFilter = "MIDI-Datei (*.mid)\0*.mid;*.midi\0Alle Dateien (*.*)\0*.*\0";
    static const char* kWavFilter = "WAV-Audio (*.wav)\0*.wav\0";

    auto baseName = [&]() -> std::string {
        if (currentPath.empty()) return "Unbenannt";
        auto u = pathFromUtf8(currentPath).stem().u8string();
        return std::string(u.begin(), u.end());
    };
    auto suggestedName = [&](const char* ext) {
        std::string n = currentPath.empty() ? song.title : baseName();
        for (char& c : n) if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) c = '_';
        return n + ext;
    };

    while (true) {
        if (glfwWindowShouldClose(window)) {
            if (!dirty) break;
            glfwSetWindowShouldClose(window, GLFW_FALSE);   // erst fragen
            pending = Pending::Quit;
            openUnsavedPopup = true;
        }
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        bool changed = false;
        bool loadedClean = false;     // Änderung durch Laden/Speichern -> nicht "dirty"
        bool loopChanged = false;
        bool quitNow = false;

        // ---------- Datei-Aktionen ----------
        auto replaceSong = [&](Song s) {
            history.push(song);
            engine.stopPlayback();
            song = std::move(s);
            roll.currentTrack = 0;
            roll.resetView();
            loop = false;
            changed = true;
        };
        auto saveAs = [&](bool askPath) -> bool {
            std::string path = currentPath;
            if (askPath || path.empty())
                path = saveFileDialog(window, kSongFilter, "mesong", suggestedName(".mesong"), "Song speichern");
            if (path.empty()) return false;
            GeneratorSettings g = generator.settings;
            if (generator.info.seed) g.seed = generator.info.seed;
            std::string err;
            if (!saveSongFile(path, song, &g, &err)) { status = err; return false; }
            currentPath = path;
            dirty = false;
            status = "Gespeichert: " + path;
            return true;
        };
        auto doPending = [&](Pending what) {
            if (what == Pending::New) {
                Song fresh;
                fresh.bpm = song.bpm;
                fresh.tracks.push_back({"Spur 1", "Piano"});
                replaceSong(fresh);
                currentPath.clear();
                loadedClean = true;
            } else if (what == Pending::Open) {
                std::string path = openFileDialog(window, kSongFilter, "Song öffnen");
                if (path.empty()) return;
                SongFile sf;
                std::string err;
                if (!loadSongFile(path, sf, &err)) { status = err; return; }
                replaceSong(sf.song);
                if (sf.hasGenerator) generator.settings = sf.generator;
                currentPath = path;
                loadedClean = true;
                status = "Geöffnet: " + path;
            } else if (what == Pending::ImportMidi) {
                std::string path = openFileDialog(window, kMidiFilter, "MIDI importieren");
                if (path.empty()) return;
                Song s;
                std::string err;
                if (!importMidi(path, s, &err)) { status = err; return; }
                replaceSong(s);
                currentPath.clear();
                status = "MIDI importiert: " + path;
            } else if (what == Pending::Quit) {
                quitNow = true;
            }
        };
        auto request = [&](Pending what) {   // fragt bei ungespeicherten Änderungen nach
            if (dirty) { pending = what; openUnsavedPopup = true; }
            else doPending(what);
        };
        auto exportMid = [&] {
            std::string path = saveFileDialog(window, kMidiFilter, "mid", suggestedName(".mid"), "MIDI exportieren");
            if (path.empty()) return;
            std::string err;
            status = exportMidi(path, song, &err) ? "MIDI gespeichert: " + path : err;
        };
        auto exportWav = [&] {
            std::string path = saveFileDialog(window, kWavFilter, "wav", suggestedName(".wav"), "WAV exportieren");
            if (path.empty()) return;
            AudioEngine offline;   // eigene Engine, damit die Wiedergabe nicht stört
            offline.loadSong(song);
            status = offline.renderToWav(path) ? "WAV gespeichert: " + path : "WAV-Export fehlgeschlagen";
        };
        // Einzelspuren ("Stems"): jede Spur als eigene WAV, alle gleich lang – zum Weitermischen in einer DAW
        auto exportStems = [&] {
            std::string path = saveFileDialog(window, kWavFilter, "wav", suggestedName(".wav"),
                                              "Einzelspuren exportieren (Basisname wählen)");
            if (path.empty()) return;
            std::string base = path;
            if (base.size() > 4 && (base.substr(base.size() - 4) == ".wav" || base.substr(base.size() - 4) == ".WAV"))
                base = base.substr(0, base.size() - 4);
            int ok = 0, total = 0;
            for (size_t i = 0; i < song.tracks.size(); ++i) {
                if (song.tracks[i].notes.empty()) continue;
                ++total;
                Song solo = song;
                for (size_t j = 0; j < solo.tracks.size(); ++j) solo.tracks[j].muted = j != i;
                solo.tracks[i].muted = false;
                std::string name = song.tracks[i].name;
                for (char& c : name) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
                AudioEngine offline;
                offline.loadSong(solo);
                if (offline.renderToWav(base + " - " + std::to_string(i + 1) + " " + name + ".wav")) ++ok;
            }
            status = std::to_string(ok) + " von " + std::to_string(total) + " Einzelspuren gespeichert: " + base + " - *.wav";
        };

        // ---------- Menü ----------
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Datei")) {
                if (ImGui::MenuItem("Neu", "Strg+N")) request(Pending::New);
                if (ImGui::MenuItem("Öffnen ...", "Strg+O")) request(Pending::Open);
                ImGui::Separator();
                if (ImGui::MenuItem("Speichern", "Strg+S")) saveAs(false);
                if (ImGui::MenuItem("Speichern unter ...", "Strg+Umschalt+S")) saveAs(true);
                ImGui::Separator();
                if (ImGui::MenuItem("MIDI importieren ...")) request(Pending::ImportMidi);
                if (ImGui::MenuItem("MIDI exportieren ...")) exportMid();
                if (ImGui::MenuItem("WAV exportieren ...")) exportWav();
                if (ImGui::MenuItem("Einzelspuren exportieren (WAV) ...")) exportStems();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Jede Spur als eigene WAV-Datei (alle gleich lang),\nz.B. zum Weitermischen in einem anderen Programm.");
                ImGui::Separator();
                if (ImGui::MenuItem("Beenden")) request(Pending::Quit);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Bearbeiten")) {
                if (ImGui::MenuItem("Rückgängig", "Strg+Z", false, !history.undo.empty())) changed |= history.doUndo(song);
                if (ImGui::MenuItem("Wiederholen", "Strg+Y", false, !history.redo.empty())) changed |= history.doRedo(song);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Generator")) {
                if (ImGui::MenuItem("Neuen Song generieren", "G", false, !generator.busy())) generator.startGeneration();
                ImGui::MenuItem("Generator-Fenster", nullptr, &generator.open);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Ansicht")) {
                ImGui::MenuItem("Piano-Roll", nullptr, &showRoll);
                ImGui::MenuItem("Noten", nullptr, &showScore);
                ImGui::MenuItem("Arrangement", nullptr, &showArrange);
                ImGui::MenuItem("Mischpult", nullptr, &showMixer);
                ImGui::MenuItem("Akkordblatt", nullptr, &showChords);
                ImGui::MenuItem("Drum-Raster", nullptr, &showDrums);
                ImGui::MenuItem("Visualizer", nullptr, &showVis);
                ImGui::MenuItem("Spielen", nullptr, &showPlay);
                ImGui::MenuItem("Song-Generator", nullptr, &generator.open);
                ImGui::MenuItem("Idee ausbauen", nullptr, &ideaOpen);
                ImGui::Separator();
                if (ImGui::MenuItem("Layout zurücksetzen")) {
                    resetLayout = true;
                    showRoll = showScore = generator.open = true;
                    showArrange = showMixer = showChords = showDrums = showVis = showPlay = true;
                }
                ImGui::EndMenu();
            }
            // Statuszeile rechts in der Menüleiste
            if (!status.empty()) {
                const float w = ImGui::CalcTextSize(status.c_str()).x;
                ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 30.f, ImGui::GetWindowWidth() - w - 20.f));
                ImGui::TextColored({0.6f, 0.9f, 0.6f, 1}, "%s", status.c_str());
            }
            ImGui::EndMainMenuBar();
        }

        // ---------- Feste Werkzeugleiste (volle Breite, direkt unter dem Menü) ----------
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImGuiStyle& style = ImGui::GetStyle();
        toolbarHeight = ImGui::GetFrameHeightWithSpacing() * 3.f + style.WindowPadding.y * 2.f;

        // ---------- Andock-Fläche darunter ----------
        // links (groß) Piano-Roll + Noten als Tabs, rechts Generator + "Idee ausbauen"
        {
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + toolbarHeight));
            ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - toolbarHeight));
            ImGui::SetNextWindowViewport(vp->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
            ImGui::Begin("##DockHost", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBackground);
            ImGui::PopStyleVar(2);
            const ImGuiID dockId = ImGui::GetID("Andockbereich");
            if (resetLayout || !ImGui::DockBuilderGetNode(dockId)) {
                resetLayout = false;
                ImGui::DockBuilderRemoveNode(dockId);
                ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockId, ImVec2(vp->WorkSize.x, vp->WorkSize.y - toolbarHeight));
                ImGuiID right, center;
                ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.27f, &right, &center);
                ImGuiID bottom;
                ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, &bottom, &center);
                ImGui::DockBuilderDockWindow("Piano-Roll", center);
                ImGui::DockBuilderDockWindow("Noten", center);
                ImGui::DockBuilderDockWindow("Arrangement", center);
                ImGui::DockBuilderDockWindow("Akkordblatt", center);
                ImGui::DockBuilderDockWindow("Drum-Raster", center);
                ImGui::DockBuilderDockWindow("Visualizer", center);
                ImGui::DockBuilderDockWindow("Spielen", center);
                ImGui::DockBuilderDockWindow("Mischpult", bottom);
                ImGui::DockBuilderDockWindow("Song-Generator", right);
                ImGui::DockBuilderDockWindow("Idee ausbauen", right);
                ImGui::DockBuilderFinish(dockId);
            }
            ImGui::DockSpace(dockId, ImVec2(0, 0));
            ImGui::End();
        }

        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, toolbarHeight));
        ImGui::Begin("Werkzeuge", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ---------- Tastenkürzel (Datei) ----------
        if (!io.WantTextInput && io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_S, false)) saveAs(io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_O, false)) request(Pending::Open);
            if (ImGui::IsKeyPressed(ImGuiKey_N, false)) request(Pending::New);
        }

        // ---------- Live-Aufnahme ----------
        // "recording" = Aufnahme scharf geschaltet. Solange der Song läuft, wird jeder gespielte Ton
        // sofort in die Spur geschrieben – gehaltene Töne wachsen live mit, bis man loslässt.
        auto commitNote = [&](Note n) {
            if (recTrack < 0 || recTrack >= int(song.tracks.size())) return;
            if (recQuant > 0) {
                std::vector<Note> v{n};
                quantizeNotes(v, quantGrid(recQuant), 0.85f, true);
                n = v.front();
            }
            if (!recHistoryPushed) { history.push(song); recHistoryPushed = true; }
            auto& notes = song.tracks[size_t(recTrack)].notes;
            notes.insert(std::upper_bound(notes.begin(), notes.end(), n, [](const Note& a, const Note& b) { return a.start < b.start; }), n);
            ++recCount;
            dirty = true;
            recNeedsReload = true;   // erst nach dem Abspielen neu laden, damit nichts abreißt
        };
        auto handleRec = [&](const AudioEngine::RecEvent& e) {
            const Tick t = Tick(e.tick);
            if (e.on) {
                if (t < -song.ticksPerBeat() / 2) return;   // zu früh im Einzähler
                recOpen[e.pitch] = {std::max<Tick>(0, t), e.velocity};
            } else {
                auto it = recOpen.find(e.pitch);
                if (it == recOpen.end()) return;
                commitNote({e.pitch, it->second.first, std::max<Tick>(song.ppq / 16, t - it->second.first), it->second.second});
                recOpen.erase(it);
            }
        };
        auto closeOpenNotes = [&] {   // Töne, die beim Anhalten noch gedrückt sind
            const Tick now = Tick(std::max(0.0, engine.position()));
            for (auto& [p, v] : recOpen) commitNote({p, v.first, std::max<Tick>(song.ppq / 8, now - v.first), v.second});
            recOpen.clear();
        };
        auto armRecording = [&] {
            if (recTarget == 1 || song.tracks.empty()) {
                history.push(song);
                const std::string freeInst = engine.liveInstrumentName();   // frei gewähltes Spiel-Instrument übernehmen
                const std::string inst = !freeInst.empty() ? freeInst : song.tracks.empty() ? "Piano" : song.tracks[size_t(roll.currentTrack)].instrument;
                song.tracks.push_back({"Aufnahme " + std::to_string(++recCounter), inst});
                roll.currentTrack = int(song.tracks.size()) - 1;
                engine.loadSong(song);
                dirty = true;
                recHistoryPushed = true;
            } else {
                recHistoryPushed = false;
            }
            recTrack = roll.currentTrack;
            recOpen.clear();
            recCount = 0;
            engine.takeRecorded();
            engine.setRecording(true);
            recording = true;
            status = "Aufnahme an: Play drücken (Leertaste) und spielen - die Töne werden live in \"" +
                     song.tracks[size_t(recTrack)].name + "\" eingezeichnet.";
        };
        auto disarmRecording = [&] {
            for (auto& e : engine.takeRecorded()) handleRec(e);
            closeOpenNotes();
            engine.setRecording(false);
            recording = false;
            if (recNeedsReload) { changed = true; recNeedsReload = false; }
            status = recCount > 0 ? "Aufgenommen: " + std::to_string(recCount) + " Noten. Tipp: 'Idee ausbauen' macht einen Song daraus."
                                  : "Aufnahme aus.";
        };
        auto toggleRecording = [&] { recording ? disarmRecording() : armRecording(); };
        if (recording) {
            for (auto& e : engine.takeRecorded()) handleRec(e);
            if (!engine.isPlaying()) {   // angehalten: offene Töne abschließen und Song neu laden
                closeOpenNotes();
                if (recNeedsReload) { changed = true; recNeedsReload = false; }
            }
            recOverlay.clear();   // gehaltene Töne wachsen sichtbar mit
            const Tick now = Tick(std::max(0.0, engine.position()));
            for (auto& [p, v] : recOpen) recOverlay.push_back({p, v.first, std::max<Tick>(1, now - v.first), v.second});
        }
        roll.overlay = recording ? &recOverlay : nullptr;
        // Freies Instrument gewählt? Dann klingt es über den Live-Kanal (außer beim Aufnehmen: dann die Ziel-Spur)
        live.setTrack(recording ? recTrack : (!engine.liveInstrumentName().empty() ? AudioEngine::kLiveTrack : roll.currentTrack));
        engine.setMetronome(metronome);

        // ---------- Transport ----------
        const bool playing = engine.isPlaying();
        auto togglePlay = [&] {
            if (engine.isPlaying()) { engine.pause(); return; }
            double pos = engine.position();
            if (loop && (pos < song.bar(loopFrom - 1) || pos >= song.bar(loopTo))) pos = double(song.bar(loopFrom - 1));
            else if (!loop && pos >= double(song.length())) pos = 0;
            if (recording && countIn) {   // beim Aufnehmen: einen Takt einzählen, ab Taktanfang
                pos = std::floor(pos / double(song.ticksPerBar())) * double(song.ticksPerBar());
                engine.playWithCountIn(pos, 1);
            } else {
                engine.seek(pos);
                engine.play();
            }
        };
        if (ImGui::Button(playing ? "  Pause  " : "  Play  ")) togglePlay();
        ImGui::SameLine();
        if (ImGui::Button("Stop")) engine.stopPlayback();
        ImGui::SameLine();
        {
            double pos = engine.position();
            int bar = int(pos / double(song.ticksPerBar())) + 1;
            int beat = int(std::fmod(pos, double(song.ticksPerBar())) / double(song.ticksPerBeat())) + 1;
            ImGui::Text("Takt %3d : %d", bar, beat);
        }
        ImGui::SameLine(0, 24);
        ImGui::SetNextItemWidth(90);
        float bpm = float(song.bpm);
        if (ImGui::DragFloat("BPM", &bpm, 0.5f, 30.f, 300.f, "%.1f")) { song.bpm = bpm; changed = true; }
        if (ImGui::IsItemActivated()) history.push(song);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        int beats = song.beatsPerBar;
        if (ImGui::InputInt("Takt /4", &beats)) {
            history.push(song);
            song.beatsPerBar = std::clamp(beats, 1, 16);
            changed = true;
        }

        ImGui::SameLine(0, 24);
        loopChanged |= ImGui::Checkbox("Loop", &loop);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        loopChanged |= ImGui::InputInt("##von", &loopFrom);
        ImGui::SameLine();
        ImGui::TextUnformatted("bis");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        loopChanged |= ImGui::InputInt("##bis", &loopTo);
        loopFrom = std::max(1, loopFrom);
        loopTo = std::max(loopFrom, loopTo);

        ImGui::SameLine(0, 24);
        ImGui::Checkbox("Folgen", &roll.followPlayhead);

        ImGui::SameLine(0, 24);
        ImGui::SetNextItemWidth(130);
        if (ImGui::SliderInt("Lautstärke", &volumePercent, 0, 150, "%d %%")) engine.masterVolume = float(volumePercent) / 100.f;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Abspiellautstärke (betrifft nur das Abhören, nicht den Song).\nDoppelklick/Strg+Klick: Wert eintippen");

        ImGui::SameLine(0, 24);
        if (ImGui::Button(generator.busy() ? "Komponiere..." : "Generator")) generator.open = !generator.open;
        ImGui::SameLine();
        ImGui::Checkbox("Endlos", &generator.endless);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Endlos-Modus (Radio): spielt Song für Song mit den Generator-Einstellungen.\n"
                              "Der nächste Song wird komponiert, während der aktuelle läuft.");

        // ---------- Spur & Bearbeiten ----------
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("Spur", song.tracks.empty() ? "-" : song.tracks[roll.currentTrack].name.c_str())) {
            for (int i = 0; i < int(song.tracks.size()); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(song.tracks[i].name.c_str(), i == roll.currentTrack)) roll.currentTrack = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Spur")) {
            history.push(song);
            song.tracks.push_back({"Spur " + std::to_string(song.tracks.size() + 1), "Piano"});
            roll.currentTrack = int(song.tracks.size()) - 1;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(song.tracks.size() <= 1);
        if (ImGui::Button("Spur löschen")) {
            history.push(song);
            song.tracks.erase(song.tracks.begin() + roll.currentTrack);
            roll.currentTrack = std::max(0, roll.currentTrack - 1);
            changed = true;
        }
        ImGui::EndDisabled();

        if (!song.tracks.empty()) {
            Track& tr = song.tracks[roll.currentTrack];
            ImGui::SameLine(0, 24);
            ImGui::SetNextItemWidth(130);
            if (ImGui::BeginCombo("Instrument", tr.instrument.c_str())) {
                for (auto& name : InstrumentRegistry::instance().names())
                    if (ImGui::Selectable(name.c_str(), name == tr.instrument)) {
                        history.push(song); tr.instrument = name; changed = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            if (ImGui::SliderFloat("Lautst.", &tr.volume, 0.f, 1.5f, "%.2f")) changed = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Stumm", &tr.muted)) changed = true;
        }

        ImGui::SameLine(0, 24);
        ImGui::Checkbox("Noten zeichnen", &roll.editEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Dynamik zeichnen", &roll.dynEdit);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Unten in der Piano-Roll die orange Lautstärke-Linie der aktuellen Spur mit der Maus malen\n"
                              "(oben = lauter). So kannst du ein Instrument gezielt hervorheben oder zurücknehmen.");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("An: Klick ins Raster setzt Noten, Ziehen verschiebt/verlängert, Rechtsklick löscht.\n"
                              "Aus: Klicks verändern nichts (nur Abspielposition im Lineal setzen).");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::Combo("Raster", &snapIndex, [](void*, int i) { return kSnaps[i].label; }, nullptr,
                         IM_ARRAYSIZE(kSnaps))) {}
        roll.snapTicks = snapToTicks(kSnaps[snapIndex], song.ppq);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderFloat("Anschlag", &roll.newVelocity, 0.1f, 1.f, "%.2f");

        ImGui::SameLine(0, 24);
        ImGui::BeginDisabled(history.undo.empty());
        if (ImGui::Button("Rückgängig")) changed |= history.doUndo(song);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(history.redo.empty());
        if (ImGui::Button("Wiederholen")) changed |= history.doRedo(song);
        ImGui::EndDisabled();

        // ---------- Spielen & Aufnehmen ----------
        if (ImGui::Checkbox("Tastatur spielen", &keyboardPlay) && !keyboardPlay) live.allOff();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Computer-Tastatur als Klavier (welche Taste welcher Ton ist, zeigt der Tab \"Spielen\"):\n"
                              "untere Reihe  Y X C V B N M , . -  (schwarz: S D  G H J  L Ö)\n"
                              "obere Reihe  Q W E R T Z U I O P Ü +  (schwarz: 2 3  5 6 7  9 0 ´)\n"
                              "Pfeil links/rechts = Oktave, Umschalt = Haltepedal, Strg+R = Aufnahme");
        if (keyboardPlay) { ImGui::SameLine(); ImGui::TextDisabled("Oktave %d (Pfeiltasten)", live.octave); }
        ImGui::SameLine(0, 20);
        ImGui::SetNextItemWidth(190);
        const std::string midiLabel = midi.current() >= 0 && midi.current() < int(midiDevices.size())
                                          ? midiDevices[size_t(midi.current())] : std::string("MIDI-Keyboard: aus");
        if (ImGui::BeginCombo("##midi", midiLabel.c_str())) {
            midiDevices = MidiInput::devices();
            if (ImGui::Selectable("aus", midi.current() < 0)) { midi.close(); live.allOff(); }
            for (int i = 0; i < int(midiDevices.size()); ++i)
                if (ImGui::Selectable(midiDevices[size_t(i)].c_str(), midi.current() == i)) {
                    if (midi.open(i, [&live](int st, int d1, int d2) { live.onMidi(st, d1, d2); }))
                        status = "MIDI-Keyboard verbunden: " + midiDevices[size_t(i)];
                    else status = "MIDI-Gerät konnte nicht geöffnet werden (von anderem Programm belegt?)";
                }
            if (midiDevices.empty()) ImGui::TextDisabled("Kein MIDI-Gerät gefunden");
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 20);
        ImGui::Checkbox("Metronom", &metronome);
        ImGui::SameLine();
        ImGui::Checkbox("Einzählen", &countIn);
        ImGui::SameLine(0, 20);
        ImGui::PushStyleColor(ImGuiCol_Button, recording ? ImVec4(0.85f, 0.15f, 0.15f, 1) : ImVec4(0.55f, 0.15f, 0.15f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.25f, 0.25f, 1));
        if (ImGui::Button(recording ? (keyboardPlay ? "  Aufnahme AN (Strg+R)  ###rec" : "  Aufnahme AN (R)  ###rec")
                                    : (keyboardPlay ? "  Aufnahme aus (Strg+R)  ###rec" : "  Aufnahme aus (R)  ###rec")))
            toggleRecording();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Aufnahme scharf schalten, dann ganz normal abspielen (Leertaste).\n"
                              "Alles, was du auf der Tastatur oder dem MIDI-Keyboard spielst, wird live\n"
                              "in die Spur gezeichnet - so lang, wie du die Taste hältst.");
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        ImGui::Combo("##ziel", &recTarget, "in aktuelle Spur\0in neue Spur\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        ImGui::Combo("Quantisieren", &recQuant, "aus\0" "1/8\0" "1/16\0" "1/8 Triole\0");
        ImGui::SameLine(0, 20);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.40f, 0.62f, 1));
        if (ImGui::Button("Idee ausbauen (KI) ...")) ideaOpen = true;
        ImGui::PopStyleColor();
        if (roll.followSuspended || score.followSuspended) {   // man schaut gerade woanders hin
            ImGui::SameLine(0, 20);
            if (ImGui::Button("Zur Abspielposition")) {
                roll.jumpToPlayhead(engine.position());
                score.jumpToPlayhead();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mitlaufen ist pausiert, weil du selbst gescrollt hast.\nBeim nächsten Play läuft die Ansicht wieder mit.");
        }

        // ---------- Tastenkürzel ----------
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) togglePlay();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) changed |= history.doUndo(song);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) changed |= history.doRedo(song);
            if (keyboardPlay && !io.KeyCtrl) {
                live.handleComputerKeyboard(live.velocity);   // Buchstaben spielen Töne (R ist jetzt ein Ton)
            } else if (keyboardPlay && io.KeyCtrl) {
                if (ImGui::IsKeyPressed(ImGuiKey_R, false)) toggleRecording();   // Strg+R = Aufnahme
            } else if (!io.KeyCtrl) {
                if (ImGui::IsKeyPressed(ImGuiKey_G, false)) generator.startGeneration();
                if (ImGui::IsKeyPressed(ImGuiKey_R, false)) toggleRecording();
            }
        }

        // ---------- Generator-Ergebnis übernehmen ----------
        if (generator.endless) {
            auto startEndlessSong = [&](EndlessSong& es) {
                replaceSong(std::move(es.song));
                currentPath.clear();
                generator.info = es.info;
                generator.endlessMood = es.mood;
                ++generator.endlessSongs;
                endlessPlayNow = true;
                endlessEndTime = -1;
                endlessNeedLaunch = true;   // gleich den übernächsten Song komponieren
                status = "Endlos: Song " + std::to_string(generator.endlessSongs) + " (" + es.mood + ")";
            };
            if (!endlessActive) {   // gerade eingeschaltet
                endlessActive = true;
                endlessWaitFirst = true;
                endlessNext.reset();
                endlessEndTime = -1;
                generator.endlessSongs = 0;
                generator.endlessMood.clear();
                if (!generator.busy() || endlessDiscard) endlessNeedLaunch = true;
                endlessDiscard = false;
                status = "Endlos-Modus: erster Song wird komponiert ...";
            }
            Song generated;
            GenerationInfo gi;
            if (generator.takeResult(generated, &gi)) {
                EndlessSong es{std::move(generated), gi, generator.endlessNextMood};
                if (endlessWaitFirst) { endlessWaitFirst = false; startEndlessSong(es); }
                else endlessNext = std::move(es);
            }
            if (endlessNeedLaunch && !generator.busy()) {
                generator.launch(generator.endlessSettings());
                endlessNeedLaunch = false;
            }
            // Song zu Ende (nicht bloß angehalten)? -> kurze Pause, dann der nächste
            const bool atEnd = !endlessWaitFirst && !engine.isPlaying() && !song.tracks.empty() &&
                               engine.position() >= double(song.length()) - 1.0;
            if (atEnd) {
                if (endlessEndTime < 0) endlessEndTime = ImGui::GetTime();
                if (endlessNext && ImGui::GetTime() - endlessEndTime >= double(generator.endlessPause)) {
                    EndlessSong es = std::move(*endlessNext);
                    endlessNext.reset();
                    startEndlessSong(es);
                } else if (!endlessNext) {
                    status = "Endlos: warte auf den nächsten Song ...";
                }
            } else {
                endlessEndTime = -1;
            }
        } else {
            if (endlessActive) {   // gerade ausgeschaltet
                endlessActive = false;
                endlessWaitFirst = endlessNeedLaunch = endlessPlayNow = false;
                endlessNext.reset();
                endlessDiscard = generator.busy();   // laufende Komposition nicht mehr einspielen
                generator.endlessMood.clear();
                status = "Endlos-Modus beendet";
            }
            Song generated;
            if (endlessDiscard) {
                GenerationInfo dropped;
                if (generator.takeResult(generated, &dropped) || !generator.busy()) endlessDiscard = false;
            } else if (std::vector<int> splice; generator.takeResult(generated, nullptr, &splice)) {
                if (!splice.empty()) {
                    // Nur einzelne Abschnitte neu: in den bestehenden Song einsetzen, Wiedergabe läuft weiter
                    history.push(song);
                    const std::string title = song.title;
                    spliceSections(song, generated, splice);
                    song.title = title;
                    roll.currentTrack = std::min(roll.currentTrack, std::max(0, int(song.tracks.size()) - 1));
                    changed = true;
                    std::string names;
                    for (int i : splice)
                        if (i >= 0 && i < int(song.sections.size())) names += (names.empty() ? "" : ", ") + song.sections[size_t(i)].name;
                    status = "Neu gesetzt: " + names;
                } else {
                    replaceSong(std::move(generated));
                    currentPath.clear();   // neuer Song -> beim Speichern nach Namen fragen
                    status = "Neuer Song: " + song.title;
                }
            }
        }

        ImGui::End();   // Werkzeuge

        // ---------- Piano-Roll (eigenes Fenster) ----------
        roll.loopEnabled = loop;
        roll.loopStart = song.bar(loopFrom - 1);
        roll.loopEnd = song.bar(loopTo);
        if (showRoll) {
            if (ImGui::Begin("Piano-Roll", &showRoll, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
                changed |= roll.draw();
            ImGui::End();
        }

        // ---------- Noten (eigenes Fenster) ----------
        if (showScore) {
            if (ImGui::Begin("Noten", &showScore, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                ImGui::Checkbox("Alle Spuren", &score.allTracks);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110);
                ImGui::SliderFloat("Zoom", &score.zoom, 0.5f, 2.5f, "%.1f");
                ImGui::SameLine();
                ImGui::TextDisabled("Klick auf einen Takt = dorthin springen");
                score.currentTrack = roll.currentTrack;
                score.follow = roll.followPlayhead;
                score.draw();
            }
            ImGui::End();
        }

        // ---------- Zusatz-Ansichten ----------
        {
            ViewContext vctx{song, engine, roll.currentTrack, [&] { history.push(song); },
                             [&](double t) {
                                 engine.seek(std::max(0.0, t));
                                 roll.jumpToPlayhead(std::max(0.0, t));
                             }};
            // Neue Fenster beim ersten Mal als Tab neben die Piano-Roll legen (auch bei altem Layout)
            auto dockNextToRoll = [&] {
                if (ImGuiWindow* w = ImGui::FindWindowByName("Piano-Roll"))
                    if (w->DockId) ImGui::SetNextWindowDockID(w->DockId, ImGuiCond_FirstUseEver);
            };
            const ImGuiWindowFlags vf = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
            if (showArrange) {
                dockNextToRoll();
                if (ImGui::Begin("Arrangement", &showArrange, vf)) changed |= arrangeView.draw(vctx);
                ImGui::End();
            }
            if (showChords) {
                dockNextToRoll();
                if (ImGui::Begin("Akkordblatt", &showChords, vf)) chordView.draw(vctx);
                ImGui::End();
            }
            if (showDrums) {
                dockNextToRoll();
                if (ImGui::Begin("Drum-Raster", &showDrums, vf)) changed |= drumView.draw(vctx);
                ImGui::End();
            }
            if (showVis) {
                dockNextToRoll();
                if (ImGui::Begin("Visualizer", &showVis, vf)) visView.draw(vctx);
                ImGui::End();
            }
            if (showPlay) {
                dockNextToRoll();
                static bool playWasVisible = false;
                const bool playVisible = ImGui::Begin("Spielen", &showPlay, vf);
                if (playVisible) {
                    // Wechselt man (nach dem Start) auf diesen Tab, spielt die Tastatur sofort Töne
                    if (!playWasVisible && ImGui::GetFrameCount() > 30 && !keyboardPlay) keyboardPlay = true;
                    changed |= playView.draw(vctx, live, keyboardPlay, recording, recTrack);
                }
                playWasVisible = playVisible;
                ImGui::End();
            }
            if (showMixer) {
                dockNextToRoll();
                if (ImGui::Begin("Mischpult", &showMixer, vf)) changed |= mixerView.draw(vctx);
                ImGui::End();
            }
        }

        generator.draw(&song);

        // ---------- KI: eigene Idee ausbauen ----------
        if (ideaOpen && !song.tracks.empty()) {
            ImGui::SetNextWindowSize({470, 560}, ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Idee ausbauen", &ideaOpen, ImGuiWindowFlags_NoCollapse)) {
                const Track& tr = song.tracks[size_t(roll.currentTrack)];
                ImGui::TextWrapped("Spiel eine Melodie oder Akkorde ein (oder zeichne sie). Die KI übernimmt deine Idee "
                                   "unverändert, findet passende Harmonien und baut daraus einen ganzen Song im selben Feeling.");
                ImGui::Separator();
                ImGui::Text("Quelle: Spur \"%s\"", tr.name.c_str());
                ImGui::RadioButton("ganze Spur", &ideaRange, 0);
                ImGui::SameLine();
                ImGui::RadioButton("nur Loop-Bereich", &ideaRange, 1);

                // Bereich bestimmen und analysieren
                const Tick bar = song.ticksPerBar();
                Tick from = 0, to = 0;
                if (ideaRange == 1) { from = song.bar(loopFrom - 1); to = song.bar(loopTo); }
                else if (!tr.notes.empty()) {
                    Tick first = tr.notes.front().start, last = 0;
                    for (auto& n : tr.notes) { first = std::min(first, n.start); last = std::max(last, n.end()); }
                    from = first / bar * bar;
                    to = (last + bar - bar / 8) / bar * bar;
                    to = std::max(to, from + bar);
                }
                std::vector<Note> ideaNotes = extractRange(tr, from, to);
                const int bars = int((to - from) / bar);
                if (ideaQuant > 0) quantizeNotes(ideaNotes, quantGrid(ideaQuant), 1.f, true);
                const KeyGuess g = detectKey(ideaNotes);
                const Feel f = analyzeFeel(ideaNotes, song, std::max(1, bars));

                ImGui::SeparatorText("Analyse");
                if (ideaNotes.empty()) ImGui::TextColored({1, 0.6f, 0.4f, 1}, "Keine Noten in diesem Bereich.");
                else {
                    ImGui::Text("%zu Noten in %d Takten (%s)", ideaNotes.size(), bars, f.harmonic ? "Akkorde" : "Melodie");
                    ImGui::Text("Tonart erkannt: %s %s   (Sicherheit %.0f %%)", noteName(g.key), scaleName(g.scale), g.confidence * 100);
                    ImGui::Text("Dichte: %.1f Noten/Schlag   Synkopen: %.0f %%   Lage: %s bis %s",
                                f.notesPerBeat, f.offbeatRatio * 100, (std::string(noteName(f.lowest)) + std::to_string(f.lowest / 12 - 1)).c_str(),
                                (std::string(noteName(f.highest)) + std::to_string(f.highest / 12 - 1)).c_str());
                }

                ImGui::SeparatorText("Wie soll die KI damit umgehen?");
                ImGui::RadioButton("Song beginnt mit meiner Idee (Strophe)", &ideaRole, 0);
                ImGui::RadioButton("Meine Idee ist der Refrain (Hook)", &ideaRole, 1);
                ImGui::SetNextItemWidth(160);
                ImGui::Combo("Quantisieren##idee", &ideaQuant, "aus\0" "1/8\0" "1/16\0" "1/8 Triole\0");
                ImGui::Checkbox("Feeling übernehmen (Dichte, Synkopen, Energie)", &ideaFeel);
                ImGui::Checkbox("Schiefe Töne auf betonten Zählzeiten korrigieren", &ideaCorrect);
                ImGui::SetNextItemWidth(160);
                if (ImGui::BeginCombo("Tonart##idee", ideaKey < 0 ? "wie erkannt" : noteName(ideaKey))) {
                    if (ImGui::Selectable("wie erkannt", ideaKey < 0)) ideaKey = -1;
                    for (int k = 0; k < 12; ++k) if (ImGui::Selectable(noteName(k), ideaKey == k)) ideaKey = k;
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160);
                if (ImGui::BeginCombo("Skala##idee", ideaScale < 0 ? "wie erkannt" : scaleName(ScaleType(ideaScale)))) {
                    if (ImGui::Selectable("wie erkannt", ideaScale < 0)) ideaScale = -1;
                    for (int k = 0; k < int(ScaleType::Count); ++k)
                        if (ImGui::Selectable(scaleName(ScaleType(k)), ideaScale == k)) ideaScale = k;
                    ImGui::EndCombo();
                }
                int ens = int(generator.settings.ensemble);
                ImGui::SetNextItemWidth(160);
                if (ImGui::Combo("Besetzung##idee", &ens, "Band\0Sinfonieorchester\0")) generator.settings.ensemble = Ensemble(ens);
                ImGui::SetNextItemWidth(160);
                float len = float(generator.settings.lengthSeconds);
                if (ImGui::SliderFloat("Songlänge", &len, 30.f, 300.f, "%.0f s")) generator.settings.lengthSeconds = len;
                ImGui::TextDisabled("Weitere Einstellungen (Instrumente, Kreativität ...) kommen aus dem Generator-Fenster.");

                ImGui::Spacing();
                ImGui::BeginDisabled(ideaNotes.empty() || generator.busy());
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.45f, 0.30f, 1));
                if (ImGui::Button(generator.busy() ? "Komponiere ..." : "Song aus meiner Idee bauen", {-1, 36})) {
                    GeneratorSettings s = generator.settings;
                    s.seed = 0;
                    s.bpm = song.bpm;
                    s.beatsPerBar = song.beatsPerBar;
                    s.structure.clear();
                    s.key = ideaKey >= 0 ? ideaKey : g.key;
                    s.scale = ideaScale >= 0 ? ideaScale : int(g.scale);
                    if (ideaCorrect) correctToScale(ideaNotes, s.key, ScaleType(s.scale), song);
                    if (ideaFeel) {
                        s.melodyDensity = std::clamp((f.notesPerBeat - 0.4f) / 1.4f, 0.05f, 1.f);
                        s.syncopation = std::clamp(f.offbeatRatio * 1.6f, 0.f, 1.f);
                        s.energy = std::clamp((f.meanVelocity - 0.3f) / 0.6f, 0.1f, 1.f);
                    }
                    auto idea = std::make_shared<MusicalIdea>();
                    idea->notes = ideaNotes;
                    idea->bars = std::max(1, bars);
                    idea->harmonic = f.harmonic;
                    idea->asChorus = ideaRole == 1;
                    if (f.harmonic) s.chordInstrument = tr.instrument; else s.melodyInstrument = tr.instrument;
                    s.idea = idea;
                    generator.launch(s);
                    status = "Die KI baut einen Song aus deiner Idee ...";
                }
                ImGui::PopStyleColor();
                ImGui::EndDisabled();
            }
            ImGui::End();
        }

        // ---------- Rückfrage bei ungespeicherten Änderungen ----------
        if (openUnsavedPopup) { ImGui::OpenPopup("Ungespeicherte Änderungen"); openUnsavedPopup = false; }
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImGuiCond_Always, {0.5f, 0.5f});
        if (ImGui::BeginPopupModal("Ungespeicherte Änderungen", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("\"%s\" hat ungespeicherte Änderungen.", baseName().c_str());
            ImGui::Text("Vorher speichern?");
            ImGui::Spacing();
            if (ImGui::Button("Speichern", {120, 0})) {
                ImGui::CloseCurrentPopup();
                if (saveAs(false)) doPending(pending);
                pending = Pending::None;
            }
            ImGui::SameLine();
            if (ImGui::Button("Verwerfen", {120, 0})) {
                ImGui::CloseCurrentPopup();
                dirty = false;
                doPending(pending);
                pending = Pending::None;
            }
            ImGui::SameLine();
            if (ImGui::Button("Abbrechen", {120, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ImGui::CloseCurrentPopup();
                pending = Pending::None;
            }
            ImGui::EndPopup();
        }

        if (changed) {
            engine.loadSong(song);
            dirty = !loadedClean;
        }
        if (endlessPlayNow) {   // Endlos-Modus: neuer Song startet von vorn
            endlessPlayNow = false;
            engine.setLoop(false, 0, 0);
            engine.seek(0);
            engine.play();
            roll.jumpToPlayhead(0);
        }

        // Fenstertitel: Name + * bei ungespeicherten Änderungen
        std::string title = baseName() + (dirty ? " *" : "") + "  -  MusicEngine";
        if (title != shownTitle) { glfwSetWindowTitle(window, title.c_str()); shownTitle = title; }
        if (quitNow) break;
        if (changed || loopChanged) engine.setLoop(loop, song.bar(loopFrom - 1), song.bar(loopTo));

        // Frei schwebende Fenster nicht über Menü und Werkzeugleiste schieben lassen
        {
            const float topY = ImGui::GetMainViewport()->WorkPos.y + toolbarHeight;
            for (ImGuiWindow* w : GImGui->Windows) {
                if (!w->WasActive || w->DockIsActive || w->ParentWindow) continue;
                if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip |
                                ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoMove)) continue;   // Menü, Leiste, Andockfläche
                if (w->Pos.y < topY) ImGui::SetWindowPos(w, ImVec2(w->Pos.x, topY), ImGuiCond_Always);
            }
        }

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.08f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    engine.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}
