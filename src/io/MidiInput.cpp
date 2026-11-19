#include "MidiInput.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

namespace {
std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void CALLBACK midiProc(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
    if (msg != MIM_DATA) return;
    const DWORD m = DWORD(param1);
    reinterpret_cast<MidiInput*>(instance)->dispatch(int(m & 0xFF), int((m >> 8) & 0x7F), int((m >> 16) & 0x7F));
}
}

MidiInput::~MidiInput() { close(); }

std::vector<std::string> MidiInput::devices() {
    std::vector<std::string> out;
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) out.push_back(narrow(caps.szPname));
        else out.push_back("MIDI-Gerät " + std::to_string(i + 1));
    }
    return out;
}

bool MidiInput::open(int index, Callback cb) {
    close();
    cb_ = std::move(cb);
    HMIDIIN h = nullptr;
    if (midiInOpen(&h, UINT(index), DWORD_PTR(&midiProc), DWORD_PTR(this), CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        return false;
    midiInStart(h);
    handle_ = h;
    index_ = index;
    return true;
}

void MidiInput::close() {
    if (!handle_) return;
    HMIDIIN h = static_cast<HMIDIIN>(handle_);
    midiInStop(h);
    midiInReset(h);
    midiInClose(h);
    handle_ = nullptr;
    index_ = -1;
}

#else
MidiInput::~MidiInput() {}
std::vector<std::string> MidiInput::devices() { return {}; }
bool MidiInput::open(int, Callback) { return false; }
void MidiInput::close() {}
#endif
