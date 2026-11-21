// Brassline - a synthesized brass section and braams for the MPC, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// 6-voice polyphonic. Each voice is three detuned saws, a narrow pulse and (for the low sections) a
// sub saw, pushed through a growl stage (soft saturation) into a low-pass that a filter envelope
// "blats" open: the Swell amount is how far the brass opens as it speaks, and it speaks as slowly as the
// Attack. A pitch Scoop makes the note rise into place, and the vibrato fades in late, like a player's.
// A Section choice sets the colour (Trumpet, Horn, Trombone, Tuba) and Braam is the wide detuned,
// heavily driven low stack of modern trailers.
//
// Playing it like an instrument (v2):
//   Velocity    sets loudness, brightness, growl and how quickly the note speaks.
//   Mod wheel / aftertouch / Dynamics   brass opens up as it gets louder (brighter, more growl, deeper vibrato).
//   Tongue      a burst of brightness and breath as the note starts.
//   Humanize    each note gets its own small pitch, level and attack offsets.
//   Mono        a solo line: last-note priority, slurs between held keys, with Glide.
//
// Signal flow: [saws + pulse + sub] -> growl (tanh) -> LP x2 (filter envelope) -> amp envelope
//              -> voice pan -> volume -> chorus -> reverb.

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

class Brassline : public mpc::Plugin
{
public:
    // Parameter order is permanent once users have projects: append new ones at the end.
    // Parameter 0 is nudged by the host on every load, so it is the harmless master volume.
    enum Params
    {
        kVolume = 0,
        kSection, kAttack, kSwell, kBrightness, kGrowl, kDetune, kScoop, kVibrato, kRelease,
        kChorusMix, kRevMix, kRevSize, kRevDamp,
        kBend,
        kDynamics, kMode, kGlide, kTongue, kHumanize,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Brassline";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', 'c', 'Z', 'E');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Brassline() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("volume",   "Volume",       -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("section",  "Section",      {"Trumpet", "Horn", "Trombone", "Tuba", "Braam"}, 2));
        addParam(logParam   ("attack",   "Attack",       5.0f, 4000.0f, 55.0f, "ms", 0));
        addParam(floatParam ("swell",    "Swell",         0.0f, 100.0f, 60.0f, "%", 0));
        addParam(logParam   ("bright",   "Brightness",   300.0f, 9000.0f, 1500.0f, "Hz", 0));
        addParam(floatParam ("growl",    "Growl",         0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam ("detune",   "Detune",        0.0f, 40.0f, 5.0f, "ct", 1));
        addParam(floatParam ("scoop",    "Scoop",         0.0f, 12.0f, 0.5f, "st", 1));
        addParam(floatParam ("vibrato",  "Vibrato",       0.0f, 100.0f, 25.0f, "%", 0));
        addParam(logParam   ("release",  "Release",       20.0f, 6000.0f, 150.0f, "ms", 0));
        addParam(floatParam ("chorusmix", "Chorus Mix",   0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("revmix",   "Rev Mix",       0.0f, 100.0f, 18.0f, "%", 0));
        addParam(floatParam ("revsize",  "Rev Size",      0.0f, 100.0f, 70.0f, "%", 0));
        addParam(floatParam ("revdamp",  "Rev Damp",      0.0f, 100.0f, 50.0f, "%", 0));
        addParam(intParam   ("bend",     "Bend Range",    0, 12, 2, "st"));
        addParam(floatParam ("dynamics", "Dynamics",      0.0f, 100.0f, 70.0f, "%", 0));
        addParam(choiceParam("mode",     "Mode",          {"Poly", "Mono"}, 0));
        addParam(floatParam ("glide",    "Glide",         0.0f, 500.0f, 40.0f, "ms", 0));
        addParam(floatParam ("tongue",   "Tongue",        0.0f, 100.0f, 40.0f, "%", 0));
        addParam(floatParam ("humanize", "Humanize",      0.0f, 100.0f, 35.0f, "%", 0));
        if (numParams() != kNumParams) mpc::log("Brassline: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        // Section: 0 Trumpet, 1 Horn, 2 Trombone, 3 Tuba, 4 Braam.
        program("Init Brass",       {});
        program("Trumpet Section",  {{kSection, 0}, {kAttack, 45}, {kSwell, 70}, {kBrightness, 2400}, {kGrowl, 25}, {kDetune, 7}, {kScoop, 1.2f}, {kVibrato, 30}, {kRevMix, 28}});
        program("French Horns",     {{kSection, 1}, {kAttack, 160}, {kSwell, 45}, {kBrightness, 1200}, {kGrowl, 15}, {kDetune, 9}, {kScoop, 0.3f}, {kVibrato, 20}, {kRelease, 450}, {kRevMix, 35}});
        program("Trombones",        {{kSection, 2}, {kAttack, 100}, {kSwell, 60}, {kBrightness, 1500}, {kGrowl, 35}, {kScoop, 1.0f}, {kRevMix, 28}});
        program("Tuba Low",         {{kSection, 3}, {kAttack, 120}, {kSwell, 40}, {kBrightness, 700}, {kGrowl, 25}, {kDetune, 6}, {kScoop, 0.2f}, {kVibrato, 10}, {kRelease, 350}});
        program("Braam",            {{kSection, 4}, {kAttack, 380}, {kSwell, 85}, {kBrightness, 1300}, {kGrowl, 70}, {kDetune, 14}, {kScoop, 2.0f}, {kVibrato, 0}, {kRelease, 1200}, {kChorusMix, 25}, {kRevMix, 40}, {kRevSize, 85}});
        program("Braam Wide",      {{kSection, 4}, {kAttack, 600}, {kSwell, 95}, {kBrightness, 1700}, {kGrowl, 85}, {kDetune, 22}, {kScoop, 3.0f}, {kVibrato, 0}, {kRelease, 1800}, {kChorusMix, 40}, {kRevMix, 50}, {kRevSize, 92}});
        program("Fanfare",          {{kSection, 0}, {kAttack, 15}, {kSwell, 80}, {kBrightness, 3200}, {kGrowl, 35}, {kDetune, 6}, {kScoop, 0.6f}, {kVibrato, 15}, {kRelease, 150}, {kRevMix, 30}});
        program("Soft Brass Swell", {{kSection, 1}, {kAttack, 1400}, {kSwell, 80}, {kBrightness, 1400}, {kGrowl, 10}, {kDetune, 10}, {kScoop, 0.0f}, {kVibrato, 30}, {kRelease, 1500}, {kChorusMix, 30}, {kRevMix, 45}});
        program("Brass Stab",       {{kSection, 0}, {kAttack, 8}, {kSwell, 90}, {kBrightness, 2800}, {kGrowl, 40}, {kScoop, 0.8f}, {kVibrato, 0}, {kRelease, 90}, {kRevMix, 20}});
        program("Solo Trumpet",     {{kSection, 0}, {kMode, 1}, {kGlide, 35}, {kAttack, 35}, {kSwell, 75}, {kBrightness, 2600}, {kGrowl, 30}, {kTongue, 55}, {kVibrato, 35}, {kRelease, 120}, {kRevMix, 16}, {kRevSize, 50}});
        program("Solo Trombone",    {{kSection, 2}, {kMode, 1}, {kGlide, 80}, {kAttack, 60}, {kSwell, 65}, {kBrightness, 1700}, {kGrowl, 35}, {kTongue, 45}, {kScoop, 1.5f}, {kVibrato, 25}, {kRevMix, 18}, {kRevSize, 55}});
        program("Dark Horns",       {{kSection, 1}, {kAttack, 260}, {kSwell, 55}, {kBrightness, 800}, {kGrowl, 45}, {kDetune, 12}, {kScoop, 0.5f}, {kRelease, 900}, {kRevMix, 40}, {kRevSize, 85}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        for (auto& v : voices_)
        {
            for (auto& o : v.osc) o.setSampleRate(sampleRate);
            v.osc[0].setWave(Wave::Saw); v.osc[1].setWave(Wave::Saw); v.osc[2].setWave(Wave::Saw);
            v.osc[3].setWave(Wave::Square); v.osc[3].setPulseWidth(0.33f); v.osc[4].setWave(Wave::Saw);
            v.f1.setSampleRate(sampleRate); v.f2.setSampleRate(sampleRate); v.bell.setSampleRate(sampleRate);
            v.ampEnv.setSampleRate(sampleRate); v.filtEnv.setSampleRate(sampleRate);
        }
        controllers_.prepare(sampleRate, 10.0f);
        fx_.prepare(sampleRate, dbToGain(get(kVolume)));
    }

    void reset() override
    {
        for (auto& v : voices_) { v.ampEnv.reset(); v.filtEnv.reset(); v.f1.reset(); v.f2.reset(); v.note = -1; }
        alloc_.reset(); controllers_.reset(); stack_.clear();
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
            handleMidi(ev);
        }
        render(L, R, pos, ctx.numFrames);
        fx_.process(L, R, ctx.numFrames, ctx.sampleRate, dbToGain(get(kVolume)), get(kChorusMix) * 0.01f, 0.4f, 0.5f,
                    get(kRevMix) * 0.01f, get(kRevSize) * 0.01f, get(kRevDamp) * 0.01f);
    }

private:
    static constexpr int kNumVoices = 6;
    static constexpr int kChunk = 64;
    static constexpr int kCtrlInterval = 16;

    struct SectionCfg { float cutoffScale, swellScale, pulse, sub, detuneScale, growlAdd, vibScale, bellHz; };
    static const SectionCfg& section(int i)
    {
        static const SectionCfg t[5] = {
            {1.35f, 1.0f, 0.30f, 0.0f, 1.0f, 0.0f, 1.0f, 1700.0f},      // Trumpet
            {0.75f, 0.8f, 0.45f, 0.0f, 1.0f, 0.0f, 1.0f, 900.0f},       // Horn
            {0.95f, 1.0f, 0.25f, 0.25f, 1.0f, 0.0f, 1.0f, 1100.0f},     // Trombone
            {0.5f, 0.7f, 0.15f, 0.65f, 1.0f, 0.0f, 0.8f, 600.0f},       // Tuba
            {0.85f, 1.3f, 0.10f, 0.55f, 2.2f, 0.25f, 0.0f, 1200.0f}};   // Braam
        return t[i < 0 ? 0 : (i > 4 ? 4 : i)];
    }

    struct Voice
    {
        PolyBlepOsc osc[5];                // 0: saw  1: saw +det  2: saw -det  3: narrow pulse  4: sub saw (octave down)
        SVF f1, f2, bell;
        ADSR ampEnv, filtEnv;
        Xorshift32 rng{0x9E3779B9u};
        int note = -1, ctrl = 0;
        float velGain = 1.0f, velN = 0.8f, panL = 0.7071f, panR = 0.7071f;
        float age = 0.0f, vibPhase = 0.0f;
        float hzCur = 0.0f, atkScale = 1.0f, humCents = 0.0f, humVol = 1.0f, humAtk = 1.0f;
    };

    struct Settings
    {
        int section = 2;
        bool mono = false;
        float swell = 0.6f, bright = 1500.0f, growl = 0.3f, detune = 5.0f, scoop = 0.5f, vibrato = 0.25f;
        float attack = 55.0f, release = 150.0f, dyn = 0.7f, tongue = 0.4f, humanize = 0.35f, glideCoef = 1.0f;
    } s_;

    void readSettings()
    {
        s_.section = getInt(kSection); s_.mono = getInt(kMode) == 1;
        s_.swell = get(kSwell) * 0.01f; s_.bright = get(kBrightness); s_.growl = get(kGrowl) * 0.01f;
        s_.detune = get(kDetune); s_.scoop = get(kScoop); s_.vibrato = get(kVibrato) * 0.01f;
        s_.attack = get(kAttack); s_.release = get(kRelease);
        s_.tongue = get(kTongue) * 0.01f; s_.humanize = get(kHumanize) * 0.01f;
        // Dynamics knob, then the mod wheel and channel aftertouch add to it: brass opens up as it gets louder.
        s_.dyn = clamp(get(kDynamics) * 0.01f + 0.5f * controllers_.modWheel() + 0.3f * controllers_.aftertouch(), 0.0f, 1.0f);
        s_.glideCoef = cine::glideCoef(get(kGlide), kCtrlInterval, sr_);
        alloc_.setBendRange((float)getInt(kBend));
    }

    void setEnv(Voice& v)
    {
        const float a = std::max(s_.attack * v.atkScale * v.humAtk, 3.0f);
        v.ampEnv.set(a, 300.0f, 0.9f, s_.release);
        v.filtEnv.set(a * 0.9f, 350.0f, 0.55f, s_.release);
    }

    void startVoice(Voice& v, int note, int velocity)
    {
        const float vn = (float)velocity / 127.0f;
        v.note = note; v.velN = vn;
        v.velGain = 0.25f + 0.75f * vn;                     // a wide dynamic range
        v.atkScale = 1.5f - 1.0f * vn;                      // hard = quick attack
        const float h = s_.humanize;
        v.humCents = 7.0f * h * v.rng.nextBipolar();
        v.humVol = 1.0f + 0.15f * h * v.rng.nextBipolar();
        v.humAtk = 1.0f + 0.3f * h * v.rng.nextBipolar();
        v.ctrl = 0; v.age = 0.0f;
        if (!v.ampEnv.isActive())
        {
            for (int i = 0; i < 5; ++i) v.osc[i].resetPhase(0.2f * (float)i);
            v.f1.reset(); v.f2.reset(); v.bell.reset(); v.vibPhase = 0.0f;
            v.hzCur = alloc_.noteToHz((float)note);
        }
        setEnv(v);
        v.ampEnv.noteOn(); v.filtEnv.noteOn();
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (s_.mono) { handleMono(ev); return; }
        if (ev.isNoteOn())
        {
            const int vi = alloc_.noteOn(ev.note(), ev.velocity());
            Voice& v = voices_[vi];
            const float pan = std::fmod((float)vi * 0.618034f, 1.0f) * 2.0f - 1.0f;
            equalPowerPan(pan * 0.6f, v.panL, v.panR);
            v.panL *= kSqrt2; v.panR *= kSqrt2;
            startVoice(v, ev.note(), ev.velocity());
        }
        else if (ev.isNoteOff()) { const int vi = alloc_.noteOff(ev.note()); if (vi >= 0) release(vi); }
        else if (ev.isAllNotesOff()) alloc_.allNotesOff([this](int vi) { release(vi); });
        else if (ev.isController() && ev.controller() == cc::Sustain)
        {
            controllers_.handle(ev);
            alloc_.setSustain(ev.controllerValue() >= 64, [this](int vi) { release(vi); });
        }
        else if (ev.isPitchBend()) alloc_.setPitchBend(ev.pitchBend());
        else controllers_.handle(ev);
    }

    // Mono: one voice, last key wins; a second key while one is held slurs to the new note without tonguing it.
    void handleMono(const mpc::MidiEvent& ev)
    {
        Voice& v = voices_[0];
        if (ev.isNoteOn())
        {
            const bool slur = !stack_.empty() && v.ampEnv.isActive();
            stack_.push(ev.note(), ev.velocity());
            v.panL = v.panR = 1.0f;
            if (slur) v.note = ev.note();
            else startVoice(v, ev.note(), ev.velocity());
        }
        else if (ev.isNoteOff())
        {
            stack_.remove(ev.note());
            if (!stack_.empty()) v.note = stack_.top();
            else release(0);
        }
        else if (ev.isAllNotesOff()) { stack_.clear(); release(0); }
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
            for (int vi = 0; vi < kNumVoices; ++vi)
            {
                Voice& v = voices_[vi];
                if (!v.ampEnv.isActive()) continue;
                renderVoice(v, L + from, R ? R + from : nullptr, n);
                if (!v.ampEnv.isActive()) alloc_.voiceFinished(vi);
            }
            from += n;
        }
    }

    void renderVoice(Voice& v, float* L, float* R, int n)
    {
        const SectionCfg& sec = section(s_.section);
        const float det = centsToRatio(s_.detune * sec.detuneScale * (s_.mono ? 0.4f : 1.0f));
        const float dyn = s_.dyn;
        const float ampDyn = (0.3f + 0.7f * dyn) / 0.79f;
        const float cutDyn = std::pow(2.0f, (dyn - 0.7f) * 2.2f);          // brass gets brighter as it gets louder
        const float growl = clamp((s_.growl + sec.growlAdd) * (0.6f + 0.8f * dyn) + 0.15f * v.velN, 0.0f, 1.0f);
        const float drive = 1.0f + growl * 5.0f;
        const float comp = 1.0f / fastTanh(drive * 0.8f);
        const float dt = 1.0f / sr_;
        setEnv(v);
        for (int i = 0; i < n; ++i)
        {
            const float fenv = v.filtEnv.process();
            const float aenv = v.ampEnv.process();
            v.age += dt;

            if ((v.ctrl++ % kCtrlInterval) == 0)
            {
                const float target = alloc_.noteToHz((float)v.note);
                v.hzCur += (target - v.hzCur) * (s_.mono ? s_.glideCoef : 1.0f);
                const float scoopRatio = std::exp2(-s_.scoop * (1.0f - std::min(v.age / 0.14f, 1.0f)) / 12.0f);
                const float vibFade = clamp((v.age - 0.35f) * 1.5f, 0.0f, 1.0f);
                v.vibPhase += 5.3f * (float)kCtrlInterval * dt; if (v.vibPhase >= 1.0f) v.vibPhase -= 1.0f;
                const float vib = centsToRatio(s_.vibrato * sec.vibScale * (0.5f + 0.8f * dyn) * vibFade * 18.0f * fastSin01(v.vibPhase));
                const float hz = v.hzCur * scoopRatio * vib * centsToRatio(v.humCents);
                v.osc[0].setFrequency(hz); v.osc[1].setFrequency(hz * det); v.osc[2].setFrequency(hz / det);
                v.osc[3].setFrequency(hz); v.osc[4].setFrequency(hz * 0.5f);
                // The tongue: a brief burst of extra brightness as the note speaks.
                const float tongue = s_.tongue * 1.6f * std::exp(-v.age / 0.045f);
                const float velBright = 0.55f + 0.9f * v.velN;
                const float octaves = s_.swell * sec.swellScale * fenv * 3.2f + tongue + 0.45f * ((float)v.note - 60.0f) / 12.0f;
                const float cutoff = clamp(s_.bright * sec.cutoffScale * velBright * cutDyn * std::pow(2.0f, octaves) * 0.5f, 120.0f, 15000.0f);
                v.f1.set(cutoff, 0.0f); v.f2.set(cutoff, 0.15f);
                v.bell.set(sec.bellHz, 0.6f);
            }
            float mix = (v.osc[0].process() + v.osc[1].process() + v.osc[2].process()) * 0.33f
                      + v.osc[3].process() * sec.pulse + v.osc[4].process() * sec.sub;
            mix += v.rng.nextBipolar() * s_.tongue * 0.35f * std::exp(-v.age / 0.03f);    // the breath of the attack
            mix = fastTanh(mix * drive * 0.8f) * comp;
            const float filtered = v.f2.process(v.f1.process(mix, SVF::Mode::Lowpass), SVF::Mode::Lowpass);
            const float bell = v.bell.process(mix, SVF::Mode::Bandpass) * 0.22f;           // the bell's resonance
            const float y = (filtered + bell) * aenv * v.velGain * v.humVol * ampDyn * 0.45f;
            L[i] += y * v.panL;
            if (R) R[i] += y * v.panR;
        }
    }

    Voice voices_[kNumVoices];
    VoiceAllocator<kNumVoices> alloc_;
    MidiControllers controllers_;
    cine::NoteStack stack_;
    cine::MasterFx fx_;
    float sr_ = 44100.0f;
};

MPC_PLUGIN_ENTRY(Brassline)
