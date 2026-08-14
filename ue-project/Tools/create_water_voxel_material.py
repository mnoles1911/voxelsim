"""Author M_WaterVoxel (W2 task spec item 4 "Rendering v0"): the active-water
voxel material UWaterChunkComponent/FWaterChunkSceneProxy render with
(VoxelWaterChunkComponent.h/.cpp), distinct from M_Ocean's implicit static
surface plane (AVoxelOceanActor -- that one keeps existing unchanged; active
water renders ON TOP of it per the task spec, overlap acceptable v0).

Same headless-Python pattern as create_voxel_material.py / create_ocean_material.py
(checked connections throughout -- see create_voxel_material.py's comment on
why a silently-failed pin connect is worth guarding against explicitly).

Translucent blue, vertex-color-driven AO, plus a stepped fill-fraction surface.

Vertex-color convention, which now DIFFERS from M_VoxelTerrain's in R:
  R = CA fill fraction 0..255 remapped to 0..1 (was: an unused fixed material-id
      placeholder). Drives the World Position Offset that seats each surface
      cell's top boundary at its own fill height, so a waterline reads as
      discrete 10 cm steps instead of popping in and out at a >=128 threshold.
  G = AO 0/85/170/255, so greedy-mesher AO shades consistently with terrain.
  B = 1 if this vertex sits on the +Z boundary of its own voxel, else 0. The
      WPO moves only those, so a partial cell's side walls shorten with its
      surface instead of standing proud of it.
  A = per-brick FOAM ACTIVITY 0..1 (W5; was a fixed 255, unused). 1 while
      vxc::WaterCA still calls this brick active, 0 once it settles and 0 for
      every implicit (worldgen) brick. The pooled path reaches it via
      ChunkParams.y, which the vertex factory already copies into colour A in
      every mode -- no new uniform-buffer member, and therefore no exposure to
      the loose-FShaderParameter trap that silently no-ops in this factory.

Terrain packs a binary sky-facing biome flag in R and per-chunk climate in B,
so this is water's OWN convention, not a shared one. The component path writes
it directly (VoxelWaterChunkComponent.cpp) and the pooled path reproduces it
under FVoxelQuadVertexFactoryParameters::WaterMode.

SUPERSEDED, ALL OF IT, BY THE SINGLE LAYER WATER PORT BELOW (2026-08-11). The
next four sections -- W5 colour, W6 still water, W3 motion, and the sort-key
argument they all lean on -- are the history of a TRANSLUCENT material, and this
material is no longer translucent. They are kept because the reasoning in them
is still the reasoning for what replaced each piece, and because two of them
record measurements (the near-WHITE regression, the grey-wash control frame)
that a future change could otherwise repeat. Read the SLW section at the bottom
for what is actually in the graph today.

W5 COLOUR: the constant tint and constant 0.55 opacity are GONE,
replaced by depth-tinted absorption and foam. Read this before touching either.

That constancy was load-bearing rather than lazy: docs/gpu-water-pool-design.md
shows the water pool was safe as ONE primitive with ONE translucent sort key
only while it held, since N identical surfaces transmit (1-0.55)^N in any blend
order. Both terms below make two water fragments differ in COLOUR and OPACITY,
not merely in lighting, so `over` composition stops being order-independent and
the single sort key stops being sound.

THE SORT WORK LANDED FIRST, IN THE SAME WAVE AND DELIBERATELY BEFORE THIS: the
water pool is now several primitives bucketed at 64 bricks (51.2 m) of world
space -- see GetOrCreateWaterPoolBucket in VoxelWaterSubsystem.cpp for why that
size and what it does and does not fix. The ordering was the point: a tint
applied over a broken sort makes a sorting artefact and a shading artefact
indistinguishable, and there is no way back from that except reverting one.

WHAT IS STILL NOT DONE, and is still a sort-key hazard if added: REFRACTION or
any scene-COLOUR read. Reading scene DEPTH (below) is not the same thing --
depth is written by the opaque pass and is invariant to translucent draw order,
so each fragment computes its own thickness identically however the stack is
composed. A scene-colour read is not, because the value it reads IS the
partially composed stack.

W6 STILL WATER (this update): gives the surface a Fresnel-weighted sky
reflection and an analytic sun glint, both independent of the foam channel, and
folds the same Fresnel into opacity. See "W6: THE STILL-WATER SURFACE" below for
the measurement that motivated it and for the correction it carries -- in short,
a settled basin was never invisible, it was a flat tinted film with no
view-dependent term, and the claim that vertex colour A was "what makes water
visible" does not survive reading this graph. Foam remains a lerp layered on
top and is unchanged; the only authored constant moved is shallow opacity,
0.55 -> 0.35.

W3 "Rendering v0" MATERIAL MOTION (this update): makes the surface look like it
is moving, material graph only, still without touching colour/opacity/refraction
-- the sort-key doc is explicit that vertex movement and lighting-only normal
variation are safe, and everything below is one of those two things:

  * Translucency lighting mode switched Volumetric NonDirectional (engine
    default, ignores the material normal entirely) -> Surface Per-Pixel
    Lighting, one clearly-flagged line, because normal-based motion cues are
    otherwise a silent no-op. Real GPU cost, easy to revert alone.
  * A two-scale panning procedural normal ripple (no texture asset exists in
    the project, so this is math nodes, same technique M_Ocean already uses)
    on the pooled vertex factory's existing world-planar UVs, masked to the
    top surface by VertexColor.B.
  * A small (<=1.5 UU) time-based WPO ripple keyed on ABSOLUTE world XY (never
    the wrapped UV -- a wrap boundary inside a brick would tear adjacent
    quads' shared vertices apart), ADDED to the existing fill-drop WPO and
    masked by VertexColor.B the same way.
  * Roughness/specular tightened slightly now that the normal actually
    reaches the lit result, so the moving normal reads as a moving glint.

NIGHT WATER (2026-08-11): the lake had no moon path and no reflection at all
after dark, and it was two separate causes that had to be fixed in two places.

  * IN C++, and this was the larger one: both directional lights sat at
    ForwardShadingPriority 0, so the renderer chose the single forward /
    translucent directional light by raw brightness and gave it to the SUN at
    midnight (LightGridInjection.cpp:1500-1520; the tiebreak runs BEFORE
    atmosphere transmittance is applied, so a sun below the horizon still
    competes at full strength). This material is BLEND_TRANSLUCENT with
    TLM_SURFACE_PER_PIXEL_LIGHTING, so all of its direct lighting comes from that
    one light -- which at night was 37 degrees underground, N.L clamped to zero,
    no direct light on the lake from either body. Fixed in
    UVoxelSkySubsystem::ApplyLightsFromState; nothing in this file could have.
    That same defect is what raised the editor's "multiple directional lights are
    competing to be the single one used for forward shading, translucent, water
    or volumetric fog" warning.
  * IN THIS FILE, two additions, both driven by the new MPC scalar
    MoonLightFraction (= moon light / sun light, written every frame by
    ApplySkyMaterialParams): an analytic MOON GLINT mirroring the sun glint node
    for node, and a NIGHT branch on the sky reflection. The second matters more
    than it sounds -- the reflection was gated by saturate(SunDirection.z), which
    is exactly zero from dusk to dawn, so the Fresnel sheen this file calls "what
    the eye reads as a liquid surface" switched off every night.

Both are scaled by the one scalar, so they vanish together at moonset and at new
moon and both track voxel.Sky.MoonIntensity. Neither reads scene colour, adds a
planar reflection, or adds an SSR probe; the standing ban above is untouched.

NEW DEPENDENCY, AND IT SHARPENS THE ORDERING RULE: this material now binds THREE
MPC_VoxelSky parameters (SunDirection, MoonDirection, MoonLightFraction) instead
of one. MoonLightFraction did not exist before 2026-08-11, so running this script
against an MPC authored by an older create_sky_material.py now RAISES, by name,
in collection_param() -- see that helper for why the check was added here at all
(it is the one binding read-back the sky generators had and this one did not).

=============================================================================
SINGLE LAYER WATER PORT (2026-08-11, Phase 1 of the lake plan)
=============================================================================

THE SHADING MODEL CHANGED, AND WITH IT THE BLEND MODE. This material was
MSM_DefaultLit + BLEND_TRANSLUCENT + TLM_SURFACE_PER_PIXEL_LIGHTING. It is now
MSM_SingleLayerWater + BLEND_OPAQUE. That is not a tweak, it is a different
renderer pass, and three consequences follow that the rest of this file has been
rewritten around:

  * OPACITY-AS-ALPHA IS GONE, and had to be. The engine refuses to compile a
    translucent Single Layer Water material outright -- MaterialShared.cpp:6425,
    "SingleLayerWater materials must be opaque or masked". So every opacity term
    this file used to author (shallow 0.42 / deep 0.98, the Fresnel opacity
    lerp, foam's 0.95) is deleted rather than re-tuned. What replaces them is
    not an approximation of them: the SLW pass reads the scene colour and depth
    of everything BEHIND the water and applies real per-channel Beer-Lambert
    transmittance to it, so "shallow water you can see the bed through, deep
    water you cannot" now falls out of the absorption coefficients instead of
    being authored twice (once as a colour, once as an alpha) and kept in sync
    by hand.
    THE PIN, HOWEVER, IS STILL LIVE AND STILL MANDATORY. MP_Opacity is active on
    any MSM_SingleLayerWater material whatever its blend mode
    (Material.cpp, IsPropertyActive_Internal), where it means
    BaseMaterialCoverageOverWater -- how much of the pixel is the opaque layer
    sitting ON the water. Its unwired default is 1, which sets
    WaterVisibility = 0 and makes the engine skip the volume entirely: no
    absorption, no scattering, no scene behind the water, at any depth. Leaving
    it empty was this file's bug for the whole of Phase 1-3 and produced exactly
    the "flat 2D water, cannot see the bed anywhere" report. It is now wired to
    the foam coverage.
  * THE SORT KEY IS GONE TOO, and this is the constraint that dissolves rather
    than moves. Everything above about translucent draw order -- the (1-a)^N
    argument, the 51.2 m bucketing, the standing ban on reading scene colour --
    was written for SORTED TRANSLUCENCY (docs/gpu-water-pool-design.md:145-157).
    SLW is not sorted: it renders in its own pass after deferred lighting
    (EMeshPass::SingleLayerWaterPass), writes depth in its own prepass, and is
    excluded from the opaque base pass entirely
    (MaterialShared.h:3717 ShouldIncludeMaterialInDefaultOpaquePass). Two
    overlapping water surfaces at one pixel are now resolved by the DEPTH TEST,
    not by a blend order, so the nearest one wins and the question the buckets
    existed to answer no longer has a wrong answer available.
    The bucketing itself is harmless and is left alone; it is now a draw-call
    granularity decision rather than a correctness one.
  * SCENE COLOUR IS READ, BY THE ENGINE, AND THAT IS ALLOWED NOW. The ban was
    scoped to translucent materials whose read IS the partially composed
    translucent stack. The SLW pass reads SceneColorWithoutSingleLayerWater --
    a copy taken BEFORE any water drew -- so every water fragment reads the same
    texture whatever else has been drawn. Same argument the star reflection
    already makes for a texture fetch, now applying to the whole volume term.

UNITS ARE 1/cm. THIS IS THE SINGLE EASIEST WAY TO BE 100x WRONG HERE.
Epic's public documentation for the Single Layer Water output node says the
coefficients are per METRE. The documentation is wrong. The engine header says
so in as many words -- MaterialExpressionSingleLayerWaterMaterialOutput.h:16,
"Valid range is [0,+inf[. Unit is 1/cm." -- and the shader agrees where it
matters: SingleLayerWaterShading.ush computes
`OpticalDepth = ExtinctionCoeff * WaterVolumeDepth`, and WaterVolumeDepth is a
difference of two SCENE DEPTHS, which are in unreal units, i.e. centimetres.
Every published absorption figure in the literature is per metre. Multiply by
0.01. There is exactly ONE node in this file that does that conversion
(`per_cm`, below) and both coefficient chains go through it, so the mistake can
only be made once and is visible in one place.

THE LOOK IS DIALLED BY THE RED-TO-BLUE ABSORPTION RATIO, not by a tint.
The old material had a shallow tint and a deep tint and lerped between them on a
single scalar Beer-Lambert term, which is a 1:1 absorption ratio wearing a
costume: every channel died at the same rate, so deep water went DARK instead of
going BLUE, and the only way to get blue was to author it. Real water is about
129:1 red-to-blue. Near-realistic shader work sits around 26:1; a deliberately
stylised one (Photon) sits around 5.6:1. The owner's brief is "clear and blue,
stylised", so the shipped default is ~11:1 -- absorption R 0.45 / G 0.10 /
B 0.04 per metre. Red is at 16% of its surface value through 2 m of water seen
from above (the view path and the light path are both attenuated, so the depth
counts roughly twice); blue is still at 85% there and at 37% through 25 m. That
is the whole "clear and blue" mechanism, and it is four numbers.

THE KNOB IS AN ABSORPTION DISTANCE, NOT A COEFFICIENT, because nobody can
picture 0.0045 1/cm. Unity HDRP's parameterisation is taken verbatim: the artist
sets the depth at which 2% of light survives, and the coefficient is derived as
`(-ln(0.02) / distance) * (1 - colour)`. So AbsorptionDistanceM is "how deep
before it goes dark" and WaterAbsorptionColor is "what colour survives longest"
-- 0 in a channel means that channel is fully absorbed at that distance, 1 means
it is not absorbed at all. The shipped pair (8.7 m, RGB 0/0.778/0.911) evaluates
to exactly the four numbers above; that is a check anyone can redo with a
calculator, which is the point of deriving them rather than typing them.

THOSE TWO SECTIONS ARE THE 2026-08-11 STATE AND THE PAIR THEY QUOTE IS
SUPERSEDED: the owner could still see the lake bed from the middle of the lake,
so on 2026-08-12 it went to 3.5 m and RGB (0, 0.30, 0.38) with the scattering
raised to match. The reasoning above is kept because it is still the reasoning
for the mechanism; the numbers are at the parameter sites, which is also no
longer where they are typed.

WHERE THE FOUR OPTICAL CONSTANTS LIVE NOW: Tools/water_optics.py, because
Tools/create_underwater_material.py is a second renderer of the same water --
a post-process Beer-Lambert pass with no access to the SLW node -- and for one
commit the two disagreed, so the pond was dark blue-green from the bank and the
old teal once you were in it. Absorption distance, absorption colour, scattering
and phase g are imported from there; every other number in this file, and every
word of the reasoning at those four sites, stays here. See the import block
below for why that is a module and not a second Material Parameter Collection.
Both generators print water_optics.summary_lines() into their log, so "were
these two built from the same numbers" is answered by the logs rather than by
opening two assets.

REFLECTION MODE 2 IS A FIRST-CLASS MODE, NOT A DEGRADED FALLBACK.
Config/DefaultEngine.ini now sets r.Water.SingleLayer.Reflection=2 (reflection
captures + skylight only). SLW has never used planar reflections, so nothing is
being given up relative to how this project already renders water. Mode 2 also
turns Lumen reflections off on water -- which costs nothing HERE because voxel
terrain has no mesh distance fields, so Lumen has no geometry to trace against
and was never available. The trade is real on a project with distance fields and
is stated so nobody re-derives it later.

WHAT WAS KEPT AND PORTED RATHER THAN REBUILT: the foam (slope + CA activity,
unchanged), the sun glint, the moon glint, the MoonLightFraction night-sky
branch (the fix for the reflection being gated on saturate(SunDirection.z),
which is zero all night), and the star reflection arm. Those now land on
BaseColor and EmissiveColor of an opaque surface instead of on a translucent
one, which changes nothing about what they compute.

ONE THING TO WATCH, STATED BECAUSE IT IS NOT MEASURED: an SLW surface receives
the engine's own reflection-capture/skylight specular, and this material also
adds a hand-authored Fresnel sky reflection on Emissive. Those may double up.
The hand-authored one is owner-tuned and is therefore kept as the default, but
it is now multiplied by LegacySkyReflectGain (scalar, default 0.0 since the
owner retired it on 2026-08-12 -- see the measurements at the node) so it can be
dialled to 0 from a MATERIAL INSTANCE, with no regeneration, if the engine's
reflection turns out to be enough on its own.

=============================================================================
THE REPEATING TILE (2026-08-11, Phase 5) -- TWO CAUSES, BOTH FIXED
=============================================================================

The owner's first complaint is that the lake "looks like a repeating tile". It
did, for two independent reasons, and fixing either alone would have left it.

CAUSE A, A 32 m UV WRAP WITH A MIRROR IN IT. The ripple read
TextureCoordinate(0), which the pooled vertex factory fills with
`fmod(Position/100, 32)` (VoxelQuadVertexFactory.ush). Two things wrong with
that as a wave-field coordinate. It repeats every 32 m, full stop -- better wave
math on top of a 32 m coordinate still repeats every 32 m. And HLSL `fmod`
carries the sign of its argument, so the coordinate is an ODD function through
world x=0 and y=0, which makes the whole wave field mirror-symmetric about both
world axes.

  THE FIX IS TO STOP READING THAT UV AT ALL, and it buys a second thing for
  free. The wave field is now keyed on ABSOLUTE WORLD XY -- the same key, and
  the same WPT_ExcludeAllShaderOffsets node, the WPO ripple has always used.
  That removes the 32 m period and the mirror together, and it also makes the
  NEAR-FIELD VOXELS AND THE FAR-FIELD SHEETS SHARE ONE CONTINUOUS WAVE FIELD
  for the first time. They did not before, and not by a small amount:
  AVoxelWaterSheetActor::AppendRectQuad anchors each sheet's UV at that sheet's
  own bounding-box corner (`UVOrigin = (Sheet.MinXUU, Sheet.MinYUU)`) and does
  not wrap at all, so a sheet's wave phase had no relationship to the voxel
  water it butts against. That file's own comment calls the mismatch invisible
  on the grounds that the voxel path's 32 m wrap made the phase discontinuous
  anyway -- true, and it is also a description of the "bad/harsh transitions
  between the near water and far lake water" the owner reported. World XY is
  the only key both paths can agree on: the sheets CANNOT adopt a wrapped UV,
  because a sheet rectangle can be a kilometre long and a wrap boundary falling
  inside one quad would smear the entire period across it.

  PRECISION, since the wrap existed to protect it. This world sits ~84 km from
  the origin, so float32 world position is granular at about 1 cm there. The
  finest octave below has a 1.4 m wavelength, where 1 cm is 0.05 rad of phase --
  invisible. It is also ten times FINER than the voxel quantisation knob's
  default 10 cm grid, so the float grid is never the visible one. At 2000 km
  out (the figure create_voxel_material.py's UV note quotes) the float grid
  would be ~24 cm and would become the visible quantum; the failure mode is a
  coarser stylised step, not a seam or a tear, because the field is a pure
  function of position and adjacent quads sharing a vertex still get identical
  answers.

CAUSE B, FOUR AXIS-ALIGNED SINES, TWO OF THEM EXACTLY COMMENSURATE. The old
ripple was `sin(1.8u)+sin(9.0u)` on one axis and `sin(1.6v)+sin(8.3v)` on the
other. 9.0/1.8 = 5.000 exactly and the two speeds 0.90/0.30 = 3.000 exactly, and
both terms sum into the SAME normal component -- so that component repeated
exactly every 3.49 m and every 20.94 s. Every wavefront was also parallel to a
world axis, and the maximum surface tilt the whole thing could produce was about
4 degrees.

THE REPLACEMENT, and each element is here because a shipped source uses it:

  * ROTATE THE WAVE DIRECTION PER OCTAVE. This is the single most effective fix
    and the one most often skipped. Mojang exposes it in Vibrant Visuals as
    `direction_increment` and ships 80 degrees over 28 octaves; Photon uses the
    golden angle. Default here is the golden angle (137.507764 deg), exposed as
    WaveDirIncrementDeg, with a non-zero STARTING angle (WaveDirBaseDeg 58.7)
    that exists because a starting angle of zero puts the longest, heaviest
    octave exactly on world +Y -- the defect this section removes, reintroduced
    by a default. 58.7 is the sweep optimum: it is the base angle that maximises
    the smallest distance between any octave's wavefront and a world axis, and
    it gets that distance to 16.2 degrees. Measured
    as a histogram of the surface gradient's direction in 10-degree bins, the
    old ripple ran from 1.2% to 8.7% per bin -- strongly bimodal, with two
    near-empty directions -- and this field runs 5.1% to 6.2%, i.e. very close
    to isotropic.
  * NON-COMMENSURATE PERIODS. Frequency multiplier 1.42 per octave, time
    multiplier 1.07 -- irrational-ish ratios rather than the old exact 5 and 3.
    Measured over a 400 m slice: the old normal's X component has a
    self-similarity of 0.987 at 3.52 m, i.e. it is an exact repeat; the new
    field's best is 0.58 at 9.2 m, which is a family resemblance and not a tile.
  * A VERY-LOW-FREQUENCY AMPLITUDE FIELD, so the lake has calm patches and
    choppy patches instead of uniform corduroy. Two diagonal sines at ~180 m
    and ~232 m, which are themselves non-commensurate and not axis-aligned.
  * OCTAVES COMBINED AS SURFACE GRADIENTS, not as blended normals. The old code
    already had this property by accident (it summed tangent XY with Z pinned to
    1.0); it is now the explicit design -- the wave function returns dH/dx and
    dH/dy and those are summed, and the normal is assembled once at the end.
  * A BETTER WAVE FUNCTION. afl_ext's "Very fast procedural ocean"
    (shadertoy.com/view/MdXyzX). Two properties earn its place: the crest shape
    is `exp(sin(x) - 1) * 0.5` rather than a plain sine, which is asymmetric and
    sharper at the crest and therefore carries harmonics a sine cannot, and the
    per-octave DRAG term (`position += direction * dwave * weight * 0.2`)
    domain-warps each octave by the one before it, so the octaves pull on each
    other instead of cleanly superimposing. Clean superposition is what makes a
    sum of sines read as a grid.

    LICENCE, and read this before touching the wave loop. The instruction for
    this work was to verify the licence at shadertoy.com/view/MdXyzX directly.
    THAT VERIFICATION FAILED: shadertoy.com returns HTTP 403 to this box, so the
    original page's licence banner could not be read first-hand. What COULD be
    read is a third-party carrier, jbritain/glimmer-shaders
    shaders/lib/water/waveNormals.glsl, whose header attributes the technique to
    afl_ext and links https://opensource.org/license/mit. That is a second-hand
    assertion, not the primary source, so NO CODE WAS COPIED FROM EITHER. The
    loop below is written here from the described mathematics -- the exp-sine
    crest, the drag warp, the geometric frequency/weight/time progressions --
    in HLSL against a UE Custom node, which shares no lines with the GLSL. The
    attribution stands as attribution; the licence position is UNVERIFIED and is
    recorded as such deliberately rather than repeated as fact.

  * THE VOXEL-STYLE KNOB the owner asked for ("slightly stylized given that it
    is a voxel world"): the sample position is quantised to the voxel grid
    before the height field is evaluated, `p = floor(p * k) / k`. Glimmer does
    this behind PIXEL_LOCKED_LIGHTING and Rethinking Voxels does the same. It is
    one line and free, it makes the ripple read as belonging to a voxel world,
    and it is also the cheapest anti-aliasing available here -- a field with no
    spatial frequencies above the voxel grid has nothing left to alias.
    WaveQuantPerVoxel is 1.0 by default (one sample per 10 cm voxel); 2 and 4
    are finer, 0 turns quantisation off. Note the pleasing accident: water quad
    VERTICES already sit on the 10 cm grid, so at k=1 the quantisation is a
    no-op for the vertex displacement and only stylises the pixel normal. The
    geometry does not acquire a staircase from this knob until k > 1.

  * THE WPO WAVE NOW HAS A NORMAL, which is the answer to "derive it or drop
    it". It was dropped-in-effect before: the vertex ripple moved geometry that
    nothing in the shading knew about, so the bob was invisible except in
    silhouette. Both now come out of ONE evaluation of ONE field -- the Custom
    node returns float3(dH/dx, dH/dy, H), the normal takes the first two and the
    displacement takes the third. Same phase, same crests, same drift. They do
    differ by a deliberate SCALAR: the shading uses the full wave amplitude
    (WaveAmplitudeM 0.25, an ~8.6 cm field) and the displacement uses a fraction
    of it (WaveWpoFraction 0.25, so ~2 cm). That is not an inconsistency to be fixed
    later, it is the point -- the geometric term is bounded by the voxel grid
    and by water clipping through its own banks, and the shading term is bounded
    by nothing, so pinning them to one number would mean either an invisible
    glint or water sloshing over the shore.

  * GLINT: A REPRESENTATIVE-POINT AREA LIGHT, the approximation Sea of Thieves
    ships. The old glint was `pow(saturate(dot(R, L)), 900)` against a surface
    whose maximum tilt was 4 degrees, which is a nearly point-sized dot -- not
    the long streak the moon-glint comment above assumes. Treating the light as
    a SPHERE and shifting L to the closest point on that sphere to the mirror
    ray gives the highlight a flat-topped core of the light's real angular size,
    with Karis's `(a/(a+sinAlpha))^2` normalisation so widening the source does
    not invent energy. The sun gets 0.55 deg (its real ~0.53). The moon gets
    2.4 deg, which is deliberately the project's ENLARGED drawn disc
    (kMoonDrawnAngularRadiusDeg, ~9x life size) rather than its real 0.5 -- the
    path on the water and the disc in the sky should be the same object, and
    the disc is the one the owner sees. The long streak itself comes from the
    wave slope distribution, which is what the new wave field is for; the
    representative point only fixes the CORE of the highlight.

=============================================================================
WHERE THE WAVE FIELD LIVES NOW (2026-08-12) -- AND IT IS NO LONGER INERT
=============================================================================

THE PHASE 5 REASONING ABOVE IS STILL THE REASONING. The octave count, the 1.42
frequency ratio, the golden-angle direction increment, the 58.7-degree sweep
optimum, the voxel quantisation knob and the drag warp are all unchanged and all
still argued where they are. What moved is the CODE, and what changed is the
DEFAULTS.

  * THE WAVE FIELD IS NOW Tools/water_wave_graph.py, design note at
    docs/water-wind-waves.md. The 110 lines of inline HLSL and the six scalar
    parameters that used to sit down at the normal/WPO assembly are gone from
    this file; the module authors the same six parameters under the same names
    with the same defaults, so an existing material instance that overrides
    WaveAmplitudeM keeps working. It adds three things: the wind STEERS the
    octaves, the wind SIZES them, and the wave BREAKS where the bed comes up
    (a fourth output, wired into the foam composite).
  * THE INTERACTIVE RIPPLE IS Tools/ripple_field_graph.py, design note at
    docs/water-interactive-ripples.md. A camera-following render target of rings
    spreading from things that enter the water, summed into the SAME gradient
    and the SAME height as the wave field, at one site, upstream of everything.

ONE PARAMETER NAME DISAPPEARED: BathyWaveDampDepthM. Its replacement is
BreakSurfFloorM -- same meaning (the depth over which a wave dies as the bed
comes up), same units, and it now also sets where the wave BREAKS, because a
real surf zone runs from the break point to the waterline and those are one
band. An instance override of the old name silently stops doing anything.

AND THE HEADLINE, STATED IN THE DOCSTRING BECAUSE IT IS A CHANGE TO A SIGNED-OFF
PICTURE: the wind feature ships ACTIVE, on water_wave_graph.DEFAULTS. The lake
this builds is NOT the lake the owner signed off on 2026-08-12. The octaves are
steered into a cone about the wind instead of spread over the whole circle,
about four times as much wave reaches the beach, and there is a white surf band
at every shoreline where the bed shelves. That is deliberate and it is the point
of the change -- the owner asked to play-test wind-driven waves at different
wind speeds. VOXEL_WATER_LEGACY_WAVES=1 rebuilds the previous field exactly; see
that arm's note below for the A/B.

Run via:
  UnrealEditor-Cmd.exe <uproject> -run=pythonscript -script=<this file> -unattended -nop4 -nosplash

MUST BE RUN THIRD, after create_sky_material.py and
create_sky_atmosphere_dome_material.py, EVERY TIME either of those runs. The
reason and the cost of getting it wrong are at the top of create_sky_material.py.
"""

import os
import sys
import time

import unreal

# The star subgraph is SHARED, not re-derived. Tools/sky_star_graph.py owns the
# one horizon->equatorial rotation, the one equirect UV and the one seam fix, and
# its module docstring says at length why a second copy is the failure mode to
# fear: two samplers of the same map that disagree produce a sky that is still
# sharp, still rotating at the right rate, and still wrong, with nothing in a
# frame to say so. A reflection of the stars that drifted from the stars would be
# exactly that defect, and the water is the surface most likely to show it (the
# real sky and its mirror image are in the same frame, a few hundred pixels
# apart).
#
# A -run=pythonscript commandlet does not put the script's own directory on
# sys.path, hence the explicit insert -- same as create_sky_material.py:417-420.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sky_star_graph import (  # noqa: E402
    SkyGraphBuilder,
    build_horizon_fade,
    build_star_uv,
    sample_starmap,
)
from bathy_field_graph import build_slant_depth, sample_bathy_field  # noqa: E402

# THE OTHER TWO SHARED SUBGRAPHS, same directory, same sys.path.insert above.
#
# water_wave_graph is imported AS A MODULE rather than by name because four of
# its attributes are read here -- build_wave_field, summary_lines, DEFAULTS and
# LEGACY_RECIPE -- and two of those are the parameter tables the wind arm
# switches between. Importing the tables by name would let this file hold a
# stale copy of a default the module has since moved, which is the exact
# two-copies-of-one-derivation failure both module docstrings are about.
#
# ripple_field_graph is imported by name because there is one entry point and it
# returns everything.
from ripple_field_graph import sample_ripple_field  # noqa: E402
import ripple_field_graph  # noqa: E402
import water_wave_graph  # noqa: E402

# THE FOUR OPTICAL CONSTANTS ARE NOT TYPED IN THIS FILE ANY MORE, and the reason
# is that this is no longer the only renderer that has to know them.
# Tools/create_underwater_material.py (2026-08-12) is a post-process material
# doing its own Beer-Lambert over the depth buffer; it cannot see the Single
# Layer Water node this file wires, so the ONLY thing that can keep the surface
# and the swim looking like the same liquid is both scripts reading one set of
# numbers. water_optics.py's module docstring records the commit where they
# disagreed and what it looked like (a dark blue-green pond that turned teal the
# moment you submerged).
#
# WHAT MOVED AND WHAT DID NOT. Four values: AbsorptionDistanceM,
# WaterAbsorptionColor, ScatteringPerMetre, WaterPhaseG. Those describe THE
# WATER. Everything else at those four sites -- the parameter names, the node
# positions, the derivation chain, and every word of the reasoning -- stayed
# here, because it is this material's business and not the shared module's. Foam,
# glints, the sky reflection and the wave field are likewise untouched.
#
# NOT AN MPC, DELIBERATELY, and this file is the reason: a Material Parameter
# Collection is the engine's own answer to "two materials, one set of numbers",
# and create_sky_material.py DELETES and recreates MPC_VoxelSky every run, which
# on 2026-08-10 left every dependent binding compiling to the ENGINE DEFAULT
# MATERIAL while the log said success. A Python import cannot fail that way: a
# missing module means this script does not run at all.
#
# THE COST, STATED SO NOBODY REDISCOVERS IT FROM A CONFUSING SCREENSHOT: the two
# materials still expose these as SEPARATE material parameters, so overriding
# AbsorptionDistanceM on an instance of M_WaterVoxel does NOT move M_Underwater
# with it. Instance overrides are for experiments; the shipped values come from
# water_optics.py plus a regeneration of both materials.
import water_optics  # noqa: E402

PACKAGE_PATH = "/Game/Voxel"
MATERIAL_NAME = "M_WaterVoxel"
FULL_PATH = PACKAGE_PATH + "/" + MATERIAL_NAME


# STAR REFLECTION: BUILT OR NOT BUILT, AND NOTHING IN BETWEEN.
#
# This exists so the feature's GPU cost can be measured honestly from ONE binary.
# The two arms are two runs of this script with the environment variable flipped,
# which produces two genuinely different shaders from the same C++ build -- the
# only comparison that isolates the material.
#
# WHY NOT A MATERIAL SWITCH OR A ZERO MULTIPLY. Multiplying the star term by a
# parameter set to 0 measures nothing: the texture fetch, the atan2, the arcsine
# and the two derivative corrections all still execute, and the compiler cannot
# fold them away because the parameter is a uniform it must assume can change.
# A StaticSwitchParameter would work, but it makes the OFF arm a different
# permutation of the SAME material asset, and this project has already been
# burned once by a water material that was silently not the material anyone
# thought it was (see the 2026-08-10 default-material failure in the module
# docstring). Two full regenerations, each logging which arm it built, leaves no
# room for that.
#
# Default is ON: once the measurement is in, the shipped asset is the one the
# owner decides to keep, and an unset variable should build the full material.
STAR_REFLECTION_ENV = "VOXEL_WATER_STAR_REFLECT"
STAR_REFLECTION = os.environ.get(STAR_REFLECTION_ENV, "1").strip().lower() not in (
    "0", "off", "false", "no", "")

# --- THE FROZEN-RIPPLE MEASUREMENT ARM ---------------------------------------
#
# VOXEL_WATER_FREEZE_TIME=1 replaces the single MaterialExpressionTime that
# drives the wave field with a CONSTANT. Everything else about the graph is
# untouched: the same eight octaves, the same frequencies, directions and
# amplitudes, the same wiring, the same instruction count to within the folding
# the compiler does on a constant phase.
#
# SINCE THE WAVE REWORK THIS ARM IS STRICTLY STRONGER THAN IT WAS. There used to
# be two independent ripple systems (a pixel normal ripple and a vertex WPO
# ripple) that merely happened to share this node, so "frozen" meant "both of
# the two things I remembered to wire to it". There is now ONE field feeding both
# the normal and the displacement, so freezing this node provably freezes every
# animated term on the surface.
#
# WHY IT EXISTS. Water flicker is measured by taking N frames from one frozen
# pose and diffing consecutive pairs. That measurement cannot tell ANIMATION
# from INSTABILITY: a panning ripple is supposed to change between frames, and
# it lands in the metric identically to a shading term that cannot make up its
# mind. Worse, the burst's true shutter spacing is ~0.43 s (each 2560x1440 PNG
# write stalls the game), so the ripple advances about twenty-six times further
# between two measured frames than it does between two frames the player sees --
# the confound is not merely present, it is AMPLIFIED by the instrument.
#
# With this arm built, any residual inter-frame difference on water is temporal
# instability by construction. This is a MEASUREMENT arm and not a shipping
# one: the default is unset = LIVE, so a normal regeneration is byte-for-byte
# the material that shipped.
FREEZE_TIME_ENV = "VOXEL_WATER_FREEZE_TIME"
FREEZE_RIPPLE_TIME = os.environ.get(FREEZE_TIME_ENV, "0").strip().lower() not in (
    "0", "off", "false", "no", "")

# --- THE SHORELINE-EFFECTS A/B ARM -------------------------------------------
#
# VOXEL_SHORE_FX=0 builds the water WITHOUT shoreline foam. Its partner in
# terrain_material_common.py reads the SAME variable and builds the terrain
# without wet-shore darkening, so one variable moves the whole shoreline
# treatment and the two halves cannot be armed inconsistently.
#
# WHY AN ARM AT ALL. Both effects were built, wired and shipped, and NEITHER has
# ever been confirmed in a screenshot -- they landed during the bathymetry work
# and the captures taken since were all aimed at other questions (the near/far
# seam, the shoreline gap, the murkiness). "It is in the graph" is not the same
# claim as "the owner can see it", and on this project the second claim is the
# only one that counts: the standing rule is that the owner judges appearance
# from captures and the implementer does not deliver a verdict.
#
# WHY GENERATION-TIME AND NOT A RUNTIME PARAMETER, which would be cheaper to
# flip. Because that is this project's answer to exactly this question already,
# and the reasoning is written down at VOXEL_MATERIAL_DEBUG
# (terrain_material_common.py): a runtime switch means a permanent branch in the
# shipping shader, and there is no per-chunk MID plumbing to drive one from a
# command line anyway. Chunk MIDs exist but carry only the ring-fade scalars
# (VoxelChunkComponent.cpp:1408-1411), and the water path has no MID at all --
# AVoxelWaterSheetActor and the pooled quads both bind /Game/Voxel/M_WaterVoxel
# directly. Building the plumbing to make this flippable would be a bigger and
# riskier change than the two regenerations it saves.
#
# DEFAULT IS ON, so an unset variable rebuilds the shipped material.
SHORE_FX_ENV = "VOXEL_SHORE_FX"
SHORE_FX = os.environ.get(SHORE_FX_ENV, "1").strip().lower() not in (
    "0", "off", "false", "no", "")

# --- THE PRE-WIND WAVE FIELD, ONE REGENERATION AWAY --------------------------
#
# THIS FILE NOW SHIPS A DIFFERENT LAKE ON PURPOSE, AND THIS IS THE WAY BACK.
#
# The default arm (unset, or 0) builds water_wave_graph.DEFAULTS: the wind steers
# the octaves, the wind sizes them, and waves break at the shore. That is NOT the
# water the owner signed off on 2026-08-12 and nobody should be able to mistake
# it for an accident -- three things are visibly different and each of them is
# the feature working:
#
#   * WindDirectionAuthority 1.0 (was 0). The eight octaves stop being spread
#     over the whole circle by the golden angle and collapse into a 35-degree
#     cone about the wind, narrow at the long octaves and 46 degrees off it in
#     the tail. The lake acquires a direction. The calm/choppy patch field also
#     starts drifting downwind instead of breathing in place.
#   * BreakSurfFloorM 0.15 m (was 0.6, under the name BathyWaveDampDepthM).
#     Measured in the design note on a 2-D shore at four phases: peak crest
#     inside the surf zone at 5 m/s goes 1.22 cm -> 4.88 cm. Waves reach the
#     beach instead of fading out 60 cm deep.
#   * BreakFoamGain 1.0 and BreakPeakGain 0.6 (both were 0). A white surf band
#     appears where the bed shelves, riding the crests, and the crests inside it
#     pitch forward. That band also raises Opacity, which is saturate(foam) --
#     the same pairing already documented at BathyFoamGain.
#
# WHY IT IS AN ARM AND NOT A ONE-LINE EDIT. water_wave_graph.LEGACY_RECIPE is the
# module's own machine-readable statement of "reproduce the field that shipped",
# verified offline against a transcription of the shipped HLSL to 5.1e-11 m in
# height. Keeping it reachable from the environment means the A/B is two runs of
# one script from one C++ build -- the same argument, and the same shape, as
# VOXEL_WATER_STAR_REFLECT above and VOXEL_SHORE_FX beside it. Setting it takes
# the regeneration; it is not a runtime flip.
#
# WHY A REGENERATION IS EVEN NEEDED FOR THIS ONE, given the whole point below is
# that the WIND itself needs none: the recipe is a set of SCALAR DEFAULTS baked
# into the asset. All five are still material parameters, so an instance
# override reproduces the legacy field without any regeneration at all -- this
# arm exists so the SHIPPED asset can be flipped back without hand-editing five
# numbers, and so the flip is recorded in a log line rather than in someone's
# memory of what they typed into an instance.
#
# DEFAULT IS THE WIND ARM. An unset variable builds the feature the owner asked
# for. That is a deliberate inversion of how VOXEL_SHORE_FX and
# VOXEL_WATER_STAR_REFLECT default (both "build what shipped"), because here
# what shipped is the thing being replaced.
LEGACY_WAVES_ENV = "VOXEL_WATER_LEGACY_WAVES"
LEGACY_WAVES = os.environ.get(LEGACY_WAVES_ENV, "0").strip().lower() not in (
    "0", "off", "false", "no", "")


def main():
    # --- SAY WHICH NUMBERS THIS BUILD WAS MADE FROM, BEFORE MAKING ANYTHING ---
    #
    # create_underwater_material.py prints the same block from the same module.
    # That is the point: when the surface and the swim disagree ON SCREEN, the
    # first question is whether they were built from the same constants, and two
    # logs answering it identically settles that in seconds without opening
    # either .uasset -- which is the only way to check an asset's baked defaults
    # otherwise, and needs an editor this project usually has occupied.
    #
    # PRINTED FIRST, BEFORE THE ASSET IS EVEN CREATED, so that a run which dies
    # later still records what it was trying to build. The same discipline as the
    # arm read-backs at the bottom of this function, one step earlier: a log that
    # only reports on success cannot describe a failure.
    #
    # It is a DERIVATION, not an echo -- absorption per metre, extinction, the
    # deep-water albedo and a transmittance table all computed by water_optics
    # from the four constants. So the log also catches the 100x unit error this
    # file warns about at `per_cm`: everything in these lines is per METRE, and a
    # transmittance of 0.33 at 1 m is a number a human can sanity-check.
    for _line in water_optics.summary_lines():
        unreal.log("M_WaterVoxel " + _line)

    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

    if unreal.EditorAssetLibrary.does_asset_exist(FULL_PATH):
        unreal.EditorAssetLibrary.delete_asset(FULL_PATH)

    factory = unreal.MaterialFactoryNew()
    material = asset_tools.create_asset(MATERIAL_NAME, PACKAGE_PATH, unreal.Material, factory)
    if material is None:
        raise RuntimeError("Failed to create material asset at " + FULL_PATH)

    # --- SINGLE LAYER WATER: OPAQUE, TWO-SIDED, ONE SHADING MODEL ----------
    #
    # BLEND_OPAQUE, and there is no choice about it. The engine rejects a
    # translucent SLW material by name -- MaterialShared.cpp:6425,
    # "SingleLayerWater materials must be opaque or masked" -- and it rejects a
    # mixed shading model too (:6427-6429). Opaque here does NOT mean the water
    # is opaque: an SLW surface writes depth and GBuffer like any opaque
    # surface, and then its own pass composites the scene behind it through the
    # absorption/scattering coefficients wired below. "See the bottom in the
    # shallows" is now a physical result of those coefficients rather than an
    # authored alpha.
    #
    # What this deletes, permanently: the whole translucent sort-key problem.
    # SLW has no sort key. Two overlapping water surfaces at one pixel are
    # resolved by the depth test in the SLW depth prepass, so the nearest wins
    # and there is no ordering left to get wrong.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)

    # TWO-SIDED IS UNCHANGED AND IS STILL LOAD-BEARING. A meshed water brick's
    # faces can be viewed from inside a flooding cavity before the player has
    # line of sight to its "outside" face -- the same reasoning M_Ocean's
    # two-sided flag documents. Note this survives the port intact: the SLW pass
    # takes the material's cull mode from the same ComputeMeshCullMode path
    # every other pass uses (SingleLayerWaterRendering.cpp
    # FSingleLayerWaterPassMeshProcessor::TryAddMeshBatch), so a two-sided SLW
    # material is not a special case anywhere.
    material.set_editor_property("two_sided", True)

    # THE SHADING MODEL IS DELIBERATELY *NOT* SET HERE. It is set further down,
    # in the block headed "AND ONLY NOW IS IT A SINGLE LAYER WATER MATERIAL",
    # immediately after the SingleLayerWaterMaterialOutput node exists and is
    # wired -- and the ordering is the whole point.
    #
    # WHY, MEASURED RATHER THAN GUESSED (2026-08-11). Setting it here produces a
    # correct asset -- and emits SEVENTEEN of these into the run log first:
    #
    #   LogMaterial: Warning: [AssetLog] ...M_WaterVoxel.uasset: Failed to
    #   compile Material for platform PCD3D_SM6, Default Material will be used
    #   in game.
    #       SingleLayerWater materials requires the use of
    #       SingleLayerWaterMaterial output node.
    #
    # one per shader map, because each set_editor_property fires
    # PostEditChangeProperty, which recompiles a material that at that moment has
    # no graph at all. They are transient and the final compile is clean.
    #
    # THEY ARE ALSO WORD-FOR-WORD THE STRING THIS PROJECT'S RELEASE RULE GREPS
    # FOR. "Grep every run log for 'Failed to compile Material' before trusting
    # any visual result" is the check that stands between a code change and a
    # water pool silently drawn with the DEFAULT MATERIAL -- which happened here
    # on 2026-08-10 and was diagnosed from the owner saying he could not see any
    # lake basins. A generator that emits that string on every successful run
    # destroys that check by making it always fire, and tools/
    # voxel-water-star-regen.ps1 correctly refuses the whole regeneration when it
    # sees them. So the property is moved rather than the guard weakened.

    blend_after = material.get_editor_property("blend_mode")
    if blend_after != unreal.BlendMode.BLEND_OPAQUE:
        raise RuntimeError(
            "blend mode did not take: %r, expected BLEND_OPAQUE. A translucent SingleLayerWater "
            "material does not compile at all (MaterialShared.cpp:6425)." % (blend_after,))

    # NOTE ON THE LINE THAT IS NO LONGER HERE: translucency_lighting_mode was
    # set to TLM_SURFACE_PER_PIXEL_LIGHTING, and it had to be, because the
    # engine's default translucency lighting mode ignores the material normal
    # outright (EngineTypes.h's own doc comment on TLM_VolumetricNonDirectional:
    # "the material normal is not taken into account"), which would have made
    # the whole panning ripple a silent no-op. That property applies only to
    # translucent materials and is now inert; setting it on an opaque material
    # is not an error but it is a lie in the asset, so it is removed. An SLW
    # surface is shaded per-pixel with a full normal by construction -- the
    # thing the old line was buying is now free.

    mel = unreal.MaterialEditingLibrary

    # --- Custom-node helper -------------------------------------------------
    #
    # WHY THERE ARE CUSTOM (HLSL) NODES IN A FILE THAT IS OTHERWISE ALL CHECKED
    # PIN CONNECTIONS. Two of the things below are loops: an eight-octave wave
    # sum with a domain warp that feeds each octave's output into the next
    # octave's input, and a closest-point-on-sphere solve. Expressed as material
    # nodes those are roughly 130 and 20 nodes respectively, hand-wired, with
    # every intermediate needing its own checked connect. The node graph's whole
    # advantage -- that a wrong pin raises at authoring time -- inverts at that
    # size: nothing in 130 correct-looking connects tells you the drag term went
    # into the wrong octave. The HLSL is fifteen lines and reads as the
    # mathematics it is.
    #
    # The BOUNDARY is still checked the same way: every input to a Custom node
    # is wired with the same raise-on-failure connect as everything else, and
    # the input NAMES are what the HLSL reads, so a rename that misses one end
    # fails to compile loudly rather than reading a stale value.
    def custom_node(name, code, inputs, x, y, output_type=None):
        """A MaterialExpressionCustom with named inputs and no wiring yet.

        `inputs` is a list of input names; the caller connects them by name
        afterwards with the usual checked mel.connect_material_expressions.
        Returns the node.
        """
        node = mel.create_material_expression(material, unreal.MaterialExpressionCustom, x, y)
        node.set_editor_property("description", name)
        node.set_editor_property("code", code)
        if output_type is not None:
            node.set_editor_property("output_type", output_type)
        # FCustomInput carries an FName and an FExpressionInput. Only the name is
        # settable from Python; the FExpressionInput is filled in by the connect
        # calls that follow, which is why the array is built name-only here.
        ins = []
        for nm in inputs:
            ci = unreal.CustomInput()
            ci.set_editor_property("input_name", nm)
            ins.append(ci)
        node.set_editor_property("inputs", ins)
        return node

    def scalar_param(name, default, x, y):
        """A named ScalarParameter, so it is tunable from a material instance
        without regenerating the asset. Every number the owner is likely to
        want to move goes through one of these."""
        node = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, x, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", default)
        return node

    def vector_param(name, r, g, b, x, y):
        node = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter, x, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", unreal.LinearColor(r, g, b, 1.0))
        return node

    def rgb(node, x, y):
        """Mask a VectorParameter's unused .a off. FLinearColor forces a fourth
        component on every vector parameter; feeding a float4 into a float3
        coefficient pin is the kind of thing that compiles and then means
        something slightly different."""
        m = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, x, y)
        m.set_editor_property("r", True)
        m.set_editor_property("g", True)
        m.set_editor_property("b", True)
        m.set_editor_property("a", False)
        if not mel.connect_material_expressions(node, "", m, ""):
            raise RuntimeError("connect vector param -> rgb mask failed")
        return m

    vertex_color = mel.create_material_expression(material, unreal.MaterialExpressionVertexColor, -500, -100)

    # ======================================================================
    # PHASE 3: THE BAKED BATHYMETRY FIELD
    # ======================================================================
    #
    # Sampled ONCE, here, before anything that uses it. Four consumers below:
    #
    #   1. depth-graded colour   -- the baked depth, slant-corrected, drives the
    #                               absorption of everything behind the water
    #   2. shoreline foam        -- from the SIGNED distance, gated by bed slope
    #   3. wave damping          -- amplitude to zero as the bed comes up
    #   (4. wet shores are the TERRAIN material's, not this one's)
    #
    # The collection is loaded HERE rather than at the sun glint further down,
    # because this is now the first thing in the file that binds to it. The glint
    # block reuses this object; there must not be two loads.
    sky_collection = unreal.load_object(None, "/Game/Voxel/MPC_VoxelSky.MPC_VoxelSky")
    if sky_collection is None:
        raise RuntimeError(
            "MPC_VoxelSky not found -- this material binds to it for the sun glint, the moon "
            "glint and (since Phase 3) the bathymetry window's placement. Run "
            "Tools/create_sky_material.py first; it is the sole author of that asset.")

    # SkyGraphBuilder for the same reason the star block uses one: bathy_field_
    # graph is written against it, it brings its own checked connects, and its
    # collection_param checks the binding by NAME against the asset as read back.
    bathy_b = SkyGraphBuilder(material, sky_collection)
    bathy = sample_bathy_field(bathy_b)

    # HOW MUCH OF THE DEPTH GRADING THE BAKED FIELD OWNS, 0..1.
    #
    # This exists because the engine will not let us simply hand it a depth.
    # SingleLayerWaterShading.ush:160 computes WaterVolumeDepth from the SCENE --
    # `BehindWaterSceneDepth - WaterSurfaceSceneDepth` -- and there is no material
    # input that overrides it. The only per-pixel hook the material has into the
    # volume is ColorScaleBehindWater, which multiplies the scene colour behind
    # the water BEFORE the engine's own transmittance (:238).
    #
    # So the two paths are combined in OPTICAL DEPTH, where they add:
    #
    #   total = engineAbsorb * (1 - a*valid) * sceneRayDepth      (engine)
    #         + engineAbsorb * (a*valid)     * slantBakedDepth    (us, via
    #                                                              ColorScaleBehindWater)
    #
    # At a = 0 this is byte-for-byte today's material. At a = 1 the absorption of
    # the bed comes entirely from the smooth baked field. SCATTERING IS NEVER
    # SPLIT and always stays with the engine, deliberately: scattering is what
    # gives the water a colour of its OWN where there is nothing behind it (a
    # grazing view against the sky), the engine's analytic single-scattering
    # integral is the good part of Phase 1, and it does not suffer from the two
    # defects the baked field is here to fix.
    #
    # THE TWO DEFECTS, precisely, because "baked is better" is not an argument:
    #   * the bed is a 10 cm voxel staircase, so a scene-depth difference steps
    #     with it and draws CONTOUR RINGS in the water colour;
    #   * where the bed is not drawn at all -- unstreamed chunks, or a bed beyond
    #     the depth-without-water buffer -- the difference collapses and the
    #     water reads as clear.
    # A baked vertical depth has neither problem. What it does NOT have is the
    # engine's exactness against real geometry, which is why a boat or a player
    # standing in the shallows still needs the engine term. Hence a blend, not a
    # replacement, and hence the default is not 1.0.
    bathy_authority = bathy_b.scalar("BathyDepthAuthority", 0.85)
    bathy_weight = bathy_b.mul(bathy_authority, bathy["validity"])

    # ======================================================================
    # THE WAVE FIELD, BUILT HERE BECAUSE FOAM READS IT
    # ======================================================================
    #
    # It used to live ~1,500 lines down, beside the normal and the WPO it feeds,
    # and the whole of that section's reasoning is still down there -- the
    # absolute-world-XY argument, the eight octaves, the quantisation knob, the
    # 58.7-degree sweep. Read it there; none of it moved and none of it changed.
    #
    # WHAT MOVED AND WHY. water_wave_graph's node returns a FOURTH output, a
    # breaking-wave signal, and its consumer is the foam composite about 500
    # lines below this point -- which is 1,200 lines ABOVE where the wave node
    # used to be built. Python evaluates top to bottom, so the field has to be
    # built before the first thing that reads it, and foam is now that thing.
    #
    # NOTHING ABOUT THE FIELD CHANGED BY MOVING IT. It is a pure function of
    # absolute world position and time; both of its original consumers (the
    # pixel normal and the vertex displacement) still read it at exactly the
    # sites they always did, from the same single evaluation.
    #
    # THIS IS ALSO THE EARLIEST POINT IT CAN GO: it reads `bathy` for depth and
    # validity, and `bathy` is sampled immediately above.
    #
    # ONE Time node still drives the whole field, so VOXEL_WATER_FREEZE_TIME is
    # still a single-node substitution rather than a graph edit -- and it now
    # freezes the breaking foam too, by construction, because the foam rides the
    # same crests off the same evaluation.
    if FREEZE_RIPPLE_TIME:
        ripple_time = mel.create_material_expression(
            material, unreal.MaterialExpressionConstant, -900, 950)
        ripple_time.set_editor_property("r", 0.0)
    else:
        ripple_time = mel.create_material_expression(
            material, unreal.MaterialExpressionTime, -900, 950)

    # ABSOLUTE WORLD XY IN METRES, and the argument for it is the longest one in
    # the wave section below ("THE COORDINATE IS ABSOLUTE WORLD XY IN METRES,
    # NOT TextureCoordinate(0)"). Short form: the pooled vertex factory's UV
    # repeats every 32 m and mirrors about the world axes, the far-field sheets
    # do not use that UV at all, and world XY is the only key both draw paths can
    # agree on. WPT_EXCLUDE_ALL_SHADER_OFFSETS because this value feeds World
    # Position Offset and a position carrying this material's own WPO would put
    # the wave into its own input.
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

    # UU -> metres. Every frequency in the wave loop is quoted in radians per
    # METRE so the wavelengths in the comments are readable as wavelengths.
    uu_to_m = mel.create_material_expression(
        material, unreal.MaterialExpressionConstant, -1120, 780)
    uu_to_m.set_editor_property("r", 0.01)
    wave_pos_m = mel.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -960, 720)
    if not mel.connect_material_expressions(world_xy, "", wave_pos_m, "A"):
        raise RuntimeError("connect world_xy -> wave_pos_m.A failed")
    if not mel.connect_material_expressions(uu_to_m, "", wave_pos_m, "B"):
        raise RuntimeError("connect uu_to_m -> wave_pos_m.B failed")

    # --- THE ARM, AND THE ONE LINE THAT DECIDES WHAT THIS LAKE LOOKS LIKE ----
    #
    # THE DEFAULT ARM SHIPS THE WIND FEATURE ON. That is a deliberate, visible
    # change to a signed-off picture, requested so the owner can play-test waves
    # at different wind speeds; the full list of what looks different is at
    # LEGACY_WAVES near the top of this file and it is not repeated here.
    #
    # STARTING FROM water_wave_graph.DEFAULTS AND NOT FROM A HAND-WRITTEN DICT is
    # the point of passing the table at all. Every number in it is argued in that
    # module against a measurement -- WindDirectionAuthority against the corduroy
    # histogram, BreakSurfFloorM against the 1.22 cm -> 4.88 cm shore crest,
    # BreakDepthRatio against McCowan's 0.78 breaker index, BreakShoalGain
    # against Green's law -- and a copy of any of them typed here would be a
    # second, silent authority on the same quantity. The dict() is so a later
    # edit to this file cannot mutate the module's table in place.
    #
    # LEGACY_RECIPE is the module's own machine-readable "reproduce the shipped
    # field", not five numbers reconstructed by reading a diff. Verified offline
    # to 5.1e-11 m in height and 8.6e-10 in the gradient components against a
    # transcription of the shipped HLSL.
    if LEGACY_WAVES:
        WAVE_DEFAULTS = dict(water_wave_graph.LEGACY_RECIPE)
    else:
        WAVE_DEFAULTS = dict(water_wave_graph.DEFAULTS)

    # --- THE CALM-LAKE FLOOR: A DECISION, AND IT IS "NO FLOOR" --------------
    #
    # THE PATH, STATED FIRST SO IT IS NOT DISCOVERED FROM A SCREENSHOT. The wave
    # amplitude scale is u^WindAmpExponent with u = |WindVectorMS| /
    # WindRefSpeedMS and the shipped exponent 1.0, and it is NOT gated by
    # WindDirectionAuthority -- only the STEERING is. So the moment
    # UVoxelWeatherSubsystem publishes WindFieldValid = 1 over a near-zero wind,
    # the lake goes to glass: a mirror, no normal variation, no displacement, no
    # surf. Every crest in the field is multiplied by that one number.
    #
    # THREE THINGS MAKE THAT SAFE ENOUGH TO SHIP AS-IS.
    #
    #   1. IT IS THE CORRECT PHYSICS AND IT IS THE FEATURE. A wind-driven field
    #      with no wind has no waves. If the owner pins 0 m/s and gets a mirror,
    #      the instrument is reading true -- that is the low end of the sweep he
    #      asked to play-test, not a failure of it.
    #   2. IT NEEDS THE SUBSYSTEM TO BE PUBLISHING A CALM. With no weather
    #      subsystem, or with voxel.Weather.Enabled 0, WindFieldValid is 0 and
    #      build_wind_input's lerp returns the MATERIAL fallback exactly --
    #      WindFallbackSpeedMS 5.0 at WindFallbackDirDeg 238.7, which is u = 1
    #      and therefore the shipped wave SIZE with the new steering. A run with
    #      no weather is never glass.
    #   3. IT IS RECOVERABLE IN A FRAME, FROM THE CONSOLE, WITH NO REGENERATION:
    #      `voxel.Weather.PinMps 5` puts it straight back.
    #
    # AND THE FLOOR IS NOT IMPLEMENTABLE HERE ANYWAY, WHICH IS THE OTHER HALF OF
    # THE DECISION. The wind expression is built inside build_wave_field, from
    # build_wind_input, and reaches the HLSL on a pin this file never touches;
    # nothing in WAVE_DEFAULTS can add a term to it. The two levers this file
    # does have are both wrong:
    #
    #   * Raising WindRefSpeedMS scales u, it does not floor it. Zero stays zero.
    #   * Dropping WindAmpExponent toward 0 flattens the whole response curve --
    #     at 0.1, a 0.05 m/s breath (u = 0.01) would produce 0.01^0.1 = 63% of
    #     the reference wave, and a 20 m/s storm only 115% of it.
    #     That destroys exactly the thing being play-tested (wave size against
    #     wind speed) in order to fix its endpoint, and pow(0, 0.1) is still 0,
    #     so it does not even fix the endpoint.
    #
    # SO IF A FLOOR IS WANTED, IT BELONGS ON THE PUBLISHED WIND, NOT ON THE
    # AMPLITUDE: one clamp on the sustained speed in UVoxelWeatherSubsystem's
    # PublishWind, before it writes WindVectorMS. A floor of 0.5 m/s gives
    # u = 0.1, H_s = 0.6 cm and a break depth of 0.8 cm -- a lake with a faint
    # texture on it rather than a mirror -- and it keeps ONE definition of "how
    # windy is it" instead of a second one hidden in a shader. That is a change
    # to weather.h/VoxelWeatherSubsystem.cpp and is deliberately not made here.
    wave_field = water_wave_graph.build_wave_field(
        bathy_b, wave_pos_m, ripple_time, bathy,
        defaults=WAVE_DEFAULTS, log=unreal.log_warning)
    wave_grad_raw = wave_field["gradient"]     # float2, dH/dx and dH/dy
    wave_height_m = wave_field["height_m"]     # float, metres
    wave_breaking = wave_field["breaking"]     # float 0..1, for foam

    # Logged HERE rather than beside water_optics.summary_lines() at the top of
    # main(), because wind_source is not known until build_wind_input has read
    # the collection back -- and wind_source is the line that matters. It says
    # MPC_VoxelSky when the two wind parameters are on the collection (they are,
    # create_sky_material.py:525 and :564) and material-fallback when they are
    # not, which is the tell that create_sky_material.py has not been re-run
    # since they landed.
    for _line in water_wave_graph.summary_lines(wave_field["wind_source"]):
        unreal.log("M_WaterVoxel " + _line)
    unreal.log("M_WaterVoxel WIND WAVE DEFAULTS (%s): %s"
               % ("LEGACY_RECIPE" if LEGACY_WAVES else "water_wave_graph.DEFAULTS",
                  ", ".join("%s=%g" % kv for kv in sorted(WAVE_DEFAULTS.items()))))

    # --- SINGLE LAYER WATER VOLUME: ABSORPTION AND SCATTERING --------------
    #
    # THIS REPLACES THE ENTIRE W5/W6/W7 DEPTH-TINT AND OPACITY GRAPH, and it is
    # worth being precise about what was deleted, because a lot of tuning went
    # into it and none of that tuning is being thrown away lightly.
    #
    # WHAT WAS THERE. SceneDepth minus PixelDepth gave the view-ray thickness in
    # unreal units; a single-channel Beer-Lambert term `1 - exp(-t/160)` turned
    # that into depth01; depth01 lerped a shallow tint (0.035, 0.26, 0.68)
    # toward a deep tint (0.008, 0.055, 0.24) and simultaneously lerped opacity
    # from 0.42 to 0.98. It worked, it was owner-judged twice, and it is gone
    # for two reasons, only one of which is the shading model.
    #
    #   1. The old opacity was a VIEW-THROUGH ALPHA, and an SLW material has no
    #      such thing: it must be opaque (MaterialShared.cpp:6425) and the scene
    #      behind the water is re-added by the shading model itself, not by
    #      blending. So that half of the graph is not re-expressed, it is
    #      replaced by the thing it was approximating -- the absorption below.
    #      NOTE THAT MP_OPACITY ITSELF DOES STILL EXIST AND IS STILL REQUIRED:
    #      on an SLW material it means "how much of this pixel is covered by the
    #      opaque layer ON the water", it defaults to 1 when unwired, and 1
    #      switches the entire water volume off. It is wired to the foam
    #      coverage down at "OPACITY IS THE SWITCH THAT TURNS THE WHOLE WATER
    #      VOLUME ON"; read that before touching this.
    #   2. It was a ONE-CHANNEL extinction wearing a two-colour costume, and
    #      that is exactly the defect the owner is looking at. `-1/160` applies
    #      identically to red, green and blue, so the water's TRANSMITTANCE was
    #      colour-neutral and the only colour came from lerping between two
    #      authored constants. An effective red:blue absorption ratio of 1:1 is
    #      why deep water went DARK rather than BLUE, and no amount of moving
    #      the two tints fixes that -- it is the difference between a body of
    #      water and a sheet of grey glass with blue paint on it.
    #
    # WHAT IS THERE NOW. Per-channel absorption and scattering coefficients
    # handed to the engine, which applies them along the real view path with the
    # real scene behind the water (SingleLayerWaterShading.ush:214-238:
    # `OpticalDepth = ExtinctionCoeff * WaterVolumeDepth`, then the analytic
    # single-scattering integral). Three things the old graph could not do come
    # free with that: the transmittance is per-channel so depth SHIFTS HUE
    # instead of just darkening; the light path from the bed up to the surface
    # is attenuated separately from the view path (MeanTransmittanceToLightSources),
    # which is what makes a lit bed under 3 m of water read blue rather than
    # merely dim; and in-scattering means the water has a colour of its OWN even
    # where nothing is behind it, which is the "grazing view against the sky"
    # case the old graph explicitly documented as broken (SceneDepth is the far
    # plane there, so it read at maximum depth tint).

    # ======================================================================
    # 1/cm. NOT 1/m. THE ONE CONVERSION NODE, AND EVERY COEFFICIENT GOES
    # THROUGH IT.
    #
    # MaterialExpressionSingleLayerWaterMaterialOutput.h:16 --
    #   "Valid range is [0,+inf[. Unit is 1/cm."
    # SingleLayerWaterShading.ush:214 --
    #   const float3 OpticalDepth = ExtinctionCoeff * WaterVolumeDepth;
    # ...where WaterVolumeDepth is a difference of two SCENE DEPTHS, and scene
    # depth in Unreal is in unreal units, i.e. centimetres.
    #
    # Epic's public documentation for this node says METRES. It is wrong, and a
    # figure taken from it lands 100x too strong -- water that goes black in
    # 9 cm. Every absorption number in the published literature is per metre, so
    # every one of them must be multiplied by 0.01 on the way in. That multiply
    # exists exactly once, here, and both chains below feed through it, so this
    # mistake is available to be made once and is visible in one node.
    # ======================================================================
    per_cm = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -1300, -700)
    per_cm.set_editor_property("r", 0.01)

    # --- ABSORPTION, VIA UNITY'S ABSORPTION-DISTANCE PARAMETERISATION -------
    #
    # Raw coefficients are unauthorable: nobody can picture 0.0045 1/cm, and the
    # three channels have to move together in a specific ratio or the hue goes
    # wrong. Unity HDRP solves this with two knobs anyone can reason about and
    # a one-line derivation, and that derivation is copied verbatim:
    #
    #     coefficient = (-ln(0.02) / absorptionDistance) * (1 - refractionColor)
    #
    # absorptionDistance is the depth at which 2% of light survives -- "how deep
    # before it goes dark". refractionColor is "what colour survives longest": 0
    # in a channel means that channel is fully gone at that distance, 1 means
    # that channel is not absorbed at all.
    #
    # THE SHIPPED PAIR AND WHERE IT COMES FROM. The owner's brief is "clear and
    # blue, stylised", and the quantity that dials that is the RED:BLUE
    # ABSORPTION RATIO. Real water is ~129:1. Near-realistic shader work sits
    # near 26:1. A deliberately stylised one (Photon) sits near 5.6:1. The
    # material this replaces was 1:1. Shipped here: ~11:1, from absorption
    # R 0.45 / G 0.10 / B 0.04 per metre.
    #
    #   8.7 m and (0.0, 0.7776, 0.9111) reproduce those three numbers exactly:
    #     R: 3.9120 / 8.7 * (1 - 0.0000) = 0.4497 per metre
    #     G: 3.9120 / 8.7 * (1 - 0.7776) = 0.0999 per metre
    #     B: 3.9120 / 8.7 * (1 - 0.9111) = 0.0400 per metre
    #
    # WHAT THAT ACTUALLY LOOKS LIKE, as transmittance rather than as a claim,
    # because "red dies in 2 m" is the sort of thing that gets repeated without
    # anyone checking it. Extinction is absorption PLUS scattering (below), so
    # the totals are R 0.455 / G 0.145 / B 0.095 per metre. Straight down
    # through d metres the light travels roughly 2d (down to the bed and back),
    # so:
    #     d = 0.3 m   RGB (0.76, 0.92, 0.94)   -- three voxels: barely tinted
    #     d = 1 m     RGB (0.40, 0.75, 0.83)   -- clearly warm-stripped
    #     d = 2 m     RGB (0.16, 0.56, 0.68)   -- reads blue
    #     d = 5 m     RGB (0.01, 0.24, 0.39)   -- bed nearly gone
    #     d = 25 m    RGB (0.00, 0.00, 0.01)   -- bed gone, colour is in-scatter
    # Deep water is therefore NOT black: it converges on the scattering albedo
    # below, which is the whole reason scattering is authored at all.
    # 8.7 -> 3.5 m, 2026-08-12, on the owner's call that the water is "too see
    # through ... even in the middle of the lake I can see the voxel terrain
    # lake bed". This is Unity's parameterisation: the depth at which 2% of
    # light survives, so extinction = -ln(0.02)/d * (1 - AbsorptionColor).
    #
    # MEASURED WHY. At 8.7 m the water still passed 30% green and 36% blue at
    # 3 m of path. The dominant term was BLUE, absorbed at only 0.04 per metre
    # -- 89% survives every metre, so no practical lake depth could hide the
    # bed. Shortening the distance alone would not have fixed it; the colour
    # below is rebalanced in the same change.
    #
    # FOR SCALE, every shipped preset is far more absorbing than real water
    # (0.265/0.055/0.010 per m), deliberately, so falloff is visible over
    # gameplay depths rather than over 50 m: Crest ships 0.90/0.30/0.35,
    # Unity HDRP's Pool 5 m and its Ocean/River preset 1.5 m. 3.5 m sits
    # between the two Unity presets.
    #
    # THE NUMBER IS NO LONGER TYPED ON THE NEXT LINE. It is
    # water_optics.ABSORPTION_DISTANCE_M, shared with the underwater
    # post-process material -- see the import at the top of this file for why,
    # and water_optics.py's docstring for the commit where the two disagreed. It
    # currently reads 3.5 m, which is what every figure above was computed
    # against; if it moves, those figures are a derivation to redo here, not a
    # second copy of the value to edit.
    absorb_distance = scalar_param(
        "AbsorptionDistanceM", water_optics.ABSORPTION_DISTANCE_M, -1300, -560)
    # (0, 0.778, 0.911) -> (0, 0.30, 0.38). The channel that survives is
    # 1 - this, so the old value made blue nearly transparent (1 - 0.911 =
    # 0.089) and that is the single reason the bed stayed visible at every
    # depth this world contains. Now: extinction = 1.118 * (1, 0.70, 0.62) =
    # 1.12 / 0.78 / 0.69 per metre, i.e. transmittance 0.33/0.46/0.50 at 1 m,
    # 0.11/0.21/0.25 at 2 m, 0.035/0.096/0.126 at 3 m -- the bed is faint by
    # two metres and gone by three, which is what was asked for.
    #
    # CORRECTION, 2026-08-12, AND THE RUN LOG WILL NOW CONTRADICT THE PARAGRAPH
    # ABOVE IF THIS IS NOT READ. Those three numbers are ABSORPTION ONLY. What
    # attenuates a path is absorption PLUS scattering, and scattering was raised
    # in the same change, so the real extinction is 1.138 / 0.882 / 0.953 per
    # metre and the real transmittance at 1 m is 0.32 / 0.41 / 0.39, not
    # 0.33 / 0.46 / 0.50. The direction of the conclusion is unchanged -- the bed
    # goes faint sooner, not later -- but the hue does move: including scattering,
    # GREEN carries furthest at short range rather than blue, because blue is
    # scattered hardest (0.26 per metre against green's 0.10). Blue still wins in
    # the deep-water limit, which is set by the ALBEDO (0.018/0.113/0.273) and not
    # by the transmittance. water_optics.summary_lines(), printed at the top of
    # this run, is the authority for all of these and prints both rows precisely
    # so this mistake is caught by reading rather than by re-deriving; the trap is
    # named in water_optics.extinction_per_m's docstring for the same reason.
    #
    # Red still dies first and blue still carries furthest, so the colour
    # SHIFTS with depth rather than merely darkening; the ratio is just far
    # tighter than the clear-water 11:1 it was.
    #
    # Also shared now: water_optics.ABSORPTION_COLOR, currently (0, 0.30, 0.38).
    # Unpacked with a star rather than indexed [0]/[1]/[2] on purpose -- a tuple
    # of the wrong length raises here, where indexing would silently drop a
    # fourth channel or read past the end at a site whose whole job is to be the
    # same three numbers the other material uses.
    absorb_color = vector_param(
        "WaterAbsorptionColor", *water_optics.ABSORPTION_COLOR, -1300, -480)
    absorb_color_rgb = rgb(absorb_color, -1120, -480)

    # -ln(0.02) = 3.9120230054281460586. The 2% survival convention is Unity's;
    # it is arbitrary but it is the convention the published numbers are quoted
    # against, so changing it here would silently re-scale every figure above.
    minus_ln002 = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -1300, -620)
    minus_ln002.set_editor_property("r", 3.9120230054)

    absorb_base = mel.create_material_expression(material, unreal.MaterialExpressionDivide, -1120, -600)
    if not mel.connect_material_expressions(minus_ln002, "", absorb_base, "A"):
        raise RuntimeError("connect minus_ln002 -> absorb_base.A failed")
    if not mel.connect_material_expressions(absorb_distance, "", absorb_base, "B"):
        raise RuntimeError("connect absorb_distance -> absorb_base.B failed")

    absorb_survive = mel.create_material_expression(material, unreal.MaterialExpressionOneMinus, -960, -480)
    if not mel.connect_material_expressions(absorb_color_rgb, "", absorb_survive, ""):
        raise RuntimeError("connect absorb_color_rgb -> absorb_survive failed")

    absorb_per_m = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -800, -540)
    if not mel.connect_material_expressions(absorb_base, "", absorb_per_m, "A"):
        raise RuntimeError("connect absorb_base -> absorb_per_m.A failed")
    if not mel.connect_material_expressions(absorb_survive, "", absorb_per_m, "B"):
        raise RuntimeError("connect absorb_survive -> absorb_per_m.B failed")

    absorb_per_cm = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -640, -540)
    if not mel.connect_material_expressions(absorb_per_m, "", absorb_per_cm, "A"):
        raise RuntimeError("connect absorb_per_m -> absorb_per_cm.A failed")
    if not mel.connect_material_expressions(per_cm, "", absorb_per_cm, "B"):
        raise RuntimeError("connect per_cm -> absorb_per_cm.B failed")

    # --- SCATTERING ---------------------------------------------------------
    #
    # WHAT THIS CONTROLS IS THE COLOUR OF DEEP WATER, and it is worth stating
    # that plainly because it is not obvious from the name. The engine's
    # single-scattering integral converges, as the water gets deep and the
    # transmittance goes to zero, on the single-scattering ALBEDO --
    # scattering / (scattering + absorption) -- times whatever light is arriving.
    # So this vector IS the answer to "what colour is the middle of the lake".
    #
    # Shipped: 0.005 / 0.045 / 0.055 per metre, which against the absorption
    # above gives an albedo of (0.011, 0.310, 0.579). Blue-dominant with real
    # green in it -- the owner's "saturated blue-green with depth" -- rather
    # than the near-black (0.008, 0.055, 0.24) the old deep tint had to author
    # by hand, and rather than the physically-correct green an inland lake would
    # actually be, which the owner explicitly did not ask for.
    #
    # IT IS ALSO WHAT KEEPS SHALLOW WATER CLEAR. Scattering adds to extinction,
    # so a large value fogs the shallows and hides the bed. At 0.3 m the totals
    # above still transmit 76-94%, so "see the bottom in the shallows" survives.
    # This is the knob to move if the lake ever needs to look silty; raising it
    # is how you get milk, and it will destroy the shallows first.
    # RAISED WITH THE ABSORPTION, and this is the half that keeps it looking
    # like water. Absorption alone only removes light: crank it and the lake
    # goes dark rather than deep. The in-scatter term is what puts colour BACK
    # and it SATURATES with distance, so past a few metres everything converges
    # on the water's own colour and loses contrast -- which is the actual
    # perceptual cue for depth.
    #
    # It is also the physically right lever for "murkier": turbid water is a
    # BRIGHT medium, not a dark one. Petzold measured clear ocean to turbid
    # harbour as a ~50x change in SCATTERING against only ~3x in absorption,
    # which is why silty water reads milky rather than black.
    #
    # 0.005/0.045/0.055 -> 0.02/0.10/0.26: a deep blue body, single-scattering
    # albedo (sigma_s / sigma_t) about 0.018/0.114/0.274, so deep water settles
    # on a saturated blue instead of on black.
    #
    # THE PAIR OF FIGURES ABOVE IS THE HISTORY OF THE CHANGE, NOT THE SOURCE OF
    # THE VALUE. What ships is water_optics.SCATTERING_PER_M, currently
    # (0.02, 0.10, 0.26) per metre, shared with the underwater material so a
    # swimmer is inside the same medium he was looking at. Same star-unpack
    # arity check as the absorption colour above.
    scatter_color = vector_param(
        "ScatteringPerMetre", *water_optics.SCATTERING_PER_M, -1300, -400)
    scatter_rgb = rgb(scatter_color, -1120, -400)
    scatter_per_cm = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -640, -420)
    if not mel.connect_material_expressions(scatter_rgb, "", scatter_per_cm, "A"):
        raise RuntimeError("connect scatter_rgb -> scatter_per_cm.A failed")
    if not mel.connect_material_expressions(per_cm, "", scatter_per_cm, "B"):
        raise RuntimeError("connect per_cm -> scatter_per_cm.B failed")

    # --- PHASE G ------------------------------------------------------------
    #
    # How directional the scattering is: 0 is isotropic, positive is forward.
    # The engine uses it for the SUN term only (SchlickPhase against the
    # refracted view ray), so what it actually controls is how much brighter the
    # water looks when you are looking roughly along the sun's direction through
    # it -- the bright band you see looking away from a low sun across a lake.
    #
    # 0.35 is a deliberate middle. Real water with particulates is strongly
    # forward-scattering (0.9+), but at 0.9 the entire in-scatter collapses into
    # a narrow lobe that a top-down view of a lake at noon never enters, so the
    # term would be invisible in exactly the shot the owner keeps taking. 0.35
    # keeps a visible sun-side brightening without turning the lake into a
    # spotlight.
    #
    # Shared as water_optics.PHASE_G, currently 0.35. It is in the shared module
    # rather than here even though the SLW node is the only consumer TODAY,
    # because it describes the medium's particulates and not this view of them:
    # the moment the underwater material grows a sun shaft or an in-scatter term
    # it needs the same anisotropy, and the failure mode of it having its own
    # copy is a lit-from-behind sun band that changes strength when you duck
    # under the surface.
    phase_g = scalar_param("WaterPhaseG", water_optics.PHASE_G, -1300, -340)

    # --- COLOR SCALE BEHIND WATER -------------------------------------------
    #
    # A multiplier on the scene colour seen THROUGH the water, before
    # transmittance. Wired to a neutral (1,1,1) parameter -- i.e. currently a
    # no-op -- because it is the correct future home for two things on the plan
    # (caustics on the bed, and a bed-darkening term) and leaving the pin
    # unconnected would mean rediscovering that it exists. The engine already
    # fades it to 1 over the first ~50 cm of water depth
    # (SingleLayerWaterShading.ush:177, `saturate(WaterVolumeDepth * 0.02)`), so
    # whatever lands here cannot produce a hard edge at the shoreline.
    behind_scale = vector_param("ColorScaleBehindWater", 1.0, 1.0, 1.0, -1300, -280)
    behind_scale_rgb = rgb(behind_scale, -1120, -280)

    # --- ...AND IT IS NOW THE BAKED DEPTH'S ONLY WAY IN ---------------------
    #
    # exp(-absorb_per_cm * a*valid * slantBakedUU), multiplied onto the neutral
    # parameter above. See the BathyDepthAuthority comment near the top of this
    # function for why this is the hook and why only absorption is split.
    #
    # UNITS. absorb_per_cm is 1/cm (that is the one conversion node, and this
    # chain reuses its output rather than making a second one). The baked depth
    # arrives in METRES, so the slant result is scaled by 100 exactly once, here.
    #
    # THE ENGINE'S OWN SHORELINE FADE IS DOING REAL WORK FOR US. :171 wraps this
    # output in `lerp(1, ColorScaleBehindWater, saturate(WaterVolumeDepth*0.02))`
    # -- a fade to neutral over the first 50 cm of water. That is exactly the
    # behaviour a baked depth term needs at the waterline: it means the baked
    # attenuation cannot draw a hard edge where the field meets the shore, even
    # if the field's own gradient there were abrupt. It is not a limitation being
    # worked around; it is the reason this hook is safe to use.
    bathy_slant_m = build_slant_depth(
        bathy_b, bathy["depth_m"],
        bathy_b.mask(bathy_b.node(unreal.MaterialExpressionCameraVectorWS), "", b=True))
    bathy_slant_uu = bathy_b.mul(bathy_slant_m, bathy_b.const(100.0))
    bathy_optical = bathy_b.mul(bathy_b.mul(absorb_per_cm, bathy_weight), bathy_slant_uu)
    bathy_neg = bathy_b.mul(bathy_optical, bathy_b.const(-1.0))
    bathy_transmit = bathy_b.node(unreal.MaterialExpressionExponential)
    bathy_b.link(bathy_neg, "", bathy_transmit, "")
    behind_scale_rgb = bathy_b.mul(behind_scale_rgb, bathy_transmit)

    # --- THE OUTPUT NODE ----------------------------------------------------
    #
    # WITHOUT THIS NODE THE MATERIAL DOES NOT COMPILE. MaterialShared.cpp:6436:
    # "SingleLayerWater materials requires the use of SingleLayerWaterMaterial
    # output node." It is a CustomOutput, not a material property, so it is
    # wired by input NAME rather than through connect_material_property -- the
    # names are the UPROPERTY names off
    # MaterialExpressionSingleLayerWaterMaterialOutput.h, which is what
    # UMaterialExpression::GetInputName falls back to when an FExpressionInput
    # carries no explicit name (MaterialExpressions.cpp:1821-1849). Each connect
    # is checked like every other connect in this file; a renamed pin in a
    # future engine version raises here rather than silently compiling to the
    # node's default (Constant3(0,0,0) for the two coefficient pins), which
    # would be perfectly clear, perfectly invisible water.
    slw_out = mel.create_material_expression(
        material, unreal.MaterialExpressionSingleLayerWaterMaterialOutput, -420, -500)
    if not mel.connect_material_expressions(scatter_per_cm, "", slw_out, "ScatteringCoefficients"):
        raise RuntimeError("connect scatter_per_cm -> SLW.ScatteringCoefficients failed")
    # THE ENGINE'S SHARE OF THE ABSORPTION: whatever the baked field did not take.
    # This is the other half of the split described at BathyDepthAuthority; the
    # two together add up to exactly one absorption, applied along two different
    # estimates of the same path. Where the field is absent (validity 0) this is
    # the full coefficient and the material is identical to Phase 1's.
    absorb_engine = bathy_b.mul(absorb_per_cm, bathy_b.one_minus(bathy_weight))
    if not mel.connect_material_expressions(absorb_engine, "", slw_out, "AbsorptionCoefficients"):
        raise RuntimeError("connect absorb_engine -> SLW.AbsorptionCoefficients failed")
    if not mel.connect_material_expressions(phase_g, "", slw_out, "PhaseG"):
        raise RuntimeError("connect phase_g -> SLW.PhaseG failed")
    if not mel.connect_material_expressions(behind_scale_rgb, "", slw_out, "ColorScaleBehindWater"):
        raise RuntimeError("connect behind_scale_rgb -> SLW.ColorScaleBehindWater failed")

    # --- AND ONLY NOW IS IT A SINGLE LAYER WATER MATERIAL -------------------
    #
    # The output node exists and is wired, so the engine's validation
    # (MaterialShared.cpp:6420-6437) can pass on the first recompile this
    # triggers. See the note at the top of main() for why this is not up there
    # with blend_mode and two_sided.
    material.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER)

    # READ IT BACK. UMaterial keeps the authored ShadingModel in one field and a
    # CACHED FMaterialShadingModelField (`ShadingModels`) in another, and it is
    # the cached one the renderer, the primitive relevance flags and the SLW mesh
    # pass all read. The cache is rebuilt by
    # UMaterial::PostEditChangeProperty -> RebuildShadingModelField(), so this
    # line is only correct while the Python setter fires a real property-change
    # notification. If that ever stops being true the asset would say
    # "Single Layer Water" in the details panel, compile without complaint as
    # DefaultLit, and render as an opaque surface with a black base colour and no
    # volume at all -- a specific, plausible, hard-to-diagnose picture.
    #
    # MEASURED ON THIS ENGINE BUILD: get_editor_property("shading_models") is NOT
    # reachable from Python (the FMaterialShadingModelField struct is not
    # exported), so the cached field cannot be read here at all. The authored
    # field can be, and is; the cached one is confirmed instead by the ABSENCE of
    # the "requires the use of SingleLayerWaterMaterial output node" compile
    # error after this point, which tools/voxel-water-star-regen.ps1 enforces on
    # every run. Reporting a bindings gap as a pass would be the worse lie, so
    # this says which of the two it checked.
    authored = material.get_editor_property("shading_model")
    if authored != unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER:
        raise RuntimeError(
            "shading model did not take: authored ShadingModel is %r, expected "
            "MSM_SINGLE_LAYER_WATER." % (authored,))
    unreal.log(
        "M_WaterVoxel SHADING MODEL: authored=%s (the CACHED ShadingModels field is not "
        "readable from Python on this build -- confirm it from the absence of a "
        "'requires the use of SingleLayerWaterMaterial output node' compile error below)"
        % (authored,))

    # --- W5 (2/3): FOAM ----------------------------------------------------
    #
    # Two signals, both of which already exist -- no new geometry, no new
    # texture, and (crucially) no new vertex-factory uniform member.
    #
    # SIGNAL 1, SLOPE. The water surface is a bilinear patch over four per-cell
    # corner heights, and the vertex factory builds its shading normal from that
    # patch (Decoded.ShadingNormal; the CPU path computes the identical
    # -dH/du,-dH/dv,1 in VoxelWaterChunkComponent.cpp). So VertexNormalWS.z is
    # already a slope reading, for free, and it is large wherever fill changes
    # fast across a cell: a spilling front, a step between fill levels, water
    # running down a slope. A settled pool has every corner equal, normal
    # straight up, and no foam anywhere -- which is the behaviour that matters,
    # because "foam everywhere all the time" is the standard way this effect
    # goes wrong.
    #
    # SIGNAL 2, ACTIVITY. VertexColor.A, 1 while vxc::WaterCA still calls the
    # brick active. This catches the case slope cannot: a brick churning
    # violently but momentarily flat.
    #
    # SIDE WALLS ARE EXCLUDED BY THE NORMAL, NOT BY VertexColor.B. B is the
    # top-BOUNDARY flag and is 1 on a side wall's upper pair of vertices, so
    # masking with it would ring every water body with a foam stripe along the
    # top of its side walls. A side face's shading normal has z == 0 exactly
    # (only the top face's normal is replaced by the corner-height gradient), so
    # saturate(N.z * 4) is 0 there and 1 on any top face that is not nearly
    # vertical -- and it also excludes bottom faces, whose z is -1.
    vertex_normal = mel.create_material_expression(material, unreal.MaterialExpressionVertexNormalWS, -1300, -200)
    surface_normal_z = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -1120, -200)
    surface_normal_z.set_editor_property("r", False)
    surface_normal_z.set_editor_property("g", False)
    surface_normal_z.set_editor_property("b", True)
    surface_normal_z.set_editor_property("a", False)
    if not mel.connect_material_expressions(vertex_normal, "", surface_normal_z, ""):
        raise RuntimeError("connect vertex_normal -> surface_normal_z failed")

    top_face_gain = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -1120, -120)
    top_face_gain.set_editor_property("r", 4.0)
    top_face_scaled = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -960, -180)
    if not mel.connect_material_expressions(surface_normal_z, "", top_face_scaled, "A"):
        raise RuntimeError("connect surface_normal_z -> top_face_scaled.A failed")
    if not mel.connect_material_expressions(top_face_gain, "", top_face_scaled, "B"):
        raise RuntimeError("connect top_face_gain -> top_face_scaled.B failed")
    top_face_mask = mel.create_material_expression(material, unreal.MaterialExpressionSaturate, -800, -180)
    if not mel.connect_material_expressions(top_face_scaled, "", top_face_mask, ""):
        raise RuntimeError("connect top_face_scaled -> top_face_mask failed")

    # SmoothStep(min, max, N.z) is 1 on flat water and 0 once the surface is
    # steep, so the foam term is its complement. The window is quoted in normal
    # z rather than in degrees on purpose, since that is what the node compares:
    #   0.985 -> ~10 degrees of tilt. Below this is ordinary bilinear wobble on
    #           a settled surface and must NOT foam, or every lake whitens.
    #   0.870 -> ~30 degrees. A full 10 cm fill step across one 10 cm voxel is
    #           45 degrees (z = 0.707), well past saturation, so a genuine
    #           spilling edge foams fully.
    slope_min = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -800, -100)
    slope_min.set_editor_property("r", 0.870)
    slope_max = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -800, -40)
    slope_max.set_editor_property("r", 0.985)
    slope_flat = mel.create_material_expression(material, unreal.MaterialExpressionSmoothStep, -650, -100)
    if not mel.connect_material_expressions(slope_min, "", slope_flat, "Min"):
        raise RuntimeError("connect slope_min -> slope_flat.Min failed")
    if not mel.connect_material_expressions(slope_max, "", slope_flat, "Max"):
        raise RuntimeError("connect slope_max -> slope_flat.Max failed")
    if not mel.connect_material_expressions(surface_normal_z, "", slope_flat, "Value"):
        raise RuntimeError("connect surface_normal_z -> slope_flat.Value failed")
    slope_foam = mel.create_material_expression(material, unreal.MaterialExpressionOneMinus, -500, -100)
    if not mel.connect_material_expressions(slope_flat, "", slope_foam, ""):
        raise RuntimeError("connect slope_flat -> slope_foam failed")

    # Activity contributes at 0.6, not 1.0: a brick can be "active" for a single
    # CA step because one cell gained a few fill units, and full whitewater for
    # that is a flicker. Slope alone can still reach 1.0, so a real breaking
    # front is not capped by this.
    activity_gain = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -650, 20)
    # Foam restored to the authored gain. It was zeroed only as a DIAGNOSTIC to
    # isolate tint-versus-foam, and that test exonerated it: with the gain at 0
    # the water was still white, so the fault was never here. The slope mask is
    # correctly complemented (flat water gets zero slope-foam) and activity is
    # binary on the CA active set, so a settled body carries none.
    activity_gain.set_editor_property("r", 0.6)
    activity_foam = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -500, -20)
    if not mel.connect_material_expressions(vertex_color, "A", activity_foam, "A"):
        raise RuntimeError("connect vertex_color.A -> activity_foam.A failed")
    if not mel.connect_material_expressions(activity_gain, "", activity_foam, "B"):
        raise RuntimeError("connect activity_gain -> activity_foam.B failed")

    # --- SIGNAL 3 (Phase 3): THE SHORELINE, FROM THE BAKED DISTANCE FIELD ---
    #
    # THE DISCIPLINE FIRST, because it is the whole design. A permanent white
    # ring around a still alpine lake is the single most recognisable tell of a
    # tutorial water shader, and both signals above were written specifically to
    # avoid producing one. This one is the most likely of the three to reproduce
    # it, so it is gated twice and neither gate is optional.
    #
    # GATE 1 -- WIDTH FROM DISTANCE, NOT FROM DEPTH. Crest's caveat: a depth
    # threshold gives a band whose width varies with how steeply the bed shelves,
    # so the same lake foams in a two-metre ribbon at one end and a forty-metre
    # sheet at the other. A DISTANCE threshold gives constant width. We bake
    # both, so the right field is used for each job -- distance sets the width
    # here, and depth is the gate below.
    #
    # GATE 2 -- ONLY WHERE THE SHORE GENUINELY SHELVES. Sea of Thieves' rule is
    # that calm water shows foam only around intersecting objects; the honest
    # generalisation for a lake is that a beach foams and a cliff face does not.
    # The bed slope is available for free and exactly, from the two baked fields
    # together: depth / distance-to-shore IS the mean gradient of the bed from
    # the waterline out to this cell, in metres per metre, with no derivative, no
    # screen-space term and no view dependence. A 1:20 shelf foams; a 1:4 drop-off
    # does not. That is what makes this term silent on a steep-sided tarn and
    # visible on a gravel beach, which is the behaviour being asked for.
    #
    # NOISE BEFORE THE THRESHOLD, NOT AFTER. Adding noise to the distance and
    # then thresholding breaks the isoline into an irregular edge; multiplying
    # noise into an already-thresholded band just makes a dotted ring. The Noise
    # node's Position defaults to absolute world position, so the pattern is
    # world-locked and does not swim when the camera moves.
    #
    # SMOOTHSTEP, NEVER STEP -- one aliased pixel-wide line is worse than no foam.
    shore_width = scalar_param("BathyFoamWidthM", 1.6, -1300, 60)
    shore_noise_m = scalar_param("BathyFoamNoiseM", 0.9, -1300, 120)
    shore_noise = mel.create_material_expression(material, unreal.MaterialExpressionNoise, -1300, 180)
    # Texture-based gradient noise: the cheap one. 2 levels at 0.35 (i.e. ~3 m
    # and ~1.4 m features) is enough to make the isoline read as an irregular
    # waterline rather than as a contour, and stops well short of a cost anyone
    # has to justify.
    # ENoiseFunction's Python name is derived from NOISEFUNCTION_GradientTex by
    # UE's camel-to-SCREAMING_SNAKE binding rule, which is exactly the kind of
    # thing that quietly changes between engine versions. Looked up by name and
    # RAISED on rather than defaulted: the default is NOISEFUNCTION_SimplexTex,
    # which is ~77 instructions and four texture lookups per level against this
    # one's ~61 and eight -- so a silent fallback would be a per-water-pixel cost
    # increase that nothing in any log would mention.
    _nf = getattr(unreal.NoiseFunction, "NOISEFUNCTION_GRADIENT_TEX", None)
    if _nf is None:
        raise RuntimeError(
            "unreal.NoiseFunction has no NOISEFUNCTION_GRADIENT_TEX on this engine build; it has %s. "
            "Pick the texture-based gradient one -- the computational variants are for cases where a "
            "texture lookup is the expensive part, which on this material it is not."
            % ([n for n in dir(unreal.NoiseFunction) if n.startswith("NOISEFUNCTION")],))
    shore_noise.set_editor_property("noise_function", _nf)
    shore_noise.set_editor_property("scale", 0.35)
    shore_noise.set_editor_property("levels", 2)
    shore_noise.set_editor_property("output_min", -1.0)
    shore_noise.set_editor_property("output_max", 1.0)
    shore_noise.set_editor_property("turbulence", False)

    # distance, perturbed. Positive inside the water, so the band we want is
    # 0 <= d <= width and the smoothstep runs DOWN from the waterline outward.
    shore_d = bathy_b.add(bathy["shore_m"], bathy_b.mul(shore_noise, shore_noise_m))
    shore_band = bathy_b.one_minus(bathy_b.ramp(shore_d, "", bathy_b.const(0.0), shore_width))
    # ...and it must not appear on the LAND side either: negative distances are
    # dry ground, where this material is not drawn anyway on the sheet, but the
    # near-field voxel quads can straddle the line.
    #
    # THE x8 IS LOAD-BEARING. saturate(shore_m) alone would ramp the cutoff over
    # the first WHOLE METRE of water, which is most of the foam band -- the term
    # would peak somewhere in the middle of its own band and be zero at the
    # waterline, which is the one place it has to be strongest. x8 puts the
    # cutoff inside 12.5 cm, well under the 1.875 m source raster, so it is a
    # sign test in practice and not a second, competing ramp.
    shore_band = bathy_b.mul(
        shore_band, bathy_b.saturate(bathy_b.mul(bathy["shore_m"], bathy_b.const(8.0))))

    # Bed gradient, metres of depth per metre of distance from the waterline. The
    # max() is not a fudge: within half a source pixel of the shoreline the
    # denominator is genuinely near zero and the ratio is meaningless there --
    # 0.5 m pins it to the finest distance the 1.875 m raster can resolve.
    shelf_lo = scalar_param("BathyFoamShelfLo", 0.05, -1300, 240)   # 1:20, foams fully
    shelf_hi = scalar_param("BathyFoamShelfHi", 0.25, -1300, 300)   # 1:4, does not foam
    bed_slope = bathy_b.div(bathy["depth_m"],
                            bathy_b.maximum(bathy["shore_m"], bathy_b.const(0.5)))
    shelf_gate = bathy_b.one_minus(bathy_b.ramp(bed_slope, "", shelf_lo, shelf_hi))

    # THE A/B ARM LANDS HERE, ON THE GAIN, and nowhere else in the graph. Every
    # node above and below is created identically in both arms, so the OFF
    # material differs from the ON material by ONE FLOAT. That is the property
    # that makes the pair readable: any difference in the two captures is this
    # gain, not a re-authored graph that also happened to change something else.
    #
    # IT IS shore_gain SPECIFICALLY, not the whole foam chain. slope_foam and
    # activity_foam describe MOVING water (a spilling front, a CA-active cell)
    # and are ~0 on a settled pond anyway, so including them would widen the arm
    # without changing the picture.
    #
    # WHAT ELSE MOVES WITH IT, STATED BECAUSE IT IS NOT OBVIOUS FROM THE NAME:
    # Opacity is saturate(foam), so the shore foam is also the only thing making
    # the water OPAQUE in the shore strip -- up to a 55% dim of the volume there.
    # Turning the gain to 0 therefore removes the foam AND that dimming together.
    # They are one mechanism rather than two, so the A/B is honest as long as
    # nobody reads the pair as "foam only".
    shore_gain = scalar_param("BathyFoamGain", 0.55 if SHORE_FX else 0.0, -1300, 360)
    shore_foam = bathy_b.mul(bathy_b.mul(bathy_b.mul(shore_band, shelf_gate), shore_gain),
                             bathy["validity"])

    # MAX, not add: the signals describe the same physical thing from different
    # directions, and a steep, active front should be fully foamed rather than
    # 1.6x foamed and clipped -- clipping would flatten the distinction between
    # "quite foamy" and "extremely foamy" over most of the interesting range.
    foam_raw = mel.create_material_expression(material, unreal.MaterialExpressionMax, -340, -60)
    if not mel.connect_material_expressions(slope_foam, "", foam_raw, "A"):
        raise RuntimeError("connect slope_foam -> foam_raw.A failed")
    if not mel.connect_material_expressions(activity_foam, "", foam_raw, "B"):
        raise RuntimeError("connect activity_foam -> foam_raw.B failed")
    foam_raw = bathy_b.maximum(foam_raw, shore_foam)

    # SIGNAL 4: BREAKING WAVES, from the wave field built ~500 lines above.
    #
    # MAX AND NOT ADD, for the reason stated at foam_raw just above -- these
    # describe one physical thing from four directions, and a wave breaking over
    # an already-foamy shore should be fully white rather than 2x white and
    # clipped, which would flatten the distinction between "quite foamy" and
    # "extremely foamy" over most of the interesting range.
    #
    # THE PERMANENT-WHITE-RING DISCIPLINE APPLIES TO THIS TERM TOO and it is
    # gated four ways inside the wave node rather than here: the band is
    # proportional to the wave (no wave, no band, at any damping depth), the foam
    # rides the CRESTS so it travels with them instead of being a painted stripe,
    # an absolute 5 cm size gate sits under the proportional one, and the whole
    # thing is multiplied by the bathymetry validity so water with no baked depth
    # looks exactly as it does today. See water_wave_graph.py, "THREE GATES ON
    # THE FOAM".
    #
    # ON THE SHIPPED ARM THIS IS LIVE: BreakFoamGain is 1.0. Under
    # VOXEL_WATER_LEGACY_WAVES=1 it is 0.0 and this contributes exactly zero.
    #
    # AND IT MOVES OPACITY WITH IT, which is not obvious from the name: Opacity
    # is saturate(foam), so anything raising foam also makes the water more
    # opaque there. That pairing is already documented at BathyFoamGain and it
    # applies here identically -- a surf band is also a band where you stop
    # seeing the bed. Correct, for whitewater, and stated so it is not read as a
    # second bug when the shoreline goes light.
    foam_raw = bathy_b.maximum(foam_raw, wave_breaking)

    foam = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -190, -60)
    if not mel.connect_material_expressions(foam_raw, "", foam, "A"):
        raise RuntimeError("connect foam_raw -> foam.A failed")
    if not mel.connect_material_expressions(top_face_mask, "", foam, "B"):
        raise RuntimeError("connect top_face_mask -> foam.B failed")

    # --- W5 (3/3): composite -- BASE COLOUR IS NOW FOAM AND NOTHING ELSE ----
    #
    # BASE COLOUR IS BLACK WHERE THERE IS NO FOAM, and that is correct rather
    # than a placeholder. For a Single Layer Water surface, BaseColor is the
    # albedo of the thin diffuse layer sitting ON the water -- the engine adds
    # the volume's colour separately, from the coefficients wired above. Putting
    # a blue in here would be double-counting: the water would get a blue
    # diffuse SURFACE plus a blue volume behind it, which reads as blue paint
    # over blue water and is precisely the "flat tinted film" complaint this
    # whole port exists to fix. Epic's own water material does the same thing
    # for the same reason.
    #
    # Foam is the exception and the only thing that belongs here. Whitewater is
    # a dense scattering layer floating on the surface: it genuinely is a
    # diffuse albedo, it genuinely should catch the sun and the sky, and it
    # genuinely should not be tinted by the volume underneath it. So the entire
    # foam graph above -- slope, CA activity, the top-face mask, all unchanged
    # from W5 -- now lands on BaseColor as a lerp up from black, and a settled
    # lake with zero foam has a black BaseColor, which is exactly right.
    foam_tint = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -190, -420)
    foam_tint.set_editor_property("constant", unreal.LinearColor(0.82, 0.90, 0.94, 1.0))
    water_base = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -190, -360)
    water_base.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, 0.0, 1.0))
    tinted = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -40, -450)
    if not mel.connect_material_expressions(water_base, "", tinted, "A"):
        raise RuntimeError("connect water_base -> tinted.A failed")
    if not mel.connect_material_expressions(foam_tint, "", tinted, "B"):
        raise RuntimeError("connect foam_tint -> tinted.B failed")
    if not mel.connect_material_expressions(foam, "", tinted, "Alpha"):
        raise RuntimeError("connect foam -> tinted.Alpha failed")

    # AO stays the LAST multiply on base colour, exactly where it was. It is a
    # geometric occlusion term from the greedy mesher and applies to whatever
    # colour the surface ended up being. It now only ever attenuates foam, which
    # is the one thing on BaseColor -- and that is still the behaviour wanted: a
    # spilling front wedged into an occluded corner should not be the brightest
    # thing in the frame.
    ao_multiply = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 120, -450)
    if not mel.connect_material_expressions(tinted, "", ao_multiply, "A"):
        raise RuntimeError("connect tinted -> ao_multiply.A failed")
    if not mel.connect_material_expressions(vertex_color, "G", ao_multiply, "B"):
        raise RuntimeError("connect vertex_color.G -> ao_multiply.B failed")
    if not mel.connect_material_property(ao_multiply, "", unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("connect ao_multiply -> BaseColor failed")

    # --- W6: THE STILL-WATER SURFACE ---------------------------------------
    #
    # WHAT THIS FIXES, stated against a measurement rather than an impression.
    # The W5 material gives still water a colour and an opacity but no SURFACE:
    # nothing in the graph above depends on where the viewer is standing, so a
    # settled basin composites as a flat tinted film and reads as haze over the
    # ground rather than as water. Measured at the tile (-12,-5) lake, camera
    # 2.4 m over the 365.1 m datum, against a no-water control frame at the same
    # pose: the shipped still surface moves the frame from mean RGB
    # (215.6, 207.0, 196.2) to (155.6, 152.5, 149.6), 99.99% of pixels differing
    # by more than 25. It was never invisible. It was a grey wash.
    #
    # THE CORRECTION THIS CARRIES. An earlier reading of the same lake concluded
    # that vertex colour A -- the foam activity -- was "what makes water visible"
    # and that a still surface therefore drew nothing. That is not what the graph
    # does: foam is a LERP on top of an already-complete colour and opacity (see
    # `tinted` and `opacity`), and with foam at 0 the opacity output is still
    # 0.55..0.72. The A/B that produced the claim compared Activity 0 against
    # Activity 1 and never took a no-water control, so "different from the foamed
    # frame" was read as "absent". Against the control it is the ACTIVITY-0 frame
    # that differs more (mean 60.0 vs 21.9). Nothing here rides the foam channel,
    # and nothing needed to.
    #
    # WHAT A SURFACE NEEDS THAT A FILM DOES NOT HAVE: a view-dependent term.
    # Fresnel is the whole of it -- water is nearly transparent looking straight
    # down and nearly a mirror at a grazing angle, and that single fact is what
    # the eye reads as "liquid surface" before any wave or glint registers.
    #
    # SORT-KEY POSITION, because the module docstring makes this the standing
    # question for any addition here. This reads NO scene colour, so the ban that
    # rules out refraction is not engaged. It does add a view-dependent term to
    # colour and opacity, which the depth tint already did -- but note it is
    # strictly weaker than the depth tint on this axis: two overlapping water
    # fragments at the same pixel share a view ray and a normal, so Fresnel gives
    # them the SAME value, where SceneDepth deliberately gives them different
    # ones. It introduces no ordering sensitivity the buckets do not already
    # carry.
    # sky_collection was loaded at the top of this function, where the Phase 3
    # bathymetry block first binds to it. ONE LOAD, deliberately: two would be two
    # objects that could in principle disagree about which parameters exist, and
    # the check below is only as good as the object it reads back from.

    # EVERY COLLECTION BINDING IN THIS FILE GOES THROUGH HERE, AND IT CHECKS THE
    # NAME AGAINST THE ASSET AS READ BACK.
    #
    # This closes the one gap create_sky_material.py's ordering note left open. That
    # file says an unresolved CollectionParameter does not fail to compile, it
    # compiles to a CONSTANT (MaterialExpressions.cpp:17179-17193), and that both
    # sky generators re-check every binding by name for exactly that reason. THIS
    # generator never did -- it set parameter_name and hoped -- which is why the
    # 2026-08-10 failure had to be diagnosed from the owner's "I don't see any lake
    # basins" rather than from a raised exception at authoring time.
    #
    # A typo or a stale MPC now raises HERE, naming the parameter and listing what
    # the collection actually has, which is a thirty-second fix instead of a
    # debugging session. Note the two failure shapes it catches are different: a
    # DELETED parameter raises, and so does a parameter this script expects that a
    # not-yet-regenerated MPC does not have -- which is precisely the ordering
    # mistake documented at the top of create_sky_material.py.
    def collection_param(name, x, y):
        have = [str(p.get_editor_property("parameter_name"))
                for p in sky_collection.get_editor_property("scalar_parameters")]
        have += [str(p.get_editor_property("parameter_name"))
                 for p in sky_collection.get_editor_property("vector_parameters")]
        if name not in have:
            raise RuntimeError(
                "MPC_VoxelSky has no parameter %r -- it has %s. If you just added it to "
                "create_sky_material.py's SCALAR_PARAMS/VECTOR_PARAMS, you have to RE-RUN that "
                "script (and then the dome, then this one, in that order -- see the ordering note "
                "at the top of create_sky_material.py). An unresolved CollectionParameter does not "
                "fail to compile, it compiles to a constant, so this check is the only thing "
                "between a typo and a silently dead term."
                % (name, sorted(have)))
        node = mel.create_material_expression(
            material, unreal.MaterialExpressionCollectionParameter, x, y)
        node.set_editor_property("collection", sky_collection)
        node.set_editor_property("parameter_name", name)
        return node

    # SunDirection is written every frame by VoxelSkySubsystem (ApplySkyMaterial
    # parameters). Reusing it rather than a constant is what keeps the glint on
    # the actual sun: the water tracks sunrise and sunset for free, and a frozen
    # -TimeScale 0 capture gets the sun the rest of the frame was lit by.
    sun_dir_param = collection_param("SunDirection", -1300, 1300)

    sun_dir3 = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -1120, 1300)
    sun_dir3.set_editor_property("r", True)
    sun_dir3.set_editor_property("g", True)
    sun_dir3.set_editor_property("b", True)
    sun_dir3.set_editor_property("a", False)
    if not mel.connect_material_expressions(sun_dir_param, "", sun_dir3, ""):
        raise RuntimeError("connect sun_dir_param -> sun_dir3 failed")

    # SUN GLINT, computed analytically instead of left to the engine's specular.
    # The material already sets Specular 0.5 / Roughness 0.08 and that survives
    # unchanged, but a translucent surface's specular response is the part of
    # translucent lighting that is least reliable to author against -- the W3
    # note above records the same lesson from the other side, where the
    # volumetric lighting mode ignored the material normal outright. reflect(V)
    # dot SunDirection is not subject to any of that: it is the mirror direction
    # against this material's own rippled normal, so the glint lands exactly
    # where the wave that produced it is.
    refl_vec = mel.create_material_expression(material, unreal.MaterialExpressionReflectionVectorWS, -1120, 1450)

    # ======================================================================
    # THE GLINT IS A REPRESENTATIVE-POINT AREA LIGHT (2026-08-11), NOT A DOT
    # PRODUCT RAISED TO A LARGE POWER.
    #
    # WHAT WAS WRONG WITH THE OLD ONE, in the terms the old comment set for
    # itself. It computed pow(saturate(dot(R, SunDirection)), 900) and justified
    # the 900 as "the sun subtends about half a degree", which is true and is
    # also not what that expression does. A Phong lobe of exponent 900 has a
    # half-width of roughly acos(0.5^(1/900)) ~= 2 degrees, but it is a smooth
    # peak with no flat top, so the highlight it returns is a fading point, and
    # it never returns the sun's actual angular SIZE. Against the old ripple --
    # whose maximum surface tilt was about 4 degrees -- the mirror ray barely
    # moved, so the result was a single small dot rather than the streak the
    # moon-glint note below assumes.
    #
    # THE FIX, which is what Sea of Thieves ships. Treat the light as a SPHERE
    # of angular radius alpha instead of a point. For each pixel, find the point
    # on that sphere closest to the mirror ray and evaluate the lobe against
    # THAT direction:
    #
    #     centerToRay = dot(L, R) * R - L        (perpendicular from L to the ray)
    #     closest     = L + centerToRay * saturate(sinAlpha / |centerToRay|)
    #
    # When the mirror ray already points inside the disc the clamp does nothing
    # and the result is exactly 1, so the highlight gets a FLAT TOP of the
    # light's real angular size with the old lobe as its falloff skirt. That is
    # the shape a real specular return off water has, and it is what lets the
    # wave slope distribution smear it into a path instead of scattering a field
    # of dots.
    #
    # ENERGY IS NOT INVENTED. Widening a light without renormalising makes the
    # highlight brighter as well as bigger, which is how this technique usually
    # goes wrong. Karis's sphere normalisation (a / (a + sinAlpha))^2, with the
    # Phong exponent mapped to a roughness-like a = 2/(exp+2), is applied so the
    # integral is preserved: at the shipped 0.55 deg the factor is ~0.98, so the
    # sun highlight is essentially unchanged in total energy and only changed in
    # shape. It matters much more for the moon at 2.4 deg, where it is ~0.72.
    #
    # ONE NODE, TWO CALLERS. The sun and the moon differ by the light direction
    # and the angular radius and by nothing else, so this is written once and
    # instantiated twice -- the same discipline the reused glint_tint below
    # already applies to the colour.
    GLINT_CODE = """
// Representative-point (closest-point-on-sphere) area specular.
// R is the mirror direction about this material's own rippled normal, so the
// highlight lands on the wave that produced it. L is the light direction.
float3 Rn = normalize(R);
float3 Ln = normalize(L);
float  sinAlpha = sin(radians(max(AngularRadiusDeg, 0.0)));
float3 centerToRay = dot(Ln, Rn) * Rn - Ln;
float  ctrLen = length(centerToRay);
float3 closest = Ln + centerToRay * saturate(sinAlpha / max(ctrLen, 1e-5));
float  d = saturate(dot(Rn, normalize(closest)));
// Karis sphere normalisation: widening the source must not add energy.
float  a = 2.0 / max(SpecExponent + 2.0, 2.0);
float  n = a / max(a + sinAlpha, 1e-5);
return pow(d, max(SpecExponent, 1.0)) * n * n;
"""

    # The exponent is SHARED by both glints and is a parameter now rather than a
    # baked 900. It sets the width of the falloff skirt around the flat top; the
    # flat top itself is the angular radius. 900 is kept as the default so a
    # regeneration with no instance overrides changes the skirt by nothing.
    glint_exponent = scalar_param("GlintSpecularExponent", 900.0, -1300, 1240)
    sun_glint_radius = scalar_param("SunGlintAngularRadiusDeg", 0.55, -1300, 1180)

    glint_pow = custom_node(
        "SunGlint", GLINT_CODE, ["R", "L", "AngularRadiusDeg", "SpecExponent"],
        -800, 1380, unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    if not mel.connect_material_expressions(refl_vec, "", glint_pow, "R"):
        raise RuntimeError("connect refl_vec -> sun glint.R failed")
    if not mel.connect_material_expressions(sun_dir3, "", glint_pow, "L"):
        raise RuntimeError("connect sun_dir3 -> sun glint.L failed")
    if not mel.connect_material_expressions(sun_glint_radius, "", glint_pow, "AngularRadiusDeg"):
        raise RuntimeError("connect sun_glint_radius -> sun glint.AngularRadiusDeg failed")
    if not mel.connect_material_expressions(glint_exponent, "", glint_pow, "SpecExponent"):
        raise RuntimeError("connect glint_exponent -> sun glint.SpecExponent failed")

    # Slightly over 1 and slightly warm: a specular sun return is brighter than
    # the sky it sits in, and clamping it to 1 is what makes a highlight read as
    # a painted white dot rather than as light.
    glint_tint = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -650, 1520)
    glint_tint.set_editor_property("constant", unreal.LinearColor(2.6, 2.45, 2.15, 1.0))
    glint = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -500, 1440)
    if not mel.connect_material_expressions(glint_pow, "", glint, "A"):
        raise RuntimeError("connect glint_pow -> glint.A failed")
    if not mel.connect_material_expressions(glint_tint, "", glint, "B"):
        raise RuntimeError("connect glint_tint -> glint.B failed")

    # ======================================================================
    # MOON GLINT -- the moon path on the water. Added 2026-08-11 because the
    # owner asked why the moon does not reflect off the lake.
    #
    # THERE WERE TWO REASONS IT DID NOT, and this material was only one of them.
    # The other was in C++ and is the bigger one: both directional lights sat at
    # ForwardShadingPriority 0, so the renderer picked the single forward /
    # translucent light by raw brightness and handed it to the SUN at midnight
    # (LightGridInjection.cpp:1500-1520). This material is BLEND_TRANSLUCENT with
    # TLM_SURFACE_PER_PIXEL_LIGHTING, so its direct lighting comes from exactly
    # that one light -- a sun below the horizon, N.L clamped to zero, no direct
    # light on the lake all night from either body. That half is fixed in
    # UVoxelSkySubsystem::ApplyLightsFromState; see kForwardMoonPrimarySunBelowDeg.
    # This half is the SPECULAR half, and neither alone is enough.
    #
    # STRUCTURALLY IDENTICAL TO THE SUN GLINT ABOVE, deliberately, down to the
    # exponent and the tint. Every difference between them is one multiply. The
    # things it therefore inherits for free: the same reflect(V) mirror direction
    # against this material's own rippled normal, so the moon path breaks up over
    # waves exactly as the sun's does; and the same reason for computing it
    # analytically rather than trusting translucent specular.
    #
    # THE ANGULAR RADIUS IS 2.4 DEGREES, WHICH REVERSES AN EARLIER DECISION HERE,
    # AND THE REASON IT CAN BE REVERSED IS THAT THE MECHANISM CHANGED.
    #
    # What this comment used to say: the exponent stays at the sun's 900, because
    # the real moon subtends the same ~0.5 deg the sun does, and because widening
    # a Phong LOBE is the way to turn a highlight into a plate of wet plastic
    # across the whole basin. Both halves of that were right about a Phong lobe.
    #
    # The glint is no longer a Phong lobe. It is a representative-point area light
    # (see the sun glint above), which separates two things the exponent used to
    # conflate: the light's angular SIZE, which is now the flat top, and the
    # falloff SKIRT, which is still the exponent and is still 900 and is still
    # shared with the sun. Widening the size no longer widens the skirt, so the
    # wet-plastic failure mode is not on the table -- the highlight gets a 2.4 deg
    # core and the same tight edge it always had, and Karis normalisation takes
    # its peak brightness down by ~28% to pay for the extra area.
    #
    # 2.4 deg is deliberately the project's DRAWN moon, not the real one. The moon
    # in this sky is enlarged about nine times (kMoonDrawnAngularRadiusDeg), and a
    # path on the water whose source is 0.5 deg while the disc above it is 2.4 deg
    # is two different moons in the same frame. The disc is the one the owner sees,
    # so the water agrees with the disc. That is the enlarged-disc cheat being
    # applied consistently rather than a second cheat.
    #
    # THE TINT IS THE SUN'S TINT SCALED BY MoonLightFraction, and that one multiply
    # is the whole physical content of this block. MoonLightFraction is written every
    # frame by UVoxelSkySubsystem::ApplySkyMaterialParams as
    # S.MoonIntensity / GetSunIntensity() -- "how much dimmer than the sun the moon
    # is, right now". So the moon's highlight is the sun's calibrated highlight,
    # dimmed by exactly the ratio the two LIGHTS are dimmed by. Nothing here needs
    # tuning and nothing here can drift from the lighting rig.
    #
    # WHAT THAT ONE SCALAR CARRIES, all of it from the C++ side with no logic here:
    #   * moonset      -- MoonHorizonGate is already inside S.MoonIntensity, so the
    #                     path fades out as the moon sets and is gone once it is down
    #   * new moon     -- MoonIlluminatedFraction is in there too, so a new moon
    #                     lays no path at all
    #   * daylight     -- the sun-suppression term zeroes it before sunrise
    #   * the owner's knob -- voxel.Sky.MoonIntensity scales it, so dialling the
    #                     moonlight moves the water and the ground by the same stops
    # Reconstructing any of those from MoonDirection and MoonPhaseFraction in this
    # graph was the alternative, and it would have been a second copy of
    # ApplyLightsFromState living in a Python asset generator, unable to see the
    # cvar, and wrong in a way no log line would report.
    moon_dir_param = collection_param("MoonDirection", -1300, 1900)

    moon_dir3 = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -1120, 1900)
    moon_dir3.set_editor_property("r", True)
    moon_dir3.set_editor_property("g", True)
    moon_dir3.set_editor_property("b", True)
    moon_dir3.set_editor_property("a", False)
    if not mel.connect_material_expressions(moon_dir_param, "", moon_dir3, ""):
        raise RuntimeError("connect moon_dir_param -> moon_dir3 failed")

    # Scalar. NOT masked -- a CollectionParameter bound to a SCALAR compiles as a
    # float and a ComponentMask on it is both unnecessary and a pin-type mismatch
    # waiting to happen. The vector ones above are masked only to drop the unused
    # .a that FLinearColor forces on them.
    moon_light_fraction = collection_param("MoonLightFraction", -1300, 2060)

    # SAME Custom node source as the sun glint, instantiated a second time. The
    # only two differences are the light direction and the angular radius, which
    # is what "structurally identical, every difference is one multiply" was
    # always meant to mean -- and now the two cannot drift apart in their MATH
    # either, only in their two inputs, because there is one GLINT_CODE string.
    moon_glint_radius = scalar_param("MoonGlintAngularRadiusDeg", 2.4, -1300, 2120)

    moon_glint_pow = custom_node(
        "MoonGlint", GLINT_CODE, ["R", "L", "AngularRadiusDeg", "SpecExponent"],
        -800, 1900, unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    if not mel.connect_material_expressions(refl_vec, "", moon_glint_pow, "R"):
        raise RuntimeError("connect refl_vec -> moon glint.R failed")
    if not mel.connect_material_expressions(moon_dir3, "", moon_glint_pow, "L"):
        raise RuntimeError("connect moon_dir3 -> moon glint.L failed")
    if not mel.connect_material_expressions(moon_glint_radius, "", moon_glint_pow, "AngularRadiusDeg"):
        raise RuntimeError("connect moon_glint_radius -> moon glint.AngularRadiusDeg failed")
    if not mel.connect_material_expressions(glint_exponent, "", moon_glint_pow, "SpecExponent"):
        raise RuntimeError("connect glint_exponent -> moon glint.SpecExponent failed")

    moon_glint_scaled = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -500, 1960)
    if not mel.connect_material_expressions(moon_glint_pow, "", moon_glint_scaled, "A"):
        raise RuntimeError("connect moon_glint_pow -> moon_glint_scaled.A failed")
    if not mel.connect_material_expressions(moon_light_fraction, "", moon_glint_scaled, "B"):
        raise RuntimeError("connect moon_light_fraction -> moon_glint_scaled.B failed")

    # glint_tint REUSED, not copied. One node, one calibration, and the moon glint
    # cannot acquire a different colour balance from the sun glint by an edit that
    # only remembers to change one of them.
    moon_glint = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -380, 1900)
    if not mel.connect_material_expressions(moon_glint_scaled, "", moon_glint, "A"):
        raise RuntimeError("connect moon_glint_scaled -> moon_glint.A failed")
    if not mel.connect_material_expressions(glint_tint, "", moon_glint, "B"):
        raise RuntimeError("connect glint_tint -> moon_glint.B failed")

    # SKY REFLECTION. A constant sky colour gated by sun altitude, NOT a scene
    # capture and NOT a reflection probe: the ban in the module docstring is on
    # reading scene COLOUR, and a probe read is the same hazard wearing a
    # different name. SunDirection.z is the sine of the sun's altitude, so
    # saturate() of it is 0 from dusk to dawn and the lake stops reflecting a
    # blue sky it cannot see -- the one piece of time-of-day behaviour this term
    # genuinely needs, and it costs one node.
    sun_alt = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, -1120, 1600)
    sun_alt.set_editor_property("r", False)
    sun_alt.set_editor_property("g", False)
    sun_alt.set_editor_property("b", True)
    sun_alt.set_editor_property("a", False)
    if not mel.connect_material_expressions(sun_dir_param, "", sun_alt, ""):
        raise RuntimeError("connect sun_dir_param -> sun_alt failed")

    day_gate = mel.create_material_expression(material, unreal.MaterialExpressionSaturate, -950, 1600)
    if not mel.connect_material_expressions(sun_alt, "", day_gate, ""):
        raise RuntimeError("connect sun_alt -> day_gate failed")

    # KNOWN LIMITATION, stated rather than left to be discovered, and the exact
    # counterpart of the SceneDepth-against-sky note in the depth section above.
    # saturate(SunDirection.z) is "is the sun up", which is not the same question
    # as "can THIS surface see the sky". A static cavern pool a hundred metres
    # underground at noon therefore still gets a sky-blue reflection at grazing
    # angles, from a sky it has no line of sight to. It is Fresnel-weighted, so
    # looking down into the pool -- how a cavern pool is normally met -- it is
    # near zero, and it is the same magnitude a surface lake gets. Fixing it
    # properly needs a sky-visibility signal that does not exist on this vertex
    # format: VertexColor.G is the greedy mesher's local AO, which is ~1 in the
    # middle of any chamber large enough to hold a pool and so cannot express it.
    # NOT VERIFIED IN A CAPTURE: -VoxelFloodTest found no flooded cavern at the
    # lake site, and the default cavern site's fine tiles are absent from this
    # box's cache (tile (-6,3) at s16, absentOnDisk=1). Downgrading the fine-tier
    # gate to get a frame would have made that frame unreproducible, which is the
    # one thing the gate exists to prevent.
    sky_tint = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -950, 1700)
    sky_tint.set_editor_property("constant", unreal.LinearColor(0.30, 0.46, 0.72, 1.0))
    sky_lit = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -800, 1650)
    if not mel.connect_material_expressions(sky_tint, "", sky_lit, "A"):
        raise RuntimeError("connect sky_tint -> sky_lit.A failed")
    if not mel.connect_material_expressions(day_gate, "", sky_lit, "B"):
        raise RuntimeError("connect day_gate -> sky_lit.B failed")

    # THE NIGHT HALF OF THE SAME REFLECTION, and the second reason the lake looked
    # dead after dark.
    #
    # saturate(SunDirection.z) above is EXACTLY ZERO from dusk to dawn. That was
    # correct as far as it went -- the lake should not mirror a blue daytime sky it
    # cannot see -- but the consequence was that the Fresnel reflection, the term
    # the comment above calls "what the eye reads as a liquid surface before any
    # wave or glint registers", switched off completely every night. A surface with
    # no reflection at a grazing angle does not read as water at any hour. Note this
    # is INDEPENDENT of the moon glint added above: the glint is a specular
    # highlight a few degrees wide, and this is the broad sheen across the whole
    # basin. Fixing only one of them leaves the lake looking wrong in the other way.
    #
    # THE NIGHT SKY IS THE DAY SKY TIMES MoonLightFraction, with no new constant.
    # sky_tint is the daylit sky's reflected colour; the night sky is lit by the
    # moon exactly as the day sky is lit by the sun, so scaling by
    # (moon light / sun light) is not an approximation of convenience, it is the
    # same ratio the two skies actually stand in -- inside this project's chosen
    # moon cheat, which is the only frame of reference that matters here. At the
    # current defaults that puts the reflected sky ~1.8 stops below its daytime
    # self once the exposure curve's night lift is counted, which sits alongside the
    # ground's -2.0 stops rather than fighting it. Re-using the SAME scalar the moon
    # glint uses means the broad sheen and the highlight can never be tuned into
    # disagreement, and both vanish together at moonset and at new moon.
    #
    # THE COLOUR IS DELIBERATELY NOT SHIFTED BLUE. It is tempting to hand the night
    # branch its own cooler tint, and this file will not: the moon's colour is
    # settled in C++ from a measurement of T_MoonColor (kMoonAlbedoTint) and is
    # deliberately NOT the old 12000 K blue, with voxel.Sky.MoonTintStrength as the
    # A/B. A blue invented here would be a second, unreachable opinion about the
    # same question, and it would win in the one place the owner is most likely to
    # be looking at when he forms a view about the night's colour.
    #
    # WHAT THIS DOES NOT DO: a moonless but starlit night still gets no sheen from
    # this term, because MoonLightFraction is 0. That is a real gap and it is left
    # open rather than papered over with an invented starlight floor -- the honest
    # value for one is a measurement nobody has taken. Starlight is NOT absent from
    # the water in that case: the SkyLight's real-time capture carries the star term
    # (voxel.Sky.StarAmbientGain, routed into the capture through
    # M_SkyAtmosphereDome's ReflectionCapturePassSwitch) and reaches this surface as
    # DIFFUSE ambient on BaseColor. What it cannot do is produce a mirrored sky at a
    # grazing angle. Giving the lake literal reflected STARS -- individual points,
    # moving with the sidereal rotation -- means sampling T_SkyStarmap along the
    # reflection vector via Tools/sky_star_graph.py's sample_starmap, which is
    # allowed (it is a texture read, not a scene-colour read, so no ban is engaged)
    # but adds the whole star subgraph to a TRANSLUCENT material drawn over very
    # large screen areas by the far-field sheets. That is a perf decision on a frame
    # already measured as render-thread bound, so it is a deliberate non-goal here
    # rather than an oversight.
    night_sky = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -800, 1780)
    if not mel.connect_material_expressions(sky_tint, "", night_sky, "A"):
        raise RuntimeError("connect sky_tint -> night_sky.A failed")
    if not mel.connect_material_expressions(moon_light_fraction, "", night_sky, "B"):
        raise RuntimeError("connect moon_light_fraction -> night_sky.B failed")

    # ADD, not LERP. The two branches are already mutually exclusive in practice --
    # the day gate is 0 whenever the sun is down and MoonLightFraction is 0 whenever
    # the sun is up (ApplyLightsFromState's sun-suppression term is what guarantees
    # the second one) -- so a lerp would need a third gate to express something the
    # inputs already express, and would be a place for the two to disagree.
    sky_total = mel.create_material_expression(material, unreal.MaterialExpressionAdd, -680, 1700)
    if not mel.connect_material_expressions(sky_lit, "", sky_total, "A"):
        raise RuntimeError("connect sky_lit -> sky_total.A failed")
    if not mel.connect_material_expressions(night_sky, "", sky_total, "B"):
        raise RuntimeError("connect night_sky -> sky_total.B failed")

    # ======================================================================
    # REFLECTED STARS (2026-08-11) -- the gap the block above names, closed.
    #
    # The night branch immediately above stops at a flat scaled sky colour, and
    # its own comment says why that is not the whole answer: a moonless night
    # gets no sheen at all, because MoonLightFraction is 0, and even a moonlit
    # one gets a featureless wash where the eye expects points of light. The
    # comment then names the fix and calls it a deliberate non-goal on perf
    # grounds. This block does it, and the perf question is now answered with a
    # measurement instead of an estimate rather than left as an assumption --
    # which is the whole reason STAR_REFLECTION above is a build-time arm.
    #
    # WHAT IS ALLOWED HERE AND WHAT IS NOT. The standing ban in the module
    # docstring is on reading scene COLOUR: refraction, planar reflections, an
    # SSR probe. All three are banned because the value they read IS the
    # partially composed translucent stack, so the answer depends on draw order
    # and two water fragments at one pixel disagree. A TEXTURE fetch along the
    # reflection vector has none of that: T_SkyStarmap is the same texture
    # whatever else has been drawn, so every fragment computes the same answer in
    # any order. The sort-key argument the Fresnel block makes a few lines up
    # applies here word for word.
    #
    # THE DIRECTION IS refl_vec, THE SAME NODE THE TWO GLINTS USE. That is not a
    # saving of one node, it is the guarantee that the mirrored sky and the two
    # mirrored light sources are all mirrored about the SAME rippled normal. If
    # the stars used their own reflection the moon path could sit in one place
    # and the reflected star field in another, on the same wave.
    #
    # THE HORIZON FADE IS DOING REAL WORK HERE, not carried along from the dome.
    # build_horizon_fade smoothsteps on the Z of the direction it is handed, and
    # the direction handed to it here is the MIRROR ray, not the gaze. So it
    # asks "does the mirror ray point at the sky or into the ground", and kills
    # the term when the answer is the ground: on the underside of a surface seen
    # from below (this material is two_sided), and on the far face of a ripple
    # steep enough to turn the mirror ray downward. Without it those pixels would
    # sample the star map's southern rows and show stars coming out of the lake
    # bed.
    #
    # THE NIGHT GATE IS StarBrightness, AND IT IS NOT MoonLightFraction.
    # StarBrightness is the MPC scalar C++ already writes every frame as the
    # sunrise fade (1 below about -12 deg solar altitude, 0 above 0 deg,
    # smoothstepped between -- VoxelSkySubsystem StarBrightnessForSunAltitude,
    # times voxel.Sky.StarGain). Using it means:
    #   * the reflected stars appear and fade at EXACTLY the moment the real ones
    #     do, because it is the same scalar M_NightSky's own gain uses, and
    #   * they survive a new moon and a moonset, which is the precise gap the
    #     block above documents and could not close with MoonLightFraction --
    #     that scalar is 0 on a moonless night, and a moonless night is when
    #     stars are most visible, not least.
    # Scaling this by MoonLightFraction as well would have been "consistent with
    # the night sky term" in wording and backwards in physics.
    #
    # BRIGHTNESS IS FRESNEL AND NOTHING ELSE. This term is added into sky_total,
    # so the multiply by `fresnel` a few lines below is the only scaling it gets:
    # about 0.02 looking straight down, rising toward 1 at a grazing angle. That
    # IS the reflectance of water, so no invented constant is needed and the
    # still-water grazing view the owner asked about -- where Fresnel is near 1 --
    # is exactly the case that shows the most stars. StarReflectGain is a plain
    # material scalar (default 1.0, i.e. physically neutral) left in as the one
    # knob, so the term can be dimmed or brightened from a material instance
    # without another regeneration.
    #
    # WHAT THIS WILL AND WILL NOT LOOK LIKE, stated now so a capture is not read
    # as a bug. The star map is sampled with EXPLICIT derivatives, so the mip is
    # chosen from how fast the reflection vector varies across the screen. On
    # STILL water the normal barely changes, the derivatives are tiny and the
    # stars are near-point sharp. On chop the mirror ray swings by degrees per
    # pixel, a low mip is selected, and the star field correctly degrades to a
    # broad glow rather than to a field of aliasing sparkle. Both are right; only
    # the first is the picture anyone imagines when they ask for this.
    #
    # THE ONE KNOWN ARTEFACT. sky_star_graph's seam fix subtracts round(du/dx),
    # which assumes a real derivative is far below 0.5 turns per pixel. On a
    # steep ripple near the celestial poles that assumption can fail and the mip
    # comes out one or two levels too sharp for a few pixels. On the DOME that
    # case cannot arise (the gaze direction is smooth); here it can. The visible
    # consequence is a little shimmer in the reflection of the polar sky on
    # rough water, and it is bounded by the same star field being dim there.
    sky_reflected = sky_total
    if STAR_REFLECTION:
        # SkyGraphBuilder rather than raw mel calls, because build_star_uv and
        # sample_starmap are written against it. It brings its own checked
        # connects and its own checked CollectionParameter binding, which is the
        # same guarantee collection_param() above gives this file's own nodes.
        b = SkyGraphBuilder(material, sky_collection)

        # Normalized explicitly. ReflectionVectorWS is unit length in practice,
        # but build_star_uv's arcsine reads this vector's Z as sin(dec) directly
        # and a length that is 1.001 is a declination error, not a brightness
        # error -- it would move stars, silently.
        refl_dir = b.normalize(refl_vec)
        refl_z = b.mask(refl_dir, "", b=True)

        star_uv, star_ddx, star_ddy = build_star_uv(b, refl_dir, refl_z)
        starmap = sample_starmap(b, star_uv, star_ddx, star_ddy)

        star_gain = b.mul(b.collection_param("StarBrightness"),
                          build_horizon_fade(b, refl_z))
        star_gain = b.mul(star_gain, b.scalar("StarReflectGain", 1.0))

        # "RGB" explicitly: a TextureSample's DEFAULT output is RGBA, and
        # multiplying a float4 into this chain would carry an alpha nobody wants
        # into the emissive sum. sky_star_graph's own docstring for
        # sample_starmap says to read RGB and not the default, for this reason.
        star_reflection = b.mul(starmap, star_gain, "RGB", "")

        # ADDED to sky_total, and therefore UPSTREAM of Fresnel, of the foam
        # suppression and of the top-face mask below. All three are wanted:
        # stars are a reflection so they must obey Fresnel; whitewater scatters
        # and must not mirror a star field; and a submerged side wall is not a
        # sky-facing surface. Attaching this after the Fresnel multiply would
        # have quietly opted out of all three.
        sky_reflected = b.add(sky_total, star_reflection)

    # Fresnel with water's real normal-incidence reflectance, 0.02. The Normal
    # input is deliberately LEFT UNCONNECTED so the node uses this material's own
    # shading normal -- which is the panning ripple authored in the W3 section
    # below. Connecting `normal_xyz` here instead would be a bug that compiles:
    # that value is TANGENT space and this pin wants world space.
    fresnel = mel.create_material_expression(material, unreal.MaterialExpressionFresnel, -650, 1650)
    fresnel.set_editor_property("exponent", 5.0)
    fresnel.set_editor_property("base_reflect_fraction", 0.02)

    reflection_fresnel = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -500, 1650)
    if not mel.connect_material_expressions(sky_reflected, "", reflection_fresnel, "A"):
        raise RuntimeError("connect sky_reflected -> reflection_fresnel.A failed")
    if not mel.connect_material_expressions(fresnel, "", reflection_fresnel, "B"):
        raise RuntimeError("connect fresnel -> reflection_fresnel.B failed")

    # THE ONE KNOB THE SLW PORT ADDS TO THIS BLOCK, AND WHY IT IS 1.0.
    #
    # Everything above -- the constant sky tint, the day gate, the
    # MoonLightFraction night branch, the reflected stars, the Fresnel weight --
    # was authored for a TRANSLUCENT surface, whose environment specular is the
    # part of translucent lighting the W6 note above calls "least reliable to
    # author against". A Single Layer Water surface is not in that position: it
    # goes through the deferred path and receives the engine's own reflection
    # from reflection captures and the skylight, and this project's skylight is a
    # real-time capture of its own sky and moon. So there is now a SECOND source
    # of reflected sky on this surface, and the two may double up.
    #
    # IT IS NOW MEASURED, AND THEY DO DOUBLE UP. 2026-08-12, the lake at
    # (-65102,-51084), camera 12 m up at pitch -30, noon, same exposure in every
    # arm (a land patch reads 0.1491-0.1494 display-linear in all five). Mid-lake
    # water, display-linear RGB:
    #
    #   both reflections (shipped)        0.245 / 0.388 / 0.510
    #   r.Water.SingleLayer.Reflection 0  0.084 / 0.169 / 0.265   (this term only)
    #   LegacySkyReflectGain 0            0.106 / 0.203 / 0.287   (engine term only)
    #   both off                          0.000 / 0.008 / 0.005   (the volume alone)
    #   the lake bed with no water at all 0.041 / 0.044 / 0.021
    #
    # So each sky reflection is on its own worth about two thirds of the water's
    # brightness, the two together are ~98% of it, and BOTH of them are several
    # times brighter than the lake bed they sit on top of. With both removed the
    # bed is plainly visible through the water and grades with depth exactly as
    # the coefficients above intend -- transmittance measured from the waterline
    # outward is (0.84,0.92,0.89) in the first few centimetres, (0.20,0.51,0.54)
    # a metre or two out, and red-dead-blue-alive beyond that.
    #
    # THE DEFAULT IS 0.0: THE OWNER RETIRED THIS TERM ON 2026-08-12, on the
    # measurements above. It is kept as a parameter rather than deleted so the
    # old look is one instance value away and the arms stay reproducible.
    #
    # WHY THIS ONE AND NOT THE ENGINE'S, since either would remove the double.
    # They are not equivalent, and the difference is exactly the defect:
    #
    #   * The ENGINE'S reflection participates in the energy split. The shader
    #     does ScatteredLuminance *= (1 - EnvBrdf) and Reflection *= EnvBrdf, so
    #     reflection and volume TRADE OFF and sum to <= 1. Turn the water's
    #     depth grading up and the reflection makes room for it.
    #   * THIS term is added to Emissive, which is outside that split -- pure
    #     addition on top of everything else. That is why it did not merely
    #     brighten the water, it BURIED the volume: measured mid-lake the volume
    #     alone reads 0.000/0.008/0.005 against this term's 0.106/0.203/0.287.
    #
    # And the engine's tracks the real sky -- this project's skylight is a
    # real-time capture, so it follows time of day, the moon and cloud, where a
    # constant-sky Fresnel cannot. Removing this term moved the relative
    # structure in the water (SD/mean of luma) from 0.127 to 0.304.
    #
    # THE HISTORY, so nobody re-adds it: this term was written when the water
    # was BLEND_TRANSLUCENT + MSM_DefaultLit, under this project's own ban
    # ("reflections stay constant-sky Fresnel, no dynamic reflection capture").
    # With captures off the table the material had to fake a sky reflection
    # itself. The Single Layer Water port put the water on the deferred path
    # where the engine composites captures and the skylight unconditionally --
    # so the port added the real thing without removing the stand-in. This is
    # the removal.
    #
    # The star reflection rides this gain too, which is correct: the engine's
    # skylight capture already contains the stars via M_SkyAtmosphereDome's
    # ReflectionCapturePassSwitch, so if one is redundant both are.
    legacy_sky_gain = scalar_param("LegacySkyReflectGain", 0.0, -650, 1760)
    reflection = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -420, 1650)
    if not mel.connect_material_expressions(reflection_fresnel, "", reflection, "A"):
        raise RuntimeError("connect reflection_fresnel -> reflection.A failed")
    if not mel.connect_material_expressions(legacy_sky_gain, "", reflection, "B"):
        raise RuntimeError("connect legacy_sky_gain -> reflection.B failed")

    # BOTH GLINTS ARE FRESNEL-FREE, day and night alike. The reflection above is
    # Fresnel-weighted and these are not, and that asymmetry is correct rather than
    # an oversight: Fresnel governs how much of the SKY a surface mirrors, while a
    # specular return off a rippled surface is dominated by the slope distribution
    # -- which the normal already supplies. Weighting the glint by Fresnel too would
    # delete the sun's own reflection when looking straight down at a calm lake at
    # noon, which is the one place everyone has seen it.
    glints = mel.create_material_expression(material, unreal.MaterialExpressionAdd, -340, 1440)
    if not mel.connect_material_expressions(glint, "", glints, "A"):
        raise RuntimeError("connect glint -> glints.A failed")
    if not mel.connect_material_expressions(moon_glint, "", glints, "B"):
        raise RuntimeError("connect moon_glint -> glints.B failed")

    surface_light = mel.create_material_expression(material, unreal.MaterialExpressionAdd, -220, 1540)
    if not mel.connect_material_expressions(reflection, "", surface_light, "A"):
        raise RuntimeError("connect reflection -> surface_light.A failed")
    if not mel.connect_material_expressions(glints, "", surface_light, "B"):
        raise RuntimeError("connect glints -> surface_light.B failed")

    # FOAM IS ADDITIVE ON TOP OF THIS, and this is the pin that makes that true
    # in the direction that matters. Froth is a scattering medium: it is the one
    # part of a water surface that does NOT mirror the sky, and letting the
    # reflection survive underneath it would put a sky sheen on whitewater. The
    # existing foam lerps on colour, opacity and roughness are unchanged and
    # still run after this, so foam continues to win where it is present and
    # contributes nothing at all where it is zero -- which is every still lake
    # and every static cavern pool.
    one_minus_foam = mel.create_material_expression(material, unreal.MaterialExpressionOneMinus, -340, 1660)
    if not mel.connect_material_expressions(foam, "", one_minus_foam, ""):
        raise RuntimeError("connect foam -> one_minus_foam failed")

    surface_unfoamed = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -190, 1600)
    if not mel.connect_material_expressions(surface_light, "", surface_unfoamed, "A"):
        raise RuntimeError("connect surface_light -> surface_unfoamed.A failed")
    if not mel.connect_material_expressions(one_minus_foam, "", surface_unfoamed, "B"):
        raise RuntimeError("connect one_minus_foam -> surface_unfoamed.B failed")

    # Masked to top faces by the SAME top_face_mask the foam uses, for the same
    # reason given there: a submerged side wall is not a sky-facing surface and
    # must not reflect one. This is a normal test, not VertexColor.B -- see the
    # foam section for why B would ring every body with a stripe.
    surface_emissive = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -40, 1600)
    if not mel.connect_material_expressions(surface_unfoamed, "", surface_emissive, "A"):
        raise RuntimeError("connect surface_unfoamed -> surface_emissive.A failed")
    if not mel.connect_material_expressions(top_face_mask, "", surface_emissive, "B"):
        raise RuntimeError("connect top_face_mask -> surface_emissive.B failed")

    # EMISSIVE, not BaseColor. A reflection is light leaving the surface, not
    # albedo: routing it through BaseColor would make it get multiplied by AO and
    # by the diffuse lighting term, so a reflected sky would darken in an
    # occluded corner, which is backwards.
    if not mel.connect_material_property(surface_emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        raise RuntimeError("connect surface_emissive -> EmissiveColor failed")

    # --- OPACITY IS THE SWITCH THAT TURNS THE WHOLE WATER VOLUME ON ---------
    #
    # READ THIS BEFORE DISCONNECTING IT AGAIN. An earlier revision of this file
    # left MP_OPACITY unwired on the stated grounds that "an SLW material is
    # BLEND_OPAQUE and MP_OPACITY is not an active input on it". That is FALSE,
    # and it cost the entire depth grading this material exists to provide.
    #
    # Material.cpp, UMaterial::IsPropertyActive_Internal:
    #     case MP_Opacity:
    #         Active = (bIsTranslucentBlendMode && !IsModulateBlendMode(BlendMode))
    #                  || ShadingModels.HasShadingModel(MSM_SingleLayerWater);
    # -- i.e. Opacity is active on a Single Layer Water material REGARDLESS of
    # blend mode. It is not the alpha of a translucent surface here. It is
    #
    #     BasePassPixelShader.usf:1140
    #         const float BaseMaterialCoverageOverWater = Opacity;
    #         const float WaterVisibility = 1.0 - BaseMaterialCoverageOverWater;
    #
    # ...the fraction of the pixel covered by the OPAQUE MATERIAL SITTING ON the
    # water -- foam, ice, a lily pad -- and the engine's own comment at the call
    # site says so: "Fade out the material contribution over to water
    # contribution according to material opacity."
    #
    # AN UNWIRED INPUT IS NOT A NEUTRAL INPUT. MP_Opacity's default is 1.0
    # (MaterialAttributeDefinitionMap.cpp:401, FVector4(1,0,0,0)), so leaving the
    # pin empty compiles to coverage = 1, WaterVisibility = 0, and
    #
    #     SingleLayerWaterShading.ush:74   if (WaterVisibility > 0.0f)
    #
    # is never entered. EvaluateWaterVolumeLighting returns a zeroed struct and
    # BasePassPixelShader does `Color += 0`. Every single thing wired above --
    # the absorption coefficients, the scattering coefficients, the phase
    # function, ColorScaleBehindWater and therefore the entire baked bathymetry
    # chain -- is computed and thrown away. What is left on screen is the
    # GBuffer half alone: a black BaseColor with Specular 0.5 and Roughness
    # 0.08, i.e. a dark mirror, plus the emissive sky reflection. That renders
    # as a flat, uniform, depth-independent sheet through which NOTHING behind
    # the water is ever visible, at any depth, including a shoreline one voxel
    # deep. It compiles clean, it has no warning, and it looks like a plausible
    # if boring water material, which is why it survived a full review.
    #
    # SO THE CORRECT VALUE IS THE FOAM COVERAGE, and that is not a coincidence
    # or a convenient reuse -- it is the same quantity the engine is asking for.
    # `foam` is already the fraction of the pixel that is whitewater, it is
    # already the alpha of the BaseColor lerp two hundred lines up, and it is
    # already masked to top faces. Where there is no foam this is 0, the volume
    # is fully visible, and the water grades by depth. Where foam is 1 the
    # surface is a lit opaque white layer and the volume is correctly hidden
    # behind it -- which is exactly what the deleted `foam_opacity` node used to
    # say, arriving at the pin the engine actually reads.
    #
    # SATURATED, because foam_raw is a max of terms that are individually in
    # 0..1 but one of which (shore_foam) carries a gain and is deliberately not
    # clipped upstream (see "MAX, not add" above). A coverage above 1 would make
    # WaterVisibility NEGATIVE, which is not a clamp the engine performs.
    water_coverage = bathy_b.saturate(foam)
    if not mel.connect_material_property(water_coverage, "", unreal.MaterialProperty.MP_OPACITY):
        raise RuntimeError(
            "connect water_coverage -> Opacity failed. This is not cosmetic: with MP_OPACITY "
            "unwired the engine reads coverage 1 / WaterVisibility 0 and skips the entire "
            "water volume, so the material ships with no absorption, no scattering and no "
            "transmitted scene behind it.")

    # Roughness tightened slightly (0.1 -> 0.08) and Specular made explicit
    # (0.5 -- the engine's own unconnected-pin default, stated rather than
    # left implicit so the two are visibly tuned as a pair) now that the
    # panning normal ripple below actually reaches the lit result (see the
    # translucency-lighting-mode comment above). A tighter specular lobe
    # turns a moving normal into a moving GLINT rather than a moving blur,
    # which is the cue that reads as "surface in motion" at a glance.
    #
    # W5: roughness is no longer a flat Constant -- it lerps to 0.62 under foam.
    # Froth is the one part of a water surface that is NOT a mirror, and leaving
    # the tight 0.08 lobe on it would put a sharp specular highlight on top of
    # whitewater, which reads as wet plastic. Specular stays flat.
    calm_roughness = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -190, -180)
    calm_roughness.set_editor_property("r", 0.08)
    foam_roughness = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -190, -130)
    foam_roughness.set_editor_property("r", 0.62)
    roughness = mel.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -40, -170)
    if not mel.connect_material_expressions(calm_roughness, "", roughness, "A"):
        raise RuntimeError("connect calm_roughness -> roughness.A failed")
    if not mel.connect_material_expressions(foam_roughness, "", roughness, "B"):
        raise RuntimeError("connect foam_roughness -> roughness.B failed")
    if not mel.connect_material_expressions(foam, "", roughness, "Alpha"):
        raise RuntimeError("connect foam -> roughness.Alpha failed")
    if not mel.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS):
        raise RuntimeError("connect roughness -> Roughness failed")

    specular = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -40, -110)
    specular.set_editor_property("r", 0.5)
    if not mel.connect_material_property(specular, "", unreal.MaterialProperty.MP_SPECULAR):
        raise RuntimeError("connect specular -> Specular failed")

    # --- Stepped fill-fraction surface (World Position Offset) --------------
    #
    # VertexColor.R carries the CA fill fraction 0..255, remapped to 0..1, for
    # the cell this face belongs to -- see UVoxelWaterSubsystem.cpp's meshing
    # sampler for how it gets there, and FVoxelQuadVertexFactoryParameters::
    # WaterMode for the pooled path's half of it. A full cell is 1.0 and an
    # empty one is 0.0.
    #
    # The quad packing cannot express a fractional height: VoxelQuadDecode.ush
    # computes FaceCoordVox = Slice + (Positive ? 1 : 0) in integers, so every
    # face lands on a voxel boundary. WPO is therefore the mechanism -- it is
    # material-only, costs no geometry, and is trivially reversible.
    #
    # ONLY TOP-BOUNDARY VERTICES MOVE, and the mesh says which those are:
    # VertexColor.B is 1 on a vertex sitting on the +Z boundary of its own
    # voxel and 0 otherwise (FVoxelQuadVertex::TopBoundary in
    # VoxelQuadDecode.ush; bTopCorner in VoxelWaterChunkComponent.cpp).
    #
    # This is deliberately NOT a face-normal test. Gating on the normal would
    # lower only +Z faces and leave a partially-filled cell's SIDE walls at
    # full height, ringing every pool with a one-voxel bathtub rim standing
    # proud of its own surface. A side face has two top vertices and two
    # bottom ones, so moving only the top pair turns it into a trapezoid whose
    # upper edge meets the lowered surface -- and bottom faces never move at
    # all, so nothing opens a gap to the floor.
    #
    # A cell at fill f drops its top boundary by (1 - f) * one voxel: a full
    # cell does not move, an almost-empty one sits almost on the floor.
    # 10.0 is VoxelCoords::VoxelSizeUU -- 10 unreal units per 10 cm voxel, the
    # same constant VoxelQuadDecode.ush quotes as VOXEL_SIZE_UU.
    one_minus_fill = mel.create_material_expression(material, unreal.MaterialExpressionOneMinus, -500, 200)
    if not mel.connect_material_expressions(vertex_color, "R", one_minus_fill, ""):
        raise RuntimeError("connect vertex_color.R -> one_minus_fill failed")

    drop_amount = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -200, 250)
    if not mel.connect_material_expressions(one_minus_fill, "", drop_amount, "A"):
        raise RuntimeError("connect one_minus_fill -> drop_amount.A failed")
    if not mel.connect_material_expressions(vertex_color, "B", drop_amount, "B"):
        raise RuntimeError("connect vertex_color.B -> drop_amount.B failed")

    # (0, 0, -VoxelSizeUU): straight down, one voxel at full drop. Multiplying
    # a float3 by the scalar above broadcasts, so this stays one node.
    down_one_voxel = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -200, 380)
    down_one_voxel.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, -10.0, 1.0))

    world_position_offset = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -50, 300)
    if not mel.connect_material_expressions(down_one_voxel, "", world_position_offset, "A"):
        raise RuntimeError("connect down_one_voxel -> world_position_offset.A failed")
    if not mel.connect_material_expressions(drop_amount, "", world_position_offset, "B"):
        raise RuntimeError("connect drop_amount -> world_position_offset.B failed")
    # NOTE: world_position_offset (the fill-drop term above) is NOT connected
    # to MP_WORLD_POSITION_OFFSET here anymore -- it is summed with the WPO
    # ripple below first, at `total_wpo`, and THAT is what gets connected.
    # See "ADD to the existing fill-drop WPO, do not replace it" further down.

    # ========================================================================
    # ONE WAVE FIELD, TWO CONSUMERS (2026-08-11) -- replaces BOTH old ripples
    # ========================================================================
    #
    # WHAT THIS DELETES. Two independent ripple systems that never knew about
    # each other:
    #
    #   * the NORMAL ripple -- four 1-D sines on TextureCoordinate(0), two per
    #     axis, cross-coupled, `sin(1.8u)+sin(9.0u)` and `sin(1.6v)+sin(8.3v)`;
    #   * the WPO ripple -- two more 1-D sines on absolute world X and Y, at
    #     ~10.5 m and ~9.2 m wavelength, driving a +/-1.5 UU vertical bob.
    #
    # AND WHY, which is the owner's first complaint in two parts.
    #
    # THE NORMAL RIPPLE REPEATED, EXACTLY, AND THE ARITHMETIC IS NOT SUBTLE.
    # 9.0/1.8 = 5.000 in space; the speeds 0.90/0.30 = 3.000 in time; and both
    # terms are added into the SAME normal component. A sum of two sines whose
    # frequencies are in an exact 5:1 ratio is periodic at the LOWER frequency,
    # so that component repeated exactly every 2*pi/1.8 = 3.49 m and every
    # 2*pi/0.30 = 20.94 s. On top of a UV that itself repeats every 32 m and is
    # mirror-symmetric about the world axes. Every wavefront was parallel to a
    # world axis, and the whole thing could not tilt the surface much: measured
    # over a 30 m patch, p50 2.8 deg, p95 4.6 deg, absolute maximum 5.7 deg.
    # "Looks like a repeating tile" is a literal reading of it.
    #
    # THE WPO RIPPLE MOVED GEOMETRY THAT THE SHADING COULD NOT SEE. It produced
    # no normal at all -- nothing downstream of it reached MP_NORMAL -- so a
    # +/-1.5 cm bob changed the silhouette and nothing else. The instruction was
    # to derive its normal or drop it. It is neither: the field is shared, so the
    # normal and the displacement are now two outputs of ONE evaluation and
    # cannot disagree about where a crest is.
    #
    # THE COORDINATE IS ABSOLUTE WORLD XY IN METRES, NOT TextureCoordinate(0).
    # This is the single most important line in the section. See the module
    # docstring's "CAUSE A" for the full argument; the short form is that the
    # pooled vertex factory's UV repeats every 32 m and mirrors about x=0 and
    # y=0, and that the far-field sheets do not use that UV at all -- they
    # anchor theirs at each sheet's own bounding-box corner, so near water and
    # far water had unrelated wave phase and met at a seam. World XY is the only
    # coordinate both draw paths can agree on, and it has no period.
    #
    # world_pos_abs keeps the WPT_ExcludeAllShaderOffsets setting the old WPO
    # ripple gave it, and the reason now applies twice over: this value feeds
    # World Position Offset, so reading a position that already contained this
    # material's own WPO would put the wave into its own input. It used to be
    # numerically moot (the fill-drop term only moves Z and this reads X/Y);
    # it is still moot for the same reason and is still set, because the day
    # someone makes the fill-drop term touch X/Y is not the day anyone will
    # remember to come back here.
    #
    # WHERE THE CODE THAT DOES ALL OF THAT NOW IS: ~1,500 lines up, immediately
    # after the bathymetry sample, under "THE WAVE FIELD, BUILT HERE BECAUSE FOAM
    # READS IT". The Time node, world_pos_abs, world_xy, uu_to_m and wave_pos_m
    # moved there verbatim, and the six scalar knobs that used to be typed out
    # below are now authored by water_wave_graph under the same names with the
    # same defaults. NOTHING ABOUT THE FIELD CHANGED BY MOVING IT -- it is a pure
    # function of absolute world position and time, and the two consumers below
    # read it exactly where they always did. The reasoning above is left here,
    # attached to the section it explains, rather than dragged 1,500 lines up
    # into a block whose subject is Python ordering.
    #
    # WHAT DID CHANGE, and it is not a move: the field is now WIND-DRIVEN and
    # ships that way. See VOXEL_WATER_LEGACY_WAVES at the top of this file.

    # --- The one knob that stays HERE, because it is applied HERE ------------
    #
    # THE OTHER SIX MOVED, AND THEY MOVED WITHOUT CHANGING. WaveAmplitudeM,
    # WaveBaseWavelengthM, WaveDirBaseDeg, WaveDirIncrementDeg,
    # WaveQuantPerVoxel and WavePatchContrast are authored by
    # water_wave_graph.build_wave_field -- same names, same defaults, same
    # meanings -- so a material instance that already overrides WaveAmplitudeM
    # keeps working across this change. Their arguments (the 8.6 cm relief and
    # the slope measurement that set 0.25, the 0.43 m short end against the
    # quantisation grid, the golden angle, the 58.7-degree sweep optimum) moved
    # with them and are in water_wave_graph.py and docs/water-wind-waves.md.
    #
    # WaveWpoFraction did NOT move, because it is not the wave field's: it is
    # applied to the field's OUTPUT, below, after the ripple is summed in.
    #
    # WaveWpoFraction is how much of the wave the GEOMETRY actually moves: 0.25 of
    # an 8.6 cm field, so about +/-2.1 cm at the extreme and +/-1.2 cm at the
    # 99th percentile. That is deliberately the budget the old WPO bob worked to
    # ("no more than 1-2 UU"), and ONE of the two reasons for that budget still
    # holds: the surface has to sit on a 10 cm voxel grid.
    #
    # THE OTHER REASON IS RETIRED AND THE SENTENCE THAT STATED IT IS DELETED. It
    # used to read "there is still no shore-damping term to stop a rising crest
    # clipping through its own bank". There is now, and it is better than a
    # damping term: water_wave_graph's surf band is proportional to the wave, so
    # inside it H(d) = d / BreakDepthRatio = 0.78 d and the crest is DEPTH-LIMITED
    # at any wind speed. Measured over a 0.8 m -> 0 m shore at four phases, the
    # vertex displacement is at most 0.30 of the local depth and stops growing
    # above 10 m/s. The bank-clipping bound is now derived rather than bought
    # with a small number, so 0.25 could be RAISED on evidence -- it is not
    # raised here, because that is a look change and the owner judges those from
    # captures.
    #
    # ONE EXCEPTION TO THE BOUND, and it is the interactive ripple, not the wave:
    # the ripple is summed into the height BELOW, downstream of the node, so the
    # depth limit cannot reach it. See the ripple block for what that costs.
    #
    # The shading and the displacement therefore differ by a scalar, and that is
    # the design rather than an inconsistency: same field, same phase, same
    # crests, same drift, with the geometry deliberately under-displaced.
    # Pinning them to one number would mean choosing between an invisible glint
    # and water sloshing over the bank.
    wave_wpo_fraction = scalar_param("WaveWpoFraction", 0.25, -1300, 920)

    # --- THE INTERACTIVE RIPPLE FIELD, SUMMED INTO BOTH OUTPUTS -------------
    #
    # A SECOND HEIGHT FIELD ON THE SAME SURFACE, from a camera-following render
    # target: rings spreading from things that entered the water. Exactly the
    # same shape as the wave field -- (dH/dx, dH/dy, H) with H in metres -- and
    # it composes with it by plain addition and nothing else. See
    # Tools/ripple_field_graph.py and docs/water-interactive-ripples.md.
    #
    # SUMMING GRADIENTS IS THE WHOLE ARGUMENT, and the note at the normal
    # assembly immediately below already makes it: averaging or lerping unit
    # normals systematically flattens slopes, which is how multi-octave water
    # ends up looking like a bin liner. Two independent height fields on one
    # surface superpose, so their GRADIENTS add, and the normal assembled from
    # the sum is the normal of the combined surface. The ripple is a ninth
    # octave from a different source. Any other order is a different and wrong
    # surface.
    #
    # BOTH SUMS HAPPEN HERE, AT ONE SITE, and every one of the four things
    # downstream of this point matters:
    #
    #   * UPSTREAM OF THE -1 AND THE APPEND. A height field's tangent-space
    #     normal is (-dH/dx, -dH/dy, 1); converting the sum once gives the
    #     combined surface's normal, converting each field separately and then
    #     blending does not.
    #   * UPSTREAM OF THE VertexColor.B TOP-FACE MASK, and on the WPO half that
    #     is not cosmetic. An unmasked vertex offset would lift a side wall's
    #     BOTTOM vertices off the floor they are sealed against and open the
    #     mesh. A ripple summed outside that mask would tear water bricks apart.
    #     On the normal half it is cosmetic and still right: a splash should no
    #     more light up a submerged wall than a wind wave should.
    #   * UPSTREAM OF THE metres->UU CONVERSION, so both terms are in metres at
    #     the point they meet and the 100.0 is applied once.
    #   * UPSTREAM OF WaveWpoFraction, which is a consequence rather than a
    #     choice: the ripple's GEOMETRY contribution is a quarter of its height,
    #     so a 9 cm ring lifts the surface 2.25 cm and the normal carries the
    #     rest. If the owner wants the ring to visibly lift the water, the fix is
    #     a separate fraction on the ripple half of this sum -- NOT raising
    #     WaveWpoFraction, which would also make the wind wave slosh over its
    #     banks.
    #
    # IT IS DELIBERATELY NOT DEPTH-DAMPED, AND IT CANNOT BE REACHED BY THE
    # DAMPING EVEN BY ACCIDENT. The wave's shore damping lives INSIDE the
    # WaveField custom node (BreakSurfFloorM), applied to the wave's own
    # amplitude before the node returns; the ripple is added to the node's
    # OUTPUT. There is no wiring by which one could reach the other. That is
    # what we want: damping a splash by depth would delete it exactly at the
    # shoreline, which is where a player enters the water. The ripple has its own
    # shore treatment and it is a HORIZONTAL one -- the simulation is masked by
    # the baked signed distance to shore, so a ring dies against the bank rather
    # than fading out with depth.
    #
    # WHAT THAT COSTS, STATED SO IT IS NOT DISCOVERED FROM A SCREENSHOT: the
    # wave's depth limit also carries a guarantee that a crest cannot stand
    # taller than the water it is in (see WaveWpoFraction above). The ripple
    # bypasses the guarantee along with the damping -- its shore mask is zero on
    # land and zero at the waterline, but 30 cm inside the water it is 1.0
    # however shallow that water is. Bounded in practice by RippleFieldGain, by
    # the mask dying 25 cm inside the waterline, and by WaveWpoFraction taking
    # the geometry to a quarter of the ring: three small numbers, not a proof.
    # Accepted -- a splash poking a centimetre through a shallow bank for a
    # second is a better failure than no splash. If it ever shows in a capture
    # the fix is a depth CLAMP on the ripple's WPO half only,
    # min(ripple_h, 0.5 * depth), which leaves the shading ring intact.
    #
    # --- AND WHY THIS IS GUARDED RATHER THAN CALLED STRAIGHT -----------------
    #
    # sample_ripple_field RAISES if the three RippleField* names are not on
    # MPC_VoxelSky, or if /Game/Voxel/RT_VoxelRippleField does not exist, and it
    # is right to: an unresolved CollectionParameter compiles to a CONSTANT
    # rather than failing (MaterialExpressions.cpp:17179-17193), so a constant
    # origin would sample one fixed texel for the entire world, and an unbound
    # texture parameter would add whatever image the engine picks to the water's
    # normal on every pixel. Both are silent. Raising is the correct default for
    # a module that cannot know who is calling it.
    #
    # IT IS THE WRONG BEHAVIOUR *HERE*, TODAY, AND THAT IS A SCHEDULING FACT
    # RATHER THAN A DISAGREEMENT. Those two prerequisites are landing with the
    # ripple subsystem, in files this change does not own
    # (create_sky_material.py's parameter tables, create_ripple_field_materials.
    # py's render target). Until they do, calling straight through would make
    # M_WaterVoxel UNGENERATABLE -- and the wind waves above, which have no such
    # dependency and which the owner is waiting to play-test, would go down with
    # them. Whole feature blocked on an unrelated one.
    #
    # So this probes and degrades, LOUDLY, which is exactly the shape
    # water_wave_graph.build_wind_input already uses for the same situation
    # (water_wave_graph.py:961-968: "IF THE COLLECTION HAS NO WIND PARAMETERS
    # THIS DOES NOT RAISE ... It logs, loudly, and takes the fallback"). The
    # degraded arm is not an approximation of the ripple, it is its exact
    # absence: the sums become the wave field alone, which is what
    # RippleFieldGain = 0 would produce anyway, minus the texture fetch.
    #
    # THE PROBE IS THE SAME TWO CHECKS THE MODULE ITSELF WOULD MAKE, so it
    # cannot pass here and fail there: name membership read back off the
    # collection (SkyGraphBuilder.mpc_names), and a load of the render target.
    # WHEN THE PREREQUISITES LAND THIS ARM DISAPPEARS ON ITS OWN -- there is no
    # environment variable and no default to remember to flip.
    ripple_missing = sorted(
        n for n in (ripple_field_graph.MPC_ORIGIN,
                    ripple_field_graph.MPC_INV_SIZE,
                    ripple_field_graph.MPC_GAIN)
        if n not in bathy_b.mpc_names())
    try:
        # try/except and not a bare None check: unreal.load_object's failure
        # mode for a package that does not exist on disk is not guaranteed to be
        # a None return on every engine build, and the entire point of this
        # block is that a missing ripple field must not take the water material
        # down with it.
        ripple_rt = unreal.load_object(None, ripple_field_graph.FIELD_TEXTURE)
    except Exception:  # noqa: BLE001 -- absence is the thing being tested for
        ripple_rt = None

    if ripple_missing or ripple_rt is None:
        unreal.log_warning(
            "M_WaterVoxel RIPPLE FIELD ARM: ABSENT -- building the water WITHOUT interactive "
            "ripples. Missing MPC_VoxelSky parameters: %s. Render target %s: %s. This is the "
            "expected state until the ripple subsystem's two authoring steps land; the wind "
            "wave field above is unaffected and the water is exactly the water it would be "
            "with RippleFieldGain at 0. TO FIX: apply docs/water-interactive-ripples.md 8.1 to "
            "create_sky_material.py, then re-run create_sky_material.py, "
            "create_sky_atmosphere_dome_material.py, create_ripple_field_materials.py and this "
            "script, in that order."
            % (ripple_missing or "none",
               ripple_field_graph.FIELD_TEXTURE,
               "missing" if ripple_rt is None else "present"))
        wave_grad_total = wave_grad_raw
        wave_height_total = wave_height_m
    else:
        ripple = sample_ripple_field(bathy_b)
        unreal.log(
            "M_WaterVoxel RIPPLE FIELD ARM: PRESENT (%s bound, RippleFieldGain gates it at "
            "runtime and the subsystem holds it at 0 until the first simulated frame exists)"
            % ripple_field_graph.FIELD_TEXTURE)
        wave_grad_total = bathy_b.add(wave_grad_raw, ripple["grad_xy"])
        wave_height_total = bathy_b.add(wave_height_m, ripple["height_m"])

        # --- THE INSTRUMENT: VOXEL_WATER_RIPPLE_DEBUG=1 ---------------------
        #
        # Routes the ripple field straight to EMISSIVE and nothing else, so the
        # water surface becomes a picture of the field instead of a surface lit
        # by it. Built because static reading had run out of road.
        #
        # THE STATE THAT FORCED THIS, recorded because every single check passed
        # and the feature still did not work. The simulation reported
        # `armed=1 published=1 steps=26585 injected=4 dropped(outside=0 full=0
        # unarmed=0 inert=0)` -- four disturbances accepted, none rejected. The
        # derive pass correctly removes the storage bias (h = HC - StateBias).
        # M_WaterVoxel's package names RT_VoxelRippleField, RippleFieldGain,
        # RippleFieldOrigin and RippleFieldInvSize, so the sampler and all three
        # collection parameters are really in the graph. The subsystem publishes
        # all three every frame and `published=1` proves the gain it published
        # was above zero. There is not one warning in the log. And no ripple is
        # visible on the water.
        #
        # When every report is healthy and the picture disagrees, the reports are
        # measuring the wrong thing, and the only way forward is to look at the
        # data itself. That is the same conclusion this project reached about
        # material regeneration -- the pinned-pose screenshot is the check that
        # works -- arrived at again one layer down.
        #
        # WHAT THE TWO OUTCOMES MEAN, so the next run is decisive:
        #   COLOUR appears around the player  -> the texture holds data and the
        #       UV lands, so the fault is downstream: the ripple's contribution
        #       to the NORMAL is real but too small to see against the wind
        #       waves, and the fix is a gain or a strength.
        #   FLAT BLACK -> the sample itself is zero, so it is the derive draw or
        #       the UV mapping, and the gain is irrelevant.
        # Green/red tint reads the two gradient channels; blue reads height.
        if os.environ.get("VOXEL_WATER_RIPPLE_DEBUG", "0").strip().lower() not in (
                "0", "off", "false", "no", ""):
            # The GRADIENT only, not the height: a float2 wired to emissive
            # reads as (R, G, 0), which is all that is needed to answer "is
            # there anything in this texture". Assembling a float3 would need an
            # AppendVector this builder does not expose, and the extra channel
            # would tell us nothing the first two do not.
            dbg_gain = scalar_param("RippleDebugGain", 20.0, -1300, 3000)
            dbg = bathy_b.mul(ripple["grad_xy"], dbg_gain)
            if not mel.connect_material_property(
                    dbg, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
                raise RuntimeError("connect ripple debug -> emissive failed")
            unreal.log(
                "M_WaterVoxel RIPPLE DEBUG ARM: ON -- emissive is the ripple field "
                "(R,G = gradient, B = height) x RippleDebugGain. THIS IS NOT A SHIPPING "
                "MATERIAL; rebuild without VOXEL_WATER_RIPPLE_DEBUG to restore it.")

    # --- NORMAL -------------------------------------------------------------
    #
    # A height field's tangent-space normal is (-dH/dx, -dH/dy, 1). Z IS PINNED
    # TO 1.0 AND THE RESULT IS NOT NORMALIZED, which is the property the old
    # code had and the instruction says to keep: it means octaves combine as
    # SURFACE GRADIENTS rather than as blended normals. Summing gradients is the
    # only correct way to combine height fields -- averaging or lerping unit
    # normals systematically flattens slopes, which is how multi-octave water
    # ends up looking like a bin liner. The eight-octave gradient sum happens
    # inside the WaveField node and the ripple is added to it immediately above;
    # this is just the assembly.
    #
    # A top face's tangent basis is world-aligned (tangent=X, bitangent=Y,
    # normal=Z -- VoxelQuadVertexFactory.ush's per-axis RotateLocalToWorld for
    # an Axis==2 face), and the far-field sheets author the same basis
    # explicitly (FProcMeshTangent(1,0,0) with an up normal), so the X/Y
    # gradient lands in the directions it was computed in on BOTH draw paths.
    # THE LOCAL .xy MASK THAT USED TO BE HERE IS GONE. build_wave_field returns
    # the gradient already masked off the float4 node, and the input here is now
    # the SUM of that and the ripple, not the wave alone.
    neg_one = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -540, 940)
    neg_one.set_editor_property("r", -1.0)
    normal_xy_raw = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -380, 880)
    if not mel.connect_material_expressions(wave_grad_total, "", normal_xy_raw, "A"):
        raise RuntimeError("connect wave_grad_total -> normal_xy_raw.A failed")
    if not mel.connect_material_expressions(neg_one, "", normal_xy_raw, "B"):
        raise RuntimeError("connect neg_one -> normal_xy_raw.B failed")

    # Masked by VertexColor.B (top-boundary flag) so only the top surface
    # shimmers and side walls stay flat -- same flag, same reasoning as the WPO
    # sections: B is 1 only on a vertex sitting on its own voxel's +Z boundary.
    # A side-wall quad has two top vertices (B=1) and two bottom (B=0); here
    # that is a per-vertex multiplier on a pixel-shader-only Normal input, so it
    # interpolates smoothly across the quad -- a side wall's shimmer fades out
    # over its height rather than snapping off, which is fine since this is a
    # lighting-only cue.
    normal_xy = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -220, 880)
    if not mel.connect_material_expressions(normal_xy_raw, "", normal_xy, "A"):
        raise RuntimeError("connect normal_xy_raw -> normal_xy.A failed")
    if not mel.connect_material_expressions(vertex_color, "B", normal_xy, "B"):
        raise RuntimeError("connect vertex_color.B -> normal_xy.B failed")

    normal_z = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -220, 960)
    normal_z.set_editor_property("r", 1.0)

    normal_xyz = mel.create_material_expression(material, unreal.MaterialExpressionAppendVector, -60, 900)
    if not mel.connect_material_expressions(normal_xy, "", normal_xyz, "A"):
        raise RuntimeError("connect normal_xy -> normal_xyz.A failed")
    if not mel.connect_material_expressions(normal_z, "", normal_xyz, "B"):
        raise RuntimeError("connect normal_z -> normal_xyz.B failed")

    if not mel.connect_material_property(normal_xyz, "", unreal.MaterialProperty.MP_NORMAL):
        raise RuntimeError("connect normal_xyz -> Normal failed")

    # --- WORLD POSITION OFFSET: THE SAME FIELD'S HEIGHT ---------------------
    #
    # The .z of the same evaluation, in metres, scaled to UU and down-weighted
    # by WaveWpoFraction. This is the answer to "derive the WPO wave's normal or
    # drop it": the normal above and this displacement are the two halves of one
    # field, so a crest that moves in the geometry is a crest that moves in the
    # shading, at the same place and the same speed.
    #
    # ADJACENT BRICKS MUST AGREE BIT FOR BIT AT A SHARED VERTEX or the mesh
    # tears open along the seam every time the phase moves. They do: this is a
    # pure function of absolute world position, which two bricks sharing a
    # physical vertex compute identically. That was the reason the old WPO
    # ripple used absolute world position and refused the wrapped UV, and it is
    # unchanged -- it is now the reason the NORMAL uses it too.
    # The .z mask that used to be built here is gone for the same reason as the
    # gradient's: build_wave_field returns height_m already masked, and what is
    # displaced is the SUM of the wave and the ripple.
    #
    # metres -> UU, and take the displacement fraction. 100.0 is the engine's
    # cm-per-metre, the same constant the UU->m conversion above inverts.
    m_to_uu = mel.create_material_expression(material, unreal.MaterialExpressionConstant, -540, 1100)
    m_to_uu.set_editor_property("r", 100.0)
    wave_height_uu = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -380, 1040)
    if not mel.connect_material_expressions(wave_height_total, "", wave_height_uu, "A"):
        raise RuntimeError("connect wave_height_total -> wave_height_uu.A failed")
    if not mel.connect_material_expressions(m_to_uu, "", wave_height_uu, "B"):
        raise RuntimeError("connect m_to_uu -> wave_height_uu.B failed")

    ripple_height = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -220, 1040)
    if not mel.connect_material_expressions(wave_height_uu, "", ripple_height, "A"):
        raise RuntimeError("connect wave_height_uu -> ripple_height.A failed")
    if not mel.connect_material_expressions(wave_wpo_fraction, "", ripple_height, "B"):
        raise RuntimeError("connect wave_wpo_fraction -> ripple_height.B failed")

    # Same B mask as the fill-drop WPO above, and for the same reason: only
    # vertices ON the top boundary may move, or a side wall's bottom edge lifts
    # off the floor it is supposed to be sealed against.
    ripple_masked = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, -60, 1040)
    if not mel.connect_material_expressions(ripple_height, "", ripple_masked, "A"):
        raise RuntimeError("connect ripple_height -> ripple_masked.A failed")
    if not mel.connect_material_expressions(vertex_color, "B", ripple_masked, "B"):
        raise RuntimeError("connect vertex_color.B -> ripple_masked.B failed")

    # (0,0,1) * scalar broadcast -> (0,0,ripple): the same broadcast idiom
    # down_one_voxel/drop_amount already use above for the fill-drop term.
    up_axis = mel.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -60, 1120)
    up_axis.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, 1.0, 1.0))
    ripple_wpo = mel.create_material_expression(material, unreal.MaterialExpressionMultiply, 100, 1080)
    if not mel.connect_material_expressions(up_axis, "", ripple_wpo, "A"):
        raise RuntimeError("connect up_axis -> ripple_wpo.A failed")
    if not mel.connect_material_expressions(ripple_masked, "", ripple_wpo, "B"):
        raise RuntimeError("connect ripple_masked -> ripple_wpo.B failed")

    # ADD to the existing fill-drop WPO, do not replace it: the stepped
    # surface from the section above and the continuous ripple here are two
    # independent reasons a vertex moves, and both have to apply at once.
    total_wpo = mel.create_material_expression(material, unreal.MaterialExpressionAdd, 800, 300)
    if not mel.connect_material_expressions(world_position_offset, "", total_wpo, "A"):
        raise RuntimeError("connect world_position_offset -> total_wpo.A failed")
    if not mel.connect_material_expressions(ripple_wpo, "", total_wpo, "B"):
        raise RuntimeError("connect ripple_wpo -> total_wpo.B failed")

    if not mel.connect_material_property(total_wpo, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET):
        raise RuntimeError("connect total_wpo -> WorldPositionOffset failed")

    mel.layout_material_expressions(material)
    mel.recompile_material(material)

    unreal.EditorAssetLibrary.save_loaded_asset(material)

    # --- READ THE SHADING MODEL AND BLEND MODE OFF THE PACKAGE ON DISK -------
    #
    # NOT off the UObject in memory, and that distinction is the entire point.
    # The in-memory read a few hundred lines up can only see the AUTHORED
    # ShadingModel field, because this engine build does not export the cached
    # FMaterialShadingModelField to Python -- so it reports this script's intent
    # with one extra step, not the artefact. This reads the bytes that were just
    # written.
    #
    # IT WORKS BECAUSE UE SERIALISES ENUM PROPERTIES BY NAME. A UMaterial's
    # ShadingModel and BlendMode are enum properties, so the package's name table
    # literally contains the string "MSM_SingleLayerWater" or "MSM_DefaultLit",
    # and "BLEND_Opaque" or "BLEND_Translucent". Same trick, same reason, as
    # tools/voxel-water-star-regen.ps1's MaterialExpressionTime check: a saved
    # UMaterial names every expression CLASS it uses, so the presence of
    # "MaterialExpressionSingleLayerWaterMaterialOutput" is proof the required
    # output node survived into the asset.
    #
    # WHAT IT IS FOR. The failure this catches is the one that has actually
    # happened on this project: an asset that everybody believes is one thing and
    # that the renderer treats as another, discovered from the owner saying he
    # cannot see the lakes. If the shading model silently failed to take, the
    # water would render as an opaque DefaultLit surface with a BLACK base colour
    # -- because BaseColor is now foam-only and foam is zero on a settled lake --
    # and a black lake at noon is a picture somebody would spend a session
    # debugging in the wrong file.
    package_ok = None
    try:
        with open(os.path.join(
                unreal.Paths.project_content_dir(), "Voxel", "M_WaterVoxel.uasset"), "rb") as fh:
            blob = fh.read().decode("latin-1")
        found = {name: (name in blob) for name in (
            "MSM_SingleLayerWater", "MSM_DefaultLit",
            "BLEND_Opaque", "BLEND_Translucent",
            "MaterialExpressionSingleLayerWaterMaterialOutput")}
        package_ok = (found["MSM_SingleLayerWater"]
                      and not found["MSM_DefaultLit"]
                      and found["BLEND_Opaque"]
                      and not found["BLEND_Translucent"]
                      and found["MaterialExpressionSingleLayerWaterMaterialOutput"])
        unreal.log("M_WaterVoxel PACKAGE READ-BACK: %s -- %s"
                   % ("SINGLE LAYER WATER, OPAQUE" if package_ok else "WRONG",
                      ", ".join("%s=%s" % (k, v) for k, v in sorted(found.items()))))
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(
            "M_WaterVoxel: could not read the saved package back (%r), so the shading model on "
            "disk is UNKNOWN. Do not read a capture taken on this build as evidence that the "
            "port is live." % (exc,))
    if package_ok is False:
        raise RuntimeError(
            "the saved M_WaterVoxel.uasset is NOT a Single Layer Water opaque material. The "
            "renderer would draw it as an opaque DefaultLit surface with a black base colour "
            "(BaseColor is foam-only now), i.e. a black lake, and nothing else in this run "
            "would have said so.")

    # THE RAN-FLAG, and it is deliberately not "success".
    #
    # This project's standing rule is that a stage must log something that
    # distinguishes "ran and found nothing" from "did not run". For an A/B built
    # by re-running this script, the thing that must be distinguishable is WHICH
    # ARM was built -- and a perf log that merely says the script succeeded
    # cannot tell an OFF arm from a run where the environment variable never
    # reached the process. So the arm is named, the variable's raw value is
    # echoed next to it, and the star node count is printed: an ON arm with zero
    # star nodes is a contradiction the line makes visible.
    #
    # The count is read back from the SAVED ASSET by asking the material for its
    # StarmapTex scalar/texture parameter, not from a variable this function set
    # -- a counter incremented next to the code that builds the node would agree
    # with the arm by construction and prove nothing.
    try:
        star_nodes = len([p for p in mel.get_texture_parameter_names(material)
                          if str(p) == "StarmapTex"])
    except AttributeError:
        # Older/newer engine Python bindings spell this differently. Falling back
        # to a direct texture lookup keeps the check real -- get_material_default_
        # texture_parameter_value raises or returns None for a parameter the
        # material does not have -- rather than turning the ran-flag into a
        # tautology.
        try:
            star_nodes = 1 if mel.get_material_default_texture_parameter_value(
                material, "StarmapTex") is not None else 0
        except Exception:  # noqa: BLE001
            # -1 is UNKNOWN, and it is deliberately not 0. Reporting "no star
            # sampler found" when the truth is "this engine build would not tell
            # me" is the precise confusion this project's ran-flag rule exists to
            # prevent, and here it would abort a correct regeneration.
            star_nodes = -1
    # --- READ THE RIPPLE-TIME ARM BACK OFF THE SAVED ASSET --------------------
    #
    # Same rule as the star arm below, for the same reason: an environment
    # variable that fails to reach this process does not error, it silently
    # builds the OTHER arm. A frozen arm that was actually built live would show
    # the full animated difference and be reported as "the flicker is not
    # animation", which is the exact conclusion this arm exists to test.
    #
    # There is exactly ONE MaterialExpressionTime in this graph (it feeds the
    # normal, the WPO and the breaking foam), so the count is 1 when live and 0
    # when frozen. -1 is UNKNOWN and is deliberately not 0 -- see the star arm's
    # note on why reporting a bindings failure as an absence is the worse lie.
    #
    # THE ASSERTION STILL HOLDS AFTER THE WAVE AND RIPPLE MODULES LANDED, and it
    # was checked rather than assumed: neither creates a Time node.
    # build_wave_field takes time as an ARGUMENT (that is why the signature has
    # one -- water_wave_graph.py:1026-1031), and the ripple's animation lives in
    # the render target's simulation, not in this material.
    time_nodes = -1
    for getter in ("expression_collection", "expressions"):
        try:
            holder = material.get_editor_property(getter)
            exprs = getattr(holder, "expressions", holder)
            time_nodes = sum(1 for e in exprs
                             if isinstance(e, unreal.MaterialExpressionTime))
            break
        except Exception:  # noqa: BLE001
            continue
    unreal.log("M_WaterVoxel RIPPLE TIME ARM: %s (%s=%r, Time nodes=%s)"
               % ("FROZEN" if FREEZE_RIPPLE_TIME else "LIVE",
                  FREEZE_TIME_ENV,
                  os.environ.get(FREEZE_TIME_ENV, "<unset>"),
                  "UNKNOWN" if time_nodes < 0 else time_nodes))
    if time_nodes < 0:
        unreal.log_warning(
            "M_WaterVoxel: could not enumerate expressions on the saved asset, so the ripple-time "
            "arm above is this script's INTENT and not a read-back. Do not read a frozen-arm "
            "measurement taken on this build as evidence about animation.")
    elif FREEZE_RIPPLE_TIME != (time_nodes == 0):
        raise RuntimeError(
            "ripple-time arm disagrees with the graph: FREEZE_RIPPLE_TIME=%s but the saved "
            "material has %d MaterialExpressionTime node(s). One of the two is a lie and the "
            "animation-vs-flicker split would inherit it." % (FREEZE_RIPPLE_TIME, time_nodes))

    # --- AND THE SHORELINE-EFFECTS ARM ---------------------------------------
    #
    # Reported as INTENT, deliberately, and NOT dressed up as a read-back. The
    # two arms above can be checked against the saved graph because they change
    # its SHAPE -- a Time node exists or it does not, a texture parameter exists
    # or it does not. This arm changes one scalar's DEFAULT VALUE and nothing
    # else, which is what makes it a clean A/B and also what makes it invisible
    # to a node census.
    #
    # So the runner does not get to trust this line alone: tools/voxel-shore-fx-ab.ps1
    # reads BathyFoamGain back off the saved .uasset instead, which is the
    # artefact rather than the claim. Same discipline as the package read-back
    # that script's sibling does for MaterialExpressionTime.
    unreal.log("M_WaterVoxel SHORE FX ARM: %s (%s=%r, BathyFoamGain default=%.3f)"
               % ("ON" if SHORE_FX else "OFF",
                  SHORE_FX_ENV,
                  os.environ.get(SHORE_FX_ENV, "<unset>"),
                  0.55 if SHORE_FX else 0.0))

    # --- AND THE WIND-WAVE ARM ------------------------------------------------
    #
    # INTENT, NOT A READ-BACK, and flagged as such for exactly the reason the
    # shore-fx arm above is: this arm changes five scalar DEFAULT VALUES and
    # nothing else about the graph's shape, which is what makes it a clean A/B
    # and also what makes it invisible to a node census. The way to check the
    # ARTEFACT is to read the five defaults back off the saved .uasset, the same
    # thing tools/voxel-shore-fx-ab.ps1 does for BathyFoamGain.
    #
    # THE DEFAULT ARM IS THE WIND ONE AND IT IS A VISIBLE CHANGE TO THE WATER.
    # Read it in the log as a statement that the lake is deliberately not the
    # lake of 2026-08-12: waves steered into a cone about the wind, reaching the
    # beach roughly four times taller, and breaking white where the bed shelves.
    unreal.log("M_WaterVoxel WIND WAVE ARM: %s (%s=%r, "
               "WindDirectionAuthority=%g, BreakSurfFloorM=%g, BreakFoamGain=%g, "
               "BreakPeakGain=%g, BreakShoalGain=%g)"
               % ("LEGACY (pre-wind field, reproduces 2026-08-12)" if LEGACY_WAVES
                  else "WIND-DRIVEN (ACTIVE -- deliberately not the 2026-08-12 water)",
                  LEGACY_WAVES_ENV,
                  os.environ.get(LEGACY_WAVES_ENV, "<unset>"),
                  WAVE_DEFAULTS["WindDirectionAuthority"],
                  WAVE_DEFAULTS["BreakSurfFloorM"],
                  WAVE_DEFAULTS["BreakFoamGain"],
                  WAVE_DEFAULTS["BreakPeakGain"],
                  WAVE_DEFAULTS["BreakShoalGain"]))

    unreal.log("M_WaterVoxel STAR REFLECTION ARM: %s (%s=%r, StarmapTex params=%s)"
               % ("ON" if STAR_REFLECTION else "OFF",
                  STAR_REFLECTION_ENV,
                  os.environ.get(STAR_REFLECTION_ENV, "<unset>"),
                  "UNKNOWN" if star_nodes < 0 else star_nodes))
    if star_nodes < 0:
        unreal.log_warning(
            "M_WaterVoxel: could not read the StarmapTex parameter back from the saved asset, so "
            "the arm above is this script's INTENT and not a read-back. Confirm the arm from the "
            "material statistics line below instead -- the ON arm has strictly more texture samples.")
    elif STAR_REFLECTION != (star_nodes > 0):
        raise RuntimeError(
            "star-reflection arm disagrees with the graph: arm=%s but %d StarmapTex "
            "texture parameters are on the saved material. One of the two is a lie "
            "and the perf A/B would inherit it." % (STAR_REFLECTION, star_nodes))

    # THE DURABLE NUMBER. Frame times belong to this box's GPU; an instruction
    # count and a texture-sample count belong to the material and transfer to any
    # machine that ever runs it. Printed for BOTH arms so the delta is a
    # subtraction rather than a recollection. str() on the struct rather than
    # named fields on purpose -- the field set of FMaterialStatistics is not
    # stable across engine versions, and a KeyError here would abort an asset
    # regeneration over a diagnostic.
    #
    # POLLED, NOT READ ONCE. Shader compilation is asynchronous, and the first
    # attempt at this line returned every field as 0 (measured 2026-08-11). Zero
    # instructions is not a cheap material, it is an absent shader map -- exactly
    # the "found nothing" / "did not run" confusion this project's ran-flag rule
    # is about, arriving in the one number that was supposed to be durable.
    #
    # There are two reasons the map can be absent and only one of them is fixable
    # here: the shaders have not finished compiling yet (this loop), or the
    # commandlet came up with a NULL RHI and never asked for them at all (pass
    # -AllowCommandletRendering; tools/voxel-water-star-regen.ps1 does). The log
    # line below says which by reporting the wait it actually did.
    try:
        stats = None
        waited = 0.0
        while waited <= 240.0:
            stats = mel.get_statistics(material)
            if int(stats.get_editor_property("num_pixel_shader_instructions")) > 0:
                break
            time.sleep(5.0)
            waited += 5.0
        unreal.log("M_WaterVoxel MATERIAL STATS (%s, waited %.0fs): %s"
                   % ("ON" if STAR_REFLECTION else "OFF", waited, str(stats)))
        if stats is None or int(stats.get_editor_property("num_pixel_shader_instructions")) == 0:
            unreal.log_warning(
                "M_WaterVoxel: pixel shader instruction count is 0 after %.0fs. That is NOT a free "
                "material -- it means no compiled shader map was available to read. Re-run with "
                "-AllowCommandletRendering, and do not report this as an instruction count."
                % waited)
    except Exception as exc:  # noqa: BLE001 -- diagnostics only, never fatal
        unreal.log_warning("M_WaterVoxel material statistics unavailable: %r" % (exc,))

    unreal.log("M_WaterVoxel created and saved at " + FULL_PATH)


main()
