#include "Views.h"
#include "audio/AudioEngine.h"
#include "theory/Theory.h"
#include <algorithm>
#include <cmath>

namespace {
// Stufe (römische Ziffer) eines Akkords relativ zur Tonart
std::string romanOf(const ChordLabel& c, int key) {
    if (c.root < 0 || key < 0) return "";
    static const char* up[] = {"I", "bII", "II", "bIII", "III", "IV", "#IV", "V", "bVI", "VI", "bVII", "VII"};
    std::string r = up[((c.root - key) % 12 + 12) % 12];
    const std::string q = chordQualityName(c.quality);
    if (chordQualityMinor(c.quality))
        for (char& ch : r) if (ch != 'b' && ch != '#') ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (q == "dim") r += "°";
    else if (q == "m7b5") r += "ø";
    else if (q == "7" || q == "m7") r += "7";
    else if (q == "maj7") r += "maj7";
    else if (q == "sus4" || q == "sus2" || q == "aug") r += q;
    return r;
}

std::string barText(const ChordLabel* a, const ChordLabel* b) {
    const std::string sa = (a && a->root >= 0) ? a->symbol : "";
    const std::string sb = (b && b->root >= 0) ? b->symbol : "";
    if (sa.empty() && sb.empty()) return "-";
    if (sb.empty() || sb == sa) return sa.empty() ? sb : sa;
    if (sa.empty()) return "- " + sb;
    return sa + "  " + sb;
}
}

void ChordSheetView::draw(ViewContext& ctx) {
    const Song& s = ctx.song;
    const Tick tpb = s.ticksPerBar();
    const bool canHalf = s.beatsPerBar % 2 == 0;
    const bool half = halfBars && canHalf;
    const Tick step = half ? tpb / 2 : tpb;
    const size_t sig = songSignature(s);
    if (sig != signature_ || step != step_) {
        chords_ = detectChords(s, step);
        signature_ = sig;
        step_ = step;
    }
    const int per = half ? 2 : 1;
    const int bars = int((chords_.size() + size_t(per) - 1) / size_t(per));
    auto chordAt = [&](int bar, int k) -> const ChordLabel* {
        const size_t i = size_t(bar * per + k);
        return i < chords_.size() ? &chords_[i] : nullptr;
    };

    // Werkzeuge
    if (canHalf) { ImGui::Checkbox("2 Akkorde pro Takt", &halfBars); ImGui::SameLine(); }
    ImGui::Checkbox("Stufen", &showRoman);
    ImGui::SameLine();
    ImGui::Checkbox("Folgen##ak", &follow);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat("Größe##ak", &textScale, 0.7f, 2.f, "%.1f");
    ImGui::SameLine();
    if (ImGui::Button("Als Text kopieren")) {
        std::string txt = s.title + "\n";
        auto addRange = [&](const std::string& name, int b0, int b1) {
            txt += "\n" + name + ":\n|";
            for (int b = b0; b < b1; ++b) {
                txt += " " + barText(chordAt(b, 0), half ? chordAt(b, 1) : nullptr) + " |";
                if ((b - b0) % 4 == 3 && b + 1 < b1) txt += "\n|";
            }
            txt += "\n";
        };
        if (s.sections.empty()) addRange("Song", 0, bars);
        for (auto& sec : s.sections)
            addRange(sec.name, int(sec.start / tpb), std::min(bars, int((sec.start + sec.length + tpb - 1) / tpb)));
        ImGui::SetClipboardText(txt.c_str());
    }
    ImGui::SetItemTooltip("Akkordblatt als Text in die Zwischenablage (zum Einfügen in Chat, Word ...)");
    if (s.key >= 0) {
        ImGui::SameLine(0, 20);
        ImGui::TextDisabled("Tonart: %s %s", noteName(s.key), s.scale >= 0 ? scaleName(ScaleType(s.scale)) : "");
    }

    ImGui::BeginChild("##akkordblatt", {0, 0}, ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double pos = std::max(0.0, ctx.engine.position());
    const int curBar = int(pos / double(std::max<Tick>(1, tpb)));
    const bool playing = ctx.engine.isPlaying();
    const float cellW = 132.f * textScale, cellH = (showRoman ? 70.f : 54.f) * textScale;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = std::max(1, int((avail + 6.f) / (cellW + 6.f)));
    ImFont* font = ImGui::GetFont();
    const float big = ImGui::GetFontSize() * 1.55f * textScale;

    struct Range { std::string name; int b0, b1; };
    std::vector<Range> ranges;
    if (s.sections.empty()) ranges.push_back({"Song", 0, bars});
    for (auto& sec : s.sections)
        ranges.push_back({sec.name, int(sec.start / tpb), std::min(bars, int((sec.start + sec.length + tpb - 1) / tpb))});

    std::string prevText;
    for (auto& r : ranges) {
        if (r.b1 <= r.b0) continue;
        // Abschnittskopf
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        dl->AddRectFilled({hp.x, hp.y + 2}, {hp.x + 6, hp.y + ImGui::GetTextLineHeight() + 2}, sectionColor(r.name));
        ImGui::SetCursorScreenPos({hp.x + 12, hp.y});
        ImGui::TextUnformatted(r.name.c_str());
        for (int b = r.b0; b < r.b1; ++b) {
            const int col = (b - r.b0) % cols;
            if (col) ImGui::SameLine(0, 6);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(b);
            if (ImGui::InvisibleButton("##takt", {cellW, cellH})) ctx.seek(double(b) * double(tpb));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool cur = b == curBar && (playing || pos > 0);
            dl->AddRectFilled(p, {p.x + cellW, p.y + cellH},
                              cur ? IM_COL32(70, 62, 30, 255) : hov ? IM_COL32(48, 50, 58, 255) : IM_COL32(36, 37, 43, 255), 5.f);
            dl->AddRect(p, {p.x + cellW, p.y + cellH}, cur ? IM_COL32(255, 210, 80, 255) : sectionColor(r.name, 0.45f), 5.f, 0, cur ? 2.f : 1.f);
            dl->AddText({p.x + 6, p.y + 3}, IM_COL32(130, 130, 140, 255), std::to_string(b + 1).c_str());
            const ChordLabel* a = chordAt(b, 0);
            const ChordLabel* c2 = half ? chordAt(b, 1) : nullptr;
            const std::string text = barText(a, c2);
            const bool repeat = text == prevText;
            prevText = text;
            dl->PushClipRect(p, {p.x + cellW - 2, p.y + cellH}, true);
            dl->AddText(font, big, {p.x + 10, p.y + 16 * textScale}, repeat ? IM_COL32(170, 170, 180, 255) : IM_COL32(245, 245, 250, 255),
                        text.c_str());
            if (showRoman && s.key >= 0) {
                std::string ro = romanOf(a ? *a : ChordLabel{}, s.key);
                if (c2 && c2->root >= 0 && (!a || c2->symbol != a->symbol)) ro += (ro.empty() ? "" : "  ") + romanOf(*c2, s.key);
                dl->AddText({p.x + 10, p.y + cellH - ImGui::GetTextLineHeight() - 5}, IM_COL32(150, 170, 210, 255), ro.c_str());
            }
            dl->PopClipRect();
            if (cur && playing && follow && curBar != lastBar_) {
                ImGui::SetScrollHereY(0.35f);
                lastBar_ = curBar;
            }
        }
        ImGui::Spacing();
    }
    if (chords_.empty()) ImGui::TextDisabled("Noch keine Noten - erst einen Song generieren oder Noten zeichnen.");
    ImGui::EndChild();
}
