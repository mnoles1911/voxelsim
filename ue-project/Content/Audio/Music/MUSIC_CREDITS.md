# Music: what is here, and which pool it is in

**29 cues and one unplayable file, sorted into pool folders on 2026-09-08.**
Before that date this directory was one flat folder and the player treated the
whole library as a single shuffled list. It no longer does: the folder a cue
sits in *is* its pool, and `docs/music-design.md` is the design the folders
implement. Section 6 of that document is the table this listing was sorted by.

Nothing below is committed. The audio is gitignored (see the recursive
`Music/**/*.wav` rules in `.gitignore`); each pool folder carries a `.gitkeep`
so the **layout** is in the repository while the bytes are not.

## The layout

```
Content/Audio/Music/
    Explore/Day     Explore/Night     Explore/Dawn     Explore/Dusk     Explore/Rain
    Cave     Town     Water     Combat     Menu     Stingers     Cinematic
```

`FVoxelUIMusic` builds **one shuffled playlist per folder**, scanning each one
recursively, and picks which playlist owns the music from what the player is
doing (`VoxelMusicPools.h`). Adding a cue is dropping a 16-bit PCM WAV into the
right folder -- no code change and no registration, exactly like dropping a
`.jpg` in `Content/UI/Backgrounds`. Grouping further inside a pool folder
(`Town/Taverns/…`) works too; the scan is recursive.

**Dawn, Dusk and Rain are empty**, and that is a supported state, not a fault:
`Explore/Dawn` borrows `Explore/Day`, and `Explore/Dusk` borrows Day then
Night, per section 4's borrow rule. `docs/music-prompts.md` (rewritten 2026-09-09 for the
survival and building game) lists the 31 cues that still
have to be generated to fill them.

## Tracks, by pool

Durations are computed from the PCM length; every file is 48 kHz stereo 16-bit
PCM, which is the only format the runtime decoder accepts.

### Explore / Day -- 11 cues

| File | Length | MB |
|---|---|---|
| 04 — Open Road _ The Central Plains.wav | 2:37 | 28.9 |
| 05 — The Greatwood _ Under Old Leaves.wav | 3:24 | 37.5 |
| 06 — The Spine _ Stone and Sky.wav | 3:47 | 41.7 |
| 08 — The Ashfields _ Grey Soil.wav | 3:23 | 37.3 |
| 09 — The Western Coast _ Caer Drowned.wav | 3:24 | 37.4 |
| 10 — The Copper Isles _ Salt and Sun.wav | 2:21 | 25.9 |
| 11 — The Sorrowmarsh _ The Mud Remembers.wav | 7:59 | 87.8 |
| 12 — The Weeping Wood _ Watched.wav | 3:23 | 37.2 |
| Discovery_Wonder.wav | 3:27 | 38.0 |
| Forest_Exploration.wav | 4:13 | 46.4 |
| World Map _ Travel (preferred).wav | 2:53 | 31.8 |

`Discovery_Wonder` is the one entry the research pass flagged as ambiguous and
it still **needs an ear check** against section 5's six rules -- a cue with a
crescendo that reads as an event does not belong in Explore. `11 — The
Sorrowmarsh` is the largest single file in the project at 87.8 MB and, at 7:59,
is also well outside section 5's three-to-six-minute rule.

### Explore / Night -- 1 cue

| File | Length | MB |
|---|---|---|
| Camp_Rest.wav | 3:33 | 39.0 |

Borrowed into Night by section 6. Night's own borrow chain is Dusk, which is
empty, so at night this one cue plays on repeat with 45-120 s of silence between
plays until the six Night cues in section 7 exist. That is the single largest
content gap in the library.

### Explore / Dawn, Explore / Dusk, Explore / Rain -- 0 cues each

Empty. Dawn borrows Day; Dusk borrows Day then Night; Rain leads the chain when
it rains and falls straight through to the clock, which it always does today
because nothing in this project reports precipitation.

### Cave -- 3 cues

| File | Length | MB |
|---|---|---|
| 07 — The Underway _ Beneath the Mountain.wav | 3:12 | 35.3 |
| Cave_Dungeon.wav | 3:34 | 39.2 |
| Stealth_Tension.wav | 2:39 | 29.1 |

`Stealth_Tension` is tension rather than ambient and is used here as the cave's
uneasy cue, which is section 6's own note on it.

### Town -- 7 cues

| File | Length | MB |
|---|---|---|
| 13 — Aldenholt _ Market and Bel.wav | 1:47 | 19.7 |
| 16 — Solgrade _ The Unwalled City.wav | 3:09 | 34.6 |
| 20 — Brightwatch _ The Frontier Garrison.wav | 3:38 | 40.0 |
| 27 — The Deep Cups _ A Dwarven Dance.wav | 2:24 | 26.4 |
| Castle_Court Interior.wav | 2:54 | 32.0 |
| Taverns_Feast.wav | 1:29 | 16.4 |
| Town_Village.wav | 1:56 | 21.3 |

**Unreachable today.** The Town pool needs a settlement-bounds signal and there
are no settlements; the predicate is stubbed false
(`VoxelUIMusicDetail::IsInsideSettlement`) and `docs/backlog.md` §15c carries
the deferral. The cues are here so that wiring the signal is one line.

### Water -- 1 cue

| File | Length | MB |
|---|---|---|
| Sea _ Sailing.wav | 2:48 | 30.9 |

Selected while the player's pawn is an `AVoxelBoat`. This one is live: board a
boat and the music crossfades to it.

### Combat -- 2 cues

| File | Length | MB |
|---|---|---|
| 38 — Enemies Gathering Strength _ The Muster of the Hand.wav | 2:58 | 32.7 |
| Boss Battle.wav | 3:09 | 34.7 |

**Unreachable today**, for the same reason and in the same shape as Town: this
project has no combat, so `VoxelUIMusicDetail::IsCombatThreatActive` returns
false. The priority, the crossfade and the 10-20 s cooldown tail are all
implemented and tested around it.

### Menu -- 1 cue

| File | Length | MB |
|---|---|---|
| 02 — Main Title _ Mira-Thal V3.wav | 4:34 | 50.3 |

The title screen and the loading curtain both draw from here. `Main Theme.wav`
(57.9 MB) was the superseded duplicate of this cue and was **deleted** on
2026-09-08 per section 6.

### Stingers -- 1 cue

| File | Length | MB |
|---|---|---|
| Defeat _ Game Over.wav | 0:17 | 3.2 |

Fired by an event, never cycled. Nothing in this project fires one yet;
stingers and sub-30 s cues are owner-deferred backlog (`docs/backlog.md` §15c).

### Cinematic -- 2 cues

| File | Length | MB |
|---|---|---|
| 49 — The Vigil _ The Night Before V2.wav | 5:12 | 57.3 |
| Emotional_Loss.wav | 2:39 | 29.3 |

Scripted moments only. **Never in any cycling pool** -- the only way to hear
one is `-VoxelMusicPool=Cinematic`, which is a deliberate pin.

## The one file that will not play

`17 — Lirien-Thal _ The Silverwood.mp4` (6.9 MB) sits in the root of this
directory, outside every pool folder, and is therefore never scanned. It is an
MP4 and the runtime decoder takes 16-bit PCM WAV only.

**It was NOT re-encoded**, because `ffmpeg` is not on this machine's PATH and is
not installed anywhere the search found. When it is available:

```
ffmpeg -i "17 — Lirien-Thal _ The Silverwood.mp4" -vn -acodec pcm_s16le -ar 48000 -ac 2 \
       "Explore/Day/17 — Lirien-Thal _ The Silverwood.wav"
```

Then delete the `.mp4`. Lirien-Thal is a location cue, so `Explore/Day` is its
pool unless an ear check says otherwise.

## Why here and not under Content/UI/

`Content/UI/` is scoped to the front end's art and typeface, and
`FVoxelUIAssetLibrary` hardcodes `UI/Backgrounds` as a subdir path. The menu was
the first consumer of music and is no longer the only one -- in-world
exploration, cave and water music under a `UI/` folder would have been wrong
within a year, which is now demonstrated rather than predicted. This directory
sits under `FPaths::ProjectContentDir()` either way, which is what the Slate
front end resolves against.

## Rules for anything committed here

1. **COMPRESSED SOURCE, NOT WAV.** This repo has no LFS. Two baked terrain tiles
   at 192 MB each blocked 112 commits once already (see the 100 MB wall in
   `.gitignore`), and the menu art was re-encoded from 30.6 MB to 2 MB for the
   same reason. Commit OGG (or MP3); keep the WAV masters outside the repo and
   name them below. Nothing has been committed under this rule yet -- the whole
   library above is local-only.
2. **RECORD THE SOURCE sha256, not just the output's**, so a re-encode is
   reproducible and a swapped source shows up in the diff rather than only in
   the ears.
3. **The `.uasset` siblings are ignored, deliberately.** An interactive editor
   session auto-imports loose media it finds under `Content/`, so `.uasset`
   files will appear next to these. Nothing references them; the ignore rule is
   recursive as of 2026-09-08 so it still covers them inside the pool folders.

## The prompts that make these files

`docs/music-prompts-rpg-era-2026-05-16.md` -- the story-era Suno portfolio (59 cues,
archived 2026-09-09) that most of the cues on disk were generated from.
`docs/music-prompts.md` section 5 -- the 31 cues the 2026-09-09 plan says are
still missing, with their prompts.

## Committed re-encodes

_None yet._

| File | Source | Output | sha256 (source) |
|---|---|---|---|
| -- | -- | -- | -- |

## Rights: CLEARED FOR COMMERCIAL RELEASE

The owner confirmed on 2026-08-25 that every asset in this directory is cleared
for use in the commercial game. That is the project's position and it does not
need re-opening per asset or per batch.

Recorded as what it is: the determination of the account holder who sourced the
material. It is not a third-party legal review and does not claim to be. If the
basis ever has to be re-established, the thing to re-check is the terms of the
generating account at the time each file was made -- not this paragraph.

Applies to the tracks the designer places in this folder, however they were
produced (the portfolio in `docs/music-prompts.md` was written for Suno).
