#pragma once
#include "analysis/Analyzer.h"
#include "core/Song.h"
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <imgui.h>

class AudioEngine;

// Gemeinsamer Zugriff der Zusatz-Ansichten auf Song, Wiedergabe und Auswahl.
struct ViewContext {
    Song& song;
    AudioEngine& engine;
    int& currentTrack;                   // ausgewählte Spur (gleich wie in Piano-Roll/Noten)
    std::function<void()> beforeEdit;    // Undo-Schnappschuss vor jeder Änderung
    std::function<void(double)> seek;   // Abspielposition setzen (Tick)
};

// Farbe einer Spur (gleich in allen Ansichten)
ImU32 trackColor(int track, float alpha = 1.f, float brightness = 1.f);
// Farbe eines Abschnitts nach seinem Namen (Intro, Strophe, Refrain …)
ImU32 sectionColor(const std::string& name, float alpha = 1.f);
// Ist die Spur ein Schlagzeug (Töne = Instrumente statt Tonhöhen)?
bool isDrumTrack(const Track& t);

// ---------------------------------------------------------------------
//  Arrangement: alle Spuren als Bahnen über die Zeit, Abschnitte als farbige Blöcke.
//  Klick = hinspringen, Rechtsklick auf einen Abschnitt = duplizieren / löschen / verschieben.
// ---------------------------------------------------------------------
class ArrangeView {
public:
    bool draw(ViewContext& ctx);
    float pxPerBar = 46.f;
    bool follow = true;
private:
    int menuSection_ = -1;
};

// Abschnitt-Werkzeuge (verschieben alle späteren Noten, Dynamik und Abschnitte mit)
void duplicateSection(Song& song, int section);
void deleteSection(Song& song, int section);
void swapSections(Song& song, int a);   // Abschnitt a mit dem folgenden tauschen

// ---------------------------------------------------------------------
//  Mischpult: pro Spur Lautstärke, Panorama, Stumm/Solo, Instrument und Pegelanzeige
// ---------------------------------------------------------------------
class MixerView {
public:
    bool draw(ViewContext& ctx);
private:
    std::vector<float> peakHold_;
    std::vector<double> peakTime_;
};

// ---------------------------------------------------------------------
//  Akkordblatt (Leadsheet): erkannte Akkorde Takt für Takt, nach Abschnitten, mit Stufen
// ---------------------------------------------------------------------
class ChordSheetView {
public:
    void draw(ViewContext& ctx);
    bool halfBars = true;      // zwei Akkorde pro Takt möglich
    bool showRoman = true;     // Stufen (I, IV, V, vi …) anzeigen
    float textScale = 1.f;
    bool follow = true;        // beim Abspielen zum aktuellen Takt scrollen
private:
    std::vector<ChordLabel> chords_;
    size_t signature_ = 0;
    Tick step_ = 0;
    int lastBar_ = -1;
};

// ---------------------------------------------------------------------
//  Drum-Raster: Schlagzeug-Muster Takt für Takt per Klick (wie ein Drumcomputer)
// ---------------------------------------------------------------------
class DrumGridView {
public:
    bool draw(ViewContext& ctx);
    int bar = 0;             // angezeigter Takt (0-basiert)
    bool follow = true;      // beim Abspielen mitlaufen
private:
    int drumTrack_ = -1;
    int paintMode_ = 0;      // 0 = nichts, 1 = setzen, -1 = löschen (beim Ziehen)
    std::vector<Note> clipboard_;
    bool hasClipboard_ = false;
};

// ---------------------------------------------------------------------
//  Visualizer: fallende Noten auf eine Klaviatur (alle Spuren in ihren Farben)
// ---------------------------------------------------------------------
class VisualizerView {
public:
    void draw(ViewContext& ctx);
    float lookahead = 4.f;   // Sekunden Vorschau
    bool showDrums = false;
    bool onlyCurrent = false;
    bool showChord = true;
private:
    int mouseNote_ = -1;
    std::vector<ChordLabel> chords_;
    size_t signature_ = 0;
};

// ---------------------------------------------------------------------
//  Spielen: große Klaviatur mit den Buchstaben der Computer-Tastatur auf jeder Taste,
//  Instrument frei wählbar (ohne den Song zu ändern), zeigt gespielte Töne und Akkord
// ---------------------------------------------------------------------
class LivePlayer;
class PlayView {
public:
    bool draw(ViewContext& ctx, LivePlayer& live, bool& keyboardPlay, bool recording, int recTrack);
private:
    int mouseNote_ = -1;
};

// Kleine Prüfsumme über die Noten (um Neuberechnungen nur bei Änderungen zu machen)
size_t songSignature(const Song& song);
