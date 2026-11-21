// Chimes - bells and tuned percussion for the MPC, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// 8-voice polyphonic, FM-synthesized (no samples). Each voice is a two-operator FM pair (a sine
// carrier phase-modulated by a sine at a fixed ratio, with an index that dies away faster than the
// note, so the attack is bright and the ring is pure) plus one extra inharmonic partial for the
// metal or wood of the bar. A choice of eight instruments sets the ratios, the index and the decay:
// Bell, Music Box, Celesta, Glockenspiel, Marimba, Tubular Bell, Vibraphone, Kalimba.
//
//   Decay      scales how long the note rings.      Brightness scales the FM index (and so the metal).
//   Hardness   how much velocity opens the index.   Tremolo    is the vibraphone motor (amplitude).
//   Release    how fast lifting the key damps the note (long = a bell left to ring).
//
// Signal flow: FM pair + inharmonic partial -> amp (-> tremolo) -> voice pan by key -> volume
//              -> chorus -> reverb.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/voice.h"
#include "dsp/midi_cc.h"
#include "cine_fx.h"

#include <string>
#include <vector>

using namespace mpc::dsp;

class Chimes : public mpc::Plugin
{
public:
    // Parameter order is permanent once users have projects: append new ones at the end.
    // Parameter 0 is nudged by the host on every load, so it is the harmless master volume.
    enum Params
    {
        kVolume = 0,
        kInstrument, kDecay, kBrightness, kHardness, kTremolo, kTremRate, kSpread, kRelease,
        kChorusMix, kRevMix, kRevSize, kRevDamp,
        kBend,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Chimes";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', 'N', 'l', 'X');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Chimes() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("volume",   "Volume",       -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("instrument", "Instrument", {"Bell", "Music Box", "Celesta", "Glockenspiel", "Marimba", "Tubular Bell", "Vibraphone", "Kalimba"}, 0));
        addParam(floatParam ("decay",    "Decay",         25.0f, 250.0f, 100.0f, "%", 0));
        addParam(floatParam ("bright",   "Brightness",    0.0f, 100.0f, 55.0f, "%", 0));
        addParam(floatParam ("hardness", "Hardness",      0.0f, 100.0f, 60.0f, "%", 0));
        addParam(floatParam ("tremolo",  "Tremolo",       0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam   ("tremrate", "Trem Rate",     0.5f, 10.0f, 4.5f, "Hz", 1));
        addParam(floatParam ("spread",   "Spread",        0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam   ("release",  "Release",       30.0f, 8000.0f, 700.0f, "ms", 0));
        addParam(floatParam ("chorusmix", "Chorus Mix",   0.0f, 100.0f, 10.0f, "%", 0));
        addParam(floatParam ("revmix",   "Rev Mix",       0.0f, 100.0f, 35.0f, "%", 0));
        addParam(floatParam ("revsize",  "Rev Size",      0.0f, 100.0f, 75.0f, "%", 0));
        addParam(floatParam ("revdamp",  "Rev Damp",      0.0f, 100.0f, 40.0f, "%", 0));
        addParam(intParam   ("bend",     "Bend Range",    0, 12, 2, "st"));
        if (numParams() != kNumParams) mpc::log("Chimes: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        // Instrument: 0 Bell, 1 Music Box, 2 Celesta, 3 Glockenspiel, 4 Marimba, 5 Tubular Bell, 6 Vibraphone, 7 Kalimba.
        program("Init Bell",        {});
        program("Church Bell",      {{kInstrument, 5}, {kDecay, 130}, {kBrightness, 60}, {kRelease, 4500}, {kRevMix, 50}, {kRevSize, 90}});
        program("Music Box",        {{kInstrument, 1}, {kRelease, 500}, {kRevMix, 30}});
        program("Dream Celesta",    {{kInstrument, 2}, {kDecay, 120}, {kRelease, 1500}, {kChorusMix, 25}, {kRevMix, 50}, {kRevSize, 85}});
        program("Glockenspiel",     {{kInstrument, 3}, {kBrightness, 60}, {kRelease, 1200}, {kRevMix, 35}});
        program("Warm Marimba",     {{kInstrument, 4}, {kBrightness, 45}, {kRelease, 350}, {kRevMix, 25}, {kRevSize, 60}});
        program("Vibraphone",       {{kInstrument, 6}, {kTremolo, 55}, {kTremRate, 4.8f}, {kRelease, 2500}, {kChorusMix, 15}, {kRevMix, 40}});
        program("Kalimba",          {{kInstrument, 7}, {kBrightness, 50}, {kRelease, 400}, {kRevMix, 30}});
        program("Frozen Bells",     {{kInstrument, 0}, {kDecay, 170}, {kBrightness, 70}, {kRelease, 5000}, {kChorusMix, 30}, {kRevMix, 65}, {kRevSize, 96}, {kRevDamp, 30}});
        program("Wonder Tower",     {{kInstrument, 3}, {kDecay, 150}, {kBrightness, 65}, {kRelease, 3500}, {kSpread, 80}, {kChorusMix, 25}, {kRevMix, 60}, {kRevSize, 92}});
        program("Soft Mallets",     {{kInstrument, 4}, {kDecay, 140}, {kBrightness, 35}, {kHardness, 40}, {kRelease, 900}, {kRevMix, 40}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        controllers_.prepare(sampleRate, 10.0f);
        fx_.prepare(sampleRate, dbToGain(get(kVolume)));
    }

    void reset() override
    {
        for (auto& v : voices_) { v.active = false; v.note = -1; }
        alloc_.reset(); controllers_.reset();
        tremPhase_ = 0.0f;
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
        fx_.process(L, R, ctx.numFrames, ctx.sampleRate, dbToGain(get(kVolume)), get(kChorusMix) * 0.01f, 0.4f, 0.4f,
                    get(kRevMix) * 0.01f, get(kRevSize) * 0.01f, get(kRevDamp) * 0.01f);
    }

private:
    static constexpr int kNumVoices = 8;
    static constexpr int kChunk = 64;

    // One instrument: FM ratio and index, how long the amplitude and the index last, the extra partial, the octave shift.
    struct Instr { float ratio, index, ampDecay, idxDecay, ratio2, level2, octave; };
    static const Instr& instr(int i)
    {
        static const Instr t[8] = {
            {3.50f, 4.0f, 5.0f, 1.40f, 5.40f, 0.25f, 0.0f},    // Bell
            {5.00f, 2.0f, 1.4f, 0.35f, 7.10f, 0.20f, 1.0f},    // Music Box
            {4.00f, 2.2f, 1.8f, 0.50f, 6.00f, 0.15f, 0.0f},    // Celesta
            {3.50f, 3.0f, 3.0f, 0.80f, 6.70f, 0.30f, 1.0f},    // Glockenspiel
            {4.00f, 1.2f, 0.9f, 0.12f, 10.0f, 0.15f, 0.0f},    // Marimba
            {2.76f, 3.2f, 6.0f, 1.80f, 5.40f, 0.30f, 0.0f},    // Tubular Bell
            {4.00f, 1.4f, 4.0f, 0.90f, 10.0f, 0.08f, 0.0f},    // Vibraphone
            {3.00f, 1.5f, 1.1f, 0.20f, 5.00f, 0.10f, 0.0f}};   // Kalimba
        return t[i < 0 ? 0 : (i > 7 ? 7 : i)];
    }

    struct Voice
    {
        bool active = false, releasing = false;
        int note = -1;
        float pc = 0.0f, pm = 0.0f, p2 = 0.0f;          // carrier, modulator, extra partial phases
        float amp = 0.0f, idx = 0.0f;                    // amplitude and index envelopes
        float cAmp = 0.99f, cIdx = 0.99f, cRel = 0.99f;
        float octaveRatio = 1.0f, hz = 440.0f, ratio = 3.5f, index = 3.0f, ratio2 = 5.4f, level2 = 0.2f;
        float velGain = 1.0f, panL = 0.7071f, panR = 0.7071f;
    };

    struct Settings
    {
        int instrument = 0;
        float decay = 1.0f, bright = 0.55f, hardness = 0.6f, tremolo = 0.0f;
        float spread = 0.5f, release = 700.0f;
    } s_;

    void readSettings(double sr)
    {
        s_.instrument = getInt(kInstrument);
        s_.decay = get(kDecay) * 0.01f; s_.bright = get(kBrightness) * 0.01f; s_.hardness = get(kHardness) * 0.01f;
        s_.tremolo = get(kTremolo) * 0.01f; s_.spread = get(kSpread) * 0.01f; s_.release = get(kRelease);
        tremInc_ = get(kTremRate) / (float)sr;
        alloc_.setBendRange((float)getInt(kBend));
    }

    float coefFor(float seconds) const { return seconds <= 0.0f ? 0.0f : std::exp(-1.0f / (seconds * sr_)); }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn())
        {
            const int vi = alloc_.noteOn(ev.note(), ev.velocity());
            Voice& v = voices_[vi];
            const Instr& in = instr(s_.instrument);
            v.active = true; v.releasing = false; v.note = ev.note();
            v.pc = v.pm = v.p2 = 0.0f;
            v.amp = 1.0f; v.idx = 1.0f;
            v.cAmp = coefFor(in.ampDecay * s_.decay);
            v.cIdx = coefFor(in.idxDecay * s_.decay);
            v.cRel = coefFor(s_.release * 0.001f);
            v.ratio = in.ratio; v.index = in.index; v.ratio2 = in.ratio2; v.level2 = in.level2;
            const float vel = (float)ev.velocity() / 127.0f;
            v.velGain = lerp(1.0f, vel, 0.6f);
            // Hardness: how much velocity opens the FM index (a harder strike is brighter).
            v.index = in.index * (0.25f + 1.5f * s_.bright) * lerp(1.0f, 0.35f + 0.9f * vel, s_.hardness);
            v.octaveRatio = std::exp2(in.octave);
            const float pan = clamp(((float)ev.note() - 60.0f) / 30.0f, -1.0f, 1.0f) * s_.spread;
            equalPowerPan(pan, v.panL, v.panR);
            v.panL *= kSqrt2; v.panR *= kSqrt2;
        }
        else if (ev.isNoteOff()) { const int vi = alloc_.noteOff(ev.note()); if (vi >= 0) voices_[vi].releasing = true; }
        else if (ev.isAllNotesOff()) alloc_.allNotesOff([this](int vi) { voices_[vi].releasing = true; });
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            controllers_.handle(ev);
            alloc_.setSustain(ev.controllerValue() >= 64, [this](int vi) { voices_[vi].releasing = true; });
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
            for (int vi = 0; vi < kNumVoices; ++vi)
            {
                Voice& v = voices_[vi];
                if (!v.active) continue;
                renderVoice(v, L + from, R ? R + from : nullptr, n);
                if (!v.active) alloc_.voiceFinished(vi);
            }
            tremPhase_ += tremInc_ * (float)n; if (tremPhase_ >= 1.0f) tremPhase_ -= 1.0f;
            from += n;
        }
    }

    void renderVoice(Voice& v, float* L, float* R, int n)
    {
        const float hz = alloc_.noteToHz((float)v.note) * v.octaveRatio;
        const float incC = hz / sr_, incM = hz * v.ratio / sr_, inc2 = hz * v.ratio2 / sr_;
        const float tremGain = 1.0f - s_.tremolo * 0.5f * (1.0f - fastSin01(tremPhase_ + 0.0f * (float)v.note));
        for (int i = 0; i < n; ++i)
        {
            v.pm += incM; v.pm -= (float)(int)v.pm;
            v.pc += incC; v.pc -= (float)(int)v.pc;
            v.p2 += inc2; v.p2 -= (float)(int)v.p2;
            const float mod = WavetableSine::lookup(v.pm) * v.index * v.idx * (1.0f / kTwoPi);
            const float car = WavetableSine::lookup(v.pc + mod);
            const float partial = WavetableSine::lookup(v.p2) * v.level2 * v.idx;
            const float y = (car + partial) * v.amp * v.velGain * tremGain * kTrim;
            L[i] += y * v.panL;
            if (R) R[i] += y * v.panR;
            v.amp *= v.releasing ? v.cRel : v.cAmp;
            v.idx *= v.cIdx;
            if (v.amp < 1.0e-4f) { v.active = false; break; }
        }
    }

    static constexpr float kTrim = 0.38f;
    Voice voices_[kNumVoices];
    VoiceAllocator<kNumVoices> alloc_;
    MidiControllers controllers_;
    cine::MasterFx fx_;
    float tremPhase_ = 0.0f, tremInc_ = 0.0f;
    float sr_ = 44100.0f;
};

MPC_PLUGIN_ENTRY(Chimes)
