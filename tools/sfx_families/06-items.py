# Category 06 -- Interactive Objects & Items      docs/sfx-library.md section 8
#
# NOTE ON TWO ID COLLISIONS, deliberately resolved here rather than silently:
# the library lists inventory_open/close and quickslot_cycle/activate in BOTH
# Cat 06 and Cat 15 (UI). They are one sound each, not two, so they are
# defined once in Cat 15 (the UI bus owns them) and are NOT repeated here.
# The lint would have caught the duplicate; this comment records the choice.

add(
    one("item_pickup_generic", "A small object picked up off the ground, a light handling rustle and shift, %s" % DRY, 0.5, 0.40, 5, "SFX", "06"),
    one("item_pickup_metal", "A metal object picked up, a light metallic clink and slide into the hand, %s" % DRY, 0.5, 0.45, 3, "SFX", "06"),
    one("item_pickup_cloth", "A cloth item picked up, a soft fabric gather and lift, %s" % DRY, 0.5, 0.35, 3, "SFX", "06"),
    one("item_pickup_potion", "A glass vial picked up, a small glass clink with liquid shifting inside, %s" % DRY, 0.5, 0.45, 3, "SFX", "06"),
    one("item_pickup_voxeldrop", "A dropped block picked up, a short dry granular scoop into the hand, %s" % DRY, 0.4, 0.40, 3, "SFX", "06"),
    one("item_drop", "An object dropped onto the ground, a single dull landing thud and settle, %s" % DRY, 0.6, 0.42, 4, "SFX", "06"),
    one("item_equip_weapon", "A weapon taken to hand and set ready, leather and steel shifting into position, %s" % DRY, 0.8, 0.45, 3, "SFX", "06"),
    one("item_equip_shield", "A shield slung onto the arm, straps pulled and wood settling against the body, %s" % DRY, 0.9, 0.42, 3, "SFX", "06"),
    one("item_equip_torch_offhand", "A torch taken into the off hand, a wooden handle gripped with the flame guttering at the movement, %s" % DRY, 0.7, 0.40, 2, "SFX", "06"),
    one("item_equip_head", "A helmet pulled on, padding compressing and steel settling over the ears, %s" % DRY, 0.9, 0.42, 3, "SFX", "06"),
    one("item_equip_body_cloth", "A cloth garment pulled on over the head, fabric dragging and settling, %s" % DRY, 1.2, 0.35, 3, "SFX", "06"),
    one("item_equip_body_mail", "A mail hauberk pulled on, a heavy cascade of iron rings dropping into place over the shoulders, %s" % DRY, 1.6, 0.42, 3, "SFX", "06"),
    one("item_equip_body_plate", "A plate cuirass closed onto the body, heavy steel halves meeting and buckles being drawn tight, %s" % DRY, 2.0, 0.45, 3, "SFX", "06"),
    one("item_equip_hands", "Gloves pulled on, leather stretching over knuckles with a final tug, %s" % DRY, 0.9, 0.38, 3, "SFX", "06"),
    one("item_equip_boots", "Boots pulled on, leather dragging over the heel and the foot settling to the floor, %s" % DRY, 1.2, 0.38, 3, "SFX", "06"),
    one("item_twohander_offhand_clear_warn", "A short dull warning knock as an off-hand item is forced away to free both hands, %s" % DRY, 0.5, 0.45, 1, "SFX", "06"),
    one("item_slot_move", "An item moved between inventory slots, a soft dry handling shift, minimal, %s" % DRY, 0.3, 0.42, 3, "UI", "06"),
    one("item_quickslot_assign", "An item assigned to a quick slot, a short dry confirming tap, minimal, %s" % DRY, 0.3, 0.45, 2, "UI", "06"),
    one("item_grid_drag", "An item dragged across an inventory grid, a faint continuous handling slide, very quiet, %s" % DRY, 0.5, 0.38, 2, "UI", "06"),
)

# Containers and doors. Four door archetypes, each with an open and a close.
DOORS = {
    "wood":    ("a plain wooden door", "hinges creaking and the plank door swinging"),
    "heavy":   ("a heavy iron-banded door", "deep hinge groan and enormous weight moving"),
    "archive": ("an old archive door", "a dry precise swing with a faint echo of a large quiet room beyond"),
    "shack":   ("a poor shack door", "a thin rattling board scraping its frame"),
}
for d, (dname, ddetail) in DOORS.items():
    add(one("door_open_%s" % d,
            "%s opening, %s, ending as it comes to rest, %s" % (dname.capitalize(), ddetail, DRY),
            1.4, 0.42, 3, "SFX", "06"))
    add(one("door_close_%s" % d,
            "%s closing, %s, ending in a solid latching thud, %s" % (dname.capitalize(), ddetail, DRY),
            1.4, 0.42, 3, "SFX", "06"))

add(
    one("chest_open", "A wooden chest lid lifted open, hinges creaking and the lid coming to rest back, %s" % DRY, 1.2, 0.42, 3, "SFX", "06"),
    one("chest_close", "A wooden chest lid dropped closed, a solid wooden thud with the latch settling, %s" % DRY, 0.9, 0.42, 3, "SFX", "06"),
    one("chest_locked_rattle", "A locked chest lid pulled against its lock, a short frustrated rattle that does not open, %s" % DRY, 0.7, 0.45, 2, "SFX", "06"),
    one("lid_creak", "An old lid moving slowly on dry hinges, a long thin creak, %s" % DRY, 1.4, 0.40, 3, "SFX", "06"),
    one("cache_open", "A hidden cache opened, a stone or board shifted aside revealing a space beneath, %s" % DRY, 1.4, 0.42, 2, "SFX", "06"),
    one("corpse_loot_rustle", "A body searched for possessions, cloth and mail shifted aside with small objects moving, %s" % DRY, 1.6, 0.38, 4, "SFX", "06"),
    one("drawer_slide", "A wooden drawer pulled open, timber sliding on timber ending in a stop, %s" % DRY, 1.0, 0.42, 3, "SFX", "06"),
    one("gate_iron", "A heavy iron gate swinging, a long metallic groan of hinges ending in a clang, %s" % DRY, 2.0, 0.45, 3, "SFX", "06"),
    one("item_torch_light", "A torch catching light, a soft ignition whoosh settling into flame, %s" % DRY, 1.4, 0.40, 2, "SFX", "06"),
    loop_("item_torch_burn_loop", "a handheld torch burning, a close steady flame flutter with pitch crackling", 12, 0.35, "SFX", "06"),
    one("item_torch_extinguish", "A torch put out, a sharp hiss and the flame dying to smoke, %s" % DRY, 1.2, 0.40, 2, "SFX", "06"),
    loop_("brazier_loop", "a standing brazier burning, a steady contained fire with occasional ember pops", 14, 0.35, "Ambient", "06"),
    one("item_bandage_tear", "A strip of linen torn for a bandage, a sharp fabric rip and wrapping, %s" % DRY, 1.2, 0.40, 3, "SFX", "06"),
    one("item_potion_gulp", "A person drinking a potion down in one, wet swallows and a final exhale, %s" % DRY, 1.6, 0.38, 3, "Voice", "06"),
    one("item_coating_apply", "A coating smeared along a blade, a slow wet drag of oil on steel, %s" % DRY, 1.4, 0.38, 2, "SFX", "06"),
    one("item_food_eat", "A person eating, chewing and swallowing a mouthful of food, %s" % DRY, 1.6, 0.35, 4, "Voice", "06"),
    one("item_drink_skin", "A person drinking from a waterskin, leather squeezing with wet gulps, %s" % DRY, 1.6, 0.38, 3, "Voice", "06"),
    one("item_whetstone_scrape", "A whetstone drawn along a blade edge, one long deliberate rasping stroke, %s" % DRY, 1.2, 0.45, 4, "SFX", "06"),
    one("item_repairkit_use", "A repair kit worked over damaged gear, small tools tapping and leather being drawn tight, %s" % DRY, 1.8, 0.40, 3, "SFX", "06"),
    one("item_condition_warn", "A short dull creak of failing equipment warning it is close to breaking, understated, %s" % DRY, 0.6, 0.42, 1, "SFX", "06"),
    one("item_break_dull", "A piece of equipment breaking, a dead snapping crack with no ring, %s" % DRY, 0.7, 0.48, 2, "SFX", "06"),
    one("save_wax_seal", "A wax seal pressed onto a document, a soft press into warm wax and the seal lifting away, deliberately understated, no jingle, %s" % DRY, 1.0, 0.42, 1, "UI", "06"),
)
