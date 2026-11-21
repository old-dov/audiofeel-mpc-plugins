// Impactor - cinematic impacts, risers and downers for the MPC, by AudioFeel.
//
// SPDX-License-Identifier: MIT
//
// Every sound is synthesized (no samples): sine bodies with pitch sweeps, filtered noise,
// inharmonic partials, detuned saws through a moving low-pass. One-shots: note-offs are ignored.
//
// MIDI map (notes 36..51, the pads of an MPC / a drum rack):
//   Impacts   36 Boom      37 Hit       38 Slam      39 Metal Hit  40 Thud
//   Risers    41 Noise Rs  42 Tone Rs   43 Tension    44 Rev Swell
//   Downers   45 Downer    46 Sub Drop  47 Whoosh
//   Other     48 Heartbeat 49 Rumble    50 Glass      51 Sting
//
// Style (Trailer, Horror, Sci-Fi, Industrial, Organic, Glitch) changes what the sounds are made of: each style
// adds its own layer to the impacts (a beating sub pair, a dissonant cluster, an FM zap, a clanging metal layer, a
// woody knock, a stutter) and colours every sound (ring modulation, hard clip and metal comb, bit-crush...).
//
// Played like an instrument (v2): velocity sets loudness, brightness, pitch and length; Variation makes every
// hit slightly different; the mod wheel brightens and pushes the drive, aftertouch opens the reverb; keys above
// the pads (note 52 and up) play the sound chosen by Keys Sound, pitched by the key.
//
// Each sound has Tune, Length, Tone, Level and Pan.
//   Length scales the sound's time: the decay of an impact, the whole run-up of a riser.
//   Tone is brightness (filter colour, overtones), a different colour for each sound.
// The whole output goes through a drive stage and a big algorithmic reverb (Mix / Size / Damp),
// which is what makes a hit sound like a trailer hit.
//
// Signal flow: 16 independent voices -> per-sound level and pan -> reverb send -> drive
// (soft saturation) -> master volume -> outputs.

#include "mpc_plugin.h"
#include "dsp/math.h"
#include "dsp/oscillator.h"
#include "dsp/noise.h"
#include "dsp/svf.h"
#include "dsp/smoother.h"
#include "dsp/midi_cc.h"

#include <algorithm>
#include <string>
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

// Freeverb-style stereo reverb. Size up to 0.98 feedback = a long hall tail.
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
        const float fb = 0.72f + 0.265f * size01;
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

class Impactor : public mpc::Plugin
{
public:
    enum Sound
    {
        kBoom = 0, kHit, kSlam, kMetal, kThud,
        kNoiseRiser, kToneRiser, kTension, kRevSwell,
        kDowner, kSubDrop, kWhoosh,
        kHeartbeat, kRumble, kGlass, kSting,
        kNumSounds
    };
    static constexpr int kFirstNote = 36;
    enum Which { kTune = 0, kLength, kTone, kLevel, kPan, kParamsPerSound };

    // Parameter order is permanent once users have projects. Global parameters first
    // (parameter 0 is nudged by the host on every load, so it is the harmless master volume).
    enum Globals { kVolume = 0, kDrive, kVelSens, kRevMix, kRevSize, kRevDamp, kFirstSoundParam };
    static int pidx(int s, Which w) { return kFirstSoundParam + s * kParamsPerSound + (int)w; }
    // Appended after the per-sound parameters: Variation (every hit slightly different) and the sound the keys above the pads play.
    enum Extras { kVariation = kFirstSoundParam + kNumSounds * kParamsPerSound, kKeysSound, kStyle, kNumParams };

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Impactor";
        i.vendor = "AudioFeel";
        i.uniqueId = MPC_FOURCC('A', 'g', 'L', 'k');   // must match "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = true;
        i.numInputs = 0;
        i.numOutputs = 2;
        return i;
    }

    Impactor() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        static const char* const tags[kNumSounds] = {"Boom", "Hit", "Slam", "Metal", "Thud", "NzRise", "TnRise", "Tens", "RvSwl",
                                                     "Downr", "SubDrp", "Whoosh", "Heart", "Rmbl", "Glass", "Sting"};
        static const char* const ids[kNumSounds]  = {"boom", "hit", "slam", "metal", "thud", "nzrise", "tnrise", "tens", "rvswl",
                                                     "downr", "subdrp", "whoosh", "heart", "rmbl", "glass", "sting"};

        addParam(floatParam("volume", "Volume",   -40.0f, 6.0f, -4.0f, "dB", 1));
        addParam(floatParam("drive",  "Drive",      0.0f, 100.0f, 20.0f, "%", 0));
        addParam(floatParam("velsens", "Vel Sens",  0.0f, 100.0f, 85.0f, "%", 0));
        addParam(floatParam("revmix",  "Rev Mix",   0.0f, 100.0f, 30.0f, "%", 0));
        addParam(floatParam("revsize", "Rev Size",  0.0f, 100.0f, 75.0f, "%", 0));
        addParam(floatParam("revdamp", "Rev Damp",  0.0f, 100.0f, 45.0f, "%", 0));
        for (int s = 0; s < kNumSounds; ++s)
        {
            const std::string id = ids[s], tag = tags[s];
            addParam(floatParam((id + "_tune").c_str(),   (tag + " Tune").c_str(),   -12.0f, 12.0f, 0.0f, "st", 1));
            addParam(floatParam((id + "_length").c_str(), (tag + " Length").c_str(),  25.0f, 250.0f, 100.0f, "%", 0));
            addParam(floatParam((id + "_tone").c_str(),   (tag + " Tone").c_str(),     0.0f, 100.0f, 50.0f, "%", 0));
            addParam(floatParam((id + "_level").c_str(),  (tag + " Level").c_str(),    0.0f, 100.0f, 80.0f, "%", 0));
            addParam(floatParam((id + "_pan").c_str(),    (tag + " Pan").c_str(),   -100.0f, 100.0f, (float)kDefaultPan[s], "%", 0));
        }
        addParam(floatParam ("variation", "Variation",   0.0f, 100.0f, 35.0f, "%", 0));
        addParam(choiceParam("keyssound", "Keys Sound",  {"Off", "Boom", "Hit", "Thud", "Sub Drop", "Metal", "Sting", "Heartbeat"}, 2));
        addParam(choiceParam("style", "Style", {"Trailer", "Horror", "Sci-Fi", "Industrial", "Organic", "Glitch"}, 0));
        if (numParams() != kNumParams) mpc::log("Impactor: parameter count mismatch (%d vs %d)", numParams(), (int)kNumParams);

        // ----- built-in programs -----
        struct Cfg { int sound; float tune, length, tone, level; };
        auto prog = [this](const char* name, int style, float drive, float revMix, float revSize, float revDamp, std::initializer_list<Cfg> cfg)
        {
            std::vector<float> v((size_t)numParams());
            for (int i = 0; i < numParams(); ++i) v[(size_t)i] = param(i).def;
            v[(size_t)kStyle] = (float)style; v[(size_t)kDrive] = drive; v[(size_t)kRevMix] = revMix; v[(size_t)kRevSize] = revSize; v[(size_t)kRevDamp] = revDamp;
            for (const Cfg& c : cfg)
            {
                v[(size_t)pidx(c.sound, kTune)] = c.tune; v[(size_t)pidx(c.sound, kLength)] = c.length;
                v[(size_t)pidx(c.sound, kTone)] = c.tone; v[(size_t)pidx(c.sound, kLevel)] = c.level;
            }
            addProgram(name, std::move(v));
        };
        prog("Init Cinema", 0, 20, 30, 75, 45, {});
        prog("Trailer Hits", 0, 30, 40, 85, 40, {{kBoom, -3, 130, 50, 95}, {kHit, -2, 140, 60, 95}, {kSlam, 0, 120, 65, 90}, {kMetal, -2, 150, 60, 70},
                                              {kNoiseRiser, 0, 120, 60, 70}, {kToneRiser, 0, 110, 55, 65}, {kSubDrop, 0, 100, 50, 90}, {kRevSwell, 0, 100, 60, 65}});
        prog("Dark Drama", 4, 25, 45, 90, 55, {{kBoom, -5, 180, 40, 95}, {kThud, -3, 140, 40, 80}, {kHeartbeat, -2, 110, 40, 90}, {kRumble, -2, 150, 35, 80},
                                            {kTension, -2, 130, 40, 70}, {kDowner, -3, 130, 35, 75}, {kSubDrop, -4, 130, 40, 90}, {kSting, -3, 120, 40, 65}});
        prog("Horror Pack", 1, 35, 50, 88, 50, {{kSting, 2, 140, 70, 85}, {kTension, 3, 150, 65, 80}, {kGlass, 0, 120, 75, 75}, {kHeartbeat, 2, 80, 45, 85},
                                             {kRumble, 0, 160, 45, 75}, {kMetal, 3, 160, 70, 70}, {kWhoosh, -2, 120, 50, 60}, {kDowner, 0, 110, 50, 70}});
        prog("Sci-Fi Sweeps", 2, 15, 30, 70, 30, {{kNoiseRiser, 2, 70, 80, 80}, {kToneRiser, 3, 70, 75, 75}, {kRevSwell, 2, 70, 80, 75}, {kWhoosh, 3, 60, 85, 80},
                                               {kDowner, 2, 70, 70, 70}, {kSubDrop, 0, 80, 50, 85}, {kGlass, 3, 70, 85, 60}, {kTension, 4, 80, 75, 65}});
        prog("Short & Punchy", 3, 40, 15, 40, 60, {{kBoom, 2, 50, 60, 90}, {kHit, 2, 45, 70, 90}, {kSlam, 3, 45, 70, 90}, {kThud, 3, 60, 60, 85},
                                                {kMetal, 3, 50, 60, 65}, {kSubDrop, 0, 50, 60, 90}, {kWhoosh, 2, 40, 70, 70}, {kHeartbeat, 4, 60, 50, 85}});
        prog("Epic Hall", 0, 25, 65, 98, 35, {{kBoom, -4, 160, 55, 95}, {kHit, -2, 140, 65, 95}, {kMetal, -3, 170, 60, 70}, {kRevSwell, 0, 140, 65, 70},
                                           {kNoiseRiser, 0, 150, 65, 70}, {kSting, 0, 140, 55, 65}, {kSubDrop, -2, 130, 50, 85}});
        prog("Industrial Plant", 3, 45, 14, 40, 55, {{kBoom, 2, 60, 60, 85}, {kHit, 0, 70, 70, 90}, {kSlam, -2, 90, 75, 95}, {kMetal, -4, 110, 70, 90},
                                                      {kThud, -2, 80, 65, 85}, {kNoiseRiser, 0, 80, 65, 70}, {kWhoosh, -2, 70, 60, 65}, {kRumble, 0, 100, 55, 70}});
        prog("Cyber Zaps", 2, 35, 20, 45, 35, {{kBoom, 3, 70, 75, 80}, {kHit, 4, 55, 80, 90}, {kSlam, 4, 55, 85, 85}, {kMetal, 3, 90, 80, 70},
                                                 {kNoiseRiser, 3, 60, 85, 75}, {kToneRiser, 4, 60, 85, 75}, {kDowner, 3, 60, 75, 70}, {kWhoosh, 4, 50, 85, 75}});
        prog("Glitch Machine", 5, 30, 12, 35, 45, {{kBoom, 0, 55, 55, 85}, {kHit, 3, 45, 65, 90}, {kSlam, 5, 40, 70, 85}, {kThud, 2, 50, 60, 85},
                                                     {kGlass, 0, 60, 70, 70}, {kWhoosh, 0, 45, 65, 65}, {kSubDrop, 0, 60, 55, 85}, {kHeartbeat, 3, 60, 55, 80}});
        prog("Ghost Town", 1, 20, 55, 92, 50, {{kSting, -2, 150, 60, 85}, {kTension, -1, 150, 55, 75}, {kRumble, -3, 150, 40, 75}, {kHeartbeat, -3, 120, 40, 80},
                                                  {kGlass, -2, 130, 55, 70}, {kDowner, -2, 130, 45, 70}, {kBoom, -5, 160, 35, 85}, {kMetal, -4, 160, 50, 70}});
        prog("Dry Foley", 4, 15, 8, 30, 70, {{kThud, 0, 70, 50, 90}, {kHit, 0, 50, 45, 85}, {kSlam, 0, 55, 55, 85}, {kHeartbeat, 0, 70, 45, 90},
                                          {kGlass, 0, 60, 60, 70}, {kWhoosh, 0, 60, 55, 75}, {kMetal, 0, 60, 50, 65}, {kRumble, 0, 60, 40, 70}});
    }

    // ----- lifecycle --------------------------------------------------------
    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = (float)sampleRate;
        for (auto& v : voices_)
        {
            v.f1.setSampleRate(sampleRate); v.f2.setSampleRate(sampleRate);
            for (auto& o : v.saw) { o.setSampleRate(sampleRate); o.setWave(Wave::Saw); }
        }
        for (auto& kv : keyVoices_) { kv.v.f1.setSampleRate(sampleRate); kv.v.f2.setSampleRate(sampleRate); for (auto& o : kv.v.saw) { o.setSampleRate(sampleRate); o.setWave(Wave::Saw); } }
        controllers_.prepare(sampleRate, 10.0f);
        reverb_.prepare(sampleRate);
        volume_.setTime(10.0f, sampleRate);
        volume_.snap(dbToGain(get(kVolume)));
    }

    void reset() override
    {
        for (auto& v : voices_) { v.active = false; v.f1.reset(); v.f2.reset(); }
        for (auto& kv : keyVoices_) kv.v.active = false;
        controllers_.reset();
        reverb_.clear();
        volume_.snap(dbToGain(get(kVolume)));
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
            if (ev.isNoteOn())
            {
                const int idx = ev.note() - kFirstNote;
                if (idx >= 0 && idx < kNumSounds) trigger(idx, ev.velocity());
                else if (idx >= kNumSounds) triggerKey(ev.note(), ev.velocity());
            }
            else controllers_.handle(ev);        // mod wheel, aftertouch
        }
        render(L, R, pos, ctx.numFrames);

        // Master: reverb, drive (soft saturation) and volume.
        // Mod wheel pushes the drive, aftertouch opens the reverb: both are live gestures.
        const float drive = 1.0f + get(kDrive) * 0.04f + 1.2f * controllers_.modWheel();
        const float comp = 1.0f / fastTanh(drive);
        const float revMix = clamp(get(kRevMix) * 0.01f + 0.4f * controllers_.aftertouch(), 0.0f, 1.0f);
        const float revSize = get(kRevSize) * 0.01f, revDamp = get(kRevDamp) * 0.01f;
        for (int i = 0; i < ctx.numFrames; ++i)
        {
            controllers_.tick();
            float l = L[i], r = R ? R[i] : L[i];
            if (revMix > 0.001f)
            {
                float wl, wr;
                reverb_.process(l, r, revSize, revDamp, wl, wr);
                l += wl * revMix * 2.4f; r += wr * revMix * 2.4f;
            }
            const float g = volume_.process();
            L[i] = softClip(fastTanh(l * drive) * comp * g);
            if (R) R[i] = softClip(fastTanh(r * drive) * comp * g);
        }
    }

private:
    static constexpr int kDefaultPan[kNumSounds] = {0, 0, 0, 15, 0, -25, 25, -15, 20, 20, 0, -20, 0, 0, 30, -10};
    static constexpr int kMetalCount = 5;
    static constexpr float kVoiceTrim = 0.42f;   // headroom: the voices are written hot, the drive stage should not be squashing them

    // One voice. Envelopes are exponential (x *= coef per sample) or driven by progress p = t / dur.
    struct SoundVoice
    {
        bool active = false;
        float t = 0.0f;                    // seconds since the trigger
        float dur = 1.0f;                  // run-up length for risers / downers
        float e1 = 0.0f, e2 = 0.0f, e3 = 0.0f;
        float c1 = 0.99f, c2 = 0.99f, c3 = 0.99f;
        float f0 = 100.0f, tone = 0.5f, vel = 1.0f, tuneRatio = 1.0f, lenScale = 1.0f;
        float ph[kMetalCount + 3] = {0};
        float brown = 0.0f, lfo = 0.0f;
        int counter = 0, hits = 0;
        // Style layer and colouring state.
        int style = 0;
        float baseHz = 110.0f, le = 1.0f, lc = 0.99f, ringPh = 0.0f, holdVal = 0.0f;
        float lph[4] = {0};
        int holdCnt = 0, cbi = 0;
        float cb[256] = {0};
        SVF f1, f2;
        PolyBlepOsc saw[3];
        Xorshift32 rng{0x9E3779B9u};
    };

    struct Snapshot { float tuneRatio, lenScale, tone, level, gainL, gainR; };

    void readSettings()
    {
        velSens_ = get(kVelSens) * 0.01f;
        variation_ = get(kVariation) * 0.01f;
        volume_.setTarget(dbToGain(get(kVolume)));
        for (int s = 0; s < kNumSounds; ++s)
        {
            Snapshot& sn = snap_[s];
            sn.tuneRatio = semitonesToRatio(get(pidx(s, kTune)));
            sn.lenScale = get(pidx(s, kLength)) * 0.01f;
            sn.tone = get(pidx(s, kTone)) * 0.01f;
            sn.level = get(pidx(s, kLevel)) * 0.01f;
            equalPowerPan(get(pidx(s, kPan)) * 0.01f, sn.gainL, sn.gainR);
            sn.gainL *= kSqrt2; sn.gainR *= kSqrt2;     // unity at centre
        }
    }

    float coefFor(float seconds) const { return seconds <= 0.0f ? 0.0f : std::exp(-1.0f / (seconds * sr_)); }

    // ----- trigger: set the coefficients for the sound's recipe ---------------
    void trigger(int s, int velocity)
    {
        if (s < 0 || s >= kNumSounds) return;
        triggerInto(voices_[s], s, velocity, 0.0f);
    }

    // Keys above the pads: the chosen sound, pitched by the key, from a small pool of voices.
    void triggerKey(int note, int velocity)
    {
        static const int map[8] = {-1, kBoom, kHit, kThud, kSubDrop, kMetal, kSting, kHeartbeat};
        const int s = map[clamp(getInt(kKeysSound), 0, 7)];
        if (s < 0) return;
        KeyVoice& kv = keyVoices_[keyNext_];
        keyNext_ = (keyNext_ + 1) % kKeyPool;
        kv.sound = s;
        triggerInto(kv.v, s, velocity, (float)(note - 60));
    }

    void triggerInto(SoundVoice& v, int s, int velocity, float extraSemis)
    {
        const Snapshot& sn = snap_[s];
        const float vn = (float)velocity / 127.0f;
        const float var = variation_, vsens = velSens_;
        // Velocity: a harder hit is louder, brighter, a touch higher and rings longer. The mod wheel brightens as well,
        // and Variation gives every hit its own small pitch, tone and length so repeats never sound like a loop.
        const float tone = clamp(sn.tone + (vn - 0.6f) * 0.6f * vsens + 0.4f * controllers_.modWheel() + 0.12f * var * rng_.nextBipolar(), 0.0f, 1.0f);
        const float tuneRatio = sn.tuneRatio * semitonesToRatio(extraSemis + (vn - 0.6f) * 0.5f * vsens + 0.35f * var * rng_.nextBipolar());
        const float lenScale = sn.lenScale * lerp(1.0f, 0.8f + 0.4f * vn, vsens) * (1.0f + 0.15f * var * rng_.nextBipolar());
        v.active = true; v.t = 0.0f; v.counter = 0; v.hits = 0;
        v.e1 = v.e2 = v.e3 = 1.0f;
        v.tone = tone; v.tuneRatio = tuneRatio; v.lenScale = lenScale;
        v.vel = lerp(1.0f, 0.12f + 0.88f * vn * vn, vsens);
        v.f1.reset(); v.f2.reset(); v.brown = 0.0f; v.lfo = 0.0f;
        for (float& p : v.ph) p = 0.0f;
        for (auto& o : v.saw) o.resetPhase();
        const float len = lenScale;
        v.style = clamp(getInt(kStyle), 0, 5);
        v.baseHz = 110.0f * tuneRatio;
        v.le = 1.0f; v.lc = coefFor(0.9f * len);
        v.ringPh = 0.0f; v.holdVal = 0.0f; v.holdCnt = 0; v.cbi = 0;
        for (float& q : v.lph) q = 0.0f;
        for (float& q : v.cb) q = 0.0f;
        if (v.style == 4) v.f2.set(500.0f + 900.0f * tone, 0.5f); else v.f2.set(6000.0f, 0.1f);

        switch (s)
        {
            case kBoom:
                v.f0 = 36.0f * tuneRatio;
                v.c1 = coefFor(1.6f * len);               // body
                v.c2 = coefFor(0.30f);                    // pitch sweep
                v.c3 = coefFor(1.1f * len);               // rumble
                v.f1.set(120.0f + 420.0f * tone, 0.2f);
                break;
            case kHit:
                v.f0 = 52.0f * tuneRatio;
                v.c1 = coefFor(0.95f * len);
                v.c2 = coefFor(0.07f);
                v.c3 = coefFor(0.32f * len);              // noise burst
                v.f1.set(700.0f + 6500.0f * tone, 0.15f);
                break;
            case kSlam:
                v.f0 = 46.0f * tuneRatio;
                v.c1 = coefFor(0.7f * len);
                v.c2 = coefFor(0.05f);
                v.c3 = coefFor(0.12f * len);
                v.f1.set(1100.0f + 3000.0f * tone, 0.3f);
                break;
            case kMetal:
                v.f0 = 170.0f * tuneRatio;
                v.c1 = coefFor(2.6f * len);               // fundamental + low partials
                v.c2 = coefFor(0.9f * len);               // upper partials
                v.c3 = coefFor(0.004f);                   // click
                break;
            case kThud:
                v.f0 = 68.0f * tuneRatio;
                v.c1 = coefFor(0.32f * len);
                v.c2 = coefFor(0.03f);
                v.c3 = coefFor(0.1f * len);
                v.f1.set(250.0f + 900.0f * tone, 0.2f);
                break;
            case kNoiseRiser:  v.dur = 4.0f * len; v.c3 = coefFor(0.03f); break;
            case kToneRiser:   v.dur = 5.0f * len; v.c3 = coefFor(0.03f); v.f0 = 110.0f * tuneRatio; break;
            case kTension:     v.dur = 6.0f * len; v.c3 = coefFor(0.5f);  v.f0 = 196.0f * tuneRatio; break;
            case kRevSwell:    v.dur = 2.6f * len; v.c3 = coefFor(0.02f); break;
            case kDowner:      v.dur = 4.0f * len; v.c3 = coefFor(0.3f);  v.f0 = 220.0f * tuneRatio; break;
            case kSubDrop:     v.dur = 1.7f * len; v.f0 = 95.0f * tuneRatio; break;
            case kWhoosh:      v.dur = 1.1f * len; break;
            case kHeartbeat:
                v.f0 = 54.0f * tuneRatio;
                v.c1 = coefFor(0.13f);                    // each thump
                v.c2 = coefFor(0.03f);
                v.f1.set(180.0f + 500.0f * tone, 0.2f);
                break;
            case kRumble:
                v.c1 = coefFor(1.05f * len);
                v.f1.set(70.0f + 330.0f * tone, 0.3f);
                break;
            case kGlass:
                v.c1 = coefFor(0.9f * len);
                v.c2 = coefFor(0.005f);
                v.f1.set(3500.0f + 4500.0f * tone, 0.1f);
                break;
            case kSting:
                v.f0 = 233.0f * tuneRatio;
                v.c1 = coefFor(2.2f * len);
                v.f1.set(1200.0f + 4500.0f * tone, 0.15f);
                break;
            default: break;
        }
    }

    float sine(float& phase, float hz)
    {
        phase += hz / sr_; if (phase >= 1.0f) phase -= 1.0f;
        return WavetableSine::lookup(phase);
    }

    // Cutoff for a filter swept over `octaves` octaves from `startHz` as p goes 0..1.
    static float sweep(float startHz, float octaves, float p) { return startHz * std::exp2(octaves * p); }

    // Ends a riser: at the end of its run the voice rings out on the short tail envelope e3.
    // Returns the amplitude multiplier (1 while running, falling tail afterwards).
    static float riserTail(SoundVoice& v, float p)
    {
        if (p < 1.0f) return 1.0f;
        const float g = v.e3;
        v.e3 *= v.c3;
        if (g < 1.0e-3f) v.active = false;
        return g;
    }

    float voiceSample(int s, SoundVoice& v)
    {
        const float dt = 1.0f / sr_;
        float out = 0.0f;
        const float n = v.rng.nextBipolar();
        v.t += dt;
        const float p = v.dur > 0.0f ? v.t / v.dur : 1.0f;      // progress of risers and downers

        switch (s)
        {
            case kBoom:
            {
                const float f = v.f0 * (1.0f + 1.4f * v.e2);
                const float body = sine(v.ph[0], f) + 0.22f * sine(v.ph[1], f * 2.0f) * v.e2;
                const float rumble = v.f1.process(n, SVF::Mode::Lowpass) * v.e3 * (0.7f + 1.2f * v.tone);
                out = fastTanh((body * v.e1 + rumble) * 1.4f);
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kHit:
            {
                const float f = v.f0 * (1.0f + 2.2f * v.e2);
                const float body = sine(v.ph[0], f);
                const float burst = v.f1.process(n, SVF::Mode::Lowpass) * v.e3 * (0.6f + 0.8f * v.tone);
                const float crack = n * v.e2 * v.e2 * 0.35f * v.tone;
                out = fastTanh((body * v.e1 * 1.1f + burst + crack) * 1.5f);
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kSlam:
            {
                const float f = v.f0 * (1.0f + 2.8f * v.e2);
                const float body = sine(v.ph[0], f);
                const float bp = v.f1.process(n, SVF::Mode::Bandpass) * v.e3 * 1.4f;
                out = fastTanh((body * v.e1 * 1.2f + bp) * (2.0f + 2.0f * v.tone));
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kMetal:
            {
                // Inharmonic bar/plate partials, upper ones dying first.
                static const float ratios[kMetalCount] = {1.0f, 2.76f, 5.40f, 8.93f, 11.34f};
                float sum = 0.0f;
                for (int i = 0; i < kMetalCount; ++i)
                {
                    const float amp = (i < 2 ? v.e1 : v.e2) * (i == 0 ? 1.0f : (0.55f - 0.08f * (float)i) * (0.4f + 1.2f * v.tone));
                    sum += sine(v.ph[i], v.f0 * ratios[i]) * amp;
                }
                out = sum * 0.5f + n * v.e3 * 0.5f;
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kThud:
            {
                const float f = v.f0 * (1.0f + 0.9f * v.e2);
                const float body = sine(v.ph[0], f);
                const float lp = v.f1.process(n, SVF::Mode::Lowpass) * v.e3 * 0.9f;
                out = fastTanh((body * v.e1 + lp) * 1.3f);
                if ((v.e1 *= v.c1) < 1.0e-4f) v.active = false;
                v.e2 *= v.c2; v.e3 *= v.c3;
                break;
            }
            case kNoiseRiser:
            {
                if ((++v.counter & 7) == 1) v.f1.set(sweep(250.0f, 5.0f + 2.0f * v.tone, std::min(p, 1.0f)), 0.35f);
                const float amp = (p < 1.0f ? p * p : 1.0f) * riserTail(v, p);
                out = v.f1.process(n, SVF::Mode::Bandpass) * amp * 2.4f;
                break;
            }
            case kToneRiser:
            {
                const float q = std::min(p, 1.0f);
                const float f = v.f0 * std::exp2(2.0f * q);
                float sum = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    v.saw[i].setFrequency(f * (1.0f + 0.006f * (float)(i - 1)));
                    sum += v.saw[i].process();
                }
                if ((++v.counter & 7) == 1) v.f1.set(sweep(350.0f, 4.0f + 1.5f * v.tone, q), 0.25f);
                const float amp = q * (0.4f + 0.6f * q) * riserTail(v, p);
                out = v.f1.process(sum * 0.35f, SVF::Mode::Lowpass) * amp * 1.4f;
                break;
            }
            case kTension:
            {
                // A dissonant cluster (root, minor second, tritone, fifth) that swells and creeps upward.
                static const float ratios[4] = {1.0f, 1.0595f, 1.4142f, 1.4983f};
                const float q = std::min(p, 1.0f);
                const float creep = 1.0f + 0.06f * q;
                v.lfo += 5.5f * dt; if (v.lfo >= 1.0f) v.lfo -= 1.0f;
                const float trem = 0.75f + 0.25f * WavetableSine::lookup(v.lfo);
                float sum = 0.0f;
                for (int i = 0; i < 4; ++i) sum += sine(v.ph[i], v.f0 * ratios[i] * creep);
                const float hiss = v.f1.process(n, SVF::Mode::Highpass) * 0.12f * v.tone * q;
                if ((++v.counter & 15) == 1) v.f1.set(3500.0f + 3000.0f * q, 0.2f);
                const float amp = q * q * trem * riserTail(v, p);
                out = (sum * 0.22f + hiss) * amp * 1.5f;
                break;
            }
            case kRevSwell:
            {
                const float q = std::min(p, 1.0f);
                if ((++v.counter & 7) == 1) v.f1.set(sweep(2500.0f, 1.8f + 0.8f * v.tone, q), 0.1f);
                float m = 0.0f;
                static const float r[kMetalCount] = {263.0f, 400.0f, 421.0f, 474.0f, 587.0f};
                for (int i = 0; i < kMetalCount; ++i)
                {
                    v.ph[i] += r[i] * 2.2f / sr_; if (v.ph[i] >= 1.0f) v.ph[i] -= 1.0f;
                    m += v.ph[i] < 0.5f ? 1.0f : -1.0f;
                }
                const float amp = q * q * q * riserTail(v, p);
                out = v.f1.process(n * 0.8f + m * 0.12f, SVF::Mode::Highpass) * amp * 1.8f;
                break;
            }
            case kDowner:
            {
                const float q = std::min(p, 1.0f);
                const float f = v.f0 * std::exp2(-2.0f * q);
                float sum = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    v.saw[i].setFrequency(f * (1.0f + 0.007f * (float)(i - 1)));
                    sum += v.saw[i].process();
                }
                if ((++v.counter & 7) == 1) v.f1.set(sweep(7000.0f * (0.3f + 0.7f * v.tone), -4.0f, q) + 80.0f, 0.2f);
                const float amp = (1.0f - q) * (1.0f - 0.5f * q) * riserTail(v, p);
                out = v.f1.process(sum * 0.35f + n * 0.18f * (1.0f - q), SVF::Mode::Lowpass) * amp * 1.5f;
                break;
            }
            case kSubDrop:
            {
                const float q = std::min(p, 1.0f);
                const float f = v.f0 * std::exp2(-2.2f * q);
                const float fade = q < 0.8f ? 1.0f : (1.0f - q) * 5.0f;
                const float a = std::min(v.t * 120.0f, 1.0f);              // 8 ms anti-click attack
                out = fastTanh(sine(v.ph[0], f) * 1.4f) * fade * a;
                if (p >= 1.0f) v.active = false;
                break;
            }
            case kWhoosh:
            {
                const float q = std::min(p, 1.0f);
                if ((++v.counter & 7) == 1) v.f1.set(sweep(300.0f, 4.2f + 0.8f * v.tone, q), 0.3f);
                const float amp = 4.0f * q * (1.0f - q);
                out = v.f1.process(n, SVF::Mode::Bandpass) * amp * 2.4f;
                if (p >= 1.0f) v.active = false;
                break;
            }
            case kHeartbeat:
            {
                // Two thumps, the second a little later and a little softer.
                if (v.hits == 0) v.hits = 1;
                if (v.hits == 1 && v.t >= 0.30f * v.lenScale) { v.hits = 2; v.e1 = 0.8f; v.e2 = 1.0f; }
                const float f = v.f0 * (1.0f + 0.8f * v.e2);
                const float body = sine(v.ph[0], f);
                const float lp = v.f1.process(n, SVF::Mode::Lowpass) * v.e2 * 0.4f * v.tone;
                out = (body * v.e1 + lp) * 1.2f;
                v.e1 *= v.c1; v.e2 *= v.c2;
                if (v.hits == 2 && v.e1 < 1.0e-4f) v.active = false;
                break;
            }
            case kRumble:
            {
                v.brown = (v.brown + 0.03f * n) * 0.998f;
                const float a = std::min(v.t * 6.0f, 1.0f);
                const float body = v.f1.process(v.brown * 12.0f, SVF::Mode::Lowpass);
                out = fastTanh(body * v.e1 * a * 1.3f);
                if ((v.e1 *= v.c1) < 1.0e-3f) v.active = false;
                break;
            }
            case kGlass:
            {
                // Shards: random short noise bursts (rarer as it fades) over a ring of high partials.
                if (v.rng.nextFloat() < 0.0016f * (0.2f + v.e1)) v.e2 = 1.0f;
                static const float r[4] = {2310.0f, 3170.0f, 4130.0f, 5390.0f};
                float ring = 0.0f;
                for (int i = 0; i < 4; ++i) ring += sine(v.ph[i], r[i] * v.tuneRatio) * (0.4f - 0.06f * (float)i);
                const float shard = v.f1.process(n, SVF::Mode::Highpass) * v.e2 * (0.4f + 0.5f * v.tone);
                out = (shard + ring * v.e1 * 0.5f * (0.3f + 0.7f * v.tone)) * 2.2f * std::min(v.e1 * 4.0f, 1.0f);
                v.e2 *= v.c2;
                if ((v.e1 *= v.c1) < 1.0e-3f) v.active = false;
                break;
            }
            case kSting:
            {
                // Three detuned saws (root, tritone, minor second) that swell in and die away.
                static const float ratios[3] = {1.0f, 1.4142f, 1.0595f};
                v.lfo += 5.0f * dt; if (v.lfo >= 1.0f) v.lfo -= 1.0f;
                const float vib = 1.0f + 0.006f * WavetableSine::lookup(v.lfo);
                float sum = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    v.saw[i].setFrequency(v.f0 * ratios[i] * vib);
                    sum += v.saw[i].process();
                }
                const float a = std::min(v.t * 4.0f, 1.0f);
                out = v.f1.process(sum * 0.4f, SVF::Mode::Lowpass) * v.e1 * a * 1.3f;
                if ((v.e1 *= v.c1) < 1.0e-3f) v.active = false;
                break;
            }
            default: v.active = false; break;
        }
        return out * v.vel;
    }

    // The Style stage: each style adds its own layer to an impact and colours every sound, so the banks differ in
    // what they are made of, not only in how much reverb they get.
    //   Trailer     a beating sub pair and a shimmering tail.         Horror      a dissonant cluster, ring-modulated.
    //   Sci-Fi      an FM zap that falls through the sound, ring mod. Industrial  a clanging metal layer, hard clip, a metal comb.
    //   Organic     a woody knock and a stone-like body.              Glitch      bit-crush, sample-rate reduction and a stutter gate.
    float stylize(int s, SoundVoice& v, float y)
    {
        const bool impact = s == kBoom || s == kHit || s == kSlam || s == kMetal || s == kThud || s == kHeartbeat || s == kGlass || s == kSting;
        const float n = v.rng.nextBipolar();
        const float le = v.le;
        v.le *= v.lc;
        const float base = v.baseHz;
        const float dt = 1.0f / sr_;
        switch (v.style)
        {
            case 0:   // Trailer
            {
                if (impact)
                {
                    const float pair = (sine(v.lph[0], base * 0.5f * 1.004f) + sine(v.lph[1], base * 0.5f / 1.004f)) * 0.3f * le;
                    const float shimmer = v.f2.process(n, SVF::Mode::Highpass) * le * le * 0.14f * (0.3f + v.tone);
                    y += (pair + shimmer) * v.vel;
                }
                break;
            }
            case 1:   // Horror
            {
                if (impact)
                {
                    const float cluster = sine(v.lph[0], base * 1.4142f) + sine(v.lph[1], base * 1.0595f * 2.0f) * 0.8f + sine(v.lph[2], base * 2.97f) * 0.5f;
                    y += cluster * 0.2f * le * v.vel;
                }
                v.ringPh += (base * 0.75f * (1.0f + 0.15f * fastSin01(v.t * 0.9f))) * dt; if (v.ringPh >= 1.0f) v.ringPh -= 1.0f;
                y = lerp(y, y * WavetableSine::lookup(v.ringPh) * 1.6f, 0.55f);
                break;
            }
            case 2:   // Sci-Fi
            {
                if (impact)
                {
                    const float f = base * 4.0f * (1.0f + 3.0f * le * le);                       // a zap that falls
                    const float mod = sine(v.lph[1], f * 2.0f) * le * 2.5f * (1.0f / kTwoPi);
                    y += WavetableSine::lookup(v.lph[0] + mod) * 0.28f * le * v.vel;
                    v.lph[0] += f * dt; if (v.lph[0] >= 1.0f) v.lph[0] -= 1.0f;
                }
                v.ringPh += 880.0f * v.tuneRatio * dt; if (v.ringPh >= 1.0f) v.ringPh -= 1.0f;
                y = lerp(y, y * WavetableSine::lookup(v.ringPh) * 1.5f, 0.4f);
                y = std::round(y * 40.0f) * (1.0f / 40.0f);                                         // a touch of digital grit
                break;
            }
            case 3:   // Industrial
            {
                if (impact)
                {
                    static const float r[4] = {1.0f, 2.41f, 3.78f, 5.2f};
                    float clang = 0.0f;
                    for (int i = 0; i < 4; ++i) clang += sine(v.lph[i], base * 3.0f * r[i]) * (1.0f - 0.2f * (float)i);
                    y += clang * 0.22f * le * le * v.vel;
                }
                y = clamp(y * 3.0f, -0.7f, 0.7f);                                                   // hard clip
                const int len = clamp((int)(sr_ / (base * 1.5f)), 24, 255);
                const float fed = v.cb[(v.cbi + 256 - len) & 255];
                v.cb[v.cbi & 255] = y + fed * 0.72f;
                v.cbi = (v.cbi + 1) & 255;
                y = y * 0.6f + fed * 0.55f;
                break;
            }
            case 4:   // Organic
            {
                if (impact)
                {
                    const float k = le * le * le;
                    const float knock = sine(v.lph[0], base * 2.5f) * 0.5f + v.f2.process(n, SVF::Mode::Bandpass) * 0.7f;
                    y += knock * k * 0.5f * v.vel;
                }
                break;
            }
            case 5:   // Glitch
            {
                if (--v.holdCnt <= 0) { v.holdVal = y; v.holdCnt = 4 + (int)(v.tone * 6.0f); }
                y = std::round(v.holdVal * 24.0f) * (1.0f / 24.0f);
                if (v.t > 0.12f)
                {
                    const float gate = fastSin01(v.t * 15.0f);
                    y *= gate > -0.2f ? 1.0f : 0.15f;                                               // the stutter
                }
                if (impact) y += (n > 0.98f ? 0.3f : 0.0f) * le;                                    // digital crackle
                break;
            }
            default: break;
        }
        return y;
    }

    void render(float* L, float* R, int from, int to)
    {
        for (int s = 0; s < kNumSounds; ++s)
        {
            SoundVoice& v = voices_[s];
            if (!v.active) continue;
            const Snapshot& sn = snap_[s];
            const float gl = sn.level * sn.gainL * kVoiceTrim, gr = sn.level * sn.gainR * kVoiceTrim;
            for (int i = from; i < to && v.active; ++i)
            {
                const float y = flushDenormal(sanitize(stylize(s, v, voiceSample(s, v))));
                L[i] += y * gl;
                if (R) R[i] += y * gr;
            }
        }
        for (KeyVoice& kv : keyVoices_)
        {
            SoundVoice& v = kv.v;
            if (!v.active) continue;
            const Snapshot& sn = snap_[kv.sound];
            const float gl = sn.level * sn.gainL * kVoiceTrim, gr = sn.level * sn.gainR * kVoiceTrim;
            for (int i = from; i < to && v.active; ++i)
            {
                const float y = flushDenormal(sanitize(stylize(kv.sound, v, voiceSample(kv.sound, v))));
                L[i] += y * gl;
                if (R) R[i] += y * gr;
            }
        }
    }

    static constexpr int kKeyPool = 4;
    struct KeyVoice { SoundVoice v; int sound = kHit; };
    SoundVoice voices_[kNumSounds];
    KeyVoice keyVoices_[kKeyPool];
    int keyNext_ = 0;
    MidiControllers controllers_;
    Xorshift32 rng_{0x2545F491u};
    float variation_ = 0.35f;
    Snapshot snap_[kNumSounds];
    Reverb reverb_;
    Smoother volume_;
    float velSens_ = 0.6f;
    float sr_ = 44100.0f;
};

MPC_PLUGIN_ENTRY(Impactor)
