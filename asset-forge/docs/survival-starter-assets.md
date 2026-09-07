# Knife, cordage and stone axe: first modeling set

Eight draft assets, rebuilt at **12.5 mm cubic pitch** at Matt's request for clearer small-item detail. These are real voxel models, exported as VXA, VOX, and exposed-face OBJ/MTL. They are editable and visible as craftable specs in Asset Forge; they have not been given a human Keep verdict or connected to gameplay recipes. The earlier 25 mm exports are preserved separately.

The [game crafting design guide](../../docs/crafting-system-design-guide.md) is the maintained system reference.

## Crafting roles

| Asset | Role |
|---|---|
| Hammerstone | Reusable shaping tool for preparing stone edges |
| Flake blade | Basic cutting implement and blade component |
| Wrapped stone knife | Blade with supported, wrapped grip |
| Prepared plant-fiber bundle | Input material for cordage |
| Cordage coil | Binding material for grips and hafted tools |
| Wooden axe haft | Shaped handle component |
| Stone axe head | Broad wedge-shaped working head |
| Lashed stone axe | Assembled haft, stone head and securing turns |

The first loose flake can process fibers before a wrapped knife exists; the wrapped knife is an improvement, avoiding a knife-requires-cordage-requires-knife cycle. The prepared haft and axe head are intermediate workpieces, not items the player simply gathers finished. A ground head and dressed haft still need appropriate abrasives and shaping processes; modeling those resource variants is outside this small batch.

## Art and scale

Knife length is approximately 32 cm; the complete axe is approximately 70 cm. The stone palette distinguishes a darker body from working edges and broader facet patches. The rebuild adds tapered edges, shallow flake scars and additional binding turns. Handle radii remain in physical metres when resolution changes. Wood coloring follows the haft rather than independent voxel noise. Lashings wrap the joint. The coil has an open center, securing hitch and a loose end.

Thin fibers and cord turns are intentionally thickened/bundled for readability at 12.5 mm. They are not physically accurate strand diameters. Small stone edges are stepped cubes, not smooth bevels. Contact-sheet views are independently enlarged; dimensions below each image communicate actual scale. Appearance palette IDs do not define chemical composition, material mass, or recipe yield.

## Rebuild and review

Run `python tools/survivalprobe.py` from `asset-forge`. Outputs go to `out/survival-starter-12_5mm/`, including `survival-starter-contact-sheet.png` and `validation.json`.

The probe checks exact pitch, deterministic regeneration, a single face-connected component, no automatic connectivity repairs, pipeline health, and lossless VXA read-back. It exports the actual cubic surface in metres with Z up. Keep OBJ and MTL together.

The generator is `forge/survival_tools.py`; eight artifact forms reuse length, beam, and depth. Other boat-specific sliders are not relevant to this family. Components are separate assets, while the assembled models are single grids; per-component game sockets, damage, physics and crafting logic remain future integration work.
