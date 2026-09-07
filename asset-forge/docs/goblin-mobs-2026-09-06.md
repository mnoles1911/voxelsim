# Goblin mob models (2026-09-06)

Five independent voxel assets now live in Asset Forge's **Creatures / Goblin mobs** subgroup:

| Asset | Height parameter | Role and modeled equipment |
| --- | --- | --- |
| goblin-raider | 1.25 m | Iron axe, rimmed shield, single shoulder plate |
| goblin-spearhunter | 1.17 m | Long spear, back quiver and fletched darts |
| goblin-stalker | 1.08 m | Paired curved daggers, close cowl and dark cloth |
| goblin-hexer | 1.20 m | Forked bone staff, blue focus, mantle and pointed hood |
| goblin-brute | 1.68 m | Spiked war club, shoulder spikes and knuckle armor |

All five export at the finest supported creature pitch: **1.25 cm / 12.5 mm**. Height is the anatomical unit; a weapon, ear or hat can extend beyond it. Faces include eye sockets, brows, nostrils, projecting jaws and paired tusks. Limbs have separate muscle masses and joints; feet and hands have explicit digits. Clothing, straps, bracers and equipment overlap physical attachment surfaces.

`forge/goblin.py` is a dedicated generator selected by `goblin.role`. These assets retain `kind=quadruped` because that existing engine kind already includes bipedal land creatures. `subcategory=goblin-mobs` provides the browsing classification without changing the binary VXM kind-count contract. The Forge panel hides ordinary quadruped anatomy sliders for an active goblin and exposes goblin height, bulk, skin, cloth and archetype controls.

The shared part vocabulary adds arm, forearm, hand, shin, foot and weapon IDs without renumbering earlier IDs. Paired joint chains preserve side: a right hand hangs from the right forearm. The exported VXA contains voxel tags and contact-derived joint origins. This is authoring geometry and rig metadata; AI, attacks, runtime animation clips and spawning behavior are separate game features.

Run `python tools/goblinprobe.py` for determinism, finest-pitch, one face-connected component, complete part tags, all joint contacts and VXA material/part/pitch round trips. Add `--write` to regenerate the five `library/goblin-*/goblin-*-0001/` entries (VOX, VXA, thumbnail, spec, realized spec and metadata). This does not invoke Keep or change curation verdicts. The authored finest-pitch models are the verified outputs; coarser preview tiers may merge or lose small anatomical features.

Review image: `out/goblin-review/goblin-lineup.png`. Five final thumbnails were visually reviewed. Verification output: `out/goblin-review/verification.json`.
