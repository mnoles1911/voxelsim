# The far terrain is missing because the data is missing

**Owner's report (2026-08-31), verbatim:** *"It only looks like there is voxel
terrain for a few kilometers in any direction from camera. There is no far
distant terrain at all. It looks like a flat plane in all directs and there
appear to be some water waters that have spawned in but with no supporting
ground around it."*

**He was right, and it is not a rendering bug.** The ground he is looking at was
never baked, and the engine renders absent ground as sea level.

## The evidence, in one line from the leg

    Fine tier (window): resident=9 tile(s) | loaded=9 absentOnDisk=66
    missing-tile queries since last log (total 98304) -- sampling beyond
    loaded tile radius -- missing-tile sea-level default.

66 of the ~75 fine tiles that view needs are not on disk. A missing fine tile
does not draw a hole; it returns **sea level**. That is the flat plane.

## Why the lakes float, which was the clue I read past

Lake sheets come from the **baked basin table**, which is present. The ground
under them comes from **fine tiles**, which are not. So the water draws
correctly over terrain that was never there. *The floating water was the
diagnosis, visible in a frame I had already measured and explained away.*

## Why altitude triggers it

At 40 m you see the nine baked tiles nearby, plus coarse-derived ridgelines
past 8 km -- both work. At 1,500 m the frame is dominated by the 2-8 km
mid-field, which is exactly the missing band. Same world, different footprint.

## The structural problem underneath

Fine tiles are 512 px at 1.875 m/px = **960 m square**. The fine tier must
cover everything inside R7 (8,192 m), so the disc is 8.5 tiles in radius:

    ~229 tiles   ~91 GB   ~40 CPU-hours     PER LOCATION

Extending the cascade to 8 km (R7-R10, built this session at the owner's
request) made the fine tier's bake appetite roughly a full-world bake anywhere
the player stands. **That cost was never priced when the rings were added.**

## FOUR REFUTED DIAGNOSES -- do not re-run these

    arm                          engagement proof              result
    "it's a plateau"             --                            WRONG, owner corrected
    vertical keep band widened   peak loaded 57,207 -> 56,922  NULL, admitted nothing
    MarchLevelChunkZ / ZCut      derives from                  CANNOT be the cause:
                                 GetResidentChunkZBound        it follows residency
    marcher chunk-step cap       census armed, 72 blocks       capRays=0 of 27,648,000
    kFirstCoarseLevel 8 -> 5     atlas logged "Serves ring     NULL: absentOnDisk 66->66,
                                 levels >= 5"                  frames identical
                                                               (detail 2.97, sky 72.0%)

The last one is the instructive failure. It looked like the obvious lever and
it could not possibly work, because the fine-tile streamer **blocks a chunk
whose fine footprint is not resident before tier routing is consulted**
(`VoxelWorldSubsystem.cpp:10356`, and three sibling sites at 16603 / 16904 /
17584). A constant downstream of the block cannot reach the decision.

## EIGHT ARMS, EIGHT NULLS -- the complete elimination list

    arm                              engagement proof                image
    vertical keep widened            loaded 57,207 -> 56,922         NULL
    MarchLevelChunkZ / ZCut          derives from residency          impossible
    marcher chunk-step cap           census armed, 72 blocks         capRays=0/27.6M
    kFirstCoarseLevel 8->5 alone     "Serves ring levels >= 5"       NULL
    real coarse tiles alone          loaded=289 rejected=0           NULL
    per-tier surface bound           built, coarse amplifier wired   NULL
    BOTH paired (boundary+tiles)     both lines confirmed            NULL
    ocean plane disabled             voxel.Water.ImplicitOcean 0     NULL

**The ocean arm is the decisive one.** The pale flat sheet SURVIVES with the
ocean off, so it is not the 164 km ocean plane. It is GROUND -- flat ground at
sea level, with the basin-table lake sheets sitting on it. That is precisely
"a missing fine tile reads as sea level", seen directly.

**Why every routing fix failed.** `Voxels` is a `vxc::World` constructed ONCE
against the fine sampler (`VoxelWorldSubsystem.cpp:6420`). Per-level routing at
:22908 redirects the RASTER WINDOW only; actual voxel generation still reads
fine, still gets sea level, still builds flat ground. Routing around absent data
cannot work when generation is hard-bound to the tier that lacks it.

**The elevation profile says the real ground out there is 2,324-3,091 m**, not
sea level. So the flat sheet is not a plateau and not the ocean: it is absent
data being rendered honestly.

## The two real options -- an OWNER decision, not an engineering one

**(a) Bake the tiles.** ~223 new tiles, ~91 GB, ~40 CPU-h, fixes this location
only. Same cost again wherever he flies next.

**(b) Let coarse-derived levels generate from coarse data.** The block exists
on purpose -- `"BLOCKED, never generated from a coarse guess"` -- to stop fake
terrain appearing where real data is expected. Relaxing it *for levels at or
beyond `kFirstCoarseLevel` only* is what the tier split was built for, and it
is what "outer blocks can read as coarse" would buy. It costs a design change
to a safety policy and needs an image verdict.

(b) is the general fix; (a) is the local one. They are not exclusive.
