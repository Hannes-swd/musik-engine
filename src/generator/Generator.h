#pragma once
#include "core/Song.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Algorithmischer Song-Generator.
// Regeln (Funktionsharmonik, Motiv-Wiederholung, Stimmführung …) sorgen dafür,
// dass es gut klingt; der Zufall (Seed) dafür, dass jeder Song anders ist.

enum class AccompStyle { Auto, Pad, Rhythm, Arpeggio, Alberti, Broken, Count };
enum class BassStyle { Auto, Whole, RootFifth, Eighths, Walking, Syncopated, Count };
enum class DrumStyle { Auto, Pop, Rock, HalfTime, FourOnFloor, Ballad, Jazz, Epic, Count };

enum class Ensemble { Band, Orchestra };

// Eine eigene musikalische Idee (z.B. live eingespielt), die der Generator ausbaut.
struct MusicalIdea {
    std::vector<Note> notes;   // ab Tick 0 = Beginn des ersten Takts der Idee (bereits quantisiert)
    int bars = 4;
    bool harmonic = false;     // true = Akkorde gespielt (sonst Melodie)
    bool asChorus = false;     // false = Song beginnt mit der Idee, true = Idee wird zum Refrain (Hook)
};

const char* accompStyleName(AccompStyle s);
const char* bassStyleName(BassStyle s);
const char* drumStyleName(DrumStyle s);
int orchStyleCount();
// Raumklang-Vorlagen (Hall/Echo): 1 = Trocken … roomPresetCount()-1; 0 = "passend" (nur im Generator)
int roomPresetCount();
const char* roomPresetName(int preset);
void applyRoomPreset(Effects& fx, int preset);
int songFormCount();
const char* songFormName(int form);    // -1 = "Zufall (passend)"
const char* songFormHint(int form);
const char* orchStyleName(int style);

struct GeneratorSettings {
    uint32_t seed = 0;             // 0 = neuer Zufalls-Seed
    double bpm = 0;                // <= 0: zufällig passend zur Skala
    int beatsPerBar = 4;           // 4/4 oder 3/4
    int key = -1;                  // 0 = C … 11 = H, -1 = zufällig
    int scale = -1;                // ScaleType, -1 = zufällig
    double lengthSeconds = 120;
    std::string structure;         // leer = automatisch, sonst z.B. "I4 V8 C8 V8 C8 B8 C8 O4"

    float creativity = 0.35f;      // 0 = brav, 1 = mutig (Sprünge, Fremdakkorde, Variation)
    float melodyDensity = 0.5f;    // wie viele Noten die Melodie hat
    float syncopation = 0.3f;      // Off-Beats & Vorgezogenes
    float energy = 0.5f;           // Lautstärke/Intensität insgesamt
    float humanize = 0.3f;         // kleine Timing-/Anschlag-Schwankungen

    // Feinheiten
    float swing = 0.f;             // 0 = gerade, 1 = volles Triolen-Swing (Shuffle)
    float ambitus = 0.5f;          // Tonumfang der Melodie: 0 = eng, 1 = weit
    float leaps = 0.5f;            // Sprungfreude: 0 = fast nur Schritte, 1 = viele große Intervalle
    float repetition = 0.5f;       // Wiedererkennung: 0 = ständig neu, 1 = Motive kehren fast gleich wieder
    int harmonicRhythm = 0;        // Akkordwechsel: 0 = automatisch, 1 = langsam (2 Takte), 2 = normal (1 Takt), 3 = schnell (1/2 Takt)
    float jazzChords = -1.f;       // Septakkorde 0..1 (-1 = aus der Kreativität ableiten)
    float colorChords = -1.f;      // Farbakkorde/Zwischendominanten 0..1 (-1 = aus der Kreativität ableiten)
    float dynamicRange = 0.5f;     // Dynamik-Umfang: 0 = gleichmäßig, 1 = sehr leise bis sehr laut
    bool useIntro = true, usePre = true, useBridge = true, useOutro = true;   // Form-Bausteine erlauben
    bool generalPause = true;      // Stille vor dem letzten Refrain
    int form = -1;                 // Songform (siehe songFormName), -1 = passend zur Stimmung würfeln
    int room = 0;                  // Raumklang: 0 = passend, sonst Raum-Vorlage (siehe roomPresetName)
    bool mixSystems = true;        // Kompositions-Systeme mischen (Vamp, Quintfall, Tresillo, Riff …)
    float systemVariety = 0.5f;    // wie viele / wie ungewöhnliche Systeme pro Song (0 = fast nur klassisch)

    // Eigene Akkordfolgen (leer = Generator wählt), z.B. "Am F C G" – je Akkord ein Takt
    std::string chordsVerse, chordsChorus, chordsPre, chordsBridge;

    // Instrumental-Solo nach dem zweiten Refrain (improvisiert über die Strophen-Akkorde)
    bool solo = false;
    std::string soloInstrument = "Lead";

    // Groove-Gefühl (Mikrotiming): 0 = präzise, 1 = laid-back (Snare/Bass etwas später), 2 = treibend (Hi-Hats vorne)
    int groove = 0;
    float grooveAmount = 0.5f;

    // Für "Abschnitt neu komponieren": Werte des Originals festhalten
    int forceKeyChangeAt = -2;     // -2 = automatisch, -1 = nie, >= 0 = Abschnitt
    int forceKeyChangeSemis = 2;
    int onlyTry = -1;              // nur diesen Kompositions-Durchgang rechnen (-1 = alle, bester gewinnt)

    AccompStyle accompaniment = AccompStyle::Auto;
    BassStyle bass = BassStyle::Auto;
    bool withMelody = true, withChords = true, withBass = true, withDrums = true;
    DrumStyle drums = DrumStyle::Auto;
    bool allowKeyChange = true;    // Rückung im letzten Refrain
    int melodyOctaveShift = 0;     // -1, 0, +1

    std::string melodyInstrument = "Piano";
    std::string chordInstrument = "Piano";
    std::string bassInstrument = "Piano";
    std::string drumInstrument = "Schlagzeug";

    Ensemble ensemble = Ensemble::Band;          // Band oder Sinfonieorchester
    int orchStyle = -1;                          // Orchester-Stil (siehe orchStyleName), -1 = Zufall
    std::shared_ptr<const MusicalIdea> idea;     // optional: eigene Idee ausbauen

    // Gründlichkeit 1..10: wie viele Kandidaten der "Kritiker" bewertet
    // (mehr = bessere Melodien, längere Rechenzeit)
    int quality = 6;
};

struct GenerationInfo {
    uint32_t seed = 0;
    std::string key;                       // z.B. "A Moll"
    double bpm = 0;
    std::string structure;                 // z.B. "I4 V8 C8 …"
    std::vector<std::string> progressions; // z.B. "Refrain: F  C  G  Am"
    std::string drums;                     // gewählter Schlagzeug-Stil
    std::string orchStyle;                 // gewählter Orchester-Stil
    std::string formName;                  // gewählte Songform
    std::vector<std::string> systems;      // Kompositions-Systeme pro Abschnitt (Harmonie · Rhythmus · Melodie)
    std::vector<std::string> techniques;   // eingesetzte Orchester-Techniken pro Abschnitt
    std::string notes;                     // Besonderheiten (Rückung …)
    float score = 0;                       // Bewertung des Kritikers
    int candidatesEvaluated = 0;

    // Aufgelöste Werte (damit ein Abschnitt passend neu komponiert werden kann)
    int orchStyleIndex = -1;
    int drumStyleIndex = 0;
    int keyChangeAt = -1, keyChangeSemis = 0;
    int bestTry = 0;
};

// Ersetzt die Abschnitte (Indizes in song.sections) von target durch die gleichen Abschnitte aus source.
// Beide Songs müssen dieselbe Form haben (gleiche Abschnittsgrenzen).
void spliceSections(Song& target, const Song& source, const std::vector<int>& sections);

// Liest ein Akkordsymbol wie "Am", "F#m7", "Bbmaj7", "Gsus4", "Hdim", "C/E" (deutsch H = B)
bool parseChordSymbol(const std::string& text, int& rootPc, std::vector<int>& intervals);

// progress (optional) wird von 0 bis 1 hochgezählt – für Fortschrittsbalken in einem anderen Thread.
Song generateSong(const GeneratorSettings& settings, GenerationInfo* info = nullptr,
                  std::atomic<float>* progress = nullptr);
