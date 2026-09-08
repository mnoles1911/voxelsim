# Music to generate, 2026-09-08: the gaps and the Suno prompts

Companion to `docs/music-design.md` (the pool system) and `docs/music-prompts.md` (the
59-cue catalogue and its template). Everything here is instrumental, in the Oblivion /
Skyrim / Jeremy Soule idiom the catalogue already leans on, and follows the catalogue's
rules: lead the style box with the lead instrument, key and BPM; structure box in
`[Instrumental ...]` sections; flute always constrained to steady stepwise lines in a fixed
register; no choir, no vocals; keys varied and never D minor (that is the Main Theme's);
3-5 minutes with a long quiet tail. Short stingers and anything under about thirty seconds
are on the backlog by owner direction, not here.

## 1. The gap list

Library on disk today: 30 playable WAVs plus one MP4 (`17 — Lirien-Thal`, needs re-encoding;
no regeneration). Against the pool targets in the design doc:

| # | pool | cue | key | BPM | lead | source |
|---|---|---|---|---|---|---|
| N1 | Explore, Night | Starfall _ The Plains at Night | E♭ Lydian | 46 | solo piano | new, §2 |
| N2 | Explore, Night | Moonrise _ Over the Greatwood | A♭ Aeolian | 44 | celesta + harp | new, §2 |
| N3 | Explore, Night | Cold Stars _ The High Passes | F# Dorian | 50 | muted French horn | new, §2 |
| N4 | Explore, Night | The Owl Hours _ Fen After Dark | C Aeolian (drone) | 42 | bass clarinet + bowed vibraphone | new, §2 |
| N5 | Explore, Night | Embers _ The Road After Dark | G Dorian | 52 | solo lute-guitar + viola | new, §2 |
| N6 | Explore, Night | Lantern _ Home Lights Far Off | C Lydian | 44 | cello harmonics + celesta | new, §2 |
| A1 | Explore, Dawn | First Light _ The Meadows Wake | B♭ Lydian | 56 | solo oboe | new, §3 |
| A2 | Explore, Dawn | Mist Lifting _ The River at Dawn | A♭ Mixolydian | 60 | low whistle | new, §3 |
| K1 | Explore, Dusk | Long Shadows _ The Road Home | G Aeolian | 58 | viola + soft horn | new, §4 |
| K2 | Explore, Dusk | Last Light _ On the Water | B Mixolydian | 54 | solo cello + hammered dulcimer | new, §4 |
| R1 | Explore, Rain | Rain on the Road | C# Dorian | 50 | piano + harp | new, §5 |
| R2 | Explore, Rain | The Storm Passing _ Grey Hills | E♭ Dorian | 48 | cor anglais | new, §5 |
| C1 | Cave | Drip and Dark _ The Deep Hollows | no centre (A drone) | 40 | bowed crotales + contrabass | new, §6 |
| C2 | Cave | The Sleeping Stone _ Old Passages | A♭ Phrygian | 48 | bass clarinet + cello | new, §6 |
| 36 | Camp / Night | Night Rest _ Sleeping Under Stars | G Lydian | 48 | music box + harp | catalogue, §7 |
| 31 | Water | Leaving Port _ The Tide Turns | B♭ Mixolydian | 84 (6/8) | fiddle + low whistle | catalogue, §7 |
| 32 | Water | At Sea _ Open Water | B Dorian | 70 | accordion + cello | catalogue, §7 |
| 39 | Combat | A Minor Skirmish _ Blades in the Brush | G Phrygian | 116 | low strings ostinato | catalogue, §7 |
| 40 | Combat | Charge Into Battle _ Sound the Horns | E♭ Mixolydian | 152 | war horns + trumpets | catalogue, §7 |
| 46 | Combat | The Fighting Retreat _ The Ashfields | F# Aeolian | 116 | war horn + strings | catalogue, §7 |
| 01 | Menu | Prelude _ The Eighth Star | F Lydian | 54 | glass harmonica | catalogue, §7 |
| 03 | Menu | End Credits _ The Long Twilight | B♭ Aeolian | 76 | solo cello | catalogue, §7 |

Twenty-two cues. Fourteen are new (sections 2-6); eight already have prompts in the
catalogue and are reproduced verbatim in section 7 so this file can be fed to Suno on its
own. Cue 36 counts for both the Camp pool and the Night pool.

Two housekeeping items that need no generation: audition `Discovery_Wonder.wav` against
the Explore rules in the design doc (it may belong in Cinematic), and re-encode
`17 — Lirien-Thal _ The Silverwood.mp4` to 16-bit PCM WAV.

## 2. Explore, Night (six cues)

The rules for every Night cue: the sparsest and slowest pool in the game; 40-52 BPM or no
pulse; one lead voice and at most two supporting colours; flat dynamics with no build that
could read as an event; a long tail so the authored silence after it starts before the
listener notices. Oblivion's night wilderness cues ("Dusk at the Market", "Watchman's
Ease") and Skyrim's "Secunda" are the reference feel: solo piano or celesta, strings held
underneath, nothing hurried.

---

**N1 — Starfall _ The Plains at Night**
*Key E♭ Lydian · 46 BPM · Human · solo piano · instrumental · 4:00*

Style prompt:
```
Solo piano in E♭ Lydian, sparse and unhurried, single notes and open fifths left to ring, high string harmonics held far underneath like starlight — the plains at night, nobody awake. 46 BPM, barely a pulse. Intimate close piano with a long soft hall tail. Flat dynamics throughout, no build, no climax, nothing that would startle a sleeping camp. Night exploration in the tradition of Jeremy Soule's Oblivion night cues and Skyrim's "Secunda". Full-length cue, developing by small variation only, no early fade, very long outro. Instrumental. No percussion, no brass, no choir, no solo vocals, no flute, no glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - solo piano, one slow E♭ Lydian phrase, long silences between notes]
[Instrumental A - the phrase repeats a fifth higher, high string harmonics hold underneath]
[Instrumental B - a second voice in the left hand, open fifths, still sparse]
[Instrumental Drift - the phrase thins to single notes, strings almost inaudible]
[Instrumental C - the opening phrase returns in the original register, unchanged]
[Instrumental Outro - one held piano chord, the string harmonic outlasts it, very long fade]
```

---

**N2 — Moonrise _ Over the Greatwood**
*Key A♭ Aeolian · 44 BPM · Aelorin · celesta + harp · instrumental · 4:00*

Style prompt:
```
Celesta and harp in slow A♭ Aeolian, a small turning figure passed between them over a single low string drone, one bass clarinet note answering now and then from the dark — moonlight through old leaves, the forest asleep. 44 BPM, free and unhurried. Long silvery reverb, soft attacks. Flat dynamics, no build, the cue should sit behind the listener. Night wilderness in the lineage of Soule's Oblivion forest cues, sparser and slower than any day cue. Full-length cue, varied only by register and spacing, no early fade, very long outro. Instrumental. No percussion, no brass, no choir, no solo vocals, no flute, no glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - low string drone on A♭, celesta states a three-note turning figure]
[Instrumental A - harp answers the figure in a lower octave, drone unchanged]
[Instrumental B - a single bass clarinet note once per phrase, far away]
[Instrumental Drift - celesta alone, the figure slower, more space between notes]
[Instrumental C - harp and celesta together once, very soft]
[Instrumental Outro - the drone alone, one last celesta note, very long fade]
```

---

**N3 — Cold Stars _ The High Passes**
*Key F# Dorian · 50 BPM · Dwarven-adjacent · muted French horn · instrumental · 4:00*

Style prompt:
```
A single muted French horn in F# Dorian, distant and cold, over slow string harmonics and a low sustained drone — the high passes at night, snow under starlight, no wind yet. 50 BPM, spacious 4/4 with long rests. Wide open-air reverb with a slow mountain slap-back. Flat dynamics, mezzo-piano at most, no brass swell, no second horn, no climax. Night mountains in the manner of Soule's Skyrim peak cues stripped down to their quietest moment. Full-length cue, the theme restated with small changes, no early fade, long outro. Instrumental. No percussion, no timpani, no choir, no solo vocals, no flute, no glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - low drone on F#, string harmonics fade in above it]
[Instrumental A - muted horn states a slow wide theme, one phrase, then rests]
[Instrumental B - the theme again a fourth higher, strings thicken very slightly]
[Instrumental Drift - horn silent, harmonics and drone only, cold and still]
[Instrumental C - the horn returns with the opening phrase, smaller than before]
[Instrumental Outro - horn holds one note and stops, drone fades slowly into nothing]
```

---

**N4 — The Owl Hours _ Fen After Dark**
*Key C Aeolian drone · 42 BPM · Dead-adjacent · bass clarinet + bowed vibraphone · instrumental · 4:00*

Style prompt:
```
Bass clarinet in low C Aeolian, slow single notes over a bowed vibraphone shimmer and a barely audible contrabass drone, occasional plucked harp harmonics like drops of water — a fen at night, still water, something awake in the reeds. 42 BPM, no fixed meter. Very long damp reverb. Uneasy but calm, never threatening: flat dynamics, no build, no stinger. Night exploration in the vein of Soule's quietest Oblivion marsh and Morrowind ambience. Full-length cue, drifting rather than developing, no early fade, very long outro. Instrumental. No percussion, no brass, no choir, no solo vocals, no flute, no glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - contrabass drone on C, bowed vibraphone shimmer fades in]
[Instrumental A - bass clarinet, three slow low notes, long rests]
[Instrumental B - harp harmonics dropped at random into the shimmer, clarinet repeats]
[Instrumental Drift - vibraphone and drone only, the fen breathing]
[Instrumental C - bass clarinet returns one octave higher, still slow]
[Instrumental Outro - shimmer thins to nothing, one last harp harmonic, very long tail]
```

---

**N5 — Embers _ The Road After Dark**
*Key G Dorian · 52 BPM · Folk / intimate · solo lute-guitar + viola · instrumental · 4:00*

Style prompt:
```
Solo nylon-string lute-guitar in G Dorian, fingerpicked slowly, a solo viola joining with a long-bowed countermelody, a soft low string pad underneath — walking the road after dark, a farmhouse light somewhere ahead. 52 BPM, gentle 3/4, unhurried. Warm close-miked guitar, a soft room reverb, viola further back. Flat dynamics, intimate throughout, no build. Folk-inflected night cue in the tradition of Soule's Oblivion "Harvest Dawn" warmth slowed to a night walk. Full-length cue, the melody restated with small ornaments, no early fade, long outro. Instrumental. No percussion, no brass, no choir, no solo vocals, no flute, no glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - solo lute-guitar, a slow G Dorian fingerpicked pattern]
[Instrumental A - the guitar states a simple folk melody over the pattern]
[Instrumental B - viola enters with a long countermelody, low string pad beneath]
[Instrumental Development - guitar and viola trade the melody, still soft]
[Instrumental Hush - guitar alone again, the pattern slower]
[Instrumental Outro - viola holds one note over the last guitar chord, long fade]
```

---

**N6 — Lantern _ Home Lights Far Off**
*Key C Lydian · 44 BPM · Human · cello harmonics + celesta · instrumental · 4:00*

Style prompt:
```
Solo cello playing slow natural harmonics in C Lydian, celesta answering in the highest register, a warm held string chord far below — a village seen from the hill at night, lanterns in windows, the walk still long. 44 BPM, free and floating. Soft glassy reverb, long tail. Flat dynamics, tender and unhurried, no build. Night exploration in the manner of Soule's Oblivion at its most pastoral, made small and quiet. Full-length cue, varied by register and spacing, no early fade, very long outro. Instrumental. No percussion, no brass, no choir, no solo vocals, no flute, no glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - warm held string chord in C Lydian, very soft]
[Instrumental A - cello harmonics state a short rising phrase, then rest]
[Instrumental B - celesta answers the phrase high above, sparse]
[Instrumental Development - cello and celesta alternate, the chord slowly shifting]
[Instrumental Hush - the held chord alone, breathing]
[Instrumental Outro - one cello harmonic, one celesta note after it, very long fade]
```

## 3. Explore, Dawn (two cues)

Dawn cues are thin and bright and rise gently, but they are still Explore: no crescendo
that reads as an event. A single woodwind over held strings, the harmony opening as the
light comes. Oblivion's "Harvest Dawn" and "Sunrise of Flutes" are the references, with the
flute rule applied.

---

**A1 — First Light _ The Meadows Wake**
*Key B♭ Lydian · 56 BPM · Human · solo oboe · instrumental · 4:00*

Style prompt:
```
Solo oboe in B♭ Lydian over slow held strings and a soft harp, a pastoral morning theme that opens a little more with each phrase — mist on the meadows, the first birds, nobody else up. 56 BPM, gentle 4/4. Warm open-air reverb, soft attacks. Gently rising in warmth but never in volume: mezzo-piano throughout, no climax, no brass. Dawn exploration in the lineage of Jeremy Soule's Oblivion "Harvest Dawn". Full-length cue, the theme restated with the harmony brightening, no early fade, long quiet outro. Instrumental. No percussion, no brass, no choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - held strings in B♭ Lydian, a single harp note, grey light]
[Instrumental A - solo oboe states a simple pastoral theme, one phrase, rests]
[Instrumental B - the theme again, harp arpeggios under it, strings warm slightly]
[Instrumental Development - oboe varies the theme upward, the harmony opening]
[Instrumental Hush - strings and harp only, the sun on the grass]
[Instrumental Outro - oboe restates the first phrase and holds its last note, long fade]
```

---

**A2 — Mist Lifting _ The River at Dawn**
*Key A♭ Mixolydian · 60 BPM · Folk / river · low whistle · instrumental · 4:00*

Style prompt:
```
Low whistle in A♭ Mixolydian playing simple steady stepwise lines in a fixed register, over harp and slow low strings, a soft frame-drum heartbeat entering late and staying quiet — mist lifting off a river at dawn, the water starting to move. 60 BPM, easy 4/4. Wide riverside reverb. Flat dynamics, warm and unhurried, no build. Dawn exploration after Soule's Oblivion pastoral cues with a Celtic folk edge. Full-length cue, the melody restated and gently ornamented, no early fade, long outro. Instrumental. No flute glissando, no pitch sweeps, no portamento, no octave runs, no whistle effects, no brass, no choir, no solo vocals, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - low strings and harp in A♭ Mixolydian, the river in mist]
[Instrumental A - low whistle states a simple stepwise melody in one register]
[Instrumental B - harp arpeggios open under the melody, strings warm]
[Instrumental Development - a soft frame drum enters very quietly, the whistle repeats and ornaments]
[Instrumental Hush - harp alone, the drum stops, light on the water]
[Instrumental Outro - whistle restates the first phrase and holds, long fade]
```

## 4. Explore, Dusk (two cues)

Dusk cues settle rather than rise: warm, lower, a little heavier than Day, with the Night
palette starting to show through by the end. Viola, soft horn, harp, cello.

---

**K1 — Long Shadows _ The Road Home**
*Key G Aeolian · 58 BPM · Human · viola + soft French horn · instrumental · 4:00*

Style prompt:
```
Solo viola in G Aeolian, warm and low, a soft French horn answering each phrase from a distance, harp and held strings beneath — long shadows across the road, the day's work done, the walk home. 58 BPM, slow 4/4. Warm evening reverb, golden and soft. Settling dynamics: the cue grows quieter and lower as it goes, never louder, no climax. Dusk exploration in the tradition of Soule's Oblivion "Dusk at the Market" and "Wings of Kynareth". Full-length cue, the theme handed down from viola to horn to strings, no early fade, long outro. Instrumental. No percussion, no choir, no solo vocals, no flute, no glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - harp and held strings in G Aeolian, evening light]
[Instrumental A - solo viola states a slow warm theme]
[Instrumental B - a distant soft French horn answers the phrase, strings hold]
[Instrumental Development - viola and horn alternate, the harmony darkening slightly]
[Instrumental Hush - strings and harp only, lower than before, the sun gone]
[Instrumental Outro - viola alone restates the first phrase, very long fade]
```

---

**K2 — Last Light _ On the Water**
*Key B Mixolydian · 54 BPM · Sea / coast · solo cello + hammered dulcimer · instrumental · 4:00*

Style prompt:
```
Solo cello in B Mixolydian over a soft hammered dulcimer ostinato and warm held strings, a low whistle doubling the cello once or twice in a fixed register — the last light on the water, a lake going still, the shore in shadow. 54 BPM, gentle 6/8. Wide soft coastal reverb. Flat dynamics, warm and settling, no build. Dusk exploration after Soule's Oblivion lakeside cues with Skyrim's cello warmth. Full-length cue, the melody restated with the dulcimer thinning out, no early fade, long outro. Instrumental. No percussion, no brass, no choir, no solo vocals, no flute glissando, no pitch sweeps, no portamento, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - hammered dulcimer alone, a soft B Mixolydian ostinato in 6/8]
[Instrumental A - solo cello states a warm rolling theme over the ostinato]
[Instrumental B - held strings join underneath, a low whistle doubles the cello briefly]
[Instrumental Development - cello varies the theme, dulcimer thinning]
[Instrumental Hush - strings and a few dulcimer notes, the water still]
[Instrumental Outro - cello holds one low note over the last dulcimer strike, long fade]
```

## 5. Explore, Rain (two cues)

Rain cues replace the Day or Night cue while it rains. They are Explore in every rule; the
difference is texture: repeated soft figures like water, greyer harmony, a distant timpani
roll at most for thunder.

---

**R1 — Rain on the Road**
*Key C# Dorian · 50 BPM · Human · piano + harp · instrumental · 4:00*

Style prompt:
```
Piano in C# Dorian playing a soft repeating figure like steady rain, harp doubling it, low strings holding a grey chord beneath, a solo viola entering late with a slow melody — walking on in the rain, hood up, the road turned to mud. 50 BPM, steady but soft. Close piano, a wet grey hall reverb. Flat dynamics, patient and unhurried, no build, no thunder. Rain exploration after Soule's Oblivion at its most melancholic and Skyrim's rain-soaked piano moments. Full-length cue, the figure constant while the melody varies above it, no early fade, long outro. Instrumental. No percussion, no brass, no choir, no solo vocals, no flute, no glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - piano alone, a soft repeating C# Dorian figure like rain]
[Instrumental A - harp doubles the figure, low strings hold a grey chord]
[Instrumental B - solo viola enters with a slow melody over the figure]
[Instrumental Development - the harmony shifts under the constant figure, viola varies]
[Instrumental Hush - piano and strings only, the rain steady]
[Instrumental Outro - the figure slows and thins to single drops, long fade]
```

---

**R2 — The Storm Passing _ Grey Hills**
*Key E♭ Dorian · 48 BPM · Dead-adjacent · cor anglais · instrumental · 4:00*

Style prompt:
```
Cor anglais in E♭ Dorian, a slow mournful theme over low strings and a soft sustained drone, one distant timpani roll like far thunder every so often, harp harmonics like drops after the rain — grey hills, the storm moving off, wet stone. 48 BPM, slow 4/4 with long rests. Vast damp outdoor reverb. Flat dynamics throughout, the thunder always distant, no build, no climax. Weather exploration in the vein of Soule's bleaker Oblivion and Morrowind wilderness cues. Full-length cue, the theme restated as the storm recedes, no early fade, long outro. Instrumental. No brass, no choir, no solo vocals, no flute, no glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - low drone on E♭, one distant timpani roll, harp harmonics like drops]
[Instrumental A - cor anglais states a slow mournful theme, low strings beneath]
[Instrumental B - the theme again with strings a little fuller, another far roll]
[Instrumental Development - cor anglais varies the theme downward, thunder farther off]
[Instrumental Hush - drone and harp harmonics only, the rain stopping]
[Instrumental Outro - cor anglais holds one note, drone fades, very long tail]
```

## 6. Cave (two cues)

The Cave pool needs one true near-ambient cue and one with a little more shape. Both stay
below the threshold of alarm: the underground is old and patient, not hostile, and the
existing `Stealth_Tension` already covers unease. Keep them clear of cue 07 (contrabassoon,
C Phrygian, anvil), which stays in the pool.

---

**C1 — Drip and Dark _ The Deep Hollows**
*No tonal centre (A drone) · 40 BPM / free · Dwarven-adjacent · bowed crotales + contrabass · instrumental · 4:30*

Style prompt:
```
Bowed crotales and a very low contrabass drone on A, no clear key, single plucked harp harmonics placed at random like water dripping into a pool far below, a bass clarinet note once a minute — the deep hollows under the mountain, dark, still, immense. 40 BPM, no meter, no pulse. Enormous cavern reverb with a very long slap. Near-ambient: flat dynamics, almost no melody, nothing threatening, the cue should vanish into the space. Underground exploration in the manner of Soule's Morrowind cave ambience and Skyrim's quietest dungeon beds. Full-length cue, drifting not developing, no early fade, very long outro. Instrumental. No percussion, no brass, no choir, no solo vocals, no flute, no glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no EDM, no build.
```
Structure prompt:
```
[Instrumental Intro - contrabass drone on A fades in, one plucked harp harmonic]
[Instrumental A - bowed crotales shimmer above, harp drops at random intervals]
[Instrumental B - a single bass clarinet note far below, then silence]
[Instrumental Drift - drone and crotales only, the drips slower]
[Instrumental C - the clarinet note once more, lower]
[Instrumental Outro - crotales fade, one last drip, the drone outlasts everything, very long tail]
```

---

**C2 — The Sleeping Stone _ Old Passages**
*Key A♭ Phrygian · 48 BPM · Dwarven · bass clarinet + cello · instrumental · 4:00*

Style prompt:
```
Bass clarinet and solo cello in slow A♭ Phrygian, a heavy stepwise theme passed between them over a low string drone, a soft tam-tam swell once per section like stone settling — old dwarven passages, worked long ago and empty now. 48 BPM, slow heavy 4/4. Huge stone reverb, long dark tail. Flat dynamics, patient and low, no climax, no anvil, nothing bright. Underground exploration after Soule's subterranean Morrowind and Skyrim Dwemer ruins at their stillest. Full-length cue, the theme descending by step each restatement, no early fade, long outro. Instrumental. No bright brass, no choir, no solo vocals, no flute, no glissando, no pitch sweeps, no synth, no electric guitar, no drum kit, no taiko, no EDM, no crescendo.
```
Structure prompt:
```
[Instrumental Intro - low string drone on A♭, one soft tam-tam swell]
[Instrumental A - bass clarinet states a slow stepwise Phrygian theme]
[Instrumental B - solo cello takes the theme a step lower, clarinet holds beneath]
[Instrumental Development - the two alternate, the drone deepening, tam-tam once]
[Instrumental Hush - drone alone, the passages empty]
[Instrumental Outro - bass clarinet restates the first phrase and stops, long fade]
```

## 7. Already authored in the catalogue, reproduced verbatim

These eight fill the Camp/Night, Water, Combat and Menu gaps and are copied unchanged from
`docs/music-prompts.md` section 7 so this file is complete on its own. Generate them as
written; the catalogue's notes about which existing files they supersede still apply
(32 supersedes `Sea _ Sailing`).

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

