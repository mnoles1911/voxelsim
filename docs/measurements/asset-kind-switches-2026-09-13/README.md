# Per-kind asset switches: `-VoxelAssetKinds=` / `-VoxelNoAssetKinds=`

Owner's ask: "easy levers to turn on and off the inclusion of each Asset Forge
type (trees, bushes, reeds, flowers, creatures, craftables) from in the game",
for development and testing.

`VoxelEcologicalPlacement.cpp`, `ApplyAssetKindSwitches`, runs once per species
table before `NextField.setSpecies`. Kinds: `tree bush rock grass reed flower
fish bird quadruped cetacean`, plus the groups `creature` (fish, bird,
quadruped, cetacean) and `plant` (tree, bush, grass, reed, flower).

- `-VoxelAssetKinds=tree` keeps ONLY the listed kinds;
- `-VoxelNoAssetKinds=bush,grass,reed,flower` drops the listed kinds;
- both given: keep-list first, then drops.

Excluded species get `weightPerMille = 0` — index-stable, so `bankId` (the
manifest row index) and every downstream table are untouched; nothing is
removed from the field, it just never places. The log proves what happened:

```
VoxelAssetKinds: kept/total per kind: tree=49/49 bush=0/20 grass=0/38 reed=0/10 flower=0/45
```

(that line is the `-VoxelAssetKinds=tree` run at the temperate forest fixture;
a run with no flag prints every kind at n/n).

Verified: builds; the trees-only fixture shows trees and nothing else in the
5 m/s moving capture `kindsTreeOnly`. That capture ALSO showed a crater at
256 m which the all-species control did not — recorded open at the time, and
now plausibly the far-ring starvation fixed in
`docs/measurements/ring-normalized-key-2026-09-14/` (asset chunks dispatch far
slower, so the deficit that starves R2/R3 arrives at a lower speed). Not
re-run with the fix.

No Settings row yet; these are command-line switches, which is what the
harnesses can pass.
