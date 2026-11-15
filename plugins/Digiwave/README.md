# Digiwave (AudioFeel)

8-voice digital-wave polysynth for Akai MPC standalone, in the spirit of 1980s "digital oscillators into an analogue filter" polysynths. Written from scratch: the 12 waveforms are built from harmonic recipes in the plugin; nothing is copied from any instrument's ROM, and it is not an emulation of a specific unit.

- **OSC**: two oscillators per voice, each with one of 12 waves (Saw, Square, Pulse, Triangle, Organ, E-Piano, Clav, Brass, Strings, Voice, Bell, Sine), octave, plus interval, detune, level and noise for oscillator 2.
- **FILTER / VCF EG**: 24 dB/oct resonant low-pass; cutoff, resonance, key tracking, EG Int (signed) and an Attack-Decay-Break-Slope-Sustain-Release envelope.
- **VCA EG**: same envelope shape, plus velocity and volume.
- **MOD**: LFO (5 shapes) with delay, to pitch and to filter; bend range; the mod wheel adds up to 2 semitones of vibrato.
- **DELAY**: stereo delay with feedback and modulation (short times + modulation = chorus / flanger).

Break-point envelope: Attack to full level, Decay down to Break, Slope from Break to Sustain (up or down), then Release. Break at 100% and Slope irrelevant gives a classic ADSR.

Programs: Init Saw, Brass Stab, Digital E.Piano, Warm Strings, Bell Keys, Clav Funk, Drawbar Organ, Choir Pad, Digital Bass, Sync Lead.

Not implemented: aftertouch routing, unison/mono mode, real oscillator sync (the "Sync Lead" name is only a lead patch with an interval), the exact characters of any hardware. Simulator CPU: about 0.5% of a desktop core with 8 voices sounding.
