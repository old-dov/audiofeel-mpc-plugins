// Dronescape - dark drones and evolving textures for the MPC, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// 6-voice polyphonic. Each voice stacks: a sine sub (an octave down), two detuned saws, an optional
// second pair of saws a chosen interval above (fifth, tritone, minor second...: the classic
// tension-drone trick), and a band of tonal noise tuned to the note. All of it runs through a
// two-pole low-pass that two slow LFOs (the second at 0.37 x the first, a different phase per voice)
// keep moving, while a slow pitch drift per voice keeps the unison from ever settling.
//
// Signal flow: [sub + saws + layer + noise] -> LP x2 (evolving) -> amp envelope -> voice pan
//              -> volume -> chorus -> reverb.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/noise.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/voice.h"
#include "dsp/midi_cc.h"
#include "cine_fx.h"

#include <string>
#include <vector>

using namespace mpc::dsp;

class Dronescape : public mpc::Plugin
{
public:
    // Parameter order is permanent once users have projects: append new ones at the end.
    // Parameter 0 is nudged by the host on every load, so it is the harmless master volume.
    enum Params
    {
        kVolume = 0,
        kSub, kSaw, kNoise, kDetune, kLayer, kLayerLevel, kDrift,
        kCutoff, kReso, kEvolveRate, kEvolveDepth, kAttack, kRelease,
        kChorusMix, kRevMix, kRevSize, kRevDamp,
        kBend,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Dronescape";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', '0', 'y', 'f');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Dronescape() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("volume",   "Volume",       -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(floatParam ("sub",      "Sub",           0.0f, 100.0f, 60.0f, "%", 0));
        addParam(floatParam ("saw",      "Saw",           0.0f, 100.0f, 70.0f, "%", 0));
        addParam(floatParam ("noise",    "Noise",         0.0f, 100.0f, 15.0f, "%", 0));
        addParam(floatParam ("detune",   "Detune",        0.0f, 50.0f, 12.0f, "ct", 1));
        addParam(choiceParam("layer",    "Layer",         {"Off", "Fifth", "Fourth", "Octave", "Tritone", "Minor 2nd", "Minor 3rd"}, 0));
        addParam(floatParam ("layerlvl", "Layer Level",   0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("drift",    "Drift",         0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam   ("cutoff",   "Cutoff",        60.0f, 12000.0f, 700.0f, "Hz", 0));
        addParam(floatParam ("reso",     "Resonance",     0.0f, 100.0f, 20.0f, "%", 0));
        addParam(logParam   ("evrate",   "Evolve Rate",   0.02f, 2.0f, 0.1f, "Hz", 2));
        addParam(floatParam ("evdepth",  "Evolve Depth",  0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam   ("attack",   "Attack",        20.0f, 12000.0f, 2500.0f, "ms", 0));
        addParam(logParam   ("release",  "Release",       50.0f, 15000.0f, 4000.0f, "ms", 0));
        addParam(floatParam ("chorusmix", "Chorus Mix",   0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam ("revmix",   "Rev Mix",       0.0f, 100.0f, 40.0f, "%", 0));
        addParam(floatParam ("revsize",  "Rev Size",      0.0f, 100.0f, 85.0f, "%", 0));
        addParam(floatParam ("revdamp",  "Rev Damp",      0.0f, 100.0f, 50.0f, "%", 0));
        addParam(intParam   ("bend",     "Bend Range",    0, 12, 2, "st"));
        if (numParams() != kNumParams) mpc::log("Dronescape: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        // Layer: 0 Off, 1 Fifth, 2 Fourth, 3 Octave, 4 Tritone, 5 Minor 2nd, 6 Minor 3rd.
        program("Init Drone",     {});
        program("Dark Drone",     {{kSub, 75}, {kSaw, 55}, {kNoise, 10}, {kCutoff, 420}, {kReso, 25}, {kEvolveRate, 0.07f}, {kEvolveDepth, 60}, {kAttack, 3500}, {kRelease, 6000}});
        program("Abyss",          {{kSub, 100}, {kSaw, 35}, {kNoise, 6}, {kDetune, 8}, {kCutoff, 260}, {kReso, 10}, {kDrift, 45}, {kAttack, 5000}, {kRelease, 9000}, {kRevMix, 55}, {kRevSize, 95}});
        program("Tension Bed",    {{kSub, 45}, {kSaw, 65}, {kLayer, 5}, {kLayerLevel, 55}, {kDetune, 14}, {kCutoff, 900}, {kReso, 30}, {kEvolveRate, 0.13f}, {kEvolveDepth, 65}, {kAttack, 4000}, {kRelease, 7000}});
        program("Tritone Dread",  {{kSub, 55}, {kSaw, 60}, {kLayer, 4}, {kLayerLevel, 60}, {kDetune, 10}, {kCutoff, 650}, {kReso, 35}, {kDrift, 50}, {kAttack, 4500}, {kRelease, 8000}, {kRevMix, 50}});
        program("Wind Texture",   {{kSub, 10}, {kSaw, 10}, {kNoise, 85}, {kCutoff, 1800}, {kReso, 60}, {kEvolveRate, 0.2f}, {kEvolveDepth, 85}, {kAttack, 3000}, {kRelease, 5000}, {kChorusMix, 35}, {kRevMix, 50}});
        program("Industrial Hum", {{kSub, 60}, {kSaw, 85}, {kNoise, 30}, {kDetune, 4}, {kCutoff, 1100}, {kReso, 45}, {kEvolveRate, 0.4f}, {kEvolveDepth, 30}, {kDrift, 15}, {kAttack, 800}, {kRelease, 2500}, {kChorusMix, 10}, {kRevMix, 25}});
        program("Ice Field",      {{kSub, 20}, {kSaw, 50}, {kLayer, 3}, {kLayerLevel, 70}, {kNoise, 25}, {kDetune, 18}, {kCutoff, 4200}, {kReso, 35}, {kEvolveRate, 0.09f}, {kEvolveDepth, 70}, {kAttack, 3500}, {kRelease, 7500}, {kChorusMix, 40}, {kRevMix, 60}});
        program("Void",           {{kSub, 90}, {kSaw, 25}, {kLayer, 1}, {kLayerLevel, 30}, {kCutoff, 180}, {kReso, 5}, {kEvolveDepth, 20}, {kDrift, 20}, {kAttack, 7000}, {kRelease, 12000}, {kRevMix, 65}, {kRevSize, 98}});
        program("Slow Breath",    {{kSub, 50}, {kSaw, 75}, {kLayer, 1}, {kLayerLevel, 35}, {kCutoff, 500}, {kReso, 22}, {kEvolveRate, 0.16f}, {kEvolveDepth, 90}, {kAttack, 2200}, {kRelease, 3500}});
        program("Minor Cloud",    {{kSub, 55}, {kSaw, 70}, {kLayer, 6}, {kLayerLevel, 55}, {kDetune, 16}, {kCutoff, 1000}, {kEvolveRate, 0.1f}, {kEvolveDepth, 55}, {kAttack, 3000}, {kRelease, 6000}, {kChorusMix, 45}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sampleRate_ = (float)sampleRate;
        for (auto& v : voices_)
        {
            for (auto& o : v.saw) { o.setSampleRate(sampleRate); o.setWave(Wave::Saw); }
            v.f1.setSampleRate(sampleRate); v.f2.setSampleRate(sampleRate); v.nf.setSampleRate(sampleRate);
            v.env.setSampleRate(sampleRate);
        }
        controllers_.prepare(sampleRate, 10.0f);
        fx_.prepare(sampleRate, dbToGain(get(kVolume)));
    }

    void reset() override
    {
        for (auto& v : voices_) { v.env.reset(); v.f1.reset(); v.f2.reset(); v.nf.reset(); v.note = -1; }
        alloc_.reset(); controllers_.reset();
        evPhase_ = 0.0f;
        fx_.reset(dbToGain(get(kVolume)));
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
        fx_.process(L, R, ctx.numFrames, ctx.sampleRate, dbToGain(get(kVolume)), get(kChorusMix) * 0.01f, 0.5f, 0.3f,
                    get(kRevMix) * 0.01f, get(kRevSize) * 0.01f, get(kRevDamp) * 0.01f);
    }

private:
    static constexpr int kNumVoices = 6;
    static constexpr int kChunk = 64;
    static constexpr int kCtrlInterval = 16;

    struct Voice
    {
        PolyBlepOsc saw[4];                // 0,1: detuned pair   2,3: pair at the layer interval
        float subPhase = 0.0f, subHz = 50.0f;
        SVF f1, f2, nf;
        ADSR env;
        Xorshift32 rng{0x9E3779B9u};
        int note = -1, ctrl = 0;
        float velGain = 1.0f, panL = 0.7071f, panR = 0.7071f;
        float driftPhase = 0.0f, driftInc = 0.0f;
    };

    struct Settings
    {
        float sub = 0.6f, saw = 0.7f, noise = 0.15f, detune = 12.0f, layerLevel = 0.5f, drift = 0.3f;
        float cutoff = 700.0f, reso = 0.2f, evDepth = 0.5f;
        int layer = 0;
    } s_;

    void readSettings(double sr)
    {
        s_.sub = get(kSub) * 0.01f; s_.saw = get(kSaw) * 0.01f; s_.noise = get(kNoise) * 0.01f;
        s_.detune = get(kDetune); s_.layer = getInt(kLayer); s_.layerLevel = get(kLayerLevel) * 0.01f;
        s_.drift = get(kDrift) * 0.01f; s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f; s_.evDepth = get(kEvolveDepth) * 0.01f;
        evInc_ = get(kEvolveRate) / (float)sr;
        alloc_.setBendRange((float)getInt(kBend));
        for (auto& v : voices_) v.env.set(get(kAttack), 500.0f, 1.0f, get(kRelease));
    }

    static float layerSemitones(int layer)
    {
        static const float st[7] = {0.0f, 7.0f, 5.0f, 12.0f, 6.0f, 1.0f, 3.0f};
        return st[layer < 0 ? 0 : (layer > 6 ? 6 : layer)];
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn())
        {
            const int vi = alloc_.noteOn(ev.note(), ev.velocity());
            Voice& v = voices_[vi];
            v.note = ev.note();
            v.velGain = lerp(1.0f, ev.velocity() / 127.0f, 0.3f);
            v.ctrl = 0;
            v.driftInc = (0.04f + 0.017f * (float)vi) / 44100.0f;
            const float pan = std::fmod((float)vi * 0.618034f, 1.0f) * 2.0f - 1.0f;
            equalPowerPan(pan * 0.8f, v.panL, v.panR);
            v.panL *= kSqrt2; v.panR *= kSqrt2;
            if (!v.env.isActive())
            {
                for (int i = 0; i < 4; ++i) v.saw[i].resetPhase(0.19f * (float)i);
                v.subPhase = 0.0f; v.f1.reset(); v.f2.reset(); v.nf.reset();
            }
            v.env.noteOn();
        }
        else if (ev.isNoteOff()) { const int vi = alloc_.noteOff(ev.note()); if (vi >= 0) voices_[vi].env.noteOff(); }
        else if (ev.isAllNotesOff()) alloc_.allNotesOff([this](int vi) { voices_[vi].env.noteOff(); });
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            controllers_.handle(ev);
            alloc_.setSustain(ev.controllerValue() >= 64, [this](int vi) { voices_[vi].env.noteOff(); });
        }
        else if (ev.isPitchBend()) alloc_.setPitchBend(ev.pitchBend());
        else controllers_.handle(ev);
    }

    void render(float* L, float* R, int from, int to)
    {
        while (from < to)
        {
            const int n = (to - from) < kChunk ? (to - from) : kChunk;
            for (int i = 0; i < n; ++i) controllers_.tick();
            evPhase_ += evInc_ * (float)n; if (evPhase_ >= 1.0f) evPhase_ -= 1.0f;
            for (int vi = 0; vi < kNumVoices; ++vi)
            {
                Voice& v = voices_[vi];
                if (!v.env.isActive()) continue;
                renderVoice(v, L + from, R ? R + from : nullptr, n, vi);
                if (!v.env.isActive()) alloc_.voiceFinished(vi);
            }
            from += n;
        }
    }

    void renderVoice(Voice& v, float* L, float* R, int n, int vi)
    {
        const float det = centsToRatio(s_.detune);
        const float layerRatio = semitonesToRatio(layerSemitones(s_.layer));
        const bool layerOn = s_.layer > 0 && s_.layerLevel > 0.001f;
        for (int i = 0; i < n; ++i)
        {
            const float aenv = v.env.process();
            if ((v.ctrl++ % kCtrlInterval) == 0)
            {
                v.driftPhase += v.driftInc * (float)kCtrlInterval; if (v.driftPhase >= 1.0f) v.driftPhase -= 1.0f;
                const float driftRatio = centsToRatio(s_.drift * 14.0f * fastSin01(v.driftPhase));
                const float hz = alloc_.noteToHz((float)v.note) * driftRatio;
                v.saw[0].setFrequency(hz * det); v.saw[1].setFrequency(hz / det);
                v.saw[2].setFrequency(hz * layerRatio * det); v.saw[3].setFrequency(hz * layerRatio / det);
                v.subHz = hz * 0.5f;
                const float l1 = fastSin01(evPhase_ + (float)vi * 0.21f);
                const float l2 = fastSin01(evPhase_ * 0.37f + (float)vi * 0.43f + 0.3f);
                const float octaves = s_.evDepth * (0.6f * l1 + 0.4f * l2) * 2.6f + 0.35f * ((float)v.note - 48.0f) / 12.0f;
                const float cutoff = s_.cutoff * std::pow(2.0f, clamp(octaves, -6.0f, 6.0f));
                v.f1.set(cutoff, 0.0f); v.f2.set(cutoff, s_.reso);
                v.nf.set(clamp(hz * 2.0f, 80.0f, 9000.0f), 0.8f);
            }
            v.subPhase += v.subHz / sampleRate_; if (v.subPhase >= 1.0f) v.subPhase -= 1.0f;
            const float sub = WavetableSine::lookup(v.subPhase);
            float saws = (v.saw[0].process() + v.saw[1].process()) * 0.5f;
            if (layerOn) saws += (v.saw[2].process() + v.saw[3].process()) * 0.5f * s_.layerLevel;
            const float noise = v.nf.process(v.rng.nextBipolar(), SVF::Mode::Bandpass) * 1.6f;
            const float mix = sub * s_.sub * 0.9f + saws * s_.saw * 0.6f + noise * s_.noise * 0.5f;
            const float y = v.f2.process(v.f1.process(mix, SVF::Mode::Lowpass), SVF::Mode::Lowpass) * aenv * v.velGain * 0.55f;
            L[i] += y * v.panL;
            if (R) R[i] += y * v.panR;
        }
    }

    Voice voices_[kNumVoices];
    VoiceAllocator<kNumVoices> alloc_;
    MidiControllers controllers_;
    cine::MasterFx fx_;
    float evPhase_ = 0.0f, evInc_ = 0.0f;
    float sampleRate_ = 44100.0f;
};

MPC_PLUGIN_ENTRY(Dronescape)
