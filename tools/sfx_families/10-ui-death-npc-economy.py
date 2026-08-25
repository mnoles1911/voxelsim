# Categories 15, 16, 17, 18 -- UI, Death & Respawn, NPC Non-Verbal, Economy
#                          docs/sfx-library.md sections 17, 18, 19, 20
#
# The UI rule from AUDIO_DESIGN is load-bearing and every prompt below obeys
# it: minimal, dry, short, NO fanfare and NO level-up jingle. Where a category
# elsewhere wanted a "sting", it is a single soft tone, not a flourish.
#
# inventory_open/close and quickslot_cycle/activate are defined HERE and not in
# Cat 06, where the library also lists them -- one sound each, on the UI bus.

add(
    one("ui_navigate", "A very short dry tick moving between menu entries, minimal and unmusical, %s" % DRY, 0.15, 0.50, 2, "UI", "15"),
    one("ui_confirm", "A short dry confirming tap, minimal, no chime and no fanfare, %s" % DRY, 0.2, 0.50, 1, "UI", "15"),
    one("ui_cancel", "A short dry backing-out tap, slightly duller than a confirm, minimal, %s" % DRY, 0.2, 0.50, 1, "UI", "15"),
    one("ui_error_thud", "A short dull negative thud refusing an action, no ring and no tone, %s" % DRY, 0.3, 0.50, 1, "UI", "15"),
    one("journal_open", "A leather journal opened, a soft cover flex and page settle, %s" % DRY, 0.8, 0.42, 1, "UI", "15"),
    one("journal_close", "A leather journal closed, a soft cover fall and settle, %s" % DRY, 0.7, 0.42, 1, "UI", "15"),
    one("journal_page_turn", "A single page turned in a book, a light paper sweep and settle, %s" % DRY, 0.6, 0.40, 3, "UI", "15"),
    one("map_open", "A folded map opened out, parchment unfolding and flattening, %s" % DRY, 1.2, 0.40, 1, "UI", "15"),
    one("map_trace_ink", "A line traced in ink across parchment, a fine continuous nib drag, %s" % DRY, 1.4, 0.40, 2, "UI", "15"),
    one("quickslot_cycle", "A very short dry tick cycling a quick slot, minimal, %s" % DRY, 0.15, 0.50, 1, "UI", "15"),
    one("quickslot_activate", "A short dry tap activating a quick slot item, minimal, %s" % DRY, 0.25, 0.48, 2, "UI", "15"),
    one("inventory_open", "An inventory opened, a soft leather and cloth shift of a pack being looked into, %s" % DRY, 0.7, 0.40, 1, "UI", "15"),
    one("inventory_close", "An inventory closed, a soft leather and cloth settle of a pack shutting, %s" % DRY, 0.6, 0.40, 1, "UI", "15"),
    one("pause_open", "A very soft low tone marking the game pausing, restrained and short, %s" % DRY, 0.5, 0.42, 1, "UI", "15"),
    one("pause_close", "A very soft low tone marking the game resuming, restrained and short, %s" % DRY, 0.5, 0.42, 1, "UI", "15"),
    one("interaction_prompt_appear", "An extremely subtle short tick as an interaction prompt appears, almost inaudible, %s" % DRY, 0.12, 0.50, 1, "UI", "15"),
    one("bark_overlay_appear", "A very soft short marker as a spoken line appears on screen, near-subliminal, %s" % DRY, 0.15, 0.50, 1, "UI", "15"),
    one("skill_node_tone", "One single soft clean tone marking a skill taken, restrained, no fanfare and no jingle, %s" % DRY, 0.9, 0.42, 1, "UI", "15"),
    one("skill_legendary_reset_tone", "One low sustained tone marking a legendary skill reset, sombre rather than triumphant, no fanfare, %s" % DRY, 1.6, 0.40, 1, "UI", "15"),
    one("quest_update_soft", "A very soft short marker that an objective has changed, understated, no fanfare, %s" % DRY, 0.6, 0.42, 1, "UI", "15"),
    one("objective_complete_soft", "A soft low tone marking an objective completed, quietly satisfying, no fanfare and no jingle, %s" % DRY, 1.0, 0.42, 1, "UI", "15"),
)

# --- Category 16: Death & Respawn -----------------------------------------
# The library is explicit: no "YOU DIED" sting. These are bodily and quiet.
add(
    one("death_collapse_thud", "A body collapsing to the ground, armour and limbs hitting hard and going still, %s" % DRY, 1.6, 0.42, 2, "SFX", "16"),
    loop_("death_near_breath_loop", "a dying person breathing in shallow failing rasps, wet and slowing", 12, 0.32, "Voice", "16"),
    loop_("death_near_heartbeat_loop", "a slow heavy heartbeat heard from inside the body, thick and muffled", 12, 0.30, "SFX", "16"),
    loop_("death_vignette_rumble_loop", "a low pressure rumble closing in as consciousness narrows, felt more than heard", 14, 0.28, "SFX", "16"),
    one("death_second_wind_cue", "A sharp indrawn breath as someone pulls back from the edge, one gasp and the pressure lifting, %s" % DRY, 1.4, 0.38, 1, "SFX", "16"),
    one("death_fade_tone", "A single low tone fading out as everything goes dark, sombre and quiet, no sting and no fanfare, %s" % DRY, 3.0, 0.35, 1, "SFX", "16"),
    one("death_return_load_soft", "A soft low return of ambient sound as the world comes back, gentle and unhurried, %s" % DRY, 2.5, 0.32, 1, "SFX", "16"),
)

# --- Category 17: NPC Non-Verbal & Crowd ----------------------------------
# Voice bus, but NOT dialogue -- these carry no words in any language, which is
# what separates them from the TTS pipeline.
add(
    one("npc_effort_grunt_m", "A short effort grunt from a man doing physical work, breath only, no words, %s" % DRY, 0.7, 0.35, 5, "Voice", "17"),
    one("npc_effort_grunt_f", "A short effort grunt from a woman doing physical work, breath only, no words, %s" % DRY, 0.7, 0.35, 5, "Voice", "17"),
    one("npc_react_surprise", "A short surprised intake of breath from a person, no words, %s" % DRY, 0.6, 0.35, 3, "Voice", "17"),
    one("npc_react_scoff", "A short dismissive scoff from a person, breath and throat only, no words, %s" % DRY, 0.6, 0.35, 3, "Voice", "17"),
    one("npc_react_laugh", "A short genuine laugh from a person, no words, %s" % DRY, 1.4, 0.35, 4, "Voice", "17"),
    one("npc_react_cough", "A person clearing their throat and coughing once or twice, no words, %s" % DRY, 1.2, 0.35, 3, "Voice", "17"),
    one("npc_react_sigh", "A person sighing heavily, tired rather than sad, no words, %s" % DRY, 1.4, 0.32, 3, "Voice", "17"),
    one("npc_react_gasp", "A sharp alarmed gasp from a person, no words, %s" % DRY, 0.6, 0.38, 3, "Voice", "17"),
    one("npc_footstep_approach", "A person walking up and stopping close by, three or four unhurried steps and a settle, %s" % DRY, 2.0, 0.38, 3, "SFX", "17"),
    one("npc_bark_pre_breath", "The small intake of breath a person takes just before speaking, no words, %s" % DRY, 0.4, 0.32, 2, "Voice", "17"),
    loop_("crowd_market_murmur_loop", "a busy market crowd murmuring, many overlapping voices with no intelligible words, wide stereo field", 20, 0.28, "Voice", "17"),
    loop_("crowd_tavern_murmur_loop", "a tavern room of drinkers talking and laughing, warm and enclosed, no intelligible words, wide stereo field", 20, 0.28, "Voice", "17"),
    loop_("crowd_court_murmur_loop", "a formal hall of people talking low and carefully, restrained and echoing, no intelligible words, wide stereo field", 20, 0.28, "Voice", "17"),
    one("vendor_callout_nonverbal", "A market trader calling out to passers-by, the shape and rhythm of a sales cry with no intelligible words, %s" % DRY, 2.0, 0.35, 4, "Voice", "17"),
    one("guard_challenge_nonverbal", "A guard barking a challenge, the shape and rhythm of an order with no intelligible words, %s" % DRY, 1.4, 0.38, 3, "Voice", "17"),
    loop_("child_play_distant_loop", "children playing somewhere out of sight, distant calls and laughter, no intelligible words", 18, 0.30, "Voice", "17"),
)

# --- Category 18: Economy & Vendor ----------------------------------------
# econ_trade_decline is deliberately absent: the library says it reuses
# ui_error_thud. Giving it its own id would generate a sound nobody plays.
add(
    loop_("econ_coin_count_loop", "coins being counted out one at a time onto a wooden counter, a steady deliberate rhythm of small metal", 12, 0.42, "SFX", "18"),
    one("econ_coin_pickup_single", "A single coin picked up, one small bright metallic clink, %s" % DRY, 0.4, 0.48, 3, "SFX", "18"),
    one("econ_coin_pickup_purse", "A purse of coins lifted, a muffled mass of metal shifting inside leather, %s" % DRY, 0.8, 0.45, 2, "SFX", "18"),
    one("econ_trade_confirm", "A trade agreed, coins pushed across a counter and goods taken up, understated, no fanfare, %s" % DRY, 1.2, 0.42, 1, "SFX", "18"),
    one("econ_vendor_open", "A trader setting out to do business, a ledger opened and goods shifted forward, %s" % DRY, 1.4, 0.40, 1, "SFX", "18"),
    one("econ_vendor_close", "A trader closing up, a ledger shut and goods drawn back, %s" % DRY, 1.4, 0.40, 1, "SFX", "18"),
)
