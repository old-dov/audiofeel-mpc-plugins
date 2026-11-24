# AudioFeel MPC plugins

Fourteen free instruments for Akai MPC standalone (and as VST2 on Windows), built with inMusic's open-source
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
| **Impactor** | Cinematic impacts, risers and downers: 16 synthesized sounds on pads (notes 36-51) plus a pitched keyboard zone, six Styles (Trailer, Horror, Sci-Fi, Industrial, Organic, Glitch) that change what the sounds are made of, velocity, Variation, big reverb. 12 programs. |
| **Warbox** | Cinematic percussion: taiko, war drums, timpani, anvil, plate, gong, military snare, rimshot, crash swell. 16 pads (notes 36-51), big reverb. 7 programs. |
| **Dronescape** | Dark drones and evolving textures, 6 voices: sub, detuned saws, an interval layer (fifth, tritone, minor second...), tonal noise, slow-moving filter and drift. 11 programs. |
| **Stringscape** | Synthesized strings, 8 voices: Violins to Basses, four articulations (legato, staccato, pizzicato, tremolo), bow noise, vibrato, dynamics on mod wheel/aftertouch, mono solo mode with glide. 13 programs. |
| **Brassline** | Synthesized brass and braams, 6 voices: Trumpet, Horn, Trombone, Tuba, Braam; swell, growl, scoop, tongue, dynamics, mono solo mode. 13 programs. |
| **Chorale** | Formant choir, 6 voices: vowel morph A-E-I-O-U, vowel motion, four singers, breath and onset, dynamics, solo mode with glide. 13 programs. |
| **Chimes** | FM bells and tuned percussion, 8 voices: bell, music box, celesta, glockenspiel, marimba, tubular bell, vibraphone, kalimba. 11 programs. |

Each plugin has its own README with parameters and limits (`plugins/<Name>/README.md`).

## Status, honestly

- The first seven were tested on an **MPC One+ with MPC OS 3.10.1** (External Plugin Support on) and in **Ableton Live 12** on Windows (VST2).
- The seven cinema instruments (Impactor, Warbox, Dronescape, Stringscape, Brassline, Chorale, Chimes) were judged by ear in **Ableton Live 12** (VST2) and tested on an **MPC One+ with MPC OS 3.10.1**, where they load and play.
- The cinema instruments imitate orchestral and cinematic sounds by synthesis; they are not sample libraries. Strings, brass and choir are best for pads, swells and underscore, less for exposed solo lines.
- The **Windows VST2 in the release zip (1.3.0 and later) has its own window**, one accent colour per plugin, drawn from the same pages as the MPC screen. It was tried in Ableton Live 12 on Arpeggio and driven in a test host for all fourteen; the other thirteen were not opened in a real DAW yet. A VST2 you build yourself with `build.py --target desktop` stays generic (the host shows its own parameter list); `windows-editor/` adds the window. The touch pages exist on the MPC.
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
- **Windows**: unzip the release's `windows-vst2` zip (or copy the `desktop/<Name>` folder you built: the `.dll` plus `plugin.json`) into your VST2 folder and rescan. 64-bit hosts only.

## Licence and credits

MIT, see `LICENSE`. Built on the MIT-licensed MPC Plugin SDK by inMusic Brands, Inc. Not affiliated with or endorsed by
inMusic, Akai Professional or Korg. "VST" is a trademark of Steinberg Media Technologies GmbH.

---

*FR : quatorze instruments gratuits pour MPC standalone (et VST2 Windows), 100 % synthèse. Voir les sections ci-dessus ; installation = dossier `VST2 Plugins` + Enhancements activé.*
