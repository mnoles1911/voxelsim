# Craftable asset production roadmap

6 September 2026 · proposed catalog · **handheld starter items use 0.0125 m cubic voxels; the raft remains at 0.025 m**. Matt requested this exception after reviewing the original 25 mm tools. The older dimensional guidance below remains applicable to larger craftables; use 12.5 mm for the knife, cordage, axe and their components.

The bamboo raft, seed 1, was approved by Matt and kept in the local Asset Forge library on this date. Other rows below are planned assets, not claims of completed models or gameplay. The system design lives in [crafting progression](../../docs/crafting-system-design-guide.md).

## First batch: the survival-to-copper loop

Model in the listed order so each group completes a usable crafting chain. Dimensions are proposed longest extents; pick sensible multiples of 25 mm during modeling. Hand proportions and first-person readability need in-game checks.

| Order | Asset / family | Approximate extent | Required visible identity |
|---|---|---|---|
| 1 | Hammerstone | 0.125–0.175 m | Asymmetric tough stone, battered working face |
| 2 | Flake knife / scraper | 0.15–0.25 m | Irregular fracture planes, obvious working edge, optional wrapped grip |
| 3 | Fiber bundle and cordage coil | 0.20–0.35 m | Loose fiber versus twisted finished rope, readable knot/tail |
| 4 | Stone axe | 0.60–0.80 m | Broad cutting head, shaped haft, load-bearing binding |
| 5 | Stone adze | 0.50–0.70 m | Cutting blade transverse to handle; distinct from axe |
| 6 | Digging stick | 1.10–1.40 m | Worn tapered end, irregular wood grain |
| 7 | Friction fire kit | 0.25–0.40 m | Board, spindle, tinder represented as a kit |
| 8 | Bone awl / sewing kit | 0.15–0.25 m | Awl plus bundled small implements; no impossible subvoxel needle |
| 9 | Woven basket | 0.35–0.55 m | Open mouth, rim, thick structural weave, stable base |
| 10 | Wooden paddle / pole | 1.50–1.90 m / 2.00–2.50 m | Paddle blade versus plain pushing pole, worn grip |
| 11 | Bushcraft bamboo raft | Existing 4 × 2 m deck | Approved model; uneven culms, nodes, crossbars, lashings and posts |
| 12 | Cooking pot | 0.25–0.35 m | Open cavity, uneven lip, soot on fired state |
| 13 | Storage crock and lid | 0.35–0.50 m | Removable lid, thick rim, contrasting closure |
| 14 | Crucible | 0.20–0.30 m | Pouring feature, thick walls, heat-used state |
| 15 | Tool-head mold family | 0.25–0.40 m | Negative cavity for axe/pick/hammer; clay and fired states |
| 16 | Charcoal pile / fuel basket | 0.35–0.60 m | Fractured dark wood, no uniform black blob |
| 17 | Pit kiln / firing station | 0.90–1.30 m | Clay workpiece loading space, fuel and fired states |
| 18 | Copper axe/pick/hammer heads | 0.20–0.40 m | Distinct functional profiles, cast surface and polished working zones |
| 19 | Hafted copper tools | 0.60–0.90 m | Reuse handles where plausible; show actual mounting style |

This is 19 production groups, not 19 total exported files. Workpiece states and genuine variants create additional assets. Review a coherent contact sheet per group before keeping new designs. Use the approved raft as the existing material/scale reference, not as a requirement that every object have bamboo coloring.

## Next batches

| Batch | Assets | Playable purpose |
|---|---|---|
| Food and textiles | Drying rack, hide frame, spindle, loom, leather bag, fishing net | Longer journeys, sailcloth and workshop inputs |
| Joinery and alloys | Bronze heads, saw, chisel, mallet, workbench, barrel, cart, wedges | Fitted construction and cargo handling |
| Iron workshop | Tongs, bellows, stone/bronze anvil, forge, bloomery, bloom/billet, iron head and fitting families | Full visible ore-to-tool process |
| Mechanical workshop | Waterwheel, wind rotor, shafts, bearing blocks, gears, hand quern, powered mill, helve hammer | Reuse the same work processes with powered throughput |
| Steel and precision | Refining furnace, treatment station, file, auger, plane, pump, tackle | Advanced shaping, drainage, lifting, boatbuilding |
| Water transport expansion | Log raft revision, cargo deck, sail rig, steering fittings, plank boat | Regional alternatives and modular upgrades |

## Shared visual language

Use coherent variation instead of per-voxel color noise. Wood needs grain-aligned bands, knots, localized bark loss, end-grain distinction, and variation between pieces. Stone needs fracture and wear tied to working faces. Copper/bronze/iron need readable profiles and localized oxidation/wear; do not paint every metal with a flat saturated swatch or uniform rust. Clay needs thickness, small asymmetry, soot, and handling wear. Bindings must wrap actual contact points.

Progress should be visible through workmanship as well as material: rough lashings → fitted sockets → standardized fittings. Avoid making amateur objects randomly crooked everywhere. Structural parts still need to meet and transmit force.

Tiny real-world details below 25 mm cannot be reproduced literally with these cubes. Represent needles, hooks, and thin fibers as kits/bundles where possible. A selectively thickened detail must retain a 25 mm grid and be documented as a readability abstraction. Do not silently use smaller voxels or scale the entire asset to fake detail. Visual voxel volume must not determine chemical content, tool mass, or recipe yield.

## Library and game contract

Each approved entry should carry a stable asset ID, seed, `craftable` category, measured voxel pitch and bounds, thumbnail, source spec, and VXA export. Keep gameplay metadata separate: item family, functional material, component slots, use sockets, grip pose, collision, mass, durability, recipe/workpiece state, and animation requirements.

For multipart equipment define head, haft, binding, and working face semantically. For stations distinguish the static body from moving or removable components. Machine pivots and tool sockets must be reviewed before animation. Do not convert every decorative voxel cluster into a simulated rigid body.

Before a group enters the library: inspect several views; confirm exactly 25 mm pitch and sensible world scale; verify no unintended detached components; verify cavities and attachment points; compare variants for meaningful shape/material differences. Before game use: check grip, collision, interaction reach, saves, and gameplay behavior. An approved render is not proof of functional boat buoyancy or tool damage behavior.
