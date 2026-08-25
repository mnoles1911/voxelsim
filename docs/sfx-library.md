<!--
PROVENANCE. Verbatim copy of design/SFX_LIBRARY.md from the Mira-Thal /
Voxelmark Godot checkout, github.com/mnoles1911/Test @ main, retrieved
2026-08-24. Upstream: 67a48a4e (2026-05-17), merged as 7eb3372d (#216).

This is the INVENTORY -- what sounds the game needs. The prompts that generate
them are docs/sfx-prompts.md. Rendered output belongs in
ue-project/Content/Audio/SFX/; see that directory's README for the folder plan
(taken from §2 below, unchanged) and for the one open decision it records.

THREE THINGS TO KNOW BEFORE READING THE COUNTS:

1. ~1,930 files is the full inventory, and §22 says plainly that a playable
   vertical slice needs 300-400 of them. Four combinatorial sets are most of
   the difference: footsteps (240), the tool x material voxel matrix (273),
   the weapon-class matrix (216), and damage-type impacts + enemies (~250).

2. 548 takes are ALREADY RENDERED and committed in the Test repo, 25.1 MB of
   loose .mp3: locomotion 287, voxel 133, environment 122, ui 6. That is 108
   distinct sound ids at ~5 variation takes each, NOT 548 distinct sounds.
   They are raw and unpruned -- the designer's own verdict was "functional but sound
   quality is rough", and the fix is a listen-and-delete curation pass, not
   regeneration. See docs/sfx-prompts.md §8b.

3. COMBAT IS NOT RENDERED. Category 02 was Phase 1's "do this first" and it is
   the one family that was deferred on budget (~9,585 ElevenLabs credits), so
   `combat/` upstream holds nothing but a .gitkeep.

THE STATUS COLUMN BELOW IS STALE, and it understates what exists. Its
"EXISTING -- 14 files" is the Godot repo's dice (8) and lockpicking (6) only:
it was written 2026-05-17, the 548 takes landed 2026-05-18 (PR #226), and the
"flip the entry to EXISTING" step in docs/sfx-prompts.md §8 task 3 was part of
the curation pass that never ran. So the library still reads NEEDED for 108
ids that have audio. Trust the prompts doc's §8 for what is rendered.

From voxelsim's point of view every entry is NEEDED regardless -- none of this
audio is in this repo yet.

Cross-references to design/AUDIO_DESIGN.md and design/MUSIC_PROMPTS.md point
at the Test repo. The music one is here as docs/music-prompts.md; AUDIO_DESIGN
(bus layout, routing, the Godot-side file conventions) was not ported, because
its engine half does not survive the move to Unreal.
-->

# SFX Library — Mira-Thal Master List

The master sound-effects inventory for Game One. **This document is the
list, not the prompts** — generation prompts (ElevenLabs / Suno) come in a
later pass, one per entry, using this as the spec.

> Cross-reference: `design/AUDIO_DESIGN.md` — bus layout, file conventions,
> the "sound tells the truth when visuals lie" philosophy, and the SFX design
> rules (diegetic cues, no UI beeps for combat, minimal menu sound).
> `design/MUSIC_PROMPTS.md` — the music side of the audio corpus.

Design references used as a guideline, as requested: **Minecraft** (per-
material block break/place/footstep families, mob vocal sets), **Kingdom Come:
Deliverance 2** (realistic weapon/armor foley, footsteps layered by armor
weight × surface, sheathing, station crafting), plus Skyrim/Oblivion for
ambient-bed scale.

---

## 1. Conventions

- **Format:** mono, 44.1 kHz, `.ogg` (spatialized in-engine via
  `AudioStreamPlayer3D`), per `AUDIO_DESIGN.md`. Ambient beds and music-like
  loops are the stereo exception (noted `[stereo]`).
- **Naming:** `snake_case`, group prefix, matches existing on-disk files
  (`lock_pin_set`, `dice_throw`). Pattern:
  `<group>_<thing>[_<qualifier>][_NN]` where `_NN` is the variation index.
  Example: `step_walk_stone_03`, `vox_mine_stone_pick_02`,
  `cmb_sword_swing_light_01`, `amb_greatwood_night_loop`.
- **Type:** `one-shot` (single trigger), `loop` (seamless, plays while a state
  holds), `cue` (very short non-musical tell — still diegetic, never a UI
  beep where the design forbids it).
- **Variations (`var`):** how many distinct files for that entry, to avoid the
  "machine-gun" repeat problem (Minecraft-style). Defaults: footsteps &
  high-frequency impacts **5**, generic one-shots **3**, unique one-shots
  **1**, loops **1** (seamless). A matrix's total = axes × `var`.
- **Status:** `EXISTING` (already on disk — 14 files) or `NEEDED`.
- **Bus:** routes per `AUDIO_DESIGN.md` →
  `SFX/Combat`, `SFX/Ambient`, `SFX` (interaction), `UI`, `Voice` (non-verbal
  human efforts share the Voice bus).
- **Gen source (filled in the next pass, shown here as a hint):** `EL` =
  ElevenLabs SFX (short one-shots, foley, impacts), `SU` = Suno (long ambient
  beds, evolving texture loops), `FOL` = better recorded as real Foley if
  budget allows. This column is a hint only; the prompt pass finalizes it.

Matrices below enumerate their axes explicitly and give a computed file
count. Every file is derivable from the pattern + axes, so the matrix *is* the
detailed list — it just isn't expanded into hundreds of literal rows.

---

## 2. Folder structure (extends `AUDIO_DESIGN.md`)

```
assets/audio/sfx/
├── locomotion/      step_*, jump_*, climb_*, swim_*, armor_*  (player foley)
├── combat/          cmb_*  (player + enemy + impacts)         → SFX/Combat
├── voxel/           vox_*  (dig/mine/chop/place/explosive/collapse)
├── crafting/        craft_*  (forge, alchemy, carpentry, cooking, …)
├── items/           item_*  (pickup/drop/equip/container/torch/consumable)
├── environment/     wx_* (weather)  water_*  fire_*            → SFX/Ambient
├── ambience/        amb_<region>_<day|night>_loop  [stereo]    → SFX/Ambient
├── creatures/       wld_*  (ambient fauna, non-combat)
├── systems/         lock_* (EXISTING), minigame_*, invest_*
├── ui/              ui_*, journal_*, map_*, save_*, skill_*     → UI
├── npc/             npc_*  (non-verbal efforts/reactions/crowd) → Voice
└── economy/         econ_*  (coin, trade)
```
(`assets/audio/lockpicking/` and `assets/audio/dice/` already exist and stay;
new mini-game/system sounds may consolidate under `systems/` later.)

---

## 3. Category 01 — Player Locomotion & Foley

**Footstep matrix.** Gaits × surfaces × `var 5`.
Gaits (4): `walk`, `run`, `sprint`, `crouch`.
Surfaces (12): `grass`, `dirt`, `stone`, `wood`, `sand`, `gravel`,
`snow`, `mud`, `marsh`, `shallow_water`, `metal`, `cave_stone`.
→ `step_<gait>_<surface>_NN` = 4 × 12 × 5 = **240 files**.

**Jump / land matrix.** `jumpland_<surface>_NN`, 12 surfaces × var 5 = **60**.
Plus `jump_exert_grunt` (var 3), `land_heavy_stagger` (var 3), `land_soft`
(var 3). = **9**.

**Armor-weight movement layer** (KCD2-style, mixed over steps by equipped
weight): `armor_cloth_move_loop`, `armor_leather_move_loop`,
`armor_mail_move_loop`, `armor_plate_move_loop`; plus per-gait stress
one-shots `armor_<tier>_run_clank` ×4 tiers var 3 = 12. = **16**.

**Traversal foley:** `climb_rock_loop`, `climb_wood_loop`, `climb_grunt`
(var3), `ladder_step_wood_loop`, `ladder_step_iron_loop`,
`vault_ledge` (var3), `slide_scree` (var3), `water_wade_shallow_loop`,
`water_entry_walk` (var3), `water_entry_run_plunge` (var3),
`water_entry_fall_deep` (var3). ≈ **24**.

**Player breath/exertion** (Voice bus): `roland_breath_idle_loop`,
`roland_breath_exert_loop` (endurance low), `roland_breath_lowhp_loop`,
`roland_breath_critical_loop`, `roland_effort_grunt` (var5),
`roland_jump_exhale` (var3). ≈ **16**.

**Section 01 ≈ 365 files.**

---

## 4. Category 02 — Combat: Player

| ID | Desc | Type | var | Bus |
|---|---|---|---|---|
| cmb_sword_swing_light | fast sharp edge whoosh | one-shot | 5 | Combat |
| cmb_sword_swing_combo | chained 3-hit swing set | one-shot | 4 | Combat |
| cmb_sword_windup_power | breath + posture/cloth shift, charge | one-shot | 3 | Combat |
| cmb_sword_land_power | heavy block-breaking impact | one-shot | 4 | Combat |
| cmb_power_abort | charge released, no fire | one-shot | 2 | Combat |
| cmb_swing_miss_air | air-displacement whoosh + effort | one-shot | 5 | Combat |
| cmb_block_hold_loop | sustained clang + scrape while held | loop | 1 | Combat |
| cmb_block_impact | hit absorbed on raised block | one-shot | 5 | Combat |
| cmb_parry_success | clean high deflection ring | one-shot | 4 | Combat |
| cmb_riposte_strike | post-parry follow-up | one-shot | 3 | Combat |
| cmb_cue_parry_green | soft diegetic chime from enemy stance (300/345/375 ms windows) | cue | 2 | Combat |
| cmb_cue_heavy_yellow | tonal warning in enemy stance | cue | 2 | Combat |
| cmb_cue_unblock_red | low thud/growl from enemy stance | cue | 2 | Combat |
| cmb_dodge_roll | fabric/foot i-frame burst | one-shot | 4 | Combat |
| cmb_dodge_step | short side-step | one-shot | 4 | Combat |
| cmb_stagger_break | 0-endurance stumble + breath (1.5 s) | one-shot | 3 | Combat |
| cmb_endurance_empty | guard broken, gasp | one-shot | 3 | Combat |
| cmb_weapon_draw | scabbard out | one-shot | 3 | Combat |
| cmb_weapon_sheathe | scabbard in | one-shot | 3 | Combat |
| cmb_lockon_toggle | subtle target-lock tick | cue | 1 | UI |
| cmb_timeslow_enter / _exit | 0.15 s lethal-hit time-warp wash (pitch-bends with Engine.time_scale) | one-shot | 1 | Combat |

**Weapon-class foley matrix.** The universal verbs above are class-agnostic;
each weapon class needs its own swing/handling foley. Classes from
`ITEM_LIBRARY.md` (only **ThrowableSpear** is combat-shipped — the rest are
COMBAT_NEXT_PHASES growth slots, but authored here so the matrix is complete):

Melee classes (8): `dagger` (pierce, fast), `shortsword` (slash, fast),
`longsword` (slash — Roland's mainline / Chalice Knight / Spine-Forged /
Ashford), `twohander` (heavy slash — greatsword / Khorumzad waraxe, no combo
chain), `waraxe` (slash, anti-armor), `mace` (blunt, anti-armor — incl.
Irontrack Hammer), `flail` (blunt, bypasses block, long recovery), `spear`
(pierce + the shipped throwable).
Actions per melee class: `swing_light`(var5), `swing_heavy`(var4),
`swing_miss_air`(var4), `draw`(var3), `sheathe`(var3),
`block_hold_loop`(var1), `parry`(var4), `special`(var3 — e.g. flail chain
whirl, spear thrust, thornback serrated tear, greataxe block-break).
→ 8 classes × 27 = **216**.

`shield` (Act II+ — Iron Kite / Steel Tower): `cmb_shield_raise`(2),
`cmb_shield_lower`(2), `cmb_shield_block_absorb`(5), `cmb_shield_bash`(4) = **13**.

`bow` (Orion / ranged — roadmap, no Roland bow in G1, but accommodated):
`cmb_bow_nock`(3), `cmb_bow_draw_creak`(3), `cmb_bow_release`(4),
`cmb_bow_arrow_whir`(3), `cmb_bow_dryfire`(1) = **14**.

**Tier/condition timbre layer** (NOT a full re-record per weapon — a layer
mixed over the base, same approach as the hit-zone pitch layer):
`cmb_tier_common_layer`, `cmb_tier_quality_layer`,
`cmb_tier_masterwork_layer`, `cmb_condition_dull_layer`,
`cmb_condition_break_fail`. = **5**.

Throwables/explosives (player; spear shipped): `cmb_spear_windup`,
`cmb_spear_throw`, `cmb_spear_inflight_loop`, `cmb_spear_embed_flesh`,
`cmb_spear_embed_wood`, `cmb_spear_embed_stone`, `cmb_spear_retrieve`,
`cmb_throw_arc`, `cmb_bomb_pitch_detonate`, `cmb_oil_splash`,
`cmb_smoke_hiss`, `cmb_caltrop_scatter`, `cmb_flash_pop`,
`cmb_ashbane_torch_throw`, `cmb_venomtip_dart`, `cmb_trap_arm`,
`cmb_trap_snap`, `cmb_tripwire_trigger`. (var 3 each ≈ **55**)

Reserved growth-slot pattern for any future weapon class (polearm, crossbow,
etc. — not invented here since not in canon): reuse the melee-class action
set under `cmb_<class>_*`.

**Section 02 ≈ 35 universal + 216 weapon matrix + 13 shield + 14 bow + 5 tier
layer + 55 throwables = ≈ 338 files.**

---

## 5. Category 03 — Combat: Impacts & Enemies

**Impact matrix** (hit reaction by damage type × what's hit — grounded in the
armor interactions named in `ITEM_LIBRARY.md`: mail resists cuts, blunt
crunches through mail, plate deflects). Damage types (5): `slash`, `pierce`,
`blunt`, `serrated` (bleed/tear), `arrow`. Targets (7): `flesh_unarmored`,
`flesh_padded`, `mail`, `plate`, `shield`, `wood`, `stone_terrain`.
`cmb_hit_<dmgtype>_<target>_NN` = 5 × 7 × var 4 = **140**. Hit-zone timbre
(head 2.0× sharper / torso / limb dampened) handled by a pitch layer, not
new files. Powder/sapper structural hits route to the voxel ejecta set
(Cat 04), not here.

**Per-enemy sets** (each: idle, alert, attack ×n, hurt, death, footstep,
specials). var defaults applied:

- **Goblin:** `cmb_goblin_idle_chatter`(5), `_alert_shout`(3),
  `_group_alert`(2), `_attack_jab`(4), `_attack_leap`(3), `_hurt`(5),
  `_death`(4), `_gib_overkill`(2, ≥80 dmg), `_flee`(3),
  `_footstep_loop`(1). ≈ **32**
- **Ashfallen** (faceless, armored — no voice, armor foley): `_footstep_heavy`(5),
  `_armor_creak_idle_loop`(1), `_telegraph_measured`(2),
  `_telegraph_heavy`(2), `_telegraph_thrust_red`(2), `_shield_bash`(3),
  `_hurt_clang_chip`(5), `_death_collapse`(3), `_blade_drop`(2). ≈ **25**
- **Wolf:** `_breath_pant_loop`(1), `_undergrowth_move`(4), `_alert_growl`(3),
  `_lunge_windup`(3), `_bite`(4), `_yelp_hurt`(4), `_death`(3),
  `_paw_steps_loop`(1). ≈ **23**
- **Bear:** `_growl_idle_loop`(1), `_charge_telegraph`(2), `_run_thunder`(3),
  `_claw_swipe`(4), `_bite`(3), `_rear_roar`(2, <30 % HP), `_slam`(3),
  `_footfall_heavy`(5), `_hurt_deep`(4), `_death_heavy`(3). ≈ **30**
- **Generic future-enemy stubs** (slot for Ashen Hand humans, Naergrim,
  beasts — TBD per `COMBAT_NEXT_PHASES.md`): reserve `cmb_<enemy>_*` family.

**Section 03 ≈ 140 impact + ≈110 enemy = ≈ 250 files** (+ growth slots).

---

## 6. Category 04 — Tools & Voxel Interaction

**Tool × material strike matrix.** The core Minecraft-like set, but with
KCD2 tool realism + a "wrong tool" dull variant.

Tools (4): `pick`, `shovel`, `axe`, `hands`.
Materials (18, from `MINING_TIME_SCALING.md` / `ITEM_LIBRARY.md`):
`grass`, `dirt`, `sand`, `clay`, `mud`, `ash`, `gravel`, `stone`,
`worked_stone`, `sandstone`, `iron_ore`, `steel_ore`, `adamant_ore`,
`coal`, `copper_ore`, `wood`, `hardwood`, `leaves`.
Events per pair: `strike` (mid-dig loop hit, var 5), `break` (voxel
destroyed, var 4), `place` (voxel set, var 3).

Not every tool×material is "correct" — but the wrong-tool dull-scrape still
needs a sound. To stay tractable: generate **correct-tool** strike/break/place
fully, and **one shared wrong-tool** scrape per material.
- Correct-tool pairs ≈ 18 materials × (strike5 + break4 + place3) = **216**
- Wrong-tool: `vox_wrongtool_<material>` 18 × var3 = **54**
- `vox_bedrock_blocked` (unbreakable thunk) var3 = 3
→ subtotal **273**.

**Edit verbs / world physics:**
`vox_dig_loop_<soft|hard>` (continuous while held, 2),
`vox_chop_tree_fell` (3), `vox_tree_topple_<wood|hardwood>` (4),
`vox_log_resolve` (2), `vox_place_schematic_confirm` (1),
`vox_place_reject_noeditzone` (1), `vox_carve_volume_cycle` (cue,1),
`vox_powdercharge_fuse` (1), `vox_powdercharge_blast` (3),
`vox_sapper_blast_heavy` (3), `vox_gravity_creak_warn` (2),
`vox_cluster_collapse` (4), `vox_cluster_impact_ground` (4),
`vox_cluster_impact_water` (3), `vox_buildmode_ghost_appear` (1),
`vox_buildmode_snap_click` (1), `vox_buildmode_reject` (1).
≈ **40**.

**Section 04 ≈ 313 files.**

---

## 7. Category 05 — Crafting & Stations

| Station | Sounds (var) |
|---|---|
| **Smithing Forge** | bellows_loop(1), fire_roar_loop(1), hammer_anvil_peak(5), hammer_anvil_weak(4), quench_hiss(3), ingot_deform_thunk(4), reheat_whoosh(2), scale_sizzle(2), tier_up_resolve(1) |
| **Grindstone** | wheel_loop(1), spark_burst(4), sharpen_complete(1) |
| **Alchemist's Still** | bubble_steep_loop(1), distill_drip_loop(1), vial_cork_seal(3), foul_residue_hiss(2), potion_complete(1) |
| **Carpentry Bench** | saw_stroke(5), plane_shave(4), wood_hammer(5), schematic_complete(1) |
| **Cooking Fire** | sizzle_loop(1), boil_loop(1), stir(3), meal_complete(1) |
| **Assembly Table** | bind_wrap(4), component_click(4), device_complete(1) |
| **Generic** | craft_quick_confirm(1), craft_care_confirm(1), craft_mastery_confirm(1), recipe_learned_chime(1) |

≈ **70 files** (`craft_*`).

---

## 8. Category 06 — Interactive Objects & Items

- **Pickup/drop:** `item_pickup_generic`(5), `item_pickup_metal`(3),
  `item_pickup_cloth`(3), `item_pickup_potion`(3), `item_pickup_voxeldrop`(3),
  `item_drop`(4). ≈ 21
- **Equip/unequip by slot:** weapon(3), shield(3), torch_offhand(2),
  head(3), body_cloth(3), body_mail(3), body_plate(3), hands(3), boots(3),
  twohander_offhand_clear_warn(1). ≈ 30
- **Inventory:** `item_slot_move`(3), `item_quickslot_assign`(2),
  `item_grid_drag`(2), `inventory_open`(1), `inventory_close`(1). ≈ 9
- **Containers/doors:** `door_open_<wood|heavy|archive|shack>`(×4 var3),
  `door_close_*`(×4 var3), `chest_open`(3), `chest_close`(3),
  `chest_locked_rattle`(2), `lid_creak`(3), `cache_open`(2),
  `corpse_loot_rustle`(4), `drawer_slide`(3), `gate_iron`(3). ≈ 50
- **Torch:** `item_torch_light`(2), `item_torch_burn_loop`(1),
  `item_torch_extinguish`(2), `brazier_loop`(1). ≈ 6
- **Consumables:** `item_bandage_tear`(3), `item_potion_gulp`(3),
  `item_coating_apply`(2), `item_food_eat`(4), `item_drink_skin`(3). ≈ 15
- **Repair/maintain:** `item_whetstone_scrape`(4), `item_repairkit_use`(3),
  `item_condition_warn`(1), `item_break_dull`(2). ≈ 10
- **Quick-slot:** `item_quickslot_cycle`(cue1), `item_quickslot_activate`(2),
  `item_quickslot_empty`(1). ≈ 4
- **Wanderer's Seal save:** `save_wax_seal`(1, understated, no jingle). 1

**Section 06 ≈ 146 files.**

---

## 9. Category 07 — Environment: Weather

States from `WEATHER_AND_ENVIRONMENT.md`: `clear`, `overcast`,
`light_rain`, `heavy_rain`, `fog`, `snow`, `ash_haze`, plus lightning.

- **Rain-on-surface loops:** `wx_rain_<light|heavy>_<soil|stone|foliage|water|wood_roof>_loop` = 2 × 5 = 10 [stereo]
- **Wind tiers:** `wx_wind_<calm|breeze|rain|storm|lethal>_loop` = 5 [stereo]
- **Thunder/lightning:** `wx_thunder_distant`(4), `wx_thunder_near_crack`(3),
  `wx_lightning_prestrike_hum`(2). ≈ 9
- **State beds:** `wx_<state>_bed_loop` ×7 [stereo] = 7
- **Transitions:** `wx_storm_front_approach`(2), `wx_rain_onset_ramp`(1),
  `wx_rain_tailoff`(1), `wx_weather_swell_30s`(1). ≈ 5
- **Snow/ash:** `wx_snowfall_hiss_loop`(1), `wx_ashfall_whisper_loop`(1),
  `wx_ash_grit_gust`(3). ≈ 5

**Section 07 ≈ 41 files.**

---

## 10. Category 08 — Environment: Water

`water_swim_surface_loop`, `water_swim_paddle_loop`,
`water_swim_submerged_loop`, `water_submerge_plunge`(3),
`water_surface_gasp`(3), `water_underwater_ambient_loop` [stereo],
`water_bubble_trail_loop`, `water_splash_small`(5),
`water_splash_medium`(4), `water_splash_large`(3),
`water_river_flow_loop` [stereo], `water_brook_trickle_loop`,
`water_channel_rush_loop` (player-dug), `water_surf_cliff_loop` [stereo],
`water_harbor_lap_loop`, `water_drip_single`(5),
`water_drip_cluster_loop`, `water_runoff_postrain_loop`,
`water_wave_shroud_boundary_loop` [stereo].
**Section 08 ≈ 26 files.**

---

## 11. Category 09 — Fire & Camp

`fire_campfire_crackle_loop` [stereo], `fire_ember_pop`(5),
`fire_log_settle`(3), `fire_ignite_whoosh`(3), `fire_tinder_kindle`(2),
`fire_extinguish_hiss`(2), `fire_smoke_fade`(1),
`fire_torch_flutter_loop`, `fire_brazier_loop` [stereo],
`camp_rest_fade_sting`(1), `camp_rest_autosave_chime`(1).
**Section 09 ≈ 19 files.**

---

## 12. Category 10 — Region Ambient Beds

Per-region, day + night, seamless **[stereo]** loops. Regions from
`lore/WORLD.md` (16): Central Plains, Spine of the World, The Underway,
The Greatwood, Western Coast, The Ashfields, The Sorrowmarsh, The Weeping
Wood, Mor-Vethrin, Aldenholt, Solgrade, Vosskara, Caer Brannoch, Lirien-Thal,
Copper Isles, Shroud Sea boundary.

`amb_<region>_day_loop` + `amb_<region>_night_loop` = 16 × 2 = **32**.
The Underway, Weeping Wood, Mor-Vethrin and Shroud-boundary use one
day/night-identical loop (still authored as 2 for transition smoothness).
Each bed carries its layer recipe (e.g. Greatwood day = filtered birdsong +
leaf-rustle + silverwood low hum) — recipes captured at prompt time.

**Sub-ambience one-shots** (randomized over beds, Minecraft "cave sound"
style): `amb_bird_call`(8 across species), `amb_owl`(3), `amb_insect_chirp`(5),
`amb_raptor_cry`(3), `amb_crow`(3), `amb_distant_wolf`(3),
`amb_distant_goblin_clatter`(3), `amb_ghostlight_shimmer`(3, Sorrowmarsh),
`amb_naergrim_whisper`(4, Weeping Wood), `amb_rockfall_tick`(3, Spine),
`amb_silverwood_hum`(1, Greatwood), `amb_stone_groan`(3, Underway),
`amb_bone_arch_resonance`(2, Mor-Vethrin), `amb_creak_dead_stump`(3,
Ashfields), `amb_foghorn_moan`(2, coast), `amb_settlement_dog`(3),
`amb_settlement_livestock`(4), `amb_distant_smith`(2),
`amb_rigging_creak`(3, ports). ≈ **66**.

**Section 10 ≈ 98 files.**

---

## 13. Category 11 — Day/Night & Time Cues

`time_dawn_birdsong_swell`(1), `time_dawn_settlement_wake`(2),
`time_dusk_birdsong_fade`(1), `time_dusk_fires_lit_murmur`(1),
`time_night_onset_layer_in`(1), `time_day_onset_layer_out`(1),
`time_period_boundary_sting`(1, ties to zone-music swap),
`worldclock_hour_chime_distant`(2, settlement bell).
**Section 11 ≈ 10 files.**

---

## 14. Category 12 — Systems: Lockpicking

EXISTING (6, on disk): `lock_false_hum`, `lock_sweep_loop`,
`lock_pin_set`, `lock_pick_snap`, `lock_open`, `lock_resonance_tone`.

NEEDED gaps: `lock_approach_swell` (rising metallic hum near pin),
`lock_false_stall` (set-bar stalls at half), `lock_backpressure_warn`
(sweep-too-fast tension creak), `lock_overlay_open`, `lock_overlay_close`,
`lock_key_turn_unlock` (silent/fast key path). ≈ **6 NEEDED**.

---

## 15. Category 13 — Systems: Mini-Games

- **Bones (dice)** — EXISTING (8): `dice_throw, dice_shake, dice_settle,
  dice_lock, coin_clink, win_chime, lose_thud, tavern_ambient`. NEEDED:
  `dice_reveal`, `dice_wager_place`, `dice_read_tell_sting`. (3)
- **Smithing minigame:** reuses `craft_*` forge set; add
  `minigame_smith_combo_streak`(2), `minigame_smith_fail_warp`(2). (4)
- **Fishing:** `fish_cast_whir`, `fish_line_release`, `fish_float_plop`,
  `fish_float_bob_loop`, `fish_strike_dip`, `fish_reel_tug`(3),
  `fish_line_tension_creak`, `fish_line_snap`, `fish_leap_splash`(3),
  `fish_net_land`, `fish_night_shimmer`. ≈ 15
- **The Fold (cards):** `card_deal`(3), `card_flip`(3),
  `card_facedown_place`, `trick_sweep`, `fold_reveal_sting`. ≈ 9
- **Axe throwing:** `axethrow_breath_loop`, `axethrow_release`(3),
  `axethrow_thunk_target`(4), `axethrow_miss_clatter`(3),
  `axethrow_bullseye_chime`, `axethrow_swing_creak`(2, trick round),
  `crowd_cheer`(3), `crowd_groan`(3). ≈ 20
- **Archery comp:** `archery_target_thock`(4), `clay_bird_launch`(3),
  `clay_bird_shatter`(3), `archery_streak_tick`, `archery_miss_whiff`(3).
  (reuses combat bow) ≈ 14
- **Herbalism/foraging:** `forage_aura_loop`, `forage_approach_pitch_rise`,
  `forage_startle_close`(2), `forage_hold_loop`,
  `forage_success_pluck_rare`(2), `forage_grab_common`(4). ≈ 11
- **Arm wrestling:** `armwr_strain_loop`, `armwr_surge_grunt`(3),
  `armwr_force_tick`, `armwr_win_slam`, `armwr_lose_slam`. ≈ 7
- **Voxel sculpture contest:** reuses pickaxe carve; add
  `sculpt_match_tick`, `sculpt_timer_warn`, `sculpt_verdict_sting`. (3)

**Section 13 ≈ 86 NEEDED + 14 EXISTING.**

---

## 16. Category 14 — Investigation & Clue

`invest_examine_foley`(3), `invest_clue_shimmer_loop`,
`invest_text_appear_whisper`(2), `invest_noted_quill_stamp`(1),
`invest_deduction_sting`(1), `invest_companion_chime`(1),
`invest_saturation_exhausted`(1, minimal). **Section 14 ≈ 10 files.**

---

## 17. Category 15 — UI, Menu & Feedback

Per `AUDIO_DESIGN.md`: minimal, dry, short, **no fanfare, no level-up jingle**.

`ui_navigate`(2), `ui_confirm`(1), `ui_cancel`(1), `ui_error_thud`(1),
`journal_open`(1), `journal_close`(1), `journal_page_turn`(3),
`map_open`(1), `map_trace_ink`(2), `quickslot_cycle`(cue1),
`quickslot_activate`(2), `inventory_open`(1), `inventory_close`(1),
`pause_open`(1), `pause_close`(1), `interaction_prompt_appear`(1, very
subtle), `bark_overlay_appear`(1), `skill_node_tone`(1, single soft tone),
`skill_legendary_reset_tone`(1), `quest_update_soft`(1),
`objective_complete_soft`(1). **Section 15 ≈ 25 files.**

---

## 18. Category 16 — Death & Respawn

`death_collapse_thud`(2), `death_near_breath_loop`(1),
`death_near_heartbeat_loop`(1), `death_vignette_rumble_loop`(1),
`death_second_wind_cue`(1, perk fires), `death_fade_tone`(1, no "YOU
DIED"), `death_return_load_soft`(1). **Section 16 ≈ 8 files.**

---

## 19. Category 17 — NPC Non-Verbal & Crowd

(Voice bus; distinct from TTS dialogue.) Generic male/female sets:
`npc_effort_grunt_m/f`(×2 var5=10), `npc_react_surprise`(3),
`npc_react_scoff`(3), `npc_react_laugh`(4), `npc_react_cough`(3),
`npc_react_sigh`(3), `npc_react_gasp`(3), `npc_footstep_approach`(3),
`npc_bark_pre_breath`(2), `crowd_market_murmur_loop` [stereo],
`crowd_tavern_murmur_loop` [stereo], `crowd_court_murmur_loop` [stereo],
`vendor_callout_nonverbal`(4), `guard_challenge_nonverbal`(3),
`child_play_distant_loop`. **Section 17 ≈ 55 files.**

---

## 20. Category 18 — Economy & Vendor

`econ_coin_count_loop`, `econ_coin_pickup_single`(3),
`econ_coin_pickup_purse`(2), `econ_trade_confirm`(1),
`econ_trade_decline`(1, reuse `ui_error`), `econ_vendor_open`(1),
`econ_vendor_close`(1). (`coin_clink` in `dice/` is a reuse candidate.)
**Section 20 ≈ 9 files.**

---

## 21. Category 19 — Magic & Spellcraft

**Canon constraint** (`lore/WORLD.md` Magic): magic is rare (~1 in 10,000
useful), costly, and *never triumphant*. **Game One has no player caster** —
the party mage (Corvus) joins Game Two; G1 magic is villain-side,
environmental, and the Aeluvain *referenced, not wielded*. Sound reads as
strained / austere / wrong — never a "fireball whoosh-boom." Tags: `[G1]`
shippable now, `[RM]` roadmap (G2/G3).

**Casting foley** (the cost cue is mandatory — every working tolls the caster):
`mag_charge_gather`(3)[RM], `mag_release_innate`(3)[RM],
`mag_channel_loop`(1)[RM], `mag_fizzle_fail`(2)[RM],
`mag_cost_toll`(3, Voice bus — gasp + cranial tinnitus swell + ragged
breath)[RM], `mag_cost_alteration_sting`(1, permanent-alteration
threshold)[RM], `mag_ritual_build_loop`(1)[G1 ambient],
`mag_ritual_against_grain_loop`(1, detuned world-thinning)[G1 ambient].
≈ **15**

**Spell-effect impacts** (lore-faithful schools only — no elemental laundry
list): `mag_env_temp_shift`(2), `mag_env_pressure_pop`(2),
`mag_env_stormcharge_hum`(2)[RM]; `mag_structural_perceive_loop`(1, Corvus)[RM];
`mag_blight_creep_loop`(1)[G1 — Ashfields/Sorrowmarsh];
`mag_ward_raise`(2)/`mag_ward_hold_loop`(1)/`mag_ward_break`(3)[RM];
`mag_aeluvain_hum_loop`(1, pure cold missing-note tone)[G1 referenced];
`mag_wrongness_pressure_loop`(1, Mordvar/Ashlord proximity dread)[G1 Act IV].
≈ **16**

**Implements:** `mag_staff_focus_tap`(3), `mag_staff_thrum_loop`(1)[RM];
`mag_item_activate`(3, enchanted object wakes — Crown piece/relic)[G1];
`mag_rune_circle_ignite`(2)/`mag_rune_circle_loop`(1)[G1 ritual set-piece];
`mag_aeluvain_unsheathe`(1)/`mag_aeluvain_strike`(2)/
`mag_aeluvain_song_complete`(1)[RM G3]. ≈ **14**

**Enemy / villain casters:** `mag_ashfallen_cast`(3, clipped joyless
cost-bearing)[G1]; `mag_hand_ritual_chant_loop`(1, non-verbal, not TTS)[G1];
`mag_ashlord_presence_loop`(1, oppressive pressure field)[G1 Act IV];
`mag_ashlord_unmask_sting`(1)[RM G2]; `mag_mordvar_ambient_loop`(1,
world-thinning, no discrete cast)[RM G3]. ≈ **7**

Bus: ambient/loop magic → `SFX/Ambient`; cast & impact one-shots →
`SFX/Combat`; the cost gasp → `Voice`. **Section 19 ≈ 52 files** — but only
~10 are `[G1]`-shippable (the ritual/blight/wrongness/Ashlord ambient beds,
the Aeluvain hum reference, enchanted-item activation). The rest are roadmap.

---

## 22. Count rollup

| # | Category | ≈ Files |
|---|---|---|
| 01 | Player Locomotion & Foley | 365 |
| 02 | Combat: Player (+ weapon-class matrix) | 338 |
| 03 | Combat: Impacts (damage-type matrix) & Enemies | 250 |
| 04 | Tools & Voxel Interaction | 313 |
| 05 | Crafting & Stations | 70 |
| 06 | Interactive Objects & Items | 146 |
| 07 | Environment: Weather | 41 |
| 08 | Environment: Water | 26 |
| 09 | Fire & Camp | 19 |
| 10 | Region Ambient Beds | 98 |
| 11 | Day/Night & Time | 10 |
| 12 | Lockpicking | 6 needed (+6 existing) |
| 13 | Mini-Games | 86 needed (+14 existing) |
| 14 | Investigation & Clue | 10 |
| 15 | UI, Menu & Feedback | 25 |
| 16 | Death & Respawn | 8 |
| 17 | NPC Non-Verbal & Crowd | 55 |
| 18 | Economy & Vendor | 9 |
| 19 | Magic & Spellcraft | 52 (~10 G1, rest roadmap) |
| | **Grand total** | **≈ 1,930 files** (~20 EXISTING) |

The number is large because of four combinatorial sets — footsteps (240),
the tool×material voxel matrix (273), the weapon-class matrix (216), and the
damage-type impact + enemy sets (~250). These can be **phased**: a playable
vertical slice needs only the live content (4 wired voxel materials, the 4
implemented enemies, the shipped spear, core surfaces, combat core,
camp/weather basics — magic is almost entirely roadmap) ≈ **300–400 files**;
the rest fills in as systems wire up.

---

## 23. Suggested generation phases (for the prompt pass)

1. **Combat & locomotion core** — Cat 02 universal verbs + the shipped
   ThrowableSpear + Roland's longsword class + the 4 implemented enemies in
   03 + the live surfaces in 01. The game is played here; do this first.
   (Other weapon classes follow as they're implemented per COMBAT_NEXT_PHASES.)
2. **Voxel & tools** — Cat 04 for the 4 wired materials (sand/dirt/grass/
   stone) + bedrock, then expand per material as wired.
3. **Camp, weather, water basics** — Cat 09, the clear/rain/wind of 07, the
   swim/splash core of 08.
4. **Region beds** — Cat 10, prioritised by Act I locations (Aldenholt,
   Central Plains, the Archive interior).
5. **Systems & UI** — Cat 12–18, lockpicking gaps + dice extras first
   (mini-games already partly on disk).
6. **Long tail** — remaining weapon classes, materials, enemies, mini-games,
   and the player-side magic set (Cat 19 `[RM]`) as systems land. The G1
   magic ambient beds (blight/wrongness/Ashlord) ride with Phase 4 region
   beds since they're environmental.

Gen-source hint for the prompt pass: one-shot foley/impacts (Cat 01–06, 16,
17 efforts) → **ElevenLabs SFX**; long evolving ambient/weather/region beds
(Cat 07, 09, 10, crowd loops) → **Suno** (instrumental, like the music doc's
texture cues); UI (Cat 15) → ElevenLabs, kept dry and minimal. Recorded Foley
is the quality ceiling for footsteps/weapon if budget ever allows.

---

## 24. Maintenance

- When a new system, enemy, weapon class, voxel material, station, mini-game,
  magic school, region, or weather state is designed, add its row/matrix here
  **before** writing its prompts, and update the §22 rollup.
- Keep naming consistent with the on-disk `lock_*` / `dice_*` precedent.
- When a file is generated and committed, flip its status to EXISTING here.
- Cross-update `AUDIO_DESIGN.md` if the bus routing or folder structure
  changes; cross-update `CLAUDE.md`'s maintenance table if scope shifts.
