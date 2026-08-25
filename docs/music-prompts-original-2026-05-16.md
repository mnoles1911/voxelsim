<!--
SUPERSEDED -- HISTORICAL RECORD ONLY. USE docs/music-prompts.md.

The FIRST version of design/MUSIC_PROMPTS.md from the Mira-Thal / Voxelmark
Godot checkout (github.com/mnoles1911/Test), verbatim at commit b644f5ca,
2026-05-16. It was reworked the same day by 576dd18a and that reworked version
is what shipped; both are here so the change is visible rather than only
described.

WHAT THE REWORK CHANGED, and why you should not generate from this file:

  * Tracks rendered SHORT. Every cue in the newer doc targets 3:00-5:00 via
    6-9 developed structure sections plus an explicit "full-length, developed,
    no early fade, long outro" line in each style box. This version lacks that.
  * FLUTE ARTIFACTS. Flute lines came out as high-to-low pitch sweeps. The
    newer doc constrains flute to "simple steady stepwise lines in a fixed
    register" and every negative list bans glissando / pitch sweeps /
    portamento / octave runs / whistle effects. That language is the fix and
    the newer doc says not to remove it. This version does not have it.
  * VOCALS. This version allows sung/chant cues more freely; the rework went
    instrumental-first (53 of 59 instrumental, choir on only six, brief and
    mostly optional, one deliberate lead-vocal exception at track 49).
  * REFERENCES leaned harder to Soule's Oblivion in the rework.

Same caveat as the newer file: neither version contains the prompts that made
the three tracks on disk in the Test repo. Those predate both docs by ten days.
-->

# Music Prompts — Mira-Thal Soundtrack (Suno)

The full music portfolio for Game One, written from the ground up as Suno
prompts. Howard Shore's *Lord of the Rings* trilogy is the primary reference;
Jeremy Soule's *Skyrim* and *Morrowind* are the secondary reference. Every
track here is a **style prompt + a lyrics/structure prompt** pair, the same
two-box format the existing `Main Theme.wav` was generated with.

> Cross-reference: `design/AUDIO_DESIGN.md` — where music plays, transition
> rules, the "music knows where it is, not what you're doing" philosophy, and
> the bus layout. This doc is the **content**; that doc is the **system**.

---

## 1. The problem this doc exists to solve

The current portfolio (`Main Theme`, `World Map _ Travel`, `Sea _ Sailing`)
sounds the same track to track. That is not a Suno failure — it is a **prompt
sameness** failure. Every prompt opened with the same words ("cinematic
medieval fantasy orchestral score, epic and melancholic"), reached for the
same lead instruments (legato strings, four French horns, Latin choir), sat in
the same modes (Aeolian/Dorian), the same tempo band (~72–96 BPM), and the
same structural arc (hushed intro → heroic theme → choir → battle → cello
outro). Suno renders the *prompt's center of gravity*. Identical centers of
gravity produce a portfolio that blurs into one long cue.

Howard Shore did not score Rohan, Gondor, Lothlórien, Moria and Mordor the
same way. Each got its **own instrument, own mode, own meter, own language,
own recording space**, and only *then* a shared motivic spine tying them
together. This document does the same thing deliberately, using a variation
matrix so no two tracks share a center of gravity, plus a leitmotif system so
the variety still feels like one world.

---

## 2. The nine variation levers

Every track is assigned a deliberately different value on these. The inventory
table in §7 shows the full grid; the rule is simple: **no two adjacent tracks,
and no two tracks in the same category, may share more than three lever
values.**

1. **Tonal center & mode** — rotate hard. Aeolian, Dorian, Phrygian, Lydian,
   Mixolydian, Locrian fragments, whole-tone/octatonic for the corrupt, pure
   major for joy, modal-major (Lydian/Mixolydian) for the heroic, free
   atonal-drone for dead places. Never default to D Aeolian.
2. **Tempo** — give an explicit BPM. Spread the portfolio across 40 BPM
   (funeral/drone) to 168 BPM (charge). Never let three tracks cluster in the
   72–96 band.
3. **Lead voice** — the single most distinctive instrument, named first in the
   style box. Rotate across the whole list in §4 (hardanger fiddle,
   nyckelharpa, low whistle, hurdy-gurdy, hammered dulcimer, bowed psaltery,
   solo viola, cor anglais, contrabassoon, war horn, anvil, boy treble, female
   soloist, male drone, glass harmonica, no lead / pure texture).
4. **Ensemble density** — solo / chamber trio / small consort / string
   orchestra / full orchestra + choir / percussion-only / drone-only.
5. **Percussion family** — none / heartbeat frame drum / hand percussion /
   military field drums & timpani / taiko & dhol war battery / anvils &
   stomps / metallic ritual (struck bowls, chains) / arrhythmic stone.
6. **Vocal treatment** — instrumental only / Latin liturgical choir / Latin
   curse-chant (the dark mirror) / Aelorin vowel-song (no consonants, no
   language) / dwarven syllabic stomp-chant / Naergrim whispered fricative
   non-language / solo female lament / solo male ballad / boys' choir.
7. **Recording space** — dry chamber / intimate close / open field / great
   stone hall / cathedral / cavern with long slap / storm-air with no reverb /
   "music box" tiny box reverb.
8. **Structural arc** — vary section count and order. Not every track is
   intro→build→theme→choir→battle→outro. Some are a single sustained mood
   (correct for ambient beds, camp, the Archive). Some are strophic (shanties,
   tavern). Some are through-composed cinematic with no repeat.
9. **Cultural palette** — every region/faction has a fixed sonic identity (§4).
   The palette decides 3–6 above before the track's mood does.

---

## 3. Leitmotif system — what keeps the variety coherent

Five recurring cells. They are *small* (3–7 notes) so Suno can carry them
across very different arrangements. When a prompt should quote one, the
structure box says so in plain language ("the Endurance cell, stated by solo
cello") — Suno follows melodic-contour instructions well when they are short.
To actually lock pitch identity across tracks, see §6 "Seeding motifs".

| Motif | Whose | Shape | Mode/feel |
|---|---|---|---|
| **The Endurance cell** | Roland, the Iron Chalice, humankind | rising step, falling third, held — "the one who endures" turned from grief to resolve | Aeolian, noble, stepwise |
| **The Song / Eighth Star** | Aelorin, the Aeluvain, the world's first music | high, slow, open — an unfinished phrase that never quite resolves (the missing eighth note) | Lydian, weightless |
| **The Hollowing** | Mordvar, the Sundered Crown's making | the Song inverted and emptied — descending open fifths that refuse to close | octatonic/whole-tone, airless |
| **The Crown** | the seven pieces | a seven-note cell, each note a different metal/timbre; heard fractured, reassembling toward the end of the game | chromatic, brittle |
| **The Hearth** | companions, home, rest | a warm four-note folk fragment, the only motif that ever sounds *complete* | Mixolydian, plain |

**The Latin mirror.** Human liturgical chant and Mordvar's curse-chant are the
same text turned inside out — this is the single strongest cohesion device in
the score and it costs nothing to deploy:

- Human / heroic (light): *Lux per umbram / ferrum per ignem / sanguis per
  saecula / terra nos vocat* — "Light through shadow, iron through fire, blood
  through ages, the land calls us."
- Mordvar / hollow (the mirror): *Nihil per nihil / cor per inane / nemo per
  saecula / nihil nos tenet* — "Nothing through nothing, heart through
  emptiness, no one through ages, nothing holds us."

Aelorin singing is **not Latin** — open vowels only, no translatable text
(*aelúriel… síoma vael… ithíli… nóa…*). Dwarven is **syllabic and consonant-
heavy**, stomped in 6/8 (*khazûn… dorrum… tharak khaz!*). Naergrim is
**whispered fricatives**, never sung, never pitched (*ssha… vesh… thrael…*).
Keeping the four vocal languages strictly separate is, by itself, a massive
anti-sameness lever.

---

## 4. Cultural sonic palettes (the Shore method)

Decide the palette first. It pre-sets the lead, mode, percussion and vocal
levers so two tracks from different cultures *cannot* converge.

| Culture / place | Lead voices | Mode | Percussion | Vocal | Space |
|---|---|---|---|---|---|
| **Human heartland** (Eldermark, Aldenholt, the road) | French horn, solo cello, oboe | Aeolian/Dorian, Mixolydian for heroism | frame drum, timpani | Latin liturgical choir | open field / cathedral |
| **Iron Chalice** (Brightwatch, the chapel, Roland) | low strings, muted trumpet, lone war horn | Aeolian, austere, few notes | a single deep field drum | unison male plainchant, sparse | dry stone |
| **Aelorin** (Greatwood, Lirien-Thal, the Aeluvain) | glass harmonica, harp, high divisi strings, female soloist | Lydian, weightless | none, or finger-struck crotales | Aelorin vowel-song, no language | shimmering long reverb |
| **Dwarven** (Karaz-Dûn, the Underway, the holds) | contrabassoon, low brass, hammered dulcimer, **anvil** | Dorian/Phrygian, heavy | anvils, boot-stomps, deep toms in 6/8 | low male syllabic stomp-chant | great stone hall, long slap |
| **Naergrim** (Mor-Vethrin, Weeping Wood) | detuned/prepared strings, bowed metal, contrabass clarinet | cluster/no tonal center | arrhythmic stone, struck chains | whispered fricative non-language | airless, close, wrong |
| **Mordvar / Ashen Hand** (Sundered Isles, the Hollowing) | dissonant low brass, contrabassoon, war battery | octatonic/whole-tone | taiko + dhol war battery | Latin curse-chant (the mirror) | huge, brutal, no warmth |
| **Sailor's Guild / sea** | concertina, fiddle, low whistle, accordion, solo male voice | Dorian/Mixolydian, rolling 6/8 & 4/4 | hand drum, deck-stomp, rope creak | call-and-response shanty crew | salt-air, medium room |
| **Tavern / folk** | lute, fiddle, hand drum, recorder, hurdy-gurdy | Mixolydian/Dorian, major | tabor, claps, foot | bawdy unison or solo balladeer | small warm room |
| **Dead places** (Sorrowmarsh, Ashfields) | bowed psaltery, breath tones, distant solo cor anglais | drone, no functional harmony | none, or one struck bowl far off | wordless distant breath | vast empty, ghost slap |

---

## 5. Track inventory (59)

Naming follows the existing convention (`Main Theme`, `Sea _ Sailing` — title
case, ` _ ` for sub-category, `.ogg` once converted per `AUDIO_DESIGN.md`).
Three names already exist on disk and are flagged **[replaces existing]** —
the new prompt is a from-scratch rewrite, not a tweak.

| # | Title | Category | Palette | Mode | BPM | Lead | Vocal | Len |
|---|---|---|---|---|---|---|---|---|
| 01 | Studio Card _ The Eighth Star | Identity | Aelorin | Lydian | 50 | glass harmonica | vowel-song | 0:25 |
| 02 | Main Title _ Mira-Thal | Identity | Human | Mixolydian→Aeolian | 84 | French horn | Latin choir | 4:30 |
| 03 | End Credits _ The Long Twilight | Identity | mixed (suite) | modulating | 76 | solo cello | full choir | 5:00 |
| 04 | Open Road _ The Central Plains | Exploration | Human | Mixolydian | 92 | oboe | none | 4:00 |
| 05 | The Greatwood _ Under Old Leaves | Exploration | Aelorin | Lydian | 60 | harp + high strings | vowel-song | 4:30 |
| 06 | The Spine _ Stone and Sky | Exploration | Dwarven-adjacent | Dorian | 66 | horn + low strings | none | 4:00 |
| 07 | The Underway _ Beneath the Mountain | Exploration | Dwarven | Phrygian | 54 | contrabassoon | male drone | 4:30 |
| 08 | The Ashfields _ Grey Soil | Exploration | Dead | drone | 44 | cor anglais | distant breath | 4:00 |
| 09 | The Western Coast _ Caer Drowned | Exploration | Sea/dead | Aeolian | 58 | low whistle | wordless female | 4:00 |
| 10 | The Copper Isles _ Salt and Sun | Exploration | Sailor | Mixolydian | 104 | fiddle | none | 3:30 |
| 11 | The Sorrowmarsh _ The Mud Remembers | Exploration | Dead | atonal drone | 40 | bowed psaltery | whispered | 4:00 |
| 12 | The Weeping Wood _ Watched | Exploration | Naergrim | cluster | 48 | prepared strings | fricative whisper | 4:00 |
| 13 | Aldenholt _ Market and Bell | Settlement | Human | Mixolydian | 100 | lute + recorder | none | 3:30 |
| 14 | Caer Brannoch _ The Cliff City | Settlement | Human/sea | Dorian | 72 | solo cello + harp | none | 4:00 |
| 15 | Vosskar _ Iron and Listening | Settlement | Iron Chalice-adj. | Aeolian | 64 | muted trumpet | unison male | 3:45 |
| 16 | Solgrade _ The Unwalled City | Settlement | Tavern/cosmo | Dorian | 96 | hurdy-gurdy | none | 3:30 |
| 17 | Lirien-Thal _ The Silverwood | Settlement | Aelorin | Lydian | 52 | glass harmonica | vowel-song | 4:30 |
| 18 | Karaz-Dûn _ Forges Never Cold | Settlement | Dwarven | Dorian | 78 (6/8) | hammered dulcimer | stomp-chant | 4:00 |
| 19 | Mor-Vethrin _ The Obsidian City | Settlement | Naergrim | no center | 46 | contrabass clarinet | fricative | 4:00 |
| 20 | Brightwatch _ The Frontier Garrison | Settlement | Iron Chalice | Aeolian | 70 | lone war horn | sparse plainchant | 3:45 |
| 21 | The Archive _ Dust and Lamplight | Interior | Human (near-non-music) | static modal | 50 | bowed vibraphone | none | 5:00 |
| 22 | The Iron Chalice _ Chapel of Endurance | Sacred | Iron Chalice | Aeolian | 56 | organ + low strings | male plainchant | 4:00 |
| 23 | The Aeluvain _ The Song With an Edge | Sacred | Aelorin | Lydian (unresolved) | 58 | solo violin harmonics | vowel-song | 4:00 |
| 24 | The Crown Assembled _ Seven Metals | Sacred | mixed | chromatic | 64 | seven timbres | Latin + curse | 3:30 |
| 25 | Tavern _ The Limping Reel | Tavern | Folk | Mixolydian | 132 | fiddle | bawdy unison | 3:00 |
| 26 | Tavern _ The Widow's Ballad | Tavern | Folk | Dorian | 68 | solo voice + lute | solo male | 3:30 |
| 27 | The Deep Cups _ A Dwarven Drinking Song | Tavern | Dwarven | Dorian | 88 (6/8) | voices + anvil | stomp-chant | 3:00 |
| 28 | The Dockside _ Sailors' Tavern | Tavern | Sailor | Mixolydian | 120 | concertina | crew chorus | 3:00 |
| 29 | The Hearth-Song _ An Aelorin Air | Tavern | Aelorin | Lydian | 56 | harp + soloist | solo female vowel | 3:30 |
| 30 | The Capstan _ Heave Her Round | Sea | Sailor | Dorian | 96 (work) | crew + drum | call-and-response | 3:00 |
| 31 | Leaving Port _ The Tide Turns | Sea | Sailor | Mixolydian | 84 | fiddle + low whistle | solo + crew | 3:30 |
| 32 | At Sea _ Open Water | Sea | Sailor | Dorian | 70 | accordion + cello | none | 4:30 |
| 33 | The Shroud _ The Storm That Never Ends | Sea | Mordvar-adj./sea | octatonic | 72→132 | full orch + battery | wordless terror | 4:00 |
| 34 | The Eastern Crossing _ Into the Storm | Sea | mixed (epic) | modulating | 80 | full orch + choir | Latin choir | 5:00 |
| 35 | Campfire _ The Sound of Rest | Camp | Folk/intimate | Mixolydian | 60 | solo guitar/lute | none | 4:00 |
| 36 | Night Rest _ Sleeping Under Stars | Camp | intimate | Lydian | 48 | music box + harp | none | 4:30 |
| 37 | The Quiet After _ Wounds and Breath | Camp | Iron Chalice-adj. | Aeolian | 52 | solo cello | none | 3:30 |
| 38 | Enemies Gathering Strength _ The Muster of the Hand | War | Mordvar | octatonic | 60→88 | low brass | curse-chant | 4:00 |
| 39 | A Minor Skirmish _ Blades in the Brush | War | Iron Chalice-adj. | Phrygian | 116 | low strings ostinato | none | 2:30 |
| 40 | Charge Into Battle _ Sound the Horns | War | Human | Mixolydian | 152 | war horns + trumpets | Latin choir | 3:00 |
| 41 | The Large Battle _ The Field of Iron | War | mixed (suite) | Aeolian/octatonic | 96→168 | full orch + battery | full choir + curse | 5:00 |
| 42 | The Siege _ Hold the Walls | War | Dwarven | Phrygian | 100 | anvils + low brass | stomp-chant | 4:30 |
| 43 | Vaeroth the Pale _ The Hierarch | Boss | Mordvar | whole-tone | 108 | contrabassoon + choir | curse-chant | 4:00 |
| 44 | The Ashlord _ The Mask of Caerith | Boss | Naergrim/Aelorin | Lydian rotted to cluster | 92 | corrupted vowel-song | mixed | 4:30 |
| 45 | Mordvar _ The Hollowing | Boss | Mordvar | the inverted Song | 50 | dissonant low brass | curse-chant | 4:30 |
| 46 | The Fighting Retreat _ The Ashfields | War | Iron Chalice | Aeolian | 116 | war horn + strings | sparse male | 4:00 |
| 47 | The Last Stand _ No Ground Behind | War | Iron Chalice/Human | Aeolian→Mixolydian | 84→144 | full orch | full choir | 4:30 |
| 48 | The Muster of the Alliance _ Many Banners | War | mixed (suite) | Mixolydian | 100 | rotating culture leads | layered choirs | 5:00 |
| 49 | The Vigil _ The Night Before | Cinematic | Iron Chalice | Aeolian | 52 | solo cello + low whistle | distant male | 4:00 |
| 50 | Heroes Reunited _ The Fellowship Whole | Cinematic | mixed (motif weave) | Mixolydian | 76 | leitmotif weave | warm choir | 4:00 |
| 51 | A Marriage _ Two Hands Bound | Cinematic | Folk/Human | major (Ionian) | 88 | harp + oboe + fiddle | joyful soloist | 3:30 |
| 52 | Grief _ What the Archive Lost | Cinematic | Human | Aeolian | 46 | solo viola | solo female lament | 4:00 |
| 53 | Noble Sacrifice _ The Blow at the Marsh | Cinematic | mixed | Aeolian→Lydian | 60 | cello → full strings | Latin choir | 4:30 |
| 54 | Betrayal _ The Mole Revealed | Cinematic | Naergrim-adj. | minor→cluster | 64 | low strings + clock tick | none | 3:00 |
| 55 | Hope Rekindled _ The Turn | Cinematic | Human | Aeolian→Mixolydian | 72→104 | solo oboe → full orch | choir build | 4:00 |
| 56 | Epilogue _ The Road Home | Cinematic | Folk/Human | Mixolydian | 80 | solo cello + oboe | none | 4:00 |
| 57 | The Return _ Released | Ending | Aelorin/Human | Lydian resolving | 58 | full strings + soloist | full choir | 5:00 |
| 58 | The Hold _ Carried Forever | Ending | Iron Chalice | Aeolian, unresolving | 54 | solo cello + low choir | low male | 5:00 |
| 59 | The Fracture _ The Price of Refusal | Ending | Mordvar-adj. | shattered, no cadence | 60 | broken orchestra | fractured choir | 5:00 |

Existing on disk → **02 replaces `Main Theme`**, **32 / 30 supersede
`Sea _ Sailing`**, and a future Travel cue (cut from this set since
`World Map _ Travel` already exists — regenerate from #04's recipe if a
replacement is wanted).

---

## 6. How to use these in Suno

**Style box.** Keep it ~350–500 characters. Suno weights the *first clause*
hardest, so each prompt below leads with its single most distinctive element
(the lead instrument or the mode/meter), **never** with the generic "cinematic
medieval fantasy orchestral." Always state an explicit BPM and mode — Suno
drifts to mid-tempo minor when you don't.

**Negative list is per-track, not global.** A tavern reel must *not* exclude
fiddle; a dwarven hall *wants* the anvil. Each prompt carries its own tailored
exclusion line.

**Instrumental vs. vocal.** Tracks marked vocal in §5 use the `[ ]` structure
tags plus written chant text exactly like the existing `Main Theme`. Tracks
marked "none" still get a structure box (it controls form and dynamics) but
every section tag stays `[Instrumental …]` and there is no lyric text.

**Seeding motifs (pitch-locking across tracks).** Prompt text alone won't make
two tracks share an exact melody. To truly carry a leitmotif: generate the
cleanest statement first (e.g. the Endurance cell in **#22** or **#02**),
then use Suno's audio upload / "cover" / persona feature with that stem as the
seed for the other tracks that quote it. The structure box still names the
motif so the arrangement bends toward it.

**Batch workflow.**
1. Generate the five **motif anchor** tracks first: 02, 22, 23, 45, 35
   (Endurance, Iron Chalice, Aeluvain/Song, Hollowing, Hearth).
2. Generate one track per culture next (04, 05, 07, 12, 30, 25) and listen
   back-to-back — if any two blur, push their levers further apart *before*
   batching the rest.
3. Generate the remaining tracks in category order; audition each new one
   against the previous category to confirm separation.
4. Render best take to stereo 44.1 kHz, convert to `.ogg`, drop in
   `assets/audio/music/` with the §5 title.

---

## 7. The prompts

Each entry: **filename → variation key → style prompt → lyrics/structure
prompt.** Copy the two prompt blocks straight into Suno's two boxes.

---

### A. Identity & Frame

---

**01 — Studio Card _ The Eighth Star**
*Lever key: Aelorin · Lydian · 50 BPM · glass harmonica · vowel-song · 25 s sting*

Style prompt:
```
Solo glass harmonica, fragile and weightless, a single unresolved Lydian phrase — the world's first music heard from very far away. Add one breath of wordless high female voice and a distant struck crotale. 50 BPM, free time, no pulse. Vast shimmering reverb, almost no attack. As if Howard Shore scored the moment before creation. 25 seconds, ending on a held unresolved note. No percussion, no brass, no choir mass, no melody resolution, no Latin, no drums, no synth pad, no modern production.
```
Lyrics/structure prompt:
```
[Intro - solo glass harmonica, one rising Lydian phrase, no pulse]
[Voice - a single wordless high female vowel, distant, joining the last note]

aaa — élu — íriel

[Outro - the note left hanging, unresolved, long shimmer to silence]
```

---

**02 — Main Title _ Mira-Thal** *[replaces existing `Main Theme`]*
*Lever key: Human · Mixolydian→Aeolian · 84 BPM · French horn · Latin choir · 4:30*

Style prompt:
```
Four French horns in open fifths stating a noble Mixolydian theme, answered by warm solo cello — the Endurance cell. Sweeping legato strings, frame-drum heartbeat, distant wooden flute, mixed Latin choir entering tenors-first. Modal, never harmonic minor. 84 BPM, 4/4, briefly 6/8 at the cello bridge. Builds hushed intro → heroic horn theme → intimate cello → choral anthem → climactic taiko battle → quiet cello close. Cathedral reverb, pianissimo to fortissimo, film-score quality, in the lineage of Shore's LOTR and Soule's Skyrim. No drum kit, no electric guitar, no synth, no modern percussion, no English vocals, no EDM, no autotune.
```
Lyrics/structure prompt:
```
[Intro - quiet, solo wooden flute over distant frame-drum heartbeat]
[Build - frame drums set 4/4, low strings ostinato in open fifths, horns enter on pedal D]
[Main Theme - four French horns state the heroic Mixolydian melody, the Endurance cell, strings answer]
[Bridge - solo cello variation in 6/8, sparse harp, intimate and melancholic]
[Choir - Latin, tenors and basses lead, sopranos enter in fifths, mezzoforte rising]

Lux per umbram
ferrum per ignem
sanguis per saecula
terra nos vocat

[Battle Climax - 108 BPM, taiko, low-string eighth-note ostinato, trombones, choir]

Vocat! Vocat! Vocat!
Terra nos vocat!

[Brief lift to D major, trumpets restate the Endurance cell in triumph]
[Sudden cutoff, fading timpani roll]
[Outro - solo cello, three-note descending Endurance tag, frame-drum heartbeat, fade]
```

---

**03 — End Credits _ The Long Twilight**
*Lever key: suite · modulating · 76 BPM · solo cello · full choir · 5:00*

Style prompt:
```
A through-composed end-credits suite that visits every culture's color in turn: opens solo cello (Endurance, Aeolian), passes to Aelorin glass harmonica and vowel-song (Lydian), to a dwarven 6/8 hall with hammered dulcimer, to a brief Latin choral anthem, settling back to solo cello. 76 BPM, modulating between sections, 4/4 and 6/8. Full orchestra and mixed choir, cathedral reverb, film-score quality, Shore-style "all themes return." Wide dynamics. No drum kit, no synth, no electric guitar, no EDM, no autotune, no rock.
```
Lyrics/structure prompt:
```
[Intro - solo cello, the full Endurance theme, unaccompanied]
[Section A - strings swell under it, frame drum, French horns answer]
[Section B - Aelorin colour: harp, glass harmonica, wordless high voice, Lydian]

aelúriel… síoma vael… ithíli… nóa

[Section C - dwarven 6/8, hammered dulcimer, low male syllabic chant under it]

khazûn… dorrum… tharak khaz

[Section D - full Latin choral restatement, fortissimo]

Lux per umbram
ferrum per ignem
sanguis per saecula
terra nos vocat

[Coda - everything falls away to the solo cello, the Endurance tag, long fade]
```

---

### B. Exploration

---

**04 — Open Road _ The Central Plains**
*Lever key: Human · Mixolydian · 92 BPM · oboe · none · 4:00*

Style prompt:
```
Solo oboe carrying a walking Mixolydian melody over light pizzicato strings and a soft frame drum — open, breathing, hopeful, lots of air between phrases. Distant French horn pads on long notes only. 92 BPM, 4/4, relaxed. Bright open-field reverb, not cathedral. Pastoral travelling music in the spirit of Shore's Shire and Soule's Skyrim overworld, but melancholic underneath. Instrumental, no vocals. No choir, no Latin, no taiko, no heavy brass, no drum kit, no synth, no electric guitar, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - solo oboe alone, free, one long phrase]
[Instrumental A - pizzicato strings and soft frame drum enter, oboe states the walking theme]
[Instrumental B - solo clarinet takes a variation, horn pad underneath, warmer]
[Instrumental Bridge - strings only, the melody slows, a melancholic minor turn, no percussion]
[Instrumental A' - oboe returns to the walking theme, fuller strings, gentle build]
[Instrumental Outro - pizzicato thins out, solo oboe alone again, fade on a held note]
```

---

**05 — The Greatwood _ Under Old Leaves**
*Lever key: Aelorin · Lydian · 60 BPM · harp + high strings · vowel-song · 4:30*

Style prompt:
```
Harp arpeggios under divisi high strings shimmering in Lydian, a wordless solo female voice drifting in and out with no language — ancient, weightless, slightly sad. Finger-struck crotales for colour, no real percussion. 60 BPM, free-floating, no strong downbeat. Very long shimmering reverb, the sound of a forest older than memory. Aelorin music in the lineage of Shore's Lothlórien. No brass, no drums, no Latin, no male choir, no synth, no electric guitar, no modern percussion, no EDM.
```
Lyrics/structure prompt:
```
[Intro - solo harp, slow Lydian arpeggio, alone]
[A - high divisi strings enter softly, a slow rising line that never quite resolves — the Song motif]
[Voice - wordless solo female, distant, no language]

aelúriel… síoma vael
ithíli ar nóa
vael… síoma…

[B - the strings rise, crotales shimmer, the voice doubles itself in fifths]
[Hush - everything drops to solo harp and one held string note]
[Outro - the Song phrase left unfinished, voice fading, very long reverb tail]
```

---

**06 — The Spine _ Stone and Sky**
*Lever key: Dwarven-adjacent · Dorian · 66 BPM · horn + low strings · none · 4:00*

Style prompt:
```
A lone French horn over slow low-string swells in Dorian — vast, cold, mountainous, the scale of a range you cannot cross. Occasional deep timpani roll like distant rockfall. 66 BPM, 4/4, spacious. Big open-air reverb with a long mountain slap-back. Grand and severe, more Soule's Skyrim peaks than warm. Instrumental. No choir, no Latin, no taiko, no hand percussion, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - low-string drone, a single distant timpani roll]
[Instrumental A - lone French horn states a wide, slow theme, low strings swell beneath]
[Instrumental B - second horn answers in canon, strings thicken, a sense of altitude]
[Instrumental Peak - full low brass on a sustained chord, timpani roll, then sudden space]
[Instrumental Outro - one horn alone again, drone underneath, fade into wind]
```

---

**07 — The Underway _ Beneath the Mountain**
*Lever key: Dwarven · Phrygian · 54 BPM · contrabassoon · male drone · 4:30*

Style prompt:
```
Contrabassoon and low strings moving in slow Phrygian steps, a low male voice droning a single syllabic note far down a stone hall, occasional struck anvil ringing into long darkness. 54 BPM, heavy 4/4, oppressive but not evil — old, deep, patient. Huge cavern reverb with a very long slap. Dwarven underground in the lineage of Shore's Moria, restrained. No bright brass, no Latin, no taiko, no hand drum, no synth, no electric guitar, no drum kit, no EDM, no female voice.
```
Lyrics/structure prompt:
```
[Intro - cavern tone, one struck anvil ringing away into the dark]
[A - contrabassoon enters low and slow, Phrygian, low strings shadow it]
[Drone - a low male voice holds one syllable, no melody, deep in the hall]

dûm… khaz… dûm…

[B - the line descends step by step, a second anvil strike marks the bottom]
[Hush - everything stops but the cavern ring and the held male note]
[Outro - contrabassoon alone, one last anvil far off, fade into stone silence]
```

---

**08 — The Ashfields _ Grey Soil**
*Lever key: Dead · drone · 44 BPM · cor anglais · distant breath · 4:00*

Style prompt:
```
A barely-moving drone of detuned low strings, a distant solo cor anglais playing short fragments that never connect into a melody, far-off wordless breath with no pitch. 44 BPM, no real pulse, dead air. Vast empty reverb, a landscape where nothing grows. Bleak ambient non-music in the spirit of Shore's Dead Marshes and Soule's bleakest tundra. No percussion, no choir, no Latin, no brass section, no synth pad, no drum kit, no electric guitar, no melody resolution, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - low detuned string drone, motionless]
[Instrumental A - distant cor anglais plays a three-note fragment, stops, silence, plays it again]
[Breath - far-off wordless human breath, no pitch, like wind through dead trees]

(unvoiced breath, no words)

[Instrumental B - the drone shifts down a semitone, nothing resolves, the fragment never completes]
[Instrumental Outro - cor anglais fragment one last time, unanswered, fade into emptiness]
```

---

**09 — The Western Coast _ Caer Drowned**
*Lever key: Sea/dead · Aeolian · 58 BPM · low whistle · wordless female · 4:00*

Style prompt:
```
A low whistle keening an Aeolian lament over slow grey string swells and the suggestion of a bell tolling underwater, a wordless distant female voice like grief carried on sea-wind. 58 BPM, 4/4 but tidal and loose. Damp wide reverb, salt and stone, a city under the water. Mournful coastal music — Shore's elegies, not his battles. No drums, no taiko, no Latin, no bright brass, no synth, no electric guitar, no drum kit, no EDM, no upbeat material.
```
Lyrics/structure prompt:
```
[Intro - grey string swell, one slow bell tone as if underwater]
[A - low whistle states a falling Aeolian lament]
[Voice - distant wordless female, grief on the wind, no language]

(wordless, long open vowels, sorrowful)

[B - strings rise to a single aching peak, the bell tolls again]
[Hush - back to one whistle line and the underwater bell]
[Outro - whistle holds its last note, bell fades beneath the water]
```

---

**10 — The Copper Isles _ Salt and Sun**
*Lever key: Sailor · Mixolydian · 104 BPM · fiddle · none · 3:30*

Style prompt:
```
A bright fiddle reel in Mixolydian over strummed cittern, hand drum and a skipping low whistle counter-line — sunlit, busy, a trading port that never sleeps. 104 BPM, 4/4 with a lilt. Medium warm room, lively. The portfolio's most upbeat exploration cue — folk-forward, Soule's friendlier towns crossed with a sea-port jig. Instrumental. No choir, no Latin, no taiko, no heavy brass, no synth, no electric guitar, no drum kit, no EDM, no melancholy.
```
Lyrics/structure prompt:
```
[Instrumental Intro - solo fiddle kicks off a bright Mixolydian phrase]
[Instrumental A - cittern strum and hand drum lock the groove, fiddle states the reel]
[Instrumental B - low whistle takes the tune, fiddle drops to a counter-line]
[Instrumental Lift - both play together, double-time feel, port at full bustle]
[Instrumental Break - hand drum and claps only for two bars, then full band back in]
[Instrumental Outro - one last full statement, sharp ensemble stop]
```

---

**11 — The Sorrowmarsh _ The Mud Remembers**
*Lever key: Dead · atonal drone · 40 BPM · bowed psaltery · whispered · 4:00*

Style prompt:
```
Bowed psaltery and bowed metal scraping a slow atonal drone, no key, the occasional far-off struck bowl, whispered voices with no words rising and sinking like ghost-lights. 40 BPM, no pulse, water that does not move. Vast haunted reverb with an unsettling ghost slap. Pure dread atmosphere — the unmaking happened here. No melody, no percussion groove, no choir, no Latin, no brass, no synth, no electric guitar, no drum kit, no EDM, no resolution.
```
Lyrics/structure prompt:
```
[Instrumental Intro - bowed psaltery, one long atonal scrape, no key]
[Instrumental A - bowed metal joins, a far struck bowl rings once]
[Whisper - many faint whispered voices, no words, rising like marsh-lights]

ssha… vesh… (whispered, no pitch, no language)

[Instrumental B - the drone clusters tighter, the bowl rings again closer]
[Whisper - the voices swell briefly then sink back]
[Outro - psaltery alone, one bowl far off, fade into still water]
```

---

**12 — The Weeping Wood _ Watched**
*Lever key: Naergrim · cluster · 48 BPM · prepared strings · fricative whisper · 4:00*

Style prompt:
```
Prepared and detuned strings playing tight tone-clusters that never resolve, contrabass clarinet groaning beneath, dry whispered fricative non-language circling close to the ear — the feeling of being watched from every tree. 48 BPM, no real pulse, wrong. Close airless space, almost no reverb, which is itself unsettling. Naergrim dread — alien, not loud. No melody, no warm harmony, no percussion, no Latin, no sung choir, no brass fanfare, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - one detuned string note bent slowly out of tune]
[Instrumental A - prepared-string cluster builds, contrabass clarinet groans under it]
[Whisper - dry fricative non-language, very close, circling the listener]

ssha… vesh… thrael… (whispered, breath only, no melody)

[Instrumental B - the cluster tightens, a string snaps a harsh harmonic]
[Whisper - whispers multiply, then cut to silence all at once]
[Outro - one held detuned note, the airless room, hard stop]
```

---

### C. Settlements

---

**13 — Aldenholt _ Market and Bell**
*Lever key: Human · Mixolydian · 100 BPM · lute + recorder · none · 3:30*

Style prompt:
```
Lute and recorder trading a warm Mixolydian tune over a tabor and tambourine, a city bell marking phrase ends — busy, friendly, background market energy without urgency. 100 BPM, 4/4 with a skip. Small warm room reverb. The largest human city at work; Soule's town themes, lighter, no melancholy. Instrumental. No choir, no Latin, no taiko, no heavy brass, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - solo lute, a turning Mixolydian figure, a single bell tone]
[Instrumental A - recorder takes the melody, tabor and tambourine set the bustle]
[Instrumental B - lute and recorder in cheerful counterpoint, the bell every four bars]
[Instrumental Bridge - just lute and tabor, quieter, a quick warm minor turn]
[Instrumental A' - full little ensemble back, brighter, market at peak]
[Instrumental Outro - thins to solo lute and one last bell, gentle stop]
```

---

**14 — Caer Brannoch _ The Cliff City**
*Lever key: Human/sea · Dorian · 72 BPM · solo cello + harp · none · 4:00*

Style prompt:
```
Solo cello singing a noble Dorian melody over harp and slow string pads, a distant sea-swell suggested in the low strings — a proud city on the cliffs above the ocean, dignified and a little lonely. 72 BPM, 4/4, stately. Medium hall with an airy sea-wind tail. Aristocratic and maritime; Shore's nobler, quieter Gondor register. Instrumental. No drums, no taiko, no Latin, no bright fanfare, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - harp alone, a slow rolling figure like the sea below]
[Instrumental A - solo cello states the noble Dorian theme, string pad enters]
[Instrumental B - violins lift the theme an octave, harp continues, dignified swell]
[Instrumental Bridge - cello and harp alone, intimate, the lonely register]
[Instrumental A' - full strings restate the theme, proud but restrained]
[Instrumental Outro - cello holds the last note, harp rolls once, fade on sea-wind]
```

---

**15 — Vosskar _ Iron and Listening**
*Lever key: Iron Chalice-adjacent · Aeolian · 64 BPM · muted trumpet · unison male · 3:45*

Style prompt:
```
A muted trumpet over spare low strings in Aeolian, severe and watchful, a small unison male choir entering on a single sustained line — a fortress city built on silence and suspicion. 64 BPM, slow 4/4. Dry stone reverb, no warmth. Austere and martial-adjacent, restrained. No taiko, no hand percussion, no bright brass, no Latin curse, no synth, no electric guitar, no drum kit, no EDM, no major-key lift.
```
Lyrics/structure prompt:
```
[Intro - one muted trumpet note, dry, alone]
[A - low strings enter beneath in slow Aeolian steps, watchful]
[Choir - unison male voices hold one sustained Latin word, no harmony yet]

audire

[B - trumpet states a short severe phrase, choir adds a bare fifth beneath]
[Hush - everything pares back to the held male note — the city listening]
[Outro - muted trumpet repeats its phrase once, dry stop, no reverb tail]
```

---

**16 — Solgrade _ The Unwalled City**
*Lever key: Tavern/cosmopolitan · Dorian · 96 BPM · hurdy-gurdy · none · 3:30*

Style prompt:
```
A hurdy-gurdy drone and melody in Dorian with a foreign lilt, hand percussion, plucked oud-like strings and a tambourine — a wealthy crossroads city, many cultures, slightly exotic, never threatening. 96 BPM, 4/4 with an off-beat sway. Medium lively room. Cosmopolitan market music, the most "outsider"-flavoured settlement cue. Instrumental. No choir, no Latin, no taiko, no heavy brass, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - hurdy-gurdy drone fades in, a single sustained chord]
[Instrumental A - hurdy-gurdy melody starts in Dorian, hand drum and tambourine join]
[Instrumental B - plucked oud-like strings take a variation with a foreign lilt]
[Instrumental Bridge - percussion and drone only, a swaying off-beat groove]
[Instrumental A' - full ensemble, the melody ornamented, busier]
[Instrumental Outro - instruments drop out one by one, hurdy-gurdy drone last, fade]
```

---

**17 — Lirien-Thal _ The Silverwood**
*Lever key: Aelorin · Lydian · 52 BPM · glass harmonica · vowel-song · 4:30*

Style prompt:
```
Glass harmonica and harp in slow Lydian over a bed of high sustained strings, a wordless female solo and a soft mixed wordless choir in fifths — a canopy city among ancestor-trees, sacred and grieving for a fading people. 52 BPM, free, no downbeat. Enormous shimmering cathedral-of-leaves reverb. The Aelorin's holiest place; Shore's Lothlórien at its most reverent. No percussion, no brass, no Latin, no drums, no synth, no electric guitar, no EDM, no sharp attack.
```
Lyrics/structure prompt:
```
[Intro - glass harmonica alone, slow Lydian, weightless]
[A - harp and high strings enter, the Song motif rises but does not resolve]
[Voice - wordless solo female, then a soft wordless choir answers in fifths]

aelúriel… síoma vael
ithíli ar nóa
síoma… vael…

[B - the choir swells gently, harp shimmering, a held unresolved chord]
[Hush - back to glass harmonica and one voice]
[Outro - the Song phrase left open, choir fading upward, very long tail]
```

---

**18 — Karaz-Dûn _ Forges Never Cold**
*Lever key: Dwarven · Dorian · 78 BPM (6/8) · hammered dulcimer · stomp-chant · 4:00*

Style prompt:
```
Hammered dulcimer and low brass in a rolling 6/8 Dorian work-rhythm, anvils struck on the strong beats, boot-stomps, a low male syllabic stomp-chant — a hold whose forges never go cold, proud and warm despite the weight. 78 BPM, 6/8, driving. Great stone-hall reverb with a long slap. Dwarven craft-pride; Shore's dwarves at work, not in mourning. No bright strings lead, no Latin, no taiko, no synth, no electric guitar, no drum kit, no EDM, no female voice.
```
Lyrics/structure prompt:
```
[Intro - one anvil strike, then the 6/8 boot-stomp pattern alone]
[A - hammered dulcimer states the rolling Dorian work-theme, anvils on the strong beats]
[Chant - low male voices, syllabic, stomped]

khazûn — dorrum — tharak khaz!
khazûn — dorrum — tharak khaz!

[B - low brass joins the dulcimer, fuller, the forge at full heat]
[Break - anvils and stomps only, four bars, then everyone back in]
[Outro - dulcimer figure slows, one last anvil strike, ring into the hall]
```

---

**19 — Mor-Vethrin _ The Obsidian City**
*Lever key: Naergrim · no tonal center · 46 BPM · contrabass clarinet · fricative · 4:00*

Style prompt:
```
Contrabass clarinet and bowed metal in a slow centreless drift, struck chains and a single stone-on-stone arrhythmic pulse, dry whispered fricative non-language — a windowless obsidian city that has held its silence two thousand years. 46 BPM, no key, no real pulse. Close, airless, wrong. Alien and ancient, never bombastic. No melody, no warm harmony, no Latin, no sung choir, no bright brass, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - contrabass clarinet, one long centreless note]
[Instrumental A - bowed metal joins, a chain struck once, arrhythmic stone pulse begins]
[Whisper - dry fricative non-language, layered, no pitch]

vesh… thrael… ssha… (breath only)

[Instrumental B - the texture thickens without resolving, chains closer]
[Whisper - whispers crowd in, the stone pulse stops dead]
[Outro - one clarinet note, one last chain, abrupt airless cut]
```

---

**20 — Brightwatch _ The Frontier Garrison**
*Lever key: Iron Chalice · Aeolian · 70 BPM · lone war horn · sparse plainchant · 3:45*

Style prompt:
```
A lone war horn over a single deep field drum and spare low strings in Aeolian, a few unison male plainchant notes — a frontier fort holding the line, weary endurance rather than glory. 70 BPM, slow 4/4. Dry stone, cold air. Iron Chalice austerity; this is Roland's home register. Restrained, no triumph. No taiko, no hand percussion, no bright brass section, no Latin curse, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - one deep field-drum hit, slow, then a lone war horn call]
[A - low strings enter beneath in bare Aeolian, the Endurance cell hinted, not stated]
[Chant - a few unison male plainchant notes, sparse, no harmony]

stamus

[B - the war horn states the Endurance cell plainly, drum keeping the slow tread]
[Hush - strings hold one low chord, the drum stops]
[Outro - the war horn calls once more, unanswered, dry fade]
```

---

### D. Interiors & Sacred

---

**21 — The Archive _ Dust and Lamplight**
*Lever key: Human / near-non-music · static modal · 50 BPM · bowed vibraphone · none · 5:00*

Style prompt:
```
Almost non-music: a static modal hum of bowed vibraphone and sustained low strings, a distant clock or bell resonance every minute or so, faint room tone — a vast library, lamplight, dust, the sound of a quiet room thinking. 50 BPM, no pulse, no melody. Dry interior with a faint long resonance. Ambient texture in the spirit of Shore's quietest interiors; designed to disappear. No percussion, no choir, no Latin, no brass, no melodic line, no synth pad, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - bowed vibraphone, one slow shimmering tone]
[Instrumental A - sustained low strings join very quietly, a distant single bell]
[Instrumental B - the harmony shifts once, almost imperceptibly, no melody appears]
[Instrumental Drift - room tone, a far bell again, the hum continues unchanged]
[Instrumental Outro - everything thins to a single held vibraphone tone, slow fade]
```

---

**22 — The Iron Chalice _ Chapel of Endurance**
*Lever key: Iron Chalice · Aeolian · 56 BPM · organ + low strings · male plainchant · 4:00*

Style prompt:
```
A church organ and low strings in solemn Aeolian, a unison male plainchant choir stating the Endurance cell as a hymn — austere, devotional, the doctrine of endurance made sound. 56 BPM, slow 4/4. Stone-chapel reverb, medium tail. The Iron Chalice's theological core; this is a primary motif anchor — keep the Endurance melody clean and central. No taiko, no hand percussion, no bright brass, no Latin curse, no female voice, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - organ alone, a low sustained Aeolian chord]
[A - unison male plainchant enters, stating the Endurance cell as a hymn line]

Ferro stamus
umbra non frangit

[B - low strings join the organ, the choir splits to a bare octave]

lux in fine
Aldrath custodit

[Swell - organ full, choir fortissimo on the Endurance cell, then sudden hush]
[Outro - one voice holds the final note over a low organ pedal, slow stone fade]
```

---

**23 — The Aeluvain _ The Song With an Edge**
*Lever key: Aelorin · Lydian, unresolved · 58 BPM · solo violin harmonics · vowel-song · 4:00*

Style prompt:
```
Solo violin in high natural harmonics, glass harmonica, and a single wordless female voice, all circling the Song motif in Lydian but never closing it — a sword that is a piece of the world's first music, beautiful and faintly painful. 58 BPM, free. Vast crystalline reverb. A motif anchor: this is the Song / Eighth Star theme in its purest form. No percussion, no brass, no Latin, no drums, no low choir, no synth, no electric guitar, no EDM, no resolution.
```
Lyrics/structure prompt:
```
[Intro - solo violin harmonic, one pure high note, hanging]
[A - glass harmonica enters, the Song motif begins to form, Lydian, unresolved]
[Voice - one wordless female voice doubles the violin, no language]

aaa — élu — íriel — siloä

[B - the phrase reaches for its final note and stops one step short — the missing eighth]
[Hush - violin and voice alone on a held harmonic]
[Outro - the Song deliberately left unfinished, shimmering, very long fade]
```

---

**24 — The Crown Assembled _ Seven Metals**
*Lever key: mixed · chromatic · 64 BPM · seven timbres · Latin + curse · 3:30*

Style prompt:
```
Seven distinct timbres enter one at a time over a chromatic low drone — iron (low strings), gold (struck metal), bronze (horn), copper (harp), silver (glass harmonica), copper-disc (anvil), obsidian (contrabass clarinet) — the seven-note Crown cell assembling, awe shot through with dread, a Latin choir and a whispered curse fighting underneath. 64 BPM, slow 4/4. Huge cold reverb. Through-composed, no repeat. No drum kit, no synth, no electric guitar, no EDM, no autotune, no comfort.
```
Lyrics/structure prompt:
```
[Intro - chromatic low drone, one struck metal tone — iron]
[Build - each timbre adds a note of the Crown cell: horn, harp, glass harmonica, anvil, clarinet]
[Choir - a Latin choir tries to hold the light line]

sanguis per saecula

[Curse - a whispered Latin curse rises against it, the mirror]

nihil per saecula

[Climax - all seven timbres sound the full Crown cell at once, brittle and vast]
[Cutoff - everything snaps off but the obsidian clarinet note]
[Outro - the clarinet bends down alone, the Crown cell unresolved, cold fade]
```

---

### E. Tavern & Folk

---

**25 — Tavern _ The Limping Reel**
*Lever key: Folk · Mixolydian · 132 BPM · fiddle · bawdy unison · 3:00*

Style prompt:
```
A fast fiddle reel in Mixolydian with a deliberate limp in the rhythm, lute, hand drum, foot-stomps and claps, a rowdy bawdy unison crowd singing the refrain off-key on purpose — a packed tavern, ale, bad dancing. 132 BPM, 4/4 with a dropped beat every phrase. Small warm boozy room. Diegetic folk, deliberately unpolished — the opposite of cathedral. No orchestra, no Latin, no choir polish, no taiko, no brass, no synth, no electric guitar, no drum kit, no EDM, no autotune.
```
Lyrics/structure prompt:
```
[Intro - fiddle scrapes off a fast reel, foot-stomp sets the limping time]
[A - lute and hand drum lock in, fiddle states the tune]
[Refrain - rowdy unison crowd, deliberately rough]

Pour it deep and pour it long,
the floor's the only judge of song!

[B - fiddle takes a wilder variation, claps double up]
[Refrain - crowd again, louder, more off-key]

Pour it deep and pour it long,
the floor's the only judge of song!

[Break - stomps and claps only, then fiddle screams back in]
[Outro - one ragged ensemble stop and a single drunk cheer]
```

---

**26 — Tavern _ The Widow's Ballad**
*Lever key: Folk · Dorian · 68 BPM · solo voice + lute · solo male · 3:30*

Style prompt:
```
A single weathered male voice and a lone lute in Dorian, slow and plain, a quiet tavern gone still to listen — a soldier's widow, a road that did not bring him back. 68 BPM, free rubato, no percussion. Intimate close room, almost no reverb. Diegetic ballad; the sad counterpart to the reel. Sparse, no orchestra, no choir, no Latin, no drums, no brass, no synth, no electric guitar, no EDM, no autotune.
```
Lyrics/structure prompt:
```
[Intro - solo lute, a slow falling Dorian figure]
[Verse 1 - one male voice, plain, unhurried]

She set two cups upon the board
and drank them both alone.

[Verse 2 - lute unchanged, the voice lower]

The road that took her soldier east
has never brought him home.

[Bridge - lute alone, a held breath in the room]
[Verse 3 - voice barely above the lute, the last line spoken almost]

She keeps his chair against the wall
and calls it not yet cold.

[Outro - one last lute phrase, the voice gone, quiet]
```

---

**27 — The Deep Cups _ A Dwarven Drinking Song**
*Lever key: Dwarven · Dorian · 88 BPM (6/8) · voices + anvil · stomp-chant · 3:00*

Style prompt:
```
A roaring low male unison drinking song in 6/8 Dorian, tankards on the table on the beat, anvil, boot-stomps, a single low brass doubling the tune — dwarves, ale, defiance, joy that sounds like a war chant. 88 BPM, 6/8, heavy swing. Big stone-hall reverb. Diegetic and proud, not refined. No strings lead, no Latin, no taiko, no female voice, no synth, no electric guitar, no drum kit, no EDM, no autotune.
```
Lyrics/structure prompt:
```
[Intro - tankards pounded on the table set the 6/8, one anvil clang]
[Verse - roaring low male unison, syllabic and stomped]

Stone for the bone and the deep for the dead,
ale for the living and gold for the head!

[Chorus - everyone, fists down on the beat]

Khaz! Khaz! Down the deep cups go!
Khaz! Khaz! Let the long hall know!

[Verse 2 - low brass doubles the tune, louder]

We dug too far and we drank too deep,
and we'll sing it again 'fore we stagger to sleep!

[Chorus - full, ragged, joyous]

Khaz! Khaz! Down the deep cups go!

[Outro - one last KHAZ!, anvil clang, a roar of laughter]
```

---

**28 — The Dockside _ Sailors' Tavern**
*Lever key: Sailor · Mixolydian · 120 BPM · concertina · crew chorus · 3:00*

Style prompt:
```
A driving concertina and fiddle in Mixolydian, a crew of rough male voices on the chorus, hand drum and a boot on the deck for percussion — a portside tavern, salt, smoke, sailors home for a night. 120 BPM, 4/4 with a roll. Medium boozy room with a little wood ring. Diegetic sea-folk; distinct from the dwarven and human tavern cues by the concertina and the call-and-response. No orchestra, no Latin, no taiko, no synth, no electric guitar, no drum kit, no EDM, no autotune.
```
Lyrics/structure prompt:
```
[Intro - concertina kicks off, a boot stamps the deck-time]
[Verse - one lead sailor voice, fiddle under it]

I sailed her out on a copper tide,

[Response - the whole crew answers]

heave away, the storm's outside!

[Chorus - full crew, concertina full]

So drink her down 'fore the morning bell,
the sea don't care and the tide won't tell!

[Verse 2 - lead voice again, fiddle wilder]

I'll spend my pay 'fore the dawn comes grey,

[Response - crew]

heave away, we sail away!

[Chorus - full again, louder]
[Outro - concertina holds a chord, crew shouts the last line, laughter]
```

---

**29 — The Hearth-Song _ An Aelorin Air**
*Lever key: Aelorin · Lydian · 56 BPM · harp + soloist · solo female vowel · 3:30*

Style prompt:
```
A single Aelorin harp and one wordless female voice in gentle Lydian — not a tavern song but a hearth-song, the rare warm Aelorin register, sung to a small circle, tender and old. 56 BPM, free, no percussion. Soft medium reverb, intimate not vast. The folk counterpart to the grand Aelorin cues — small and human-scaled despite the language. No orchestra, no brass, no Latin, no drums, no choir mass, no synth, no electric guitar, no EDM.
```
Lyrics/structure prompt:
```
[Intro - solo Aelorin harp, a slow tender Lydian figure]
[A - one wordless female voice enters softly, no language, an intimate melody]

síoma… vael… aelúriel…
nóa ithíli… vael…

[B - harp answers the voice phrase for phrase, like two people by a fire]
[Hush - voice alone for one phrase, unaccompanied]
[A' - harp returns under the voice, the air repeated, warmer]
[Outro - harp holds the last chord, the voice fades on an open vowel]
```

---

### F. Sea

---

**30 — The Capstan _ Heave Her Round**
*Lever key: Sailor · Dorian · 96 BPM (work) · crew + drum · call-and-response · 3:00*

Style prompt:
```
A true work shanty: a lead male voice calls, a crew answers on the heave, a hand drum and the rhythmic creak of rope and capstan keep the pull — Dorian, muscular, functional, the rhythm is the work. 96 BPM, 4/4, every other bar is the haul. Open deck, salt-air, little reverb. Sailor's Guild labour music; strophic, repetitive by design. No orchestra, no Latin, no taiko, no synth, no electric guitar, no drum kit, no EDM, no autotune.
```
Lyrics/structure prompt:
```
[Intro - rope creak and a slow hand drum, the capstan starting to turn]
[Call & Response 1 - lead calls, crew answers on the heave]

Lead: Away to the deep where the grey gulls fly,
Crew: Heave! Her round!
Lead: We'll see the green land by and by,
Crew: Heave! Her round!

[Verse - drum steady, the pull settles into rhythm]
[Call & Response 2 - same shape, the crew louder, the work harder]

Lead: The Shroud's behind and the sun's ahead,
Crew: Heave! Her round!
Lead: We'll drink to the living and not the dead,
Crew: Heave! Her round!

[Lift - everyone, the anchor breaks free]

Heave! Her round! Heave! Her round!

[Outro - the capstan stops, rope settles, one last "round", silence]
```

---

**31 — Leaving Port _ The Tide Turns**
*Lever key: Sailor · Mixolydian · 84 BPM · fiddle + low whistle · solo + crew · 3:30*

Style prompt:
```
A hopeful Mixolydian sea-air: low whistle and fiddle over rolling 6/8 strings, a solo male voice with a small crew on the refrain, gulls and a far bell — a ship leaving harbour, the bittersweet lift of departure. 84 BPM, 6/8, rolling like a wake. Medium open reverb, salt-air. The optimistic sea cue; melodic where #30 is functional. No taiko, no heavy brass, no Latin, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - low whistle alone, a rising Mixolydian phrase, a far harbour bell]
[A - rolling 6/8 strings enter, fiddle joins the whistle, the ship pulls away]
[Verse - solo male voice, warm, looking back at the land]

The harbour shrinks to a thread of stone,
and still she takes the tide.

[Refrain - small crew joins]

So let her run, let the grey gulls cry,
there's green land on the far side.

[B - fiddle takes the tune up an octave, fuller, hopeful]
[Refrain - solo and crew together, the swell at its peak]
[Outro - whistle alone again, the bell once more, fade on open water]
```

---

**32 — At Sea _ Open Water** *[supersedes existing `Sea _ Sailing`]*
*Lever key: Sailor · Dorian · 70 BPM · accordion + cello · none · 4:30*

Style prompt:
```
A slow majestic Dorian theme on accordion answered by solo cello over long rolling string swells — no crew, no work, just a ship alone on a vast calm sea, grand and a little lonely. 70 BPM, 4/4, tidal and broad. Wide open-ocean reverb. The cinematic sailing cue, instrumental; replaces the old generic sea track with a clear lead pairing and roomier dynamics. No vocals, no taiko, no Latin, no drum kit, no synth, no electric guitar, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - long low string swell, the sea breathing]
[Instrumental A - accordion states a broad Dorian theme, unhurried]
[Instrumental B - solo cello answers the accordion phrase, strings swell under both]
[Instrumental Build - the full string section lifts the theme, grand, open water to the horizon]
[Instrumental Hush - back to accordion and one cello line, the loneliness of it]
[Instrumental Outro - cello holds the last note, the swell recedes, long fade]
```

---

**33 — The Shroud _ The Storm That Never Ends**
*Lever key: Mordvar-adj./sea · octatonic · 72→132 BPM · full orch + battery · wordless terror · 4:00*

Style prompt:
```
An octatonic storm: churning low strings, dissonant brass stabs, a war battery of toms and timpani building from a heave to a frenzy, wordless terrified massed voices with no language — the permanent storm that swallows every ship. Starts 72 BPM, accelerates to 132. 4/4 driving into chaos. Vast wet roaring reverb. The sea as enemy; Shore's storm scale, no resolution, no calm. No Latin, no melody, no folk instruments, no synth, no electric guitar, no drum kit, no EDM, no triumph.
```
Lyrics/structure prompt:
```
[Instrumental Intro - low strings churn, distant timpani, 72 BPM, dread building]
[Instrumental Build - dissonant brass stabs, toms enter, tempo creeps up]
[Voices - massed wordless terror, no language, rising with the storm]

(unpitched mass cry, no words)

[Instrumental Storm - 132 BPM, full battery, brass screaming octatonic, total chaos]
[Voices - the cry peaks and is swallowed by the orchestra]
[Cutoff - a single brass note left ringing in the wet dark]
[Instrumental Outro - low string churn returns, unresolved, the storm goes on, fade]
```

---

**34 — The Eastern Crossing _ Into the Storm**
*Lever key: mixed (epic) · modulating · 80 BPM · full orch + choir · Latin choir · 5:00*

Style prompt:
```
The grand crossing: the Endurance cell on full strings and horns against the Shroud's octatonic storm, a Latin choir holding the light line as the orchestra fights the sea, modulating upward each section toward defiant resolve. 80 BPM, 4/4, broad and building. Huge cinematic reverb. Through-composed set-piece — dread and courage braided, Shore's largest seafaring register, ends resolved but costly. No drum kit, no synth, no electric guitar, no EDM, no autotune, no folk lead.
```
Lyrics/structure prompt:
```
[Intro - low storm churn under a lone horn stating the Endurance cell]
[A - full strings take the Endurance theme, defiant against the rising sea]
[Choir - Latin, holding the light line over the storm]

Lux per umbram
ferrum per ignem

[B - the storm surges, brass and battery, the choir pushes through it, key lifts]
[Climax - full orchestra and choir, the Endurance cell fortissimo, the crossing made]

sanguis per saecula
terra nos vocat

[Cost - sudden hush, solo cello alone with the Endurance tag, what it took]
[Outro - strings return softly, resolved but weary, long fade]
```

---

### G. Camp & Rest

---

**35 — Campfire _ The Sound of Rest**
*Lever key: Folk/intimate · Mixolydian · 60 BPM · solo guitar/lute · none · 4:00*

Style prompt:
```
A solo lute-guitar, finger-picked, in gentle Mixolydian, the Hearth motif stated plainly and completely — the only fully-resolved theme in the score — with a soft low whistle answering once. 60 BPM, free, no percussion. Very close intimate reverb, fire-side. A motif anchor: keep the Hearth fragment warm and simple. Designed to feel safe. No orchestra, no choir, no Latin, no drums, no brass, no synth, no electric guitar, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - solo lute-guitar, a quiet finger-picked figure]
[Instrumental A - the Hearth motif stated plainly, warm, complete]
[Instrumental B - a soft low whistle answers the Hearth phrase once, then is gone]
[Instrumental A' - lute alone again, the Hearth motif repeated, a touch slower]
[Instrumental Outro - the last chord allowed to ring, fire-close, gentle fade]
```

---

**36 — Night Rest _ Sleeping Under Stars**
*Lever key: intimate · Lydian · 48 BPM · music box + harp · none · 4:30*

Style prompt:
```
A music box and harp in slow Lydian, a single sustained string note far underneath like a held breath — barely music, the sound of sleep under an open sky. 48 BPM, no pulse, no melody to follow. Tiny "music box" close reverb over a vast soft tail. The quietest cue in the score; it should almost disappear. No percussion, no choir, no Latin, no brass, no voice, no synth, no electric guitar, no drum kit, no EDM, no build.
```
Lyrics/structure prompt:
```
[Instrumental Intro - music box alone, a slow Lydian turning figure]
[Instrumental A - harp doubles it very softly, a low string note holds underneath]
[Instrumental B - the figure simplifies, fewer notes, slower, drifting]
[Instrumental Drift - music box only, winding down, almost stopping]
[Instrumental Outro - one last music-box note, the low string note fades after it]
```

---

**37 — The Quiet After _ Wounds and Breath**
*Lever key: Iron Chalice-adj. · Aeolian · 52 BPM · solo cello · none · 3:30*

Style prompt:
```
A solo cello alone in Aeolian, slow, breathing, the Endurance cell played as exhaustion rather than heroism, one low sustained string note for a floor — the silence after a hard fight, not victory, just survival. 52 BPM, rubato, no percussion. Dry close room, a little air. Designed silence-adjacent; the decompression cue. No choir, no Latin, no brass, no drums, no second melody, no synth, no electric guitar, no EDM, no swell.
```
Lyrics/structure prompt:
```
[Instrumental Intro - solo cello, one long breath of a note, alone]
[Instrumental A - the Endurance cell played slowly, tired, not triumphant]
[Instrumental B - a low sustained string note enters as a floor, the cello sinks lower]
[Instrumental Hush - the cello stops; only the low note and room air]
[Instrumental Outro - one final cello phrase, unresolved, allowed to die away]
```

---

### H. War & Combat

---

**38 — Enemies Gathering Strength _ The Muster of the Hand**
*Lever key: Mordvar · octatonic · 60→88 BPM · low brass · curse-chant · 4:00*

Style prompt:
```
A slow octatonic dread-build: a single low brass note, a distant war drum that multiplies, a whispered Latin curse-chant accreting voices, tempo creeping 60 to 88 — not a battle, the patient assembly of something terrible. 4/4, relentless acceleration. Vast cold reverb. The Ashen Hand massing; menace by accumulation, never release. No melody, no folk instruments, no bright brass, no resolution, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - one low brass note, 60 BPM, a single far war drum]
[Instrumental Build - more drums answer from further off, the brass note bends down]
[Curse - whispered Latin, the mirror text, one voice then many]

nihil per nihil
cor per inane

[Instrumental B - tempo 76, low strings add an octatonic ostinato, drums closing in]
[Curse - the chant hardens, no longer whispered, still no melody]

nemo per saecula
nihil nos tenet

[Instrumental Peak - 88 BPM, full low brass and battery, massed and waiting]
[Cutoff - it does not resolve; it simply stops, poised — long uneasy fade]
```

---

**39 — A Minor Skirmish _ Blades in the Brush**
*Lever key: Iron Chalice-adj. · Phrygian · 116 BPM · low strings ostinato · none · 2:30*

Style prompt:
```
A tight Phrygian low-string ostinato, a snare-less field drum, short stabbing horn figures — a brief, contained fight, no chorus, no glory, over before it swells. 116 BPM, 4/4, lean and nervy. Dry medium room, no cathedral. Short by design (2:30); the small-stakes combat texture. No choir, no Latin, no taiko, no big brass theme, no synth, no electric guitar, no drum kit, no EDM, no triumphant climax.
```
Lyrics/structure prompt:
```
[Instrumental Intro - low-string Phrygian ostinato starts immediately, no ramp]
[Instrumental A - field drum enters, short horn stabs punctuate, tension tight]
[Instrumental B - the ostinato shifts up a step, strings sharper, the fight quickens]
[Instrumental Peak - one hard tutti hit, then the ostinato alone, thinning]
[Instrumental Outro - drum drops out, ostinato slows and stops mid-phrase — it's over]
```

---

**40 — Charge Into Battle _ Sound the Horns**
*Lever key: Human · Mixolydian · 152 BPM · war horns + trumpets · Latin choir · 3:00*

Style prompt:
```
War horns and trumpets blazing the Endurance cell in bright Mixolydian, full strings galloping, timpani and frame drums hammering a charge, a Latin choir roaring the light text — the moment the line goes forward. 152 BPM, 4/4, headlong. Big heroic field reverb. Pure forward momentum; Shore's Rohan charge register. No drum kit, no synth, no electric guitar, no EDM, no autotune, no slow section, no minor wallow.
```
Lyrics/structure prompt:
```
[Intro - a single rising war-horn call, then the full battery slams in at 152]
[A - trumpets blaze the Endurance cell, strings gallop beneath, no hesitation]
[Choir - Latin, full voice, the light text shouted not sung]

Ferrum per ignem!
Terra nos vocat!

[B - horns answer the trumpets in canon, the charge accelerates feel]
[Climax - everything at once, the Endurance cell fortissimo, choir roaring]

Vocat! Vocat! Terra nos vocat!

[Outro - one last horn blast and a hard tutti stop — no fade, no comedown]
```

---

**41 — The Large Battle _ The Field of Iron**
*Lever key: mixed (suite) · Aeolian/octatonic · 96→168 BPM · full orch + battery · full choir + curse · 5:00*

Style prompt:
```
A full battle suite: the Endurance cell (Aeolian, human choir) versus the Hollowing (octatonic, curse-chant) traded across the orchestra, taiko and dhol war battery, multiple tempo gears 96 to 168, a desperate mid-battle hush, then a brutal return. 4/4 through 6/8. Vast cinematic reverb. Through-composed, Shore's Pelennor scale — the score's biggest set-piece. No drum kit, no synth, no electric guitar, no EDM, no autotune, no clean victory.
```
Lyrics/structure prompt:
```
[Intro - distant battery, the Endurance cell on horns, 96 BPM, the lines meet]
[A - human choir Latin against the orchestra, the light side pressing]

ferrum per ignem!

[B - the Hollowing answers, octatonic brass, curse-chant rising, tempo 132]

nihil per nihil!

[Hush - sudden near-silence, a solo cello plays the Endurance tag, the battle's cost]
[Return - 168 BPM, full battery, both choirs at once, total collision]

Terra nos vocat! / Nihil nos tenet!

[Cutoff - a single timpani roll cut dead]
[Outro - solo cello, the Endurance tag unfinished, smoke clearing, slow fade]
```

---

**42 — The Siege _ Hold the Walls**
*Lever key: Dwarven · Phrygian · 100 BPM · anvils + low brass · stomp-chant · 4:30*

Style prompt:
```
A defensive grind: anvils and low brass in heavy Phrygian, a relentless boot-stomp like ram-blows on a gate, dwarven syllabic stomp-chant of defiance — attrition, walls, holding not charging. 100 BPM, heavy 4/4, no acceleration, just endurance. Great stone-hall reverb with a long slap. Distinct from the charge: this one digs in. No bright trumpets, no Latin, no taiko frenzy, no synth, no electric guitar, no drum kit, no EDM, no rout.
```
Lyrics/structure prompt:
```
[Intro - one massive low-brass hit like a ram on the gate, then the stomp begins]
[A - anvils mark the beat, low brass states a grim Phrygian figure, walls holding]
[Chant - dwarven, syllabic, defiant, stomped]

khaz! tharak khaz! dûm-dûm khaz!

[B - the ram-blows quicken, the figure tightens, the wall strains but holds]
[Hush - one breath where the stomp stops — the lull between assaults]
[Return - the stomp slams back harder, chant full-throated, the line holds]

khaz! tharak khaz!

[Outro - the ram-blows slow, one last anvil rings out over the held hall]
```

---

**43 — Vaeroth the Pale _ The Hierarch**
*Lever key: Mordvar · whole-tone · 108 BPM · contrabassoon + choir · curse-chant · 4:00*

Style prompt:
```
A cold whole-tone boss theme: contrabassoon and muted low brass over a precise mechanical pulse, a controlled Latin curse-chant — a brilliant, fragile zealot, not a brute; the menace is intellect and certainty. 108 BPM, 4/4, clinical. Large cold reverb. Distinct from Mordvar (slow and vast) and the Ashlord (tragic) — Vaeroth is sharp and exact. No warm strings, no folk, no triumphant brass, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - a precise mechanical low pulse, contrabassoon enters whole-tone, cold]
[A - muted low brass states Vaeroth's clipped theme, exact, controlled]
[Curse - a measured Latin curse-chant, no passion, total certainty]

cor per inane
nemo per saecula

[B - the pulse tightens, strings add a brittle whole-tone shimmer, pressure rising]
[Climax - brass and chant snap to full force, still controlled, then a fracture]
[Cutoff - the mechanical pulse stutters and stops — the fragility shows]
[Outro - contrabassoon alone, one bent note, cold fade]
```

---

**44 — The Ashlord _ The Mask of Caerith**
*Lever key: Naergrim/Aelorin · Lydian rotted to cluster · 92 BPM · corrupted vowel-song · mixed · 4:30*

Style prompt:
```
A tragedy wearing armour: the Aelorin Song motif in Lydian, beautiful for two phrases, then rotting into Naergrim clusters and detuned strings — a Second Age Vigil-Keeper turned, the music remembers what he was. 92 BPM, 4/4 decaying into no pulse. Vast reverb curdling to airless close. The most tragic villain cue; never purely monstrous. No taiko frenzy, no Latin curse, no folk, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - the Aelorin Song motif, glass harmonica, achingly beautiful, Lydian]
[Voice - a wordless Aelorin vowel-line, pure, for one phrase]

aelúriel… síoma…

[Turn - the harmony curdles, strings detune, the voice cracks into a Naergrim whisper]

…ssha… vesh…

[A - the Song motif returns warped, low brass beneath it, grief and menace at once]
[Climax - the beauty and the rot collide, full and dissonant, the mask holding]
[Cutoff - everything drops to one detuned harmonic — what's left of him]
[Outro - a single broken fragment of the Song, unresolved, airless fade]
```

---

**45 — Mordvar _ The Hollowing**
*Lever key: Mordvar · the inverted Song · 50 BPM · dissonant low brass · curse-chant · 4:30*

Style prompt:
```
The franchise's dark anchor: the Song motif inverted and emptied — descending open fifths on dissonant low brass and contrabassoon that refuse to close, a vast slow Latin curse-chant (the mirror text), war battery felt more than heard. 50 BPM, immense 4/4, glacial. Enormous airless reverb, no warmth anywhere. A motif anchor — keep the Hollowing's inverted shape exact and recognisable. No melody resolution, no folk, no bright brass, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - one immense low-brass open fifth, descending, refusing to resolve]
[A - contrabassoon states the Hollowing — the Song motif inverted and emptied]
[Curse - the vast slow Latin mirror text, low voices, no melody]

Nihil per nihil
cor per inane
nemo per saecula
nihil nos tenet

[B - the descending fifths stack, war battery felt under the floor, no acceleration]
[Climax - the full Hollowing fortissimo, the curse at its widest, then nothing]
[Cutoff - total silence for a beat]
[Outro - one low note bends downward forever, airless, very long fade]
```

---

**46 — The Fighting Retreat _ The Ashfields**
*Lever key: Iron Chalice · Aeolian · 116 BPM · war horn + strings · sparse male · 4:00*

Style prompt:
```
Heroic loss: the Endurance cell on a strained war horn over driving Aeolian strings and a hard field-drum tread, sparse male voices — a retreat that is also a victory, ground given so that people live. 116 BPM, 4/4, urgent but disciplined, never a rout. Big cold field reverb. Defiant melancholy; Shore's "noble defeat" register. No triumphant fanfare, no Latin, no taiko frenzy, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - hard field-drum tread, a strained war-horn call, no triumph in it]
[A - driving Aeolian strings, the Endurance cell on the horn, holding shape under pressure]
[Voices - sparse male, half-spoken, grim]

stamus… stamus…

[B - the strings press harder, the tread quickens, discipline not panic]
[Hush - one bar where it nearly breaks — solo horn alone, then strings catch it]
[Return - the Endurance cell restated, defiant, the line still ordered, falling back]
[Outro - the tread recedes into distance, horn last, fade — they got out]
```

---

**47 — The Last Stand _ No Ground Behind**
*Lever key: Iron Chalice/Human · Aeolian→Mixolydian · 84→144 BPM · full orch · full choir · 4:30*

Style prompt:
```
From dread to defiance: a low Aeolian dread-bed and a slow Endurance statement that gathers the full orchestra and a Latin choir, accelerating 84 to 144 as it modulates Aeolian to Mixolydian — backs to the wall, then everything given at once. 4/4. Vast cinematic reverb. Through-composed; the desperate-courage set-piece, distinct from the charge by starting in despair. No drum kit, no synth, no electric guitar, no EDM, no autotune.
```
Lyrics/structure prompt:
```
[Intro - low Aeolian dread-bed, 84 BPM, a lone cello with the Endurance tag]
[A - strings gather under it, drums enter slow, the choir hums low, no words yet]
[Build - tempo lifts, the Endurance cell strengthens, Latin choir finds the text]

ferrum per ignem

[Turn - modulation to Mixolydian, 144 BPM, defiance breaks through the dread]
[Climax - full orchestra and choir, the Endurance cell at full cry, all of it given]

Terra nos vocat!

[Cutoff - one tutti chord held, then cut — the outcome left unsaid]
[Outro - a single horn holds the Endurance tag over silence, slow fade]
```

---

**48 — The Muster of the Alliance _ Many Banners**
*Lever key: mixed (suite) · Mixolydian · 100 BPM · rotating culture leads · layered choirs · 5:00*

Style prompt:
```
A muster suite where each culture's palette enters in turn and then layers: human horns and Latin choir, Aelorin glass harmonica and vowel-song, dwarven 6/8 dulcimer and stomp-chant, sea concertina — all converging on the Endurance cell. 100 BPM, modulating, 4/4 over 6/8. Huge field reverb. Through-composed; the "the world stands together" cue, every palette deliberately distinct then unified. No drum kit, no synth, no electric guitar, no EDM, no autotune.
```
Lyrics/structure prompt:
```
[Intro - lone human war horn states the Endurance cell over a field drum]
[Human - Latin choir and horns take it, banners of the kingdoms]

ferrum per ignem

[Aelorin - glass harmonica and wordless vowel-song layer the Song motif over it]

aelúriel… síoma…

[Dwarven - 6/8 hammered dulcimer and low stomp-chant join, the holds answer]

khazûn… tharak khaz!

[Sea - a concertina and crew refrain ride in over the top]
[Convergence - all palettes lock onto the Endurance cell at once, layered choirs]

Terra nos vocat!

[Outro - the leads peel away to the lone war horn that began it, proud fade]
```

---

### I. Cinematic & Story

---

**49 — The Vigil _ The Night Before**
*Lever key: Iron Chalice · Aeolian · 52 BPM · solo cello + low whistle · distant male · 4:00*

Style prompt:
```
The night before the battle: a solo cello and a far low whistle in Aeolian, a single field drum like a slow heartbeat, a distant unison male line barely there — dread and resolve held very quietly, no swelling. 52 BPM, rubato, almost still. Cold open-camp reverb, fires and dark. Designed restraint; the calm before, not the storm. No big brass, no Latin choir mass, no taiko, no synth, no electric guitar, no drum kit, no EDM, no climax.
```
Lyrics/structure prompt:
```
[Intro - one slow field-drum beat like a heart, a solo cello enters Aeolian]
[A - the Endurance cell played softly, questioning, not heroic]
[Distant Voice - a far unison male line, almost inaudible, no full words]

stamus…

[B - a low whistle answers the cello from across the camp, lonely]
[Hush - cello alone, the drum stops, the longest silence in the cue]
[Outro - the Endurance tag, unfinished, the drum-heart one last time, fade to dark]
```

---

**50 — Heroes Reunited _ The Fellowship Whole**
*Lever key: mixed (motif weave) · Mixolydian · 76 BPM · leitmotif weave · warm choir · 4:00*

Style prompt:
```
A motif-weave reunion: the Hearth fragment opens, then Roland's Endurance cell, the Aelorin Song, a dwarven dulcimer figure and a sea phrase all braid together warmly in Mixolydian — companions back together, the score's themes embracing. 76 BPM, 4/4, glowing. Warm medium hall. The emotional pay-off cue; recognisably every character's theme at once. No battle battery, no curse-chant, no synth, no electric guitar, no drum kit, no EDM, no dissonance.
```
Lyrics/structure prompt:
```
[Intro - the Hearth fragment, solo lute, warm and complete]
[A - solo cello adds the Endurance cell over it, gently, like a greeting]
[B - glass harmonica laces the Aelorin Song through, then a dwarven dulcimer figure]
[Weave - all the motifs braid together, strings warm underneath, a soft choir hums]

(wordless warm choir, open vowels, no language)

[Climax - the full ensemble, every theme audible at once, glowing not loud]
[Outro - back to the Hearth fragment, lute alone, resolved, a held warm chord]
```

---

**51 — A Marriage _ Two Hands Bound**
*Lever key: Folk/Human · major (Ionian) · 88 BPM · harp + oboe + fiddle · joyful soloist · 3:30*

Style prompt:
```
Unambiguous joy — the score's only pure major-key cue: harp, oboe and a warm fiddle dancing an Ionian processional, hand drum and a single bright bell, a joyful solo female voice with a small glad chorus. 88 BPM, 4/4 with a lift. Warm bright room, a hall full of people. Folk-ceremonial, light; deliberately not orchestral-grand — a real wedding, not a coronation. No minor wallow, no Latin dirge, no taiko, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - solo harp, a bright rising Ionian figure, one clear bell]
[A - oboe takes a glad processional melody, fiddle and hand drum lift it]
[Voice - a joyful solo female, with a small warm chorus answering]

Two hands bound and the long road done,
the hearth is lit, the two are one.

[B - fiddle leads a dancing variation, the room clapping along]
[Refrain - soloist and chorus together, the brightest moment in the score]

The hearth is lit, the two are one!

[Outro - harp and bell as at the start, a warm settled major chord, glad fade]
```

---

**52 — Grief _ What the Archive Lost**
*Lever key: Human · Aeolian · 46 BPM · solo viola · solo female lament · 4:00*

Style prompt:
```
Pure loss: a solo viola in slow Aeolian, almost no accompaniment, a single solo female voice in a wordless lament — Henrietta's death, intimate grief, no orchestra to hide behind. 46 BPM, rubato, no percussion ever. Close dry room, the sound of one person mourning. The sadness cue; small on purpose. No choir mass, no Latin, no brass, no drums, no swell, no synth, no electric guitar, no EDM, no resolution that comforts.
```
Lyrics/structure prompt:
```
[Intro - solo viola alone, a slow falling Aeolian phrase, bare]
[A - a single wordless female voice enters, a lament, no language]

(wordless, low, grieving — open vowels only)

[B - the viola answers the voice, they trade phrases, no other instrument]
[Hush - the voice alone for one phrase, unbearable and quiet]
[A' - viola takes the lament one last time, lower, slower]
[Outro - the viola does not resolve the final note; it simply stops. Silence.]
```

---

**53 — Noble Sacrifice _ The Blow at the Marsh**
*Lever key: mixed · Aeolian→Lydian · 60 BPM · cello → full strings · Latin choir · 4:30*

Style prompt:
```
A death that means something: a solo cello (Endurance, Aeolian) carried up by gathering strings into the Aelorin Song (Lydian) as a Latin choir lifts — grief turned to transcendence, the cost paid and accepted. 60 BPM, 4/4, a slow inexorable rise. Vast cathedral reverb. The noble-death cue, distinct from pure Grief by its upward resolution into the Song. No battery, no curse-chant, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - solo cello, the Endurance cell, alone and tired]
[A - strings gather under it slowly, the harmony beginning to lift Aeolian toward Lydian]
[Choir - a Latin choir enters low, the light text, reverent]

lux in fine

[Turn - the Endurance cell transforms into the Aelorin Song, the key opens to Lydian]
[Climax - full strings and choir, the Song motif finally allowed to nearly resolve]

terra nos vocat

[Release - everything drops to one held Lydian chord — the cost accepted]
[Outro - solo cello returns, at peace, one last Endurance tag, long warm fade]
```

---

**54 — Betrayal _ The Mole Revealed**
*Lever key: Naergrim-adj. · minor→cluster · 64 BPM · low strings + clock tick · none · 3:00*

Style prompt:
```
Cold realisation: low strings in tightening minor over a dry mechanical clock tick, a trusted-warm motif fragment heard once then soured into a Naergrim cluster — the moment a friend turns out to be the knife. 64 BPM, 4/4, clinical and dropping. Close airless room. Through-composed; the score's "trust breaks" cue, no comfort, no bombast. No choir, no Latin, no taiko, no warm resolution, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Instrumental Intro - a dry mechanical tick, alone, like a clock in a quiet room]
[Instrumental A - low strings enter minor, a familiar warm motif fragment heard once, trusting]
[Instrumental Turn - the fragment sours, detunes, curdles into a Naergrim cluster]
[Instrumental B - the strings tighten downward, the tick speeds slightly, the floor goes out]
[Instrumental Cutoff - the tick stops dead — the realisation lands]
[Instrumental Outro - one airless cluster held, no resolution, hard fade]
```

---

**55 — Hope Rekindled _ The Turn**
*Lever key: Human · Aeolian→Mixolydian · 72→104 BPM · solo oboe → full orch · choir build · 4:00*

Style prompt:
```
The turn from despair: a lone oboe in fragile Aeolian, a fragment of the Endurance cell finding its feet, strings and then a Latin choir gathering as the key opens to Mixolydian and the tempo lifts 72 to 104 — not victory yet, but the moment it becomes possible. 4/4. Warm growing reverb. Through-composed arc from doubt to resolve. No battle battery, no curse, no synth, no electric guitar, no drum kit, no EDM, no premature triumph.
```
Lyrics/structure prompt:
```
[Intro - solo oboe, fragile Aeolian, a broken piece of the Endurance cell]
[A - the oboe finds the whole cell, hesitant; soft strings agree underneath]
[Build - the key warms toward Mixolydian, tempo lifts, a choir hums in, hope catching]

(wordless warm choir rising)

[Turn - 104 BPM, the Endurance cell stated whole and strong for the first time]
[Climax - full strings and a Latin choir, bright but not yet triumphant — possibility]

terra nos vocat

[Outro - it does not over-resolve; it lifts and holds, open, hopeful, fade up]
```

---

**56 — Epilogue _ The Road Home**
*Lever key: Folk/Human · Mixolydian · 80 BPM · solo cello + oboe · none · 4:00*

Style prompt:
```
Quiet closure: solo cello and oboe trading the Endurance cell and the Hearth fragment, gently, in warm Mixolydian over light strings — the war over, the road leading home, earned peace not fanfare. 80 BPM, 4/4, unhurried. Warm open-field reverb at dusk. The denouement; recognisable themes at rest. No battery, no choir mass, no Latin, no taiko, no synth, no electric guitar, no drum kit, no EDM, no swell.
```
Lyrics/structure prompt:
```
[Instrumental Intro - solo cello, the Endurance cell, calm now, no weight on it]
[Instrumental A - oboe answers with the Hearth fragment, the two themes at peace]
[Instrumental B - light strings join warmly, the road opening ahead, gentle motion]
[Instrumental Bridge - cello and oboe alone again, intimate, almost home]
[Instrumental A' - the themes restated together, settled, complete]
[Instrumental Outro - one warm held chord at dusk, long peaceful fade]
```

---

### Endings (Game Three — authored finale cues)

---

**57 — The Return _ Released**
*Lever key: Aelorin/Human · Lydian resolving · 58 BPM · full strings + soloist · full choir · 5:00*

Style prompt:
```
Resolution and release: the Aelorin Song motif, unfinished for the entire trilogy, finally completes — full strings, a wordless soloist, a Latin choir resolving the light text, the missing eighth note at last sounded. 58 BPM, 4/4, a slow opening-out. Vast warm cathedral reverb. The "Mordvar dissolved, the fear resolved" ending; the only cue where the Song is allowed to close. No battery, no curse, no dissonance, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - glass harmonica, the Song motif, still unfinished, fragile]
[A - full strings gather it up, warm, the Endurance cell entwined beneath]
[Choir - the Latin light text, complete, unhurried]

Lux per umbram
ferrum per ignem

[Turn - the Song reaches its final note — and this time it resolves, the eighth sounded]
[Climax - full choir and strings on the resolved Song, release not triumph]

sanguis per saecula
terra nos vocat

[Outro - everything settles onto the home chord, the soloist holding it, long warm fade]
```

---

**58 — The Hold _ Carried Forever**
*Lever key: Iron Chalice · Aeolian, unresolving · 54 BPM · solo cello + low choir · low male · 5:00*

Style prompt:
```
Love as permanent cost: the Endurance cell on solo cello and a low male choir, dignified and warm but the harmony never fully resolves — the weight is carried, not put down, forever. 54 BPM, slow 4/4. Deep stone reverb. The "contained within the bloodline" ending; beautiful and unresolved on purpose, distinct from The Return by its withheld cadence. No battery, no curse, no bright brass, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - solo cello, the Endurance cell, steady, accepting]
[A - a low male choir enters beneath, warm, dignified, no words yet]
[Choir - a few sustained Latin words, the weight named]

stamus… portamus…

[B - the harmony rises as if to resolve — and holds back, the cadence withheld]
[Climax - cello and choir at their fullest, noble, but never closing the chord]
[Hush - back to solo cello, the Endurance cell, the weight still there]
[Outro - the final note held, unresolved, carried — very long fade, no cadence]
```

---

**59 — The Fracture _ The Price of Refusal**
*Lever key: Mordvar-adj. · shattered, no cadence · 60 BPM · broken orchestra · fractured choir · 5:00*

Style prompt:
```
The price of refusal: the Endurance cell and the Hollowing fragment each broken, neither winning, an orchestra that keeps almost cohering and shattering, a choir split between light text and curse with neither prevailing — survival without resolution, the cost on the world. 60 BPM, 4/4 destabilising. Vast cold reverb. The bleak ending; deliberately denied catharsis, distinct from both others by having no settled chord at all. No clean victory, no synth, no electric guitar, no drum kit, no EDM.
```
Lyrics/structure prompt:
```
[Intro - a broken fragment of the Endurance cell, strings, it doesn't complete]
[A - the Hollowing answers, also broken, neither motif able to finish]
[Choir - light text and curse text overlapping, fighting, neither winning]

terra nos vocat… / …nihil nos tenet…

[B - the orchestra gathers as if toward a climax — and shatters before it lands]
[Collapse - fragments of every theme scattered, no key, no centre]
[Cutoff - a single unresolved note, suspended, wrong]
[Outro - it does not resolve and does not fade cleanly — it just stops. Silence.]
```

---

## 8. Production & maintenance notes

- **Lengths.** Suno tends to 2–4 min per generation; for the 4:30–5:00
  set-pieces (03, 17, 33, 34, 41, 48, 57–59) generate in two passes and join,
  or use Suno's extend. The §5 lengths are targets, not hard cuts.
- **Audition in pairs.** The whole point of this doc is separation — always
  listen to a new track against the previous one in its category before
  accepting the take. If they blur, push levers 1–3 further before re-rolling.
- **`.ogg`, stereo, 44.1 kHz** per `AUDIO_DESIGN.md §Audio File Conventions`;
  drop into `assets/audio/music/` with the §5 title.
- **Wiring** is out of scope here — `AUDIO_DESIGN.md` covers the MusicPlayer
  autoload, crossfade and ducking. This doc only supplies content.
- **When this doc changes:** add the row to §5, give it a distinct lever key,
  and update `CLAUDE.md`'s maintenance table reference if the track set's
  scope shifts. New named character/leitmotif → add it to §3.
