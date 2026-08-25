# Category 01 -- Player Locomotion & Foley        docs/sfx-library.md section 3
#
# Footstep matrix: 4 gaits x 12 surfaces x var 5 = 240 files / 48 ids.
# The 6 "live" surfaces (grass dirt stone wood sand shallow_water) already have
# hand-authored rows in the prompts doc and are rendered; the generator skips
# them. The other 6 surfaces are new.

GAITS = {
    "walk":   ("A single footstep", "", 0.5),
    "run":    ("A single fast running footstep", "harder impact, ", 0.5),
    "sprint": ("A single hard sprinting footstep", "heavy fast impact, ", 0.5),
    "crouch": ("A single very soft slow crouched footstep",
               "careful muffled press, faint, ", 0.6),
}

SURFACES = {
    "grass":         ("on grass and soil", "faint dry grass crunch", 0.40),
    "dirt":          ("on bare packed dirt", "soft earthy thud and slight grit", 0.40),
    "stone":         ("on stone flagging", "hard leather-on-rock tap with a faint scuff", 0.45),
    "wood":          ("on an old wooden plank floor", "dull hollow knock with a slight creak", 0.45),
    "sand":          ("into dry sand", "soft muffled compression and fine grain shift", 0.40),
    "gravel":        ("on loose gravel", "sharp scattering of small stones crunching underfoot", 0.45),
    "snow":          ("into fresh snow", "tight high squeak and soft powder compression", 0.40),
    "mud":           ("into thick mud", "wet sucking squelch as the boot pulls free", 0.40),
    "marsh":         ("into boggy marsh ground", "waterlogged squelch through reeds and peat", 0.40),
    "shallow_water": ("into shallow water over mud", "a low wet splash and squelch", 0.40),
    "metal":         ("on a metal grating", "hard ringing clank with a faint metallic sustain", 0.45),
    "cave_stone":    ("on damp cave stone", "hard contact with a gritty scrape and a touch of stone dust", 0.45),
}

WET = ("shallow_water", "mud", "marsh")

for g, (gphrase, gextra, gdur) in GAITS.items():
    for s, (sphrase, sdetail, infl) in SURFACES.items():
        dur = gdur + (0.1 if s in WET else 0.0)
        if g == "sprint" and s == "shallow_water":
            dur = 0.7
        add(one("step_%s_%s" % (g, s),
                "%s %s, %s%s, %s" % (gphrase, sphrase, gextra, sdetail, DRY),
                round(dur, 1), infl, 5, "SFX", "01"))

# Jump / land matrix: 12 surfaces x var 5.
for s, (sphrase, sdetail, infl) in SURFACES.items():
    add(one("jumpland_%s" % s,
            "A person landing hard from a jump %s, a single heavy two-foot "
            "impact, %s, knees absorbing, %s" % (sphrase, sdetail, DRY),
            0.8, infl, 5, "SFX", "01"))

add(
    one("jump_exert_grunt",
        "A short sharp effort grunt from a man launching into a jump, breath only, no words, %s" % DRY,
        0.6, 0.35, 3, "Voice", "01"),
    one("land_heavy_stagger",
        "A person landing badly and staggering, heavy boot impact then two scrambling recovery steps and a winded grunt, %s" % DRY,
        1.4, 0.40, 3, "SFX", "01"),
    one("land_soft",
        "A light controlled landing from a short drop, soft boot contact and cloth settle, %s" % DRY,
        0.6, 0.40, 3, "SFX", "01"),
)

# Armor-weight movement layer, mixed over steps by equipped weight (KCD2-style).
ARMOR = {
    "cloth":   ("soft linen and wool clothing shifting and rustling", 0.30),
    "leather": ("supple leather armour creaking and straps flexing", 0.32),
    "mail":    ("a mail hauberk shifting, thousands of small iron rings chiming and sliding against each other", 0.35),
    "plate":   ("steel plate armour moving, heavy plates knocking, leather straps creaking and mail voiders sliding underneath", 0.38),
}
for tier, (body, infl) in ARMOR.items():
    add(loop_("armor_%s_move_loop" % tier, "%s as a person walks" % body,
              14, infl, "SFX", "01"))
    add(one("armor_%s_run_clank" % tier,
            "A burst of %s at a hard run, louder and more agitated, a few seconds of stressed movement, %s" % (body, DRY),
            1.2, infl, 3, "SFX", "01"))

add(
    loop_("climb_rock_loop", "a person climbing a rock face, hands gripping and scraping stone, boots scuffing for purchase, gritty and effortful", 12, 0.35, "SFX", "01"),
    loop_("climb_wood_loop", "a person climbing a wooden structure, hands slapping and gripping timber, boots scuffing planks, faint creaking", 12, 0.35, "SFX", "01"),
    one("climb_grunt", "A short strained effort grunt from a man pulling himself upward, breath only, no words, %s" % DRY, 0.7, 0.35, 3, "Voice", "01"),
    loop_("ladder_step_wood_loop", "a person climbing a wooden ladder, rhythmic boot contacts on rungs with faint timber creak", 12, 0.35, "SFX", "01"),
    loop_("ladder_step_iron_loop", "a person climbing an iron ladder, rhythmic boot contacts ringing on cold metal rungs", 12, 0.38, "SFX", "01"),
    one("vault_ledge", "A person vaulting over a low ledge, hands slapping stone, a scrape of cloth and boot, landing on the far side, %s" % DRY, 1.2, 0.40, 3, "SFX", "01"),
    one("slide_scree", "A person sliding down loose scree, a rush of cascading small stones and skidding boots, %s" % DRY, 1.8, 0.40, 3, "SFX", "01"),
    loop_("water_wade_shallow_loop", "a person wading steadily through shin-deep water, rhythmic wet pushes and streaming drips", 14, 0.35, "Ambient", "01"),
    one("water_entry_walk", "A person walking down into water, a slow deepening wade and swirl, %s" % DRY, 1.5, 0.40, 3, "Ambient", "01"),
    one("water_entry_run_plunge", "A person running into water and plunging forward, a hard fast splash and churn, %s" % DRY, 1.5, 0.40, 3, "Ambient", "01"),
    one("water_entry_fall_deep", "A body falling from height into deep water, a heavy plunging impact cutting to muffled underwater, %s" % DRY, 2.0, 0.40, 3, "Ambient", "01"),
)

add(
    loop_("roland_breath_idle_loop", "a man breathing quietly at rest, slow and even, close and intimate, no words", 14, 0.30, "Voice", "01"),
    loop_("roland_breath_exert_loop", "a man breathing hard from exertion, fast and heavy through the mouth, no words", 12, 0.32, "Voice", "01"),
    loop_("roland_breath_lowhp_loop", "a wounded man breathing in shallow ragged pulls with a faint catch of pain, no words", 12, 0.32, "Voice", "01"),
    loop_("roland_breath_critical_loop", "a badly wounded man breathing in short desperate rasps, wet and failing, no words", 12, 0.34, "Voice", "01"),
    one("roland_effort_grunt", "A short hard effort grunt from a man swinging a weapon, breath only, no words, %s" % DRY, 0.6, 0.35, 5, "Voice", "01"),
    one("roland_jump_exhale", "A sharp exhale from a man pushing off into a jump, breath only, no words, %s" % DRY, 0.5, 0.35, 3, "Voice", "01"),
)
