#include "PianoRoll.h"
#include "audio/AudioEngine.h"
#include "instruments/InstrumentRegistry.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr float kKeyboardW = 64.f;
constexpr float kRulerH = 40.f;
constexpr float kSectionH = 18.f;   // oberer Teil des Lineals: Abschnitte
constexpr float kScrollH = 14.f;    // waagerechte Scrollleiste unten

ImU32 sectionColor(const std::string& name) {
    switch (name.empty() ? '?' : name[0]) {
    case 'I': return IM_COL32(90, 110, 140, 255);    // Intro
    case 'S': return IM_COL32(70, 120, 90, 255);     // Strophe
    case 'P': return IM_COL32(130, 120, 60, 255);    // Pre-Refrain
    case 'R': return IM_COL32(160, 80, 70, 255);     // Refrain
    case 'B': return IM_COL32(120, 80, 150, 255);    // Bridge
    case 'O': return IM_COL32(90, 100, 120, 255);    // Outro
    default: return IM_COL32(90, 90, 90, 255);
    }
}

bool isBlack(int p) {
    static const bool b[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    return b[((p % 12) + 12) % 12];
}

ImU32 trackColor(int track, float velocity, float alpha = 1.f) {
    static const float hues[] = {0.58f, 0.08f, 0.33f, 0.83f, 0.15f, 0.48f, 0.0f, 0.70f};
    float h = hues[track % 8];
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, 0.65f, 0.45f + 0.55f * velocity, r, g, b);
    return ImGui::ColorConvertFloat4ToU32({r, g, b, alpha});
}
}

int PianoRoll::yToPitch(float y) const {
    return kTopPitch - int(std::floor((y - gridMin_.y + scrollY_) / keyHeight));
}

Tick PianoRoll::snap() const {
    return ImGui::GetIO().KeyAlt ? 1 : std::max<Tick>(1, snapTicks);
}

int PianoRoll::hitTest(ImVec2 m, bool& onEdge) const {
    if (currentTrack < 0 || currentTrack >= int(song_.tracks.size())) return -1;
    auto& notes = song_.tracks[currentTrack].notes;
    for (int i = int(notes.size()) - 1; i >= 0; --i) {
        const Note& n = notes[i];
        float x0 = tickToX(double(n.start)), x1 = tickToX(double(n.end()));
        float y0 = pitchToY(n.pitch), y1 = y0 + keyHeight;
        if (m.x >= x0 && m.x < x1 && m.y >= y0 && m.y < y1) {
            onEdge = m.x > x1 - std::min(7.f, (x1 - x0) * 0.35f);
            return i;
        }
    }
    return -1;
}

void PianoRoll::preview(int pitch) {
    if (pitch == previewPitch_) return;
    stopPreview();
    if (pitch < kLowPitch || pitch > kTopPitch) return;
    engine_.noteOn(currentTrack, pitch, newVelocity);
    previewPitch_ = pitch;
}

void PianoRoll::stopPreview() {
    if (previewPitch_ >= 0) engine_.noteOff(currentTrack, previewPitch_);
    previewPitch_ = -1;
}

bool PianoRoll::draw() {
    changed_ = false;
    currentTrack = std::clamp(currentTrack, 0, std::max(0, int(song_.tracks.size()) - 1));

    origin_ = ImGui::GetCursorScreenPos();
    size_ = ImGui::GetContentRegionAvail();
    if (size_.x < 200 || size_.y < 80) return false;
    gridMin_ = {origin_.x + kKeyboardW, origin_.y + kRulerH};
    gridMax_ = {origin_.x + size_.x, origin_.y + size_.y - kScrollH};

    ImGui::InvisibleButton("##pianoroll", size_,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();

    handleInput(hovered);
    updateScrubNotes();

    // Abspielposition verfolgen – außer man hat selbst weggescrollt (dann bis zum nächsten Play pausiert)
    const double playTick = engine_.position();
    const bool playing = engine_.isPlaying();
    if (playing && !wasPlaying_) followSuspended = false;
    wasPlaying_ = playing;
    if (followPlayhead && playing && !followSuspended) {
        float px = tickToX(playTick);
        if (px > gridMax_.x - 40 || px < gridMin_.x)
            scrollX_ = std::max(0.f, float(playTick) * pxPerTick() - 40.f);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin_, gridMax_, true);
    drawGrid(dl);
    drawNotes(dl);

    // Playhead
    dl->PushClipRect(gridMin_, gridMax_, true);
    float px = tickToX(playTick);
    dl->AddLine({px, gridMin_.y}, {px, gridMax_.y}, IM_COL32(255, 220, 80, 230), 2.f);
    dl->PopClipRect();

    drawRuler(dl);
    drawKeyboard(dl);
    dl->PopClipRect();
    drawScrollbar(dl);
    return changed_;
}

// Alle Töne (aller hörbaren Spuren), deren Anfang zwischen a und b liegt, kurz anspielen
void PianoRoll::scrubNotesBetween(double a, double b) {
    const double lo = std::min(a, b), hi = std::max(a, b);
    const double now = ImGui::GetTime();
    int count = 0;
    for (int t = 0; t < int(song_.tracks.size()); ++t) {
        const Track& tr = song_.tracks[size_t(t)];
        if (tr.muted) continue;
        for (const Note& n : tr.notes) {
            if (double(n.start) <= lo || double(n.start) > hi) continue;
            if (++count > 24) return;   // bei sehr schnellem Wischen nicht alles auf einmal
            engine_.noteOn(t, n.pitch, n.velocity * 0.8f);
            scrubNotes_.push_back({t, n.pitch, now + 0.18});
        }
    }
}

void PianoRoll::updateScrubNotes() {
    const double now = ImGui::GetTime();
    for (size_t i = 0; i < scrubNotes_.size();) {
        if (scrubNotes_[i].until <= now) {
            engine_.noteOff(scrubNotes_[i].track, scrubNotes_[i].pitch);
            scrubNotes_.erase(scrubNotes_.begin() + long(i));
        } else ++i;
    }
}

void PianoRoll::drawScrollbar(ImDrawList* dl) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 tMin{gridMin_.x, gridMax_.y + 2}, tMax{gridMax_.x, gridMax_.y + kScrollH - 1};
    const float viewW = gridMax_.x - gridMin_.x;
    // Gesamtbreite: Song + 8 Takte Luft (damit man auch über das Ende hinaus zeichnen kann)
    const float contentW = std::max(viewW, float(song_.length() + song_.ticksPerBar() * 8) * pxPerTick());
    const float trackW = tMax.x - tMin.x;
    const float thumbW = std::max(30.f, trackW * viewW / contentW);
    const float maxScroll = std::max(1.f, contentW - viewW);
    scrollX_ = std::min(scrollX_, maxScroll);
    const float thumbX = tMin.x + (trackW - thumbW) * (scrollX_ / maxScroll);

    const ImVec2 m = io.MousePos;
    const bool overTrack = ImGui::IsWindowHovered() && m.x >= tMin.x && m.x <= tMax.x && m.y >= tMin.y - 2 && m.y <= tMax.y + 2;
    const float before = scrollX_;
    if (overTrack && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        scrollDrag_ = true;
        // Klick neben den Schieber: dorthin springen (Schieber mittig unter die Maus)
        scrollDragOffset_ = (m.x >= thumbX && m.x <= thumbX + thumbW) ? m.x - thumbX : thumbW * 0.5f;
    }
    if (scrollDrag_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) scrollDrag_ = false;
        else {
            const float x = std::clamp(m.x - scrollDragOffset_ - tMin.x, 0.f, trackW - thumbW);
            scrollX_ = (trackW - thumbW) > 0 ? x / (trackW - thumbW) * maxScroll : 0.f;
        }
    }
    if (overTrack && io.MouseWheel != 0.f) scrollX_ = std::clamp(scrollX_ - io.MouseWheel * 120.f, 0.f, maxScroll);
    if (scrollX_ != before && engine_.isPlaying()) followSuspended = true;

    dl->AddRectFilled(tMin, tMax, IM_COL32(30, 31, 36, 255), 4.f);
    const float tx = tMin.x + (trackW - thumbW) * (scrollX_ / maxScroll);
    const bool hot = scrollDrag_ || overTrack;
    dl->AddRectFilled({tx, tMin.y + 1}, {tx + thumbW, tMax.y - 1}, hot ? IM_COL32(120, 130, 155, 255) : IM_COL32(85, 92, 110, 255), 4.f);
    // Abspielposition als Markierung auf der Leiste
    const float pp = tMin.x + float(engine_.position()) * pxPerTick() / contentW * trackW;
    if (pp >= tMin.x && pp <= tMax.x) dl->AddLine({pp, tMin.y}, {pp, tMax.y}, IM_COL32(255, 220, 80, 220), 2.f);
}

void PianoRoll::handleInput(bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 m = io.MousePos;
    const float gridH = gridMax_.y - gridMin_.y;
    const float totalH = (kTopPitch - kLowPitch + 1) * keyHeight;

    // --- Scrollen & Zoomen ---  (seitliches Scrollen/Zoomen pausiert das Mitlaufen)
    const float scrollXBefore = scrollX_, zoomBefore = pxPerBeat;
    if (hovered && io.MouseWheel != 0.f && m.y < gridMax_.y) {   // über der Scrollleiste übernimmt die Leiste
        if (io.KeyCtrl) {
            double mouseTick = xToTick(m.x);
            pxPerBeat = std::clamp(pxPerBeat * std::pow(1.15f, io.MouseWheel), 8.f, 800.f);
            scrollX_ = float(mouseTick) * pxPerTick() - (m.x - gridMin_.x);
        } else if (io.KeyShift) {
            scrollX_ -= io.MouseWheel * 100.f;
        } else {
            scrollY_ -= io.MouseWheel * keyHeight * 3.f;
        }
    }
    if (hovered && io.MouseWheelH != 0.f) scrollX_ -= io.MouseWheelH * 100.f;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.f)) {
        scrollX_ -= io.MouseDelta.x;
        scrollY_ -= io.MouseDelta.y;
    }
    if ((scrollX_ != scrollXBefore || pxPerBeat != zoomBefore) && engine_.isPlaying()) followSuspended = true;
    scrollX_ = std::max(0.f, scrollX_);
    scrollY_ = std::clamp(scrollY_, 0.f, std::max(0.f, totalH - gridH));

    if (song_.tracks.empty()) return;
    auto& notes = song_.tracks[currentTrack].notes;

    const bool inGrid = m.x >= gridMin_.x && m.y >= gridMin_.y && m.x < gridMax_.x && m.y < gridMax_.y;
    const bool inRuler = m.y >= origin_.y && m.y < gridMin_.y && m.x >= gridMin_.x && m.x < gridMax_.x;
    const bool inKeys = m.x >= origin_.x && m.x < gridMin_.x && m.y >= gridMin_.y && m.y < gridMax_.y;
    const Tick s = snap();

    // Mauszeiger
    if (hovered && inGrid && drag_ == Drag::None && editEnabled) {
        bool edge = false;
        if (hitTest(m, edge) >= 0) ImGui::SetMouseCursor(edge ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeAll);
    }
    if (drag_ == Drag::Resize || drag_ == Drag::Create) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // --- Dynamik-Linie zeichnen (unten im Raster) ---
    {
        const float base = gridMax_.y - 6.f;
        const bool inLane = inGrid && m.y >= base - kDynH - 4.f;
        if (dynEdit && hovered && inLane && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            dynDrag_ = true;
            editedThisDrag_ = false;
            dynLastTick_ = std::max(0.0, xToTick(m.x));
        }
        if (dynDrag_) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dynDrag_ = false;
            else {
                const double t = std::max(0.0, xToTick(m.x));
                const float v = std::clamp((base - m.y) / kDynH * 1.2f, 0.f, 1.2f);
                edit();
                auto& e = song_.tracks[currentTrack].expression;
                if (e.empty()) {   // bisher konstant: Start- und Endpunkt anlegen
                    e.push_back({0, 1.f});
                    e.push_back({std::max<Tick>(song_.length(), Tick(t) + 1), 1.f});
                }
                const Tick lo = Tick(std::min(dynLastTick_, t)), hi = Tick(std::max(dynLastTick_, t));
                e.erase(std::remove_if(e.begin(), e.end(), [&](const std::pair<Tick, float>& p) {
                            return (p.first > lo && p.first < hi) || p.first == Tick(t);
                        }), e.end());
                const auto pos = std::lower_bound(e.begin(), e.end(), Tick(t), [](const std::pair<Tick, float>& p, Tick x) { return p.first < x; });
                e.insert(pos, {Tick(t), v});
                dynLastTick_ = t;
            }
        }
    }

    // --- Linke Maustaste: Klick ---
    if (hovered && !dynDrag_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        editedThisDrag_ = false;
        if (inRuler) {   // Lineal: klicken = springen, ziehen = Scrubbing (mit Ton)
            rulerDrag_ = true;
            resumeAfterScrub_ = engine_.isPlaying();
            if (resumeAfterScrub_) engine_.pause();
            const double t = std::max(0.0, xToTick(m.x));
            engine_.seek(t);
            lastScrubTick_ = t;
            followSuspended = false;
        } else if (inKeys) {
            preview(yToPitch(m.y));
        } else if (inGrid && editEnabled) {   // Noten nur mit Häkchen "Noten zeichnen" verändern
            bool edge = false;
            int hit = hitTest(m, edge);
            dragStartTick_ = xToTick(m.x);
            dragStartPitch_ = yToPitch(m.y);
            if (hit >= 0) {
                drag_ = edge ? Drag::Resize : Drag::Move;
                dragNote_ = hit;
                dragOrig_ = notes[hit];
                lastLength_ = dragOrig_.length;
                preview(dragOrig_.pitch);
            } else if (dragStartPitch_ >= kLowPitch && dragStartPitch_ <= kTopPitch) {
                edit();
                Tick start = Tick(std::floor(std::max(0.0, dragStartTick_) / double(s))) * s;
                notes.push_back({dragStartPitch_, start, std::max(lastLength_, s), newVelocity});
                dragNote_ = int(notes.size()) - 1;
                dragOrig_ = notes.back();
                drag_ = Drag::Create;
                preview(dragStartPitch_);
            }
        }
    }

    // --- Scrubbing im Lineal: Position folgt der Maus, überstrichene Töne erklingen kurz ---
    if (rulerDrag_) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const double t = std::max(0.0, xToTick(m.x));
            if (std::abs(t - lastScrubTick_) > 0.5) {
                scrubNotesBetween(lastScrubTick_, t);
                engine_.scrubTo(t);
                lastScrubTick_ = t;
            }
        } else {
            rulerDrag_ = false;
            if (resumeAfterScrub_) engine_.play();
        }
    }

    // --- Ziehen ---
    if (drag_ != Drag::None && drag_ != Drag::Erase && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        dragNote_ >= 0 && dragNote_ < int(notes.size())) {
        const double deltaTicks = xToTick(m.x) - dragStartTick_;
        Note n = notes[dragNote_];
        if (drag_ == Drag::Move) {
            Tick d = Tick(std::round(deltaTicks / double(s))) * s;
            n.start = std::max<Tick>(0, dragOrig_.start + d);
            n.pitch = std::clamp(dragOrig_.pitch + (yToPitch(m.y) - dragStartPitch_), kLowPitch, kTopPitch);
            preview(n.pitch);
        } else {   // Resize oder Create
            double endT = double(dragOrig_.end()) + deltaTicks;
            Tick end = Tick(std::round(endT / double(s))) * s;
            n.length = std::max<Tick>(s, end - n.start);
        }
        const Note& cur = notes[dragNote_];
        if (n.start != cur.start || n.pitch != cur.pitch || n.length != cur.length) {
            edit();   // Snapshot vor der Änderung (nur einmal pro Zug)
            notes[dragNote_] = n;
        }
    }

    // --- Rechte Maustaste: löschen (auch per Ziehen) ---
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && inGrid && editEnabled) {
        editedThisDrag_ = false;
        drag_ = Drag::Erase;
    }
    if (drag_ == Drag::Erase && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        bool edge = false;
        int hit = hitTest(m, edge);
        if (hit >= 0) {
            edit();
            notes.erase(notes.begin() + hit);
        }
    }

    // --- Loslassen ---
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if ((drag_ == Drag::Move || drag_ == Drag::Resize || drag_ == Drag::Create) &&
            dragNote_ >= 0 && dragNote_ < int(notes.size()))
            lastLength_ = notes[dragNote_].length;
        if (drag_ != Drag::Erase) drag_ = Drag::None;
        dragNote_ = -1;
        stopPreview();
    }
    if (drag_ == Drag::Erase && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) drag_ = Drag::None;
}

void PianoRoll::drawGrid(ImDrawList* dl) {
    dl->PushClipRect(gridMin_, gridMax_, true);
    dl->AddRectFilled(gridMin_, gridMax_, IM_COL32(38, 40, 46, 255));

    // Zeilen
    int pTop = std::min(kTopPitch, yToPitch(gridMin_.y));
    int pBot = std::max(kLowPitch, yToPitch(gridMax_.y));
    for (int p = pTop; p >= pBot; --p) {
        float y = pitchToY(p);
        if (isBlack(p)) dl->AddRectFilled({gridMin_.x, y}, {gridMax_.x, y + keyHeight}, IM_COL32(30, 32, 37, 255));
        ImU32 c = (p % 12 == 0) ? IM_COL32(80, 84, 95, 255) : IM_COL32(48, 50, 57, 255);
        dl->AddLine({gridMin_.x, y + keyHeight}, {gridMax_.x, y + keyHeight}, c);
    }

    // Loop-Bereich
    if (loopEnabled && loopEnd > loopStart)
        dl->AddRectFilled({tickToX(double(loopStart)), gridMin_.y}, {tickToX(double(loopEnd)), gridMax_.y},
                          IM_COL32(90, 140, 255, 18));

    // Spalten: Raster, Schläge, Takte
    const Tick beat = song_.ticksPerBeat(), bar = song_.ticksPerBar();
    const Tick step = snapTicks * pxPerTick() >= 5.f ? snapTicks : beat;
    double t0 = std::max(0.0, xToTick(gridMin_.x));
    double t1 = xToTick(gridMax_.x);
    for (Tick t = Tick(t0 / double(step)) * step; double(t) <= t1; t += step) {
        float x = tickToX(double(t));
        ImU32 c = (t % bar == 0)    ? IM_COL32(110, 115, 130, 255)
                  : (t % beat == 0) ? IM_COL32(68, 72, 82, 255)
                                    : IM_COL32(50, 52, 59, 255);
        dl->AddLine({x, gridMin_.y}, {x, gridMax_.y}, c);
    }

    // Song-Ende
    float endX = tickToX(double(song_.length()));
    dl->AddLine({endX, gridMin_.y}, {endX, gridMax_.y}, IM_COL32(255, 90, 90, 90), 1.f);
    dl->PopClipRect();
}

void PianoRoll::drawNotes(ImDrawList* dl) {
    dl->PushClipRect(gridMin_, gridMax_, true);
    // Andere Spuren als Schatten
    for (int t = 0; t < int(song_.tracks.size()); ++t) {
        if (t == currentTrack) continue;
        for (auto& n : song_.tracks[t].notes) {
            float y = pitchToY(n.pitch);
            dl->AddRectFilled({tickToX(double(n.start)), y + 1}, {tickToX(double(n.end())), y + keyHeight - 1},
                              trackColor(t, 0.3f, 0.35f), 2.f);
        }
    }
    if (!song_.tracks.empty()) {
        auto& notes = song_.tracks[currentTrack].notes;
        for (int i = 0; i < int(notes.size()); ++i) {
            const Note& n = notes[i];
            float x0 = tickToX(double(n.start)), x1 = tickToX(double(n.end()));
            float y = pitchToY(n.pitch);
            if (x1 < gridMin_.x || x0 > gridMax_.x || y + keyHeight < gridMin_.y || y > gridMax_.y) continue;
            bool active = i == dragNote_;
            dl->AddRectFilled({x0, y + 1}, {x1, y + keyHeight - 1}, trackColor(currentTrack, n.velocity), 3.f);
            dl->AddRect({x0, y + 1}, {x1, y + keyHeight - 1},
                        active ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 140), 3.f, 0, active ? 2.f : 1.f);
            // Griff am rechten Rand
            if (x1 - x0 > 12) dl->AddLine({x1 - 4, y + 4}, {x1 - 4, y + keyHeight - 4}, IM_COL32(0, 0, 0, 90));
        }
    }
    // Dynamik-Verlauf der aktuellen Spur als Linie am unteren Rand
    if (!song_.tracks.empty() && (!song_.tracks[currentTrack].expression.empty() || dynEdit)) {
        const Track& tr = song_.tracks[currentTrack];
        const float base = gridMax_.y - 6.f, h = kDynH;
        dl->AddRectFilled({gridMin_.x, base - h - 4}, {gridMax_.x, base + 4}, IM_COL32(0, 0, 0, 60));
        ImVec2 prev{-1, -1};
        for (float x = gridMin_.x; x <= gridMax_.x; x += 3.f) {
            const float v = std::clamp(tr.expressionAt(xToTick(x)) / 1.2f, 0.f, 1.f);
            const ImVec2 p{x, base - v * h};
            if (prev.x >= 0) dl->AddLine(prev, p, IM_COL32(255, 160, 60, 220), 2.f);
            prev = p;
        }
        dl->AddText({gridMin_.x + 6, base - h - 2}, IM_COL32(255, 180, 90, 200),
                    dynEdit ? "Dynamik (mit der Maus zeichnen)" : "Dynamik");
    }
    if (overlay)   // Aufnahme läuft: neue Noten rot
        for (auto& n : *overlay) {
            float y = pitchToY(n.pitch);
            dl->AddRectFilled({tickToX(double(n.start)), y + 1}, {tickToX(double(n.end())), y + keyHeight - 1},
                              IM_COL32(235, 80, 70, 230), 3.f);
        }
    dl->PopClipRect();
}

void PianoRoll::drawRuler(ImDrawList* dl) {
    ImVec2 rMin{gridMin_.x, origin_.y}, rMax{gridMax_.x, gridMin_.y};
    dl->PushClipRect(rMin, rMax, true);
    dl->AddRectFilled(rMin, rMax, IM_COL32(28, 29, 34, 255));

    // Abschnitte (Intro, Strophe, Refrain …)
    for (auto& sec : song_.sections) {
        float x0 = tickToX(double(sec.start)), x1 = tickToX(double(sec.start + sec.length));
        if (x1 < rMin.x || x0 > rMax.x) continue;
        dl->AddRectFilled({x0 + 1, rMin.y + 1}, {x1 - 1, rMin.y + kSectionH - 1}, sectionColor(sec.name), 3.f);
        dl->PushClipRect({std::max(x0, rMin.x), rMin.y}, {std::min(x1, rMax.x), rMin.y + kSectionH}, true);
        dl->AddText({std::max(x0, rMin.x) + 5, rMin.y + 1}, IM_COL32(240, 240, 245, 255), sec.name.c_str());
        dl->PopClipRect();
    }
    rMin.y += kSectionH;

    if (loopEnabled && loopEnd > loopStart)
        dl->AddRectFilled({tickToX(double(loopStart)), rMin.y + 14}, {tickToX(double(loopEnd)), rMax.y},
                          IM_COL32(90, 140, 255, 160));

    const Tick bar = song_.ticksPerBar(), beat = song_.ticksPerBeat();
    double t0 = std::max(0.0, xToTick(rMin.x)), t1 = xToTick(rMax.x);
    const bool showBeats = beat * pxPerTick() > 18.f;
    // Bei starkem Herauszoomen nur jeden n-ten Takt beschriften
    int labelEvery = std::max(1, int(std::ceil(40.f / (bar * pxPerTick()))));
    for (Tick t = Tick(t0 / double(beat)) * beat; double(t) <= t1; t += beat) {
        float x = tickToX(double(t));
        if (t % bar == 0) {
            int barNo = int(t / bar);
            dl->AddLine({x, rMin.y + 10}, {x, rMax.y}, IM_COL32(170, 175, 190, 255));
            if (barNo % labelEvery == 0) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", barNo + 1);
                dl->AddText({x + 4, rMin.y + 2}, IM_COL32(210, 215, 225, 255), buf);
            }
        } else if (showBeats) {
            dl->AddLine({x, rMin.y + 16}, {x, rMax.y}, IM_COL32(100, 104, 118, 255));
        }
    }
    float px = tickToX(engine_.position());
    dl->AddTriangleFilled({px - 6, rMin.y + 10}, {px + 6, rMin.y + 10}, {px, rMax.y}, IM_COL32(255, 220, 80, 255));
    dl->PopClipRect();
    dl->AddRectFilled(origin_, {gridMin_.x, gridMin_.y}, IM_COL32(28, 29, 34, 255));
}

void PianoRoll::drawKeyboard(ImDrawList* dl) {
    ImVec2 kMin{origin_.x, gridMin_.y}, kMax{gridMin_.x, gridMax_.y};
    dl->PushClipRect(kMin, kMax, true);
    int pTop = std::min(kTopPitch, yToPitch(kMin.y));
    int pBot = std::max(kLowPitch, yToPitch(kMax.y));
    // Instrument der aktuellen Spur (für Schlagzeug-Beschriftungen)
    const std::string instName = song_.tracks.empty() ? std::string() : song_.tracks[currentTrack].instrument;
    if (instName != labelInstName_) {
        labelInstName_ = instName;
        labelInst_ = InstrumentRegistry::instance().create(instName);
    }
    if (labelInst_ && labelInst_->isDrumKit()) {
        // Schlagzeug: jede Zeile ist ein Instrument
        for (int p = pTop; p >= pBot; --p) {
            float y = pitchToY(p);
            const char* label = labelInst_->noteLabel(p);
            ImU32 fill = p == previewPitch_ ? IM_COL32(255, 200, 80, 255)
                         : label           ? IM_COL32(70, 76, 92, 255)
                                           : IM_COL32(40, 42, 48, 255);
            dl->AddRectFilled({kMin.x, y}, {kMax.x, y + keyHeight}, fill);
            dl->AddLine({kMin.x, y + keyHeight}, {kMax.x, y + keyHeight}, IM_COL32(25, 25, 30, 255));
            if (label && keyHeight >= 10)
                dl->AddText(nullptr, std::min(ImGui::GetFontSize(), keyHeight + 1.f), {kMin.x + 3, y},
                            IM_COL32(225, 228, 235, 255), label);
        }
        dl->PopClipRect();
        return;
    }

    dl->AddRectFilled(kMin, kMax, IM_COL32(232, 232, 236, 255));   // weiße Tasten
    const float blackW = (kMax.x - kMin.x) * 0.62f;
    for (int p = pTop; p >= pBot; --p) {
        float y = pitchToY(p);
        bool pressed = p == previewPitch_;
        if (isBlack(p)) {
            dl->AddRectFilled({kMin.x, y}, {kMin.x + blackW, y + keyHeight},
                              pressed ? IM_COL32(255, 190, 60, 255) : IM_COL32(22, 22, 26, 255), 2.f);
        } else {
            if (pressed) dl->AddRectFilled({kMin.x, y}, {kMax.x, y + keyHeight}, IM_COL32(255, 205, 90, 255));
            // Trennlinien zwischen benachbarten weißen Tasten (B|C und E|F)
            int pc = ((p % 12) + 12) % 12;
            if (pc == 0 || pc == 5) dl->AddLine({kMin.x, y + keyHeight}, {kMax.x, y + keyHeight}, IM_COL32(140, 140, 150, 255));
            else dl->AddLine({kMin.x + blackW, y + keyHeight}, {kMax.x, y + keyHeight}, IM_COL32(190, 190, 198, 255));
        }
        if (p % 12 == 0 && keyHeight >= 10) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "C%d", p / 12 - 1);
            dl->AddText({kMax.x - 26, y + (keyHeight - ImGui::GetFontSize()) * 0.5f}, IM_COL32(40, 40, 50, 255), buf);
        }
    }
    dl->AddLine({kMax.x - 1, kMin.y}, {kMax.x - 1, kMax.y}, IM_COL32(0, 0, 0, 255));
    dl->PopClipRect();
}
