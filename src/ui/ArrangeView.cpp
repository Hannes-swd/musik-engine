#include "Views.h"
#include "audio/AudioEngine.h"
#include "instruments/InstrumentRegistry.h"
#include <algorithm>
#include <cmath>

// ===================== Gemeinsame Helfer =====================
ImU32 trackColor(int track, float alpha, float brightness) {
    const float h = std::fmod(0.58f + float(track) * 0.137f, 1.f);
    ImVec4 c = ImColor::HSV(h, 0.55f, std::clamp(0.88f * brightness, 0.f, 1.f), alpha);
    return ImGui::ColorConvertFloat4ToU32(c);
}

ImU32 sectionColor(const std::string& name, float alpha) {
    auto starts = [&](const char* p) { return name.rfind(p, 0) == 0; };
    ImVec4 c{0.45f, 0.48f, 0.55f, alpha};                                  // Intro/Outro
    if (starts("Strophe")) c = {0.25f, 0.45f, 0.75f, alpha};
    else if (starts("Pre")) c = {0.55f, 0.38f, 0.75f, alpha};
    else if (starts("Refrain")) c = {0.85f, 0.52f, 0.20f, alpha};
    else if (starts("Bridge")) c = {0.30f, 0.65f, 0.40f, alpha};
    else if (starts("Solo")) c = {0.80f, 0.30f, 0.30f, alpha};
    else if (starts("Outro")) c = {0.40f, 0.40f, 0.45f, alpha};
    return ImGui::ColorConvertFloat4ToU32(c);
}

bool isDrumTrack(const Track& t) {
    static std::map<std::string, bool> cache;
    auto it = cache.find(t.instrument);
    if (it != cache.end()) return it->second;
    auto inst = InstrumentRegistry::instance().create(t.instrument);
    const bool d = inst && inst->isDrumKit();
    cache[t.instrument] = d;
    return d;
}

size_t songSignature(const Song& song) {
    size_t h = song.tracks.size() * 1000003u + size_t(song.ppq) + size_t(song.beatsPerBar) * 7919u;
    for (auto& t : song.tracks) {
        h = h * 31u + t.notes.size() + (t.muted ? 17u : 0u) + std::hash<std::string>{}(t.instrument);
        for (auto& n : t.notes) h = h * 1315423911u + size_t(n.start) * 131u + size_t(n.pitch) * 7u + size_t(n.length);
    }
    return h;
}

// ===================== Abschnitt-Werkzeuge =====================
namespace {
void shiftExpression(Track& t, Tick from, Tick by) {
    for (auto& p : t.expression) if (p.first >= from) p.first += by;
}
void sortAll(Song& s) {
    for (auto& t : s.tracks) {
        std::sort(t.notes.begin(), t.notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
        std::stable_sort(t.expression.begin(), t.expression.end(),
                         [](const std::pair<Tick, float>& a, const std::pair<Tick, float>& b) { return a.first < b.first; });
    }
    std::sort(s.dynamics.begin(), s.dynamics.end(), [](const DynamicMark& a, const DynamicMark& b) { return a.start < b.start; });
}
}

void duplicateSection(Song& song, int i) {
    if (i < 0 || i >= int(song.sections.size())) return;
    const Section sec = song.sections[size_t(i)];
    const Tick a = sec.start, L = sec.length, b = a + L;
    const Tick tol = song.ppq / 8;
    for (auto& t : song.tracks) {
        std::vector<Note> copies;
        for (auto& n : t.notes)
            if (n.start >= a - tol && n.start < b - tol) { Note c = n; c.start += L; copies.push_back(c); }
        for (auto& n : t.notes) if (n.start >= b - tol) n.start += L;
        t.notes.insert(t.notes.end(), copies.begin(), copies.end());
        if (!t.expression.empty()) {
            const float atEnd = t.expressionAt(double(b - 1)), atStart = t.expressionAt(double(a));
            std::vector<std::pair<Tick, float>> copyEx;
            for (auto& p : t.expression) if (p.first >= a && p.first < b) copyEx.push_back({p.first + L, p.second});
            shiftExpression(t, b, L);
            t.expression.insert(t.expression.end(), copyEx.begin(), copyEx.end());
            t.expression.push_back({b - 1, atEnd});
            t.expression.push_back({b, atStart});
        }
    }
    std::vector<DynamicMark> dcopy;
    for (auto& d : song.dynamics) if (d.start >= a && d.start < b) { DynamicMark c = d; c.start += L; c.end += L; dcopy.push_back(c); }
    for (auto& d : song.dynamics) if (d.start >= b) { d.start += L; d.end += L; }
    song.dynamics.insert(song.dynamics.end(), dcopy.begin(), dcopy.end());
    for (auto& s : song.sections) if (s.start >= b) s.start += L;
    Section copy = sec;
    copy.start = b;
    song.sections.insert(song.sections.begin() + i + 1, copy);
    sortAll(song);
}

void deleteSection(Song& song, int i) {
    if (i < 0 || i >= int(song.sections.size()) || song.sections.size() < 2) return;
    const Section sec = song.sections[size_t(i)];
    const Tick a = sec.start, L = sec.length, b = a + L;
    const Tick tol = song.ppq / 8;
    for (auto& t : song.tracks) {
        auto& ns = t.notes;
        ns.erase(std::remove_if(ns.begin(), ns.end(), [&](const Note& n) { return n.start >= a - tol && n.start < b - tol; }), ns.end());
        for (auto& n : ns) {
            if (n.start < a - tol && n.end() > a) n.length = std::max<Tick>(1, a - n.start);
            else if (n.start >= b - tol) n.start = std::max<Tick>(0, n.start - L);
        }
        auto& ex = t.expression;
        ex.erase(std::remove_if(ex.begin(), ex.end(), [&](const std::pair<Tick, float>& p) { return p.first >= a && p.first < b; }), ex.end());
        for (auto& p : ex) if (p.first >= b) p.first -= L;
    }
    auto& dm = song.dynamics;
    dm.erase(std::remove_if(dm.begin(), dm.end(), [&](const DynamicMark& d) { return d.start >= a && d.start < b; }), dm.end());
    for (auto& d : dm) if (d.start >= b) { d.start -= L; d.end -= L; }
    song.sections.erase(song.sections.begin() + i);
    for (auto& s : song.sections) if (s.start >= b) s.start -= L;
    // Zuletzt gelöscht? Der Schlussabschnitt bekommt den letzten Takt dazu
    sortAll(song);
}

void swapSections(Song& song, int i) {
    if (i < 0 || i + 1 >= int(song.sections.size())) return;
    Section A = song.sections[size_t(i)], B = song.sections[size_t(i) + 1];
    const Tick s = A.start, La = A.length, Lb = B.length;
    const Tick tol = song.ppq / 8;
    auto region = [&](Tick t) { return (t >= s - tol && t < s + La - tol) ? 0 : (t >= s + La - tol && t < s + La + Lb - tol) ? 1 : -1; };
    for (auto& t : song.tracks) {
        for (auto& n : t.notes) {
            const int r = region(n.start);
            if (r == 0) n.start += Lb;
            else if (r == 1) n.start -= La;
        }
        if (!t.expression.empty()) {
            const float before = t.expressionAt(double(s - 1)), after = t.expressionAt(double(s + La + Lb));
            for (auto& p : t.expression) {
                if (p.first >= s && p.first < s + La) p.first += Lb;
                else if (p.first >= s + La && p.first < s + La + Lb) p.first -= La;
            }
            if (s > 0) t.expression.push_back({s - 1, before});
            t.expression.push_back({s + La + Lb, after});
        }
    }
    for (auto& d : song.dynamics) {
        if (d.start >= s && d.start < s + La) { d.start += Lb; d.end += Lb; }
        else if (d.start >= s + La && d.start < s + La + Lb) { d.start -= La; d.end -= La; }
    }
    B.start = s;
    A.start = s + Lb;
    song.sections[size_t(i)] = B;
    song.sections[size_t(i) + 1] = A;
    sortAll(song);
}

// ===================== Arrangement-Ansicht =====================
bool ArrangeView::draw(ViewContext& ctx) {
    Song& s = ctx.song;
    bool changed = false;
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("Zoom##arr", &pxPerBar, 12.f, 160.f, "%.0f px/Takt");
    ImGui::SameLine();
    ImGui::Checkbox("Folgen##arr", &follow);
    ImGui::SameLine();
    ImGui::TextDisabled("Klick = hinspringen, Doppelklick auf Bahn = in Piano-Roll öffnen, Rechtsklick auf Abschnitt = bearbeiten");

    const Tick tpb = s.ticksPerBar();
    const double pos = ctx.engine.position();
    const int bars = int((s.length() + tpb - 1) / std::max<Tick>(1, tpb)) + 2;
    const float headerW = 178.f, rulerH = 20.f, secH = 24.f;
    const float laneH = s.tracks.size() > 10 ? 30.f : 40.f;
    const float contentW = headerW + float(bars) * pxPerBar + 40.f;
    const float contentH = rulerH + secH + float(s.tracks.size()) * laneH + 8.f;

    ImGui::BeginChild("##arrange", {0, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const float winW = ImGui::GetWindowWidth();
    const float pinX = winPos.x;                       // Spurköpfe bleiben links stehen
    auto tx = [&](double t) { return origin.x + headerW + float(t / double(tpb)) * pxPerBar; };
    auto xt = [&](float x) { return double(x - origin.x - headerW) / pxPerBar * double(tpb); };
    const float visL = pinX + headerW, visR = winPos.x + winW;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsWindowHovered();

    // Lineal
    const float yR = origin.y, yS = origin.y + rulerH, yL = yS + secH;
    dl->AddRectFilled({visL, yR}, {visR, yR + rulerH}, IM_COL32(38, 40, 46, 255));
    const int every = pxPerBar >= 28 ? 1 : pxPerBar >= 16 ? 2 : 4;
    for (int b = 0; b < bars; ++b) {
        const float x = tx(double(b) * double(tpb));
        if (x < visL - 40 || x > visR) continue;
        dl->AddLine({x, yR + (b % every == 0 ? 6.f : 13.f)}, {x, yR + rulerH}, IM_COL32(110, 110, 120, 255));
        if (b % every == 0) dl->AddText({x + 3, yR + 2}, IM_COL32(180, 180, 190, 255), std::to_string(b + 1).c_str());
    }
    // Abschnitte
    for (int i = 0; i < int(s.sections.size()); ++i) {
        const auto& sec = s.sections[size_t(i)];
        const float x0 = tx(double(sec.start)), x1 = tx(double(sec.start + sec.length));
        if (x1 < visL || x0 > visR) continue;
        const bool hov = hovered && mouse.y >= yS && mouse.y < yS + secH && mouse.x >= std::max(x0, visL) && mouse.x < x1;
        dl->AddRectFilled({x0 + 1, yS + 2}, {x1 - 1, yS + secH - 2}, sectionColor(sec.name, hov ? 1.f : 0.8f), 4.f);
        dl->PushClipRect({std::max(x0, visL), yS}, {x1 - 3, yS + secH}, true);
        dl->AddText({std::max(x0, visL) + 5, yS + 4}, IM_COL32(255, 255, 255, 235), sec.name.c_str());
        dl->PopClipRect();
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ctx.seek(double(sec.start));
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { menuSection_ = i; ImGui::OpenPopup("##abschnitt"); }
    }
    // Spuren
    for (int i = 0; i < int(s.tracks.size()); ++i) {
        const Track& t = s.tracks[size_t(i)];
        const float y0 = yL + float(i) * laneH, y1 = y0 + laneH;
        dl->AddRectFilled({visL, y0}, {visR, y1}, i % 2 ? IM_COL32(30, 31, 36, 255) : IM_COL32(35, 36, 42, 255));
        if (i == ctx.currentTrack) dl->AddRect({visL, y0}, {visR, y1}, trackColor(i, 0.7f), 0, 0, 1.5f);
        // Abschnittsgrenzen
        for (auto& sec : s.sections) {
            const float x = tx(double(sec.start));
            if (x >= visL && x <= visR) dl->AddLine({x, y0}, {x, y1}, IM_COL32(70, 70, 80, 255));
        }
        int lo = 127, hi = 0;
        for (auto& n : t.notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
        const float span = float(std::max(1, hi - lo + 1));
        const bool audible = !t.muted;
        const ImU32 col = trackColor(i, audible ? 0.95f : 0.3f);
        for (auto& n : t.notes) {
            const float x0 = tx(double(n.start)), x1 = std::max(x0 + 1.5f, tx(double(n.end())));
            if (x1 < visL || x0 > visR) continue;
            const float fy = 1.f - (float(n.pitch - lo) + 0.5f) / span;
            const float y = y0 + 4.f + fy * (laneH - 10.f);
            dl->AddRectFilled({std::max(x0, visL), y}, {x1, y + 2.5f}, col);
        }
        // Klick / Doppelklick in der Bahn
        if (hovered && mouse.y >= y0 && mouse.y < y1 && mouse.x >= visL) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { ctx.currentTrack = i; ImGui::SetWindowFocus("Piano-Roll"); }
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ctx.currentTrack = i;
                const double tk = std::max(0.0, xt(mouse.x));
                ctx.seek(std::floor(tk / double(s.ticksPerBeat())) * double(s.ticksPerBeat()));
            }
        }
    }
    // Lineal klicken/ziehen = Position
    if (hovered && mouse.y >= yR && mouse.y < yR + rulerH && mouse.x >= visL && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        ctx.seek(std::max(0.0, xt(mouse.x)));
    // Abspielposition
    {
        const float x = tx(std::max(0.0, pos));
        if (x >= visL && x <= visR) dl->AddLine({x, yR}, {x, yL + float(s.tracks.size()) * laneH}, IM_COL32(255, 210, 80, 255), 2.f);
    }
    // Spurköpfe (fest links)
    dl->AddRectFilled({pinX, yR}, {pinX + headerW, yL + float(s.tracks.size()) * laneH}, IM_COL32(26, 27, 31, 255));
    dl->AddText({pinX + 8, yR + 3}, IM_COL32(150, 150, 160, 255), "Takt");
    dl->AddText({pinX + 8, yS + 5}, IM_COL32(150, 150, 160, 255), "Abschnitte");
    for (int i = 0; i < int(s.tracks.size()); ++i) {
        Track& t = s.tracks[size_t(i)];
        const float y0 = yL + float(i) * laneH;
        dl->AddRectFilled({pinX + 3, y0 + 4}, {pinX + 8, y0 + laneH - 4}, trackColor(i));
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos({pinX + 14, y0 + 3});
        ImGui::PushStyleColor(ImGuiCol_Text, i == ctx.currentTrack ? ImVec4(1, 1, 1, 1) : ImVec4(0.8f, 0.8f, 0.82f, 1));
        if (ImGui::Selectable(t.name.c_str(), i == ctx.currentTrack, 0, {headerW - 72, 0})) ctx.currentTrack = i;
        ImGui::PopStyleColor();
        if (laneH >= 36.f) {
            ImGui::SetCursorScreenPos({pinX + 14, y0 + 20});
            ImGui::TextDisabled("%s", t.instrument.c_str());
        }
        ImGui::SetCursorScreenPos({pinX + headerW - 54, y0 + 5});
        ImGui::PushStyleColor(ImGuiCol_Button, t.muted ? ImVec4(0.75f, 0.3f, 0.25f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        if (ImGui::SmallButton("M")) {
            if (ctx.beforeEdit) ctx.beforeEdit();
            t.muted = !t.muted;
            ctx.engine.setTrackMuted(i, t.muted);
            changed = true;
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Stumm");
        ImGui::SameLine(0, 3);
        ImGui::PushStyleColor(ImGuiCol_Button, t.solo ? ImVec4(0.85f, 0.7f, 0.2f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        if (ImGui::SmallButton("S")) {
            if (ctx.beforeEdit) ctx.beforeEdit();
            t.solo = !t.solo;
            ctx.engine.setTrackSolo(i, t.solo);
            changed = true;
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Solo (nur Solo-Spuren hören)");
        ImGui::PopID();
    }
    // Abschnitt-Menü
    if (ImGui::BeginPopup("##abschnitt")) {
        const int i = menuSection_;
        if (i >= 0 && i < int(s.sections.size())) {
            ImGui::TextDisabled("%s", s.sections[size_t(i)].name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Hierher springen")) ctx.seek(double(s.sections[size_t(i)].start));
            if (ImGui::MenuItem("Duplizieren (Kopie dahinter)")) {
                if (ctx.beforeEdit) ctx.beforeEdit();
                duplicateSection(s, i);
                changed = true;
            }
            if (ImGui::MenuItem("Nach vorne schieben", nullptr, false, i > 0)) {
                if (ctx.beforeEdit) ctx.beforeEdit();
                swapSections(s, i - 1);
                changed = true;
            }
            if (ImGui::MenuItem("Nach hinten schieben", nullptr, false, i + 1 < int(s.sections.size()))) {
                if (ctx.beforeEdit) ctx.beforeEdit();
                swapSections(s, i);
                changed = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Löschen", nullptr, false, s.sections.size() > 1)) {
                if (ctx.beforeEdit) ctx.beforeEdit();
                deleteSection(s, i);
                changed = true;
            }
        }
        ImGui::EndPopup();
    }
    // Mitlaufen
    if (follow && ctx.engine.isPlaying()) {
        const float px = headerW + float(pos / double(tpb)) * pxPerBar;
        const float sx = ImGui::GetScrollX();
        if (px < sx + headerW || px > sx + winW - 60.f) ImGui::SetScrollX(std::max(0.f, px - headerW - (winW - headerW) * 0.2f));
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy({contentW, contentH});
    ImGui::EndChild();
    return changed;
}
