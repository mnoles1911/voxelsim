<!--
PROVENANCE. Verbatim copy of design/SFX_PROMPTS.md from the Mira-Thal /
Voxelmark Godot checkout, github.com/mnoles1911/Test @ main, retrieved
2026-08-24. Upstream merged as 7eb3372d (#216); §8 last updated 2026-05-18.

The generation pass for docs/sfx-library.md, written for ElevenLabs Sound
Effects: one row per entry, each with the prompt text, duration,
prompt_influence and loop flag. Covers Phase 1 (combat + locomotion core,
camp/weather/water basics) plus a started Phase 2 (voxel/terrain editing).

WHY THIS IS THE MORE USEFUL OF THE TWO DOCS: §8 is an honest status record --
what was rendered, what it cost, and what is wrong with the result.

  * Real calibrated cost: ~8.2 credits/second + ~19 credits/generation floor.
    One cycle spent ~84k-118k credits for the 548 takes.
  * Combat (Cat 02, ~429 gens, ~9,585 cr) is the outstanding render.
  * §8b is the quality record and it is unflattering in a useful way: the
    takes were bulk-committed UNPRUNED at the designer's explicit direction,
    skipping curation, and the in-game verdict was "sound quality is rough".
    The stated fix is entirely non-code -- audition each id's ~7 takes, keep
    the strongest 3-5, delete the rest, and re-roll the footstep prompts,
    which are the poorest source material.

Read that before spending anything: the lesson already paid for is that
generating more takes is not what makes this sound good.

§8a's wiring status describes a Godot `AudioManager` autoload and does not
transfer to Unreal. The prompts themselves are engine-independent.

THIS DOC IS NO LONGER A SUBSET OF THE LIBRARY. Sections 1-7b are the original
hand-authored Phase 1 and 2 prompts, verbatim and untouched. Section 7c was
added 2026-08-24 and carries a row for EVERY remaining library id, so the two
documents now match 1:1 -- 724 library ids, 725 prompt rows (the extra is one
coarse stand-in, explained below).

  docs/sfx-library.md      what sounds exist, as prose and matrices
  tools/sfx_families/*.py  those matrices expanded to ids, one file per category
  section 7c below         the generated prompt row for each
  tools/lint-sfx-coverage.py   fails if either side gains or loses an id

Section 7c is GENERATED. Edit tools/sfx_families/, then run
`python tools/gen-sfx-prompts.py`. Sections 1-7b are hand-authored and the
generator never touches them, because 108 of their ids have audio rendered from
that exact prompt text and rewriting it would break reproducibility.

FOURTEEN IDS WERE RENDERED UNDER DIFFERENT NAMES THAN THE LIBRARY GIVES. The
Phase 2 voxel render wrote `vox_<tool>_<event>_<material>` where the library
specifies `vox_<material>_<event>` with the correct tool implied, and two impact
ids dropped the `_terrain` suffix. The files on disk carry the rendered names.
This is recorded as an alias table in tools/sfx_spec.py rather than fixed,
because fixing it costs something either way -- renaming the doc breaks
reproducibility against rendered audio, renaming the library breaks its own
convention and the folder plan built on it. The lint prints the drift on every
run so the decision stays visible instead of expiring quietly.
-->

# SFX Prompts — ElevenLabs Pass (Phase 1 Core)

Generation prompts for the Phase 1 core of `design/SFX_LIBRARY.md`, written
for **ElevenLabs Sound Effects**. This is the copy-paste / batch-feed spec:
one row per library entry.

> Source spec: `design/SFX_LIBRARY.md` (the inventory). Routing/format:
> `design/AUDIO_DESIGN.md`. This doc only covers **Phase 1** (combat &
> locomotion core + camp/weather/water basics). Phases 2–6 follow after review.

---

## 1. How to use this doc

**Columns**
- **id** — file stem. Render `var` takes; save as `<id>_01.ogg … _0N.ogg`
  into the `assets/audio/sfx/<folder>/` from `AUDIO_DESIGN.md` /
  `SFX_LIBRARY.md §2`. Unique entries (`var 1`) save as `<id>.ogg`.
- **prompt** — paste verbatim into the ElevenLabs SFX text box.
- **dur** — `duration_seconds`. A number, or `auto` to let ElevenLabs decide
  (good for organic one-shots). Loops use a fixed 12–20 s.
- **infl** — `prompt_influence` 0.0–1.0. Higher = more literal/precise (good
  for mechanical, impacts, cues); lower = more organic variation (good for
  foley, breath, ambient).
- **loop** — `Y`: prompt is written as a seamless loop; render long, then
  loop in Godot (ElevenLabs has no true loop, so the prompt forces a
  consistent no-start/no-end texture and we loop in-engine). `N`: one-shot.
- **var** — how many takes to keep (the variation count from SFX_LIBRARY).
  **Workflow:** generate ~`var × 2` takes from the *same* prompt, keep the
  `var` best/most-distinct. ElevenLabs varies naturally per generation —
  do **not** reword the prompt per variation.
- **bus** — engine routing (`AUDIO_DESIGN.md`).

**Global rules baked into every prompt (don't re-add per row):**
- Mono, **dry, close-mic'd, no reverb** — the engine adds spatialization and
  per-environment reverb. Prompts say "dry, close, no reverb".
- **No music, no musicality, no melody** — these are sound effects.
- Single discrete event for one-shots; consistent texture for loops.
- Realistic / grounded, low-fantasy (KCD2/Minecraft reference), not cartoony.

**Batch feed:** the table is API-ready — iterate rows, call ElevenLabs SFX
with `text=prompt`, `duration_seconds=dur`, `prompt_influence=infl`,
`loop=(loop==Y)`, request `var×2` generations per row. **`tools/render_sfx.py`
does exactly this** — it parses these tables and drives the
`/v1/sound-generation` endpoint into a per-id review folder (idempotent,
cost-capped, `--dry-run`/`--mock`). See `tools/README.md → render_sfx.py`.

**Phase 1 scope (~165 entries):** Cat 01 live-surface locomotion · Cat 02
universal combat verbs + Roland's longsword + shipped spear · Cat 03 core
impact matrix + the 4 implemented enemies · Cat 09 fire & camp · Cat 07
weather basics · Cat 08 water core.

---

## 2. Category 01 — Locomotion (live surfaces)

Footsteps: live surfaces only (`grass, dirt, stone, wood, sand,
shallow_water`) × gaits (`walk, run, sprint, crouch`). Single footstep =
one shoe contact (engine triggers per step); render as a *single step*, not
a sequence. `infl` mid (0.4) so takes vary naturally.

**Why footsteps are `loop=N` (incl. sprint) — by design, not an oversight.**
A footstep file is one foot plant; the walk/run/sprint *cadence* lives in the
locomotion code, not the audio. The engine fires one `step_*` one-shot per
foot plant (animation footstep marker or a distance-based emitter that fires
faster as speed rises), random-picking one of the `var` takes with slight
pitch/volume jitter (anti-"machine-gun"). Looping would lock the rhythm to a
fixed tempo (feet de-sync from sound on accel/decel/stop), keep the wrong
surface when the player crosses materials mid-stride, and pop on start/stop.
This is the Skyrim/KCD2/Minecraft model. The *continuous* part of locomotion
is what loops: the `armor_*_move_loop` rustle/jingle bed (played under the
steps while moving), `water_wade_shallow_loop`, `climb_rock_loop`, and the
`roland_breath_*_loop` states. Rule across the whole doc: **discrete impact
= one-shot; continuous texture/state = loop.** Implementation requirement:
the locomotion system must trigger per foot plant, pick the variation and
surface per step, and jitter pitch/volume.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| step_walk_grass | A single soft footstep on grass and soil, a leather boot pressing down, faint dry grass crunch, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_walk_dirt | A single footstep on bare packed dirt, soft earthy thud, slight grit, leather boot, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_walk_stone | A single footstep on stone flagging, hard leather-on-rock tap with a faint scuff, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_walk_wood | A single footstep on an old wooden plank floor, dull hollow knock with a slight creak, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_walk_sand | A single footstep into dry sand, soft muffled compression, fine grain shift, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_walk_shallow_water | A single footstep into shallow water over mud, a low wet splash and squelch, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| step_run_grass | A single fast running footstep on grass and soil, harder impact, dry grass scuff, leather boot, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_run_dirt | A single fast running footstep on packed dirt, firm earthy impact and grit, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_run_stone | A single fast running footstep on stone, sharp hard boot strike with scuff, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_run_wood | A single fast running footstep on wooden planks, loud hollow knock and creak, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_run_sand | A single fast running footstep in sand, hard muffled compression, kicked grain, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_run_shallow_water | A single fast running footstep through shallow water, hard wet splash, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| step_sprint_grass | A single hard sprinting footstep on grass, heavy fast impact and grass tear, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_sprint_dirt | A single hard sprinting footstep on dirt, heavy fast earthy slam and grit spray, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_sprint_stone | A single hard sprinting footstep on stone, loud sharp boot slam and skid, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_sprint_wood | A single hard sprinting footstep on wood planks, loud hollow boom and creak, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_sprint_sand | A single hard sprinting footstep in sand, heavy muffled thud, sand spray, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_sprint_shallow_water | A single hard sprinting footstep through shallow water, big hard splash and spray, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 5 | SFX |
| step_crouch_grass | A single very soft slow crouched footstep on grass, careful muffled press, faint, dry close mono, no reverb, no music | 0.6 | 0.35 | N | 5 | SFX |
| step_crouch_dirt | A single very soft slow crouched footstep on dirt, careful muffled earthy press, dry close mono, no reverb, no music | 0.6 | 0.35 | N | 5 | SFX |
| step_crouch_stone | A single soft slow crouched footstep on stone, quiet controlled leather contact, faint scuff, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| step_crouch_wood | A single soft slow crouched footstep on wood, careful low creak, suppressed knock, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| step_crouch_sand | A single soft slow crouched footstep in sand, near-silent muffled grain shift, dry close mono, no reverb, no music | 0.6 | 0.35 | N | 5 | SFX |
| step_crouch_shallow_water | A single slow careful crouched footstep into shallow water, gentle controlled wet trickle, dry close mono, no reverb, no music | 0.7 | 0.35 | N | 5 | SFX |

Jump / land (live surfaces) + effort:

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| jumpland_grass | A body landing from a jump onto grass and soil, a firm two-foot thud with grass crunch, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 5 | SFX |
| jumpland_dirt | A body landing onto packed dirt, firm earthy double thud and grit, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 5 | SFX |
| jumpland_stone | A body landing onto stone, hard heavy boot impact with a sharp scuff, dry close mono, no reverb, no music | 0.7 | 0.5 | N | 5 | SFX |
| jumpland_wood | A body landing onto a wood plank floor, loud hollow boom and timber creak, dry close mono, no reverb, no music | 0.7 | 0.5 | N | 5 | SFX |
| jumpland_sand | A body landing into sand, heavy muffled compression and grain spray, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 5 | SFX |
| jumpland_shallow_water | A body landing into shallow water, big heavy splash and spray, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 5 | SFX |
| jump_exert_grunt | A short light male effort grunt on jumping, breath push, no words, dry close mono, no reverb, no music | 0.6 | 0.35 | N | 3 | Voice |
| land_heavy_stagger | A heavy hard landing from a high fall, boots slamming and a stumbling scuff, pained breath, dry close mono, no reverb, no music | 1.0 | 0.5 | N | 3 | SFX |
| land_soft | A gentle low-height landing, soft controlled boot touch, faint cloth, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 3 | SFX |

Armor-weight movement loops (mixed over steps by equipped weight) + traversal
+ player breath:

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| armor_cloth_move_loop | Perfectly seamless loop of soft cloth and leather garment rustle from a walking body, steady consistent texture, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.3 | Y | 1 | SFX |
| armor_leather_move_loop | Perfectly seamless loop of creaking leather armor flexing on a moving body, steady consistent, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.35 | Y | 1 | SFX |
| armor_mail_move_loop | Perfectly seamless loop of chainmail rings shifting and jingling on a walking body, steady consistent metallic rustle, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.4 | Y | 1 | SFX |
| armor_plate_move_loop | Perfectly seamless loop of plate armor clanking and leather straps creaking on a moving body, steady heavy consistent, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.45 | Y | 1 | SFX |
| water_wade_shallow_loop | Perfectly seamless loop of a person wading steadily through shallow water, continuous rhythmic sloshing, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.35 | Y | 1 | SFX |
| water_entry_walk | A person walking into water from shore, steps turning to wading splashes, dry close mono, no reverb, no music | 1.5 | 0.4 | N | 3 | SFX |
| water_entry_run_plunge | A person running and plunging into deep water, a big heavy splash and churn, dry close mono, no reverb, no music | 1.5 | 0.45 | N | 3 | SFX |
| climb_rock_loop | Perfectly seamless loop of hands and boots scrabbling and gripping on rock while climbing, grit and cloth strain, steady, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.35 | Y | 1 | SFX |
| climb_grunt | A short strained male effort grunt while pulling up a climb, no words, dry close mono, no reverb, no music | 0.7 | 0.35 | N | 3 | Voice |
| vault_ledge | A quick body vault over a ledge, a hand slap on stone, cloth scuff and a light landing, dry close mono, no reverb, no music | 0.9 | 0.4 | N | 3 | SFX |
| roland_breath_idle_loop | Perfectly seamless loop of calm quiet steady human breathing at rest, relaxed, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.3 | Y | 1 | Voice |
| roland_breath_exert_loop | Perfectly seamless loop of heavy winded human breathing after exertion, fast and laboured but controlled, steady, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.35 | Y | 1 | Voice |
| roland_breath_lowhp_loop | Perfectly seamless loop of pained laboured human breathing, strained and uneven, hurt but not theatrical, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.35 | Y | 1 | Voice |
| roland_breath_critical_loop | Perfectly seamless loop of ragged desperate shallow human breathing, badly wounded, gasping, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.4 | Y | 1 | Voice |
| roland_effort_grunt | A short sharp male combat effort grunt, exertion, no words, dry close mono, no reverb, no music | 0.5 | 0.35 | N | 5 | Voice |
| roland_jump_exhale | A short sharp breath exhale on physical effort, no words, dry close mono, no reverb, no music | 0.4 | 0.35 | N | 3 | Voice |

---

## 3. Category 02 — Combat: Player (universal + longsword + spear)

Universal combat verbs (the parry/heavy/unblock cues are **diegetic from the
enemy's stance, not UI beeps** — keep them physical, per `AUDIO_DESIGN.md`):

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| cmb_block_hold_loop | Perfectly seamless loop of a sword blade braced under continuous pressure, a low metallic resonant strain with faint scrape, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 6 | 0.4 | Y | 1 | Combat |
| cmb_block_impact | A heavy blow caught on a raised steel sword, a hard resonant clang with a scrape, dry close mono, no reverb, no music | 0.8 | 0.55 | N | 5 | Combat |
| cmb_parry_success | A clean sharp steel-on-steel parry, a bright high ringing deflection, satisfying and precise, dry close mono, no reverb, no music | 0.7 | 0.6 | N | 4 | Combat |
| cmb_riposte_strike | A fast follow-up sword strike biting into a body, a quick whoosh and wet armored hit, dry close mono, no reverb, no music | 0.7 | 0.5 | N | 3 | Combat |
| cmb_cue_parry_green | A brief soft physical chime resonance from an enemy weapon stance, very short, subtle, not electronic, dry close mono, no reverb, no music | 0.4 | 0.55 | N | 2 | Combat |
| cmb_cue_heavy_yellow | A low tonal warning resonance from an enemy winding up a heavy blow, a physical creak of force gathering, short, not electronic, dry close mono, no reverb, no music | 0.6 | 0.55 | N | 2 | Combat |
| cmb_cue_unblock_red | A low menacing thud and growl-like surge from an enemy committing to an unblockable strike, physical not electronic, dry close mono, no reverb, no music | 0.6 | 0.55 | N | 2 | Combat |
| cmb_dodge_roll | A fast body roll across the ground, cloth and armor tumble with a quick scuff, dry close mono, no reverb, no music | 0.8 | 0.4 | N | 4 | Combat |
| cmb_dodge_step | A quick sharp evasive side-step, fast cloth and foot scuff, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 4 | Combat |
| cmb_stagger_break | A guard broken by exhaustion, a stumbling armored stagger with a sharp winded gasp, dry close mono, no reverb, no music | 1.2 | 0.45 | N | 3 | Combat |
| cmb_endurance_empty | A sharp exhausted gasp as stamina fails, breath emptied, no words, dry close mono, no reverb, no music | 0.7 | 0.35 | N | 3 | Voice |
| cmb_lockon_toggle | A very short subtle physical tick of focus snapping onto a target, minimal, not electronic, dry close mono, no reverb, no music | 0.3 | 0.5 | N | 1 | UI |
| cmb_timeslow_enter | A short downward-pitching whoosh as time slows after a lethal hit, air warping low, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 1 | Combat |
| cmb_timeslow_exit | A short upward-pitching whoosh as time snaps back to normal speed, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 1 | Combat |

Longsword class (Roland's mainline weapon):

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| cmb_longsword_swing_light | A fast light sword swing through air, a quick sharp steel whoosh, dry close mono, no reverb, no music | 0.5 | 0.5 | N | 5 | Combat |
| cmb_longsword_swing_heavy | A slow heavy committed two-handed sword swing through air, a deep powerful whoosh, dry close mono, no reverb, no music | 0.8 | 0.5 | N | 4 | Combat |
| cmb_longsword_swing_miss_air | A sword swung hard and missing, a wide hollow air-displacement whoosh, dry close mono, no reverb, no music | 0.7 | 0.5 | N | 4 | Combat |
| cmb_longsword_draw | A longsword drawn from a leather scabbard, a smooth metallic scrape ending in a light ring, dry close mono, no reverb, no music | 0.9 | 0.5 | N | 3 | Combat |
| cmb_longsword_sheathe | A longsword sliding into a leather scabbard, a metallic scrape ending in a soft seat, dry close mono, no reverb, no music | 0.9 | 0.5 | N | 3 | Combat |
| cmb_longsword_block_hold_loop | Perfectly seamless loop of a longsword held braced under pressure, low metallic strain and faint grind, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 6 | 0.4 | Y | 1 | Combat |
| cmb_longsword_parry | A longsword deflecting an incoming blade, a sharp bright clean ring, dry close mono, no reverb, no music | 0.6 | 0.6 | N | 4 | Combat |
| cmb_longsword_special | A powerful committed longsword thrust and finisher, a hard whoosh into a heavy armored impact, dry close mono, no reverb, no music | 0.9 | 0.5 | N | 3 | Combat |

Tier / condition timbre layers (mixed over the base hit, not standalone hits):

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| cmb_tier_common_layer | A thin dull iron resonance layer for a low-quality blade impact, plain and slightly muted, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 1 | Combat |
| cmb_tier_quality_layer | A brighter cleaner steel resonance layer for a fine blade impact, clear ring, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 1 | Combat |
| cmb_tier_masterwork_layer | A rich sustained bell-like steel resonance layer for a masterwork blade impact, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 1 | Combat |
| cmb_condition_dull_layer | A dull flat lifeless metallic layer for a worn damaged blade impact, no ring, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 1 | Combat |
| cmb_condition_break_fail | A worn blade failing under a blocked blow, a cracked dull metallic give and rattle, dry close mono, no reverb, no music | 0.7 | 0.5 | N | 2 | Combat |

Shipped ThrowableSpear set:

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| cmb_spear_windup | A spear drawn back and braced to throw, cloth and arm tension with a faint shaft creak, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 3 | Combat |
| cmb_spear_throw | A spear thrown hard, a sharp whoosh of a wooden shaft cutting air, dry close mono, no reverb, no music | 0.6 | 0.5 | N | 3 | Combat |
| cmb_spear_inflight_loop | Perfectly seamless loop of a spear shaft spinning and whirring through the air, steady consistent whir, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 4 | 0.4 | Y | 1 | Combat |
| cmb_spear_embed_flesh | A spear point striking and sinking into a body, a hard wet meaty impact and shaft quiver, dry close mono, no reverb, no music | 0.7 | 0.55 | N | 3 | Combat |
| cmb_spear_embed_wood | A spear point striking and sticking into wood, a sharp solid thunk and shaft vibration, dry close mono, no reverb, no music | 0.7 | 0.55 | N | 3 | Combat |
| cmb_spear_embed_stone | A spear point striking stone and skittering off, a hard sharp clang and clatter, dry close mono, no reverb, no music | 0.7 | 0.55 | N | 3 | Combat |
| cmb_spear_retrieve | A spear pulled free from a body or surface and grabbed, a wet or wooden tug and a wooden handle grab, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 3 | Combat |
| cmb_throw_arc | A short light whoosh of a thrown object arcing through the air, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 3 | Combat |

---

## 4. Category 03 — Impacts (core) & the 4 Enemies

Core impact matrix — Phase 1 live damage types `slash` (longsword) and
`pierce` (spear) against the seven targets:

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| cmb_hit_slash_flesh_unarmored | A sword slashing into unarmored flesh, a fast wet cutting impact, grounded not gory-cartoon, dry close mono, no reverb, no music | 0.6 | 0.55 | N | 4 | Combat |
| cmb_hit_slash_flesh_padded | A sword slash landing on padded gambeson over a body, a muffled cloth-dampened thud with a faint cut, dry close mono, no reverb, no music | 0.6 | 0.55 | N | 4 | Combat |
| cmb_hit_slash_mail | A sword blow against chainmail, a deflecting metallic clatter of rings, little penetration, dry close mono, no reverb, no music | 0.6 | 0.6 | N | 4 | Combat |
| cmb_hit_slash_plate | A sword blow glancing off steel plate, a hard bright clang and skid, no penetration, dry close mono, no reverb, no music | 0.6 | 0.6 | N | 4 | Combat |
| cmb_hit_slash_shield | A sword strike on a wooden shield, a solid woody thud with a metal rim ring, dry close mono, no reverb, no music | 0.6 | 0.55 | N | 4 | Combat |
| cmb_hit_slash_wood | A sword chopping into a wooden surface, a sharp bite and splinter, dry close mono, no reverb, no music | 0.6 | 0.55 | N | 4 | Combat |
| cmb_hit_slash_stone | A sword striking stone, a hard bright clang and scrape with a spark feel, dry close mono, no reverb, no music | 0.6 | 0.6 | N | 4 | Combat |
| cmb_hit_pierce_flesh_unarmored | A spear or point thrust into unarmored flesh, a sharp wet stab, grounded not cartoon, dry close mono, no reverb, no music | 0.6 | 0.55 | N | 4 | Combat |
| cmb_hit_pierce_flesh_padded | A point driven through padded cloth into a body, a muffled tearing stab, dry close mono, no reverb, no music | 0.6 | 0.55 | N | 4 | Combat |
| cmb_hit_pierce_mail | A point driven against chainmail, a metallic ring with strained rings parting, dry close mono, no reverb, no music | 0.6 | 0.6 | N | 4 | Combat |
| cmb_hit_pierce_plate | A point skidding off steel plate, a sharp hard scrape and clang, no penetration, dry close mono, no reverb, no music | 0.6 | 0.6 | N | 4 | Combat |
| cmb_hit_pierce_shield | A spear point punching into a wooden shield, a hard splintering thunk, dry close mono, no reverb, no music | 0.6 | 0.55 | N | 4 | Combat |
| cmb_hit_pierce_wood | A spear point stabbing into wood, a sharp solid thunk, dry close mono, no reverb, no music | 0.6 | 0.55 | N | 4 | Combat |
| cmb_hit_pierce_stone | A spear point jabbing stone and slipping, a sharp clack and scrape, dry close mono, no reverb, no music | 0.6 | 0.6 | N | 4 | Combat |

**Goblin** (small, wiry, swarm, unarmored — vocalizations are guttural and
non-human, never words):

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| cmb_goblin_idle_chatter | Low guttural muttering and clicking of a small wiry goblin creature idling, non-verbal, menacing but small, dry close mono, no reverb, no music | 1.5 | 0.4 | N | 5 | Combat |
| cmb_goblin_alert_shout | A sharp guttural shriek of a goblin spotting an enemy, an alarm screech, non-verbal, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 3 | Combat |
| cmb_goblin_group_alert | Several goblins screeching and snarling together as a pack rouses, non-verbal, dry close mono, no reverb, no music | 1.5 | 0.4 | N | 2 | Combat |
| cmb_goblin_attack_jab | A goblin's quick vicious attack snarl with a small weapon jab whoosh, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 4 | Combat |
| cmb_goblin_attack_leap | A goblin shrieking and leaping in to attack, a lunging snarl, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 3 | Combat |
| cmb_goblin_hurt | A goblin taking a hit, a sharp pained guttural yelp, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 5 | Combat |
| cmb_goblin_death | A goblin killed, a choked guttural death cry collapsing to a small body fall, dry close mono, no reverb, no music | 1.2 | 0.45 | N | 4 | Combat |
| cmb_goblin_gib_overkill | A goblin destroyed by a massive overkill blow, a wet violent burst and spatter, grounded, dry close mono, no reverb, no music | 0.9 | 0.5 | N | 2 | Combat |
| cmb_goblin_flee | A goblin panicking and fleeing, frightened gibbering and scrambling, non-verbal, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 3 | Combat |
| cmb_goblin_footstep_loop | Perfectly seamless loop of a small light creature's scrabbling running footsteps on dirt, quick and erratic, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 6 | 0.35 | Y | 1 | Combat |

**Ashfallen** (elite, faceless, heavy armor — **no voice**; identity is armor
foley and breath behind a helm):

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| cmb_ashfallen_footstep_heavy | A single slow heavy armored footstep, a steel boot and plate weight pressing down with a strap creak, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 5 | Combat |
| cmb_ashfallen_armor_creak_idle_loop | Perfectly seamless loop of heavy plate armor and leather straps creaking with slow breathing behind a helm, steady, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.4 | Y | 1 | Combat |
| cmb_ashfallen_telegraph_measured | A measured heavy sword raised to strike, a slow deliberate armored shift and blade lift, physical tell, dry close mono, no reverb, no music | 0.8 | 0.5 | N | 2 | Combat |
| cmb_ashfallen_telegraph_heavy | A big heavy blow winding up, armor straining and a deep gathering shift of force, physical tell, dry close mono, no reverb, no music | 0.9 | 0.5 | N | 2 | Combat |
| cmb_ashfallen_telegraph_thrust_red | A short sharp committed armored lunge wind-up for an unblockable thrust, a hard exhale behind a helm, dry close mono, no reverb, no music | 0.7 | 0.5 | N | 2 | Combat |
| cmb_ashfallen_shield_bash | A heavy steel shield bash slamming forward, a hard flat metallic impact, dry close mono, no reverb, no music | 0.7 | 0.55 | N | 3 | Combat |
| cmb_ashfallen_hurt_clang_chip | A blow landing on heavy armor, a hard metallic clang and chip with a stifled grunt behind a helm, dry close mono, no reverb, no music | 0.7 | 0.55 | N | 5 | Combat |
| cmb_ashfallen_death_collapse | A heavily armored warrior killed, a final stifled breath and a heavy plate-armored body crashing to the ground, dry close mono, no reverb, no music | 1.5 | 0.5 | N | 3 | Combat |
| cmb_ashfallen_blade_drop | A rusted heavy blade dropping and clattering onto the ground, dry close mono, no reverb, no music | 0.9 | 0.5 | N | 2 | Combat |

**Wolf** (pack flanker — audible before seen):

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| cmb_wolf_breath_pant_loop | Perfectly seamless loop of a large wolf panting and breathing low, steady, slightly threatening, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 8 | 0.35 | Y | 1 | Combat |
| cmb_wolf_undergrowth_move | A wolf moving fast through brush and undergrowth, rustling leaves and paws, dry close mono, no reverb, no music | 1.0 | 0.35 | N | 4 | Combat |
| cmb_wolf_alert_growl | A low rising menacing wolf growl as it locks on, dry close mono, no reverb, no music | 1.0 | 0.45 | N | 3 | Combat |
| cmb_wolf_lunge_windup | A wolf snarling and coiling to lunge, a fast aggressive bark-snarl, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 3 | Combat |
| cmb_wolf_bite | A wolf's fast snapping bite, jaws clashing with a wet snap, dry close mono, no reverb, no music | 0.5 | 0.5 | N | 4 | Combat |
| cmb_wolf_yelp_hurt | A wolf hit and yelping in pain, a sharp canine cry, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 4 | Combat |
| cmb_wolf_death | A wolf killed, a final pained snarl-whine cut short, body drop, dry close mono, no reverb, no music | 1.0 | 0.45 | N | 3 | Combat |
| cmb_wolf_paw_steps_loop | Perfectly seamless loop of a four-legged animal trotting fast on soil and leaves, soft rapid paw pattern, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 6 | 0.35 | Y | 1 | Combat |

**Bear** (solo mini-boss, heavy and slow):

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| cmb_bear_growl_idle_loop | Perfectly seamless loop of a huge bear breathing and low rumbling growls, slow and massive, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 9 | 0.4 | Y | 1 | Combat |
| cmb_bear_charge_telegraph | A bear rearing into a charge with a deep explosive roar, dry close mono, no reverb, no music | 1.2 | 0.5 | N | 2 | Combat |
| cmb_bear_run_thunder | A massive bear running, thunderous heavy four-legged ground impacts, dry close mono, no reverb, no music | 1.5 | 0.45 | N | 3 | Combat |
| cmb_bear_claw_swipe | A bear's wide heavy claw swipe, a huge whoosh of force, dry close mono, no reverb, no music | 0.8 | 0.5 | N | 4 | Combat |
| cmb_bear_bite | A bear's huge crushing bite, massive jaws snapping wetly, dry close mono, no reverb, no music | 0.7 | 0.5 | N | 3 | Combat |
| cmb_bear_rear_roar | A wounded enraged bear rearing up with a colossal echoing roar, dry close mono, no reverb, no music | 2.0 | 0.5 | N | 2 | Combat |
| cmb_bear_slam | A bear slamming both forelimbs down, a tremendous ground-shaking impact, dry close mono, no reverb, no music | 1.0 | 0.5 | N | 3 | Combat |
| cmb_bear_footfall_heavy | A single colossal bear footfall on soil, deep heavy weight, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 5 | Combat |
| cmb_bear_hurt_deep | A bear taking a hit, a deep enraged pained roar-grunt, dry close mono, no reverb, no music | 0.9 | 0.45 | N | 4 | Combat |
| cmb_bear_death_heavy | A bear killed, a final deep collapsing roar and a massive body crashing down, dry close mono, no reverb, no music | 2.0 | 0.5 | N | 3 | Combat |

---

## 5. Category 09 — Fire & Camp

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| fire_campfire_crackle_loop | Perfectly seamless loop of a steady campfire, continuous wood crackle and soft flame whoosh, consistent, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 18 | 0.3 | Y | 1 | Ambient |
| fire_ember_pop | A single sharp pop and spark snap from a fire ember, dry close mono, no reverb, no music | 0.4 | 0.4 | N | 5 | Ambient |
| fire_log_settle | A burning log shifting and collapsing in a fire with a soft crumble and spark burst, dry close mono, no reverb, no music | 1.0 | 0.35 | N | 3 | Ambient |
| fire_ignite_whoosh | A fire catching and flaring up, a soft whoomph of flame taking hold, dry close mono, no reverb, no music | 1.0 | 0.45 | N | 3 | Ambient |
| fire_tinder_kindle | Tinder and small twigs catching, faint crackle building from a struck spark, dry close mono, no reverb, no music | 1.5 | 0.35 | N | 2 | Ambient |
| fire_extinguish_hiss | A fire doused, a sharp steam hiss and sputter dying out, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 2 | Ambient |
| fire_smoke_fade | Faint soft smoke and last embers fading after a fire is out, very quiet, dry close mono, no reverb, no music | 1.5 | 0.3 | N | 1 | Ambient |
| fire_torch_flutter_loop | Perfectly seamless loop of a handheld torch flame fluttering and guttering, steady, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.3 | Y | 1 | Ambient |
| fire_brazier_loop | Perfectly seamless loop of a large steady brazier fire burning, fuller and deeper than a torch, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 16 | 0.3 | Y | 1 | Ambient |
| camp_rest_fade_sting | A very soft brief tonal breath as the world fades to rest, gentle, almost silent, dry close mono, no reverb, no music | 1.0 | 0.3 | N | 1 | UI |
| camp_rest_autosave_chime | A single very soft understated low resonance marking a quiet autosave, no fanfare, dry close mono, no reverb, no music | 0.8 | 0.4 | N | 1 | UI |

---

## 6. Category 07 — Weather (basics)

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| wx_clear_bed_loop | Perfectly seamless loop of a calm clear-day outdoor ambience, very gentle air and faint distant openness, steady, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| wx_wind_calm_loop | Perfectly seamless loop of soft light calm wind, gentle steady air movement, no gusts, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, mono, no reverb, no music | 18 | 0.25 | Y | 1 | Ambient |
| wx_wind_breeze_loop | Perfectly seamless loop of a moderate breeze through open land, steady with mild swells, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, mono, no reverb, no music | 18 | 0.3 | Y | 1 | Ambient |
| wx_wind_storm_loop | Perfectly seamless loop of strong howling storm wind, powerful sustained gusting, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, mono, no reverb, no music | 18 | 0.35 | Y | 1 | Ambient |
| wx_rain_light_soil_loop | Perfectly seamless loop of light rain falling on soil and grass, soft steady patter, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, mono, no reverb, no music | 16 | 0.3 | Y | 1 | Ambient |
| wx_rain_light_stone_loop | Perfectly seamless loop of light rain on stone and pavement, fine bright steady patter, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, mono, no reverb, no music | 16 | 0.3 | Y | 1 | Ambient |
| wx_rain_heavy_soil_loop | Perfectly seamless loop of heavy rain on soil and earth, dense drumming downpour, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, mono, no reverb, no music | 16 | 0.35 | Y | 1 | Ambient |
| wx_rain_heavy_foliage_loop | Perfectly seamless loop of heavy rain hammering a forest canopy, dense leafy roar, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, mono, no reverb, no music | 16 | 0.35 | Y | 1 | Ambient |
| wx_thunder_distant | A low distant rolling thunder rumble far away, dry, mono, no reverb, no music | 4 | 0.35 | N | 4 | Ambient |
| wx_thunder_near_crack | A close violent thunder crack and sharp boom rolling off, dry, mono, no reverb, no music | 4 | 0.45 | N | 3 | Ambient |
| wx_rain_onset_ramp | Rain beginning, the first scattered drops building into a steady patter, mono, no reverb, no music | 6 | 0.3 | N | 1 | Ambient |
| wx_rain_tailoff | Rain easing off, a steady patter thinning to scattered last drops, mono, no reverb, no music | 6 | 0.3 | N | 1 | Ambient |

---

## 7. Category 08 — Water (core)

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| water_swim_surface_loop | Perfectly seamless loop of a person swimming at the surface, steady rhythmic strokes and splashes, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 16 | 0.3 | Y | 1 | Ambient |
| water_swim_submerged_loop | Perfectly seamless loop of a body moving underwater, muffled low swishes and kicks, constant unchanging texture and level start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, mono, no reverb, no music | 16 | 0.3 | Y | 1 | Ambient |
| water_submerge_plunge | A body dropping underwater, a heavy plunging splash cutting to muffled, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 3 | Ambient |
| water_surface_gasp | A person breaking the water surface with a sharp gasp and water-shedding splash, dry close mono, no reverb, no music | 1.0 | 0.4 | N | 3 | Voice |
| water_underwater_ambient_loop | Perfectly seamless loop of a low muffled underwater ambience with faint bubble drift, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, mono, no reverb, no music | 20 | 0.22 | Y | 1 | Ambient |
| water_splash_small | A small light water splash, a foot or hand entering, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | Ambient |
| water_splash_medium | A medium water splash, a body-sized entry, dry close mono, no reverb, no music | 0.9 | 0.4 | N | 4 | Ambient |
| water_splash_large | A large heavy water splash and churn, a big mass hitting water, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 3 | Ambient |
| water_drip_single | A single isolated water drip falling and plopping, dry close mono, no reverb, no music | 0.5 | 0.35 | N | 5 | Ambient |

---

## 7b. Phase 2 (started) — Category 04: Voxel / Terrain Editing

The core destructible-terrain verbs — the most-heard sounds in a voxel game.
**Scoped to the 4 wired materials** (sand, dirt, grass, stone) + bedrock,
wrong-tool, dig loops, gravity collapse, explosives, and build-mode cues.
Axe/wood and the ore tiers are deferred to a later sub-phase (not wired yet).
~27 entries / ~133 generations; interim-model estimate well under budget —
**render with `--credit-cap 12000`** as the hard safety (your monthly
remainder is tight). The first run also finally ground-truths the per-gen
floor: note your credit balance before/after and tell me.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| vox_shovel_strike_dirt | A single shovel blade biting into packed dirt, a soft earthy chop with grit and crumble, dry close mono, no reverb, no music | 0.4 | 0.45 | N | 4 | SFX |
| vox_shovel_break_dirt | A clod of dirt breaking loose and crumbling away as a block is removed, soft earthy collapse, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 4 | SFX |
| vox_place_dirt | A block of dirt set and tamped into place, a soft compact thud, dry close mono, no reverb, no music | 0.4 | 0.45 | N | 3 | SFX |
| vox_shovel_strike_sand | A single shovel scoop into dry sand, a soft granular shove and hiss, dry close mono, no reverb, no music | 0.4 | 0.4 | N | 4 | SFX |
| vox_shovel_break_sand | Sand giving way and pouring as a block is removed, a soft granular collapse, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 4 | SFX |
| vox_place_sand | A block of sand dropped into place, a soft heavy granular thud, dry close mono, no reverb, no music | 0.4 | 0.4 | N | 3 | SFX |
| vox_shovel_strike_grass | A shovel cutting through grass turf and root into soil, a tearing crunch and earthy chop, dry close mono, no reverb, no music | 0.4 | 0.45 | N | 4 | SFX |
| vox_shovel_break_grass | A clump of grassy turf ripped free and crumbling, root tear and soil patter, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 4 | SFX |
| vox_place_grass | A turf block set down, a soft muffled grassy thud, dry close mono, no reverb, no music | 0.4 | 0.45 | N | 3 | SFX |
| vox_pick_strike_stone | A single pickaxe striking solid stone, a sharp hard ringing chip with grit spray, dry close mono, no reverb, no music | 0.4 | 0.55 | N | 4 | SFX |
| vox_pick_break_stone | A stone block shattering apart under a pick, a hard crack and rubble fall, dry close mono, no reverb, no music | 0.5 | 0.55 | N | 4 | SFX |
| vox_place_stone | A heavy stone block set into place, a solid grinding thunk, dry close mono, no reverb, no music | 0.4 | 0.5 | N | 3 | SFX |
| vox_bedrock_blocked | A tool striking unbreakable bedrock with no progress, a dead hard dull clank that does not yield, dry close mono, no reverb, no music | 0.4 | 0.55 | N | 3 | SFX |
| vox_wrongtool_soft | A wrong tool scraping ineffectively at soil, a dull glancing scuff with little effect, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 3 | SFX |
| vox_wrongtool_stone | A wrong tool glancing off stone ineffectively, a dull flat scrape with no bite, dry close mono, no reverb, no music | 0.5 | 0.5 | N | 3 | SFX |
| vox_dig_loop_soft | Perfectly seamless loop of continuous shovel digging in soft soil, steady rhythmic earthy scoops, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.3 | Y | 1 | SFX |
| vox_dig_loop_hard | Perfectly seamless loop of continuous pickaxe mining stone, steady rhythmic hard chipping, constant unchanging texture and level from start to end, no onset transient, no attack, no fade, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.3 | Y | 1 | SFX |
| vox_cluster_collapse | A mass of unsupported voxels giving way and collapsing, a heavy rumbling tumble of earth and stone, dry close mono, no reverb, no music | 1.5 | 0.5 | N | 3 | SFX |
| vox_cluster_impact_ground | A collapsed chunk of terrain crashing down onto the ground, a heavy debris impact and settle, dry close mono, no reverb, no music | 0.9 | 0.5 | N | 4 | SFX |
| vox_cluster_impact_water | A collapsed chunk of terrain crashing into water, a heavy splash and churn, dry close mono, no reverb, no music | 1.0 | 0.45 | N | 3 | SFX |
| vox_powdercharge_fuse | A powder charge fuse sizzling and burning down, a tense sputtering hiss, dry close mono, no reverb, no music | 2.0 | 0.4 | N | 2 | SFX |
| vox_powdercharge_blast | A powder charge detonating in stone, a hard concussive blast with a rubble burst, dry close mono, no reverb, no music | 1.5 | 0.55 | N | 3 | SFX |
| vox_sapper_blast_heavy | A heavy sapper charge detonating, a huge deep concussive explosion shattering timber and stone, dry close mono, no reverb, no music | 2.0 | 0.55 | N | 3 | SFX |
| vox_buildmode_ghost_appear | A soft brief tonal shimmer as a placement ghost block appears, subtle, not electronic, dry close mono, no reverb, no music | 0.4 | 0.4 | N | 1 | SFX |
| vox_buildmode_snap_click | A short crisp snap as a placed block locks to the grid, a clean physical click, dry close mono, no reverb, no music | 0.3 | 0.55 | N | 2 | SFX |
| vox_buildmode_reject | A low dull refusal thud when a block placement is blocked, physical not a beep, dry close mono, no reverb, no music | 0.4 | 0.5 | N | 2 | SFX |
| vox_carve_volume_cycle | A very short subtle tick switching the carve volume size, minimal, not electronic, dry close mono, no reverb, no music | 0.3 | 0.5 | N | 1 | UI |

vox_* files route to `assets/audio/sfx/voxel/` (per `SFX_LIBRARY.md §2`).
Render: `python3 tools/render_sfx.py --category 04 --credit-cap 12000`.

---

<!-- BEGIN GENERATED PROMPTS -- edit tools/sfx_families/, not this block -->

## 7c. The rest of the library, generated

Every remaining id in `docs/sfx-library.md`, one row each, so the two documents
match 1:1. `tools/lint-sfx-coverage.py` proves it and fails if either side
drifts.

**These rows are generated from `tools/sfx_families/*.py`. Edit those, not
this block** -- anything typed between the markers is overwritten on the next
`python tools/gen-sfx-prompts.py`. The hand-authored Phase 1 and 2 sections
above are outside the markers and are never touched, because 108 of their ids
have audio rendered from that exact text.

Read section 8 before rendering any of this. The lesson already paid for is
that the 548 takes on disk sound rough because nothing was pruned, not because
there were too few of them -- so curate as you go rather than generating the
whole set and sorting it out later.

### Category 01 -- Locomotion (remaining surfaces, armour, traversal, breath)

39 ids, 171 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| step_walk_gravel | A single footstep on loose gravel, sharp scattering of small stones crunching underfoot, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_walk_snow | A single footstep into fresh snow, tight high squeak and soft powder compression, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_walk_mud | A single footstep into thick mud, wet sucking squelch as the boot pulls free, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| step_walk_marsh | A single footstep into boggy marsh ground, waterlogged squelch through reeds and peat, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| step_walk_metal | A single footstep on a metal grating, hard ringing clank with a faint metallic sustain, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_walk_cave_stone | A single footstep on damp cave stone, hard contact with a gritty scrape and a touch of stone dust, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_run_gravel | A single fast running footstep on loose gravel, harder impact, sharp scattering of small stones crunching underfoot, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_run_snow | A single fast running footstep into fresh snow, harder impact, tight high squeak and soft powder compression, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_run_mud | A single fast running footstep into thick mud, harder impact, wet sucking squelch as the boot pulls free, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| step_run_marsh | A single fast running footstep into boggy marsh ground, harder impact, waterlogged squelch through reeds and peat, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| step_run_metal | A single fast running footstep on a metal grating, harder impact, hard ringing clank with a faint metallic sustain, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_run_cave_stone | A single fast running footstep on damp cave stone, harder impact, hard contact with a gritty scrape and a touch of stone dust, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_sprint_gravel | A single hard sprinting footstep on loose gravel, heavy fast impact, sharp scattering of small stones crunching underfoot, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_sprint_snow | A single hard sprinting footstep into fresh snow, heavy fast impact, tight high squeak and soft powder compression, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| step_sprint_mud | A single hard sprinting footstep into thick mud, heavy fast impact, wet sucking squelch as the boot pulls free, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| step_sprint_marsh | A single hard sprinting footstep into boggy marsh ground, heavy fast impact, waterlogged squelch through reeds and peat, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| step_sprint_metal | A single hard sprinting footstep on a metal grating, heavy fast impact, hard ringing clank with a faint metallic sustain, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_sprint_cave_stone | A single hard sprinting footstep on damp cave stone, heavy fast impact, hard contact with a gritty scrape and a touch of stone dust, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 5 | SFX |
| step_crouch_gravel | A single very soft slow crouched footstep on loose gravel, careful muffled press, faint, sharp scattering of small stones crunching underfoot, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 5 | SFX |
| step_crouch_snow | A single very soft slow crouched footstep into fresh snow, careful muffled press, faint, tight high squeak and soft powder compression, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| step_crouch_mud | A single very soft slow crouched footstep into thick mud, careful muffled press, faint, wet sucking squelch as the boot pulls free, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 5 | SFX |
| step_crouch_marsh | A single very soft slow crouched footstep into boggy marsh ground, careful muffled press, faint, waterlogged squelch through reeds and peat, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 5 | SFX |
| step_crouch_metal | A single very soft slow crouched footstep on a metal grating, careful muffled press, faint, hard ringing clank with a faint metallic sustain, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 5 | SFX |
| step_crouch_cave_stone | A single very soft slow crouched footstep on damp cave stone, careful muffled press, faint, hard contact with a gritty scrape and a touch of stone dust, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 5 | SFX |
| jumpland_gravel | A person landing hard from a jump on loose gravel, a single heavy two-foot impact, sharp scattering of small stones crunching underfoot, knees absorbing, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 5 | SFX |
| jumpland_snow | A person landing hard from a jump into fresh snow, a single heavy two-foot impact, tight high squeak and soft powder compression, knees absorbing, dry close mono, no reverb, no music | 0.8 | 0.4 | N | 5 | SFX |
| jumpland_mud | A person landing hard from a jump into thick mud, a single heavy two-foot impact, wet sucking squelch as the boot pulls free, knees absorbing, dry close mono, no reverb, no music | 0.8 | 0.4 | N | 5 | SFX |
| jumpland_marsh | A person landing hard from a jump into boggy marsh ground, a single heavy two-foot impact, waterlogged squelch through reeds and peat, knees absorbing, dry close mono, no reverb, no music | 0.8 | 0.4 | N | 5 | SFX |
| jumpland_metal | A person landing hard from a jump on a metal grating, a single heavy two-foot impact, hard ringing clank with a faint metallic sustain, knees absorbing, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 5 | SFX |
| jumpland_cave_stone | A person landing hard from a jump on damp cave stone, a single heavy two-foot impact, hard contact with a gritty scrape and a touch of stone dust, knees absorbing, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 5 | SFX |
| armor_cloth_run_clank | A burst of soft linen and wool clothing shifting and rustling at a hard run, louder and more agitated, a few seconds of stressed movement, dry close mono, no reverb, no music | 1.2 | 0.3 | N | 3 | SFX |
| armor_leather_run_clank | A burst of supple leather armour creaking and straps flexing at a hard run, louder and more agitated, a few seconds of stressed movement, dry close mono, no reverb, no music | 1.2 | 0.32 | N | 3 | SFX |
| armor_mail_run_clank | A burst of a mail hauberk shifting, thousands of small iron rings chiming and sliding against each other at a hard run, louder and more agitated, a few seconds of stressed movement, dry close mono, no reverb, no music | 1.2 | 0.35 | N | 3 | SFX |
| armor_plate_run_clank | A burst of steel plate armour moving, heavy plates knocking, leather straps creaking and mail voiders sliding underneath at a hard run, louder and more agitated, a few seconds of stressed movement, dry close mono, no reverb, no music | 1.2 | 0.38 | N | 3 | SFX |
| climb_wood_loop | Perfectly seamless loop of a person climbing a wooden structure, hands slapping and gripping timber, boots scuffing planks, faint creaking, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.35 | Y | 1 | SFX |
| ladder_step_wood_loop | Perfectly seamless loop of a person climbing a wooden ladder, rhythmic boot contacts on rungs with faint timber creak, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.35 | Y | 1 | SFX |
| ladder_step_iron_loop | Perfectly seamless loop of a person climbing an iron ladder, rhythmic boot contacts ringing on cold metal rungs, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.38 | Y | 1 | SFX |
| slide_scree | A person sliding down loose scree, a rush of cascading small stones and skidding boots, dry close mono, no reverb, no music | 1.8 | 0.4 | N | 3 | SFX |
| water_entry_fall_deep | A body falling from height into deep water, a heavy plunging impact cutting to muffled underwater, dry close mono, no reverb, no music | 2 | 0.4 | N | 3 | Ambient |

---

### Category 02 -- Combat: Player (universal, weapon matrix, throwables)

83 ids, 275 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| cmb_sword_swing_light | A fast sharp sword swing through the air, a bright edge whoosh, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | Combat |
| cmb_sword_swing_combo | A chained set of three fast sword swings, three bright edge whooshes in quick succession with a final harder cut, dry close mono, no reverb, no music | 1.6 | 0.4 | N | 4 | Combat |
| cmb_sword_windup_power | A fighter winding up a heavy sword blow, a drawn breath, cloth and leather shifting, the blade rising, no impact, dry close mono, no reverb, no music | 1 | 0.35 | N | 3 | Combat |
| cmb_sword_land_power | A heavy block-breaking sword impact, a deep steel crunch with real weight behind it, dry close mono, no reverb, no music | 0.9 | 0.45 | N | 4 | Combat |
| cmb_power_abort | A charged sword swing released without firing, the blade lowering, cloth settling and a short frustrated exhale, dry close mono, no reverb, no music | 0.8 | 0.35 | N | 2 | Combat |
| cmb_swing_miss_air | A weapon swung hard through empty air, a deep air-displacement whoosh with a breath of effort, no impact, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 5 | Combat |
| cmb_weapon_draw | A sword drawn from a leather scabbard, a fast metallic slither ending in a clear ring, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 3 | Combat |
| cmb_weapon_sheathe | A sword slid back into a leather scabbard, a controlled metallic slide ending in a soft seat, dry close mono, no reverb, no music | 0.9 | 0.45 | N | 3 | Combat |
| cmb_dagger_swing_light | A dagger swung fast in a light attack, light and fast, a thin quick hiss of a short blade, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 5 | Combat |
| cmb_dagger_swing_heavy | A dagger swung hard in a heavy attack, slower and far stronger, light and fast, a thin quick hiss of a short blade, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 4 | Combat |
| cmb_dagger_swing_miss_air | A dagger swung through empty air and missing entirely, air displacement and effort, no impact, light and fast, a thin quick hiss of a short blade, dry close mono, no reverb, no music | 0.7 | 0.42 | N | 4 | Combat |
| cmb_dagger_draw | A dagger drawn ready to fight, metal and leather, light and fast, a thin quick hiss of a short blade, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 3 | Combat |
| cmb_dagger_sheathe | A dagger put away, metal and leather settling, light and fast, a thin quick hiss of a short blade, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 3 | Combat |
| cmb_dagger_parry | A dagger catching an incoming blow and turning it aside, a bright ringing slide of steel, light and fast, a thin quick hiss of a short blade, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 4 | Combat |
| cmb_dagger_special | A dagger performing its signature move, the single most distinctive sound this weapon makes, light and fast, a thin quick hiss of a short blade, dry close mono, no reverb, no music | 1 | 0.42 | N | 3 | Combat |
| cmb_dagger_block_hold_loop | Perfectly seamless loop of a dagger turned edge-on against pressure, a thin strained scrape of a short blade barely holding, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.42 | Y | 1 | Combat |
| cmb_shortsword_swing_light | A shortsword swung fast in a light attack, quick and clean, a compact bright edge, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 5 | Combat |
| cmb_shortsword_swing_heavy | A shortsword swung hard in a heavy attack, slower and far stronger, quick and clean, a compact bright edge, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 4 | Combat |
| cmb_shortsword_swing_miss_air | A shortsword swung through empty air and missing entirely, air displacement and effort, no impact, quick and clean, a compact bright edge, dry close mono, no reverb, no music | 0.7 | 0.42 | N | 4 | Combat |
| cmb_shortsword_draw | A shortsword drawn ready to fight, metal and leather, quick and clean, a compact bright edge, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 3 | Combat |
| cmb_shortsword_sheathe | A shortsword put away, metal and leather settling, quick and clean, a compact bright edge, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 3 | Combat |
| cmb_shortsword_parry | A shortsword catching an incoming blow and turning it aside, a bright ringing slide of steel, quick and clean, a compact bright edge, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 4 | Combat |
| cmb_shortsword_special | A shortsword performing its signature move, the single most distinctive sound this weapon makes, quick and clean, a compact bright edge, dry close mono, no reverb, no music | 1 | 0.42 | N | 3 | Combat |
| cmb_shortsword_block_hold_loop | Perfectly seamless loop of a shortsword held against pressure, a compact steel grind under load, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.42 | Y | 1 | Combat |
| cmb_twohander_swing_light | A greatsword swung fast in a light attack, huge and slow, a deep heavy sweep of a two-handed blade, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | Combat |
| cmb_twohander_swing_heavy | A greatsword swung hard in a heavy attack, slower and far stronger, huge and slow, a deep heavy sweep of a two-handed blade, dry close mono, no reverb, no music | 0.9 | 0.4 | N | 4 | Combat |
| cmb_twohander_swing_miss_air | A greatsword swung through empty air and missing entirely, air displacement and effort, no impact, huge and slow, a deep heavy sweep of a two-handed blade, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 4 | Combat |
| cmb_twohander_draw | A greatsword drawn ready to fight, metal and leather, huge and slow, a deep heavy sweep of a two-handed blade, dry close mono, no reverb, no music | 0.8 | 0.4 | N | 3 | Combat |
| cmb_twohander_sheathe | A greatsword put away, metal and leather settling, huge and slow, a deep heavy sweep of a two-handed blade, dry close mono, no reverb, no music | 0.9 | 0.4 | N | 3 | Combat |
| cmb_twohander_parry | A greatsword catching an incoming blow and turning it aside, a bright ringing slide of steel, huge and slow, a deep heavy sweep of a two-handed blade, dry close mono, no reverb, no music | 0.8 | 0.4 | N | 4 | Combat |
| cmb_twohander_special | A greatsword performing its signature move, the single most distinctive sound this weapon makes, huge and slow, a deep heavy sweep of a two-handed blade, dry close mono, no reverb, no music | 1 | 0.4 | N | 3 | Combat |
| cmb_twohander_block_hold_loop | Perfectly seamless loop of a greatsword braced against pressure, a deep slow grind of heavy steel, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.4 | Y | 1 | Combat |
| cmb_waraxe_swing_light | A war axe swung fast in a light attack, top-heavy and brutal, a blunt-backed head chopping through, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 5 | Combat |
| cmb_waraxe_swing_heavy | A war axe swung hard in a heavy attack, slower and far stronger, top-heavy and brutal, a blunt-backed head chopping through, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 4 | Combat |
| cmb_waraxe_swing_miss_air | A war axe swung through empty air and missing entirely, air displacement and effort, no impact, top-heavy and brutal, a blunt-backed head chopping through, dry close mono, no reverb, no music | 0.7 | 0.42 | N | 4 | Combat |
| cmb_waraxe_draw | A war axe drawn ready to fight, metal and leather, top-heavy and brutal, a blunt-backed head chopping through, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 3 | Combat |
| cmb_waraxe_sheathe | A war axe put away, metal and leather settling, top-heavy and brutal, a blunt-backed head chopping through, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 3 | Combat |
| cmb_waraxe_parry | A war axe catching an incoming blow and turning it aside, a bright ringing slide of steel, top-heavy and brutal, a blunt-backed head chopping through, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 4 | Combat |
| cmb_waraxe_special | A war axe performing its signature move, the single most distinctive sound this weapon makes, top-heavy and brutal, a blunt-backed head chopping through, dry close mono, no reverb, no music | 1 | 0.42 | N | 3 | Combat |
| cmb_waraxe_block_hold_loop | Perfectly seamless loop of an axe haft braced against pressure, wood creaking with steel grinding above it, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.42 | Y | 1 | Combat |
| cmb_mace_swing_light | A mace swung fast in a light attack, blunt and heavy, a solid steel head with no edge at all, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 5 | Combat |
| cmb_mace_swing_heavy | A mace swung hard in a heavy attack, slower and far stronger, blunt and heavy, a solid steel head with no edge at all, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 4 | Combat |
| cmb_mace_swing_miss_air | A mace swung through empty air and missing entirely, air displacement and effort, no impact, blunt and heavy, a solid steel head with no edge at all, dry close mono, no reverb, no music | 0.7 | 0.42 | N | 4 | Combat |
| cmb_mace_draw | A mace drawn ready to fight, metal and leather, blunt and heavy, a solid steel head with no edge at all, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 3 | Combat |
| cmb_mace_sheathe | A mace put away, metal and leather settling, blunt and heavy, a solid steel head with no edge at all, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 3 | Combat |
| cmb_mace_parry | A mace catching an incoming blow and turning it aside, a bright ringing slide of steel, blunt and heavy, a solid steel head with no edge at all, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 4 | Combat |
| cmb_mace_special | A mace performing its signature move, the single most distinctive sound this weapon makes, blunt and heavy, a solid steel head with no edge at all, dry close mono, no reverb, no music | 1 | 0.42 | N | 3 | Combat |
| cmb_mace_block_hold_loop | Perfectly seamless loop of a mace haft braced against pressure, a dull metallic press with no ring, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.42 | Y | 1 | Combat |
| cmb_flail_swing_light | A flail swung fast in a light attack, a chained head whirling, links rattling before the strike, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | Combat |
| cmb_flail_swing_heavy | A flail swung hard in a heavy attack, slower and far stronger, a chained head whirling, links rattling before the strike, dry close mono, no reverb, no music | 0.9 | 0.4 | N | 4 | Combat |
| cmb_flail_swing_miss_air | A flail swung through empty air and missing entirely, air displacement and effort, no impact, a chained head whirling, links rattling before the strike, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 4 | Combat |
| cmb_flail_draw | A flail drawn ready to fight, metal and leather, a chained head whirling, links rattling before the strike, dry close mono, no reverb, no music | 0.8 | 0.4 | N | 3 | Combat |
| cmb_flail_sheathe | A flail put away, metal and leather settling, a chained head whirling, links rattling before the strike, dry close mono, no reverb, no music | 0.9 | 0.4 | N | 3 | Combat |
| cmb_flail_parry | A flail catching an incoming blow and turning it aside, a bright ringing slide of steel, a chained head whirling, links rattling before the strike, dry close mono, no reverb, no music | 0.8 | 0.4 | N | 4 | Combat |
| cmb_flail_special | A flail performing its signature move, the single most distinctive sound this weapon makes, a chained head whirling, links rattling before the strike, dry close mono, no reverb, no music | 1 | 0.4 | N | 3 | Combat |
| cmb_flail_block_hold_loop | Perfectly seamless loop of a flail haft braced against pressure, the chain and head swinging and knocking under load, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.4 | Y | 1 | Combat |
| cmb_spear_swing_light | A spear swung fast in a light attack, a long shaft thrusting, a lean piercing whoosh, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 5 | Combat |
| cmb_spear_swing_heavy | A spear swung hard in a heavy attack, slower and far stronger, a long shaft thrusting, a lean piercing whoosh, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 4 | Combat |
| cmb_spear_swing_miss_air | A spear swung through empty air and missing entirely, air displacement and effort, no impact, a long shaft thrusting, a lean piercing whoosh, dry close mono, no reverb, no music | 0.7 | 0.42 | N | 4 | Combat |
| cmb_spear_draw | A spear drawn ready to fight, metal and leather, a long shaft thrusting, a lean piercing whoosh, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 3 | Combat |
| cmb_spear_sheathe | A spear put away, metal and leather settling, a long shaft thrusting, a lean piercing whoosh, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 3 | Combat |
| cmb_spear_parry | A spear catching an incoming blow and turning it aside, a bright ringing slide of steel, a long shaft thrusting, a lean piercing whoosh, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 4 | Combat |
| cmb_spear_special | A spear performing its signature move, the single most distinctive sound this weapon makes, a long shaft thrusting, a lean piercing whoosh, dry close mono, no reverb, no music | 1 | 0.42 | N | 3 | Combat |
| cmb_spear_block_hold_loop | Perfectly seamless loop of a spear shaft braced crosswise against pressure, wood straining and creaking under weight, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.42 | Y | 1 | Combat |
| cmb_shield_raise | A shield brought up into guard, a fast shift of wood and iron rim with a leather strap creak, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 2 | Combat |
| cmb_shield_lower | A shield lowered out of guard, wood and iron settling against the body, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 2 | Combat |
| cmb_shield_block_absorb | A heavy blow absorbed on a wooden shield, a deep thud into planking with an iron rim ring, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 5 | Combat |
| cmb_shield_bash | A shield driven forward as a weapon, a blunt wooden slam with an iron rim crack, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_bow_nock | An arrow nocked to a bowstring, a small wooden click and a faint string touch, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 3 | Combat |
| cmb_bow_draw_creak | A heavy bow drawn back, wood and horn creaking under increasing tension, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 3 | Combat |
| cmb_bow_release | A bowstring released, a sharp snapping thrum with the arrow leaving, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 4 | Combat |
| cmb_bow_arrow_whir | An arrow in flight passing close by, a fast fletching whir dopplering past, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 3 | Combat |
| cmb_bow_dryfire | A bow released with no arrow nocked, a hard hollow slap of string on limb, wrong and jarring, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 1 | Combat |
| cmb_bomb_pitch_detonate | A small powder bomb detonating, a sharp cracking blast with debris scattering after, dry close mono, no reverb, no music | 1.4 | 0.45 | N | 3 | Combat |
| cmb_oil_splash | A clay flask of oil shattering and splashing across the ground, glass break and thick liquid spread, dry close mono, no reverb, no music | 1 | 0.42 | N | 3 | Combat |
| cmb_smoke_hiss | A smoke pot igniting, a sustained pressurised hiss building then settling, dry close mono, no reverb, no music | 2 | 0.38 | N | 3 | Combat |
| cmb_caltrop_scatter | A handful of iron caltrops thrown across stone, sharp scattering metallic tinks settling, dry close mono, no reverb, no music | 1.2 | 0.45 | N | 3 | Combat |
| cmb_flash_pop | A flash charge going off, a bright hard crack with a short ringing tail, dry close mono, no reverb, no music | 1 | 0.45 | N | 3 | Combat |
| cmb_ashbane_torch_throw | A burning torch thrown through the air and landing, flame roar dopplering past then guttering on the ground, dry close mono, no reverb, no music | 1.6 | 0.4 | N | 3 | Combat |
| cmb_venomtip_dart | A small dart thrown, a thin fast hiss ending in a light sharp stick, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 3 | Combat |
| cmb_trap_arm | A jaw trap being set, ratcheting metal under tension locking into place, dry close mono, no reverb, no music | 1.2 | 0.45 | N | 3 | Combat |
| cmb_trap_snap | A jaw trap snapping shut, a violent metallic clash of springs and teeth, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 3 | Combat |
| cmb_tripwire_trigger | A tripwire pulled taut and releasing, a thin wire twang and a mechanism letting go, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 3 | Combat |

---

### Category 03 -- Combat: Impacts & Enemies

21 ids, 84 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| cmb_hit_blunt_flesh_unarmored | A combat hit, a heavy blunt mass crushing into bare flesh, a wet meaty impact, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_blunt_flesh_padded | A combat hit, a heavy blunt mass crushing into a padded gambeson over flesh, a dulled thudding impact through thick cloth, dry close mono, no reverb, no music | 0.7 | 0.42 | N | 4 | Combat |
| cmb_hit_blunt_mail | A combat hit, a heavy blunt mass crushing into a mail hauberk, iron rings bursting and chiming with the shock carrying through, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_blunt_plate | A combat hit, a heavy blunt mass crushing into steel plate armour, a hard ringing deflection off shaped steel, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 4 | Combat |
| cmb_hit_blunt_shield | A combat hit, a heavy blunt mass crushing into a wooden shield, a deep thud into planking with an iron rim ring, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_blunt_wood | A combat hit, a heavy blunt mass crushing into a wooden beam, a splitting bite into timber, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_blunt_stone_terrain | A combat hit, a heavy blunt mass crushing into bare stone, a sharp skidding clank with sparks and stone chips, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 4 | Combat |
| cmb_hit_serrated_flesh_unarmored | A combat hit, a jagged serrated edge tearing through bare flesh, a wet meaty impact, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_serrated_flesh_padded | A combat hit, a jagged serrated edge tearing through a padded gambeson over flesh, a dulled thudding impact through thick cloth, dry close mono, no reverb, no music | 0.7 | 0.42 | N | 4 | Combat |
| cmb_hit_serrated_mail | A combat hit, a jagged serrated edge tearing through a mail hauberk, iron rings bursting and chiming with the shock carrying through, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_serrated_plate | A combat hit, a jagged serrated edge tearing through steel plate armour, a hard ringing deflection off shaped steel, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 4 | Combat |
| cmb_hit_serrated_shield | A combat hit, a jagged serrated edge tearing through a wooden shield, a deep thud into planking with an iron rim ring, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_serrated_wood | A combat hit, a jagged serrated edge tearing through a wooden beam, a splitting bite into timber, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_serrated_stone_terrain | A combat hit, a jagged serrated edge tearing through bare stone, a sharp skidding clank with sparks and stone chips, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 4 | Combat |
| cmb_hit_arrow_flesh_unarmored | A combat hit, an arrow striking and burying into bare flesh, a wet meaty impact, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_arrow_flesh_padded | A combat hit, an arrow striking and burying into a padded gambeson over flesh, a dulled thudding impact through thick cloth, dry close mono, no reverb, no music | 0.7 | 0.42 | N | 4 | Combat |
| cmb_hit_arrow_mail | A combat hit, an arrow striking and burying into a mail hauberk, iron rings bursting and chiming with the shock carrying through, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_arrow_plate | A combat hit, an arrow striking and burying into steel plate armour, a hard ringing deflection off shaped steel, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 4 | Combat |
| cmb_hit_arrow_shield | A combat hit, an arrow striking and burying into a wooden shield, a deep thud into planking with an iron rim ring, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_arrow_wood | A combat hit, an arrow striking and burying into a wooden beam, a splitting bite into timber, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | Combat |
| cmb_hit_arrow_stone_terrain | A combat hit, an arrow striking and burying into bare stone, a sharp skidding clank with sparks and stone chips, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 4 | Combat |

---

### Category 04 -- Tools & Voxel Interaction (remaining materials)

66 ids, 236 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| vox_wrongtool_grass | The wrong tool scraping uselessly against grass turf, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 3 | SFX |
| vox_wrongtool_dirt | The wrong tool scraping uselessly against packed dirt, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 3 | SFX |
| vox_wrongtool_sand | The wrong tool scraping uselessly against dry sand, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.38 | N | 3 | SFX |
| vox_clay_strike | A tool striking dense wet clay mid-dig, a heavy sucking cut, thick and sticky, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| vox_clay_break | A block of dense wet clay finally giving way and breaking apart, a heavy sucking cut, thick and sticky then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.4 | N | 4 | SFX |
| vox_clay_place | A block of dense wet clay set into place, a heavy sucking cut, thick and sticky, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 3 | SFX |
| vox_wrongtool_clay | The wrong tool scraping uselessly against dense wet clay, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 3 | SFX |
| vox_mud_strike | A tool striking thick mud mid-dig, a wet sucking slop with water squeezing out, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 5 | SFX |
| vox_mud_break | A block of thick mud finally giving way and breaking apart, a wet sucking slop with water squeezing out then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.4 | N | 4 | SFX |
| vox_mud_place | A block of thick mud set into place, a wet sucking slop with water squeezing out, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 3 | SFX |
| vox_wrongtool_mud | The wrong tool scraping uselessly against thick mud, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 3 | SFX |
| vox_ash_strike | A tool striking deep dry ash mid-dig, a soft powdery whump with fine dust rising, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.38 | N | 5 | SFX |
| vox_ash_break | A block of deep dry ash finally giving way and breaking apart, a soft powdery whump with fine dust rising then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.38 | N | 4 | SFX |
| vox_ash_place | A block of deep dry ash set into place, a soft powdery whump with fine dust rising, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.38 | N | 3 | SFX |
| vox_wrongtool_ash | The wrong tool scraping uselessly against deep dry ash, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.38 | N | 3 | SFX |
| vox_gravel_strike | A tool striking loose gravel mid-dig, a harsh rattling scoop of small stones, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 5 | SFX |
| vox_gravel_break | A block of loose gravel finally giving way and breaking apart, a harsh rattling scoop of small stones then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.45 | N | 4 | SFX |
| vox_gravel_place | A block of loose gravel set into place, a harsh rattling scoop of small stones, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 3 | SFX |
| vox_wrongtool_gravel | The wrong tool scraping uselessly against loose gravel, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 3 | SFX |
| vox_worked_stone_strike | A tool striking dressed worked stone mid-dig, a clean hard strike on a cut block, sharp and precise, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.48 | N | 5 | SFX |
| vox_worked_stone_break | A block of dressed worked stone finally giving way and breaking apart, a clean hard strike on a cut block, sharp and precise then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.48 | N | 4 | SFX |
| vox_worked_stone_place | A block of dressed worked stone set into place, a clean hard strike on a cut block, sharp and precise, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 3 | SFX |
| vox_wrongtool_worked_stone | The wrong tool scraping uselessly against dressed worked stone, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 3 | SFX |
| vox_sandstone_strike | A tool striking soft sandstone mid-dig, a gritty crumbling strike, softer than granite, sand shedding, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 5 | SFX |
| vox_sandstone_break | A block of soft sandstone finally giving way and breaking apart, a gritty crumbling strike, softer than granite, sand shedding then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.45 | N | 4 | SFX |
| vox_sandstone_place | A block of soft sandstone set into place, a gritty crumbling strike, softer than granite, sand shedding, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 3 | SFX |
| vox_wrongtool_sandstone | The wrong tool scraping uselessly against soft sandstone, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 3 | SFX |
| vox_iron_ore_strike | A tool striking iron ore in rock mid-dig, a dense metallic-edged strike, harder and duller than plain stone, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.48 | N | 5 | SFX |
| vox_iron_ore_break | A block of iron ore in rock finally giving way and breaking apart, a dense metallic-edged strike, harder and duller than plain stone then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.48 | N | 4 | SFX |
| vox_iron_ore_place | A block of iron ore in rock set into place, a dense metallic-edged strike, harder and duller than plain stone, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 3 | SFX |
| vox_wrongtool_iron_ore | The wrong tool scraping uselessly against iron ore in rock, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 3 | SFX |
| vox_steel_ore_strike | A tool striking hard steel-bearing ore mid-dig, a very hard bright strike with a metallic ring under it, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.48 | N | 5 | SFX |
| vox_steel_ore_break | A block of hard steel-bearing ore finally giving way and breaking apart, a very hard bright strike with a metallic ring under it then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.48 | N | 4 | SFX |
| vox_steel_ore_place | A block of hard steel-bearing ore set into place, a very hard bright strike with a metallic ring under it, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 3 | SFX |
| vox_wrongtool_steel_ore | The wrong tool scraping uselessly against hard steel-bearing ore, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 3 | SFX |
| vox_adamant_ore_strike | A tool striking adamant ore mid-dig, an extremely hard strike that barely bites, a high crystalline ring and shock back up the haft, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.5 | N | 5 | SFX |
| vox_adamant_ore_break | A block of adamant ore finally giving way and breaking apart, an extremely hard strike that barely bites, a high crystalline ring and shock back up the haft then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.5 | N | 4 | SFX |
| vox_adamant_ore_place | A block of adamant ore set into place, an extremely hard strike that barely bites, a high crystalline ring and shock back up the haft, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.5 | N | 3 | SFX |
| vox_wrongtool_adamant_ore | The wrong tool scraping uselessly against adamant ore, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.5 | N | 3 | SFX |
| vox_coal_strike | A tool striking a coal seam mid-dig, a brittle crumbling crack with sooty fragments falling, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 5 | SFX |
| vox_coal_break | A block of a coal seam finally giving way and breaking apart, a brittle crumbling crack with sooty fragments falling then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.45 | N | 4 | SFX |
| vox_coal_place | A block of a coal seam set into place, a brittle crumbling crack with sooty fragments falling, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 3 | SFX |
| vox_wrongtool_coal | The wrong tool scraping uselessly against a coal seam, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 3 | SFX |
| vox_copper_ore_strike | A tool striking copper ore mid-dig, a softer metallic strike with a warm dull ring, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.46 | N | 5 | SFX |
| vox_copper_ore_break | A block of copper ore finally giving way and breaking apart, a softer metallic strike with a warm dull ring then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.46 | N | 4 | SFX |
| vox_copper_ore_place | A block of copper ore set into place, a softer metallic strike with a warm dull ring, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.46 | N | 3 | SFX |
| vox_wrongtool_copper_ore | The wrong tool scraping uselessly against copper ore, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.46 | N | 3 | SFX |
| vox_wood_strike | A tool striking a pine trunk mid-dig, a solid axe bite into softwood with a splitting crack, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 5 | SFX |
| vox_wood_break | A block of a pine trunk finally giving way and breaking apart, a solid axe bite into softwood with a splitting crack then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.45 | N | 4 | SFX |
| vox_wood_place | A block of a pine trunk set into place, a solid axe bite into softwood with a splitting crack, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 3 | SFX |
| vox_wrongtool_wood | The wrong tool scraping uselessly against a pine trunk, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 3 | SFX |
| vox_hardwood_strike | A tool striking a dense oak trunk mid-dig, a heavy axe bite into hardwood, tight and resistant, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.46 | N | 5 | SFX |
| vox_hardwood_break | A block of a dense oak trunk finally giving way and breaking apart, a heavy axe bite into hardwood, tight and resistant then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.46 | N | 4 | SFX |
| vox_hardwood_place | A block of a dense oak trunk set into place, a heavy axe bite into hardwood, tight and resistant, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.46 | N | 3 | SFX |
| vox_wrongtool_hardwood | The wrong tool scraping uselessly against a dense oak trunk, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.46 | N | 3 | SFX |
| vox_leaves_strike | A tool striking dense leaves and small branches mid-dig, a light tearing rustle of foliage being cut away, a single hit with the work continuing, dry close mono, no reverb, no music | 0.6 | 0.35 | N | 5 | SFX |
| vox_leaves_break | A block of dense leaves and small branches finally giving way and breaking apart, a light tearing rustle of foliage being cut away then the mass collapsing and falling away, dry close mono, no reverb, no music | 1 | 0.35 | N | 4 | SFX |
| vox_leaves_place | A block of dense leaves and small branches set into place, a light tearing rustle of foliage being cut away, a short settling thud as it seats, dry close mono, no reverb, no music | 0.7 | 0.35 | N | 3 | SFX |
| vox_wrongtool_leaves | The wrong tool scraping uselessly against dense leaves and small branches, a dull ineffective scrape with no progress and no break, dry close mono, no reverb, no music | 0.7 | 0.35 | N | 3 | SFX |
| vox_chop_tree_fell | The final axe cut that fells a tree, a deep splitting crack and the trunk beginning to give, dry close mono, no reverb, no music | 1.4 | 0.45 | N | 3 | SFX |
| vox_tree_topple_wood | A pine tree toppling and crashing down, a long tearing fall through branches ending in a heavy ground impact, dry close mono, no reverb, no music | 3.5 | 0.42 | N | 4 | SFX |
| vox_tree_topple_hardwood | A massive oak toppling and crashing down, a slower heavier fall with splitting timber ending in a ground-shaking impact, dry close mono, no reverb, no music | 4 | 0.42 | N | 4 | SFX |
| vox_log_resolve | A felled log settling into its final resting position, a short heavy roll and stop, dry close mono, no reverb, no music | 1 | 0.4 | N | 2 | SFX |
| vox_place_schematic_confirm | A built structure snapping into place complete, a solid satisfying settle of timber and stone, understated, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 1 | SFX |
| vox_place_reject_noeditzone | A placement refused, a short dull negative thunk with no ring, unmistakably a rejection, dry close mono, no reverb, no music | 0.4 | 0.48 | N | 1 | SFX |
| vox_gravity_creak_warn | Unsupported ground beginning to fail, a low ominous creak and grinding shift warning of collapse, no collapse yet, dry close mono, no reverb, no music | 1.8 | 0.4 | N | 2 | SFX |

---

### Category 05 -- Crafting & Stations

32 ids, 71 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| craft_forge_bellows_loop | Perfectly seamless loop of a smithing bellows being worked, a slow rhythmic push of air into a fire, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.35 | Y | 1 | SFX |
| craft_forge_fire_roar_loop | Perfectly seamless loop of a forge fire under forced air, a steady deep roar of burning charcoal, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.35 | Y | 1 | SFX |
| craft_forge_hammer_anvil_peak | A smith striking hot iron on an anvil at full force, a bright ringing hammer blow with a clean sustain, dry close mono, no reverb, no music | 1 | 0.48 | N | 5 | SFX |
| craft_forge_hammer_anvil_weak | A smith striking hot iron off-centre, a dull mistimed hammer blow with no ring, dry close mono, no reverb, no music | 0.8 | 0.48 | N | 4 | SFX |
| craft_forge_quench_hiss | Hot steel plunged into a quench trough, a violent burst of steam settling into a fading hiss, dry close mono, no reverb, no music | 2 | 0.42 | N | 3 | SFX |
| craft_forge_ingot_deform_thunk | A heated ingot deforming under the hammer, a dense dull thunk of metal moving, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 4 | SFX |
| craft_forge_reheat_whoosh | A workpiece pushed back into the forge fire, a soft whoosh of flame swallowing it, dry close mono, no reverb, no music | 1.2 | 0.38 | N | 2 | SFX |
| craft_forge_scale_sizzle | Hot scale flaking off steel and sizzling, small crackling pops, dry close mono, no reverb, no music | 1 | 0.4 | N | 2 | SFX |
| craft_forge_tier_up_resolve | A finished blade settling as it reaches a higher quality, a clean single ring of good steel, understated, no fanfare, dry close mono, no reverb, no music | 1.2 | 0.42 | N | 1 | SFX |
| craft_grindstone_wheel_loop | Perfectly seamless loop of a foot-treadle grindstone turning, a steady stone rumble with the treadle knocking, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.38 | Y | 1 | SFX |
| craft_grindstone_spark_burst | A blade pressed to a spinning grindstone, a harsh grinding shriek with sparks throwing, dry close mono, no reverb, no music | 1.2 | 0.45 | N | 4 | SFX |
| craft_grindstone_sharpen_complete | A blade lifted from the grindstone finished, the wheel running free and a single test ring off the edge, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 1 | SFX |
| craft_still_bubble_steep_loop | Perfectly seamless loop of a glass vessel of liquid steeping over low heat, slow irregular bubbling, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.35 | Y | 1 | SFX |
| craft_still_distill_drip_loop | Perfectly seamless loop of a distillation condenser dripping into a glass receiver, slow regular drips with a faint glass ring, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.35 | Y | 1 | SFX |
| craft_still_vial_cork_seal | A cork pressed into a glass vial, a tight squeaking push ending in a soft seated pop, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 3 | SFX |
| craft_still_foul_residue_hiss | A failed brew souring in the vessel, an ugly sputtering hiss with a thick bubble collapsing, dry close mono, no reverb, no music | 1.4 | 0.4 | N | 2 | SFX |
| craft_still_potion_complete | A finished potion settling in glass, a soft liquid swirl and one clean glass ring, understated, no fanfare, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 1 | SFX |
| craft_bench_saw_stroke | A hand saw drawn through a plank, one full rasping stroke with the blade flexing, dry close mono, no reverb, no music | 1.2 | 0.42 | N | 5 | SFX |
| craft_bench_plane_shave | A hand plane pushed along timber, a long clean shaving curl peeling away, dry close mono, no reverb, no music | 1.2 | 0.42 | N | 4 | SFX |
| craft_bench_wood_hammer | A wooden mallet driving a joint home, a dull solid knock of wood on wood, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 5 | SFX |
| craft_bench_schematic_complete | A wooden assembly seating together finished, a final firm knock and settle, understated, no fanfare, dry close mono, no reverb, no music | 1 | 0.42 | N | 1 | SFX |
| craft_cook_sizzle_loop | Perfectly seamless loop of food frying in a pan over a fire, a steady bright sizzle with occasional spits, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.35 | Y | 1 | SFX |
| craft_cook_boil_loop | Perfectly seamless loop of a pot boiling over a fire, steady rolling bubbles with a faint lid rattle, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.35 | Y | 1 | SFX |
| craft_cook_stir | A wooden spoon stirring a thick pot, a slow wet drag around the sides, dry close mono, no reverb, no music | 1.2 | 0.38 | N | 3 | SFX |
| craft_cook_meal_complete | A finished meal lifted off the fire, a pot set down and a spoon laid aside, understated, no fanfare, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 1 | SFX |
| craft_assembly_bind_wrap | Cord being wrapped tight around a haft, several fast turns pulling taut, dry close mono, no reverb, no music | 1.2 | 0.42 | N | 4 | SFX |
| craft_assembly_component_click | Two fitted components pressed together, a small precise mechanical click, dry close mono, no reverb, no music | 0.4 | 0.48 | N | 4 | SFX |
| craft_assembly_device_complete | A small device coming together finished, a final component seating with a quiet mechanical settle, understated, no fanfare, dry close mono, no reverb, no music | 1 | 0.45 | N | 1 | SFX |
| craft_quick_confirm | A quick craft finishing, one short dry practical sound of work set down, minimal, no fanfare, dry close mono, no reverb, no music | 0.5 | 0.42 | N | 1 | SFX |
| craft_care_confirm | A careful craft finishing, a slower deliberate settle of a finished piece being placed, minimal, no fanfare, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 1 | SFX |
| craft_mastery_confirm | A masterwork craft finishing, a single clean sustained ring of exceptional quality, still restrained, no fanfare, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 1 | SFX |
| craft_recipe_learned_chime | A recipe understood, one soft low tone with a quill stroke under it, very restrained, no fanfare, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 1 | SFX |

---

### Category 06 -- Interactive Objects & Items

49 ids, 134 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| item_pickup_generic | A small object picked up off the ground, a light handling rustle and shift, dry close mono, no reverb, no music | 0.5 | 0.4 | N | 5 | SFX |
| item_pickup_metal | A metal object picked up, a light metallic clink and slide into the hand, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 3 | SFX |
| item_pickup_cloth | A cloth item picked up, a soft fabric gather and lift, dry close mono, no reverb, no music | 0.5 | 0.35 | N | 3 | SFX |
| item_pickup_potion | A glass vial picked up, a small glass clink with liquid shifting inside, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 3 | SFX |
| item_pickup_voxeldrop | A dropped block picked up, a short dry granular scoop into the hand, dry close mono, no reverb, no music | 0.4 | 0.4 | N | 3 | SFX |
| item_drop | An object dropped onto the ground, a single dull landing thud and settle, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 4 | SFX |
| item_equip_weapon | A weapon taken to hand and set ready, leather and steel shifting into position, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 3 | SFX |
| item_equip_shield | A shield slung onto the arm, straps pulled and wood settling against the body, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 3 | SFX |
| item_equip_torch_offhand | A torch taken into the off hand, a wooden handle gripped with the flame guttering at the movement, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 2 | SFX |
| item_equip_head | A helmet pulled on, padding compressing and steel settling over the ears, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 3 | SFX |
| item_equip_body_cloth | A cloth garment pulled on over the head, fabric dragging and settling, dry close mono, no reverb, no music | 1.2 | 0.35 | N | 3 | SFX |
| item_equip_body_mail | A mail hauberk pulled on, a heavy cascade of iron rings dropping into place over the shoulders, dry close mono, no reverb, no music | 1.6 | 0.42 | N | 3 | SFX |
| item_equip_body_plate | A plate cuirass closed onto the body, heavy steel halves meeting and buckles being drawn tight, dry close mono, no reverb, no music | 2 | 0.45 | N | 3 | SFX |
| item_equip_hands | Gloves pulled on, leather stretching over knuckles with a final tug, dry close mono, no reverb, no music | 0.9 | 0.38 | N | 3 | SFX |
| item_equip_boots | Boots pulled on, leather dragging over the heel and the foot settling to the floor, dry close mono, no reverb, no music | 1.2 | 0.38 | N | 3 | SFX |
| item_twohander_offhand_clear_warn | A short dull warning knock as an off-hand item is forced away to free both hands, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 1 | SFX |
| item_slot_move | An item moved between inventory slots, a soft dry handling shift, minimal, dry close mono, no reverb, no music | 0.3 | 0.42 | N | 3 | UI |
| item_quickslot_assign | An item assigned to a quick slot, a short dry confirming tap, minimal, dry close mono, no reverb, no music | 0.3 | 0.45 | N | 2 | UI |
| item_grid_drag | An item dragged across an inventory grid, a faint continuous handling slide, very quiet, dry close mono, no reverb, no music | 0.5 | 0.38 | N | 2 | UI |
| door_open_wood | A plain wooden door opening, hinges creaking and the plank door swinging, ending as it comes to rest, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 3 | SFX |
| door_close_wood | A plain wooden door closing, hinges creaking and the plank door swinging, ending in a solid latching thud, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 3 | SFX |
| door_open_heavy | A heavy iron-banded door opening, deep hinge groan and enormous weight moving, ending as it comes to rest, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 3 | SFX |
| door_close_heavy | A heavy iron-banded door closing, deep hinge groan and enormous weight moving, ending in a solid latching thud, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 3 | SFX |
| door_open_archive | An old archive door opening, a dry precise swing with a faint echo of a large quiet room beyond, ending as it comes to rest, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 3 | SFX |
| door_close_archive | An old archive door closing, a dry precise swing with a faint echo of a large quiet room beyond, ending in a solid latching thud, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 3 | SFX |
| door_open_shack | A poor shack door opening, a thin rattling board scraping its frame, ending as it comes to rest, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 3 | SFX |
| door_close_shack | A poor shack door closing, a thin rattling board scraping its frame, ending in a solid latching thud, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 3 | SFX |
| chest_open | A wooden chest lid lifted open, hinges creaking and the lid coming to rest back, dry close mono, no reverb, no music | 1.2 | 0.42 | N | 3 | SFX |
| chest_close | A wooden chest lid dropped closed, a solid wooden thud with the latch settling, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 3 | SFX |
| chest_locked_rattle | A locked chest lid pulled against its lock, a short frustrated rattle that does not open, dry close mono, no reverb, no music | 0.7 | 0.45 | N | 2 | SFX |
| lid_creak | An old lid moving slowly on dry hinges, a long thin creak, dry close mono, no reverb, no music | 1.4 | 0.4 | N | 3 | SFX |
| cache_open | A hidden cache opened, a stone or board shifted aside revealing a space beneath, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 2 | SFX |
| corpse_loot_rustle | A body searched for possessions, cloth and mail shifted aside with small objects moving, dry close mono, no reverb, no music | 1.6 | 0.38 | N | 4 | SFX |
| drawer_slide | A wooden drawer pulled open, timber sliding on timber ending in a stop, dry close mono, no reverb, no music | 1 | 0.42 | N | 3 | SFX |
| gate_iron | A heavy iron gate swinging, a long metallic groan of hinges ending in a clang, dry close mono, no reverb, no music | 2 | 0.45 | N | 3 | SFX |
| item_torch_light | A torch catching light, a soft ignition whoosh settling into flame, dry close mono, no reverb, no music | 1.4 | 0.4 | N | 2 | SFX |
| item_torch_burn_loop | Perfectly seamless loop of a handheld torch burning, a close steady flame flutter with pitch crackling, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.35 | Y | 1 | SFX |
| item_torch_extinguish | A torch put out, a sharp hiss and the flame dying to smoke, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 2 | SFX |
| brazier_loop | Perfectly seamless loop of a standing brazier burning, a steady contained fire with occasional ember pops, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.35 | Y | 1 | Ambient |
| item_bandage_tear | A strip of linen torn for a bandage, a sharp fabric rip and wrapping, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 3 | SFX |
| item_potion_gulp | A person drinking a potion down in one, wet swallows and a final exhale, dry close mono, no reverb, no music | 1.6 | 0.38 | N | 3 | Voice |
| item_coating_apply | A coating smeared along a blade, a slow wet drag of oil on steel, dry close mono, no reverb, no music | 1.4 | 0.38 | N | 2 | SFX |
| item_food_eat | A person eating, chewing and swallowing a mouthful of food, dry close mono, no reverb, no music | 1.6 | 0.35 | N | 4 | Voice |
| item_drink_skin | A person drinking from a waterskin, leather squeezing with wet gulps, dry close mono, no reverb, no music | 1.6 | 0.38 | N | 3 | Voice |
| item_whetstone_scrape | A whetstone drawn along a blade edge, one long deliberate rasping stroke, dry close mono, no reverb, no music | 1.2 | 0.45 | N | 4 | SFX |
| item_repairkit_use | A repair kit worked over damaged gear, small tools tapping and leather being drawn tight, dry close mono, no reverb, no music | 1.8 | 0.4 | N | 3 | SFX |
| item_condition_warn | A short dull creak of failing equipment warning it is close to breaking, understated, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 1 | SFX |
| item_break_dull | A piece of equipment breaking, a dead snapping crack with no ring, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 2 | SFX |
| save_wax_seal | A wax seal pressed onto a document, a soft press into warm wax and the seal lifting away, deliberately understated, no jingle, dry close mono, no reverb, no music | 1 | 0.42 | N | 1 | UI |

---

### Category 07 -- Environment: Weather (full grid)

20 ids, 24 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| wx_rain_light_foliage_loop | Perfectly seamless loop of light steady rain falling on leaves and branches taking the drops in a broad soft rustle, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| wx_rain_light_water_loop | Perfectly seamless loop of light steady rain falling on an open water surface stippled with countless small impacts, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| wx_rain_light_wood_roof_loop | Perfectly seamless loop of light steady rain falling on a plank roof drumming overhead with hollow resonance, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| wx_rain_heavy_stone_loop | Perfectly seamless loop of heavy pouring rain falling on hard flagging with bright sharp ticks and running water, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| wx_rain_heavy_water_loop | Perfectly seamless loop of heavy pouring rain falling on an open water surface stippled with countless small impacts, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| wx_rain_heavy_wood_roof_loop | Perfectly seamless loop of heavy pouring rain falling on a plank roof drumming overhead with hollow resonance, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| wx_wind_rain_loop | Perfectly seamless loop of a wet driving wind, steady wind carrying rain, gusting in waves, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| wx_wind_lethal_loop | Perfectly seamless loop of a killing gale, overwhelming screaming wind, violent and continuous, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| wx_lightning_prestrike_hum | The charged moment before a close lightning strike, a rising electrical hum with the air going tight, no strike, dry close mono, no reverb, no music | 1.6 | 0.38 | N | 2 | Ambient |
| wx_overcast_bed_loop | Perfectly seamless loop of a grey overcast day, still heavy air with muted distant sound and no birdsong, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| wx_light_rain_bed_loop | Perfectly seamless loop of light rain over open ground, a soft even patter with gentle wind, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| wx_heavy_rain_bed_loop | Perfectly seamless loop of heavy rain over open ground, a dense roaring downpour with running water, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| wx_fog_bed_loop | Perfectly seamless loop of thick fog, deadened muffled air with sound swallowed and dripping condensation, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| wx_snow_bed_loop | Perfectly seamless loop of steady snowfall, an eerie hush with sound absorbed and only faint wind, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| wx_ash_haze_bed_loop | Perfectly seamless loop of an ashfall haze, dry dead air with fine grit drifting and no living sound at all, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| wx_storm_front_approach | A storm front arriving, distant wind and thunder building steadily closer over several seconds, dry close mono, no reverb, no music | 8 | 0.3 | N | 2 | Ambient |
| wx_weather_swell_30s | A slow thirty second swell of weather intensity rising and easing again, a single long breath of wind and rain, dry close mono, no reverb, no music | 30 | 0.25 | N | 1 | Ambient |
| wx_snowfall_hiss_loop | Perfectly seamless loop of snow falling steadily, a fine dry hiss with the world hushed around it, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| wx_ashfall_whisper_loop | Perfectly seamless loop of ash falling steadily, a dry whispering drift of fine particles, dead and lifeless, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| wx_ash_grit_gust | A gust driving ash and grit past, a dry abrasive rush, dry close mono, no reverb, no music | 2 | 0.35 | N | 3 | Ambient |

---

### Category 08 -- Environment: Water (remaining)

10 ids, 10 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| water_swim_paddle_loop | Perfectly seamless loop of a person treading water and paddling in place, small continuous hand and foot splashes, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 16 | 0.3 | Y | 1 | Ambient |
| water_bubble_trail_loop | Perfectly seamless loop of a stream of bubbles rising through water, continuous small wet pops, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.3 | Y | 1 | Ambient |
| water_river_flow_loop | Perfectly seamless loop of a river flowing steadily over a rocky bed, continuous rushing water, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| water_brook_trickle_loop | Perfectly seamless loop of a small brook trickling over stones, light bright running water, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 16 | 0.3 | Y | 1 | Ambient |
| water_channel_rush_loop | Perfectly seamless loop of water running fast down a narrow dug channel, a confined urgent rush, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 16 | 0.3 | Y | 1 | Ambient |
| water_surf_cliff_loop | Perfectly seamless loop of heavy surf breaking against cliffs, deep swells collapsing and dragging back, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| water_harbor_lap_loop | Perfectly seamless loop of harbour water lapping against stone and timber, small regular slaps with hulls creaking faintly, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| water_drip_cluster_loop | Perfectly seamless loop of scattered water drips falling in a wet cave, irregular plops at different distances, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 18 | 0.28 | Y | 1 | Ambient |
| water_runoff_postrain_loop | Perfectly seamless loop of water running off after rain, gutters and channels draining with steady trickles, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 18 | 0.28 | Y | 1 | Ambient |
| water_wave_shroud_boundary_loop | Perfectly seamless loop of an unnatural sea boundary, waves moving against themselves with a wrong low harmonic underneath, unsettling, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |

---

### Category 10 -- Region Ambient Beds

51 ids, 93 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| amb_central_plains_day_loop | Perfectly seamless loop of wide open grassland under a big sky, grass moving in the wind, skylarks and distant insects, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_central_plains_night_loop | Perfectly seamless loop of wide open grassland at night, wind through grass, crickets and a far-off owl, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_spine_of_the_world_day_loop | Perfectly seamless loop of high bare mountains, thin cold wind over rock, a distant raptor cry and occasional loose stone, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_spine_of_the_world_night_loop | Perfectly seamless loop of high bare mountains at night, bitter thin wind and utter emptiness, rare rockfall, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_the_underway_day_loop | Perfectly seamless loop of a vast worked stone tunnel deep underground, still dead air, distant dripping and faint structural groans, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_the_underway_night_loop | Perfectly seamless loop of a vast worked stone tunnel deep underground, still dead air, distant dripping and faint structural groans, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_the_greatwood_day_loop | Perfectly seamless loop of an ancient dense forest by day, filtered birdsong, leaf rustle and a low silverwood hum beneath it, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_the_greatwood_night_loop | Perfectly seamless loop of an ancient dense forest at night, owls and small movements in the undergrowth, the low silverwood hum stronger, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_western_coast_day_loop | Perfectly seamless loop of a rocky drowned coastline, surf against stone, gulls and a hollow wind off the water, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_western_coast_night_loop | Perfectly seamless loop of a rocky drowned coastline at night, heavy surf, wind and a distant foghorn moan, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_the_ashfields_day_loop | Perfectly seamless loop of a dead grey plain under ashfall, dry wind over lifeless ground, no birds and no insects at all, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_the_ashfields_night_loop | Perfectly seamless loop of a dead grey plain at night, cold dry wind, absolute lifelessness, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_the_sorrowmarsh_day_loop | Perfectly seamless loop of a wide sour marsh, standing water, frogs and reed rattle with a thick heavy air, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_the_sorrowmarsh_night_loop | Perfectly seamless loop of a wide sour marsh at night, croaking and insect drone with faint uneasy shimmering tones, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_the_weeping_wood_day_loop | Perfectly seamless loop of a wrong quiet forest, sound arriving slightly late, dripping and faint whispering just below hearing, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_the_weeping_wood_night_loop | Perfectly seamless loop of a wrong quiet forest, sound arriving slightly late, dripping and faint whispering just below hearing, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_mor_vethrin_day_loop | Perfectly seamless loop of an immense boneyard of arches, wind resonating through hollow structures in long low tones, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_mor_vethrin_night_loop | Perfectly seamless loop of an immense boneyard of arches, wind resonating through hollow structures in long low tones, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_aldenholt_day_loop | Perfectly seamless loop of a small farming town by day, distant voices, livestock, a smith working and cart wheels on packed earth, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_aldenholt_night_loop | Perfectly seamless loop of a small farming town at night, dogs, a few late voices, shutters and wind, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_solgrade_day_loop | Perfectly seamless loop of a prosperous walled city by day, dense crowd murmur, market activity, bells and hooves on stone, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_solgrade_night_loop | Perfectly seamless loop of a prosperous walled city at night, sparse footsteps, distant watch calls and a quiet hum of a settled city, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_vosskara_day_loop | Perfectly seamless loop of a hard industrial town by day, hammering, furnaces, heavy carts and shouted work, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_vosskara_night_loop | Perfectly seamless loop of a hard industrial town at night, banked furnaces roaring low, occasional metal and few voices, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_caer_brannoch_day_loop | Perfectly seamless loop of a fortified keep by day, wind on battlements, drilling soldiers, gates and armoured movement, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_caer_brannoch_night_loop | Perfectly seamless loop of a fortified keep at night, wind on battlements, watch footsteps and torches guttering, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_lirien_thal_day_loop | Perfectly seamless loop of an old elegant settlement by day, water features, soft distant strings and quiet civil movement, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_lirien_thal_night_loop | Perfectly seamless loop of an old elegant settlement at night, water features and a still hush with faint wind chimes, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_copper_isles_day_loop | Perfectly seamless loop of a warm island port by day, harbour water, rigging, gulls and dock work, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_copper_isles_night_loop | Perfectly seamless loop of a warm island port at night, harbour water lapping, rigging creaking and distant tavern noise, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_shroud_sea_boundary_day_loop | Perfectly seamless loop of the edge of an unnatural sea, waves moving wrongly against each other with a low harmonic underneath and no gulls, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_shroud_sea_boundary_night_loop | Perfectly seamless loop of the edge of an unnatural sea, waves moving wrongly against each other with a low harmonic underneath and no gulls, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_bird_call | A single wild bird call at a distance, natural and unhurried, dry close mono, no reverb, no music | 1.4 | 0.35 | N | 8 | Ambient |
| amb_owl | A single owl call at night, distant and clear, dry close mono, no reverb, no music | 1.6 | 0.35 | N | 3 | Ambient |
| amb_insect_chirp | A short burst of insect chirping close by, dry and rhythmic, dry close mono, no reverb, no music | 1.6 | 0.32 | N | 5 | Ambient |
| amb_raptor_cry | A hunting bird crying high overhead, a thin piercing call carrying far, dry close mono, no reverb, no music | 1.6 | 0.35 | N | 3 | Ambient |
| amb_crow | A crow calling harshly two or three times, dry close mono, no reverb, no music | 1.4 | 0.35 | N | 3 | Ambient |
| amb_distant_wolf | A wolf howling far away, a long rising and falling call, dry close mono, no reverb, no music | 3 | 0.35 | N | 3 | Ambient |
| amb_distant_goblin_clatter | Distant goblin activity, faint clattering and jabbering carried on the wind, dry close mono, no reverb, no music | 2 | 0.32 | N | 3 | Ambient |
| amb_ghostlight_shimmer | A faint eerie shimmering tone drifting past over marsh water, cold and unnatural, dry close mono, no reverb, no music | 2.5 | 0.3 | N | 3 | Ambient |
| amb_naergrim_whisper | A whisper just below the threshold of words, wrong and close, no intelligible language, dry close mono, no reverb, no music | 2.5 | 0.3 | N | 4 | Ambient |
| amb_rockfall_tick | A few loose stones shifting and falling on a mountainside, small and distant, dry close mono, no reverb, no music | 1.6 | 0.35 | N | 3 | Ambient |
| amb_silverwood_hum | Perfectly seamless loop of an almost inaudible low tonal hum coming from ancient trees, felt more than heard, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.25 | Y | 1 | Ambient |
| amb_stone_groan | A deep structural groan of enormous stone settling far underground, dry close mono, no reverb, no music | 3 | 0.32 | N | 3 | Ambient |
| amb_bone_arch_resonance | Wind finding a hollow arch and resonating it into a long low tone, dry close mono, no reverb, no music | 4 | 0.3 | N | 2 | Ambient |
| amb_creak_dead_stump | A dead tree stump creaking as it flexes in the wind, dry and hollow, dry close mono, no reverb, no music | 2 | 0.35 | N | 3 | Ambient |
| amb_foghorn_moan | A distant foghorn sounding once across water, low and mournful, dry close mono, no reverb, no music | 3.5 | 0.32 | N | 2 | Ambient |
| amb_settlement_dog | A dog barking a few times somewhere in a village, dry close mono, no reverb, no music | 2 | 0.35 | N | 3 | Ambient |
| amb_settlement_livestock | Livestock in a village, a cow or goat calling with movement in a pen, dry close mono, no reverb, no music | 2 | 0.35 | N | 4 | Ambient |
| amb_distant_smith | A smith working somewhere out of sight, a few ringing hammer blows carrying across a town, dry close mono, no reverb, no music | 2.5 | 0.35 | N | 2 | Ambient |
| amb_rigging_creak | Ship rigging and timber creaking at a quay, ropes working against wood, dry close mono, no reverb, no music | 2.5 | 0.32 | N | 3 | Ambient |

---

### Category 11 -- Day/Night & Time Cues

8 ids, 10 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| time_dawn_birdsong_swell | Dawn arriving, birdsong building from nothing into a full chorus over several seconds, dry close mono, no reverb, no music | 8 | 0.3 | N | 1 | Ambient |
| time_dawn_settlement_wake | A settlement waking at dawn, shutters, first voices and movement starting up, dry close mono, no reverb, no music | 6 | 0.3 | N | 2 | Ambient |
| time_dusk_birdsong_fade | Dusk falling, birdsong thinning away to a few last calls and then silence, dry close mono, no reverb, no music | 8 | 0.3 | N | 1 | Ambient |
| time_dusk_fires_lit_murmur | Evening in a settlement, fires being lit and voices gathering indoors, dry close mono, no reverb, no music | 6 | 0.3 | N | 1 | Ambient |
| time_night_onset_layer_in | Night sounds fading in over the top of a quieting day, insects and owls arriving, dry close mono, no reverb, no music | 6 | 0.3 | N | 1 | Ambient |
| time_day_onset_layer_out | Night sounds fading out as day arrives, insects thinning away, dry close mono, no reverb, no music | 6 | 0.3 | N | 1 | Ambient |
| time_period_boundary_sting | A very short understated tonal marker for the turn of a time period, one soft low tone, diegetic and restrained, dry close mono, no reverb, no music | 1.5 | 0.35 | N | 1 | Ambient |
| worldclock_hour_chime_distant | A settlement bell marking the hour, heard from some distance with air between, dry close mono, no reverb, no music | 4 | 0.35 | N | 2 | Ambient |

---

### Category 12 -- Systems: Lockpicking

12 ids, 21 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| lock_sweep_loop | Perfectly seamless loop of a lockpick sweeping slowly across pin stacks, a continuous fine metallic scrape, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.42 | Y | 1 | SFX |
| lock_false_hum | Perfectly seamless loop of a false set holding under tension, a faint sustained metallic hum that is almost right, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.4 | Y | 1 | SFX |
| lock_pin_set | A lock pin setting into place, a small precise metallic click, dry close mono, no reverb, no music | 0.3 | 0.5 | N | 3 | SFX |
| lock_pick_snap | A lockpick snapping under too much tension, a thin sharp metallic break, dry close mono, no reverb, no music | 0.4 | 0.5 | N | 3 | SFX |
| lock_open | A lock giving way and turning open, pins dropping and the mechanism rotating free, dry close mono, no reverb, no music | 1 | 0.45 | N | 2 | SFX |
| lock_resonance_tone | A faint resonant tone rising from a lock as the correct tension is found, diegetic and metallic, dry close mono, no reverb, no music | 1.5 | 0.4 | N | 2 | SFX |
| lock_approach_swell | A rising metallic hum as a lockpick nears a correct pin, tension building without resolving, dry close mono, no reverb, no music | 1.5 | 0.4 | N | 2 | SFX |
| lock_false_stall | A set bar stalling at half travel, a small dead metallic stop that refuses to go further, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 2 | SFX |
| lock_backpressure_warn | A lock creaking under too much tension too fast, a strained metallic warning before anything breaks, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 2 | SFX |
| lock_overlay_open | A lockpicking view opening, a soft close-in metallic settle as attention narrows to the lock, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 1 | UI |
| lock_overlay_close | A lockpicking view closing, tools withdrawn and the world opening back out, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 1 | UI |
| lock_key_turn_unlock | A key turning a lock open cleanly, a solid mechanical rotation and bolt withdrawing, dry close mono, no reverb, no music | 1 | 0.45 | N | 1 | SFX |

---

### Category 13 -- Systems: Mini-Games

52 ids, 103 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| dice_throw | A handful of bone dice thrown onto a wooden table, a scattering tumble coming to rest, dry close mono, no reverb, no music | 1.4 | 0.45 | N | 3 | SFX |
| dice_shake | Bone dice shaken in a cupped hand, a fast dry rattling, dry close mono, no reverb, no music | 1.2 | 0.42 | N | 3 | SFX |
| dice_settle | The last die rocking to a stop on wood, two small taps and stillness, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 3 | SFX |
| dice_lock | A die pushed aside and locked out of play, a short deliberate wooden slide and stop, dry close mono, no reverb, no music | 0.5 | 0.45 | N | 3 | SFX |
| dice_reveal | Dice uncovered under a cup lifted away, a wooden lift with the dice revealed beneath, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 3 | SFX |
| dice_wager_place | Coins pushed forward as a wager, a small stack sliding across a table and settling, dry close mono, no reverb, no music | 0.9 | 0.45 | N | 3 | SFX |
| dice_read_tell_sting | A very short understated tonal marker as an opponent gives something away, diegetic and restrained, dry close mono, no reverb, no music | 0.8 | 0.4 | N | 2 | SFX |
| minigame_smith_combo_streak | Three well-timed hammer blows landing in sequence on an anvil, rising in confidence, dry close mono, no reverb, no music | 1.8 | 0.45 | N | 2 | SFX |
| minigame_smith_fail_warp | A hammer blow going badly wrong, metal deforming with a dull tearing groan, dry close mono, no reverb, no music | 1.2 | 0.45 | N | 2 | SFX |
| fish_cast_whir | A fishing line cast out, a fast whirring release of line through rings, dry close mono, no reverb, no music | 1.2 | 0.42 | N | 1 | SFX |
| fish_line_release | A fishing line let go to run free, a light continuous ticking release, dry close mono, no reverb, no music | 1 | 0.4 | N | 1 | SFX |
| fish_float_plop | A float landing on still water, a single small plop with rings spreading, dry close mono, no reverb, no music | 0.7 | 0.42 | N | 1 | SFX |
| fish_float_bob_loop | Perfectly seamless loop of a float bobbing gently on water, small irregular laps against it, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.3 | Y | 1 | SFX |
| fish_strike_dip | A float pulled sharply under, a sudden decisive dip and splash, dry close mono, no reverb, no music | 0.6 | 0.45 | N | 1 | SFX |
| fish_reel_tug | A fish fighting against the line, a straining tug with the rod flexing, dry close mono, no reverb, no music | 1.2 | 0.42 | N | 3 | SFX |
| fish_line_tension_creak | A fishing line and rod under dangerous tension, a thin creaking strain, dry close mono, no reverb, no music | 1.4 | 0.42 | N | 1 | SFX |
| fish_line_snap | A fishing line breaking, a sharp thin snap and the tension gone, dry close mono, no reverb, no music | 0.5 | 0.48 | N | 1 | SFX |
| fish_leap_splash | A fish breaking the surface and falling back, a bright leaping splash, dry close mono, no reverb, no music | 1 | 0.42 | N | 3 | SFX |
| fish_net_land | A fish landed in a net, wet thrashing in mesh coming to rest, dry close mono, no reverb, no music | 1.6 | 0.4 | N | 1 | SFX |
| fish_night_shimmer | A faint cold shimmering tone over night water, understated and strange, dry close mono, no reverb, no music | 2.5 | 0.3 | N | 1 | SFX |
| card_deal | A playing card dealt onto a table, a single light slide and settle, dry close mono, no reverb, no music | 0.4 | 0.45 | N | 3 | SFX |
| card_flip | A card turned face up, a short crisp flip against the table, dry close mono, no reverb, no music | 0.3 | 0.48 | N | 3 | SFX |
| card_facedown_place | A card placed face down deliberately, a quiet controlled press onto the table, dry close mono, no reverb, no music | 0.4 | 0.45 | N | 1 | SFX |
| trick_sweep | A won trick swept in across the table, several cards gathered and pulled close, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 1 | SFX |
| fold_reveal_sting | A very short understated tonal marker as a hand is revealed, diegetic and restrained, dry close mono, no reverb, no music | 0.9 | 0.4 | N | 1 | SFX |
| axethrow_breath_loop | Perfectly seamless loop of a person steadying their breathing before a throw, slow controlled breaths, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.32 | Y | 1 | Voice |
| axethrow_release | A throwing axe released, a grunt of effort and the axe leaving the hand tumbling, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 3 | SFX |
| axethrow_thunk_target | A throwing axe biting into a wooden target, a hard splitting thock with the haft quivering, dry close mono, no reverb, no music | 0.9 | 0.48 | N | 4 | SFX |
| axethrow_miss_clatter | A throwing axe missing and clattering away across the ground, dry close mono, no reverb, no music | 1.4 | 0.45 | N | 3 | SFX |
| axethrow_bullseye_chime | A throwing axe hitting dead centre, a hard bite with one clean ring off the head, dry close mono, no reverb, no music | 1 | 0.45 | N | 1 | SFX |
| axethrow_swing_creak | A trick throw wound up slowly, the haft creaking under a held rotation, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 2 | SFX |
| crowd_cheer | A small crowd cheering a good throw, brief and genuine, no words, dry close mono, no reverb, no music | 2 | 0.35 | N | 3 | Voice |
| crowd_groan | A small crowd groaning at a bad throw, brief and disappointed, no words, dry close mono, no reverb, no music | 1.8 | 0.35 | N | 3 | Voice |
| archery_target_thock | An arrow striking a straw archery butt, a dense packed thock, dry close mono, no reverb, no music | 0.6 | 0.48 | N | 4 | SFX |
| clay_bird_launch | A clay target flung from a thrower, a fast mechanical release and the disc whirring away, dry close mono, no reverb, no music | 1 | 0.45 | N | 3 | SFX |
| clay_bird_shatter | A clay target shattering in mid air, a sharp brittle burst with fragments falling, dry close mono, no reverb, no music | 1.2 | 0.48 | N | 3 | SFX |
| archery_streak_tick | A very short dry tick marking a scoring streak, minimal, dry close mono, no reverb, no music | 0.2 | 0.5 | N | 1 | SFX |
| archery_miss_whiff | An arrow passing wide of a target, a fast whiff past with no impact, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 3 | SFX |
| forage_aura_loop | Perfectly seamless loop of a faint natural shimmer marking something worth finding nearby, understated and organic, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.3 | Y | 1 | SFX |
| forage_approach_pitch_rise | A faint natural tone rising slowly as a search closes in, understated, dry close mono, no reverb, no music | 2.5 | 0.32 | N | 1 | SFX |
| forage_startle_close | Something small bolting away through undergrowth at close range, a sudden startling rush, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 2 | SFX |
| forage_hold_loop | Perfectly seamless loop of a hand held steady over a plant, tiny sustained leaf and stem contact, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.3 | Y | 1 | SFX |
| forage_success_pluck_rare | A rare plant taken cleanly, a careful stem snap with a soft settling of leaves, dry close mono, no reverb, no music | 1 | 0.4 | N | 2 | SFX |
| forage_grab_common | A common plant pulled up, a quick tearing of stem and root from soil, dry close mono, no reverb, no music | 0.8 | 0.4 | N | 4 | SFX |
| armwr_strain_loop | Perfectly seamless loop of two people straining against each other at arm wrestling, sustained tremor of effort and creaking table, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 10 | 0.35 | Y | 1 | Voice |
| armwr_surge_grunt | A hard surge of effort in a contest of strength, a rising grunt pushing through, no words, dry close mono, no reverb, no music | 1.2 | 0.35 | N | 3 | Voice |
| armwr_force_tick | A very short dry tick marking force shifting in a contest, minimal, dry close mono, no reverb, no music | 0.2 | 0.5 | N | 1 | SFX |
| armwr_win_slam | An arm driven down onto a table in victory, a hard flat slam with the table jumping, dry close mono, no reverb, no music | 0.7 | 0.48 | N | 1 | SFX |
| armwr_lose_slam | An arm forced down onto a table in defeat, a hard flat slam with a defeated exhale after, dry close mono, no reverb, no music | 1 | 0.45 | N | 1 | SFX |
| sculpt_match_tick | A very short dry tick marking time passing in a contest, minimal, dry close mono, no reverb, no music | 0.2 | 0.5 | N | 1 | SFX |
| sculpt_timer_warn | A low understated warning tone as contest time runs short, diegetic and restrained, dry close mono, no reverb, no music | 1 | 0.42 | N | 1 | SFX |
| sculpt_verdict_sting | A short understated tonal marker as a verdict is given, restrained, no fanfare, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 1 | SFX |

---

### Category 14 -- Investigation & Clue

7 ids, 10 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| invest_examine_foley | An object being examined closely, turned in the hands with small handling sounds, dry close mono, no reverb, no music | 1.6 | 0.38 | N | 3 | SFX |
| invest_clue_shimmer_loop | Perfectly seamless loop of a faint sustained shimmer marking something worth attention, very restrained, almost subliminal, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.28 | Y | 1 | SFX |
| invest_text_appear_whisper | Faint paper and quill whisper as written text is read out of a document, no words, dry close mono, no reverb, no music | 1.4 | 0.32 | N | 2 | SFX |
| invest_noted_quill_stamp | A note recorded, a short quill stroke and a soft stamp onto paper, dry close mono, no reverb, no music | 1 | 0.42 | N | 1 | SFX |
| invest_deduction_sting | A short understated tonal marker as pieces connect, one low clean tone, no fanfare, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 1 | SFX |
| invest_companion_chime | A very soft low tone marking a companion noticing something, restrained and diegetic, dry close mono, no reverb, no music | 0.9 | 0.4 | N | 1 | SFX |
| invest_saturation_exhausted | A minimal dry tone marking that a scene has nothing further to give, understated to the point of near silence, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 1 | SFX |

---

### Category 15 -- UI, Menu & Feedback

21 ids, 26 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| ui_navigate | A very short dry tick moving between menu entries, minimal and unmusical, dry close mono, no reverb, no music | 0.15 | 0.5 | N | 2 | UI |
| ui_confirm | A short dry confirming tap, minimal, no chime and no fanfare, dry close mono, no reverb, no music | 0.2 | 0.5 | N | 1 | UI |
| ui_cancel | A short dry backing-out tap, slightly duller than a confirm, minimal, dry close mono, no reverb, no music | 0.2 | 0.5 | N | 1 | UI |
| ui_error_thud | A short dull negative thud refusing an action, no ring and no tone, dry close mono, no reverb, no music | 0.3 | 0.5 | N | 1 | UI |
| journal_open | A leather journal opened, a soft cover flex and page settle, dry close mono, no reverb, no music | 0.8 | 0.42 | N | 1 | UI |
| journal_close | A leather journal closed, a soft cover fall and settle, dry close mono, no reverb, no music | 0.7 | 0.42 | N | 1 | UI |
| journal_page_turn | A single page turned in a book, a light paper sweep and settle, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 3 | UI |
| map_open | A folded map opened out, parchment unfolding and flattening, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 1 | UI |
| map_trace_ink | A line traced in ink across parchment, a fine continuous nib drag, dry close mono, no reverb, no music | 1.4 | 0.4 | N | 2 | UI |
| quickslot_cycle | A very short dry tick cycling a quick slot, minimal, dry close mono, no reverb, no music | 0.15 | 0.5 | N | 1 | UI |
| quickslot_activate | A short dry tap activating a quick slot item, minimal, dry close mono, no reverb, no music | 0.25 | 0.48 | N | 2 | UI |
| inventory_open | An inventory opened, a soft leather and cloth shift of a pack being looked into, dry close mono, no reverb, no music | 0.7 | 0.4 | N | 1 | UI |
| inventory_close | An inventory closed, a soft leather and cloth settle of a pack shutting, dry close mono, no reverb, no music | 0.6 | 0.4 | N | 1 | UI |
| pause_open | A very soft low tone marking the game pausing, restrained and short, dry close mono, no reverb, no music | 0.5 | 0.42 | N | 1 | UI |
| pause_close | A very soft low tone marking the game resuming, restrained and short, dry close mono, no reverb, no music | 0.5 | 0.42 | N | 1 | UI |
| interaction_prompt_appear | An extremely subtle short tick as an interaction prompt appears, almost inaudible, dry close mono, no reverb, no music | 0.12 | 0.5 | N | 1 | UI |
| bark_overlay_appear | A very soft short marker as a spoken line appears on screen, near-subliminal, dry close mono, no reverb, no music | 0.15 | 0.5 | N | 1 | UI |
| skill_node_tone | One single soft clean tone marking a skill taken, restrained, no fanfare and no jingle, dry close mono, no reverb, no music | 0.9 | 0.42 | N | 1 | UI |
| skill_legendary_reset_tone | One low sustained tone marking a legendary skill reset, sombre rather than triumphant, no fanfare, dry close mono, no reverb, no music | 1.6 | 0.4 | N | 1 | UI |
| quest_update_soft | A very soft short marker that an objective has changed, understated, no fanfare, dry close mono, no reverb, no music | 0.6 | 0.42 | N | 1 | UI |
| objective_complete_soft | A soft low tone marking an objective completed, quietly satisfying, no fanfare and no jingle, dry close mono, no reverb, no music | 1 | 0.42 | N | 1 | UI |

---

### Category 16 -- Death & Respawn

7 ids, 8 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| death_collapse_thud | A body collapsing to the ground, armour and limbs hitting hard and going still, dry close mono, no reverb, no music | 1.6 | 0.42 | N | 2 | SFX |
| death_near_breath_loop | Perfectly seamless loop of a dying person breathing in shallow failing rasps, wet and slowing, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.32 | Y | 1 | Voice |
| death_near_heartbeat_loop | Perfectly seamless loop of a slow heavy heartbeat heard from inside the body, thick and muffled, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.3 | Y | 1 | SFX |
| death_vignette_rumble_loop | Perfectly seamless loop of a low pressure rumble closing in as consciousness narrows, felt more than heard, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.28 | Y | 1 | SFX |
| death_second_wind_cue | A sharp indrawn breath as someone pulls back from the edge, one gasp and the pressure lifting, dry close mono, no reverb, no music | 1.4 | 0.38 | N | 1 | SFX |
| death_fade_tone | A single low tone fading out as everything goes dark, sombre and quiet, no sting and no fanfare, dry close mono, no reverb, no music | 3 | 0.35 | N | 1 | SFX |
| death_return_load_soft | A soft low return of ambient sound as the world comes back, gentle and unhurried, dry close mono, no reverb, no music | 2.5 | 0.32 | N | 1 | SFX |

---

### Category 17 -- NPC Non-Verbal & Crowd

16 ids, 45 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| npc_effort_grunt_m | A short effort grunt from a man doing physical work, breath only, no words, dry close mono, no reverb, no music | 0.7 | 0.35 | N | 5 | Voice |
| npc_effort_grunt_f | A short effort grunt from a woman doing physical work, breath only, no words, dry close mono, no reverb, no music | 0.7 | 0.35 | N | 5 | Voice |
| npc_react_surprise | A short surprised intake of breath from a person, no words, dry close mono, no reverb, no music | 0.6 | 0.35 | N | 3 | Voice |
| npc_react_scoff | A short dismissive scoff from a person, breath and throat only, no words, dry close mono, no reverb, no music | 0.6 | 0.35 | N | 3 | Voice |
| npc_react_laugh | A short genuine laugh from a person, no words, dry close mono, no reverb, no music | 1.4 | 0.35 | N | 4 | Voice |
| npc_react_cough | A person clearing their throat and coughing once or twice, no words, dry close mono, no reverb, no music | 1.2 | 0.35 | N | 3 | Voice |
| npc_react_sigh | A person sighing heavily, tired rather than sad, no words, dry close mono, no reverb, no music | 1.4 | 0.32 | N | 3 | Voice |
| npc_react_gasp | A sharp alarmed gasp from a person, no words, dry close mono, no reverb, no music | 0.6 | 0.38 | N | 3 | Voice |
| npc_footstep_approach | A person walking up and stopping close by, three or four unhurried steps and a settle, dry close mono, no reverb, no music | 2 | 0.38 | N | 3 | SFX |
| npc_bark_pre_breath | The small intake of breath a person takes just before speaking, no words, dry close mono, no reverb, no music | 0.4 | 0.32 | N | 2 | Voice |
| crowd_market_murmur_loop | Perfectly seamless loop of a busy market crowd murmuring, many overlapping voices with no intelligible words, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Voice |
| crowd_tavern_murmur_loop | Perfectly seamless loop of a tavern room of drinkers talking and laughing, warm and enclosed, no intelligible words, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Voice |
| crowd_court_murmur_loop | Perfectly seamless loop of a formal hall of people talking low and carefully, restrained and echoing, no intelligible words, wide stereo field, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Voice |
| vendor_callout_nonverbal | A market trader calling out to passers-by, the shape and rhythm of a sales cry with no intelligible words, dry close mono, no reverb, no music | 2 | 0.35 | N | 4 | Voice |
| guard_challenge_nonverbal | A guard barking a challenge, the shape and rhythm of an order with no intelligible words, dry close mono, no reverb, no music | 1.4 | 0.38 | N | 3 | Voice |
| child_play_distant_loop | Perfectly seamless loop of children playing somewhere out of sight, distant calls and laughter, no intelligible words, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 18 | 0.3 | Y | 1 | Voice |

---

### Category 18 -- Economy & Vendor

6 ids, 9 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| econ_coin_count_loop | Perfectly seamless loop of coins being counted out one at a time onto a wooden counter, a steady deliberate rhythm of small metal, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.42 | Y | 1 | SFX |
| econ_coin_pickup_single | A single coin picked up, one small bright metallic clink, dry close mono, no reverb, no music | 0.4 | 0.48 | N | 3 | SFX |
| econ_coin_pickup_purse | A purse of coins lifted, a muffled mass of metal shifting inside leather, dry close mono, no reverb, no music | 0.8 | 0.45 | N | 2 | SFX |
| econ_trade_confirm | A trade agreed, coins pushed across a counter and goods taken up, understated, no fanfare, dry close mono, no reverb, no music | 1.2 | 0.42 | N | 1 | SFX |
| econ_vendor_open | A trader setting out to do business, a ledger opened and goods shifted forward, dry close mono, no reverb, no music | 1.4 | 0.4 | N | 1 | SFX |
| econ_vendor_close | A trader closing up, a ledger shut and goods drawn back, dry close mono, no reverb, no music | 1.4 | 0.4 | N | 1 | SFX |

---

### Category 19 -- Magic & Spellcraft

31 ids, 51 files at the declared variation counts.

| id | prompt | dur | infl | loop | var | bus |
|---|---|---|---|---|---|---|
| mag_charge_gather | Power being gathered with effort, a strained rising pressure with the air going wrong around it, austere, never triumphant, dry close mono, no reverb, no music | 2 | 0.35 | N | 3 | Combat |
| mag_release_innate | A working released, a short unglamorous discharge that costs something, dry and strained, never triumphant, no whoosh and no boom, dry close mono, no reverb, no music | 1.4 | 0.38 | N | 3 | Combat |
| mag_channel_loop | Perfectly seamless loop of a working held open under strain, a sustained wrong pressure with the air refusing it, austere and never triumphant, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.32 | Y | 1 | Combat |
| mag_fizzle_fail | A working collapsing before it takes, a strained gathering that gutters out into nothing, disappointing and dry, dry close mono, no reverb, no music | 1.6 | 0.38 | N | 2 | Combat |
| mag_cost_toll | The price of a working landing on the caster, a pained gasp with a cranial tinnitus swell rising and ragged breathing after, no words, dry close mono, no reverb, no music | 3 | 0.35 | N | 3 | Voice |
| mag_cost_alteration_sting | The moment a working changes the caster permanently, a single cold wrong tone with no resolution, deeply unwelcome, dry close mono, no reverb, no music | 2 | 0.35 | N | 1 | Voice |
| mag_ritual_build_loop | Perfectly seamless loop of a long ritual building in a stone space, low sustained pressure with a slow wrongness accumulating underneath, no chanting, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| mag_ritual_against_grain_loop | Perfectly seamless loop of a ritual working against the grain of the world, a detuned thinning drone that sounds like reality being stretched, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.26 | Y | 1 | Ambient |
| mag_env_temp_shift | The temperature of a space changing unnaturally fast, air contracting with a low groan and frost ticking, dry close mono, no reverb, no music | 2.5 | 0.32 | N | 2 | Ambient |
| mag_env_pressure_pop | Air pressure dropping sharply in a room, a dull ear-popping thump with everything going momentarily flat, dry close mono, no reverb, no music | 1.2 | 0.38 | N | 2 | Ambient |
| mag_env_stormcharge_hum | Perfectly seamless loop of the air before an unnatural storm, a low electrical hum with everything held too tight, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 14 | 0.3 | Y | 1 | Ambient |
| mag_structural_perceive_loop | Perfectly seamless loop of the sound of seeing how something is put together, a faint ordered ringing of structure revealing itself, cold and analytical, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.28 | Y | 1 | Ambient |
| mag_blight_creep_loop | Perfectly seamless loop of blight spreading slowly through living ground, a dry crackling rot advancing with life going quiet ahead of it, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 18 | 0.28 | Y | 1 | Ambient |
| mag_ward_raise | A protective ward raised, a strained tightening of air into a boundary, effortful rather than grand, dry close mono, no reverb, no music | 1.6 | 0.35 | N | 2 | Combat |
| mag_ward_hold_loop | Perfectly seamless loop of a protective ward holding, a sustained pressure boundary humming under load, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.3 | Y | 1 | Combat |
| mag_ward_break | A protective ward failing, a pressure boundary tearing open with a sick collapsing snap, dry close mono, no reverb, no music | 1.4 | 0.4 | N | 3 | Combat |
| mag_aeluvain_hum_loop | Perfectly seamless loop of a pure cold tone with one note conspicuously missing from it, beautiful and incomplete, referenced from a distance, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 16 | 0.28 | Y | 1 | Ambient |
| mag_wrongness_pressure_loop | Perfectly seamless loop of the presence of something that should not exist, an oppressive low pressure field with dread in it and no melody, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.26 | Y | 1 | Ambient |
| mag_staff_focus_tap | A staff butt tapped once on stone to focus a working, a plain wooden knock with a faint wrong resonance after, dry close mono, no reverb, no music | 1.2 | 0.4 | N | 3 | Combat |
| mag_staff_thrum_loop | Perfectly seamless loop of a staff held ready, a low uneasy thrum running through the wood, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 12 | 0.3 | Y | 1 | Combat |
| mag_item_activate | An enchanted object waking, a cold reluctant stirring of something old, austere and unwelcoming, never triumphant, dry close mono, no reverb, no music | 2 | 0.35 | N | 3 | Combat |
| mag_rune_circle_ignite | A rune circle taking light, a strained rising tone with stone heating and the air pulling inward, never triumphant, dry close mono, no reverb, no music | 2.5 | 0.35 | N | 2 | Ambient |
| mag_rune_circle_loop | Perfectly seamless loop of a rune circle burning steadily, a sustained cold tone with the air held wrong above it, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 16 | 0.28 | Y | 1 | Ambient |
| mag_aeluvain_unsheathe | A legendary blade drawn, a clean cold ring that keeps going slightly too long, dry close mono, no reverb, no music | 2.5 | 0.38 | N | 1 | Combat |
| mag_aeluvain_strike | A legendary blade landing, a clean cut with an impossible pure tone underneath it, dry close mono, no reverb, no music | 1.6 | 0.4 | N | 2 | Combat |
| mag_aeluvain_song_complete | A missing note finally sounding and a long incomplete tone resolving at last, the one moment in this set allowed to feel like relief, dry close mono, no reverb, no music | 4 | 0.32 | N | 1 | Combat |
| mag_ashfallen_cast | An armoured revenant working magic, a clipped joyless discharge that visibly costs it, no voice and never triumphant, dry close mono, no reverb, no music | 1.6 | 0.38 | N | 3 | Combat |
| mag_hand_ritual_chant_loop | Perfectly seamless loop of a group ritual working, low rhythmic non-verbal intoning with no intelligible words in any language, strained and joyless, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.28 | Y | 1 | Ambient |
| mag_ashlord_presence_loop | Perfectly seamless loop of the presence of an overwhelming hostile power nearby, an oppressive pressure field with everything else going quiet under it, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.26 | Y | 1 | Ambient |
| mag_ashlord_unmask_sting | A concealed power revealing itself, a single low wrong tone with the air collapsing inward, dreadful rather than grand, dry close mono, no reverb, no music | 3 | 0.32 | N | 1 | Ambient |
| mag_mordvar_ambient_loop | Perfectly seamless loop of the world thinning near something ancient and wrong, a sustained absence where sound should be, with no discrete event in it, constant unchanging texture and level from the first instant to the last, no onset transient, no attack, no fade in or out, no swell, built to repeat with an inaudible join, dry close mono, no reverb, no music | 20 | 0.24 | Y | 1 | Ambient |

---

<!-- END GENERATED PROMPTS -->

## 8. Generation progress, spend & remaining work

Status record as of the first generation push (kept current — update when a
batch is rendered or assets are curated into the repo).

### Generated this cycle — raw renders now COMMITTED in-repo (uncurated)

| Category | Status | ~Gens | Notes |
|---|---|---|---|
| 08 Water core | ✅ rendered | 44 | loops re-rendered with hardened wording, validated seamless |
| 07 Weather basics | ✅ rendered | 41 | loop-heavy; loops validated seamless |
| 01 Locomotion (live surfaces) | ✅ rendered | 287 | footsteps/jump/armor/breath/etc. |
| 09 Fire & Camp | ✅ rendered | 43 | |
| 04 Voxel / Terrain (scoped) | ✅ rendered | 133 | dig/mine/place/collapse/explosive/build |
| **02 Combat / Impacts / Enemies** | ⛔ NOT rendered | 429 | ~9,585 cr — deferred (budget) |

**Update 2026-05-18 — raw renders committed (PR #226).** At the designer's
explicit direction the curate-first step was *skipped*: all **548** raw
`.mp3` takes were bulk-placed into `assets/audio/sfx/<folder>/` as
`<id>_NN.mp3` variation sets (mapped by the exact `AudioManager` prefix
rules, `_vNN`→`_NN`). They are now live — every wired call site plays them
immediately, random-picking across each id's set. They remain **raw and
uncurated** (rough/again-identical takes still in the pool, no `.ogg`
conversion, no loop-Import pass). Distribution: locomotion 287, voxel 133,
environment 122 (weather/water/fire), ui 6 (`camp_rest_*`). Combat (Cat 02)
is still unrendered, so `combat/` holds only the `.gitkeep`.

### Spend & cost model (calibrated, locked)

- Real ElevenLabs cost from two clean batches (Water 1,697; Cat 04 ~3,000):
  **≈ 8.2 credits/sec + ≈ 19 credits/generation floor.** `render_sfx.py`
  uses a slightly conservative **9 cr/s, 20 cr/gen** (estimates run ~6% high
  so the cap stays protective).
- This cycle: ~84k → ~118k used; **~12k credits banked**, generation paused.
- The early "Locomotion 1.9× overrun / 50-credit floor" was a
  mis-attribution of multi-batch spend, since corrected.

### Remaining work / TODO

1. **Render Combat (Cat 02, ~9,585 cr)** next billing cycle — fits one
   fresh cycle. `render_sfx.py --category 02 --credit-cap 11000`.
2. **Continue the master library** per `SFX_LIBRARY.md §22`: remaining
   weather/water extras, region ambient beds (Cat 10), systems/UI/mini-game
   gaps (Cat 12–18), then the long tail. Draft each phase's prompt table
   here first, budget-scoped, same hardened format.
3. **Curation pass** — now a **prune-in-place** job, not a placement job
   (raw takes already committed, see Update above). For each id: audition
   its `<id>_NN.mp3` set against the Desktop `0_KEEP` label, **delete the
   weak takes in `assets/audio/sfx/`** (loop → keep the best seam-checked
   one, `var`≥2 → keep the strongest few, `var`1 → keep 1). Optionally
   `ffmpeg -i in.mp3 -ac 1 -ar 44100 -c:a libvorbis <id>_NN.ogg` (a
   matching `.ogg` auto-supersedes the `.mp3`), set loop files'
   Import→Loop=On, flip the entry to EXISTING in `SFX_LIBRARY.md`. No
   credits. This is the §8b quality pass.
4. ✅ **Wire call sites** (see §8a) — DONE for every rendered family
   (campfire, NoEditZone, footsteps, dig+dig-loop, weather bed, water).
   Combat is the only unwired family, blocked on its render (#1).

### 8a. Wiring status — `AudioManager` autoload

Built and registered (`scripts/AudioManager.gd`, autoload after
`ProfilerOverlay`). Single API: `AudioManager.play(id, world_pos)`,
`play_loop(id, world_pos) -> handle`, `stop_loop(handle)`. Resolves
`assets/audio/sfx/<folder>/<id>[.ogg|_NN.ogg]`, random-picks variation,
routes to the bus by id-prefix, **no-ops with one warning if the file
isn't placed yet** (so wiring is safe before curation; sounds switch on as
`.ogg`s land). Needs in-editor verification (no headless Godot here).

**Wired so far** (low-risk, signal/cosmetic; both no-op silently until the
`.ogg` is curated in — verify in-editor via the one-time Output warning):

- ✅ **Camp/fire** — `CampfireFlicker3D._ready/_exit_tree` →
  `play_loop("fire_campfire_crackle_loop")` at the fire, stopped on exit.
- ✅ **NoEditZone reject** — `AudioManager` subscribes to
  `VoxelEditManager.edit_rejected_no_edit_zone` →
  `play("vox_bedrock_blocked", pos)`. Done from the audio layer (deferred,
  guarded) so **no gameplay-script edit**.
- ✅ **Footsteps** — `Player3D._update_footsteps` (post-move): distance-
  cadence, gait from `_is_crouching/_is_sprinting`, **real surface
  detection** via `_surface_under_player()` — reads the voxel material
  under the feet (canonical `VoxelEditManager.world_to_voxel` + terrain
  voxel tool on `CHANNEL_TYPE` + `VoxelMaterialRegistry`, mapped to
  grass/dirt/stone/wood/sand buckets) and `_in_water` → `shallow_water`.
  Gated on-floor + moving; airborne/jump/deep-swim skipped.

- ✅ **Voxel dig/mine** — `EditToolHandler._carve` (post-accept) →
  `_dig_sfx_id(equipped_id, material.id_string)` → `vox_pick_strike_stone`
  / `vox_shovel_strike_<grass|sand|dirt>` / `vox_wrongtool_<stone|soft>`.
  One strike per accepted carve; axe/wood + break/place are a later
  sub-phase (Cat-04 axe set not rendered yet).
- ✅ **Continuous dig loop** — `EditToolHandler._update_dig_loop` (per
  frame, self-correcting) → `vox_dig_loop_hard` (pickaxe) /
  `vox_dig_loop_soft` while the mine button is held with a manual tool;
  stops the instant mining stops. Addresses "no sound *while* mining".
- ✅ **AudioManager polish** — one-shot `play()` now applies per-trigger
  pitch (~±6%) + volume (~±2 dB) jitter (loops exempt), and resolves
  `.mp3` as well as `.ogg`.
- ✅ **Weather ambience bed** — `AudioManager` subscribes to
  `WeatherManager.weather_state_changed` AND primes the current state at
  wire time (the manager seeds its first state without emitting), then
  swaps a single ambient loop via `WEATHER_BED` (state-name → `wx_*`):
  clear/fog → `wx_clear_bed_loop`, overcast/snow → `wx_wind_breeze_loop`,
  light/heavy rain → `wx_rain_*_soil_loop`. Idempotent (no restart pop if
  the bed is unchanged); done from the audio layer, **no gameplay edit**.
  No dedicated fog/snow beds rendered yet — reuse documented in code.
- ✅ **Water** — `Player3D._update_water_state` → `_update_water_audio()`
  (post water-query, pre-movement). Edge one-shots at the player:
  `water_splash_medium` (enter) / `water_splash_small` (exit) /
  `water_submerge_plunge` (head under) / `water_surface_gasp` (break
  surface, → Voice bus). Plus ONE self-correcting flat ambience bed:
  `water_underwater_ambient_loop` submerged / `water_swim_surface_loop`
  wading-swimming / none on land — same one-loop pattern as the weather
  bed (idempotent, self-heals if the file imports late). Fly-mode toggle
  explicitly stops the bed (water update is skipped while flying).

**Still to wire** (per-system, when that system is touched — not all at once):

- Combat — melee/`ThrowableSpear`/enemy scripts → `cmb_*` (once Combat
  SFX are rendered next cycle). This is the only remaining call-site
  family, and it is blocked on the Cat 02 render (task #7).

### 8b. Known quality gaps — FUTURE IMPROVEMENT (flagged 2026-05-18)

In-game footstep audio was confirmed working end-to-end but **sounds poor**
in its current uncurated state. These are polish items, *not* bugs — keep
the wiring, revisit the quality here:

1. ✅ **Variation rotation** — addressed for footsteps: all rendered
   takes (24 step ids × ~7) bulk-copied as `<id>_01..0N.mp3` sets, so
   AudioManager random-picks across them (no more identical-clip
   machine-gun). Still **bulk, not auditioned** — see #4.
2. ✅ **Pitch/volume jitter** — added to `AudioManager.play` (one-shots
   only; ~±6% pitch, ~±2 dB), so even one take varies per trigger.
3. ✅ **Cadence** — `STEP_DIST_*` raised to 1.6/2.1/1.1 (~2.8/4/1.8 per
   sec). Still a feel knob; revisit if it reads fast/slow in play.
4. ⏳ **Takes unaudited.** The bulk-copied sets include every candidate,
   not the best. Real curation = listen, prune each id to the strongest
   takes, optionally convert to `.ogg`. Still task #8.

Net: footsteps should now sound markedly better (varied + jittered);
remaining work is the listen-and-prune curation, not code.

**Confirmed working in-game 2026-05-18 (clean run, no errors):** footsteps
(surface-aware + varied + jittered), dig strike-on-break + continuous dig
loop, campfire loop, NoEditZone reject — all firing correctly. Designer
verdict: functional but **sound quality is rough** — accepted and deferred.
The remaining quality levers, for the future polish cycle, are entirely
non-code:
  a. **Curate (task #8):** audition each id's ~7 bulk takes, keep the 3–5
     strongest, delete the rest. Most of the "bad" is weak/again-identical
     takes surviving because nothing was pruned.
  b. **Re-roll the weakest ids:** footstep takes are the poorest source
     material; regenerate those prompts (the footstep rows in §2) next
     credit cycle and re-curate. Tools/process unchanged.
  c. Optional: fine-tune `STEP_DIST_*` / jitter ranges by feel once (a)+(b)
     give good source audio (judging cadence on bad samples is misleading).
Pipeline + wiring are **done**; this is a pure audio-quality pass for later.

**Update 2026-05-18 (PR #226) — raw takes committed in-repo.** The full
548-take pool now lives in `assets/audio/sfx/` (was Desktop-only). This
*improves* perceived variety immediately (real `<id>_NN` sets → genuine
random rotation, not the 1–2-clip machine-gun) but does **not** fix the
"sounds rough" verdict — the weak/again-identical takes are still in the
pool because nothing was pruned. The §8b quality pass is therefore now a
**delete-in-place curation** (remove bad takes from the repo folders) plus
optional `.ogg` conversion + footstep re-roll — no re-placement needed,
no credits. Combat takes are still absent (Cat 02 unrendered).

---

## 9. Maintenance

- This doc is generated *from* `SFX_LIBRARY.md`. If an entry's design
  changes, change it there first, then regenerate the prompt here.
- Keep the global rules (mono, dry, no reverb, no music) in §1, not in every
  cell, so prompts stay terse and ElevenLabs stays focused.
- As phases are generated, append their tables here and update §8.
