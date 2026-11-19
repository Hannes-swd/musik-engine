#include "Views.h"
#include "audio/AudioEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr int kLow = 21, kHigh = 108;   // A0 … C8 (88 Tasten)
bool isBlack(int p) {
    const int pc = ((p % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
int whiteIndex(int p) {   // Anzahl weißer Tasten unterhalb von p
    int n = 0;
    for (int q = kLow; q < p; ++q) if (!isBlack(q)) ++n;
    return n;
}
}

void VisualizerView::draw(ViewContext& ctx) {
    const Song& s = ctx.song;
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("Vorschau", &lookahead, 1.f, 10.f, "%.1f s");
    ImGui::SameLine();
    ImGui::Checkbox("Schlagzeug zeigen", &showDrums);
    ImGui::SameLine();
    ImGui::Checkbox("Nur aktuelle Spur", &onlyCurrent);
    ImGui::SameLine();
    ImGui::Checkbox("Akkord anzeigen", &showChord);
    ImGui::SameLine();
    ImGui::TextDisabled("Tasten anklicken = spielen");

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 100 || avail.y < 100) return;
    ImGui::InvisibleButton("##vis", avail);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float W = avail.x, H = avail.y;
    const float kbH = std::clamp(H * 0.17f, 50.f, 110.f);
    const float kbTop = p0.y + H - kbH;
    const float fallH = kbTop - p0.y;
    const int whites = whiteIndex(kHigh + 1);
    const float ww = W / float(whites);
    auto keyRect = [&](int p, float& x0, float& x1) {
        if (!isBlack(p)) {
            x0 = p0.x + float(whiteIndex(p)) * ww;
            x1 = x0 + ww;
        } else {
            const float c = p0.x + float(whiteIndex(p)) * ww;
            x0 = c - ww * 0.32f;
            x1 = c + ww * 0.32f;
        }
    };

    // Hintergrund mit Taktlinien
    dl->AddRectFilled(p0, {p0.x + W, kbTop}, IM_COL32(16, 17, 22, 255));
    for (int p = kLow; p <= kHigh; ++p)
        if (((p % 12) + 12) % 12 == 0) {   // C-Linien
            float x0, x1;
            keyRect(p, x0, x1);
            dl->AddLine({x0, p0.y}, {x0, kbTop}, IM_COL32(32, 34, 42, 255));
        }
    const double pos = std::max(0.0, ctx.engine.position());
    const double now = s.ticksToSeconds(pos);
    const double secPerBar = s.ticksToSeconds(double(s.ticksPerBar()));
    if (secPerBar > 0) {
        for (double t = std::ceil(now / secPerBar) * secPerBar; t < now + lookahead; t += secPerBar) {
            const float y = kbTop - float((t - now) / lookahead) * fallH;
            dl->AddLine({p0.x, y}, {p0.x + W, y}, IM_COL32(40, 42, 52, 255));
        }
    }

    // Fallende Noten
    std::vector<ImU32> active(size_t(kHigh + 1), 0);
    for (int ti = 0; ti < int(s.tracks.size()); ++ti) {
        const Track& t = s.tracks[size_t(ti)];
        if (t.muted) continue;
        if (onlyCurrent && ti != ctx.currentTrack) continue;
        if (!showDrums && isDrumTrack(t)) continue;
        const ImU32 col = trackColor(ti, 0.9f);
        const ImU32 glow = trackColor(ti, 1.f, 1.35f);
        for (auto& n : t.notes) {
            if (n.pitch < kLow || n.pitch > kHigh) continue;
            const double a = s.ticksToSeconds(double(n.start)), b = s.ticksToSeconds(double(n.end()));
            if (b < now || a > now + lookahead) continue;
            float x0, x1;
            keyRect(n.pitch, x0, x1);
            const float yb = kbTop - float((a - now) / lookahead) * fallH;
            const float yt = kbTop - float((b - now) / lookahead) * fallH;
            const bool on = a <= now && b > now;
            const float top = std::max(p0.y, yt), bot = std::min(kbTop, yb);
            if (bot - top < 1.f) continue;
            dl->AddRectFilled({x0 + 1, top}, {x1 - 1, bot}, on ? glow : col, 3.f);
            if (on) {
                active[size_t(n.pitch)] = glow;
                dl->AddRectFilled({x0 - 2, kbTop - 6}, {x1 + 2, kbTop}, trackColor(ti, 0.5f, 1.4f), 2.f);
            }
        }
    }

    // Klaviatur
    int mouseKey = -1;
    const ImVec2 m = ImGui::GetIO().MousePos;
    for (int pass = 0; pass < 2; ++pass)
        for (int p = kLow; p <= kHigh; ++p) {
            if (isBlack(p) != (pass == 1)) continue;
            float x0, x1;
            keyRect(p, x0, x1);
            const float y1 = pass ? kbTop + kbH * 0.62f : kbTop + kbH;
            const bool hit = hovered && m.x >= x0 && m.x < x1 && m.y >= kbTop && m.y < y1;
            if (hit) mouseKey = p;   // schwarze Tasten (zweiter Durchgang) gewinnen
            ImU32 c = pass ? IM_COL32(25, 25, 28, 255) : IM_COL32(235, 235, 238, 255);
            if (active[size_t(p)]) c = active[size_t(p)];
            if (p == mouseNote_) c = IM_COL32(255, 200, 90, 255);
            dl->AddRectFilled({x0 + (pass ? 0.f : 0.5f), kbTop}, {x1 - (pass ? 0.f : 0.5f), y1}, c, 2.f);
            if (!pass) {
                dl->AddRect({x0, kbTop}, {x1, y1}, IM_COL32(60, 60, 66, 255), 2.f);
                if (((p % 12) + 12) % 12 == 0 && ww > 9.f)
                    dl->AddText({x0 + 2, y1 - ImGui::GetTextLineHeight() - 2}, IM_COL32(120, 120, 130, 255),
                                ("C" + std::to_string(p / 12 - 1)).c_str());
            }
        }
    // Maus spielt Töne auf der aktuellen Spur
    const int tr = std::clamp(ctx.currentTrack, 0, std::max(0, int(s.tracks.size()) - 1));
    if (!s.tracks.empty()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && mouseKey >= 0 && mouseKey != mouseNote_ && ImGui::IsItemActive()) {
            if (mouseNote_ >= 0) ctx.engine.noteOff(tr, mouseNote_);
            ctx.engine.noteOn(tr, mouseKey, 0.8f);
            mouseNote_ = mouseKey;
        }
        if (mouseNote_ >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            ctx.engine.noteOff(tr, mouseNote_);
            mouseNote_ = -1;
        }
    }

    // Einblendung: Abschnitt, Akkord, Zeit
    std::string secName;
    for (auto& sec : s.sections) if (pos >= double(sec.start) && pos < double(sec.start + sec.length)) secName = sec.name;
    const double total = s.durationSeconds();
    char tbuf[64];
    std::snprintf(tbuf, sizeof tbuf, "%d:%02d / %d:%02d", int(now) / 60, int(now) % 60, int(total) / 60, int(total) % 60);
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    float y = p0.y + 10;
    if (!secName.empty()) {
        dl->AddText(font, fs * 1.4f, {p0.x + 14, y}, sectionColor(secName), secName.c_str());
        y += fs * 1.6f;
    }
    if (showChord) {
        const size_t sig = songSignature(s);
        if (sig != signature_) {
            chords_ = detectChords(s, s.ticksPerBeat());
            signature_ = sig;
        }
        const size_t ci = size_t(pos / double(std::max<Tick>(1, s.ticksPerBeat())));
        if (ci < chords_.size() && chords_[ci].root >= 0)
            dl->AddText(font, fs * 2.4f, {p0.x + 14, y}, IM_COL32(250, 250, 255, 235), chords_[ci].symbol.c_str());
    }
    const ImVec2 ts = ImGui::CalcTextSize(tbuf);
    dl->AddText({p0.x + W - ts.x - 14, p0.y + 10}, IM_COL32(170, 170, 180, 255), tbuf);
    if (!s.title.empty()) {
        const ImVec2 tt = ImGui::CalcTextSize(s.title.c_str());
        dl->AddText({p0.x + W - tt.x - 14, p0.y + 12 + ts.y}, IM_COL32(110, 110, 120, 255), s.title.c_str());
    }
}
