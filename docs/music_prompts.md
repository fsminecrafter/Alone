# Music prompts (MusicGPT)

The game has two kinds of music:

- **Exploration**: your tracks in `data/assets/audio/` ("Acute loneliness",
  "FakeCops", "LapsedGuardian", "Notalone", "Scarred forests"). They play in a
  random order with 20–90 s of silence between them.
- **Danger**: tracks in `data/assets/audio/danger/`. They fade in within about
  a second when something is hunting you, loop while it lasts, and fade out
  about ten seconds after you are safe. Today that is a charging boar or
  moose. Later it will be other survivors hunting you.

The danger music should feel like it belongs with the exploration tracks:
the same cold, lonely Swedish autumn, only now something is after you. It has
to **loop cleanly**, start strong (it only plays once the danger has started),
and have **no vocals**.

## Main prompt: "Hunted"

Paste this into MusicGPT:

```
Dark, tense instrumental survival-horror music for being hunted in a cold, empty Swedish forest town. 100 BPM, D minor. Pulsing low synth bass, heavy heartbeat kick, tight stuttering string ostinato, metallic scrapes, distant detuned drones, sharp taiko hits. Rising dread, no melody resolution, no vocals. Seamless loop, 2 minutes.
```

## Variations

Make two or three of these so it does not get repetitive. All of them go in
`data/assets/audio/danger/`; the game cycles through them.

**Stalked (slower, for when the threat is close but not on you yet)**

```
Minimal, creeping dark ambient for being stalked through a foggy Nordic spruce forest at dusk. 70 BPM, low cello drones, muffled heartbeat, sparse prepared piano notes, breath-like noise swells, distant metallic clanks, sub-bass pulses. Tense and lonely, no vocals, no drums kit. Seamless loop, 2 minutes.
```

**Chase (fast, for an open attack)**

```
Aggressive cinematic chase music, survival horror, cold northern atmosphere. 140 BPM, driving distorted synth bass ostinato, pounding toms and taiko, frantic staccato violins, harsh industrial percussion, alarm-like brass stabs, panicked rhythm. Relentless, no melody, no vocals. Seamless loop, 90 seconds.
```

**Lost survivors (for when the "others" arrive later)**

```
Paranoid electronic thriller score for being hunted by other people in an abandoned Swedish town at night. 110 BPM, ticking hi-hat clock pulse, gritty analog bass, glitching radio static, tense string swells, low male choir hum without words, sudden silences. Cold, human, threatening. No lyrics. Seamless loop, 2 minutes.
```

## Tips

- If MusicGPT adds a fade-out at the end, cut it off. The track loops, so a
  fade makes a gap.
- Keep the volume close to your exploration tracks. The game uses the same
  music volume slider for both.
- Export as `.ogg` or `.mp3`. Short names without spaces are fine, e.g.
  `danger_hunted.ogg`.
