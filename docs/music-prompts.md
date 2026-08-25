<!--
PROVENANCE -- READ BEFORE EDITING.

This file is a VERBATIM copy of design/MUSIC_PROMPTS.md from the Mira-Thal /
Voxelmark Godot checkout, github.com/mnoles1911/Test, retrieved 2026-08-24 from
branch `main`. Upstream history: authored 2026-05-16 (b644f5ca), reworked the
same day (576dd18a), squash-merged as 7eb3372d "Audio system: music/SFX design
corpus + ElevenLabs pipeline + AudioManager runtime & wiring" (#216).

It is copied here because voxelsim's front end inherited that soundtrack (see
docs/adr/0009) and the Test repo is no longer worked in. The pre-rework version
is kept alongside as music-prompts-original-2026-05-16.md; that one is
SUPERSEDED and is here for the record only. Prefer THIS file.

WHAT THIS DOC DOES NOT CONTAIN, and it matters: the prompts that generated the
three tracks currently on disk in the Test repo -- `Main Theme.wav`,
`Sea _ Sailing.wav`, `World Map _ Travel.wav`. Those were committed 2026-05-06
(PR #150), ten days BEFORE this doc was written, and their prompts were never
recorded anywhere -- not in the PR body, not in design/AUDIO_DESIGN.md, not in
any commit. This portfolio treats all three as pre-existing and marks them for
replacement: track 02 replaces `Main Theme`, tracks 32/30 supersede
`Sea _ Sailing`, and a `World Map _ Travel` replacement regenerates from 04.
So there is no recipe to reproduce the current three; there is a designed
replacement for each.

Rendered output belongs in ue-project/Content/Audio/Music/ -- as OGG, not WAV.
See that directory's MUSIC_CREDITS.md for the size rule and why.

Cross-references below to design/AUDIO_DESIGN.md, design/SFX_LIBRARY.md and
design/SFX_PROMPTS.md point at the Test repo, not at this one. They have not
been rewritten, because rewriting them would make the copy non-verbatim and a
diff against upstream useless.
-->

# Music Prompts — Mira-Thal Soundtrack (Suno)

The full music portfolio for Game One, written as Suno prompts. **Instrumental,
epic-orchestral, lush and pastoral — Oblivion first.** Primary inspiration is
Jeremy Soule's *Oblivion* soundtrack, then *Morrowind*, then *Skyrim* and
Howard Shore's *Lord of the Rings*. The score is instrumental; choir is a rare
brief exception on a handful of the biggest cues only. Each track is a **style
prompt + a structure prompt** pair.

> Cross-reference: `design/AUDIO_DESIGN.md` — where music plays, transition
> rules, the bus layout. This doc is the **content**; that doc is the
> **system**.

---

## 1. Reference anchor: the Main Theme

The Main Theme (track **02**) is the score's identity anchor. Its core
**D-minor heroic-melancholic theme** and its **Lux per umbram** Latin identity
are kept, but it has been **reworked in place**: the choir is cut way down to a
single brief, distant, light statement (no grand choral anthem, no "Vocat"
battle shout), the references lean to Soule's *Oblivion*, and the flute is
constrained to steady stepwise lines. It is **D minor (Aeolian), 84 BPM**.
Everything else branches *off* it but must not sound like it transposed. Two
rules follow:

- **Key variety.** The Main Theme owns D minor. Every other cue is in a
  deliberately different key and/or mode (see the **Key** column in §5) so the
  portfolio reads as a varied score, not one piece moved around the keyboard.
  The only D-centred exceptions are three *intentional* thematic callbacks
  (37, 51, 57) — flagged where they occur.
- **Choir is rare.** Even the Main Theme now uses only one brief light Latin
  statement. The rest of the soundtrack leans hard instrumental: choir appears
  on only **six** cues total (02 plus five), always brief, light, and on the
  other five explicitly optional/omittable. See §3.

---

## 2. Two problems this doc solves

**Problem A — sameness.** The old portfolio blurred together: same opening
words, same leads, same Aeolian/Dorian, same 72–96 BPM, same D-ish tonality,
same arc. Soule didn't write *Oblivion* as one cue — "Reign of the Septims,"
"Through the Valleys," "Auriel's Ascension" and "Peace of Akatosh" share a
world but not an instrument, key, tempo or mood. This doc enforces that with a
variation matrix (this section), explicit per-track keys (§5), cultural
palettes (§4) and a leitmotif system (§3).

**Problem B — length & flute artifacts.** Tracks rendered short, and flute
lines came out as weird high-to-low pitch sweeps. Both are now fixed in every
prompt:

- **Length:** every cue targets **3:00–5:00 minimum**, via 6–9 developed
  structure sections plus an explicit "full-length, developed, no early fade,
  long outro" instruction in each style box. See §6 for the Suno length
  workflow.
- **Flute discipline (global):** flute and recorder are allowed but only as
  **simple, steady, stepwise melodic lines in a fixed register**. Every
  prompt that uses flute says so, and every negative list carries: *no flute
  glissando, no pitch sweeps, no portamento, no octave-spanning runs, no
  breathy whistle effects*. This is what kills the "top-to-bottom flute noise."

---

## 3. The nine variation levers + the choir rule

Every track gets deliberately different values; §5 shows the grid. Rule: **no
two tracks in the same category may share more than three lever values.**

1. **Key & mode** — an explicit key signature per track (§5). Spread across
   the circle of fifths and all church modes; never default to D minor.
2. **Tempo** — explicit BPM, spread 40–168. Never cluster three in 72–96.
3. **Lead voice** — the single most distinctive *instrument*, named first.
4. **Ensemble density** — solo / chamber / small consort / string orchestra /
   full orchestra / percussion-only / drone-only.
5. **Percussion family** — none / soft frame drum / hand percussion / timpani
   & field drums / taiko & dhol battery / anvils & stomps / arrhythmic stone.
6. **Vocal treatment** — **instrumental (default — 53 of 59)**. Choir appears
   on **only these six**, brief, light, never solo, never lead melody: **02**
   (one brief light distant Latin statement, sung once), **24, 34, 41, 45, 47**
   (brief texture only — each style box says "choir optional, may be omitted
   for fully instrumental"). No solo voices, no sung verses, no spoken word —
   **except the one authored exception, 49** (a diegetic solo-vocal folk
   ballad sung the night before battle, "Jenny of Oldstones"-style, with
   original lyrics).
7. **Recording space** — dry chamber / intimate close / open field / great
   stone hall / cathedral / cavern / storm-air / "music box".
8. **Structural arc** — vary section count/order; 6–9 sections to reach length.
9. **Cultural palette** — fixed sonic identity per region/faction (§4) sets
   levers 3–6 before mood does.

**Leitmotifs (instrumental, identified by contour and transposed freely):**

| Motif | Whose | Shape | Feel |
|---|---|---|---|
| **Endurance cell** | Roland, Iron Chalice, humankind | rising step, falling third, held | Aeolian, noble |
| **The Song / Eighth Star** | Aelorin, the Aeluvain | high, slow, stops one note short | Lydian, weightless |
| **The Hollowing** | Mordvar | the Song inverted and emptied, descending fifths that never close | octatonic, airless |
| **The Crown** | the seven pieces | seven-note cell, each note a different timbre, heard fractured | chromatic, brittle |
| **The Hearth** | companions, home, rest | warm four-note folk fragment, the only motif that resolves | Mixolydian, plain |

Where the six choir cues use voices, it is **massed only** — the Latin war-cry
*Lux per umbram / ferrum per ignem / sanguis per saecula / terra nos vocat* or
its mirror *Nihil per nihil / cor per inane / nemo per saecula / nihil nos
tenet*, or wordless vowels — used like a brass section, never a soloist, and
omittable.

---

## 4. Cultural sonic palettes (Oblivion-leaning, instrumental)

Lush, orchestral, pastoral-forward — Soule's *Oblivion* warmth: harp, solo
woodwinds, French horn, warm strings, sparing percussion. Decide the palette
first; it pre-sets levers 3–6.

| Culture / place | Lead instruments | Mode tendency | Percussion | Space |
|---|---|---|---|---|
| **Human heartland** (Eldermark, Aldenholt, the road) | French horn, oboe, solo cello, harp | Mixolydian/Dorian, Aeolian | soft frame drum, light timpani | open field / hall |
| **Iron Chalice** (Brightwatch, the chapel, Roland) | low strings, muted trumpet, lone war horn, organ | Aeolian, austere | one deep field drum | dry stone |
| **Aelorin** (Greatwood, Lirien-Thal, the Aeluvain) | harp, glass harmonica, celesta, high strings, clarinet | Lydian, weightless | none / finger crotales | shimmering long reverb |
| **Dwarven** (Karaz-Dûn, the Underway, the holds) | contrabassoon, low brass, hammered dulcimer, anvil | Dorian/Phrygian | anvils, stomps, 6/8 toms | great stone hall |
| **Naergrim** (Mor-Vethrin, Weeping Wood) | detuned/prepared strings, bowed metal, contrabass clarinet | cluster / no center | arrhythmic stone, chains | airless, close |
| **Mordvar / Ashen Hand** | dissonant low brass, contrabassoon, war battery | octatonic/whole-tone | taiko + dhol battery | huge, brutal |
| **Sailor's Guild / sea** | concertina, fiddle, low whistle, accordion | Dorian/Mixolydian, rolling 6/8 | hand drum, deck-stomp | salt-air, medium room |
| **Tavern / folk** | fiddle, lute, recorder, hurdy-gurdy | Mixolydian/Dorian, major | tabor, foot, claps | small warm room |
| **Dead places** (Sorrowmarsh, Ashfields) | bowed psaltery, cor anglais, low strings, bowed metal | drone, no harmony | none / one far bowl | vast empty |

Flute/recorder in any palette: simple steady stepwise lines, fixed register,
**no glissando or pitch sweeps**.

---

## 5. Track inventory (59)

Naming follows the existing convention (`Main Theme`, `Sea _ Sailing`).
**Lengths are 3:00 floors.** **Key** is explicit and deliberately varied — note
how few touch D. "Choir" = brief optional massed texture (six cues only); all
others fully instrumental.

| # | Title | Category | Palette | Key | BPM | Lead | Choir | Len |
|---|---|---|---|---|---|---|---|---|
| 01 | Prelude _ The Eighth Star | Identity | Aelorin | F Lydian | 54 | glass harmonica | — | 3:00 |
| 02 | Main Theme | Identity | Human | D minor (Aeolian) | 84 | strings/horns | 1 brief | 4:30 |
| 03 | End Credits _ The Long Twilight | Identity | suite | B♭ Aeolian → mod. | 76 | solo cello | — | 5:00 |
| 04 | Open Road _ The Central Plains | Exploration | Human | G Mixolydian | 92 | oboe | — | 4:00 |
| 05 | The Greatwood _ Under Old Leaves | Exploration | Aelorin | A Lydian | 60 | harp + high strings | — | 4:30 |
| 06 | The Spine _ Stone and Sky | Exploration | Dwarven-adj. | E Dorian | 66 | French horn | — | 4:00 |
| 07 | The Underway _ Beneath the Mountain | Exploration | Dwarven | C Phrygian | 54 | contrabassoon | — | 4:30 |
| 08 | The Ashfields _ Grey Soil | Exploration | Dead | G drone | 44 | cor anglais | — | 4:00 |
| 09 | The Western Coast _ Caer Drowned | Exploration | Sea/dead | B Aeolian | 58 | low whistle | — | 4:00 |
| 10 | The Copper Isles _ Salt and Sun | Exploration | Sailor | D Mixolydian | 104 | fiddle | — | 4:00 |
| 11 | The Sorrowmarsh _ The Mud Remembers | Exploration | Dead | atonal (E♭) | 40 | bowed psaltery | — | 4:00 |
| 12 | The Weeping Wood _ Watched | Exploration | Naergrim | cluster (F#) | 48 | prepared strings | — | 4:00 |
| 13 | Aldenholt _ Market and Bell | Settlement | Human | C Mixolydian | 100 | lute + recorder | — | 4:00 |
| 14 | Caer Brannoch _ The Cliff City | Settlement | Human/sea | G Dorian | 72 | solo cello + harp | — | 4:00 |
| 15 | Vosskar _ Iron and Listening | Settlement | Iron Chalice-adj. | F Aeolian | 64 | muted trumpet | — | 4:00 |
| 16 | Solgrade _ The Unwalled City | Settlement | Tavern/cosmo | A Dorian | 96 | hurdy-gurdy | — | 4:00 |
| 17 | Lirien-Thal _ The Silverwood | Settlement | Aelorin | D♭ Lydian | 52 | glass harmonica | — | 4:30 |
| 18 | Karaz-Dûn _ Forges Never Cold | Settlement | Dwarven | B♭ Dorian | 78 (6/8) | hammered dulcimer | — | 4:00 |
| 19 | Mor-Vethrin _ The Obsidian City | Settlement | Naergrim | no center (C#) | 46 | contrabass clarinet | — | 4:00 |
| 20 | Brightwatch _ The Frontier Garrison | Settlement | Iron Chalice | C Aeolian | 70 | lone war horn | — | 4:00 |
| 21 | The Archive _ Dust and Lamplight | Interior | Human/near-non-music | A static modal | 50 | bowed vibraphone | — | 5:00 |
| 22 | The Iron Chalice _ Chapel of Endurance | Sacred | Iron Chalice | E Aeolian | 56 | organ + low strings | — | 4:00 |
| 23 | The Aeluvain _ The Song With an Edge | Sacred | Aelorin | E Lydian | 58 | solo violin harmonics | — | 4:00 |
| 24 | The Crown Assembled _ Seven Metals | Sacred | mixed | chromatic (C) | 64 | seven timbres | brief opt. | 4:00 |
| 25 | Tavern _ The Limping Reel | Tavern | Folk | A Mixolydian | 132 | fiddle | — | 4:00 |
| 26 | The Widow's Lament _ A Quiet Room | Tavern | Folk | F Dorian | 68 | lute + viola | — | 4:00 |
| 27 | The Deep Cups _ A Dwarven Dance | Tavern | Dwarven | C Dorian | 88 (6/8) | dulcimer + anvil | — | 4:00 |
| 28 | The Dockside _ Salt and Strings | Tavern | Sailor | E Mixolydian | 120 | concertina | — | 4:00 |
| 29 | The Hearth _ An Aelorin Air | Tavern | Aelorin | B Lydian | 56 | harp + celesta | — | 4:00 |
| 30 | The Capstan _ Heave Her Round | Sea | Sailor | D Dorian | 96 | hand drum + low whistle | — | 4:00 |
| 31 | Leaving Port _ The Tide Turns | Sea | Sailor | B♭ Mixolydian | 84 (6/8) | fiddle + low whistle | — | 4:00 |
| 32 | At Sea _ Open Water | Sea | Sailor | B Dorian | 70 | accordion + cello | — | 4:30 |
| 33 | The Shroud _ The Storm That Never Ends | Sea | Mordvar-adj. | octatonic (F) | 72→132 | full orch + battery | — | 4:00 |
| 34 | The Eastern Crossing _ Into the Storm | Sea | mixed (epic) | G Aeolian → mod. | 80 | full orch | brief opt. | 5:00 |
| 35 | Campfire _ The Sound of Rest | Camp | Folk/intimate | F Mixolydian | 60 | solo lute-guitar | — | 4:00 |
| 36 | Night Rest _ Sleeping Under Stars | Camp | intimate | G Lydian | 48 | music box + harp | — | 4:30 |
| 37 | The Quiet After _ Wounds and Breath | Camp | Iron Chalice-adj. | D Aeolian* | 52 | solo cello | — | 4:00 |
| 38 | Enemies Gathering Strength _ The Muster of the Hand | War | Mordvar | octatonic (B♭) | 60→88 | low brass | — | 4:00 |
| 39 | A Minor Skirmish _ Blades in the Brush | War | Iron Chalice-adj. | G Phrygian | 116 | low strings ostinato | — | 3:30 |
| 40 | Charge Into Battle _ Sound the Horns | War | Human | E♭ Mixolydian | 152 | war horns + trumpets | — | 4:00 |
| 41 | The Large Battle _ The Field of Iron | War | mixed (suite) | E Aeolian → octatonic | 96→168 | full orch + battery | brief opt. | 5:00 |
| 42 | The Siege _ Hold the Walls | War | Dwarven | E Phrygian | 100 | anvils + low brass | — | 4:30 |
| 43 | Vaeroth the Pale _ The Hierarch | Boss | Mordvar | whole-tone (D) | 108 | contrabassoon | — | 4:00 |
| 44 | The Ashlord _ The Mask of Caerith | Boss | Naergrim/Aelorin | B Lydian → cluster | 92 | corrupted glass harmonica | — | 4:30 |
| 45 | Mordvar _ The Hollowing | Boss | Mordvar | octatonic (A) | 50 | dissonant low brass | brief opt. | 4:30 |
| 46 | The Fighting Retreat _ The Ashfields | War | Iron Chalice | F# Aeolian | 116 | war horn + strings | — | 4:00 |
| 47 | The Last Stand _ No Ground Behind | War | Iron Chalice/Human | C Aeolian → C Mixolydian | 84→144 | full orch | brief opt. | 4:30 |
| 48 | The Muster of the Alliance _ Many Banners | War | mixed (suite) | D Mixolydian (mod.) | 100 | rotating culture leads | — | 5:00 |
| 49 | The Vigil _ The Night Before | Cinematic | Folk ballad | A Aeolian | 100 (6/8) | solo voice + harp | LEAD VOCAL | 3:45 |
| 50 | Heroes Reunited _ The Fellowship Whole | Cinematic | mixed (motif weave) | A♭ Mixolydian | 76 | leitmotif weave | — | 4:00 |
| 51 | A Marriage _ Two Hands Bound | Cinematic | Folk/Human | D major (Ionian)* | 88 | harp + oboe + fiddle | — | 4:00 |
| 52 | Grief _ What the Archive Lost | Cinematic | Human | E♭ Aeolian | 46 | solo viola | — | 4:00 |
| 53 | Noble Sacrifice _ The Blow at the Marsh | Cinematic | mixed | G Aeolian → G Lydian | 60 | cello → full strings | — | 4:30 |
| 54 | Betrayal _ The Mole Revealed | Cinematic | Naergrim-adj. | B♭ minor → cluster | 64 | low strings + clock tick | — | 3:30 |
| 55 | Hope Rekindled _ The Turn | Cinematic | Human | A Aeolian → A Mixolydian | 72→104 | solo oboe → full orch | — | 4:00 |
| 56 | Epilogue _ The Road Home | Cinematic | Folk/Human | E♭ Mixolydian | 80 | solo cello + oboe | — | 4:00 |
| 57 | The Return _ Released | Ending | Aelorin/Human | D Lydian* (resolving) | 58 | full strings | brief opt. | 5:00 |
| 58 | The Hold _ Carried Forever | Ending | Iron Chalice | E Aeolian (unresolving) | 54 | solo cello + low strings | — | 5:00 |
| 59 | The Fracture _ The Price of Refusal | Ending | Mordvar-adj. | shattered (D/A) | 60 | broken orchestra | — | 5:00 |

`*` = the three intentional D-tonic callbacks to the Main Theme: **37** the
exhausted Endurance in D Aeolian, **51** joy as the parallel **D major**, **57**
the Main Theme's D transformed to **D Lydian** for the trilogy's resolution.
Existing on disk → **02 replaces `Main Theme`** (reworked in place), **32 / 30
supersede `Sea _ Sailing`**; regenerate a `World Map _ Travel` replacement
from #04.

---

## 6. How to use these in Suno

**Style box.** ~350–500 chars. Suno weights the first clause hardest — each
prompt below leads with its lead instrument or its explicit key/meter, never
the generic "cinematic medieval fantasy orchestral." Always state the explicit
key and BPM (the §5 Key column).

**Length.** For the 5:00 set-pieces (03, 21, 34, 41, 48, 57, 58, 59) generate
in two passes and stitch, or use Suno **Extend** to length then add a manual
fade. The 6–9-section structure boxes give enough material; if a take still
stops short, regenerate with the structure box only and a one-line style —
Suno respects long structures more than long styles.

**Flute.** Any prompt using flute/recorder constrains it to *simple steady
stepwise lines in a fixed register* and every negative list bans *flute
glissando / pitch sweeps / portamento / octave runs / whistle effects*. Do not
remove that language — it is what prevents the high-to-low flute artifact.

**Choir.** Only 02 (one brief light distant Latin statement, sung once) and
24/34/41/45/47 (brief, optional). Those five say "choir optional — may be
omitted for fully instrumental." If in doubt, render them instrumental; the
soundtrack is instrumental-first.

**Solo vocal.** Exactly one cue, **49 — The Vigil**, is a full lead-vocal
folk ballad with sung lyrics ("Jenny of Oldstones"-style, ~100 BPM 6/8). It
is the deliberate diegetic exception — every other cue stays instrumental
(or, for the six above, brief massed choir only).

**Reference lean.** Lead with *Oblivion* (Soule's lush pastoral orchestral),
then *Morrowind* (sparse contemplative, "Nerevar Rising" horn/lute), then
*Skyrim* and Shore. Most style boxes name them.

**Batch / audition.** (1) Motif anchors 02, 22, 23, 45, 35. (2) One per
culture 04, 05, 18, 12, 30, 25, 38 — listen back-to-back, push levers 1–3 apart
if any blur. (3) Extremes 36, 40, 52, 41. (4) Category order, auditioning each
against the previous in its category. Render best take → stereo 44.1 kHz →
`.ogg` → `assets/audio/music/` with the §5 title.

---

## 7. The prompts

Each entry: **filename → lever key → style prompt → structure prompt.**

---

### A. Identity & Frame

---

**01 — Prelude _ The Eighth Star**
*Key F Lydian · 54 BPM · Aelorin · glass harmonica · instrumental · 3:00*

Style prompt:
```
Solo glass harmonica in F Lydian opening a fragile weightless theme — the world's first music heard from far away — answered by harp, celesta and a slow bed of high divisi strings. 54 BPM, free time, no strong pulse. Vast crystalline cathedral reverb, soft attacks. Lush pastoral fantasy prelude in the tradition of Jeremy Soule's Oblivion and Morrowind. Full-length cue, developed and through-composed, sustain and vary the theme, no early fade, long outro. No percussion, no brass, no choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no synth, no drums, no electric guitar, no EDM, no melody resolution.
```
Structure prompt:
```
[Instrumental Intro - solo glass harmonica, one rising F Lydian phrase, no pulse]
[Instrumental A - harp enters, the Song motif begins to form, weightless]
[Instrumental B - celesta and high divisi strings swell underneath, the phrase widens]
[Instrumental Development - the motif gently varied and inverted, still unresolved]
[Instrumental Peak - strings and harmonica at their fullest, reaching upward]
[Instrumental Hush - back to solo glass harmonica and one held string note]
[Instrumental Outro - the Song left one note short, very long shimmering fade]
```

---

**02 — Main Theme** *[reworked in place — D-minor theme + Lux per umbram kept, choir cut way down, Oblivion lean, flute fixed]*
*Key D minor (Aeolian) · 84 BPM · Human · strings/horns · one brief light Latin statement · 4:30*

Style prompt:
```
Lush cinematic fantasy orchestral score, epic and melancholic, D minor, 84 BPM, 4/4, in the tradition of Jeremy Soule's Oblivion and Morrowind. Sweeping legato strings, four French horns in open fifths, solo cello with warm vibrato, soaring solo violin, harp, oboe, soft frame drums and hand percussion. One brief, distant, light Latin choir statement only — far-off and mezzo-piano, never a battle chant or grand anthem. Distant wooden flute playing simple steady stepwise lines in a fixed register. Modal Aeolian and Dorian, never harmonic minor. Builds from a hushed ambient intro through the heroic main theme and an intimate cello response to an Oblivion-grand orchestral climax, then a quiet solo cello outro. Cathedral reverb on strings, wide dynamic range pianissimo to fortissimo, film-score quality. Full-length cue, developed and through-composed, no early fade, long outro. No drum kit, no electric guitar, no synth, no modern percussion, no English vocals, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no taiko battle chant, no rock, no EDM, no autotune.
```
Lyrics/structure prompt:
```
[Instrumental Intro - quiet ambient, distant wooden flute on steady stepwise notes, soft frame-drum heartbeat]

[Instrumental Build - frame drums establish a gentle 4/4, low strings ostinato in open fifths, French horns enter on a sustained pedal D]

[Instrumental Main Theme - violins carry the heroic melancholic melody, French horn pad underneath, frame drums steady]

[Instrumental Bridge - solo cello variation, sparse harp, briefly 6/8 time, intimate and melancholic]

[Instrumental Solo - soaring solo violin restates the main theme an octave higher, warm string pad underneath]

[Choir - one brief, light, distant Latin statement, mezzo-piano, no battle force, sung once only]

Lux per umbram
ferrum per ignem
sanguis per saecula
terra nos vocat

[Instrumental Climax - Oblivion-grand: full strings and horns restate the main theme, harp and oboe soaring over the top, frame drums lift; no taiko, no choir shout]

[Instrumental Lift - brief modulation to D major, trumpets and horns restate the theme warmly]

[Instrumental Outro - solo cello plays a three-note descending motif, distant flute and soft frame-drum heartbeat, long fade to silence]
```

---

**03 — End Credits _ The Long Twilight**
*Key B♭ Aeolian → modulating · 76 BPM · suite · solo cello · instrumental · 5:00*

Style prompt:
```
A through-composed end-credits suite starting B♭ Aeolian and modulating each section, visiting every culture's instrumental colour: solo cello (Endurance), Aelorin harp and glass harmonica, a dwarven 6/8 dulcimer-and-anvil hall, a grand brass-and-strings anthem, settling back to solo cello. 76 BPM, 4/4 and 6/8. Full orchestra, cathedral reverb, lush Oblivion-style warmth, Soule and Shore "all themes return." Full 5-minute suite, developed, no early fade, long outro. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no drum kit, no synth, no electric guitar, no EDM.
```
Structure prompt:
```
[Instrumental Intro - solo cello, the full Endurance theme in B♭ Aeolian, unaccompanied]
[Instrumental Section A - strings swell under it, soft frame drum, French horns answer]
[Instrumental Section B - modulate up; Aelorin colour: harp, glass harmonica, celesta, the Song]
[Instrumental Section C - dwarven 6/8, hammered dulcimer, anvil, low brass]
[Instrumental Section D - full orchestral anthem, the Endurance cell fortissimo]
[Instrumental Section E - a quiet reprise of the Hearth fragment on solo oboe]
[Instrumental Coda - back to solo cello, the Endurance tag, very long fade]
```

---

### B. Exploration

---

**04 — Open Road _ The Central Plains**
*Key G Mixolydian · 92 BPM · Human · oboe · instrumental · 4:00*

Style prompt:
```
Solo oboe in G Mixolydian carrying a walking melody over light pizzicato strings, harp and a soft frame drum — open, breathing, hopeful, lots of air — distant French horn pads on long notes. 92 BPM, 4/4, relaxed. Bright open-field reverb. Lush pastoral travelling music in the spirit of Jeremy Soule's Oblivion "Through the Valleys" and Morrowind, melancholic underneath. Full-length cue, multiple variations and a contrasting bridge, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no taiko, no heavy brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - solo oboe alone, free, one long G Mixolydian phrase]
[Instrumental A - pizzicato strings, harp and soft frame drum enter, oboe states the walking theme]
[Instrumental B - solo clarinet takes a variation, horn pad underneath, warmer]
[Instrumental Bridge - strings only, the melody slows, a melancholic minor turn, no percussion]
[Instrumental C - flute doubles the oboe on a steady stepwise variation, gentle lift]
[Instrumental A' - oboe returns to the walking theme, fuller strings, building]
[Instrumental Outro - pizzicato thins, solo oboe alone again, long fade on a held note]
```

---

**05 — The Greatwood _ Under Old Leaves**
*Key A Lydian · 60 BPM · Aelorin · harp + high strings · instrumental · 4:30*

Style prompt:
```
Harp arpeggios in A Lydian under shimmering high divisi strings and celesta, the Song motif rising and never quite resolving — ancient, weightless, faintly sad. Finger-struck crotales for colour, no real percussion. 60 BPM, free-floating. Very long shimmering reverb. Aelorin exploration in the lineage of Soule's Oblivion forest cues and Morrowind's quietest wilderness. Full-length cue, several motif variations, no early fade, very long outro. Instrumental. No brass, no drums, no choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no synth, no electric guitar, no EDM.
```
Structure prompt:
```
[Instrumental Intro - solo harp, slow A Lydian arpeggio, alone]
[Instrumental A - high divisi strings enter softly, the Song motif rises, unresolved]
[Instrumental B - celesta threads a counter-figure, crotales shimmer, strings double in fifths]
[Instrumental Development - the motif varied higher, the harmony opening]
[Instrumental Peak - strings at their fullest shimmer, harp cascading]
[Instrumental Hush - solo harp and one held string note]
[Instrumental Outro - the Song phrase left unfinished, very long reverb tail]
```

---

**06 — The Spine _ Stone and Sky**
*Key E Dorian · 66 BPM · Dwarven-adj. · French horn · instrumental · 4:00*

Style prompt:
```
A lone French horn in E Dorian over slow low-string swells — vast, cold, mountainous — occasional deep timpani like distant rockfall. 66 BPM, 4/4, spacious. Big open-air reverb with a long mountain slap-back. Grand and severe, Soule's Skyrim peaks crossed with Oblivion's wide vistas. Full-length cue, a canon section and a full-brass peak, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no taiko, no hand percussion, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - low-string drone in E, a single distant timpani roll]
[Instrumental A - lone French horn states a wide slow theme, low strings swell beneath]
[Instrumental B - second horn answers in canon, strings thicken, altitude rising]
[Instrumental Development - the theme expands across the brass, timpani marking scale]
[Instrumental Peak - full low brass on a sustained chord, timpani roll, then space]
[Instrumental Reprise - lone horn restates the theme, smaller]
[Instrumental Outro - one horn alone, drone underneath, long fade into wind]
```

---

**07 — The Underway _ Beneath the Mountain**
*Key C Phrygian · 54 BPM · Dwarven · contrabassoon · instrumental · 4:30*

Style prompt:
```
Contrabassoon and low strings in slow C Phrygian steps, occasional struck anvil ringing into long darkness, a deep tom marking distant time — old, deep, patient, oppressive not evil. 54 BPM, heavy 4/4. Huge cavern reverb with a very long slap. Dwarven underground, Soule's subterranean gloom. Full-length cue, a descending sequence and a deep climax, no early fade, long outro. Instrumental. No bright brass, no choir, no solo vocals, no flute, no glissando, no synth, no electric guitar, no taiko, no hand drum, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - cavern tone, one struck anvil ringing into the dark]
[Instrumental A - contrabassoon enters low and slow, C Phrygian, low strings shadow it]
[Instrumental B - the line descends step by step, a deep tom marks the depth]
[Instrumental Development - low brass joins, the descent sequenced lower, anvil at the bottom]
[Instrumental Peak - the fullest low chord, cavern ringing, then sudden space]
[Instrumental Hush - contrabassoon alone over the cavern ring]
[Instrumental Outro - one last anvil far off, long fade into stone silence]
```

---

**08 — The Ashfields _ Grey Soil**
*Key G drone (no functional harmony) · 44 BPM · Dead · cor anglais · instrumental · 4:00*

Style prompt:
```
A barely-moving detuned low-string drone centred on G, a distant solo cor anglais playing short fragments that never connect, far breath-tone winds with no pitch. 44 BPM, no real pulse, dead air. Vast empty reverb. Bleak ambient non-music — Morrowind's bleakest ash wastes, darker. Full-length cue, developing only by slow drone shifts and recurring fragments, no early fade, long outro. Instrumental. No percussion, no choir, no solo vocals, no flute glissando, no pitch sweeps, no brass section, no synth pad, no drum kit, no electric guitar, no melody resolution, no EDM.
```
Structure prompt:
```
[Instrumental Intro - low detuned string drone on G, motionless]
[Instrumental A - distant cor anglais plays a three-note fragment, stops, silence, again]
[Instrumental B - the drone shifts down a semitone, breath-tone winds far off]
[Instrumental Development - the fragment recurs transposed, never completing]
[Instrumental Sink - the drone descends, everything thinning]
[Instrumental Outro - cor anglais fragment one last time, unanswered, very long fade]
```

---

**09 — The Western Coast _ Caer Drowned**
*Key B Aeolian · 58 BPM · Sea/dead · low whistle · instrumental · 4:00*

Style prompt:
```
A low whistle keening a B Aeolian lament over slow grey string swells and a bell tolling as if underwater, a cor anglais answering. Whistle plays steady stepwise lines only. 58 BPM, 4/4 tidal and loose. Damp wide reverb, salt and stone. Mournful coastal music — Soule's melancholic coast. Full-length cue, a rising central climax and a reprise, no early fade, long outro. Instrumental. No drums, no choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no taiko, no bright brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - grey string swell in B minor, one slow bell tone as if underwater]
[Instrumental A - low whistle states a falling Aeolian lament, steady, no pitch bends]
[Instrumental B - cor anglais answers the whistle, strings thickening]
[Instrumental Development - the lament varied and rising, the bell tolls again]
[Instrumental Peak - strings reach a single aching crest]
[Instrumental Hush - back to one whistle line and the underwater bell]
[Instrumental Outro - whistle holds its last note, bell fades beneath the water, long tail]
```

---

**10 — The Copper Isles _ Salt and Sun**
*Key D Mixolydian · 104 BPM · Sailor · fiddle · instrumental · 4:00*

Style prompt:
```
A bright instrumental fiddle reel in D Mixolydian over strummed cittern, hand drum and a skipping low whistle counter-line (steady stepwise, fixed register) — sunlit, busy, a trading port that never sleeps. 104 BPM, 4/4 with a lilt. Medium warm room. The portfolio's most upbeat exploration cue, folk-forward. Full-length cue, multiple tunes, a percussion break, a final full statement, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no taiko, no heavy brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - solo fiddle kicks off a bright D Mixolydian phrase]
[Instrumental A - cittern strum and hand drum lock the groove, fiddle states the reel]
[Instrumental B - low whistle takes a second tune, fiddle drops to a counter-line]
[Instrumental C - a third related tune, both leads trading every two bars]
[Instrumental Break - hand drum and claps only, four bars]
[Instrumental Lift - full band back, double-time feel, port at full bustle]
[Instrumental Outro - one last full statement, sharp ensemble stop, short ring]
```

---

**11 — The Sorrowmarsh _ The Mud Remembers**
*Key atonal (loose E♭ center) · 40 BPM · Dead · bowed psaltery · instrumental · 4:00*

Style prompt:
```
Bowed psaltery and bowed metal scraping a slow atonal drift around E♭, no functional key, an occasional far struck bowl, breath-tone winds rising and sinking like ghost-lights. 40 BPM, no pulse. Vast haunted reverb with an unsettling ghost slap. Pure dread atmosphere — the unmaking happened here. Full-length cue, developing only by drone density and the recurring bowl, no early fade, long outro. Instrumental. No melody, no percussion groove, no choir, no solo vocals, no flute, no glissando, no brass, no synth, no electric guitar, no drum kit, no EDM, no resolution.
```
Structure prompt:
```
[Instrumental Intro - bowed psaltery, one long atonal scrape near E♭]
[Instrumental A - bowed metal joins, a far struck bowl rings once]
[Instrumental B - breath-tone winds rise like marsh-lights, no pitch]
[Instrumental Development - the drift clusters tighter, the bowl rings closer]
[Instrumental Swell - the texture briefly crowds in, then sinks back]
[Instrumental Outro - psaltery alone, one bowl far off, very long fade into still water]
```

---

**12 — The Weeping Wood _ Watched**
*Key cluster (no center, around F#) · 48 BPM · Naergrim · prepared strings · instrumental · 4:00*

Style prompt:
```
Prepared and detuned strings playing tight tone-clusters around F# that never resolve, contrabass clarinet groaning beneath, a dry bowed-metal scrape circling close — the feeling of being watched from every tree. 48 BPM, no real pulse, wrong. Close airless space, almost no reverb. Naergrim dread — alien, not loud. Full-length cue, developing by cluster density and recurring snapped harmonics, no early fade, long outro. Instrumental. No melody, no warm harmony, no percussion, no choir, no solo vocals, no flute, no glissando, no brass fanfare, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - one detuned string note bent slowly out of tune]
[Instrumental A - prepared-string cluster builds, contrabass clarinet groans under it]
[Instrumental B - a dry bowed-metal scrape circles, getting closer]
[Instrumental Development - the cluster tightens, a string snaps a harsh harmonic]
[Instrumental Crowd - the texture multiplies, pressing in]
[Instrumental Cut - everything stops at once but one detuned note]
[Instrumental Outro - that note held in the airless room, slow uneasy fade]
```

---

### C. Settlements

---

**13 — Aldenholt _ Market and Bell**
*Key C Mixolydian · 100 BPM · Human · lute + recorder · instrumental · 4:00*

Style prompt:
```
Lute and recorder trading a warm C Mixolydian tune over a tabor and tambourine, a city bell marking phrase ends — busy, friendly, background market energy. Recorder plays steady stepwise lines, fixed register. 100 BPM, 4/4 with a skip. Small warm room reverb. The largest human city at work, Soule's town themes lighter. Full-length cue, multiple tunes and a quieter bridge, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no taiko, no heavy brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - solo lute, a turning C Mixolydian figure, a single bell tone]
[Instrumental A - recorder takes the melody, tabor and tambourine set the bustle]
[Instrumental B - lute and recorder in cheerful counterpoint, the bell every four bars]
[Instrumental Bridge - just lute and tabor, quieter, a quick warm minor turn]
[Instrumental C - a second brighter tune, fiddle doubling the recorder]
[Instrumental A' - full little ensemble back, market at peak]
[Instrumental Outro - thins to solo lute and one last bell, gentle stop]
```

---

**14 — Caer Brannoch _ The Cliff City**
*Key G Dorian · 72 BPM · Human/sea · solo cello + harp · instrumental · 4:00*

Style prompt:
```
Solo cello singing a noble G Dorian melody over harp and slow string pads, a distant sea-swell in the low strings — a proud city on the cliffs, dignified and a little lonely. 72 BPM, 4/4, stately. Medium hall with an airy sea-wind tail. Aristocratic and maritime, Oblivion's noble register. Full-length cue, an octave lift and an intimate bridge, no early fade, long outro. Instrumental. No drums, no choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no taiko, no bright fanfare, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - harp alone, a slow rolling figure like the sea below]
[Instrumental A - solo cello states the noble G Dorian theme, string pad enters]
[Instrumental B - violins lift the theme an octave, harp continues, dignified swell]
[Instrumental Bridge - cello and harp alone, intimate, the lonely register]
[Instrumental Development - the theme varied, low strings suggest the sea-swell]
[Instrumental A' - full strings restate the theme, proud but restrained]
[Instrumental Outro - cello holds the last note, harp rolls once, long fade on sea-wind]
```

---

**15 — Vosskar _ Iron and Listening**
*Key F Aeolian · 64 BPM · Iron Chalice-adj. · muted trumpet · instrumental · 4:00*

Style prompt:
```
A muted trumpet in F Aeolian over spare low strings, severe and watchful, a low clarinet shadowing it — a fortress city built on silence and suspicion. 64 BPM, slow 4/4. Dry stone reverb, no warmth. Austere and martial-adjacent, restrained. Full-length cue, slow accumulation and a single restrained peak, no early fade, long outro. Instrumental. No taiko, no hand percussion, no choir, no solo vocals, no flute, no glissando, no bright brass, no synth, no electric guitar, no drum kit, no EDM, no major lift.
```
Structure prompt:
```
[Instrumental Intro - one muted trumpet note in F minor, dry, alone]
[Instrumental A - low strings enter beneath in slow F Aeolian, watchful]
[Instrumental B - a low clarinet shadows the trumpet a fifth below]
[Instrumental Development - the phrase tightens, low strings thicken, pressure without release]
[Instrumental Peak - a single restrained tutti swell, then withdrawn]
[Instrumental Hush - everything pares back to the muted trumpet — the city listening]
[Instrumental Outro - trumpet repeats its phrase once, dry stop, short tail]
```

---

**16 — Solgrade _ The Unwalled City**
*Key A Dorian · 96 BPM · Tavern/cosmo · hurdy-gurdy · instrumental · 4:00*

Style prompt:
```
A hurdy-gurdy drone and melody in A Dorian with a foreign lilt, hand percussion, plucked oud-like strings and a tambourine — a wealthy crossroads city, many cultures, slightly exotic, never threatening. 96 BPM, 4/4 with an off-beat sway. Medium lively room. Cosmopolitan market music, the most outsider-flavoured settlement cue. Full-length cue, several variations and a percussion-and-drone break, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no taiko, no heavy brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - hurdy-gurdy drone fades in, a single sustained A chord]
[Instrumental A - hurdy-gurdy melody in A Dorian, hand drum and tambourine join]
[Instrumental B - plucked oud-like strings take a variation with a foreign lilt]
[Instrumental Bridge - percussion and drone only, a swaying off-beat groove]
[Instrumental C - a second ornamented tune over the drone, busier]
[Instrumental A' - full ensemble, the first tune embellished]
[Instrumental Outro - instruments drop out one by one, hurdy-gurdy drone last, long fade]
```

---

**17 — Lirien-Thal _ The Silverwood**
*Key D♭ Lydian · 52 BPM · Aelorin · glass harmonica · instrumental · 4:30*

Style prompt:
```
Glass harmonica and harp in slow D♭ Lydian over high sustained strings and celesta — a canopy city among ancestor-trees, sacred and grieving for a fading people. 52 BPM, free. Enormous shimmering cathedral-of-leaves reverb. The Aelorin's holiest place, Oblivion's most reverent register. Full-length cue, the Song motif varied and a long unresolved climax, no early fade, very long outro. Instrumental. No percussion, no brass, no choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no drums, no synth, no electric guitar, no EDM, no sharp attack.
```
Structure prompt:
```
[Instrumental Intro - glass harmonica alone, slow D♭ Lydian, weightless]
[Instrumental A - harp and high strings enter, the Song motif rises, unresolved]
[Instrumental B - celesta laces a counter-figure, strings double in fifths]
[Instrumental Development - the motif varied higher, harmony opening]
[Instrumental Peak - strings and harmonica at their fullest, a held unresolved chord]
[Instrumental Hush - back to glass harmonica and harp]
[Instrumental Outro - the Song phrase left open, shimmer fading upward, very long tail]
```

---

**18 — Karaz-Dûn _ Forges Never Cold**
*Key B♭ Dorian · 78 BPM (6/8) · Dwarven · hammered dulcimer · instrumental · 4:00*

Style prompt:
```
Hammered dulcimer and low brass in a rolling 6/8 B♭ Dorian work-rhythm, anvils on the strong beats, boot-stomp percussion — a hold whose forges never go cold, proud and warm despite the weight. 78 BPM, 6/8, driving. Great stone-hall reverb with a long slap. Dwarven craft-pride. Full-length cue, a brass-fuller middle, a percussion break, a final statement, no early fade, long outro. Instrumental. No bright strings lead, no choir, no solo vocals, no flute, no glissando, no taiko, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - one anvil strike, then the 6/8 boot-stomp pattern alone]
[Instrumental A - hammered dulcimer states the rolling B♭ Dorian work-theme, anvils on the beats]
[Instrumental B - low brass joins the dulcimer, fuller, the forge at full heat]
[Instrumental Development - the theme varied, dulcimer trading with brass]
[Instrumental Break - anvils and stomps only, four bars]
[Instrumental A' - full ensemble back, the work-theme at its proudest]
[Instrumental Outro - dulcimer figure slows, one last anvil strike, long ring into the hall]
```

---

**19 — Mor-Vethrin _ The Obsidian City**
*Key no center (around C#) · 46 BPM · Naergrim · contrabass clarinet · instrumental · 4:00*

Style prompt:
```
Contrabass clarinet and bowed metal in a slow centreless drift around C#, struck chains and a single stone-on-stone arrhythmic pulse — a windowless obsidian city silent two thousand years. 46 BPM, no key, no real pulse. Close, airless, wrong. Alien and ancient, never bombastic. Full-length cue, developing only by texture density and recurring chains, no early fade, long outro. Instrumental. No melody, no warm harmony, no choir, no solo vocals, no flute, no glissando, no bright brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - contrabass clarinet, one long centreless note]
[Instrumental A - bowed metal joins, a chain struck once, arrhythmic stone pulse begins]
[Instrumental B - the texture thickens without resolving, chains closer]
[Instrumental Development - bowed metal layers, the stone pulse irregular and nearer]
[Instrumental Crowd - the texture presses in, chains repeated]
[Instrumental Cut - the stone pulse stops dead]
[Instrumental Outro - one clarinet note, one last chain, abrupt airless fade]
```

---

**20 — Brightwatch _ The Frontier Garrison**
*Key C Aeolian · 70 BPM · Iron Chalice · lone war horn · instrumental · 4:00*

Style prompt:
```
A lone war horn in C Aeolian over a single deep field drum and spare low strings, a cor anglais shadowing the horn — a frontier fort holding the line, weary endurance not glory. 70 BPM, slow 4/4. Dry stone, cold air. Iron Chalice austerity, Roland's home register. Full-length cue, the Endurance cell stated, varied and restated, no early fade, long outro. Instrumental. No taiko, no hand percussion, no choir, no solo vocals, no flute glissando, no pitch sweeps, no bright brass section, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - one deep field-drum hit, slow, then a lone war horn call in C minor]
[Instrumental A - low strings enter beneath in bare C Aeolian, the Endurance cell hinted]
[Instrumental B - cor anglais shadows the horn, the cell stated plainly]
[Instrumental Development - strings thicken, the cell varied, the slow tread continuing]
[Instrumental Peak - a single restrained swell, the war horn at its fullest]
[Instrumental Hush - strings hold one low chord, the drum stops]
[Instrumental Outro - the war horn calls once more, unanswered, long dry fade]
```

---

### D. Interiors & Sacred

---

**21 — The Archive _ Dust and Lamplight**
*Key A static modal (no melody) · 50 BPM · Human/near-non-music · bowed vibraphone · instrumental · 5:00*

Style prompt:
```
Almost non-music: a static modal hum on A of bowed vibraphone and sustained low strings, a distant bell every minute, faint room tone — a vast library, lamplight, dust. 50 BPM, no pulse, no melody. Dry interior with a faint long resonance. Ambient texture, Oblivion's quiet interiors; designed to disappear. Full 5-minute bed, developing only by near-imperceptible harmonic shifts, no early fade, very long outro. Instrumental. No percussion, no choir, no solo vocals, no flute, no glissando, no brass, no melodic line, no synth pad, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - bowed vibraphone, one slow shimmering A tone]
[Instrumental A - sustained low strings join very quietly, a distant single bell]
[Instrumental B - the harmony shifts once, almost imperceptibly]
[Instrumental Drift - room tone, a far bell again, the hum unchanged]
[Instrumental C - a second barely-there harmonic shift, slightly warmer]
[Instrumental Drift 2 - the bell once more, the hum settling back]
[Instrumental Outro - thins to a single held vibraphone tone, very long slow fade]
```

---

**22 — The Iron Chalice _ Chapel of Endurance**
*Key E Aeolian · 56 BPM · Iron Chalice · organ + low strings · instrumental · 4:00*

Style prompt:
```
A church organ and low strings in solemn E Aeolian stating the Endurance cell as a hymn, a muted trumpet doubling at the peak — austere, devotional. 56 BPM, slow 4/4. Stone-chapel reverb. The Iron Chalice's theological core; a primary motif anchor — keep the Endurance melody clean and central. Full-length cue, hymn / organ-full variation / reprise, no early fade, long outro. Instrumental. No taiko, no hand percussion, no choir, no solo vocals, no flute, no glissando, no bright brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - organ alone, a low sustained E Aeolian chord]
[Instrumental A - organ states the Endurance cell as a hymn line]
[Instrumental B - low strings join, the cell harmonised in bare octaves]
[Instrumental Development - the hymn varied, organ registration widening]
[Instrumental Swell - organ full, muted trumpet doubles the Endurance cell fortissimo]
[Instrumental Hush - sudden drop to organ pedal and one low string note]
[Instrumental Outro - the Endurance tag once more on the organ, long slow stone fade]
```

---

**23 — The Aeluvain _ The Song With an Edge**
*Key E Lydian (unresolved) · 58 BPM · Aelorin · solo violin harmonics · instrumental · 4:00*

Style prompt:
```
Solo violin in high natural harmonics, glass harmonica and celesta circling the Song motif in E Lydian but never closing it — a sword that is a piece of the world's first music, beautiful and faintly painful. 58 BPM, free. Vast crystalline reverb. A motif anchor: the Song / Eighth Star in its purest form. Full-length cue, stated / varied / reaching / left one note short, no early fade, very long outro. Instrumental. No percussion, no brass, no choir, no solo vocals, no flute, no glissando, no drums, no synth, no electric guitar, no EDM, no resolution.
```
Structure prompt:
```
[Instrumental Intro - solo violin harmonic, one pure high note, hanging]
[Instrumental A - glass harmonica enters, the Song motif begins to form, E Lydian]
[Instrumental B - celesta doubles the violin, the phrase widens]
[Instrumental Development - the motif varied higher, harmony opening, no cadence]
[Instrumental Peak - violin and harmonica reach for the final note and stop one step short]
[Instrumental Hush - violin and glass harmonica alone on a held harmonic]
[Instrumental Outro - the Song deliberately unfinished, shimmering, very long fade]
```

---

**24 — The Crown Assembled _ Seven Metals**
*Key chromatic (C center) · 64 BPM · mixed · seven timbres · brief optional choir · 4:00*

Style prompt:
```
Seven distinct timbres enter one at a time over a chromatic low drone on C — low strings, struck metal, horn, harp, glass harmonica, anvil, contrabass clarinet — the seven-note Crown cell assembling, awe shot through with dread. A brief light massed choir is OPTIONAL at the climax only and may be omitted for fully instrumental; never solo. 64 BPM, slow 4/4. Huge cold reverb. Through-composed. Full-length cue, no early fade, long outro. No solo vocals, no sung verses, no flute, no glissando, no drum kit, no synth, no electric guitar, no EDM.
```
Structure prompt:
```
[Instrumental Intro - chromatic low drone on C, one struck metal tone — iron]
[Instrumental Build 1 - horn adds a note of the Crown cell, then harp]
[Instrumental Build 2 - glass harmonica, anvil, contrabass clarinet each add a note]
[Instrumental Development - the partial Crown cell circles, brittle, gathering]
[Choir - OPTIONAL brief massed texture, no solo, may be omitted entirely]

sanguis per saecula

[Instrumental Climax - all seven timbres sound the full Crown cell at once, vast and brittle]
[Instrumental Outro - everything snaps off but the obsidian clarinet, it bends down alone, cold fade]
```

---

### E. Tavern & Folk (instrumental)

---

**25 — Tavern _ The Limping Reel**
*Key A Mixolydian · 132 BPM · Folk · fiddle · instrumental · 4:00*

Style prompt:
```
A fast instrumental fiddle reel in A Mixolydian with a deliberate limp in the rhythm, lute, hand drum, foot-stomps and claps — a packed tavern, ale, bad dancing. 132 BPM, 4/4 with a dropped beat every phrase. Small warm boozy room. Diegetic folk, deliberately unpolished. Full-length cue, three linked tunes with a stomp break and a wild final round, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no orchestra, no taiko, no brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - fiddle scrapes off a fast A Mixolydian reel, foot-stomp sets the limp]
[Instrumental A - lute and hand drum lock in, fiddle states the first tune]
[Instrumental B - a second tune, fiddle wilder, claps doubling]
[Instrumental C - a third related tune, lute taking the lead]
[Instrumental Break - stomps and claps only, four bars]
[Instrumental A' - first tune back, full and frantic, double-time feel]
[Instrumental Outro - one ragged ensemble stop, a single fiddle flourish, short ring]
```

---

**26 — The Widow's Lament _ A Quiet Room**
*Key F Dorian · 68 BPM · Folk · lute + viola · instrumental · 4:00*

Style prompt:
```
A lone lute and a solo viola in slow F Dorian, plain and unhurried — a quiet tavern gone still, a melody that grieves without a word. 68 BPM, free rubato, no percussion. Intimate close room, almost no reverb. Diegetic instrumental lament, the sad counterpart to the reel. Full-length cue, stated by lute, answered by viola, varied, reprised lower, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute, no glissando, no orchestra, no drums, no brass, no synth, no electric guitar, no EDM.
```
Structure prompt:
```
[Instrumental Intro - solo lute, a slow falling F Dorian figure]
[Instrumental A - solo viola enters, states the lament over the lute]
[Instrumental B - lute and viola trade the phrase, no other instrument]
[Instrumental Development - the air varied, the viola lower and slower]
[Instrumental Hush - viola alone for one phrase, unaccompanied]
[Instrumental A' - lute returns under the viola, the lament one last time]
[Instrumental Outro - the final note left unresolved, lute fading, silence]
```

---

**27 — The Deep Cups _ A Dwarven Dance**
*Key C Dorian · 88 BPM (6/8) · Dwarven · dulcimer + anvil · instrumental · 4:00*

Style prompt:
```
A roaring instrumental dwarven dance in 6/8 C Dorian: hammered dulcimer and low brass on the tune, anvil and tankard-on-table percussion, boot-stomps — ale, defiance, joy that sounds like a war chant with no words. 88 BPM, 6/8, heavy swing. Big stone-hall reverb. Diegetic and proud. Full-length cue, several rowdy variations, a stomp break, a final round, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute, no glissando, no strings lead, no taiko, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - tankards pounded on the table set the 6/8, one anvil clang]
[Instrumental A - hammered dulcimer states the rowdy C Dorian tune, anvil on the beat]
[Instrumental B - low brass doubles the tune, louder, fuller]
[Instrumental C - a second variation, dulcimer trading with brass]
[Instrumental Break - stomps and tankard hits only, four bars]
[Instrumental A' - the tune back at its rowdiest, everyone in]
[Instrumental Outro - one last anvil clang, a final dulcimer flourish, a roar of the hall]
```

---

**28 — The Dockside _ Salt and Strings**
*Key E Mixolydian · 120 BPM · Sailor · concertina · instrumental · 4:00*

Style prompt:
```
A driving instrumental concertina and fiddle jig in E Mixolydian, hand drum and a boot on the deck, a low whistle counter-line (steady stepwise, fixed register) — a portside tavern, salt, smoke, sailors home for a night. 120 BPM, 4/4 with a roll. Medium boozy room with a little wood ring. Diegetic sea-folk. Full-length cue, several tunes and a percussion break, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no orchestra, no taiko, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - concertina kicks off in E Mixolydian, a boot stamps the deck-time]
[Instrumental A - fiddle joins, hand drum locks the groove, concertina states the jig]
[Instrumental B - low whistle takes a second tune, concertina dropping to chords]
[Instrumental C - a third tune, fiddle and concertina trading]
[Instrumental Break - deck-stomp and hand drum only, four bars]
[Instrumental A' - full band back, the jig at full bustle]
[Instrumental Outro - concertina holds a chord, one last fiddle flourish, short ring]
```

---

**29 — The Hearth _ An Aelorin Air**
*Key B Lydian · 56 BPM · Aelorin · harp + celesta · instrumental · 4:00*

Style prompt:
```
A single Aelorin harp and celesta in gentle B Lydian, a soft clarinet taking the tune (steady stepwise, no pitch bends) — not a grand cue but a hearth air, the rare warm small-scale Aelorin register. 56 BPM, free, no percussion. Soft medium reverb, intimate not vast. The folk counterpart to the grand Aelorin cues. Full-length cue, stated / answered / varied / reprised, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no orchestra, no brass, no drums, no synth, no electric guitar, no EDM.
```
Structure prompt:
```
[Instrumental Intro - solo Aelorin harp, a slow tender B Lydian figure]
[Instrumental A - a soft clarinet states an intimate melody over the harp]
[Instrumental B - celesta answers the clarinet phrase for phrase, like two by a fire]
[Instrumental Development - the air gently varied, harp arpeggios widening]
[Instrumental Hush - clarinet alone for one phrase]
[Instrumental A' - harp and celesta return under the clarinet, warmer]
[Instrumental Outro - harp holds the last chord, the air fading on an open note]
```

---

### F. Sea (instrumental)

---

**30 — The Capstan _ Heave Her Round**
*Key D Dorian · 96 BPM · Sailor · hand drum + low whistle · instrumental · 4:00*

Style prompt:
```
An instrumental work-rhythm in D Dorian: a hand drum and the rhythmic creak of rope and capstan keeping the pull, a low whistle and fiddle stating a muscular tune on the heave (whistle steady stepwise, fixed register). 96 BPM, 4/4, every other bar the haul. Open deck, salt-air, little reverb. Sailor's Guild labour music, strophic but developed. Full-length cue, slow start to full effort and easing off, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no orchestra, no taiko, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - rope creak and a slow hand drum, the capstan starting to turn]
[Instrumental A - low whistle states the muscular D Dorian tune on the heave]
[Instrumental B - fiddle joins, the pull settles into rhythm, drum steadier]
[Instrumental Development - the tune varied, the work harder, percussion fuller]
[Instrumental Lift - everything at full effort, the anchor breaking free]
[Instrumental Ease - the pull slows, drum thinning]
[Instrumental Outro - the capstan stops, rope settles, one last whistle note, silence]
```

---

**31 — Leaving Port _ The Tide Turns**
*Key B♭ Mixolydian · 84 BPM (6/8) · Sailor · fiddle + low whistle · instrumental · 4:00*

Style prompt:
```
A hopeful instrumental sea-air in B♭ Mixolydian: low whistle and fiddle over rolling 6/8 strings, gulls and a far harbour bell (whistle steady stepwise, fixed register, no bends) — a ship leaving harbour, the bittersweet lift of departure. 84 BPM, 6/8, rolling like a wake. Medium open reverb, salt-air. The optimistic sea cue. Full-length cue, stated / lifted an octave / varied / reprised, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no taiko, no heavy brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - low whistle alone, a rising B♭ Mixolydian phrase, a far harbour bell]
[Instrumental A - rolling 6/8 strings enter, fiddle joins the whistle, the ship pulls away]
[Instrumental B - fiddle takes the tune up an octave, fuller, hopeful]
[Instrumental Development - the air varied, strings swelling, land falling behind]
[Instrumental Peak - whistle and fiddle together at the swell's crest]
[Instrumental Hush - back to low whistle and one string line]
[Instrumental Outro - whistle alone again, the bell once more, long fade on open water]
```

---

**32 — At Sea _ Open Water** *[supersedes existing `Sea _ Sailing`]*
*Key B Dorian · 70 BPM · Sailor · accordion + cello · instrumental · 4:30*

Style prompt:
```
A slow majestic B Dorian theme on accordion answered by solo cello over long rolling string swells — no crew, no work, a ship alone on a vast calm sea, grand and a little lonely. 70 BPM, 4/4, tidal and broad. Wide open-ocean reverb. The cinematic sailing cue; replaces the old generic sea track, Oblivion-broad. Full-length cue, stated / answered / lifted by full strings / reprised intimately, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute, no glissando, no taiko, no drum kit, no synth, no electric guitar, no EDM.
```
Structure prompt:
```
[Instrumental Intro - long low string swell in B minor, the sea breathing]
[Instrumental A - accordion states a broad B Dorian theme, unhurried]
[Instrumental B - solo cello answers the accordion phrase, strings swell under both]
[Instrumental Development - the theme varied, the horizon widening]
[Instrumental Build - the full string section lifts the theme, grand, open water]
[Instrumental Hush - back to accordion and one cello line, the loneliness of it]
[Instrumental Outro - cello holds the last note, the swell recedes, very long fade]
```

---

**33 — The Shroud _ The Storm That Never Ends**
*Key octatonic (F center) · 72→132 BPM · Mordvar-adj. · full orch + battery · instrumental · 4:00*

Style prompt:
```
An octatonic storm centred on F: churning low strings, dissonant brass stabs, a war battery of toms and timpani building from a heave to a frenzy — the permanent storm that swallows every ship. Starts 72 BPM, accelerates to 132. 4/4 into chaos. Vast wet roaring reverb. The sea as enemy, no resolution. Fully instrumental — terror carried by orchestra and battery, no voices. Full-length cue, long build, sustained frenzy, unresolved tail, no early fade. No choir, no solo vocals, no flute, no glissando, no folk instruments, no synth, no electric guitar, no drum kit, no EDM, no triumph.
```
Structure prompt:
```
[Instrumental Intro - low strings churn octatonic on F, distant timpani, 72 BPM, dread building]
[Instrumental Build - dissonant brass stabs, toms enter, tempo creeps up]
[Instrumental Surge - strings and brass climbing, the storm taking the ship]
[Instrumental Storm - 132 BPM, full battery, brass screaming octatonic, total chaos]
[Instrumental Peak - the chaos at its widest, no melody, only force]
[Instrumental Cutoff - a single brass note left ringing in the wet dark]
[Instrumental Outro - low string churn returns, unresolved, the storm goes on, long fade]
```

---

**34 — The Eastern Crossing _ Into the Storm**
*Key G Aeolian → modulating · 80 BPM · mixed (epic) · full orch · brief optional choir · 5:00*

Style prompt:
```
The grand crossing: the Endurance cell on full strings and horns in G Aeolian against an octatonic storm, modulating upward toward defiant resolve. A brief light massed choir is OPTIONAL at the climax only and may be omitted for fully instrumental; never solo. 80 BPM, 4/4, broad and building. Huge cinematic reverb. Through-composed set-piece, Soule's largest seafaring scale; ends resolved but costly. Full 5-minute cue, no early fade, long outro. No solo vocals, no sung verses, no flute glissando, no pitch sweeps, no drum kit, no synth, no electric guitar, no EDM.
```
Structure prompt:
```
[Instrumental Intro - low storm churn under a lone horn stating the Endurance cell in G minor]
[Instrumental A - full strings take the Endurance theme, defiant against the rising sea]
[Instrumental B - the storm surges, brass and battery, the strings push through, key lifts]
[Instrumental Development - Endurance and storm traded, modulating higher]
[Choir - OPTIONAL brief massed swell at the climax, no solo, may be omitted]

terra nos vocat

[Instrumental Climax - full orchestra, the Endurance cell fortissimo, the crossing made]
[Instrumental Cost - sudden hush, solo cello alone with the Endurance tag]
[Instrumental Outro - strings return softly, resolved but weary, long fade]
```

---

### G. Camp & Rest (instrumental)

---

**35 — Campfire _ The Sound of Rest**
*Key F Mixolydian · 60 BPM · Folk/intimate · solo lute-guitar · instrumental · 4:00*

Style prompt:
```
A solo lute-guitar, finger-picked, in gentle F Mixolydian, the Hearth motif stated plainly and completely — the only fully-resolved theme in the score — a soft clarinet answering (steady stepwise, no bends). 60 BPM, free, no percussion. Very close intimate reverb, fire-side. A motif anchor; designed to feel safe, Oblivion's camp warmth. Full-length cue, stated / answered / gently varied / reprised, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no orchestra, no drums, no brass, no synth, no electric guitar, no EDM.
```
Structure prompt:
```
[Instrumental Intro - solo lute-guitar, a quiet finger-picked F Mixolydian figure]
[Instrumental A - the Hearth motif stated plainly, warm, complete]
[Instrumental B - a soft clarinet answers the Hearth phrase, then is gone]
[Instrumental Development - the lute varies the motif gently, unhurried]
[Instrumental C - clarinet and lute together once, the warmest moment]
[Instrumental A' - lute alone again, the Hearth motif, a touch slower]
[Instrumental Outro - the last chord allowed to ring, fire-close, long gentle fade]
```

---

**36 — Night Rest _ Sleeping Under Stars**
*Key G Lydian · 48 BPM · intimate · music box + harp · instrumental · 4:30*

Style prompt:
```
A music box and harp in slow G Lydian, a single sustained string note far underneath like a held breath — barely music, the sound of sleep under an open sky. 48 BPM, no pulse, no melody to follow. Tiny "music box" close reverb over a vast soft tail. The quietest cue in the score; it should almost disappear. Full-length cue, developing only by simplification and drift, no early fade, very long outro. Instrumental. No percussion, no choir, no solo vocals, no flute, no glissando, no brass, no synth, no electric guitar, no drum kit, no EDM, no build.
```
Structure prompt:
```
[Instrumental Intro - music box alone, a slow G Lydian turning figure]
[Instrumental A - harp doubles it very softly, a low string note holds underneath]
[Instrumental B - the figure simplifies, fewer notes, slower]
[Instrumental Drift - music box only, winding down, almost stopping]
[Instrumental C - harp returns once, the figure barely there]
[Instrumental Outro - one last music-box note, the low string note fades after it, very long tail]
```

---

**37 — The Quiet After _ Wounds and Breath**
*Key D Aeolian* (deliberate Main Theme callback) · 52 BPM · Iron Chalice-adj. · solo cello · instrumental · 4:00*

Style prompt:
```
A solo cello alone in D Aeolian — deliberately the Main Theme's key, the Endurance cell played as exhaustion not heroism — one low sustained string note for a floor. The single intentional D-minor callback: the hero's theme, emptied of triumph. 52 BPM, rubato, no percussion. Dry close room, a little air. The decompression cue, silence-adjacent. Full-length cue, stated tired / sinking / paused / barely restated, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute, no glissando, no brass, no drums, no synth, no electric guitar, no EDM, no swell.
```
Structure prompt:
```
[Instrumental Intro - solo cello in D minor, one long breath of a note, alone]
[Instrumental A - the Endurance cell played slowly, tired, not triumphant]
[Instrumental B - a low sustained D string note enters as a floor, the cello sinks lower]
[Instrumental Development - the cell barely varied, even slower]
[Instrumental Hush - the cello stops; only the low note and room air]
[Instrumental C - cello returns for one faint phrase]
[Instrumental Outro - one final cello note, unresolved, allowed to die away, long tail]
```

---

### H. War & Combat

---

**38 — Enemies Gathering Strength _ The Muster of the Hand**
*Key octatonic (B♭ center) · 60→88 BPM · Mordvar · low brass · instrumental · 4:00*

Style prompt:
```
A slow octatonic dread-build centred on B♭: a single low brass note, a distant war drum that multiplies, low strings accreting beneath, tempo creeping 60 to 88 — not a battle, the patient assembly of something terrible. 4/4, relentless acceleration. Vast cold reverb. Menace by accumulation, never release. Fully instrumental, no voices. Full-length cue, long accumulation to a poised unresolved peak, no early fade, long uneasy outro. No choir, no solo vocals, no flute, no glissando, no folk, no bright brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - one low brass note on B♭, 60 BPM, a single far war drum]
[Instrumental Build - more drums answer from further off, the brass note bends down]
[Instrumental A - tempo 72, low strings add an octatonic ostinato]
[Instrumental B - tempo 80, brass stacks, drums closing in, the mass growing]
[Instrumental Development - the ostinato thickens, no melody, only pressure]
[Instrumental Peak - 88 BPM, full low brass and battery, massed and waiting]
[Instrumental Outro - it does not resolve; it simply stops, poised — long uneasy fade]
```

---

**39 — A Minor Skirmish _ Blades in the Brush**
*Key G Phrygian · 116 BPM · Iron Chalice-adj. · low strings ostinato · instrumental · 3:30*

Style prompt:
```
A tight G Phrygian low-string ostinato, a snare-less field drum, short stabbing horn figures — a brief contained fight, no glory, lean and nervy. 116 BPM, 4/4. Dry medium room. A 3:30 combat texture with two escalations and a quick comedown; deliberately not a set-piece. Full-length, no early fade. Instrumental. No choir, no solo vocals, no flute, no glissando, no taiko, no big brass theme, no synth, no electric guitar, no drum kit, no EDM, no triumphant climax.
```
Structure prompt:
```
[Instrumental Intro - low-string G Phrygian ostinato starts immediately, no ramp]
[Instrumental A - field drum enters, short horn stabs punctuate, tension tight]
[Instrumental B - the ostinato shifts up a step, strings sharper, the fight quickens]
[Instrumental Escalation - a second gear, horn stabs doubling, drum harder]
[Instrumental Peak - one hard tutti hit, then the ostinato alone, thinning]
[Instrumental Comedown - drum drops out, ostinato slowing]
[Instrumental Outro - the ostinato stops mid-phrase — it's over, short ring]
```

---

**40 — Charge Into Battle _ Sound the Horns**
*Key E♭ Mixolydian · 152 BPM · Human · war horns + trumpets · instrumental · 4:00*

Style prompt:
```
War horns and trumpets blazing the Endurance cell in bright E♭ Mixolydian, full strings galloping, timpani and frame drums hammering a charge. 152 BPM, 4/4, headlong. Big heroic field reverb. Pure forward momentum — Soule's heroic brass at full gallop, no voices. Full-length cue, two charge waves with a brief regroup and a bigger final wave, no early fade, hard final tag. Instrumental. No choir, no solo vocals, no flute, no glissando, no drum kit, no synth, no electric guitar, no EDM, no slow section, no minor wallow.
```
Structure prompt:
```
[Instrumental Intro - a single rising war-horn call, then the full battery slams in at 152]
[Instrumental A - trumpets blaze the Endurance cell in E♭, strings gallop beneath]
[Instrumental B - horns answer the trumpets in canon, the charge accelerating feel]
[Instrumental Regroup - a two-bar drop to drums and low strings, tension coiling]
[Instrumental Final Wave - everything at once, the Endurance cell fortissimo]
[Instrumental Peak - trumpets and horns together, the line breaking through]
[Instrumental Outro - one last horn blast and a hard tutti stop, short ring — no comedown]
```

---

**41 — The Large Battle _ The Field of Iron**
*Key E Aeolian → octatonic · 96→168 BPM · mixed (suite) · full orch + battery · brief optional choir · 5:00*

Style prompt:
```
A full battle suite: the Endurance cell (E Aeolian) versus the Hollowing (octatonic) traded across the orchestra, taiko and dhol war battery, tempo gears 96 to 168, a desperate mid-battle hush, then a brutal return. A brief light massed choir is OPTIONAL at the collision only and may be omitted for fully instrumental; never solo. 4/4 through 6/8. Vast cinematic reverb. Through-composed, the score's biggest set-piece. Full 5-minute cue, no early fade, long outro. No solo vocals, no sung verses, no flute glissando, no pitch sweeps, no drum kit, no synth, no electric guitar, no EDM, no clean victory.
```
Structure prompt:
```
[Instrumental Intro - distant battery, the Endurance cell on horns in E minor, 96 BPM]
[Instrumental A - strings and brass press the Endurance theme, the lines meet]
[Instrumental B - the Hollowing answers, octatonic brass, tempo 132]
[Instrumental Hush - sudden near-silence, a solo cello plays the Endurance tag, the cost]
[Choir - OPTIONAL brief massed texture at the return, no solo, may be omitted]

terra nos vocat / nihil nos tenet

[Instrumental Return - 168 BPM, full battery, both themes colliding, total force]
[Instrumental Cutoff - a single timpani roll cut dead]
[Instrumental Outro - solo cello, the Endurance tag unfinished, smoke clearing, long fade]
```

---

**42 — The Siege _ Hold the Walls**
*Key E Phrygian · 100 BPM · Dwarven · anvils + low brass · instrumental · 4:30*

Style prompt:
```
A defensive grind: anvils and low brass in heavy E Phrygian, a relentless boot-stomp like ram-blows on a gate — attrition, walls, holding not charging. 100 BPM, heavy 4/4, no acceleration, just endurance. Great stone-hall reverb with a long slap. Distinct from the charge: this digs in. Fully instrumental, no voices. Full-length cue, assault / lull / harder assault / hold, no early fade, long outro. No choir, no solo vocals, no flute, no glissando, no bright trumpets, no taiko frenzy, no synth, no electric guitar, no drum kit, no EDM, no rout.
```
Structure prompt:
```
[Instrumental Intro - one massive low-brass hit like a ram on the gate, then the stomp begins]
[Instrumental A - anvils mark the beat, low brass states a grim E Phrygian figure]
[Instrumental B - the ram-blows quicken, the figure tightens, the wall strains]
[Instrumental Lull - one breath where the stomp stops — between assaults]
[Instrumental Return - the stomp slams back harder, low brass fuller, the line holds]
[Instrumental Development - the figure sequenced, the assault relentless]
[Instrumental Outro - the ram-blows slow, one last anvil rings over the held hall, long tail]
```

---

**43 — Vaeroth the Pale _ The Hierarch**
*Key whole-tone (D center) · 108 BPM · Mordvar · contrabassoon · instrumental · 4:00*

Style prompt:
```
A cold whole-tone boss theme on D: contrabassoon and muted low brass over a precise mechanical pulse — a brilliant, fragile zealot, not a brute; menace is intellect and certainty. 108 BPM, 4/4, clinical. Large cold reverb. Distinct from Mordvar (slow, vast) and the Ashlord (tragic) — Vaeroth is sharp and exact. Fully instrumental, no voices. Full-length cue, stated / tightened / climaxed / fractured, no early fade, long outro. No choir, no solo vocals, no flute, no glissando, no warm strings, no folk, no triumphant brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - a precise mechanical low pulse, contrabassoon enters whole-tone on D, cold]
[Instrumental A - muted low brass states Vaeroth's clipped theme, exact, controlled]
[Instrumental B - the pulse tightens, brittle whole-tone string shimmer, pressure rising]
[Instrumental Development - the theme sequenced upward, mechanical and relentless]
[Instrumental Climax - brass snaps to full force, still controlled, then a fracture]
[Instrumental Cutoff - the mechanical pulse stutters and stops — the fragility shows]
[Instrumental Outro - contrabassoon alone, one bent note, long cold fade]
```

---

**44 — The Ashlord _ The Mask of Caerith**
*Key B Lydian → cluster · 92 BPM · Naergrim/Aelorin · corrupted glass harmonica · instrumental · 4:30*

Style prompt:
```
A tragedy wearing armour: the Aelorin Song motif on glass harmonica in B Lydian, beautiful for two phrases, then rotting into Naergrim clusters and detuned strings — a Second Age Vigil-Keeper turned; the music remembers what he was. 92 BPM, 4/4 decaying into no pulse. Vast reverb curdling to airless close. The most tragic villain cue; never purely monstrous. Fully instrumental, no voices. Full-length cue, beauty / turn / warped return / collision, no early fade, long outro. No choir, no solo vocals, no flute, no glissando, no taiko frenzy, no folk, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - the Aelorin Song motif, glass harmonica, achingly beautiful, B Lydian]
[Instrumental A - high strings carry the Song, pure, for one more phrase]
[Instrumental Turn - the harmony curdles, strings detune, the Song warps toward cluster]
[Instrumental B - the Song returns warped, low brass beneath it, grief and menace at once]
[Instrumental Development - the beauty fights the rot, neither winning]
[Instrumental Climax - the two collide, full and dissonant, the mask holding]
[Instrumental Cutoff - everything drops to one detuned harmonic — what's left of him]
[Instrumental Outro - a single broken fragment of the Song, unresolved, long airless fade]
```

---

**45 — Mordvar _ The Hollowing**
*Key octatonic (A center) · 50 BPM · Mordvar · dissonant low brass · brief optional choir · 4:30*

Style prompt:
```
The franchise's dark anchor: the Song motif inverted and emptied — descending open fifths on dissonant low brass and contrabassoon, octatonic on A, that refuse to close, war battery felt more than heard. A brief slow massed choir is OPTIONAL at the widest point only and may be omitted for fully instrumental; never solo. 50 BPM, immense 4/4, glacial. Enormous airless reverb. A motif anchor — keep the Hollowing's inverted shape exact. Full-length cue, no early fade, very long outro. No solo vocals, no sung verses, no flute, no glissando, no folk, no bright brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - one immense low-brass open fifth on A, descending, refusing to resolve]
[Instrumental A - contrabassoon states the Hollowing — the Song inverted and emptied]
[Instrumental B - the descending fifths stack, war battery felt under the floor]
[Instrumental Development - the inversion sequenced lower, octatonic, airless]
[Choir - OPTIONAL brief slow massed texture at the widest point, no solo, may be omitted]

nihil nos tenet

[Instrumental Climax - the full Hollowing fortissimo, the widest point, then nothing]
[Instrumental Cutoff - total silence for a beat]
[Instrumental Outro - one low note bends downward forever, airless, very long fade]
```

---

**46 — The Fighting Retreat _ The Ashfields**
*Key F# Aeolian · 116 BPM · Iron Chalice · war horn + strings · instrumental · 4:00*

Style prompt:
```
Heroic loss: the Endurance cell on a strained war horn in F# Aeolian over driving strings and a hard field-drum tread — a retreat that is also a victory, ground given so people live. 116 BPM, 4/4, urgent but disciplined, never a rout. Big cold field reverb. Defiant melancholy. Fully instrumental, no voices. Full-length cue, the cell under pressure, a near-break, a defiant restatement, a recede, no early fade, long outro. No choir, no solo vocals, no flute, no glissando, no triumphant fanfare, no taiko frenzy, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - hard field-drum tread, a strained war-horn call in F# minor, no triumph]
[Instrumental A - driving F# Aeolian strings, the Endurance cell on the horn under pressure]
[Instrumental B - the strings press harder, the tread quickens, discipline not panic]
[Instrumental Near-break - one bar where it nearly fails — solo horn alone, then strings catch it]
[Instrumental Defiance - the Endurance cell restated, the line still ordered, falling back]
[Instrumental Development - the cell varied lower, the tread relentless]
[Instrumental Outro - the tread recedes into distance, horn last, long fade — they got out]
```

---

**47 — The Last Stand _ No Ground Behind**
*Key C Aeolian → C Mixolydian · 84→144 BPM · Iron Chalice/Human · full orch · brief optional choir · 4:30*

Style prompt:
```
From dread to defiance: a low C Aeolian dread-bed and a slow Endurance statement that gathers the full orchestra, accelerating 84 to 144 as it modulates to C Mixolydian — backs to the wall, then everything given at once. A brief light massed choir is OPTIONAL at the climax only and may be omitted for fully instrumental; never solo. 4/4. Vast cinematic reverb. Through-composed; starts in despair. Full-length cue, long build, full climax, withheld outcome, no early fade, long outro. No solo vocals, no sung verses, no flute glissando, no pitch sweeps, no drum kit, no synth, no electric guitar, no EDM.
```
Structure prompt:
```
[Instrumental Intro - low C Aeolian dread-bed, 84 BPM, a lone cello with the Endurance tag]
[Instrumental A - strings gather under it, drums enter slow, the orchestra low and heavy]
[Instrumental Build - tempo lifts, the Endurance cell strengthening]
[Instrumental Turn - modulation to C Mixolydian, 144 BPM, defiance breaking through]
[Choir - OPTIONAL brief massed swell at the climax, no solo, may be omitted]

terra nos vocat

[Instrumental Climax - full orchestra, the Endurance cell at full cry]
[Instrumental Cutoff - one tutti chord held, then cut — outcome unsaid]
[Instrumental Outro - a single horn holds the Endurance tag over silence, long fade]
```

---

**48 — The Muster of the Alliance _ Many Banners**
*Key D Mixolydian (modulating) · 100 BPM · mixed (suite) · rotating culture leads · instrumental · 5:00*

Style prompt:
```
A muster suite in D Mixolydian where each culture's instrumental palette enters in turn and then layers — human horns, Aelorin glass harmonica, dwarven 6/8 dulcimer and anvil, sea concertina — all converging on the Endurance cell, modulating up at the convergence. 100 BPM, 4/4 over 6/8. Huge field reverb. Through-composed, every palette distinct then unified, no voices. Full 5-minute cue, no early fade, long outro. No choir, no solo vocals, no flute glissando, no pitch sweeps, no drum kit, no synth, no electric guitar, no EDM.
```
Structure prompt:
```
[Instrumental Intro - lone human war horn states the Endurance cell in D over a field drum]
[Instrumental Human - horns and strings take it, banners of the kingdoms]
[Instrumental Aelorin - glass harmonica and high strings layer the Song motif over it]
[Instrumental Dwarven - 6/8 hammered dulcimer and anvil join, the holds answer]
[Instrumental Sea - a concertina figure rides in over the top]
[Instrumental Convergence - all palettes lock onto the Endurance cell, modulating up]
[Instrumental Peak - full orchestra unified, the banners together]
[Instrumental Outro - the leads peel away to the lone war horn that began it, long proud fade]
```

---

### I. Cinematic & Story (instrumental)

---

**49 — The Vigil _ The Night Before** *[authored exception — the one solo-vocal ballad]*
*Key A Aeolian · 100 BPM (6/8) · Folk ballad · solo folk voice + harp · LEAD VOCAL · 3:45*

A diegetic folk lament sung around the last fire the night before battle, in
the spirit of the Game of Thrones song "Jenny of Oldstones" — slow compound
6/8, fragile unadorned voice, harp, building strings. Original lyrics telling
this world's history (the fallen star and the forged blade, the three peoples
marching to the marsh, the enemy who only lay still and gathers still, the
muster at dawn).

Style prompt:
```
A slow, plaintive medieval folk ballad in the spirit of the Game of Thrones song "Jenny of Oldstones" — a single fragile, warm, unadorned solo voice (female, or a plain unforced male) over fingered harp, with a slow string section that swells and recedes. A Aeolian, compound 6/8 at ~100 BPM, deeply melancholic and intimate. No percussion, or only a faint distant heartbeat. The song the soldiers hear around the last fire the night before battle — grief and quiet resolve, never triumphant. Builds from solo voice and harp through swelling cellos and violins to a fuller refrain, then strips back to voice and harp for the close. Soft intimate chapel reverb. Diegetic folk lament sung in the common tongue, not orchestral score. Full-length cue, no early fade, long quiet outro. No drum kit, no taiko, no choir wall, no Latin, no synth, no electric guitar, no flute glissando, no pitch sweeps, no portamento, no EDM, no autotune.
```
Lyrics/structure prompt:
```
[Intro - solo harp, slow 6/8, a few bare falling notes, no voice yet]

[Verse 1 - solo voice, fragile, harp only]

In the old grey year when the star came down,
the smiths of the wood took its light to the ground,
and they hammered a song into edge and to name,
a blade for the hand that would carry the flame.

[Verse 2 - voice and harp, a low cello enters under the last line]

The men raised their banners, the deep-folk their stone,
the elves left their glades and they marched out alone,
to the marsh where the wide world held breath in the dark,
where one man would strike and not live past the mark.

[Refrain - the voice opens, soft strings swell in]

So bank the last embers and lay down your fear,
for the men and the elves and the deep-folk are here;
they gave us a morning they would not live to see —
sing them low, sing them low, to the cold grey sea.

[Verse 3 - strings fuller, the story darkens]

He fell with the crown lying shattered and black,
and the sea took the towers and gave nothing back,
but the dark was not ended, it only lay still,
and it learned, and it waited, and it gathers there still.

[Refrain - fuller, strings at their warmest, the voice stronger]

So bank the last embers and lay down your fear,
the men and the elves and the deep-folk are here;
we'll give them a morning we may not live to see —
sing us low, sing us low, to the cold grey sea.

[Bridge - instrumental, strings swell and ache, then thin away to harp]

[Final Verse - solo voice and harp again, quiet, resolved]

Come dawn we will stand where the old fathers stood,
with iron, with starlight, with stone and with blood,
and whatever the long night and the morning may bring,
let them say that we held, let them say that we could sing.

[Outro Refrain - voice almost a whisper over bare harp, fading]

So bank the last embers... and lay down your fear...
the three peoples keep watch... and they all of them here...
(harp alone, one last falling phrase, long fade to dark)
```

---

**50 — Heroes Reunited _ The Fellowship Whole**
*Key A♭ Mixolydian · 76 BPM · mixed (motif weave) · leitmotif weave · instrumental · 4:00*

Style prompt:
```
A motif-weave reunion in A♭ Mixolydian: the Hearth fragment opens, then the Endurance cell on solo cello, the Aelorin Song on glass harmonica, a dwarven dulcimer figure and a sea phrase all braid warmly — companions back together. 76 BPM, 4/4, glowing. Warm medium hall. The emotional pay-off cue; every theme at once, Oblivion's reunion warmth, no voices. Full-length cue, themes introduced and braided to a glowing peak, then back to the Hearth, no early fade, long outro. Instrumental. No battle battery, no choir, no solo vocals, no flute glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no dissonance.
```
Structure prompt:
```
[Instrumental Intro - the Hearth fragment, solo lute, warm and complete, A♭ Mixolydian]
[Instrumental A - solo cello adds the Endurance cell over it, gently, like a greeting]
[Instrumental B - glass harmonica laces the Aelorin Song through]
[Instrumental C - a dwarven dulcimer figure and a sea phrase join the weave]
[Instrumental Weave - all the motifs braid together, strings warm underneath]
[Instrumental Peak - the full ensemble, every theme audible at once, glowing not loud]
[Instrumental Outro - back to the Hearth fragment, lute alone, a held warm chord, long fade]
```

---

**51 — A Marriage _ Two Hands Bound**
*Key D major (Ionian)* (deliberate Main Theme parallel-major callback) · 88 BPM · Folk/Human · harp + oboe + fiddle · instrumental · 4:00*

Style prompt:
```
Unambiguous joy in D major — deliberately the parallel major of the Main Theme's D minor, the hero's key turned to light: harp, oboe and a warm fiddle dancing an Ionian processional, hand drum and a single bright bell. 88 BPM, 4/4 with a lift. Warm bright room, a hall full of people. Folk-ceremonial, light; a real wedding, not a coronation. Full-length cue, processional / dancing middle / tender bridge / glad reprise, no early fade, long outro. Instrumental. No minor wallow, no choir, no solo vocals, no flute glissando, no pitch sweeps, no Latin dirge, no taiko, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - solo harp, a bright rising D major figure, one clear bell]
[Instrumental A - oboe takes a glad processional melody, fiddle and hand drum lift it]
[Instrumental B - fiddle leads a dancing variation, the room clapping along]
[Instrumental Bridge - harp and oboe alone, tender, a held warm moment]
[Instrumental C - the processional fuller, recorder doubling the oboe steadily]
[Instrumental Reprise - the brightest statement, everyone in, glad]
[Instrumental Outro - harp and bell as at the start, a warm settled D major chord, long glad fade]
```

---

**52 — Grief _ What the Archive Lost**
*Key E♭ Aeolian · 46 BPM · Human · solo viola · instrumental · 4:00*

Style prompt:
```
Pure loss: a solo viola in slow E♭ Aeolian, almost no accompaniment, a solo cello answering — intimate grief, no orchestra to hide behind. 46 BPM, rubato, no percussion ever. Close dry room, the sound of one person mourning. The sadness cue; small on purpose. Full-length cue, the lament stated, answered, unbearably bare, restated lower, unresolved, no early fade, long outro. Instrumental. No choir, no solo vocals, no flute, no glissando, no Latin, no brass, no drums, no swell, no synth, no electric guitar, no EDM, no comfort.
```
Structure prompt:
```
[Instrumental Intro - solo viola alone, a slow falling E♭ Aeolian phrase, bare]
[Instrumental A - a solo cello enters beneath, a low counter-line, grieving]
[Instrumental B - viola and cello trade the lament, no other instrument]
[Instrumental Hush - the viola alone for one phrase, unbearable and quiet]
[Instrumental Development - the lament restated lower, slower]
[Instrumental C - cello holds one low note as the viola sinks]
[Instrumental Outro - the viola does not resolve the final note; it simply stops. Silence.]
```

---

**53 — Noble Sacrifice _ The Blow at the Marsh**
*Key G Aeolian → G Lydian · 60 BPM · mixed · cello → full strings · instrumental · 4:30*

Style prompt:
```
A death that means something: a solo cello (Endurance, G Aeolian) carried up by gathering strings into the Aelorin Song (G Lydian) on glass harmonica and high strings — grief turned to transcendence, the cost paid and accepted. 60 BPM, 4/4, a slow inexorable rise. Vast cathedral reverb. The redemptive elegy, no voices. Full-length cue, statement / gathering / transformation / release / peace, no early fade, long outro. Instrumental. No battery, no choir, no solo vocals, no flute glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - solo cello, the Endurance cell in G minor, alone and tired]
[Instrumental A - strings gather under it slowly, harmony lifting toward Lydian]
[Instrumental B - the Endurance cell begins to transform into the Aelorin Song]
[Instrumental Turn - glass harmonica takes the Song, the key opens fully to G Lydian]
[Instrumental Climax - full strings and harmonica, the Song allowed to nearly resolve]
[Instrumental Release - everything drops to one held Lydian chord — the cost accepted]
[Instrumental Outro - solo cello returns at peace, one last Endurance tag, long warm fade]
```

---

**54 — Betrayal _ The Mole Revealed**
*Key B♭ minor → cluster · 64 BPM · Naergrim-adj. · low strings + clock tick · instrumental · 3:30*

Style prompt:
```
Cold realisation: low strings in tightening B♭ minor over a dry mechanical clock tick, a trusted-warm motif fragment heard once then soured into a cluster — the moment a friend turns out to be the knife. 64 BPM, 4/4, clinical and dropping. Close airless room. Through-composed; no comfort, no bombast. Full 3:30 cue, tick / warm fragment / souring / drop / dead stop, no early fade, short cold outro. Instrumental. No choir, no solo vocals, no flute, no glissando, no taiko, no warm resolution, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - a dry mechanical tick, alone, like a clock in a quiet room]
[Instrumental A - low strings enter B♭ minor, a familiar warm motif fragment heard once, trusting]
[Instrumental Turn - the fragment sours, detunes, curdles toward a cluster]
[Instrumental B - the strings tighten downward, the tick speeds slightly]
[Instrumental Development - the cluster thickens, the floor going out]
[Instrumental Cutoff - the tick stops dead — the realisation lands]
[Instrumental Outro - one airless cluster held, no resolution, hard short fade]
```

---

**55 — Hope Rekindled _ The Turn**
*Key A Aeolian → A Mixolydian · 72→104 BPM · Human · solo oboe → full orch · instrumental · 4:00*

Style prompt:
```
The turn from despair: a lone oboe in fragile A Aeolian, a fragment of the Endurance cell finding its feet, strings and brass gathering as the key opens to A Mixolydian and the tempo lifts 72 to 104 — not victory yet, but the moment it becomes possible. 4/4. Warm growing reverb. Soule's dawn cue, no voices. Full-length cue, fragile / finding / build / turn / open lift, no early fade, long outro. Instrumental. No battle battery, no choir, no solo vocals, no flute glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no premature triumph.
```
Structure prompt:
```
[Instrumental Intro - solo oboe, fragile A Aeolian, a broken piece of the Endurance cell]
[Instrumental A - the oboe finds the whole cell, hesitant; soft strings agree underneath]
[Instrumental B - clarinet and strings answer, the key warming toward A Mixolydian]
[Instrumental Build - tempo lifts, horns enter low, hope catching]
[Instrumental Turn - 104 BPM, the Endurance cell stated whole and strong for the first time]
[Instrumental Climax - full strings and horns, bright but not yet triumphant — possibility]
[Instrumental Outro - it does not over-resolve; it lifts and holds, open, hopeful, long fade up]
```

---

**56 — Epilogue _ The Road Home**
*Key E♭ Mixolydian · 80 BPM · Folk/Human · solo cello + oboe · instrumental · 4:00*

Style prompt:
```
Quiet closure: solo cello and oboe trading the Endurance cell and the Hearth fragment, gently, in warm E♭ Mixolydian over light strings — the war over, the road leading home, earned peace not fanfare. 80 BPM, 4/4, unhurried. Warm open-field reverb at dusk. The denouement, Soule's warm send-off, no voices. Full-length cue, both themes stated / varied / intimate bridge / settled reprise, no early fade, long outro. Instrumental. No battery, no choir, no solo vocals, no flute glissando, no pitch sweeps, no Latin, no taiko, no synth, no electric guitar, no drum kit, no EDM, no swell.
```
Structure prompt:
```
[Instrumental Intro - solo cello, the Endurance cell in E♭, calm now, no weight on it]
[Instrumental A - oboe answers with the Hearth fragment, the two themes at peace]
[Instrumental B - light strings join warmly, the road opening ahead]
[Instrumental Development - the themes gently varied, dusk light]
[Instrumental Bridge - cello and oboe alone again, intimate, almost home]
[Instrumental Reprise - the themes restated together, settled, complete]
[Instrumental Outro - one warm held chord at dusk, very long peaceful fade]
```

---

### Endings (Game Three — authored finale cues, instrumental)

---

**57 — The Return _ Released**
*Key D Lydian* (the Main Theme's D, transformed to resolution) · 58 BPM · Aelorin/Human · full strings · brief optional choir · 5:00*

Style prompt:
```
Resolution and release in D Lydian — deliberately the Main Theme's D, lifted from minor into a resolving Lydian for the trilogy's end: the Aelorin Song motif, unfinished for the whole trilogy, finally completes on full strings and glass harmonica, the missing eighth note at last sounded. A brief light wordless massed choir swell is OPTIONAL at the climax only and may be omitted for fully instrumental; never solo. 58 BPM, 4/4, a slow opening-out. Vast warm cathedral reverb. The only cue where the Song closes. Full 5-minute cue, no early fade, very long outro. No solo vocals, no sung verses, no flute glissando, no pitch sweeps, no battery, no dissonance, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - glass harmonica, the Song motif in D, still unfinished, fragile]
[Instrumental A - full strings gather it up, warm, the Endurance cell entwined beneath]
[Instrumental B - the Song varied, rising, the harmony opening to D Lydian]
[Instrumental Turn - the Song reaches its final note — and this time it resolves, the eighth sounded]
[Choir - OPTIONAL brief wordless massed swell at the resolution, no solo, may be omitted]

(wordless swell only — texture, omittable)

[Instrumental Climax - full strings on the resolved Song, release not triumph]
[Instrumental Outro - everything settles onto the home chord, glass harmonica last, very long warm fade]
```

---

**58 — The Hold _ Carried Forever**
*Key E Aeolian (unresolving) · 54 BPM · Iron Chalice · solo cello + low strings · instrumental · 5:00*

Style prompt:
```
Love as permanent cost: the Endurance cell on solo cello and a low string section in E Aeolian, dignified and warm but the harmony never fully resolves — the weight is carried, not put down, forever. 54 BPM, slow 4/4. Deep stone reverb. Beautiful and unresolved on purpose, distinct from The Return by its withheld cadence. Fully instrumental, no voices. Full 5-minute cue, statement / gathering / withheld cadence / noble peak / still-carried close, no early fade, very long outro. No choir, no solo vocals, no flute, no glissando, no battery, no bright brass, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - solo cello, the Endurance cell in E Aeolian, steady, accepting]
[Instrumental A - a low string section enters beneath, warm, dignified]
[Instrumental B - the harmony rises as if to resolve — and holds back, the cadence withheld]
[Instrumental Development - the cell varied, the weight named in the low strings]
[Instrumental Climax - cello and strings at their fullest, noble, but never closing the chord]
[Instrumental Hush - back to solo cello, the Endurance cell, the weight still there]
[Instrumental Outro - the final note held, unresolved, carried — very long fade, no cadence]
```

---

**59 — The Fracture _ The Price of Refusal**
*Key shattered (no key — fragments around D and A) · 60 BPM · Mordvar-adj. · broken orchestra · instrumental · 5:00*

Style prompt:
```
The price of refusal: the Endurance cell (around D) and the Hollowing fragment (around A) each broken, neither winning, an orchestra that keeps almost cohering and shattering — survival without resolution, the cost on the world. 60 BPM, 4/4 destabilising, no settled key. Vast cold reverb. The bleak ending; deliberately denied catharsis, no settled chord at all, no voices. Full 5-minute cue, broken statements / failed gathering / collapse / suspended non-ending, no early fade, abrupt close. Instrumental. No clean victory, no choir, no solo vocals, no flute, no glissando, no synth, no electric guitar, no drum kit, no EDM.
```
Structure prompt:
```
[Instrumental Intro - a broken fragment of the Endurance cell around D, strings, it doesn't complete]
[Instrumental A - the Hollowing answers around A, also broken, neither motif able to finish]
[Instrumental B - the two fragments overlap and interfere, no key, no centre]
[Instrumental Build - the orchestra gathers as if toward a climax — and shatters before it lands]
[Instrumental Collapse - fragments of every theme scattered, unmoored]
[Instrumental Suspension - a single unresolved note, suspended, wrong]
[Instrumental Outro - it does not resolve and does not fade cleanly — it just stops. Silence.]
```

---

## 8. Production & maintenance notes

- **Lengths are 3:00 floors.** For the 5:00 set-pieces (03, 21, 34, 41, 48,
  57, 58, 59) generate in two passes and stitch, or Suno **Extend** the best
  take to length then add a manual fade. If a take still stops short,
  regenerate with the structure box only and a one-line style.
- **Flute discipline is load-bearing.** Keep the "steady stepwise, fixed
  register" wording and the "no glissando / pitch sweeps / portamento" negative
  on every flute-bearing prompt — that is what prevents the high-to-low flute
  artifact. The same applies to whistle leads (9, 30, 31, 49).
- **Key variety is the anti-sameness spine.** The Main Theme owns D minor; the
  only other D-tonic cues are the three deliberate callbacks (37, 51, 57). If a
  new cue is added, give it a key not already heavily used in its category.
- **Instrumental-first.** Only 02 (one brief light Latin statement) and
  24/34/41/45/47 (brief, optional, omittable) use voices. Render any of the
  five instrumental if unsure.
- **Reference order:** Oblivion → Morrowind → Skyrim → Shore. Lead style boxes
  with the instrument/key, name the reference late.
- **`.ogg`, stereo, 44.1 kHz** per `AUDIO_DESIGN.md`; into `assets/audio/music/`
  with the §5 title.
- **When this doc changes:** add the row to §5 with a distinct key and lever
  key; if a new cue needs choir, justify it and add its number to §3/§6;
  update `CLAUDE.md`'s maintenance reference if scope shifts.
