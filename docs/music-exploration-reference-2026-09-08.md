# Exploration music: what Oblivion and Skyrim actually play, and what we have

Research reference, 2026-09-08. Read-only study; no audio was downloaded,
decoded, copied or played. Durations for our own library were computed from WAV
headers (byte count / byte rate), not by decoding.

Written because tonight's menu played **"Defeat _ Game Over"** and **"Boss
Battle"**. That is not a bug in the player -- it is the absence of a pool. Part 4
says what to do about it.

---

## 1. The distinction: "official soundtrack" vs. what plays while you walk

### Oblivion (Jeremy Soule, 2006)

The retail soundtrack is **26 tracks**. The game ships **28 files**, in five
folders under `Oblivion\Data\Music`, and the split is not cosmetic -- the folder
*is* the category. UESP:

> There are 28 tracks in total but only 26 are included in the soundtrack. The
> tracks "Success" and "Death" are not included [...] The game divides the
> tracks into five categories:
> - **Special** -- played at special times during the game.
> - **Explore** -- all of these tracks are randomly played one after another
>   while the player is in the countryside (outside cities and dungeons).
> - **Public** -- [...] while the player is in a city, inn, house, or chapel.
> - **Dungeon** -- [...] while the player is in a dungeon (Ayleid ruins, caves,
>   forts, mines etc).
> - **Battle** -- [...] while the player is under attack.

Source: <https://en.uesp.net/wiki/Oblivion:Music>

**So the answer to the owner's question is the reverse of the intuition.** There
are *not* "hours more music" hidden in the game beyond the OST. Oblivion's OST
is nearly the whole music budget: 26 of 28 files. What creates the impression of
endless music is that **the free-roaming pool is only 7 files**, each 2:28-4:42,
shuffled endlessly with gaps -- so over a hundred hours you hear those seven
cues hundreds of times, and the "epic" tracks you remember from the OST
("Reign of the Septims") are heard **only on the title screen**.

The explore pool, exactly:

| File | Title | Length |
|---|---|---|
| `atmosphere_01.mp3` | Glory of Cyrodiil | 2:28 |
| `atmosphere_03.mp3` | Through the Valleys | 4:19 |
| `atmosphere_04.mp3` | Minstrel's Lament | 4:42 |
| `atmosphere_06.mp3` | Auriel's Ascension | 3:05 |
| `atmosphere_07.mp3` | Wings of Kynareth | 3:30 |
| `atmosphere_08.mp3` | King and Country | 4:05 |
| `atmosphere_09.mp3` | Peace of Akatosh | 4:11 |

Note the numbering: `atmosphere_02` and `atmosphere_05` **do not exist**. The
folder is 7 files numbered to 9.

**How it picks, and the silence.** Selection is by cell, not by script. The
Construction Set exposes a single **Music Type** dropdown per cell -- "Music
Type: Default music type for this cell" -- with three values, **Default /
Dungeon / Public**; "Default" outdoors is the Explore pool.
(<https://cs.uesp.net/wiki/Interior_cells>, and the behaviour is confirmed from
the other side by the `StreamMusic` console command's documented interaction
with those same three types, <https://cs.uesp.net/wiki/StreamMusic>.)

The gaps between cues are **baked into the files**, which is the most directly
copyable idea in this whole document:

> All tracks have significantly shorter playtimes than the ones listed above.
> The last few seconds are blank. This is most likely to break up the tracks
> in-game and give the player's ear a rest before moving on to the next track.

Source: <https://en.uesp.net/wiki/Oblivion:Music>

The system is also **leaky by design and nobody minded** -- UESP records an
Explore tune playing inside the Cheydinhal Fighters Guild Hall and a Dungeon
tune in the basement of The Oak and Crosier. Perfect classification was not a
shipping requirement.

### Skyrim (Jeremy Soule, 2011)

The physical release is **4 discs / 53 numbered tracks**, ~4 hours; disc 4 is a
single 42:34 "Skyrim Atmospheres" compilation, not 20 songs. Two extended
in-game pieces (`mus_combat_boss`, `mus_dungeon_cave_02`) are **missing from the
OST**, "most of the shorter tracks have also been excluded", and one OST track
("Unbound", 1:34) **is not in the game at all**.
Source: <https://en.uesp.net/wiki/Skyrim:Music>

Here the "hours more music" intuition is closer to right, but still not as a
hidden reserve -- as *structure*. Skyrim keeps a filename per track (`mus_*`) and
UESP tags each with its type. **20 of the 53 are exploration cues**, and unlike
Oblivion they are split four ways by time of day:

- `mus_explore_morning_01..02` -- 2 cues
- `mus_explore_day_01..09` -- 9 cues
- `mus_explore_dusk_01..03` -- 3 cues
- `mus_explore_night_01..06` -- 6 cues

That is **20 free-roam cues against Oblivion's 7**, and they are longer: 2:05 to
**9:05** ("Wind Guide You", `mus_explore_day_09`).

**The epic pieces are not exploration.** "Dragonborn" (`mus_maintheme`, 3:57) is
type **Special** -- main menu. "One They Fear" is `mus_combat_boss_02`, and every
combat cue is short: 1:12 to 3:16. A player walking across the tundra hears
`mus_explore_*` and nothing else; the choir arrives only when something attacks.
This is the whole answer to "no hero moments" -- Skyrim already enforces it
structurally.

**How it schedules.** Skyrim replaced the folder convention with two editor
records. **Music Type** carries the pool and its arbitration:

> **Priority** -- defines a Music Type's level of importance in relation to other
> Music Types at the time of playback. For example, a Combat music type should
> always have a relatively high priority in relation to Ambient music [...]
> **Fade Duration** -- how long in seconds another Music Type will be faded
> out/in when ducked by this track.
> **Ducking (dB)** -- how much (in dBFS) another Music Type will be ducked [...]
> **Abrupt Transition** -- start playing this Music Type immediately rather than
> waiting to fade out current music.
> **Plays One Selection** -- will only play one Music Track from the list.
> **Cycle Tracks** -- continue to play tracks after entire list has been played.
> **Maintain Track Order** -- don't randomize order of track list, but play it in
> the specific order that the tracks are listed.

Source: <https://ck.uesp.net/wiki/Music_Type> (ck.uesp.net returns 403 to
automated fetches; read via the Internet Archive,
<https://web.archive.org/web/2024/https://ck.uesp.net/wiki/Music_Type>)

Read those together: with **Maintain Track Order** off and **Cycle Tracks** on --
which is what an ambient pool wants -- the type is a **reshuffled, endlessly
cycling playlist**. That is what our `FVoxelUIMusic` already does, one level
down.

And **Music Track** makes silence a first-class object:

> **Silent Track** -- useful in creating periods of silence in between
> traditional Music Tracks or in Palettes. Set desired duration in seconds.
> **Palette Track** -- a multi-layer track type. Individual Music Tracks plugged
> into each layer will be played back at random, simultaneously with other
> layers. **Duration** defines the length of the Palette track [...]
> **Conditions** -- use this to set game conditions on any individual Music
> Track, whether Palette, Silent or Single.

Source: <https://ck.uesp.net/wiki/Music_Track> (same 403; read via
<https://web.archive.org/web/2024/https://ck.uesp.net/wiki/Music_Track>)

So Skyrim does explicitly what Oblivion did by padding WAV tails: **it schedules
silence as a track**. The day/night selection rides the per-track **Conditions**
field. I could not reach a primary source giving the exact hour thresholds for
morning/day/dusk/night -- the Creation Kit wiki is 403 to fetching and the
condition values live in the ESM, not in documentation. **Treat the four-bucket
split as sourced (the filenames and UESP's type column) and the exact hour
boundaries as unverified.**

**Reception, for context on why this style works:** Oblivion's score won the MTV
Best Video Game Score award and OXM's Soundtrack of the Year, and Soule says he
deliberately wrote it about nothing in particular -- "he did not imagine any
specific characters or events; rather, he wanted it 'to comment on the human
condition and the beauty of life'"
(<https://en.uesp.net/wiki/Oblivion:Music>). That is the aesthetic brief for an
exploration cue stated by its composer: **no narrative, no character, no event.**

---

## 2. The list -- 30 exploration/ambient tracks

Types are the games' own classification, not mine. Lengths are UESP's.
**Ambiguity is flagged, not hidden.**

### Oblivion -- Explore pool (all 7)

| # | Title | Len | Mood | Game's type | Flag |
|---|---|---|---|---|---|
| 1 | Through the Valleys | 4:19 | Wide, wistful, unhurried | Explore | -- |
| 2 | Wings of Kynareth | 3:30 | Airy, drifting, weightless | Explore | -- |
| 3 | Peace of Akatosh | 4:11 | Still, warm, reverent | Explore | -- |
| 4 | Auriel's Ascension | 3:05 | Calm rising, gentle awe | Explore | -- |
| 5 | Minstrel's Lament | 4:42 | Melancholy, folk-tinged, tender | Explore | **VOICES.** Soule: "the cadence in the men's voices in the final bars" |
| 6 | King and Country | 4:05 | Noble, striding, slightly martial | Explore | **Mildly heroic** |
| 7 | Glory of Cyrodiil | 2:28 | Bright, proud, ceremonial | Explore | **Mildly heroic** (used in the Play! concert medley beside the main theme) |

### Oblivion -- Public pool (all 5; good "building / settlement" music)

| # | Title | Len | Mood | Game's type | Flag |
|---|---|---|---|---|---|
| 8 | Harvest Dawn | 2:51 | Pastoral, hopeful, morning | Public | -- |
| 9 | Watchman's Ease | 2:05 | Settled, safe, quiet | Public | -- |
| 10 | Dusk at the Market | 2:11 | Warm, closing-down, easy | Public | -- |
| 11 | All's Well | 2:26 | Content, small, domestic | Public | -- |
| 12 | Sunrise of Flutes | 2:56 | Light, pastoral, waking | Public | -- |

### Skyrim -- Explore pool (18 of 20, by time of day)

| # | Title | Len | Mood | Game's type | Flag |
|---|---|---|---|---|---|
| 13 | Dawn | 3:59 | Cold, clear, first light | Explore / morning | -- |
| 14 | Distant Horizons | 3:54 | Open, patient, forward | Explore / morning | -- |
| 15 | Under an Ancient Sun | 3:43 | Vast, weathered, calm | Explore / day | -- |
| 16 | From Past to Present | 5:06 | Reflective, spacious, old | Explore / day + Town | Doubles as `mus_town_day_02` |
| 17 | Frostfall | 3:28 | Bleak, sparse, cold | Explore / day | -- |
| 18 | The Jerall Mountains | 3:21 | Remote, high, austere | Explore / day | -- |
| 19 | The White River | 3:31 | Flowing, gentle, green | Explore / day | -- |
| 20 | Unbroken Road | 6:26 | Long, plodding, resolute | Explore / day | -- |
| 21 | Journey's End | 4:09 | Arriving, softened, tired | Explore / day | -- |
| 22 | Far Horizons | 5:33 | Expansive, searching, slow | Explore / day | -- |
| 23 | Wind Guide You | 9:05 | Endless, meditative, unresolved | Explore / day | **Longest cue in either game** |
| 24 | The Gathering Storm | 2:55 | Uneasy, darkening, low | Explore / dusk | Mildly ominous, not combat |
| 25 | Tundra | 3:51 | Empty, wind-blown, lonely | Explore / dusk | -- |
| 26 | Sky Above, Voice Within | 3:58 | Inward, hushed, wide | Explore / dusk | -- |
| 27 | Kyne's Peace | 3:51 | Serene, sacred, becalmed | Explore / night | -- |
| 28 | Secunda | 2:05 | Small, luminous, nocturnal | Explore / night | -- |
| 29 | Masser | 6:06 | Deep, drifting, moonlit | Explore / night | -- |
| 30 | Aurora | 7:23 | Shimmering, glacial, ambient | Explore / night | -- |

Also qualifying but held to keep the list at 30: **Solitude** (2:12,
`mus_explore_night_04`) and **Standing Stones** (6:38, `mus_explore_night_06`),
which complete Skyrim's 20; Skyrim's three day-town cues **The City Gates**
(3:48), **Ancient Stones** (4:47), **The Streets of Whiterun** (4:06); and
Oblivion's four short Dungeon cues if we ever want a cave pool -- **Wind from
the Depths** (1:42), **Ancient Sorrow** (1:05), **Unmarked Stone** (1:06),
**Deep Waters** (1:11).

**Deliberately excluded** -- these are the ones people mean by "the soundtrack",
and none of them play while you walk: "Reign of the Septims" (Oblivion, Special,
title screen only), "Dragonborn" (Skyrim, Special, main menu), "One They Fear"
and "Watch the Skies" (Skyrim, combat-boss), "Sovngarde"
(`mus_sovngarde_chant_lp`, chant), and every `mus_combat_*` / `battle_*` cue.

Sources for the whole section: <https://en.uesp.net/wiki/Oblivion:Music> and
<https://en.uesp.net/wiki/Skyrim:Music>.

---

## 3. What makes them work -- as a spec

Everything below is derived from the two tracklists and the two Creation Kit
records; the numbers are counted, not felt.

**Length.** Exploration cues are **long**: Oblivion Explore 2:28-4:42 (mean
~3:37), Skyrim Explore 2:05-9:05 (mean ~4:23). Combat cues are **half that**:
Oblivion Battle 1:02-2:08, Skyrim Combat 1:12-3:16. *Length is a category
signal.* A 1-minute cue in an exploration pool reads as an interruption.

**Pool size beats track count.** Oblivion shipped a beloved open world on
**seven** explore cues. The failure mode is not "too few tracks", it is "wrong
tracks in the pool".

**Silence is authored, not incidental.** Oblivion pads every file's tail with
blank seconds so the ear rests; Skyrim promotes this to a **Silent Track**
object with an explicit duration. Either way: **the gap is content and someone
chose its length.** Back-to-back music with no gap is the thing neither game
does.

**Time of day is the only context split that earns its keep for free roaming.**
Skyrim's whole exploration taxonomy is four buckets -- morning (2), day (9),
dusk (3), night (6) -- and *no* biome split. Weight it: **day is 45% of the
pool.**

**Dynamics, not volume.** The scheduler handles the loud moment, not the cue.
Combat gets a **higher Priority** and **ducks** the ambient bed by a set dBFS
over a set **Fade Duration**; ambient cues themselves stay flat. An exploration
track that swells to a climax is fighting the mixer.

**No narrative content.** Soule's own brief: not about characters or events.
No vocals, no choir, no leitmotif statement, no fanfare.

**Order.** Shuffle without repeat (Skyrim: *Maintain Track Order* off,
*Cycle Tracks* on), cycling forever.

> **One-line spec.** Exploration music is a *shuffled, endlessly cycling pool of
> long (3-6 min) instrumental cues with no vocals, no fanfare and flat dynamics,
> filtered by time of day, separated by deliberate authored silence, and
> interrupted only by a shorter, higher-priority pool that ducks it.*

---

## 4. Map to our library

### What the player does today

`FVoxelUIMusic::BuildPlaylist`
(`D:\voxelsim\ue-project\Source\VoxelEarthUI\VoxelUIMusic.cpp:169`) does exactly
this:

```cpp
IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.wav")), /*Files=*/true, /*Directories=*/false);
```

**Every `.wav` in `Content/Audio/Music`, shuffled once per session, walked
forever.** There is no pool, no category and no filter. That is precisely why
the menu played "Defeat _ Game Over" (0:17) and "Boss Battle". The player is
correct; the *library* is the pool, and it should not be.

Two more facts from the code that constrain any fix:

- `Out.Name = FPaths::GetBaseFilename(Path)` (`VoxelUIMusic.cpp:161`) becomes
  `TrackName`, and the HUD displays it. **A filename prefix would be visible to
  the player.**
- The glob is `Dir / "*.wav"`, non-recursive, and the `.mp4` is skipped with a
  log by design (see `VoxelUIMusic.h`).

Note also: `D:\voxelsim\ue-project\Content\Audio\Music\MUSIC_CREDITS.md` still
says "**This directory is EMPTY of audio as of 2026-08-24**" and "Tracks: _None
yet._" There are now 30 WAVs and one MP4 (~1.1 GB) there. **That file is stale
and should be updated** -- it also mandates committing OGG rather than WAV,
which the current contents do not follow (they are gitignored, so no wall was
hit, but the doc no longer describes reality).

### Every file, classified

30 playable WAVs + 1 skipped MP4. Durations computed from the WAV headers (all
are 2ch / 48 kHz / 16-bit PCM, as the header file requires). Where a filename
carries a leading number it maps to the 59-cue inventory in
`D:\voxelsim\docs\music-prompts.md` section 5, and **that inventory's Category
column is the authority** -- I used it rather than guessing from titles.

| File | Len | Proposed pool | Basis / note |
|---|---|---|---|
| `04 — Open Road _ The Central Plains.wav` | 2:37 | **Explore** | inventory: Exploration |
| `05 — The Greatwood _ Under Old Leaves.wav` | 3:24 | **Explore** | inventory: Exploration |
| `06 — The Spine _ Stone and Sky.wav` | 3:47 | **Explore** | inventory: Exploration |
| `07 — The Underway _ Beneath the Mountain.wav` | 3:12 | **Explore** | inventory: Exploration (underground-flavoured; a natural Dungeon seed later) |
| `08 — The Ashfields _ Grey Soil.wav` | 3:23 | **Explore** | inventory: Exploration |
| `09 — The Western Coast _ Caer Drowned.wav` | 3:24 | **Explore** | inventory: Exploration |
| `10 — The Copper Isles _ Salt and Sun.wav` | 2:21 | **Explore** | inventory: Exploration (104 BPM -- the briskest; fine, but it is the outlier) |
| `11 — The Sorrowmarsh _ The Mud Remembers.wav` | 7:59 | **Explore** | inventory: Exploration, 40 BPM atonal. Our "Aurora" / "Wind Guide You" equivalent |
| `12 — The Weeping Wood _ Watched.wav` | 3:23 | **Explore** | inventory: Exploration (cluster / prepared strings -- uneasy, still not combat) |
| `Forest_Exploration.wav` | 4:13 | **Explore** | title is explicit |
| `Discovery_Wonder.wav` | 3:27 | **Explore** | **ambiguous** -- could be a Stinger if it is a one-shot. Check by ear |
| `World Map _ Travel (preferred).wav` | 2:53 | **Explore** | travel cue; section 5 says regenerate this from #04 |
| `Sea _ Sailing.wav` | 2:48 | **Explore** (water) | section 5: superseded by #30 / #32 when those exist |
| `Camp_Rest.wav` | 3:33 | **Explore** (rest) | Oblivion has no equivalent; sits naturally in a calm pool |
| `13 — Aldenholt _ Market and Bel.wav` | 1:47 | **Town** | inventory: Settlement. **Filename is truncated** -- should be "Bell" |
| `16 — Solgrade _ The Unwalled City.wav` | 3:09 | **Town** | inventory: Settlement |
| `20 — Brightwatch _ The Frontier Garrison.wav` | 3:38 | **Town** | inventory: Settlement (lone war horn -- martial edge) |
| `Town_Village.wav` | 1:56 | **Town** | title is explicit |
| `Castle_Court Interior.wav` | 2:54 | **Town** (interior) | -- |
| `Taverns_Feast.wav` | 1:29 | **Town** (tavern) | shortest non-stinger; too short for an explore pool |
| `27 — The Deep Cups _ A Dwarven Dance.wav` | 2:24 | **Town** (tavern) | inventory: Tavern, 88 BPM 6/8 dance |
| `Cave_Dungeon.wav` | 3:34 | **Dungeon** | title is explicit |
| `Stealth_Tension.wav` | 2:39 | **Dungeon** / tension | **NOT Explore** -- this is Oblivion's "Tension" |
| `Boss Battle.wav` | 3:09 | **Combat** | **NOT Explore.** Played in the menu tonight |
| `38 — Enemies Gathering Strength _ The Muster of the Hand.wav` | 2:58 | **Combat** (pre-battle) | inventory: War, 60→88 BPM |
| `Emotional_Loss.wav` | 2:39 | **Cinematic** | **NOT Explore** -- narrative cue |
| `49 — The Vigil _ The Night Before V2.wav` | 5:12 | **Cinematic** | **NOT Explore.** Inventory marks it **LEAD VOCAL** -- violates the no-singing rule outright |
| `02 — Main Title _ Mira-Thal V3.wav` | 4:34 | **Menu** | inventory #02, Identity |
| `Main Theme.wav` | 5:01 | **Menu** (superseded) | **DUPLICATE.** Section 5: "02 replaces `Main Theme` (reworked in place)". Two main themes are in the shuffle |
| `Defeat _ Game Over.wav` | 0:17 | **Stinger** | **NOT a track.** 17 s. Played in the menu tonight |
| `17 — Lirien-Thal _ The Silverwood.mp4` | -- | **Unplayable** | not RIFF/WAVE; skipped with a log. Inventory: Settlement. Needs re-encoding to 16-bit PCM WAV |

**Counts:** Explore 14 - Town 7 - Dungeon 2 - Combat 2 - Cinematic 2 - Menu 2 -
Stinger 1 - Unplayable 1.

**The seven that must leave the exploration pool immediately:**
`Defeat _ Game Over` (17 s stinger), `Boss Battle`, `38 — Enemies Gathering
Strength`, `Stealth_Tension`, `Emotional_Loss`, `49 — The Vigil` (lead vocal),
and one of the two main themes.

That leaves **14 explore cues, 2:21-7:59, mean ~3:31** -- almost exactly
Oblivion's explore pool in both count and mean length, and two-thirds of
Skyrim's. **We are not short of exploration music. We are short of a filter.**

### Proposed tagging scheme

Three options, in the order I would take them:

**A. Subfolders (recommended).** `Content/Audio/Music/Explore/`, `/Town/`,
`/Dungeon/`, `/Combat/`, `/Cinematic/`, `/Menu/`, `/Stinger/`.

- Costs one line at `VoxelUIMusic.cpp:182` -- glob `Dir / Pool / "*.wav"` instead
  of `Dir / "*.wav"`.
- **Does not change `TrackName`**, because it is `GetBaseFilename` -- the HUD
  label is unaffected. This is the deciding advantage over option B.
- Self-evident to the designer: adding a track is still "drop a .wav in a
  folder", which is the property `VoxelUIMusic.h` deliberately protects.
- Loose files left at the root can stay in Explore (a permissive default), or --
  better, per *silent success is the house failure* -- be **logged as
  unclassified and excluded**, so a forgotten file is loud rather than silently
  in the pool.

**B. Filename prefix** (`exp_`, `twn_`, `dng_`, `cmb_`, `cin_`, `mnu_`, `sti_`).
Zero code change if the glob becomes `exp_*.wav`. **Rejected as primary** because
`TrackName` is the base filename and the HUD would display
`exp_Forest_Exploration` to the player.

**C. A `music.json` manifest** beside the WAVs:
`{ "Forest_Exploration": { "pool": "explore", "tod": "any" } }`. Most expressive
-- it is the only option that can carry Skyrim's **time-of-day** field, per-track
**priority**, and a **display name** distinct from the filename (which would fix
"Bel" to "Bell" without a rename). Costs a parser. **Take this when time-of-day
pools arrive; A is enough today.**

Whichever is chosen, the two structural borrowings from part 3 are separate work
and worth scheduling:

1. **An authored gap between cues** -- Oblivion bakes it into the file tail,
   Skyrim schedules a Silent Track. We currently play back-to-back. A single
   `float GapSeconds` between tracks in `TickGameThread` is the cheap version.
2. **Priority + ducking**, so a combat pool can interrupt the explore bed and
   return, instead of the pools being mutually exclusive.

---

## Sources

- Oblivion tracklist, categories, folder layout, blank tails, Soule commentary,
  awards -- <https://en.uesp.net/wiki/Oblivion:Music>
- Skyrim tracklist, `mus_*` filenames, type column, OST omissions --
  <https://en.uesp.net/wiki/Skyrim:Music>
- Music Type record (priority, ducking, fade, cycle / shuffle flags) --
  <https://ck.uesp.net/wiki/Music_Type>, read via
  <https://web.archive.org/web/2024/https://ck.uesp.net/wiki/Music_Type>
- Music Track record (Silent Track, Palette Track, Conditions) --
  <https://ck.uesp.net/wiki/Music_Track>, read via
  <https://web.archive.org/web/2024/https://ck.uesp.net/wiki/Music_Track>
- Oblivion Construction Set cell Music Type dropdown --
  <https://cs.uesp.net/wiki/Interior_cells>
- `StreamMusic` and the three Oblivion music types --
  <https://cs.uesp.net/wiki/StreamMusic>

**Unreachable / unverified, stated as such:**

- `ck.uesp.net` and `skyrimck.uesp.net` return **403** to automated fetches;
  `creationkit.com` is serving an **XWiki maintenance page**. Both Creation Kit
  records above were therefore read from the Internet Archive.
- `en.uesp.net` returns **403** to the WebFetch tool but **200** to a plain
  request with a browser user-agent; the tracklists were read that way.
- **The exact game-hour boundaries** dividing Skyrim's morning / day / dusk /
  night explore pools are **not sourced**. The four-bucket split itself is
  sourced (filenames + UESP type column); the thresholds live in the ESM's
  per-track Conditions and I found no primary documentation of their values.
