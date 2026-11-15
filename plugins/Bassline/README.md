# Bassline (AudioFeel)

Monophonic bass synth for Akai MPC standalone. Main oscillator (sine/triangle/saw/square) + detuned "Fat" oscillator + sine sub, into a resonant low-pass with its own envelope and drive.

- **OSC**: Sub, Fat, Detune, Glide, Wave, Octave, Pulse Width, Legato.
- **FILTER**: Cutoff, Resonance, Env Amount (signed), Drive, Filter Decay, Key Track, Accent.
- **AMP**: Attack, Decay, Sustain, Release, Volume.

Playing: last-note priority. With Legato on, overlapping notes glide without retriggering (303 style) and releasing the newest key returns to the previous one. Accent makes velocity drive both loudness and filter-envelope depth. Pitch bend is +-2 semitones.

Programs: Acid 303, Sub Weight, Fat Saw, Square Pluck, Reese, Wobble Soft.

Not implemented: sustain pedal, mod wheel, portamento modes other than linear-in-pitch glide.
CPU: a single voice, well under 1% of a desktop core in the simulator.
