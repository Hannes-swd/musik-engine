#pragma once
#include "core/Song.h"
#include "generator/Generator.h"
#include <atomic>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Fenster mit allen Generator-Einstellungen. Rechnet im Hintergrund-Thread.
class GeneratorPanel {
public:
    ~GeneratorPanel();

    // current = der Song im Editor (für "Abschnitte bearbeiten")
    void draw(const Song* current = nullptr);
    void startGeneration();
    // Startet mit fertigen Einstellungen (z.B. "Idee ausbauen"), ohne die Felder des Fensters zu benutzen.
    // splice: nur diese Abschnitte des aktuellen Songs ersetzen (leer = ganzer neuer Song)
    void launch(GeneratorSettings s, std::vector<int> splice = {});
    bool busy() const { return running_; }
    // Liefert true, wenn ein fertiger Song abgeholt werden kann.
    // Mit infoOut landet die Song-Info dort (statt sofort im Fenster angezeigt zu werden).
    // spliceOut: nicht leer = nur diese Abschnitte in den aktuellen Song übernehmen (spliceSections)
    bool takeResult(Song& out, GenerationInfo* infoOut = nullptr, std::vector<int>* spliceOut = nullptr);

    // Stimmungsvorlagen
    static int presetCount();
    static const char* presetName(int preset);
    static const char* presetHint(int preset);

    // Beschreibung in Worten ("episch, langsam, Orchester, in d-moll") -> Einstellungen.
    // Liefert eine kurze Zusammenfassung, was erkannt wurde.
    static std::string applyPromptTo(GeneratorSettings& s, const std::string& text, std::mt19937& rng,
                                     bool& randomBpm, float& bpm, int& meter, int* presetOut = nullptr);

    // Endlos-Modus ("Radio"): Einstellungen für den nächsten Song (neuer Seed, evtl. andere Stimmung)
    GeneratorSettings endlessSettings();

    GeneratorSettings settings;
    GenerationInfo info;
    bool open = true;

    bool endless = false;          // Endlos-Modus an
    bool endlessVary = false;      // Stimmung von Song zu Song wechseln
    float endlessPause = 1.f;      // Pause zwischen zwei Songs (Sekunden)
    int endlessSongs = 0;          // gespielte Songs in dieser Endlos-Sitzung
    std::string endlessMood;       // Stimmung des aktuell laufenden Endlos-Songs
    std::string endlessNextMood;   // Stimmung des Songs, der gerade vorbereitet wird

    // Setzt alle Regler einer Vorlage auf die übergebenen Einstellungen
    static void applyPresetTo(GeneratorSettings& s, int preset, std::mt19937& rng, bool& randomBpm, float& bpm, int& meter);

private:
    void applyPreset(int preset);
    void drawSections(const Song& song);
    // Abschnitt idx neu komponieren (newIdeas) oder mit geänderter Energie neu setzen (energyDelta)
    void requestSection(const Song& song, int idx, bool newIdeas, int energyDelta);

    bool lockSeed_ = false;
    bool randomBpm_ = true;
    float bpmValue_ = 110.f;
    int meterIndex_ = 0;
    int lastPreset_ = -1;
    int lastEndlessPreset_ = -1;
    double lastSeconds_ = 0;
    std::mt19937 rng_{std::random_device{}()};

    // Beschreibung
    std::string prompt_, promptFeedback_;

    // Abschnitte bearbeiten: womit der aktuelle Song komponiert wurde
    std::string songTitle_;                // Titel des zuletzt generierten Songs (zur Zuordnung)
    std::string songStructure_;            // Form-Text inkl. Energie-Zeichen
    GeneratorSettings songSettings_;
    GenerationInfo songInfo_;
    std::vector<uint32_t> secSeed_;        // pro Abschnitt: Seed, aus dem er stammt
    std::vector<int> secTry_;              // … und welcher Kompositions-Durchgang
    bool spliceAll_ = true;                // gleiche Abschnitte (alle Refrains …) mitändern

    std::thread worker_;
    std::atomic<bool> running_{false}, ready_{false};
    std::atomic<float> progress_{0.f};
    std::mutex mutex_;
    Song result_;
    GenerationInfo resultInfo_;
    GeneratorSettings jobSettings_, resultSettings_;
    std::vector<int> jobSplice_, resultSplice_;
};
