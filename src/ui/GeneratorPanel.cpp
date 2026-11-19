#include "GeneratorPanel.h"
#include "instruments/InstrumentRegistry.h"
#include "theory/Theory.h"
#include <algorithm>
#include <chrono>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <sstream>
#include <cctype>
#include <cstring>
#include <regex>

namespace {
void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool instrumentCombo(const char* label, std::string& value) {
    bool changed = false;
    if (ImGui::BeginCombo(label, value.c_str())) {
        for (auto& n : InstrumentRegistry::instance().names())
            if (ImGui::Selectable(n.c_str(), n == value)) { value = n; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

struct PresetDef { const char* name; const char* hint; };
const PresetDef kPresets[] = {
    {"Fröhlich", "Dur, flottes Tempo, Pop-Groove, eingängige Wiederholungen."},
    {"Traurig", "Moll, langsam, Klavier mit Streichern, viel Dynamik."},
    {"Episch", "Band/Hybrid: Kino-Groove mit Toms, Chor-Flächen, große Bögen."},
    {"Ruhig", "Pentatonik, Flöte und Gitarre, wenig Lautstärke-Wechsel, ohne Schlagzeug."},
    {"Jazzig", "Dorisch, Swing, Walking-Bass, viele Septakkorde und Farbakkorde."},
    {"Orchester", "Sinfonieorchester, filmisch, Orchester-Stil zufällig."},
    {"Melancholisch", "Moll mit Septakkorden, E-Piano-Arpeggios über Pad, nachdenklich."},
    {"Romantisch", "Orchester, romantischer Stil: weite Melodiebögen und große Dynamik."},
    {"Abenteuer", "Orchester, Film-Stil in Dur/Lydisch: schnell, heldenhafte Sprünge."},
    {"Spannung", "Orchester, mysteriös: phrygisch/harmonisch Moll, langsame Akkordwechsel, extreme Dynamik."},
    {"Tanzbar", "Four-on-the-floor, Synth-Lead, starke Wiederholung, treibend."},
    {"Lofi / Chill", "Swing, jazzige Akkorde, Gitarre und E-Piano, entspannt und leicht schief."},
    {"Märchen", "Glocken, Harfe und Pizzicato in Lydisch, verspielt, ohne Schlagzeug."},
    {"Heldenhaft", "Orchester im Marsch-Stil: Fanfaren, große Sprünge, kraftvoll."},
    {"Walzer", "Orchester im 3/4-Takt: bum-tschak-tschak, schwungvoll."},
    {"Weltraum", "Lydisch, Glocken über weiten Pad-Flächen, sehr langsame Akkorde, schwebend."},
    {"Weihnachtlich", "Glocken, Klavier, Kontrabass, leichter Swing und warme Septakkorde."},
    {"Barock", "Orchester im Barock-/Kammerstil: Dorisch/Moll, gleichmäßiger Akkordwechsel, Terrassendynamik."},
    {"Western", "Mixolydisch oder Moll, Gitarren-Rhythmus, weite Sprünge, bVII-Akkorde."},
    {"Sommer-Pop", "Marimba, Gitarre, synkopiert und sonnig, sehr eingängig."},
    {"Rock", "Verzerrte E-Gitarren-Akkorde, treibendes Schlagzeug, Gitarren-Solo."},
    {"Paris (Musette)", "Akkordeon im Walzertakt, Gitarre und Kontrabass - wie im Café."},
    {"Karibik", "Steel Drum und Ukulele, Offbeat-Groove mit Congas, Bongos und Shaker."},
    {"Bluegrass", "Banjo-Melodie, Mandolinen-Tremolo, Gitarre und Kontrabass - schnell und fröhlich."},
    {"Kirche / Feierlich", "Kirchenorgel mit Chor, langsam und erhaben, ohne Schlagzeug."},
    {"Anden", "Panflöte über Gitarre und Mandoline, Moll-Pentatonik, sehnsuchtsvoll."},
    {"Traumland", "Kalimba und Harfe, laid-back Groove, warm und verträumt."},
    {"Zufall", "Würfelt alle Regler, Instrumente und Besetzung neu."},
};
constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));
}

int GeneratorPanel::presetCount() { return kPresetCount; }
const char* GeneratorPanel::presetName(int p) { return p >= 0 && p < kPresetCount ? kPresets[p].name : "Eigene"; }
const char* GeneratorPanel::presetHint(int p) { return p >= 0 && p < kPresetCount ? kPresets[p].hint : ""; }

GeneratorPanel::~GeneratorPanel() {
    if (worker_.joinable()) worker_.join();
}

void GeneratorPanel::startGeneration() {
    if (running_) return;
    if (!lockSeed_) settings.seed = rng_() | 1u;
    settings.bpm = randomBpm_ ? 0.0 : double(bpmValue_);
    settings.beatsPerBar = meterIndex_ == 1 ? 3 : 4;
    settings.idea.reset();
    launch(settings);
}

void GeneratorPanel::launch(GeneratorSettings copy, std::vector<int> splice) {
    if (running_) return;
    if (worker_.joinable()) worker_.join();
    if (copy.seed == 0) copy.seed = rng_() | 1u;
    running_ = true;
    ready_ = false;
    progress_ = 0.f;
    worker_ = std::thread([this, copy, splice] {
        auto t0 = std::chrono::steady_clock::now();
        GenerationInfo gi;
        Song s = generateSong(copy, &gi, &progress_);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        {
            std::lock_guard lock(mutex_);
            result_ = std::move(s);
            resultInfo_ = gi;
            resultSettings_ = copy;
            resultSplice_ = splice;
            lastSeconds_ = secs;
        }
        ready_ = true;
        running_ = false;
    });
}

bool GeneratorPanel::takeResult(Song& out, GenerationInfo* infoOut, std::vector<int>* spliceOut) {
    if (!ready_) return false;
    std::lock_guard lock(mutex_);
    out = std::move(result_);
    if (spliceOut) *spliceOut = resultSplice_;
    if (!resultSplice_.empty() && spliceOut) {
        // Nur einzelne Abschnitte: Buchführung aktualisieren, Song-Info bleibt (bis auf die Form)
        songStructure_ = resultInfo_.structure;
        for (int i : resultSplice_)
            if (i >= 0 && size_t(i) < secSeed_.size()) { secSeed_[size_t(i)] = resultInfo_.seed; secTry_[size_t(i)] = resultInfo_.bestTry; }
        info.structure = resultInfo_.structure;
        songInfo_.structure = resultInfo_.structure;
    } else {
        if (infoOut) *infoOut = resultInfo_;
        else info = resultInfo_;
        songTitle_ = out.title;
        songStructure_ = resultInfo_.structure;
        songSettings_ = resultSettings_;
        songInfo_ = resultInfo_;
        secSeed_.assign(out.sections.size(), resultInfo_.seed);
        secTry_.assign(out.sections.size(), resultInfo_.bestTry);
    }
    ready_ = false;
    return true;
}

namespace {
std::vector<std::string> splitWords(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    std::string t;
    while (in >> t) out.push_back(t);
    return out;
}
int energyMarks(const std::string& tok) {
    int m = 0;
    for (char c : tok) m += c == '+' ? 1 : c == '-' ? -1 : 0;
    return m;
}
std::string withMarks(const std::string& tok, int marks) {
    std::string base;
    for (char c : tok) if (c != '+' && c != '-') base += c;
    marks = std::clamp(marks, -4, 4);
    return base + std::string(size_t(std::abs(marks)), marks > 0 ? '+' : '-');
}
}

void GeneratorPanel::requestSection(const Song& song, int idx, bool newIdeas, int energyDelta) {
    if (running_) return;
    auto tokens = splitWords(songStructure_);
    if (tokens.size() != song.sections.size() || idx < 0 || idx >= int(tokens.size())) return;
    std::vector<int> targets = {idx};
    if (newIdeas && spliceAll_) {   // alle Abschnitte derselben Art (gleicher Buchstabe und Länge)
        auto core = [](const std::string& t) { std::string b; for (char c : t) if (c != '+' && c != '-') b += c; return b; };
        targets.clear();
        for (int i = 0; i < int(tokens.size()); ++i)
            if (core(tokens[size_t(i)]) == core(tokens[size_t(idx)])) targets.push_back(i);
    }
    if (energyDelta != 0) tokens[size_t(idx)] = withMarks(tokens[size_t(idx)], energyMarks(tokens[size_t(idx)]) + energyDelta);
    std::string structure;
    for (auto& t : tokens) structure += t + " ";

    GeneratorSettings c = songSettings_;
    c.structure = structure;
    c.key = song.key >= 0 ? song.key : c.key;
    c.scale = song.scale >= 0 ? song.scale : c.scale;
    c.bpm = song.bpm;
    c.beatsPerBar = song.beatsPerBar;
    c.orchStyle = songInfo_.orchStyleIndex;
    c.drums = DrumStyle(std::clamp(songInfo_.drumStyleIndex, 0, int(DrumStyle::Count) - 1));
    c.forceKeyChangeAt = songInfo_.keyChangeAt;
    c.forceKeyChangeSemis = songInfo_.keyChangeSemis;
    if (newIdeas) {
        c.seed = rng_() | 1u;
        c.onlyTry = -1;
    } else {   // gleiche Ideen, nur andere Energie: aus demselben Seed/Durchgang wie bisher
        c.seed = secSeed_.empty() ? songInfo_.seed : secSeed_[size_t(idx)];
        c.onlyTry = secTry_.empty() ? songInfo_.bestTry : secTry_[size_t(idx)];
    }
    launch(c, targets);
}

void GeneratorPanel::drawSections(const Song& song) {
    const auto tokens = splitWords(songStructure_);
    if (songTitle_.empty() || song.title != songTitle_ || tokens.size() != song.sections.size()) {
        ImGui::TextDisabled("Nur für Songs, die in dieser Sitzung generiert wurden.");
        return;
    }
    ImGui::Checkbox("Gleiche Abschnitte mitändern", &spliceAll_);
    helpMarker("An: \"Neu\" bei einem Refrain komponiert alle Refrains neu (sie bleiben untereinander gleich).\n"
               "Aus: nur genau dieser eine Abschnitt.");
    ImGui::BeginDisabled(running_ || endless);
    if (ImGui::BeginTable("##abschnitte", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Abschnitt", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Takte");
        ImGui::TableSetupColumn("Energie");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        int action = -1, delta = 0;
        bool fresh = false;
        for (int i = 0; i < int(song.sections.size()); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(song.sections[size_t(i)].name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%lld", (long long)(song.sections[size_t(i)].length / std::max<Tick>(1, song.ticksPerBar())));
            ImGui::TableNextColumn();
            const int marks = energyMarks(tokens[size_t(i)]);
            if (ImGui::SmallButton("-")) { action = i; delta = -1; }
            ImGui::SetItemTooltip("Leiser / ruhiger (gleiche Melodie, weniger Instrumente)");
            ImGui::SameLine();
            ImGui::Text("%+d", marks);
            ImGui::SameLine();
            if (ImGui::SmallButton("+")) { action = i; delta = 1; }
            ImGui::SetItemTooltip("Lauter / voller (gleiche Melodie, mehr Instrumente)");
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Neu")) { action = i; fresh = true; }
            ImGui::SetItemTooltip("Diesen Abschnitt neu komponieren (neue Melodie und Akkorde, Rest bleibt)");
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (action >= 0) requestSection(song, action, fresh, delta);
    }
    ImGui::EndDisabled();
}

GeneratorSettings GeneratorPanel::endlessSettings() {
    GeneratorSettings c = settings;
    if (endlessVary) {
        // Neue Stimmung würfeln (nicht zweimal dieselbe hintereinander)
        int p = lastEndlessPreset_;
        for (int t = 0; t < 8 && p == lastEndlessPreset_; ++t) p = int(rng_() % unsigned(kPresetCount));
        lastEndlessPreset_ = p;
        bool rb = randomBpm_;
        float bpm = bpmValue_;
        int meter = 0;
        applyPresetTo(c, p, rng_, rb, bpm, meter);
        c.key = -1;
        c.bpm = rb ? 0.0 : double(bpm) * std::uniform_real_distribution<double>(0.93, 1.07)(rng_);
        c.beatsPerBar = meter == 1 ? 3 : 4;
        endlessNextMood = kPresets[p].name;
    } else {
        c.bpm = randomBpm_ ? 0.0 : double(bpmValue_);
        c.beatsPerBar = meterIndex_ == 1 ? 3 : 4;
        endlessNextMood = lastPreset_ >= 0 ? kPresets[lastPreset_].name : "eigene Einstellungen";
    }
    c.seed = rng_() | 1u;
    c.idea.reset();
    return c;
}

void GeneratorPanel::applyPreset(int preset) {
    applyPresetTo(settings, preset, rng_, randomBpm_, bpmValue_, meterIndex_);
    lastPreset_ = preset;
}

void GeneratorPanel::applyPresetTo(GeneratorSettings& s, int preset, std::mt19937& rng, bool& randomBpm, float& bpm, int& meter) {
    auto f = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    auto coin = [&](float p) { return f(0.f, 1.f) < p; };
    auto either = [&](ScaleType a, ScaleType b, float pa) { return int(coin(pa) ? a : b); };
    auto inst = [&](const char* mel, const char* chords, const char* bass) {
        s.melodyInstrument = mel; s.chordInstrument = chords; s.bassInstrument = bass;
    };
    auto orchestra = [&](int style) { s.ensemble = Ensemble::Orchestra; s.orchStyle = style; };
    // Grundzustand: alles auf "automatisch" / Standard
    s.accompaniment = AccompStyle::Auto;
    s.bass = BassStyle::Auto;
    s.drums = DrumStyle::Auto;
    s.withMelody = s.withChords = s.withBass = s.withDrums = true;
    s.drumInstrument = "Schlagzeug";
    s.ensemble = Ensemble::Band;
    s.orchStyle = -1;
    s.humanize = 0.3f;
    s.melodyOctaveShift = 0;
    s.allowKeyChange = true;
    s.swing = 0.f; s.ambitus = 0.5f; s.leaps = 0.5f; s.repetition = 0.5f; s.harmonicRhythm = 0;
    s.jazzChords = -1.f; s.colorChords = -1.f; s.dynamicRange = 0.5f;
    s.useIntro = s.usePre = s.useBridge = s.useOutro = true;
    s.generalPause = true;
    s.solo = false; s.soloInstrument = "Lead"; s.groove = 0; s.grooveAmount = 0.5f;   // eigene Akkorde bleiben erhalten
    s.form = -1;   // Songform passend zur Stimmung würfeln
    s.room = 0;    // Raumklang passend zur Musik
    randomBpm = false;
    meter = 0;

    switch (preset) {
    case 0:   // Fröhlich
        s.scale = int(ScaleType::Major); bpm = 120; s.melodyDensity = 0.6f; s.syncopation = 0.35f;
        s.energy = 0.65f; s.creativity = 0.3f; s.drums = DrumStyle::Pop;
        s.repetition = 0.65f; s.ambitus = 0.55f; s.jazzChords = 0.15f; s.colorChords = 0.15f;
        inst("Piano", "E-Piano", "Synth-Bass");
        break;
    case 1:   // Traurig
        s.scale = int(ScaleType::NaturalMinor); bpm = 72; s.melodyDensity = 0.35f; s.syncopation = 0.1f;
        s.energy = 0.35f; s.creativity = 0.3f; s.drums = DrumStyle::Ballad;
        s.leaps = 0.4f; s.dynamicRange = 0.7f; s.jazzChords = 0.2f; s.colorChords = 0.25f;
        inst("Piano", "Streicher", "Kontrabass");
        break;
    case 2:   // Episch (Band/Hybrid)
        s.scale = int(ScaleType::NaturalMinor); bpm = 90; s.melodyDensity = 0.4f; s.syncopation = 0.2f;
        s.energy = 0.9f; s.creativity = 0.35f; s.drums = DrumStyle::Epic;
        s.dynamicRange = 0.9f; s.ambitus = 0.7f; s.leaps = 0.55f; s.repetition = 0.65f;
        s.jazzChords = 0.f; s.colorChords = 0.3f;
        inst("Streicher", "Chor", "Synth-Bass");
        break;
    case 3:   // Ruhig
        s.scale = int(ScaleType::MajorPentatonic); bpm = 78; s.melodyDensity = 0.3f; s.syncopation = 0.1f;
        s.energy = 0.25f; s.creativity = 0.25f; s.accompaniment = AccompStyle::Arpeggio; s.withDrums = false;
        s.dynamicRange = 0.25f; s.leaps = 0.3f; s.ambitus = 0.4f; s.harmonicRhythm = 1;
        s.jazzChords = 0.1f; s.colorChords = 0.1f; s.generalPause = false;
        inst("Flöte", "Gitarre", "Kontrabass");
        break;
    case 4:   // Jazzig
        s.scale = int(ScaleType::Dorian); bpm = 112; s.melodyDensity = 0.55f; s.syncopation = 0.6f;
        s.energy = 0.55f; s.creativity = 0.7f; s.bass = BassStyle::Walking; s.drums = DrumStyle::Jazz;
        s.swing = 0.7f; s.jazzChords = 0.9f; s.colorChords = 0.6f; s.leaps = 0.6f; s.ambitus = 0.6f;
        s.repetition = 0.35f; s.humanize = 0.45f; s.solo = true; s.soloInstrument = "Vibraphon"; s.groove = 1; s.grooveAmount = 0.4f;
        inst("Saxofon", "Piano", "Kontrabass");   // Sax-Melodie, Vibraphon-Solo
        break;
    case 5:   // Orchester (filmisch)
        s.scale = either(ScaleType::NaturalMinor, ScaleType::Major, 0.66f); bpm = 84;
        s.melodyDensity = 0.4f; s.syncopation = 0.12f; s.energy = 0.7f; s.creativity = 0.35f;
        s.dynamicRange = 0.75f;
        orchestra(-1);
        break;
    case 6:   // Melancholisch
        s.scale = either(ScaleType::NaturalMinor, ScaleType::Dorian, 0.6f); bpm = 84;
        s.melodyDensity = 0.45f; s.syncopation = 0.2f; s.energy = 0.4f; s.creativity = 0.35f;
        s.drums = DrumStyle::Ballad; s.accompaniment = AccompStyle::Arpeggio;
        s.jazzChords = 0.45f; s.colorChords = 0.35f; s.leaps = 0.45f; s.repetition = 0.55f; s.dynamicRange = 0.55f;
        inst("E-Piano", "Pad", "Synth-Bass");
        break;
    case 7:   // Romantisch
        s.scale = either(ScaleType::Major, ScaleType::NaturalMinor, 0.6f); bpm = 76;
        s.melodyDensity = 0.45f; s.syncopation = 0.1f; s.energy = 0.5f; s.creativity = 0.4f;
        s.ambitus = 0.75f; s.leaps = 0.6f; s.dynamicRange = 0.85f; s.colorChords = 0.45f; s.jazzChords = 0.25f;
        orchestra(1);
        break;
    case 8:   // Abenteuer
        s.scale = either(ScaleType::Major, ScaleType::Lydian, 0.7f); bpm = 128;
        s.melodyDensity = 0.55f; s.syncopation = 0.25f; s.energy = 0.8f; s.creativity = 0.4f;
        s.ambitus = 0.8f; s.leaps = 0.65f; s.repetition = 0.6f; s.dynamicRange = 0.8f; s.colorChords = 0.35f; s.jazzChords = 0.05f;
        orchestra(0);
        break;
    case 9:   // Spannung / Thriller
        s.scale = either(ScaleType::Phrygian, ScaleType::HarmonicMinor, 0.5f); bpm = 96;
        s.melodyDensity = 0.35f; s.syncopation = 0.3f; s.energy = 0.55f; s.creativity = 0.5f;
        s.harmonicRhythm = 1; s.ambitus = 0.4f; s.leaps = 0.35f; s.repetition = 0.7f; s.dynamicRange = 0.95f;
        s.colorChords = 0.5f; s.jazzChords = 0.1f; s.allowKeyChange = false;
        orchestra(5);
        break;
    case 10:  // Tanzbar
        s.scale = either(ScaleType::NaturalMinor, ScaleType::Major, 0.55f); bpm = 124;
        s.melodyDensity = 0.55f; s.syncopation = 0.5f; s.energy = 0.85f; s.creativity = 0.3f;
        s.drums = DrumStyle::FourOnFloor; s.bass = BassStyle::Syncopated; s.accompaniment = AccompStyle::Rhythm;
        s.repetition = 0.8f; s.ambitus = 0.4f; s.leaps = 0.4f; s.dynamicRange = 0.4f; s.harmonicRhythm = 2;
        s.jazzChords = 0.2f; s.colorChords = 0.1f;
        s.groove = 2; s.grooveAmount = 0.5f;
        inst("Lead", "Pad", "Synth-Bass");
        break;
    case 11:  // Lofi / Chill
        s.scale = either(ScaleType::Dorian, ScaleType::Major, 0.5f); bpm = 82;
        s.melodyDensity = 0.4f; s.syncopation = 0.45f; s.energy = 0.35f; s.creativity = 0.45f;
        s.drums = DrumStyle::HalfTime; s.swing = 0.55f; s.humanize = 0.6f;
        s.jazzChords = 0.85f; s.colorChords = 0.3f; s.dynamicRange = 0.3f; s.repetition = 0.7f; s.harmonicRhythm = 2;
        s.generalPause = false; s.allowKeyChange = false;
        s.groove = 1; s.grooveAmount = 0.75f;
        inst("Gitarre", "E-Piano", "Kontrabass");
        break;
    case 12:  // Märchen
        s.scale = either(ScaleType::Lydian, ScaleType::Major, 0.6f); bpm = 96;
        s.melodyDensity = 0.5f; s.syncopation = 0.15f; s.energy = 0.4f; s.creativity = 0.45f;
        s.withDrums = false; s.accompaniment = AccompStyle::Arpeggio;
        s.colorChords = 0.4f; s.jazzChords = 0.15f; s.ambitus = 0.65f; s.leaps = 0.55f; s.dynamicRange = 0.6f;
        inst("Celesta", "Harfe", "Pizzicato");
        break;
    case 13:  // Heldenhaft (Marsch)
        s.scale = int(ScaleType::Major); bpm = 112;
        s.melodyDensity = 0.5f; s.syncopation = 0.2f; s.energy = 0.85f; s.creativity = 0.35f;
        s.ambitus = 0.8f; s.leaps = 0.7f; s.repetition = 0.6f; s.dynamicRange = 0.7f; s.colorChords = 0.3f; s.jazzChords = 0.f;
        orchestra(3);
        break;
    case 14:  // Walzer
        s.scale = either(ScaleType::Major, ScaleType::HarmonicMinor, 0.6f); bpm = 156; meter = 1;
        s.melodyDensity = 0.5f; s.syncopation = 0.05f; s.energy = 0.55f; s.creativity = 0.35f;
        s.colorChords = 0.35f; s.jazzChords = 0.2f; s.dynamicRange = 0.65f; s.ambitus = 0.65f;
        orchestra(4);
        break;
    case 15:  // Weltraum (Ambient)
        s.scale = int(ScaleType::Lydian); bpm = 70;
        s.melodyDensity = 0.25f; s.syncopation = 0.05f; s.energy = 0.3f; s.creativity = 0.5f;
        s.withDrums = false; s.accompaniment = AccompStyle::Pad; s.harmonicRhythm = 1;
        s.ambitus = 0.7f; s.leaps = 0.6f; s.repetition = 0.6f; s.colorChords = 0.5f; s.jazzChords = 0.4f;
        s.dynamicRange = 0.6f; s.generalPause = false; s.humanize = 0.4f; s.allowKeyChange = false;
        inst("Glocken", "Pad", "Synth-Bass");
        break;
    case 16:  // Weihnachtlich
        s.scale = int(ScaleType::Major); bpm = 104;
        s.melodyDensity = 0.55f; s.syncopation = 0.15f; s.energy = 0.5f; s.creativity = 0.3f;
        s.drums = DrumStyle::Ballad; s.swing = 0.3f;
        s.jazzChords = 0.35f; s.colorChords = 0.4f; s.repetition = 0.7f; s.ambitus = 0.55f;
        inst("Celesta", "Piano", "Kontrabass");
        break;
    case 17:  // Barock / Mittelalter
        s.scale = either(ScaleType::Dorian, ScaleType::NaturalMinor, 0.5f); bpm = 100;
        s.melodyDensity = 0.6f; s.syncopation = 0.1f; s.energy = 0.55f; s.creativity = 0.3f;
        s.harmonicRhythm = 2; s.jazzChords = 0.f; s.colorChords = 0.1f; s.leaps = 0.4f;
        s.dynamicRange = 0.35f; s.generalPause = false; s.allowKeyChange = false;
        orchestra(6);
        break;
    case 18:  // Western
        s.scale = either(ScaleType::Mixolydian, ScaleType::NaturalMinor, 0.5f); bpm = 100;
        s.melodyDensity = 0.45f; s.syncopation = 0.25f; s.energy = 0.6f; s.creativity = 0.4f;
        s.drums = DrumStyle::Pop; s.swing = 0.25f; s.accompaniment = AccompStyle::Rhythm; s.bass = BassStyle::RootFifth;
        s.colorChords = 0.45f; s.jazzChords = 0.f; s.leaps = 0.65f; s.ambitus = 0.75f;
        s.solo = true; s.soloInstrument = "Gitarre";
        inst("Mundharmonika", "Gitarre", "Kontrabass");
        break;
    case 19:  // Sommer-Pop
        s.scale = either(ScaleType::Major, ScaleType::MajorPentatonic, 0.7f); bpm = 108;
        s.melodyDensity = 0.6f; s.syncopation = 0.55f; s.energy = 0.7f; s.creativity = 0.3f;
        s.drums = DrumStyle::Pop; s.accompaniment = AccompStyle::Rhythm; s.harmonicRhythm = 2;
        s.repetition = 0.75f; s.ambitus = 0.45f; s.leaps = 0.4f; s.jazzChords = 0.2f; s.colorChords = 0.15f;
        s.groove = 2; s.grooveAmount = 0.35f;
        inst("Marimba", "Gitarre", "Synth-Bass");
        break;
    case 20:  // Rock
        s.scale = either(ScaleType::NaturalMinor, ScaleType::Mixolydian, 0.6f); bpm = 128;
        s.melodyDensity = 0.5f; s.syncopation = 0.35f; s.energy = 0.88f; s.creativity = 0.3f;
        s.drums = DrumStyle::Rock; s.accompaniment = AccompStyle::Rhythm; s.bass = BassStyle::Eighths;
        s.repetition = 0.7f; s.leaps = 0.5f; s.dynamicRange = 0.55f; s.jazzChords = 0.f; s.colorChords = 0.25f;
        s.solo = true; s.soloInstrument = "E-Gitarre"; s.groove = 2; s.grooveAmount = 0.3f;
        inst("Lead", "E-Gitarre", "Synth-Bass");
        break;
    case 21:  // Paris (Musette)
        s.scale = either(ScaleType::HarmonicMinor, ScaleType::Major, 0.55f); bpm = 168; meter = 1;
        s.melodyDensity = 0.6f; s.syncopation = 0.1f; s.energy = 0.55f; s.creativity = 0.35f;
        s.withDrums = false; s.accompaniment = AccompStyle::Broken; s.bass = BassStyle::RootFifth;
        s.jazzChords = 0.3f; s.colorChords = 0.35f; s.leaps = 0.45f; s.repetition = 0.6f; s.dynamicRange = 0.5f;
        s.swing = 0.2f;
        inst("Akkordeon", "Gitarre", "Kontrabass");
        break;
    case 22:  // Karibik
        s.scale = either(ScaleType::Major, ScaleType::MajorPentatonic, 0.6f); bpm = 104;
        s.melodyDensity = 0.55f; s.syncopation = 0.6f; s.energy = 0.62f; s.creativity = 0.3f;
        s.drums = DrumStyle::Pop; s.accompaniment = AccompStyle::Rhythm; s.bass = BassStyle::Syncopated;
        s.repetition = 0.72f; s.ambitus = 0.5f; s.leaps = 0.4f; s.jazzChords = 0.15f; s.colorChords = 0.2f;
        s.groove = 1; s.grooveAmount = 0.4f; s.solo = coin(0.4f); s.soloInstrument = "Marimba";
        inst("Steel Drum", "Ukulele", "Synth-Bass");
        break;
    case 23:  // Bluegrass
        s.scale = either(ScaleType::Major, ScaleType::Mixolydian, 0.65f); bpm = 150;
        s.melodyDensity = 0.75f; s.syncopation = 0.2f; s.energy = 0.72f; s.creativity = 0.3f;
        s.drums = DrumStyle::Pop; s.accompaniment = AccompStyle::Rhythm; s.bass = BassStyle::RootFifth;
        s.repetition = 0.65f; s.ambitus = 0.6f; s.leaps = 0.45f; s.jazzChords = 0.f; s.colorChords = 0.2f;
        s.solo = true; s.soloInstrument = "Mandoline"; s.groove = 2; s.grooveAmount = 0.35f;
        inst("Banjo", "Gitarre", "Kontrabass");
        break;
    case 24:  // Kirche / Feierlich
        s.scale = either(ScaleType::Major, ScaleType::NaturalMinor, 0.6f); bpm = 66;
        s.melodyDensity = 0.35f; s.syncopation = 0.f; s.energy = 0.5f; s.creativity = 0.2f;
        s.withDrums = false; s.accompaniment = AccompStyle::Pad; s.bass = BassStyle::Whole; s.harmonicRhythm = 2;
        s.repetition = 0.6f; s.ambitus = 0.55f; s.leaps = 0.3f; s.jazzChords = 0.f; s.colorChords = 0.1f;
        s.dynamicRange = 0.6f; s.humanize = 0.15f;
        s.solo = coin(0.3f); s.soloInstrument = "Trompeten";
        inst("Kirchenorgel", "Chor", "Kirchenorgel");
        break;
    case 25:  // Anden
        s.scale = either(ScaleType::MinorPentatonic, ScaleType::NaturalMinor, 0.55f); bpm = 96;
        s.melodyDensity = 0.5f; s.syncopation = 0.3f; s.energy = 0.55f; s.creativity = 0.35f;
        s.drums = DrumStyle::Ballad; s.accompaniment = AccompStyle::Rhythm; s.bass = BassStyle::RootFifth;
        s.repetition = 0.6f; s.ambitus = 0.7f; s.leaps = 0.55f; s.jazzChords = 0.f; s.colorChords = 0.2f;
        s.solo = coin(0.4f); s.soloInstrument = "Mandoline";
        inst("Panflöte", "Gitarre", "Kontrabass");
        break;
    case 26:  // Traumland
        s.scale = either(ScaleType::Major, ScaleType::Lydian, 0.6f); bpm = 78;
        s.melodyDensity = 0.4f; s.syncopation = 0.3f; s.energy = 0.38f; s.creativity = 0.35f;
        s.drums = DrumStyle::Ballad; s.accompaniment = AccompStyle::Broken; s.harmonicRhythm = 1;
        s.repetition = 0.7f; s.ambitus = 0.45f; s.leaps = 0.4f; s.jazzChords = 0.35f; s.colorChords = 0.35f;
        s.groove = 1; s.grooveAmount = 0.55f; s.swing = 0.25f; s.humanize = 0.4f;
        inst("Kalimba", "Harfe", "Kontrabass");
        break;
    default: {  // Zufall
        auto pick = [&](std::initializer_list<const char*> l) {
            return *(l.begin() + std::uniform_int_distribution<int>(0, int(l.size()) - 1)(rng));
        };
        s.key = -1; s.scale = -1; randomBpm = true;
        s.melodyDensity = f(0.25f, 0.7f); s.syncopation = f(0.05f, 0.6f); s.energy = f(0.3f, 0.85f);
        s.creativity = f(0.15f, 0.6f);
        s.swing = coin(0.25f) ? f(0.3f, 0.7f) : 0.f;
        s.ambitus = f(0.3f, 0.8f); s.leaps = f(0.3f, 0.7f); s.repetition = f(0.35f, 0.8f);
        s.harmonicRhythm = coin(0.5f) ? 0 : int(rng() % 3u) + 1;
        s.jazzChords = f(0.f, 0.7f); s.colorChords = f(0.f, 0.5f); s.dynamicRange = f(0.3f, 0.9f);
        s.useIntro = coin(0.9f); s.usePre = coin(0.6f); s.useBridge = coin(0.8f); s.useOutro = coin(0.9f);
        inst(pick({"Piano", "E-Piano", "Flöte", "Lead", "Glocken", "Marimba", "Gitarre", "Streicher", "Saxofon", "Celesta",
                   "Vibraphon", "Xylophon", "Mundharmonika", "Akkordeon", "Steel Drum", "Kalimba", "Panflöte", "Banjo", "Mandoline"}),
             pick({"Piano", "E-Piano", "Streicher", "Pad", "Orgel", "Gitarre", "Harfe", "Chor", "Cembalo", "Akkordeon", "E-Gitarre", "Ukulele", "Kirchenorgel"}),
             pick({"Synth-Bass", "Kontrabass", "Piano"}));
        s.withDrums = coin(0.85f);
        s.solo = coin(0.3f); s.soloInstrument = pick({"Lead", "Gitarre", "E-Piano", "Flöte", "Orgel", "Saxofon", "Tenorsaxofon", "E-Gitarre", "Vibraphon", "Mandoline", "Steel Drum"});
        s.groove = int(rng() % 3u); s.grooveAmount = f(0.3f, 0.8f);
        if (coin(0.25f)) orchestra(-1);
        break;
    }
    }
}

// ---------------------------------------------------------------------
//  Beschreibung in Worten -> Einstellungen (wie ein "Prompt" bei Suno/Soundraw, aber regelbasiert)
// ---------------------------------------------------------------------
std::string GeneratorPanel::applyPromptTo(GeneratorSettings& s, const std::string& text, std::mt19937& rng,
                                          bool& randomBpm, float& bpm, int& meter, int* presetOut) {
    // Kleinschreibung (inkl. Umlaute)
    std::string t;
    for (char c : text) t += char(std::tolower(static_cast<unsigned char>(c)));
    auto replaceAll = [&](const std::string& a, const std::string& b) {
        for (size_t p = t.find(a); p != std::string::npos; p = t.find(a, p + b.size())) t.replace(p, a.size(), b);
    };
    replaceAll("\xC3\x84", "\xC3\xA4");   // Ä -> ä
    replaceAll("\xC3\x96", "\xC3\xB6");   // Ö -> ö
    replaceAll("\xC3\x9C", "\xC3\xBC");   // Ü -> ü
    auto has = [&](const char* w) { return t.find(w) != std::string::npos; };
    auto word = [&](const char* w) {   // ganzes Wort (nicht "dur" in "durch")
        const std::string sw(w);
        for (size_t p = t.find(sw); p != std::string::npos; p = t.find(sw, p + 1)) {
            const bool leftOk = p == 0 || !(std::isalpha(static_cast<unsigned char>(t[p - 1])) || (static_cast<unsigned char>(t[p - 1]) >= 0x80));
            const size_t e = p + sw.size();
            const bool rightOk = e >= t.size() || !(std::isalpha(static_cast<unsigned char>(t[e])) || (static_cast<unsigned char>(t[e]) >= 0x80));
            if (leftOk && rightOk) return true;
        }
        return false;
    };
    std::vector<std::string> found;

    // 1. Stimmung: das zuerst genannte Stimmungswort wählt die Vorlage
    static const std::pair<const char*, int> moods[] = {
        {"fröhlich", 0}, {"happy", 0}, {"lustig", 0}, {"heiter", 0}, {"gute laune", 0},
        {"traurig", 1}, {"trauer", 1}, {"sad", 1},
        {"episch", 2}, {"epic", 2}, {"gewaltig", 2}, {"bombast", 2},
        {"ruhig", 3}, {"calm", 3}, {"friedlich", 3},
        {"jazz", 4},
        {"filmmusik", 5}, {"soundtrack", 5},
        {"melanchol", 6}, {"nachdenklich", 6}, {"wehmut", 6}, {"wehmüt", 6},
        {"romant", 7}, {"liebe", 7}, {"love", 7}, {"zärtlich", 7},
        {"abenteuer", 8}, {"adventure", 8}, {"reise", 8}, {"entdeck", 8},
        {"spannung", 9}, {"spannend", 9}, {"thriller", 9}, {"horror", 9}, {"grusel", 9}, {"düster", 9}, {"dunkel", 9},
        {"mysteri", 9}, {"bedrohlich", 9},
        {"tanz", 10}, {"dance", 10}, {"party", 10}, {"club", 10}, {"disco", 10}, {"techno", 10},
        {"lofi", 11}, {"lo-fi", 11}, {"chill", 11}, {"lernen", 11}, {"entspann", 11},
        {"märchen", 12}, {"fantasy", 12}, {"magisch", 12}, {"verzaubert", 12},
        {"held", 13}, {"marsch", 13}, {"heroisch", 13}, {"sieg", 13}, {"triumph", 13},
        {"walzer", 14}, {"waltz", 14},
        {"weltraum", 15}, {"space", 15}, {"ambient", 15}, {"schweb", 15}, {"sterne", 15}, {"galax", 15},
        {"weihnacht", 16}, {"christmas", 16}, {"winter", 16},
        {"barock", 17}, {"mittelalter", 17}, {"ritter", 17}, {"klassisch", 17},
        {"western", 18}, {"cowboy", 18}, {"wüste", 18},
        {"sommer", 19}, {"strand", 19}, {"sonnig", 19}, {"urlaub", 19},
        {"rock", 20}, {"metal", 20}, {"punk", 20},
        {"paris", 21}, {"musette", 21}, {"café", 21}, {"cafe", 21}, {"chanson", 21},
        {"karibik", 22}, {"reggae", 22}, {"insel", 22}, {"calypso", 22}, {"hawaii", 22}, {"latin", 22}, {"salsa", 22},
        {"bluegrass", 23}, {"country", 23}, {"hillbilly", 23},
        {"kirche", 24}, {"feierlich", 24}, {"hymne", 24}, {"erhaben", 24}, {"gottesdienst", 24},
        {"anden", 25}, {"peru", 25}, {"inka", 25}, {"sehnsucht", 25},
        {"traum", 26}, {"verträumt", 26}, {"einschlaf", 26}, {"spieluhr", 26}, {"gute nacht", 26},
    };
    int preset = -1;
    size_t bestPos = std::string::npos;
    for (auto& [w, p] : moods) {
        const size_t pos = t.find(w);
        if (pos != std::string::npos && pos < bestPos) { bestPos = pos; preset = p; }
    }
    const bool wantOrch = has("orchester") || has("sinfoni") || has("symphon");
    if (preset < 0 && wantOrch) preset = 5;
    if (preset >= 0) {
        applyPresetTo(s, preset, rng, randomBpm, bpm, meter);
        found.push_back(std::string("Stimmung ") + presetName(preset));
    }
    if (presetOut) *presetOut = preset;

    // 2. Besetzung
    if (wantOrch) { s.ensemble = Ensemble::Orchestra; found.push_back("Orchester"); }
    else if (word("band") || word("pop")) { s.ensemble = Ensemble::Band; found.push_back("Band"); }

    // 3. Tempo
    std::smatch m;
    if (std::regex_search(t, m, std::regex("(\\d{2,3})\\s*bpm"))) {
        bpm = std::clamp(float(std::stoi(m[1].str())), 40.f, 220.f);
        randomBpm = false;
        found.push_back(std::to_string(int(bpm)) + " BPM");
    } else if (has("sehr langsam")) { bpm = 60; randomBpm = false; found.push_back("sehr langsam"); }
    else if (has("langsam") || word("slow")) { bpm = std::clamp(randomBpm ? 72.f : bpm * 0.78f, 50.f, 180.f); randomBpm = false; found.push_back("langsam"); }
    else if (has("sehr schnell")) { bpm = 160; randomBpm = false; found.push_back("sehr schnell"); }
    else if (has("schnell") || has("flott") || has("rasant") || word("fast")) {
        bpm = std::clamp(randomBpm ? 140.f : bpm * 1.25f, 50.f, 180.f); randomBpm = false; found.push_back("schnell");
    }

    // 4. Tonart ("in d-moll", "Fis-Dur", "a moll")
    if (std::regex_search(t, m, std::regex("(^|[^a-z])([a-h])(is|es|s|#|b)?[- ]?(moll|dur)($|[^a-z])"))) {
        static const int base[] = {9, 10, 0, 2, 4, 5, 7, 11};   // a b c d e f g h (deutsch: b = Bb)
        const char letter = m[2].str()[0];
        int k = base[letter - 'a'];
        const std::string acc = m[3].str();
        if (acc == "is" || acc == "#") ++k;
        else if (acc == "es" || acc == "s" || (acc == "b" && letter != 'b')) --k;
        s.key = (k + 12) % 12;
        s.scale = m[4].str() == "moll" ? int(ScaleType::NaturalMinor) : int(ScaleType::Major);
        found.push_back(std::string("Tonart ") + noteName(s.key) + (m[4].str() == "moll" ? " Moll" : " Dur"));
    } else if (word("moll") || word("minor")) { s.scale = int(ScaleType::NaturalMinor); found.push_back("Moll"); }
    else if (word("dur") || word("major")) { s.scale = int(ScaleType::Major); found.push_back("Dur"); }
    if (has("dorisch")) { s.scale = int(ScaleType::Dorian); found.push_back("Dorisch"); }
    if (has("lydisch")) { s.scale = int(ScaleType::Lydian); found.push_back("Lydisch"); }
    if (has("pentaton")) {
        s.scale = (s.scale >= 0 && scaleIsMinor(ScaleType(s.scale))) ? int(ScaleType::MinorPentatonic) : int(ScaleType::MajorPentatonic);
        found.push_back("Pentatonik");
    }

    // 5. Länge
    if (std::regex_search(t, m, std::regex("(\\d+)\\s*(min|minute)"))) {
        s.lengthSeconds = std::clamp(std::stod(m[1].str()) * 60.0, 20.0, 600.0);
        found.push_back(std::to_string(int(s.lengthSeconds)) + " s");
    } else if (std::regex_search(t, m, std::regex("(\\d+)\\s*(s|sek)([^a-z]|$)"))) {
        s.lengthSeconds = std::clamp(std::stod(m[1].str()), 20.0, 600.0);
        found.push_back(std::to_string(int(s.lengthSeconds)) + " s");
    } else if (word("kurz") || word("kurzer") || word("kurzes")) { s.lengthSeconds = 60; found.push_back("kurz"); }
    else if (word("lang") || word("langer") || word("langes") || word("lange")) { s.lengthSeconds = 220; found.push_back("lang"); }

    // 6. Takt, Groove, Swing
    if (has("3/4") || has("dreiviertel")) { meter = 1; found.push_back("3/4-Takt"); }
    if (has("swing") || has("shuffle")) { s.swing = 0.6f; found.push_back("Swing"); }
    if (has("laid back") || has("laid-back") || has("lässig") || has("groovig")) { s.groove = 1; s.grooveAmount = 0.6f; found.push_back("Groove laid-back"); }
    if (has("treibend") || has("drive") || has("energisch")) { s.groove = 2; s.grooveAmount = 0.6f; found.push_back("Groove treibend"); }

    // 7. Energie / Charakter
    if (word("leise") || has("zart") || has("sanft") || word("soft")) { s.energy = std::max(0.f, s.energy - 0.2f); found.push_back("leise"); }
    if (word("laut") || has("kraftvoll") || has("wuchtig") || has("power") || has("mächtig")) { s.energy = std::min(1.f, s.energy + 0.2f); found.push_back("kraftvoll"); }
    if (has("experiment") || has("verrückt") || has("mutig") || has("ungewöhnlich")) { s.creativity = 0.75f; found.push_back("mutig"); }
    if (has("einfach") || has("simpel") || has("schlicht")) { s.creativity = 0.15f; s.jazzChords = 0.f; found.push_back("einfach"); }
    if (has("ohrwurm") || has("eingängig") || has("catchy")) { s.repetition = 0.85f; found.push_back("eingängig"); }

    // 8. Bausteine
    const bool noDrums = has("ohne schlagzeug") || has("ohne drums") || has("ohne beat");
    if (noDrums) { s.withDrums = false; found.push_back("ohne Schlagzeug"); }
    else if (has("mit schlagzeug") || has("mit drums") || has("mit beat")) s.withDrums = true;
    if (has("ohne solo")) s.solo = false;
    else if (word("solo") || has("improvis")) { s.solo = true; found.push_back("mit Solo"); }

    // 9. Instrumente (erstes = Melodie, zweites = Begleitung) – nur Band
    if (s.ensemble == Ensemble::Band) {
        static const std::pair<const char*, const char*> insts[] = {
            {"e-gitarre", "E-Gitarre"}, {"verzerrt", "E-Gitarre"}, {"tenorsax", "Tenorsaxofon"}, {"saxofon", "Saxofon"},
            {"saxophon", "Saxofon"}, {"sax", "Saxofon"}, {"mundharmonika", "Mundharmonika"}, {"akkordeon", "Akkordeon"},
            {"cembalo", "Cembalo"}, {"celesta", "Celesta"}, {"xylophon", "Xylophon"}, {"vibraphon", "Vibraphon"},
            {"röhrenglocke", "Röhrenglocken"}, {"piccolo", "Piccolo"}, {"tuba", "Tuba"},
            {"kirchenorgel", "Kirchenorgel"}, {"pfeifenorgel", "Kirchenorgel"}, {"panflöte", "Panflöte"}, {"panfloete", "Panflöte"},
            {"ukulele", "Ukulele"}, {"mandoline", "Mandoline"}, {"banjo", "Banjo"}, {"steel drum", "Steel Drum"},
            {"steeldrum", "Steel Drum"}, {"steelpan", "Steel Drum"}, {"kalimba", "Kalimba"}, {"daumenklavier", "Kalimba"},
            {"e-piano", "E-Piano"}, {"rhodes", "E-Piano"}, {"glockenspiel", "Glocken"}, {"klavier", "Piano"}, {"piano", "Piano"},
            {"gitarre", "Gitarre"}, {"guitar", "Gitarre"}, {"flöte", "Flöte"}, {"glocke", "Glocken"}, {"marimba", "Marimba"},
            {"orgel", "Orgel"}, {"streicher", "Streicher"}, {"geige", "Streicher"}, {"chor", "Chor"}, {"synth", "Lead"},
            {"harfe", "Harfe"},
        };
        std::vector<std::pair<size_t, const char*>> hits;
        std::string masked = t;
        for (auto& [w, name] : insts) {
            const size_t pos = masked.find(w);
            if (pos == std::string::npos) continue;
            bool dup = false;
            for (auto& h : hits) dup = dup || std::string(h.second) == name;
            if (!dup) hits.push_back({pos, name});
            masked.replace(pos, std::strlen(w), std::string(std::strlen(w), '#'));   // "e-piano" nicht nochmal als "piano"
        }
        std::sort(hits.begin(), hits.end());
        if (!hits.empty()) { s.melodyInstrument = hits[0].second; found.push_back(std::string("Melodie: ") + hits[0].second); }
        if (hits.size() > 1) { s.chordInstrument = hits[1].second; found.push_back(std::string("Begleitung: ") + hits[1].second); }
    }

    if (found.empty()) return "Nichts erkannt - probier z.B. \"episch, langsam, Orchester, in d-moll\".";
    std::string out = "Erkannt: ";
    for (size_t i = 0; i < found.size(); ++i) out += (i ? ", " : "") + found[i];
    return out;
}

void GeneratorPanel::draw(const Song* current) {
    if (!open) return;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize({430, disp.y - 110}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({300, 200}, {800, std::max(200.f, disp.y - 100)});
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 450, 90}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Song-Generator", &open)) { ImGui::End(); return; }
    auto& s = settings;
    const float w = 230.f;

    ImGui::SeparatorText("Stimmung (Vorlage)");
    {
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float avail = ImGui::GetContentRegionAvail().x;
        const int cols = std::max(2, int((avail + sp) / (105.f + sp)));
        const float bw = (avail - sp * float(cols - 1)) / float(cols);
        for (int i = 0; i < kPresetCount; ++i) {
            if (i % cols) ImGui::SameLine();
            const bool active = i == lastPreset_;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(kPresets[i].name, {bw, 0})) applyPreset(i);
            if (active) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", kPresets[i].hint);
        }
    }
    // Beschreibung in eigenen Worten
    {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 95);
        const bool enter = ImGui::InputTextWithHint("##prompt", "Beschreibung, z.B. episch, langsam, Orchester, in d-moll",
                                                    &prompt_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Übernehmen", {90, 0}) || enter) && !prompt_.empty()) {
            int p = -1;
            promptFeedback_ = applyPromptTo(settings, prompt_, rng_, randomBpm_, bpmValue_, meterIndex_, &p);
            if (p >= 0) lastPreset_ = p;
        }
        ImGui::SetItemTooltip("Versteht u.a. Stimmungen (episch, traurig, Weltraum ...), Tempo (langsam, 90 bpm),\n"
                              "Tonart (in d-moll, Fis-Dur), Länge (3 min, kurz), Besetzung (Orchester, Band),\n"
                              "Instrumente (Klavier, Gitarre, Flöte ...), ohne Schlagzeug, mit Solo, Swing,\n"
                              "laid-back, treibend, leise/kraftvoll, eingängig, einfach/mutig, 3/4.");
        if (!promptFeedback_.empty()) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("%s", promptFeedback_.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    int ens = int(s.ensemble);
    ImGui::SetNextItemWidth(w);
    if (ImGui::Combo("Besetzung", &ens, "Band\0Sinfonieorchester\0")) s.ensemble = Ensemble(ens);
    if (s.ensemble == Ensemble::Orchestra) {
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("Orchester-Stil", orchStyleName(s.orchStyle))) {
            if (ImGui::Selectable("Zufall", s.orchStyle < 0)) s.orchStyle = -1;
            for (int i = 0; i < orchStyleCount(); ++i)
                if (ImGui::Selectable(orchStyleName(i), s.orchStyle == i)) s.orchStyle = i;
            ImGui::EndCombo();
        }
        helpMarker("Jeder Stil hat eigene Lieblings-Techniken (Ostinati, Pizzicato, Tremolo, Kanon, Marsch-Trommel ...).\n"
                   "Pro Abschnitt wird daraus zufällig, aber passend zur Energie gewählt.");
    }
    helpMarker("Sinfonieorchester: Holzbläser, Hörner, Trompeten, Posaunen, Pauken, Harfe und Streicher.\n"
               "Die Melodie wandert je nach Abschnitt durch die Instrumente, der Refrain wird zum Tutti.");

    // ---- Endlos-Modus ----
    ImGui::SeparatorText("Endlos-Modus (Radio)");
    {
        ImGui::PushStyleColor(ImGuiCol_Button, endless ? ImVec4(0.55f, 0.35f, 0.15f, 1) : ImVec4(0.22f, 0.30f, 0.45f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, endless ? ImVec4(0.68f, 0.43f, 0.18f, 1) : ImVec4(0.28f, 0.38f, 0.58f, 1));
        if (ImGui::Button(endless ? "Endlos-Modus beenden" : "Endlos spielen", {-1, 30})) endless = !endless;
        ImGui::PopStyleColor(2);
        ImGui::Checkbox("Stimmung wechseln", &endlessVary);
        helpMarker("Aus: jeder Song nutzt genau deine Einstellungen (nur der Seed ist neu).\n"
                   "An: für jeden Song wird eine andere Stimmungsvorlage gewürfelt.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::SliderFloat("Pause", &endlessPause, 0.f, 5.f, "%.1f s");
        if (endless) {
            if (!endlessMood.empty()) ImGui::Text("Song %d läuft:  %s", endlessSongs, endlessMood.c_str());
            if (running_) ImGui::TextDisabled("Nächster Song wird komponiert (%s) ...", endlessNextMood.c_str());
            else if (endlessSongs > 0) ImGui::TextDisabled("Nächster Song ist bereit (%s).", endlessNextMood.c_str());
        } else {
            ImGui::TextWrapped("Spielt Song für Song ohne Ende - der nächste wird komponiert, während der aktuelle läuft.");
        }
    }

    ImGui::SeparatorText("Grundlagen");
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Tonart", s.key < 0 ? "Zufall" : noteName(s.key))) {
        if (ImGui::Selectable("Zufall", s.key < 0)) s.key = -1;
        for (int k = 0; k < 12; ++k)
            if (ImGui::Selectable(noteName(k), s.key == k)) s.key = k;
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Skala", s.scale < 0 ? "Zufall" : scaleName(ScaleType(s.scale)))) {
        if (ImGui::Selectable("Zufall", s.scale < 0)) s.scale = -1;
        for (int k = 0; k < int(ScaleType::Count); ++k)
            if (ImGui::Selectable(scaleName(ScaleType(k)), s.scale == k)) s.scale = k;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Tempo zufällig", &randomBpm_);
    if (!randomBpm_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w - 120);
        ImGui::SliderFloat("BPM##gen", &bpmValue_, 50.f, 180.f, "%.0f");
    }
    ImGui::SetNextItemWidth(w);
    ImGui::Combo("Taktart", &meterIndex_, "4/4\0" "3/4\0");
    ImGui::SetNextItemWidth(w);
    float len = float(s.lengthSeconds);
    if (ImGui::SliderFloat("Länge", &len, 20.f, 300.f, "%.0f s")) s.lengthSeconds = len;
    ImGui::BeginDisabled(!s.structure.empty());
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Songform", songFormName(s.form), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(songFormName(-1), s.form < 0)) s.form = -1;
        ImGui::SetItemTooltip("%s", songFormHint(-1));
        for (int i = 0; i < songFormCount(); ++i) {
            if (ImGui::Selectable(songFormName(i), s.form == i)) s.form = i;
            ImGui::SetItemTooltip("%s", songFormHint(i));
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    helpMarker(s.structure.empty() ? songFormHint(s.form) : "Ausgeschaltet, weil unten eine eigene Form eingetragen ist.");
    ImGui::SetNextItemWidth(w);
    ImGui::InputTextWithHint("Eigene Form", "leer = Songform oben", &s.structure);
    helpMarker("Abschnitte mit Taktzahl, z.B.  I4 V8 C8 V8 C8 B8 C8 O4\n"
               "I = Intro (mitten im Song = Zwischenspiel), V/S = Strophe, P = Pre-Refrain, C/R = Refrain,\n"
               "B = Bridge, O = Outro, L = Instrumental-Solo.\n"
               "Dahinter:  + / - = mehr/weniger Energie,  ~ = Breakdown (abgespeckt),  > = ausblenden.\n"
               "Beispiel:  C8 V8 C8 B8~ C8++ O4>");
    if (s.structure.empty()) {
        ImGui::TextDisabled("Bausteine:");
        ImGui::SameLine(); ImGui::Checkbox("Intro", &s.useIntro);
        ImGui::SameLine(); ImGui::Checkbox("Pre", &s.usePre);
        ImGui::SameLine(); ImGui::Checkbox("Bridge", &s.useBridge);
        ImGui::SameLine(); ImGui::Checkbox("Outro", &s.useOutro);
        helpMarker("Welche Abschnitte die automatische Form verwenden darf (Pre = Pre-Refrain/Aufbau vor dem Refrain).");
    }

    ImGui::SeparatorText("Charakter");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("Kreativität", &s.creativity, 0.f, 1.f, "%.2f");
    helpMarker("0 = brav und vorhersehbar, 1 = mutig: mehr Sprünge, Fremdakkorde, Septimen, Variation.");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("Melodie-Dichte", &s.melodyDensity, 0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("Synkopen", &s.syncopation, 0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("Energie", &s.energy, 0.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("Menschlich", &s.humanize, 0.f, 1.f, "%.2f");
    helpMarker("Kleine Schwankungen in Timing und Anschlag, damit es nicht maschinell klingt.");
    ImGui::SetNextItemWidth(w);
    int oct = s.melodyOctaveShift;
    if (ImGui::SliderInt("Melodie-Oktave", &oct, -1, 1)) s.melodyOctaveShift = oct;

    ImGui::SeparatorText("Kompositions-Systeme");
    ImGui::Checkbox("Systeme mischen", &s.mixSystems);
    helpMarker("Jeder Song würfelt sich eine eigene Mischung aus erprobten Bauweisen, jeder Abschnitt nimmt sich daraus eine:\n"
               "Harmonie: Pop-Loop, Quintfall, Modaler Vamp, Orgelpunkt, Andalusische Kadenz, Line Cliché,\n"
               "          chromatische Mediante, Plagal (Gospel), Kanon-Folge ...\n"
               "Rhythmus (Band): Tresillo 3-3-2, Clave, Offbeat (Reggae), Bossa, treibende Achtel\n"
               "Melodie: Pentatonik-Hook, Riff, Arpeggio-Thema, Choral\n"
               "Welche gewählt wurden, steht unten bei \"Letzter Song\".");
    ImGui::BeginDisabled(!s.mixSystems);
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("Vielfalt##sys", &s.systemVariety, 0.f, 1.f, "%.2f");
    helpMarker("0 = meist klassisch, wenige Systeme\n1 = viele verschiedene Systeme, fast jeder Abschnitt anders");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Feinheiten");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("Swing", &s.swing, 0.f, 1.f, "%.2f");
    helpMarker("0 = gerade Achtel, 1 = volles Shuffle (Triolen-Gefühl wie im Jazz/Blues).");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("Tonumfang", &s.ambitus, 0.f, 1.f, "%.2f");
    helpMarker("Wie weit die Melodie nach oben und unten ausgreifen darf.");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("Sprungfreude", &s.leaps, 0.f, 1.f, "%.2f");
    helpMarker("0 = fast nur Tonschritte (sanglich), 1 = viele große Intervalle (heldenhaft, kantig).");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("Wiedererkennung", &s.repetition, 0.f, 1.f, "%.2f");
    helpMarker("0 = Motive werden ständig neu variiert, 1 = Motive kehren fast gleich wieder (Ohrwurm).");
    ImGui::SetNextItemWidth(w);
    ImGui::Combo("Akkordwechsel", &s.harmonicRhythm, "automatisch\0langsam (2 Takte)\0normal (1 Takt)\0schnell (1/2 Takt)\0");
    auto autoSlider = [&](const char* label, const char* id, float& v, const char* help) {
        bool automatic = v < 0.f;
        if (ImGui::Checkbox(id, &automatic)) v = automatic ? -1.f : std::clamp(s.creativity * 0.5f, 0.f, 1.f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(60.f, w - ImGui::GetItemRectSize().x - ImGui::GetStyle().ItemSpacing.x));
        if (automatic) {
            ImGui::BeginDisabled();
            float shown = s.creativity * 0.5f;
            ImGui::SliderFloat(label, &shown, 0.f, 1.f, "aus Kreativität");
            ImGui::EndDisabled();
        } else {
            ImGui::SliderFloat(label, &v, 0.f, 1.f, "%.2f");
        }
        helpMarker(help);
    };
    autoSlider("Jazz-Harmonien", "auto##jazz", s.jazzChords,
               "Anteil der Septakkorde (Cmaj7, Am7, G7 ...). Haken = aus der Kreativität ableiten.");
    autoSlider("Farbakkorde", "auto##color", s.colorChords,
               "Geliehene Akkorde (iv, bVI, bVII) und Zwischendominanten - sorgen für Überraschung.");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderFloat("Dynamik-Umfang", &s.dynamicRange, 0.f, 1.f, "%.2f");
    helpMarker("0 = alles etwa gleich laut, 1 = sehr leise Strophen/Bridge bis zum gewaltigen Schluss-Refrain.");
    ImGui::Checkbox("Generalpause vor dem letzten Refrain", &s.generalPause);
    helpMarker("Bei kraftvoller Musik: ein Schlag Stille, dann setzt der letzte Refrain mit voller Wucht ein.");
    ImGui::SetNextItemWidth(w - 110);
    ImGui::Combo("##groove", &s.groove, "Groove: präzise\0Groove: laid-back\0Groove: treibend\0");
    ImGui::SameLine();
    ImGui::BeginDisabled(s.groove == 0);
    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat("Stärke##groove", &s.grooveAmount, 0.f, 1.f, "%.2f");
    ImGui::EndDisabled();
    helpMarker("Mikrotiming wie bei echten Musikern (Band-Besetzung):\n"
               "laid-back = Snare und Bass minimal hinter dem Schlag (entspannt, Soul/Lofi),\n"
               "treibend = Hi-Hats leicht vor dem Schlag (nach vorne, Pop/Dance).\n"
               "Dazu Betonungen auf den Hi-Hats.");
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Raumklang", roomPresetName(s.room))) {
        for (int i = 0; i < roomPresetCount(); ++i)
            if (ImGui::Selectable(roomPresetName(i), s.room == i)) s.room = i;
        ImGui::EndCombo();
    }
    helpMarker("Hall & Echo für den neuen Song. \"Passend zur Musik\": Orchester im Konzertsaal oder in der Kathedrale,\n"
               "Jazz im kleinen Club, Balladen mit großem Raum und Echo auf der Melodie, Dance mit punktiertem Echo ...\n"
               "Nachträglich einstellbar im Mischpult.");

    ImGui::SeparatorText("Arrangement");
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Begleitung##stil", accompStyleName(s.accompaniment))) {
        for (int i = 0; i < int(AccompStyle::Count); ++i)
            if (ImGui::Selectable(accompStyleName(AccompStyle(i)), int(s.accompaniment) == i)) s.accompaniment = AccompStyle(i);
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Bass##stil", bassStyleName(s.bass))) {
        for (int i = 0; i < int(BassStyle::Count); ++i)
            if (ImGui::Selectable(bassStyleName(BassStyle(i)), int(s.bass) == i)) s.bass = BassStyle(i);
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("Schlagzeug##stil", drumStyleName(s.drums))) {
        for (int i = 0; i < int(DrumStyle::Count); ++i)
            if (ImGui::Selectable(drumStyleName(DrumStyle(i)), int(s.drums) == i)) s.drums = DrumStyle(i);
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Melodie", &s.withMelody);
    ImGui::SameLine();
    ImGui::Checkbox("Akkorde", &s.withChords);
    ImGui::SameLine();
    ImGui::Checkbox("Bass", &s.withBass);
    ImGui::SameLine();
    ImGui::Checkbox("Drums", &s.withDrums);
    ImGui::Checkbox("Rückung im letzten Refrain erlauben", &s.allowKeyChange);
    ImGui::Checkbox("Instrumental-Solo", &s.solo);
    helpMarker("Nach dem zweiten Refrain improvisiert ein Instrument über die Strophen-Akkorde:\n"
               "Phrasen mit Atempausen, Steigerung zum Höhepunkt, Akkordtöne auf den Zählzeiten,\n"
               "chromatische Annäherungen. Im Form-Text: L8 = 8 Takte Solo.");
    if (s.solo && s.ensemble == Ensemble::Band) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130);
        instrumentCombo("##soloinst", s.soloInstrument);
    }
    if (ImGui::TreeNodeEx("Eigene Akkorde")) {
        ImGui::TextWrapped("Wie ein Akkordblatt: je Akkord ein Takt, z.B.  Am F C G  oder  Dm7 G7 Cmaj7 A7. "
                           "Leer = der Generator wählt. Die Tonart wird aus den Akkorden erkannt (wenn 'Zufall').");
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("Strophe##ak", "z.B. Am F C G", &s.chordsVerse);
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("Refrain##ak", "z.B. F G Am Am", &s.chordsChorus);
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("Pre-Refrain##ak", "optional", &s.chordsPre);
        ImGui::SetNextItemWidth(w);
        ImGui::InputTextWithHint("Bridge##ak", "optional", &s.chordsBridge);
        helpMarker("Erlaubt: C, Am, F#m, Bb (oder B), H, Fis, Es, G7, Cmaj7, Dm7, Hdim, Esus4, Caug, C/E.\n"
                   "Deutsch: H = h, B = b. Englisch \"Bb\" geht auch.");
        if (ImGui::SmallButton("Akkorde leeren")) s.chordsVerse.clear(), s.chordsChorus.clear(), s.chordsPre.clear(), s.chordsBridge.clear();
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("Instrumente", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(w);
        instrumentCombo("Melodie##inst", s.melodyInstrument);
        ImGui::SetNextItemWidth(w);
        instrumentCombo("Begleitung##inst", s.chordInstrument);
        ImGui::SetNextItemWidth(w);
        instrumentCombo("Bass##inst", s.bassInstrument);
        ImGui::SetNextItemWidth(w);
        instrumentCombo("Schlagzeug##inst", s.drumInstrument);
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Berechnung");
    ImGui::SetNextItemWidth(w);
    ImGui::SliderInt("Gründlichkeit", &s.quality, 1, 10);
    helpMarker("Wie viele Melodie-Kandidaten der eingebaute Kritiker bewertet und verfeinert, "
               "und wie viele komplette Songs zur Auswahl komponiert werden. Mehr = besser, aber langsamer.");
    ImGui::SetNextItemWidth(w);
    ImGui::InputScalar("Seed", ImGuiDataType_U32, &s.seed);
    ImGui::SameLine();
    ImGui::Checkbox("festhalten", &lockSeed_);
    helpMarker("Gleicher Seed + gleiche Einstellungen = gleicher Song. Ohne 'festhalten' gibt es jedes Mal einen neuen.");

    ImGui::Spacing();
    if (running_) {
        ImGui::ProgressBar(progress_.load(), {-1, 34}, endless ? "Komponiere nächsten Song ..." : "Komponiere ...");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.45f, 0.30f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.58f, 0.38f, 1));
        if (ImGui::Button("Song generieren  (G)", {-1, 34})) startGeneration();
        ImGui::PopStyleColor(2);
    }

    if (!info.key.empty()) {
        ImGui::SeparatorText(endless ? "Aktueller Song" : "Letzter Song");
        ImGui::Text("%s,  %.0f BPM,  Seed %u", info.key.c_str(), info.bpm, info.seed);
        if (!info.formName.empty()) ImGui::Text("Songform: %s", info.formName.c_str());
        ImGui::TextWrapped("Form: %s", info.structure.c_str());
        if (!info.orchStyle.empty()) {
            ImGui::Text("Orchester-Stil: %s", info.orchStyle.c_str());
            for (auto& t : info.techniques) ImGui::BulletText("%s", t.c_str());
        } else {
            ImGui::Text("Schlagzeug: %s", info.drums.c_str());
        }
        for (auto& p : info.progressions) ImGui::BulletText("%s", p.c_str());
        if (!info.systems.empty() && ImGui::TreeNodeEx("Kompositions-Systeme", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (auto& sy : info.systems) ImGui::BulletText("%s", sy.c_str());
            ImGui::TreePop();
        }
        if (!info.notes.empty()) ImGui::TextColored({0.9f, 0.8f, 0.4f, 1}, "%s", info.notes.c_str());
        ImGui::TextDisabled("%d Kandidaten bewertet in %.2f s, Note %.1f", info.candidatesEvaluated, lastSeconds_, info.score);
    }
    if (current && !current->sections.empty()) {
        ImGui::SeparatorText("Abschnitte bearbeiten");
        drawSections(*current);
    }
    ImGui::End();
}
