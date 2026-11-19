#pragma once
#include "core/Song.h"
#include <imgui.h>
#include <string>

class AudioEngine;

// Notenansicht: setzt den Song als Partitur (eine oder alle Spuren) mit automatischem
// Zeilenumbruch, Vorzeichen, Balken, Bindebögen, Pausen und Hilfslinien.
// Klick auf einen Takt = Abspielposition setzen, Strg+Mausrad = Zoom.
class ScoreView {
public:
    ScoreView(Song& song, AudioEngine& engine) : song_(song), engine_(engine) {}

    // Lädt die Notenschrift (Bravura, SMuFL). Vor dem ersten Frame aufrufen.
    static bool loadFont(const std::string& path);

    void draw();

    bool allTracks = true;   // Partitur aller Spuren oder nur die aktuelle
    int currentTrack = 0;
    float zoom = 1.f;
    bool follow = true;      // beim Abspielen mitscrollen
    bool followSuspended = false;   // man hat selbst gescrollt -> bis zum nächsten Play pausiert
    void jumpToPlayhead() { followSuspended = false; forceJump_ = true; }

private:
    bool wasPlaying_ = false, autoScrolled_ = false, forceJump_ = false;
    float prevScroll_ = 0.f;
    Song& song_;
    AudioEngine& engine_;
    static ImFont* font_;
    static float emRatio_;   // em / (Oberlänge - Unterlänge) der Notenschrift
};
