// Digiwave - an 8-voice digital-wave polysynth for the MPC, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// Inspired by the architecture of 1980s "digital oscillator + analogue filter" polysynths
// (two single-cycle wave oscillators per voice, a resonant 24 dB low-pass, break-point
// envelopes, a digital delay). Everything is written from scratch: the waveforms are
// built here from harmonic recipes, nothing is copied from any instrument's ROM.
//
// Signal flow, per voice (8 voices):
//   osc 1 (wave, octave) + osc 2 (wave, octave, interval, detune) + noise
//     -> two cascaded 12 dB state-variable low-passes = 24 dB/oct (resonance on the second)
//        cutoff = Cutoff * 2^( EG Int * VCF EG + Key Track + LFO Filter )
//     -> VCA EG * velocity
//   all voices -> master volume -> stereo delay (can be modulated for chorus/flange)
//
// Envelopes ("ADSBR"): Attack to full level, Decay down to the Break level, Slope from
// Break to Sustain (up or down), hold while the key is down, Release.
//
// Waveforms are 2048-point single cycles built by additive synthesis, one band-limited
// copy per octave so nothing aliases at any pitch; they are built in prepare().

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/svf.h"
#include "dsp/smoother.h"
#include "dsp/voice.h"
#include "dsp/lfo.h"
#include "dsp/noise.h"
#include "dsp/delay_line.h"
#include "dsp/midi_cc.h"

#include <vector>

using namespace mpc::dsp;

namespace {

// ---------------------------------------------------------------------------
// Break-point envelope: Attack -> Decay (to Break) -> Slope (to Sustain) -> Release
// ---------------------------------------------------------------------------
class BreakEG
{
public:
    void set(float attackMs, float decayMs, float breakLevel, float slopeMs, float sustain, float releaseMs, double sr)
    {
        brk_ = clamp(breakLevel, 0.0f, 1.0f); sus_ = clamp(sustain, 0.0f, 1.0f);
        aInc_ = 1.0f / fmaxf(1.0f, attackMs * 0.001f * (float)sr);
        dInc_ = (1.0f - brk_) / fmaxf(1.0f, decayMs * 0.001f * (float)sr);
        sInc_ = std::fabs(brk_ - sus_) / fmaxf(1.0f, slopeMs * 0.001f * (float)sr);
        rCoef_ = std::exp(-6.9078f / fmaxf(1.0f, releaseMs * 0.001f * (float)sr));   // -60 dB over the release time
    }
    void noteOn() { stage_ = Stage::Attack; }
    void noteOff() { if (stage_ != Stage::Idle) stage_ = Stage::Release; }
    void reset() { stage_ = Stage::Idle; level_ = 0.0f; }
    bool isActive() const { return stage_ != Stage::Idle; }

    float process()
    {
        switch (stage_)
        {
            case Stage::Idle: break;
            case Stage::Attack:  level_ += aInc_; if (level_ >= 1.0f) { level_ = 1.0f; stage_ = Stage::Decay; } break;
            case Stage::Decay:   level_ -= dInc_; if (level_ <= brk_) { level_ = brk_; stage_ = Stage::Slope; } break;
            case Stage::Slope:
                if (std::fabs(sus_ - level_) <= sInc_) { level_ = sus_; stage_ = Stage::Sustain; }
                else level_ += (sus_ > level_) ? sInc_ : -sInc_;
                break;
            case Stage::Sustain: level_ = sus_; break;
            case Stage::Release: level_ *= rCoef_; if (level_ < 1.0e-4f) { level_ = 0.0f; stage_ = Stage::Idle; } break;
        }
        return level_;
    }

private:
    static float fmaxf(float a, float b) { return a > b ? a : b; }
    enum class Stage { Idle, Attack, Decay, Slope, Sustain, Release };
    Stage stage_ = Stage::Idle;
    float level_ = 0.0f, brk_ = 0.6f, sus_ = 0.4f, aInc_ = 0.01f, dInc_ = 0.001f, sInc_ = 0.001f, rCoef_ = 0.999f;
};

// ---------------------------------------------------------------------------
// The wavetable bank: 12 waveforms x 10 octave-band copies of 2048 points
// ---------------------------------------------------------------------------
constexpr int kTableSize = 2048;
constexpr int kNumWaves = 12;
constexpr int kNumMips = 10;                // mip m is used for fundamentals in [27.5 * 2^m, 27.5 * 2^(m+1))
constexpr int kMaxHarmonics = 64;

const char* const kWaveNames[kNumWaves] = {
    "Saw", "Square", "Pulse", "Triangle", "Organ", "E-Piano", "Clav", "Brass", "Strings", "Voice", "Bell", "Sine"
};

// Amplitude (signed) of harmonic h (1-based) for each waveform.
float harmonicAmp(int wave, int h)
{
    const float fh = (float)h;
    switch (wave)
    {
        case 0:  return 1.0f / fh;                                                            // saw
        case 1:  return (h & 1) ? 1.0f / fh : 0.0f;                                           // square
        case 2:  return std::sin(kPi * fh * 0.25f) / fh;                                       // 25% pulse
        case 3:  return (h & 1) ? ((((h - 1) / 2) & 1) ? -1.0f : 1.0f) / (fh * fh) : 0.0f;   // triangle
        case 4:  { static const float d[9] = {0, 1.0f, 0.8f, 0.55f, 0.5f, 0.0f, 0.4f, 0.0f, 0.3f}; return h <= 8 ? d[h] : 0.0f; }   // organ drawbars
        case 5:  { static const float d[9] = {0, 1.0f, 0.45f, 0.10f, 0.28f, 0.04f, 0.12f, 0.02f, 0.05f}; return h <= 8 ? d[h] : 0.0f; }  // electric piano
        case 6:  return (h % 4 == 0) ? 0.0f : 1.0f / std::sqrt(fh);                            // clavinet: bright, comb-like
        case 7:  return (1.0f / std::pow(fh, 0.7f)) * (1.0f + 1.6f * std::exp(-sq(fh - 4.0f) / 6.0f));   // brass: formant around 4
        case 8:  return 1.0f / std::pow(fh, 1.15f) * (h == 2 ? 1.3f : 1.0f);                   // strings: soft saw
        case 9:  return (1.0f / fh) * (std::exp(-sq(fh - 3.0f) / 2.0f) + 0.7f * std::exp(-sq(fh - 9.0f) / 5.0f) + 0.08f);   // voice: two formants
        case 10: { static const float d[13] = {0, 0.8f, 0.0f, 0.5f, 0.0f, 0.4f, 0.0f, 0.0f, 0.3f, 0.0f, 0.0f, 0.25f, 0.0f}; return h <= 12 ? d[h] : 0.0f; }  // bell
        default: return h == 1 ? 1.0f : 0.0f;                                                  // sine
    }
}

class WaveBank
{
public:
    void build(double sampleRate)
    {
        if (built_ && builtSr_ == sampleRate) return;
        // One master sine so the additive loop is integer indexing only (fast to build on the MPC).
        std::vector<float> sine((size_t)kTableSize);
        for (int i = 0; i < kTableSize; ++i) sine[(size_t)i] = std::sin(kTwoPi * (float)i / (float)kTableSize);
        data_.assign((size_t)kNumWaves * kNumMips * (kTableSize + 1), 0.0f);

        for (int w = 0; w < kNumWaves; ++w)
        {
            float norm = 1.0f;
            for (int m = 0; m < kNumMips; ++m)
            {
                const float topHz = 27.5f * (float)(1 << (m + 1));
                int maxH = (int)(0.45f * (float)sampleRate / topHz);
                if (maxH > kMaxHarmonics) maxH = kMaxHarmonics;
                if (maxH < 1) maxH = 1;
                float* t = &data_[index(w, m)];
                for (int h = 1; h <= maxH; ++h)
                {
                    const float a = harmonicAmp(w, h);
                    if (a == 0.0f) continue;
                    for (int i = 0; i < kTableSize; ++i) t[i] += a * sine[(size_t)((h * i) & (kTableSize - 1))];
                }
                if (m == 0)
                {
                    float pk = 1.0e-6f;
                    for (int i = 0; i < kTableSize; ++i) pk = std::fmax(pk, std::fabs(t[i]));
                    norm = 1.0f / pk;       // all copies of one wave share the factor from the fullest one
                }
                for (int i = 0; i < kTableSize; ++i) t[i] *= norm;
                t[kTableSize] = t[0];       // guard point for interpolation
            }
        }
        built_ = true; builtSr_ = sampleRate;
    }

    // Table for a wave at a given pitch.
    const float* table(int wave, float hz) const
    {
        int m = 0;
        if (hz > 27.5f) { m = (int)std::floor(std::log2(hz / 27.5f)); if (m > kNumMips - 1) m = kNumMips - 1; }
        return &data_[index(clamp(wave, 0, kNumWaves - 1), m)];
    }
    bool ready() const { return built_; }

private:
    static size_t index(int w, int m) { return ((size_t)w * kNumMips + (size_t)m) * (kTableSize + 1); }
    std::vector<float> data_;
    bool built_ = false;
    double builtSr_ = 0.0;
};

inline float readTable(const float* t, float phase01)
{
    const float p = phase01 * (float)kTableSize;
    const int i = (int)p;
    const float f = p - (float)i;
    return t[i] + f * (t[i + 1] - t[i]);
}

} // namespace

class Digiwave : public mpc::Plugin
{
public:
    // Parameter order is permanent once users have projects: append new ones at the end.
    // Parameter 0 is nudged by the host on every load, so it is the harmless master volume.
    enum Params
    {
        kVolume = 0,
        kWave1, kOct1, kWave2, kOct2, kInterval, kDetune, kOsc2Level, kNoise,
        kCutoff, kReso, kKeyTrack, kEgInt,
        kFAttack, kFDecay, kFBreak, kFSlope, kFSustain, kFRelease,
        kAAttack, kADecay, kABreak, kASlope, kASustain, kARelease, kVelocity,
        kLfoWave, kLfoRate, kLfoDelay, kLfoPitch, kLfoFilter, kBend,
        kDelayTime, kDelayFeedback, kDelayMod, kDelayMix,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Digiwave";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', '2', 's', 'K');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Digiwave() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        std::vector<std::string> waves;
        for (int i = 0; i < kNumWaves; ++i) waves.push_back(kWaveNames[i]);

        addParam(floatParam ("volume",  "Volume",       -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("wave1",   "Wave 1",       waves, 0));
        addParam(intParam   ("oct1",    "Octave 1",    -2, 2, 0));
        addParam(choiceParam("wave2",   "Wave 2",       waves, 0));
        addParam(intParam   ("oct2",    "Octave 2",    -2, 2, 0));
        addParam(intParam   ("interval", "Interval",   -12, 12, 0, "st"));
        addParam(floatParam ("detune",  "Detune",      -50.0f, 50.0f, 6.0f, "ct", 0));
        addParam(floatParam ("osc2lvl", "Osc2 Level",   0.0f, 100.0f, 70.0f, "%", 0));
        addParam(floatParam ("noise",   "Noise",        0.0f, 100.0f, 0.0f, "%", 0));

        addParam(logParam   ("cutoff",  "Cutoff",       30.0f, 16000.0f, 2500.0f, "Hz", 0));
        addParam(floatParam ("reso",    "Resonance",    0.0f, 100.0f, 15.0f, "%", 0));
        addParam(floatParam ("keytrk",  "Key Track",    0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("egint",   "EG Int",      -100.0f, 100.0f, 40.0f, "%", 0));

        addParam(logParam   ("fatk",    "VCF Attack",   1.0f, 5000.0f, 5.0f, "ms", 0));
        addParam(logParam   ("fdec",    "VCF Decay",    1.0f, 5000.0f, 400.0f, "ms", 0));
        addParam(floatParam ("fbrk",    "VCF Break",    0.0f, 100.0f, 60.0f, "%", 0));
        addParam(logParam   ("fslp",    "VCF Slope",    1.0f, 5000.0f, 600.0f, "ms", 0));
        addParam(floatParam ("fsus",    "VCF Sustain",  0.0f, 100.0f, 40.0f, "%", 0));
        addParam(logParam   ("frel",    "VCF Release",  1.0f, 5000.0f, 250.0f, "ms", 0));

        addParam(logParam   ("aatk",    "VCA Attack",   1.0f, 5000.0f, 4.0f, "ms", 0));
        addParam(logParam   ("adec",    "VCA Decay",    1.0f, 5000.0f, 300.0f, "ms", 0));
        addParam(floatParam ("abrk",    "VCA Break",    0.0f, 100.0f, 80.0f, "%", 0));
        addParam(logParam   ("aslp",    "VCA Slope",    1.0f, 5000.0f, 500.0f, "ms", 0));
        addParam(floatParam ("asus",    "VCA Sustain",  0.0f, 100.0f, 70.0f, "%", 0));
        addParam(logParam   ("arel",    "VCA Release",  1.0f, 5000.0f, 250.0f, "ms", 0));
        addParam(floatParam ("velsens", "Velocity",     0.0f, 100.0f, 60.0f, "%", 0));

        addParam(choiceParam("lfowave", "LFO Wave",     LFO::shapeNames(), 0));
        addParam(logParam   ("lforate", "LFO Rate",     0.05f, 30.0f, 5.0f, "Hz", 2));
        addParam(floatParam ("lfodly",  "LFO Delay",    0.0f, 2000.0f, 0.0f, "ms", 0, 0.5f));
        addParam(floatParam ("lfopitch", "LFO Pitch",   0.0f, 12.0f, 0.0f, "st", 2, 0.5f));
        addParam(floatParam ("lfofilt", "LFO Filter",   0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam   ("bend",    "Bend Range",   0, 12, 2, "st"));

        addParam(logParam   ("dlytime", "Delay Time",   1.0f, 600.0f, 280.0f, "ms", 0));
        addParam(floatParam ("dlyfb",   "Delay Fback",  0.0f, 90.0f, 25.0f, "%", 0));
        addParam(floatParam ("dlymod",  "Delay Mod",    0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("dlymix",  "Delay Mix",    0.0f, 100.0f, 0.0f, "%", 0));

        if (numParams() != kNumParams) mpc::log("Digiwave: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        enum { Saw, Square, Pulse, Tri, Organ, EPiano, Clav, Brass, Strings, Vox, Bell, Sine };   // wave indices
        program("Init Saw",     {});
        program("Brass Stab",   {{kWave1, Brass}, {kWave2, Saw}, {kDetune, 8}, {kCutoff, 700}, {kReso, 20}, {kEgInt, 70},
                                 {kFAttack, 40}, {kFDecay, 250}, {kFBreak, 55}, {kFSlope, 500}, {kFSustain, 45},
                                 {kAAttack, 12}, {kASustain, 80}, {kARelease, 120}});
        program("Digital E.Piano", {{kWave1, EPiano}, {kWave2, Bell}, {kOct2, 1}, {kOsc2Level, 25}, {kDetune, 3}, {kCutoff, 4200}, {kReso, 5},
                                 {kEgInt, 25}, {kFDecay, 600}, {kFBreak, 30}, {kFSlope, 1500}, {kFSustain, 15}, {kKeyTrack, 80},
                                 {kADecay, 700}, {kABreak, 55}, {kASlope, 2500}, {kASustain, 0}, {kARelease, 300}, {kVelocity, 85},
                                 {kDelayMix, 18}, {kDelayTime, 340}, {kDelayFeedback, 30}});
        program("Warm Strings", {{kWave1, Strings}, {kWave2, Saw}, {kDetune, 11}, {kOsc2Level, 85}, {kCutoff, 1600}, {kReso, 8}, {kEgInt, 20},
                                 {kFAttack, 600}, {kFDecay, 900}, {kFBreak, 80}, {kFSustain, 65}, {kAAttack, 450}, {kADecay, 800},
                                 {kABreak, 90}, {kASustain, 85}, {kARelease, 900}, {kLfoPitch, 0.15f}, {kLfoRate, 5.5f}, {kLfoDelay, 700},
                                 {kDelayTime, 18}, {kDelayMod, 55}, {kDelayFeedback, 15}, {kDelayMix, 40}});
        program("Bell Keys",    {{kWave1, Bell}, {kWave2, Sine}, {kOct2, 2}, {kOsc2Level, 35}, {kDetune, 0}, {kCutoff, 6500}, {kReso, 5}, {kEgInt, 15},
                                 {kADecay, 1200}, {kABreak, 40}, {kASlope, 3500}, {kASustain, 0}, {kARelease, 1500}, {kVelocity, 80},
                                 {kDelayMix, 30}, {kDelayTime, 380}, {kDelayFeedback, 40}});
        program("Clav Funk",    {{kWave1, Clav}, {kWave2, Pulse}, {kOsc2Level, 40}, {kDetune, 2}, {kCutoff, 900}, {kReso, 25}, {kEgInt, 80},
                                 {kKeyTrack, 100}, {kFDecay, 150}, {kFBreak, 15}, {kFSlope, 300}, {kFSustain, 5},
                                 {kADecay, 220}, {kABreak, 40}, {kASlope, 600}, {kASustain, 0}, {kARelease, 80}, {kVelocity, 90}});
        program("Drawbar Organ", {{kWave1, Organ}, {kWave2, Organ}, {kOct2, 1}, {kOsc2Level, 50}, {kDetune, 0}, {kCutoff, 5500}, {kReso, 0},
                                 {kEgInt, 0}, {kAAttack, 3}, {kABreak, 100}, {kASustain, 100}, {kARelease, 40}, {kVelocity, 20},
                                 {kLfoPitch, 0.1f}, {kLfoRate, 6.5f}});
        program("Choir Pad",    {{kWave1, Vox}, {kWave2, Tri}, {kDetune, 9}, {kOsc2Level, 60}, {kCutoff, 2400}, {kReso, 10}, {kEgInt, 10},
                                 {kFAttack, 700}, {kAAttack, 800}, {kABreak, 100}, {kASustain, 90}, {kARelease, 1400},
                                 {kDelayTime, 22}, {kDelayMod, 70}, {kDelayMix, 45}});
        program("Digital Bass", {{kWave1, Square}, {kWave2, Saw}, {kOct1, -1}, {kOct2, -1}, {kDetune, 5}, {kOsc2Level, 60}, {kCutoff, 380}, {kReso, 35},
                                 {kEgInt, 75}, {kKeyTrack, 40}, {kFDecay, 140}, {kFBreak, 20}, {kFSlope, 400}, {kFSustain, 10},
                                 {kADecay, 400}, {kABreak, 70}, {kASustain, 60}, {kARelease, 90}});
        program("Sync Lead",    {{kWave1, Saw}, {kWave2, Square}, {kInterval, 7}, {kDetune, 7}, {kOsc2Level, 55}, {kCutoff, 3200}, {kReso, 40},
                                 {kEgInt, 35}, {kLfoPitch, 0.25f}, {kLfoRate, 5.0f}, {kLfoDelay, 450}, {kASustain, 85},
                                 {kDelayTime, 330}, {kDelayFeedback, 35}, {kDelayMix, 22}});
        program("Analog Brass Pad", {{kWave1, Brass}, {kWave2, Saw}, {kDetune, 10}, {kCutoff, 1100}, {kReso, 15}, {kEgInt, 50},
                                 {kFAttack, 350}, {kFDecay, 600}, {kFBreak, 70}, {kFSustain, 55}, {kAAttack, 250}, {kABreak, 95},
                                 {kASustain, 85}, {kARelease, 600}});
        program("Glass Keys",   {{kWave1, Bell}, {kWave2, EPiano}, {kOct2, 1}, {kOsc2Level, 40}, {kCutoff, 7000}, {kReso, 5}, {kEgInt, 15},
                                 {kADecay, 900}, {kABreak, 35}, {kASlope, 3000}, {kASustain, 0}, {kARelease, 800}, {kVelocity, 80},
                                 {kDelayTime, 360}, {kDelayFeedback, 35}, {kDelayMix, 22}});
        program("Soft Organ",   {{kWave1, Organ}, {kWave2, Sine}, {kOct2, 1}, {kOsc2Level, 35}, {kDetune, 0}, {kCutoff, 3800}, {kReso, 0},
                                 {kEgInt, 0}, {kAAttack, 12}, {kABreak, 100}, {kASustain, 100}, {kARelease, 90}, {kVelocity, 15},
                                 {kLfoPitch, 0.08f}, {kLfoRate, 6.0f}, {kDelayTime, 12}, {kDelayMod, 50}, {kDelayMix, 25}});
        program("Reed Lead",    {{kWave1, Brass}, {kWave2, Pulse}, {kDetune, 6}, {kOsc2Level, 50}, {kCutoff, 1900}, {kReso, 25}, {kEgInt, 25},
                                 {kLfoPitch, 0.2f}, {kLfoRate, 5.5f}, {kLfoDelay, 500}, {kASustain, 90}, {kARelease, 150}});
        program("Space Choir",  {{kWave1, Vox}, {kWave2, Strings}, {kDetune, 12}, {kOsc2Level, 70}, {kCutoff, 2800}, {kReso, 8}, {kEgInt, 10},
                                 {kFAttack, 900}, {kAAttack, 900}, {kABreak, 100}, {kASustain, 90}, {kARelease, 2000},
                                 {kDelayTime, 420}, {kDelayFeedback, 45}, {kDelayMix, 35}});
        program("Clav Wah",     {{kWave1, Clav}, {kWave2, Clav}, {kInterval, 12}, {kOsc2Level, 35}, {kDetune, 3}, {kCutoff, 500}, {kReso, 45},
                                 {kEgInt, 90}, {kKeyTrack, 80}, {kFDecay, 120}, {kFBreak, 10}, {kFSlope, 250}, {kFSustain, 5},
                                 {kADecay, 200}, {kABreak, 40}, {kASlope, 500}, {kASustain, 0}, {kARelease, 70}, {kVelocity, 90}});
        program("Tine Piano",   {{kWave1, EPiano}, {kWave2, Sine}, {kOct2, 1}, {kOsc2Level, 30}, {kDetune, 2}, {kCutoff, 4800}, {kReso, 3},
                                 {kEgInt, 20}, {kFDecay, 500}, {kFBreak, 30}, {kFSlope, 1200}, {kFSustain, 10}, {kKeyTrack, 85},
                                 {kADecay, 650}, {kABreak, 50}, {kASlope, 2200}, {kASustain, 0}, {kARelease, 280}, {kVelocity, 85},
                                 {kDelayMix, 15}, {kDelayTime, 300}, {kDelayFeedback, 25}});
        program("Marimba",      {{kWave1, Sine}, {kWave2, Tri}, {kOct2, 2}, {kOsc2Level, 25}, {kDetune, 0}, {kCutoff, 3000}, {kReso, 0},
                                 {kEgInt, 30}, {kFDecay, 200}, {kFBreak, 20}, {kFSlope, 300}, {kFSustain, 5}, {kADecay, 350},
                                 {kABreak, 30}, {kASlope, 600}, {kASustain, 0}, {kARelease, 150}, {kVelocity, 90}});
        program("Super Saws",   {{kWave1, Saw}, {kWave2, Saw}, {kDetune, 14}, {kOsc2Level, 100}, {kCutoff, 3000}, {kReso, 10}, {kEgInt, 30},
                                 {kFAttack, 40}, {kFDecay, 500}, {kFBreak, 70}, {kFSustain, 60}, {kAAttack, 20}, {kABreak, 100},
                                 {kASustain, 90}, {kARelease, 250}, {kDelayTime, 16}, {kDelayMod, 45}, {kDelayMix, 30}});
        program("Hollow Square", {{kWave1, Square}, {kWave2, Pulse}, {kDetune, 5}, {kOsc2Level, 60}, {kCutoff, 2200}, {kReso, 20}, {kEgInt, 25},
                                 {kFDecay, 350}, {kFBreak, 50}, {kFSustain, 35}, {kASustain, 80}, {kARelease, 150}});
        program("Whistle",      {{kWave1, Sine}, {kWave2, Sine}, {kOct2, 1}, {kOsc2Level, 15}, {kDetune, 3}, {kCutoff, 6000}, {kEgInt, 0},
                                 {kLfoPitch, 0.3f}, {kLfoRate, 5.5f}, {kLfoDelay, 800}, {kAAttack, 30}, {kABreak, 100}, {kASustain, 90},
                                 {kARelease, 200}});
        program("Chorus Strings", {{kWave1, Strings}, {kWave2, Strings}, {kDetune, 9}, {kOsc2Level, 80}, {kCutoff, 2000}, {kReso, 5}, {kEgInt, 15},
                                 {kFAttack, 400}, {kAAttack, 380}, {kABreak, 100}, {kASustain, 90}, {kARelease, 900},
                                 {kDelayTime, 14}, {kDelayMod, 60}, {kDelayFeedback, 10}, {kDelayMix, 50}});
        program("Resonant Sweep", {{kWave1, Saw}, {kWave2, Saw}, {kDetune, 8}, {kCutoff, 300}, {kReso, 70}, {kEgInt, 80}, {kFAttack, 900},
                                 {kFDecay, 1500}, {kFBreak, 90}, {kFSlope, 2000}, {kFSustain, 20}, {kAAttack, 100}, {kABreak, 100},
                                 {kASustain, 90}, {kARelease, 800}});
        program("Fifth Lead",   {{kWave1, Saw}, {kWave2, Saw}, {kInterval, 7}, {kDetune, 3}, {kOsc2Level, 60}, {kCutoff, 2800}, {kReso, 30},
                                 {kEgInt, 30}, {kLfoPitch, 0.2f}, {kLfoRate, 5.0f}, {kLfoDelay, 400}, {kASustain, 85}, {kARelease, 180},
                                 {kDelayTime, 320}, {kDelayFeedback, 30}, {kDelayMix, 20}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = sampleRate;
        bank_.build(sampleRate);            // allocates: prepare() only
        for (auto& v : voices_) { v.f1.setSampleRate(sampleRate); v.f2.setSampleRate(sampleRate); }
        lfo_.setSampleRate(sampleRate);
        controllers_.prepare(sampleRate, 10.0f);
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
        const int maxDelay = (int)(sampleRate * 0.62) + 8;
        dlyL_.prepare(maxDelay); dlyR_.prepare(maxDelay);
    }

    void reset() override
    {
        for (auto& v : voices_) { v.vcf.reset(); v.vca.reset(); v.f1.reset(); v.f2.reset(); v.note = -1; }
        alloc_.reset(); lfo_.reset(); controllers_.reset();
        dlyL_.reset(); dlyR_.reset(); modPhase_ = 0.0f;
        volume_.snap(dbToGain(get(kVolume)));
    }

    // ----- audio ------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        if (!bank_.ready()) bank_.build(ctx.sampleRate);   // only if a host skipped prepare()
        readSettings(ctx);

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(ctx, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            handleMidi(ev);
        }
        render(ctx, pos, ctx.numFrames);
        applyDelayAndVolume(ctx);
    }

private:
    static constexpr int kNumVoices = 8;
    static constexpr int kChunk = 64;
    static constexpr int kCtrlInterval = 16;

    struct Voice
    {
        float phase1 = 0.0f, phase2 = 0.0f, inc1 = 0.0f, inc2 = 0.0f;
        const float* tab1 = nullptr; const float* tab2 = nullptr;
        SVF f1, f2;
        BreakEG vcf, vca;
        int note = -1, ctrl = 0;
        float velGain = 1.0f;
        uint32_t lfoAge = 0;
    };

    struct Settings
    {
        int wave1 = 0, wave2 = 0, oct1 = 0, oct2 = 0, interval = 0;
        float detune = 6.0f, lvl2 = 0.7f, noise = 0.0f, cutoff = 2500.0f, reso = 0.15f, keyTrack = 0.5f, egInt = 0.4f;
        float velSens = 0.6f, lfoPitch = 0.0f, lfoFilter = 0.0f, lfoDelaySamples = 0.0f;
    } s_;

    void readSettings(mpc::ProcessContext& ctx)
    {
        s_.wave1 = getInt(kWave1); s_.wave2 = getInt(kWave2); s_.oct1 = getInt(kOct1); s_.oct2 = getInt(kOct2);
        s_.interval = getInt(kInterval); s_.detune = get(kDetune); s_.lvl2 = get(kOsc2Level) * 0.01f; s_.noise = get(kNoise) * 0.01f;
        s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f; s_.keyTrack = get(kKeyTrack) * 0.01f; s_.egInt = get(kEgInt) * 0.01f;
        s_.velSens = get(kVelocity) * 0.01f; s_.lfoPitch = get(kLfoPitch); s_.lfoFilter = get(kLfoFilter) * 0.01f;
        s_.lfoDelaySamples = get(kLfoDelay) * 0.001f * (float)ctx.sampleRate;

        alloc_.setBendRange((float)getInt(kBend));
        volume_.setTarget(dbToGain(get(kVolume)));
        lfo_.setShape(getInt(kLfoWave)); lfo_.setFrequency(get(kLfoRate));

        const double sr = ctx.sampleRate;
        for (auto& v : voices_)
        {
            v.vcf.set(get(kFAttack), get(kFDecay), get(kFBreak) * 0.01f, get(kFSlope), get(kFSustain) * 0.01f, get(kFRelease), sr);
            v.vca.set(get(kAAttack), get(kADecay), get(kABreak) * 0.01f, get(kASlope), get(kASustain) * 0.01f, get(kARelease), sr);
        }
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn())
        {
            const int vi = alloc_.noteOn(ev.note(), ev.velocity());
            Voice& v = voices_[vi];
            v.note = ev.note();
            v.velGain = lerp(1.0f, ev.velocity() / 127.0f, s_.velSens);
            if (!v.vca.isActive()) { v.phase1 = 0.0f; v.phase2 = 0.25f; v.f1.reset(); v.f2.reset(); }
            v.vcf.noteOn(); v.vca.noteOn();
            v.ctrl = 0; v.lfoAge = 0;
        }
        else if (ev.isNoteOff())
        {
            const int vi = alloc_.noteOff(ev.note());
            if (vi >= 0) release(vi);
        }
        else if (ev.isAllNotesOff()) alloc_.allNotesOff([this](int vi) { release(vi); });
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            controllers_.handle(ev);
            alloc_.setSustain(ev.controllerValue() >= 64, [this](int vi) { release(vi); });
        }
        else if (ev.isPitchBend()) alloc_.setPitchBend(ev.pitchBend());
        else controllers_.handle(ev);
    }

    void release(int vi) { voices_[vi].vcf.noteOff(); voices_[vi].vca.noteOff(); }

    void render(mpc::ProcessContext& ctx, int from, int to)
    {
        float* out = ctx.outputs[0];
        for (int i = from; i < to; ++i) out[i] = 0.0f;
        while (from < to)
        {
            const int n = (to - from) < kChunk ? (to - from) : kChunk;
            for (int i = 0; i < n; ++i) { controllers_.tick(); lfoBuf_[i] = lfo_.process(); }
            const float vib = s_.lfoPitch + 2.0f * controllers_.modWheel();   // the mod wheel adds up to 2 semitones of vibrato

            for (int vi = 0; vi < kNumVoices; ++vi)
            {
                Voice& v = voices_[vi];
                if (!v.vca.isActive()) continue;
                renderVoice(v, out + from, n, vib);
                if (!v.vca.isActive()) alloc_.voiceFinished(vi);
            }
            from += n;
        }
    }

    void renderVoice(Voice& v, float* out, int n, float vibSemitones)
    {
        const float inv = 1.0f / (float)sr_;
        for (int i = 0; i < n; ++i)
        {
            const float fenv = v.vcf.process();
            const float aenv = v.vca.process();

            if ((v.ctrl++ % kCtrlInterval) == 0)
            {
                // LFO fades in after "LFO Delay".
                const float fade = s_.lfoDelaySamples > 1.0f ? clamp((float)v.lfoAge / s_.lfoDelaySamples, 0.0f, 1.0f) : 1.0f;
                const float lfo = lfoBuf_[i] * fade;
                const float note = (float)v.note + lfo * vibSemitones;
                const float hz1 = alloc_.noteToHz(note + 12.0f * (float)s_.oct1);
                const float hz2 = alloc_.noteToHz(note + 12.0f * (float)s_.oct2 + (float)s_.interval) * centsToRatio(s_.detune);
                v.inc1 = hz1 * inv; v.inc2 = hz2 * inv;
                v.tab1 = bank_.table(s_.wave1, hz1); v.tab2 = bank_.table(s_.wave2, hz2);

                const float octaves = s_.egInt * fenv * 6.0f + s_.keyTrack * ((float)v.note - 60.0f) / 12.0f + s_.lfoFilter * lfo * 3.0f;
                const float cutoff = s_.cutoff * std::pow(2.0f, clamp(octaves, -9.0f, 9.0f));
                v.f1.set(cutoff, 0.0f);
                v.f2.set(cutoff, s_.reso);
                v.lfoAge += kCtrlInterval;
            }

            const float o1 = readTable(v.tab1, v.phase1);
            const float o2 = readTable(v.tab2, v.phase2);
            v.phase1 += v.inc1; if (v.phase1 >= 1.0f) v.phase1 -= 1.0f;
            v.phase2 += v.inc2; if (v.phase2 >= 1.0f) v.phase2 -= 1.0f;
            const float mix = (o1 + o2 * s_.lvl2 + noise_.nextBipolar() * s_.noise) / (1.0f + s_.lvl2 + s_.noise);
            const float y = v.f2.process(v.f1.process(mix, SVF::Mode::Lowpass), SVF::Mode::Lowpass);
            out[i] += y * aenv * v.velGain * 1.0f;
        }
    }

    // Master volume and the stereo delay (modulated for chorus/flanger sounds).
    void applyDelayAndVolume(mpc::ProcessContext& ctx)
    {
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const float mix = get(kDelayMix) * 0.01f, fb = get(kDelayFeedback) * 0.01f, modDepth = get(kDelayMod) * 0.01f;
        const float baseSamples = get(kDelayTime) * 0.001f * (float)ctx.sampleRate;
        const float modInc = 0.7f / (float)ctx.sampleRate;
        const bool fx = mix > 0.001f;

        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float dry = L[i];
            float l = dry, r = dry;
            if (fx)
            {
                modPhase_ += modInc; if (modPhase_ >= 1.0f) modPhase_ -= 1.0f;
                const float swing = modDepth * 0.45f * baseSamples;     // up to +-45% of the delay time
                const float tl = baseSamples + swing * fastSin01(modPhase_);
                const float tr = baseSamples * 1.07f + swing * fastSin01(modPhase_ + 0.5f);
                const float wl = dlyL_.readLinear(tl), wr = dlyR_.readLinear(tr);
                dlyL_.write(softClip(dry + wl * fb));
                dlyR_.write(softClip(dry + wr * fb));
                l = dry + wl * mix; r = dry + wr * mix;
            }
            const float g = volume_.process();
            L[i] = softClip(l * g);
            if (R) R[i] = softClip(r * g);
        }
    }

    WaveBank bank_;
    Voice voices_[kNumVoices];
    VoiceAllocator<kNumVoices> alloc_;
    LFO lfo_;
    MidiControllers controllers_;
    Smoother volume_;
    DelayLine dlyL_, dlyR_;
    Xorshift32 noise_{0x1234ABCDu};
    float lfoBuf_[kChunk] = {0};
    float modPhase_ = 0.0f;
    double sr_ = 44100.0;
};

MPC_PLUGIN_ENTRY(Digiwave)
