// cine_fx.h - shared master effects for the AudioFeel cinema series: volume, stereo chorus, reverb.
//
// SPDX-License-Identifier: MIT
//
// One copy of this header sits beside each plugin's .cpp, so every plugin stays a self-contained
// project folder. The reverb is a plain Freeverb-style algorithmic one (8 damped combs + 4 allpasses
// per channel), not tuned to any hardware.

#ifndef AUDIOFEEL_CINE_FX_H
#define AUDIOFEEL_CINE_FX_H

#include "dsp/math.h"
#include "dsp/smoother.h"
#include "dsp/delay_line.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cine {

using namespace mpc::dsp;

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
        const float fb = 0.72f + 0.265f * size01;           // up to 0.985: a long hall tail
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

// Volume -> stereo chorus -> reverb, in place on the output buffers.
class MasterFx
{
public:
    void prepare(double sr, float volumeGain)
    {
        const int maxChorus = (int)(sr * 0.04) + 8;
        chL_.prepare(maxChorus); chR_.prepare(maxChorus);
        reverb_.prepare(sr);
        volume_.setTime(10.0f, sr);
        volume_.snap(volumeGain);
    }
    void reset(float volumeGain)
    {
        chL_.reset(); chR_.reset(); reverb_.clear(); chPhase_ = 0.0f;
        volume_.snap(volumeGain);
    }

    // chMix/chDepth/revMix/revSize/revDamp are 0..1, chRate in Hz, revGain scales the wet signal.
    void process(float* L, float* R, int n, double sr, float volumeGain,
                 float chMix, float chDepth, float chRate, float revMix, float revSize, float revDamp, float revGain = 1.6f)
    {
        volume_.setTarget(volumeGain);
        const float chInc = chRate / (float)sr;
        const float baseSamples = 0.012f * (float)sr, swing = 0.005f * (float)sr * chDepth;
        const bool chorusOn = chMix > 0.001f, reverbOn = revMix > 0.001f;
        for (int i = 0; i < n; ++i)
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
                l += wl * revMix * revGain; r += wr * revMix * revGain;
            }
            L[i] = softClip(l);
            if (R) R[i] = softClip(r);
        }
    }

private:
    DelayLine chL_, chR_;
    Reverb reverb_;
    Smoother volume_;
    float chPhase_ = 0.0f;
};

// Held keys in the order they were pressed, for mono / legato playing (last note has priority).
struct NoteStack
{
    int count = 0;
    int notes[16] = {0};
    int vels[16] = {0};
    void clear() { count = 0; }
    void remove(int note)
    {
        int j = 0;
        for (int i = 0; i < count; ++i) if (notes[i] != note) { notes[j] = notes[i]; vels[j] = vels[i]; ++j; }
        count = j;
    }
    void push(int note, int vel)
    {
        remove(note);
        if (count == 16) { for (int i = 1; i < 16; ++i) { notes[i - 1] = notes[i]; vels[i - 1] = vels[i]; } count = 15; }
        notes[count] = note; vels[count] = vel; ++count;
    }
    bool empty() const { return count == 0; }
    int top() const { return notes[count - 1]; }
    int topVel() const { return vels[count - 1]; }
};

// One-pole glide coefficient for a control tick of `ticks` samples: 1 = jump, smaller = slower.
inline float glideCoef(float glideMs, int ticks, float sr)
{
    if (glideMs < 1.0f) return 1.0f;
    return 1.0f - std::exp(-(float)ticks / (glideMs * 0.001f * sr));
}

} // namespace cine

#endif
