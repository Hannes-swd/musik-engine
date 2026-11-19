#include "LivePlayer.h"
#include <algorithm>
#include <cstdio>
#include <imgui.h>

void LivePlayer::setTrack(int track) {
    if (track == track_) return;
    allOff();
    std::lock_guard l(mutex_);
    track_ = track;
}

void LivePlayer::noteOn(int pitch, float vel) {
    std::lock_guard l(mutex_);
    if (held_.count(pitch) || sustained_.count(pitch)) engine_.noteOff(track_, pitch);
    sustained_.erase(pitch);
    held_.insert(pitch);
    engine_.noteOn(track_, pitch, vel);
}

void LivePlayer::noteOff(int pitch) {
    std::lock_guard l(mutex_);
    if (!held_.erase(pitch)) return;
    if (pedal_) sustained_.insert(pitch);
    else engine_.noteOff(track_, pitch);
}

void LivePlayer::sustain(bool down) {
    std::lock_guard l(mutex_);
    pedal_ = down;
    if (!down) {
        for (int p : sustained_) engine_.noteOff(track_, p);
        sustained_.clear();
    }
}

void LivePlayer::allOff() {
    std::lock_guard l(mutex_);
    for (int p : held_) engine_.noteOff(track_, p);
    for (int p : sustained_) engine_.noteOff(track_, p);
    held_.clear();
    sustained_.clear();
    keyPitch_.clear();
}

void LivePlayer::onMidi(int status, int d1, int d2) {
    const int type = status & 0xF0;
    if (type == 0x90 && d2 > 0) noteOn(d1, d2 / 127.f);
    else if (type == 0x80 || (type == 0x90 && d2 == 0)) noteOff(d1);
    else if (type == 0xB0 && d1 == 64) sustain(d2 >= 64);
}

const std::vector<LivePlayer::KeyMap>& LivePlayer::keyMap() {
    // ImGui meldet Buchstaben passend zum Tastatur-Layout; Ö/Ü/´/+ kommen als die Tasten an
    // derselben Stelle der US-Tastatur (; [ = ]) an.
    static const std::vector<KeyMap> m = {
        // untere Reihe: Oktave o
        {ImGuiKey_Y, 0, "Y"}, {ImGuiKey_S, 1, "S"}, {ImGuiKey_X, 2, "X"}, {ImGuiKey_D, 3, "D"}, {ImGuiKey_C, 4, "C"},
        {ImGuiKey_V, 5, "V"}, {ImGuiKey_G, 6, "G"}, {ImGuiKey_B, 7, "B"}, {ImGuiKey_H, 8, "H"}, {ImGuiKey_N, 9, "N"},
        {ImGuiKey_J, 10, "J"}, {ImGuiKey_M, 11, "M"}, {ImGuiKey_Comma, 12, ","}, {ImGuiKey_L, 13, "L"},
        {ImGuiKey_Period, 14, "."}, {ImGuiKey_Semicolon, 15, "\xC3\x96"}, {ImGuiKey_Minus, 16, "-"},
        // obere Reihe: Oktave o+1
        {ImGuiKey_Q, 12, "Q"}, {ImGuiKey_2, 13, "2"}, {ImGuiKey_W, 14, "W"}, {ImGuiKey_3, 15, "3"}, {ImGuiKey_E, 16, "E"},
        {ImGuiKey_R, 17, "R"}, {ImGuiKey_5, 18, "5"}, {ImGuiKey_T, 19, "T"}, {ImGuiKey_6, 20, "6"}, {ImGuiKey_Z, 21, "Z"},
        {ImGuiKey_7, 22, "7"}, {ImGuiKey_U, 23, "U"}, {ImGuiKey_I, 24, "I"}, {ImGuiKey_9, 25, "9"}, {ImGuiKey_O, 26, "O"},
        {ImGuiKey_0, 27, "0"}, {ImGuiKey_P, 28, "P"}, {ImGuiKey_LeftBracket, 29, "\xC3\x9C"}, {ImGuiKey_Equal, 30, "\xC2\xB4"},
        {ImGuiKey_RightBracket, 31, "+"},
    };
    return m;
}

void LivePlayer::handleComputerKeyboard(float vel) {
    const auto& keys = keyMap();
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) octave = std::max(1, octave - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) octave = std::min(6, octave + 1);
    // Umschalt halten = Haltepedal
    const bool shift = ImGui::GetIO().KeyShift;
    if (shift != shiftPedal_) { shiftPedal_ = shift; sustain(shift); }
    for (auto& k : keys) {
        if (ImGui::IsKeyPressed(k.key, false)) {
            const int pitch = std::clamp((octave + 1) * 12 + k.semitone, 0, 127);
            keyPitch_[int(k.key)] = pitch;
            noteOn(pitch, vel);
        }
        if (ImGui::IsKeyReleased(k.key)) {
            auto it = keyPitch_.find(int(k.key));
            if (it != keyPitch_.end()) { noteOff(it->second); keyPitch_.erase(it); }
        }
    }
}
