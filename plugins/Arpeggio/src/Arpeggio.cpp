// Arpeggio - an arpeggiator with its own pluck/lead sound for the MPC, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// A plugin cannot send MIDI to other MPC tracks, so this is an instrument: you hold
// notes (or a chord) and it plays them one after another with its own synth voice.
//
// Signal flow:
//   held notes -> pattern (Up, Down, ...) x octaves -> step clock (tempo-synced)
//     -> voice: osc + detuned "fat" osc -> SVF low-pass (filter env) -> amp ADSR * velocity
//     -> pan (ping-pong with "Spread") -> master volume -> outputs
//
// Timing:
//   * While the MPC plays, steps lock to the song position, so the arp stays on the grid.
//     Each step has an absolute index (song position / step length); a step is fired only
//     once, even if the host position wobbles by a rounding error.
//   * When the MPC is stopped, the clock free-runs and the first key you press starts the
//     pattern at once.
//   * Swing delays every second step by up to half a step (sample-accurate).
//   * Gate is the note length as a percentage of the step; the note off is sample-accurate.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/svf.h"
#include "dsp/adsr.h"
#include "dsp/smoother.h"
#include "dsp/tempo.h"

#include <cstdint>

using namespace mpc::dsp;

class Arpeggio : public mpc::Plugin
{
public:
    // Order is permanent once users have projects: append new parameters at the end.
    // Parameter 0 is nudged by the host on every load, so it is the harmless master volume.
    enum Params
    {
        kVolume = 0,   // dB
        kPattern,      // choice
        kRate,         // note division
        kOctaves,      // 1..4
        kGate,         // % of the step
        kSwing,        // %
        kLatch,        // bool
        kTranspose,    // semitones
        kVelSens,      // % (0 = every note the same loudness)
        kWave,         // Sine / Triangle / Saw / Square
        kPulseWidth,   // %
        kFat,          // % detuned second oscillator
        kCutoff,       // Hz
        kReso,         // %
        kEnvAmount,    // % (signed)
        kFilterDecay,  // ms
        kDrive,        // %
        kAttack,       // ms
        kDecay,        // ms
        kSustain,      // %
        kRelease,      // ms
        kSpread,       // % stereo ping-pong
        kNumParams
    };

    enum Pattern { kUp = 0, kDown, kUpDown, kDownUp, kRandom, kAsPlayed, kChord };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Arpeggio";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', '6', '8', 'Q');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Arpeggio() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("volume",  "Volume",       -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(choiceParam("pattern", "Pattern",      {"Up", "Down", "Up/Down", "Down/Up", "Random", "As Played", "Chord"}, kUp));
        addParam(choiceParam("rate",    "Rate",         divisionNames(), kDivisionSixteenth));
        addParam(intParam   ("octaves", "Octaves",      1, 4, 2));
        addParam(floatParam ("gate",    "Gate",         5.0f, 100.0f, 60.0f, "%", 0));
        addParam(floatParam ("swing",   "Swing",        0.0f, 100.0f, 0.0f, "%", 0));
        addParam(boolParam  ("latch",   "Latch",        false));
        addParam(intParam   ("transp",  "Transpose",   -24, 24, 0, "st"));
        addParam(floatParam ("velsens", "Velocity",     0.0f, 100.0f, 70.0f, "%", 0));
        addParam(choiceParam("wave",    "Wave",         {"Sine", "Triangle", "Saw", "Square"}, 2));
        addParam(floatParam ("pw",      "Pulse Width",  5.0f, 95.0f, 50.0f, "%", 0));
        addParam(floatParam ("fat",     "Fat",          0.0f, 100.0f, 40.0f, "%", 0));
        addParam(logParam   ("cutoff",  "Cutoff",       60.0f, 18000.0f, 2500.0f, "Hz", 0));
        addParam(floatParam ("reso",    "Resonance",    0.0f, 100.0f, 25.0f, "%", 0));
        addParam(floatParam ("envamt",  "Env Amount",  -100.0f, 100.0f, 50.0f, "%", 0));
        addParam(logParam   ("fdecay",  "Filter Decay", 20.0f, 3000.0f, 250.0f, "ms", 0));
        addParam(floatParam ("drive",   "Drive",        0.0f, 100.0f, 0.0f, "%", 0));
        addParam(logParam   ("attack",  "Attack",       0.5f, 2000.0f, 2.0f, "ms", 1));
        addParam(logParam   ("decay",   "Decay",        10.0f, 4000.0f, 350.0f, "ms", 0));
        addParam(floatParam ("sustain", "Sustain",      0.0f, 100.0f, 30.0f, "%", 0));
        addParam(logParam   ("release", "Release",      5.0f, 4000.0f, 250.0f, "ms", 0));
        addParam(floatParam ("spread",  "Spread",       0.0f, 100.0f, 35.0f, "%", 0));

        if (numParams() != kNumParams) mpc::log("Arpeggio: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        auto program = [this](const char* name, std::initializer_list<std::pair<int, float>> overrides)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            for (const auto& o : overrides) v[(size_t)o.first] = o.second;
            addProgram(name, std::move(v));
        };
        program("Classic Pluck", {});
        program("Trance Gate",   {{kPattern, kUp}, {kOctaves, 3}, {kGate, 45}, {kFat, 70}, {kCutoff, 1800}, {kEnvAmount, 70},
                                  {kFilterDecay, 140}, {kSustain, 0}, {kDecay, 180}, {kSpread, 60}});
        program("Soft Bells",    {{kPattern, kUpDown}, {kRate, kDivisionEighth}, {kGate, 90}, {kWave, 0}, {kFat, 20}, {kCutoff, 6000},
                                  {kReso, 5}, {kEnvAmount, 10}, {kDecay, 900}, {kSustain, 0}, {kRelease, 900}, {kSpread, 70}});
        program("Acid Run",      {{kOctaves, 2}, {kGate, 55}, {kSwing, 25}, {kFat, 0}, {kCutoff, 500}, {kReso, 65}, {kEnvAmount, 80},
                                  {kFilterDecay, 160}, {kDrive, 35}, {kSustain, 0}, {kDecay, 220}, {kSpread, 0}});
        program("Random Square", {{kPattern, kRandom}, {kOctaves, 3}, {kGate, 40}, {kWave, 3}, {kPulseWidth, 30}, {kFat, 0},
                                  {kCutoff, 2200}, {kEnvAmount, 60}, {kFilterDecay, 120}, {kSustain, 0}, {kDecay, 140}, {kSpread, 50}});
        program("Pad Shimmer",   {{kGate, 100}, {kWave, 1}, {kFat, 60}, {kCutoff, 3500}, {kEnvAmount, 20}, {kAttack, 30},
                                  {kDecay, 800}, {kSustain, 70}, {kRelease, 800}, {kSpread, 90}});
        // Patterns: Up 0, Down 1, Up/Down 2, Down/Up 3, Random 4, As Played 5, Chord 6.
        // Rates: 2 1/32, 3 1/16T, 4 1/16, 5 1/8T, 7 1/8, 10 1/4. Waves: 0 Sine, 1 Triangle, 2 Saw, 3 Square.
        program("Bubble Triplets", {{kPattern, kUpDown}, {kRate, 5}, {kOctaves, 2}, {kGate, 55}, {kWave, 1}, {kFat, 30}, {kCutoff, 3500},
                                  {kEnvAmount, 40}, {kFilterDecay, 180}, {kSustain, 0}, {kDecay, 220}, {kSpread, 70}});
        program("Fast Runner",   {{kPattern, kUp}, {kRate, 2}, {kOctaves, 4}, {kGate, 35}, {kWave, 2}, {kFat, 20}, {kCutoff, 4000},
                                  {kReso, 15}, {kEnvAmount, 30}, {kDecay, 120}, {kSustain, 0}, {kSpread, 50}});
        program("Deep Bass Arp", {{kPattern, kUp}, {kOctaves, 1}, {kGate, 70}, {kTranspose, -12}, {kWave, 3}, {kPulseWidth, 40}, {kFat, 0},
                                  {kCutoff, 500}, {kReso, 35}, {kEnvAmount, 70}, {kFilterDecay, 150}, {kDrive, 20}, {kDecay, 200},
                                  {kSustain, 0}, {kSpread, 0}});
        program("Dreamy Down",   {{kPattern, kDown}, {kRate, 7}, {kOctaves, 3}, {kGate, 100}, {kWave, 0}, {kFat, 40}, {kCutoff, 5000},
                                  {kAttack, 5}, {kDecay, 1200}, {kSustain, 0}, {kRelease, 1500}, {kSpread, 90}});
        program("Chord Stabs",   {{kPattern, kChord}, {kRate, 10}, {kGate, 25}, {kWave, 2}, {kFat, 70}, {kCutoff, 1400}, {kEnvAmount, 60},
                                  {kFilterDecay, 160}, {kSustain, 0}, {kDecay, 200}, {kSpread, 40}});
        program("Swing Funk",    {{kPattern, kUpDown}, {kOctaves, 2}, {kGate, 45}, {kSwing, 55}, {kWave, 3}, {kPulseWidth, 35}, {kFat, 0},
                                  {kCutoff, 1500}, {kReso, 30}, {kEnvAmount, 60}, {kFilterDecay, 120}, {kSustain, 0}, {kDecay, 150}});
        program("Played Order",  {{kPattern, kAsPlayed}, {kOctaves, 1}, {kGate, 60}, {kWave, 2}, {kFat, 35}, {kCutoff, 2200},
                                  {kEnvAmount, 45}, {kSustain, 0}, {kDecay, 260}});
        program("Down Up Lead",  {{kPattern, kDownUp}, {kOctaves, 2}, {kGate, 85}, {kWave, 2}, {kFat, 60}, {kCutoff, 3000}, {kReso, 20},
                                  {kEnvAmount, 25}, {kAttack, 3}, {kDecay, 400}, {kSustain, 50}, {kRelease, 250}, {kSpread, 45}});
        program("Sparkle",       {{kPattern, kRandom}, {kRate, 3}, {kOctaves, 4}, {kGate, 30}, {kWave, 0}, {kFat, 0}, {kCutoff, 9000},
                                  {kVelSens, 40}, {kDecay, 300}, {kSustain, 0}, {kSpread, 100}});
        program("Slow Pad Arp",  {{kPattern, kUp}, {kRate, 10}, {kOctaves, 2}, {kGate, 100}, {kWave, 1}, {kFat, 50}, {kCutoff, 2000},
                                  {kAttack, 80}, {kDecay, 900}, {kSustain, 60}, {kRelease, 1200}, {kSpread, 80}});
        program("Hard Techno",   {{kPattern, kUp}, {kOctaves, 1}, {kGate, 30}, {kTranspose, -12}, {kWave, 2}, {kFat, 20}, {kCutoff, 450},
                                  {kReso, 55}, {kEnvAmount, 85}, {kFilterDecay, 110}, {kDrive, 55}, {kDecay, 130}, {kSustain, 0}, {kSpread, 0}});
        program("Latch Ambient", {{kPattern, kUpDown}, {kRate, 7}, {kOctaves, 3}, {kLatch, 1}, {kWave, 1}, {kFat, 50}, {kCutoff, 3500},
                                  {kAttack, 15}, {kDecay, 1000}, {kSustain, 20}, {kRelease, 1600}, {kSpread, 85}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        for (auto& v : voices_)
        {
            v.osc.setSampleRate(sampleRate); v.fat.setSampleRate(sampleRate);
            v.filter.setSampleRate(sampleRate);
            v.ampEnv.setSampleRate(sampleRate); v.filtEnv.setSampleRate(sampleRate);
        }
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
    }

    void reset() override
    {
        for (auto& v : voices_) { v.ampEnv.reset(); v.filtEnv.reset(); v.filter.reset(); v.gateLeft = -1; v.age = 0; }
        for (auto& t : trigs_) t.used = false;
        numHeld_ = 0; numKeysDown_ = 0; noteIndex_ = 0; side_ = 1; voiceClock_ = 0;
        lastFired_ = -1; nextStepAt_ = 0.0; wasPlaying_ = false;
        volume_.snap(dbToGain(get(kVolume)));
    }

    // ----- audio ------------------------------------------------------------
    void process(mpc::ProcessContext& ctx) override
    {
        readSettings();
        const int n = ctx.numFrames;
        const double stepBeats = (double)divisionBeats(getInt(kRate));
        stepSamples_ = (double)beatsToSamples((float)stepBeats, ctx.transport.bpm, ctx.sampleRate);
        if (stepSamples_ < 1.0) stepSamples_ = 1.0;

        // Where is the next step boundary? Follow the host's song position while playing.
        playing_ = ctx.transport.playing && ctx.transport.ppqValid;
        if (playing_)
        {
            const double stepPos = ctx.transport.ppqPos / stepBeats;
            long long k = (long long)std::ceil(stepPos - 1.0e-6);       // first boundary at or after "now"
            // A boundary that fell in the very last sample of the previous block was carried, not fired.
            // The host position is now just past it, so the formula above would skip it: fire it now.
            const long long expected = lastFired_ + 1;
            if (lastFired_ >= 0 && k == expected + 1 && (stepPos - (double)expected) * stepSamples_ < 2.0) k = expected;
            if (k <= lastFired_)
            {
                if (lastFired_ - k <= 1) k = lastFired_ + 1;           // rounding: already fired
                else lastFired_ = k - 1;                               // the song jumped back (loop, locate)
            }
            nextStepAt_ = ((double)k - stepPos) * stepSamples_;
            if (nextStepAt_ < 0.0) nextStepAt_ = 0.0;
            lastFired_ = k - 1;                                         // so the loop fires k next
        }
        else if (wasPlaying_)
        {
            // Transport stopped: keep the grid going from here.
            nextStepAt_ = stepSamples_;
        }
        wasPlaying_ = playing_;

        int pos = 0, e = 0;
        for (;;)
        {
            // Earliest of: next MIDI event, next swung trigger, next step boundary, end of block.
            int t = n;
            if (e < ctx.numMidiEvents && ctx.midi[e].sampleOffset < t) t = ctx.midi[e].sampleOffset;
            for (const auto& tr : trigs_) if (tr.used && tr.at < t) t = tr.at;
            if (nextStepAt_ < (double)n) { const int s = (int)std::ceil(nextStepAt_); if (s < t) t = s; }
            if (t < pos) t = pos;

            if (t > pos) { renderVoices(ctx, pos, t); pos = t; }
            if (pos >= n) break;

            // At this sample: MIDI first (so a key landing on a step is heard), then triggers, then the step.
            while (e < ctx.numMidiEvents && ctx.midi[e].sampleOffset <= pos) handleMidi(ctx.midi[e++], pos);
            for (auto& tr : trigs_)
                if (tr.used && tr.at <= pos) { tr.used = false; for (int k = 0; k < tr.count; ++k) startVoice(tr.notes[k], tr.vel, tr.gate, tr.side); }
            while (nextStepAt_ <= (double)pos)
            {
                fireStep(lastFired_ + 1, pos);
                nextStepAt_ += stepSamples_;
            }
        }
        // Next block: positions are relative to its first sample.
        nextStepAt_ -= (double)n;
        for (auto& tr : trigs_) if (tr.used) tr.at -= n;
    }

private:
    static constexpr int kNumVoices = 8;
    static constexpr int kMaxHeld = 16;
    static constexpr int kCtrlInterval = 16;

    struct Voice
    {
        PolyBlepOsc osc, fat;
        SVF filter;
        ADSR ampEnv, filtEnv;
        float vel = 1.0f, panL = 0.7071f, panR = 0.7071f, noteN = 60.0f;
        int gateLeft = -1;       // samples until note off (-1 = no gate running)
        uint32_t age = 0;
        int ctrl = 0;
    };

    struct Held { int note = 0; int vel = 100; };

    // A delayed (swung) step waiting for its sample.
    struct Trig { bool used = false; int at = 0, count = 0, vel = 100, gate = 0, side = 1; int notes[kMaxHeld] = {0}; };

    struct Settings
    {
        int wave = 2;
        float fat = 0.4f, cutoff = 2500.0f, reso = 0.25f, envAmt = 0.5f, velSens = 0.7f;
    } s_;

    void readSettings()
    {
        s_.wave = getInt(kWave); s_.fat = get(kFat) * 0.01f;
        s_.cutoff = get(kCutoff); s_.reso = get(kReso) * 0.01f; s_.envAmt = get(kEnvAmount) * 0.01f;
        s_.velSens = get(kVelSens) * 0.01f;
        const float aa = get(kAttack), ad = get(kDecay), as = get(kSustain) * 0.01f, ar = get(kRelease), fd = get(kFilterDecay);
        const float pw = get(kPulseWidth) * 0.01f, drive = 1.0f + get(kDrive) * 0.09f;
        for (auto& v : voices_)
        {
            v.osc.setWave(s_.wave); v.fat.setWave(s_.wave);
            v.osc.setPulseWidth(pw); v.fat.setPulseWidth(pw);
            v.filter.setDrive(drive);
            v.ampEnv.set(aa, ad, as, ar);
            v.filtEnv.set(1.0f, fd, 0.0f, fd);
        }
        volume_.setTarget(dbToGain(get(kVolume)));
    }

    // ----- MIDI: collect the held notes ------------------------------------
    void removeNote(int note)
    {
        int j = 0;
        for (int i = 0; i < numHeld_; ++i) if (held_[i].note != note) held_[j++] = held_[i];
        numHeld_ = j;
    }

    void handleMidi(const mpc::MidiEvent& ev, int pos)
    {
        if (ev.isNoteOn())
        {
            // Latch: a fresh chord (every key had been released) replaces the latched notes.
            if (getBool(kLatch) && numKeysDown_ == 0) numHeld_ = 0;
            ++numKeysDown_;
            const bool first = (numHeld_ == 0);
            removeNote(ev.note());
            if (numHeld_ < kMaxHeld) { held_[numHeld_].note = ev.note(); held_[numHeld_].vel = ev.velocity(); ++numHeld_; }
            if (first)
            {
                noteIndex_ = 0;
                if (!playing_) nextStepAt_ = (double)pos;   // free-running: start right now
            }
        }
        else if (ev.isNoteOff())
        {
            if (numKeysDown_ > 0) --numKeysDown_;
            if (!getBool(kLatch)) removeNote(ev.note());
        }
        else if (ev.isAllNotesOff())
        {
            numHeld_ = 0; numKeysDown_ = 0;
            for (auto& t : trigs_) t.used = false;
            for (auto& v : voices_) if (v.ampEnv.isActive()) { v.ampEnv.noteOff(); v.filtEnv.noteOff(); v.gateLeft = -1; }
        }
    }

    // ----- one step of the arpeggio -----------------------------------------
    void fireStep(long long step, int pos)
    {
        lastFired_ = step;
        if (numHeld_ == 0) { noteIndex_ = 0; return; }

        int notes[kMaxHeld], count = 0, vel = 100;
        pickNotes(notes, count, vel);
        side_ = -side_;

        const int gate = (int)std::fmax(8.0, (double)get(kGate) * 0.01 * stepSamples_);
        const float swing = get(kSwing) * 0.01f;
        if ((step & 1) != 0 && swing > 0.0f)
        {
            for (auto& tr : trigs_)
            {
                if (tr.used) continue;
                tr.used = true;
                tr.at = pos + (int)std::fmax(1.0, (double)swing * 0.5 * stepSamples_);
                tr.count = count; tr.vel = vel; tr.gate = gate; tr.side = side_;
                for (int k = 0; k < count; ++k) tr.notes[k] = notes[k];
                return;
            }
            // Queue full (cannot happen at sane rates): play straight.
        }
        for (int k = 0; k < count; ++k) startVoice(notes[k], vel, gate, side_);
    }

    // Choose the note(s) for this step from the held notes. No allocation.
    void pickNotes(int* out, int& count, int& vel)
    {
        Held sorted[kMaxHeld];
        for (int i = 0; i < numHeld_; ++i) sorted[i] = held_[i];
        for (int i = 1; i < numHeld_; ++i)
        {
            Held key = sorted[i]; int j = i - 1;
            while (j >= 0 && sorted[j].note > key.note) { sorted[j + 1] = sorted[j]; --j; }
            sorted[j + 1] = key;
        }
        const int N = numHeld_;
        const int L = N * getInt(kOctaves);          // steps in one full upward run
        const int pattern = getInt(kPattern);
        const int transp = getInt(kTranspose);
        const int ni = (int)(noteIndex_ & 0x3fffffffu);
        count = 0;

        if (pattern == kChord)
        {
            for (int i = 0; i < N && count < kNumVoices - 2; ++i) out[count++] = clampNote(sorted[i].note + transp);
            vel = sorted[N - 1].vel;
            return;
        }

        int idx = 0, note = 60;
        switch (pattern)
        {
            case kDown:
                idx = L - 1 - (ni % L);
                break;
            case kUpDown:      // 1 2 3 4 3 2 | 1 2 3 ... (end notes are not repeated)
            case kDownUp:
                if (L >= 2)
                {
                    const int period = 2 * (L - 1), p = ni % period;
                    idx = p < L ? p : period - p;
                    if (pattern == kDownUp) idx = L - 1 - idx;
                }
                break;
            case kRandom:
                idx = (int)(nextRandom() % (uint32_t)L);
                break;
            case kAsPlayed:
                idx = ni % L;
                note = held_[idx % N].note + 12 * (idx / N);
                vel = held_[idx % N].vel;
                break;
            case kUp:
            default:
                idx = ni % L;
                break;
        }
        if (pattern != kAsPlayed) { note = sorted[idx % N].note + 12 * (idx / N); vel = sorted[idx % N].vel; }
        ++noteIndex_;
        out[count++] = clampNote(note + transp);
    }

    static int clampNote(int n) { return n < 0 ? 0 : (n > 127 ? 127 : n); }
    uint32_t nextRandom() { rng_ = rng_ * 1664525u + 1013904223u; return rng_ >> 8; }

    // ----- voices -----------------------------------------------------------
    void startVoice(int note, int velocity, int gateSamples, int side)
    {
        // Reuse an idle voice, otherwise steal the oldest.
        int vi = 0; uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < kNumVoices; ++i)
        {
            if (!voices_[i].ampEnv.isActive()) { vi = i; break; }
            if (voices_[i].age < oldest) { oldest = voices_[i].age; vi = i; }
        }
        Voice& v = voices_[vi];
        const float hz = midiToHz((float)note);
        v.noteN = (float)note;
        v.vel = lerp(1.0f, velocity / 127.0f, s_.velSens);
        v.age = ++voiceClock_;
        v.gateLeft = gateSamples;
        v.ctrl = 0;
        if (!v.ampEnv.isActive()) { v.osc.resetPhase(); v.fat.resetPhase(0.31f); v.filter.reset(); }
        v.osc.setFrequency(hz);
        v.fat.setFrequency(hz * centsToRatio(9.0f));
        equalPowerPan((float)side * get(kSpread) * 0.008f, v.panL, v.panR);
        v.ampEnv.noteOn(); v.filtEnv.noteOn();
    }

    void renderVoices(mpc::ProcessContext& ctx, int from, int to)
    {
        float* L = ctx.outputs[0];
        float* R = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        for (int i = from; i < to; ++i) { L[i] = 0.0f; if (R) R[i] = 0.0f; }

        for (int vi = 0; vi < kNumVoices; ++vi)
        {
            Voice& v = voices_[vi];
            if (!v.ampEnv.isActive()) continue;
            for (int i = from; i < to; ++i)
            {
                if (v.gateLeft >= 0 && --v.gateLeft < 0) { v.ampEnv.noteOff(); v.filtEnv.noteOff(); }
                const float aenv = v.ampEnv.process();
                const float fenv = v.filtEnv.process();
                if ((v.ctrl++ % kCtrlInterval) == 0)
                {
                    const float octaves = s_.envAmt * fenv * 5.0f + (v.noteN - 60.0f) * (0.3f / 12.0f);
                    v.filter.set(s_.cutoff * std::pow(2.0f, clamp(octaves, -8.0f, 8.0f)), s_.reso);
                }
                const float mix = (v.osc.process() + v.fat.process() * s_.fat) * (1.0f / (1.0f + s_.fat));
                const float y = v.filter.process(mix, SVF::Mode::Lowpass) * aenv * v.vel * 0.7f;
                L[i] += y * v.panL * kSqrt2;
                if (R) R[i] += y * v.panR * kSqrt2;
                if (!v.ampEnv.isActive()) break;
            }
        }
        for (int i = from; i < to; ++i)
        {
            const float g = volume_.process();
            L[i] = softClip(L[i]) * g;
            if (R) R[i] = softClip(R[i]) * g;
        }
    }

    Voice voices_[kNumVoices];
    Held held_[kMaxHeld];
    Trig trigs_[4];
    int numHeld_ = 0, numKeysDown_ = 0;
    Smoother volume_;

    // step clock
    double nextStepAt_ = 0.0;      // samples from the start of the current block to the next step
    double stepSamples_ = 5512.0;  // length of one step in samples
    long long lastFired_ = -1;     // absolute index of the last step fired
    bool playing_ = false, wasPlaying_ = false;

    uint32_t noteIndex_ = 0;
    int side_ = 1;
    uint32_t voiceClock_ = 0;
    uint32_t rng_ = 12345u;
};

MPC_PLUGIN_ENTRY(Arpeggio)
