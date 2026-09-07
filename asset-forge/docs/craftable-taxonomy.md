# Craftable item taxonomy

Craftable items are grouped by their role, independently of the procedural
generator stored in `kind`. Classification uses the existing hash-excluded
`subcategory` field, so reorganizing an asset does not change its geometry or seed.

| Subcategory | Current assets |
|---|---|
| Tools (`tools`) | Hammerstone, flake blade, stone knife, assembled stone axe |
| Materials (`materials`) | Prepared plant fibers, cordage coil |
| Components (`components`) | Wooden axe haft, stone axe head |
| Vehicles (`vehicles`) | Canoe, log raft, bamboo raft, glider |

A flake blade is filed as a tool because it can cut independently; it can also
serve as a knife component in recipes. Cordage remains a material even when a
recipe uses it to bind components. Components are shaped workpieces intended
for assembly. Vehicles carry or transport players and cargo.

Forge's second selector and the Asset Library filter use these subcategories.
The shared `artifact` generator is labeled “Craftable objects”; it is not a
vehicle classification. New subcategories can be created through New asset type
and appear in the browsing controls as soon as a spec uses them. Uncategorized
craftables appear under Ungrouped rather than being assumed to be vehicles.

Future groups such as Containers, Stations, Structures, Weapons and Armor should
be added when their first assets are authored. The current menus show populated
groups, so users do not encounter empty catalogs.
