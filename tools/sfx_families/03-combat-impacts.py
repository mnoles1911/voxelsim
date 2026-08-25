# Category 03 -- Combat: Impacts & Enemies        docs/sfx-library.md section 5
#
# Impact matrix: 5 damage types x 7 targets x var 4 = 140 files / 35 ids.
# Hit-zone timbre (head sharper / torso / limb dampened) is a pitch layer at
# runtime, NOT new files -- do not add head/torso/limb ids here.
# Powder and sapper structural hits belong to the voxel ejecta set (Cat 04).

DMG = {
    "slash":    "a cutting edge drawn hard through",
    "pierce":   "a point driven into",
    "blunt":    "a heavy blunt mass crushing into",
    "serrated": "a jagged serrated edge tearing through",
    "arrow":    "an arrow striking and burying into",
}
TARGETS = {
    "flesh_unarmored": ("bare flesh", "a wet meaty impact", 0.45),
    "flesh_padded":    ("a padded gambeson over flesh", "a dulled thudding impact through thick cloth", 0.42),
    "mail":            ("a mail hauberk", "iron rings bursting and chiming with the shock carrying through", 0.45),
    "plate":           ("steel plate armour", "a hard ringing deflection off shaped steel", 0.48),
    "shield":          ("a wooden shield", "a deep thud into planking with an iron rim ring", 0.45),
    "wood":            ("a wooden beam", "a splitting bite into timber", 0.45),
    "stone_terrain":   ("bare stone", "a sharp skidding clank with sparks and stone chips", 0.48),
}
for d, dphrase in DMG.items():
    for t, (tname, tdetail, tinfl) in TARGETS.items():
        add(one("cmb_hit_%s_%s" % (d, t),
                "A combat hit, %s %s, %s, %s" % (dphrase, tname, tdetail, DRY),
                0.7, tinfl, 4, "Combat", "03"))

# Per-enemy sets. Creature and armour vocalisations are non-verbal by design --
# these are not TTS dialogue and must carry no words in any language.
add(
    one("cmb_goblin_idle_chatter", "A small vicious goblin muttering and chittering to itself, guttural nonsense, no real words, %s" % DRY, 1.5, 0.35, 5, "Combat", "03"),
    one("cmb_goblin_alert_shout", "A goblin spotting prey and shrieking a sharp alarm, guttural, no words, %s" % DRY, 0.9, 0.38, 3, "Combat", "03"),
    one("cmb_goblin_group_alert", "Several goblins taking up an alarm shriek together, overlapping guttural cries, no words, %s" % DRY, 1.6, 0.35, 2, "Combat", "03"),
    one("cmb_goblin_attack_jab", "A goblin lunging with a short jabbing attack and a spitting snarl, %s" % DRY, 0.7, 0.40, 4, "Combat", "03"),
    one("cmb_goblin_attack_leap", "A goblin leaping at its target with a rising screech, %s" % DRY, 1.0, 0.40, 3, "Combat", "03"),
    one("cmb_goblin_hurt", "A goblin taking a wound, a sharp pained yelp, guttural, no words, %s" % DRY, 0.7, 0.38, 5, "Combat", "03"),
    one("cmb_goblin_death", "A goblin dying, a choked descending cry cutting off into a body fall, %s" % DRY, 1.4, 0.38, 4, "Combat", "03"),
    one("cmb_goblin_gib_overkill", "A small body destroyed by an overwhelming blow, a violent wet burst and scatter, %s" % DRY, 1.0, 0.45, 2, "Combat", "03"),
    one("cmb_goblin_flee", "A goblin breaking and running, a panicked receding jabber with scrambling feet, %s" % DRY, 1.6, 0.35, 3, "Combat", "03"),
    loop_("cmb_goblin_footstep_loop", "small bare feet scampering fast over dirt, light and quick", 8, 0.35, "Combat", "03"),
)

# Ashfallen: faceless and armoured. The library is explicit that it has NO
# voice -- every cue below is armour foley, and adding a vocal would break the
# creature's whole design.
add(
    one("cmb_ashfallen_footstep_heavy", "A single heavy armoured footfall, steel plate and mail carrying enormous weight onto stone, no voice, %s" % DRY, 0.9, 0.45, 5, "Combat", "03"),
    loop_("cmb_ashfallen_armor_creak_idle_loop", "old steel plate armour creaking and settling as something heavy stands inside it, no voice and no breathing", 12, 0.35, "Combat", "03"),
    one("cmb_ashfallen_telegraph_measured", "An armoured figure shifting into a measured attack stance, plate grinding as the weight transfers, no voice, %s" % DRY, 1.0, 0.40, 2, "Combat", "03"),
    one("cmb_ashfallen_telegraph_heavy", "An armoured figure winding up a devastating blow, plate straining and steel rising, no voice, %s" % DRY, 1.2, 0.40, 2, "Combat", "03"),
    one("cmb_ashfallen_telegraph_thrust_red", "An armoured figure setting for an unblockable thrust, a low grinding shift of plate with a dread weight to it, no voice, %s" % DRY, 1.1, 0.40, 2, "Combat", "03"),
    one("cmb_ashfallen_shield_bash", "An armoured figure driving a heavy shield forward, a brutal slam of iron and wood, no voice, %s" % DRY, 0.8, 0.45, 3, "Combat", "03"),
    one("cmb_ashfallen_hurt_clang_chip", "A blow landing on old armour, a dull clang with a chip of metal breaking away, no voice, %s" % DRY, 0.8, 0.45, 5, "Combat", "03"),
    one("cmb_ashfallen_death_collapse", "An armoured figure collapsing, a heavy cascade of plate and mail hitting the ground and settling, no voice, %s" % DRY, 2.0, 0.42, 3, "Combat", "03"),
    one("cmb_ashfallen_blade_drop", "A heavy sword dropped from a dead hand onto stone, a hard metallic clatter coming to rest, %s" % DRY, 1.2, 0.45, 2, "Combat", "03"),
)

add(
    loop_("cmb_wolf_breath_pant_loop", "a large wolf panting steadily, fast shallow breaths through bared teeth", 10, 0.35, "Combat", "03"),
    one("cmb_wolf_undergrowth_move", "A large animal moving through undergrowth, branches and leaves pushed aside with quick paw falls, %s" % DRY, 1.2, 0.38, 4, "Combat", "03"),
    one("cmb_wolf_alert_growl", "A wolf catching a scent and growling low in warning, rising in threat, %s" % DRY, 1.5, 0.38, 3, "Combat", "03"),
    one("cmb_wolf_lunge_windup", "A wolf coiling to spring, a scrabble of claws and a sharp intake of breath, %s" % DRY, 0.8, 0.38, 3, "Combat", "03"),
    one("cmb_wolf_bite", "A wolf snapping its jaws shut on flesh, a hard wet snap of teeth with a snarl, %s" % DRY, 0.7, 0.42, 4, "Combat", "03"),
    one("cmb_wolf_yelp_hurt", "A wolf taking a wound, a sharp high yelp breaking into a snarl, %s" % DRY, 0.8, 0.38, 4, "Combat", "03"),
    one("cmb_wolf_death", "A wolf dying, a descending whining cry falling away into a body hitting the ground, %s" % DRY, 1.6, 0.38, 3, "Combat", "03"),
    loop_("cmb_wolf_paw_steps_loop", "a large four-legged animal running over forest floor, fast rhythmic paw falls on leaf litter", 8, 0.35, "Combat", "03"),
)

add(
    loop_("cmb_bear_growl_idle_loop", "a huge bear breathing and rumbling low in its chest, deep and continuous", 12, 0.35, "Combat", "03"),
    one("cmb_bear_charge_telegraph", "A huge bear rearing and setting to charge, a deep chesty huff with heavy weight shifting, %s" % DRY, 1.4, 0.38, 2, "Combat", "03"),
    one("cmb_bear_run_thunder", "A huge bear charging at full speed, heavy thundering paw falls closing fast, %s" % DRY, 1.8, 0.38, 3, "Combat", "03"),
    one("cmb_bear_claw_swipe", "A huge bear swiping with one paw, a heavy air whoosh ending in a raking impact, %s" % DRY, 0.9, 0.42, 4, "Combat", "03"),
    one("cmb_bear_bite", "A huge bear closing its jaws, a massive wet crunch with a rumbling snarl, %s" % DRY, 0.9, 0.42, 3, "Combat", "03"),
    one("cmb_bear_rear_roar", "A wounded bear rearing up and roaring, enormous and enraged, %s" % DRY, 2.2, 0.40, 2, "Combat", "03"),
    one("cmb_bear_slam", "A huge bear slamming both forelegs down, a ground-shaking double impact, %s" % DRY, 1.0, 0.42, 3, "Combat", "03"),
    one("cmb_bear_footfall_heavy", "A single enormous animal footfall, immense weight compressing the ground, %s" % DRY, 0.8, 0.42, 5, "Combat", "03"),
    one("cmb_bear_hurt_deep", "A huge bear taking a deep wound, a pained bellowing roar dropping into a growl, %s" % DRY, 1.6, 0.40, 4, "Combat", "03"),
    one("cmb_bear_death_heavy", "A huge bear dying, a long failing roar collapsing into an enormous body hitting the ground, %s" % DRY, 2.6, 0.40, 3, "Combat", "03"),
)
