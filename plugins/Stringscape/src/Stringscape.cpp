// Stringscape - a synthesized string orchestra for the MPC, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// 8-voice polyphonic. Each voice is an ensemble of detuned saws (plus optional octave-down and
// octave-up layers) through a bowed-string style low-pass that opens as the note blooms, a soft
// body resonance, a fading-in vibrato and a touch of bow noise. A Section choice sets the colour
// (violins to basses) and an Articulation choice sets the playing style: Legato, Staccato,
// Pizzicato or Tremolo. Chorus and reverb at the output.
//
// Playing it like an instrument (v2):
//   Velocity    sets loudness, brightness and how fast the bow bites (hard = quick, bright attack).
//   Mod wheel / aftertouch / Dynamics   crescendo: louder, brighter, deeper vibrato.
//   Bow         every note starts with a scrape of noise and a slightly flat pitch that settles.
//   Humanize    each note gets its own small pitch, level and attack offsets.
//   Mono        a solo line: last-note priority, legato between held keys, with Glide.
//
// Signal flow: [saws + low/high layers] -> LP x2 (follows the amp swell) + body band + bow noise
//              -> amp envelope (-> tremolo) -> voice pan -> volume -> chorus -> reverb.

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

class Stringscape : public mpc::Plugin
{
public:
    // Parameter order is permanent once users have projects: append new ones at the end.
    // Parameter 0 is nudged by the host on every load, so it is the harmless master volume.
    enum Params
    {
        kVolume = 0,
        kSection, kArticulation, kBrightness, kEnsemble, kVibrato, kBow, kLowLayer, kHighLayer,
        kAttack, kRelease,
        kChorusMix, kRevMix, kRevSize, kRevDamp,
        kBend,
        kDynamics, kMode, kGlide, kHumanize,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Stringscape";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', 'C', 'z', 'R');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Stringscape() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("volume",   "Volume",       -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("section",  "Section",      {"Violins", "Violas", "Cellos", "Basses", "Full"}, 4));
        addParam(choiceParam("artic",    "Articulation", {"Legato", "Staccato", "Pizzicato", "Tremolo"}, 0));
        addParam(logParam   ("bright",   "Brightness",   400.0f, 12000.0f, 3200.0f, "Hz", 0));
        addParam(floatParam ("ensemble", "Ensemble",      0.0f, 40.0f, 10.0f, "ct", 1));
        addParam(floatParam ("vibrato",  "Vibrato",       0.0f, 100.0f, 35.0f, "%", 0));
        addParam(floatParam ("bow",      "Bow Noise",     0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam ("lowlayer", "Low Layer",     0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("highlayer", "High Layer",   0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam   ("attack",   "Attack",        5.0f, 6000.0f, 150.0f, "ms", 0));
        addParam(logParam   ("release",  "Release",       30.0f, 8000.0f, 450.0f, "ms", 0));
        addParam(floatParam ("chorusmix", "Chorus Mix",   0.0f, 100.0f, 12.0f, "%", 0));
        addParam(floatParam ("revmix",   "Rev Mix",       0.0f, 100.0f, 22.0f, "%", 0));
        addParam(floatParam ("revsize",  "Rev Size",      0.0f, 100.0f, 75.0f, "%", 0));
        addParam(floatParam ("revdamp",  "Rev Damp",      0.0f, 100.0f, 45.0f, "%", 0));
        addParam(intParam   ("bend",     "Bend Range",    0, 12, 2, "st"));
        addParam(floatParam ("dynamics", "Dynamics",      0.0f, 100.0f, 70.0f, "%", 0));
        addParam(choiceParam("mode",     "Mode",          {"Poly", "Mono"}, 0));
        addParam(floatParam ("glide",    "Glide",         0.0f, 500.0f, 60.0f, "ms", 0));
        addParam(floatParam ("humanize", "Humanize",      0.0f, 100.0f, 35.0f, "%", 0));
        if (numParams() != kNumParams) mpc::log("Stringscape: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        // Section: 0 Violins, 1 Violas, 2 Cellos, 3 Basses, 4 Full.  Articulation: 0 Legato, 1 Staccato, 2 Pizzicato, 3 Tremolo.
        program("Full Ensemble",    {});
        program("Violins Legato",   {{kSection, 0}, {kBrightness, 4200}, {kEnsemble, 16}, {kVibrato, 40}, {kAttack, 380}, {kHighLayer, 20}});
        program("Violas Warm",      {{kSection, 1}, {kBrightness, 2600}, {kEnsemble, 12}, {kVibrato, 35}, {kAttack, 450}});
        program("Cellos Epic",      {{kSection, 2}, {kBrightness, 2400}, {kEnsemble, 12}, {kVibrato, 45}, {kAttack, 550}, {kRelease, 1400}, {kRevMix, 40}});
        program("Basses Low",       {{kSection, 3}, {kBrightness, 1300}, {kEnsemble, 9}, {kVibrato, 20}, {kAttack, 650}, {kRelease, 1200}, {kBow, 35}});
        program("Slow Swell",       {{kSection, 4}, {kBrightness, 2800}, {kAttack, 1800}, {kRelease, 2200}, {kVibrato, 30}, {kRevMix, 40}, {kRevSize, 85}});
        program("Staccato Strings", {{kSection, 4}, {kArticulation, 1}, {kBrightness, 3800}, {kEnsemble, 10}, {kVibrato, 0}, {kBow, 40}, {kRevMix, 22}});
        program("Pizzicato",        {{kSection, 4}, {kArticulation, 2}, {kBrightness, 3200}, {kEnsemble, 5}, {kVibrato, 0}, {kBow, 0}, {kRevMix, 25}});
        program("Tremolo Tension",  {{kSection, 4}, {kArticulation, 3}, {kBrightness, 3400}, {kEnsemble, 18}, {kAttack, 600}, {kRelease, 1500}, {kRevMix, 40}});
        program("Octave Strings",   {{kSection, 4}, {kBrightness, 3600}, {kLowLayer, 55}, {kHighLayer, 45}, {kEnsemble, 15}, {kAttack, 500}, {kRevMix, 35}});
        program("Solo Violin",      {{kSection, 0}, {kMode, 1}, {kGlide, 70}, {kBrightness, 4500}, {kEnsemble, 8}, {kVibrato, 55}, {kBow, 40}, {kAttack, 90}, {kRelease, 260}, {kDynamics, 65}, {kChorusMix, 0}, {kRevMix, 18}, {kRevSize, 55}});
        program("Solo Cello",       {{kSection, 2}, {kMode, 1}, {kGlide, 90}, {kBrightness, 3000}, {kEnsemble, 8}, {kVibrato, 50}, {kBow, 50}, {kAttack, 130}, {kRelease, 320}, {kDynamics, 65}, {kChorusMix, 0}, {kRevMix, 22}, {kRevSize, 60}});
        program("Hall Strings",     {{kSection, 4}, {kBrightness, 3000}, {kEnsemble, 18}, {kChorusMix, 40}, {kAttack, 600}, {kRelease, 1600}, {kRevMix, 55}, {kRevSize, 92}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        for (auto& v : voices_)
        {
            for (auto& o : v.saw) { o.setSampleRate(sampleRate); o.setWave(Wave::Saw); }
            v.f1.setSampleRate(sampleRate); v.f2.setSampleRate(sampleRate); v.body.setSampleRate(sampleRate); v.bowf.setSampleRate(sampleRate);
            v.env.setSampleRate(sampleRate);
        }
        controllers_.prepare(sampleRate, 10.0f);
        fx_.prepare(sampleRate, dbToGain(get(kVolume)));
    }

    void reset() override
    {
        for (auto& v : voices_) { v.env.reset(); v.f1.reset(); v.f2.reset(); v.body.reset(); v.bowf.reset(); v.note = -1; }
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
        fx_.process(L, R, ctx.numFrames, ctx.sampleRate, dbToGain(get(kVolume)), get(kChorusMix) * 0.01f, 0.5f, 0.45f,
                    get(kRevMix) * 0.01f, get(kRevSize) * 0.01f, get(kRevDamp) * 0.01f);
    }

private:
    static constexpr int kNumVoices = 8;
    static constexpr int kChunk = 64;
    static constexpr int kCtrlInterval = 16;

    struct SectionCfg { float cutoffScale, vibRate, bowScale, lowLayer, bodyHz, attackScale; };
    static const SectionCfg& section(int i)
    {
        static const SectionCfg t[5] = {
            {1.0f, 5.9f, 1.0f, 0.0f, 1100.0f, 1.0f},     // Violins
            {0.8f, 5.5f, 1.0f, 0.0f, 850.0f, 1.1f},      // Violas
            {0.6f, 5.0f, 1.1f, 0.35f, 550.0f, 1.25f},    // Cellos
            {0.42f, 4.6f, 1.2f, 0.7f, 330.0f, 1.5f},     // Basses
            {0.9f, 5.7f, 1.0f, 0.3f, 800.0f, 1.1f}};     // Full
        return t[i < 0 ? 0 : (i > 4 ? 4 : i)];
    }

    struct Voice
    {
        PolyBlepOsc saw[4];                // 0,1: ensemble pair   2: octave down   3: octave up
        SVF f1, f2, body, bowf;
        ADSR env;
        Xorshift32 rng{0x9E3779B9u};
        int note = -1, ctrl = 0;
        float velGain = 1.0f, velN = 0.8f, panL = 0.7071f, panR = 0.7071f;
        float age = 0.0f, vibPhase = 0.0f, trem = 0.0f, bowEnv = 0.0f;
        float hzCur = 0.0f, atkScale = 1.0f, humCents = 0.0f, humVol = 1.0f, humAtk = 1.0f, humVib = 1.0f;
    };

    struct Settings
    {
        int section = 4, artic = 0;
        bool mono = false;
        float bright = 3200.0f, ensemble = 10.0f, vibrato = 0.35f, bow = 0.25f, low = 0.0f, high = 0.0f;
        float attack = 150.0f, release = 450.0f, dyn = 0.7f, humanize = 0.35f, glideCoef = 1.0f;
    } s_;

    void readSettings()
    {
        s_.section = getInt(kSection); s_.artic = getInt(kArticulation); s_.mono = getInt(kMode) == 1;
        s_.bright = get(kBrightness); s_.ensemble = get(kEnsemble); s_.vibrato = get(kVibrato) * 0.01f; s_.bow = get(kBow) * 0.01f;
        s_.low = get(kLowLayer) * 0.01f; s_.high = get(kHighLayer) * 0.01f;
        s_.attack = get(kAttack); s_.release = get(kRelease);
        s_.humanize = get(kHumanize) * 0.01f;
        // Dynamics knob, then the mod wheel and channel aftertouch add to it: a crescendo under the fingers.
        s_.dyn = clamp(get(kDynamics) * 0.01f + 0.5f * controllers_.modWheel() + 0.3f * controllers_.aftertouch(), 0.0f, 1.0f);
        s_.glideCoef = cine::glideCoef(get(kGlide), kCtrlInterval, sr_);
        alloc_.setBendRange((float)getInt(kBend));
    }

    // The envelope depends on the articulation and, per voice, on velocity (hard = quick attack) and humanize.
    void setEnv(Voice& v)
    {
        const float sec = section(s_.section).attackScale;
        const float a = std::max(s_.attack * sec * v.atkScale * v.humAtk, 3.0f);
        switch (s_.artic)
        {
            case 1:  v.env.set(std::max(14.0f * v.atkScale, 4.0f), 150.0f, 0.0f, 130.0f); break;   // staccato
            case 2:  v.env.set(2.0f, 280.0f, 0.0f, 200.0f); break;                                 // pizzicato
            case 3:  v.env.set(std::max(a * 0.5f, 30.0f), 400.0f, 1.0f, s_.release * 0.6f); break; // tremolo
            default: v.env.set(a, 600.0f, 1.0f, s_.release); break;                                // legato
        }
    }

    void startVoice(Voice& v, int note, int velocity)
    {
        const float vn = (float)velocity / 127.0f;
        v.note = note;
        v.velN = vn;
        v.velGain = 0.2f + 0.8f * vn;                       // a wide dynamic range
        v.atkScale = 1.6f - 1.2f * vn;                      // 0.4 (hard) .. 1.6 (soft)
        const float h = s_.humanize;
        v.humCents = 8.0f * h * v.rng.nextBipolar();
        v.humVol = 1.0f + 0.17f * h * v.rng.nextBipolar();
        v.humAtk = 1.0f + 0.3f * h * v.rng.nextBipolar();
        v.humVib = 1.0f + 0.1f * h * v.rng.nextBipolar();
        v.ctrl = 0; v.age = 0.0f; v.bowEnv = 1.0f;
        if (!v.env.isActive())
        {
            for (int i = 0; i < 4; ++i) v.saw[i].resetPhase(0.23f * (float)i);
            v.f1.reset(); v.f2.reset(); v.body.reset(); v.bowf.reset(); v.vibPhase = 0.0f; v.trem = 0.0f;
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
            startVoice(v, ev.note(), ev.velocity());
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

    // Mono: one voice, last key wins; a second key while one is held glides without a new bow.
    void handleMono(const mpc::MidiEvent& ev)
    {
        Voice& v = voices_[0];
        if (ev.isNoteOn())
        {
            const bool legato = !stack_.empty() && v.env.isActive();
            stack_.push(ev.note(), ev.velocity());
            v.panL = v.panR = 1.0f;
            if (legato) v.note = ev.note();
            else startVoice(v, ev.note(), ev.velocity());
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
        const SectionCfg& sec = section(s_.section);
        const float det = centsToRatio(s_.mono ? s_.ensemble * 0.3f : s_.ensemble);   // a solo line is a tighter sound
        const float lowAmt = clamp(s_.low + sec.lowLayer, 0.0f, 1.0f);
        const bool pizz = s_.artic == 2, trem = s_.artic == 3, legato = s_.artic == 0;
        const float dt = 1.0f / sr_;
        const float dyn = s_.dyn;
        const float ampDyn = (0.3f + 0.7f * dyn) / 0.79f;
        const float cutDyn = (0.45f + 1.1f * dyn) / 1.22f;
        const float vibDyn = 0.5f + 0.8f * dyn;
        setEnv(v);
        for (int i = 0; i < n; ++i)
        {
            const float aenv = v.env.process();
            v.age += dt;
            v.bowEnv *= 0.9996f;

            if ((v.ctrl++ % kCtrlInterval) == 0)
            {
                const float target = alloc_.noteToHz((float)v.note);
                v.hzCur += (target - v.hzCur) * (s_.mono ? s_.glideCoef : 1.0f);
                const float vibFade = legato || trem ? std::min(v.age * 1.6f, 1.0f) : 0.0f;
                v.vibPhase += sec.vibRate * v.humVib * (float)kCtrlInterval * dt; if (v.vibPhase >= 1.0f) v.vibPhase -= 1.0f;
                const float vib = centsToRatio(s_.vibrato * vibDyn * vibFade * 22.0f * fastSin01(v.vibPhase));
                // The bow bites: the note starts a little flat and settles, more so when struck hard.
                const float scrape = centsToRatio(-30.0f * v.bowEnv * v.velN + v.humCents);
                const float hz = v.hzCur * vib * scrape;
                v.saw[0].setFrequency(hz * det); v.saw[1].setFrequency(hz / det);
                v.saw[2].setFrequency(hz * 0.5f); v.saw[3].setFrequency(hz * 2.0f);
                // The bow: the low-pass opens as the note blooms; a plucked string darkens as it decays.
                const float bloom = pizz ? 0.35f + 0.65f * aenv : 0.45f + 0.55f * aenv;
                const float velBright = 0.55f + 0.9f * v.velN;
                const float cutoff = clamp(s_.bright * sec.cutoffScale * bloom * velBright * cutDyn * std::pow(2.0f, 0.5f * ((float)v.note - 60.0f) / 12.0f), 120.0f, 15000.0f);
                v.f1.set(cutoff, 0.0f); v.f2.set(cutoff, 0.1f);
                v.body.set(sec.bodyHz, 0.55f);
                v.bowf.set(1800.0f, 0.0f);
                v.trem += 9.5f * (float)kCtrlInterval * dt; if (v.trem >= 1.0f) v.trem -= 1.0f;
            }
            float saws = (v.saw[0].process() + v.saw[1].process()) * 0.5f;
            saws += v.saw[2].process() * lowAmt * 0.6f + v.saw[3].process() * s_.high * 0.5f;
            const float filtered = v.f2.process(v.f1.process(saws, SVF::Mode::Lowpass), SVF::Mode::Lowpass);
            const float body = v.body.process(saws, SVF::Mode::Bandpass) * 0.25f;
            const float noise = v.bowf.process(v.rng.nextBipolar(), SVF::Mode::Highpass) * (0.04f + 0.5f * v.bowEnv) * s_.bow * sec.bowScale * (pizz ? 0.0f : 1.0f);
            float amp = aenv;
            if (trem) amp *= 0.45f + 0.55f * (0.5f + 0.5f * fastSin01(v.trem));
            const float y = (filtered * 0.85f + body + noise) * amp * v.velGain * v.humVol * ampDyn * 0.55f;
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

MPC_PLUGIN_ENTRY(Stringscape)
