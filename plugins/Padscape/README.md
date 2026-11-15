# Padscape (AudioFeel)

8-voice pad synth for Akai MPC standalone and VST2 hosts. Two layers per voice (each a detuned pair, with its own wave and octave), a 24 dB low-pass moved by a slow LFO at a different phase per voice, slow envelopes, per-voice stereo placement, a stereo chorus and a Freeverb-style reverb.

- **OSC**: Wave A, Wave B, Octave B, Blend, Detune, Width, Bend Range.
- **FILTER**: Cutoff, Resonance, Env Amount (signed), Key Track, LFO Rate/Depth. **AMP**: Attack, Decay, Sustain, Release, Velocity, Volume.
- **CHORUS**: Mix, Depth, Rate. **REVERB**: Mix, Size, Damp.

Programs: Init Pad, Warm Strings Pad, Glass Pad, Dark Drone, Soft Choir, Evolving Saw, Soft Sweep, Cinematic Swell, Shimmer Pad, Ambient Wash, Slow Square, Lush Ensemble, Sub Pad, Pluck Pad.

Not implemented: unison per voice beyond the detuned pairs, sustain-pedal-free hold mode. The reverb is a plain algorithmic one, not tuned to any hardware.
