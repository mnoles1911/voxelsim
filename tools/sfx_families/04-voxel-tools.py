# Category 04 -- Tools & Voxel Interaction        docs/sfx-library.md section 6
#
# Tool x material strike matrix. The library's tractability rule is followed
# exactly: generate CORRECT-TOOL strike/break/place fully per material, plus
# ONE shared wrong-tool scrape per material. That is why there is no
# vox_<tool>_<material>_* id here -- the correct tool is implied by the
# material, and a full 4x18x3 cross would be 216 ids nobody asked for.
#
# The 4 wired materials (sand dirt grass stone) plus bedrock are already
# rendered and hand-authored in the prompts doc; the generator skips those.

MATERIALS = {
    "grass":        ("grass turf", "a soft tearing of roots and soil", 0.40),
    "dirt":         ("packed dirt", "a dull earthy chop with loose grit falling", 0.40),
    "sand":         ("dry sand", "a soft granular shove with grains pouring", 0.38),
    "clay":         ("dense wet clay", "a heavy sucking cut, thick and sticky", 0.40),
    "mud":          ("thick mud", "a wet sucking slop with water squeezing out", 0.40),
    "ash":          ("deep dry ash", "a soft powdery whump with fine dust rising", 0.38),
    "gravel":       ("loose gravel", "a harsh rattling scoop of small stones", 0.45),
    "stone":        ("solid stone", "a hard ringing pick strike with chips flying", 0.48),
    "worked_stone": ("dressed worked stone", "a clean hard strike on a cut block, sharp and precise", 0.48),
    "sandstone":    ("soft sandstone", "a gritty crumbling strike, softer than granite, sand shedding", 0.45),
    "iron_ore":     ("iron ore in rock", "a dense metallic-edged strike, harder and duller than plain stone", 0.48),
    "steel_ore":    ("hard steel-bearing ore", "a very hard bright strike with a metallic ring under it", 0.48),
    "adamant_ore":  ("adamant ore", "an extremely hard strike that barely bites, a high crystalline ring and shock back up the haft", 0.50),
    "coal":         ("a coal seam", "a brittle crumbling crack with sooty fragments falling", 0.45),
    "copper_ore":   ("copper ore", "a softer metallic strike with a warm dull ring", 0.46),
    "wood":         ("a pine trunk", "a solid axe bite into softwood with a splitting crack", 0.45),
    "hardwood":     ("a dense oak trunk", "a heavy axe bite into hardwood, tight and resistant", 0.46),
    "leaves":       ("dense leaves and small branches", "a light tearing rustle of foliage being cut away", 0.35),
}

EVENTS = {
    "strike": ("A tool striking %s mid-dig, %s, a single hit with the work continuing", 0.6, 5),
    "break":  ("A block of %s finally giving way and breaking apart, %s then the mass collapsing and falling away", 1.0, 4),
    "place":  ("A block of %s set into place, %s, a short settling thud as it seats", 0.7, 3),
}

for m, (mname, mdetail, minfl) in MATERIALS.items():
    for ev, (tmpl, dur, var) in EVENTS.items():
        add(one("vox_%s_%s" % (m, ev), (tmpl % (mname, mdetail)) + ", " + DRY,
                dur, minfl, var, "SFX", "04"))
    add(one("vox_wrongtool_%s" % m,
            "The wrong tool scraping uselessly against %s, a dull ineffective "
            "scrape with no progress and no break, %s" % (mname, DRY),
            0.7, minfl, 3, "SFX", "04"))

add(one("vox_bedrock_blocked",
        "A pick striking unbreakable bedrock, a dead solid thunk that gives "
        "nothing at all, the shock jarring back up the haft, %s" % DRY,
        0.7, 0.50, 3, "SFX", "04"))

# Edit verbs and world physics.
add(
    loop_("vox_dig_loop_soft", "continuous digging in soft ground, a steady rhythm of shovel bites and earth being turned", 10, 0.38, "SFX", "04"),
    loop_("vox_dig_loop_hard", "continuous digging in hard rock, a steady rhythm of pick strikes ringing and chips falling", 10, 0.42, "SFX", "04"),
    one("vox_chop_tree_fell", "The final axe cut that fells a tree, a deep splitting crack and the trunk beginning to give, %s" % DRY, 1.4, 0.45, 3, "SFX", "04"),
    one("vox_tree_topple_wood", "A pine tree toppling and crashing down, a long tearing fall through branches ending in a heavy ground impact, %s" % DRY, 3.5, 0.42, 4, "SFX", "04"),
    one("vox_tree_topple_hardwood", "A massive oak toppling and crashing down, a slower heavier fall with splitting timber ending in a ground-shaking impact, %s" % DRY, 4.0, 0.42, 4, "SFX", "04"),
    one("vox_log_resolve", "A felled log settling into its final resting position, a short heavy roll and stop, %s" % DRY, 1.0, 0.40, 2, "SFX", "04"),
    one("vox_place_schematic_confirm", "A built structure snapping into place complete, a solid satisfying settle of timber and stone, understated, %s" % DRY, 0.9, 0.42, 1, "SFX", "04"),
    one("vox_place_reject_noeditzone", "A placement refused, a short dull negative thunk with no ring, unmistakably a rejection, %s" % DRY, 0.4, 0.48, 1, "SFX", "04"),
    one("vox_carve_volume_cycle", "A very short dry tick marking a carve volume changing size, minimal and mechanical, %s" % DRY, 0.2, 0.50, 1, "SFX", "04"),
    loop_("vox_powdercharge_fuse", "a powder fuse burning, a steady sputtering hiss with sparks", 8, 0.42, "SFX", "04"),
    one("vox_powdercharge_blast", "A powder charge detonating in rock, a deep concussive blast with stone bursting and debris raining down, %s" % DRY, 2.5, 0.45, 3, "SFX", "04"),
    one("vox_sapper_blast_heavy", "A heavy sapper charge bringing down a structure, an enormous low blast followed by a long collapse of stone, %s" % DRY, 4.0, 0.45, 3, "SFX", "04"),
    one("vox_gravity_creak_warn", "Unsupported ground beginning to fail, a low ominous creak and grinding shift warning of collapse, no collapse yet, %s" % DRY, 1.8, 0.40, 2, "SFX", "04"),
    one("vox_cluster_collapse", "A mass of unsupported blocks breaking loose and falling, a tumbling cascade of rock, %s" % DRY, 2.2, 0.45, 4, "SFX", "04"),
    one("vox_cluster_impact_ground", "A falling mass of rock slamming into the ground, a heavy scattering impact with debris settling, %s" % DRY, 1.8, 0.45, 4, "SFX", "04"),
    one("vox_cluster_impact_water", "A falling mass of rock crashing into deep water, an enormous splash and churn with the surface slapping back, %s" % DRY, 2.2, 0.42, 3, "SFX", "04"),
    one("vox_buildmode_ghost_appear", "A very soft dry tick as a build ghost appears, minimal, almost subliminal, %s" % DRY, 0.2, 0.50, 1, "SFX", "04"),
    one("vox_buildmode_snap_click", "A short crisp click as a build ghost snaps to a grid position, dry and mechanical, %s" % DRY, 0.2, 0.50, 1, "SFX", "04"),
    one("vox_buildmode_reject", "A short dull negative thunk refusing a build placement, no ring, %s" % DRY, 0.3, 0.48, 1, "SFX", "04"),
)
