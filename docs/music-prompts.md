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
4. **One family of instruments, never the same band twice.** The score draws on one
   acoustic family: piano, harp, nylon-string guitar or lute, hammered dulcimer, solo
   cello, viola and violin, string quartet, oboe, cor anglais, clarinet and bass clarinet,
   low whistle (constrained, see section 4), celesta and music box, soft string pads, a
   soft frame drum, plus two palette colours where section 2 names them (a soft sustained
   French horn in Highland and Dusk, bowed vibraphone or crotales in Marsh and
   Underground). No choir, no vocals, no war horns, no battery, no synth, no accordion.
   But a family is not a lineup: **no two cues in a pool share a lead instrument, an
   ensemble, or an opening gesture**, and every cue names one concrete genre or style
   idiom of its own. The first pass of this plan (2026-09-09) broke that and Suno
   rendered thirty-one cues that sounded like one. Section 5 carries the levers.

## 1b. Distinctness, the lesson of the first render

Suno keys on the first clause of the style box, on genre words, and on the shape of the
lyrics box. Thirty-one prompts that all opened "solo X in KEY at N BPM over held strings",
all cited Soule, all ended with the same negative list and all used the same seven section
names came back as one piece with the instruments swapped. Background music still has to
be thirty-one different pieces. So every cue now sets six levers, and section 5 lists
them per cue; the rules are hard:

1. **Genre idiom**, one concrete tag per cue, used at most three times across the score:
   Celtic slow air, English pastoral strings, chamber folk, minimalist piano, Nordic
   folk, Renaissance lute, Appalachian dulcimer, neoclassical strings, ambient drone,
   music-box lullaby, baroque chorale, Satie-like piano, post-rock ambient (acoustic),
   Breton harp, sea shanty slowed to a lament, and so on. A composer name may follow the
   tag once ("in the manner of Vaughan Williams"); "Soule" appears in at most a third of
   the score.
2. **Ensemble**, from solo to quartet, and the ensemble is named as a whole: solo piano;
   guitar duo; harp and clarinet; string quartet; dulcimer with a drone; cello and piano;
   woodwind trio; plucked consort; bowed metal over contrabass.
3. **Meter and motion**: 3/4, 4/4, 6/8, 5/4, 7/8 slowly, or free; and whether the piece
   is sustained (bowed, held) or in motion (plucked ostinato, arpeggio, walking figure).
   Slow does not mean static; half the score should have a gentle pulse.
4. **Opening gesture**, different for every cue in a pool: begins mid-texture; begins on a
   single repeated note; begins with the bass alone; begins with a chord and silence;
   begins with an ostinato already running; begins with two instruments in unison; begins
   with a rising scale; begins on a drone that only later becomes a chord.
5. **Structural shape**, one of: theme and variations; ground bass or passacaglia;
   two-part with a contrasting middle; rondo (the opening idea returns between episodes);
   slow chorale; ostinato piece that changes harmony under a fixed figure; call and
   response between two instruments; ambient drift with no theme; arch (thin, fuller, thin)
   without a climax. Section boxes are written to the shape, so no two cues in a pool
   share a section list.
6. **Space**: close and dry; small wooden room; open field; stone hall; cathedral tail;
   cave; "inside the music box".

The negative list stays, but it is placed last and kept short, and the phrase "no
crescendo, no climax" is the only wording every cue repeats.

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

## 5. Inventory of the 31 new cues, with their levers

Pool, title, key, BPM and length are fixed; the six levers of section 1b are fixed too,
so that no two cues in a pool share a lead, an ensemble, an idiom or an opening.

| # | pool | title | key | BPM | meter | idiom | ensemble (lead first) | opening | shape | space |
|---|---|---|---|---|---|---|---|---|---|---|
| D1 | Explore/Day | Slow Fields | G Mixolydian | 66 | 3/4 | English pastoral strings (Vaughan Williams) | string quartet, oboe over it | begins mid-texture, quartet already moving | two-part with a contrasting middle | open field |
| D2 | Explore/Day | Under Tall Pines | A Lydian | 54 | 4/4 free | Breton harp | harp and clarinet | harp alone, one arpeggio repeated | rondo | small wooden room |
| A1 | Explore/Dawn | First Light on the Meadow | B♭ Lydian | 58 | 6/8 | Celtic slow air | solo oboe, then guitar | single repeated note, then the air | theme and variations | open field |
| A2 | Explore/Dawn | Mist Off the River | A♭ Mixolydian | 60 | 4/4 | Nordic folk (Valheim-like) | low whistle (constrained), dulcimer, cello | bass alone (cello), the whistle after | ground bass | open field, water |
| A3 | Explore/Dawn | Frost Lifting | E Lydian | 48 | free | minimalist piano (Einaudi-like) | solo piano, cello harmonics late | a chord and silence | arch without climax | stone hall |
| K1 | Explore/Dusk | Long Shadows Home | G Aeolian | 56 | 3/4 | chamber folk | viola, guitar, soft horn late | two instruments in unison | call and response | small wooden room |
| K2 | Explore/Dusk | Last Light on the Water | B Mixolydian | 52 | 6/8 | Appalachian dulcimer | hammered dulcimer, cello, harp | ostinato already running | ostinato with changing harmony | open field, water |
| K3 | Explore/Dusk | The Woods Go Quiet | F Dorian | 50 | 5/4 | neoclassical strings (Arnalds-like) | string quartet, clarinet | rising scale in the cello | slow chorale | cathedral tail |
| N1 | Explore/Night | Starfall | E♭ Lydian | 44 | free | Satie-like piano | solo piano | a chord and silence | theme and variations | close and dry |
| N2 | Explore/Night | Moonrise Through Leaves | A♭ Aeolian | 46 | 4/4 | music-box lullaby | celesta, harp, bass clarinet once | drone that becomes a chord | ambient drift | inside the music box |
| N3 | Explore/Night | Cold Stars | F♯ Dorian | 48 | 4/4 | Nordic folk, hymn-like | soft horn, string harmonics, piano | bass alone (piano low octave) | slow chorale | stone hall |
| N4 | Explore/Night | The Owl Hours | C Aeolian | 42 | free | ambient drone | bass clarinet, bowed vibraphone, contrabass | drone that becomes a chord | ambient drift | cave |
| N5 | Explore/Night | Embers on the Road | G Dorian | 52 | 6/8 | Renaissance lute | lute, viola | lute alone, one figure repeated | ground bass | small wooden room |
| N6 | Explore/Night | Lantern in the Window | C Lydian | 40 | 3/4 | post-rock ambient, acoustic | cello harmonics, celesta, soft pad | begins mid-texture, pad already sounding | arch without climax | cathedral tail |
| R1 | Explore/Rain | Rain on the Road | C♯ Dorian | 54 | 4/4 | minimalist piano (Nils Frahm-like) | piano, harp, viola | ostinato already running | ostinato with changing harmony | close and dry |
| R2 | Explore/Rain | The Storm Passing | E♭ Dorian | 46 | free | English pastoral, wind-led | cor anglais, string quartet, distant timpani roll | rising scale in the cor anglais | two-part with a contrasting middle | open field |
| R3 | Explore/Rain | Under the Eaves | B♭ Dorian | 50 | 3/4 | chamber folk, guitar duo | two nylon guitars | two instruments in unison | rondo | small wooden room |
| H1 | Hearth | Turned Earth | F Mixolydian | 68 | 4/4 | Appalachian dulcimer with guitar | dulcimer, guitar, soft frame drum | ostinato already running | ostinato with changing harmony | close and dry |
| H2 | Hearth | Sawdust | D Mixolydian | 62 | 6/8 | chamber folk, fiddle-led (slow) | violin, guitar, cello | violin alone, one phrase repeated | theme and variations | small wooden room |
| H3 | Hearth | The Bench by the Fire | A Dorian | 54 | 4/4 | minimalist piano with cello | piano, cello | piano bass alone | call and response | close and dry |
| H4 | Hearth | Stone on Stone | E♭ Mixolydian | 60 | 5/4 | Nordic folk, plucked | plucked consort: lute, harp, dulcimer | begins mid-texture | rondo | stone hall |
| H5 | Hearth | Supper | B♭ major | 58 | 3/4 | Stardew-like waltz, acoustic | guitar, oboe, clarinet | a chord and silence, then the waltz | two-part with a contrasting middle | small wooden room |
| H6 | Hearth | The Day's Work Done | F♯ Aeolian | 44 | free | music-box lullaby with piano | music box, piano | music box alone, one turn of the figure | ambient drift | inside the music box |
| C1 | Cave | Drip and Dark | A drone | 40 | free | ambient drone, bowed metal | bowed crotales, contrabass, harp harmonics | drone that becomes a chord | ambient drift | cave |
| C2 | Cave | The Sleeping Stone | A♭ Phrygian | 46 | 4/4 | baroque chorale, low | bass clarinet, cello, viola | bass alone | slow chorale | cave |
| W1 | Water | Slack Water | B Dorian | 54 | 6/8 | sea shanty slowed to a lament | viola, dulcimer, harp | rising scale in the viola | ground bass | open field, water |
| W2 | Water | The Far Shore | E Mixolydian | 60 | 4/4 | Celtic slow air | low whistle (constrained), harp, cello | harp alone, one arpeggio repeated | theme and variations | open field, water |
| X1 | Danger | Something in the Trees | G Phrygian | 84 | 7/8 | neoclassical strings, tense | low strings ostinato, frame drum, clarinet | ostinato already running | ostinato with changing harmony | stone hall |
| X2 | Danger | Held Breath | B♭ Aeolian | 72 | 4/4 | ambient drone, tense | cello, bowed metal, soft drum | drone that becomes a chord | arch without climax | cave |
| M1 | Menu | Prelude, a Quiet Morning | F Lydian | 52 | 3/4 | baroque chorale, bright | piano, harp, oboe | two instruments in unison | slow chorale | cathedral tail |
| M2 | Menu | Prelude, After Dark | E Aeolian | 46 | free | neoclassical strings, nocturne | cello, celesta, string quartet | bass alone (cello) | theme and variations | stone hall |

Keys are unique within each pool; leads, ensembles, idioms and openings are unique within
each pool; tempos are spread 40-68 outside Danger. Danger keeps a pulse and stays under 90.

## 6. The prompts

One entry per cue in the section 5 order: **title, key, BPM, meter, idiom, ensemble,
length**, a style box and a structure box. Written to sections 1b and 4: the first clause
is the ensemble, idiom and key; the section list follows the cue's shape; the opening
gesture is the first section.

---

**Slow Fields**
*Key G Mixolydian · 66 BPM · 3/4 · English pastoral strings (Vaughan Williams) · string quartet, oboe over it · instrumental · 6:00*

Style prompt:
```
A string quartet with an oboe above it, English pastoral writing in G Mixolydian, 66 BPM in a slow 3/4 that turns. The bows are long; the oboe keeps simple steady stepwise lines in a fixed register. Open field, wide air. Flat dynamics; variation, register and thinning. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax. No glissando, no pitch sweeps, no portamento.
```
Structure prompt:
```
[Instrumental Part One - the quartet is already moving as the cue begins, a turning 3/4 in G Mixolydian, no introduction]
[Instrumental Part One Widens - the oboe enters above with a plain stepwise line, one register only]
[Instrumental Part One Lower - the quartet takes the same material down, violas leading]
[Instrumental Middle - a contrasting idea, quartet alone, longer bows, oboe resting]
[Instrumental Middle Thinned - two voices of the quartet only, the field very wide]
[Instrumental Part One Returns - the opening turn comes back at the opening level, oboe above]
[Instrumental Part One Thinned - notes taken away, the 3/4 still turning underneath]
[Instrumental Long Tail - one held quartet chord, oboe gone, very long quiet tail]
```

---

**Under Tall Pines**
*Key A Lydian · 54 BPM · 4/4 free · Breton harp · harp and clarinet · instrumental · 6:00*

Style prompt:
```
Harp and clarinet, the Breton harp idiom, A Lydian at 54 BPM in a loose 4/4 you could not tap: the arpeggio never stops and the clarinet holds long above it. Recorded in a small wooden room, close and woody. The shaded calm of Soule's Skyrim forests. Flat dynamics; development by variation, register and thinning. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Refrain - harp alone, one A Lydian arpeggio repeated, small wooden room]
[Instrumental Episode 1 - clarinet enters long and low above the arpeggio]
[Instrumental Refrain - the harp arpeggio returns unchanged, clarinet resting]
[Instrumental Episode 2 - the harmony under the arpeggio moves, clarinet answering]
[Instrumental Refrain - the arpeggio again, a register lower, same level]
[Instrumental Episode 3 - clarinet alone over a held harp chord, shaded]
[Instrumental Refrain Thinned - the arpeggio with notes missing, still turning]
[Instrumental Long Tail - one harp chord in the wooden room, very long quiet tail]
```

---

**First Light on the Meadow**
*Key B♭ Lydian · 58 BPM · 6/8 · Celtic slow air · solo oboe, then guitar · instrumental · 5:00*

Style prompt:
```
Over a nylon guitar, a solo oboe carries a Celtic slow air in B♭ Lydian, 58 BPM, a lilting 6/8 that rolls. The oboe keeps simple steady stepwise lines in a fixed register. Open field at first light. Flat dynamics; variation, register and thinning, never a build. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax. No glissando, no pitch sweeps, no portamento.
```
Structure prompt:
```
[Instrumental Theme - one oboe note repeated alone, then the slow air opens out in B♭ Lydian]
[Instrumental Variation 1 - nylon guitar joins beneath in a lilting 6/8, the air unchanged]
[Instrumental Variation 2 - the air ornamented lightly, still one register]
[Instrumental Variation 3 - guitar takes the air, the oboe holding one note]
[Instrumental Variation 4 - the air with notes removed, wide open field]
[Instrumental Theme Lower - the air a fifth down, guitar rolling under it]
[Instrumental Theme Returns - the air as at the opening, no louder]
[Instrumental Long Tail - guitar last, one open chord, very long quiet tail]
```

---

**Mist Off the River**
*Key A♭ Mixolydian · 60 BPM · 4/4 · Nordic folk (Valheim-like) · low whistle (constrained), dulcimer, cello · instrumental · 5:00*

Style prompt:
```
Low whistle, dulcimer and cello playing Nordic folk in A♭ Mixolydian, 60 BPM, a walking 4/4. The whistle keeps simple steady stepwise lines in a fixed register; the cello's ground never stops. Valheim's calm. Open field and water. Flat dynamics; variation and thinning. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax. No glissando, no pitch sweeps, no portamento.
```
Structure prompt:
```
[Instrumental Ground - the cello alone, a low A♭ Mixolydian ground, walking 4/4, mist on water]
[Instrumental Variation 1 - low whistle enters above, simple steady stepwise line, fixed register]
[Instrumental Variation 2 - hammered dulcimer fills between them, the ground unchanged]
[Instrumental Variation 3 - the whistle line varied stepwise, dulcimer thinning]
[Instrumental Variation 4 - dulcimer alone over the ground, whistle resting]
[Instrumental Variation 5 - whistle returns lower, the ground still walking]
[Instrumental Ground Bare - the cello's ground with nothing above it]
[Instrumental Long Tail - the ground slowing to one held note, very long quiet tail]
```

---

**Frost Lifting**
*Key E Lydian · 48 BPM · free · minimalist piano (Einaudi-like) · solo piano, cello harmonics late · instrumental · 5:00*

Style prompt:
```
Solo piano, with cello harmonics arriving late: minimalist writing in E Lydian, 48 BPM, unmetered, with no pulse to tap. Einaudi-like, plain and repeating, wide gaps between the notes. A stone hall with a long tail around every one of them. Flat dynamics; the piece fills in and empties out, never gets louder. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Thin - one E Lydian piano chord, then silence in the stone hall, unmetered]
[Instrumental Filling - a second chord, then a third, the gaps shortening]
[Instrumental Fuller - a plain repeating figure in the left hand, still quiet]
[Instrumental Fullest, Still Quiet - cello harmonics arrive high above the figure, no louder]
[Instrumental Emptying - the figure loses notes, the harmonics holding longer]
[Instrumental Thin Again - single chords with silence between, as at the opening]
[Instrumental Last Chord - one chord alone, a cello harmonic fading over it]
[Instrumental Long Tail - the stone hall's tail alone, very long and quiet]
```

---

**Long Shadows Home**
*Key G Aeolian · 56 BPM · 3/4 · chamber folk · viola, guitar, soft horn late · instrumental · 6:00*

Style prompt:
```
Chamber folk for viola and nylon guitar, with a soft sustained horn entering late: G Aeolian, 56 BPM, a slow 3/4 that leans into every bar. The viola's bows are long and the guitar answers rather than accompanies. A small wooden room, close, little reverb. Flat dynamics; variation, register and thinning. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Call - viola and guitar in unison, one leaning G Aeolian phrase in 3/4]
[Instrumental Response - the guitar answers alone, the same phrase shortened]
[Instrumental Call - viola alone, long bows, the phrase taken lower]
[Instrumental Response - a soft horn answers from further off, one phrase only, never a stab]
[Instrumental Call - guitar leads, the viola answering inside the phrase]
[Instrumental Response - horn and viola answer together, no louder than before]
[Instrumental Call Alone - viola with no answer at all, the room very close]
[Instrumental Long Tail - guitar last, one open chord, very long quiet tail]
```

---

**Last Light on the Water**
*Key B Mixolydian · 52 BPM · 6/8 · Appalachian dulcimer · hammered dulcimer, cello, harp · instrumental · 6:00*

Style prompt:
```
Hammered dulcimer leading cello and harp through Appalachian writing in B Mixolydian, 52 BPM, a rolling 6/8. The dulcimer figure never stops; the cello's bows are long underneath it. Open field and water at last light. Soule's Oblivion at its quietest. Flat dynamics; variation, register and thinning only. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Figure Alone - the dulcimer ostinato already running as the cue begins, rolling 6/8]
[Instrumental Harmony Shifts - the cello moves the chord beneath, the figure unchanged]
[Instrumental Harmony Shifts Again - harp adds open fifths, a second chord change]
[Instrumental Figure Lower - the same ostinato an octave down, cello holding]
[Instrumental New Harmony Under the Figure - the chord darkens, level unchanged]
[Instrumental Harp Takes the Figure - dulcimer resting, the figure still turning]
[Instrumental Figure Thins - notes dropped from the ostinato, the water flattening]
[Instrumental Long Tail - one held cello note under a last dulcimer strike, very long quiet tail]
```

---

**The Woods Go Quiet**
*Key F Dorian · 50 BPM · 5/4 · neoclassical strings (Arnalds-like) · string quartet, clarinet · instrumental · 6:00*

Style prompt:
```
This is a slow chorale for string quartet and clarinet, neoclassical, F Dorian at 50 BPM in an uneven 5/4 that never hurries. The bows are long and the harmony moves one chord at a time. Arnalds-like. A cathedral tail stands behind everything. Flat dynamics; variation, register and thinning, never a peak. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Verse 1 - the cello alone climbs a slow F Dorian scale, uneven 5/4, cathedral tail]
[Instrumental Verse 2 - the quartet answers in long-bowed chords, one chord per bar]
[Instrumental Verse 3 - clarinet holds a line above the chorale, shaded]
[Instrumental Verse 4 - the same chords a register lower, viola on top]
[Instrumental Verse 5 - two voices only, the woods emptying, level unchanged]
[Instrumental Verse 6 - the cello's rising scale returns under the chorale]
[Instrumental Last Verse - one chord held, the clarinet silent]
[Instrumental Long Tail - the cathedral tail alone, very long and quiet]
```

---

**Starfall**
*Key E♭ Lydian · 44 BPM · free · Satie-like piano · solo piano · instrumental · 6:00*

Style prompt:
```
Solo piano, nothing else: Satie-like writing in E♭ Lydian at 44 BPM, unmetered, with no pulse anywhere in it. Felt hammers, close and dry, almost no room around the notes, and the theme is four bars long and never grows. Flat dynamics; each variation takes notes away rather than adding them. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Theme - one E♭ Lydian chord, a silence, then the plain theme, unmetered]
[Instrumental Variation 1 - the theme with that chord answering it, close and dry]
[Instrumental Variation 2 - the same notes an octave higher, felt hammers]
[Instrumental Variation 3 - the theme with half its notes gone]
[Instrumental Variation 4 - only the chords remain, the melody implied]
[Instrumental Variation 5 - the theme very low, one hand alone]
[Instrumental Variation 6 - two notes and a long silence]
[Instrumental Theme Returns - the opening theme unchanged, no louder]
[Instrumental Long Tail - one last chord, very long quiet tail]
```

---

**Moonrise Through Leaves**
*Key A♭ Aeolian · 46 BPM · 4/4 · music-box lullaby · celesta, harp, bass clarinet once · instrumental · 6:00*

Style prompt:
```
Celesta and harp, with one bass clarinet entrance in the whole cue: a music-box lullaby in A♭ Aeolian, 46 BPM, a slow 4/4 with barely a pulse in it. The sound is inside the music box, tiny and close, the mechanism just audible. Flat dynamics; the piece drifts rather than develops and nothing ever arrives. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Drift 1 - one held A♭ note that slowly becomes a chord, inside the music box]
[Instrumental Drift 2 - celesta places single notes inside that chord]
[Instrumental Drift 3 - harp joins, the two passing the same small figure]
[Instrumental Drift 4 - the chord changes underneath, nothing else moves]
[Instrumental Drift 5 - a bass clarinet holds one low note, once, then goes]
[Instrumental Drift 6 - celesta alone, the harp gone, very small]
[Instrumental Drift 7 - the chord thinning back towards one note]
[Instrumental Long Tail - a last celesta note over that held note, very long quiet tail]
```

---

**Cold Stars**
*Key F♯ Dorian · 48 BPM · 4/4 · Nordic folk, hymn-like · soft horn, string harmonics, piano · instrumental · 6:00*

Style prompt:
```
Scored for a soft sustained horn, string harmonics and piano: Nordic folk turned hymn-like, F♯ Dorian, 48 BPM, a slow 4/4 of held chords. The horn is never a stab; the bows are long and high. A stone hall with a long tail. Soule's Skyrim at its quietest. Flat dynamics; variation, register and thinning only. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Verse 1 - piano alone in a low octave, F♯ Dorian, stone hall, slow 4/4]
[Instrumental Verse 2 - string harmonics answer high above, thin and cold]
[Instrumental Verse 3 - the soft horn sustains one line through the chords, never a stab]
[Instrumental Verse 4 - the chorale a fifth lower, piano holding the bass]
[Instrumental Verse 5 - harmonics and horn only, the piano resting]
[Instrumental Verse 6 - the low octave returns under the chorale]
[Instrumental Verse 7 - one chord held, the horn silent]
[Instrumental Long Tail - a single low piano note in the hall, very long quiet tail]
```

---

**The Owl Hours**
*Key C Aeolian · 42 BPM · free · ambient drone · bass clarinet, bowed vibraphone, contrabass · instrumental · 6:00*

Style prompt:
```
An ambient drone in C Aeolian at 42 BPM, unmetered and pulseless, carried by bass clarinet, bowed vibraphone and contrabass. The bowed metal shimmers without attack; the clarinet's notes are few and far apart. Cave space, close stone and a long tail. Flat dynamics; the cue drifts and thins and nothing ever arrives. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Drift 1 - a low C drone that only slowly becomes a chord, unmetered, cave stone]
[Instrumental Drift 2 - bass clarinet places a few dark notes inside it]
[Instrumental Drift 3 - bowed vibraphone shimmers far above the drone]
[Instrumental Drift Widens - the contrabass adds a second note, the space larger]
[Instrumental Drift 4 - the clarinet drifts to new pitches, no centre]
[Instrumental Drift Thins - bowed metal alone over the drone, the clarinet gone]
[Instrumental Drift 5 - the clarinet returns at the opening level, uneasy but calm]
[Instrumental Long Tail - the drone thinning to one quiet note, very long tail]
```

---

**Embers on the Road**
*Key G Dorian · 52 BPM · 6/8 · Renaissance lute · lute, viola · instrumental · 6:00*

Style prompt:
```
Renaissance lute writing for lute and viola in G Dorian, 52 BPM, a rolling 6/8: the lute's figure never stops and the viola's bows are long over it. In the manner of Dowland, slowed to a walk. A small wooden room, close and nearly dry. Flat dynamics; each turn of the ground varies it and none of them builds. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Ground - the lute alone, one G Dorian figure repeated, rolling 6/8, wooden room]
[Instrumental Variation 1 - viola enters with long bows over the ground]
[Instrumental Variation 2 - the lute ornaments the ground, viola holding]
[Instrumental Variation 3 - the ground an octave lower, viola above it]
[Instrumental Variation 4 - viola alone over the bare ground, very few notes]
[Instrumental Variation 5 - the ground plainest of all, no ornament left]
[Instrumental Variation 6 - viola and lute together at the opening level]
[Instrumental Long Tail - the ground slowing to one open chord, very long quiet tail]
```

---

**Lantern in the Window**
*Key C Lydian · 40 BPM · 3/4 · post-rock ambient, acoustic · cello harmonics, celesta, soft pad · instrumental · 6:00*

Style prompt:
```
Cello harmonics, celesta and a soft string pad, acoustic post-rock ambient in C Lydian, 40 BPM, a very slow 3/4 you feel rather than count. The harmonics are held for whole bars; the celesta gives one note at a time. A cathedral tail around all of it. Flat dynamics; the arch fills and empties without ever peaking. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Thin - the pad is already sounding as the cue begins, C Lydian, very slow 3/4]
[Instrumental Filling - cello harmonics enter high above, held for whole bars]
[Instrumental Fuller - celesta places single notes between the harmonics]
[Instrumental Widest, Still Quiet - all three together, no louder than the pad was alone]
[Instrumental Emptying - the celesta drops out, harmonics holding longer]
[Instrumental Thinner - one harmonic over the pad, the cathedral tail audible]
[Instrumental Thin Again - the pad alone, as at the opening]
[Instrumental Long Tail - the pad fading with the hall, very long quiet tail]
```

---

**Rain on the Road**
*Key C♯ Dorian · 54 BPM · 4/4 · minimalist piano (Nils Frahm-like) · piano, harp, viola · instrumental · 6:00*

Style prompt:
```
Piano with harp and viola under it, minimalist writing in C♯ Dorian, 54 BPM, an even 4/4 in which the piano figure never stops from the first bar to the last. Nils Frahm-like: felt hammers, close and dry, the mechanism audible. Flat dynamics; only the harmony beneath the figure changes. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Figure Alone - the piano figure already running as the cue starts, even 4/4, close and dry]
[Instrumental Harmony Shifts - viola moves the chord under the unchanged figure]
[Instrumental Harp Doubles the Figure - the same notes displaced, rain implied and never played]
[Instrumental Harmony Shifts Again - a darker chord, the figure identical]
[Instrumental Figure Lower - the piano takes it an octave down, viola holding]
[Instrumental Figure Alone Again - harp and viola resting, the figure still going]
[Instrumental Figure Thins - notes dropped one at a time, dynamics unchanged]
[Instrumental Long Tail - the last few notes of the figure, very long quiet tail]
```

---

**The Storm Passing**
*Key E♭ Dorian · 46 BPM · free · English pastoral, wind-led · cor anglais, string quartet, distant timpani roll · instrumental · 6:00*

Style prompt:
```
Cor anglais, string quartet, one distant timpani roll: English pastoral, wind-led, E♭ Dorian, 46 BPM, unmetered. The cor anglais keeps simple steady stepwise lines in a fixed register; the bows are long. Open field. Flat dynamics; the roll is distant thunder, not a hit. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax. No glissando, no pitch sweeps, no portamento.
```
Structure prompt:
```
[Instrumental Part One - the cor anglais climbs a slow E♭ Dorian scale alone, unmetered, open field]
[Instrumental Part One Widens - the quartet holds long bows beneath the line]
[Instrumental Middle - a contrasting idea in the quartet alone, weather still overhead]
[Instrumental Middle, Distant - one far-off timpani roll under the strings, thunder leaving, never a hit]
[Instrumental Middle Thins - two string voices only, the sky clearing]
[Instrumental Part One Returns - the rising line again on cor anglais, no louder]
[Instrumental Part One Lower - the same line a fifth down, the quartet thinning]
[Instrumental Long Tail - one held string chord, the wind gone, very long quiet tail]
```

---

**Under the Eaves**
*Key B♭ Dorian · 50 BPM · 3/4 · chamber folk, guitar duo · two nylon guitars · instrumental · 6:00*

Style prompt:
```
Nothing but two nylon guitars: chamber folk in B♭ Dorian, 50 BPM, a quiet 3/4 that rocks gently and never pushes. Close-miked in a small wooden room, fingerboard noise left in, rain outside a shelter you built. Flat dynamics; the refrain returns unchanged and the episodes vary around it. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Refrain - both guitars in unison, one rocking B♭ Dorian phrase in 3/4, small wooden room]
[Instrumental Episode 1 - the second guitar splits off into a counter-figure]
[Instrumental Refrain Lower - unison again, a register down, close and woody]
[Instrumental Episode 2 - one guitar alone, the phrase taken further down]
[Instrumental Refrain - unison at the opening level, no louder]
[Instrumental Episode 3 - both guitars in slow chords, the rain outside implied]
[Instrumental Refrain Alone - the unison with notes missing, still rocking in 3/4]
[Instrumental Long Tail - one held guitar chord, very long quiet tail]
```

---

**Turned Earth**
*Key F Mixolydian · 68 BPM · 4/4 · Appalachian dulcimer with guitar · dulcimer, guitar, soft frame drum · instrumental · 6:00*

Style prompt:
```
Hammered dulcimer, nylon guitar and a soft frame drum keep an Appalachian figure in F Mixolydian, 68 BPM, an easy 4/4: the figure never stops and the drum is an even hand pulse, no accents. Close and dry, planting weather. Flat dynamics; only the harmony under the figure moves, and the drum leaves before the end. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Figure Alone - the dulcimer figure already running as the cue begins, easy 4/4, close and dry]
[Instrumental Drum Joins - a soft frame drum takes an even hand pulse, no accents]
[Instrumental Harmony Shifts - the guitar moves the chord, the figure unchanged]
[Instrumental Guitar Takes the Figure - dulcimer answering above, the drum steady]
[Instrumental Harmony Shifts Again - a warmer chord under the same notes]
[Instrumental Figure Lower - the whole figure an octave down, level unchanged]
[Instrumental Drum Leaves - the frame drum stops, the figure alone and quieter]
[Instrumental Long Tail - one open guitar chord over a last dulcimer strike, very long quiet tail]
```

---

**Sawdust**
*Key D Mixolydian · 62 BPM · 6/8 · chamber folk, fiddle-led (slow) · violin, guitar, cello · instrumental · 6:00*

Style prompt:
```
Violin leading guitar and cello, chamber folk slowed almost to stillness, D Mixolydian at 62 BPM in a lilting 6/8 that rolls under everything. Workbench company: close-miked in a small wooden room, unhurried and domestic. Flat dynamics; each variation changes register or takes notes away, and none of them builds. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Theme - the violin alone, one D Mixolydian phrase repeated, lilting 6/8]
[Instrumental Variation 1 - nylon guitar rolls underneath, the phrase unchanged]
[Instrumental Variation 2 - cello takes the phrase low, violin holding above]
[Instrumental Variation 3 - guitar alone with the phrase, plain and unhurried]
[Instrumental Variation 4 - violin ornaments it lightly, no louder]
[Instrumental Variation 5 - the phrase with half its notes gone, the wooden room close]
[Instrumental Theme Returns - all three at the opening level, unhurried]
[Instrumental Long Tail - cello last under a held guitar chord, very long quiet tail]
```

---

**The Bench by the Fire**
*Key A Dorian · 54 BPM · 4/4 · minimalist piano with cello · piano, cello · instrumental · 6:00*

Style prompt:
```
Just piano and cello: minimalist writing in A Dorian at 54 BPM, an unhurried 4/4 with a soft walking left hand. The cello's bows are long, and the two take turns rather than play together. Close and dry, soft pedal, firelight at the place you built. Flat dynamics; variation, register and thinning only. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Call - the piano's left hand alone, a walking A Dorian bass, unhurried 4/4]
[Instrumental Response - the cello answers with one long-bowed phrase]
[Instrumental Call - the right hand joins the bass, close and dry, soft pedal]
[Instrumental Response - the cello answers lower, fewer notes]
[Instrumental Call and Response Together - the two overlap once, no louder]
[Instrumental Call - piano alone again, the bass still walking]
[Instrumental Response Alone - one held cello note, no piano at all]
[Instrumental Long Tail - the walking bass slowing to a stop, very long quiet tail]
```

---

**Stone on Stone**
*Key E♭ Mixolydian · 60 BPM · 5/4 · Nordic folk, plucked · plucked consort: lute, harp, dulcimer · instrumental · 6:00*

Style prompt:
```
A plucked consort of lute, harp and hammered dulcimer playing Nordic folk in E♭ Mixolydian, 60 BPM, an uneven 5/4 that is unhurried rather than driven. Everything is plucked; nothing is bowed and nothing is held. A stone hall with a long tail. Flat dynamics; the refrain returns unchanged between the episodes. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Refrain - the consort is already playing as the cue begins, uneven 5/4, stone hall]
[Instrumental Episode 1 - lute alone with a new figure, harp answering late]
[Instrumental Refrain - all three again, the refrain unchanged]
[Instrumental Episode 2 - dulcimer leads, the harmony moving under it]
[Instrumental Refrain Lower - the refrain an octave down, level unchanged]
[Instrumental Episode 3 - harp alone, slow and wide, the hall audible around it]
[Instrumental Refrain Thinned - the consort with notes missing, still in 5/4]
[Instrumental Long Tail - one plucked chord ringing out in the hall, very long quiet tail]
```

---

**Supper**
*Key B♭ major · 58 BPM · 3/4 · Stardew-like waltz, acoustic · guitar, oboe, clarinet · instrumental · 6:00*

Style prompt:
```
An acoustic waltz in B♭ major for nylon guitar, oboe and clarinet, 58 BPM, 3/4 throughout, gently turning. The winds keep simple steady stepwise lines in a fixed register. Stardew-like warmth in a small wooden room. Flat dynamics; variation, register and thinning. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax. No glissando, no pitch sweeps, no portamento.
```
Structure prompt:
```
[Instrumental Part One - one warm B♭ guitar chord, a silence, then the waltz begins turning in 3/4]
[Instrumental Part One Widens - the oboe takes the tune, plain and stepwise, one register]
[Instrumental Part One Shared - clarinet joins below the oboe, the table settling]
[Instrumental Middle - a contrasting strain, guitar and clarinet only, the waltz still turning]
[Instrumental Middle Quieter - guitar alone, fewer notes, small wooden room]
[Instrumental Part One Returns - the waltz and the oboe as at the opening, no louder]
[Instrumental Part One Thinned - the tune with notes taken away, still in 3/4]
[Instrumental Long Tail - one held guitar chord, the winds gone, very long quiet tail]
```

---

**The Day's Work Done**
*Key F♯ Aeolian · 44 BPM · free · music-box lullaby with piano · music box, piano · instrumental · 6:00*

Style prompt:
```
Music box first, piano second: a lullaby in F♯ Aeolian, 44 BPM, unmetered, with no pulse to tap at all. The recording sits inside the music box, tiny and close, the mechanism audible under the notes. Flat dynamics; the cue drifts and thins and never arrives anywhere. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Drift 1 - the music box alone, one turn of a small F♯ Aeolian figure, unmetered]
[Instrumental Drift 2 - soft piano answers underneath, soft pedal, very near]
[Instrumental Drift 3 - the figure turns again, simplified, the room settling]
[Instrumental Drift Slows - piano alone holding a low chord, the box silent]
[Instrumental Drift 4 - the music box winds down, almost stopping]
[Instrumental Drift 5 - one turn of the figure returns, barely there]
[Instrumental Drift Returns - piano and music box together at the opening level]
[Instrumental Long Tail - a last music-box note, mechanism audible, very long quiet tail]
```

---

**Drip and Dark**
*Key A drone · 40 BPM · free · ambient drone, bowed metal · bowed crotales, contrabass, harp harmonics · instrumental · 6:00*

Style prompt:
```
Bowed crotales over a contrabass, with harp harmonics far above: an ambient drone on an A centre, 40 BPM, unmetered, with no key and no melody to follow. Bowed metal only, no attack anywhere. Deep cave space, wet stone and a very long tail. Flat dynamics; the cue drifts and thins for six minutes and arrives nowhere. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Drift 1 - a contrabass drone on A that only slowly becomes a chord, cave stone, unmetered]
[Instrumental Drift 2 - bowed crotales enter far above, single tones, no melody]
[Instrumental Drift 3 - harp harmonics fall between them like water]
[Instrumental Drift 4 - the drone thickens by one note, the cave widening]
[Instrumental Drift 5 - crotales drift to new pitches, no centre]
[Instrumental Drift 6 - the drone alone, very long, almost unchanging]
[Instrumental Drift 7 - one crotale tone with silence around it]
[Instrumental Drift 8 - harp harmonics once more, then gone]
[Instrumental Long Tail - the drone thinning to nothing, very long quiet tail]
```

---

**The Sleeping Stone**
*Key A♭ Phrygian · 46 BPM · 4/4 · baroque chorale, low · bass clarinet, cello, viola · instrumental · 6:00*

Style prompt:
```
Bass clarinet, cello and viola in a low baroque chorale, A♭ Phrygian at 46 BPM, a slow 4/4 of one chord per bar. The bows are long, the clarinet holds under everything, and the Phrygian second darkens each cadence. Deep cave space, wet stone and a long tail. Flat dynamics; register and thinning only, never a peak. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Verse 1 - the bass clarinet alone in the cave, a low A♭ Phrygian line, slow 4/4]
[Instrumental Verse 2 - cello adds the chord above, one chord per bar]
[Instrumental Verse 3 - viola completes the chorale, all bows long]
[Instrumental Verse 4 - the same chords lower, the stone space larger]
[Instrumental Verse 5 - cello and viola only, the clarinet resting]
[Instrumental Verse 6 - the Phrygian second darkens the cadence, level unchanged]
[Instrumental Verse 7 - one chord held, nothing moving]
[Instrumental Verse 8 - the bass clarinet alone again, as at the opening]
[Instrumental Long Tail - the cave's tail alone, very long and quiet]
```

---

**Slack Water**
*Key B Dorian · 54 BPM · 6/8 · sea shanty slowed to a lament · viola, dulcimer, harp · instrumental · 6:00*

Style prompt:
```
Viola, hammered dulcimer and harp turn a sea shanty into a lament: B Dorian, 54 BPM, a rolling 6/8 that never quickens. The viola's bows are long over a ground that repeats for the whole cue and never stops. Open water under a boat, wide air. Flat dynamics; each turn of the ground varies it and none of them builds. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Ground - the viola climbs a slow B Dorian scale, then the ground settles under it, rolling 6/8]
[Instrumental Variation 1 - harp takes the ground in open fifths, viola long-bowed above]
[Instrumental Variation 2 - hammered dulcimer joins, the shanty tune plain and slow]
[Instrumental Variation 3 - the tune lowered a register, the ground unchanged]
[Instrumental Variation 4 - dulcimer alone over the ground, viola resting]
[Instrumental Variation 5 - the viola's rising scale returns inside the tune]
[Instrumental Ground Alone - the ground with notes missing, the water flattening]
[Instrumental Long Tail - one held viola note over the last of the ground, very long quiet tail]
```

---

**The Far Shore**
*Key E Mixolydian · 60 BPM · 4/4 · Celtic slow air · low whistle (constrained), harp, cello · instrumental · 6:00*

Style prompt:
```
Low whistle with harp and cello, a Celtic slow air in E Mixolydian, 60 BPM, a walking 4/4. The whistle keeps simple steady stepwise lines in a fixed register; the harp's arpeggio never stops. Open water. Soule's quiet Oblivion coast. Flat dynamics; variation and thinning. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax. No glissando, no pitch sweeps, no portamento.
```
Structure prompt:
```
[Instrumental Theme - the harp alone, one E Mixolydian arpeggio repeated, walking 4/4]
[Instrumental Variation 1 - low whistle states the air above it, stepwise, fixed register]
[Instrumental Variation 2 - the cello holds a low note, the arpeggio unchanged]
[Instrumental Variation 3 - the air varied stepwise, still one register]
[Instrumental Variation 4 - harp and cello alone, the air remembered and not played]
[Instrumental Variation 5 - the arpeggio widens into open fifths, whistle silent]
[Instrumental Variation 6 - the whistle returns with the air, no louder]
[Instrumental Theme Returns Alone - the harp with the opening arpeggio, nothing above it]
[Instrumental Long Tail - one held cello note under a last harp figure, very long quiet tail]
```

---

**Something in the Trees**
*Key G Phrygian · 84 BPM · 7/8 · neoclassical strings, tense · low strings ostinato, frame drum, clarinet · instrumental · 4:00*

Style prompt:
```
Low strings hold an ostinato under a soft frame drum and one clarinet: tense neoclassical strings in G Phrygian, 84 BPM, an off-balance 7/8 that never resolves. The figure never stops and the drum is even and unaccented. A stone hall with a long tail. A raised pulse, not a battle: flat dynamics, no build, no arrival. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Figure Alone - the low strings ostinato already running, off-balance 7/8, stone hall]
[Instrumental Drum Joins - a soft frame drum takes the 7/8 evenly, no accents]
[Instrumental Harmony Shifts - the chord under the figure darkens, level unchanged]
[Instrumental Clarinet Above - one held clarinet note over the ostinato, watchful]
[Instrumental Drum Leaves - the frame drum stops, low strings alone and very quiet]
[Instrumental Long Tail - the ostinato thinning to one repeated note, very long quiet tail]
```

---

**Held Breath**
*Key B♭ Aeolian · 72 BPM · 4/4 · ambient drone, tense · cello, bowed metal, soft drum · instrumental · 4:00*

Style prompt:
```
Cello against bowed metal and a very soft frame drum, a tense ambient drone in B♭ Aeolian, 72 BPM, an even 4/4 held back the whole way. The cello's bows are long; the drum is a flat pulse with no accents. Cave space, close stone. A raised pulse, not a battle: the arch fills and empties and never peaks. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Thin - a low B♭ drone that slowly becomes a chord, bowed metal over it, cave stone]
[Instrumental Filling - the cello enters low, few notes, long bows, held back]
[Instrumental Fuller - a very soft frame drum keeps an even 4/4, no accents]
[Instrumental Emptying - the drum stops, cello and bowed metal only]
[Instrumental Thin Again - bowed metal alone over the drone, as at the opening]
[Instrumental Long Tail - one held cello note under the fading drone, very long quiet tail]
```

---

**Prelude, a Quiet Morning**
*Key F Lydian · 52 BPM · 3/4 · baroque chorale, bright · piano, harp, oboe · instrumental · 5:00*

Style prompt:
```
Piano, harp and oboe in a bright baroque chorale, F Lydian, 52 BPM, a walking 3/4 of one chord per bar. The oboe keeps simple steady stepwise lines in a fixed register. A cathedral tail; a title screen that asks nothing. Flat dynamics, no fanfare; thinning and variation. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax. No glissando, no pitch sweeps, no portamento.
```
Structure prompt:
```
[Instrumental Verse 1 - piano and harp in unison, one bright F Lydian phrase, walking 3/4]
[Instrumental Verse 2 - the chorale opens into chords, one per bar, cathedral tail]
[Instrumental Verse 3 - the oboe takes the top line, plain and stepwise]
[Instrumental Verse 4 - the same chords a register lower, harp arpeggiating them]
[Instrumental Verse 5 - piano alone with the chorale, no fanfare]
[Instrumental Verse 6 - unison returns between harp and oboe, no louder]
[Instrumental Chorale Thinned - two voices only, one chord held in the hall]
[Instrumental Long Tail - the cathedral tail alone, very long and quiet]
```

---

**Prelude, After Dark**
*Key E Aeolian · 46 BPM · free · neoclassical strings, nocturne · cello, celesta, string quartet · instrumental · 5:00*

Style prompt:
```
A nocturne for cello, celesta and string quartet, neoclassical, E Aeolian at 46 BPM, unmetered, with no pulse to tap. The cello's bows are very long; the celesta places one note at a time. A stone hall with a long tail. Soule's quietest night writing. Flat dynamics, no fanfare; each variation thins the one before. Full-length cue, developed and through-composed, no early fade, long outro. No vocals, no choir, no drum kit, no brass stabs, no synth, no electric guitar, no crescendo, no climax.
```
Structure prompt:
```
[Instrumental Theme - the cello alone in a low register, one E Aeolian theme, unmetered, stone hall]
[Instrumental Variation 1 - celesta places single notes above the theme]
[Instrumental Variation 2 - the string quartet holds the harmony behind, long bows]
[Instrumental Variation 3 - the theme moves to the viola, cello holding the bass]
[Instrumental Variation 4 - celesta and quartet only, the theme implied]
[Instrumental Variation 5 - the theme with notes taken away, no fanfare]
[Instrumental Theme Alone Again - the cello as at the opening, no louder]
[Instrumental Long Tail - one celesta note over a held quartet chord, very long quiet tail]
```
