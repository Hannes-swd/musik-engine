#include "Views.h"
#include "audio/AudioEngine.h"
#include "instruments/InstrumentRegistry.h"
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
// Reihenfolge der Zeilen (oben Becken, unten Bassdrum – wie in Drumcomputern)
const int kRowOrder[] = {49, 57, 51, 53, 46, 42, 44, 50, 48, 47, 45, 43, 41, 39, 38, 40, 37, 36, 35, 54, 56, 70};

struct Pattern { const char* name; std::vector<std::pair<int, const char*>> rows; };
const std::vector<Pattern>& patterns4() {
    static const std::vector<Pattern> p = {
        {"Rock", {{36, "x.......x.x....."}, {38, "....x.......x..."}, {42, "x.x.x.x.x.x.x.x."}}},
        {"Pop", {{36, "x.....x.x......."}, {38, "....x.......x..."}, {42, "x.x.x.x.x.x.x.x."}, {46, "..............x."}}},
        {"Four-on-the-floor", {{36, "x...x...x...x..."}, {39, "....x.......x..."}, {42, "..x...x...x...x."}}},
        {"Halftime", {{36, "x.........x....."}, {38, "........x......."}, {42, "x.x.x.x.x.x.x.x."}}},
        {"Breakbeat", {{36, "x.........xx...."}, {38, "....x..x.x..x..."}, {42, "x.x.x.x.x.x.x.x."}}},
        {"Ballade", {{36, "x.......x......."}, {37, "....x.......x..."}, {42, "x...x...x...x..."}}},
        {"Shuffle (Jazz)", {{51, "x..x.xx..x.xx..x"}, {44, "....x.......x..."}, {36, "x.......x......."}}},
    };
    return p;
}
const std::vector<Pattern>& patterns3() {
    static const std::vector<Pattern> p = {
        {"Walzer", {{36, "x..........."}, {38, "....x...x..."}, {42, "x...x...x..."}}},
        {"6/8-Gefühl", {{36, "x.....x....."}, {38, "......x....."}, {42, "x.xx.xx.xx.x"}}},
    };
    return p;
}
}

bool DrumGridView::draw(ViewContext& ctx) {
    Song& s = ctx.song;
    bool changed = false;
    // Schlagzeug-Spuren
    std::vector<int> drums;
    for (int i = 0; i < int(s.tracks.size()); ++i) if (isDrumTrack(s.tracks[size_t(i)])) drums.push_back(i);
    if (ctx.currentTrack >= 0 && ctx.currentTrack < int(s.tracks.size()) && isDrumTrack(s.tracks[size_t(ctx.currentTrack)]))
        drumTrack_ = ctx.currentTrack;
    if (std::find(drums.begin(), drums.end(), drumTrack_) == drums.end()) drumTrack_ = drums.empty() ? -1 : drums.front();
    if (drumTrack_ < 0) {
        ImGui::TextWrapped("Dieser Song hat noch keine Schlagzeug-Spur.");
        if (ImGui::Button("Schlagzeug-Spur anlegen")) {
            if (ctx.beforeEdit) ctx.beforeEdit();
            Track t{"Schlagzeug", "Schlagzeug"};
            t.volume = 0.75f;
            s.tracks.push_back(t);
            drumTrack_ = int(s.tracks.size()) - 1;
            ctx.currentTrack = drumTrack_;
            changed = true;
        }
        return changed;
    }
    Track& tr = s.tracks[size_t(drumTrack_)];
    static std::map<std::string, std::unique_ptr<IInstrument>> labelInst;
    auto& inst = labelInst[tr.instrument];
    if (!inst) inst = InstrumentRegistry::instance().create(tr.instrument);

    const Tick tpb = s.ticksPerBar(), six = s.ppq / 4;
    const int steps = int(tpb / six);
    const int totalBars = std::max(1, int((s.length() + tpb - 1) / tpb));
    const double pos = ctx.engine.position();
    const bool playing = ctx.engine.isPlaying();
    if (follow && playing && pos >= 0) bar = int(pos / double(tpb));
    bar = std::clamp(bar, 0, std::max(0, totalBars - 1 + 8));

    // ---- Werkzeuge ----
    if (drums.size() > 1) {
        ImGui::SetNextItemWidth(150);
        if (ImGui::BeginCombo("##drumspur", tr.name.c_str())) {
            for (int d : drums)
                if (ImGui::Selectable(s.tracks[size_t(d)].name.c_str(), d == drumTrack_)) { drumTrack_ = d; ctx.currentTrack = d; }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    } else {
        ImGui::TextUnformatted(tr.name.c_str());
        ImGui::SameLine();
    }
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) { bar = std::max(0, bar - 1); follow = false; }
    ImGui::SameLine();
    int shown = bar + 1;
    ImGui::SetNextItemWidth(70);
    if (ImGui::InputInt("##takt", &shown, 0)) { bar = std::max(0, shown - 1); follow = false; }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) { bar = bar + 1; follow = false; }
    ImGui::SameLine();
    ImGui::Text("von %d", totalBars);
    const Section* sec = nullptr;
    for (auto& x : s.sections) if (Tick(bar) * tpb >= x.start && Tick(bar) * tpb < x.start + x.length) sec = &x;
    if (sec) { ImGui::SameLine(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(sectionColor(sec->name)), "%s", sec->name.c_str()); }
    ImGui::SameLine(0, 16);
    ImGui::Checkbox("Folgen##drum", &follow);

    const Tick b0 = Tick(bar) * tpb, b1 = b0 + tpb;
    auto inBar = [&](const Note& n) { return n.start >= b0 - six / 2 && n.start < b1 - six / 2; };
    auto clearBar = [&](Tick from) {
        tr.notes.erase(std::remove_if(tr.notes.begin(), tr.notes.end(),
                                      [&](const Note& n) { return n.start >= from - six / 2 && n.start < from + tpb - six / 2; }),
                       tr.notes.end());
    };
    if (ImGui::Button("Leeren")) { if (ctx.beforeEdit) ctx.beforeEdit(); clearBar(b0); changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Kopieren")) {
        clipboard_.clear();
        for (auto& n : tr.notes) if (inBar(n)) { Note c = n; c.start -= b0; clipboard_.push_back(c); }
        hasClipboard_ = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasClipboard_);
    if (ImGui::Button("Einfügen")) {
        if (ctx.beforeEdit) ctx.beforeEdit();
        clearBar(b0);
        for (Note c : clipboard_) { c.start += b0; tr.notes.push_back(c); }
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!sec);
    if (ImGui::Button("Auf ganzen Abschnitt")) {
        if (ctx.beforeEdit) ctx.beforeEdit();
        std::vector<Note> pat;
        for (auto& n : tr.notes) if (inBar(n)) { Note c = n; c.start -= b0; pat.push_back(c); }
        for (Tick x = sec->start; x < sec->start + sec->length - six; x += tpb) {
            if (x == b0) continue;
            clearBar(x);
            for (Note c : pat) { c.start += x; tr.notes.push_back(c); }
        }
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Dieses Muster in jeden Takt des Abschnitts kopieren");
    ImGui::SameLine();
    const auto& pats = steps == 16 ? patterns4() : patterns3();
    ImGui::SetNextItemWidth(150);
    if ((steps == 16 || steps == 12) && ImGui::BeginCombo("##vorlage", "Vorlage ...")) {
        for (auto& p : pats)
            if (ImGui::Selectable(p.name)) {
                if (ctx.beforeEdit) ctx.beforeEdit();
                clearBar(b0);
                for (auto& [pitch, row] : p.rows)
                    for (int k = 0; row[k] && k < steps; ++k)
                        if (row[k] == 'x') {
                            const bool strong = k % 4 == 0;
                            tr.notes.push_back({pitch, b0 + Tick(k) * six, six, strong ? 0.9f : 0.72f});
                        }
                changed = true;
            }
        ImGui::EndCombo();
    }

    // ---- Zeilen ----
    std::vector<int> rows;
    for (int p : kRowOrder) if (inst && inst->noteLabel(p)) rows.push_back(p);
    for (auto& n : tr.notes)
        if (inBar(n) && std::find(rows.begin(), rows.end(), n.pitch) == rows.end()) rows.push_back(n.pitch);

    ImGui::TextDisabled("Klick = setzen/entfernen (ziehen malt), Umschalt = Akzent, Strg = leise, Rechtsklick = löschen");
    ImGui::BeginChild("##drumgrid", {0, 0}, ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float labelW = 120.f;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float cw = std::clamp((ImGui::GetContentRegionAvail().x - labelW - 8.f) / float(steps), 16.f, 48.f);
    const float ch = 24.f;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered = ImGui::IsWindowHovered();
    const int playStep = (playing && pos >= double(b0) && pos < double(b1)) ? int((pos - double(b0)) / double(six)) : -1;
    const int stepsPerBeat = int(s.ticksPerBeat() / six);

    // Kopfzeile: Zählzeiten
    for (int k = 0; k < steps; ++k) {
        const float x = o.x + labelW + float(k) * cw;
        if (k % stepsPerBeat == 0)
            dl->AddText({x + 3, o.y}, IM_COL32(170, 170, 180, 255), std::to_string(k / stepsPerBeat + 1).c_str());
        else
            dl->AddText({x + 3, o.y}, IM_COL32(90, 90, 100, 255), (k % stepsPerBeat == stepsPerBeat / 2) ? "+" : ".");
    }
    const float gy = o.y + 18.f;
    int hoverRow = -1, hoverStep = -1;
    for (int r = 0; r < int(rows.size()); ++r) {
        const int p = rows[size_t(r)];
        const float y = gy + float(r) * ch;
        const char* label = inst ? inst->noteLabel(p) : nullptr;
        dl->AddText({o.x + 4, y + 4}, IM_COL32(200, 200, 210, 255), label ? label : ("Ton " + std::to_string(p)).c_str());
        for (int k = 0; k < steps; ++k) {
            const float x = o.x + labelW + float(k) * cw;
            const Tick st = b0 + Tick(k) * six;
            float vel = 0.f;
            for (auto& n : tr.notes)
                if (n.pitch == p && n.start >= st - six / 2 && n.start < st + six / 2) vel = std::max(vel, n.velocity);
            const bool beatCol = (k / stepsPerBeat) % 2 == 0;
            ImU32 bg = beatCol ? IM_COL32(40, 42, 50, 255) : IM_COL32(34, 35, 42, 255);
            if (k == playStep) bg = IM_COL32(70, 64, 36, 255);
            dl->AddRectFilled({x + 1, y + 1}, {x + cw - 1, y + ch - 1}, bg, 3.f);
            if (vel > 0.f) {
                const float b = 0.45f + 0.55f * std::clamp(vel, 0.f, 1.f);
                dl->AddRectFilled({x + 3, y + 3}, {x + cw - 3, y + ch - 3}, trackColor(drumTrack_, 1.f, b * 1.1f), 3.f);
            }
            if (hovered && mouse.x >= x && mouse.x < x + cw && mouse.y >= y && mouse.y < y + ch) {
                hoverRow = r;
                hoverStep = k;
                dl->AddRect({x + 1, y + 1}, {x + cw - 1, y + ch - 1}, IM_COL32(255, 255, 255, 120), 3.f);
            }
        }
        dl->AddLine({o.x, y + ch}, {o.x + labelW + float(steps) * cw, y + ch}, IM_COL32(25, 25, 30, 255));
    }
    for (int k = 0; k <= steps; k += stepsPerBeat) {
        const float x = o.x + labelW + float(k) * cw;
        dl->AddLine({x, gy}, {x, gy + float(rows.size()) * ch}, IM_COL32(90, 90, 105, 255), 1.5f);
    }
    // Bedienung
    auto cellOn = [&](int p, Tick st) {
        for (auto& n : tr.notes) if (n.pitch == p && n.start >= st - six / 2 && n.start < st + six / 2) return true;
        return false;
    };
    auto setCell = [&](int p, Tick st, bool on, float vel) {
        const bool cur = cellOn(p, st);
        if (on == cur) return;
        if (on) {
            tr.notes.push_back({p, st, six, vel});
            ctx.engine.noteOn(drumTrack_, p, vel);
            ctx.engine.noteOff(drumTrack_, p);
        } else {
            tr.notes.erase(std::remove_if(tr.notes.begin(), tr.notes.end(),
                                          [&](const Note& n) { return n.pitch == p && n.start >= st - six / 2 && n.start < st + six / 2; }),
                           tr.notes.end());
        }
        changed = true;
    };
    const ImGuiIO& io = ImGui::GetIO();
    const float vel = io.KeyShift ? 1.f : io.KeyCtrl ? 0.45f : 0.8f;
    if (hoverRow >= 0) {
        const int p = rows[size_t(hoverRow)];
        const Tick st = b0 + Tick(hoverStep) * six;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (ctx.beforeEdit) ctx.beforeEdit();
            paintMode_ = cellOn(p, st) ? -1 : 1;
            setCell(p, st, paintMode_ > 0, vel);
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (ctx.beforeEdit) ctx.beforeEdit();
            paintMode_ = -1;
            setCell(p, st, false, vel);
        } else if (paintMode_ != 0 && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right))) {
            setCell(p, st, paintMode_ > 0, vel);
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) paintMode_ = 0;
    if (changed)
        std::sort(tr.notes.begin(), tr.notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
    ImGui::Dummy({labelW + float(steps) * cw, 18.f + float(rows.size()) * ch + 4.f});
    ImGui::EndChild();
    return changed;
}
