#include "SongIO.h"
#include "instruments/InstrumentRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

using nlohmann::json;

namespace {
void setError(std::string* e, const std::string& msg) { if (e) *e = msg; }
}

std::filesystem::path pathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// ===================== Song-Datei (JSON) =====================
static json generatorToJson(const GeneratorSettings& g) {
    return {
        {"seed", g.seed}, {"bpm", g.bpm}, {"beatsPerBar", g.beatsPerBar}, {"key", g.key}, {"scale", g.scale},
        {"lengthSeconds", g.lengthSeconds}, {"structure", g.structure}, {"creativity", g.creativity},
        {"melodyDensity", g.melodyDensity}, {"syncopation", g.syncopation}, {"energy", g.energy},
        {"humanize", g.humanize}, {"accompaniment", int(g.accompaniment)}, {"bass", int(g.bass)},
        {"withMelody", g.withMelody}, {"withChords", g.withChords}, {"withBass", g.withBass},
        {"allowKeyChange", g.allowKeyChange}, {"melodyOctaveShift", g.melodyOctaveShift},
        {"melodyInstrument", g.melodyInstrument}, {"chordInstrument", g.chordInstrument},
        {"bassInstrument", g.bassInstrument}, {"quality", g.quality},
        {"withDrums", g.withDrums}, {"drums", int(g.drums)}, {"drumInstrument", g.drumInstrument},
        {"ensemble", int(g.ensemble)}, {"orchStyle", g.orchStyle},
        {"swing", g.swing}, {"ambitus", g.ambitus}, {"leaps", g.leaps}, {"repetition", g.repetition},
        {"harmonicRhythm", g.harmonicRhythm}, {"jazzChords", g.jazzChords}, {"colorChords", g.colorChords},
        {"dynamicRange", g.dynamicRange}, {"useIntro", g.useIntro}, {"usePre", g.usePre},
        {"useBridge", g.useBridge}, {"useOutro", g.useOutro}, {"generalPause", g.generalPause},
        {"chordsVerse", g.chordsVerse}, {"chordsChorus", g.chordsChorus}, {"chordsPre", g.chordsPre},
        {"chordsBridge", g.chordsBridge}, {"solo", g.solo}, {"soloInstrument", g.soloInstrument},
        {"groove", g.groove}, {"grooveAmount", g.grooveAmount}, {"form", g.form}, {"room", g.room}, {"mixSystems", g.mixSystems}, {"systemVariety", g.systemVariety},
    };
}

static GeneratorSettings generatorFromJson(const json& j) {
    GeneratorSettings g;
    g.seed = j.value("seed", g.seed);
    g.bpm = j.value("bpm", g.bpm);
    g.beatsPerBar = j.value("beatsPerBar", g.beatsPerBar);
    g.key = j.value("key", g.key);
    g.scale = j.value("scale", g.scale);
    g.lengthSeconds = j.value("lengthSeconds", g.lengthSeconds);
    g.structure = j.value("structure", g.structure);
    g.creativity = j.value("creativity", g.creativity);
    g.melodyDensity = j.value("melodyDensity", g.melodyDensity);
    g.syncopation = j.value("syncopation", g.syncopation);
    g.energy = j.value("energy", g.energy);
    g.humanize = j.value("humanize", g.humanize);
    g.accompaniment = AccompStyle(std::clamp(j.value("accompaniment", 0), 0, int(AccompStyle::Count) - 1));
    g.bass = BassStyle(std::clamp(j.value("bass", 0), 0, int(BassStyle::Count) - 1));
    g.withMelody = j.value("withMelody", g.withMelody);
    g.withChords = j.value("withChords", g.withChords);
    g.withBass = j.value("withBass", g.withBass);
    g.allowKeyChange = j.value("allowKeyChange", g.allowKeyChange);
    g.melodyOctaveShift = j.value("melodyOctaveShift", g.melodyOctaveShift);
    g.melodyInstrument = j.value("melodyInstrument", g.melodyInstrument);
    g.chordInstrument = j.value("chordInstrument", g.chordInstrument);
    g.bassInstrument = j.value("bassInstrument", g.bassInstrument);
    g.quality = j.value("quality", g.quality);
    g.withDrums = j.value("withDrums", g.withDrums);
    g.drums = DrumStyle(std::clamp(j.value("drums", 0), 0, int(DrumStyle::Count) - 1));
    g.drumInstrument = j.value("drumInstrument", g.drumInstrument);
    g.ensemble = j.value("ensemble", 0) == 1 ? Ensemble::Orchestra : Ensemble::Band;
    g.orchStyle = j.value("orchStyle", g.orchStyle);
    g.swing = j.value("swing", g.swing);
    g.ambitus = j.value("ambitus", g.ambitus);
    g.leaps = j.value("leaps", g.leaps);
    g.repetition = j.value("repetition", g.repetition);
    g.harmonicRhythm = std::clamp(j.value("harmonicRhythm", g.harmonicRhythm), 0, 3);
    g.jazzChords = j.value("jazzChords", g.jazzChords);
    g.colorChords = j.value("colorChords", g.colorChords);
    g.dynamicRange = j.value("dynamicRange", g.dynamicRange);
    g.useIntro = j.value("useIntro", g.useIntro);
    g.usePre = j.value("usePre", g.usePre);
    g.useBridge = j.value("useBridge", g.useBridge);
    g.useOutro = j.value("useOutro", g.useOutro);
    g.generalPause = j.value("generalPause", g.generalPause);
    g.chordsVerse = j.value("chordsVerse", g.chordsVerse);
    g.chordsChorus = j.value("chordsChorus", g.chordsChorus);
    g.chordsPre = j.value("chordsPre", g.chordsPre);
    g.chordsBridge = j.value("chordsBridge", g.chordsBridge);
    g.solo = j.value("solo", g.solo);
    g.soloInstrument = j.value("soloInstrument", g.soloInstrument);
    g.groove = std::clamp(j.value("groove", g.groove), 0, 2);
    g.grooveAmount = j.value("grooveAmount", g.grooveAmount);
    g.form = std::clamp(j.value("form", g.form), -1, songFormCount() - 1);
    g.room = std::clamp(j.value("room", g.room), 0, roomPresetCount() - 1);
    g.mixSystems = j.value("mixSystems", g.mixSystems);
    g.systemVariety = std::clamp(j.value("systemVariety", g.systemVariety), 0.f, 1.f);
    return g;
}

bool saveSongFile(const std::string& path, const Song& song, const GeneratorSettings* generator, std::string* error) {
    json j;
    j["format"] = "MusicEngine-Song";
    j["version"] = 1;
    j["title"] = song.title;
    j["bpm"] = song.bpm;
    j["ppq"] = song.ppq;
    j["beatsPerBar"] = song.beatsPerBar;
    j["beatUnit"] = song.beatUnit;
    j["key"] = song.key;
    j["scale"] = song.scale;
    j["sections"] = json::array();
    for (auto& s : song.sections) j["sections"].push_back({{"name", s.name}, {"start", s.start}, {"length", s.length}});
    j["tracks"] = json::array();
    for (auto& t : song.tracks) {
        json notes = json::array();
        for (auto& n : t.notes) notes.push_back({n.pitch, n.start, n.length, std::round(n.velocity * 1000.f) / 1000.f});
        json expr = json::array();
        for (auto& [tick, v] : t.expression) expr.push_back({tick, std::round(v * 1000.f) / 1000.f});
        j["tracks"].push_back({{"name", t.name}, {"instrument", t.instrument}, {"volume", t.volume},
                               {"muted", t.muted}, {"solo", t.solo}, {"pan", t.pan}, {"reverb", t.reverb}, {"delay", t.delay}, {"notes", notes}, {"expression", expr}});
    }
    j["dynamics"] = json::array();
    for (auto& d : song.dynamics) j["dynamics"].push_back({int(d.kind), d.start, d.end, d.level});
    j["fx"] = {{"on", song.fx.on}, {"roomSize", song.fx.roomSize}, {"damping", song.fx.damping},
               {"reverbLevel", song.fx.reverbLevel}, {"preDelay", song.fx.preDelay}, {"delayNote", song.fx.delayNote},
               {"delayFeedback", song.fx.delayFeedback}, {"delayLevel", song.fx.delayLevel},
               {"mastering", song.fx.mastering}, {"masterAmount", song.fx.masterAmount}};
    if (generator) j["generator"] = generatorToJson(*generator);

    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { setError(error, "Datei kann nicht geschrieben werden: " + path); return false; }
    f << j.dump(1);
    return bool(f);
}

bool loadSongFile(const std::string& path, SongFile& out, std::string* error) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { setError(error, "Datei kann nicht geöffnet werden: " + path); return false; }
    try {
        json j = json::parse(f);
        if (j.value("format", "") != "MusicEngine-Song") { setError(error, "Keine MusicEngine-Songdatei."); return false; }
        Song s;
        s.title = j.value("title", s.title);
        if (j.contains("fx") && j["fx"].is_object()) {
            const auto& fx = j["fx"];
            s.fx.on = fx.value("on", s.fx.on);
            s.fx.roomSize = std::clamp(fx.value("roomSize", s.fx.roomSize), 0.f, 1.f);
            s.fx.damping = std::clamp(fx.value("damping", s.fx.damping), 0.f, 1.f);
            s.fx.reverbLevel = std::clamp(fx.value("reverbLevel", s.fx.reverbLevel), 0.f, 2.f);
            s.fx.preDelay = std::clamp(fx.value("preDelay", s.fx.preDelay), 0.f, 0.19f);
            s.fx.delayNote = std::clamp(fx.value("delayNote", s.fx.delayNote), 0, 4);
            s.fx.delayFeedback = std::clamp(fx.value("delayFeedback", s.fx.delayFeedback), 0.f, 0.9f);
            s.fx.delayLevel = std::clamp(fx.value("delayLevel", s.fx.delayLevel), 0.f, 2.f);
            s.fx.mastering = fx.value("mastering", s.fx.mastering);
            s.fx.masterAmount = std::clamp(fx.value("masterAmount", s.fx.masterAmount), 0.f, 1.f);
        }
        s.bpm = std::clamp(j.value("bpm", s.bpm), 10.0, 999.0);
        s.ppq = std::max(24, j.value("ppq", s.ppq));
        s.beatsPerBar = std::clamp(j.value("beatsPerBar", s.beatsPerBar), 1, 32);
        s.beatUnit = j.value("beatUnit", s.beatUnit);
        if (s.beatUnit != 2 && s.beatUnit != 4 && s.beatUnit != 8 && s.beatUnit != 16) s.beatUnit = 4;
        s.key = std::clamp(j.value("key", -1), -1, 11);
        s.scale = std::clamp(j.value("scale", -1), -1, 8);
        for (auto& js : j.value("sections", json::array()))
            s.sections.push_back({js.value("name", ""), js.value("start", Tick(0)), js.value("length", Tick(0))});
        for (auto& jt : j.value("tracks", json::array())) {
            Track t;
            t.name = jt.value("name", t.name);
            t.instrument = jt.value("instrument", t.instrument);
            t.volume = jt.value("volume", t.volume);
            t.muted = jt.value("muted", t.muted);
            t.solo = jt.value("solo", t.solo);
            t.pan = std::clamp(jt.value("pan", t.pan), -1.f, 1.f);
            t.reverb = std::clamp(jt.value("reverb", t.reverb), 0.f, 1.f);
            t.delay = std::clamp(jt.value("delay", t.delay), 0.f, 1.f);
            for (auto& jn : jt.value("notes", json::array())) {
                if (!jn.is_array() || jn.size() < 3) continue;
                Note n;
                n.pitch = std::clamp(jn[0].get<int>(), 0, 127);
                n.start = std::max<Tick>(0, jn[1].get<Tick>());
                n.length = std::max<Tick>(1, jn[2].get<Tick>());
                n.velocity = jn.size() > 3 ? std::clamp(jn[3].get<float>(), 0.f, 1.f) : 0.8f;
                t.notes.push_back(n);
            }
            for (auto& je : jt.value("expression", json::array()))
                if (je.is_array() && je.size() >= 2) t.expression.push_back({je[0].get<Tick>(), std::clamp(je[1].get<float>(), 0.f, 2.f)});
            s.tracks.push_back(std::move(t));
        }
        for (auto& jd : j.value("dynamics", json::array()))
            if (jd.is_array() && jd.size() >= 4)
                s.dynamics.push_back({DynamicMark::Kind(std::clamp(jd[0].get<int>(), 0, 2)), jd[1].get<Tick>(), jd[2].get<Tick>(),
                                      std::clamp(jd[3].get<int>(), 0, 5)});
        if (s.tracks.empty()) s.tracks.push_back({"Spur 1", "Piano"});
        out.song = std::move(s);
        out.hasGenerator = j.contains("generator");
        if (out.hasGenerator) out.generator = generatorFromJson(j["generator"]);
        return true;
    } catch (const std::exception& e) {
        setError(error, std::string("Fehler beim Lesen: ") + e.what());
        return false;
    }
}

// ===================== MIDI-Export =====================
namespace {
void putVarLen(std::vector<uint8_t>& b, uint32_t v) {
    uint8_t tmp[5];
    int n = 0;
    tmp[n++] = v & 0x7F;
    while (v >>= 7) tmp[n++] = uint8_t((v & 0x7F) | 0x80);
    while (n--) b.push_back(tmp[n]);
}
void put16(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void put32(std::vector<uint8_t>& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }

void putMetaText(std::vector<uint8_t>& b, uint32_t delta, uint8_t type, const std::string& text) {
    putVarLen(b, delta);
    b.push_back(0xFF);
    b.push_back(type);
    putVarLen(b, uint32_t(text.size()));
    b.insert(b.end(), text.begin(), text.end());
}

void writeChunk(std::ofstream& f, const char* id, const std::vector<uint8_t>& data) {
    f.write(id, 4);
    std::vector<uint8_t> len;
    put32(len, uint32_t(data.size()));
    f.write(reinterpret_cast<const char*>(len.data()), 4);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}
}

bool exportMidi(const std::string& path, const Song& song, std::string* error) {
    std::ofstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { setError(error, "Datei kann nicht geschrieben werden: " + path); return false; }

    std::vector<uint8_t> hdr;
    put16(hdr, 1);                                   // Format 1
    put16(hdr, uint16_t(song.tracks.size() + 1));    // + Dirigenten-Spur
    put16(hdr, uint16_t(song.ppq));
    writeChunk(f, "MThd", hdr);

    // Spur 0: Tempo, Taktart, Abschnitts-Marker
    {
        std::vector<uint8_t> t;
        putMetaText(t, 0, 0x03, song.title);
        const uint32_t usPerQuarter = uint32_t(std::lround(60000000.0 / song.bpm));
        putVarLen(t, 0);
        t.insert(t.end(), {0xFF, 0x51, 0x03, uint8_t(usPerQuarter >> 16), uint8_t(usPerQuarter >> 8), uint8_t(usPerQuarter)});
        int dd = 0;
        for (int u = song.beatUnit; u > 1; u >>= 1) ++dd;
        putVarLen(t, 0);
        t.insert(t.end(), {0xFF, 0x58, 0x04, uint8_t(song.beatsPerBar), uint8_t(dd), 24, 8});
        Tick last = 0;
        for (auto& s : song.sections) {
            putMetaText(t, uint32_t(s.start - last), 0x06, s.name);
            last = s.start;
        }
        putVarLen(t, 0);
        t.insert(t.end(), {0xFF, 0x2F, 0x00});
        writeChunk(f, "MTrk", t);
    }

    for (size_t i = 0; i < song.tracks.size(); ++i) {
        const Track& tr = song.tracks[i];
        int ch = int(i % 15);
        if (ch >= 9) ++ch;   // Kanal 10 ist für Schlagzeug reserviert
        int program = 0;
        if (auto inst = InstrumentRegistry::instance().create(tr.instrument)) {
            program = inst->midiProgram();
            if (inst->isDrumKit()) ch = 9;
        }

        std::vector<uint8_t> t;
        putMetaText(t, 0, 0x03, tr.name);
        putVarLen(t, 0);
        t.push_back(uint8_t(0xC0 | ch));
        t.push_back(uint8_t(std::clamp(program, 0, 127)));
        putVarLen(t, 0);
        t.insert(t.end(), {uint8_t(0xB0 | ch), 7, uint8_t(std::clamp(int(tr.volume * 100), 0, 127))});
        putVarLen(t, 0);   // Panorama (Controller 10): 0 links, 64 Mitte, 127 rechts
        t.insert(t.end(), {uint8_t(0xB0 | ch), 10, uint8_t(std::clamp(int(std::lround(64.f + tr.pan * 63.f)), 0, 127))});
        putVarLen(t, 0);   // Hall-Anteil (Controller 91)
        t.insert(t.end(), {uint8_t(0xB0 | ch), 91, uint8_t(std::clamp(int(std::lround(song.fx.on ? tr.reverb * 127.f : 0.f)), 0, 127))});

        struct Ev { Tick tick; int kind; int a; int b; };   // kind: 0 = Controller 11, 1 = Note aus, 2 = Note an
        std::vector<Ev> evs;
        for (auto& n : tr.notes) {
            evs.push_back({n.start, 2, n.pitch, std::clamp(int(std::lround(n.velocity * 127)), 1, 127)});
            evs.push_back({n.end(), 1, n.pitch, 0});
        }
        // Dynamik-Automation als "Expression" (Controller 11), in 1/16-Schritten abgetastet
        if (!tr.expression.empty()) {
            int lastVal = -1;
            const Tick step = std::max<Tick>(1, song.ppq / 4);
            for (Tick tk = 0; tk <= tr.expression.back().first; tk += step) {
                const int v = std::clamp(int(std::lround(tr.expressionAt(double(tk)) * 110.f)), 0, 127);
                if (v != lastVal) { evs.push_back({tk, 0, 11, v}); lastVal = v; }
            }
        }
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) {
            return a.tick != b.tick ? a.tick < b.tick : a.kind < b.kind;
        });
        Tick last = 0;
        for (auto& e : evs) {
            putVarLen(t, uint32_t(e.tick - last));
            last = e.tick;
            t.push_back(uint8_t((e.kind == 0 ? 0xB0 : e.kind == 2 ? 0x90 : 0x80) | ch));
            t.push_back(uint8_t(std::clamp(e.a, 0, 127)));
            t.push_back(uint8_t(e.b));
        }
        putVarLen(t, 0);
        t.insert(t.end(), {0xFF, 0x2F, 0x00});
        writeChunk(f, "MTrk", t);
    }
    return bool(f);
}

// ===================== MIDI-Import =====================
bool importMidi(const std::string& path, Song& out, std::string* error) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { setError(error, "Datei kann nicht geöffnet werden: " + path); return false; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    auto need = [&](size_t n) { if (pos + n > d.size()) throw std::runtime_error("Datei ist abgeschnitten"); };
    auto rd16 = [&] { need(2); uint32_t v = (d[pos] << 8) | d[pos + 1]; pos += 2; return v; };
    auto rd32 = [&] { need(4); uint32_t v = (uint32_t(d[pos]) << 24) | (d[pos + 1] << 16) | (d[pos + 2] << 8) | d[pos + 3]; pos += 4; return v; };
    auto rdVar = [&] { uint32_t v = 0; for (int i = 0; i < 4; ++i) { need(1); uint8_t c = d[pos++]; v = (v << 7) | (c & 0x7F); if (!(c & 0x80)) break; } return v; };

    try {
        need(4);
        if (std::string(reinterpret_cast<char*>(&d[0]), 4) != "MThd") throw std::runtime_error("Keine MIDI-Datei");
        pos = 4;
        uint32_t hlen = rd32();
        size_t hend = pos + hlen;
        rd16();   // Format
        uint32_t ntrk = rd16();
        uint32_t division = rd16();
        if (division & 0x8000) throw std::runtime_error("SMPTE-Zeitbasis wird nicht unterstützt");
        pos = hend;

        Song s;
        s.ppq = 480;
        const double scale = 480.0 / double(std::max(1u, division));
        bool tempoSet = false;

        for (uint32_t ti = 0; ti < ntrk && pos + 8 <= d.size(); ++ti) {
            need(8);
            std::string id(reinterpret_cast<char*>(&d[pos]), 4);
            pos += 4;
            uint32_t len = rd32();
            size_t end = std::min(d.size(), pos + len);
            if (id != "MTrk") { pos = end; continue; }

            std::string trackName;
            std::map<int, std::vector<Note>> byChannel;
            std::map<int, int> programs;
            std::map<int, std::vector<std::pair<Tick, float>>> exprBy;
            std::map<std::pair<int, int>, std::vector<std::pair<Tick, float>>> open;
            Tick tick = 0;
            uint8_t running = 0;
            while (pos < end) {
                tick += rdVar();
                need(1);
                uint8_t st = d[pos];
                if (st & 0x80) ++pos; else st = running;
                if (st == 0xFF) {
                    need(1);
                    uint8_t type = d[pos++];
                    uint32_t l = rdVar();
                    need(l);
                    if (type == 0x03) trackName.assign(reinterpret_cast<char*>(&d[pos]), l);
                    if (type == 0x51 && l == 3 && !tempoSet) {
                        uint32_t us = (d[pos] << 16) | (d[pos + 1] << 8) | d[pos + 2];
                        if (us) s.bpm = 60000000.0 / us;
                        tempoSet = true;
                    }
                    if (type == 0x58 && l >= 2) { s.beatsPerBar = std::max(1, int(d[pos])); s.beatUnit = 1 << d[pos + 1]; }
                    if (type == 0x06) s.sections.push_back({std::string(reinterpret_cast<char*>(&d[pos]), l), Tick(tick * scale), 0});
                    pos += l;
                } else if (st == 0xF0 || st == 0xF7) {
                    uint32_t l = rdVar();
                    pos += l;
                } else {
                    running = st;
                    const int type = st & 0xF0, ch = st & 0x0F;
                    const int nData = (type == 0xC0 || type == 0xD0) ? 1 : 2;
                    need(nData);
                    int a = d[pos], b = nData > 1 ? d[pos + 1] : 0;
                    pos += nData;
                    const Tick t = Tick(std::llround(double(tick) * scale));
                    if (type == 0x90 && b > 0) open[{ch, a}].push_back({t, b / 127.f});
                    else if (type == 0x80 || (type == 0x90 && b == 0)) {
                        auto& stack = open[{ch, a}];
                        if (!stack.empty()) {
                            auto [st0, vel] = stack.front();
                            stack.erase(stack.begin());
                            byChannel[ch].push_back({a, st0, std::max<Tick>(1, t - st0), vel});
                        }
                    } else if (type == 0xC0) programs[ch] = a;
                    else if (type == 0xB0 && a == 11) exprBy[ch].push_back({t, b / 110.f});   // Expression = Dynamik
                }
            }
            pos = end;
            for (auto& [ch, notes] : byChannel) {
                Track tr;
                tr.name = trackName.empty() ? "Spur " + std::to_string(s.tracks.size() + 1) : trackName;
                if (byChannel.size() > 1) tr.name += " (Kanal " + std::to_string(ch + 1) + ")";
                tr.instrument = ch == 9 ? "Schlagzeug" : "Piano";
                std::sort(notes.begin(), notes.end(), [](const Note& x, const Note& y) { return x.start < y.start; });
                tr.notes = std::move(notes);
                tr.expression = exprBy[ch];
                s.tracks.push_back(std::move(tr));
            }
        }
        // Abschnittslängen aus den Markern ableiten
        Tick songEnd = s.length();
        for (size_t i = 0; i < s.sections.size(); ++i) {
            Tick next = i + 1 < s.sections.size() ? s.sections[i + 1].start : songEnd;
            s.sections[i].length = std::max<Tick>(0, next - s.sections[i].start);
        }
        if (s.tracks.empty()) throw std::runtime_error("Keine Noten gefunden");
        auto stem = pathFromUtf8(path).stem().u8string();
        s.title = std::string(stem.begin(), stem.end());
        out = std::move(s);
        return true;
    } catch (const std::exception& e) {
        setError(error, std::string("MIDI-Import fehlgeschlagen: ") + e.what());
        return false;
    }
}
