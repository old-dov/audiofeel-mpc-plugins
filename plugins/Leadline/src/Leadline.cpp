// Leadline - a monophonic lead synth for the MPC and VST2 hosts, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// Signal flow (one voice, last-note priority):
//
//   unison stack (1-7 oscillators of one wave, detuned and spread across the stereo field)
//   + sub oscillator (sine, one octave below)
//     -> two cascaded 12 dB state-variable low-passes per channel = 24 dB/oct, with drive
//        cutoff = Cutoff * 2^( Env Amount * filter EG + Key Track )
//     -> amp ADSR * velocity -> master volume -> ping-pong delay -> outputs
//
// Playing style: glide between notes, optional legato (overlapping notes glide without
// retriggering the envelopes), a vibrato LFO that fades in after "Vib Delay" (the mod wheel
// adds more), a slow pulse-width LFO for the square wave, and pitch bend (range is a parameter).

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/lfo.h"
#include "dsp/smoother.h"
#include "dsp/delay_line.h"
#include "dsp/midi_cc.h"

#include <vector>

using namespace mpc::dsp;

class Leadline : public mpc::Plugin
{
public:
    // Parameter order is permanent once users have projects: append new ones at the end.
    // Parameter 0 is nudged by the host on every load, so it is the harmless master volume.
    enum Params
    {
        kVolume = 0,
        kWave, kPulseWidth, kPwmDepth, kPwmRate, kUnison, kDetune, kStereo, kSub, kOctave, kGlide, kLegato, kBend,
        kCutoff, kReso, kEnvAmount, kKeyTrack, kDrive,
        kFAttack, kFDecay, kFSustain, kFRelease,
        kAAttack, kADecay, kASustain, kARelease, kVelocity,
        kVibRate, kVibDepth, kVibDelay,
        kDelayTime, kDelayFeedback, kDelayMix,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Leadline";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', 'B', 'S', 'W');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Leadline() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("volume",  "Volume",       -40.0f, 6.0f, -8.0f, "dB", 1));
        addParam(choiceParam("wave",    "Wave",         {"Sine", "Triangle", "Saw", "Square"}, 2));
        addParam(floatParam ("pw",      "Pulse Width",  5.0f, 95.0f, 50.0f, "%", 0));
        addParam(floatParam ("pwmdepth", "PWM Depth",   0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam   ("pwmrate", "PWM Rate",     0.1f, 8.0f, 0.7f, "Hz", 2));
        addParam(intParam   ("unison",  "Unison",       1, 7, 3));
        addParam(floatParam ("detune",  "Detune",       0.0f, 60.0f, 14.0f, "ct", 0));
        addParam(floatParam ("stereo",  "Stereo",       0.0f, 100.0f, 70.0f, "%", 0));
        addParam(floatParam ("sub",     "Sub",          0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam   ("octave",  "Octave",      -2, 2, 0));
        addParam(floatParam ("glide",   "Glide",        0.0f, 600.0f, 40.0f, "ms", 0, 0.5f));
        addParam(boolParam  ("legato",  "Legato",       true));
        addParam(intParam   ("bend",    "Bend Range",   0, 12, 2, "st"));
        addParam(logParam   ("cutoff",  "Cutoff",       80.0f, 18000.0f, 4500.0f, "Hz", 0));
        addParam(floatParam ("reso",    "Resonance",    0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam ("envamt",  "Env Amount",  -100.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam ("keytrk",  "Key Track",    0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("drive",   "Drive",        0.0f, 100.0f, 10.0f, "%", 0));
        addParam(logParam   ("fatk",    "VCF Attack",   0.5f, 3000.0f, 3.0f, "ms", 1));
        addParam(logParam   ("fdec",    "VCF Decay",    5.0f, 4000.0f, 400.0f, "ms", 0));
        addParam(floatParam ("fsus",    "VCF Sustain",  0.0f, 100.0f, 40.0f, "%", 0));
        addParam(logParam   ("frel",    "VCF Release",  5.0f, 4000.0f, 250.0f, "ms", 0));
        addParam(logParam   ("aatk",    "VCA Attack",   0.5f, 3000.0f, 3.0f, "ms", 1));
        addParam(logParam   ("adec",    "VCA Decay",    5.0f, 4000.0f, 300.0f, "ms", 0));
        addParam(floatParam ("asus",    "VCA Sustain",  0.0f, 100.0f, 85.0f, "%", 0));
        addParam(logParam   ("arel",    "VCA Release",  5.0f, 4000.0f, 220.0f, "ms", 0));
        addParam(floatParam ("velsens", "Velocity",     0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam   ("vibrate", "Vib Rate",     0.5f, 12.0f, 5.5f, "Hz", 2));
        addParam(floatParam ("vibdepth", "Vib Depth",   0.0f, 2.0f, 0.15f, "st", 2, 0.5f));
        addParam(floatParam ("vibdelay", "Vib Delay",   0.0f, 2000.0f, 300.0f, "ms", 0, 0.5f));
        addParam(logParam   ("dlytime", "Delay Time",   10.0f, 700.0f, 340.0f, "ms", 0));
        addParam(floatParam ("dlyfb",   "Delay Fback",  0.0f, 90.0f, 30.0f, "%", 0));
        addParam(floatParam ("dlymix",  "Delay Mix",    0.0f, 100.0f, 0.0f, "%", 0));

        if (numParams() != kNumParams) mpc::log("Leadline: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        // Waves: 0 Sine, 1 Triangle, 2 Saw, 3 Square.
        program("Init Lead",       {});
        program("Supersaw Lead",   {{kWave, 2}, {kUnison, 7}, {kDetune, 24}, {kStereo, 85}, {kCutoff, 6500}, {kReso, 12}, {kEnvAmount, 20},
                                    {kASustain, 85}, {kDelayMix, 18}, {kDelayTime, 360}, {kDelayFeedback, 30}});
        program("Soft Sine Lead",  {{kWave, 0}, {kUnison, 1}, {kGlide, 70}, {kCutoff, 9000}, {kReso, 5}, {kEnvAmount, 0}, {kDrive, 0},
                                    {kVibDepth, 0.25f}, {kVibDelay, 500}, {kDelayMix, 22}, {kDelayTime, 380}, {kDelayFeedback, 35}});
        program("Square Mono",     {{kWave, 3}, {kUnison, 2}, {kDetune, 8}, {kStereo, 40}, {kCutoff, 3800}, {kReso, 22}, {kEnvAmount, 35},
                                    {kPwmDepth, 40}, {kPwmRate, 0.6f}, {kSub, 25}});
        program("Screamer",        {{kWave, 2}, {kUnison, 3}, {kDetune, 18}, {kDrive, 65}, {kCutoff, 2600}, {kReso, 42}, {kEnvAmount, 45},
                                    {kFDecay, 500}, {kGlide, 30}, {kVibDepth, 0.2f}});
        program("Trance Pluck",    {{kWave, 2}, {kUnison, 5}, {kDetune, 20}, {kStereo, 80}, {kCutoff, 1800}, {kReso, 20}, {kEnvAmount, 65},
                                    {kFDecay, 220}, {kFSustain, 10}, {kADecay, 260}, {kASustain, 0}, {kARelease, 120}, {kLegato, 0}, {kGlide, 0},
                                    {kDelayMix, 25}, {kDelayTime, 330}, {kDelayFeedback, 40}});
        program("Portamento Lead", {{kWave, 2}, {kUnison, 1}, {kGlide, 190}, {kCutoff, 3000}, {kReso, 25}, {kEnvAmount, 25}, {kSub, 30},
                                    {kVibDepth, 0.2f}});
        program("Hoover Stack",    {{kWave, 2}, {kUnison, 7}, {kDetune, 45}, {kStereo, 100}, {kDrive, 55}, {kCutoff, 2000}, {kReso, 30},
                                    {kEnvAmount, 20}, {kGlide, 60}, {kASustain, 100}});
        program("Retro Pulse",     {{kWave, 3}, {kPulseWidth, 30}, {kPwmDepth, 65}, {kPwmRate, 0.5f}, {kUnison, 2}, {kDetune, 10}, {kStereo, 55},
                                    {kCutoff, 3500}, {kReso, 18}, {kEnvAmount, 30}});
        program("Warm Triangle",   {{kWave, 1}, {kUnison, 3}, {kDetune, 10}, {kStereo, 60}, {kCutoff, 5200}, {kReso, 8}, {kEnvAmount, 10},
                                    {kSub, 30}, {kVibDepth, 0.2f}, {kVibDelay, 400}});
        program("Glass Lead",      {{kWave, 1}, {kUnison, 2}, {kDetune, 15}, {kStereo, 70}, {kCutoff, 10000}, {kReso, 10}, {kEnvAmount, 15},
                                    {kDelayMix, 35}, {kDelayTime, 400}, {kDelayFeedback, 45}});
        program("Acid Lead",       {{kWave, 2}, {kUnison, 1}, {kGlide, 50}, {kCutoff, 700}, {kReso, 66}, {kEnvAmount, 82}, {kFDecay, 190},
                                    {kFSustain, 15}, {kDrive, 42}, {kKeyTrack, 40}, {kLegato, 1}});
        program("Epic Stack",      {{kWave, 2}, {kUnison, 7}, {kDetune, 30}, {kStereo, 100}, {kCutoff, 5200}, {kEnvAmount, 25}, {kAAttack, 25},
                                    {kARelease, 450}, {kDelayMix, 30}, {kDelayTime, 380}, {kDelayFeedback, 42}, {kVibDepth, 0.2f}});
        program("Whistle",         {{kWave, 0}, {kUnison, 1}, {kGlide, 90}, {kCutoff, 12000}, {kEnvAmount, 0}, {kVibRate, 5.5f}, {kVibDepth, 0.35f},
                                    {kVibDelay, 700}, {kAAttack, 40}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = sampleRate;
        for (auto& o : osc_) o.setSampleRate(sampleRate);
        sub_.setSampleRate(sampleRate);
        for (auto& f : filt_) f.setSampleRate(sampleRate);
        ampEnv_.setSampleRate(sampleRate); filtEnv_.setSampleRate(sampleRate);
        vib_.setSampleRate(sampleRate); pwm_.setSampleRate(sampleRate);
        vib_.setShape(0); pwm_.setShape(0);
        sub_.setWave(0);   // the sub is a sine
        controllers_.prepare(sampleRate, 10.0f);
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
        const int maxDelay = (int)(sampleRate * 0.72) + 8;
        dlyL_.prepare(maxDelay); dlyR_.prepare(maxDelay);
    }

    void reset() override
    {
        ampEnv_.reset(); filtEnv_.reset();
        for (auto& f : filt_) f.reset();
        held_ = 0; bend_ = 0.0f; ctr_ = 0; velN_ = 0.0f; age_ = 0;
        controllers_.reset(); vib_.reset(); pwm_.reset();
        dlyL_.reset(); dlyR_.reset();
        volume_.snap(dbToGain(get(kVolume)));
    }

    // ----- audio ------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx.sampleRate);

        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(L, R, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            handleMidi(ev);
        }
        render(L, R, pos, ctx.numFrames);
        applyDelay(L, R, ctx);
    }

private:
    static constexpr int kMaxUnison = 7;
    static constexpr int kMaxHeld = 16;
    static constexpr int kCtrlInterval = 8;

    struct Settings
    {
        int wave = 2, octave = 0, unison = 3;
        float pw = 0.5f, pwmDepth = 0.0f, detune = 14.0f, stereo = 0.7f, sub = 0.0f, glideMs = 0.0f;
        bool legato = true;
        float cutoff = 4500.0f, reso = 0.2f, envAmt = 0.3f, keyTrack = 0.5f, vibDepth = 0.15f, vibDelaySamples = 0.0f, velSens = 0.5f;
    } s_;

    void readSettings(double sr)
    {
        s_.wave = getInt(kWave); s_.octave = getInt(kOctave); s_.unison = getInt(kUnison);
        s_.pw = get(kPulseWidth) * 0.01f; s_.pwmDepth = get(kPwmDepth) * 0.01f;
        s_.detune = get(kDetune); s_.stereo = get(kStereo) * 0.01f; s_.sub = get(kSub) * 0.01f;
        s_.glideMs = get(kGlide); s_.legato = getBool(kLegato);
        s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f; s_.envAmt = get(kEnvAmount) * 0.01f; s_.keyTrack = get(kKeyTrack) * 0.01f;
        s_.vibDepth = get(kVibDepth); s_.vibDelaySamples = get(kVibDelay) * 0.001f * (float)sr; s_.velSens = get(kVelocity) * 0.01f;

        for (int i = 0; i < kMaxUnison; ++i) osc_[i].setWave(s_.wave);
        const float drive = 1.0f + get(kDrive) * 0.09f;
        for (auto& f : filt_) f.setDrive(drive);
        ampEnv_.set(get(kAAttack), get(kADecay), get(kASustain) * 0.01f, get(kARelease));
        filtEnv_.set(get(kFAttack), get(kFDecay), get(kFSustain) * 0.01f, get(kFRelease));
        glide_.setTime(s_.glideMs, sr);
        vib_.setFrequency(get(kVibRate)); pwm_.setFrequency(get(kPwmRate));
        volume_.setTarget(dbToGain(get(kVolume)));

        // Pan positions of the unison stack: evenly spread, scaled by Stereo.
        const int n = s_.unison;
        for (int i = 0; i < kMaxUnison; ++i)
        {
            const float p = (n > 1) ? ((float)i - 0.5f * (float)(n - 1)) / (0.5f * (float)(n - 1)) : 0.0f;   // -1..1
            equalPowerPan(p * s_.stereo, gl_[i], gr_[i]);
            gl_[i] *= kSqrt2; gr_[i] *= kSqrt2;
        }
        norm_ = 0.7f / std::sqrt((float)n);
        bendRange_ = (float)getInt(kBend);
    }

    // ----- MIDI: monophonic with a stack of held notes (last note wins) ------
    void removeHeld(int note)
    {
        int j = 0;
        for (int i = 0; i < held_; ++i) if (heldNote_[i] != note) heldNote_[j++] = heldNote_[i];
        held_ = j;
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn())
        {
            removeHeld(ev.note());
            const bool wasHeld = held_ > 0;
            if (held_ < kMaxHeld) heldNote_[held_++] = ev.note();
            const bool sounding = ampEnv_.isActive();
            if (s_.glideMs > 0.0f && sounding) glide_.setTarget((float)ev.note());
            else glide_.snap((float)ev.note());
            if (!(s_.legato && wasHeld && ampEnv_.isHeld()))
            {
                velN_ = ev.velocity() / 127.0f;
                if (!sounding)
                {
                    for (int i = 0; i < kMaxUnison; ++i) osc_[i].resetPhase(phaseSeed(i));
                    sub_.resetPhase();
                    for (auto& f : filt_) f.reset();
                }
                ampEnv_.noteOn(); filtEnv_.noteOn();
                age_ = 0; ctr_ = 0;
            }
        }
        else if (ev.isNoteOff())
        {
            const bool wasTop = held_ > 0 && heldNote_[held_ - 1] == ev.note();
            removeHeld(ev.note());
            if (held_ == 0) { ampEnv_.noteOff(); filtEnv_.noteOff(); }
            else if (wasTop) glide_.setTarget((float)heldNote_[held_ - 1]);
        }
        else if (ev.isAllNotesOff()) { held_ = 0; ampEnv_.noteOff(); filtEnv_.noteOff(); }
        else if (ev.isPitchBend()) bend_ = (float)ev.pitchBend() / 8192.0f;
        else controllers_.handle(ev);
    }

    static float phaseSeed(int i) { return std::fmod(0.1234f + 0.6180339f * (float)i, 1.0f); }

    void render(float* L, float* R, int from, int to)
    {
        const float octShift = 12.0f * (float)s_.octave;
        const float velGain = lerp(1.0f, velN_, s_.velSens);
        const int n = s_.unison;

        for (int i = from; i < to; ++i)
        {
            controllers_.tick();
            const float note = glide_.process();
            const float aenv = ampEnv_.process();
            const float fenv = filtEnv_.process();
            const float vibLfo = vib_.process();
            const float pwmLfo = pwm_.process();

            if ((ctr_++ % kCtrlInterval) == 0)
            {
                const float fade = s_.vibDelaySamples > 1.0f ? clamp((float)age_ / s_.vibDelaySamples, 0.0f, 1.0f) : 1.0f;
                const float vibSt = vibLfo * (s_.vibDepth * fade + 2.0f * controllers_.modWheel());
                const float baseNote = note + octShift + vibSt + bend_ * bendRange_;
                const float hz = midiToHz(baseNote);
                for (int u = 0; u < n; ++u)
                {
                    const float off = (n > 1) ? ((float)u - 0.5f * (float)(n - 1)) / (0.5f * (float)(n - 1)) : 0.0f;   // -1..1
                    osc_[u].setFrequency(hz * centsToRatio(off * s_.detune));
                    osc_[u].setPulseWidth(s_.pw + s_.pwmDepth * 0.4f * pwmLfo);
                }
                sub_.setFrequency(hz * 0.5f);
                const float octaves = s_.envAmt * fenv * 5.0f + s_.keyTrack * (note - 60.0f) / 12.0f;
                const float cutoff = s_.cutoff * std::pow(2.0f, clamp(octaves, -8.0f, 8.0f));
                for (int c = 0; c < 2; ++c) { filt_[c * 2].set(cutoff, 0.0f); filt_[c * 2 + 1].set(cutoff, s_.reso); }
                age_ += kCtrlInterval;
            }

            float outL = 0.0f, outR = 0.0f;
            if (ampEnv_.isActive())
            {
                float sl = 0.0f, sr = 0.0f;
                for (int u = 0; u < n; ++u) { const float y = osc_[u].process(); sl += y * gl_[u]; sr += y * gr_[u]; }
                sl *= norm_; sr *= norm_;
                const float sb = sub_.process() * s_.sub * 0.6f;
                sl += sb; sr += sb;
                outL = filt_[1].process(filt_[0].process(sl, SVF::Mode::Lowpass), SVF::Mode::Lowpass) * aenv * velGain;
                outR = filt_[3].process(filt_[2].process(sr, SVF::Mode::Lowpass), SVF::Mode::Lowpass) * aenv * velGain;
            }
            L[i] = outL;
            if (R) R[i] = outR;
        }
    }

    // Master volume and the ping-pong delay.
    void applyDelay(float* L, float* R, mpc::ProcessContext& ctx)
    {
        const float mix = get(kDelayMix) * 0.01f, fb = get(kDelayFeedback) * 0.01f;
        const float tSamples = get(kDelayTime) * 0.001f * (float)ctx.sampleRate;
        const bool fx = mix > 0.001f;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            float l = L[i], r = R ? R[i] : L[i];
            if (fx)
            {
                const float wl = dlyL_.readLinear(tSamples), wr = dlyR_.readLinear(tSamples * 1.5f > (float)dlyR_.maxDelay() ? (float)dlyR_.maxDelay() : tSamples * 1.5f);
                dlyL_.write(softClip(l + wr * fb));       // ping-pong: each side feeds the other
                dlyR_.write(softClip(r + wl * fb));
                l += wl * mix; r += wr * mix;
            }
            const float g = volume_.process();
            L[i] = softClip(l * g);
            if (R) R[i] = softClip(r * g);
        }
    }

    PolyBlepOsc osc_[kMaxUnison];
    PolyBlepOsc sub_;
    SVF filt_[4];                       // [0,1] left channel, [2,3] right channel
    ADSR ampEnv_, filtEnv_;
    LFO vib_, pwm_;
    Smoother glide_, volume_;
    MidiControllers controllers_;
    DelayLine dlyL_, dlyR_;

    float gl_[kMaxUnison] = {1}, gr_[kMaxUnison] = {1};
    float norm_ = 0.4f, bend_ = 0.0f, bendRange_ = 2.0f, velN_ = 0.0f;
    int heldNote_[kMaxHeld] = {0}, held_ = 0, ctr_ = 0;
    uint32_t age_ = 0;
    double sr_ = 44100.0;
};

MPC_PLUGIN_ENTRY(Leadline)
