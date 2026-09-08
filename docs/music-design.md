# Music design: pools, silence, time of day, and what belongs in each

Owner-accepted 2026-09-08 ("assuming i accept your music architecture recommendations.
write this to a game design file"). Grounded in `docs/music-exploration-reference-2026-09-08.md`
(how Oblivion and Skyrim actually schedule music, with sources) and in
`docs/music-prompts.md` (the 59-cue inventory, keys, palettes, leitmotif rules that every
new cue must follow). This file is the *system*; that file is the *catalogue*.

## 1. The one-line specification

Exploration music is a shuffled, endlessly cycling pool of long instrumental cues with no
vocals, no fanfare and flat dynamics, filtered by time of day, separated by deliberate
authored silence, and interrupted only by shorter, higher-priority pools that duck it.

Everything below is that sentence made concrete.

## 2. Pools, in priority order

Higher priority ducks lower. A pool change is a crossfade of 2-4 s, never a hard cut,
except Stingers, which play over whatever is running and hand back.

| # | pool | when it owns the music | character | selection |
|---|---|---|---|---|
| 1 | **Stingers** | an event fires: defeat, discovery, boss reveal | seconds, one-shot | fired by the event, never cycled |
| 2 | **Combat** | a threat is active | shorter, higher energy, clear pulse | shuffled; ends with a 10-20 s cooldown tail before Explore resumes |
| 3 | **Cave** | the underground veil is engaged (we already have this signal) | sparse, low, near-ambient, long | shuffled, same gap rule as Explore |
| 3 | **Town** | inside a settlement's bounds (when settlements exist) | warmer, folk instruments, medium tempo | shuffled, same gap rule |
| 3 | **Water** | the player is aboard a vessel | open, rolling, low whistle / accordion / cello | shuffled, same gap rule |
| 4 | **Explore** | everything else: walking, building, boating on foot | 3-6 min, instrumental, flat dynamics | shuffled, no repeat until exhausted, **filtered by time of day** |
| 5 | **Menu** | title and loading screens | the main title plus two or three calm pieces | fixed small set, shuffled |
| – | **Cinematic** | scripted moments only | narrative cues | never in any cycling pool |

Ties at priority 3 resolve by specificity: Water beats Cave beats Town (a boat in a
flooded cavern is still a boat).

## 3. Silence is authored, not accidental

Both reference games do this and it is the single biggest difference from what we did
before tonight (back-to-back tracks). Rules:

- **Explore, Cave, Town, Water:** a randomised gap of **45-120 s** between cues, drawn
  fresh each time. Never two cues touching.
- **Menu:** a gap of 20-40 s.
- **Combat:** no gap while the threat lasts; the cooldown tail is the gap.
- A cue's own authored tail (the long fade every prompt asks for) counts toward the gap.
- The gap resets on a pool change, so entering a town does not wait out a 90 s silence.

With 14 Explore cues at ~3:30 plus gaps, one cycle runs over an hour before repetition,
which is exactly the Oblivion illusion with the same pool size.

## 4. Time of day filters Explore

The sky clock exists (the journal already reads it). Four slots, boundaries in game hours:

| slot | hours | pool character |
|---|---|---|
| Dawn | 05:00-07:00 | thin, bright, rising; solo oboe / flute over held strings |
| Day | 07:00-18:00 | the widest pool; walking tempo 60-92 BPM |
| Dusk | 18:00-20:00 | warm, settling, lower; viola, horn, harp |
| Night | 20:00-05:00 | the sparsest and slowest; piano, celesta, music box, harmonics |

A cue belongs to exactly one slot. A slot with fewer than four cues borrows from its
neighbours in this order: Dawn borrows Day, Dusk borrows Day then Night, Night borrows
Dusk. Day never borrows. The boundaries are a first draft; they are read from one table
so they can move.

## 5. What belongs in Explore (the rule for every future cue)

A cue is Explore only if it satisfies **all** of these:

1. Instrumental. No vocals, no choir (the six brief-choir exceptions in the catalogue are
   Identity/War/Cinematic cues, never Explore).
2. Slow to walking tempo, 40-92 BPM. The one 104 BPM cue in the pool today is the outlier.
3. Sparse texture: solo strings, woodwinds, harp, piano, hammered dulcimer, soft pads.
   Frame drum at most; no drum hits, no battery.
4. Flat dynamics: no crescendo that would read as an event, no fanfare, no brass stabs.
5. Three to six minutes, with a long quiet tail rather than a hard stop.
6. Follows the catalogue's key-variety rule: D minor belongs to the Main Theme.

Consistency of instrument family across the pool is what gives the world its identity;
variety comes from the gaps and the time-of-day filter, not from stylistic range.

Anything that fails one rule moves to another pool or out.

## 6. The current library, sorted into pools

Thirty playable WAVs plus one unplayable MP4 (from the research doc, which used the
catalogue's Category column as authority). Pool targets are in section 7.

| pool | cues on disk | note |
|---|---|---|
| Explore, Day | 04 Open Road, 05 Greatwood, 06 Spine, 08 Ashfields, 09 Western Coast, 10 Copper Isles, 11 Sorrowmarsh, 12 Weeping Wood, Forest_Exploration, Discovery_Wonder, World Map _ Travel | 11; Discovery_Wonder is ambiguous and needs an ear check; none are night or dawn cues |
| Explore, Night | Camp_Rest (borrowed) | effectively **zero** |
| Explore, Dawn / Dusk | — | **zero** |
| Water | Sea _ Sailing | 1 |
| Cave | 07 Underway, Cave_Dungeon, Stealth_Tension | 3 (Stealth_Tension is tension, not ambient; usable as the cave's uneasy cue) |
| Town | 13 Aldenholt, 16 Solgrade, 20 Brightwatch, Town_Village, Castle_Court Interior, Taverns_Feast, 27 Deep Cups | 7 |
| Combat | Boss Battle, 38 Enemies Gathering Strength | 2 |
| Menu | 02 Main Title | 1 (`Main Theme.wav` is the superseded duplicate; remove) |
| Cinematic | Emotional_Loss, 49 The Vigil (lead vocal) | 2; never cycled |
| Stingers | Defeat _ Game Over (0:17) | 1 |
| Unplayable | 17 Lirien-Thal (.mp4) | re-encode to 16-bit PCM WAV |

**Immediate curation:** remove `Main Theme.wav`; take 49, Emotional_Loss, Boss Battle,
38, Stealth_Tension and Defeat out of anything Explore; re-encode 17; update
`Content/Audio/Music/MUSIC_CREDITS.md`, which still says the folder is empty.

## 7. Targets, and the gaps

| pool | target | on disk | gap |
|---|---|---|---|
| Explore, Day | 10 | 11 | none (audit Discovery_Wonder) |
| Explore, Night | 6 | 0 | **6** |
| Explore, Dawn | 2 | 0 | **2** |
| Explore, Dusk | 2 | 0 | **2** |
| Explore, weather (rain) | 2 | 0 | **2** (optional layer, played in place of Day/Night when raining) |
| Cave | 4 | 3 | **1-2** |
| Water | 3 | 1 | **2** (catalogue 31, 32) |
| Town | 6 | 7 | none |
| Combat | 5 | 2 | **3** (catalogue 39, 40, 46) |
| Menu | 3 | 1 | **2** (catalogue 01, 03) |
| Camp / rest | 2 | 1 | **1** (catalogue 36, which also serves Night) |

Total to generate: about **20 cues**. The Night/Dawn/Dusk/rain/cave cues do not exist in
the catalogue and get new prompts; the rest already have prompts in
`docs/music-prompts.md` section 7 and are generated as written. Both lists, with the
Suno prompts, are in `docs/music-generation-prompts-2026-09-08.md`.

Stingers and any cue under about thirty seconds are **backlog** by owner direction.

## 8. Mechanics (what the player does)

- **Subfolders, not filename prefixes.** The filename feeds the now-playing label.
  `Content/Audio/Music/Explore/{Day,Night,Dawn,Dusk,Rain}`, `Cave`, `Town`, `Water`,
  `Combat`, `Menu`, `Stingers`, `Cinematic`. The player builds one shuffled playlist per
  folder; the glob becomes recursive-by-folder rather than a flat `*.wav`.
- **Time of day** from the sky subsystem's epoch, through the one boundary table above.
- **Gaps** as in section 3, drawn per cue.
- **Priority and ducking** as in section 2, with a 2-4 s crossfade.
- **Memory across sessions:** remember the last five cues played per pool so a relaunch
  does not open on the same one.
- **Transport** (prev / play-pause / next, hotkeys `,` `.` `/`) acts on the current pool.
- **Unattended runs** stay silent in game (a measurement leg must not read 90 MB of music
  in its first seconds); menu music on legs is unchanged.
- A `music.json` manifest replaces folders only if per-cue metadata beyond pool and slot
  becomes necessary (leitmotif tags, per-biome weighting). Not now.

## 9. Backlog (not this pass)

Stingers and sub-30 s cues; per-biome weighting of the Explore pool (marsh cues near
marshes); a settlement bounds signal for Town; Interior/Sacred (catalogue 21-24) once
interiors exist; the Cinematic and Ending sets (49-59) once the story beats exist.
