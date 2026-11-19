// Notensatz für die Partituransicht.
// Glyphen aus der SMuFL-Schrift "Bravura" (1 em = 4 Notenlinien-Abstände).
#include "ScoreView.h"
#include "analysis/Analyzer.h"
#include "audio/AudioEngine.h"
#include "instruments/InstrumentRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <vector>

ImFont* ScoreView::font_ = nullptr;

namespace {
// SMuFL-Codepunkte
enum : unsigned {
    G_CLEF = 0xE050, F_CLEF = 0xE062, PERC_CLEF = 0xE069, BRACE = 0xE000,
    HEAD_WHOLE = 0xE0A2, HEAD_HALF = 0xE0A3, HEAD_BLACK = 0xE0A4, HEAD_X = 0xE0A9,
    DOT = 0xE1E7, FLAG8_UP = 0xE240, FLAG8_DOWN = 0xE241, FLAG16_UP = 0xE242, FLAG16_DOWN = 0xE243,
    FLAT = 0xE260, NATURAL = 0xE261, SHARP = 0xE262,
    REST_WHOLE = 0xE4E3, REST_HALF = 0xE4E4, REST_QUARTER = 0xE4E5, REST_8 = 0xE4E6, REST_16 = 0xE4E7,
    TIME0 = 0xE080, MET_QUARTER = 0xECA5,
};

const ImU32 kInk = IM_COL32(20, 20, 24, 255);
const ImU32 kPaper = IM_COL32(250, 248, 242, 255);

std::string utf8(unsigned cp) {
    std::string s;
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    return s;
}

enum class Clef { Treble, Bass, Perc };

struct StaffDef {
    int track = 0;
    Clef clef = Clef::Treble;
    int lo = 0, hi = 127;        // Tonhöhen-Filter (Klaviersystem: oben >= 60)
    bool braceTop = false;       // erstes von zwei Systemen einer Spur
    bool drums = false;
    std::string label;
};

// Ein Notenwert/Pausenwert innerhalb eines Takts (Einheit: Sechzehntel)
struct Piece {
    int start = 0, len = 0;
    std::vector<int> pitches;
    bool rest = false, tieNext = false, fullRest = false;
};

// Spelling: Tonhöhe -> (Stammton 0..6, Alteration) abhängig von der Vorzeichnung
struct Spelled { int letter; int alter; int dia; };
Spelled spell(int pitch, int fifths) {
    static const int sharpL[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6}, sharpA[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    static const int flatL[12] = {0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6}, flatA[12] = {0, -1, 0, -1, 0, 0, -1, 0, -1, 0, -1, 0};
    const int pc = ((pitch % 12) + 12) % 12, oct = pitch / 12 - 1;
    const int l = fifths >= 0 ? sharpL[pc] : flatL[pc];
    const int a = fifths >= 0 ? sharpA[pc] : flatA[pc];
    return {l, a, oct * 7 + l};
}
int keyAlter(int letter, int fifths) {
    static const int sharpOrder[7] = {3, 0, 4, 1, 5, 2, 6}, flatOrder[7] = {6, 2, 5, 1, 4, 0, 3};
    for (int i = 0; i < std::abs(fifths) && i < 7; ++i)
        if ((fifths > 0 ? sharpOrder[i] : flatOrder[i]) == letter) return fifths > 0 ? 1 : -1;
    return 0;
}
// Anzahl Vorzeichen (+ Kreuze / - Bs) der Dur-Tonart mit Grundton pc
int majorFifths(int pc) {
    static const int f[12] = {0, -5, 2, -3, 4, -1, -6, 1, -4, 3, -2, 5};
    return f[((pc % 12) + 12) % 12];
}
int fifthsFor(int key, int scale) {
    // Parallele Dur-Tonart der Skala (Kirchentonarten = Modi einer Dur-Leiter)
    int off = 0;
    switch (ScaleType(scale)) {
    case ScaleType::NaturalMinor: case ScaleType::HarmonicMinor: case ScaleType::MinorPentatonic: off = 3; break;
    case ScaleType::Dorian: off = -2; break;
    case ScaleType::Phrygian: off = -4; break;
    case ScaleType::Lydian: off = -5; break;
    case ScaleType::Mixolydian: off = -7; break;
    default: off = 0; break;
    }
    return majorFifths(key + off);
}

// Schlagzeug: Tonhöhe -> Notenlinien-Position und Kreuz-Notenkopf
void drumPos(int pitch, int& pos, bool& xhead) {
    xhead = false;
    switch (pitch) {
    case 35: case 36: pos = 1; break;
    case 38: case 40: pos = 5; break;
    case 37: case 39: pos = 5; xhead = true; break;
    case 42: case 44: case 46: pos = 9; xhead = true; break;
    case 49: case 57: case 52: case 55: pos = 10; xhead = true; break;
    case 51: case 59: case 53: pos = 8; xhead = true; break;
    case 50: pos = 7; break;
    case 48: pos = 6; break;
    case 47: pos = 6; break;
    case 45: pos = 4; break;
    case 43: pos = 2; break;
    case 41: pos = 0; break;
    default: pos = 4; xhead = true; break;
    }
}

int diaPos(int dia, Clef c) { return c == Clef::Bass ? dia - 18 : dia - 30; }   // 0 = unterste Linie

// Takt einer Notenzeile in Notenwerte zerlegen
std::vector<Piece> buildMeasure(const Track& tr, const StaffDef& sd, Tick m0, Tick six, int steps) {
    struct Seg { int qs, qe, pitch; bool tieOut; };
    std::vector<Seg> segs;
    const Tick m1 = m0 + Tick(steps) * six;
    for (const Note& n : tr.notes) {
        if (n.pitch < sd.lo || n.pitch > sd.hi) continue;
        if (n.end() <= m0 + six / 4 || n.start >= m1) continue;
        int qs = n.start <= m0 ? 0 : int(std::llround(double(n.start - m0) / double(six)));
        if (qs >= steps) continue;   // gehört (gerundet) zum nächsten Takt
        int qe = n.end() >= m1 ? steps : int(std::llround(double(n.end() - m0) / double(six)));
        qe = std::clamp(qe, qs + 1, steps);
        segs.push_back({qs, qe, n.pitch, n.end() > m1 + six / 2});
    }
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.qs < b.qs; });

    // Einsätze zu Akkorden gruppieren (eine Stimme pro Notenzeile)
    std::vector<Piece> events;
    for (size_t i = 0; i < segs.size();) {
        Piece p;
        p.start = segs[i].qs;
        int end = 0;
        size_t j = i;
        for (; j < segs.size() && segs[j].qs == p.start; ++j) {
            if (std::find(p.pitches.begin(), p.pitches.end(), segs[j].pitch) == p.pitches.end()) p.pitches.push_back(segs[j].pitch);
            end = std::max(end, segs[j].qe);
            p.tieNext = p.tieNext || segs[j].tieOut;
        }
        const int nextStart = j < segs.size() ? segs[j].qs : steps;
        if (end > nextStart) { end = nextStart; p.tieNext = false; }
        p.len = std::max(1, end - p.start);
        std::sort(p.pitches.begin(), p.pitches.end());
        events.push_back(p);
        i = j;
    }

    std::vector<Piece> out;
    if (events.empty()) {
        Piece r;
        r.start = 0; r.len = steps; r.rest = true; r.fullRest = true;
        out.push_back(r);
        return out;
    }
    static const int vals[] = {16, 12, 8, 6, 4, 3, 2, 1};
    auto fits = [&](int st, int v, bool rest) {
        if (st + v > steps) return false;
        int align = v == 16 ? 16 : v == 12 ? steps : v == 8 ? 8 : v == 6 ? 4 : v == 4 ? 4 : v == 3 ? 2 : v == 2 ? 2 : 1;
        if (steps % 8 != 0 && v == 8) align = 4;   // z.B. 3/4: Halbe auf jedem Schlag
        if (st % align) return false;
        if (rest && (v == 12 || v == 6 || v == 3)) return false;   // keine punktierten Pausen
        return true;
    };
    auto decompose = [&](int st, int len, const Piece& proto) {
        while (len > 0) {
            int v = 1;
            for (int c : vals) if (c <= len && fits(st, c, proto.rest)) { v = c; break; }
            Piece p = proto;
            p.start = st;
            p.len = v;
            p.tieNext = !proto.rest && (len > v || proto.tieNext);
            out.push_back(p);
            st += v;
            len -= v;
        }
    };
    int cursor = 0;
    Piece restProto;
    restProto.rest = true;
    for (auto& ev : events) {
        if (ev.start > cursor) decompose(cursor, ev.start - cursor, restProto);
        decompose(ev.start, ev.len, ev);
        cursor = ev.start + ev.len;
    }
    if (cursor < steps) decompose(cursor, steps - cursor, restProto);
    return out;
}
}   // namespace

// ImGui skaliert Schriften so, dass (Oberlänge - Unterlänge) = Pixelgröße ist. SMuFL-Schriften
// rechnen aber in "em" (1 em = 4 Linienabstände) und haben riesige Ober-/Unterlängen.
// Darum lesen wir unitsPerEm (Tabelle 'head') und ascender/descender ('hhea') aus der Datei.
float ScoreView::emRatio_ = 1.f;

static float readEmRatio(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) { return o + 2 <= d.size() ? unsigned(d[o] << 8 | d[o + 1]) : 0u; };
    auto u32 = [&](size_t o) { return o + 4 <= d.size() ? (size_t(u16(o)) << 16 | u16(o + 2)) : size_t(0); };
    const unsigned numTables = u16(4);
    size_t head = 0, hhea = 0;
    for (unsigned i = 0; i < numTables; ++i) {
        const size_t rec = 12 + size_t(i) * 16;
        if (rec + 16 > d.size()) break;
        const std::string tag(reinterpret_cast<const char*>(&d[rec]), 4);
        if (tag == "head") head = u32(rec + 8);
        if (tag == "hhea") hhea = u32(rec + 8);
    }
    if (!head || !hhea) return 1.f;
    const float upem = float(u16(head + 18));
    const float asc = float(int16_t(u16(hhea + 4))), desc = float(int16_t(u16(hhea + 6)));
    return (upem > 0 && asc - desc > 0) ? upem / (asc - desc) : 1.f;
}

bool ScoreView::loadFont(const std::string& path) {
    const std::filesystem::path p(std::u8string(path.begin(), path.end()));
    if (!std::filesystem::exists(p)) return false;
    emRatio_ = std::clamp(readEmRatio(p), 0.05f, 1.f);
    static const ImWchar ranges[] = {0xE000, 0xE0FF, 0xE1E0, 0xE1FF, 0xE240, 0xE26F, 0xE4E0, 0xE4FF, 0xE080, 0xE09F,
                                     0xE520, 0xE54F, 0xECA0, 0xECAF, 0};
    ImFontConfig cfg;
    cfg.OversampleH = 1;
    // Glyphen im Atlas mit 1 em = 64 px rendern
    font_ = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), 64.f / emRatio_, &cfg, ranges);
    return font_ != nullptr;
}

void ScoreView::draw() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 200 || avail.y < 100) return;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kPaper));
    ImGui::BeginChild("##score", avail, ImGuiChildFlags_None, ImGuiWindowFlags_NoMove);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;

    if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0)
        zoom = std::clamp(zoom * std::pow(1.1f, io.MouseWheel), 0.5f, 2.5f);

    const float sp = 8.f * zoom;   // Abstand zweier Notenlinien
    const float fontPx = 4.f * sp / emRatio_;   // ImGui-Größe, bei der 1 em = 4 Linienabstände
    auto glyph = [&](unsigned cp, float x, float baseline, ImU32 col = kInk, float scale = 1.f) {
        if (!font_) return;
        const float size = fontPx * scale;
        dl->AddText(font_, size, ImVec2(x, baseline - font_->Ascent * size / font_->FontSize), col, utf8(cp).c_str());
    };
    auto glyphW = [&](unsigned cp) {
        if (!font_) return sp * 1.2f;
        return font_->CalcTextSizeA(fontPx, 1e9f, 0.f, utf8(cp).c_str()).x;
    };
    const float headW = glyphW(HEAD_BLACK);

    // ---------- Notenzeilen festlegen ----------
    std::vector<StaffDef> staves;
    for (int t = 0; t < int(song_.tracks.size()); ++t) {
        if (!allTracks && t != currentTrack) continue;
        const Track& tr = song_.tracks[size_t(t)];
        if (allTracks && tr.notes.empty() && song_.tracks.size() > 1) continue;
        static std::map<std::string, bool> drumCache;   // Instrument einmal fragen, nicht jedes Bild neu anlegen
        auto dc = drumCache.find(tr.instrument);
        if (dc == drumCache.end()) {
            auto inst = InstrumentRegistry::instance().create(tr.instrument);
            dc = drumCache.emplace(tr.instrument, inst && inst->isDrumKit()).first;
        }
        StaffDef sd;
        sd.track = t;
        sd.label = tr.name;
        if (dc->second) {
            sd.clef = Clef::Perc;
            sd.drums = true;
            staves.push_back(sd);
            continue;
        }
        int lo = 127, hi = 0;
        double sum = 0;
        for (auto& n : tr.notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); sum += n.pitch; }
        const double avg = tr.notes.empty() ? 65 : sum / double(tr.notes.size());
        if (!tr.notes.empty() && lo < 55 && hi > 67 && hi - lo > 20) {   // Klaviersystem
            StaffDef top = sd, bot = sd;
            top.clef = Clef::Treble; top.lo = 60; top.braceTop = true;
            bot.clef = Clef::Bass; bot.hi = 59; bot.label.clear();
            staves.push_back(top);
            staves.push_back(bot);
        } else {
            sd.clef = avg < 57 ? Clef::Bass : Clef::Treble;
            staves.push_back(sd);
        }
    }
    if (staves.empty()) {
        ImGui::TextColored({0.2f, 0.2f, 0.2f, 1}, "Keine Noten vorhanden.");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    // ---------- Tonart / Vorzeichen ----------
    int key = song_.key, scale = song_.scale;
    if (key < 0 || scale < 0) {
        std::vector<Note> all;
        for (auto& sd : staves) if (!sd.drums) for (auto& n : song_.tracks[size_t(sd.track)].notes) all.push_back(n);
        KeyGuess g = detectKey(all);
        key = g.key;
        scale = int(g.scale);
    }
    const int fifths = fifthsFor(key, scale);

    // ---------- Takte zerlegen ----------
    const Tick six = song_.ppq / 4;
    const int steps = int(song_.ticksPerBar() / six);
    const int measures = std::max(1, int((song_.length() + song_.ticksPerBar() - 1) / song_.ticksPerBar()));
    std::vector<std::vector<std::vector<Piece>>> pieces(static_cast<size_t>(measures));   // [Takt][Zeile]
    std::vector<std::vector<float>> onsetX(static_cast<size_t>(measures));                // relative x je Sechzehntel
    std::vector<float> mWidth(static_cast<size_t>(measures));
    const float mPad = 1.6f * sp;
    for (int m = 0; m < measures; ++m) {
        const Tick m0 = Tick(m) * song_.ticksPerBar();
        std::set<int> onsets;
        pieces[size_t(m)].resize(staves.size());
        for (size_t s = 0; s < staves.size(); ++s) {
            pieces[size_t(m)][s] = buildMeasure(song_.tracks[size_t(staves[s].track)], staves[s], m0, six, steps);
            for (auto& p : pieces[size_t(m)][s]) if (!p.fullRest) onsets.insert(p.start);
        }
        onsets.insert(steps);
        if (onsets.size() == 1) onsets.insert(0);
        // Abstand nach Dauer (wie im Notenstich: längere Werte bekommen mehr, aber nicht proportional mehr Platz)
        std::vector<float>& ox = onsetX[size_t(m)];
        ox.assign(size_t(steps + 1), 0.f);
        float x = 0;
        int prev = -1;
        for (int o : onsets) {
            if (prev >= 0) {
                const float seg = sp * (1.5f + 1.7f * std::sqrt(float(o - prev)));
                for (int k = prev; k <= o; ++k) ox[size_t(k)] = x + seg * float(k - prev) / float(o - prev);
                x += seg;
            }
            prev = o;
        }
        mWidth[size_t(m)] = std::max(x + mPad * 1.5f, sp * 6.f);
    }

    // ---------- Zeilenumbruch in Systeme ----------
    const float nameW = 95.f * zoom, braceW = 1.2f * sp;
    const float clefW = 3.2f * sp, keyW = float(std::abs(fifths)) * 1.05f * sp + 0.6f * sp, timeW = 2.6f * sp;
    const float left = 16.f + nameW + braceW;
    struct System { int first, count; float scale; float header; };
    std::vector<System> systems;
    {
        int m = 0;
        while (m < measures) {
            const float header = clefW + keyW + (systems.empty() ? timeW : 0.f);
            const float rowW = width - left - header - 20.f;
            float sum = 0;
            int c = 0;
            while (m + c < measures && (c == 0 || sum + mWidth[size_t(m + c)] <= rowW)) sum += mWidth[size_t(m + c++)];
            float stretch = rowW / sum;
            if (m + c >= measures && stretch > 1.35f) stretch = 1.f;   // letzte Zeile nicht auseinanderziehen
            systems.push_back({m, c, std::max(stretch, 0.5f), header});
            m += c;
        }
    }

    // Vertikale Abstände
    std::vector<float> staffY(staves.size());
    float y = 0;
    for (size_t s = 0; s < staves.size(); ++s) {
        staffY[s] = y;
        const bool grandPair = staves[s].braceTop;
        y += 4 * sp + (grandPair ? 4.5f * sp : 6.5f * sp);
    }
    const float systemH = y + 3.f * sp;
    const float headerH = 70.f * zoom;

    // ---------- Kopf: Titel, Tempo ----------
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + headerH + systemH * float(systems.size()) + 60), kPaper);
    {
        ImFont* f = ImGui::GetFont();
        const float ts = 22.f * zoom;
        const ImVec2 tsz = f->CalcTextSizeA(ts, 1e9f, 0, song_.title.c_str());
        dl->AddText(f, ts, ImVec2(origin.x + (width - tsz.x) * 0.5f, origin.y + 10 * zoom), kInk, song_.title.c_str());
        glyph(MET_QUARTER, origin.x + left, origin.y + headerH - 8 * zoom, kInk, 0.8f);
        char buf[32];
        std::snprintf(buf, sizeof buf, "= %.0f", song_.bpm);
        dl->AddText(f, 15 * zoom, ImVec2(origin.x + left + 1.6f * sp, origin.y + headerH - 8 * zoom - 15 * zoom), kInk, buf);
    }

    // ---------- Abspielposition ----------
    const double playTick = engine_.position();
    const int playM = int(playTick / double(song_.ticksPerBar()));
    const bool playing = engine_.isPlaying();
    // Selbst gescrollt (Mausrad/Scrollbalken)? Dann Mitlaufen bis zum nächsten Play pausieren.
    {
        const float sc = ImGui::GetScrollY();
        if (playing && !wasPlaying_) followSuspended = false;
        else if (playing && !autoScrolled_ && std::abs(sc - prevScroll_) > 0.5f) followSuspended = true;
        wasPlaying_ = playing;
        autoScrolled_ = false;
        prevScroll_ = sc;
    }

    const ImVec2 clipMin = dl->GetClipRectMin(), clipMax = dl->GetClipRectMax();
    const ImVec2 mouse = io.MousePos;
    const bool clicked = ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    // Sektions-Starts (für Probenziffern)
    std::map<int, std::string> sectionAt;
    for (auto& sec : song_.sections) sectionAt[int(sec.start / song_.ticksPerBar())] = sec.name;

    for (size_t si = 0; si < systems.size(); ++si) {
        const System& sys = systems[si];
        const float sy = origin.y + headerH + float(si) * systemH;
        const float x0 = origin.x + left;

        // Takt-x-Positionen
        std::vector<float> mx(size_t(sys.count + 1));
        mx[0] = x0 + sys.header;
        for (int c = 0; c < sys.count; ++c) mx[size_t(c + 1)] = mx[size_t(c)] + mWidth[size_t(sys.first + c)] * sys.scale;

        // Mitscrollen
        if (((follow && playing && !followSuspended) || forceJump_) && playM >= sys.first && playM < sys.first + sys.count) {
            const float top = sy - origin.y, bottom = top + systemH;
            const float sc = ImGui::GetScrollY(), vh = ImGui::GetWindowHeight();
            if (forceJump_ || top < sc || bottom > sc + vh) {
                ImGui::SetScrollY(std::max(0.f, top - 20.f));
                autoScrolled_ = true;
            }
            forceJump_ = false;
        }
        if (sy > clipMax.y || sy + systemH < clipMin.y) continue;   // nicht sichtbar

        const float sysTop = sy + staffY.front(), sysBot = sy + staffY.back() + 4 * sp;

        // Aktueller Takt hervorheben + Klick zum Springen
        for (int c = 0; c < sys.count; ++c) {
            const int m = sys.first + c;
            if (m == playM && playTick >= 0) {
                dl->AddRectFilled(ImVec2(mx[size_t(c)], sysTop - sp), ImVec2(mx[size_t(c + 1)], sysBot + sp), IM_COL32(120, 170, 255, 45));
                const float frac = float((playTick - double(m) * double(song_.ticksPerBar())) / double(song_.ticksPerBar()));
                const float px = mx[size_t(c)] + mPad * sys.scale + onsetX[size_t(m)][size_t(std::clamp(int(frac * float(steps)), 0, steps))] * sys.scale;
                dl->AddLine(ImVec2(px, sysTop - sp), ImVec2(px, sysBot + sp), IM_COL32(230, 120, 20, 200), 2.f);
            }
            if (clicked && mouse.x >= mx[size_t(c)] && mouse.x < mx[size_t(c + 1)] && mouse.y >= sysTop - 2 * sp && mouse.y <= sysBot + 2 * sp)
                engine_.seek(double(Tick(m) * song_.ticksPerBar()));
            // Probenziffer / Abschnittsname
            auto it = sectionAt.find(m);
            if (it != sectionAt.end()) {
                ImFont* f = ImGui::GetFont();
                const float fs = 14.f * zoom;
                const ImVec2 sz = f->CalcTextSizeA(fs, 1e9f, 0, it->second.c_str());
                const ImVec2 p0(mx[size_t(c)] + 2, sysTop - 4.2f * sp - sz.y);
                dl->AddRect(ImVec2(p0.x - 3, p0.y - 2), ImVec2(p0.x + sz.x + 3, p0.y + sz.y + 2), kInk, 2.f, 0, 1.2f);
                dl->AddText(f, fs, p0, kInk, it->second.c_str());
            }
        }
        // Taktnummer
        {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%d", sys.first + 1);
            dl->AddText(ImGui::GetFont(), 12.f * zoom, ImVec2(x0, sysTop - 3.2f * sp), IM_COL32(90, 90, 90, 255), buf);
        }

        for (size_t s = 0; s < staves.size(); ++s) {
            const StaffDef& sd = staves[s];
            const float top = sy + staffY[s];
            const float bottomLine = top + 4 * sp;
            auto posY = [&](int pos) { return bottomLine - float(pos) * sp * 0.5f; };

            // Instrumentname
            if (!sd.label.empty()) {
                ImFont* f = ImGui::GetFont();
                const float fs = 13.f * zoom;
                const float ly = sd.braceTop ? top + 6 * sp : top + 1.2f * sp;
                const ImVec2 sz = f->CalcTextSizeA(fs, nameW, 0, sd.label.c_str());
                dl->AddText(f, fs, ImVec2(origin.x + 10 + nameW - sz.x - 6, ly), kInk, sd.label.c_str());
            }
            // Linien
            const float xEnd = mx.back();
            for (int l = 0; l < 5; ++l) dl->AddLine(ImVec2(x0, top + float(l) * sp), ImVec2(xEnd, top + float(l) * sp), kInk, 1.f);
            // Klammer fürs Klaviersystem
            if (sd.braceTop) glyph(BRACE, x0 - braceW, top + 12.5f * sp, kInk, 12.5f / 4.f);
            // Schlüssel
            if (sd.clef == Clef::Treble) glyph(G_CLEF, x0 + 0.4f * sp, posY(2));
            else if (sd.clef == Clef::Bass) glyph(F_CLEF, x0 + 0.4f * sp, posY(6));
            else glyph(PERC_CLEF, x0 + 0.8f * sp, posY(4));
            // Vorzeichnung
            if (!sd.drums && fifths != 0) {
                static const int sharpPos[7] = {8, 5, 9, 6, 3, 7, 4}, flatPos[7] = {4, 7, 3, 6, 2, 5, 1};
                const int shift = sd.clef == Clef::Bass ? -2 : 0;
                for (int i = 0; i < std::abs(fifths); ++i) {
                    const int pos = (fifths > 0 ? sharpPos[i] : flatPos[i]) + shift;
                    glyph(fifths > 0 ? SHARP : FLAT, x0 + clefW + float(i) * 1.05f * sp, posY(pos));
                }
            }
            // Taktart (nur erste Zeile)
            if (si == 0) {
                const float tx = x0 + clefW + keyW;
                auto digits = [&](int v, int pos) {
                    std::string s = std::to_string(v);
                    float xx = tx;
                    for (char ch : s) { glyph(TIME0 + unsigned(ch - '0'), xx, posY(pos)); xx += 1.6f * sp; }
                };
                digits(song_.beatsPerBar, 6);
                digits(song_.beatUnit, 2);
            }
            // Taktstriche
            for (int c = 0; c <= sys.count; ++c) {
                const bool last = sys.first + c == measures && c == sys.count;
                const float bx = mx[size_t(c)];
                const float yb = (sd.braceTop && s + 1 < staves.size()) ? sy + staffY[s + 1] + 4 * sp : bottomLine;
                if (c == 0) dl->AddLine(ImVec2(x0, top), ImVec2(x0, yb), kInk, 1.2f);
                else if (last) {
                    dl->AddLine(ImVec2(bx - 4, top), ImVec2(bx - 4, yb), kInk, 1.f);
                    dl->AddLine(ImVec2(bx, top), ImVec2(bx, yb), kInk, 3.f);
                } else dl->AddLine(ImVec2(bx, top), ImVec2(bx, yb), kInk, 1.f);
            }

            // ---------- Noten des Systems ----------
            for (int c = 0; c < sys.count; ++c) {
                const int m = sys.first + c;
                const auto& ps = pieces[size_t(m)][s];
                const float mx0 = mx[size_t(c)] + mPad * sys.scale;
                auto xAt = [&](int step) { return mx0 + onsetX[size_t(m)][size_t(step)] * sys.scale; };
                std::map<int, int> accState;   // dia -> aktuelle Alteration (Versetzungszeichen gelten bis Taktende)

                // Balkengruppen: Achtel/Sechzehntel im selben Schlag
                std::vector<int> group(ps.size(), -1);
                {
                    int gid = 0;
                    for (size_t i = 0; i < ps.size();) {
                        if (ps[i].rest || ps[i].len > 3) { ++i; continue; }
                        size_t j = i + 1;
                        const int beatIdx = ps[i].start / 4;
                        while (j < ps.size() && !ps[j].rest && ps[j].len <= 3 && ps[j].start / 4 == beatIdx) ++j;
                        if (j - i >= 2) { for (size_t k = i; k < j; ++k) group[k] = gid; ++gid; }
                        i = j;
                    }
                }
                // Positionen aller Töne vorberechnen
                struct NoteGfx { std::vector<int> pos; std::vector<int> alter; std::vector<bool> xhead; float x; bool up; float stemEnd; };
                std::vector<NoteGfx> gfx(ps.size());
                for (size_t i = 0; i < ps.size(); ++i) {
                    const Piece& p = ps[i];
                    NoteGfx& g = gfx[i];
                    g.x = xAt(p.start);
                    if (p.rest) continue;
                    for (int pitch : p.pitches) {
                        int pos;
                        bool xh = false;
                        int alter = 0;
                        if (sd.drums) drumPos(pitch, pos, xh);
                        else {
                            Spelled spl = spell(pitch, fifths);
                            pos = diaPos(spl.dia, sd.clef);
                            alter = spl.alter;
                            int cur = accState.count(spl.dia) ? accState[spl.dia] : keyAlter(spl.letter, fifths);
                            if (alter != cur) { accState[spl.dia] = alter; alter = alter == 0 ? 2 : alter; }   // 2 = Auflösungszeichen
                            else alter = 99;   // kein Zeichen
                        }
                        g.pos.push_back(pos);
                        g.alter.push_back(alter);
                        g.xhead.push_back(xh);
                    }
                    float avg = 0;
                    for (int q : g.pos) avg += float(q);
                    avg /= float(g.pos.size());
                    g.up = sd.drums ? true : avg < 4.f;
                }
                // Gemeinsame Halsrichtung je Balkengruppe
                std::map<int, std::pair<float, int>> gAvg;
                for (size_t i = 0; i < ps.size(); ++i)
                    if (group[i] >= 0) for (int q : gfx[i].pos) { gAvg[group[i]].first += float(q); gAvg[group[i]].second++; }
                for (size_t i = 0; i < ps.size(); ++i)
                    if (group[i] >= 0 && !sd.drums) gfx[i].up = gAvg[group[i]].first / float(gAvg[group[i]].second) < 4.f;

                // Hälse berechnen
                for (size_t i = 0; i < ps.size(); ++i) {
                    if (ps[i].rest) continue;
                    auto& g = gfx[i];
                    const int hi = *std::max_element(g.pos.begin(), g.pos.end()), lo = *std::min_element(g.pos.begin(), g.pos.end());
                    g.stemEnd = g.up ? posY(hi) - 3.5f * sp : posY(lo) + 3.5f * sp;
                    if (g.up && g.stemEnd > posY(4)) g.stemEnd = posY(4);   // Hals mindestens bis zur Mittellinie
                    if (!g.up && g.stemEnd < posY(4)) g.stemEnd = posY(4);
                }
                // Balken: gemeinsame Höhe je Gruppe (waagerecht)
                std::map<int, float> beamY;
                for (size_t i = 0; i < ps.size(); ++i) {
                    if (group[i] < 0) continue;
                    auto it = beamY.find(group[i]);
                    const float v = gfx[i].stemEnd;
                    if (it == beamY.end()) beamY[group[i]] = v;
                    else it->second = gfx[i].up ? std::min(it->second, v) : std::max(it->second, v);
                }
                for (size_t i = 0; i < ps.size(); ++i) if (group[i] >= 0) gfx[i].stemEnd = beamY[group[i]];

                for (size_t i = 0; i < ps.size(); ++i) {
                    const Piece& p = ps[i];
                    const NoteGfx& g = gfx[i];
                    if (p.rest) {
                        if (p.fullRest) { glyph(REST_WHOLE, (mx[size_t(c)] + mx[size_t(c + 1)]) * 0.5f - sp * 0.6f, posY(6)); continue; }
                        const unsigned r = p.len >= 16 ? REST_WHOLE : p.len >= 8 ? REST_HALF : p.len >= 4 ? REST_QUARTER : p.len >= 2 ? REST_8 : REST_16;
                        glyph(r, g.x, posY(r == REST_WHOLE ? 6 : 4));
                        continue;
                    }
                    const bool whole = p.len >= 16;
                    const bool hollow = p.len >= 8;
                    const bool dotted = p.len == 12 || p.len == 6 || p.len == 3;
                    // Notenköpfe (Sekunden versetzt)
                    std::vector<float> hx(g.pos.size(), g.x);
                    for (size_t k = 1; k < g.pos.size(); ++k)
                        if (g.pos[k] - g.pos[k - 1] == 1 && hx[k - 1] == g.x) hx[k] = g.up ? g.x + headW : g.x - headW;
                    for (size_t k = 0; k < g.pos.size(); ++k) {
                        const int pos = g.pos[k];
                        const float yy = posY(pos);
                        // Hilfslinien
                        for (int l = -2; l >= pos; l -= 2) dl->AddLine(ImVec2(hx[k] - 0.4f * sp, posY(l)), ImVec2(hx[k] + headW + 0.4f * sp, posY(l)), kInk, 1.f);
                        for (int l = 10; l <= pos; l += 2) dl->AddLine(ImVec2(hx[k] - 0.4f * sp, posY(l)), ImVec2(hx[k] + headW + 0.4f * sp, posY(l)), kInk, 1.f);
                        const unsigned head = g.xhead[k] ? HEAD_X : whole ? HEAD_WHOLE : hollow ? HEAD_HALF : HEAD_BLACK;
                        glyph(head, hx[k], yy);
                        // Versetzungszeichen
                        const int a = g.alter[k];
                        if (a != 99 && !sd.drums) {
                            const unsigned acc = a == 1 ? SHARP : a == -1 ? FLAT : NATURAL;
                            glyph(acc, g.x - glyphW(acc) - 0.25f * sp, yy);
                        }
                        if (dotted) glyph(DOT, g.x + headW + 0.35f * sp, (pos % 2 == 0) ? yy - sp * 0.5f : yy);
                    }
                    // Hals + Fähnchen
                    if (!whole) {
                        const float hiY = posY(*std::max_element(g.pos.begin(), g.pos.end()));
                        const float loY = posY(*std::min_element(g.pos.begin(), g.pos.end()));
                        const float sx = g.up ? g.x + headW - 0.6f : g.x + 0.6f;
                        dl->AddLine(ImVec2(sx, g.up ? loY - 0.2f * sp : hiY + 0.2f * sp), ImVec2(sx, g.stemEnd), kInk, 1.2f * zoom);
                        if (group[i] < 0 && p.len <= 3) {
                            const unsigned flag = p.len == 1 ? (g.up ? FLAG16_UP : FLAG16_DOWN) : (g.up ? FLAG8_UP : FLAG8_DOWN);
                            glyph(flag, sx - 0.6f, g.stemEnd);
                        }
                    }
                    // Bindebogen
                    if (p.tieNext) {
                        float x2;
                        if (i + 1 < ps.size()) x2 = gfx[i + 1].x;
                        else x2 = mx[size_t(c + 1)] + 0.8f * sp;
                        const float ty = posY(g.pos.front()) + (g.up ? 0.9f * sp : -0.9f * sp);
                        const float bend = g.up ? 0.9f * sp : -0.9f * sp;
                        dl->AddBezierCubic(ImVec2(g.x + headW, ty), ImVec2(g.x + headW + 0.3f * (x2 - g.x), ty + bend),
                                           ImVec2(x2 - 0.3f * (x2 - g.x), ty + bend), ImVec2(x2, ty), kInk, 1.3f * zoom);
                    }
                }
                // Balken zeichnen
                for (size_t i = 0; i < ps.size();) {
                    if (group[i] < 0) { ++i; continue; }
                    size_t j = i;
                    while (j + 1 < ps.size() && group[j + 1] == group[i]) ++j;
                    const bool up = gfx[i].up;
                    auto stemX = [&](size_t k) { return up ? gfx[k].x + headW - 0.6f : gfx[k].x + 0.6f; };
                    const float by = gfx[i].stemEnd;
                    const float th = 0.5f * sp;
                    const float dir = up ? 1.f : -1.f;   // zweiter Balken liegt zum Notenkopf hin
                    dl->AddRectFilled(ImVec2(stemX(i), up ? by : by - th), ImVec2(stemX(j) + 1.f, up ? by + th : by), kInk);
                    for (size_t k = i; k <= j; ++k) {
                        if (ps[k].len != 1) continue;   // Sechzehntel: zweiter Balken
                        const float y2 = by + dir * 0.75f * sp;
                        float xa = stemX(k), xb;
                        if (k + 1 <= j && ps[k + 1].len == 1) xb = stemX(k + 1);
                        else if (k > i && ps[k - 1].len == 1) continue;
                        else xb = k + 1 <= j ? xa + sp : xa - sp;
                        dl->AddRectFilled(ImVec2(std::min(xa, xb), up ? y2 : y2 - th), ImVec2(std::max(xa, xb) + 1.f, up ? y2 + th : y2), kInk);
                    }
                    i = j + 1;
                }
            }

            // ---------- Dynamik: pp … ff und Gabeln unter der Zeile ----------
            if (!sd.drums && !(s > 0 && staves[s - 1].braceTop)) {
                const Tick barT = song_.ticksPerBar();
                const Tick sysA = Tick(sys.first) * barT, sysB = Tick(sys.first + sys.count) * barT;
                auto xOf = [&](Tick t) {
                    const int m = std::clamp(int(t / barT), sys.first, sys.first + sys.count - 1);
                    const int stp = std::clamp(int((t - Tick(m) * barT) / six), 0, steps);
                    return mx[size_t(m - sys.first)] + mPad * sys.scale + onsetX[size_t(m)][size_t(stp)] * sys.scale;
                };
                const float dy = bottomLine + 2.8f * sp;
                for (const auto& d : song_.dynamics) {
                    if (d.kind == DynamicMark::Level) {
                        if (d.start < sysA || d.start >= sysB) continue;
                        static const unsigned glyphs[6] = {0xE52B, 0xE520, 0xE52C, 0xE52D, 0xE522, 0xE52F};   // pp p mp mf f ff
                        glyph(glyphs[std::clamp(d.level, 0, 5)], xOf(d.start) - 0.3f * sp, dy + 0.6f * sp, kInk, 0.85f);
                    } else {
                        const Tick a = std::max(d.start, sysA), b = std::min(d.end, sysB - 1);
                        if (a >= b) continue;
                        const float xa = xOf(a) + 2.2f * sp, xb = std::max(xOf(a) + 2.4f * sp, (b >= sysB - 1) ? mx.back() - 4.f : xOf(b));
                        const float h = 0.55f * sp;
                        if (d.kind == DynamicMark::Crescendo) {   // <
                            dl->AddLine(ImVec2(xa, dy), ImVec2(xb, dy - h), kInk, 1.1f);
                            dl->AddLine(ImVec2(xa, dy), ImVec2(xb, dy + h), kInk, 1.1f);
                        } else {                                   // >
                            dl->AddLine(ImVec2(xa, dy - h), ImVec2(xb, dy), kInk, 1.1f);
                            dl->AddLine(ImVec2(xa, dy + h), ImVec2(xb, dy), kInk, 1.1f);
                        }
                    }
                }
            }
        }
        // Systemklammer links über alle Zeilen
        dl->AddLine(ImVec2(x0, sysTop), ImVec2(x0, sysBot), kInk, 1.6f);
    }

    if (!font_) {
        dl->AddText(ImVec2(origin.x + 20, origin.y + 20), IM_COL32(200, 40, 40, 255),
                    "Notenschrift Bravura.otf nicht gefunden - bitte neben MusicEngine.exe legen.");
    }
    ImGui::Dummy(ImVec2(width, headerH + systemH * float(systems.size()) + 60.f));
    ImGui::EndChild();
    ImGui::PopStyleColor();
}
