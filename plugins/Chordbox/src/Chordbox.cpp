// Chordbox - a chord synth for the MPC and VST2 hosts, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// One key plays a whole chord. Two ways to choose the chord:
//   Mode "Manual": every key plays the chord type selected by Chord (Major, Minor, 7, maj9...).
//   Mode "Scale" : the key you press is a note of the scale (Key + Scale); the chord is built from
//                  the scale's own notes on that degree (stacked thirds), so the white keys of a
//                  C major scale give C, Dm, Em, F, G, Am, Bdim. Keys outside the scale fall back
//                  to a Chord-type chord. "Extension" picks triad / 7th / 9th / sus2 / sus4 / add9.
// Then Inversion, Voicing (close / open drop-2 / wide with a low root) and Strum shape the chord.
//
// Signal flow, per voice (16 voices, a chord uses 3-7 of them):
//   two detuned oscillators (Wave) + sub sine -> 24 dB low-pass (two cascaded SVF)
//   cutoff = Cutoff * 2^( Env Amount * filter EG + Key Track ) -> ADSR -> stereo position
//   all voices -> volume -> stereo chorus -> ping-pong delay -> outputs.
// Voices are allocated per chord; the oldest are stolen when the 16 are in use.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"
#include "dsp/delay_line.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace mpc::dsp;

class Chordbox : public mpc::Plugin
{
public:
    // Parameter order is permanent once users have projects: append new ones at the end.
    // Parameter 0 is nudged by the host on every load, so it is the harmless master volume.
    enum Params
    {
        kVolume = 0,
        kMode, kChord, kKey, kScale, kExtension, kInversion, kVoicing, kStrum, kOctave,
        kWave, kDetune, kSub, kWidth,
        kCutoff, kReso, kEnvAmount, kKeyTrack,
        kAttack, kDecay, kSustain, kRelease, kVelocity,
        kChorusMix, kChorusDepth, kChorusRate,
        kDelayTime, kDelayFback, kDelayMix,
        kBend,
        kNumParams
    };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Chordbox";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', 'E', 'v', 'W');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Chordbox() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        std::vector<std::string> chordNames;
        for (int c = 0; c < kNumChords; ++c) chordNames.push_back(kChords[c].name);
        addParam(floatParam ("volume",  "Volume",       -40.0f, 6.0f, -8.0f, "dB", 1));
        addParam(choiceParam("mode",    "Mode",         {"Manual", "Scale"}, 0));
        addParam(choiceParam("chord",   "Chord",        chordNames, 1));
        addParam(choiceParam("key",     "Key",          {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}, 0));
        addParam(choiceParam("scale",   "Scale",        {"Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Harm Minor", "Pent Minor"}, 0));
        addParam(choiceParam("ext",     "Extension",    {"Triad", "7th", "9th", "Sus2", "Sus4", "Add9"}, 0));
        addParam(intParam   ("inv",     "Inversion",    0, 3, 0));
        addParam(choiceParam("voicing", "Voicing",      {"Close", "Open", "Wide"}, 0));
        addParam(floatParam ("strum",   "Strum",        0.0f, 120.0f, 0.0f, "ms", 0));
        addParam(intParam   ("octave",  "Octave",       -2, 2, 0));
        addParam(choiceParam("wave",    "Wave",         {"Sine", "Triangle", "Saw", "Square"}, 2));
        addParam(floatParam ("detune",  "Detune",       0.0f, 40.0f, 9.0f, "ct", 1));
        addParam(floatParam ("sub",     "Sub",          0.0f, 100.0f, 0.0f, "%", 0));
        addParam(floatParam ("width",   "Width",        0.0f, 100.0f, 70.0f, "%", 0));
        addParam(logParam   ("cutoff",  "Cutoff",       80.0f, 16000.0f, 2400.0f, "Hz", 0));
        addParam(floatParam ("reso",    "Resonance",    0.0f, 100.0f, 10.0f, "%", 0));
        addParam(floatParam ("envamt",  "Env Amount",  -100.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam ("keytrk",  "Key Track",    0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam   ("attack",  "Attack",       1.0f, 6000.0f, 8.0f, "ms", 0));
        addParam(logParam   ("decay",   "Decay",        10.0f, 6000.0f, 600.0f, "ms", 0));
        addParam(floatParam ("sustain", "Sustain",      0.0f, 100.0f, 70.0f, "%", 0));
        addParam(logParam   ("release", "Release",      10.0f, 8000.0f, 350.0f, "ms", 0));
        addParam(floatParam ("velsens", "Velocity",     0.0f, 100.0f, 50.0f, "%", 0));
        addParam(floatParam ("chorusmix", "Chorus Mix", 0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam ("chorusdepth", "Chorus Depth", 0.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam   ("chorusrate", "Chorus Rate", 0.05f, 3.0f, 0.5f, "Hz", 2));
        addParam(logParam   ("delaytime", "Delay Time", 40.0f, 900.0f, 375.0f, "ms", 0));
        addParam(floatParam ("delayfb", "Delay Fback",  0.0f, 90.0f, 35.0f, "%", 0));
        addParam(floatParam ("delaymix", "Delay Mix",   0.0f, 100.0f, 0.0f, "%", 0));
        addParam(intParam   ("bend",    "Bend Range",   0, 12, 2, "st"));
        if (numParams() != kNumParams) mpc::log("Chordbox: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        // Chords: 0 Major 1 Minor 2 Dim 3 Aug 4 Sus2 5 Sus4 6 Power 7 6 8 m6 9 7 10 maj7 11 m7 12 mMaj7 13 dim7 14 m7b5
        //         15 7sus4 16 add9 17 madd9 18 9 19 maj9 20 m9 21 m11 22 13 23 7#9
        // Waves: 0 Sine 1 Triangle 2 Saw 3 Square.  Scale mode: kMode=1.
        program("Init Chords",      {});
        program("Soft Piano Minor", {{kChord, 11}, {kWave, 1}, {kDetune, 5}, {kCutoff, 2600}, {kEnvAmount, 40}, {kAttack, 4}, {kDecay, 900}, {kSustain, 30}, {kRelease, 500}, {kChorusMix, 30}});
        program("Warm Saw Stack",   {{kChord, 10}, {kWave, 2}, {kDetune, 12}, {kCutoff, 1800}, {kAttack, 15}, {kSustain, 80}, {kRelease, 600}, {kChorusMix, 40}});
        program("Neo Soul Keys",    {{kChord, 20}, {kVoicing, 1}, {kWave, 1}, {kDetune, 6}, {kCutoff, 2200}, {kEnvAmount, 35}, {kAttack, 5}, {kDecay, 800}, {kSustain, 40}, {kRelease, 500}, {kChorusMix, 35}});
        program("Lofi Rhodes-ish",  {{kChord, 11}, {kVoicing, 1}, {kWave, 0}, {kSub, 20}, {kDetune, 4}, {kCutoff, 1400}, {kEnvAmount, 20}, {kAttack, 3}, {kDecay, 700}, {kSustain, 35}, {kRelease, 450}, {kChorusMix, 55}, {kChorusDepth, 60}, {kChorusRate, 0.35f}});
        program("Deep House Stab",  {{kChord, 11}, {kWave, 2}, {kDetune, 10}, {kCutoff, 900}, {kReso, 20}, {kEnvAmount, 65}, {kAttack, 2}, {kDecay, 220}, {kSustain, 0}, {kRelease, 160}, {kDelayMix, 25}, {kDelayTime, 375}});
        program("Techno Stab",      {{kChord, 1}, {kWave, 3}, {kDetune, 7}, {kCutoff, 700}, {kReso, 30}, {kEnvAmount, 75}, {kAttack, 1}, {kDecay, 140}, {kSustain, 0}, {kRelease, 90}, {kDelayMix, 20}, {kDelayTime, 280}, {kDelayFback, 45}});
        program("Trance Supersaw",  {{kChord, 0}, {kWave, 2}, {kDetune, 28}, {kWidth, 100}, {kCutoff, 4500}, {kEnvAmount, 20}, {kAttack, 5}, {kSustain, 85}, {kRelease, 400}, {kChorusMix, 40}, {kDelayMix, 20}});
        program("Cinematic Strings",{{kChord, 1}, {kVoicing, 2}, {kWave, 2}, {kDetune, 14}, {kWidth, 100}, {kCutoff, 1500}, {kEnvAmount, 30}, {kAttack, 700}, {kDecay, 1200}, {kSustain, 85}, {kRelease, 1500}, {kChorusMix, 55}});
        program("Glass Organ",      {{kChord, 0}, {kVoicing, 1}, {kWave, 0}, {kSub, 30}, {kDetune, 3}, {kCutoff, 6000}, {kEnvAmount, 0}, {kAttack, 4}, {kSustain, 100}, {kRelease, 120}});
        program("Strummed Guitar",  {{kChord, 16}, {kVoicing, 1}, {kStrum, 38}, {kWave, 3}, {kDetune, 4}, {kCutoff, 1500}, {kEnvAmount, 45}, {kAttack, 2}, {kDecay, 700}, {kSustain, 20}, {kRelease, 350}});
        program("Harp Strum",       {{kChord, 19}, {kVoicing, 1}, {kStrum, 70}, {kWave, 1}, {kDetune, 2}, {kCutoff, 4200}, {kEnvAmount, 30}, {kAttack, 2}, {kDecay, 1500}, {kSustain, 0}, {kRelease, 900}, {kDelayMix, 25}});
        program("Pop Piano Triads", {{kMode, 1}, {kWave, 1}, {kDetune, 5}, {kCutoff, 3000}, {kEnvAmount, 35}, {kAttack, 3}, {kDecay, 900}, {kSustain, 35}, {kRelease, 450}, {kChorusMix, 25}});
        program("Scale: Major 7ths",{{kMode, 1}, {kExtension, 1}, {kVoicing, 1}, {kWave, 1}, {kDetune, 6}, {kCutoff, 2400}, {kAttack, 5}, {kDecay, 800}, {kSustain, 45}, {kRelease, 500}, {kChorusMix, 35}});
        program("Scale: Minor Pad", {{kMode, 1}, {kScale, 1}, {kExtension, 1}, {kVoicing, 2}, {kWave, 2}, {kDetune, 14}, {kWidth, 100}, {kCutoff, 1300}, {kEnvAmount, 30}, {kAttack, 600}, {kDecay, 1500}, {kSustain, 85}, {kRelease, 1600}, {kChorusMix, 55}, {kDelayMix, 15}});
        program("Scale: Dorian Funk",{{kMode, 1}, {kScale, 2}, {kExtension, 2}, {kVoicing, 1}, {kWave, 2}, {kDetune, 8}, {kCutoff, 1500}, {kReso, 18}, {kEnvAmount, 55}, {kAttack, 2}, {kDecay, 250}, {kSustain, 10}, {kRelease, 180}});
        program("Scale: Phrygian Dark",{{kMode, 1}, {kScale, 3}, {kExtension, 0}, {kVoicing, 2}, {kWave, 2}, {kDetune, 12}, {kCutoff, 800}, {kReso, 15}, {kEnvAmount, 30}, {kAttack, 400}, {kSustain, 90}, {kRelease, 1500}, {kChorusMix, 45}});
        program("Scale: Lydian Dream",{{kMode, 1}, {kScale, 4}, {kExtension, 2}, {kVoicing, 2}, {kWave, 1}, {kDetune, 9}, {kWidth, 100}, {kCutoff, 4000}, {kAttack, 500}, {kDecay, 1500}, {kSustain, 85}, {kRelease, 1800}, {kChorusMix, 50}, {kDelayMix, 25}});
        program("Scale: Mixo Rock", {{kMode, 1}, {kScale, 5}, {kExtension, 4}, {kWave, 3}, {kDetune, 6}, {kCutoff, 1700}, {kEnvAmount, 40}, {kAttack, 3}, {kDecay, 500}, {kSustain, 50}, {kRelease, 250}});
        program("Scale: Harm Minor",{{kMode, 1}, {kScale, 6}, {kExtension, 1}, {kVoicing, 1}, {kWave, 2}, {kDetune, 10}, {kCutoff, 1600}, {kEnvAmount, 35}, {kAttack, 30}, {kSustain, 75}, {kRelease, 600}, {kChorusMix, 35}});
        program("Scale: Pentatonic",{{kMode, 1}, {kScale, 7}, {kExtension, 3}, {kWave, 1}, {kDetune, 6}, {kCutoff, 3000}, {kAttack, 6}, {kDecay, 900}, {kSustain, 40}, {kRelease, 600}, {kDelayMix, 20}});
        program("Ambient Sus Chords",{{kMode, 1}, {kExtension, 3}, {kVoicing, 2}, {kWave, 0}, {kSub, 25}, {kDetune, 8}, {kWidth, 100}, {kCutoff, 3500}, {kAttack, 1200}, {kDecay, 2000}, {kSustain, 90}, {kRelease, 3000}, {kChorusMix, 50}, {kDelayMix, 35}, {kDelayTime, 520}, {kDelayFback, 55}});
        program("Fifths Power",     {{kChord, 6}, {kWave, 2}, {kDetune, 8}, {kSub, 30}, {kCutoff, 1600}, {kReso, 10}, {kEnvAmount, 30}, {kAttack, 3}, {kSustain, 85}, {kRelease, 200}});
        program("Jazz 9th Comping", {{kChord, 18}, {kVoicing, 1}, {kWave, 1}, {kDetune, 4}, {kCutoff, 2000}, {kEnvAmount, 30}, {kAttack, 4}, {kDecay, 600}, {kSustain, 30}, {kRelease, 350}, {kChorusMix, 20}});
        program("Gospel 13ths",     {{kChord, 22}, {kVoicing, 1}, {kWave, 2}, {kDetune, 7}, {kCutoff, 2200}, {kEnvAmount, 25}, {kAttack, 8}, {kSustain, 75}, {kRelease, 450}, {kChorusMix, 40}});
        program("Sad 7b5",          {{kChord, 14}, {kVoicing, 1}, {kWave, 1}, {kDetune, 6}, {kCutoff, 1500}, {kAttack, 40}, {kSustain, 70}, {kRelease, 900}, {kChorusMix, 40}, {kDelayMix, 18}});
        program("Dub Chord Echo",   {{kChord, 11}, {kWave, 3}, {kDetune, 5}, {kCutoff, 800}, {kReso, 25}, {kEnvAmount, 50}, {kAttack, 1}, {kDecay, 160}, {kSustain, 0}, {kRelease, 120}, {kDelayMix, 55}, {kDelayTime, 450}, {kDelayFback, 62}});
        program("Wide Poly Pluck",  {{kChord, 19}, {kVoicing, 2}, {kWave, 2}, {kDetune, 16}, {kWidth, 100}, {kCutoff, 1100}, {kReso, 15}, {kEnvAmount, 60}, {kAttack, 2}, {kDecay, 500}, {kSustain, 0}, {kRelease, 400}, {kChorusMix, 35}, {kDelayMix, 25}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = sampleRate;
        for (auto& v : voices_)
        {
            v.a1.setSampleRate(sampleRate); v.a2.setSampleRate(sampleRate); v.s.setSampleRate(sampleRate);
            v.f1.setSampleRate(sampleRate); v.f2.setSampleRate(sampleRate);
            v.ampEnv.setSampleRate(sampleRate); v.filtEnv.setSampleRate(sampleRate);
        }
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
        const int maxChorus = (int)(sampleRate * 0.04) + 8;
        chL_.prepare(maxChorus); chR_.prepare(maxChorus);
        dlL_.prepare((int)(sampleRate * 1.0) + 8); dlR_.prepare((int)(sampleRate * 1.0) + 8);
    }

    void reset() override
    {
        for (auto& v : voices_) { v.ampEnv.reset(); v.filtEnv.reset(); v.f1.reset(); v.f2.reset(); v.note = -1; v.pending = false; v.held = false; }
        sustain_ = false; bend_ = 0.0f; age_ = 0;
        chL_.reset(); chR_.reset(); dlL_.reset(); dlR_.reset();
        chPhase_ = 0.0f;
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
    static constexpr int kNumVoices = 16;
    static constexpr int kChunk = 64;
    static constexpr int kCtrlInterval = 16;
    static constexpr int kMaxChord = 7;

    struct ChordDef { const char* name; int n; int semis[6]; };
    static constexpr int kNumChords = 24;
    static constexpr ChordDef kChords[kNumChords] = {
        {"Major",  3, {0, 4, 7}},          {"Minor",  3, {0, 3, 7}},          {"Dim",    3, {0, 3, 6}},
        {"Aug",    3, {0, 4, 8}},          {"Sus2",   3, {0, 2, 7}},          {"Sus4",   3, {0, 5, 7}},
        {"Power",  3, {0, 7, 12}},         {"6",      4, {0, 4, 7, 9}},       {"m6",     4, {0, 3, 7, 9}},
        {"7",      4, {0, 4, 7, 10}},      {"maj7",   4, {0, 4, 7, 11}},      {"m7",     4, {0, 3, 7, 10}},
        {"mMaj7",  4, {0, 3, 7, 11}},      {"dim7",   4, {0, 3, 6, 9}},       {"m7b5",   4, {0, 3, 6, 10}},
        {"7sus4",  4, {0, 5, 7, 10}},      {"add9",   4, {0, 4, 7, 14}},      {"m add9", 4, {0, 3, 7, 14}},
        {"9",      5, {0, 4, 7, 10, 14}},  {"maj9",   5, {0, 4, 7, 11, 14}}, {"m9",     5, {0, 3, 7, 10, 14}},
        {"m11",    6, {0, 3, 7, 10, 14, 17}}, {"13",  6, {0, 4, 7, 10, 14, 21}}, {"7#9",  5, {0, 4, 7, 10, 15}},
    };

    struct Voice
    {
        PolyBlepOsc a1, a2, s;
        SVF f1, f2;
        ADSR ampEnv, filtEnv;
        int note = -1;            // sounding MIDI note
        int key = -1;             // the key that triggered the chord
        bool pending = false;     // waiting for its strum delay
        bool held = false;        // key released while the sustain pedal is down
        int delay = 0;            // strum delay left, in samples
        int ctrl = 0;
        unsigned long long age = 0;
        float velGain = 1.0f, panL = 0.7071f, panR = 0.7071f;
    };

    struct Settings
    {
        int wave = 2;
        float detune = 9.0f, sub = 0.0f, width = 0.7f, cutoff = 2400.0f, reso = 0.1f, envAmt = 0.3f, keyTrack = 0.5f, velSens = 0.5f;
    } s_;

    static float bendRatio(float bend, int range) { return std::pow(2.0f, bend * (float)range / 12.0f); }

    void readSettings(double sr)
    {
        (void)sr;
        s_.wave = getInt(kWave);
        s_.detune = get(kDetune); s_.sub = get(kSub) * 0.01f; s_.width = get(kWidth) * 0.01f;
        s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f; s_.envAmt = get(kEnvAmount) * 0.01f; s_.keyTrack = get(kKeyTrack) * 0.01f;
        s_.velSens = get(kVelocity) * 0.01f;
        volume_.setTarget(dbToGain(get(kVolume)));

        const float aa = get(kAttack), ad = get(kDecay), as = get(kSustain) * 0.01f, ar = get(kRelease);
        for (auto& v : voices_)
        {
            v.a1.setWave(s_.wave); v.a2.setWave(s_.wave); v.s.setWave(0);
            v.ampEnv.set(aa, ad, as, ar);
            v.filtEnv.set(aa * 0.8f, ad, as, ar);
        }
    }

    // ----- chord building ---------------------------------------------------
    // Fills `out` with MIDI notes, lowest first; returns the count (at most kMaxChord).
    int buildChord(int key, int* out) const
    {
        int semis[kMaxChord]; int n = 0;
        bool done = false;
        if (getInt(kMode) == 1)
        {
            static const int scales[8][8] = {{0, 2, 4, 5, 7, 9, 11, 7}, {0, 2, 3, 5, 7, 8, 10, 7}, {0, 2, 3, 5, 7, 9, 10, 7}, {0, 1, 3, 5, 7, 8, 10, 7},
                                             {0, 2, 4, 6, 7, 9, 11, 7}, {0, 2, 4, 5, 7, 9, 10, 7}, {0, 2, 3, 5, 7, 8, 11, 7}, {0, 3, 5, 7, 10, 0, 0, 5}};
            static const int sizes[8] = {7, 7, 7, 7, 7, 7, 7, 5};
            const int sc = clamp(getInt(kScale), 0, 7);
            const int size = sizes[sc];
            const int rel = ((key - getInt(kKey)) % 12 + 12) % 12;
            int degree = -1;
            for (int d = 0; d < size; ++d) if (scales[sc][d] == rel) degree = d;
            if (degree >= 0)
            {
                // scale note at (degree + steps), wrapping octaves, relative to the played key
                auto at = [&](int steps) {
                    const int idx = degree + steps;
                    return scales[sc][idx % size] + 12 * (idx / size) - scales[sc][degree];
                };
                const int ext = getInt(kExtension);
                // Pentatonic has no "thirds" worth stacking the same way; stacking by 2 scale steps still sounds fine.
                switch (ext)
                {
                    case 0: semis[n++] = at(0); semis[n++] = at(2); semis[n++] = at(4); break;
                    case 1: semis[n++] = at(0); semis[n++] = at(2); semis[n++] = at(4); semis[n++] = at(6); break;
                    case 2: semis[n++] = at(0); semis[n++] = at(2); semis[n++] = at(4); semis[n++] = at(6); semis[n++] = at(8); break;
                    case 3: semis[n++] = 0; semis[n++] = 2; semis[n++] = 7; break;
                    case 4: semis[n++] = 0; semis[n++] = 5; semis[n++] = 7; break;
                    default: semis[n++] = at(0); semis[n++] = at(2); semis[n++] = at(4); semis[n++] = at(8); break;
                }
                done = true;
            }
        }
        if (!done)
        {
            const ChordDef& c = kChords[clamp(getInt(kChord), 0, kNumChords - 1)];
            for (int i = 0; i < c.n; ++i) semis[n++] = c.semis[i];
        }

        // Inversions: move the lowest note up an octave, n times.
        const int inv = getInt(kInversion);
        for (int k = 0; k < inv && n > 1; ++k)
        {
            const int lowest = semis[0];
            for (int i = 0; i < n - 1; ++i) semis[i] = semis[i + 1];
            semis[n - 1] = lowest + 12;
        }
        std::sort(semis, semis + n);

        // Voicing: Open = drop 2 (second note from the top goes down an octave); Wide = drop 2 plus a low root.
        const int voicing = getInt(kVoicing);
        if (voicing >= 1 && n >= 3) { semis[n - 2] -= 12; std::sort(semis, semis + n); }
        int count = 0;
        const int base = key + 12 * getInt(kOctave);
        if (voicing == 2 && n < kMaxChord) out[count++] = clamp(base - 12 + semis[0] + 0, 0, 127);
        for (int i = 0; i < n; ++i) out[count++] = clamp(base + semis[i], 0, 127);
        return count;
    }

    void handleMidi(const mpc::MidiEvent& ev)
    {
        if (ev.isNoteOn()) startChord(ev.note(), ev.velocity());
        else if (ev.isNoteOff()) releaseKey(ev.note());
        else if (ev.isAllNotesOff()) { for (int i = 0; i < kNumVoices; ++i) releaseVoice(voices_[i]); sustain_ = false; }
        else if (ev.isController() && ev.controller() == 64)
        {
            const bool down = ev.controllerValue() >= 64;
            if (sustain_ && !down) for (auto& v : voices_) if (v.held) { v.held = false; releaseVoice(v); }
            sustain_ = down;
        }
        else if (ev.isPitchBend()) bend_ = (float)ev.pitchBend() / 8192.0f;
    }

    void releaseVoice(Voice& v)
    {
        if (v.pending) { v.pending = false; v.note = -1; return; }
        v.ampEnv.noteOff(); v.filtEnv.noteOff();
    }

    void releaseKey(int key)
    {
        for (auto& v : voices_)
        {
            if (v.key != key || v.note < 0) continue;
            if (sustain_) v.held = true; else releaseVoice(v);
        }
    }

    int takeVoice()
    {
        int best = 0; unsigned long long oldest = ~0ull;
        for (int i = 0; i < kNumVoices; ++i)
        {
            const Voice& v = voices_[i];
            if (v.note < 0 && !v.ampEnv.isActive()) return i;
            const unsigned long long a = v.ampEnv.isActive() ? v.age : 0ull;
            if (a < oldest) { oldest = a; best = i; }
        }
        return best;
    }

    void startChord(int key, int velocity)
    {
        // Retriggering the same key: release its previous chord first.
        for (auto& v : voices_) if (v.key == key && v.note >= 0) { v.held = false; releaseVoice(v); v.key = -1; }

        int notes[kMaxChord];
        const int n = buildChord(key, notes);
        const float strumSamples = get(kStrum) * 0.001f * (float)sr_;
        ++age_;
        for (int i = 0; i < n; ++i)
        {
            const int vi = takeVoice();
            Voice& v = voices_[vi];
            v.note = notes[i]; v.key = key; v.held = false; v.age = age_ * 16 + (unsigned long long)i;
            v.velGain = lerp(1.0f, velocity / 127.0f, s_.velSens);
            const float spread = n > 1 ? (float)i / (float)(n - 1) * 2.0f - 1.0f : 0.0f;   // low notes left, high notes right
            equalPowerPan(spread * s_.width * 0.8f, v.panL, v.panR);
            v.panL *= kSqrt2; v.panR *= kSqrt2;
            v.delay = (int)(strumSamples * (float)i);
            v.pending = true;
            v.ctrl = 0;
        }
    }

    void startVoice(Voice& v)
    {
        v.pending = false;
        if (!v.ampEnv.isActive())
        {
            v.a1.resetPhase(0.00f); v.a2.resetPhase(0.41f); v.s.resetPhase(0.0f);
            v.f1.reset(); v.f2.reset();
        }
        v.ampEnv.noteOn(); v.filtEnv.noteOn();
    }

    void render(float* L, float* R, int from, int to)
    {
        while (from < to)
        {
            const int n = (to - from) < kChunk ? (to - from) : kChunk;
            for (int vi = 0; vi < kNumVoices; ++vi)
            {
                Voice& v = voices_[vi];
                if (v.note < 0) continue;
                if (!v.pending && !v.ampEnv.isActive()) { v.note = -1; v.key = -1; continue; }
                renderVoice(v, L + from, R ? R + from : nullptr, n);
            }
            from += n;
        }
    }

    void renderVoice(Voice& v, float* L, float* R, int n)
    {
        const float detA = centsToRatio(s_.detune);
        const float bendR = bendRatio(bend_, getInt(kBend));
        for (int i = 0; i < n; ++i)
        {
            if (v.pending)
            {
                if (v.delay-- > 0) continue;
                startVoice(v);
            }
            const float fenv = v.filtEnv.process();
            const float aenv = v.ampEnv.process();

            if ((v.ctrl++ % kCtrlInterval) == 0)
            {
                const float hz = mpc::noteToHz((float)v.note) * bendR;
                v.a1.setFrequency(hz * detA); v.a2.setFrequency(hz / detA); v.s.setFrequency(hz * 0.5f);
                const float octaves = s_.envAmt * fenv * 5.0f + s_.keyTrack * ((float)v.note - 60.0f) / 12.0f;
                const float cutoff = s_.cutoff * std::pow(2.0f, clamp(octaves, -8.0f, 8.0f));
                v.f1.set(cutoff, 0.0f); v.f2.set(cutoff, s_.reso);
            }

            const float osc = (v.a1.process() + v.a2.process()) * 0.5f + v.s.process() * s_.sub;
            const float y = v.f2.process(v.f1.process(osc, SVF::Mode::Lowpass), SVF::Mode::Lowpass) * aenv * v.velGain * 0.35f;
            L[i] += y * v.panL;
            if (R) R[i] += y * v.panR;
            if (!v.ampEnv.isActive()) { v.note = -1; v.key = -1; break; }
        }
    }

    // Volume, stereo chorus, ping-pong delay.
    void applyEffects(float* L, float* R, mpc::ProcessContext& ctx)
    {
        const float chMix = get(kChorusMix) * 0.01f, chDepth = get(kChorusDepth) * 0.01f;
        const float chInc = get(kChorusRate) / (float)ctx.sampleRate;
        const float baseSamples = 0.012f * (float)ctx.sampleRate, swing = 0.005f * (float)ctx.sampleRate * chDepth;
        const float dMix = get(kDelayMix) * 0.01f, dFb = get(kDelayFback) * 0.01f;
        const float dSamples = clamp(get(kDelayTime) * 0.001f * (float)ctx.sampleRate, 2.0f, (float)ctx.sampleRate * 0.99f);
        const bool chorusOn = chMix > 0.001f, delayOn = dMix > 0.001f;

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
            if (delayOn)
            {
                const float wl = dlL_.readLinear(dSamples), wr = dlR_.readLinear(dSamples);
                dlL_.write(sanitize(l + wr * dFb));       // cross-feedback = ping-pong
                dlR_.write(sanitize(r + wl * dFb));
                l += wl * dMix; r += wr * dMix;
            }
            L[i] = softClip(l);
            if (R) R[i] = softClip(r);
        }
    }

    Voice voices_[kNumVoices];
    Smoother volume_;
    DelayLine chL_, chR_, dlL_, dlR_;
    float chPhase_ = 0.0f, bend_ = 0.0f;
    bool sustain_ = false;
    unsigned long long age_ = 0;
    double sr_ = 44100.0;
};

constexpr Chordbox::ChordDef Chordbox::kChords[Chordbox::kNumChords];

MPC_PLUGIN_ENTRY(Chordbox)
