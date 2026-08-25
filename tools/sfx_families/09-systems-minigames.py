# Categories 12, 13, 14 -- Lockpicking, Mini-Games, Investigation
#                                   docs/sfx-library.md sections 14, 15, 16
#
# Lockpicking already has 6 sounds on disk in the Godot repo (lock_false_hum,
# lock_sweep_loop, lock_pin_set, lock_pick_snap, lock_open, lock_resonance_tone)
# and NONE of them are in voxelsim. They are listed here anyway: this spec is
# the full inventory, and the generator decides what needs a prompt by looking
# at the prompts doc, not at what happens to exist in another repository.

add(
    loop_("lock_sweep_loop", "a lockpick sweeping slowly across pin stacks, a continuous fine metallic scrape", 12, 0.42, "SFX", "12"),
    loop_("lock_false_hum", "a false set holding under tension, a faint sustained metallic hum that is almost right", 10, 0.40, "SFX", "12"),
    one("lock_pin_set", "A lock pin setting into place, a small precise metallic click, %s" % DRY, 0.3, 0.50, 3, "SFX", "12"),
    one("lock_pick_snap", "A lockpick snapping under too much tension, a thin sharp metallic break, %s" % DRY, 0.4, 0.50, 3, "SFX", "12"),
    one("lock_open", "A lock giving way and turning open, pins dropping and the mechanism rotating free, %s" % DRY, 1.0, 0.45, 2, "SFX", "12"),
    one("lock_resonance_tone", "A faint resonant tone rising from a lock as the correct tension is found, diegetic and metallic, %s" % DRY, 1.5, 0.40, 2, "SFX", "12"),
    one("lock_approach_swell", "A rising metallic hum as a lockpick nears a correct pin, tension building without resolving, %s" % DRY, 1.5, 0.40, 2, "SFX", "12"),
    one("lock_false_stall", "A set bar stalling at half travel, a small dead metallic stop that refuses to go further, %s" % DRY, 0.6, 0.45, 2, "SFX", "12"),
    one("lock_backpressure_warn", "A lock creaking under too much tension too fast, a strained metallic warning before anything breaks, %s" % DRY, 0.8, 0.45, 2, "SFX", "12"),
    one("lock_overlay_open", "A lockpicking view opening, a soft close-in metallic settle as attention narrows to the lock, %s" % DRY, 0.7, 0.40, 1, "UI", "12"),
    one("lock_overlay_close", "A lockpicking view closing, tools withdrawn and the world opening back out, %s" % DRY, 0.7, 0.40, 1, "UI", "12"),
    one("lock_key_turn_unlock", "A key turning a lock open cleanly, a solid mechanical rotation and bolt withdrawing, %s" % DRY, 1.0, 0.45, 1, "SFX", "12"),
)

# --- Category 13: Mini-games ----------------------------------------------
# Bones (dice) has 8 existing sounds in the Godot repo; the NEEDED three are
# listed with them for the same reason as lockpicking above.
add(
    one("dice_throw", "A handful of bone dice thrown onto a wooden table, a scattering tumble coming to rest, %s" % DRY, 1.4, 0.45, 3, "SFX", "13"),
    one("dice_shake", "Bone dice shaken in a cupped hand, a fast dry rattling, %s" % DRY, 1.2, 0.42, 3, "SFX", "13"),
    one("dice_settle", "The last die rocking to a stop on wood, two small taps and stillness, %s" % DRY, 0.8, 0.45, 3, "SFX", "13"),
    one("dice_lock", "A die pushed aside and locked out of play, a short deliberate wooden slide and stop, %s" % DRY, 0.5, 0.45, 3, "SFX", "13"),
    one("dice_reveal", "Dice uncovered under a cup lifted away, a wooden lift with the dice revealed beneath, %s" % DRY, 0.9, 0.42, 3, "SFX", "13"),
    one("dice_wager_place", "Coins pushed forward as a wager, a small stack sliding across a table and settling, %s" % DRY, 0.9, 0.45, 3, "SFX", "13"),
    one("dice_read_tell_sting", "A very short understated tonal marker as an opponent gives something away, diegetic and restrained, %s" % DRY, 0.8, 0.40, 2, "SFX", "13"),
    one("minigame_smith_combo_streak", "Three well-timed hammer blows landing in sequence on an anvil, rising in confidence, %s" % DRY, 1.8, 0.45, 2, "SFX", "13"),
    one("minigame_smith_fail_warp", "A hammer blow going badly wrong, metal deforming with a dull tearing groan, %s" % DRY, 1.2, 0.45, 2, "SFX", "13"),
    one("fish_cast_whir", "A fishing line cast out, a fast whirring release of line through rings, %s" % DRY, 1.2, 0.42, 1, "SFX", "13"),
    one("fish_line_release", "A fishing line let go to run free, a light continuous ticking release, %s" % DRY, 1.0, 0.40, 1, "SFX", "13"),
    one("fish_float_plop", "A float landing on still water, a single small plop with rings spreading, %s" % DRY, 0.7, 0.42, 1, "SFX", "13"),
    loop_("fish_float_bob_loop", "a float bobbing gently on water, small irregular laps against it", 14, 0.30, "SFX", "13"),
    one("fish_strike_dip", "A float pulled sharply under, a sudden decisive dip and splash, %s" % DRY, 0.6, 0.45, 1, "SFX", "13"),
    one("fish_reel_tug", "A fish fighting against the line, a straining tug with the rod flexing, %s" % DRY, 1.2, 0.42, 3, "SFX", "13"),
    one("fish_line_tension_creak", "A fishing line and rod under dangerous tension, a thin creaking strain, %s" % DRY, 1.4, 0.42, 1, "SFX", "13"),
    one("fish_line_snap", "A fishing line breaking, a sharp thin snap and the tension gone, %s" % DRY, 0.5, 0.48, 1, "SFX", "13"),
    one("fish_leap_splash", "A fish breaking the surface and falling back, a bright leaping splash, %s" % DRY, 1.0, 0.42, 3, "SFX", "13"),
    one("fish_net_land", "A fish landed in a net, wet thrashing in mesh coming to rest, %s" % DRY, 1.6, 0.40, 1, "SFX", "13"),
    one("fish_night_shimmer", "A faint cold shimmering tone over night water, understated and strange, %s" % DRY, 2.5, 0.30, 1, "SFX", "13"),
    one("card_deal", "A playing card dealt onto a table, a single light slide and settle, %s" % DRY, 0.4, 0.45, 3, "SFX", "13"),
    one("card_flip", "A card turned face up, a short crisp flip against the table, %s" % DRY, 0.3, 0.48, 3, "SFX", "13"),
    one("card_facedown_place", "A card placed face down deliberately, a quiet controlled press onto the table, %s" % DRY, 0.4, 0.45, 1, "SFX", "13"),
    one("trick_sweep", "A won trick swept in across the table, several cards gathered and pulled close, %s" % DRY, 0.9, 0.42, 1, "SFX", "13"),
    one("fold_reveal_sting", "A very short understated tonal marker as a hand is revealed, diegetic and restrained, %s" % DRY, 0.9, 0.40, 1, "SFX", "13"),
    loop_("axethrow_breath_loop", "a person steadying their breathing before a throw, slow controlled breaths", 10, 0.32, "Voice", "13"),
    one("axethrow_release", "A throwing axe released, a grunt of effort and the axe leaving the hand tumbling, %s" % DRY, 0.8, 0.42, 3, "SFX", "13"),
    one("axethrow_thunk_target", "A throwing axe biting into a wooden target, a hard splitting thock with the haft quivering, %s" % DRY, 0.9, 0.48, 4, "SFX", "13"),
    one("axethrow_miss_clatter", "A throwing axe missing and clattering away across the ground, %s" % DRY, 1.4, 0.45, 3, "SFX", "13"),
    one("axethrow_bullseye_chime", "A throwing axe hitting dead centre, a hard bite with one clean ring off the head, %s" % DRY, 1.0, 0.45, 1, "SFX", "13"),
    one("axethrow_swing_creak", "A trick throw wound up slowly, the haft creaking under a held rotation, %s" % DRY, 1.2, 0.40, 2, "SFX", "13"),
    one("crowd_cheer", "A small crowd cheering a good throw, brief and genuine, no words, %s" % DRY, 2.0, 0.35, 3, "Voice", "13"),
    one("crowd_groan", "A small crowd groaning at a bad throw, brief and disappointed, no words, %s" % DRY, 1.8, 0.35, 3, "Voice", "13"),
    one("archery_target_thock", "An arrow striking a straw archery butt, a dense packed thock, %s" % DRY, 0.6, 0.48, 4, "SFX", "13"),
    one("clay_bird_launch", "A clay target flung from a thrower, a fast mechanical release and the disc whirring away, %s" % DRY, 1.0, 0.45, 3, "SFX", "13"),
    one("clay_bird_shatter", "A clay target shattering in mid air, a sharp brittle burst with fragments falling, %s" % DRY, 1.2, 0.48, 3, "SFX", "13"),
    one("archery_streak_tick", "A very short dry tick marking a scoring streak, minimal, %s" % DRY, 0.2, 0.50, 1, "SFX", "13"),
    one("archery_miss_whiff", "An arrow passing wide of a target, a fast whiff past with no impact, %s" % DRY, 0.6, 0.42, 3, "SFX", "13"),
    loop_("forage_aura_loop", "a faint natural shimmer marking something worth finding nearby, understated and organic", 12, 0.30, "SFX", "13"),
    one("forage_approach_pitch_rise", "A faint natural tone rising slowly as a search closes in, understated, %s" % DRY, 2.5, 0.32, 1, "SFX", "13"),
    one("forage_startle_close", "Something small bolting away through undergrowth at close range, a sudden startling rush, %s" % DRY, 1.2, 0.40, 2, "SFX", "13"),
    loop_("forage_hold_loop", "a hand held steady over a plant, tiny sustained leaf and stem contact", 10, 0.30, "SFX", "13"),
    one("forage_success_pluck_rare", "A rare plant taken cleanly, a careful stem snap with a soft settling of leaves, %s" % DRY, 1.0, 0.40, 2, "SFX", "13"),
    one("forage_grab_common", "A common plant pulled up, a quick tearing of stem and root from soil, %s" % DRY, 0.8, 0.40, 4, "SFX", "13"),
    loop_("armwr_strain_loop", "two people straining against each other at arm wrestling, sustained tremor of effort and creaking table", 10, 0.35, "Voice", "13"),
    one("armwr_surge_grunt", "A hard surge of effort in a contest of strength, a rising grunt pushing through, no words, %s" % DRY, 1.2, 0.35, 3, "Voice", "13"),
    one("armwr_force_tick", "A very short dry tick marking force shifting in a contest, minimal, %s" % DRY, 0.2, 0.50, 1, "SFX", "13"),
    one("armwr_win_slam", "An arm driven down onto a table in victory, a hard flat slam with the table jumping, %s" % DRY, 0.7, 0.48, 1, "SFX", "13"),
    one("armwr_lose_slam", "An arm forced down onto a table in defeat, a hard flat slam with a defeated exhale after, %s" % DRY, 1.0, 0.45, 1, "SFX", "13"),
    one("sculpt_match_tick", "A very short dry tick marking time passing in a contest, minimal, %s" % DRY, 0.2, 0.50, 1, "SFX", "13"),
    one("sculpt_timer_warn", "A low understated warning tone as contest time runs short, diegetic and restrained, %s" % DRY, 1.0, 0.42, 1, "SFX", "13"),
    one("sculpt_verdict_sting", "A short understated tonal marker as a verdict is given, restrained, no fanfare, %s" % DRY, 1.2, 0.40, 1, "SFX", "13"),
)

# --- Category 14: Investigation & Clue ------------------------------------
add(
    one("invest_examine_foley", "An object being examined closely, turned in the hands with small handling sounds, %s" % DRY, 1.6, 0.38, 3, "SFX", "14"),
    loop_("invest_clue_shimmer_loop", "a faint sustained shimmer marking something worth attention, very restrained, almost subliminal", 12, 0.28, "SFX", "14"),
    one("invest_text_appear_whisper", "Faint paper and quill whisper as written text is read out of a document, no words, %s" % DRY, 1.4, 0.32, 2, "SFX", "14"),
    one("invest_noted_quill_stamp", "A note recorded, a short quill stroke and a soft stamp onto paper, %s" % DRY, 1.0, 0.42, 1, "SFX", "14"),
    one("invest_deduction_sting", "A short understated tonal marker as pieces connect, one low clean tone, no fanfare, %s" % DRY, 1.2, 0.40, 1, "SFX", "14"),
    one("invest_companion_chime", "A very soft low tone marking a companion noticing something, restrained and diegetic, %s" % DRY, 0.9, 0.40, 1, "SFX", "14"),
    one("invest_saturation_exhausted", "A minimal dry tone marking that a scene has nothing further to give, understated to the point of near silence, %s" % DRY, 0.8, 0.42, 1, "SFX", "14"),
)
