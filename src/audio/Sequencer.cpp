#include "Sequencer.h"
#include <algorithm>

void Sequencer::setSong(const Song& song) {
    events_.clear();
    for (int t = 0; t < int(song.tracks.size()); ++t) {
        for (auto& n : song.tracks[t].notes) {
            if (n.length <= 0) continue;
            events_.push_back({n.start, t, n.pitch, n.velocity, true});
            events_.push_back({n.end(), t, n.pitch, 0.f, false});
        }
    }
    // Bei gleichem Tick zuerst Note-Off, damit wiederholte Töne neu angeschlagen werden
    std::stable_sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return !a.on && b.on;
    });
    bpm_ = song.bpm;
    ppq_ = song.ppq;
    songEnd_ = song.length();
    moveCursorTo(pos_);
}

void Sequencer::seek(double tick, Sink& sink) {
    sink.seqAllNotesOff();
    pos_ = std::max(0.0, tick);
    position_ = pos_;
    moveCursorTo(pos_);
}

void Sequencer::setLoop(bool enabled, Tick start, Tick end) {
    looping_ = enabled && end > start;
    loopStart_ = start;
    loopEnd_ = end;
}

void Sequencer::moveCursorTo(double tick) {
    cursor_ = size_t(std::lower_bound(events_.begin(), events_.end(), tick,
                                      [](const Event& e, double t) { return double(e.tick) < t; })
                     - events_.begin());
}

void Sequencer::fireUntil(double limitTick, Sink& sink) {
    while (cursor_ < events_.size() && double(events_[cursor_].tick) < limitTick) {
        auto& e = events_[cursor_++];
        if (e.on) sink.seqNoteOn(e.track, e.pitch, e.velocity);
        else sink.seqNoteOff(e.track, e.pitch);
    }
}

void Sequencer::advance(int frames, Sink& sink) {
    if (!playing_) return;
    const double ticksPerSample = bpm_ * ppq_ / (60.0 * sampleRate_);
    double endTick = pos_ + frames * ticksPerSample;

    while (looping_ && endTick >= double(loopEnd_)) {
        fireUntil(double(loopEnd_), sink);
        sink.seqAllNotesOff();
        endTick = double(loopStart_) + (endTick - double(loopEnd_));
        pos_ = double(loopStart_);
        moveCursorTo(pos_);
    }
    fireUntil(endTick, sink);
    pos_ = endTick;

    if (!looping_ && cursor_ >= events_.size() && pos_ >= double(songEnd_)) {
        playing_ = false;   // Song zu Ende, Töne klingen natürlich aus
    }
    position_ = pos_;
}
