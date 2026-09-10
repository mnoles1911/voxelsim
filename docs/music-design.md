# Music design: pools, silence, time of day, and what belongs in each

Owner-accepted 2026-09-08 ("assuming i accept your music architecture recommendations.
write this to a game design file"); reframed 2026-09-09 when the owner restarted the OST
from first principles for what the game now is: an open-ended survival and building game
with long solo sessions and quiet chores, not a story RPG. Grounded in
`docs/music-exploration-reference-2026-09-08.md` (how Oblivion and Skyrim actually
schedule music, with sources). The content, cue by cue with the Suno prompts, is
`docs/music-prompts.md`. This file is the *system*; that file is the *catalogue*.

## 1. The one-line specification

The score is background by design: a shuffled, endlessly cycling pool of long, slow,
instrumental cues with flat dynamics, filtered by time of day and by where the player
is, separated by deliberate silence, and interrupted only briefly and rarely.

## 2. Pools, in priority order

Higher priority ducks lower. A pool change is a crossfade of about 3 s, never a hard cut,
except Stingers, which play over whatever is running and hand back.

| # | pool | when it owns the music | character | selection |
|---|---|---|---|---|
| 1 | **Stingers** | an event fires: death, discovery | seconds, one-shot | fired by the event, never cycled; backlog |
| 2 | **Danger** | a threat is active (rare) | a raised pulse, 72-100 BPM, low strings and a frame drum, no brass, no anthem | shuffled; a 10-20 s cooldown tail before the previous pool resumes |
| 3 | **Cave** | underground (the veil signal) | near-ambient, stone, no melody to follow | shuffled, same gap rule as Explore |
| 3 | **Hearth** | at the place the player built: building, farming, crafting, cooking, evenings | intimate, close-miked, domestic, unhurried | shuffled, same gap rule (the signal, "near your own structures", is not built yet) |
| 3 | **Water** | aboard a vessel | rolling, open fifths, slow | shuffled, same gap rule |
| 4 | **Explore** | everything else | 5-7 min, 40-72 BPM, flat dynamics, **filtered by time of day and weather** | shuffled, no repeat until exhausted |
| 5 | **Menu** | title and loading screens | the main title plus two quiet preludes | fixed small set, shuffled |
| – | **Archive** | never | the story-era cues (towns, war, cinematics) | not scanned |

Ties at priority 3 resolve by specificity: Water beats Cave beats Hearth. Hearth
replaces the old Town pool in the same slot of the code (`EVoxelMusicPool`) and the
`Town` folder; Danger replaces Combat the same way. The folders and enum names are
renamed together with the signals that select them; until then the design names are the
ones in this document and the folder names are the old ones.

## 3. Silence is authored, not accidental

Both reference games do this and it is the single biggest difference from a playlist.

- **Explore, Cave, Hearth, Water:** a randomised gap of **60-150 s** between cues, drawn
  fresh each time. Never two cues touching. Longer than the first draft's 45-120 because
  the cues themselves are now 5-7 minutes.
- **Menu:** a gap of 20-40 s.
- **Danger:** no gap while the threat lasts; the cooldown tail is the gap.
- A cue's own authored tail counts toward the gap.
- The gap resets on a pool change.

With ten Day cues of 6 minutes plus gaps, one daytime cycle runs well over an hour before
a repeat.

## 4. Time of day and weather filter Explore

The sky clock exists. Four slots, boundaries in game hours, read from one table in
`VoxelMusicPools.cpp`:

| slot | hours | character |
|---|---|---|
| Dawn | 05:00-07:00 | thin, bright, rising gently; solo oboe or whistle over held strings |
| Day | 07:00-18:00 | the widest pool; 50-72 BPM, walking pace at most |
| Dusk | 18:00-20:00 | warm, settling, lower; viola, soft horn, harp |
| Night | 20:00-05:00 | the sparsest and slowest; piano, celesta, music box, harmonics |

Rain leads the chain while it rains (the Rain folder), so a wet day plays rain cues when
it has them and day cues when it does not. A slot with fewer than four cues borrows: Dawn
borrows Day; Dusk borrows Day then Night; Night borrows Dusk, then Day as a floor. The
walk stops as soon as it has four cues, so Night reaches Day only while Night and Dusk
together hold fewer than four.

## 5. What belongs in Explore (the rule for every future cue)

A cue is Explore only if it satisfies **all** of these:

1. Instrumental. No vocals, no choir.
2. Slow: 40-72 BPM, and most cues with no pulse a listener could tap.
3. Sparse: the one instrument family in `docs/music-prompts.md` section 1; a soft frame
   drum at most; no drum hits, no battery, no brass.
4. Flat dynamics: no crescendo that would read as an event, no fanfare. Development is
   by variation, register and thinning.
5. Five to seven minutes, with a long quiet tail rather than a hard stop.
6. A key of its own within its pool; D minor belongs to the Main Theme.

Hearth, Cave and Water cues follow the same six rules with their own palette. Danger is
the one pool allowed a pulse, and even it is a raised pulse rather than a battle.

## 6. The current library, sorted

Thirty playable WAVs and one MP4 on disk. Per `docs/music-prompts.md` section 3:

| pool | kept | note |
|---|---|---|
| Explore, Day | 04 Open Road, 05 Greatwood, 06 Spine, 08 Ashfields, 09 Western Coast, 11 Sorrowmarsh, 12 Weeping Wood, Forest_Exploration | 8; audition World Map _ Travel and Discovery_Wonder against section 5 |
| Explore, Night | Camp_Rest (borrowed) | effectively zero |
| Explore, Dawn / Dusk / Rain | — | zero |
| Hearth | — | zero; the new heart of the score |
| Cave | 07 Underway, Cave_Dungeon | 2 |
| Water | Sea _ Sailing | 1 |
| Danger | 38 Enemies Gathering Strength, Stealth_Tension | 2 |
| Menu | 02 Main Title | 1 |
| Stingers | Defeat _ Game Over | 1 |
| Archive | the seven Town cues, Boss Battle, Emotional_Loss, 49 The Vigil, 10 Copper Isles, Main Theme.wav, the Lirien-Thal MP4 | story-era; not scanned |

## 7. Targets and the gap

| pool | target | kept | to generate |
|---|---|---|---|
| Explore, Day | 10 | 8 | 2 |
| Explore, Dawn | 3 | 0 | 3 |
| Explore, Dusk | 3 | 0 | 3 |
| Explore, Night | 6 | 0 | 6 |
| Explore, Rain | 3 | 0 | 3 |
| Hearth | 6 | 0 | 6 |
| Cave | 4 | 2 | 2 |
| Water | 3 | 1 | 2 |
| Danger | 3 | 2 | 2 |
| Menu | 3 | 1 | 2 |

**31 new cues**, all with prompts in `docs/music-prompts.md` section 6. Generate the six
Hearth cues and the six Night cues first: together they cover the hours the game is now
about, and today both pools are empty.

## 8. Mechanics (what the player does)

Implemented 2026-09-08 (`VoxelMusicPools.h/.cpp`, `VoxelUIMusic.h/.cpp`, tests
`VoxelEarth.Music.*`):

- **Subfolders, not filename prefixes.** The filename feeds the now-playing label.
  `Content/Audio/Music/Explore/{Day,Night,Dawn,Dusk,Rain}`, `Cave`, `Town` (Hearth),
  `Water`, `Combat` (Danger), `Menu`, `Stingers`, `Cinematic`, plus `Archive` (unscanned,
  to be added with the retirement in section 6). One shuffled playlist per folder,
  scanned recursively; `pools built -- N cue(s): ...` at boot names every count.
- **Time of day** from the sky subsystem, through the boundary table; **rain** from the
  weather subsystem once it publishes precipitation (today it publishes wind only, so
  the Rain predicate is a named stub).
- **Gaps** per section 3, drawn per cue. The 45-120 s of the first draft is in code
  today; move to 60-150 s with the first long cues.
- **Priority and ducking** per section 2 with a 3 s crossfade on one decoded buffer.
- **Signals**: Water (aboard `AVoxelBoat`) and Cave (the clipmap veil latch) are real;
  Hearth ("near your own structures"), Danger (no threat signal exists), Rain and Stingers
  are named stub predicates in `VoxelUIMusic.cpp`, one line each when the systems exist.
- **Memory across sessions:** the last five cues per pool are remembered, so a relaunch
  does not reopen on the same one.
- **Transport** (prev / play-pause / next, hotkeys `,` `.` `/`) acts on the current pool.
- **Unattended runs** stay silent in game; menu music on legs is unchanged.
- **Log lines:** `VoxelUIMusic: pool=<pool> slot=<slot> cue='<name>' gap=<s>` per
  selection and `pool change <old> -> <new> (crossfade 3.0s)`. Proven on the title screen
  2026-09-09 (`pool=Menu ... gap=24.2`); the in-world pools have not been heard live.

## 9. Backlog

Stingers and any cue under thirty seconds; the Hearth signal; a precipitation signal;
the Archive folder and the two folder renames; per-biome weighting of Explore (a marsh
cue near marshes) once the palette column can be read from a manifest; a Settings row for
the gap length and a "no silence" arm.
