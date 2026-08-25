# Category 02 -- Combat: Player                   docs/sfx-library.md section 4
#
# Universal verbs sit over the weapon matrix and are class-agnostic. Only the
# ThrowableSpear is combat-shipped today; the rest of the weapon classes are
# COMBAT_NEXT_PHASES growth slots that the library authors anyway so the matrix
# is complete. None of Category 02 has ever been rendered -- it was Phase 1's
# own "do this first" and was deferred on budget (~9,585 ElevenLabs credits).

add(
    one("cmb_sword_swing_light", "A fast sharp sword swing through the air, a bright edge whoosh, %s" % DRY, 0.6, 0.40, 5, "Combat", "02"),
    one("cmb_sword_swing_combo", "A chained set of three fast sword swings, three bright edge whooshes in quick succession with a final harder cut, %s" % DRY, 1.6, 0.40, 4, "Combat", "02"),
    one("cmb_sword_windup_power", "A fighter winding up a heavy sword blow, a drawn breath, cloth and leather shifting, the blade rising, no impact, %s" % DRY, 1.0, 0.35, 3, "Combat", "02"),
    one("cmb_sword_land_power", "A heavy block-breaking sword impact, a deep steel crunch with real weight behind it, %s" % DRY, 0.9, 0.45, 4, "Combat", "02"),
    one("cmb_power_abort", "A charged sword swing released without firing, the blade lowering, cloth settling and a short frustrated exhale, %s" % DRY, 0.8, 0.35, 2, "Combat", "02"),
    one("cmb_swing_miss_air", "A weapon swung hard through empty air, a deep air-displacement whoosh with a breath of effort, no impact, %s" % DRY, 0.7, 0.40, 5, "Combat", "02"),
    loop_("cmb_block_hold_loop", "a raised blade holding against pressure, sustained steel-on-steel clang and grinding scrape", 10, 0.38, "Combat", "02"),
    one("cmb_block_impact", "A blow absorbed on a raised blade, a hard steel clang with the shock carrying into the arms, %s" % DRY, 0.7, 0.45, 5, "Combat", "02"),
    one("cmb_parry_success", "A clean high parry, a bright ringing deflection of steel on steel sliding away, %s" % DRY, 0.8, 0.45, 4, "Combat", "02"),
    one("cmb_riposte_strike", "A fast follow-up sword strike after a parry, a sharp cut landing with immediate weight, %s" % DRY, 0.7, 0.42, 3, "Combat", "02"),
    one("cmb_cue_parry_green", "A very short soft diegetic chime from an enemy stance, a single clean high metallic tick, understated, not a UI beep, %s" % DRY, 0.3, 0.50, 2, "Combat", "02"),
    one("cmb_cue_heavy_yellow", "A short low tonal warning from an enemy stance, a dull metallic swell, understated and diegetic, not a UI beep, %s" % DRY, 0.4, 0.50, 2, "Combat", "02"),
    one("cmb_cue_unblock_red", "A short low thud with a growl underneath from an enemy stance, threatening and diegetic, not a UI beep, %s" % DRY, 0.5, 0.50, 2, "Combat", "02"),
    one("cmb_dodge_roll", "A fighter rolling across the ground, a burst of fabric and leather scuffing with a fast body impact and recovery, %s" % DRY, 0.9, 0.40, 4, "Combat", "02"),
    one("cmb_dodge_step", "A short sharp side-step, one quick boot scuff and cloth snap, %s" % DRY, 0.5, 0.40, 4, "Combat", "02"),
    one("cmb_stagger_break", "A fighter stumbling as his guard collapses, an off-balance scrape of boots and a winded breath, %s" % DRY, 1.5, 0.38, 3, "Combat", "02"),
    one("cmb_endurance_empty", "A fighter gasping hard for air as his guard breaks, exhausted, breath only, no words, %s" % DRY, 1.0, 0.35, 3, "Voice", "02"),
    one("cmb_weapon_draw", "A sword drawn from a leather scabbard, a fast metallic slither ending in a clear ring, %s" % DRY, 0.8, 0.45, 3, "Combat", "02"),
    one("cmb_weapon_sheathe", "A sword slid back into a leather scabbard, a controlled metallic slide ending in a soft seat, %s" % DRY, 0.9, 0.45, 3, "Combat", "02"),
    one("cmb_lockon_toggle", "A very subtle short tick marking a target lock, dry and minimal, almost inaudible, %s" % DRY, 0.2, 0.50, 1, "UI", "02"),
    one("cmb_timeslow_enter", "A very short downward pitch-bending wash marking time slowing, air pulling low and thick, %s" % DRY, 0.5, 0.35, 1, "Combat", "02"),
    one("cmb_timeslow_exit", "A very short upward pitch-bending wash marking time resuming, air snapping back to normal, %s" % DRY, 0.5, 0.35, 1, "Combat", "02"),
)

# Weapon-class foley matrix: 8 melee classes x (7 one-shot actions + block loop).
WEAPONS = {
    "dagger":     ("A dagger", "light and fast, a thin quick hiss of a short blade", 0.42),
    "shortsword": ("A shortsword", "quick and clean, a compact bright edge", 0.42),
    "longsword":  ("A longsword", "a full-length steel blade with weight and reach", 0.42),
    "twohander":  ("A greatsword", "huge and slow, a deep heavy sweep of a two-handed blade", 0.40),
    "waraxe":     ("A war axe", "top-heavy and brutal, a blunt-backed head chopping through", 0.42),
    "mace":       ("A mace", "blunt and heavy, a solid steel head with no edge at all", 0.42),
    "flail":      ("A flail", "a chained head whirling, links rattling before the strike", 0.40),
    "spear":      ("A spear", "a long shaft thrusting, a lean piercing whoosh", 0.42),
}
ACTIONS = {
    "swing_light":    ("swung fast in a light attack", 0.6, 5),
    "swing_heavy":    ("swung hard in a heavy attack, slower and far stronger", 0.9, 4),
    "swing_miss_air": ("swung through empty air and missing entirely, air displacement and effort, no impact", 0.7, 4),
    "draw":           ("drawn ready to fight, metal and leather", 0.8, 3),
    "sheathe":        ("put away, metal and leather settling", 0.9, 3),
    "parry":          ("catching an incoming blow and turning it aside, a bright ringing slide of steel", 0.8, 4),
    "special":        ("performing its signature move, the single most distinctive sound this weapon makes", 1.0, 3),
}
BLOCK_BODY = {
    "dagger":     "a dagger turned edge-on against pressure, a thin strained scrape of a short blade barely holding",
    "shortsword": "a shortsword held against pressure, a compact steel grind under load",
    "longsword":  "a longsword held against pressure, sustained steel grinding with the blade flexing",
    "twohander":  "a greatsword braced against pressure, a deep slow grind of heavy steel",
    "waraxe":     "an axe haft braced against pressure, wood creaking with steel grinding above it",
    "mace":       "a mace haft braced against pressure, a dull metallic press with no ring",
    "flail":      "a flail haft braced against pressure, the chain and head swinging and knocking under load",
    "spear":      "a spear shaft braced crosswise against pressure, wood straining and creaking under weight",
}
for w, (wname, wchar, winfl) in WEAPONS.items():
    for a, (aphrase, adur, avar) in ACTIONS.items():
        add(one("cmb_%s_%s" % (w, a),
                "%s %s, %s, %s" % (wname, aphrase, wchar, DRY),
                adur, winfl, avar, "Combat", "02"))
    add(loop_("cmb_%s_block_hold_loop" % w, BLOCK_BODY[w], 10, winfl, "Combat", "02"))

add(
    one("cmb_shield_raise", "A shield brought up into guard, a fast shift of wood and iron rim with a leather strap creak, %s" % DRY, 0.6, 0.42, 2, "Combat", "02"),
    one("cmb_shield_lower", "A shield lowered out of guard, wood and iron settling against the body, %s" % DRY, 0.6, 0.42, 2, "Combat", "02"),
    one("cmb_shield_block_absorb", "A heavy blow absorbed on a wooden shield, a deep thud into planking with an iron rim ring, %s" % DRY, 0.8, 0.45, 5, "Combat", "02"),
    one("cmb_shield_bash", "A shield driven forward as a weapon, a blunt wooden slam with an iron rim crack, %s" % DRY, 0.7, 0.45, 4, "Combat", "02"),
    one("cmb_bow_nock", "An arrow nocked to a bowstring, a small wooden click and a faint string touch, %s" % DRY, 0.5, 0.45, 3, "Combat", "02"),
    one("cmb_bow_draw_creak", "A heavy bow drawn back, wood and horn creaking under increasing tension, %s" % DRY, 1.2, 0.40, 3, "Combat", "02"),
    one("cmb_bow_release", "A bowstring released, a sharp snapping thrum with the arrow leaving, %s" % DRY, 0.6, 0.45, 4, "Combat", "02"),
    one("cmb_bow_arrow_whir", "An arrow in flight passing close by, a fast fletching whir dopplering past, %s" % DRY, 0.7, 0.40, 3, "Combat", "02"),
    one("cmb_bow_dryfire", "A bow released with no arrow nocked, a hard hollow slap of string on limb, wrong and jarring, %s" % DRY, 0.6, 0.45, 1, "Combat", "02"),
)

# Tier / condition timbre layers -- mixed OVER the base hit, never a re-record.
add(
    one("cmb_tier_common_layer", "A thin dull metallic layer for a plain common blade, unremarkable steel with no ring, to be mixed under a weapon hit, %s" % DRY, 0.5, 0.45, 1, "Combat", "02"),
    one("cmb_tier_quality_layer", "A clean bright metallic layer for a well-made blade, a clear short ring, to be mixed under a weapon hit, %s" % DRY, 0.6, 0.45, 1, "Combat", "02"),
    one("cmb_tier_masterwork_layer", "A rich sustained metallic layer for a masterwork blade, a pure singing ring with long decay, to be mixed under a weapon hit, %s" % DRY, 0.9, 0.45, 1, "Combat", "02"),
    one("cmb_condition_dull_layer", "A dead flat metallic layer for a dull damaged blade, a lifeless clack with no ring at all, to be mixed under a weapon hit, %s" % DRY, 0.5, 0.45, 1, "Combat", "02"),
    one("cmb_condition_break_fail", "A weapon failing under strain, a sharp metallic crack as steel splits, %s" % DRY, 0.8, 0.48, 1, "Combat", "02"),
)

# Throwables and explosives (player side; the spear is the shipped one).
add(
    one("cmb_spear_windup", "A spear drawn back overhead ready to throw, cloth and shaft shifting, a held breath, %s" % DRY, 0.9, 0.38, 3, "Combat", "02"),
    one("cmb_spear_throw", "A spear thrown hard, a grunt of effort and the shaft leaving the hand with a fast whoosh, %s" % DRY, 0.8, 0.42, 3, "Combat", "02"),
    loop_("cmb_spear_inflight_loop", "a spear in flight, a steady lean rushing whistle of a shaft cutting air", 6, 0.35, "Combat", "02"),
    one("cmb_spear_embed_flesh", "A spearhead driving into a body, a wet heavy punch with the shaft shuddering after, %s" % DRY, 0.8, 0.45, 3, "Combat", "02"),
    one("cmb_spear_embed_wood", "A spearhead driving into a wooden target, a hard splitting thock with the shaft quivering after, %s" % DRY, 0.9, 0.45, 3, "Combat", "02"),
    one("cmb_spear_embed_stone", "A spearhead striking stone and failing to bite, a sharp metallic clank and skid, %s" % DRY, 0.8, 0.45, 3, "Combat", "02"),
    one("cmb_spear_retrieve", "A spear pulled free from where it stuck, a dragging release and the shaft coming up to hand, %s" % DRY, 0.9, 0.40, 3, "Combat", "02"),
    one("cmb_throw_arc", "A thrown object arcing through the air, a light tumbling whoosh, %s" % DRY, 0.7, 0.38, 3, "Combat", "02"),
    one("cmb_bomb_pitch_detonate", "A small powder bomb detonating, a sharp cracking blast with debris scattering after, %s" % DRY, 1.4, 0.45, 3, "Combat", "02"),
    one("cmb_oil_splash", "A clay flask of oil shattering and splashing across the ground, glass break and thick liquid spread, %s" % DRY, 1.0, 0.42, 3, "Combat", "02"),
    one("cmb_smoke_hiss", "A smoke pot igniting, a sustained pressurised hiss building then settling, %s" % DRY, 2.0, 0.38, 3, "Combat", "02"),
    one("cmb_caltrop_scatter", "A handful of iron caltrops thrown across stone, sharp scattering metallic tinks settling, %s" % DRY, 1.2, 0.45, 3, "Combat", "02"),
    one("cmb_flash_pop", "A flash charge going off, a bright hard crack with a short ringing tail, %s" % DRY, 1.0, 0.45, 3, "Combat", "02"),
    one("cmb_ashbane_torch_throw", "A burning torch thrown through the air and landing, flame roar dopplering past then guttering on the ground, %s" % DRY, 1.6, 0.40, 3, "Combat", "02"),
    one("cmb_venomtip_dart", "A small dart thrown, a thin fast hiss ending in a light sharp stick, %s" % DRY, 0.6, 0.42, 3, "Combat", "02"),
    one("cmb_trap_arm", "A jaw trap being set, ratcheting metal under tension locking into place, %s" % DRY, 1.2, 0.45, 3, "Combat", "02"),
    one("cmb_trap_snap", "A jaw trap snapping shut, a violent metallic clash of springs and teeth, %s" % DRY, 0.7, 0.48, 3, "Combat", "02"),
    one("cmb_tripwire_trigger", "A tripwire pulled taut and releasing, a thin wire twang and a mechanism letting go, %s" % DRY, 0.6, 0.45, 3, "Combat", "02"),
)
