#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------------
//  Raumklang-Effekte für den Effekt-Bus der Engine.
//
//  Reverb: Schroeder/Moorer-Hall nach dem bekannten "Freeverb"-Aufbau (gemeinfrei, Jezar 2000):
//  8 parallele Kammfilter mit gedämpfter Rückkopplung + 4 Allpässe pro Kanal, rechts leicht
//  verstimmt (Stereobreite). Davor Vorverzögerung (Pre-Delay) und Tiefen-Filter, damit der Bass
//  nicht matschig wird.
//
//  Delay: tempo-synchrones Ping-Pong-Echo (links/rechts im Wechsel), in der Rückkopplung
//  höhen- und tiefengefiltert – jede Wiederholung klingt etwas dumpfer, wie bei Bandechos.
// ---------------------------------------------------------------------

class Reverb {
public:
    void setSampleRate(float sr) {
        sr_ = sr;
        const float s = sr / 44100.f;
        static const int comb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int ap[4] = {556, 441, 341, 225};
        constexpr int spread = 23;
        for (int i = 0; i < 8; ++i) {
            combL_[i].init(int(float(comb[i]) * s));
            combR_[i].init(int(float(comb[i] + spread) * s));
        }
        for (int i = 0; i < 4; ++i) {
            apL_[i].init(int(float(ap[i]) * s));
            apR_[i].init(int(float(ap[i] + spread) * s));
        }
        pre_.assign(size_t(sr * 0.2f) + 1, 0.f);
        preW_ = 0;
        update();
    }
    // room 0..1 (Raumgröße/Nachhall), damp 0..1 (Höhendämpfung), width 0..1, preDelay in Sekunden
    void setParams(float room, float damp, float width, float preDelay) {
        room_ = std::clamp(room, 0.f, 1.f);
        damp_ = std::clamp(damp, 0.f, 1.f);
        width_ = std::clamp(width, 0.f, 1.f);
        preDelay_ = std::clamp(preDelay, 0.f, 0.19f);
        update();
    }
    void clear() {
        for (auto& c : combL_) c.clear();
        for (auto& c : combR_) c.clear();
        for (auto& a : apL_) a.clear();
        for (auto& a : apR_) a.clear();
        std::fill(pre_.begin(), pre_.end(), 0.f);
        hp_ = 0.f;
        hpIn_ = 0.f;
    }
    // Ein Stereo-Sample verarbeiten -> nasses (Hall-)Signal
    void process(float inL, float inR, float& outL, float& outR) {
        if (pre_.empty()) { outL = outR = 0.f; return; }
        // Tiefen raus (ca. 180 Hz): der Hall soll Glanz und Raum geben, keinen Bassbrei
        const float x = (inL + inR) * kFixedGain;
        hp_ = hpA_ * (hp_ + x - hpIn_);
        hpIn_ = x;
        // Vorverzögerung
        pre_[preW_] = hp_;
        const size_t n = pre_.size();
        const size_t r = (preW_ + n - preSamples_) % n;
        const float in = pre_[r];
        preW_ = (preW_ + 1) % n;
        float l = 0.f, rr = 0.f;
        for (int i = 0; i < 8; ++i) {
            l += combL_[i].process(in, fb_, d1_, d2_);
            rr += combR_[i].process(in, fb_, d1_, d2_);
        }
        for (int i = 0; i < 4; ++i) {
            l = apL_[i].process(l);
            rr = apR_[i].process(rr);
        }
        outL = l * wet1_ + rr * wet2_;
        outR = rr * wet1_ + l * wet2_;
    }
    // Ungefähre Nachhallzeit in Sekunden (für das Ausklingen beim WAV-Export)
    float tailSeconds() const { return 0.8f + room_ * 4.f; }

private:
    static constexpr float kFixedGain = 0.015f;
    struct Comb {
        std::vector<float> buf;
        size_t pos = 0;
        float store = 0.f;
        void init(int n) { buf.assign(size_t(std::max(1, n)), 0.f); pos = 0; store = 0.f; }
        void clear() { std::fill(buf.begin(), buf.end(), 0.f); store = 0.f; }
        float process(float in, float fb, float d1, float d2) {
            const float out = buf[pos];
            store = out * d2 + store * d1;
            buf[pos] = in + store * fb;
            if (++pos >= buf.size()) pos = 0;
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        size_t pos = 0;
        void init(int n) { buf.assign(size_t(std::max(1, n)), 0.f); pos = 0; }
        void clear() { std::fill(buf.begin(), buf.end(), 0.f); }
        float process(float in) {
            const float b = buf[pos];
            const float out = -in + b;
            buf[pos] = in + b * 0.5f;
            if (++pos >= buf.size()) pos = 0;
            return out;
        }
    };
    void update() {
        fb_ = room_ * 0.28f + 0.7f;
        d1_ = damp_ * 0.4f;
        d2_ = 1.f - d1_;
        wet1_ = 0.5f * (width_ * 0.5f + 0.5f);
        wet2_ = 0.5f * ((1.f - width_) * 0.5f);
        preSamples_ = pre_.empty() ? 0 : std::min(pre_.size() - 1, size_t(preDelay_ * sr_));
        hpA_ = 1.f / (1.f + 2.f * 3.14159265f * 180.f / sr_);
    }
    Comb combL_[8], combR_[8];
    Allpass apL_[4], apR_[4];
    std::vector<float> pre_;
    size_t preW_ = 0, preSamples_ = 0;
    float sr_ = 48000.f, room_ = 0.6f, damp_ = 0.45f, width_ = 1.f, preDelay_ = 0.02f;
    float fb_ = 0.868f, d1_ = 0.18f, d2_ = 0.82f, wet1_ = 0.5f, wet2_ = 0.f;
    float hp_ = 0.f, hpIn_ = 0.f, hpA_ = 0.97f;
};

class PingPongDelay {
public:
    void setSampleRate(float sr) {
        sr_ = sr;
        bufL_.assign(size_t(sr * 2.5f) + 1, 0.f);
        bufR_.assign(bufL_.size(), 0.f);
        pos_ = 0;
        update();
    }
    void setParams(float seconds, float feedback) {
        seconds_ = std::clamp(seconds, 0.02f, 2.4f);
        feedback_ = std::clamp(feedback, 0.f, 0.9f);
        update();
    }
    void clear() {
        std::fill(bufL_.begin(), bufL_.end(), 0.f);
        std::fill(bufR_.begin(), bufR_.end(), 0.f);
        lpL_ = lpR_ = hpL_ = hpR_ = hpInL_ = hpInR_ = 0.f;
    }
    void process(float inL, float inR, float& outL, float& outR) {
        if (bufL_.empty()) { outL = outR = 0.f; return; }
        const size_t n = bufL_.size();
        // gleitende Verzögerungszeit (kein Knacken bei Tempowechsel)
        cur_ += (target_ - cur_) * 0.0005f;
        const float rp = float(pos_) - cur_;
        const float rpos = rp < 0.f ? rp + float(n) : rp;
        const size_t i0 = size_t(rpos) % n, i1 = (i0 + 1) % n;
        const float fr = rpos - std::floor(rpos);
        const float dl = bufL_[i0] + (bufL_[i1] - bufL_[i0]) * fr;
        const float dr = bufR_[i0] + (bufR_[i1] - bufR_[i0]) * fr;
        // Rückkopplung: dumpfer und ohne Tiefen (wie ein Bandecho)
        lpL_ += (dr - lpL_) * lpA_;
        lpR_ += (dl - lpR_) * lpA_;
        hpL_ = hpA_ * (hpL_ + lpL_ - hpInL_); hpInL_ = lpL_;
        hpR_ = hpA_ * (hpR_ + lpR_ - hpInR_); hpInR_ = lpR_;
        // Ping-Pong: Eingang (Mitte) startet links, dann wechseln die Seiten
        bufL_[pos_] = (inL + inR) * 0.5f + hpL_ * feedback_;
        bufR_[pos_] = hpR_ * feedback_;
        pos_ = (pos_ + 1) % n;
        outL = dl;
        outR = dr;
    }

private:
    void update() {
        target_ = std::min(float(bufL_.size()) - 2.f, seconds_ * sr_);
        if (cur_ <= 0.f) cur_ = target_;
        lpA_ = 1.f - std::exp(-2.f * 3.14159265f * 4500.f / sr_);
        hpA_ = 1.f / (1.f + 2.f * 3.14159265f * 200.f / sr_);
    }
    std::vector<float> bufL_, bufR_;
    size_t pos_ = 0;
    float sr_ = 48000.f, seconds_ = 0.25f, feedback_ = 0.35f, target_ = 0.f, cur_ = 0.f;
    float lpL_ = 0.f, lpR_ = 0.f, hpL_ = 0.f, hpR_ = 0.f, hpInL_ = 0.f, hpInR_ = 0.f, lpA_ = 0.4f, hpA_ = 0.97f;
};

// ---------------------------------------------------------------------
//  Mastering-Kette für die Summe (wie im Studio, in dieser Reihenfolge):
//   1. Hochpass 28 Hz       – unhörbares Tieffrequenz-Grummeln weg
//   2. Klang-EQ             – etwas Wärme (Low-Shelf 120 Hz) und Glanz/"Air" (High-Shelf 9 kHz)
//   3. Bus-Kompressor       – "Kleber": hält den Mix zusammen, sanft (weiches Knie, RMS-artig)
//   4. Stereo-Breite        – Seitenanteil leicht anheben (Bass bleibt mittig)
//   5. Limiter              – fängt Spitzen ab (Decke -1 dBFS), kein hartes Clipping
// ---------------------------------------------------------------------
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1L = 0, z2L = 0, z1R = 0, z2R = 0;
    void clear() { z1L = z2L = z1R = z2R = 0; }
    void process(float& l, float& r) {
        const float yl = b0 * l + z1L;
        z1L = b1 * l - a1 * yl + z2L;
        z2L = b2 * l - a2 * yl;
        const float yr = b0 * r + z1R;
        z1R = b1 * r - a1 * yr + z2R;
        z2R = b2 * r - a2 * yr;
        l = yl;
        r = yr;
    }
    // Formeln nach dem "Audio EQ Cookbook" (R. Bristow-Johnson)
    void highpass(float f, float q, float sr) {
        const float w = 2.f * 3.14159265f * f / sr, c = std::cos(w), al = std::sin(w) / (2.f * q);
        const float a0 = 1.f + al;
        b0 = (1.f + c) / 2.f / a0; b1 = -(1.f + c) / a0; b2 = b0;
        a1 = -2.f * c / a0; a2 = (1.f - al) / a0;
    }
    void shelf(bool high, float f, float dB, float sr) {
        const float A = std::pow(10.f, dB / 40.f), w = 2.f * 3.14159265f * f / sr;
        const float c = std::cos(w), s = std::sin(w), al = s / 2.f * std::sqrt(2.f), sq = 2.f * std::sqrt(A) * al;
        float a0;
        if (high) {
            b0 = A * ((A + 1) + (A - 1) * c + sq); b1 = -2 * A * ((A - 1) + (A + 1) * c); b2 = A * ((A + 1) + (A - 1) * c - sq);
            a0 = (A + 1) - (A - 1) * c + sq; a1 = 2 * ((A - 1) - (A + 1) * c); a2 = (A + 1) - (A - 1) * c - sq;
        } else {
            b0 = A * ((A + 1) - (A - 1) * c + sq); b1 = 2 * A * ((A - 1) - (A + 1) * c); b2 = A * ((A + 1) - (A - 1) * c - sq);
            a0 = (A + 1) + (A - 1) * c + sq; a1 = -2 * ((A - 1) + (A + 1) * c); a2 = (A + 1) + (A - 1) * c - sq;
        }
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    }
};

class MasterBus {
public:
    void setSampleRate(float sr) {
        sr_ = sr;
        hp_.highpass(28.f, 0.707f, sr);
        update();
        clear();
    }
    // amount 0..1: wie kräftig gemastert wird (Kompression, Wärme/Glanz, Breite, Lautheit)
    void setAmount(float amount) {
        amount_ = std::clamp(amount, 0.f, 1.f);
        update();
    }
    void clear() {
        hp_.clear(); low_.clear(); high_.clear();
        env_ = 0.f; limGain_ = 1.f; sideLp_ = 0.f; sideLpR_ = 0.f;
    }
    float gainReductionDb() const { return grDb_; }

    void process(float* lr, int frames) {
        float maxGr = 0.f;
        for (int i = 0; i < frames; ++i) {
            float l = lr[2 * i], r = lr[2 * i + 1];
            hp_.process(l, r);
            low_.process(l, r);
            high_.process(l, r);
            // Kompressor: Lautheit (RMS) beider Kanäle, mittlere Attack, langsames Release – "Kleber", kein Plattmachen
            const float x = 0.5f * (l * l + r * r);
            const float k = x > env_ ? atk_ : rel_;
            env_ = x + k * (env_ - x);
            const float lvl = 10.f * std::log10(std::max(env_, 1e-12f)) + 3.f;
            float gr = 0.f;   // dB Reduktion
            const float over = lvl - thr_;
            if (over > knee_ / 2.f) gr = over * (1.f - 1.f / ratio_);
            else if (over > -knee_ / 2.f) {
                const float o = over + knee_ / 2.f;
                gr = (1.f - 1.f / ratio_) * o * o / (2.f * knee_);
            }
            maxGr = std::max(maxGr, gr);
            const float g = std::pow(10.f, (makeup_ - gr) / 20.f);
            l *= g;
            r *= g;
            // Stereo-Breite: nur Seitenanteil oberhalb ~200 Hz verstärken (Bass bleibt mittig)
            const float mid = 0.5f * (l + r), side = 0.5f * (l - r);
            sideLp_ += (side - sideLp_) * sideA_;
            const float sideHigh = side - sideLp_;
            const float side2 = side + sideHigh * width_;
            l = mid + side2;
            r = mid - side2;
            // Limiter: sofort runter, langsam wieder hoch; Decke -1 dBFS
            const float peak = std::max(std::abs(l), std::abs(r));
            const float need = peak > ceiling_ ? ceiling_ / peak : 1.f;
            limGain_ = need < limGain_ ? need : limGain_ + (1.f - limGain_) * limRel_;
            l *= limGain_;
            r *= limGain_;
            lr[2 * i] = l;
            lr[2 * i + 1] = r;
        }
        grDb_ = maxGr;
    }

private:
    void update() {
        const float a = amount_;
        low_.shelf(false, 120.f, 1.2f * a, sr_);
        high_.shelf(true, 9000.f, 2.4f * a, sr_);
        thr_ = -14.f - 6.f * a;           // je mehr Mastering, desto früher greift der Kompressor
        ratio_ = 1.25f + 0.75f * a;
        knee_ = 8.f;
        makeup_ = 1.f + 2.5f * a;         // Lautheit zurückgewinnen (maßvoll: die Dynamik soll bleiben)
        width_ = 0.35f * a;
        atk_ = std::exp(-1.f / (0.03f * sr_));
        rel_ = std::exp(-1.f / (0.35f * sr_));
        limRel_ = 1.f - std::exp(-1.f / (0.08f * sr_));
        sideA_ = 1.f - std::exp(-2.f * 3.14159265f * 200.f / sr_);
        ceiling_ = 0.89f;
    }
    Biquad hp_, low_, high_;
    float sr_ = 48000.f, amount_ = 0.5f;
    float thr_ = -17.f, ratio_ = 2.25f, knee_ = 8.f, makeup_ = 4.25f, width_ = 0.17f;
    float atk_ = 0.998f, rel_ = 0.9999f, limRel_ = 0.0003f, sideA_ = 0.026f, ceiling_ = 0.89f;
    float env_ = 0.f, limGain_ = 1.f, sideLp_ = 0.f, sideLpR_ = 0.f, grDb_ = 0.f;
};
