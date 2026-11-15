// Drumbox - a 16-pad synthesized drum machine for the MPC, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// Every sound is synthesized (no samples): sine bodies with pitch envelopes, filtered noise,
// stacked square waves through a high-pass for the metallic sounds.
//
// MIDI map (notes 36..51, the pads of an MPC / a drum rack):
//   36 Kick     37 Rim      38 Snare    39 Clap
//   40 Snare 2  41 Tom Lo   42 Hat Cl.  43 Tom Mid
//   44 Shaker   45 Tom Hi   46 Hat Op.  47 Conga
//   48 Cowbell  49 Crash    50 Clave    51 Ride
//
// Each pad has Tune, Decay, Tone, Level and Pan. What "Tone" means depends on the drum:
//   kick: click and pitch sweep   snare: noise against body   clap: filter colour
//   hats/cymbals: brightness      toms/conga: overtone        rim/cowbell/clave: pitch spread
// Closed hat chokes open hat. Drums are one-shots: note-offs are ignored.
//
// Signal flow: 16 independent drum voices -> per-drum level and pan -> master drive
// (soft saturation) -> master volume -> outputs.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/noise.h"
#include "dsp/svf.h"
#include "dsp/smoother.h"

#include <string>
#include <vector>

using namespace mpc::dsp;

class Drumbox : public mpc::Plugin
{
public:
    enum Drum { kKick = 0, kRim, kSnare, kClap, kSnare2, kTomLo, kHatCl, kTomMid, kShaker, kTomHi, kHatOp, kConga, kCowbell, kCrash, kClave, kRide, kNumDrums };
    static constexpr int kFirstNote = 36;
    enum Which { kTune = 0, kDecay, kTone, kLevel, kPan, kParamsPerDrum };

    // Parameter order is permanent once users have projects. Global parameters first
    // (parameter 0 is nudged by the host on every load, so it is the harmless master volume).
    enum Globals { kVolume = 0, kDrive, kVelSens, kFirstDrumParam };
    static int pidx(int drum, Which w) { return kFirstDrumParam + drum * kParamsPerDrum + (int)w; }
    static constexpr int kNumParams = kFirstDrumParam + kNumDrums * kParamsPerDrum;

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Drumbox";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', 'B', 'p', 'V');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Drumbox() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        static const char* const tags[kNumDrums] = {"Kick", "Rim", "Snare", "Clap", "Snr2", "TomL", "CHat", "TomM", "Shkr", "TomH", "OHat", "Cnga", "Cowb", "Crash", "Clav", "Ride"};
        static const char* const ids[kNumDrums]  = {"kick", "rim", "snare", "clap", "snr2", "toml", "chat", "tomm", "shkr", "tomh", "ohat", "cnga", "cowb", "crash", "clav", "ride"};

        addParam(floatParam("volume", "Volume",   -40.0f, 6.0f, -4.0f, "dB", 1));
        addParam(floatParam("drive",  "Drive",      0.0f, 100.0f, 15.0f, "%", 0));
        addParam(floatParam("velsens", "Vel Sens",  0.0f, 100.0f, 70.0f, "%", 0));
        for (int d = 0; d < kNumDrums; ++d)
        {
            const std::string id = ids[d], tag = tags[d];
            addParam(floatParam((id + "_tune").c_str(),  (tag + " Tune").c_str(),  -12.0f, 12.0f, 0.0f, "st", 1));
            addParam(floatParam((id + "_decay").c_str(), (tag + " Decay").c_str(),  20.0f, 250.0f, 100.0f, "%", 0));
            addParam(floatParam((id + "_tone").c_str(),  (tag + " Tone").c_str(),    0.0f, 100.0f, 50.0f, "%", 0));
            addParam(floatParam((id + "_level").c_str(), (tag + " Level").c_str(),   0.0f, 100.0f, 80.0f, "%", 0));
            addParam(floatParam((id + "_pan").c_str(),   (tag + " Pan").c_str(),  -100.0f, 100.0f, (float)kDefaultPan[d], "%", 0));
        }
        if (numParams() != kNumParams) mpc::log("Drumbox: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        // ----- kits (built-in programs) -----
        struct Cfg { int drum; float tune, decay, tone, level; };
        auto kit = [this](const char* name, float drive, std::initializer_list<Cfg> cfg)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            v[(size_t)kDrive] = drive;
            for (const Cfg& c : cfg)
            {
                v[(size_t)pidx(c.drum, kTune)] = c.tune; v[(size_t)pidx(c.drum, kDecay)] = c.decay;
                v[(size_t)pidx(c.drum, kTone)] = c.tone; v[(size_t)pidx(c.drum, kLevel)] = c.level;
            }
            addProgram(name, std::move(v));
        };
        kit("Init Kit", 15, {});
        kit("808 Deep", 20, {{kKick, -4, 190, 30, 90}, {kSnare, -1, 100, 40, 80}, {kClap, 0, 110, 40, 75}, {kHatCl, 0, 90, 40, 60},
                             {kHatOp, 0, 110, 40, 60}, {kTomLo, -3, 120, 30, 80}, {kTomMid, -2, 120, 30, 80}, {kTomHi, -1, 120, 30, 80},
                             {kCowbell, 0, 100, 40, 65}, {kClave, 0, 90, 50, 70}});
        kit("909 Punch", 30, {{kKick, 1, 110, 75, 95}, {kSnare, 1, 90, 70, 85}, {kClap, 0, 100, 65, 80}, {kHatCl, 2, 80, 70, 60},
                              {kHatOp, 2, 100, 70, 60}, {kRide, 0, 100, 60, 55}, {kCrash, 0, 100, 65, 55}, {kTomLo, 1, 90, 50, 80}});
        kit("Tight Techno", 40, {{kKick, 0, 70, 85, 100}, {kSnare, 2, 60, 80, 75}, {kClap, 1, 70, 70, 80}, {kHatCl, 3, 55, 80, 60},
                                 {kHatOp, 3, 70, 80, 55}, {kShaker, 0, 80, 60, 60}, {kRim, 0, 80, 60, 70}, {kCowbell, 2, 70, 50, 55}});
        kit("Lo-Fi Dust", 55, {{kKick, -2, 130, 20, 90}, {kSnare, -3, 120, 25, 80}, {kClap, -2, 120, 25, 70}, {kHatCl, -2, 90, 25, 55},
                               {kHatOp, -2, 120, 25, 50}, {kShaker, -2, 120, 30, 60}, {kRim, -2, 100, 30, 70}, {kRide, -2, 110, 25, 50}});
        kit("Trap Hard", 45, {{kKick, -6, 260, 55, 100}, {kSnare, 2, 80, 85, 85}, {kClap, 1, 90, 80, 85}, {kHatCl, 4, 45, 90, 55},
                              {kHatOp, 4, 85, 90, 50}, {kTomLo, -6, 200, 30, 80}, {kCrash, 0, 90, 80, 55}, {kClave, 0, 70, 50, 60}});
        kit("Electro Snap", 35, {{kKick, 2, 80, 80, 95}, {kSnare, 3, 70, 90, 85}, {kSnare2, 2, 60, 90, 80}, {kClap, 2, 70, 80, 80},
                                 {kHatCl, 5, 55, 85, 60}, {kHatOp, 5, 80, 85, 55}, {kCowbell, 3, 80, 60, 60}, {kClave, 2, 70, 60, 70}});
        kit("Perc Jam", 20, {{kKick, 0, 100, 40, 85}, {kSnare, 0, 90, 50, 70}, {kRim, 0, 100, 60, 80}, {kConga, 0, 120, 55, 85},
                             {kTomLo, 0, 110, 45, 80}, {kTomMid, 0, 110, 45, 80}, {kTomHi, 0, 110, 45, 80}, {kShaker, 0, 110, 50, 75},
                             {kCowbell, 0, 110, 50, 70}, {kClave, 0, 100, 50, 75}});
        kit("Soft Room", 5, {{kKick, 0, 120, 35, 85}, {kSnare, 0, 110, 35, 70}, {kRim, 0, 100, 45, 70}, {kHatCl, -1, 100, 30, 50},
                             {kHatOp, -1, 120, 30, 50}, {kRide, 0, 120, 35, 50}, {kTomLo, 0, 110, 35, 75}, {kShaker, 0, 100, 40, 55}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        for (auto& v : voices_) { v.f1.setSampleRate(sampleRate); v.f2.setSampleRate(sampleRate); }
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
    }

    void reset() override
    {
        for (int d = 0; d < kNumDrums; ++d) { voices_[d].active = false; voices_[d].f1.reset(); voices_[d].f2.reset(); }
        volume_.snap(dbToGain(get(kVolume)));
    }

    // ----- audio ------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx.sampleRate);

        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        for (int i = 0; i < ctx.numFrames; ++i) { L[i] = 0.0f; if (R) R[i] = 0.0f; }

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(L, R, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            if (ev.isNoteOn()) trigger(ev.note() - kFirstNote, ev.velocity());
            else if (ev.isAllNotesOff()) {}   // one-shots: nothing to release
        }
        render(L, R, pos, ctx.numFrames);

        // Master: drive (soft saturation) and volume.
        const float drive = 1.0f + get(kDrive) * 0.04f;
        const float comp = 1.0f / fastTanh(drive);
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float g = volume_.process();
            L[i] = softClip(fastTanh(L[i] * drive) * comp * g);
            if (R) R[i] = softClip(fastTanh(R[i] * drive) * comp * g);
        }
    }

private:
    static constexpr int kDefaultPan[kNumDrums] = {0, -15, 0, 10, 0, -30, 25, 0, 35, 30, 30, -20, -10, -25, 20, 25};
    static constexpr int kMetalCount = 6;

    // One drum voice. Envelopes are exponential: x *= coef each sample.
    struct DrumVoice
    {
        bool active = false;
        float t = 0.0f;                    // seconds since the trigger
        float e1 = 0.0f, e2 = 0.0f, e3 = 0.0f;
        float c1 = 0.99f, c2 = 0.99f, c3 = 0.99f;
        float f0 = 100.0f, tone = 0.5f, vel = 1.0f, tuneRatio = 1.0f;
        float ph[kMetalCount] = {0};
        int burst = 0;
        bool choked = false;
        SVF f1, f2;
        Xorshift32 rng{0x9E3779B9u};
    };

    struct Snapshot { float tuneRatio, decayScale, tone, level, gainL, gainR; };

    void readSettings(double sr)
    {
        (void)sr;
        velSens_ = get(kVelSens) * 0.01f;
        volume_.setTarget(dbToGain(get(kVolume)));
        for (int d = 0; d < kNumDrums; ++d)
        {
            Snapshot& s = snap_[d];
            s.tuneRatio = semitonesToRatio(get(pidx(d, kTune)));
            s.decayScale = get(pidx(d, kDecay)) * 0.01f;
            s.tone = get(pidx(d, kTone)) * 0.01f;
            s.level = get(pidx(d, kLevel)) * 0.01f;
            equalPowerPan(get(pidx(d, kPan)) * 0.01f, s.gainL, s.gainR);
            s.gainL *= kSqrt2; s.gainR *= kSqrt2;     // unity at centre
        }
    }

    float coefFor(float seconds) const { return seconds <= 0.0f ? 0.0f : std::exp(-1.0f / (seconds * sr_)); }

    // ----- trigger: set the coefficients for the drum's recipe ----------------
    void trigger(int d, int velocity)
    {
        if (d < 0 || d >= kNumDrums) return;
        DrumVoice& v = voices_[d];
        const Snapshot& s = snap_[d];
        v.active = true; v.choked = false; v.t = 0.0f; v.burst = 0;
        v.e1 = v.e2 = v.e3 = 1.0f;
        v.tone = s.tone; v.tuneRatio = s.tuneRatio;
        v.vel = lerp(1.0f, (float)velocity / 127.0f, velSens_);
        v.f1.reset(); v.f2.reset();
        const float dec = s.decayScale;
        for (int i = 0; i < kMetalCount; ++i) v.ph[i] = 0.0f;

        switch (d)
        {
            case kKick:
                v.f0 = 46.0f * s.tuneRatio;
                v.c1 = coefFor(0.15f * dec);                  // amplitude
                v.c2 = coefFor(0.012f + 0.03f * s.tone);      // pitch sweep
                v.c3 = coefFor(0.003f);                       // click
                break;
            case kTomLo: case kTomMid: case kTomHi:
                v.f0 = (d == kTomLo ? 88.0f : d == kTomMid ? 128.0f : 185.0f) * s.tuneRatio;
                v.c1 = coefFor(0.17f * dec); v.c2 = coefFor(0.04f); v.c3 = coefFor(0.06f * dec);
                break;
            case kConga:
                v.f0 = 330.0f * s.tuneRatio;
                v.c1 = coefFor(0.11f * dec); v.c2 = coefFor(0.03f); v.c3 = coefFor(0.012f);
                break;
            case kSnare:
                v.f0 = 185.0f * s.tuneRatio;
                v.c1 = coefFor(0.10f * dec);                  // noise
                v.c2 = coefFor(0.075f * dec);                 // body
                v.c3 = coefFor(0.02f);                        // body pitch drop
                v.f1.set(1600.0f * s.tuneRatio, 0.1f);
                break;
            case kSnare2:
                v.f0 = 220.0f * s.tuneRatio;
                v.c1 = coefFor(0.065f * dec); v.c2 = coefFor(0.05f * dec); v.c3 = coefFor(0.015f);
                v.f1.set(2400.0f * s.tuneRatio, 0.1f);
                break;
            case kRim:
                v.f0 = 480.0f * s.tuneRatio;
                v.c1 = coefFor(0.022f * dec); v.c2 = coefFor(0.002f); v.c3 = coefFor(0.012f * dec);
                break;
            case kClap:
                v.c1 = coefFor(0.0025f);                       // each burst
                v.c2 = coefFor(0.07f * dec);                   // tail (used after the bursts)
                v.f1.set(900.0f + 1700.0f * s.tone, 0.45f);
                break;
            case kHatCl: case kHatOp:
                v.c1 = coefFor((d == kHatCl ? 0.035f : 0.16f) * dec);
                v.c2 = coefFor(0.004f);
                v.f1.set(4500.0f + 5500.0f * s.tone, 0.15f);
                if (d == kHatCl) { DrumVoice& open = voices_[kHatOp]; if (open.active) { open.choked = true; open.c1 = coefFor(0.004f); } }
                break;
            case kShaker:
                v.c1 = coefFor(0.07f * dec); v.c2 = coefFor(0.012f);
                v.f1.set(5500.0f * s.tuneRatio + 2500.0f * s.tone, 0.3f);
                break;
            case kCowbell:
                v.f0 = 540.0f * s.tuneRatio;
                v.c1 = coefFor(0.11f * dec); v.c2 = coefFor(0.004f);
                v.f1.set(900.0f + 1400.0f * s.tone, 0.35f);
                break;
            case kCrash:
                v.c1 = coefFor(1.1f * dec); v.c2 = coefFor(0.3f * dec);
                v.f1.set(4200.0f + 3500.0f * s.tone, 0.1f);
                break;
            case kRide:
                v.f0 = 1180.0f * s.tuneRatio;
                v.c1 = coefFor(0.75f * dec); v.c2 = coefFor(0.35f * dec); v.c3 = coefFor(0.5f * dec);
                v.f1.set(3500.0f + 3000.0f * s.tone, 0.1f);
                break;
            case kClave:
                v.f0 = 2350.0f * s.tuneRatio;
                v.c1 = coefFor(0.035f * dec); v.c2 = coefFor(0.004f);
                break;
            default: break;
        }
    }

    // ----- one sample of a drum --------------------------------------------
    // The metallic oscillator bank (inharmonic square waves), shared by hats, cowbell and cymbals.
    float metal(DrumVoice& v, float scale, int count)
    {
        static const float ratios[kMetalCount] = {263.0f, 400.0f, 421.0f, 474.0f, 587.0f, 845.0f};
        float sum = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            v.ph[i] += ratios[i] * scale / sr_;
            if (v.ph[i] >= 1.0f) v.ph[i] -= 1.0f;
            sum += v.ph[i] < 0.5f ? 1.0f : -1.0f;
        }
        return sum * (1.0f / (float)count);
    }

    float sine(float& phase, float hz)
    {
        phase += hz / sr_; if (phase >= 1.0f) phase -= 1.0f;
        return WavetableSine::lookup(phase);
    }

    float drumSample(int d, DrumVoice& v)
    {
        const float dt = 1.0f / sr_;
        float out = 0.0f;
        const float n = v.rng.nextBipolar();
        v.t += dt;

        switch (d)
        {
            case kKick:
            {
                const float f = v.f0 * (1.0f + (2.0f + 5.0f * v.tone) * v.e2);
                const float body = sine(v.ph[0], f);
                const float click = n * v.e3 * (0.15f + 0.6f * v.tone);
                out = fastTanh((body * v.e1 + click) * 1.6f);
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kTomLo: case kTomMid: case kTomHi:
            {
                const float f = v.f0 * (1.0f + 0.7f * v.e2);
                const float body = sine(v.ph[0], f);
                const float over = sine(v.ph[1], f * 2.15f) * v.tone * 0.35f * v.e3;
                out = (body + over) * v.e1 + n * v.e2 * 0.08f * v.tone;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kConga:
            {
                const float f = v.f0 * (1.0f + 0.35f * v.e2);
                const float body = sine(v.ph[0], f) + sine(v.ph[1], f * 1.93f) * v.tone * 0.35f;
                out = body * v.e1 + n * v.e3 * 0.25f * v.tone;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kSnare: case kSnare2:
            {
                const float drop = 1.0f + 0.3f * v.e3;
                const float bodyHz = v.f0 * drop;
                const float body = sine(v.ph[0], bodyHz) * 0.7f + sine(v.ph[1], bodyHz * 1.78f) * 0.3f;
                const float hp = v.f1.process(n, SVF::Mode::Highpass);
                out = body * v.e2 * (1.0f - 0.4f * v.tone) * 1.15f + hp * v.e1 * (0.22f + 0.6f * v.tone);
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kRim:
            {
                const float a = sine(v.ph[0], v.f0) + sine(v.ph[1], v.f0 * (2.0f + 1.3f * v.tone)) * 0.6f;
                out = a * v.e1 * 0.8f + n * v.e3 * 0.4f;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e3 *= v.c3;
                break;
            }
            case kClap:
            {
                // Three quick bursts 9.5 ms apart, then the tail.
                if (v.burst < 3 && v.t >= (float)(v.burst + 1) * 0.0095f)
                {
                    ++v.burst;
                    v.e1 = 1.0f;
                    if (v.burst == 3) v.c1 = v.c2;       // from now on the tail decay
                }
                const float bp = v.f1.process(n, SVF::Mode::Bandpass);
                out = bp * v.e1 * 2.0f;
                v.e1 *= v.c1;
                if (v.burst >= 3 && v.e1 < 1.0e-4f) v.active = false;
                break;
            }
            case kHatCl: case kHatOp:
            {
                const float m = metal(v, v.tuneRatio, kMetalCount) * 0.8f + n * 0.25f;
                const float hp = v.f1.process(m, SVF::Mode::Highpass);
                out = hp * v.e1 * 1.5f;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                break;
            }
            case kShaker:
            {
                const float bp = v.f1.process(n, SVF::Mode::Bandpass);
                out = bp * (1.0f - v.e2) * v.e1 * 2.0f;
                v.e2 *= v.c2;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                break;
            }
            case kCowbell:
            {
                const float sq = (sine(v.ph[0], v.f0) > 0.0f ? 1.0f : -1.0f) + (sine(v.ph[1], v.f0 * (1.48f + 0.15f * v.tone)) > 0.0f ? 1.0f : -1.0f);
                const float bp = v.f1.process(sq * 0.5f, SVF::Mode::Bandpass);
                out = bp * v.e1 * 1.6f;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                break;
            }
            case kCrash:
            {
                const float m = metal(v, 1.35f * v.tuneRatio, kMetalCount) * 0.6f + n * 0.45f * v.e2;
                const float hp = v.f1.process(m, SVF::Mode::Highpass);
                out = hp * v.e1 * 1.2f;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2;
                break;
            }
            case kRide:
            {
                const float m = metal(v, 0.9f * v.tuneRatio, kMetalCount) * 0.45f;
                const float bell = sine(v.ph[0], v.f0) * 0.35f * v.tone * v.e3 + sine(v.ph[1], v.f0 * 1.5f) * 0.15f * v.tone * v.e3;
                const float hp = v.f1.process(m + n * 0.12f * v.e2, SVF::Mode::Highpass);
                out = (hp + bell) * v.e1 * 1.4f;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kClave:
            {
                out = sine(v.ph[0], v.f0) * v.e1 * 0.9f;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                break;
            }
            default: v.active = false; break;
        }
        return out * v.vel;
    }

    void render(float* L, float* R, int from, int to)
    {
        for (int d = 0; d < kNumDrums; ++d)
        {
            DrumVoice& v = voices_[d];
            if (!v.active) continue;
            const Snapshot& s = snap_[d];
            const float gl = s.level * s.gainL, gr = s.level * s.gainR;
            for (int i = from; i < to && v.active; ++i)
            {
                const float y = sanitize(drumSample(d, v));
                L[i] += y * gl;
                if (R) R[i] += y * gr;
            }
        }
    }

    DrumVoice voices_[kNumDrums];
    Snapshot snap_[kNumDrums];
    Smoother volume_;
    float velSens_ = 0.7f;
    float sr_ = 44100.0f;
};

MPC_PLUGIN_ENTRY(Drumbox)
