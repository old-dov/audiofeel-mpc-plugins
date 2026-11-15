// Padscape - an 8-voice pad synth with chorus and reverb for the MPC and VST2 hosts, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// Signal flow, per voice (8 voices):
//   layer A (wave, detuned pair) + layer B (wave, own octave, detuned pair), blended
//     -> two cascaded 12 dB low-passes = 24 dB/oct
//        cutoff = Cutoff * 2^( Env Amount * filter EG + Key Track + slow LFO )
//        (each voice sees the LFO at a different phase, so the pad moves instead of pumping)
//     -> slow ADSR -> per-voice stereo position (Width)
//   all voices -> master volume -> stereo chorus -> reverb -> outputs
//
// The reverb is a Freeverb-style network (8 damped combs + 4 allpasses per side).
// All buffers are allocated in prepare(); nothing is allocated while playing.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"
#include "dsp/voice.h"
#include "dsp/delay_line.h"
#include "dsp/midi_cc.h"

#include <vector>

using namespace mpc::dsp;

namespace {

// One comb filter with damping (low-pass in the feedback loop).
struct Comb
{
    std::vector<float> buf; int idx = 0; float store = 0.0f;
    void prepare(int n) { buf.assign((size_t)(n < 1 ? 1 : n), 0.0f); idx = 0; store = 0.0f; }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); idx = 0; store = 0.0f; }
    float process(float in, float feedback, float damp)
    {
        const float out = buf[(size_t)idx];
        store = out * (1.0f - damp) + store * damp;
        buf[(size_t)idx] = in + store * feedback;
        if (++idx >= (int)buf.size()) idx = 0;
        return out;
    }
};

struct Allpass
{
    std::vector<float> buf; int idx = 0;
    void prepare(int n) { buf.assign((size_t)(n < 1 ? 1 : n), 0.0f); idx = 0; }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); idx = 0; }
    float process(float in)
    {
        const float y = buf[(size_t)idx];
        buf[(size_t)idx] = in + y * 0.5f;
        if (++idx >= (int)buf.size()) idx = 0;
        return y - in;
    }
};

class Reverb
{
public:
    void prepare(double sr)
    {
        static const int combs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int aps[4] = {556, 441, 341, 225};
        const double k = sr / 44100.0;
        for (int i = 0; i < 8; ++i) { cl_[i].prepare((int)(combs[i] * k)); cr_[i].prepare((int)((combs[i] + 23) * k)); }
        for (int i = 0; i < 4; ++i) { al_[i].prepare((int)(aps[i] * k)); ar_[i].prepare((int)((aps[i] + 23) * k)); }
    }
    void clear() { for (int i = 0; i < 8; ++i) { cl_[i].clear(); cr_[i].clear(); } for (int i = 0; i < 4; ++i) { al_[i].clear(); ar_[i].clear(); } }
    void process(float inL, float inR, float size01, float damp01, float& outL, float& outR)
    {
        const float fb = 0.70f + 0.28f * size01;
        const float damp = 0.05f + 0.8f * damp01;
        const float in = (inL + inR) * 0.015f;
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < 8; ++i) { l += cl_[i].process(in, fb, damp); r += cr_[i].process(in, fb, damp); }
        for (int i = 0; i < 4; ++i) { l = al_[i].process(l); r = ar_[i].process(r); }
        outL = l; outR = r;
    }
private:
    Comb cl_[8], cr_[8];
    Allpass al_[4], ar_[4];
};

} // namespace

class Padscape : public mpc::Plugin
{
public:
    // Parameter order is permanent once users have projects: append new ones at the end.
    // Parameter 0 is nudged by the host on every load, so it is the harmless master volume.
    enum Params
    {
        kVolume = 0,
        kWaveA, kWaveB, kOctaveB, kDetune, kBlend, kWidth,
        kCutoff, kReso, kEnvAmount, kKeyTrack, kLfoRate, kLfoDepth,
        kAttack, kDecay, kSustain, kRelease, kVelocity,
        kChorusMix, kChorusDepth, kChorusRate,
        kReverbMix, kReverbSize, kReverbDamp,
        kBend,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Padscape";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', 'o', 'd', 'E');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Padscape() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        const std::vector<std::string> waves = {"Sine", "Triangle", "Saw", "Square"};
        addParam(floatParam ("volume",  "Volume",       -40.0f, 6.0f, -8.0f, "dB", 1));
        addParam(choiceParam("wavea",   "Wave A",       waves, 2));
        addParam(choiceParam("waveb",   "Wave B",       waves, 1));
        addParam(intParam   ("octb",    "Octave B",    -2, 2, 0));
        addParam(floatParam ("detune",  "Detune",       0.0f, 40.0f, 9.0f, "ct", 1));
        addParam(floatParam ("blend",   "Blend",        0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("width",   "Width",        0.0f, 100.0f, 70.0f, "%", 0));
        addParam(logParam   ("cutoff",  "Cutoff",       80.0f, 16000.0f, 2200.0f, "Hz", 0));
        addParam(floatParam ("reso",    "Resonance",    0.0f, 100.0f, 12.0f, "%", 0));
        addParam(floatParam ("envamt",  "Env Amount",  -100.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam ("keytrk",  "Key Track",    0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam   ("lforate", "LFO Rate",     0.03f, 4.0f, 0.25f, "Hz", 2));
        addParam(floatParam ("lfodepth", "LFO Depth",   0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam   ("attack",  "Attack",       5.0f, 8000.0f, 700.0f, "ms", 0));
        addParam(logParam   ("decay",   "Decay",        10.0f, 8000.0f, 900.0f, "ms", 0));
        addParam(floatParam ("sustain", "Sustain",      0.0f, 100.0f, 85.0f, "%", 0));
        addParam(logParam   ("release", "Release",      10.0f, 12000.0f, 1600.0f, "ms", 0));
        addParam(floatParam ("velsens", "Velocity",     0.0f, 100.0f, 35.0f, "%", 0));
        addParam(floatParam ("chorusmix", "Chorus Mix", 0.0f, 100.0f, 35.0f, "%", 0));
        addParam(floatParam ("chorusdepth", "Chorus Depth", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam   ("chorusrate", "Chorus Rate", 0.05f, 3.0f, 0.4f, "Hz", 2));
        addParam(floatParam ("revmix",  "Reverb Mix",   0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam ("revsize", "Reverb Size",  0.0f, 100.0f, 70.0f, "%", 0));
        addParam(floatParam ("revdamp", "Reverb Damp",  0.0f, 100.0f, 45.0f, "%", 0));
        addParam(intParam   ("bend",    "Bend Range",   0, 12, 2, "st"));

        if (numParams() != kNumParams) mpc::log("Padscape: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        // Waves: 0 Sine, 1 Triangle, 2 Saw, 3 Square.
        program("Init Pad",         {});
        program("Warm Strings Pad", {{kWaveA, 2}, {kWaveB, 2}, {kDetune, 13}, {kBlend, 60}, {kCutoff, 1700}, {kEnvAmount, 25}, {kAttack, 900},
                                     {kRelease, 1800}, {kChorusMix, 50}, {kReverbMix, 28}});
        program("Glass Pad",        {{kWaveA, 0}, {kWaveB, 1}, {kOctaveB, 1}, {kDetune, 6}, {kBlend, 45}, {kCutoff, 7000}, {kReso, 6}, {kEnvAmount, 10},
                                     {kAttack, 1100}, {kRelease, 2600}, {kChorusMix, 40}, {kReverbMix, 45}, {kReverbSize, 80}});
        program("Dark Drone",       {{kWaveA, 2}, {kWaveB, 3}, {kOctaveB, -1}, {kDetune, 11}, {kBlend, 55}, {kCutoff, 600}, {kReso, 25}, {kEnvAmount, 15},
                                     {kLfoRate, 0.12f}, {kLfoDepth, 55}, {kAttack, 2200}, {kSustain, 100}, {kRelease, 3500}, {kReverbMix, 40}, {kReverbSize, 85}});
        program("Soft Choir",       {{kWaveA, 1}, {kWaveB, 0}, {kOctaveB, 1}, {kDetune, 10}, {kBlend, 40}, {kCutoff, 2600}, {kReso, 8}, {kEnvAmount, 10},
                                     {kAttack, 1300}, {kRelease, 2200}, {kChorusMix, 55}, {kChorusDepth, 60}, {kReverbMix, 40}});
        program("Evolving Saw",     {{kWaveA, 2}, {kWaveB, 2}, {kDetune, 16}, {kBlend, 50}, {kCutoff, 1200}, {kReso, 28}, {kEnvAmount, 20}, {kLfoRate, 0.15f},
                                     {kLfoDepth, 70}, {kAttack, 1500}, {kRelease, 2500}, {kChorusMix, 40}, {kReverbMix, 32}});
        program("Soft Sweep",       {{kWaveA, 2}, {kWaveB, 1}, {kDetune, 8}, {kCutoff, 500}, {kReso, 20}, {kEnvAmount, 70}, {kAttack, 2600}, {kDecay, 3500},
                                     {kSustain, 60}, {kRelease, 2500}, {kReverbMix, 35}});
        program("Cinematic Swell",  {{kWaveA, 2}, {kWaveB, 2}, {kOctaveB, -1}, {kDetune, 15}, {kBlend, 55}, {kCutoff, 900}, {kEnvAmount, 55}, {kAttack, 3800},
                                     {kDecay, 4500}, {kSustain, 80}, {kRelease, 5500}, {kChorusMix, 35}, {kReverbMix, 55}, {kReverbSize, 92}});
        program("Shimmer Pad",      {{kWaveA, 1}, {kWaveB, 0}, {kOctaveB, 2}, {kDetune, 7}, {kBlend, 35}, {kCutoff, 9000}, {kReso, 5}, {kEnvAmount, 5},
                                     {kAttack, 1000}, {kRelease, 3000}, {kChorusMix, 60}, {kChorusDepth, 70}, {kChorusRate, 0.6f}, {kReverbMix, 50}, {kReverbSize, 88}});
        program("Ambient Wash",     {{kWaveA, 0}, {kWaveB, 1}, {kDetune, 5}, {kBlend, 50}, {kCutoff, 3200}, {kEnvAmount, 10}, {kAttack, 2400}, {kRelease, 4500},
                                     {kChorusMix, 30}, {kReverbMix, 70}, {kReverbSize, 95}, {kReverbDamp, 55}});
        program("Slow Square",      {{kWaveA, 3}, {kWaveB, 2}, {kDetune, 10}, {kBlend, 35}, {kCutoff, 1100}, {kReso, 15}, {kEnvAmount, 30}, {kAttack, 1200},
                                     {kRelease, 1800}, {kChorusMix, 45}, {kReverbMix, 30}});
        program("Lush Ensemble",    {{kWaveA, 2}, {kWaveB, 2}, {kDetune, 18}, {kBlend, 50}, {kWidth, 100}, {kCutoff, 2000}, {kEnvAmount, 20}, {kAttack, 800},
                                     {kRelease, 1500}, {kChorusMix, 70}, {kChorusDepth, 65}, {kReverbMix, 35}});
        program("Sub Pad",          {{kWaveA, 0}, {kWaveB, 1}, {kOctaveB, -1}, {kDetune, 3}, {kBlend, 60}, {kCutoff, 900}, {kReso, 0}, {kEnvAmount, 0},
                                     {kKeyTrack, 20}, {kAttack, 600}, {kRelease, 1400}, {kChorusMix, 15}, {kReverbMix, 20}});
        program("Pluck Pad",        {{kWaveA, 2}, {kWaveB, 3}, {kDetune, 9}, {kBlend, 40}, {kCutoff, 1000}, {kReso, 18}, {kEnvAmount, 65}, {kAttack, 12},
                                     {kDecay, 900}, {kSustain, 35}, {kRelease, 900}, {kChorusMix, 35}, {kReverbMix, 30}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_)
        {
            v.a1.setSampleRate(sampleRate); v.a2.setSampleRate(sampleRate);
            v.b1.setSampleRate(sampleRate); v.b2.setSampleRate(sampleRate);
            v.f1.setSampleRate(sampleRate); v.f2.setSampleRate(sampleRate);
            v.ampEnv.setSampleRate(sampleRate); v.filtEnv.setSampleRate(sampleRate);
        }
        controllers_.prepare(sampleRate, 10.0f);
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
        const int maxChorus = (int)(sampleRate * 0.04) + 8;
        chL_.prepare(maxChorus); chR_.prepare(maxChorus);
        reverb_.prepare(sampleRate);
    }

    void reset() override
    {
        for (auto& v : voices_) { v.ampEnv.reset(); v.filtEnv.reset(); v.f1.reset(); v.f2.reset(); v.note = -1; }
        alloc_.reset(); controllers_.reset();
        chL_.reset(); chR_.reset(); reverb_.clear();
        lfoPhase_ = 0.0f; chPhase_ = 0.0f;
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
            handleMidi(ev);
        }
        render(L, R, pos, ctx.numFrames);
        applyEffects(L, R, ctx);
    }

private:
    static constexpr int kNumVoices = 8;
    static constexpr int kChunk = 64;
    static constexpr int kCtrlInterval = 16;

    struct Voice
    {
        PolyBlepOsc a1, a2, b1, b2;
        SVF f1, f2;
        ADSR ampEnv, filtEnv;
        int note = -1, ctrl = 0, index = 0;
        float velGain = 1.0f, panL = 0.7071f, panR = 0.7071f;
    };

    struct Settings
    {
        int waveA = 2, waveB = 1, octB = 0;
        float detune = 9.0f, blend = 0.5f, width = 0.7f, cutoff = 2200.0f, reso = 0.12f, envAmt = 0.2f, keyTrack = 0.5f, lfoDepth = 0.3f, velSens = 0.35f;
    } s_;

    void readSettings(double sr)
    {
        s_.waveA = getInt(kWaveA); s_.waveB = getInt(kWaveB); s_.octB = getInt(kOctaveB);
        s_.detune = get(kDetune); s_.blend = get(kBlend) * 0.01f; s_.width = get(kWidth) * 0.01f;
        s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f; s_.envAmt = get(kEnvAmount) * 0.01f; s_.keyTrack = get(kKeyTrack) * 0.01f;
        s_.lfoDepth = get(kLfoDepth) * 0.01f; s_.velSens = get(kVelocity) * 0.01f;
        lfoInc_ = get(kLfoRate) / (float)sr;

        alloc_.setBendRange((float)getInt(kBend));
        volume_.setTarget(dbToGain(get(kVolume)));

        const float aa = get(kAttack), ad = get(kDecay), as = get(kSustain) * 0.01f, ar = get(kRelease);
        for (auto& v : voices_)
        {
            v.a1.setWave(s_.waveA); v.a2.setWave(s_.waveA);
            v.b1.setWave(s_.waveB); v.b2.setWave(s_.waveB);
            v.ampEnv.set(aa, ad, as, ar);
            v.filtEnv.set(aa * 0.8f, ad, as, ar);
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
            v.index = vi;
            const float pan = std::fmod((float)vi * 0.618034f, 1.0f) * 2.0f - 1.0f;      // a fixed position per voice slot
            equalPowerPan(pan * s_.width, v.panL, v.panR);
            v.panL *= kSqrt2; v.panR *= kSqrt2;
            if (!v.ampEnv.isActive())
            {
                v.a1.resetPhase(0.00f); v.a2.resetPhase(0.37f); v.b1.resetPhase(0.61f); v.b2.resetPhase(0.13f);
                v.f1.reset(); v.f2.reset();
            }
            v.ampEnv.noteOn(); v.filtEnv.noteOn();
            v.ctrl = 0;
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

    void release(int vi) { voices_[vi].ampEnv.noteOff(); voices_[vi].filtEnv.noteOff(); }

    void render(float* L, float* R, int from, int to)
    {
        while (from < to)
        {
            const int n = (to - from) < kChunk ? (to - from) : kChunk;
            for (int i = 0; i < n; ++i) controllers_.tick();
            lfoPhase_ += lfoInc_ * (float)n; if (lfoPhase_ >= 1.0f) lfoPhase_ -= 1.0f;

            for (int vi = 0; vi < kNumVoices; ++vi)
            {
                Voice& v = voices_[vi];
                if (!v.ampEnv.isActive()) continue;
                renderVoice(v, L + from, R ? R + from : nullptr, n, vi);
                if (!v.ampEnv.isActive()) alloc_.voiceFinished(vi);
            }
            from += n;
        }
    }

    void renderVoice(Voice& v, float* L, float* R, int n, int vi)
    {
        const float detA = centsToRatio(s_.detune), detB = centsToRatio(s_.detune * 0.7f);
        for (int i = 0; i < n; ++i)
        {
            const float fenv = v.filtEnv.process();
            const float aenv = v.ampEnv.process();

            if ((v.ctrl++ % kCtrlInterval) == 0)
            {
                const float hz = alloc_.noteToHz((float)v.note);
                const float hzB = hz * semitonesToRatio(12.0f * (float)s_.octB);
                v.a1.setFrequency(hz * detA); v.a2.setFrequency(hz / detA);
                v.b1.setFrequency(hzB * detB); v.b2.setFrequency(hzB / detB);
                const float lfo = fastSin01(lfoPhase_ + (float)vi * 0.17f);       // a different phase per voice slot
                const float octaves = s_.envAmt * fenv * 5.0f + s_.keyTrack * ((float)v.note - 60.0f) / 12.0f + s_.lfoDepth * lfo * 2.0f;
                const float cutoff = s_.cutoff * std::pow(2.0f, clamp(octaves, -8.0f, 8.0f));
                v.f1.set(cutoff, 0.0f); v.f2.set(cutoff, s_.reso);
            }

            const float layerA = (v.a1.process() + v.a2.process()) * 0.5f;
            const float layerB = (v.b1.process() + v.b2.process()) * 0.5f;
            const float mix = layerA * (1.0f - s_.blend) + layerB * s_.blend;
            const float y = v.f2.process(v.f1.process(mix, SVF::Mode::Lowpass), SVF::Mode::Lowpass) * aenv * v.velGain * 0.7f;
            L[i] += y * v.panL;
            if (R) R[i] += y * v.panR;
        }
    }

    // Master volume, stereo chorus, reverb.
    void applyEffects(float* L, float* R, mpc::ProcessContext& ctx)
    {
        const float chMix = get(kChorusMix) * 0.01f, chDepth = get(kChorusDepth) * 0.01f;
        const float chInc = get(kChorusRate) / (float)ctx.sampleRate;
        const float baseSamples = 0.012f * (float)ctx.sampleRate, swing = 0.005f * (float)ctx.sampleRate * chDepth;
        const float revMix = get(kReverbMix) * 0.01f, revSize = get(kReverbSize) * 0.01f, revDamp = get(kReverbDamp) * 0.01f;
        const bool chorusOn = chMix > 0.001f, reverbOn = revMix > 0.001f;

        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float g = volume_.process();
            float l = L[i] * g, r = (R ? R[i] : L[i]) * g;

            if (chorusOn)
            {
                chPhase_ += chInc; if (chPhase_ >= 1.0f) chPhase_ -= 1.0f;
                const float wl = chL_.readLinear(baseSamples + swing * fastSin01(chPhase_));
                const float wr = chR_.readLinear(baseSamples + swing * fastSin01(chPhase_ + 0.5f));
                chL_.write(l); chR_.write(r);
                l = l * (1.0f - 0.35f * chMix) + wl * chMix * 0.8f;
                r = r * (1.0f - 0.35f * chMix) + wr * chMix * 0.8f;
            }
            if (reverbOn)
            {
                float wl, wr;
                reverb_.process(l, r, revSize, revDamp, wl, wr);
                l += wl * revMix * 1.6f; r += wr * revMix * 1.6f;
            }
            L[i] = softClip(l);
            if (R) R[i] = softClip(r);
        }
    }

    Voice voices_[kNumVoices];
    VoiceAllocator<kNumVoices> alloc_;
    MidiControllers controllers_;
    Smoother volume_;
    DelayLine chL_, chR_;
    Reverb reverb_;
    float lfoPhase_ = 0.0f, lfoInc_ = 0.0f, chPhase_ = 0.0f;
    double sr_ = 44100.0;
};

MPC_PLUGIN_ENTRY(Padscape)
