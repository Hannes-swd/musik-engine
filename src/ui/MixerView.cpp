#include "Views.h"
#include "audio/AudioEngine.h"
#include "instruments/InstrumentRegistry.h"
#include "generator/Generator.h"
#include <algorithm>
#include <cmath>

namespace {
// Pegel 0..1+ -> Anzeige 0..1 (dB-Skala von -48 dB bis +3 dB)
float meterPos(float level) {
    if (level <= 1e-5f) return 0.f;
    const float db = 20.f * std::log10(level);
    return std::clamp((db + 48.f) / 51.f, 0.f, 1.f);
}

void drawMeter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float level, float hold) {
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255), 2.f);
    const float h = p1.y - p0.y;
    const float v = meterPos(level);
    // Farbverlauf grün -> gelb -> rot
    const float yTop = p1.y - v * h;
    const float yYellow = p1.y - meterPos(0.5f) * h, yRed = p1.y - meterPos(0.89f) * h;
    auto seg = [&](float a, float b, ImU32 c) {
        const float t = std::max(a, yTop);
        if (t < b) dl->AddRectFilled({p0.x + 1, t}, {p1.x - 1, b}, c);
    };
    seg(yYellow, p1.y - 1, IM_COL32(70, 200, 90, 255));
    seg(yRed, yYellow, IM_COL32(220, 200, 60, 255));
    seg(p0.y + 1, yRed, IM_COL32(230, 70, 60, 255));
    const float yh = p1.y - meterPos(hold) * h;
    if (hold > 1e-4f) dl->AddLine({p0.x + 1, yh}, {p1.x - 1, yh}, hold >= 0.99f ? IM_COL32(255, 80, 70, 255) : IM_COL32(230, 230, 230, 200));
    // Skala: 0 dB, -12, -24
    for (float db : {0.f, -12.f, -24.f}) {
        const float y = p1.y - std::clamp((db + 48.f) / 51.f, 0.f, 1.f) * h;
        dl->AddLine({p1.x, y}, {p1.x + 3, y}, IM_COL32(120, 120, 130, 255));
    }
}
}

bool MixerView::draw(ViewContext& ctx) {
    Song& s = ctx.song;
    bool changed = false;
    float master = 0.f;
    const std::vector<float> levels = ctx.engine.trackLevels(&master);
    const double now = ImGui::GetTime();
    const size_t n = s.tracks.size();
    peakHold_.resize(n + 1, 0.f);
    peakTime_.resize(n + 1, 0.0);
    auto hold = [&](size_t i, float lv) {   // Spitzenwert 1,5 s halten
        if (lv >= peakHold_[i] || now - peakTime_[i] > 1.5) { peakHold_[i] = lv; peakTime_[i] = now; }
        return peakHold_[i];
    };

    ImGui::BeginChild("##mixer", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const float stripW = 96.f;
    const float faderH = std::clamp(ImGui::GetContentRegionAvail().y - 200.f, 80.f, 320.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool anySolo = false;
    for (auto& t : s.tracks) anySolo = anySolo || t.solo;

    for (size_t i = 0; i <= n; ++i) {
        const bool isMaster = i == n;
        if (i) ImGui::SameLine(0, 6);
        ImGui::PushID(int(i));
        ImGui::BeginGroup();
        const ImVec2 top = ImGui::GetCursorScreenPos();
        dl->AddRectFilled({top.x - 3, top.y - 2}, {top.x + stripW + 3, top.y + faderH + 190},
                          isMaster ? IM_COL32(44, 40, 34, 255) : (int(i) == ctx.currentTrack ? IM_COL32(44, 46, 56, 255) : IM_COL32(34, 35, 40, 255)), 4.f);
        if (isMaster) {
            ImGui::TextColored({1.f, 0.85f, 0.5f, 1}, "Master");
            ImGui::TextDisabled("Abhören");
            ImGui::Dummy({stripW, ImGui::GetFrameHeight()});
        } else {
            Track& t = s.tracks[i];
            dl->AddRectFilled({top.x, top.y}, {top.x + stripW, top.y + 3}, trackColor(int(i)));
            ImGui::Dummy({stripW, 3});
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + stripW);
            if (ImGui::Selectable(t.name.c_str(), int(i) == ctx.currentTrack, 0, {stripW, 0})) ctx.currentTrack = int(i);
            ImGui::PopTextWrapPos();
            ImGui::SetNextItemWidth(stripW);
            if (ImGui::BeginCombo("##inst", t.instrument.c_str(), ImGuiComboFlags_HeightLarge)) {
                for (auto& name : InstrumentRegistry::instance().names())
                    if (ImGui::Selectable(name.c_str(), name == t.instrument)) {
                        if (ctx.beforeEdit) ctx.beforeEdit();
                        t.instrument = name;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
        }
        // Fader + Pegel
        const ImVec2 fp = ImGui::GetCursorScreenPos();
        if (isMaster) {
            float v = ctx.engine.masterVolume.load();
            if (ImGui::VSliderFloat("##fader", {34, faderH}, &v, 0.f, 1.5f, "")) ctx.engine.masterVolume = v;
            ImGui::SetItemTooltip("Abspiellautstärke %.0f %%", v * 100.f);
            drawMeter(dl, {fp.x + 44, fp.y}, {fp.x + 58, fp.y + faderH}, master, hold(i, master));
        } else {
            Track& t = s.tracks[i];
            float v = t.volume;
            if (ImGui::VSliderFloat("##fader", {34, faderH}, &v, 0.f, 1.5f, "")) {
                t.volume = v;
                ctx.engine.setTrackVolume(int(i), v);
                changed = true;
            }
            if (ImGui::IsItemActivated() && ctx.beforeEdit) ctx.beforeEdit();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {   // Doppelklick = 100 %
                t.volume = 1.f;
                ctx.engine.setTrackVolume(int(i), 1.f);
                changed = true;
            }
            ImGui::SetItemTooltip("Lautstärke %.0f %%  (Doppelklick = 100 %%)", t.volume * 100.f);
            const float lv = i < levels.size() ? levels[i] : 0.f;
            drawMeter(dl, {fp.x + 44, fp.y}, {fp.x + 58, fp.y + faderH}, lv, hold(i, lv));
        }
        ImGui::SetCursorScreenPos({fp.x, fp.y + faderH + 4});
        if (!isMaster) {
            Track& t = s.tracks[i];
            const float db = t.volume > 1e-4f ? 20.f * std::log10(t.volume) : -99.f;
            ImGui::TextDisabled(db > -90.f ? "%+.1f dB" : "-inf dB", db);
            // Panorama
            ImGui::SetNextItemWidth(stripW);
            float pan = t.pan;
            const std::string panLabel = std::abs(pan) < 0.01f ? std::string("Mitte")
                                       : std::string(pan < 0 ? "L " : "R ") + std::to_string(int(std::lround(std::abs(pan) * 100.f)));
            if (ImGui::SliderFloat("##pan", &pan, -1.f, 1.f, panLabel.c_str(), ImGuiSliderFlags_None)) {
                t.pan = std::abs(pan) < 0.03f ? 0.f : pan;
                ctx.engine.setTrackPan(int(i), t.pan);
                changed = true;
            }
            if (ImGui::IsItemActivated() && ctx.beforeEdit) ctx.beforeEdit();
            ImGui::SetItemTooltip("Panorama (links/rechts). Rechtsklick = Mitte");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { t.pan = 0.f; ctx.engine.setTrackPan(int(i), 0.f); changed = true; }
            // Effekt-Anteile: Hall und Echo
            {
                float rv = t.reverb * 100.f, dv = t.delay * 100.f;
                ImGui::SetNextItemWidth(stripW);
                if (ImGui::SliderFloat("##hall", &rv, 0.f, 100.f, "Hall %.0f%%")) {
                    t.reverb = rv / 100.f;
                    ctx.engine.setTrackSends(int(i), t.reverb, t.delay);
                    changed = true;
                }
                if (ImGui::IsItemActivated() && ctx.beforeEdit) ctx.beforeEdit();
                ImGui::SetItemTooltip("Wie viel dieser Spur in den Hall geht (Raumklang)");
                ImGui::SetNextItemWidth(stripW);
                if (ImGui::SliderFloat("##echo", &dv, 0.f, 100.f, "Echo %.0f%%")) {
                    t.delay = dv / 100.f;
                    ctx.engine.setTrackSends(int(i), t.reverb, t.delay);
                    changed = true;
                }
                if (ImGui::IsItemActivated() && ctx.beforeEdit) ctx.beforeEdit();
                ImGui::SetItemTooltip("Wie viel dieser Spur ins Echo geht (Wiederholungen im Takt)");
            }
            // Stumm / Solo
            ImGui::PushStyleColor(ImGuiCol_Button, t.muted ? ImVec4(0.75f, 0.3f, 0.25f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
            if (ImGui::Button("M", {stripW / 2 - 2, 0})) {
                if (ctx.beforeEdit) ctx.beforeEdit();
                t.muted = !t.muted;
                ctx.engine.setTrackMuted(int(i), t.muted);
                changed = true;
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Stumm");
            ImGui::SameLine(0, 4);
            ImGui::PushStyleColor(ImGuiCol_Button, t.solo ? ImVec4(0.85f, 0.7f, 0.2f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
            if (ImGui::Button("S", {stripW / 2 - 2, 0})) {
                if (ctx.beforeEdit) ctx.beforeEdit();
                t.solo = !t.solo;
                ctx.engine.setTrackSolo(int(i), t.solo);
                changed = true;
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Solo");
            if (anySolo && !t.solo && !t.muted) ImGui::TextDisabled("(still: Solo)");
        } else {
            const float v = ctx.engine.masterVolume.load();
            ImGui::TextDisabled("%.0f %%", v * 100.f);
            if (ImGui::Button("Solos aus", {stripW, 0})) {
                if (ctx.beforeEdit) ctx.beforeEdit();
                for (size_t k = 0; k < n; ++k)
                    if (s.tracks[k].solo) { s.tracks[k].solo = false; ctx.engine.setTrackSolo(int(k), false); changed = true; }
            }
            if (ImGui::Button("Alle an", {stripW, 0})) {
                if (ctx.beforeEdit) ctx.beforeEdit();
                for (size_t k = 0; k < n; ++k)
                    if (s.tracks[k].muted) { s.tracks[k].muted = false; ctx.engine.setTrackMuted(int(k), false); changed = true; }
            }
            ImGui::SetItemTooltip("Alle Stummschaltungen aufheben");
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }

    // ---- Effekte: Hall & Echo (für den ganzen Song) ----
    ImGui::SameLine(0, 14);
    ImGui::BeginGroup();
    {
        Effects& fx = s.fx;
        const float fw = 250.f;
        const ImVec2 top = ImGui::GetCursorScreenPos();
        dl->AddRectFilled({top.x - 4, top.y - 2}, {top.x + fw + 8, top.y + faderH + 290}, IM_COL32(36, 40, 50, 255), 4.f);
        bool fxChanged = false;
        auto act = [&] { if (ImGui::IsItemActivated() && ctx.beforeEdit) ctx.beforeEdit(); };
        ImGui::TextColored({0.7f, 0.85f, 1.f, 1}, "Raumklang");
        ImGui::SameLine(fw - 105);
        if (ImGui::Checkbox("Hall & Echo", &fx.on)) { if (ctx.beforeEdit) ctx.beforeEdit(); fxChanged = true; }
        ImGui::BeginDisabled(!fx.on);
        ImGui::SetNextItemWidth(fw - 40);
        if (ImGui::BeginCombo("Raum", "Vorlage wählen ...")) {
            for (int p = 1; p < roomPresetCount(); ++p)
                if (ImGui::Selectable(roomPresetName(p))) {
                    if (ctx.beforeEdit) ctx.beforeEdit();
                    applyRoomPreset(fx, p);
                    fxChanged = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SeparatorText("Hall");
        ImGui::SetNextItemWidth(fw - 90);
        fxChanged |= ImGui::SliderFloat("Raumgröße", &fx.roomSize, 0.f, 1.f, "%.2f"); act();
        ImGui::SetItemTooltip("Wie lange der Hall nachklingt: klein = Zimmer, groß = Kathedrale");
        ImGui::SetNextItemWidth(fw - 90);
        fxChanged |= ImGui::SliderFloat("Dämpfung", &fx.damping, 0.f, 1.f, "%.2f"); act();
        ImGui::SetItemTooltip("Mehr Dämpfung = dunklerer, weicher Raum (Vorhänge, Holz); weniger = heller (Stein, Glas)");
        ImGui::SetNextItemWidth(fw - 90);
        fxChanged |= ImGui::SliderFloat("Lautstärke##hall", &fx.reverbLevel, 0.f, 2.f, "%.2f"); act();
        float preMs = fx.preDelay * 1000.f;
        ImGui::SetNextItemWidth(fw - 90);
        if (ImGui::SliderFloat("Vorverzög.", &preMs, 0.f, 150.f, "%.0f ms")) { fx.preDelay = preMs / 1000.f; fxChanged = true; }
        act();
        ImGui::SetItemTooltip("Kurze Pause, bevor der Hall einsetzt - hält den Klang klar, auch bei viel Hall");
        ImGui::SeparatorText("Echo");
        ImGui::SetNextItemWidth(fw - 90);
        if (ImGui::Combo("Zeit", &fx.delayNote, "1/4\0" "1/8\0" "1/8 punktiert\0" "1/8 Triole\0" "1/16\0")) {
            if (ctx.beforeEdit) ctx.beforeEdit();
            fxChanged = true;
        }
        ImGui::SetItemTooltip("Abstand der Wiederholungen - passt sich automatisch dem Tempo an");
        ImGui::SetNextItemWidth(fw - 90);
        fxChanged |= ImGui::SliderFloat("Wiederh.", &fx.delayFeedback, 0.f, 0.85f, "%.2f"); act();
        ImGui::SetItemTooltip("Rückkopplung: wie viele Wiederholungen zu hören sind");
        ImGui::SetNextItemWidth(fw - 90);
        fxChanged |= ImGui::SliderFloat("Lautstärke##echo", &fx.delayLevel, 0.f, 2.f, "%.2f"); act();
        ImGui::EndDisabled();
        ImGui::TextDisabled("Wie viel jede Spur abbekommt:\nRegler \"Hall\" / \"Echo\" in den Spuren.");
        ImGui::SeparatorText("Mastering");
        if (ImGui::Checkbox("an##master", &fx.mastering)) { if (ctx.beforeEdit) ctx.beforeEdit(); fxChanged = true; }
        ImGui::SetItemTooltip("Wie im Tonstudio: Tiefen-Putzfilter, etwas Wärme und Glanz, sanfter Bus-Kompressor\n"
                              "(\"Kleber\"), Stereo-Breite und ein Limiter gegen Übersteuern.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!fx.mastering);
        ImGui::SetNextItemWidth(fw - 130);
        float amt = fx.masterAmount * 100.f;
        if (ImGui::SliderFloat("Stärke##master", &amt, 0.f, 100.f, "%.0f %%")) { fx.masterAmount = amt / 100.f; fxChanged = true; }
        act();
        ImGui::SetItemTooltip("Wenig = natürlich und dynamisch, viel = dichter, breiter, glänzender");
        {
            const float gr = fx.mastering ? ctx.engine.masterReductionDb() : 0.f;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float bw = fw - 10.f, frac = std::clamp(gr / 8.f, 0.f, 1.f);
            dl->AddRectFilled(p, {p.x + bw, p.y + 8}, IM_COL32(25, 25, 30, 255), 2.f);
            dl->AddRectFilled({p.x + bw * (1.f - frac), p.y}, {p.x + bw, p.y + 8}, IM_COL32(230, 160, 60, 255), 2.f);
            ImGui::Dummy({bw, 10});
            ImGui::TextDisabled("Kompressor: -%.1f dB", gr);
        }
        ImGui::EndDisabled();
        if (fxChanged) {
            ctx.engine.setEffects(fx);
            changed = true;
        }
    }
    ImGui::EndGroup();
    ImGui::EndChild();
    return changed;
}
