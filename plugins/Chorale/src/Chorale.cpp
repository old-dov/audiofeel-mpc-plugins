// Chorale - a synthesized choir for the MPC, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// 6-voice polyphonic formant choir. Each voice is a stack of detuned saws (the "glottal" source) plus
// breath noise, sent through three parallel band-pass filters tuned to the first three formants of a
// vowel. Vowel morphs smoothly A - E - I - O - U; Vowel Motion lets a slow LFO move the vowel on its
// own (a different phase per voice, for the "ahh-ooh" drift of a real choir); Voice sets the singer
// (bass to soprano, by scaling the formants). The detuned stack and a slow vibrato that fades in give
// the unison its choral size. Chorus and a large reverb at the output.
//
// Formant table: 1st-3rd formant centre frequencies of the five vowels, textbook averages.
//
// Singing it like a person (v2):
//   Velocity    sets loudness, brightness and how quickly the note starts; hard singing opens the vowel.
//   Mod wheel / aftertouch / Dynamics   crescendo: louder, brighter, an opener vowel, deeper vibrato.
//   Onset       every note starts with a breath of noise, a slightly flat pitch and a vowel that settles.
//   Humanize    each note gets its own small pitch, vowel, level and attack offsets.
//   Solo        one singer: last-note priority, notes glide into each other on the same breath.
//
// Signal flow: [saws + breath] -> LP -> formant bank (3 band-passes) -> amp envelope -> voice pan
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

class Chorale : public mpc::Plugin
{
public:
    // Parameter order is permanent once users have projects: append new ones at the end.
    // Parameter 0 is nudged by the host on every load, so it is the harmless master volume.
    enum Params
    {
        kVolume = 0,
        kSinger, kVowel, kMotion, kMotionRate, kChoir, kBreath, kBrightness, kVibrato,
        kAttack, kRelease,
        kChorusMix, kRevMix, kRevSize, kRevDamp,
        kBend,
        kDynamics, kMode, kGlide, kOnset, kHumanize,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Chorale";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', 'j', 'T', 'R');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Chorale() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("volume",   "Volume",       -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("singer",   "Singer",       {"Bass", "Tenor", "Alto", "Soprano"}, 1));
        addParam(floatParam ("vowel",    "Vowel",         0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("motion",   "Vowel Motion",  0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam   ("motionrate", "Motion Rate", 0.03f, 3.0f, 0.2f, "Hz", 2));
        addParam(floatParam ("choir",    "Choir Size",    0.0f, 40.0f, 14.0f, "ct", 1));
        addParam(floatParam ("breath",   "Breath",        0.0f, 100.0f, 12.0f, "%", 0));
        addParam(logParam   ("bright",   "Brightness",    1000.0f, 9000.0f, 4500.0f, "Hz", 0));
        addParam(floatParam ("vibrato",  "Vibrato",       0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam   ("attack",   "Attack",        10.0f, 6000.0f, 380.0f, "ms", 0));
        addParam(logParam   ("release",  "Release",       30.0f, 8000.0f, 1300.0f, "ms", 0));
        addParam(floatParam ("chorusmix", "Chorus Mix",   0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam ("revmix",   "Rev Mix",       0.0f, 100.0f, 40.0f, "%", 0));
        addParam(floatParam ("revsize",  "Rev Size",      0.0f, 100.0f, 85.0f, "%", 0));
        addParam(floatParam ("revdamp",  "Rev Damp",      0.0f, 100.0f, 45.0f, "%", 0));
        addParam(intParam   ("bend",     "Bend Range",    0, 12, 2, "st"));
        addParam(floatParam ("dynamics", "Dynamics",      0.0f, 100.0f, 70.0f, "%", 0));
        addParam(choiceParam("mode",     "Mode",          {"Choir", "Solo"}, 0));
        addParam(floatParam ("glide",    "Glide",         0.0f, 500.0f, 80.0f, "ms", 0));
        addParam(floatParam ("onset",    "Onset",         0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("humanize", "Humanize",      0.0f, 100.0f, 35.0f, "%", 0));
        if (numParams() != kNumParams) mpc::log("Chorale: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        // Singer: 0 Bass, 1 Tenor, 2 Alto, 3 Soprano.  Vowel: 0 A, 25 E, 50 I, 75 O, 100 U.
        program("Ahh Choir",        {});
        program("Ooh Choir",        {{kVowel, 75}, {kBrightness, 3200}, {kBreath, 8}});
        program("Cathedral",        {{kSinger, 2}, {kVowel, 10}, {kAttack, 900}, {kRelease, 2500}, {kChorusMix, 35}, {kRevMix, 60}, {kRevSize, 95}});
        program("Epic Ahh",         {{kSinger, 2}, {kVowel, 5}, {kChoir, 20}, {kBrightness, 6500}, {kBreath, 10}, {kVibrato, 35}, {kAttack, 350}, {kRevMix, 45}});
        program("Male Choir",       {{kSinger, 0}, {kVowel, 15}, {kChoir, 12}, {kBrightness, 3800}, {kAttack, 450}, {kVibrato, 25}});
        program("Boys Choir",       {{kSinger, 3}, {kVowel, 20}, {kChoir, 10}, {kBrightness, 7000}, {kBreath, 14}, {kVibrato, 40}, {kAttack, 300}});
        program("Vowel Sweep",      {{kSinger, 1}, {kVowel, 50}, {kMotion, 85}, {kMotionRate, 0.25f}, {kAttack, 700}, {kChoir, 16}});
        program("Slow Ooh-Ahh",     {{kSinger, 2}, {kVowel, 55}, {kMotion, 60}, {kMotionRate, 0.09f}, {kAttack, 1500}, {kRelease, 2200}, {kRevMix, 50}});
        program("Whisper",          {{kSinger, 2}, {kVowel, 30}, {kBreath, 90}, {kChoir, 6}, {kVibrato, 5}, {kBrightness, 3500}, {kAttack, 400}, {kRevMix, 45}});
        program("Eerie Voices",     {{kSinger, 3}, {kVowel, 60}, {kMotion, 70}, {kMotionRate, 0.14f}, {kChoir, 28}, {kBreath, 30}, {kVibrato, 55}, {kAttack, 1200}, {kRelease, 2500}, {kRevMix, 55}});
        program("Solo Soprano",     {{kSinger, 3}, {kMode, 1}, {kVowel, 15}, {kGlide, 90}, {kChoir, 8}, {kBrightness, 6500}, {kVibrato, 50}, {kOnset, 60}, {kAttack, 160}, {kRelease, 500}, {kRevMix, 35}, {kRevSize, 80}});
        program("Solo Tenor",       {{kSinger, 1}, {kMode, 1}, {kVowel, 20}, {kGlide, 80}, {kChoir, 8}, {kBrightness, 4800}, {kVibrato, 45}, {kOnset, 55}, {kAttack, 140}, {kRelease, 450}, {kRevMix, 32}, {kRevSize, 75}});
        program("Requiem",          {{kSinger, 1}, {kVowel, 80}, {kChoir, 18}, {kBrightness, 2800}, {kAttack, 900}, {kRelease, 2200}, {kVibrato, 20}, {kRevMix, 55}, {kRevSize, 94}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        for (auto& v : voices_)
        {
            for (auto& o : v.saw) { o.setSampleRate(sampleRate); o.setWave(Wave::Saw); }
            for (auto& f : v.fm) f.setSampleRate(sampleRate);
            v.lp.setSampleRate(sampleRate);
            v.env.setSampleRate(sampleRate);
        }
        controllers_.prepare(sampleRate, 10.0f);
        fx_.prepare(sampleRate, dbToGain(get(kVolume)));
    }

    void reset() override
    {
        for (auto& v : voices_) { v.env.reset(); for (auto& f : v.fm) f.reset(); v.lp.reset(); v.note = -1; }
        alloc_.reset(); controllers_.reset(); stack_.clear();
        motionPhase_ = 0.0f;
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
        fx_.process(L, R, ctx.numFrames, ctx.sampleRate, dbToGain(get(kVolume)), get(kChorusMix) * 0.01f, 0.5f, 0.4f,
                    get(kRevMix) * 0.01f, get(kRevSize) * 0.01f, get(kRevDamp) * 0.01f);
    }

private:
    static constexpr int kNumVoices = 6;
    static constexpr int kChunk = 64;
    static constexpr int kCtrlInterval = 16;

    // Formant centre frequencies (Hz) of the vowels A, E, I, O, U.
    static const float* vowelFormants(int vowel)
    {
        static const float t[5][3] = {{800.0f, 1150.0f, 2800.0f}, {400.0f, 1700.0f, 2600.0f}, {350.0f, 1900.0f, 2700.0f},
                                      {450.0f, 800.0f, 2830.0f}, {325.0f, 700.0f, 2530.0f}};
        return t[vowel < 0 ? 0 : (vowel > 4 ? 4 : vowel)];
    }

    struct Voice
    {
        PolyBlepOsc saw[3];
        SVF fm[3];                         // the three formant band-passes
        SVF lp;
        ADSR env;
        Xorshift32 rng{0x9E3779B9u};
        int note = -1, ctrl = 0;
        float velGain = 1.0f, velN = 0.8f, panL = 0.7071f, panR = 0.7071f;
        float age = 0.0f, vibPhase = 0.0f, motionOffset = 0.0f;
        float hzCur = 0.0f, atkScale = 1.0f, humCents = 0.0f, humVowel = 0.0f, humVol = 1.0f, humAtk = 1.0f;
    };

    struct Settings
    {
        float singerScale = 1.0f, vowel = 0.0f, motion = 0.0f, choir = 14.0f, breath = 0.12f, bright = 4500.0f, vibrato = 0.3f;
        float attack = 550.0f, release = 1300.0f, dyn = 0.7f, onset = 0.5f, humanize = 0.35f, glideCoef = 1.0f;
        bool mono = false;
    } s_;

    void readSettings(double sr)
    {
        static const float scale[4] = {0.82f, 1.0f, 1.12f, 1.28f};
        s_.singerScale = scale[clamp(getInt(kSinger), 0, 3)];
        s_.vowel = get(kVowel) * 0.04f;                // 0..4 across the five vowels
        s_.motion = get(kMotion) * 0.01f; s_.choir = get(kChoir); s_.breath = get(kBreath) * 0.01f;
        s_.bright = get(kBrightness); s_.vibrato = get(kVibrato) * 0.01f;
        s_.attack = get(kAttack); s_.release = get(kRelease);
        s_.onset = get(kOnset) * 0.01f; s_.humanize = get(kHumanize) * 0.01f; s_.mono = getInt(kMode) == 1;
        // Dynamics knob, then the mod wheel and channel aftertouch add to it: louder singing opens the vowel and brightens it.
        s_.dyn = clamp(get(kDynamics) * 0.01f + 0.5f * controllers_.modWheel() + 0.3f * controllers_.aftertouch(), 0.0f, 1.0f);
        s_.glideCoef = cine::glideCoef(get(kGlide), kCtrlInterval, sr_);
        motionInc_ = get(kMotionRate) / (float)sr;
        alloc_.setBendRange((float)getInt(kBend));
    }

    void setEnv(Voice& v) { v.env.set(std::max(s_.attack * v.atkScale * v.humAtk, 5.0f), 400.0f, 0.95f, s_.release); }

    void startVoice(Voice& v, int note, int velocity, int slot)
    {
        const float vn = (float)velocity / 127.0f;
        v.note = note; v.velN = vn;
        v.velGain = 0.25f + 0.75f * vn;                     // a wide dynamic range
        v.atkScale = 1.5f - 1.0f * vn;                      // sung hard = quick attack
        const float h = s_.humanize;
        v.humCents = 9.0f * h * v.rng.nextBipolar();
        v.humVowel = 0.25f * h * v.rng.nextBipolar();
        v.humVol = 1.0f + 0.15f * h * v.rng.nextBipolar();
        v.humAtk = 1.0f + 0.35f * h * v.rng.nextBipolar();
        v.motionOffset = std::fmod((float)slot * 0.27f + 0.5f * (v.rng.nextFloat()), 1.0f);
        v.ctrl = 0; v.age = 0.0f;
        if (!v.env.isActive())
        {
            for (int i = 0; i < 3; ++i) v.saw[i].resetPhase(0.31f * (float)i);
            for (auto& f : v.fm) f.reset();
            v.lp.reset(); v.vibPhase = 0.0f;
            v.hzCur = alloc_.noteToHz((float)note);
        }
        setEnv(v);
        v.env.noteOn();
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (s_.mono) { handleMono(ev); return; }
        if (ev.isNoteOn())
        {
            const int vi = alloc_.noteOn(ev.note(), ev.velocity());
            Voice& v = voices_[vi];
            const float pan = std::fmod((float)vi * 0.618034f, 1.0f) * 2.0f - 1.0f;
            equalPowerPan(pan * 0.7f, v.panL, v.panR);
            v.panL *= kSqrt2; v.panR *= kSqrt2;
            startVoice(v, ev.note(), ev.velocity(), vi);
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

    // Mono (a solo singer): one voice, last key wins; a second key while one is held glides on the same breath.
    void handleMono(const mpc::MidiEvent& ev)
    {
        Voice& v = voices_[0];
        if (ev.isNoteOn())
        {
            const bool legato = !stack_.empty() && v.env.isActive();
            stack_.push(ev.note(), ev.velocity());
            v.panL = v.panR = 1.0f;
            if (legato) v.note = ev.note();
            else startVoice(v, ev.note(), ev.velocity(), 0);
        }
        else if (ev.isNoteOff())
        {
            stack_.remove(ev.note());
            if (!stack_.empty()) v.note = stack_.top();
            else v.env.noteOff();
        }
        else if (ev.isAllNotesOff()) { stack_.clear(); v.env.noteOff(); }
        else if (ev.isPitchBend()) alloc_.setPitchBend(ev.pitchBend());
        else controllers_.handle(ev);
    }

    void render(float* L, float* R, int from, int to)
    {
        while (from < to)
        {
            const int n = (to - from) < kChunk ? (to - from) : kChunk;
            for (int i = 0; i < n; ++i) controllers_.tick();
            motionPhase_ += motionInc_ * (float)n; if (motionPhase_ >= 1.0f) motionPhase_ -= 1.0f;
            for (int vi = 0; vi < kNumVoices; ++vi)
            {
                Voice& v = voices_[vi];
                if (!v.env.isActive()) continue;
                renderVoice(v, L + from, R ? R + from : nullptr, n);
                if (!v.env.isActive()) alloc_.voiceFinished(vi);
            }
            from += n;
        }
    }

    void renderVoice(Voice& v, float* L, float* R, int n)
    {
        const float det = centsToRatio(s_.mono ? s_.choir * 0.3f : s_.choir);   // a solo singer is a tighter sound
        const float dt = 1.0f / sr_;
        const float dyn = s_.dyn;
        const float ampDyn = (0.3f + 0.7f * dyn) / 0.79f;
        const float brightDyn = (0.5f + 1.0f * dyn) / 1.2f;
        static const float gains[3] = {1.0f, 0.55f, 0.28f};
        setEnv(v);
        for (int i = 0; i < n; ++i)
        {
            const float aenv = v.env.process();
            v.age += dt;
            const float onsetEnv = s_.onset * std::exp(-v.age / 0.07f);          // the attack of a sung note: breath and a vowel that settles

            if ((v.ctrl++ % kCtrlInterval) == 0)
            {
                const float target = alloc_.noteToHz((float)v.note);
                v.hzCur += (target - v.hzCur) * (s_.mono ? s_.glideCoef : 1.0f);
                const float vibFade = clamp((v.age - 0.25f) * 1.4f, 0.0f, 1.0f);
                v.vibPhase += 5.2f * (float)kCtrlInterval * dt; if (v.vibPhase >= 1.0f) v.vibPhase -= 1.0f;
                const float vib = centsToRatio(s_.vibrato * (0.5f + 0.8f * dyn) * vibFade * 20.0f * fastSin01(v.vibPhase));
                const float hz = v.hzCur * vib * centsToRatio(v.humCents - 30.0f * onsetEnv);
                v.saw[0].setFrequency(hz); v.saw[1].setFrequency(hz * det); v.saw[2].setFrequency(hz / det);

                // The vowel: morph between neighbours, moved by the slow motion LFO (a different phase per voice),
                // opened toward "ah" by loudness, and starting from the consonant-like end of the range on the onset.
                const float mod = s_.motion * 1.8f * fastSin01(motionPhase_ + v.motionOffset);
                const float open = -0.45f * (dyn - 0.7f) - 0.35f * (v.velN - 0.7f);
                const float pos = clamp(s_.vowel + mod + v.humVowel + open + 0.7f * onsetEnv, 0.0f, 3.999f);
                const int i0 = (int)pos; const float fr = pos - (float)i0;
                const float* a = vowelFormants(i0); const float* b = vowelFormants(i0 + 1);
                for (int k = 0; k < 3; ++k)
                    v.fm[k].set(clamp(lerp(a[k], b[k], fr) * s_.singerScale, 150.0f, 7000.0f), k == 0 ? 0.8f : 0.86f);
                v.lp.set(s_.bright * brightDyn * (0.6f + 0.8f * v.velN), 0.0f);
            }
            const float breath = s_.breath + 0.9f * onsetEnv;
            const float src = (v.saw[0].process() + v.saw[1].process() + v.saw[2].process()) * 0.33f + v.rng.nextBipolar() * breath * 0.6f;
            const float shaped = v.lp.process(src, SVF::Mode::Lowpass);
            float y = 0.0f;
            for (int k = 0; k < 3; ++k) y += v.fm[k].process(shaped, SVF::Mode::Bandpass) * gains[k];
            y *= aenv * v.velGain * v.humVol * ampDyn * kTrim;
            L[i] += y * v.panL;
            if (R) R[i] += y * v.panR;
        }
    }

    static constexpr float kTrim = 0.45f;
    Voice voices_[kNumVoices];
    VoiceAllocator<kNumVoices> alloc_;
    MidiControllers controllers_;
    cine::NoteStack stack_;
    cine::MasterFx fx_;
    float motionPhase_ = 0.0f, motionInc_ = 0.0f;
    float sr_ = 44100.0f;
};

MPC_PLUGIN_ENTRY(Chorale)
