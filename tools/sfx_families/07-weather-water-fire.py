# Categories 07, 08, 09 -- Weather, Water, Fire & Camp
#                                        docs/sfx-library.md sections 9, 10, 11
#
# Most of these are rendered already (Phase 1 basics + Phase 3) and the
# generator will skip them; what is new is the full rain-on-surface grid, the
# remaining wind tiers, the per-state beds and the transition set.
#
# Everything marked [stereo] in the library is a bed, and beds are the one
# place the mono rule does not apply -- the prompt says so explicitly so a
# renderer does not collapse them.

RAIN_SURFACES = {
    "soil":      "soft earth absorbing the drops with a low granular patter",
    "stone":     "hard flagging with bright sharp ticks and running water",
    "foliage":   "leaves and branches taking the drops in a broad soft rustle",
    "water":     "an open water surface stippled with countless small impacts",
    "wood_roof": "a plank roof drumming overhead with hollow resonance",
}
for intensity, iword in (("light", "light steady rain"), ("heavy", "heavy pouring rain")):
    for surf, detail in RAIN_SURFACES.items():
        add(loop_("wx_rain_%s_%s_loop" % (intensity, surf),
                  "%s falling on %s, wide stereo field" % (iword, detail),
                  20, 0.28, "Ambient", "07"))

WINDS = {
    "calm":   ("barely moving air", "the faintest breath of movement, almost silence"),
    "breeze": ("a light breeze", "gentle air moving through grass and leaves"),
    "rain":   ("a wet driving wind", "steady wind carrying rain, gusting in waves"),
    "storm":  ("a storm wind", "powerful sustained roaring gusts with a low howl"),
    "lethal": ("a killing gale", "overwhelming screaming wind, violent and continuous"),
}
for w, (wname, wdetail) in WINDS.items():
    add(loop_("wx_wind_%s_loop" % w,
              "%s, %s, wide stereo field" % (wname, wdetail), 20, 0.28, "Ambient", "07"))

add(
    one("wx_thunder_distant", "Distant thunder rolling across a valley, a long low rumble with no sharp crack, %s" % DRY, 4.0, 0.35, 4, "Ambient", "07"),
    one("wx_thunder_near_crack", "A lightning strike close by, a violent splitting crack followed by a collapsing rumble, %s" % DRY, 4.0, 0.40, 3, "Ambient", "07"),
    one("wx_lightning_prestrike_hum", "The charged moment before a close lightning strike, a rising electrical hum with the air going tight, no strike, %s" % DRY, 1.6, 0.38, 2, "Ambient", "07"),
)

WX_STATES = {
    "clear":     "a calm clear day, faint air movement and distant birds, spacious and open",
    "overcast":  "a grey overcast day, still heavy air with muted distant sound and no birdsong",
    "light_rain": "light rain over open ground, a soft even patter with gentle wind",
    "heavy_rain": "heavy rain over open ground, a dense roaring downpour with running water",
    "fog":       "thick fog, deadened muffled air with sound swallowed and dripping condensation",
    "snow":      "steady snowfall, an eerie hush with sound absorbed and only faint wind",
    "ash_haze":  "an ashfall haze, dry dead air with fine grit drifting and no living sound at all",
}
for s, detail in WX_STATES.items():
    add(loop_("wx_%s_bed_loop" % s, "%s, wide stereo field" % detail, 20, 0.25, "Ambient", "07"))

add(
    one("wx_storm_front_approach", "A storm front arriving, distant wind and thunder building steadily closer over several seconds, %s" % DRY, 8.0, 0.30, 2, "Ambient", "07"),
    one("wx_rain_onset_ramp", "Rain beginning, the first scattered drops building into steady rainfall, %s" % DRY, 6.0, 0.30, 1, "Ambient", "07"),
    one("wx_rain_tailoff", "Rain ending, steady rainfall thinning to scattered last drops and dripping, %s" % DRY, 6.0, 0.30, 1, "Ambient", "07"),
    # THE ONE DELIBERATE OVER-LENGTH ENTRY. Every other loop is clamped to the
    # 20 s the prompts doc calls for ("Loops use a fixed 12-20 s"), because
    # ElevenLabs caps SFX duration and an over-length request comes back
    # truncated rather than refused -- which looks like a bad take, not a bad
    # request. This one keeps 30 s because its id names the duration and the
    # library specifies the swell as a 30-second shape. If the API will not
    # give 30 s, render it in two halves and join, the way section 6 of the
    # music doc handles the long cues -- do NOT quietly shorten it to 20 and
    # leave the id saying 30.
    one("wx_weather_swell_30s", "A slow thirty second swell of weather intensity rising and easing again, a single long breath of wind and rain, %s" % DRY, 30.0, 0.25, 1, "Ambient", "07"),
    loop_("wx_snowfall_hiss_loop", "snow falling steadily, a fine dry hiss with the world hushed around it", 20, 0.25, "Ambient", "07"),
    loop_("wx_ashfall_whisper_loop", "ash falling steadily, a dry whispering drift of fine particles, dead and lifeless", 20, 0.25, "Ambient", "07"),
    one("wx_ash_grit_gust", "A gust driving ash and grit past, a dry abrasive rush, %s" % DRY, 2.0, 0.35, 3, "Ambient", "07"),
)

# --- Category 08: Water ---------------------------------------------------
# The first block is already rendered and hand-authored in the prompts doc.
# It is listed anyway: this spec is the FULL inventory, so that the lint can
# check both directions. The generator skips anything the doc already has.
add(
    loop_("water_swim_surface_loop", "a person swimming at the surface, steady rhythmic strokes and splashes", 16, 0.30, "Ambient", "08"),
    loop_("water_swim_submerged_loop", "a body moving underwater, muffled low swishes and kicks", 16, 0.30, "Ambient", "08"),
    one("water_submerge_plunge", "A body dropping underwater, a heavy plunging splash cutting to muffled, %s" % DRY, 1.2, 0.40, 3, "Ambient", "08"),
    one("water_surface_gasp", "A person breaking the water surface with a sharp gasp and water-shedding splash, %s" % DRY, 1.0, 0.40, 3, "Voice", "08"),
    loop_("water_underwater_ambient_loop", "a low muffled underwater ambience with faint bubble drift", 20, 0.22, "Ambient", "08"),
    one("water_splash_small", "A small light water splash, a foot or hand entering, %s" % DRY, 0.6, 0.40, 5, "Ambient", "08"),
    one("water_splash_medium", "A medium water splash, a body-sized entry, %s" % DRY, 0.9, 0.40, 4, "Ambient", "08"),
    one("water_splash_large", "A large heavy water splash and churn, a big mass hitting water, %s" % DRY, 1.2, 0.40, 3, "Ambient", "08"),
    one("water_drip_single", "A single isolated water drip falling and plopping, %s" % DRY, 0.5, 0.35, 5, "Ambient", "08"),
)

add(
    loop_("water_swim_paddle_loop", "a person treading water and paddling in place, small continuous hand and foot splashes", 16, 0.30, "Ambient", "08"),
    loop_("water_bubble_trail_loop", "a stream of bubbles rising through water, continuous small wet pops", 14, 0.30, "Ambient", "08"),
    loop_("water_river_flow_loop", "a river flowing steadily over a rocky bed, continuous rushing water, wide stereo field", 20, 0.28, "Ambient", "08"),
    loop_("water_brook_trickle_loop", "a small brook trickling over stones, light bright running water", 16, 0.30, "Ambient", "08"),
    loop_("water_channel_rush_loop", "water running fast down a narrow dug channel, a confined urgent rush", 16, 0.30, "Ambient", "08"),
    loop_("water_surf_cliff_loop", "heavy surf breaking against cliffs, deep swells collapsing and dragging back, wide stereo field", 20, 0.28, "Ambient", "08"),
    loop_("water_harbor_lap_loop", "harbour water lapping against stone and timber, small regular slaps with hulls creaking faintly", 20, 0.28, "Ambient", "08"),
    loop_("water_drip_cluster_loop", "scattered water drips falling in a wet cave, irregular plops at different distances", 18, 0.28, "Ambient", "08"),
    loop_("water_runoff_postrain_loop", "water running off after rain, gutters and channels draining with steady trickles", 18, 0.28, "Ambient", "08"),
    loop_("water_wave_shroud_boundary_loop", "an unnatural sea boundary, waves moving against themselves with a wrong low harmonic underneath, unsettling, wide stereo field", 20, 0.25, "Ambient", "08"),
)

# --- Category 09: Fire & Camp ---------------------------------------------
# First block already rendered; listed for the same reason as the water set.
add(
    loop_("fire_campfire_crackle_loop", "a campfire burning steadily, close crackling and popping wood, wide stereo field", 18, 0.32, "Ambient", "09"),
    one("fire_ember_pop", "A single ember popping in a fire, one sharp small crack with a spark, %s" % DRY, 0.4, 0.42, 5, "Ambient", "09"),
    one("fire_ignite_whoosh", "A fire catching and flaring up, a soft rising whoosh settling into flame, %s" % DRY, 1.4, 0.40, 3, "Ambient", "09"),
    one("fire_extinguish_hiss", "A fire doused out, a hard burst of steam collapsing into a fading hiss, %s" % DRY, 2.0, 0.40, 2, "Ambient", "09"),
    one("camp_rest_fade_sting", "A very soft low tone as camp rest begins and the world fades, restrained, no fanfare, %s" % DRY, 1.6, 0.38, 1, "Ambient", "09"),
    one("camp_rest_autosave_chime", "A single quiet low tone marking the game saved at camp, deliberately understated, no jingle, %s" % DRY, 1.0, 0.40, 1, "Ambient", "09"),
)

add(
    one("fire_log_settle", "A burning log collapsing in a fire, a soft crumbling shift with a burst of sparks, %s" % DRY, 1.4, 0.38, 3, "Ambient", "09"),
    one("fire_tinder_kindle", "Tinder catching from a spark, tiny crackles building into a small flame, %s" % DRY, 2.5, 0.35, 2, "Ambient", "09"),
    one("fire_smoke_fade", "The last smoke rising from a dead fire, a faint dry whisper fading to nothing, %s" % DRY, 3.0, 0.30, 1, "Ambient", "09"),
    loop_("fire_torch_flutter_loop", "a torch flame fluttering in moving air, a close irregular flapping of fire", 12, 0.35, "Ambient", "09"),
    loop_("fire_brazier_loop", "a large standing brazier burning steadily, a deep contained roar with ember pops, wide stereo field", 18, 0.32, "Ambient", "09"),
)
