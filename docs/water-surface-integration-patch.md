# Water surface: one patch for wind waves and interactive ripples

**Status:** written, **not applied**. No editor was available and nothing here has
been built or run.

Two features were written separately and both need to touch the same few lines of
`ue-project/Tools/create_water_voxel_material.py`:

- **Wind-driven waves** — `ue-project/Tools/water_wave_graph.py`, designed in
  `docs/water-wind-waves.md`. Replaces the wave field that is inlined in the
  water material today with a wind-steered one, and adds a breaking-wave signal
  for foam.
- **Interactive ripples** — `ue-project/Tools/ripple_field_graph.py` plus
  `create_ripple_field_materials.py`, designed in
  `docs/water-interactive-ripples.md`. A camera-following height field for rings
  spreading from things that enter the water.

Each wrote its own patch note without seeing the other's. Both notes are now
partly stale: the line numbers have drifted, the wave note deletes the code the
ripple note anchors on, and one of them assumes an MPC change that has since
shipped. **This document supersedes both patch notes** (`water-wind-waves.md`
§"Integration patch note", `water-interactive-ripples.md` §8.2). The design
reasoning in those two documents is still current and still worth reading; only
the edit instructions are replaced.

**The headline correction.** Applied as the wave note writes it, this patch
changes the water on the next regeneration — visibly, at every shoreline and in
the direction the waves run. The owner signed off on the current water on
2026-08-12. §6 below is the set of defaults that makes both features ship inert,
and it is the part of this document to argue with first if you are going to argue
with any of it.

---

## 1. Read this before applying anything

**Order matters and two of the three prerequisites are hard failures, not
warnings.**

| Prerequisite | If missing |
|---|---|
| `MPC_VoxelSky` has `WindVectorMS` and `WindFieldValid` | **Already shipped** — `create_sky_material.py:525` and `:564`. Nothing to do. If they were absent, `build_wind_input` would log loudly and use its material fallback; it deliberately does not raise. |
| `MPC_VoxelSky` has `RippleFieldOrigin`, `RippleFieldInvSize`, `RippleFieldGain` | **Not shipped.** `sample_ripple_field` calls `b.collection_param(...)`, which **raises**. Water generation stops dead. Apply `water-interactive-ripples.md` §8.1 to `create_sky_material.py` and re-run it first. |
| `/Game/Voxel/RT_VoxelRippleField` exists | `sample_ripple_field` **raises** with an explanatory message. Run `create_ripple_field_materials.py` first. |

So the two halves fail very differently. The wave half degrades gracefully to a
known-good picture; the ripple half is all-or-nothing. **Land the wave half
first, on its own, and capture it.** Then the ripple half. If you must do both in
one pass, do the three prerequisites above before touching this file.

Regeneration order, which is `water-interactive-ripples.md` §8.6 and is correct:

```
create_sky_material.py               (deletes and recreates MPC_VoxelSky)
create_sky_atmosphere_dome_material.py
create_ripple_field_materials.py     (sole author of RT_VoxelRippleField)
create_water_voxel_material.py
```

**One document that is dead and must not be applied.**
`docs/weather-system-v0.md` still carries two superseded things: a five-parameter
wind draft (`WindFlowDirection`, `WindSpeedMps`, `WindSustainedMps`,
`WindGustMps`, lines 266–269 and 553–566) that was rejected as three copies of
the same quantity and **does not exist on the collection**; and a patch at lines
586–601 that drives `WaveDirBaseDeg` and `WaveAmplitudeM` directly from the MPC.
`water_wave_graph.py` subsumes that patch entirely — it drives the same two
quantities from `WindVectorMS`, with the fetch-limited scaling and the direction
convention worked out. Applying both would drive wave direction and wave height
from two places at once. Use the module; treat `weather-system-v0.md` §"the
material side" as history.

Neither of the two patch notes being merged here uses the rejected names. That
part of both is clean.

---

## 2. The edits, in order

Match on the quoted text, not on line numbers — the numbers below are for
orientation only and the two source notes are already wrong about them.

### Edit 1 — imports (currently line 441)

**Match:**

```python
from bathy_field_graph import build_slant_depth, sample_bathy_field  # noqa: E402
```

**Replace with:**

```python
from bathy_field_graph import build_slant_depth, sample_bathy_field  # noqa: E402
from ripple_field_graph import sample_ripple_field  # noqa: E402
import water_wave_graph  # noqa: E402
```

*Why:* both modules live beside this file and the `sys.path.insert` five lines
above is what makes that work under `-run=pythonscript`. `water_wave_graph` is
imported as a module rather than by name because three of its attributes are
needed (`build_wave_field`, `summary_lines`, `LEGACY_RECIPE`).

### Edit 2 — build the wave field early (currently line 807–808)

The wave field has to move about 1,500 lines up the file. That is the largest
mechanical part of this patch and there is one reason for it: the wave node now
also returns a **breaking** signal, and its consumer is the foam composite, which
is 1,200 lines above where the wave node is built today.

**Match:**

```python
    bathy_authority = bathy_b.scalar("BathyDepthAuthority", 0.85)
    bathy_weight = bathy_b.mul(bathy_authority, bathy["validity"])
```

**Replace with:**

```python
    bathy_authority = bathy_b.scalar("BathyDepthAuthority", 0.85)
    bathy_weight = bathy_b.mul(bathy_authority, bathy["validity"])

    # --- THE WAVE FIELD, BUILT HERE BECAUSE FOAM READS IT -------------------
    #
    # It used to live 1,500 lines down, beside the normal and the WPO it feeds.
    # It is here now because water_wave_graph's node returns a fourth output --
    # a breaking-wave signal -- and the foam composite below is its consumer.
    # Nothing about the field changed by moving it: it is a pure function of
    # absolute world position and time, and both of its original consumers still
    # read it exactly where they always did.
    #
    # THIS IS ALSO THE EARLIEST POINT IT CAN GO. It reads `bathy`, and `bathy` is
    # built immediately above.
    #
    # ONE Time node still drives the whole field, so VOXEL_WATER_FREEZE_TIME is
    # still a single-node substitution -- and it now freezes the breaking foam
    # too, by construction.
    if FREEZE_RIPPLE_TIME:
        ripple_time = mel.create_material_expression(
            material, unreal.MaterialExpressionConstant, -900, 950)
        ripple_time.set_editor_property("r", 0.0)
    else:
        ripple_time = mel.create_material_expression(
            material, unreal.MaterialExpressionTime, -900, 950)

    world_pos_abs = mel.create_material_expression(
        material, unreal.MaterialExpressionWorldPosition, -1300, 700)
    world_pos_abs.set_editor_property(
        "world_position_shader_offset",
        unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)

    world_xy = mel.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -1120, 700)
    world_xy.set_editor_property("r", True)
    world_xy.set_editor_property("g", True)
    world_xy.set_editor_property("b", False)
    world_xy.set_editor_property("a", False)
    if not mel.connect_material_expressions(world_pos_abs, "", world_xy, ""):
        raise RuntimeError("connect world_pos_abs -> world_xy failed")

    uu_to_m = mel.create_material_expression(
        material, unreal.MaterialExpressionConstant, -1120, 780)
    uu_to_m.set_editor_property("r", 0.01)
    wave_pos_m = mel.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -960, 720)
    if not mel.connect_material_expressions(world_xy, "", wave_pos_m, "A"):
        raise RuntimeError("connect world_xy -> wave_pos_m.A failed")
    if not mel.connect_material_expressions(uu_to_m, "", wave_pos_m, "B"):
        raise RuntimeError("connect uu_to_m -> wave_pos_m.B failed")

    # THE WIND FEATURE SHIPS SWITCHED OFF, AND THAT IS THE POINT OF THIS LINE.
    #
    # water_wave_graph.DEFAULTS is the module's view of a good lake, and it is a
    # DIFFERENT lake from the one the owner signed off on 2026-08-12: waves
    # steered into a 35-degree cone about the wind, four times as much wave
    # reaching the beach, and a white surf band at every shoreline. All real
    # improvements; none of them a thing to apply to a signed-off picture without
    # being asked.
    #
    # LEGACY_RECIPE is the module's own machine-readable statement of "reproduce
    # the shipped field", verified offline against a transcription of the shipped
    # HLSL to 5.1e-11 m. Shipping it means a regeneration reproduces today's
    # water, and switching the feature on is five scalar overrides on a material
    # instance -- no regeneration, and reversible in the editor.
    #
    # See docs/water-surface-integration-patch.md section 6 for the full table
    # and for what each default does if it is wrong.
    WAVE_DEFAULTS = dict(water_wave_graph.LEGACY_RECIPE)

    wave_field = water_wave_graph.build_wave_field(
        bathy_b, wave_pos_m, ripple_time, bathy,
        defaults=WAVE_DEFAULTS, log=unreal.log_warning)
    wave_grad_raw = wave_field["gradient"]     # float2, dH/dx and dH/dy
    wave_height_m = wave_field["height_m"]     # float, metres
    wave_breaking = wave_field["breaking"]     # float 0..1, for foam

    # Logged here rather than beside water_optics.summary_lines() a few hundred
    # lines up, because wind_source is not known until the build has run.
    for _line in water_wave_graph.summary_lines(wave_field["wind_source"]):
        unreal.log("M_WaterVoxel " + _line)
    unreal.log("M_WaterVoxel wind waves SHIPPED INERT (LEGACY_RECIPE): %s"
               % ", ".join("%s=%g" % kv for kv in sorted(WAVE_DEFAULTS.items())))
```

*Why:* foam needs the breaking signal, and the breaking signal is an output of
the wave node.

*Departure from the wave note:* it put the log line at step 6, up beside
`water_optics.summary_lines()` at line 589, then noted that this cannot work
because the build has not run yet. Folded into the build instead. Also, the
`defaults=` argument is new here — see §6.

### Edit 3 — the breaking signal into foam (currently line 1396)

**Match:**

```python
    foam_raw = bathy_b.maximum(foam_raw, shore_foam)
```

**Replace with:**

```python
    foam_raw = bathy_b.maximum(foam_raw, shore_foam)
    # SIGNAL 4: BREAKING WAVES. MAX and not add, for the reason stated just
    # above -- these describe one physical thing from four directions, and a
    # breaking wave over a foamy shore should be fully white rather than 2x
    # white and clipped. Gated three ways inside the wave node (wave size, crest
    # phase, bathymetry validity) plus BreakFoamGain, which ships at 0, so on the
    # shipped material this contributes exactly zero.
    foam_raw = bathy_b.maximum(foam_raw, wave_breaking)
```

*Why:* it is what the wave field's fourth output is for. Note that `foam` also
drives Opacity (`Opacity = saturate(foam)`), so anything that raises foam also
makes the water more opaque there — that pairing is already documented at
`BathyFoamGain` and it applies to this signal too.

### Edit 4 — delete the moved coordinate block (currently 2326–2354)

Delete from the line

```python
    if FREEZE_RIPPLE_TIME:
```

through, and including, the line

```python
        raise RuntimeError("connect uu_to_m -> wave_pos_m.B failed")
```

*Why:* moved verbatim into Edit 2. `if FREEZE_RIPPLE_TIME:` occurs exactly once
inside this function, so the anchor is unambiguous.

### Edit 5 — delete the knob block, in two pieces (currently 2356–2477)

**5a.** Delete from

```python
    # --- The knobs, all of them instance-tunable ---------------------------
```

through, and including,

```python
    wave_amp = scalar_param("WaveAmplitudeM", 0.25, -1300, 860)
```

**5b.** Delete from

```python
    # Wavelength of the FIRST octave, in metres. Eight octaves at the 1.42
```

through, and including,

```python
    wave_patch = scalar_param("WavePatchContrast", 0.55, -1300, 1160)
```

**KEEP the block between them** — the `WaveWpoFraction` comment and
`wave_wpo_fraction = scalar_param("WaveWpoFraction", 0.25, -1300, 920)`.

*Why:* `water_wave_graph` authors all six of those parameters itself, under the
same names and the same defaults, so an existing material instance that overrides
`WaveAmplitudeM` keeps working. `WaveWpoFraction` is applied **outside** the node
and stays here.

*Note for whoever applies it:* the long comment at `WaveWpoFraction` explains the
0.25 by saying "there is still no shore-damping term to stop a rising crest
clipping through its own bank". That sentence is now out of date — the node
derives a depth limit instead (`water-wind-waves.md`, "It hands back a guarantee
that used to be a guess"). Leaving the comment stale is a small, real cost of
keeping this patch minimal; correcting it is a two-line follow-up and not part of
this change.

### Edit 6 — delete the inline HLSL (currently 2479–2590)

Delete from the two-line anchor

```python
    # ------------------------------------------------------------------------
    # THE WAVE FIELD ITSELF.
```

through, and including, the closing

```python
"""
```

of `WAVE_CODE` (the line immediately before `    wave = custom_node(`).

*Why:* the HLSL now lives in `water_wave_graph.wave_code()`. The attribution and
licence note in that comment block moved with it — it is in the module's
docstring, unchanged, including the unverified-licence caveat.

### Edit 7 — delete the node build, the shore damping and the wiring loop (currently 2592–2627)

Delete from

```python
    wave = custom_node(
```

through, and including,

```python
            raise RuntimeError("connect -> WaveField.%s failed" % pin)
```

*Why:* `build_wave_field` does all three. The Phase-3 shore damping
(`BathyWaveDampDepthM`) is inside that range and is deliberately deleted: the node
owns the depth response now, as `BreakSurfFloorM`. With `BreakSurfFloorM = 0.6`
the node computes `saturate(depth / 0.6)` and lerps it against `Validity` —
arithmetically the same expression the deleted `bathy_b.ramp` / `bathy_b.lerp`
pair builds, which I checked by reading both (`GraphBuilder.ramp` is
`saturate((v - lo) / (hi - lo))`, linear, and the HLSL is `saturate(xr)`, also
linear).

**`BathyWaveDampDepthM` disappears as a parameter name.** Any material instance
or console tweak that overrides it silently stops doing anything. Its replacement
is `BreakSurfFloorM`, same meaning, same units.

### Edit 8 — the ripple field and the two sums (currently just before line 2629)

Insert immediately **before** the surviving comment

```python
    # --- NORMAL -------------------------------------------------------------
```

**Insert:**

```python
    # --- THE INTERACTIVE RIPPLE FIELD, SUMMED INTO BOTH OUTPUTS -------------
    #
    # A second height field on the same surface, from a camera-following render
    # target: rings spreading from things that entered the water. Same shape as
    # the wave field -- (dH/dx, dH/dy, H) with H in metres -- and it composes
    # with it by plain addition and nothing else.
    #
    # SUMMING GRADIENTS IS THE WHOLE ARGUMENT, and the note at the normal
    # assembly immediately below already makes it: averaging or lerping unit
    # normals systematically flattens slopes. Two independent height fields on
    # one surface superpose, so their gradients add, and the normal assembled
    # from the sum is the normal of the combined surface. Any other order is a
    # different and wrong surface.
    #
    # IT IS DELIBERATELY NOT DEPTH-DAMPED. The wave field's shore damping lives
    # INSIDE the WaveField node (BreakSurfFloorM), applied to the wave's own
    # amplitude, so the ripple is downstream of it and cannot be reached by it.
    # That is what we want: damping a splash by depth would delete it exactly at
    # the shoreline, which is where a player enters the water. The ripple has its
    # own shore treatment and it is a horizontal one -- the SIMULATION is masked
    # by the baked signed distance to shore, so a ripple dies against the bank
    # rather than fading out with depth.
    #
    # WHAT THAT COSTS, stated so it is not discovered from a screenshot: the
    # wave's depth limit also carries a guarantee that a crest cannot stand
    # taller than the water it is in. The ripple bypasses that guarantee along
    # with the damping. Bounded in practice by RippleFieldGain, by the shore mask
    # dying 25 cm inside the waterline, and by WaveWpoFraction taking the
    # geometry to a quarter of the ring -- but it is a bound by three small
    # numbers rather than a proof. Accepted: a splash poking a centimetre through
    # a shallow bank for a second is a better failure than no splash.
    ripple = sample_ripple_field(bathy_b)

    # Both sums happen HERE, at one site, upstream of the -1, upstream of the
    # VertexColor.B top-face mask, upstream of the metres->UU conversion and
    # upstream of WaveWpoFraction. See section 4 of
    # docs/water-surface-integration-patch.md for why each of those matters.
    wave_grad_total = bathy_b.add(wave_grad_raw, ripple["grad_xy"])
    wave_height_total = bathy_b.add(wave_height_m, ripple["height_m"])
```

*Why:* this is the "both adds at the same site" the wave note asked for, written
out. It sits next to its two consumers rather than 1,500 lines away, because
unlike the wave field it has no early consumer.

### Edit 9 — delete the gradient mask and repoint the normal (currently 2645–2657)

**Match** (thirteen lines, contiguous):

```python
    wave_grad = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -540, 860)
    wave_grad.set_editor_property("r", True)
    wave_grad.set_editor_property("g", True)
    wave_grad.set_editor_property("b", False)
    wave_grad.set_editor_property("a", False)
    if not mel.connect_material_expressions(wave, "", wave_grad, ""):
        raise RuntimeError("connect wave -> wave_grad failed")

    neg_one = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -540, 940)
    neg_one.set_editor_property("r", -1.0)
    normal_xy_raw = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -380, 880)
    if not mel.connect_material_expressions(wave_grad, "", normal_xy_raw, "A"):
        raise RuntimeError("connect wave_grad -> normal_xy_raw.A failed")
```

**Replace with:**

```python
    neg_one = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -540, 940)
    neg_one.set_editor_property("r", -1.0)
    normal_xy_raw = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -380, 880)
    if not mel.connect_material_expressions(wave_grad_total, "", normal_xy_raw, "A"):
        raise RuntimeError("connect wave_grad_total -> normal_xy_raw.A failed")
```

*Why:* `build_wave_field` returns the `.xy` mask already made, so the local mask
node is redundant; and the input is now the sum, not the wave alone.

### Edit 10 — delete the height mask and repoint the WPO (currently 2701–2715)

**Match:**

```python
    wave_height_m = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -540, 1020)
    wave_height_m.set_editor_property("r", False)
    wave_height_m.set_editor_property("g", False)
    wave_height_m.set_editor_property("b", True)
    wave_height_m.set_editor_property("a", False)
    if not mel.connect_material_expressions(wave, "", wave_height_m, ""):
        raise RuntimeError("connect wave -> wave_height_m failed")
```

**Delete those seven lines**, keeping the comment block above them, then:

**Match:**

```python
    if not mel.connect_material_expressions(wave_height_m, "", wave_height_uu, "A"):
        raise RuntimeError("connect wave_height_m -> wave_height_uu.A failed")
```

**Replace with:**

```python
    if not mel.connect_material_expressions(wave_height_total, "", wave_height_uu, "A"):
        raise RuntimeError("connect wave_height_total -> wave_height_uu.A failed")
```

*Why:* same as Edit 9, for the displacement half.

### Edit 11 — two comment corrections (optional but cheap)

At the Time-node census, currently line 2857:

**Match:** `    # There is exactly ONE MaterialExpressionTime in this graph (it feeds the`
followed by `    # normal waves and the WPO waves both), so the count is 1 when live and 0`

Reword the parenthesis to `(it feeds the normal, the WPO and the breaking foam)`.
The assertion itself is still correct and still passes: neither
`build_wave_field` nor `sample_ripple_field` creates a `Time` node — the wave
takes time as an argument, and the ripple's animation lives in the render target,
not in this material.

In the module docstring, beside the existing "THE REPEATING TILE (2026-08-11,
Phase 5)" section, add a pointer saying the wave field now lives in
`Tools/water_wave_graph.py` with its design note at `docs/water-wind-waves.md`,
and the ripple in `Tools/ripple_field_graph.py` with
`docs/water-interactive-ripples.md`. The Phase 5 reasoning stays where it is — it
is still the reasoning for the octave count, the 1.42 frequency ratio, the
quantisation knob and the drag warp, none of which changed.

---

## 3. Deliberately not part of this patch

- **The ripple does not feed foam.** It could, and a splash arguably should be
  white. Nobody has designed the gate that stops a passing camera drawing foam
  everywhere. Separate change.
- **No `RippleWpoFraction`.** The ripple's height goes through `WaveWpoFraction`
  (0.25), so a 9 cm ring lifts geometry 2.25 cm and the normal carries the rest.
  If the owner wants the ring to visibly lift the surface, the fix is a separate
  fraction on the ripple half of the sum — *not* raising `WaveWpoFraction`, which
  would also make the wind wave slosh over its banks.
- **`bathy_field_graph.sample_bathy_field` is not given a `world_xy` argument**
  (`water-interactive-ripples.md` §8.3). It removes a five-line duplication and
  touches a file three shipped materials read. Not worth the blast radius today.
- **`WaveWpoFraction` is not raised**, even though the node's depth limit now
  justifies considering it. That is a look change and the owner judges those from
  captures.

---

## 4. The composition order at the shared site

Stated once, precisely, because both patch notes gesture at it and neither writes
it down.

```
wave_grad_total   = wave.gradient  +  ripple.grad_xy      # dimensionless, world XY
wave_height_total = wave.height_m  +  ripple.height_m     # metres
```

and then, unchanged from what ships today:

```
Normal = append( wave_grad_total * (-1) * VertexColor.B ,  1.0 )

WPO.z  = wave_height_total * 100 * WaveWpoFraction * VertexColor.B
WPO    = fill_drop_offset + (0,0,1) * WPO.z
```

Four things sit in a specific place and each of them is load-bearing.

**The sum is before the `-1` and the append.** A height field's tangent-space
normal is `(-dH/dx, -dH/dy, 1)`. Summing the gradients first and converting once
gives the normal of the *combined* surface, which is what physical superposition
means. Converting each field to a normal and then combining them does not: unit
normals do not add, and averaging or lerping them systematically flattens slopes.
That argument is already written at the normal assembly in the file — it is why
the wave loop accumulates gradients across octaves instead of blending normals —
and the ripple is just a ninth octave from a different source. Both patch notes
agree on this and both are right.

**Both terms are inside the `VertexColor.B` mask, not outside it.** `B` is 1 only
on a vertex sitting on its own voxel's +Z boundary — the top-face flag. On the
**normal** it is a per-vertex multiplier on a pixel-shader input, so it
interpolates down a side wall and the shimmer fades out over the wall's height
instead of snapping off. Cosmetic, and correct for both terms: a splash should no
more light up a submerged wall than a wind wave should. On the **WPO** it is not
cosmetic at all. An unmasked vertex offset would lift a side wall's *bottom*
vertices off the floor they are sealed against and open the mesh. A ripple summed
outside that mask would tear water bricks apart. It must be inside.

**Both terms are downstream of the wave's depth damping, and that is automatic.**
The damping is `ampEff = amp * saturate(depth / dSurf)` **inside** the WaveField
custom node, applied to the wave's own amplitude before the node returns. The
ripple is added to the node's *output*. There is no wiring by which the damping
could reach it.

**The ripple term is upstream of `WaveWpoFraction`.** A consequence rather than a
choice: the ripple's geometry contribution is a quarter of its height. Flagged in
§3.

### Does the ripple agent's bypass argument hold? Yes — with a caveat, and its stated mechanism is stale

The ripple note argues: *the ripple must bypass `BathyWaveDampDepthM`, because
damping a splash at the shoreline deletes it exactly where a player enters the
water; and it bypasses it for free, because that term is applied to
`WaveAmplitudeM`, which is upstream.*

Checked against the code:

- **The conclusion is right.** Depth-damping a locally-generated disturbance is
  the wrong physics. The wave damping exists because a wind-wave train arriving
  from offshore cannot stand taller than the water it reaches. A splash is
  created at the point of entry, at whatever depth the player is standing in. The
  two are different problems and the wave's answer does not transfer.
- **The mechanism it cites is stale after this patch.**
  `BathyWaveDampDepthM` ceases to exist (Edit 7). The damping moves inside the
  node as `BreakSurfFloorM`. The bypass still holds, and holds *more* cleanly —
  under the old arrangement the damping was a graph node outside the wave, so a
  future edit could have accidentally routed the ripple through it; now it is
  unreachable by construction. The comment in Edit 8 above is rewritten to say
  so.
- **One thing the note does not say, and should.** The wave patch's depth limit
  comes with a guarantee: inside the surf zone the crest is bounded by the local
  depth at any wind speed, so it cannot clip its own bank. The ripple bypasses
  the guarantee along with the damping. Its own shore treatment — the
  simulation multiplied by `saturate(shore_m / 0.25)` — is a *horizontal* mask.
  It correctly gives zero on land and zero at the waterline, but 30 cm inside the
  water it gives 1.0 regardless of how shallow that water is. So a large enough
  ring in shallow enough water can put geometry above the local surface.
- **The trade, stated:** in practice this is bounded by `RippleFieldGain`, by the
  25 cm shore mask, and by `WaveWpoFraction` taking geometry to a quarter of the
  ring. That is three small numbers, not a proof. I would take it — a splash
  clipping a bank for a moment is a far better failure than a splash that
  vanishes exactly where the player made it — but it should be a known trade, not
  a surprise. If it ever shows in a capture, the fix is a depth *clamp* on the
  ripple's WPO half only (`min(ripple_h, 0.5 * depth)`), which leaves the shading
  ring intact and only limits the geometry. Not implemented.

---

## 5. Conflicts between the two patch notes, and how they are resolved

**1. The ripple note anchors on code the wave note deletes.** All three of its
edit sites are inside the wave note's deletion ranges: it inserts "after the
`WaveField` input wiring loop" (deleted, Edit 7), and repoints connects from
`wave_grad` (deleted, Edit 9) and the `wave_height_m` mask node (deleted, Edit
10). Applied in the order the notes suggest, the second one applied fails to find
its anchors. **Resolved** by re-anchoring: the ripple is sampled at the top of
what survives of the old wave section, and the sums use `wave_grad_raw` and
`wave_height_m` as returned by `build_wave_field`. The wave module deliberately
keeps the name `height_m`, so half the ripple note's naming already lines up.

**2. Both notes claim the same insertion point and neither knows it.** The wave
note moves the wave *build* to line ~808; the ripple note inserts at ~2627.
Because the wave build moves and the normal/WPO assembly does not, the two do not
actually collide once re-anchored — but the ripple's sums must be built *after*
the wave build, which is now 1,500 lines earlier, and that is only obvious once
both are on the page. **Resolved:** wave early (foam needs it), ripple late (next
to its only consumers).

**3. The damping rationale.** Covered in §4. Conclusion kept, wording replaced.

**4. The wave note's step 8 is stale — the MPC parameters already shipped.** It
says the two wind parameters are "not part of this change and not applied", and
predicts `build_wind_input` will log a warning and use the material fallback.
`create_sky_material.py:525` and `:564` already carry `WindFieldValid` (0.0) and
`WindVectorMS` (0,0,0,0). So the build takes the **MPC path**, and the log will
say `wind source THIS BUILD: MPC_VoxelSky`, not `material-fallback`. This is
still safe at generation time — `WindFieldValid` defaults to 0 and
`build_wind_input` returns `lerp(fallback, mpc, 0)` = the fallback exactly — but
it changes what a reviewer should expect in the log, and it opens a **runtime**
path to a changed lake that the wave note does not discuss. See §6.

**5. The wave note's log placement cannot work as written.** Step 6 puts
`summary_lines(wave_field["wind_source"])` at line 589, before the material even
exists. The note spots this and does not resolve it. **Resolved:** the log moves
down to the build.

**6. `weather-system-v0.md` would double-drive the same two quantities.** Covered
in §1. It is dead; `water_wave_graph.py` replaces it.

**7. Neither note mentions the other's cost.** Each estimates its own instruction
count and both say plainly that nothing was measured. Merged, the water material
gains the wave node's extra instructions *and* a texture sample that runs in both
the vertex and pixel shaders. Nobody has measured either. See §8.

---

## 6. The shipped defaults: what makes this change nothing

This is the hard requirement. The owner signed off on the current water on
2026-08-12; the next regeneration must reproduce it unless someone deliberately
opts in.

### The wave half does NOT ship inert on the module's own defaults

`water_wave_graph.DEFAULTS` is the module's view of a good lake and it is a
different lake. The wave note's `build_wave_field(...)` call passes no
`defaults=`, so it would ship `DEFAULTS`. **That is the single most consequential
correction in this document**, and it is why Edit 2 passes
`defaults=water_wave_graph.LEGACY_RECIPE`.

| Parameter | Module default | **Must ship** | What goes wrong if it is the module default |
|---|---|---|---|
| `WindDirectionAuthority` | 1.0 | **0.0** | The eight octave directions collapse from a whole-circle golden-angle spread into a 35° cone about the wind, and the calm/choppy patch field starts drifting downwind. Visible with no weather subsystem at all, because the fallback wind is always present. The single most visible change. |
| `BreakSurfFloorM` | 0.15 | **0.6** | This is the old `BathyWaveDampDepthM`. At 0.15 the wave reaches the beach roughly four times taller (measured in the design note: 1.22 cm → 4.88 cm at 5 m/s). Intended, and a clear change to the shoreline. |
| `BreakFoamGain` | 1.0 | **0.0** | A surf band appears at every shoreline. |
| `BreakShoalGain` | 0.4 | **0.0** | Waves grow up to 1.4× as they approach the break line. |
| `BreakPeakGain` | 0.6 | **0.0** | Crests get peaked. Already dead when `BreakFoamGain` is 0 (the peaking is multiplied by `breaking`), but set explicitly — belt and braces, and it is what `LEGACY_RECIPE` says. |
| `WaveAmplitudeM` 0.25, `WaveBaseWavelengthM` 5.0, `WaveDirBaseDeg` 58.7, `WaveDirIncrementDeg` 137.507764, `WaveQuantPerVoxel` 1.0, `WavePatchContrast` 0.55 | as shipped | unchanged | Nothing. The module already carries the shipped values. |
| `WindFallbackSpeedMS` 5.0 vs `WindRefSpeedMS` 5.0 | equal | keep equal | If they differ, the speed ratio `u ≠ 1` and both the amplitude scale (`u^1.0`) and the wavelength scale (`u^0.68`) leave 1.0. The whole legacy reproduction rests on this pair being equal. |
| `WaveWpoFraction` | 0.25 | 0.25 | Stays in the water material; not touched. |

I verified by reading the HLSL that this set really is the shipped field:
`WindDirAuthority = 0` makes the per-octave angle `baseAng + dIncRad*i` (the
shipped accumulation) and zeroes the patch advection; `u = 1` makes `ampScale`
and `lenScale` exactly 1.0 and `omega0` exactly 2.0 (the shipped time
multiplier); with `BreakSurfFloorM = 0.6` and a 6 cm wave the break depth is
7.6 cm, far below the floor, so `dSurf = 0.6` and the damping is
`lerp(1, saturate(depth/0.6), Validity)` — the same expression, term for term, as
the `bathy_b.ramp` / `bathy_b.lerp` pair being deleted; `BreakFoamGain = 0` makes
`breaking` identically zero, which in turn zeroes the peaking. The module's own
docstring reports 5.1e-11 m maximum difference against an offline transcription
of the shipped HLSL.

### There is a runtime path that bypasses all of that

`WindFieldValid` and `WindVectorMS` are already on the collection. The moment
`UVoxelWeatherSubsystem` writes `WindFieldValid = 1`, `u` stops being 1 and the
lake changes **even under `LEGACY_RECIPE`** — the direction is still locked
(authority 0) but the *size* is not, because `ampScale = u^1`. Two specific
outcomes worth knowing about:

- A published wind of 5 m/s in any direction: no visible change. That is the
  calibration point.
- A published **dead calm** (`WindVectorMS = 0`, `WindFieldValid = 1`): `u = 0`,
  `ampScale = 0`, and the lake goes to **glass**. Not a bug — it is the
  designed-for behaviour of a wind-driven field — but it is a large change gated
  on a single scalar written by C++, and it is not something the wave note calls
  out.

So "nothing changes" is: `LEGACY_RECIPE` **and** `WindFieldValid` stays 0. The
first is a code default in this patch; the second is owned by the weather
subsystem, which is not yet publishing. Worth a line in whatever lands that
subsystem.

### The ripple half does ship inert, on collection defaults

| Parameter | Where | **Must ship** | Effect |
|---|---|---|---|
| `RippleFieldGain` | MPC (`create_sky_material.py`, §8.1) | **0.0** | Both ripple outputs are `× (gain × edge_fade)`. Gain 0 makes both sums add exactly 0.0. This is the off switch and I confirmed it by reading `sample_ripple_field`. |
| `RippleFieldInvSize` | MPC | **0.0** | Collapses every world position onto one texel. Harmless *only because gain is 0*. These two are a pair — `InvSize` at 0 with a non-zero gain would smear one texel across the world. |
| `RippleFieldOrigin` | MPC | **(0,0,0,0)** | Same pairing. |
| `RippleFieldTex` | material texture parameter | bound to the real RT | Not a gate. See below. |

**The one thing that is not free at default.** The texture sample runs whether or
not the gain is zero. `RippleFieldGain` is a *collection* parameter, so the
compiler cannot fold it to a constant and cannot dead-strip the fetch. The
shipped-inert material therefore pays one texture fetch per water pixel and one
per water vertex for a result that is multiplied by zero. That is the honest cost
of "ships inert but switchable without a regeneration". The alternative is a
generation-time arm like `VOXEL_SHORE_FX` that omits the sample entirely — and
its cost is that switching ripples on then requires regenerating the water
material. I would keep the sample: this project already has an arm-vs-parameter
precedent recorded at `BreakFoamGain`, and the same reasoning applies.

### How a reviewer verifies "no change" from a capture

1. Regenerate with **`VOXEL_WATER_FREEZE_TIME=1`**, same seed, same pinned camera
   pose, same clock. Freezing time removes the only frame-to-frame variation and
   makes a diff meaningful at all.
2. Diff against the frozen capture of the signed-off build.
3. **Expect visually identical, not bitwise zero.** The module reproduces the
   field to 5.1e-11 m in float64 offline, but on the GPU in float32 the
   arithmetic is re-associated in two places — the octave angle is computed as
   `base + inc*i` instead of accumulated, and the damping is applied after the
   octave loop instead of folded into the amplitude before it. Expect a scatter
   of least-significant-bit differences. **Structure** in the diff is the signal;
   noise is not.

Each of the five ways this can go wrong has its own tell, so a bad capture names
its own cause:

| What you see | What is wrong |
|---|---|
| The waves all run one way; the corduroy has turned | `WindDirectionAuthority` ≠ 0 |
| A white band at every shoreline | `BreakFoamGain` ≠ 0 |
| Waves visibly larger right at the bank | `BreakSurfFloorM` ≠ 0.6 |
| The lake is glass | `WindFieldValid` raised over a dead-calm wind |
| Rings, or a faint square, near the camera | `RippleFieldGain` ≠ 0 |

Three log lines to check on the generation:

- `M_WaterVoxel wind waves SHIPPED INERT (LEGACY_RECIPE): BreakFoamGain=0, BreakPeakGain=0, BreakShoalGain=0, BreakSurfFloorM=0.6, WindDirectionAuthority=0`
- `M_WaterVoxel   wind source THIS BUILD: MPC_VoxelSky` — **not**
  `material-fallback`, now that the two parameters have shipped. If it says
  `material-fallback`, `create_sky_material.py` has not been re-run since the
  wind parameters landed.
- `M_WaterVoxel RIPPLE TIME ARM: LIVE (VOXEL_WATER_FREEZE_TIME=..., Time nodes=1)`
  — the census assertion already in the file. It still holds after this patch and
  will raise if it does not.

---

## 7. Rollback

**One argument.** Restore

```python
        defaults=WAVE_DEFAULTS,
```

to `WAVE_DEFAULTS = dict(water_wave_graph.LEGACY_RECIPE)` in Edit 2 and
regenerate the water material. That reverts the wave half completely.

The ripple half needs no code change at all: `RippleFieldGain` is a collection
value, so `voxel.Water.Ripple.Enable 0` (or writing 0 to the parameter) reverts it
at runtime, in the frame.

If the graph itself is suspect rather than its tuning, the next-smallest revert is
two lines — change `wave_grad_total` back to `wave_grad_raw` and
`wave_height_total` back to `wave_height_m` in Edits 9 and 10, which removes the
ripple from the material entirely and leaves the wave module in place.

---

## 8. What I could not determine without the editor

Explicitly, rather than plausibly. **No editor and no shader compiler were
available. Nothing below has been built, run or photographed.** What I did do:
read every site in `create_water_voxel_material.py`, `water_wave_graph.py`,
`ripple_field_graph.py`, `sky_star_graph.py`, `terrain_material_common.py`,
`bathy_field_graph.py` and `create_sky_material.py`, and byte-compile all of them
with `python -m py_compile` (all pass; none were modified).

1. **Neither HLSL block compiles-checks.** `water_wave_graph.wave_code()` and the
   ripple step/derive shaders are unverified. The wave module's named risks are
   the `const float SPREAD_SHAPE[8] = {...}` initialiser inside a Custom node and
   the `round()` intrinsic; the ripple module's are in its §11.
2. **Whether the ripple's texture sample compiles in the vertex shader.** Its
   height feeds World Position Offset, so the fetch runs per water vertex, where
   automatic mip selection is unavailable. **Strong precedent that it is fine:**
   the *shipped* material already does exactly this — the bathymetry texture's
   `depth_m` reaches WPO through the wave amplitude, and it builds today. Risk is
   low but it is not zero and I have not compiled it.
3. **Whether `bathy_b.collection_param` resolves the three `RippleField*` names.**
   It reads the live `MPC_VoxelSky`, so it depends on `create_sky_material.py`
   having been edited *and* re-run in the same editor session. I could do
   neither. This is the most likely first-run failure and it fails loudly.
4. **Instruction count and frame cost.** Both design notes give estimates and both
   say plainly that nothing was measured. Merged, the material gains the wave
   node's extra instructions plus a vertex-and-pixel texture fetch. `ProfileGPU`
   at a pinned pose over a lake is the instrument. Not run.
5. **The GPU float32 delta between the shipped field and `LEGACY_RECIPE`.** The
   5.1e-11 m figure is an offline float64 comparison. What a float32 shader
   actually produces after the two re-associations described in §6 is unmeasured,
   so I cannot promise a pixel-identical capture — only that any difference
   should be unstructured.
6. **Whether the ripple's duplicate `WorldPosition` node is common-subexpression
   eliminated.** `sample_ripple_field` builds its own rather than accepting one,
   and it is on the do-not-edit list, so the merged graph has two identical
   `WPT_EXCLUDE_ALL_SHADER_OFFSETS` world-position nodes. Almost certainly folded
   by the compiler. Not checked.
7. **Whether the enlarged graph trips any editor limit** — node count,
   `layout_material_expressions`, Custom-node input count (the wave node has 24
   pins). Unknown.
8. **The look.** Nobody has seen either feature. No verdict is offered on whether
   the wind waves or the ripples look like water; that is a capture, and the owner
   judges it.
