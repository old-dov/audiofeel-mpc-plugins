# AudioFeel MPC plugins

Seven free instruments for Akai MPC standalone (and as VST2 on Windows), built with inMusic's open-source
[MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk). All sounds are synthesized, no samples.

| Plugin | What it is |
|---|---|
| **Bassline** | Monophonic bass synth: main + detuned "Fat" oscillators, sub, resonant low-pass, glide/legato, slow "Motion" LFO. 21 presets. |
| **Arpeggio** | Arpeggiator with its own synth voice (a plugin cannot send MIDI to other tracks). 7 patterns, tempo-synced, gate, swing, latch. 18 presets. |
| **Digiwave** | 8-voice digital-wave polysynth: two 12-wave oscillators per voice, 24 dB low-pass, break-point envelopes, LFO, stereo delay. 24 presets. Written from scratch, inspired by the architecture of 1980s digital-oscillator synths; it is not an emulation of any instrument and copies nothing from any ROM. |
| **Drumbox** | 16-pad synthesized drum machine (MIDI notes 36-51), per-pad tune/decay/tone/level/pan, 9 kits. |
| **Leadline** | Monophonic lead synth: unison stack of 1-7 oscillators, sub, 24 dB low-pass with drive, glide/legato, fading vibrato, PWM, ping-pong delay. 14 presets. |
| **Padscape** | 8-voice pad synth: two detuned layers per voice, 24 dB low-pass moved by a slow LFO (different phase per voice), chorus, reverb. 14 presets. |
| **Chordbox** | Chord synth: one key plays a chord, either a chosen chord type (24 types) or the chord of that degree in a key and scale, with inversions, voicings, strum, chorus and delay. 28 presets. |

Each plugin has its own README with parameters and limits (`plugins/<Name>/README.md`).

## Status, honestly

- Tested on an **MPC One+ with MPC OS 3.10.1** (External Plugin Support on) and in **Ableton Live 12** on Windows (VST2).
- Desktop builds have **no custom UI**: the host shows its own parameter list. The touch pages exist only on the MPC.
- Sound quality was judged by ear on one setup; no other hosts or MPC models were tested.
- Presets are starting points of my own making.

## Build

You need Python 3 (the SDK downloads the Zig compiler on first use).

```
git clone https://github.com/inmusic-software/mpc-plugin-builder-sdk
git clone https://github.com/old-dov/audiofeel-mpc-plugins
cp -r audiofeel-mpc-plugins/plugins/* mpc-plugin-builder-sdk/plugins/     # Windows: copy the folders
cd mpc-plugin-builder-sdk

python tools/build.py plugins/Bassline                    # MPC folder, in plugins/Bassline/build/
python tools/build.py plugins/Bassline --target desktop   # VST2 for this computer
```

## Install

- **MPC**: copy the `AudioFeel - VST - <Name>` folder into a `VST2 Plugins` folder on a drive (USB stick), then on the MPC
  enable Preferences > Enhancements (Beta) > Enable Enhancements and External Plugin Support, and restart its app.
  Unsupported by Akai/inMusic: read the warning the MPC shows. (If a drive is mounted `noexec`, plugins may not load from it.)
- **Windows**: copy the `desktop/<Name>` folder (the `.dll` plus `plugin.json`) into your VST2 folder and rescan.

## Licence and credits

MIT, see `LICENSE`. Built on the MIT-licensed MPC Plugin SDK by inMusic Brands, Inc. Not affiliated with or endorsed by
inMusic, Akai Professional or Korg. "VST" is a trademark of Steinberg Media Technologies GmbH.

---

*FR : sept instruments gratuits pour MPC standalone (et VST2 Windows), 100 % synthèse. Voir les sections ci-dessus ; installation = dossier `VST2 Plugins` + Enhancements activé.*
