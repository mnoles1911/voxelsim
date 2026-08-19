# Local interactive water ripples

**STATUS 2026-08-19: WIRED, LIVE AND TUNED.** §8's integration edits were
applied on 2026-08-13 (`f2ea97c`) and the system has been on screen since. It was
then reported dead by the owner and was not: `c031b03` established with a
read-back probe that every stage worked and that **three defaults** were hiding
it — half-life 1.8 → 5.0 s, gain 1.0 → 2.5, player entry 0.09/0.5 → 0.22/0.9 m,
object impact 0.07 → 0.18 m. A 9 cm ripple was competing with a moving 6 cm
wind-wave field on the same normal. **Read that commit before tuning anything
here**, including its list of five measurements taken where the answer could not
appear. Original status: built, four bugs fixed, not yet wired in.

Four confirmed bugs were fixed on 2026-08-12, one of which had every ripple
lasting twice as long as configured. They are recorded with their measurements in
**§12**, which is the section to read before touching the step shader or the
splat window test.

**What it does, in one sentence.** Things that enter the water near the camera
make rings that spread out, run into the shore and fade, and the water surface
tilts and lifts where they pass.

**What it is not.** It does not move water. It has no opinion about basins,
volumes, particles, or where the water level is. `docs/water-architecture.md` §1
names the things that actually simulate water in this game — baked basins, a
scalar hydrology ledger, a GPU PBF solver — and this is none of them. It is a
layer added to the water's *appearance*, in the same place and the same way as
the wind wave that is already there.

**Coined term used throughout: the *ripple field*.** A 51.2 m square of water
surface that follows the camera, on which a wave equation runs on the GPU. Every
consumer reads it as one texture.

| file | what it is |
|---|---|
| `ue-project/Source/VoxelEarth/VoxelRippleField.h/.cpp` | `UVoxelRippleFieldSubsystem` — owns the render targets, steps the simulation, publishes the window, injects disturbances |
| `ue-project/Tools/create_ripple_field_materials.py` | authors the three render targets and the two step materials |
| `ue-project/Tools/ripple_field_graph.py` | the shared contract (asset paths, resolution, bias) plus `sample_ripple_field()`, the one function every consumer calls |

---

## 1. The simulation, and why this one

The classic explicit two-level wave equation on a height field:

```
h(t+dt) = 2·h(t) − h(t−dt) + (c·dt/dx)² · ∇²h(t)
```

with the 5-point Laplacian, then damped and masked. It is three lines of
arithmetic, it needs no history beyond two frames, and it is what most shipped
games use for interactive water.

**Why not something better.** The alternatives were considered and are all wrong
for this job. FFT/Tessendorf synthesises an *ambient* spectrum and cannot be
disturbed at a point — it is the wrong tool for "a thing fell in here". A height
field advected by a fluid solver is the PBF work in
`docs/water-architecture.md` §3 and is orders of magnitude more expensive for a
cosmetic effect. A pre-baked ring texture scrolled outward from each impact
cannot interact with the shore, cannot superpose correctly when two things land
near each other, and needs a per-impact draw; the wave equation gets all three
for free because superposition and reflection *are* the equation.

**The derivative comes from a second pass, not from the water material.** The
simulation produces heights; a normal needs slope. Differencing in the water
material costs four extra taps on every water pixel, and since 2026-08-11 lake
sheets draw lakes at every range, water can be most of the screen. Differencing
once, in a 512×512 pass, costs 262,144 pixels whatever water covers, and leaves
the water material at **one** tap. It also fixes a second problem for free: the
ping-pong means the newest heights alternate between two assets every step, and a
material asset cannot follow that without a dynamic instance — which the
far-field lake sheet deliberately does not have (`VoxelWaterSheetActor.h:47-52`).
The derive pass always writes the same target, so the water material samples one
fixed asset. Same argument as `VoxelBathyField.h`'s "WHY AN ASSET".

---

## 2. Numbers

| | value | why |
|---|---|---|
| resolution | 512 × 512 | with the texel size below, this is the extent; and 512 is the size the bathymetry field already settled on for the same class of thing |
| texel | **10 cm = one voxel** | the finest grid the rest of the water uses (`WaveQuantPerVoxel = 1` quantises the wind wave to the same lattice, `create_water_voxel_material.py:2455-2472`); four samples per wavelength puts the shortest honest ripple at 40 cm |
| world extent | **51.2 m square, ±25.6 m from the camera** | falls out of the two above — and ±25.6 m is exactly the half-extent of the retired near-field voxel water disc (`VoxelWaterSheetActor.h:203`, `VoxelWaterSubsystem.h:515`), i.e. the radius at which this project already decided detailed water stops being worth drawing |
| state format | 2 × RG32f (`RTF_RG32f`), R = h(t), G = h(t−dt), both biased +0.5 | see §3 |
| field format | RGBA16f, R/G = ∂h/∂x, ∂h/∂y, B = h in metres, **A unwritten** | signed values near zero, where fp16 resolves ~1e-5 — no bias and no precision problem. There is no RGB-only float target, so the fourth channel is paid for and not written: a canvas draw only guarantees the three emissive channels, so nothing may read `.a` |
| memory | **6 MB** | 2 + 2 + 2 |
| timestep | **fixed 1/60 s**, accumulated, max 4 substeps/frame | §4 |
| wave speed | **1.6 m/s** (`voxel.Water.Ripple.SpeedMPS`) | roughly right for the 30–60 cm gravity–capillary waves a splash makes: a ring crosses 5 m in about 3 s. Gives Courant 0.2667 against the 2D limit 0.7071 — a 2.65× margin |
| damping | **half-life 1.8 s** (`voxel.Water.Ripple.HalfLifeSec`) | ≈0.9936 per step, applied to **both** time levels; a splash is at 10% after 6 s. Stored as a half-life so changing the substep rate cannot silently change how long ripples last. It came out at 3.6 s until 2026-08-12 — §12.1 |
| shore mask | full strength 25 cm inside the water | 2.5 texels — a soft absorber rather than a hard wall, because a hard wall on a 10 cm grid rings |
| disturbance slots | 8 per step, queue of 64 | drains at 480/s; a material has no arrays so the count is baked into the shader |

---

## 3. Two format decisions with a failure mode behind each

**The state is stored biased by +0.5, in fp32.** A height field is *signed* — a
trough is a negative number — and it is written through a material's emissive
output. This box cannot currently verify that a negative emissive value survives
the canvas draw path into a float render target (the editor is unavailable, and
the reports that exist differ by RHI and engine version). If it does not, every
trough clamps to zero and the field becomes a bed of positive bumps: wrong, and
wrong in a way that looks like a tuning problem rather than a format problem. So
the state is offset into positive territory and every reader subtracts the
offset.

That choice then forces the second one. At fp16 the spacing between
representable numbers near 0.5 is 2⁻¹¹ = 0.000488 — and the wave equation's
entire update term, `Courant² · ∇²h`, is about 0.0711 × 0.01 = **7 × 10⁻⁴ m**.
The same order as the quantisation step; the simulation would advance in ragged
jumps. fp32 has 6 × 10⁻⁸ spacing there and the question disappears, for 2 MB per
target.

If someone with an editor confirms negatives survive, this is one constant
(`ripple_field_graph.STATE_BIAS`, mirrored at `VoxelRippleField.h`'s
`kStateBias`) and the targets can drop to RG16f for half the memory.

**Target gamma is pinned to 1.0 and read back.** It is already 1.0 for float
formats, so today the line changes nothing. It is there because a gamma curve
applied to a biased height field inside a feedback loop compounds every step, and
the day somebody switches a target to RGBA8 to save memory is the day that stops
being true.

---

## 4. The long-frame problem

The explicit scheme is stable while `C = c·dt/dx < 1/√2` and diverges within a
few frames above it. Frame time here is not fixed, and the GPU spike tail is this
project's known p95 problem (`docs/water-architecture.md` §3), so **dt cannot be
the frame time** — one 40 ms frame would multiply C by 2.4 and detonate the
field, permanently, because the next step reads the wrecked state back in.

So: **a fixed 1/60 s step with an accumulator.** Leftover time carries to the
next frame. A frame that owes more than 4 steps **drops** the excess rather than
catching up — catching up after a hitch is how a fixed-step loop turns one long
frame into several, which is both a GPU spike immediately after a GPU spike and a
visible speed-up of every ripple on screen. Ripples run momentarily slow after a
stall, which nobody can see.

Two more guards, both because a poisoned render target is permanent:

- `voxel.Water.Ripple.SpeedMPS` is **clamped** so C never exceeds 0.6 (3.6 m/s at
  this grid and timestep), and says so in the log the first time it bites.
- The step shader clamps `|h|` to 2 m with `min(max(...))` rather than `clamp()`,
  because D3D's `min`/`max` return the non-NaN operand and `clamp` has no defined
  NaN behaviour. Inf and NaN both map to something finite and the field heals on
  the next step.

---

## 5. The moving window

The origin is **snapped to whole texels** and recomputed every frame that steps.
The step material reads its source at `Uv + ShiftUv`, where `ShiftUv` is the
origin's motion in UV — always an exact multiple of 1/512, so a
nearest-neighbour tap reproduces the previous texel bit for bit. The field
**scrolls**; it does not blur. A sub-texel shift would run the whole field through
a bilinear filter 60 times a second and the ripples would visibly dissolve
whenever the player walked, which would read as a damping bug.

There is no recentre threshold and no hysteresis — the opposite of
`VoxelBathyField`'s `kRecentreFraction`, and correctly so, because the costs are
opposite. Moving the bathymetry window costs eight block decodes and a 2 MB
upload, so it is done every 120 m. Moving this one costs an add on a UV that was
already being computed, so it is done always, and the field stays centred on the
camera to within one texel.

**Three things happen at the edge, and they are different mechanisms.**

1. **The sponge (inside the simulation).** A wave equation with a hard boundary
   *reflects*, and a square reflecting wall 25.6 m out would ring every splash
   back at the player as a box-shaped echo. The outer band — from 19.5 m to
   25.6 m — is multiplied down toward 0.80 per step. A wave takes 3.8 s to cross
   it, which at 60 steps/s is an attenuation of 3.3e-11. Nothing reflects. (That
   figure was 5.8e-6 until 2026-08-12: the sponge shared the damping bug and its
   realised floor was √0.80 = 0.8944. Five orders of margin either way, which is
   why only the recurrence could find it — §12.1.)
2. **The scroll guard (inside the simulation).** Texels scrolling in from outside
   the previous window have no history; clamp addressing would streak the border
   texel across the new band. The shader writes flat water there instead. This
   also means a **teleport needs no special case**: if the window jumps further
   than its own width, every texel is out of range and the field self-clears in
   one step.
3. **The fade (inside the water material).** `sample_ripple_field` fades the
   contribution out between 21.5 m and 24.8 m from the camera. Not redundant with
   the sponge: the sponge only acts on texels that have been *stepped*, and a
   camera at 100 m/s drags 17 fresh texels into the window per step.

---

## 6. Where ripples are allowed

The bake already knows. `bake_ver` 27's `bathy_shore` plane is a **signed
distance** to the nearest shoreline in metres, positive in water, and the step
multiplies the field by `saturate(shore_m / 0.25)` every step: zero on land, zero
at the waterline, one 25 cm inside the water. That is both "no ripples on dry
ground" and "a shore for a ripple to die against", out of one number.

The signed distance is *why this works at 10 cm when the source raster is
1.875 m* — `bathy_field_graph.py:43-48` makes the point that the bake ships a
distance rather than a binary mask precisely so a consumer can find the zero
crossing to a decimetre. A binary wet mask here would give every lake a 1.875 m
staircase of reflecting wall.

**Where the bake has no answer there is no mask** — `lerp(1, wet, validity)`, the
same idiom and the same reason as the wave field's shore damping
(`create_water_voxel_material.py:2614-2616`). It costs nothing to allow: the
ripple field is only ever *read* by the water material, so a ripple over dry land
is invisible whatever the simulation believes.

`voxel.Water.Ripple.MaskEnable 0` removes the mask. That is the diagnostic for
"my test drop did nothing", because a drop on a spot the bake calls dry is
silently deleted and otherwise looks identical to the field not running.

---

## 7. Injecting a disturbance

```cpp
Ripple->AddDisturbance(WorldPos, RadiusM, StrengthM);
UVoxelRippleFieldSubsystem::AddDisturbanceAt(World, WorldPos, RadiusM, StrengthM);  // static
```

Safe from anywhere on the game thread at any time — before the first frame, with
the feature disabled, with no assets, with no water nearby. The disturbance is
queued, or **dropped and counted** — never anything else, and never silently.
A caller must never have to ask whether ripples are on.

There are four ways it can be dropped and they are four different diagnoses, all
printed by `voxel.Water.Ripple.Stat`:

| counter | meaning | what to do |
|---|---|---|
| `unarmed` | the subsystem never armed: assets missing, or the size/format guard refused | read the startup log; run `create_ripple_field_materials.py` |
| `inert` | the **caller** passed strength 0, or a non-finite strength/radius/position | the fault is upstream — a strength CVar at 0, an impact fraction that came out 0 |
| `outside` | not one texel of the ring falls in the 51.2 m window | get within 25.6 m; the field is small |
| `full` | the 64-deep queue overflowed | a bug; something is injecting faster than 480/s |

`injected` plus those four equals every call `AddDisturbance` has ever received.
If that identity ever fails, a path out of that function has stopped counting.
`unarmed` and `inert` were **not counted at all** until 2026-08-12 (§12.3), which
made "the hooks fire and the field is off" and "nothing ever calls in" the same
empty output.

**Through the step material, not a canvas splat.** The obvious alternative is
`BeginDrawCanvasToRenderTarget` + a small additive quad. It was rejected: it
needs a third material, a second render-target transition, and an *additive
blend that this box cannot verify behaves as expected on a float target* — and
the step pass is already touching every texel, so eight distance evaluations
(2.1 M ops across the field) is noise next to the five texture taps that pixel
already does. Eight slots drain at 480 disturbances per second at 60 steps/s.

**Shape: a raised cosine**, whose value *and slope* both reach zero at the edge. A
shape with a kink injects energy at every spatial frequency the grid carries,
including the two-texel checkerboard — which does not propagate. It sits at the
impact point and sparkles.

**Added to h(t) only, leaving h(t−dt) alone.** The scheme's velocity is
`h(t) − h(t−dt)`, so that makes the disturbance a displacement *and* an upward
velocity in one gesture, and it collapses into an outgoing ring within a few
steps instead of sitting there and sagging.

**Radius is widened to 30 cm if smaller** (said once). Below about three texels a
raised-cosine bump is mostly the grid, and what radiates from it is noise.

### The two cases the owner named

Both are hooked **two ways**. The zero-edit way runs today
(`voxel.Water.Ripple.AutoWatch`, default on): the subsystem polls the player pawn
and every `AVoxelExplosive`/`AVoxelDebris` for `IsUnderwaterAtWorld`, and injects
on the frame the flag flips, taking the impact speed from the position delta and
the radius from the actor's own bounds. The exact way is a one-line patch inside
each class (§8.4, §8.5) and is better for one specific reason: for the player,
the movement component zeroes `VerticalVelocity` the instant it decides you are
swimming (`VoxelCharacterMovement.cpp:662`), so by the time a poller sees the
flag the real impact speed is already gone from the mover's own state.

Anything else — agents, dropped items, future entities — needs its own line.
`AddDisturbanceAt` exists so that line is one call that cannot fail.

---

## 8. The integration edits, written out and NOT applied

Apply 8.1 → 8.2 → 8.3, then regenerate **sky → dome → ripple → water, in that
order** (`tools/voxel-water-star-regen.ps1`; see 8.6). 8.4–8.6 are independent
and optional.

Until 8.1 and 8.2 are applied the subsystem logs one warning at startup naming
this file, keeps simulating, and publishes nothing — the render targets are still
live and inspectable in the texture visualiser.

### 8.1 `ue-project/Tools/create_sky_material.py` — three MPC parameters

An MPC cannot hold a texture, but it is this project's only CPU→material channel
that does not need a dynamic material instance, and the lake sheet has none by
design. So the *placement* of the window goes here, exactly as the bathymetry
window's does.

In `SCALAR_PARAMS` (after `("BathyFieldValid", 0.0)`, line 510):

```python
    # --- THE INTERACTIVE RIPPLE FIELD ---------------------------------------
    #
    # Read by M_WaterVoxel, written by UVoxelRippleFieldSubsystem
    # (VoxelRippleField.cpp) EVERY FRAME -- unlike the bathymetry window, which
    # moves every 120 m, this one follows the camera to within one 10 cm texel.
    # The field itself is a render target, /Game/Voxel/RT_VoxelRippleField,
    # covering a 51.2 m square; the material turns a world XY into a UV with the
    # same one multiply-add the bathymetry field uses.
    #
    #   RippleFieldInvSize -- 1 / (window width in UE units) = 1 / 5120.
    #   RippleFieldGain    -- global strength, and the feature's off switch.
    #
    # DEFAULT 0.0 ON BOTH, for the reason BathyFieldValid defaults to zero: an
    # undriven collection must fail to the OLD water, visibly and safely, rather
    # than to a plausible-looking wrong one. Gain 0 means the water material adds
    # exactly nothing, so a run with no ripple subsystem renders the water this
    # project rendered before the feature existed.
    ("RippleFieldInvSize", 0.0),
    ("RippleFieldGain", 0.0),
```

In `VECTOR_PARAMS` (after `("BathyFieldOrigin", ...)`, line 523):

```python
    # World UU of the ripple window's MINIMUM corner, in xy; z and w unused.
    # See the two scalars above. With RippleFieldGain at 0 nothing reads it.
    ("RippleFieldOrigin", 0.0, 0.0, 0.0, 0.0),
```

### 8.2 `ue-project/Tools/create_water_voxel_material.py` — four lines

**(a)** with the other imports, next to line 441:

```python
from ripple_field_graph import sample_ripple_field  # noqa: E402
```

**(b)** immediately after the `WaveField` input wiring loop ends (after line
2627, `raise RuntimeError("connect -> WaveField.%s failed" % pin)`):

```python
    # --- THE INTERACTIVE RIPPLE FIELD, ADDED INTO THE SAME TWO OUTPUTS ------
    #
    # One evaluation, two consumers, exactly like the wave field above: the
    # gradient goes into the normal and the height into the displacement, and
    # they are two halves of one field so a crest that moves in the geometry is a
    # crest that moves in the shading.
    #
    # ADDED, NOT BLENDED. Summing GRADIENTS is the only correct way to combine
    # height fields -- the note at wave_grad below says why (averaging unit
    # normals systematically flattens slopes). The ripple is a second height
    # field on the same surface; it superposes.
    #
    # IT IS DELIBERATELY NOT DAMPED BY BathyWaveDampDepthM. That term exists to
    # stop a wind-wave crest clipping through its own bank, and it is applied to
    # WaveAmplitudeM, which is upstream of here -- so the ripple bypasses it for
    # free, and must. Depth-damping a splash would delete it precisely at the
    # shoreline, which is where a player enters the water. The ripple has its own
    # shore treatment and it is a better one: the SIMULATION is masked by the
    # baked signed distance, so a ripple dies against the shore rather than being
    # faded out by depth.
    ripple = sample_ripple_field(bathy_b)
```

**(c)** change one connect at line 2656. `wave_grad` (line 2645) is the wave's
gradient; sum the ripple's into it *before* the `-1` and *before* the
`VertexColor.B` top-boundary mask, so the ripple inherits both:

```python
    wave_grad_total = bathy_b.add(wave_grad, ripple["grad_xy"])
    normal_xy_raw = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -380, 880)
    if not mel.connect_material_expressions(wave_grad_total, "", normal_xy_raw, "A"):
        raise RuntimeError("connect wave_grad_total -> normal_xy_raw.A failed")
```

**(d)** change one connect at line 2715, the same way, between `wave_height_m`
(line 2701) and `wave_height_uu`:

```python
    wave_height_total = bathy_b.add(wave_height_m, ripple["height_m"])
    wave_height_uu = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -380, 1040)
    if not mel.connect_material_expressions(wave_height_total, "", wave_height_uu, "A"):
        raise RuntimeError("connect wave_height_total -> wave_height_uu.A failed")
```

**A consequence worth stating rather than discovering.** The ripple's height goes
through `WaveWpoFraction` (0.25), so a 9 cm ring moves geometry ±2.25 cm — a
quarter of a voxel — and the normal carries the rest. That is on purpose (the
fraction exists because the surface sits on a 10 cm grid), but if the owner wants
the ring to visibly *lift* the surface, the change is a separate
`RippleWpoFraction` parameter on the ripple half of the sum, not a bigger
`WaveWpoFraction` — which would also make the wind wave slosh over its banks.

### 8.3 `ue-project/Tools/bathy_field_graph.py` — optional, removes a duplicate

`create_ripple_field_materials.py` re-expresses five lines of that module's UV
arithmetic, because `sample_bathy_field` builds its own
`MaterialExpressionWorldPosition` internally and the step material is a canvas
pass with no geometry, where world position is meaningless. That module's own
docstring is explicit that duplication here is the thing to avoid. The clean fix:

```python
def sample_bathy_field(b, world_xy=None):
    ...
    if world_xy is None:
        world_pos = b.node(unreal.MaterialExpressionWorldPosition)
        world_xy = b.mask(world_pos, "", r=True, g=True)
```

then `sample_bathy_field(b, world_xy=my_xy)` from the ripple generator. Not
applied because that file is read by three shipped materials and this one is not
built yet; the duplicate is bounded (the texture path, parameter name and channel
layout are *imported*, not retyped).

### 8.4 `VoxelCharacterMovement.cpp` — the player entering water

Between lines 630 and 632, where `bSwimmingLastTick` still holds last frame's
value and `VerticalVelocity` still holds the descent speed (it is destroyed at
line 662):

```cpp
	// A RIPPLE ON THE WAY IN. This is the only false->true edge of the swim flag
	// in the project, and it is the only place the impact speed still exists --
	// VerticalVelocity is zeroed thirty lines down (:662) the moment we decide we
	// are swimming, so a poller watching from outside sees the entry with the
	// speed already gone. Cosmetic only: AddDisturbanceAt queues and returns, it
	// cannot fail, and it does nothing at all when ripples are off.
	if (bSwimming && !bSwimmingLastTick)
	{
		const double DownMPS = FMath::Max(0.0, -VerticalVelocity) / 100.0;
		const float Fraction = static_cast<float>(FMath::Clamp(DownMPS / 6.0, 0.25, 1.0));
		UVoxelRippleFieldSubsystem::AddDisturbanceAt(
			GetWorld(), FVector(Pos.X, Pos.Y, Pos.Z), 0.5f, 0.09f * Fraction);
	}
```

plus `#include "VoxelRippleField.h"`. Set `voxel.Water.Ripple.AutoWatch 0` once
this is in, or the entry fires twice.

### 8.5 `VoxelExplosive.cpp` / `VoxelDebris.cpp` — a thrown volume landing

`VoxelExplosive.cpp:146`, after the zero-travel early-out and before the terrain
sweep, where both segment endpoints are already in scope:

```cpp
	// The two-point crossing test. Both endpoints exist here and nowhere else in
	// this Tick, so this is the one place a fast-moving charge cannot tunnel
	// through the surface between frames without being noticed.
	if (const UVoxelWaterSubsystem* Water = GetWorld()->GetSubsystem<UVoxelWaterSubsystem>())
	{
		if (Water->IsUnderwaterAtWorld(Now) && !Water->IsUnderwaterAtWorld(LastTickLocationUU))
		{
			const double DownMPS = FMath::Max(0.0, LastTickLocationUU.Z - Now.Z)
			                     / FMath::Max(DeltaSeconds, KINDA_SMALL_NUMBER) / 100.0;
			UVoxelRippleFieldSubsystem::AddDisturbanceAt(
				GetWorld(), Now, 0.35f,
				static_cast<float>(0.07 * FMath::Clamp(DownMPS / 6.0, 0.25, 1.0)));
		}
	}
```

`VoxelDebris.cpp:152` wants the same shape but needs one new `bool bWasUnderwater`
member, since that class keeps no previous position. The auto-watcher already
covers both; these exist because they are exact and cost nothing per frame.

### 8.6 `tools/voxel-water-star-regen.ps1` — regeneration order

Add to `$scripts` (line 95) between the dome and the water material. It must run
**after** `create_sky_material.py` (it binds to `MPC_VoxelSky`, which that script
deletes and recreates) and **before** `create_water_voxel_material.py` (which
loads `RT_VoxelRippleField` by name and raises if it is absent):

```powershell
$scripts = if ($WaterOnly) { @('create_water_voxel_material.py') } else { @(
    'create_sky_material.py',
    'create_sky_atmosphere_dome_material.py',
    'create_ripple_field_materials.py',
    'create_water_voxel_material.py'
)}
```

### 8.7 `VoxelEarthHUD.cpp` — optional counters

The house pattern is a POD snapshot struct in `VoxelDebug.h` read by
`AVoxelEarthHUD::DrawHUD` (example at `VoxelEarthHUD.cpp:239-247`). Not done:
this subsystem's counters are available through `voxel.Water.Ripple.Stat`, and
adding a struct to `VoxelDebug.h` is a header every file in the module includes.

---

## 9. Cost

**Estimates, not measurements.** Nothing here has been run — the editor was
unavailable for the whole of this work. `ProfileGPU` at a pinned pose over a lake
is the instrument that would settle it, with `voxel.Water.Ripple.Enable 0/1` as
the two arms.

- **GPU, per frame:** one to two step passes (262,144 pixels; five point taps of
  RG32f, one bilinear tap of the bathymetry texture, eight distance evaluations,
  ~40 ALU) plus one derive pass (four taps). Bandwidth-bound at roughly 12.5 MB
  per step pass; against ~600 GB/s that is ~21 µs, so **~0.05–0.08 ms/frame**.
- **The unbounded part**, and the one to actually watch: the water material takes
  **one extra RGBA16f bilinear tap plus ~6 ALU on every water pixel**. That scales
  with how much water is on screen, not with the field, and water can be most of
  the screen.
- **Game thread:** one tick, a handful of material-parameter writes, and one
  `IsUnderwaterAtWorld` per watched actor if `AutoWatch` is on.
- **Memory:** 6 MB of render targets.

A standing warning applies to all four numbers: this project's frame at 2K is
render-thread bound with the game thread idle roughly three quarters of the
frame, and work added to or removed from a subsystem has repeatedly failed to
show up in frame time in either direction. Measure it (`ProfileGPU`,
`stat unit`); do not reason about it.

---

## 10. How to see it

### 10.0 What has to be generated, and in what order

Nothing in this section works from a stale content tree. The chain is
`create_sky_material.py` → `create_sky_atmosphere_dome_material.py` →
**`create_ripple_field_materials.py`** → `create_water_voxel_material.py`, and
each arrow is load-bearing:

- **after the sky**, because `create_sky_material.py` *deletes and recreates*
  `MPC_VoxelSky`, and both the step material (for the bathymetry window) and the
  water material (for the ripple window) bind to it. Anything that binds to that
  collection and runs before it is silently unbound — see the standing warning at
  §10.3 step 5.
- **before the water**, because `sample_ripple_field` loads
  `RT_VoxelRippleField` by name and raises if it is absent.

`create_ripple_field_materials.py` additionally needs
`create_bathy_info_texture.py` to have run at some point: the step material
samples the baked shoreline distance and raises if that texture is missing.

`tools/voxel-water-star-regen.ps1` does **not** yet include the ripple script
(§8.6 is one line and is not applied).

### 10.1 Playing with it in the editor

**One blocker first.** §8.1 — the three `RippleField*` entries in
`create_sky_material.py`'s parameter tables — is *not applied*, and it is not
optional. `sample_ripple_field` calls `SkyGraphBuilder.collection_param`, which
**raises** on a name that is not on the collection, so the moment
`create_water_voxel_material.py` starts calling it, that script fails outright
rather than producing water with no ripples. Apply §8.1 and §8.2 together, or
neither.

With both applied and the chain above regenerated:

```
voxel.Water.Ripple.Stat            # armed=1 is the only line that matters here
voxel.Water.Ripple.DropHere 0.6 0.12
```

Stand in or beside a lake, look down, and run `DropHere`. That injects a 60 cm
ring of 12 cm at the pawn's own position and lets it run at the frame rate. To
make the effect unmissable while judging whether it is *working* (as opposed to
whether it looks right):

```
voxel.Water.Ripple.Gain 4          # 4x what is drawn; does NOT touch the sim
voxel.Water.Ripple.DropHere 0.8 0.25
```

`Gain` scales only what the water material reads, so it is safe to turn up to
absurdity and back down without disturbing a settling ring. `StrengthM` on the
drop scales what is *simulated* — a 25 cm ring is roughly 3× the ambient wind
wave and will read as a genuine splash. Turn `Gain` back to 1 before judging the
look; at 4 the normal is wrong, not just strong.

**Jumping in** works today without any patch to `create_water_voxel_material.py`
being applied *to the simulation* — `voxel.Water.Ripple.AutoWatch` is on by
default and polls the pawn's `IsUnderwaterAtWorld`, injecting on the frame it
flips (§7). But the *rendering* half is not optional: with §8.1/§8.2 unapplied
the ring is simulated and nothing reads it, so the water looks exactly as it does
today. The render targets are still live and inspectable in the texture
visualiser, which is how to confirm the simulation half independently of the
rendering half.

Two things about the natural path that are working-as-intended and look like
bugs:

- **`AutoWatch` fires once per crossing**, on the false→true edge of submersion.
  Bobbing at the surface does not re-fire it; you have to get out and jump in
  again.
- **`AutoWatch` is suppressed entirely while `voxel.Water.Ripple.Freeze 1`.** It
  keeps tracking, so thawing does not fire a backlog, but nothing enters a frozen
  field. If jumping in "stopped working", check `Freeze`.

### 10.2 What will make it invisible even when it is working

In rough order of how often each will be the answer:

1. **§8.1/§8.2 not applied.** The startup log says so in one line naming this
   file. Nothing can read the field; there is no visual symptom to diagnose.
2. **`RippleFieldGain` at 0.** Its default on the collection is 0 *by design* —
   an undriven collection must fail to the old water. It goes above zero only
   once the subsystem has simulated a frame, so no pawn means no gain.
3. **You are more than 25.6 m from the ring.** The window is 51.2 m across and
   follows the camera; the water material's own fade reaches zero at 24.8 m.
   Walking away from a ripple deletes it, correctly and permanently.
4. **The bake calls that spot dry.** The simulation is masked by the baked
   shoreline distance, so a drop on land is deleted at the first step.
   `voxel.Water.Ripple.MaskEnable 0` is the diagnostic.
5. **Scale.** A 9 cm ring is added to an 8.6 cm ambient wind wave, and the height
   half goes through `WaveWpoFraction` (0.25), so it moves geometry by ±2.25 cm —
   a quarter of a voxel. Most of the read is in the *normal*, which means it is
   most visible at grazing angles with something bright to reflect, and least
   visible looking straight down at flat-lit water.
6. **The water is drawing with UE's default material and the log says success.**
   `create_sky_material.py` recreates `MPC_VoxelSky` and unbinds every dependent;
   this project has lost a night to that once already. Grep the generation log
   for `Failed to compile Material` **before** diagnosing anything visual, and
   if the water looks like grey plastic rather than water, the ripple is not
   your problem.

### 10.3 How to photograph it

The owner judges screenshots (`docs/water-architecture.md` §4). A ripple that
only exists when a player jumps into a lake cannot be captured headlessly at all
— there is no player, and no frame at which the effect is reproducibly the same.
`voxel.Water.Ripple.Drop` with its `Steps` argument is what makes it
photographable:

```
voxel.Water.Ripple.MaskEnable 1
voxel.Water.Ripple.Drop <XUU> <YUU> 0.6 0.09 90     # inject, then 1.5 s of settling
voxel.Water.Ripple.Freeze 1                          # hold it there
<capture at the pinned pose>
voxel.Water.Ripple.Gain 0                            # the A arm, same field, not redrawn
<capture again>
```

90 steps at 1.6 m/s puts the ring at about 3 m radius. `Gain 0` is the right A/B
switch rather than `Enable 0`, because it changes what is *drawn* without
disturbing what is *simulated* — the two frames differ in one thing.

**If nothing appears**, in order:

1. `voxel.Water.Ripple.Stat`. `steps=0` means the simulation never ran; check the
   startup log for the asset or size guard.
2. `droppedOutside > 0` means the drop position is not within 25.6 m of the
   camera. The field is small.
3. `voxel.Water.Ripple.MaskEnable 0` and repeat. If it appears now, the bake calls
   that spot dry.
4. Check the startup log for the `MPC_VoxelSky has no RippleFieldOrigin` warning —
   §8.1 has not been applied and nothing can read the field.
5. Grep the log for `Failed to compile Material` before diagnosing anything
   visual. This is a standing project rule and it has cost a night before:
   `create_sky_material.py` recreates `MPC_VoxelSky` and unbinds every dependent,
   after which UE draws all water with the **default material** while the log
   reports success.

---

## 11. What could not be verified

No compiler and no editor were available. Written carefully, but unproven:

- **The C++ does not compile-check.** Highest-risk lines: `Super::IsTickable()`
  on `UTickableWorldSubsystem`; `TEnumAsByte` comparison on
  `UTextureRenderTarget2D::RenderTargetFormat`; `EAllowShrinking::No` on
  `TArray::RemoveAt` (UE ≥5.5 spelling).
- **Whether `DrawMaterialToRenderTarget` accepts an `MD_Surface` material without
  a usage flag.** It is the standard blueprint render-target workflow, so it
  should; if UE complains about material usage, the fix is one line in
  `finish_material` setting the UI/canvas usage.
- **Whether a negative emissive value survives to a float render target.** The
  +0.5 bias exists precisely so the answer does not matter. See §3.
- **The two HLSL blocks do not compile-check.** The known hazard is guarded: the
  engine's `Common.ush` `#define PI`, which is why nothing in either block is
  named `PI`, `C`, or anything else short. `float4 SP[8] = { Splat0, ... }` inside
  a Custom node, and `Texture2D`-free arithmetic-only inputs, are the two things
  to look at first if it does not.
- **Every number in §9.** Estimates.
- **The look.** Nobody has seen this. No verdict is offered on whether 1.6 m/s and
  a 1.8 s half-life look like water; those are the two knobs, they are console
  variables, and the owner judges the screenshot.
- **The 2026-08-12 fixes recompile nothing.** §12's HLSL change has not been
  through a shader compiler either; the one new identifier is `AttenPerStep`,
  chosen long for the `Common.ush` macro reason above.

---

## 12. Bugs found and fixed, 2026-08-12

Four, found by an independent review that simulated the recurrence rather than
looking at water. That method is the point of this section: **not one of these
four had a visual symptom anyone would have recognised.** Ripples that last 3.6 s
instead of 1.8 s still look like ripples.

### 12.1 The damping half-life was exactly 2× what was configured (HIGH)

`create_ripple_field_materials.py`, `STEP_CODE`. `Damp` multiplied only the new
level `hn`; the `h(t)` handed on as the next step's `h(t−dt)` carried the shore
mask alone. The recurrence was then

```
A(n+1) = D · (2·A(n) − A(n−1) + C²·∇²A(n))
```

whose characteristic polynomial `z² − D(2 − C²λ)z + D` has root **product** `D`,
so every oscillatory mode decayed as **√D per step**, not `D`.

**Measured** on a 1-D ring at the shipped numbers (`HalfLifeSec` 1.8, `dt` 1/60,
`C` 0.2667), envelope sampled over one oscillation period:

| | per-step | realised half-life | amplitude at 6 s |
|---|---|---|---|
| before | 0.996819 (= √0.993603) | **3.626 s** | 0.35 |
| after | 0.993754 | **1.844 s** | 0.100 |
| analytic target | 0.993603 | 1.800 s | 0.099 |

The residual 0.04 s is the measurement's envelope window, not the scheme.
`voxel.Water.Ripple.HalfLifeSec`'s claim of "~10% after 6 s" is now true; it was
31.5% (analytic) before.

**The fix**: one factor `AttenPerStep = Damp · mask · sponge`, applied to *both*
returned time levels. Substituting `h(n) = Fⁿ·u(n)` then turns the recurrence
into exactly the undamped scheme in `u`, so the amplitude is `Fⁿ` for **every**
mode — travelling, standing, the uniform offset, the two-texel checkerboard —
with no change to wave speed or dispersion.

**The rejected fix**, and why, because it is the one that looks cheaper: damping
`hn` alone with `2^(−2·dt/HalfLife)` also lands the oscillatory modes on the right
decay and is one edit on the C++ side. It was rejected because (a) it fixes only
the damping, leaving the **sponge** — identical structure, identical bug, floor
0.80 realised as √0.80 = 0.8944 and a 6.1 m crossing attenuating by 5.8e-6 rather
than 3.3e-11 — still broken in the boundary condition, and (b) it is exact only
where the two roots share a magnitude, which fails at λ = 0: asymmetric damping
turns `z² − 2Dz + D` into a **complex pair**, i.e. it makes a uniform offset
across the window *oscillate* instead of sinking. Symmetric damping gives
`(z − F)²`, a real double root.

**The near-miss worth remembering.** The shore mask was already applied to both
time levels, which is exactly why the mask got its full per-step factor while the
damping and the sponge got their square roots. The same reasoning, applied to the
line two below it, would have caught this in 2026-08-11's review.

### 12.2 The splat window test did the opposite of its own comment (MEDIUM)

`VoxelRippleField.cpp`, `FillSplatSlots`. The comment claimed rings were rejected
"including the case where only part of the ring would land inside"; the test
`U < −RadiusUV || U > 1.0 + RadiusUV` rejects only rings **entirely** outside, so
a ring centred at U = 1.0 was injected with half of it clipped.

**Resolved in favour of the code**: partial rings are still injected, and the
comment now says so and says why. Three reasons, in order of weight:

1. The clip can only happen within `RadiusUV` of the window edge — 1.0 m for the
   0.5 m player ring — and that band is inside the sponge (which starts 6.1 m
   before the edge and attenuates by 3.3e-11 across a crossing) *and* outside the
   water material's own fade (zero at 24.8 m of the 25.6 m half-window). The
   straight edge a clip makes is born invisible, inside an absorber.
2. Rejecting partials has a silent pathology and clipping has none: `RadiusUV`
   comes from the caller and `AutoWatch` takes it from actor bounds, so any
   radius over half the window could never be entirely inside and would be
   dropped from *every* position including directly under the camera — counted
   as "outside" when it was centred.
3. The window follows the camera, so the only disturbances near its edge are 25 m
   away, which are the ones the fade is already erasing. There is no near-field
   splash in that band to trade away.

`DroppedOutside` therefore counts exactly one thing: not one texel of that ring
lands in the window.

### 12.3 A dropped disturbance was documented as counted and was not (MINOR)

`AddDisturbance` returned early, silently, when the subsystem was unarmed or the
strength was zero, against a header comment promising "dropped (and counted)".
Now counted as `unarmed` and `inert` respectively (§7), and non-finite
strength/radius/position joins `inert` — an infinity reaching a splat slot makes
the shader's poison guard blank a *disc* of water to flat rather than ripple it.

### 12.4 `AutoWatch` derived strength from wall-clock delta (MINOR)

`Δz / DeltaTime` is the average vertical speed over the frame containing the
crossing, so the same jump makes a different ring at 30 fps and at 120. **Not
fixed** — the honest fix is the §8.4/§8.5 hooks, which have the real impact speed
— but bounded and now stated: `ImpactFraction` saturates at 6 m/s (a 1.8 m fall)
and floors at 0.25, so only entries between 1.5 and 6 m/s vary at all, and there
by at most the ratio of the two frame times.

What *was* fixed: **`Freeze` now suppresses `AutoWatch` injection entirely** (it
keeps tracking, so thawing does not fire a stale crossing), because a
frame-rate-dependent strength entering a deliberately frozen field is the one
combination a capture cannot reproduce.

Also, `AutoWatch` ran **two** `TActorIterator` passes per frame while the CVar's
cost note claimed "one water query per watched actor per frame".
`TActorIterator<T>` walks every actor in every level and filters with `IsA`, so
two classes cost 2 × O(all actors). It is now one pass with two `IsA` checks, and
the cost note describes the pass as well as the query.
