#pragma once
#include "core/Song.h"
#include "instruments/IInstrument.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <imgui.h>

class AudioEngine;

// Piano-Roll-Editor: Noten zeichnen, verschieben, verlängern, löschen.
//  Linksklick leer   -> Note setzen (ziehen = Länge)
//  Linksziehen Note  -> verschieben, am rechten Rand -> Länge ändern
//  Rechtsklick/-ziehen -> löschen
//  Mausrad = hoch/runter, Shift+Rad = links/rechts, Strg+Rad = Zoom
//  Mittlere Maustaste = verschieben, Alt = ohne Raster
//  Klick ins Lineal = Abspielposition setzen, Klick auf Tasten = vorhören
class PianoRoll {
public:
    PianoRoll(Song& song, AudioEngine& engine) : song_(song), engine_(engine) {}

    // Zeichnet den Editor in den restlichen Platz des aktuellen ImGui-Fensters.
    // Gibt true zurück, wenn der Song verändert wurde.
    bool draw();

    int currentTrack = 0;
    Tick snapTicks = 120;          // Raster (Standard 1/16 bei ppq 480)
    float newVelocity = 0.8f;
    float pxPerBeat = 90.f;        // Zoom horizontal
    float keyHeight = 15.f;        // Zoom vertikal
    bool followPlayhead = true;
    bool editEnabled = false;      // nur dann setzen/verschieben/löschen Klicks Noten
    bool dynEdit = false;          // Dynamik-Linie der aktuellen Spur mit der Maus zeichnen
    bool followSuspended = false;  // man hat selbst weggescrollt -> Mitlaufen pausiert bis zum nächsten Play

    // Ansicht zur Abspielposition springen und Mitlaufen fortsetzen
    void jumpToPlayhead(double tick) { followSuspended = false; scrollX_ = std::max(0.f, float(tick) * pxPerTick() - 40.f); }

    bool loopEnabled = false;      // nur zur Anzeige
    Tick loopStart = 0, loopEnd = 0;

    std::function<void()> beforeEdit;   // für Undo: wird vor jeder Änderung aufgerufen

    void resetView() { scrollX_ = 0; }

    const std::vector<Note>* overlay = nullptr;   // gerade aufgenommene Noten (live angezeigt)

private:
    static constexpr int kTopPitch = 108;   // C8
    static constexpr int kLowPitch = 21;    // A0

    enum class Drag { None, Create, Move, Resize, Erase };

    float tickToX(double t) const { return gridMin_.x + float(t) * pxPerTick() - scrollX_; }
    double xToTick(float x) const { return (x - gridMin_.x + scrollX_) / pxPerTick(); }
    float pitchToY(int p) const { return gridMin_.y + (kTopPitch - p) * keyHeight - scrollY_; }
    int yToPitch(float y) const;
    float pxPerTick() const { return pxPerBeat / float(song_.ppq); }
    Tick snap() const;
    int hitTest(ImVec2 m, bool& onEdge) const;

    void handleInput(bool hovered);
    void drawGrid(ImDrawList* dl);
    void drawNotes(ImDrawList* dl);
    void drawRuler(ImDrawList* dl);
    void drawKeyboard(ImDrawList* dl);
    void drawScrollbar(ImDrawList* dl);   // waagerechte Leiste: ziehen/klicken/Mausrad = vor und zurück schauen
    void scrubNotesBetween(double a, double b);
    void updateScrubNotes();
    struct ScrubNote { int track; int pitch; double until; };
    std::vector<ScrubNote> scrubNotes_;
    bool rulerDrag_ = false, resumeAfterScrub_ = false;
    bool dynDrag_ = false;
    double dynLastTick_ = 0;
    static constexpr float kDynH = 60.f;   // Höhe der Dynamik-Spur unten im Raster
    double lastScrubTick_ = 0;
    bool scrollDrag_ = false;
    float scrollDragOffset_ = 0.f;
    void preview(int pitch);
    void stopPreview();
    void edit() { if (!editedThisDrag_ && beforeEdit) beforeEdit(); editedThisDrag_ = true; changed_ = true; }

    Song& song_;
    AudioEngine& engine_;

    ImVec2 origin_, size_, gridMin_, gridMax_;
    float scrollX_ = 0, scrollY_ = (kTopPitch - 76) * 15.f;

    Drag drag_ = Drag::None;
    int dragNote_ = -1;
    Note dragOrig_{};
    double dragStartTick_ = 0;
    int dragStartPitch_ = 0;
    Tick lastLength_ = 480;
    int previewPitch_ = -1;
    bool changed_ = false;
    bool editedThisDrag_ = false;
    bool wasPlaying_ = false;

    std::string labelInstName_;
    std::unique_ptr<IInstrument> labelInst_;   // für Tasten-Beschriftung (Schlagzeug)
};
