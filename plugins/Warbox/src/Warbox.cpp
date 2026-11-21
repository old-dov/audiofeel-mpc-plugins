// Warbox - cinematic percussion for the MPC, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// 16 synthesized sounds (no samples) for epic scores: taiko, war drums, timpani, big toms, an epic
// kick, a stomp, anvil, metal plate, gong, a military snare, a rimshot and a swelling crash.
// Membranes are sine bodies with a pitch sweep and a few inharmonic overtones; metals are banks of
// inharmonic partials; skins and cymbals get filtered noise. One-shots: note-offs are ignored.
//
// MIDI map (notes 36..51, the pads of an MPC / a drum rack):
//   Drums   36 Taiko Hi   37 Taiko Lo   38 War Drum    39 Frame Drum
//           40 Timp Hi    41 Timp Lo    42 Floor Tom   43 Epic Kick   44 Stomp
//   Metal   45 Anvil      46 Plate      47 Gong
//   Hits    48 Mil Snare  49 Rimshot    50 Crash Swell 51 Taiko Rim
//
// Each sound has Tune, Length, Tone, Level and Pan; the whole kit goes through Drive and a big reverb.
// Signal flow: 16 voices -> level and pan -> drive (soft saturation) -> volume -> reverb.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/noise.h"
#include "dsp/svf.h"
#include "cine_fx.h"

#include <string>
#include <vector>

using namespace mpc::dsp;

class Warbox : public mpc::Plugin
{
public:
    enum Sound
    {
        kTaikoHi = 0, kTaikoLo, kWarDrum, kFrame, kTimpHi, kTimpLo, kFloorTom, kEpicKick, kStomp,
        kAnvil, kPlate, kGong,
        kMilSnare, kRimshot, kCrash, kTaikoRim,
        kNumSounds
    };
    static constexpr int kFirstNote = 36;
    enum Which { kTune = 0, kLength, kTone, kLevel, kPan, kParamsPerSound };

    // Parameter order is permanent once users have projects. Global parameters first
    // (parameter 0 is nudged by the host on every load, so it is the harmless master volume).
    enum Globals { kVolume = 0, kDrive, kVelSens, kRevMix, kRevSize, kRevDamp, kFirstSoundParam };
    static int pidx(int s, Which w) { return kFirstSoundParam + s * kParamsPerSound + (int)w; }
    static constexpr int kNumParams = kFirstSoundParam + kNumSounds * kParamsPerSound;

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Warbox";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', 'G', 'H', 't');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Warbox() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        static const char* const tags[kNumSounds] = {"TaikoH", "TaikoL", "War", "Frame", "TimpH", "TimpL", "FlrTom", "EpicK", "Stomp",
                                                     "Anvil", "Plate", "Gong", "MilSnr", "Rimsht", "Crash", "TkRim"};
        static const char* const ids[kNumSounds]  = {"taikohi", "taikolo", "war", "frame", "timphi", "timplo", "floortom", "epickick", "stomp",
                                                     "anvil", "plate", "gong", "milsnare", "rimshot", "crash", "taikorim"};

        addParam(floatParam("volume", "Volume",   -40.0f, 6.0f, -4.0f, "dB", 1));
        addParam(floatParam("drive",  "Drive",      0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("velsens", "Vel Sens",  0.0f, 100.0f, 70.0f, "%", 0));
        addParam(floatParam("revmix",  "Rev Mix",   0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam("revsize", "Rev Size",  0.0f, 100.0f, 75.0f, "%", 0));
        addParam(floatParam("revdamp", "Rev Damp",  0.0f, 100.0f, 45.0f, "%", 0));
        for (int s = 0; s < kNumSounds; ++s)
        {
            const std::string id = ids[s], tag = tags[s];
            addParam(floatParam((id + "_tune").c_str(),   (tag + " Tune").c_str(),   -12.0f, 12.0f, 0.0f, "st", 1));
            addParam(floatParam((id + "_length").c_str(), (tag + " Length").c_str(),  25.0f, 250.0f, 100.0f, "%", 0));
            addParam(floatParam((id + "_tone").c_str(),   (tag + " Tone").c_str(),     0.0f, 100.0f, 50.0f, "%", 0));
            addParam(floatParam((id + "_level").c_str(),  (tag + " Level").c_str(),    0.0f, 100.0f, 80.0f, "%", 0));
            addParam(floatParam((id + "_pan").c_str(),    (tag + " Pan").c_str(),   -100.0f, 100.0f, (float)kDefaultPan[s], "%", 0));
        }
        if (numParams() != kNumParams) mpc::log("Warbox: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        struct Cfg { int sound; float tune, length, tone, level; };
        auto prog = [this](const char* name, float drive, float revMix, float revSize, float revDamp, std::initializer_list<Cfg> cfg)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            v[(size_t)kDrive] = drive; v[(size_t)kRevMix] = revMix; v[(size_t)kRevSize] = revSize; v[(size_t)kRevDamp] = revDamp;
            for (const Cfg& c : cfg)
            {
                v[(size_t)pidx(c.sound, kTune)] = c.tune; v[(size_t)pidx(c.sound, kLength)] = c.length;
                v[(size_t)pidx(c.sound, kTone)] = c.tone; v[(size_t)pidx(c.sound, kLevel)] = c.level;
            }
            addProgram(name, std::move(v));
        };
        prog("Init Epic", 20, 30, 75, 45, {});
        prog("Taiko Ensemble", 25, 40, 80, 45, {{kTaikoHi, 0, 110, 60, 85}, {kTaikoLo, 0, 120, 55, 95}, {kWarDrum, 0, 100, 50, 85}, {kFrame, 0, 100, 50, 70},
                                                {kTaikoRim, 0, 100, 60, 70}, {kStomp, 0, 100, 50, 75}, {kFloorTom, 0, 110, 55, 80}, {kEpicKick, 0, 110, 50, 90}});
        prog("Orchestral Timpani", 12, 50, 85, 45, {{kTimpHi, 0, 120, 55, 85}, {kTimpLo, 0, 130, 55, 90}, {kFloorTom, 2, 100, 45, 70}, {kCrash, 0, 110, 55, 65},
                                                    {kGong, 0, 100, 50, 60}, {kMilSnare, 0, 90, 55, 70}, {kRimshot, 0, 90, 55, 65}, {kFrame, 0, 100, 50, 65}});
        prog("War Machine", 40, 40, 85, 50, {{kWarDrum, -2, 140, 55, 100}, {kTaikoLo, -2, 130, 55, 95}, {kEpicKick, -2, 130, 60, 95}, {kAnvil, 0, 120, 60, 80},
                                             {kPlate, -2, 120, 60, 75}, {kStomp, -2, 110, 60, 85}, {kMilSnare, 0, 110, 65, 80}, {kCrash, 0, 120, 60, 65}});
        prog("Gong & Metal", 15, 55, 90, 40, {{kGong, 0, 150, 55, 90}, {kPlate, 0, 150, 55, 85}, {kAnvil, 0, 140, 55, 85}, {kCrash, 0, 130, 55, 70},
                                              {kTimpLo, 0, 110, 50, 70}, {kTaikoRim, 0, 100, 55, 60}});
        prog("Tight Trailer", 35, 20, 50, 60, {{kEpicKick, 2, 60, 65, 95}, {kStomp, 2, 70, 65, 90}, {kTaikoHi, 2, 70, 65, 85}, {kTaikoLo, 2, 75, 60, 90},
                                               {kRimshot, 2, 70, 65, 80}, {kMilSnare, 2, 70, 65, 80}, {kAnvil, 2, 70, 60, 70}});
        prog("Dry Stage", 10, 8, 30, 70, {{kTaikoHi, 0, 80, 50, 85}, {kTaikoLo, 0, 80, 50, 90}, {kFrame, 0, 90, 50, 80}, {kFloorTom, 0, 80, 50, 80},
                                          {kStomp, 0, 80, 50, 80}, {kMilSnare, 0, 80, 50, 75}, {kRimshot, 0, 80, 50, 75}, {kTaikoRim, 0, 80, 50, 70}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        for (auto& v : voices_) { v.f1.setSampleRate(sampleRate); v.f2.setSampleRate(sampleRate); }
        fx_.prepare(sampleRate, dbToGain(get(kVolume)));
    }

    void reset() override
    {
        for (auto& v : voices_) { v.active = false; v.f1.reset(); v.f2.reset(); }
        fx_.reset(dbToGain(get(kVolume)));
    }

    // ----- audio ------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings();
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        for (int i = 0; i < ctx.numFrames; ++i) { L[i] = 0.0f; if (R) R[i] = 0.0f; }

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(L, R, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            if (ev.isNoteOn()) trigger(ev.note() - kFirstNote, ev.velocity());
        }
        render(L, R, pos, ctx.numFrames);

        const float drive = 1.0f + get(kDrive) * 0.04f;
        const float comp = 1.0f / fastTanh(drive);
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            L[i] = fastTanh(L[i] * drive) * comp;
            if (R) R[i] = fastTanh(R[i] * drive) * comp;
        }
        fx_.process(L, R, ctx.numFrames, ctx.sampleRate, dbToGain(get(kVolume)), 0.0f, 0.0f, 0.0f,
                    get(kRevMix) * 0.01f, get(kRevSize) * 0.01f, get(kRevDamp) * 0.01f, 2.4f);
    }

private:
    static constexpr int kDefaultPan[kNumSounds] = {-20, 20, 0, -10, -25, 25, 15, 0, 0, 20, -20, 0, 0, 10, -15, 25};
    static constexpr int kPartials = 6;
    static constexpr float kVoiceTrim = 0.42f;   // headroom: the voices are written hot, the drive stage should not squash them

    struct SoundVoice
    {
        bool active = false;
        float t = 0.0f;
        float e1 = 0.0f, e2 = 0.0f, e3 = 0.0f;
        float c1 = 0.99f, c2 = 0.99f, c3 = 0.99f;
        float f0 = 100.0f, tone = 0.5f, vel = 1.0f, tuneRatio = 1.0f, lenScale = 1.0f;
        float ph[kPartials + 2] = {0};
        SVF f1, f2;
        Xorshift32 rng{0x9E3779B9u};
    };

    struct Snapshot { float tuneRatio, lenScale, tone, level, gainL, gainR; };

    void readSettings()
    {
        velSens_ = get(kVelSens) * 0.01f;
        for (int s = 0; s < kNumSounds; ++s)
        {
            Snapshot& sn = snap_[s];
            sn.tuneRatio = semitonesToRatio(get(pidx(s, kTune)));
            sn.lenScale = get(pidx(s, kLength)) * 0.01f;
            sn.tone = get(pidx(s, kTone)) * 0.01f;
            sn.level = get(pidx(s, kLevel)) * 0.01f;
            equalPowerPan(get(pidx(s, kPan)) * 0.01f, sn.gainL, sn.gainR);
            sn.gainL *= kSqrt2; sn.gainR *= kSqrt2;     // unity at centre
        }
    }

    float coefFor(float seconds) const { return seconds <= 0.0f ? 0.0f : std::exp(-1.0f / (seconds * sr_)); }

    void trigger(int s, int velocity)
    {
        if (s < 0 || s >= kNumSounds) return;
        SoundVoice& v = voices_[s];
        const Snapshot& sn = snap_[s];
        v.active = true; v.t = 0.0f;
        v.e1 = v.e2 = v.e3 = 1.0f;
        v.tone = sn.tone; v.tuneRatio = sn.tuneRatio; v.lenScale = sn.lenScale;
        v.vel = lerp(1.0f, (float)velocity / 127.0f, velSens_);
        v.f1.reset(); v.f2.reset();
        for (float& p : v.ph) p = 0.0f;
        const float len = sn.lenScale;

        switch (s)
        {
            case kTaikoHi: v.f0 = 125.0f * sn.tuneRatio; v.c1 = coefFor(0.42f * len); v.c2 = coefFor(0.05f);  v.c3 = coefFor(0.04f);  v.f1.set(1800.0f + 2500.0f * sn.tone, 0.2f); break;
            case kTaikoLo: v.f0 = 72.0f * sn.tuneRatio;  v.c1 = coefFor(0.85f * len); v.c2 = coefFor(0.07f);  v.c3 = coefFor(0.05f);  v.f1.set(1200.0f + 2000.0f * sn.tone, 0.2f); break;
            case kWarDrum: v.f0 = 52.0f * sn.tuneRatio;  v.c1 = coefFor(1.5f * len);  v.c2 = coefFor(0.12f);  v.c3 = coefFor(0.5f * len); v.f1.set(110.0f + 300.0f * sn.tone, 0.2f); break;
            case kFrame:   v.f0 = 165.0f * sn.tuneRatio; v.c1 = coefFor(0.32f * len); v.c2 = coefFor(0.03f);  v.c3 = coefFor(0.05f);  v.f1.set(2500.0f + 3000.0f * sn.tone, 0.2f); break;
            case kTimpHi:  v.f0 = 116.0f * sn.tuneRatio; v.c1 = coefFor(1.7f * len);  v.c2 = coefFor(0.06f);  v.c3 = coefFor(0.5f * len); break;
            case kTimpLo:  v.f0 = 82.0f * sn.tuneRatio;  v.c1 = coefFor(2.1f * len);  v.c2 = coefFor(0.07f);  v.c3 = coefFor(0.6f * len); break;
            case kFloorTom:v.f0 = 96.0f * sn.tuneRatio;  v.c1 = coefFor(0.75f * len); v.c2 = coefFor(0.05f);  v.c3 = coefFor(0.2f * len); v.f1.set(1500.0f + 2500.0f * sn.tone, 0.2f); break;
            case kEpicKick:v.f0 = 46.0f * sn.tuneRatio;  v.c1 = coefFor(1.05f * len); v.c2 = coefFor(0.045f); v.c3 = coefFor(0.006f); break;
            case kStomp:   v.f0 = 92.0f * sn.tuneRatio;  v.c1 = coefFor(0.2f * len);  v.c2 = coefFor(0.03f);  v.c3 = coefFor(0.07f * len); v.f1.set(300.0f + 1200.0f * sn.tone, 0.2f); break;
            case kAnvil:   v.f0 = 620.0f * sn.tuneRatio; v.c1 = coefFor(1.2f * len);  v.c2 = coefFor(0.4f * len);  v.c3 = coefFor(0.004f); break;
            case kPlate:   v.f0 = 430.0f * sn.tuneRatio; v.c1 = coefFor(2.2f * len);  v.c2 = coefFor(0.9f * len);  v.c3 = coefFor(0.09f);  v.f1.set(2500.0f + 3500.0f * sn.tone, 0.2f); break;
            case kGong:    v.f0 = 130.0f * sn.tuneRatio; v.c1 = coefFor(6.0f * len);  v.c2 = coefFor(2.6f * len);  v.c3 = coefFor(3.0f * len); v.f1.set(3000.0f + 3000.0f * sn.tone, 0.1f); break;
            case kMilSnare:v.f0 = 190.0f * sn.tuneRatio; v.c1 = coefFor(0.36f * len); v.c2 = coefFor(0.1f * len);  v.c3 = coefFor(0.02f);  v.f1.set(1800.0f + 1800.0f * sn.tone, 0.15f); break;
            case kRimshot: v.f0 = 430.0f * sn.tuneRatio; v.c1 = coefFor(0.14f * len); v.c2 = coefFor(0.1f * len);  v.c3 = coefFor(0.012f); v.f1.set(2200.0f + 2500.0f * sn.tone, 0.2f); break;
            case kCrash:   v.c1 = coefFor(3.0f * len);   v.c2 = coefFor(1.0f * len);   v.c3 = coefFor(0.05f);  v.f1.set(4000.0f + 3500.0f * sn.tone, 0.1f); break;
            case kTaikoRim:v.f0 = 640.0f * sn.tuneRatio; v.c1 = coefFor(0.1f * len);  v.c2 = coefFor(0.03f);  v.c3 = coefFor(0.01f);  break;
            default: break;
        }
    }

    float sine(float& phase, float hz)
    {
        phase += hz / sr_; if (phase >= 1.0f) phase -= 1.0f;
        return WavetableSine::lookup(phase);
    }

    // A skin: sine body with a pitch sweep (e2), an overtone pair (e3) and a snap of filtered noise.
    float membrane(SoundVoice& v, float sweep, float over, float snap, float noiseGain)
    {
        const float n = v.rng.nextBipolar();
        const float f = v.f0 * (1.0f + sweep * v.e2);
        const float body = sine(v.ph[0], f);
        const float ov = (sine(v.ph[1], f * 1.59f) * 0.5f + sine(v.ph[2], f * 2.14f) * 0.3f) * over * v.e3;
        const float sn = v.f1.process(n, SVF::Mode::Bandpass) * v.e3 * snap * noiseGain;
        return body * v.e1 + ov + sn;
    }

    float voiceSample(int s, SoundVoice& v)
    {
        float out = 0.0f;
        const float n = v.rng.nextBipolar();
        v.t += 1.0f / sr_;

        switch (s)
        {
            case kTaikoHi: case kTaikoLo: case kFrame: case kFloorTom:
            {
                const float sweep = s == kTaikoLo ? 0.6f : (s == kFrame ? 0.3f : 0.5f);
                out = membrane(v, sweep, 0.3f + 0.5f * v.tone, 0.5f + 0.6f * v.tone, s == kFrame ? 1.6f : 1.0f);
                if (s == kTaikoHi || s == kTaikoLo) out = fastTanh(out * 1.5f);
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kWarDrum:
            {
                const float f = v.f0 * (1.0f + 0.55f * v.e2);
                const float body = sine(v.ph[0], f) + 0.3f * sine(v.ph[1], f * 1.5f) * v.e2;
                const float rumble = v.f1.process(n, SVF::Mode::Lowpass) * v.e3 * (0.5f + 1.0f * v.tone);
                out = fastTanh((body * v.e1 + rumble) * 1.5f);
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kTimpHi: case kTimpLo:
            {
                // Tuned membrane modes (1, 1.5, 1.99, 2.44, 2.89): the pitch is the 1.5 mode's octave-ish reading.
                static const float ratios[5] = {1.0f, 1.5f, 1.99f, 2.44f, 2.89f};
                static const float amps[5] = {1.0f, 0.6f, 0.35f, 0.22f, 0.12f};
                const float f = v.f0 * (1.0f + 0.22f * v.e2);
                float sum = 0.0f;
                for (int i = 0; i < 5; ++i) sum += sine(v.ph[i], f * ratios[i]) * amps[i] * (i == 0 ? v.e1 : v.e3 * (0.4f + 1.2f * v.tone));
                const float thump = v.f1.process(n, SVF::Mode::Lowpass) * v.e2 * 0.3f;
                out = sum * 0.55f + thump;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kEpicKick:
            {
                const float f = v.f0 * (1.0f + 3.0f * v.e2);
                const float body = sine(v.ph[0], f);
                out = fastTanh((body * v.e1 + n * v.e3 * (0.2f + 0.6f * v.tone)) * 1.8f);
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kStomp:
            {
                const float f = v.f0 * (1.0f + 0.9f * v.e2);
                const float body = sine(v.ph[0], f);
                const float lp = v.f1.process(n, SVF::Mode::Lowpass) * v.e3 * 1.1f;
                out = fastTanh((body * v.e1 + lp) * 1.6f);
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kAnvil:
            {
                static const float ratios[4] = {1.0f, 2.41f, 3.78f, 5.2f};
                float sum = 0.0f;
                for (int i = 0; i < 4; ++i) sum += sine(v.ph[i], v.f0 * ratios[i]) * (i < 2 ? v.e1 : v.e2 * (0.3f + 1.2f * v.tone)) * (1.0f - 0.18f * (float)i);
                out = sum * 0.5f + n * v.e3 * 0.7f;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kPlate:
            {
                static const float ratios[5] = {1.0f, 1.59f, 2.14f, 2.65f, 3.5f};
                float sum = 0.0f;
                for (int i = 0; i < 5; ++i) sum += sine(v.ph[i], v.f0 * ratios[i]) * (i < 2 ? v.e1 : v.e2 * (0.3f + 1.2f * v.tone)) * (0.9f - 0.12f * (float)i);
                const float burst = v.f1.process(n, SVF::Mode::Bandpass) * v.e3 * 1.2f;
                out = sum * 0.45f + burst;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kGong:
            {
                // Slow swell, then a long inharmonic ring; the upper partials fade first.
                static const float ratios[kPartials] = {1.0f, 1.47f, 2.09f, 2.56f, 3.10f, 4.20f};
                static const float amps[kPartials] = {1.0f, 0.7f, 0.5f, 0.4f, 0.3f, 0.2f};
                const float a = std::min(v.t * 9.0f, 1.0f);
                float sum = 0.0f;
                for (int i = 0; i < kPartials; ++i) sum += sine(v.ph[i], v.f0 * ratios[i] * (1.0f + 0.0007f * (float)i)) * amps[i] * (i < 2 ? v.e1 : v.e2 * (0.3f + 1.0f * v.tone));
                const float wash = v.f1.process(n, SVF::Mode::Highpass) * v.e3 * 0.25f * std::min(v.t * 5.0f, 1.0f);
                out = (sum * 0.4f + wash) * a;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kMilSnare:
            {
                const float f = v.f0 * (1.0f + 0.25f * v.e3);
                const float body = sine(v.ph[0], f) * 0.7f + sine(v.ph[1], f * 1.78f) * 0.3f;
                const float hp = v.f1.process(n, SVF::Mode::Highpass);
                out = body * v.e2 * 1.2f + hp * v.e1 * (0.35f + 0.6f * v.tone);
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kRimshot:
            {
                const float crack = v.f1.process(n, SVF::Mode::Bandpass) * v.e3 * 2.0f;
                const float ring = (sine(v.ph[0], v.f0) + 0.6f * sine(v.ph[1], v.f0 * 2.4f)) * v.e1 * 0.7f;
                const float thump = sine(v.ph[2], 82.0f * v.tuneRatio) * v.e2 * 0.8f;
                out = crack + ring + thump;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kCrash:
            {
                static const float r[5] = {263.0f, 400.0f, 421.0f, 474.0f, 587.0f};
                float m = 0.0f;
                for (int i = 0; i < 5; ++i)
                {
                    v.ph[i] += r[i] * 1.8f * v.tuneRatio / sr_; if (v.ph[i] >= 1.0f) v.ph[i] -= 1.0f;
                    m += v.ph[i] < 0.5f ? 1.0f : -1.0f;
                }
                const float a = std::min(v.t * 14.0f, 1.0f);              // the swell: ~70 ms in
                const float hp = v.f1.process(n * 0.7f + m * 0.14f, SVF::Mode::Highpass);
                out = hp * v.e1 * a * 1.6f + hp * v.e2 * 0.5f;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2;
                break;
            }
            case kTaikoRim:
            {
                const float tick = sine(v.ph[0], v.f0) * v.e1 + sine(v.ph[1], v.f0 * 1.9f) * v.e2 * 0.5f;
                out = tick * 0.9f + n * v.e3 * 0.5f;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            default: v.active = false; break;
        }
        return out * v.vel;
    }

    void render(float* L, float* R, int from, int to)
    {
        for (int s = 0; s < kNumSounds; ++s)
        {
            SoundVoice& v = voices_[s];
            if (!v.active) continue;
            const Snapshot& sn = snap_[s];
            const float gl = sn.level * sn.gainL * kVoiceTrim, gr = sn.level * sn.gainR * kVoiceTrim;
            for (int i = from; i < to && v.active; ++i)
            {
                const float y = sanitize(voiceSample(s, v));
                L[i] += y * gl;
                if (R) R[i] += y * gr;
            }
        }
    }

    SoundVoice voices_[kNumSounds];
    Snapshot snap_[kNumSounds];
    cine::MasterFx fx_;
    float velSens_ = 0.7f;
    float sr_ = 44100.0f;
};

MPC_PLUGIN_ENTRY(Warbox)
