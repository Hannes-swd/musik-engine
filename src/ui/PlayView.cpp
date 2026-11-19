#include "Views.h"
#include "LivePlayer.h"
#include "audio/AudioEngine.h"
#include "instruments/InstrumentRegistry.h"
#include "theory/Theory.h"
#include <algorithm>
#include <cmath>

namespace {
bool isBlackKey(int p) {
    const int pc = ((p % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
std::string noteLabel(int p) { return std::string(noteName(p)) + std::to_string(p / 12 - 1); }

// Akkord aus gleichzeitig klingenden Tönen benennen (gleiche Schablonen wie das Akkordblatt)
std::string chordOf(const std::set<int>& notes) {
    if (notes.size() < 3) return "";
    Song tmp;
    Track t;
    for (int p : notes) t.notes.push_back({p, 0, tmp.ppq, 0.8f});
    tmp.tracks.push_back(t);
    const auto c = detectChords(tmp, tmp.ppq);
    return (!c.empty() && c[0].root >= 0) ? c[0].symbol : "";
}
}

bool PlayView::draw(ViewContext& ctx, LivePlayer& live, bool& keyboardPlay, bool recording, int recTrack) {
    Song& s = ctx.song;
    bool changed = false;
    const std::string liveInst = ctx.engine.liveInstrumentName();
    const int cur = std::clamp(ctx.currentTrack, 0, std::max(0, int(s.tracks.size()) - 1));
    const std::string trackInst = s.tracks.empty() ? "Piano" : s.tracks[size_t(cur)].instrument;

    // ---- Kopfzeile: an/aus, Oktave, Anschlag, Pedal ----
    ImGui::PushStyleColor(ImGuiCol_Button, keyboardPlay ? ImVec4(0.20f, 0.55f, 0.30f, 1) : ImVec4(0.30f, 0.32f, 0.38f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, keyboardPlay ? ImVec4(0.25f, 0.65f, 0.36f, 1) : ImVec4(0.38f, 0.40f, 0.48f, 1));
    if (ImGui::Button(keyboardPlay ? "  Tastatur spielt Töne: AN  " : "  Tastatur spielt Töne: AUS  ", {0, 30})) {
        keyboardPlay = !keyboardPlay;
        if (!keyboardPlay) live.allOff();
    }
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("An: die Buchstaben auf den Tasten unten spielen Töne.\n"
                          "Aus: die Tastatur hat wieder ihre normalen Kürzel (G = Song generieren ...).");
    ImGui::SameLine(0, 20);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Oktave");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##okt-", ImGuiDir_Left)) { live.allOff(); live.octave = std::max(1, live.octave - 1); }
    ImGui::SameLine();
    ImGui::Text("%d", live.octave);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##okt+", ImGuiDir_Right)) { live.allOff(); live.octave = std::min(6, live.octave + 1); }
    ImGui::SetItemTooltip("Auch mit den Pfeiltasten links/rechts");
    ImGui::SameLine(0, 20);
    ImGui::SetNextItemWidth(130);
    ImGui::SliderFloat("Anschlag##live", &live.velocity, 0.1f, 1.f, "%.2f");
    ImGui::SameLine(0, 20);
    const bool pedal = live.pedal();
    ImGui::PushStyleColor(ImGuiCol_Text, pedal ? ImVec4(1.f, 0.8f, 0.3f, 1) : ImVec4(0.5f, 0.5f, 0.55f, 1));
    ImGui::TextUnformatted(pedal ? "Haltepedal AN" : "Haltepedal (Umschalt)");
    ImGui::PopStyleColor();

    // ---- Klang ----
    ImGui::SeparatorText("Instrument");
    {
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float avail = ImGui::GetContentRegionAvail().x;
        const int cols = std::max(3, int((avail + sp) / (112.f + sp)));
        const float bw = (avail - sp * float(cols - 1)) / float(cols);
        auto instButton = [&](const std::string& label, bool active, const char* tip) {
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.2f, 1));
            const bool clicked = ImGui::Button(label.c_str(), {bw, 0});
            if (active) ImGui::PopStyleColor();
            if (tip) ImGui::SetItemTooltip("%s", tip);
            return clicked;
        };
        int idx = 0;
        if (instButton("Wie Spur", liveInst.empty(), "Spielt mit dem Instrument der ausgewählten Spur")) {
            live.allOff();
            ctx.engine.setLiveInstrument("");
        }
        ++idx;
        for (auto& name : InstrumentRegistry::instance().names()) {
            if (idx % cols) ImGui::SameLine();
            if (instButton(name, liveInst == name, nullptr)) {
                live.allOff();
                ctx.engine.setLiveInstrument(name);
            }
            ++idx;
        }
        ImGui::TextDisabled("Klingt gerade: %s", liveInst.empty() ? (trackInst + " (Spur \"" + (s.tracks.empty() ? std::string("-") : s.tracks[size_t(cur)].name) + "\")").c_str()
                                                                   : (liveInst + " (frei, ändert den Song nicht)").c_str());
        if (!liveInst.empty() && !s.tracks.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(("Für Spur \"" + s.tracks[size_t(cur)].name + "\" übernehmen").c_str())) {
                if (ctx.beforeEdit) ctx.beforeEdit();
                s.tracks[size_t(cur)].instrument = liveInst;
                changed = true;
            }
            ImGui::SetItemTooltip("Die ausgewählte Spur spielt danach mit diesem Instrument");
        }
    }

    // ---- Klaviatur ----
    const std::set<int> sounding = live.sounding();
    const int base = live.basePitch();
    const int lo = base - 12, hi = base + 36;   // vier Oktaven, die gespielten 2½ mittendrin
    std::map<int, std::vector<const char*>> labels;
    for (auto& k : LivePlayer::keyMap()) labels[base + k.semitone].push_back(k.label);

    ImGui::Spacing();
    // gespielte Töne + Akkord
    {
        std::string txt;
        for (int p : sounding) txt += noteLabel(p) + "  ";
        const std::string chord = chordOf(sounding);
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (!chord.empty()) dl->AddText(font, fs * 2.f, p, IM_COL32(255, 220, 120, 255), chord.c_str());
        const float cw = chord.empty() ? 0.f : font->CalcTextSizeA(fs * 2.f, 10000.f, 0.f, chord.c_str()).x + 18.f;
        dl->AddText(font, fs * 1.2f, {p.x + cw, p.y + fs * 0.5f},
                    IM_COL32(200, 200, 210, 255), txt.empty() ? (keyboardPlay ? "Drück eine Taste ..." : "") : txt.c_str());
        ImGui::Dummy({10, fs * 2.2f});
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float helpH = ImGui::GetTextLineHeightWithSpacing() * 4.5f;
    const float kh = std::clamp(avail.y - helpH, 90.f, 300.f);
    int whites = 0;
    for (int p = lo; p <= hi; ++p) if (!isBlackKey(p)) ++whites;
    const float ww = avail.x / float(whites);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##klavier", {avail.x, kh});
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto keyX = [&](int p, float& x0, float& x1) {
        int wi = 0;
        for (int q = lo; q < p; ++q) if (!isBlackKey(q)) ++wi;
        if (!isBlackKey(p)) { x0 = o.x + float(wi) * ww; x1 = x0 + ww; }
        else { const float c = o.x + float(wi) * ww; x0 = c - ww * 0.33f; x1 = c + ww * 0.33f; }
    };
    const ImU32 playCol = trackColor(cur, 1.f, 1.3f);
    int mouseKey = -1;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float fs = ImGui::GetFontSize();
    for (int pass = 0; pass < 2; ++pass)
        for (int p = lo; p <= hi; ++p) {
            if (isBlackKey(p) != (pass == 1)) continue;
            float x0, x1;
            keyX(p, x0, x1);
            const float y1 = o.y + (pass ? kh * 0.62f : kh);
            if (hovered && m.x >= x0 && m.x < x1 && m.y >= o.y && m.y < y1) mouseKey = p;
            const bool on = sounding.count(p) > 0;
            const bool mapped = labels.count(p) > 0;
            ImU32 c;
            if (on) c = playCol;
            else if (pass == 0) c = mapped ? IM_COL32(238, 242, 250, 255) : IM_COL32(190, 192, 198, 255);
            else c = mapped ? IM_COL32(30, 34, 48, 255) : IM_COL32(55, 55, 60, 255);
            dl->AddRectFilled({x0 + (pass ? 0.f : 1.f), o.y}, {x1 - (pass ? 0.f : 1.f), y1}, c, 3.f);
            if (!pass) dl->AddRect({x0, o.y}, {x1, y1}, IM_COL32(70, 70, 78, 255), 3.f);
            // Buchstaben
            auto it = labels.find(p);
            if (it != labels.end()) {
                float ly = y1 - fs * 1.35f * float(it->second.size()) - 4.f;
                for (const char* lb : it->second) {
                    const ImVec2 ts = ImGui::CalcTextSize(lb);
                    const float cx = (x0 + x1) * 0.5f;
                    const ImU32 bgc = pass ? IM_COL32(70, 110, 200, 255) : IM_COL32(60, 100, 190, 255);
                    dl->AddRectFilled({cx - std::max(ts.x, fs) * 0.5f - 3, ly}, {cx + std::max(ts.x, fs) * 0.5f + 3, ly + fs + 2}, bgc, 4.f);
                    dl->AddText({cx - ts.x * 0.5f, ly + 1}, IM_COL32(255, 255, 255, 255), lb);
                    ly += fs * 1.35f;
                }
            }
            // Notenname auf jedem C
            if (!pass && ((p % 12) + 12) % 12 == 0)
                dl->AddText({x0 + 3, o.y + 4}, IM_COL32(110, 110, 125, 255), noteLabel(p).c_str());
        }
    // Maus spielt Töne (Ziehen = Glissando)
    if (active && mouseKey >= 0 && mouseKey != mouseNote_) {
        if (mouseNote_ >= 0) live.noteOff(mouseNote_);
        live.noteOn(mouseKey, live.velocity);
        mouseNote_ = mouseKey;
    }
    if (mouseNote_ >= 0 && !active) { live.noteOff(mouseNote_); mouseNote_ = -1; }

    // ---- Hilfe ----
    ImGui::Spacing();
    ImGui::TextDisabled("Untere Reihe  Y X C V B N M , . -  = weiße Tasten (Oktave %d), darüber  S D  G H J  L Ö  = schwarze Tasten",
                        live.octave);
    ImGui::TextDisabled("Obere Reihe  Q W E R T Z U I O P Ü +  = weiße Tasten (Oktave %d), darüber  2 3  5 6 7  9 0 ´  = schwarze Tasten",
                        live.octave + 1);
    ImGui::TextDisabled("Pfeil links/rechts = Oktave, Umschalt halten = Haltepedal, Leertaste = Abspielen, Strg+R = Aufnahme. "
                        "Ein MIDI-Keyboard (oben auswählen) leuchtet hier auch mit.");
    if (recording && recTrack >= 0 && recTrack < int(s.tracks.size()))
        ImGui::TextColored({1.f, 0.4f, 0.35f, 1}, "Aufnahme läuft: gespielte Töne werden in Spur \"%s\" gezeichnet (beim Abspielen).",
                           s.tracks[size_t(recTrack)].name.c_str());
    return changed;
}
