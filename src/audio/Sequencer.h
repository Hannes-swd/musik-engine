#pragma once
#include "core/Song.h"
#include <algorithm>
#include <atomic>
#include <vector>

// Spielt einen Song ab: wandelt Audio-Samples in Ticks um und feuert
// Note-On/Off-Ereignisse. Läuft im Audio-Thread (von AudioEngine aufgerufen).
class Sequencer {
public:
    struct Sink {
        virtual ~Sink() = default;
        virtual void seqNoteOn(int track, int pitch, float velocity) = 0;
        virtual void seqNoteOff(int track, int pitch) = 0;
        virtual void seqAllNotesOff() = 0;
    };

    void setSong(const Song& song);
    void setSampleRate(double sr) { sampleRate_ = sr; }
    void setTempo(double bpm) { bpm_ = bpm; }
    double tempo() const { return bpm_; }

    void play() { playing_ = true; }
    void pause(Sink& sink) { playing_ = false; sink.seqAllNotesOff(); }
    void stop(Sink& sink) { playing_ = false; seek(0, sink); }
    void seek(double tick, Sink& sink);
    // Position setzen, ohne klingende Töne abzuschneiden (Scrubbing im Lineal)
    void setPosition(double tick) { pos_ = std::max(0.0, tick); position_ = pos_; moveCursorTo(pos_); }
    // Wie seek, erlaubt aber negative Positionen (Einzähler vor Takt 1)
    void startAt(double tick, Sink& sink) { sink.seqAllNotesOff(); pos_ = tick; position_ = pos_; moveCursorTo(pos_); playing_ = true; }
    void setLoop(bool enabled, Tick start = 0, Tick end = 0);

    bool isPlaying() const { return playing_; }
    double position() const { return position_.load(); }   // thread-sicher lesbar
    Tick songEnd() const { return songEnd_; }

    // Schiebt die Zeit um frames Samples weiter und feuert fällige Ereignisse.
    void advance(int frames, Sink& sink);

private:
    struct Event { Tick tick; int track; int pitch; float velocity; bool on; };

    void fireUntil(double limitTick, Sink& sink);
    void moveCursorTo(double tick);

    std::vector<Event> events_;
    size_t cursor_ = 0;
    double pos_ = 0;
    std::atomic<double> position_{0};
    double sampleRate_ = 48000;
    double bpm_ = 120;
    int ppq_ = 480;
    Tick songEnd_ = 0;
    bool playing_ = false;
    bool looping_ = false;
    Tick loopStart_ = 0, loopEnd_ = 0;
};
