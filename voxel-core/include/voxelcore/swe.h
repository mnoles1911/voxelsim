#pragma once
// Shallow-water (depth-averaged 2D) surface flow + force field, and its
// coupling to the per-voxel water CA (plan §4 Water track, W4: "SWE + force
// field" — the item §4 itself lists among the historical slip risks, as
// "W4 CA<->SWE coupling").
//
// STATUS: this header ships DISABLED. `SweGrid` is a standalone reference
// core that nothing in voxel-core or the engine constructs; `SweCaCoupler`
// carries an explicit master flag (`SweCoupleConfig::enabled`) that DEFAULTS
// TO FALSE and makes step() a total no-op. `kWaterCAVersion` is NOT bumped by
// anything in this file and no WaterCA tick rule is touched, so every pinned
// water golden is byte-identical. See docs/adr/0004-swe-fixed-point-coupling.md
// — the numerics and the coupling contract below are PENDING Matt's sign-off,
// and this file exists so that decision is made against running, measured code
// rather than a proposal.
//
// Engine-free and terrain-free exactly like waterca.h/rivernet.h: the only
// terrain dependency is a caller-supplied solidity callback.
//
// =======================================================================
// 1. WHY NOT "REAL" SWE — the numerics decision, in short
// =======================================================================
// The conservative shallow-water equations
//     d(h)/dt   + div(h*u)                     = 0
//     d(h*u)/dt + div(h*u (x) u) + g*h*grad(h+b) = friction
// are conventionally discretised with an approximate Riemann solver (HLL/
// HLLC/Roe). Those need wave speeds u +/- sqrt(g*h) — a square root per face
// per tick — and, more damagingly, they need a WELL-BALANCED (C-property)
// treatment so that the pressure flux and the bed-slope source term cancel
// EXACTLY over a lake at rest. In floating point that cancellation is already
// only approximate and is the single most-written-about failure mode of SWE
// solvers (spurious currents over uneven beds). In fixed point, where every
// term is separately truncated, it is not achievable at all without carrying
// the cancellation symbolically — which is a rewrite of the scheme, not a
// port of it. voxel-core is integer-only (doctrine §2.3, CI `float-ban`)
// because worldgen and simulation must be bit-identical across machines and
// GPU vendors, and the M0 cross-vendor gate failed for exactly this class of
// reason. So "port the textbook solver to fixed point" is the wrong shape of
// answer.
//
// What ships here instead is the VIRTUAL-PIPE reduced SWE (the Mei-class
// hydraulic-network formulation used in production terrain-erosion and game
// water): the same depth-averaged mass equation, solved conservatively, with
// the momentum equation LINEARISED — the nonlinear advection term
// div(h*u (x) u) is dropped and momentum is carried as a per-face stateful
// flux driven by the hydraulic head difference and bled by a damping term
// standing in for friction. This is a documented physical simplification (it
// loses supercritical/shock behaviour — a true dam-break bore front travels
// at the wrong speed, and hydraulic jumps are not resolved), NOT a numerical
// approximation of a scheme we wish we had. And in exchange it is, in
// integers, strictly BETTER behaved than the faithful scheme:
//
//   * Lake at rest is EXACT, not approximately exact. Head difference zero
//     => flux increment identically zero => the damping term decays the
//     stored flux to exactly 0 in finitely many ticks (see `shiftSym` below
//     for the one trap that has to be dodged to make that true) => zero
//     transport, forever, over an arbitrarily uneven bed. There is no
//     source-term cancellation to get wrong because there is no source term:
//     the bed enters only through `head = bed*255 + depth`, one addition.
//   * Mass conservation is STRUCTURAL, in the identical sense waterca.h
//     means it: a face's transported amount is one integer, subtracted from
//     exactly one column and added to exactly one column, in the same loop.
//     Summed over all faces the net change is exactly zero every tick. There
//     is no rounding residue to leak, because nothing is ever rounded ON the
//     conserved quantity — only on the (non-conserved) momentum accumulator.
//   * It cannot blow up. Depth is clamped to [0, maxColumnDepth] by the
//     source-side outflow cap and the target-side headroom cap; the flux
//     accumulator is clamped to +/-maxFluxPerTick. Instability in a float
//     solver means NaN/inf and a dead frame; here the worst reachable state
//     is a bounded, ugly oscillation. The CFL condition (section 4) is
//     therefore a QUALITY constraint, not a safety one.
//   * No sqrt, no division in the hot path except the one exactly-remaindered
//     division in the outflow cap.
//
// The honest cost is written down and quantified in section 4 (a settle
// deadband, and the loss of shock physics). ADR-0004 is where that trade is
// put to Matt.
//
// =======================================================================
// 2. STATE AND UNITS (all integer; no float appears in this subsystem)
// =======================================================================
// The SWE domain is a 2D grid of COLUMNS at voxel pitch. Per column:
//   bed   : int32, the voxel z of the topmost SOLID voxel the sheet rests on.
//   depth : int32, total water in the column in FILL UNITS, the same unit
//           WaterCA uses (255 fill units == one full voxel of water). depth
//           is a column TOTAL, not a per-voxel fraction, so a column 6 voxels
//           deep holds depth == 1530. This is the single most important
//           representational choice in the file: it makes the SWE column and
//           a stack of CA cells the SAME currency, so every CA<->SWE transfer
//           in section 5 is an integer move with no unit conversion and
//           therefore no rounding.
//   head  : bed*255 + depth, in fill units. A genuine elevation measure,
//           because 255 fill units of depth is exactly one voxel of height.
//
// Per FACE (not per cell — see below):
//   fluxQ8 : int32, the momentum state, in Q8 fill-units per tick (i.e. the
//            transported volume per tick, times 256).
//
// WHY FACE-OWNED FLUX. The classical virtual-pipe formulation stores four
// pipes per cell (L,R,T,B) and must keep f_right(i) == -f_left(i+1) as an
// invariant maintained by the update rule. Here each face is stored ONCE,
// signed, owned by the lower-coordinate column (`fluxX_[i]` is the face
// between column i and i+1 in x; positive means flow in the + direction).
// The antisymmetry is then not an invariant to maintain but a property of the
// representation, which removes an entire class of conservation bug and
// halves the state. It is also what makes the apply phase's conservation
// argument a one-liner (section 3, phase 4).
//
// WHY Q8 ON THE FLUX. The flux accumulator carries 8 fractional bits while
// the transported amount is `fluxQ8 >> 8`. Without those bits the scheme has
// a hard DEADBAND: a head difference smaller than 2^gainShift produces a
// truncated-to-zero increment, so the surface settles only to within half a
// voxel and visibly steps. With them, an arbitrarily small head difference
// still produces a nonzero sub-unit increment that ACCUMULATES tick over tick
// until it crosses a whole unit. The residual deadband is then set by the
// damping-limited steady state rather than by truncation, and is 16 fill
// units (6.3% of a voxel) at the default constants — derived exactly in
// section 4. Quantifying this is the entire point of carrying the extra
// bits, and the number is what ADR-0004 asks Matt to accept.
//
// =======================================================================
// 3. THE TICK — four phases, Jacobi, order-independent by construction
// =======================================================================
// Unlike waterca.h's Phase READ/APPLY, this needs NO 8-way colouring. The CA
// colours its rounds because its lateral rule is an INSTANTANEOUS
// equalization (`flow = (self - neighbour)/2`), which off a single shared
// snapshot checkerboards and, in a small closed loop, can cycle forever. The
// pipe rule is not an equalization: it is a rate-limited, damped, stateful
// flux. A plain simultaneous (Jacobi) update off one tick-start snapshot is
// its correct and standard formulation, and it is the formulation a GPU port
// wants. One tick is therefore ONE pass, in four phases:
//
//   PHASE 1 — FLUX UPDATE (per face, reads tick-start state only)
//     d  = head(A) - head(B)                    // A = lower-coord side
//     f' = shiftSym(f * dampingQ8, 8) + shiftSym(d << 8, gainShift)
//     f' = clamp(f', +/- (maxFluxPerTick << 8))
//     Faces on the grid's outer boundary have no partner column and are hard
//     walls: flux forced to exactly 0. (The reference grid is closed; the
//     real perimeter is the coupler's job, section 5.)
//
//   PHASE 2 — SOURCE-SIDE CAP ("a column never sends more than it has")
//     transport(face) = shiftSym(f', 8). For each column, its up-to-4
//     OUTGOING transports (the faces where the sign points away from it) are
//     summed; if that sum exceeds the column's depth, every one of them is
//     scaled by depth/sum with integer floor, and the remainder
//     (depth - sum-of-floors) is handed out one unit each to the first
//     `remainder` faces in the FIXED order +x, -x, +y, -y. Sum of the scaled
//     amounts is then exactly `depth`, by construction — the same
//     floor-plus-explicit-remainder trick Phase C of waterca.h uses, and it
//     is why depth can never go negative and no unit is ever lost to
//     rounding. Every face has exactly ONE side it flows away from, so no
//     face is capped by two columns and there is no write race.
//
//   PHASE 3 — TARGET-SIDE CAP ("a column never receives past its capacity")
//     Each column gathers its up-to-4 INBOUND amounts, in the same fixed
//     order, against a budget of (maxColumnDepth - depth); whatever is left
//     when the budget hits zero is admitted as 0. Every face has exactly one
//     side it flows INTO, so again each face is visited by exactly one
//     column. This mirrors waterca.h's GATHER phase and inherits its
//     argument verbatim.
//
//   PHASE 4 — APPLY
//     For every face, its final admitted magnitude m is SUBTRACTED from the
//     source column's delta and ADDED to the target column's delta, in the
//     same statement. Summed over all faces the total delta is therefore
//     exactly zero — conservation is a property of the loop, not a checked
//     invariant. The face's stored flux is then set to the full-precision
//     f' if the face was NOT capped (momentum, including its sub-unit part,
//     survives untouched — this is what makes the sheet slosh rather than
//     creep) and to (m << 8) if it WAS (a column cannot keep momentum for
//     water it does not have).
//
// ORDER INDEPENDENCE. Every phase either reads only tick-start state (1) or
// touches each face from exactly one side (2,3,4). `stepWithColumnOrder()`
// exposes this the way `WaterCA::stepWithOrder` does: feed the columns in any
// permutation and get byte-identical resulting depths, fluxes and digest.
// step() is stepWithColumnOrder(identity).
//
// =======================================================================
// 4. STABILITY, THE DEADBAND, AND WHAT IS NOT MODELLED
// =======================================================================
// STABILITY (the CFL analogue). Linearising two adjacent columns about a flat
// state, with damping D = dampingQ8/256 and gain G = 2^-gainShift, the tick
// map on (head difference, flux) has characteristic polynomial
//     z^2 - (1 + D) z + (D + L*G) = 0
// where L is the largest eigenvalue of the discrete Laplacian on the grid
// (4 in 1D, 8 in 2D). Both roots lie inside the unit circle iff the product
// of the roots is below 1, i.e.
//     D + L*G < 1        =>  dampingQ8 + ((L << 8) >> gainShift) < 256.
// `SweConfig::stableIn2D()` is exactly that inequality with L = 8, evaluated
// in integers, and the constructor CLAMPS an unstable config rather than
// trusting the caller. At the defaults (dampingQ8 = 224, gainShift = 7) the
// left side is 224 + 16 = 240 < 256, a ~7% margin. This is a genuine CFL-type
// constraint and it is the reason the constants are not free parameters a
// designer may tune without re-checking. Note that violating it does not
// produce NaN — depth stays clamped in [0, maxColumnDepth] regardless (see
// section 1) — it produces sustained ringing.
//
// THE DEADBAND (the honest cost, quantified). Hold a head difference d fixed;
// the flux accumulator's steady state is f* = d * 2^(8-gainShift) * 256 /
// (256 - dampingQ8), and transport is nonzero only once f* >= 256. So the
// smallest head difference this scheme will act on is
//     d_min = 2^gainShift * (256 - dampingQ8) / 256
// which at the defaults is 128 * 32 / 256 = 16 fill units = 6.3% of one
// voxel. A settled SWE surface is therefore flat to +/-16 fill units, against
// WaterCA Phase C's +/-1. `sweSettleTolerance()` computes it so tests assert
// against the derivation rather than a magic number. Lowering it is a pure
// trade against the stability margin above (raise dampingQ8 toward 256 and
// d_min falls, but so does the margin) and that trade is one of the things
// ADR-0004 puts to Matt.
//
// NOT MODELLED, deliberately: supercritical flow, hydraulic jumps, and the
// correct dam-break bore speed (all consequences of dropping the advection
// term); vertical structure of any kind (the depth-averaged assumption — the
// coupler's promotion predicate in section 5 is precisely the test for where
// that assumption is legitimate); and surface tension/turbulence (W5).
//
// =======================================================================
// 5. THE CA <-> SWE COUPLING
// =======================================================================
// The two solvers model genuinely different regimes and the coupling must not
// pretend otherwise. The CA is a 3D volumetric automaton: correct in confined
// space (caves, pipes, a breach, a drain shaft, an inrush), where flow is not
// depth-averageable and the vertical direction carries real structure. SWE is
// a 2D depth-averaged sheet: correct on a large open free surface (lake,
// ocean shelf, floodplain), where the CA is both wrong (Phase C equalises a
// body INSTANTLY to a static level, so a lake has no waves, no current and no
// force to push a boat) and expensive (a whole-body flood fill per tick).
//
// OWNERSHIP — the answer to "what owns a cell that is in both": NOTHING is
// ever in both. Ownership is a partition, decided per COLUMN and enforced per
// VOXEL, and the two solvers never write the same voxel in the same tick:
//
//   * A column (x,y) is either in the SWE domain or it is not.
//   * If it IS: the voxels from bed+1 up to the top of the sheet are
//     SWE-owned. The CA holds NO fill there — the coupler's absorb step
//     (below) is what guarantees that, by evacuating any CA fill that appears
//     inside the sheet's z-range into the column's `depth` in the same tick
//     it appears. Voxels at or below `bed` (subsurface: the cave under the
//     lake, the aquifer, the drain shaft) stay CA-owned unconditionally. So
//     "a cave under a lake" is not a conflict, it is the ordinary case: the
//     lake is SWE, the cave is CA, and the solid bed between them is the
//     ownership boundary.
//   * If it is NOT: every voxel in the column is CA-owned. The CA is
//     unmodified and unaware.
//
// So the boundary is a SURFACE, not a volume, and it is always a solid bed or
// a domain perimeter — never a shared cell. That is what makes the exchange
// expressible as a ledgered integer transfer instead of a flux-matching
// condition between two different discretisations.
//
// The partition is enforced on BOTH sides, and it took two mechanisms:
//   * SWE side: a CA-owned column is marked inactive (`setColumnActive`), so
//     the tick rules treat it as a hard wall and the sheet cannot push water
//     into it at all.
//   * CA side: the absorb channel (b) evacuates CA fill that appears inside
//     an SWE column's sheet range.
// Be precise about what that second one guarantees, because it is rate
// limited (mechanism 3 below) and therefore weaker than the first. The
// GUARANTEE is: no voxel is ever WRITTEN by both solvers, and CA fill inside
// a sheet range is drained at up to absorbPerTick per column per tick. It is
// NOT "such fill is always instantaneously zero": a CA source pushing water
// into a sheet faster than absorbPerTick leaves a bounded standing residue
// until it stops. That residue is conserved and correctly ledgered — it is
// simply water in transit across the boundary, exactly like the metered
// inrush in the other direction — but it does mean a renderer must draw the
// union of the two, not the sheet alone. Sizing absorbPerTick against the
// fastest CA inflow a design permits is a tuning question, and it is one of
// the things ADR-0004 flags rather than silently picks.
//
// EXCHANGE — three channels, each a single integer move recorded in both
// ledgers, so that CA.totalVolume() + SweGrid::totalVolume() is invariant
// across any sequence of coupled ticks (the headline test):
//
//   (a) DRAIN, SWE -> CA. "Dig a hole in the lake floor." Every tick the
//       coupler re-reads solidity at the column's `bed`. If that voxel is no
//       longer solid, the column is PUNCTURED: min(depth, drainPerTick) fill
//       units leave `depth` and are placed into the CA voxel at (x,y,bed) via
//       `WaterCA::addWaterAt`, which wakes the owning brick so the CA carries
//       it away in 3D from there. Only what the CA actually accepted is
//       debited (`addWaterAt` returns the placed amount), so a full cell below
//       simply back-pressures the drain instead of destroying volume.
//       The bed is deliberately NOT re-seated downward to follow the hole.
//       Doing so was the first design and it is WRONG: it would move the
//       ownership boundary down over voxels the CA is at that moment carrying
//       water through, putting a cell in both domains — the exact thing the
//       partition above forbids. A punctured column is instead simply a
//       METERED SOURCE into the CA at the puncture voxel, which is also the
//       physically right story (a hole in a lake floor is confined 3D flow,
//       i.e. CA territory by definition of the split). The hand-over completes
//       on its own with no extra machinery: a non-solid bed fails the
//       eligibility predicate, so after `demoteDwellTicks` the column demotes
//       and the CA takes the whole column. The observable behaviour is a
//       metered inrush with visibly falling sheet depth for the dwell window,
//       then a clean full transfer — which is the desired game feel, and it
//       falls out of the hysteresis rather than being special-cased.
//   (b) ABSORB, CA -> SWE. "Water wells up a shaft into the basin", and also
//       the mechanism that enforces the no-shared-cell rule above. Any CA
//       fill found in an SWE column's sheet z-range is removed via
//       `WaterCA::removeWaterAt` and added to `depth`, up to absorbPerTick.
//   (c) MEMBERSHIP CHANGE. Promotion moves a column's CA fill into the sheet
//       (channel (b) run once at full rate); demotion moves the sheet's depth
//       back down into CA cells bottom-up. Both are ledgered identically.
//
// NO OSCILLATION — three separate mechanisms, because this is the failure
// mode the plan flags as W4's risk:
//   1. HYSTERESIS ON MEMBERSHIP. Promotion requires the eligibility predicate
//      to hold for `promoteDwellTicks` CONSECUTIVE ticks; demotion requires
//      it to fail for `demoteDwellTicks` consecutive ticks, and the default
//      demote dwell is 4x the promote dwell. A column on the edge of
//      eligibility therefore cannot chatter between owners: it is a Schmitt
//      trigger in the time domain, and every membership flip costs a full
//      dwell window.
//   2. ONE DIRECTION PER COLUMN PER TICK. Drain and absorb are never both
//      executed on the same column in the same tick; the coupler picks by the
//      puncture test, evaluated once, at tick start. Bidirectional handoff
//      within a tick is the classic way two coupled domains ping-pong volume
//      and it is structurally excluded rather than tuned away.
//   3. RATE LIMITS, NOT EQUALIZATION. Every transfer is capped per tick
//      (drainPerTick / absorbPerTick). Neither side ever tries to reach
//      equilibrium with the other in one tick, so neither can overshoot it;
//      the exchange is a first-order lag, which cannot oscillate.
//
// ELIGIBILITY (where the depth-averaged assumption is legitimate) is a
// concrete, cheap predicate — see `SweCaCoupler::eligible`: the column must
// rest on a solid bed, have at least `openClearanceVoxels` of non-solid space
// above the bed (no lid: a flooded tunnel is NOT a free surface), and have at
// least `minOpenNeighbours` of its 4 lateral neighbours non-solid just above
// the bed (open, not a one-voxel pipe: horizontal scale must exceed depth).
//
// DETERMINISM. Membership, dwell counters and every transfer are pure
// functions of (column set, solidity, CA contents, SWE contents) evaluated in
// a fixed column-index order, with no hashing and no iteration-order
// dependence — the same standard the CA holds itself to. The coupler is
// nonetheless the part of this file with the most determinism surface, which
// is the second reason it defaults OFF.
//
// =======================================================================
// 6. THE FORCE FIELD
// =======================================================================
// waterca.h's Phase C notes that flow MOMENTUM "needs Layer C's SWE patches"
// and is not modelled. The face fluxes ARE that momentum, and `velocityAt()`
// is the force field the plan asks W4 for: the per-column depth-averaged
// velocity that W5 advects foam/particles with, that pushes a boat, and that
// gives a breach its inrush jet instead of an instant level change. Derivation
// is exact and integer: a face carries `f` fill units per tick through a
// cross-section of (depth/255) voxels by 1 voxel, so the horizontal
// displacement is (f/255)/(depth/255) = f/depth voxels per tick, hence
//     v_mm_per_sec = f * kVoxelSizeMm * ticksPerSecond / depth
// with the two opposite faces averaged and C++'s truncate-toward-zero
// division (which, unlike a right shift, is already sign-symmetric).

#include <cstdint>
#include <functional>
#include <vector>

#include "voxelcore/core.h"
#include "voxelcore/waterca.h"

namespace vxc {

// Bumped on any deliberate change to the tick rules in section 3 or the
// coupling rules in section 5, exactly like kWorldGenVersion/kWaterCAVersion/
// kRiverNetVersion. Independent of kWaterCAVersion: nothing in this file
// changes a CA tick rule, so a bump here never invalidates a water golden.
inline constexpr uint32_t kSweVersion = 1;

// Sign-symmetric arithmetic right shift. THE ONE FIXED-POINT TRAP IN THIS
// FILE, and it is load-bearing for the lake-at-rest guarantee in section 1.
// A plain `v >> s` on a negative value rounds toward -infinity, so the damping
// term applied to a stored flux of -1 gives (-1 * 224) >> 8 == -224 >> 8 ==
// -1: the flux STICKS at -1 forever and a "settled" lake carries a permanent
// one-unit-per-tick leftward current, while the mirror-image +1 case decays to
// 0 correctly. Rounding both signs toward zero makes decay symmetric and makes
// the resting state exactly, and reachably, zero.
constexpr int64_t shiftSym(int64_t v, int32_t s) {
    return v >= 0 ? (v >> s) : -((-v) >> s);
}

struct SweConfig {
    // Per-tick flux retention, Q8 (256 would be frictionless). Stands in for
    // the friction/turbulent-dissipation term the linearised momentum
    // equation drops. See section 4 for its role in both stability and the
    // settle deadband — these pull in opposite directions.
    int32_t dampingQ8 = 224;

    // Flux gain: the head difference (fill units) contributes
    // (d << 8) >> gainShift to the Q8 flux accumulator each tick. Larger
    // gainShift == weaker gain == more stable and slower.
    int32_t gainShift = 7;

    // Hard clamp on the flux accumulator, in fill units per tick (stored
    // internally << 8). Bounds the state so no config can produce unbounded
    // values even outside the stability region.
    int32_t maxFluxPerTick = 1 << 16;

    // Column capacity in fill units (255 == one voxel). Default is 4096
    // voxels of head, far past any plausible sheet.
    int32_t maxColumnDepth = 255 * 4096;

    // Used only by velocityAt() to convert per-tick transport to mm/s.
    int32_t ticksPerSecond = 20;

    // Section 4's CFL analogue with L = 8 (2D discrete Laplacian). Evaluated
    // in integers; the SweGrid constructor clamps rather than trusts.
    bool stableIn2D() const {
        return dampingQ8 + static_cast<int32_t>((int64_t{8} << 8) >> gainShift) < 256;
    }
};

// Section 4's d_min = 2^gainShift * (256 - dampingQ8) / 256, in fill units:
// the smallest head difference the scheme will act on, i.e. the tolerance a
// settled surface is flat to. Exposed so tests assert against the derivation.
constexpr int32_t sweSettleTolerance(const SweConfig& cfg) {
    return static_cast<int32_t>(((int64_t{1} << cfg.gainShift) * (256 - cfg.dampingQ8)) >> 8);
}

// Depth-averaged column velocity (section 6), mm/s, integer.
struct SweVelocity {
    int32_t xMmPerSec = 0;
    int32_t yMmPerSec = 0;

    friend bool operator==(const SweVelocity&, const SweVelocity&) = default;
};

// A dense rectangular sheet of columns at voxel pitch. Dense-rect rather than
// the CA's hashed bricks on purpose: this is the REFERENCE core for the
// numerics, and a sparse/tiled residency scheme is an independent concern
// that would only obscure the conservation and order-independence arguments.
// A production sheet tiles this; the tick rules do not change.
class SweGrid {
public:
    // Inclusive column origin (voxel x,y of index 0,0) and extent.
    SweGrid(int64_t originVx, int64_t originVy, int32_t sizeX, int32_t sizeY,
            const SweConfig& cfg = SweConfig{});

    int32_t sizeX() const { return sizeX_; }
    int32_t sizeY() const { return sizeY_; }
    int64_t originVx() const { return originVx_; }
    int64_t originVy() const { return originVy_; }
    const SweConfig& config() const { return cfg_; }

    bool inBounds(int64_t vx, int64_t vy) const {
        return vx >= originVx_ && vy >= originVy_ && vx < originVx_ + sizeX_ &&
               vy < originVy_ + sizeY_;
    }

    // Voxel z of the topmost solid voxel this column's sheet rests on.
    void setBed(int64_t vx, int64_t vy, int32_t bedZ);
    int32_t bedAt(int64_t vx, int64_t vy) const;

    // --- Domain membership (THE PERIMETER) ---------------------------------
    //
    // An INACTIVE column is a hard wall, treated by the tick rules exactly
    // like the grid's outer boundary: every face touching it carries zero
    // flux and transports nothing, in either direction.
    //
    // This is not a convenience, it is what makes §5's ownership partition a
    // property of the NUMERICS rather than merely of the coupler's
    // bookkeeping. Without it the sheet keeps happily flowing into a column
    // the coupler has just handed to the CA — which is a genuine double-owned
    // cell and a genuine conservation-visible bug, found by
    // swe_coupler_puncture_meters_the_inrush_then_hands_the_column_over
    // before this existed. `SweCaCoupler` drives this directly from
    // membership, so a CA-owned column cannot receive sheet water at all.
    //
    // Default is ACTIVE, so a standalone SweGrid (no coupler) behaves as a
    // plain closed basin. `SweCaCoupler` deactivates every column when it is
    // enabled, and thereafter activates exactly its promoted set.
    void setColumnActive(int64_t vx, int64_t vy, bool on);
    bool columnActive(int64_t vx, int64_t vy) const;

    // Column water total in fill units (255 == one full voxel).
    int32_t depthAt(int64_t vx, int64_t vy) const;

    // head = bed*255 + depth, fill units (section 2).
    int64_t headAt(int64_t vx, int64_t vy) const;

    // Adds/removes fill units; returns the amount actually added/removed
    // (capped by maxColumnDepth / by what is present). Ledger-tracked, in the
    // same "only what actually moved is accounted" style as
    // WaterCA::addWater's return value.
    int32_t addWater(int64_t vx, int64_t vy, int32_t amount);
    int32_t removeWater(int64_t vx, int64_t vy, int32_t amount);

    // One tick (section 3). Equivalent to stepWithColumnOrder(identity).
    void step();

    // Runs one tick visiting columns in `order` instead of index order.
    // Duplicates are ignored and any permutation of the full column index set
    // produces byte-identical depths, fluxes and digest — the direct analogue
    // of WaterCA::stepWithOrder, and the property a GPU port needs.
    void stepWithColumnOrder(const std::vector<int32_t>& order);

    // Depth-averaged velocity, mm/s (section 6). Zero for a dry column.
    SweVelocity velocityAt(int64_t vx, int64_t vy) const;

    // Signed face flux in fill units per tick (positive == toward +x / +y),
    // i.e. the transported volume the last tick committed on that face.
    // Diagnostics, force-field consumers and tests.
    int32_t faceFluxX(int64_t vx, int64_t vy) const;
    int32_t faceFluxY(int64_t vx, int64_t vy) const;

    // O(1) running conservation ledger (waterca.h's totalVolume() contract:
    // step() must never change it; only addWater/removeWater may).
    int64_t totalVolume() const { return totalVolume_; }
    // Independent re-sum for cross-checking the ledger (tests, not hot path).
    int64_t recomputeVolume() const;

    // Deterministic digest over bed, depth and both face fluxes in column
    // index order (already canonical — a dense grid has no hash order).
    void digest(Digest& d) const;

private:
    int32_t index(int64_t vx, int64_t vy) const {
        return static_cast<int32_t>((vx - originVx_) + int64_t{sizeX_} * (vy - originVy_));
    }
    void runTick(const std::vector<int32_t>& order);

    int64_t originVx_ = 0, originVy_ = 0;
    int32_t sizeX_ = 0, sizeY_ = 0;
    SweConfig cfg_;

    std::vector<int32_t> bed_;    // voxel z of top solid
    std::vector<int32_t> depth_;  // fill units
    std::vector<uint8_t> active_; // 0 == hard wall (see setColumnActive)
    std::vector<int32_t> fluxXQ8_; // face i <-> i+1 in x, Q8 fill/tick
    std::vector<int32_t> fluxYQ8_; // face i <-> i+sizeX in y, Q8 fill/tick

    // Per-tick scratch, kept as members to avoid per-tick allocation. Never
    // read across ticks (every phase writes before it reads), so they carry
    // no state and do not participate in the digest.
    std::vector<int32_t> desiredXQ8_, desiredYQ8_;
    std::vector<int32_t> magX_, magY_;
    std::vector<int32_t> delta_;

    int64_t totalVolume_ = 0;
};

struct SweCoupleConfig {
    // MASTER FLAG. ADR-0004 is PENDING, so this defaults to FALSE and
    // SweCaCoupler::step() is a total no-op until a caller opts in. Nothing
    // in voxel-core or the engine sets it today.
    bool enabled = false;

    // Section 5 hysteresis. demoteDwell >> promoteDwell on purpose.
    int32_t promoteDwellTicks = 8;
    int32_t demoteDwellTicks = 32;

    // Section 5 eligibility predicate.
    int32_t openClearanceVoxels = 4;
    int32_t minOpenNeighbours = 3;

    // Section 5 rate limits (fill units per column per tick). Both are well
    // under 255 so a single tick's transfer always fits one CA cell.
    int32_t drainPerTick = 64;
    int32_t absorbPerTick = 128;

    // How far above the bed the coupler scans for CA fill to absorb, in
    // voxels. Bounds the per-tick cost of channel (b).
    int32_t sheetScanVoxels = 8;
};

// The CA<->SWE boundary (section 5). Holds references to both solvers and
// owns nothing but the membership/dwell bookkeeping and the transfer ledgers.
class SweCaCoupler {
public:
    using SolidFn = std::function<MaterialId(int64_t vx, int64_t vy, int64_t vz)>;

    SweCaCoupler(SweGrid& grid, WaterCA& ca, SolidFn solid,
                 const SweCoupleConfig& cfg = SweCoupleConfig{});

    const SweCoupleConfig& config() const { return cfg_; }
    // Enabling also seats the grid's perimeter: every non-promoted column
    // becomes a hard wall, so the sheet can never flow into CA-owned ground.
    void setEnabled(bool on) {
        cfg_.enabled = on;
        if (on) deactivateAll();
    }

    // One coupling tick: membership hysteresis, then per-column drain OR
    // absorb (never both — section 5, mechanism 2). A total no-op when
    // cfg_.enabled is false: it reads nothing and writes nothing, so a caller
    // that has not opted in pays only the branch.
    void step();

    // True once the column has been SWE-owned (i.e. promotion's dwell window
    // has elapsed). Every other column is CA-owned in full.
    bool isSweColumn(int64_t vx, int64_t vy) const;

    // Forces membership without waiting out the dwell window, for tests and
    // for a caller seeding a known-open body (e.g. a generated lake) at world
    // load. Runs the same promote/demote transfer channels, so it is
    // ledger-exact.
    void forcePromote(int64_t vx, int64_t vy);
    void forceDemote(int64_t vx, int64_t vy);

    // Ledgers. Every unit that has crossed the boundary is counted in exactly
    // one of these; the coupled-system invariant that must hold after ANY
    // sequence of ticks is
    //     ca.totalVolume() + grid.totalVolume() == (total injected)
    // which these two make checkable directionally as well as in total.
    int64_t transferredToCA() const { return toCA_; }
    int64_t transferredToSWE() const { return toSWE_; }

    // Columns currently SWE-owned, and columns punctured on the most recent
    // step() (diagnostics/tests).
    int32_t sweColumnCount() const { return sweColumns_; }
    int32_t lastPuncturedCount() const { return lastPunctured_; }

private:
    bool solidAt(int64_t vx, int64_t vy, int64_t vz) const {
        return solid_(vx, vy, vz) != MAT_AIR;
    }
    void deactivateAll();
    bool eligible(int64_t vx, int64_t vy) const;
    void promote(int32_t i, int64_t vx, int64_t vy);
    void demote(int32_t i, int64_t vx, int64_t vy);
    int32_t absorbColumn(int64_t vx, int64_t vy, int32_t budget);
    void drainColumn(int32_t i, int64_t vx, int64_t vy);

    SweGrid& grid_;
    WaterCA& ca_;
    SolidFn solid_;
    SweCoupleConfig cfg_;

    std::vector<uint8_t> member_;   // 0 = CA-owned, 1 = SWE-owned
    std::vector<int32_t> eligDwell_; // consecutive eligible ticks
    std::vector<int32_t> inelDwell_; // consecutive ineligible ticks

    int64_t toCA_ = 0, toSWE_ = 0;
    int32_t sweColumns_ = 0, lastPunctured_ = 0;
};

} // namespace vxc
