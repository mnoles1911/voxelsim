# Category 19 -- Magic & Spellcraft               docs/sfx-library.md section 21
#
# THE CANON CONSTRAINT IS THE WHOLE BRIEF HERE, and every prompt below is
# written against it. Magic in this world is rare (~1 in 10,000 useful), costly
# and NEVER triumphant. Game One has no player caster at all: G1 magic is
# villain-side, environmental, and the Aeluvain is referenced rather than
# wielded. So none of these may sound like a fireball whoosh-boom -- they read
# strained, austere and wrong. Every prompt carries "no triumphant", and the
# cost cues carry pain.
#
# The library tags each entry [G1] shippable now or [RM] roadmap (G2/G3). That
# tag is carried into the prompts doc so a budget pass can render the ~10 G1
# ambient beds without paying for the roadmap set.

G1 = " [G1]"
RM = " [RM]"

# Casting foley. The cost cue is mandatory: every working tolls the caster.
add(
    one("mag_charge_gather", "Power being gathered with effort, a strained rising pressure with the air going wrong around it, austere, never triumphant, %s" % DRY, 2.0, 0.35, 3, "Combat", "19"),
    one("mag_release_innate", "A working released, a short unglamorous discharge that costs something, dry and strained, never triumphant, no whoosh and no boom, %s" % DRY, 1.4, 0.38, 3, "Combat", "19"),
    loop_("mag_channel_loop", "a working held open under strain, a sustained wrong pressure with the air refusing it, austere and never triumphant", 12, 0.32, "Combat", "19"),
    one("mag_fizzle_fail", "A working collapsing before it takes, a strained gathering that gutters out into nothing, disappointing and dry, %s" % DRY, 1.6, 0.38, 2, "Combat", "19"),
    one("mag_cost_toll", "The price of a working landing on the caster, a pained gasp with a cranial tinnitus swell rising and ragged breathing after, no words, %s" % DRY, 3.0, 0.35, 3, "Voice", "19"),
    one("mag_cost_alteration_sting", "The moment a working changes the caster permanently, a single cold wrong tone with no resolution, deeply unwelcome, %s" % DRY, 2.0, 0.35, 1, "Voice", "19"),
    loop_("mag_ritual_build_loop", "a long ritual building in a stone space, low sustained pressure with a slow wrongness accumulating underneath, no chanting", 20, 0.28, "Ambient", "19"),
    loop_("mag_ritual_against_grain_loop", "a ritual working against the grain of the world, a detuned thinning drone that sounds like reality being stretched", 20, 0.26, "Ambient", "19"),
)

# Spell-effect impacts. Lore-faithful schools only -- no elemental laundry list.
add(
    one("mag_env_temp_shift", "The temperature of a space changing unnaturally fast, air contracting with a low groan and frost ticking, %s" % DRY, 2.5, 0.32, 2, "Ambient", "19"),
    one("mag_env_pressure_pop", "Air pressure dropping sharply in a room, a dull ear-popping thump with everything going momentarily flat, %s" % DRY, 1.2, 0.38, 2, "Ambient", "19"),
    loop_("mag_env_stormcharge_hum", "the air before an unnatural storm, a low electrical hum with everything held too tight", 14, 0.30, "Ambient", "19"),
    loop_("mag_structural_perceive_loop", "the sound of seeing how something is put together, a faint ordered ringing of structure revealing itself, cold and analytical", 12, 0.28, "Ambient", "19"),
    loop_("mag_blight_creep_loop", "blight spreading slowly through living ground, a dry crackling rot advancing with life going quiet ahead of it", 18, 0.28, "Ambient", "19"),
    one("mag_ward_raise", "A protective ward raised, a strained tightening of air into a boundary, effortful rather than grand, %s" % DRY, 1.6, 0.35, 2, "Combat", "19"),
    loop_("mag_ward_hold_loop", "a protective ward holding, a sustained pressure boundary humming under load", 12, 0.30, "Combat", "19"),
    one("mag_ward_break", "A protective ward failing, a pressure boundary tearing open with a sick collapsing snap, %s" % DRY, 1.4, 0.40, 3, "Combat", "19"),
    loop_("mag_aeluvain_hum_loop", "a pure cold tone with one note conspicuously missing from it, beautiful and incomplete, referenced from a distance", 16, 0.28, "Ambient", "19"),
    loop_("mag_wrongness_pressure_loop", "the presence of something that should not exist, an oppressive low pressure field with dread in it and no melody", 20, 0.26, "Ambient", "19"),
)

# Implements.
add(
    one("mag_staff_focus_tap", "A staff butt tapped once on stone to focus a working, a plain wooden knock with a faint wrong resonance after, %s" % DRY, 1.2, 0.40, 3, "Combat", "19"),
    loop_("mag_staff_thrum_loop", "a staff held ready, a low uneasy thrum running through the wood", 12, 0.30, "Combat", "19"),
    one("mag_item_activate", "An enchanted object waking, a cold reluctant stirring of something old, austere and unwelcoming, never triumphant, %s" % DRY, 2.0, 0.35, 3, "Combat", "19"),
    one("mag_rune_circle_ignite", "A rune circle taking light, a strained rising tone with stone heating and the air pulling inward, never triumphant, %s" % DRY, 2.5, 0.35, 2, "Ambient", "19"),
    loop_("mag_rune_circle_loop", "a rune circle burning steadily, a sustained cold tone with the air held wrong above it", 16, 0.28, "Ambient", "19"),
    one("mag_aeluvain_unsheathe", "A legendary blade drawn, a clean cold ring that keeps going slightly too long, %s" % DRY, 2.5, 0.38, 1, "Combat", "19"),
    one("mag_aeluvain_strike", "A legendary blade landing, a clean cut with an impossible pure tone underneath it, %s" % DRY, 1.6, 0.40, 2, "Combat", "19"),
    one("mag_aeluvain_song_complete", "A missing note finally sounding and a long incomplete tone resolving at last, the one moment in this set allowed to feel like relief, %s" % DRY, 4.0, 0.32, 1, "Combat", "19"),
)

# Enemy and villain casters.
add(
    one("mag_ashfallen_cast", "An armoured revenant working magic, a clipped joyless discharge that visibly costs it, no voice and never triumphant, %s" % DRY, 1.6, 0.38, 3, "Combat", "19"),
    loop_("mag_hand_ritual_chant_loop", "a group ritual working, low rhythmic non-verbal intoning with no intelligible words in any language, strained and joyless", 20, 0.28, "Ambient", "19"),
    loop_("mag_ashlord_presence_loop", "the presence of an overwhelming hostile power nearby, an oppressive pressure field with everything else going quiet under it", 22, 0.26, "Ambient", "19"),
    one("mag_ashlord_unmask_sting", "A concealed power revealing itself, a single low wrong tone with the air collapsing inward, dreadful rather than grand, %s" % DRY, 3.0, 0.32, 1, "Ambient", "19"),
    loop_("mag_mordvar_ambient_loop", "the world thinning near something ancient and wrong, a sustained absence where sound should be, with no discrete event in it", 25, 0.24, "Ambient", "19"),
)
