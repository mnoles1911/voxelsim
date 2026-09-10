# The OST, from first principles (2026-09-09): music for a survival and building game

Owner direction, 2026-09-09: "start over on the game OST from first principles ... all of
the scores [were] very fast bpm and we need more OST music that can reasonably be played as
almost background music as the player explores and does normal activities. Not every
moment is an epic or fast paced scene. The nature of the game has changed: this is no
longer an RPG linear story focused game, rather it is an open ended survival and building
game that will have long periods of solo play and downtime doing manual or evening minor
tasks to plant crops, chop trees, craft items."

The previous plan (59 cues for the story era) is archived at
`docs/music-prompts-rpg-era-2026-05-16.md`. This document replaces it. The system that
plays the music is `docs/music-design.md`; this document is the content, and both were
rewritten together so they agree. The prompts are still written for Suno.

## 1. What the music is for now

The player spends most of a session alone: walking a procedurally generated world, chopping
trees, planting and harvesting, mining, building, crafting at a bench, boating between
shores, sheltering from weather, and sitting out the evening at a camp or a house they
built. There is no story beat to score. Danger exists but is rare and short.

So the score is **background by design**. It should be possible to have it on for a
three-hour session without noticing when a cue starts or ends, and to notice, when it is
switched off, that the world feels emptier. The references are Jeremy Soule's exploration
and town cues for Oblivion and Skyrim at their quietest (not the combat or the anthems),
plus the calm end of survival and building games: Minecraft's piano and pad pieces,
Valheim's sparse acoustic themes, Stardew Valley's daytime and evening loops.

Four consequences, each of which reverses a rule of the old plan:

1. **Slow.** Every pool except Danger sits at 40 to 72 BPM, and most cues have no pulse a
   listener could tap. The old plan spread tempo 40 to 168 on purpose; this one does not.
2. **Long, and few.** Cues run 5 to 7 minutes with long tails, and the player leaves 60 to
   150 seconds of silence between them. Fewer, longer cues beat many short ones for a game
   that is played for hours.
3. **Flat.** No crescendo, no fanfare, no brass stab, no drum hit. Development is by
   variation, register and thinning, never by building to a peak. A cue may swell gently
   and settle, never arrive.
4. **One family of instruments.** The whole score is played by the same small band, so
   any two cues could be from the same evening: piano, harp, nylon-string guitar or lute,
   hammered dulcimer, solo cello, viola and violin, oboe, cor anglais, clarinet and bass
   clarinet, low whistle (constrained, see section 4), celesta and music box, soft string
   pads, and at most a soft frame drum. Two palette colours sit at the edge of the family
   and are allowed only where section 2 names them: a soft, sustained French horn
   (Highland and Dusk; never a stab) and bowed vibraphone or crotales (Marsh and
   Underground). No choir, no vocals, no war horns, no battery, no synth, no accordion.

## 2. The palettes: biomes, not factions

The old score gave each faction a sonic identity. There are no factions. The world does
have biomes, and a cue is coloured by where the player is likely to hear it. This is
flavour on top of the one instrument family, not a second band.

| palette | colour | typical lead |
|---|---|---|
| Meadow and plains | warm, open, major-leaning modes (Mixolydian, Lydian) | oboe, guitar, piano |
| Forest | shaded, held chords, small figures | harp, celesta, clarinet |
| Highland and snow | cold, sparse, long reverb | solo horn softly, cello harmonics, piano |
| Coast and water | rolling 6/8 or slow 4/4, open fifths | low whistle, cello, dulcimer |
| Marsh and fen | uneasy but calm, drones, bowed metal | bass clarinet, bowed vibraphone |
| Underground | near-ambient, stone reverb, no melody to follow | contrabass, crotales, clarinet |
| Hearth (your own place) | intimate, close-miked, domestic | guitar, piano, music box, dulcimer |

## 3. Pools, targets and what to keep from disk

The pools are the ones `docs/music-design.md` implements. Two changed with the game: the
Town pool is now **Hearth** (music for being at the place you built, doing chores), and
the Combat pool is now **Danger** (rare, restrained, no anthems). The folder names in
`Content/Audio/Music` follow the design doc; a rename of the two folders lands with the
signals that select them.

| pool | when | target | on disk and kept | new cues to write |
|---|---|---|---|---|
| Explore / Day | walking, gathering, 07:00-18:00 | 10 | 04 Open Road, 05 Greatwood, 06 Spine, 08 Ashfields, 09 Western Coast, 11 Sorrowmarsh, 12 Weeping Wood, Forest_Exploration (8; audition World Map _ Travel and Discovery_Wonder, keep if they pass section 1) | 2 |
| Explore / Dawn | 05:00-07:00 | 3 | none | 3 |
| Explore / Dusk | 18:00-20:00 | 3 | none | 3 |
| Explore / Night | 20:00-05:00 | 6 | Camp_Rest (borrowed) | 6 |
| Explore / Rain | while it rains, any hour | 3 | none | 3 |
| Hearth | at your base: building, farming, crafting, cooking, evenings | 6 | none | 6 |
| Cave | underground | 4 | 07 Underway, Cave_Dungeon | 2 |
| Water | aboard a boat | 3 | Sea _ Sailing | 2 |
| Danger | a threat is active | 3 | 38 Enemies Gathering Strength, Stealth_Tension | 2 (restrained, 80-100 BPM at most) |
| Menu | title and loading | 3 | 02 Main Title | 2 |
| Stingers | events, seconds long | backlog | Defeat _ Game Over | 0 (backlog) |

Total: 31 new cues. Forty-four in the pools when done.

**Retire to `Content/Audio/Music/Archive/`** (not scanned by the player, kept for
reference): the seven Town cues (13 Aldenholt, 16 Solgrade, 20 Brightwatch, Town_Village,
Castle_Court Interior, Taverns_Feast, 27 Deep Cups: tavern and market music for towns the
game no longer has), Boss Battle, Emotional_Loss, 49 The Vigil (lead vocal), 10 Copper
Isles (104 BPM fiddle, too quick for the rule), and `Main Theme.wav` (superseded). The
Lirien-Thal MP4 is retired with the Town set, so it no longer needs re-encoding.

## 4. The Suno method (kept from the old plan, it works)

- **Style box**, 350 to 500 characters. Lead with the lead instrument, the key and the
  BPM; Suno weights the first clause hardest. Name the reference lean once (Soule's
  Oblivion or Skyrim exploration; for Hearth, Valheim or Stardew calm). Always end with
  "full-length cue, developed and through-composed, no early fade, long outro" and a
  negative list.
- **Structure box**, 7 to 9 `[Instrumental ...]` sections. That is what gets 5 to 7
  minutes; a short structure renders a short track. Use Extend if a take still stops
  short.
- **Flute and whistle** are constrained to "simple steady stepwise lines in a fixed
  register" and every negative list bans glissando, pitch sweeps, portamento, octave
  runs and whistle effects. This language is what prevents the high-to-low sweep
  artefact. Do not remove it.
- **Negative list**, every cue: no vocals, no choir, no drum kit, no taiko, no brass
  stabs, no synth, no electric guitar, no EDM, no crescendo, no climax. Danger cues may
  keep a frame drum and low strings. Elsewhere the only percussion is a soft, even frame
  drum, and only where the table names it (H1); it drops out before the outro.
- **Keys** vary across the score and D minor stays reserved for the Main Theme. No two
  cues in one pool share the same key.
- **Render**: best take, stereo 48 kHz 16-bit PCM WAV, into the pool folder named in
  section 3. The filename is the on-screen track title; use the titles below.

## 5. Inventory of the 31 new cues

| # | pool | title | palette | key | BPM | lead | len |
|---|---|---|---|---|---|---|---|
| D1 | Explore/Day | Slow Fields | Meadow | G Mixolydian | 60 | nylon guitar + oboe | 6:00 |
| D2 | Explore/Day | Under Tall Pines | Forest | A Lydian | 54 | harp + clarinet | 6:00 |
| A1 | Explore/Dawn | First Light on the Meadow | Meadow | B♭ Lydian | 56 | solo oboe | 5:00 |
| A2 | Explore/Dawn | Mist Off the River | Coast | A♭ Mixolydian | 58 | low whistle (constrained) | 5:00 |
| A3 | Explore/Dawn | Frost Lifting | Highland | E Lydian | 50 | piano + cello harmonics | 5:00 |
| K1 | Explore/Dusk | Long Shadows Home | Meadow | G Aeolian | 56 | viola + soft horn | 5:30 |
| K2 | Explore/Dusk | Last Light on the Water | Coast | B Mixolydian | 52 | cello + hammered dulcimer | 5:30 |
| K3 | Explore/Dusk | The Woods Go Quiet | Forest | F Dorian | 50 | clarinet + harp | 5:30 |
| N1 | Explore/Night | Starfall | Meadow | E♭ Lydian | 46 | solo piano | 6:00 |
| N2 | Explore/Night | Moonrise Through Leaves | Forest | A♭ Aeolian | 44 | celesta + harp | 6:00 |
| N3 | Explore/Night | Cold Stars | Highland | F♯ Dorian | 48 | muted horn + string harmonics | 6:00 |
| N4 | Explore/Night | The Owl Hours | Marsh | C Aeolian drone | 42 | bass clarinet + bowed vibraphone | 6:00 |
| N5 | Explore/Night | Embers on the Road | Meadow | G Dorian | 50 | lute-guitar + viola | 6:00 |
| N6 | Explore/Night | Lantern in the Window | Meadow | C Lydian | 44 | cello harmonics + celesta | 6:00 |
| R1 | Explore/Rain | Rain on the Road | Meadow | C♯ Dorian | 50 | piano + harp | 6:00 |
| R2 | Explore/Rain | The Storm Passing | Highland | E♭ Dorian | 48 | cor anglais | 6:00 |
| R3 | Explore/Rain | Under the Eaves | Hearth | B♭ Dorian | 46 | guitar + soft pad | 6:00 |
| H1 | Hearth | Turned Earth | Hearth/Meadow | F Mixolydian | 62 | nylon guitar + frame drum (soft) | 6:00 |
| H2 | Hearth | Sawdust | Hearth | D Mixolydian | 58 | hammered dulcimer + piano | 6:00 |
| H3 | Hearth | The Bench by the Fire | Hearth | A Dorian | 54 | piano + cello | 6:30 |
| H4 | Hearth | Stone on Stone | Hearth/Highland | E♭ Mixolydian | 60 | dulcimer + viola | 6:00 |
| H5 | Hearth | Supper | Hearth | B♭ Ionian (major) | 56 | guitar + oboe | 6:00 |
| H6 | Hearth | The Day's Work Done | Hearth | F♯ Aeolian | 48 | music box + piano | 6:30 |
| C1 | Cave | Drip and Dark | Underground | no centre (A drone) | 40 | bowed crotales + contrabass | 7:00 |
| C2 | Cave | The Sleeping Stone | Underground | A♭ Phrygian | 46 | bass clarinet + cello | 6:30 |
| W1 | Water | Slack Water | Coast | B Dorian | 54 | viola + hammered dulcimer | 6:00 |
| W2 | Water | The Far Shore | Coast | E Mixolydian | 60 | low whistle (constrained) + harp | 6:00 |
| X1 | Danger | Something in the Trees | Forest | G Phrygian | 84 | low strings ostinato + frame drum | 4:00 |
| X2 | Danger | Held Breath | Marsh | B♭ Aeolian | 72 | cello + bowed metal + soft drum | 4:00 |
| M1 | Menu | Prelude, a Quiet Morning | Meadow | F Lydian | 52 | piano + harp | 5:00 |
| M2 | Menu | Prelude, After Dark | Highland | E Aeolian | 46 | cello + celesta | 5:00 |

Keys are unique within each pool. Danger is the only pool above 72 BPM and it still has
no brass and no battery; it is a raised pulse, not a battle.

## 6. The prompts

One entry per cue in the section 5 order: **title, key, BPM, lead, palette, length**, a
style box and a structure box. Written to the rules of section 4.

---

**Slow Fields**
*Key G Mixolydian · 60 BPM · Meadow · nylon guitar + oboe · instrumental · 6:00*

Style prompt:
```
Nylon-string guitar in G Mixolydian at 60 BPM, warm open meadow air, a quiet oboe answering over soft string pads. Flat dynamics, no pulse; variation and thinning. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax. No flute glissando, no pitch sweeps, no portamento, no octave runs, no whistle effects.
```
Structure prompt:
```
[Instrumental Intro - solo nylon guitar, slow G Mixolydian figure, open air, no pulse]
[Instrumental A - oboe enters above with a plain stepwise line in one register]
[Instrumental B - soft string pads underneath, the guitar figure moves down a register]
[Instrumental Development - the same figure varied, oboe answering later each time]
[Instrumental Drift - guitar alone, notes taken away, dynamics unchanged]
[Instrumental Hush - one held pad note, oboe silent, almost nothing]
[Instrumental Reprise - guitar and oboe return, no louder than the opening]
[Instrumental Outro - guitar last, one open chord, very long quiet tail]
```

---

**Under Tall Pines**
*Key A Lydian · 54 BPM · Forest · harp + clarinet · instrumental · 6:00*

Style prompt:
```
Harp in A Lydian at 54 BPM, small repeating figures under a low soft clarinet, held chords on string pads, celesta touching a note now and then. Shaded forest light, flat dynamics, no pulse; development by variation, register and thinning only. Soule's Skyrim exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - solo harp, a small A Lydian figure, long gaps]
[Instrumental A - low clarinet enters beneath, one slow line, shaded]
[Instrumental B - string pads hold a chord under both, celesta adds single notes]
[Instrumental Development - the harp figure varied and moved higher, clarinet resting]
[Instrumental Drift - pads and celesta only, the figure implied not played]
[Instrumental Hush - harp alone, three notes, very quiet]
[Instrumental Reprise - clarinet returns once at the opening level, no louder]
[Instrumental Outro - harp and one held pad chord, very long quiet tail]
```

---

**First Light on the Meadow**
*Key B♭ Lydian · 56 BPM · Meadow · solo oboe · instrumental · 5:00*

Style prompt:
```
Solo oboe in B♭ Lydian at 56 BPM over harp and soft string pads, warm and open, the first hour of daylight. Flat dynamics, no pulse; variation and thinning only. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax. No flute glissando, no pitch sweeps, no portamento, no octave runs, no whistle effects.
```
Structure prompt:
```
[Instrumental Intro - soft string pad alone, one B♭ Lydian chord opening]
[Instrumental A - solo oboe enters, a plain stepwise line in a fixed register]
[Instrumental B - harp underneath in slow open figures, the pad thinning]
[Instrumental Development - the oboe line varied, harp moving to a lower register]
[Instrumental Hush - harp alone, oboe silent, very quiet]
[Instrumental Reprise - oboe returns with the opening line, unchanged in level]
[Instrumental Outro - pad and one last oboe note, very long quiet tail]
```

---

**Mist Off the River**
*Key A♭ Mixolydian · 58 BPM · Coast · low whistle (constrained) · instrumental · 5:00*

Style prompt:
```
Low whistle in A♭ Mixolydian at 58 BPM, simple steady stepwise lines in a fixed register, over cello and open fifths on harp. Cool river mist, flat dynamics, no pulse. Soule's Skyrim exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax. No flute glissando, no pitch sweeps, no portamento, no octave runs, no whistle effects.
```
Structure prompt:
```
[Instrumental Intro - harp in slow open fifths, mist on water, no pulse]
[Instrumental A - low whistle enters, simple steady stepwise line, fixed register]
[Instrumental B - solo cello holds a low note beneath, harp thinning]
[Instrumental Development - the whistle line varied stepwise, still one register]
[Instrumental Hush - cello and harp only, whistle silent]
[Instrumental Reprise - whistle returns with the opening line, same level]
[Instrumental Outro - harp last over a held cello note, very long quiet tail]
```

---

**Frost Lifting**
*Key E Lydian · 50 BPM · Highland · piano + cello harmonics · instrumental · 5:00*

Style prompt:
```
Solo piano in E Lydian at 50 BPM, cold and sparse, with cello harmonics held far behind it and a thin string pad. Long mountain reverb, wide space between notes, flat dynamics, no pulse; the piece develops only by variation, register and thinning. Soule's Skyrim exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - solo piano, single E Lydian notes far apart, long reverb]
[Instrumental A - cello harmonics enter high and thin behind the piano]
[Instrumental B - a cold string pad underneath, the piano figure widening in register]
[Instrumental Development - the figure varied, fewer notes each time, no louder]
[Instrumental Hush - cello harmonics alone, piano silent]
[Instrumental Reprise - piano returns with the opening notes, unchanged in level]
[Instrumental Outro - one low piano note under a held harmonic, very long quiet tail]
```

---

**Long Shadows Home**
*Key G Aeolian · 56 BPM · Meadow · viola + soft horn · instrumental · 5:30*

Style prompt:
```
Solo viola in G Aeolian at 56 BPM, warm and low, with a soft distant horn answering once a phrase over string pads and nylon guitar. Late-afternoon meadow light, flat dynamics, no pulse; development by variation, register and thinning only. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - solo viola, one slow G Aeolian phrase, warm and low]
[Instrumental A - nylon guitar underneath in a plain repeating figure]
[Instrumental B - a soft distant horn answers the viola once, quietly, then rests]
[Instrumental Development - the viola phrase varied, guitar moving to a lower register]
[Instrumental Drift - string pad only, both leads resting]
[Instrumental Hush - viola alone, two notes, very quiet]
[Instrumental Reprise - the horn answers once more, no louder than before]
[Instrumental Outro - guitar and a held pad chord, very long quiet tail]
```

---

**Last Light on the Water**
*Key B Mixolydian · 52 BPM · Coast · cello + hammered dulcimer · instrumental · 5:30*

Style prompt:
```
Solo cello in B Mixolydian at 52 BPM over hammered dulcimer in rolling open fifths, harp beneath. Slow water, flat dynamics, no pulse to tap; development by variation, register and thinning only, never a peak. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - hammered dulcimer alone, rolling open fifths, slow water]
[Instrumental A - solo cello enters with one long B Mixolydian line]
[Instrumental B - harp joins beneath, dulcimer thinning to single strikes]
[Instrumental Development - the cello line varied and taken lower, dulcimer resting]
[Instrumental Drift - harp and dulcimer only, no melody, level unchanged]
[Instrumental Hush - cello alone on one held note]
[Instrumental Reprise - dulcimer returns with the opening figure, same level]
[Instrumental Outro - dulcimer last under a held cello note, very long quiet tail]
```

---

**The Woods Go Quiet**
*Key F Dorian · 50 BPM · Forest · clarinet + harp · instrumental · 5:30*

Style prompt:
```
Low clarinet in F Dorian at 50 BPM over harp figures and held string-pad chords, celesta touching one note now and then. Shaded, thinning light, flat dynamics, no pulse; the piece develops by variation, register and thinning only. Soule's Skyrim exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - harp alone, a small F Dorian figure, shaded]
[Instrumental A - low clarinet enters with one slow line, unhurried]
[Instrumental B - string pads hold beneath, celesta places single notes]
[Instrumental Development - the harp figure varied and thinned, clarinet lower]
[Instrumental Drift - celesta and pad only, the woods emptying]
[Instrumental Hush - harp alone, two notes, very quiet]
[Instrumental Reprise - clarinet returns with the opening line, no louder]
[Instrumental Outro - one held pad chord under a last harp note, very long quiet tail]
```

---

**Starfall**
*Key E♭ Lydian · 46 BPM · Meadow · solo piano · instrumental · 6:00*

Style prompt:
```
Solo piano in E♭ Lydian at 46 BPM, felt hammers, very slow, wide space between notes, a thin string pad far underneath. Open night sky over meadow. Flat dynamics, no pulse, no melody to follow; variation, register and thinning only. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - solo felt piano, single E♭ Lydian notes, long gaps]
[Instrumental A - a thin string pad appears far underneath, barely there]
[Instrumental B - the piano moves to a higher register, same few notes]
[Instrumental Development - the figure varied, notes taken away one by one]
[Instrumental Drift - pad alone holding, piano silent for a long while]
[Instrumental Hush - two piano notes, nothing else]
[Instrumental Reprise - the opening figure returns low, unchanged in level]
[Instrumental Outro - one last piano note over the pad, very long quiet tail]
```

---

**Moonrise Through Leaves**
*Key A♭ Aeolian · 44 BPM · Forest · celesta + harp · instrumental · 6:00*

Style prompt:
```
Celesta and harp in A♭ Aeolian at 44 BPM, small shaded figures passing between them, one held string-pad note underneath. Moonlight through branches. Flat dynamics, no pulse, nothing arrives; variation, register and thinning only. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - celesta alone, a small A♭ Aeolian figure, cool and shaded]
[Instrumental A - harp answers with the same figure a register lower]
[Instrumental B - a held string-pad note underneath, both leads thinning]
[Instrumental Development - the figure passed back and forth and varied, no louder]
[Instrumental Drift - pad only, the figure implied by memory]
[Instrumental Hush - celesta alone, three notes]
[Instrumental Reprise - harp returns with the opening figure, same level]
[Instrumental Outro - celesta last over the held pad note, very long quiet tail]
```

---

**Cold Stars**
*Key F♯ Dorian · 48 BPM · Highland · muted horn + string harmonics · instrumental · 6:00*

Style prompt:
```
A softly muted horn in F♯ Dorian at 48 BPM, one long phrase at a time, over string harmonics and a thin pad in cold mountain reverb. Sparse, very quiet, flat dynamics, no pulse; development by variation, register and thinning only, never a peak. Soule's Skyrim exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - string harmonics alone, cold air, long reverb]
[Instrumental A - a softly muted horn plays one long F♯ Dorian phrase, then rests]
[Instrumental B - thin pad underneath, harmonics moving up a register]
[Instrumental Development - the horn phrase varied and shortened, level unchanged]
[Instrumental Drift - harmonics and pad only, the horn silent]
[Instrumental Hush - one held harmonic, almost nothing]
[Instrumental Reprise - the horn phrase returns once, no louder than the first]
[Instrumental Outro - harmonics thinning over the pad, very long quiet tail]
```

---

**The Owl Hours**
*Key C Aeolian drone · 42 BPM · Marsh · bass clarinet + bowed vibraphone · instrumental · 6:00*

Style prompt:
```
Bass clarinet in C Aeolian at 42 BPM over a low C drone and bowed vibraphone, uneasy but calm, marsh air. Almost no melody, flat dynamics, no pulse; development by drift, register and thinning only, never a peak. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - a low C drone alone, still water, no pulse]
[Instrumental A - bass clarinet enters low and slow, few notes, uneasy but calm]
[Instrumental B - bowed vibraphone adds long shimmering tones above the drone]
[Instrumental Development - the clarinet line drifts and varies, drone unchanged]
[Instrumental Drift - drone and bowed metal only, clarinet resting]
[Instrumental Hush - the drone thins to one quiet note]
[Instrumental Reprise - bass clarinet returns with the opening notes, same level]
[Instrumental Outro - bowed vibraphone over the fading drone, very long quiet tail]
```

---

**Embers on the Road**
*Key G Dorian · 50 BPM · Meadow · lute-guitar + viola · instrumental · 6:00*

Style prompt:
```
Lute-like nylon guitar in G Dorian at 50 BPM, plain repeating figure, with a warm solo viola answering and a faint string pad. Night road, banked fire, flat dynamics, no pulse; development by variation, register and thinning only. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - lute-like nylon guitar alone, a plain G Dorian figure]
[Instrumental A - solo viola answers warmly, one phrase, then rests]
[Instrumental B - a faint string pad underneath, guitar figure moving lower]
[Instrumental Development - the figure varied, viola entering later each time]
[Instrumental Drift - guitar only, notes taken away, level unchanged]
[Instrumental Hush - one held viola note, guitar silent]
[Instrumental Reprise - guitar and viola together at the opening level]
[Instrumental Outro - guitar last, one open chord, very long quiet tail]
```

---

**Lantern in the Window**
*Key C Lydian · 44 BPM · Meadow · cello harmonics + celesta · instrumental · 6:00*

Style prompt:
```
Cello harmonics in C Lydian at 44 BPM, high and thin, with celesta placing single notes and a soft pad beneath. One warm light in an open dark field. Flat dynamics, no pulse; variation, register and thinning only. Soule's Skyrim exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - cello harmonics alone, high and thin, C Lydian]
[Instrumental A - celesta places single notes between them, warm and small]
[Instrumental B - a soft pad beneath, harmonics moving down a register]
[Instrumental Development - the celesta figure varied, harmonics holding longer]
[Instrumental Drift - pad and one harmonic, celesta silent]
[Instrumental Hush - celesta alone, two notes]
[Instrumental Reprise - harmonics return with the opening shape, no louder]
[Instrumental Outro - one celesta note over the held pad, very long quiet tail]
```

---

**Rain on the Road**
*Key C♯ Dorian · 50 BPM · Meadow · piano + harp · instrumental · 6:00*

Style prompt:
```
Solo piano in C♯ Dorian at 50 BPM with harp answering in the gaps and a soft string pad behind, the patter of rain implied by the figures, never by percussion. Flat dynamics, no pulse; variation, register and thinning only. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - solo piano, an even C♯ Dorian figure, rain implied not played]
[Instrumental A - harp answers in the gaps, the same figure displaced]
[Instrumental B - a soft string pad behind both, piano moving lower]
[Instrumental Development - the figure varied, harp thinning to single notes]
[Instrumental Drift - harp and pad only, piano resting]
[Instrumental Hush - two piano notes, very quiet]
[Instrumental Reprise - the opening figure returns, unchanged in level]
[Instrumental Outro - harp last over a held pad chord, very long quiet tail]
```

---

**The Storm Passing**
*Key E♭ Dorian · 48 BPM · Highland · cor anglais · instrumental · 6:00*

Style prompt:
```
Cor anglais in E♭ Dorian at 48 BPM, cold and sparse over long-reverb string pads and a distant piano. Weather moving away, flat dynamics, no pulse, nothing arrives. Soule's Skyrim exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax. No flute glissando, no pitch sweeps, no portamento, no octave runs, no whistle effects.
```
Structure prompt:
```
[Instrumental Intro - long-reverb string pad alone, cold and wide, weather leaving]
[Instrumental A - cor anglais enters with one plain E♭ Dorian line, fixed register]
[Instrumental B - a distant piano places single notes under it, pad thinning]
[Instrumental Development - the line varied and taken lower, level unchanged]
[Instrumental Drift - piano and pad only, cor anglais silent]
[Instrumental Hush - one held pad note, almost nothing]
[Instrumental Reprise - cor anglais returns with the opening line, no louder]
[Instrumental Outro - piano last under the fading pad, very long quiet tail]
```

---

**Under the Eaves**
*Key B♭ Dorian · 46 BPM · Hearth · guitar + soft pad · instrumental · 6:00*

Style prompt:
```
Nylon guitar in B♭ Dorian at 46 BPM, close-miked and domestic, over a soft string pad, rain outside a shelter you built. Flat dynamics, no pulse, nothing arrives; variation, register and thinning only. Soule's Skyrim calm with Valheim and Stardew warmth. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - close-miked nylon guitar alone, a slow B♭ Dorian figure]
[Instrumental A - a soft string pad settles underneath, warm and near]
[Instrumental B - the guitar figure moves lower, fingerboard noise left in]
[Instrumental Development - the figure varied, quieter, rain outside implied]
[Instrumental Drift - pad only, guitar resting for a long while]
[Instrumental Hush - three guitar notes, very close and small]
[Instrumental Reprise - the opening figure returns, unchanged in level]
[Instrumental Outro - one held guitar chord over the pad, very long quiet tail]
```

---

**Turned Earth**
*Key F Mixolydian · 62 BPM · Hearth/Meadow · nylon guitar + frame drum (soft) · instrumental · 6:00*

Style prompt:
```
Nylon guitar in F Mixolydian at 62 BPM, close-miked and warm, with a very soft frame drum keeping an easy hand pulse and a little dulcimer. Planting weather, company not scenery. Flat dynamics, no build; variation, register and thinning only. Soule's Skyrim calm with Valheim and Stardew warmth. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - close-miked nylon guitar alone, an easy F Mixolydian figure]
[Instrumental A - a very soft frame drum joins with a light hand pulse, warm room]
[Instrumental B - hammered dulcimer answers the guitar in a higher register]
[Instrumental Development - the figure varied, the drum staying soft and even]
[Instrumental Drift - guitar and drum only, dulcimer resting, level unchanged]
[Instrumental Hush - guitar alone, the drum stops, a few notes]
[Instrumental Reprise - drum and dulcimer return at the opening level, no louder]
[Instrumental Outro - guitar last, the drum gone, one open chord, very long quiet tail]
```

---

**Sawdust**
*Key D Mixolydian · 58 BPM · Hearth · hammered dulcimer + piano · instrumental · 6:00*

Style prompt:
```
Hammered dulcimer in D Mixolydian at 58 BPM, close-miked, with piano answering underneath and a quiet nylon guitar. Workbench company: unhurried, domestic, warm. Flat dynamics, no build; variation, register and thinning only. Soule's Skyrim calm with Valheim and Stardew warmth. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - close-miked hammered dulcimer alone, a plain D Mixolydian figure]
[Instrumental A - piano answers underneath, soft-pedalled and near]
[Instrumental B - a quiet nylon guitar joins, the three trading the same figure]
[Instrumental Development - the figure varied and taken lower, no louder]
[Instrumental Drift - piano only, dulcimer and guitar resting]
[Instrumental Hush - dulcimer alone, single strikes, very quiet]
[Instrumental Reprise - all three return at the opening level, unhurried]
[Instrumental Outro - piano last under one held dulcimer note, very long quiet tail]
```

---

**The Bench by the Fire**
*Key A Dorian · 54 BPM · Hearth · piano + cello · instrumental · 6:30*

Style prompt:
```
Solo piano in A Dorian at 54 BPM, close and soft-pedalled, with a warm solo cello answering a phrase at a time. Firelight, an evening at the place you built; company, not scenery. Flat dynamics, no build; variation, register and thinning only. Soule's Skyrim calm with Valheim and Stardew warmth. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - close piano alone, a slow A Dorian phrase, soft pedal]
[Instrumental A - warm solo cello answers the phrase, near and unhurried]
[Instrumental B - the two settle into an easy exchange, firelight, no pulse]
[Instrumental Development - the phrase varied, piano moving to a lower register]
[Instrumental Drift - cello holds one long note, piano resting]
[Instrumental Hush - piano alone, three notes, very quiet]
[Instrumental C - a nylon guitar joins quietly for one pass, then leaves]
[Instrumental Reprise - piano and cello return at the opening level, no louder]
[Instrumental Outro - cello last under one held piano chord, very long quiet tail]
```

---

**Stone on Stone**
*Key E♭ Mixolydian · 60 BPM · Hearth/Highland · dulcimer + viola · instrumental · 6:00*

Style prompt:
```
Hammered dulcimer in E♭ Mixolydian at 60 BPM, close-miked, with solo viola and a cold thin pad behind it. Building in stone on high ground, patient and domestic. Flat dynamics, no build; variation, register and thinning only. Soule's Skyrim calm with Valheim and Stardew warmth. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - close hammered dulcimer alone, a patient E♭ Mixolydian figure]
[Instrumental A - solo viola enters warm and low, one phrase at a time]
[Instrumental B - a cold thin pad behind both, high-ground air, still domestic]
[Instrumental Development - the dulcimer figure varied, viola holding longer notes]
[Instrumental Drift - viola and pad only, dulcimer resting]
[Instrumental Hush - single dulcimer strikes, far apart]
[Instrumental Reprise - viola returns with the opening phrase, no louder]
[Instrumental Outro - dulcimer last over the fading pad, very long quiet tail]
```

---

**Supper**
*Key B♭ Ionian (major) · 56 BPM · Hearth · guitar + oboe · instrumental · 6:00*

Style prompt:
```
Nylon guitar in B♭ Ionian at 56 BPM with a soft oboe, close-miked and domestic, a shared table. Warm room, little reverb. Flat, unhurried, no pulse. Soule's Skyrim calm with Valheim and Stardew warmth. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax. No flute glissando, no pitch sweeps, no portamento, no octave runs, no whistle effects.
```
Structure prompt:
```
[Instrumental Intro - close nylon guitar alone, a warm B♭ major figure, small room]
[Instrumental A - soft oboe enters with a plain line in a fixed register]
[Instrumental B - celesta places a few notes above, the table settling]
[Instrumental Development - the guitar figure varied, oboe answering later]
[Instrumental Drift - guitar only, quieter, fewer notes]
[Instrumental Hush - one held oboe note over a single guitar chord]
[Instrumental Reprise - guitar and oboe together at the opening level, unhurried]
[Instrumental Outro - guitar last, one warm chord, very long quiet tail]
```

---

**The Day's Work Done**
*Key F♯ Aeolian · 48 BPM · Hearth · music box + piano · instrumental · 6:30*

Style prompt:
```
Music box in F♯ Aeolian at 48 BPM, tiny and close, with soft piano beneath and one held string-pad note. The end of an evening at your own place; intimate, unhurried, almost still. Flat dynamics, no build; variation and thinning only. Soule's Skyrim calm with Valheim and Stardew warmth. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - music box alone, a small turning F♯ Aeolian figure, very close]
[Instrumental A - soft piano beneath, answering the same figure slowly]
[Instrumental B - one held string-pad note underneath, the room settling]
[Instrumental Development - the figure simplified, fewer notes, no louder]
[Instrumental Drift - music box winding down, almost stopping]
[Instrumental Hush - piano alone, two notes, very quiet]
[Instrumental C - the music box returns once, barely there]
[Instrumental Reprise - piano and music box together at the opening level]
[Instrumental Outro - one last music-box note over the held pad, very long quiet tail]
```

---

**Drip and Dark**
*Key no centre (A drone) · 40 BPM · Underground · bowed crotales + contrabass · instrumental · 7:00*

Style prompt:
```
Bowed crotales and a low contrabass drone on A at 40 BPM, no tonal centre, no melody to follow, near-ambient stone. Long cavern reverb, single notes far apart, flat dynamics, no pulse; development by drift and thinning only. Morrowind cave ambience, Soule at his quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - a low contrabass drone on A alone, long cavern reverb]
[Instrumental A - bowed crotales enter far above, single tones, no melody]
[Instrumental B - the drone thickens by one note, stone space widening]
[Instrumental Development - crotales drift to new pitches, no centre, no pulse]
[Instrumental Drift - drone only, very long, almost unchanging]
[Instrumental Hush - one crotale tone, then silence around it]
[Instrumental C - a clarinet holds one low note far off, then goes]
[Instrumental Reprise - crotales and drone as at the opening, same level]
[Instrumental Outro - the drone thinning to nothing, very long quiet tail]
```

---

**The Sleeping Stone**
*Key A♭ Phrygian · 46 BPM · Underground · bass clarinet + cello · instrumental · 6:30*

Style prompt:
```
Bass clarinet in A♭ Phrygian at 46 BPM over a low sustained cello, near-ambient, stone reverb, no melody to follow. Very dark, very slow, flat dynamics, no pulse; development by drift, register and thinning only. Morrowind cave ambience, Soule at his quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - low sustained cello alone, stone reverb, no pulse]
[Instrumental A - bass clarinet enters beneath it, a few dark A♭ Phrygian notes]
[Instrumental B - the cello moves down a step, the space feeling larger]
[Instrumental Development - the clarinet notes drift and thin, level unchanged]
[Instrumental Drift - cello only, holding, almost nothing happening]
[Instrumental Hush - one bass clarinet note, alone in the reverb]
[Instrumental C - bowed crotales shimmer once far above, then gone]
[Instrumental Reprise - clarinet and cello as at the opening, no louder]
[Instrumental Outro - the cello note fading under the stone, very long quiet tail]
```

---

**Slack Water**
*Key B Dorian · 54 BPM · Coast · viola + hammered dulcimer · instrumental · 6:00*

Style prompt:
```
Solo viola in B Dorian at 54 BPM, long-bowed and unhurried, over a hammered dulcimer and harp in slow rolling open fifths. Still water under a boat, flat dynamics, no pulse; development by variation, register and thinning only. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - harp in slow rolling open fifths, still water, no pulse]
[Instrumental A - solo viola enters, long-bowed, one slow B Dorian line]
[Instrumental B - hammered dulcimer takes the rolling fifths, harp thinning to single notes]
[Instrumental Development - the viola line varied and taken lower, no louder]
[Instrumental Drift - harp and dulcimer only, viola resting]
[Instrumental Hush - one held viola note, almost nothing]
[Instrumental Reprise - viola returns with the opening line, same level]
[Instrumental Outro - harp last over the fading dulcimer, very long quiet tail]
```

---

**The Far Shore**
*Key E Mixolydian · 60 BPM · Coast · low whistle (constrained) + harp · instrumental · 6:00*

Style prompt:
```
Low whistle in E Mixolydian at 60 BPM, simple steady stepwise lines in a fixed register, over harp in rolling open fifths and a low cello. Flat dynamics, slow 6/8. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax. No flute glissando, no pitch sweeps, no portamento, no octave runs, no whistle effects.
```
Structure prompt:
```
[Instrumental Intro - harp alone in rolling open fifths, slow 6/8, open water]
[Instrumental A - low whistle enters, simple steady stepwise line, fixed register]
[Instrumental B - a low cello holds underneath, harp widening into fifths]
[Instrumental Development - the whistle line varied stepwise, same register throughout]
[Instrumental Drift - harp and cello only, whistle silent, level unchanged]
[Instrumental Hush - one held cello note under a single harp figure]
[Instrumental Reprise - whistle returns with the opening line, no louder]
[Instrumental Outro - harp last over the fading cello, very long quiet tail]
```

---

**Something in the Trees**
*Key G Phrygian · 84 BPM · Forest · low strings ostinato + frame drum · instrumental · 4:00*

Style prompt:
```
Low strings ostinato in G Phrygian at 84 BPM under a soft frame drum, shaded forest, watchful. A raised pulse, not a battle: restrained, flat dynamics, no build, no arrival, development by variation and thinning only. Soule's Oblivion at its quietest, never his combat. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - low strings ostinato alone in G Phrygian, quiet and even]
[Instrumental A - a soft frame drum joins, restrained, no accents]
[Instrumental B - a viola holds one shaded note above the ostinato]
[Instrumental Development - the ostinato varied and thinned, dynamics unchanged]
[Instrumental Hush - drum stops, low strings alone, very quiet]
[Instrumental Outro - the ostinato slowing and thinning to one note, long quiet tail]
```

---

**Held Breath**
*Key B♭ Aeolian · 72 BPM · Marsh · cello + bowed metal + soft drum · instrumental · 4:00*

Style prompt:
```
Solo cello in B♭ Aeolian at 72 BPM over bowed metal and a very soft frame drum, marsh drone underneath, uneasy but calm. A raised pulse, not a battle: restrained throughout, flat dynamics, no build, no arrival. Soule's Oblivion at its quietest, never his combat. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - a low marsh drone with bowed metal shimmering over it]
[Instrumental A - solo cello enters low in B♭ Aeolian, few notes, held back]
[Instrumental B - a very soft frame drum keeps an even pulse, no accents]
[Instrumental Development - the cello line varied and thinned, level unchanged]
[Instrumental Hush - drum stops, bowed metal alone over the drone]
[Instrumental Outro - one held cello note over the fading drone, long quiet tail]
```

---

**Prelude, a Quiet Morning**
*Key F Lydian · 52 BPM · Meadow · piano + harp · instrumental · 5:00*

Style prompt:
```
Solo piano in F Lydian at 52 BPM with harp answering and a thin string pad, warm and open, a title screen that asks nothing. Flat dynamics, no pulse, no fanfare; development by variation, register and thinning only. Soule's Oblivion exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - solo piano, one open F Lydian phrase, warm morning air]
[Instrumental A - harp answers the phrase, a thin string pad underneath]
[Instrumental B - the piano moves higher, the harp figure widening]
[Instrumental Development - the phrase varied, no fanfare, level unchanged]
[Instrumental Hush - harp alone, three notes, very quiet]
[Instrumental Reprise - the opening phrase returns on piano, no louder]
[Instrumental Outro - one held pad chord under a last piano note, very long quiet tail]
```

---

**Prelude, After Dark**
*Key E Aeolian · 46 BPM · Highland · cello + celesta · instrumental · 5:00*

Style prompt:
```
Solo cello in E Aeolian at 46 BPM, low and warm, with celesta placing single notes above and long cold reverb around both. A title screen at night; flat dynamics, no pulse, no fanfare; variation, register and thinning only. Soule's Skyrim exploration at its quietest. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no taiko, no brass stabs, no synth, no electric guitar, no EDM, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Intro - solo cello alone, one low E Aeolian phrase, cold reverb]
[Instrumental A - celesta places single notes above, far apart]
[Instrumental B - a thin string pad behind both, the space widening]
[Instrumental Development - the cello phrase varied and taken lower, no fanfare]
[Instrumental Hush - celesta alone, two notes, almost nothing]
[Instrumental Reprise - the cello phrase returns as at the opening, no louder]
[Instrumental Outro - one held cello note under a last celesta note, very long quiet tail]
```
