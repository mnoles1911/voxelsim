# Categories 10 and 11 -- Region Ambient Beds, Day/Night & Time Cues
#                                       docs/sfx-library.md sections 12 and 13
#
# 16 regions x day/night = 32 beds. The library notes that four regions use a
# day/night-identical loop but are still authored as two "for transition
# smoothness" -- so both ids exist here, with prompts that differ only where
# the region genuinely differs. Each bed carries its layer recipe, which the
# library says is "captured at prompt time"; this file is that capture.

REGIONS = {
    "central_plains": (
        "wide open grassland under a big sky, grass moving in the wind, skylarks and distant insects",
        "wide open grassland at night, wind through grass, crickets and a far-off owl"),
    "spine_of_the_world": (
        "high bare mountains, thin cold wind over rock, a distant raptor cry and occasional loose stone",
        "high bare mountains at night, bitter thin wind and utter emptiness, rare rockfall"),
    "the_underway": (
        "a vast worked stone tunnel deep underground, still dead air, distant dripping and faint structural groans",
        "a vast worked stone tunnel deep underground, still dead air, distant dripping and faint structural groans"),
    "the_greatwood": (
        "an ancient dense forest by day, filtered birdsong, leaf rustle and a low silverwood hum beneath it",
        "an ancient dense forest at night, owls and small movements in the undergrowth, the low silverwood hum stronger"),
    "western_coast": (
        "a rocky drowned coastline, surf against stone, gulls and a hollow wind off the water",
        "a rocky drowned coastline at night, heavy surf, wind and a distant foghorn moan"),
    "the_ashfields": (
        "a dead grey plain under ashfall, dry wind over lifeless ground, no birds and no insects at all",
        "a dead grey plain at night, cold dry wind, absolute lifelessness"),
    "the_sorrowmarsh": (
        "a wide sour marsh, standing water, frogs and reed rattle with a thick heavy air",
        "a wide sour marsh at night, croaking and insect drone with faint uneasy shimmering tones"),
    "the_weeping_wood": (
        "a wrong quiet forest, sound arriving slightly late, dripping and faint whispering just below hearing",
        "a wrong quiet forest, sound arriving slightly late, dripping and faint whispering just below hearing"),
    "mor_vethrin": (
        "an immense boneyard of arches, wind resonating through hollow structures in long low tones",
        "an immense boneyard of arches, wind resonating through hollow structures in long low tones"),
    "aldenholt": (
        "a small farming town by day, distant voices, livestock, a smith working and cart wheels on packed earth",
        "a small farming town at night, dogs, a few late voices, shutters and wind"),
    "solgrade": (
        "a prosperous walled city by day, dense crowd murmur, market activity, bells and hooves on stone",
        "a prosperous walled city at night, sparse footsteps, distant watch calls and a quiet hum of a settled city"),
    "vosskara": (
        "a hard industrial town by day, hammering, furnaces, heavy carts and shouted work",
        "a hard industrial town at night, banked furnaces roaring low, occasional metal and few voices"),
    "caer_brannoch": (
        "a fortified keep by day, wind on battlements, drilling soldiers, gates and armoured movement",
        "a fortified keep at night, wind on battlements, watch footsteps and torches guttering"),
    "lirien_thal": (
        "an old elegant settlement by day, water features, soft distant strings and quiet civil movement",
        "an old elegant settlement at night, water features and a still hush with faint wind chimes"),
    "copper_isles": (
        "a warm island port by day, harbour water, rigging, gulls and dock work",
        "a warm island port at night, harbour water lapping, rigging creaking and distant tavern noise"),
    "shroud_sea_boundary": (
        "the edge of an unnatural sea, waves moving wrongly against each other with a low harmonic underneath and no gulls",
        "the edge of an unnatural sea, waves moving wrongly against each other with a low harmonic underneath and no gulls"),
}
for r, (day, night) in REGIONS.items():
    add(loop_("amb_%s_day_loop" % r, "%s, wide stereo field" % day, 20, 0.25, "Ambient", "10"))
    add(loop_("amb_%s_night_loop" % r, "%s, wide stereo field" % night, 20, 0.25, "Ambient", "10"))

# Sub-ambience one-shots, randomised over the beds (the Minecraft cave-sound
# model). These must sit UNDER the bed, so they are quiet and un-startling
# except where the design wants a startle.
add(
    one("amb_bird_call", "A single wild bird call at a distance, natural and unhurried, %s" % DRY, 1.4, 0.35, 8, "Ambient", "10"),
    one("amb_owl", "A single owl call at night, distant and clear, %s" % DRY, 1.6, 0.35, 3, "Ambient", "10"),
    one("amb_insect_chirp", "A short burst of insect chirping close by, dry and rhythmic, %s" % DRY, 1.6, 0.32, 5, "Ambient", "10"),
    one("amb_raptor_cry", "A hunting bird crying high overhead, a thin piercing call carrying far, %s" % DRY, 1.6, 0.35, 3, "Ambient", "10"),
    one("amb_crow", "A crow calling harshly two or three times, %s" % DRY, 1.4, 0.35, 3, "Ambient", "10"),
    one("amb_distant_wolf", "A wolf howling far away, a long rising and falling call, %s" % DRY, 3.0, 0.35, 3, "Ambient", "10"),
    one("amb_distant_goblin_clatter", "Distant goblin activity, faint clattering and jabbering carried on the wind, %s" % DRY, 2.0, 0.32, 3, "Ambient", "10"),
    one("amb_ghostlight_shimmer", "A faint eerie shimmering tone drifting past over marsh water, cold and unnatural, %s" % DRY, 2.5, 0.30, 3, "Ambient", "10"),
    one("amb_naergrim_whisper", "A whisper just below the threshold of words, wrong and close, no intelligible language, %s" % DRY, 2.5, 0.30, 4, "Ambient", "10"),
    one("amb_rockfall_tick", "A few loose stones shifting and falling on a mountainside, small and distant, %s" % DRY, 1.6, 0.35, 3, "Ambient", "10"),
    loop_("amb_silverwood_hum", "an almost inaudible low tonal hum coming from ancient trees, felt more than heard", 20, 0.25, "Ambient", "10"),
    one("amb_stone_groan", "A deep structural groan of enormous stone settling far underground, %s" % DRY, 3.0, 0.32, 3, "Ambient", "10"),
    one("amb_bone_arch_resonance", "Wind finding a hollow arch and resonating it into a long low tone, %s" % DRY, 4.0, 0.30, 2, "Ambient", "10"),
    one("amb_creak_dead_stump", "A dead tree stump creaking as it flexes in the wind, dry and hollow, %s" % DRY, 2.0, 0.35, 3, "Ambient", "10"),
    one("amb_foghorn_moan", "A distant foghorn sounding once across water, low and mournful, %s" % DRY, 3.5, 0.32, 2, "Ambient", "10"),
    one("amb_settlement_dog", "A dog barking a few times somewhere in a village, %s" % DRY, 2.0, 0.35, 3, "Ambient", "10"),
    one("amb_settlement_livestock", "Livestock in a village, a cow or goat calling with movement in a pen, %s" % DRY, 2.0, 0.35, 4, "Ambient", "10"),
    one("amb_distant_smith", "A smith working somewhere out of sight, a few ringing hammer blows carrying across a town, %s" % DRY, 2.5, 0.35, 2, "Ambient", "10"),
    one("amb_rigging_creak", "Ship rigging and timber creaking at a quay, ropes working against wood, %s" % DRY, 2.5, 0.32, 3, "Ambient", "10"),
)

# --- Category 11: Day/Night & Time cues -----------------------------------
add(
    one("time_dawn_birdsong_swell", "Dawn arriving, birdsong building from nothing into a full chorus over several seconds, %s" % DRY, 8.0, 0.30, 1, "Ambient", "11"),
    one("time_dawn_settlement_wake", "A settlement waking at dawn, shutters, first voices and movement starting up, %s" % DRY, 6.0, 0.30, 2, "Ambient", "11"),
    one("time_dusk_birdsong_fade", "Dusk falling, birdsong thinning away to a few last calls and then silence, %s" % DRY, 8.0, 0.30, 1, "Ambient", "11"),
    one("time_dusk_fires_lit_murmur", "Evening in a settlement, fires being lit and voices gathering indoors, %s" % DRY, 6.0, 0.30, 1, "Ambient", "11"),
    one("time_night_onset_layer_in", "Night sounds fading in over the top of a quieting day, insects and owls arriving, %s" % DRY, 6.0, 0.30, 1, "Ambient", "11"),
    one("time_day_onset_layer_out", "Night sounds fading out as day arrives, insects thinning away, %s" % DRY, 6.0, 0.30, 1, "Ambient", "11"),
    one("time_period_boundary_sting", "A very short understated tonal marker for the turn of a time period, one soft low tone, diegetic and restrained, %s" % DRY, 1.5, 0.35, 1, "Ambient", "11"),
    one("worldclock_hour_chime_distant", "A settlement bell marking the hour, heard from some distance with air between, %s" % DRY, 4.0, 0.35, 2, "Ambient", "11"),
)
