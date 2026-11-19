#pragma once
#include "audio/AudioEngine.h"
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <imgui.h>

// Live spielen: Computer-Tastatur und MIDI-Keyboard -> Audio-Engine.
// Kümmert sich um das Sustain-Pedal und merkt sich, welche Töne gerade klingen.
//
// Tastatur (deutsches Layout, wie bei Trackern/DAWs – zwei Reihen = gut 2½ Oktaven):
//   untere Reihe:  Y X C V B N M , . -   = weiße Tasten ab C (Oktave o)
//                   S D   G H J   L Ö    = schwarze Tasten dazu
//   obere Reihe:   Q W E R T Z U I O P Ü + = weiße Tasten ab C (Oktave o+1)
//                   2 3   5 6 7   9 0  ´  = schwarze Tasten dazu
//   Pfeil links/rechts = Oktave runter/hoch, Umschalt halten = Haltepedal
class LivePlayer {
public:
    explicit LivePlayer(AudioEngine& engine) : engine_(engine) {}

    struct KeyMap { ImGuiKey key; int semitone; const char* label; };
    static const std::vector<KeyMap>& keyMap();   // semitone relativ zum C der eingestellten Oktave

    void setTrack(int track);
    int track() const { return track_; }

    void noteOn(int pitch, float vel);
    void noteOff(int pitch);
    void sustain(bool down);
    bool pedal() { std::lock_guard l(mutex_); return pedal_; }
    void allOff();

    // MIDI-Nachricht vom Keyboard (beliebiger Thread)
    void onMidi(int status, int d1, int d2);

    // Pro Frame aus dem UI-Thread aufrufen (nur wenn Tastatur-Spielen aktiv)
    void handleComputerKeyboard(float vel);

    int octave = 4;            // untere Reihe (Y) = C4, obere Reihe (Q) = C5
    float velocity = 0.8f;     // Anschlag der Computer-Tastatur
    int basePitch() const { return (octave + 1) * 12; }
    std::set<int> held() { std::lock_guard l(mutex_); return held_; }
    // gerade klingend (gedrückt oder vom Pedal gehalten)
    std::set<int> sounding() {
        std::lock_guard l(mutex_);
        std::set<int> s = held_;
        s.insert(sustained_.begin(), sustained_.end());
        return s;
    }

private:
    AudioEngine& engine_;
    std::mutex mutex_;
    int track_ = 0;
    bool pedal_ = false;
    bool shiftPedal_ = false;  // Haltepedal über die Umschalttaste
    std::set<int> held_;       // Tasten gedrückt
    std::set<int> sustained_;  // losgelassen, aber vom Pedal gehalten
    std::map<int, int> keyPitch_;   // Computer-Taste -> gespielte Tonhöhe
};
