#pragma once
#include "audio/Effects.h"
#include "audio/Sequencer.h"
#include "core/Song.h"
#include "instruments/IInstrument.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct ma_device;

// Öffnet das Audiogerät, spielt Songs über den Sequencer ab und mischt
// alle Spuren. Alle öffentlichen Methoden sind thread-sicher.
class AudioEngine : private Sequencer::Sink {
public:
    AudioEngine();
    ~AudioEngine();

    bool start(int sampleRate = 48000);
    void stop();

    // Lädt bzw. aktualisiert einen Song: legt pro Spur ein Instrument aus der
    // Registry an (bestehende werden wiederverwendet). Kann jederzeit aufgerufen werden.
    void loadSong(const Song& song);

    // Transport
    void play();
    void pause();
    void stopPlayback();
    void seek(double tick);
    void scrubTo(double tick);   // Position verschieben, ohne Töne abzuschneiden
    void setLoop(bool enabled, Tick start = 0, Tick end = 0);
    void setTempo(double bpm);
    bool isPlaying();
    double position() const { return sequencer_.position(); }

    // Spur-Einstellungen
    void setTrackVolume(int track, float volume);
    void setTrackMuted(int track, bool muted);
    void setTrackSolo(int track, bool solo);
    void setTrackPan(int track, float pan);
    void setTrackSends(int track, float reverb, float delay);   // Anteil in Hall / Echo
    void setEffects(const Effects& fx);                         // Raumklang des Songs
    float masterReductionDb() { std::lock_guard l(mutex_); return master_.gainReductionDb(); }   // Kompressor-Anzeige
    // Pegel (Spitzenwert 0..1+, fällt langsam ab) pro Spur und gesamt – für die Pegelanzeigen im Mischpult
    std::vector<float> trackLevels(float* master = nullptr);

    // Live spielen (z.B. Vorhören beim Notenzeichnen)
    void noteOn(int track, int pitch, float velocity);
    void noteOff(int track, int pitch);
    void allNotesOff();

    // Freies Spielen mit eigenem Instrument (unabhängig von den Spuren des Songs).
    // Töne an Spur kLiveTrack landen bei diesem Instrument. Leerer Name = aus.
    static constexpr int kLiveTrack = -2;
    void setLiveInstrument(const std::string& name);
    std::string liveInstrumentName() { std::lock_guard l(mutex_); return liveName_; }

    // Aufnahme & Metronom
    struct RecEvent { double tick; int pitch; float velocity; bool on; };
    void setMetronome(bool on) { std::lock_guard l(mutex_); metronome_ = on; }
    // Startet die Wiedergabe mit Einzähler: countInBars Takte Klicks vor fromTick
    void playWithCountIn(double fromTick, int countInBars);
    void setRecording(bool on);
    bool isRecording() { std::lock_guard l(mutex_); return recording_; }
    // Holt die seit dem letzten Aufruf aufgenommenen Live-Ereignisse (Zeit in Ticks)
    std::vector<RecEvent> takeRecorded();

    // Rendert den geladenen Song ohne Audiogerät in eine WAV-Datei.
    // tailSeconds = Ausklingzeit nach der letzten Note.
    bool renderToWav(const std::string& path, double tailSeconds = 2.0, int sampleRate = 48000);

    // Abspiellautstärke 0..1.5 (1 = normal). Wird im Audio-Thread gelesen, daher atomar.
    std::atomic<float> masterVolume{1.f};

private:
    struct Event { enum Type { On, Off, AllOff } type; int track; int pitch; float velocity; };
    struct Channel {
        std::unique_ptr<IInstrument> instrument;
        std::string instrumentName;
        std::vector<std::pair<Tick, float>> expression;   // Dynamik-Verlauf
        float lastGain = 1.f;                             // für weiches Überblenden zwischen Blöcken
        float volume = 1.f;
        bool muted = false;
        bool solo = false;
        float pan = 0.f;
        float reverb = 0.2f, delay = 0.f;                 // Effekt-Anteile
        float meter = 0.f;                                // Spitzenpegel (Anzeige)
    };

    static void callback(ma_device* device, void* out, const void* in, unsigned frameCount);
    void process(float* out, int frameCount);   // sperrt mutex_
    void processLocked(float* out, int frameCount);

    void seqNoteOn(int track, int pitch, float velocity) override;
    void seqNoteOff(int track, int pitch) override;
    void seqAllNotesOff() override;

    std::unique_ptr<ma_device> device_;
    std::vector<Channel> channels_;
    std::vector<Event> pending_;
    std::vector<float> scratch_;
    Sequencer sequencer_;
    std::unique_ptr<IInstrument> click_;   // Metronom-Klang
    bool metronome_ = false, recording_ = false;
    std::vector<RecEvent> recorded_;
    Tick ticksPerBeat_ = 480;
    int beatsPerBar_ = 4;
    std::mutex mutex_;
    int sampleRate_ = 48000;
    bool running_ = false;
    bool anySolo_ = false;
    // Effekt-Bus: Hall + Echo
    Reverb reverb_;
    PingPongDelay delay_;
    MasterBus master_;
    Effects fx_;
    double bpm_ = 120.0;
    std::vector<float> revBus_, delBus_;
    void updateEffects();
    void initEffects(int sampleRate);
    std::unique_ptr<IInstrument> liveInst_;
    std::string liveName_;
    float masterMeter_ = 0.f;
    void updateSolo();
    bool audible(const Channel& c) const { return !c.muted && (!anySolo_ || c.solo); }
};
