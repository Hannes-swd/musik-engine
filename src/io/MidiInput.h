#pragma once
#include <functional>
#include <string>
#include <vector>

// MIDI-Eingang (angeschlossenes Keyboard). Der Callback kommt aus einem fremden Thread.
class MidiInput {
public:
    using Callback = std::function<void(int status, int data1, int data2)>;

    ~MidiInput();
    static std::vector<std::string> devices();
    bool open(int index, Callback cb);
    void close();
    int current() const { return index_; }

    // intern (vom Betriebssystem aufgerufen)
    void dispatch(int status, int d1, int d2) { if (cb_) cb_(status, d1, d2); }

private:
    void* handle_ = nullptr;
    int index_ = -1;
    Callback cb_;
};
