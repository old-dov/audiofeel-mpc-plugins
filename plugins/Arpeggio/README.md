# Arpeggio (AudioFeel)

Arpeggiator with its own synth sound for Akai MPC standalone. Plugins cannot send MIDI to other tracks, so this is an instrument: hold notes or a chord and it plays them with its own voice.

- **ARP**: Rate (1/64 ... 2/1, triplets and dotted), Octaves (1-4), Gate, Swing, Pattern (Up, Down, Up/Down, Down/Up, Random, As Played, Chord), Transpose, Velocity, Latch.
- **SOUND**: Cutoff, Resonance, Env Amount, Filter Decay, Fat, Pulse Width, Drive, Wave.
- **AMP**: Attack, Decay, Sustain, Release, Volume, Spread (stereo ping-pong).

Timing: while the MPC plays, steps lock to the song position and tempo. When stopped, the clock free-runs and the first key starts the pattern at once. Swing and gate are sample-accurate.

Programs: Classic Pluck, Trance Gate, Soft Bells, Acid Run, Random Square, Pad Shimmer.

Verified in the host simulator only (steps land every 125 ms at 120 BPM in 1/16, identical for 32- and 512-frame blocks); not yet heard on hardware. Up to 8 voices; chords are limited to 6 notes.
