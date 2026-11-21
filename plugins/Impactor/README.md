# Impactor (AudioFeel)

Cinematic impacts, risers and downers for Akai MPC standalone and VST2 hosts. 16 synthesized sounds on notes 36-51 (the pads of an MPC or an Ableton Drum Rack), no samples, one big reverb over everything.

| Notes | Family | Sounds |
|---|---|---|
| 36-40 | Impacts | Boom, Hit, Slam, Metal Hit, Thud |
| 41-44 | Risers | Noise Riser, Tone Riser, Tension (dissonant cluster), Reverse Swell |
| 45-47 | Downers | Downer, Sub Drop, Whoosh |
| 48-51 | Other | Heartbeat, Rumble, Glass, Sting |

- **Style**: Trailer, Horror, Sci-Fi, Industrial, Organic, Glitch. Each style adds its own layer to the impacts (a beating sub pair and shimmer; a dissonant cluster; an FM zap; a clanging metal layer; a woody knock; a stutter gate) and colours every sound (ring modulation, hard clip with a metal comb, bit-crush and sample-rate reduction...), so the programs differ in material, not only in reverb.
- **Playing it**: velocity sets loudness, brightness, pitch and length; Variation makes every hit slightly different (pitch, tone, length); the mod wheel brightens and pushes the drive; aftertouch opens the reverb; keys above the pads (note 52 and up) play the sound chosen by **Keys Sound** (Boom, Hit, Thud, Sub Drop, Metal, Sting, Heartbeat), pitched by the key, 4 voices.
- **Per sound**: Tune (+-12 st), Length (25-250 %: the decay of an impact, the whole run-up of a riser), Tone (brightness), Level, Pan.
- **Master**: Volume, Drive, Vel Sens, Rev Mix / Size / Damp (Freeverb-style, long hall tail).

Programs: Init Cinema, Trailer Hits, Dark Drama (Organic), Horror Pack, Sci-Fi Sweeps, Short & Punchy (Industrial), Epic Hall, Industrial Plant, Cyber Zaps, Glitch Machine, Ghost Town, Dry Foley (Organic).

Not implemented: risers are not tempo-synced (their length is in seconds, scaled by Length), sounds are one-shots (note-off is ignored), no choke groups. Desktop builds have no custom UI; the MPC shows touch pages.
CPU: ~0.3 % of a desktop core per instance in the host simulator; not measured on the MPC.
