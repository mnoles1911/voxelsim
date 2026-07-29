# Terrain amplification proposal vs ADR-0006 — reconciliation

**Status:** analysis only. The proposal was **PARKED** behind ADR-0006 by Matt's
decision (2026-07-24).

> **SUPERSEDED 2026-07-28 — read `docs/terrain-amplification-plan.md` instead.**
> That plan unparks this work. All three collisions recorded below (integer
> mirror cost, O(1) random access, granularity) dissolve once §6's geomorphic
> passes move off the client and into the **server-side tile bake**: baked bytes
> need no HLSL mirror, no fixed-point rewrite, and no relaxation at query time.
> The "Networked constraint" objection is resolved the same way — the baked fine
> tier becomes a hard dependency, never a progressive enhancement. The analysis
> below is still accurate and still worth reading; only the verdict changed.

**Source:** `docs/research/terrain-amplification-design-doc.md` (copied into the
repo so it is not stranded outside it). Section numbers below refer to that doc.

**Why this file exists:** the proposal is written against a generic
"UE5 client with a voxel chunk streamer" and does not know this project's
determinism doctrine, its CPU/GPU mirror contract, or its chunk granularity.
Three of its stages collide with those. Recording the analysis so the decisions
are not re-derived — or worse, missed — when the work resumes.

---

## Verdict: complementary, not competing

They load **opposite halves** of the architecture.

- **ADR-0006** relieves the *display* path: per-chunk `FScene::AddPrimitive` →
  GPU-resident geometry drawn via O(1) primitives and indirect draws.
- **This proposal** loads the *authority* path: `materialAt`, which collision,
  digging, water CA and replication all read. Its clasts (§7), stratigraphy
  (§8.2) and overhangs (§8.3) are all *materials you can dig*, so none of it can
  live behind ADR-0006's display-only carve-out.

Better than merely compatible: **ADR-0006 creates the headroom this proposal
spends.** Today the CPU generates every streamed chunk — the `(32+2)²`
`Amplifier::column` grid alone is ~42% of a level-0 job. Under ADR-0006 that
whole-chunk generation moves to the GPU for display, leaving the CPU amplifier
serving only sparse on-demand queries (a raycast, a dig region). That is exactly
the budget a much heavier amplifier needs.

---

## Collision A — every stage must be written twice, in integers

`docs/determinism.md`: *"No floating point anywhere in world derivation.
`float`/`double` are banned in `voxel-core/src` and `voxel-core/include` (CI
greps for them)."* And `voxelcore/biome.h` states the mirror contract outright:
*"this logic is mirrored bit-for-bit in worldgen.ush's ColumnMain. ANY change
here must be made identically in both places and re-verified by vxc_gpu."*

**ADR-0006 does not relax this.** Its invariant 3 exempts the *mesh* from
cross-vendor bit-exactness, explicitly not voxel state; the §2 determinism gate
(columns → voxelize → digest parity) is unchanged.

The proposal is written in floats throughout: softmax blend weights (§4),
per-octave gains `g[regime][o]` (§5.2), `depth = a * flow^b` with b≈0.3–0.4
(§6.2), smooth-min SDF unions (§7), fractional thermal transfer (§6.1).

Every stage therefore needs: a fixed-point redesign, an HLSL mirror, and a
digest-parity test. Call it **~2× the implementation cost per stage**, plus a
`kWorldGenVersion` bump per landing (world-breaking: invalidates saved edit logs
and goldens). The roadmap's "P0: 1–2 wks" is optimistic by a large factor.

Precedent exists — ADR-0004 already does fixed-point coupling for SWE — so this
is a known cost, not an unknown one. `flow^0.35` specifically needs a versioned
integer lookup table or fixed-point log/exp.

## Collision B — §6 breaks O(1) random access

§6.1 thermal relaxation runs K=8–16 iterations over a working grid; §6.2 rill
carving needs local flow routing over tile+apron. Both make the surface height at
a point a function of a *relaxation over a neighbourhood*, not of the point.

Collision, digging and water do **point** queries. And under ADR-0006 the CPU
path becomes *purely* sparse point queries — precisely the access pattern a
grid-iterative generator is worst at. A single collision query would have to
materialise a whole tile grid.

The proposal's §9 rule 3 claims it "preserves O(1) random access." For
tile-batch generation that is true. For a point query it is not.

Mitigation if §6 is ever wanted: a cached working-grid tier (see Collision C).
There is no way to make it display-only — falling through visibly solid ground
is not an option in a digging game.

## Collision C — granularity mismatch

The doc assumes 512 m regions with a 16-cell apron: ~1.6% overhead. **R0 chunks
here are 3.2 m.** A 4 m apron on a 3.2 m chunk is >100% overhead.

So §6 cannot run per chunk. It needs an intermediate cached "region" tier between
tiles and chunks (compute the 25 cm working grid per ~51.2 m region, cache it,
have chunks sample it). That is a genuinely new architectural layer, and it is
the single largest hidden cost in the proposal.

## Networked constraint — §2's fallback is a desync hazard

Matt confirmed (2026-07-24) that the authority model matters: multiple clients
must agree on voxel state.

§2 proposes: *"if `T1` is late, client renders from `T30` + procedural stages
only, and hot-swaps when `T1` arrives."* Two clients with different tile
availability would compute **different terrain** — different collision, and edits
applied over different bases. That is a desync, not a visual pop.

If the learned-SR stage ever lands, `T1` must be a **hard dependency** with the
server pinning the authoritative version, never a progressive enhancement.

---

## Good news: three stages substantially exist

| Proposal | Already in `voxel-core` |
|---|---|
| §4 regime classifier ("the 250-line layer") | `biome.h` — morphology gates + Whittaker climate table, integer-only, GPU-mirrored. **Extend, don't rebuild.** |
| §3 flow accumulation | `rivernet.h` — D8 steepest-descent flow accumulation, integer/fixed-point, already builds a segment graph |
| §8.2 stratigraphy | `ColumnSample` already carries `topsoilMm` / `subsoilMm` / `bedrockDepthMm` / `surfaceMat` |

Also already present and relevant: `caves.h`, `caverns.h` (the underground work
Matt wants refactored) and `CaveColumn`/`CavernColumn` carried in `ColumnSample`.

## The useful split — most of the payoff needs no architecture change

| O(1)-compatible today | Needs the new region tier |
|---|---|
| §4 regime (extend `biome.h`) | §6.1 thermal relaxation |
| §5 regime-conditioned spectral synthesis | §6.2 rill / channel carving |
| §7 clast scattering (hash → SDF, bounded neighbourhood) | §6.3 deposition / soil smoothing |
| §8.2 stratigraphy extension | |
| §8.3 strata banding, ledges, bank undercuts (pure 3D noise) | |

**Only §6 ruptures the model.** Calibrated spectra, boulders, bedding planes,
ledged cliffs, undercut banks and soil profiles — the bulk of the "standing at
10 cm" payoff — all fit the current pure-function architecture directly.

---

## Decisions recorded (Matt, 2026-07-24)

1. **Amplification is parked** until the GPU path lands. Sequencing was offered
   as "ADR-0006 first with the O(1)-safe stages running alongside in
   `voxel-core`"; Matt chose full park instead. Accepted cost: terrain keeps
   reading as noise while the GPU path is built.
2. **§6 is deferred behind §5/§7/§8** even once work resumes — ship the
   O(1)-compatible stages, look at the result, then judge whether the causal
   structures (talus aprons, connected rills) justify the region tier. They may
   not be needed.
3. **Learned SR (§2) is a maybe**, gated on GPU access. Keep §4–§8 written
   against a generic "parent heightfield + channels" interface — as the doc's own
   design rule requires — so SR can drop in later with no rework. Build no
   serving path now.

## If it resumes, the order that respects all of the above

1. §4 regime — extend `biome.h`, add the debug colorizer first (the doc is right
   that it is the highest-leverage tuning tool).
2. §5 spectral synthesis — replace, do not layer over, the current noise;
   integer fixed-point gains, HLSL mirror, digest parity, `kWorldGenVersion` bump.
3. §8.2/§8.3 stratigraphy + strata banding — extends `ColumnSample`, cheapest
   realism-per-line in the whole doc.
4. §7 clasts — hash-placed SDFs, bounded per-chunk list.
5. Re-evaluate §6 with real screenshots in hand.
