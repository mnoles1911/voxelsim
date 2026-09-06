# F7 determinism boundary (authored)

The written per-client/deterministic boundary list Phase F7 of
[water-ocean-tides-plan-2026-09-04.md](water-ocean-tides-plan-2026-09-04.md)
requires. Purpose: so nobody ever "fixes" a cosmetic divergence into a synced
system, and nobody ever lets a float into the authority layer. Kept as its own
file (linked from the plan) so plan-doc appends do not collide with it.

## The rule, in one sentence

Anything that decides **water presence, level, or gameplay** computes in
integer millimetres from `(spec, epoch)` and shared ledger state and is
**identical on every machine by construction**; anything that only decides
**what water looks like** is per-client by doctrine, may use floats, GPU state
and local time freely, and must **never be read back into a predicate**.

## Deterministic by construction (the authority layer)

| System | Mechanism that makes it deterministic | Pinned by |
|---|---|---|
| Tide (`voxel-core/include/voxelcore/tide.h`) | Pure `f(epoch)`, never saved state. Integer phase via `floorMod`/`floorDiv` (core.h's one division idiom), 4096-entry compile-time Q16 sine LUT — **no libm anywhere** (`sin()` is not bit-specified across libms/compilers/FMA). Per-component floor division, so no evaluation order/grouping/strategy can disagree with another. Datum steps go through `quantiseTideMm` (floor-to-quantum, uniform bins, 25 mm default) which also absorbs the LUT's ~1.2 mm granularity | test_tide.cpp exact-integer pins; test_waterdeterminism.cpp whole-sweep golden (F7) |
| Ocean connectivity (`voxel-core/include/voxelcore/oceanconnect.h`) | Integer-only wet test (`ground < tideNowMm`, strict), row-major border seeds + deep-margin seed guard, FIFO frontier with fixed neighbour order, 4-connected. Engine-free; two machines produce identical `outBits` byte for byte | test_oceanconnect.cpp byte-compare + mutation arms; test_waterdeterminism.cpp multi-fixture multi-tide golden (F7) |
| `TidalDatumSource` (`voxel-core/include/voxelcore/tidal.h`) | Pure int32 arithmetic on the ledger datum: non-tidal passes through bit-exact; connected rides the tide; disconnected holds at the sill; `max()` with the ledger (ledger wins when higher); the `kNoWaterMm` sentinel can never win the max | test_oceanconnect.cpp truth table |
| `WaterSurfaceZAtWorld` inputs (VoxelWaterSubsystem) | Composes only deterministic integers: ledger/basin datum mm + quantised tide offset mm. The float conversion to engine Z happens per-client **on identical integers**, last, and feeds no world-derivation | Phase A off-arm state-identical leg (SCOREBOARD 2026-09-05) |
| Buoyancy (Phase D) | Reads the **same column contract** (`WaterSurfaceZAtWorld`, datum+tide) as the swimmer's submersion predicate — one source of truth for "where the surface is", so physics and the underwater switch cannot disagree with each other or across machines given the epoch. (Server-vs-client vehicle physics *authority* is an MP-architecture decision explicitly out of Phase F) | Phase D legs; column contract shared with `IsUnderwaterAtWorld` |

## Per-client by doctrine (cosmetics — exempt, protected)

| System | The rule that protects it |
|---|---|
| Ripples (`VoxelRippleField`, 512² GPU wave-equation sim) | The **material-only standing rule**: the field feeds material inputs (normals/WPO) and nothing else — no collision, no gameplay predicate, no readback. Divergence between clients is invisible by construction because nothing downstream can observe it |
| Foam (shore SDF + ocean crest/wind foam, F2) | Derived in-shader from the wave field's own octave slopes + wind; reads no gameplay state and writes none. Off arm byte-identical behind `voxel.Water.FoamV2` |
| Caustics (F1) | Additive **light term** in floor/underwater materials only, cvar-gated (`voxel.Water.Caustics`, 0 = byte-identical off arm); a function of world XY + material time that no predicate reads |
| WPO displacement (sheet tessellation waves, ocean grid) | Vertex-shader offset only. Collision, submersion (`IsUnderwaterAtWorld`) and buoyancy all read the tided **datum**, never displaced geometry — the visual surface may disagree with the datum by a wave height, per-client, and nothing breaks |
| Wakes (`AddSweptDisturbance` → RippleField, D4/F5) | Inherits the ripple material-only rule; the ±25.6 m camera-window bound means distant entities are wake-less per-client by design (documented, not fixed) |

Doctrine consequence, stated for the future reader: if two clients ever show
different foam/ripple/caustic patterns at the same spot, that is **working as
designed**. The fix for a *waterline* disagreement is in the authority layer
and its epoch input — never in syncing a cosmetic.

## The named prerequisite: sky-epoch replication

Tide is pure `f(epoch)`; two clients with skewed clocks get skewed **seas**,
not just skewed sunsets. The prerequisite named by F7 —
`VoxelSkySubsystem.h:35-61` (the old TODO location) — is now implemented per
that header's doctrine block: the clock rides the existing `AVoxelEditRelay`
as two scalars (epoch + time scale, the ServerSeed pattern), authority pushes
at a low fixed cadence (`TickReplicatedClock`), clients adopt with a bounded
smooth correction (`AdoptReplicatedEpoch`), standalone untouched. Full MP
session gates remain deferred until an MP test harness exists — stated, not
hidden.

## F7 test pins (what enforces this page)

`voxel-core/tests/test_waterdeterminism.cpp`: two independent in-process
evaluations (independently-constructed specs/grids/buffers) of a broad sweep —
`tideOffsetMm`/`quantiseTideMm`/`tideVelocityUmPerS` over 4 specs × epochs
from −4·10¹⁵ to +4·10¹⁵ ms, plus `oceanConnectivityFill` bits+stats over 3
fixture families × 6 tide levels — byte-compared to each other AND to golden
FNV-1a-64 hashes pinned as literals (the cross-process contract, same sense as
test_tide.cpp's pinned table values; the vxctest harness has no golden-file
record/replay mode, so the pinned literal is the recording). Red-armed before
landing: a libm-sin tide rewrite and an 8-connected fill both move the goldens
and fail the pins.
