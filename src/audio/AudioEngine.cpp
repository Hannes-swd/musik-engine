#include "AudioEngine.h"
#include "instruments/InstrumentRegistry.h"
#include "miniaudio.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace {
constexpr int kChunk = 64;   // Ereignis-Auflösung: 64 Samples (~1.3 ms bei 48 kHz)
}

AudioEngine::AudioEngine() : device_(std::make_unique<ma_device>()) {
    scratch_.resize(kChunk * 2);
    revBus_.resize(kChunk * 2);
    delBus_.resize(kChunk * 2);
    initEffects(48000);
}

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::start(int sampleRate) {
    if (running_) return true;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = static_cast<ma_uint32>(sampleRate);
    cfg.dataCallback = &AudioEngine::callback;
    cfg.pUserData = this;

    if (ma_device_init(nullptr, &cfg, device_.get()) != MA_SUCCESS) return false;
    {
        std::lock_guard lock(mutex_);
        sampleRate_ = static_cast<int>(device_->sampleRate);
        initEffects(sampleRate_);
        sequencer_.setSampleRate(sampleRate_);
        for (auto& c : channels_) c.instrument->setSampleRate(float(sampleRate_));
        if (liveInst_) liveInst_->setSampleRate(float(sampleRate_));
    }
    if (ma_device_start(device_.get()) != MA_SUCCESS) {
        ma_device_uninit(device_.get());
        return false;
    }
    running_ = true;
    return true;
}

void AudioEngine::stop() {
    if (!running_) return;
    ma_device_uninit(device_.get());
    running_ = false;
}

void AudioEngine::loadSong(const Song& song) {
    std::lock_guard lock(mutex_);
    // Instrumente wiederverwenden, wenn Spur und Instrument gleich bleiben,
    // damit klingende Töne beim Editieren nicht abreißen.
    std::vector<Channel> channels;
    for (size_t i = 0; i < song.tracks.size(); ++i) {
        const Track& t = song.tracks[i];
        Channel c;
        if (i < channels_.size() && channels_[i].instrumentName == t.instrument) {
            c = std::move(channels_[i]);
        } else {
            c.instrument = InstrumentRegistry::instance().create(t.instrument);
            if (!c.instrument) c.instrument = InstrumentRegistry::instance().create("Piano");
            c.instrument->setSampleRate(float(sampleRate_));
            c.instrumentName = t.instrument;
        }
        c.volume = t.volume;
        c.muted = t.muted;
        c.solo = t.solo;
        c.pan = std::clamp(t.pan, -1.f, 1.f);
        c.reverb = std::clamp(t.reverb, 0.f, 1.f);
        c.delay = std::clamp(t.delay, 0.f, 1.f);
        c.expression = t.expression;
        channels.push_back(std::move(c));
    }
    channels_ = std::move(channels);
    updateSolo();
    ticksPerBeat_ = song.ticksPerBeat();
    beatsPerBar_ = song.beatsPerBar;
    fx_ = song.fx;
    bpm_ = song.bpm > 0 ? song.bpm : 120.0;
    updateEffects();
    // Während der Wiedergabe könnten gelöschte Noten sonst hängen bleiben
    if (sequencer_.isPlaying()) seqAllNotesOff();
    sequencer_.setSampleRate(sampleRate_);
    sequencer_.setSong(song);
}

void AudioEngine::play() { std::lock_guard l(mutex_); sequencer_.play(); }
void AudioEngine::pause() { std::lock_guard l(mutex_); sequencer_.pause(*this); }
void AudioEngine::stopPlayback() { std::lock_guard l(mutex_); sequencer_.stop(*this); }
void AudioEngine::seek(double tick) { std::lock_guard l(mutex_); sequencer_.seek(tick, *this); }
void AudioEngine::scrubTo(double tick) { std::lock_guard l(mutex_); sequencer_.setPosition(tick); }
void AudioEngine::setLoop(bool e, Tick s, Tick en) { std::lock_guard l(mutex_); sequencer_.setLoop(e, s, en); }
void AudioEngine::setTempo(double bpm) {
    std::lock_guard l(mutex_);
    sequencer_.setTempo(bpm);
    if (bpm > 0) { bpm_ = bpm; updateEffects(); }
}
bool AudioEngine::isPlaying() { std::lock_guard l(mutex_); return sequencer_.isPlaying(); }

void AudioEngine::setTrackVolume(int track, float volume) {
    std::lock_guard l(mutex_);
    if (track >= 0 && track < int(channels_.size())) channels_[track].volume = volume;
}

void AudioEngine::setTrackMuted(int track, bool muted) {
    std::lock_guard l(mutex_);
    if (track >= 0 && track < int(channels_.size())) {
        channels_[track].muted = muted;
        if (muted) channels_[track].instrument->allNotesOff();
    }
}

void AudioEngine::playWithCountIn(double fromTick, int countInBars) {
    std::lock_guard l(mutex_);
    sequencer_.startAt(std::max(0.0, fromTick) - double(countInBars) * double(ticksPerBeat_ * beatsPerBar_), *this);
}

void AudioEngine::setRecording(bool on) {
    std::lock_guard l(mutex_);
    recording_ = on;
}

std::vector<AudioEngine::RecEvent> AudioEngine::takeRecorded() {
    std::lock_guard l(mutex_);
    std::vector<RecEvent> out;
    out.swap(recorded_);
    return out;
}

void AudioEngine::noteOn(int track, int pitch, float velocity) {
    std::lock_guard lock(mutex_);
    pending_.push_back({Event::On, track, pitch, velocity});
}

void AudioEngine::noteOff(int track, int pitch) {
    std::lock_guard lock(mutex_);
    pending_.push_back({Event::Off, track, pitch, 0});
}

void AudioEngine::allNotesOff() {
    std::lock_guard lock(mutex_);
    pending_.push_back({Event::AllOff, -1, 0, 0});
}

// --- Sequencer::Sink (wird unter mutex_ aufgerufen) ---
void AudioEngine::seqNoteOn(int track, int pitch, float velocity) {
    if (track < 0 || track >= int(channels_.size()) || !audible(channels_[track])) return;
    channels_[track].instrument->noteOn(pitch, velocity);
}

void AudioEngine::seqNoteOff(int track, int pitch) {
    if (track < 0 || track >= int(channels_.size())) return;
    channels_[track].instrument->noteOff(pitch);
}

void AudioEngine::seqAllNotesOff() {
    for (auto& c : channels_) c.instrument->allNotesOff();
}

// --- Audio ---
void AudioEngine::callback(ma_device* device, void* out, const void*, unsigned frameCount) {
    static_cast<AudioEngine*>(device->pUserData)->process(static_cast<float*>(out), int(frameCount));
}

void AudioEngine::process(float* out, int frameCount) {
    std::lock_guard lock(mutex_);
    processLocked(out, frameCount);
}

void AudioEngine::processLocked(float* out, int frameCount) {
    std::memset(out, 0, sizeof(float) * 2 * frameCount);

    for (auto& e : pending_) {
        if (e.type == Event::AllOff) { seqAllNotesOff(); if (liveInst_) liveInst_->allNotesOff(); continue; }
        // Live gespielte Töne (nicht stummgeschaltet, damit man sich beim Aufnehmen hört)
        if (e.track >= 0 && e.track < int(channels_.size())) {
            if (e.type == Event::On) channels_[e.track].instrument->noteOn(e.pitch, e.velocity);
            else channels_[e.track].instrument->noteOff(e.pitch);
        } else if (e.track == kLiveTrack && liveInst_) {   // freies Spielen mit eigenem Instrument
            if (e.type == Event::On) liveInst_->noteOn(e.pitch, e.velocity);
            else liveInst_->noteOff(e.pitch);
        }
        if (recording_ && sequencer_.isPlaying())
            recorded_.push_back({sequencer_.position(), e.pitch, e.velocity, e.type == Event::On});
    }
    pending_.clear();

    if (!click_) {
        click_ = InstrumentRegistry::instance().create("Schlagzeug");
        if (click_) click_->setSampleRate(float(sampleRate_));
    }

    for (int offset = 0; offset < frameCount; offset += kChunk) {
        const int n = std::min(kChunk, frameCount - offset);
        const double before = sequencer_.position();
        sequencer_.advance(n, *this);
        const double after = sequencer_.position();

        // Metronom: Klick auf jedem Schlag (immer im Einzähler, sonst wenn eingeschaltet)
        if (click_ && sequencer_.isPlaying() && after > before && (metronome_ || before < 0)) {
            const double tpb = double(ticksPerBeat_);
            for (long long b = (long long)std::ceil(before / tpb); double(b) * tpb < after; ++b) {
                const bool downbeat = ((b % beatsPerBar_) + beatsPerBar_) % beatsPerBar_ == 0;
                click_->noteOn(downbeat ? 34 : 33, downbeat ? 0.9f : 0.6f);
            }
        }

        float* dst = out + offset * 2;
        const bool playing = sequencer_.isPlaying();
        const float meterDecay = std::exp(-float(n) / (0.35f * float(sampleRate_)));
        float* rev = revBus_.data();
        float* del = delBus_.data();
        std::fill(rev, rev + n * 2, 0.f);
        std::fill(del, del + n * 2, 0.f);
        for (auto& c : channels_) {
            std::fill(scratch_.begin(), scratch_.begin() + n * 2, 0.f);
            c.instrument->render(scratch_.data(), n);
            // Dynamik-Automation nur beim Abspielen; Übergang innerhalb des Blocks gleitend (kein Knacken)
            const float target = (playing ? Track::evalExpression(c.expression, after) : 1.f) * (audible(c) ? c.volume : 0.f);
            // Panorama (gleiche Lautheit): Mitte = beide Seiten 1
            const float pa = (c.pan + 1.f) * 0.7853982f;
            const float panL = std::cos(pa) * 1.4142136f, panR = std::sin(pa) * 1.4142136f;
            float peak = 0.f;
            const float start = c.lastGain;
            const float rs = fx_.on ? c.reverb : 0.f, ds = fx_.on ? c.delay : 0.f;
            for (int i = 0; i < n; ++i) {
                const float g = start + (target - start) * float(i + 1) / float(n);
                const float l = scratch_[2 * i] * g * std::min(1.f, panL), r = scratch_[2 * i + 1] * g * std::min(1.f, panR);
                dst[2 * i] += l;
                dst[2 * i + 1] += r;
                // Effekt-Wege (nach Lautstärke und Panorama: "post fader")
                rev[2 * i] += l * rs;
                rev[2 * i + 1] += r * rs;
                del[2 * i] += l * ds;
                del[2 * i + 1] += r * ds;
                peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
            }
            c.lastGain = target;
            c.meter = std::max(peak, c.meter * meterDecay);
        }
        if (liveInst_) {   // freies Spiel-Instrument: etwas Hall dazu
            std::fill(scratch_.begin(), scratch_.begin() + n * 2, 0.f);
            liveInst_->render(scratch_.data(), n);
            for (int i = 0; i < n * 2; ++i) {
                dst[i] += scratch_[size_t(i)];
                if (fx_.on) rev[i] += scratch_[size_t(i)] * 0.25f;
            }
        }
        // Echo (geht zum Teil auch in den Hall), dann Hall
        if (fx_.on) {
            for (int i = 0; i < n; ++i) {
                float dl, dr;
                delay_.process(del[2 * i], del[2 * i + 1], dl, dr);
                dst[2 * i] += dl * fx_.delayLevel;
                dst[2 * i + 1] += dr * fx_.delayLevel;
                rev[2 * i] += dl * 0.3f;
                rev[2 * i + 1] += dr * 0.3f;
                float wl, wr;
                reverb_.process(rev[2 * i], rev[2 * i + 1], wl, wr);
                dst[2 * i] += wl * fx_.reverbLevel * 2.8f;
                dst[2 * i + 1] += wr * fx_.reverbLevel * 2.8f;
            }
        }
        if (click_) click_->render(dst, n);
    }

    // Master + weicher Limiter
    // Weicher Limiter (feste Aussteuerung), danach die eingestellte Abspiellautstärke
    const float vol = masterVolume.load(std::memory_order_relaxed);
    float mp = 0.f;
    if (fx_.mastering) {
        // Mastering-Kette (EQ, Bus-Kompressor, Breite, Limiter), danach nur noch Sicherheits-Sättigung
        master_.process(out, frameCount);
        for (int i = 0; i < frameCount * 2; ++i) {
            const float x = out[i];   // Limiter hat schon begrenzt: nur ganz oben noch weich abrunden
            out[i] = (std::abs(x) < 0.9f ? x : std::copysign(0.9f + 0.1f * std::tanh((std::abs(x) - 0.9f) * 10.f), x)) * vol;
            mp = std::max(mp, std::abs(out[i]));
        }
    } else {
        for (int i = 0; i < frameCount * 2; ++i) {
            out[i] = std::tanh(out[i] * 0.8f) * vol;
            mp = std::max(mp, std::abs(out[i]));
        }
    }
    masterMeter_ = std::max(mp, masterMeter_ * std::exp(-float(frameCount) / (0.35f * float(sampleRate_))));
}

bool AudioEngine::renderToWav(const std::string& path, double tailSeconds, int sampleRate) {
    if (running_) return false;   // nur ohne laufendes Audiogerät

    ma_encoder_config cfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2,
                                                   static_cast<ma_uint32>(sampleRate));
    ma_encoder encoder;
#ifdef _WIN32
    // UTF-8 -> Wide, damit Umlaute im Pfad funktionieren
    const std::filesystem::path p(std::u8string(path.begin(), path.end()));
    if (ma_encoder_init_file_w(p.wstring().c_str(), &cfg, &encoder) != MA_SUCCESS) return false;
#else
    if (ma_encoder_init_file(path.c_str(), &cfg, &encoder) != MA_SUCCESS) return false;
#endif

    std::lock_guard lock(mutex_);
    sampleRate_ = sampleRate;
    initEffects(sampleRate);
    if (fx_.on) tailSeconds = std::max(tailSeconds, double(reverb_.tailSeconds()));
    sequencer_.setSampleRate(sampleRate);
    for (auto& c : channels_) c.instrument->setSampleRate(float(sampleRate));
    sequencer_.setLoop(false);
    sequencer_.seek(0, *this);
    sequencer_.play();

    std::vector<float> buffer(1024 * 2);
    long long tailFrames = static_cast<long long>(tailSeconds * sampleRate);
    while (sequencer_.isPlaying() || tailFrames > 0) {
        if (!sequencer_.isPlaying()) tailFrames -= 1024;
        processLocked(buffer.data(), 1024);
        ma_encoder_write_pcm_frames(&encoder, buffer.data(), 1024, nullptr);
    }
    ma_encoder_uninit(&encoder);
    sequencer_.seek(0, *this);
    return true;
}

// --- Solo, Panorama, Pegel ---
void AudioEngine::updateSolo() {
    anySolo_ = false;
    for (auto& c : channels_) anySolo_ = anySolo_ || c.solo;
    for (auto& c : channels_)
        if (!audible(c)) c.instrument->allNotesOff();
}

void AudioEngine::setTrackSolo(int track, bool solo) {
    std::lock_guard l(mutex_);
    if (track < 0 || track >= int(channels_.size())) return;
    channels_[size_t(track)].solo = solo;
    updateSolo();
}

void AudioEngine::setTrackPan(int track, float pan) {
    std::lock_guard l(mutex_);
    if (track >= 0 && track < int(channels_.size())) channels_[size_t(track)].pan = std::clamp(pan, -1.f, 1.f);
}

std::vector<float> AudioEngine::trackLevels(float* master) {
    std::lock_guard l(mutex_);
    std::vector<float> out;
    out.reserve(channels_.size());
    for (auto& c : channels_) out.push_back(c.meter);
    if (master) *master = masterMeter_;
    return out;
}

// --- Freies Spielen ---
void AudioEngine::setLiveInstrument(const std::string& name) {
    std::lock_guard l(mutex_);
    if (name == liveName_) return;
    liveName_ = name;
    liveInst_.reset();
    if (name.empty()) return;
    liveInst_ = InstrumentRegistry::instance().create(name);
    if (!liveInst_) { liveName_.clear(); return; }
    liveInst_->setSampleRate(float(sampleRate_));
}

// --- Hall & Echo ---
void AudioEngine::initEffects(int sampleRate) {
    reverb_.setSampleRate(float(sampleRate));
    delay_.setSampleRate(float(sampleRate));
    master_.setSampleRate(float(sampleRate));
    updateEffects();
}

void AudioEngine::updateEffects() {
    reverb_.setParams(fx_.roomSize, fx_.damping, 1.f, fx_.preDelay);
    master_.setAmount(fx_.masterAmount);
    static const double note[] = {1.0, 0.5, 0.75, 1.0 / 3.0, 0.25};   // in Schlägen
    const double beats = note[std::clamp(fx_.delayNote, 0, 4)];
    delay_.setParams(float(60.0 / std::max(20.0, bpm_) * beats), fx_.delayFeedback);
}

void AudioEngine::setEffects(const Effects& fx) {
    std::lock_guard l(mutex_);
    const bool wasOn = fx_.on;
    fx_ = fx;
    updateEffects();
    if (!wasOn && fx_.on) { reverb_.clear(); delay_.clear(); }
}

void AudioEngine::setTrackSends(int track, float reverb, float delay) {
    std::lock_guard l(mutex_);
    if (track < 0 || track >= int(channels_.size())) return;
    channels_[size_t(track)].reverb = std::clamp(reverb, 0.f, 1.f);
    channels_[size_t(track)].delay = std::clamp(delay, 0.f, 1.f);
}

std::vector<float> AudioEngine::renderRange(double fromTick, double toTick, int sampleRate) {
    std::vector<float> out;
    if (running_) return out;
    std::lock_guard lock(mutex_);
    sampleRate_ = sampleRate;
    sequencer_.setSampleRate(sampleRate);
    for (auto& c : channels_) c.instrument->setSampleRate(float(sampleRate));
    initEffects(sampleRate);
    sequencer_.setLoop(false);
    sequencer_.seek(std::max(0.0, fromTick), *this);
    sequencer_.play();
    std::vector<float> buffer(512 * 2);
    for (int guard = 0; guard < 200000 && sequencer_.position() < toTick; ++guard) {
        processLocked(buffer.data(), 512);
        out.insert(out.end(), buffer.begin(), buffer.end());
        if (!sequencer_.isPlaying()) break;
    }
    sequencer_.seek(0, *this);
    return out;
}
