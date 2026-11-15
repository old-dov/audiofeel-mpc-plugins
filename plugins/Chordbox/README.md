# Chordbox (AudioFeel)

Chord synth for Akai MPC standalone and VST2 hosts: one key plays a whole chord.

- **Manual mode**: every key plays the selected chord type (24 types: triads, sus, power, 6ths, 7ths, 9ths, m11, 13, 7#9...).
- **Scale mode**: pick a Key and a Scale (major, minor, dorian, phrygian, lydian, mixolydian, harmonic minor, minor pentatonic); each scale key plays the chord that belongs to its degree, with Extension = triad / 7th / 9th / sus2 / sus4 / add9. Keys outside the scale fall back to the Chord type.
- **Shape**: Inversion (0-3), Voicing (close / open drop-2 / wide with low root), Strum (0-120 ms, low note first), Octave.
- **Sound**: two detuned oscillators + sub, 24 dB low-pass with envelope and key tracking, ADSR, velocity, stereo spread (low notes left, high right), chorus, ping-pong delay.

16 voices; a chord uses 3 to 7 of them, the oldest are stolen first. Sustain pedal and pitch bend work.

Not implemented: arpeggio (see Arpeggio), per-note editing of chords, reverb. Scale mode's pentatonic stacks scale steps rather than true thirds.
