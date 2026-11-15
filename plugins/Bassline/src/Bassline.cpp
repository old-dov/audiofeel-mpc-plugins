// Bassline - a monophonic bass synth for the MPC, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// Signal flow (one voice, last-note priority):
//
//   main osc (sine / triangle / saw / square, PolyBLEP)  ---+
//   fat osc(s) (own wave, detuned up - and down in Trio) ---+--> mix
//        Motion: a slow LFO sweeps the detune (phasing) and the cutoff
//   sub osc  (sine, one octave below the main osc)       ---+
//        -> state-variable low-pass (cutoff + filter envelope + key tracking, with drive)
//        -> amp ADSR * velocity -> master volume -> both outputs
//
// Playing style:
//   * Glide slides between notes (linear in pitch). With "Legato" on, notes played
//     while another is still held glide without retriggering the envelopes (303 style);
//     releasing the newest note returns to the previous held note.
//   * "Accent" makes velocity control both the loudness and how hard the filter
//     envelope opens: hit a pad hard for a squelchy accented note.
//   * Pitch bend is +-2 semitones.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"

using namespace mpc::dsp;

class Bassline : public mpc::Plugin
{
public:
    // Parameter order is permanent once users have projects: append new ones at the end.
    // Parameter 0 is nudged by the host on every load, so it is the harmless master volume.
    enum Params
    {
        kVolume = 0,   // dB
        kWave,         // Sine / Triangle / Saw / Square
        kPulseWidth,   // % (square only)
        kSub,          // % level of the sub oscillator
        kFat,          // % level of the detuned second oscillator
        kDetune,       // cents of the second oscillator
        kOctave,       // octave shift of the whole voice
        kGlide,        // ms
        kLegato,       // bool
        kCutoff,       // Hz
        kReso,         // %
        kEnvAmount,    // % (signed): how far the filter envelope moves the cutoff
        kFilterDecay,  // ms
        kDrive,        // %
        kKeyTrack,     // %
        kAccent,       // %
        kAttack,       // ms
        kDecay,        // ms
        kSustain,      // %
        kRelease,      // ms
        kFatWave,      // Same / Sine / Triangle / Saw / Square: wave of the Fat oscillators
        kFatMode,      // Duo (one detuned osc) / Trio (one up, one down)
        kMotion,       // % depth of the slow LFO on detune and cutoff
        kMotionRate,   // Hz
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Bassline";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', 'P', 'w', '6');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Bassline() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("volume",  "Volume",     -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("wave",    "Wave",       {"Sine", "Triangle", "Saw", "Square"}, 2));
        addParam(floatParam ("pw",      "Pulse Width", 5.0f, 95.0f, 50.0f, "%", 0));
        addParam(floatParam ("sub",     "Sub",         0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("fat",     "Fat",         0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("detune",  "Detune",      0.0f, 50.0f, 12.0f, "ct", 0));
        addParam(intParam   ("octave",  "Octave",     -2, 1, 0));
        addParam(floatParam ("glide",   "Glide",       0.0f, 500.0f, 40.0f, "ms", 0, 0.5f));
        addParam(boolParam  ("legato",  "Legato",      true));
        addParam(logParam   ("cutoff",  "Cutoff",      30.0f, 12000.0f, 500.0f, "Hz", 0));
        addParam(floatParam ("reso",    "Resonance",   0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam ("envamt",  "Env Amount", -100.0f, 100.0f, 60.0f, "%", 0));
        addParam(logParam   ("fdecay",  "Filter Decay", 20.0f, 2000.0f, 250.0f, "ms", 0));
        addParam(floatParam ("drive",   "Drive",       0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam ("keytrk",  "Key Track",   0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam ("accent",  "Accent",      0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam   ("attack",  "Attack",      0.5f, 500.0f, 2.0f, "ms", 1));
        addParam(logParam   ("decay",   "Decay",       10.0f, 3000.0f, 400.0f, "ms", 0));
        addParam(floatParam ("sustain", "Sustain",     0.0f, 100.0f, 80.0f, "%", 0));
        addParam(logParam   ("release", "Release",     5.0f, 2000.0f, 80.0f, "ms", 0));
        addParam(choiceParam("fatwave", "Fat Wave",    {"Same", "Sine", "Triangle", "Saw", "Square"}, 0));
        addParam(choiceParam("fatmode", "Fat Mode",    {"Duo", "Trio"}, 0));
        addParam(floatParam ("motion",  "Motion",      0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam   ("mrate",   "Motion Rate", 0.1f, 12.0f, 1.5f, "Hz", 2));

        if (numParams() != kNumParams) mpc::log("Bassline: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        program("Acid 303",   {{kWave, 2}, {kSub, 0}, {kGlide, 60}, {kCutoff, 380}, {kReso, 70}, {kEnvAmount, 85},
                               {kFilterDecay, 180}, {kDrive, 40}, {kAccent, 80}, {kDecay, 250}, {kSustain, 60}, {kRelease, 50}});
        program("Sub Weight", {{kWave, 0}, {kSub, 80}, {kFat, 0}, {kGlide, 0}, {kCutoff, 400}, {kReso, 5}, {kEnvAmount, 10},
                               {kDrive, 10}, {kAccent, 30}, {kSustain, 100}, {kRelease, 120}});
        // Fat Saw: bright 3-saw stack with a plucky filter envelope, no movement.
        program("Fat Saw",    {{kWave, 2}, {kSub, 35}, {kFat, 85}, {kFatMode, 1}, {kDetune, 20}, {kCutoff, 1800}, {kReso, 20},
                               {kEnvAmount, 45}, {kFilterDecay, 260}, {kDrive, 10}, {kKeyTrack, 60}, {kAccent, 55},
                               {kDecay, 350}, {kSustain, 55}, {kRelease, 90}});
        program("Square Pluck", {{kWave, 3}, {kPulseWidth, 35}, {kSub, 40}, {kGlide, 0}, {kLegato, 0}, {kCutoff, 300},
                               {kReso, 35}, {kEnvAmount, 90}, {kFilterDecay, 110}, {kDrive, 15}, {kAccent, 70},
                               {kDecay, 180}, {kSustain, 0}, {kRelease, 60}});
        // Reese: two dark saws, narrow detune that phases slowly, heavy drive, long glide, no pluck.
        program("Reese",      {{kWave, 2}, {kSub, 40}, {kFat, 100}, {kFatMode, 0}, {kDetune, 18}, {kGlide, 90}, {kCutoff, 480},
                               {kReso, 28}, {kEnvAmount, 8}, {kFilterDecay, 900}, {kDrive, 70}, {kKeyTrack, 20}, {kAccent, 25},
                               {kSustain, 100}, {kRelease, 220}, {kMotion, 85}, {kMotionRate, 0.5f}});
        program("Wobble Soft", {{kWave, 1}, {kSub, 60}, {kGlide, 100}, {kCutoff, 260}, {kReso, 15}, {kEnvAmount, 30},
                               {kDrive, 0}, {kAccent, 20}, {kAttack, 15}, {kSustain, 90}, {kRelease, 250}});
        program("Growl Square", {{kWave, 3}, {kPulseWidth, 40}, {kSub, 55}, {kFat, 80}, {kFatWave, 3}, {kFatMode, 0}, {kDetune, 30},
                               {kGlide, 30}, {kCutoff, 520}, {kReso, 45}, {kEnvAmount, 20}, {kDrive, 55}, {kSustain, 100},
                               {kMotion, 90}, {kMotionRate, 3.2f}});
        // Wave indices: 0 Sine, 1 Triangle, 2 Saw, 3 Square. Fat Wave: 0 Same, 1 Sine, 2 Triangle, 3 Saw, 4 Square.
        program("Init Bass",    {});
        program("Deep Sub",     {{kWave, 0}, {kSub, 70}, {kGlide, 25}, {kCutoff, 300}, {kReso, 0}, {kEnvAmount, 0}, {kDrive, 0},
                                 {kAccent, 20}, {kSustain, 100}, {kRelease, 150}});
        program("Moog Round",   {{kWave, 2}, {kFat, 50}, {kFatWave, 4}, {kDetune, 4}, {kSub, 40}, {kGlide, 30}, {kCutoff, 420}, {kReso, 35},
                                 {kEnvAmount, 55}, {kFilterDecay, 200}, {kDrive, 25}, {kKeyTrack, 40}, {kDecay, 300}, {kSustain, 65}});
        program("Slap Funk",    {{kWave, 3}, {kPulseWidth, 30}, {kSub, 30}, {kGlide, 0}, {kLegato, 0}, {kCutoff, 250}, {kReso, 30},
                                 {kEnvAmount, 100}, {kFilterDecay, 90}, {kDrive, 20}, {kKeyTrack, 100}, {kAccent, 85},
                                 {kDecay, 150}, {kSustain, 25}, {kRelease, 60}});
        program("Dub Sub",      {{kWave, 1}, {kSub, 50}, {kGlide, 120}, {kCutoff, 200}, {kReso, 5}, {kEnvAmount, 0}, {kDrive, 15},
                                 {kSustain, 100}, {kRelease, 300}});
        program("Neuro Motion", {{kWave, 2}, {kFat, 100}, {kFatMode, 1}, {kFatWave, 4}, {kDetune, 28}, {kGlide, 20}, {kCutoff, 600},
                                 {kReso, 45}, {kEnvAmount, 15}, {kDrive, 65}, {kSustain, 100}, {kMotion, 100}, {kMotionRate, 2.2f}});
        program("Techno Rumble", {{kWave, 2}, {kSub, 60}, {kGlide, 0}, {kLegato, 0}, {kCutoff, 220}, {kReso, 50}, {kEnvAmount, 55},
                                 {kFilterDecay, 120}, {kDrive, 50}, {kKeyTrack, 20}, {kDecay, 160}, {kSustain, 50}, {kRelease, 40}});
        program("Pulse Bass",   {{kWave, 3}, {kPulseWidth, 18}, {kSub, 40}, {kCutoff, 700}, {kReso, 25}, {kEnvAmount, 45},
                                 {kFilterDecay, 180}, {kDrive, 10}, {kSustain, 70}});
        program("Rubber Band",  {{kWave, 2}, {kGlide, 150}, {kCutoff, 350}, {kReso, 60}, {kEnvAmount, 70}, {kFilterDecay, 300},
                                 {kDrive, 30}, {kAccent, 60}, {kMotion, 25}, {kMotionRate, 4.0f}});
        program("Wide Stack",   {{kWave, 2}, {kFat, 80}, {kFatMode, 1}, {kDetune, 25}, {kSub, 55}, {kCutoff, 1500}, {kReso, 15},
                                 {kEnvAmount, 35}, {kFilterDecay, 400}, {kDrive, 25}, {kSustain, 85}, {kRelease, 120}});
        program("Sine Pluck",   {{kWave, 0}, {kSub, 0}, {kGlide, 0}, {kLegato, 0}, {kCutoff, 1200}, {kReso, 10}, {kEnvAmount, 40},
                                 {kFilterDecay, 120}, {kDecay, 250}, {kSustain, 0}, {kRelease, 80}});
        program("Gritty Square", {{kWave, 3}, {kSub, 0}, {kFat, 50}, {kFatWave, 3}, {kDetune, 9}, {kCutoff, 1800}, {kReso, 30},
                                 {kEnvAmount, 30}, {kDrive, 80}, {kSustain, 85}});
        program("Wobble Deep",  {{kWave, 2}, {kSub, 60}, {kGlide, 40}, {kCutoff, 280}, {kReso, 45}, {kEnvAmount, 0}, {kDrive, 35},
                                 {kSustain, 100}, {kMotion, 100}, {kMotionRate, 0.9f}});
        program("Wobble Fast",  {{kWave, 2}, {kSub, 60}, {kGlide, 40}, {kCutoff, 350}, {kReso, 35}, {kEnvAmount, 0}, {kDrive, 35},
                                 {kSustain, 100}, {kMotion, 100}, {kMotionRate, 5.5f}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        main_.setSampleRate(sampleRate);
        fat_.setSampleRate(sampleRate);
        fat2_.setSampleRate(sampleRate);
        sub_.setSampleRate(sampleRate);
        filter_.setSampleRate(sampleRate);
        ampEnv_.setSampleRate(sampleRate);
        filtEnv_.setSampleRate(sampleRate);
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
        sr_ = sampleRate;
    }

    void reset() override
    {
        ampEnv_.reset(); filtEnv_.reset(); filter_.reset();
        lfoPhase_ = 0.0f; held_ = 0; bend_ = 0.0f; ctr_ = 0; velN_ = 0.0f;
        volume_.snap(dbToGain(get(kVolume)));
    }

    // ----- audio ------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings(ctx.sampleRate);

        int pos = 0;
        for (int e = 0; e < ctx.numMidiEvents; ++e)
        {
            const mpc::MidiEvent& ev = ctx.midi[e];
            if (ev.sampleOffset > pos) { render(ctx, pos, ev.sampleOffset); pos = ev.sampleOffset; }
            handleMidi(ev);
        }
        render(ctx, pos, ctx.numFrames);
    }

private:
    static constexpr int kMaxHeld = 16;
    static constexpr int kCtrlInterval = 8;   // samples between pitch / filter coefficient updates

    struct Settings
    {
        int wave = 2, octave = 0;
        float pw = 0.5f, sub = 0.5f, fat = 0.0f, detune = 12.0f, glideMs = 0.0f, motion = 0.0f, motionRate = 1.5f;
        int fatWave = 0, fatMode = 0;
        bool legato = true;
        float cutoff = 500.0f, reso = 0.3f, envAmt = 0.6f, drive = 0.2f, keyTrack = 0.3f, accent = 0.5f;
    } s_;

    void readSettings(double sr)
    {
        s_.wave = getInt(kWave); s_.octave = getInt(kOctave);
        s_.pw = get(kPulseWidth) * 0.01f; s_.sub = get(kSub) * 0.01f; s_.fat = get(kFat) * 0.01f;
        s_.detune = get(kDetune); s_.glideMs = get(kGlide); s_.legato = getBool(kLegato);
        s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f; s_.envAmt = get(kEnvAmount) * 0.01f;
        s_.drive = get(kDrive) * 0.01f; s_.keyTrack = get(kKeyTrack) * 0.01f; s_.accent = get(kAccent) * 0.01f;

        s_.fatWave = getInt(kFatWave); s_.fatMode = getInt(kFatMode);
        s_.motion = get(kMotion) * 0.01f; s_.motionRate = get(kMotionRate);
        const int fw = s_.fatWave == 0 ? s_.wave : s_.fatWave - 1;   // Same, or Sine..Square
        main_.setWave(s_.wave); fat_.setWave(fw); fat2_.setWave(fw);
        main_.setPulseWidth(s_.pw); fat_.setPulseWidth(s_.pw); fat2_.setPulseWidth(s_.pw);
        filter_.setDrive(1.0f + s_.drive * 9.0f);
        ampEnv_.set(get(kAttack), get(kDecay), get(kSustain) * 0.01f, get(kRelease));
        filtEnv_.set(1.0f, get(kFilterDecay), 0.0f, get(kFilterDecay));
        glide_.setTime(s_.glideMs, sr);
        volume_.setTarget(dbToGain(get(kVolume)));
    }

    // ----- MIDI: monophonic with a stack of held notes (last note wins) ------
    void removeHeld(int note)
    {
        int j = 0;
        for (int i = 0; i < held_; ++i) if (heldNote_[i] != note) { heldNote_[j] = heldNote_[i]; heldVel_[j] = heldVel_[i]; ++j; }
        held_ = j;
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn())
        {
            removeHeld(ev.note());
            const bool wasHeld = held_ > 0;
            if (held_ < kMaxHeld) { heldNote_[held_] = ev.note(); heldVel_[held_] = ev.velocity(); ++held_; }
            const bool sounding = ampEnv_.isActive();
            if (s_.glideMs > 0.0f && sounding) glide_.setTarget((float)ev.note());
            else glide_.snap((float)ev.note());
            // Legato: keep the envelopes running when another key is still down.
            if (!(s_.legato && wasHeld && ampEnv_.isHeld()))
            {
                velN_ = ev.velocity() / 127.0f;
                if (!sounding) { main_.resetPhase(); fat_.resetPhase(0.37f); fat2_.resetPhase(0.71f); sub_.resetPhase(); filter_.reset(); }
                ampEnv_.noteOn(); filtEnv_.noteOn();
                ctr_ = 0;
            }
        }
        else if (ev.isNoteOff())
        {
            const bool wasTop = held_ > 0 && heldNote_[held_ - 1] == ev.note();
            removeHeld(ev.note());
            if (held_ == 0) { ampEnv_.noteOff(); filtEnv_.noteOff(); }
            else if (wasTop) glide_.setTarget((float)heldNote_[held_ - 1]);   // back to the previous key
        }
        else if (ev.isAllNotesOff())
        {
            held_ = 0; ampEnv_.noteOff(); filtEnv_.noteOff();
        }
        else if (ev.isPitchBend())
        {
            bend_ = (float)ev.pitchBend() / 8192.0f * 2.0f;   // +-2 semitones
        }
    }

    void render(mpc::ProcessContext& ctx, int from, int to)
    {
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const float octShift = 12.0f * (float)s_.octave;
        const bool trio = s_.fatMode == 1;
        const float lfoInc = s_.motionRate / (float)sr_ * (float)kCtrlInterval;   // LFO advances once per control tick
        // The accent: velocity scales loudness and the filter envelope depth.
        const float velGain = lerp(1.0f, velN_, s_.accent);
        const float envBoost = clamp(1.0f + s_.accent * (velN_ - 0.6f) * 2.0f, 0.2f, 2.0f);

        for (int i = from; i < to; ++i)
        {
            const float note = glide_.process();
            const float aenv = ampEnv_.process();
            const float fenv = filtEnv_.process();

            if ((ctr_++ % kCtrlInterval) == 0)
            {
                lfoPhase_ += lfoInc; if (lfoPhase_ >= 1.0f) lfoPhase_ -= 1.0f;
                const float lfo = fastSin01(lfoPhase_);
                // Motion: the detune breathes between 0 and 2x its setting (slow phasing), the cutoff sweeps +-1 octave.
                const float det = std::fmax(0.0f, s_.detune * (1.0f + s_.motion * lfo));
                const float hz = midiToHz(note + octShift + bend_);
                main_.setFrequency(hz);
                fat_.setFrequency(hz * centsToRatio(det));
                fat2_.setFrequency(hz * centsToRatio(-det * 0.8f));
                sub_.setFrequency(hz * 0.5f);
                const float octaves = s_.envAmt * envBoost * fenv * 5.0f
                                    + s_.keyTrack * (note - 36.0f) / 12.0f
                                    + s_.motion * lfo;
                filter_.set(s_.cutoff * std::pow(2.0f, clamp(octaves, -8.0f, 8.0f)), s_.reso);
            }

            float y = 0.0f;
            if (aenv > 0.0f || ampEnv_.isActive())
            {
                float fatSum = fat_.process();
                if (trio) fatSum += fat2_.process();
                const float mix = main_.process() + fatSum * s_.fat + sub_.process() * s_.sub;
                // Keep the summed level roughly constant whatever the mix.
                const float norm = 1.0f / (1.0f + s_.fat * (trio ? 2.0f : 1.0f) + s_.sub * 0.5f);
                y = filter_.process(mix * norm, SVF::Mode::Lowpass) * aenv * velGain;
            }
            y = softClip(y * 0.8f) * volume_.process();
            L[i] = y;
            if (R) R[i] = y;
        }
    }

    mpc::dsp::PolyBlepOsc main_, fat_, fat2_;
    float lfoPhase_ = 0.0f;
    mpc::dsp::WavetableSine sub_;
    SVF filter_;
    ADSR ampEnv_, filtEnv_;
    Smoother glide_, volume_;

    int heldNote_[kMaxHeld] = {0}, heldVel_[kMaxHeld] = {0}, held_ = 0;
    float bend_ = 0.0f, velN_ = 0.0f;
    int ctr_ = 0;
    double sr_ = 44100.0;
};

MPC_PLUGIN_ENTRY(Bassline)
