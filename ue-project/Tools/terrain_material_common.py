"""Shared biome/texture graph for M_VoxelTerrain and M_VoxelClipmap.

WHY THIS IS SHARED CODE AND NOT TWO COPIES
------------------------------------------
The clipmap draws the 50 km vista and the voxel rings draw the near field. If
their colour schemes diverge the seam between them is obvious -- and they DID
diverge: before this change the clipmap lerped green->grey on a slope byte in
VertexColor.R while the voxel material ignored VertexColor entirely and used a
flat beige constant. The vista was pale green, the ground was beige.

Both meshes now write the SAME vertex colour encoding (see VoxelClimateProbe.h):

    R = vxc::MaterialId          -> T_VoxelPalette index
    G = AO * GI  (clipmap: 255)  -> multiplied into BaseColor at the end
    B = temperature              -> T_VoxelBiomeLUT V axis
    A = precipitation            -> T_VoxelBiomeLUT U axis

...and both materials decode it with build_terrain_base_color() below. The two
can no longer drift, because there is one graph. What differs between them is
only ScalarParameter DEFAULTS (see the two call sites), which is the intended
axis of variation: a clipmap vertex normal is a real terrain normal, a voxel
face normal is axis-aligned, so the slope term has to be weighted differently.

TEXTURES / MOIRE
----------------
The cave-lighting agent established that the shimmer is GEOMETRIC aliasing of
10 cm voxel steps at grazing angles, and that "mip/aniso cannot help:
M_VoxelTerrain/M_VoxelClipmap are vertex-colour driven with no textures and no
meaningful UVs, so there is nothing to mip." This graph is what gives them
something to mip: T_VoxelDetail is sampled on real UVs with a full mip chain
and anisotropic filtering. That does not remove geometric aliasing -- nothing
in a material can -- but it stops the terrain being a single flat albedo where
every voxel step edge is a pure, maximally-visible shading discontinuity.
"""

import os

import unreal

PALETTE_TEXTURE = "/Game/Voxel/T_VoxelPalette.T_VoxelPalette"
BIOME_LUT_TEXTURE = "/Game/Voxel/T_VoxelBiomeLUT.T_VoxelBiomeLUT"
DETAIL_TEXTURE = "/Game/Voxel/T_VoxelDetail.T_VoxelDetail"

from terrain_palette import PALETTE_WIDTH, biome_tinted_runs  # noqa: E402


def load_texture(path):
    """unreal.load_object, not EditorAssetLibrary.load_asset.

    A -run=pythonscript commandlet does not wait for the asset registry's
    background scan, so load_asset fails with "could not be found in the Asset
    Registry" even when the .uasset is on disk. create_voxel_material.py
    documents the same trap for DitherTemporalAA.
    """
    tex = unreal.load_object(None, path)
    if tex is None:
        raise RuntimeError(
            "failed to load %s -- run gen_terrain_textures.py then "
            "import_terrain_textures.py first" % path)
    return tex


class GraphBuilder:
    """Thin wrapper that CHECKS every connection.

    A silently-failed pin connect produced an invisible-terrain material once
    (2026-07-19) and cost a debug session; create_voxel_material.py has checked
    its connects ever since. Doing it in one place keeps the graph below
    readable without giving that up.
    """

    def __init__(self, material):
        self.material = material
        self.mel = unreal.MaterialEditingLibrary
        self._x = -2400
        self._y = 0

    def _place(self):
        self._y += 90
        if self._y > 1600:
            self._y = 0
            self._x += 300
        return self._x, self._y

    def node(self, cls):
        x, y = self._place()
        return self.mel.create_material_expression(self.material, cls, x, y)

    def link(self, src, src_out, dst, dst_in):
        if not self.mel.connect_material_expressions(src, src_out, dst, dst_in):
            raise RuntimeError("connect %s.%s -> %s.%s failed"
                               % (type(src).__name__, src_out or "<default>",
                                  type(dst).__name__, dst_in or "<default>"))

    def prop(self, src, src_out, material_property):
        if not self.mel.connect_material_property(src, src_out, material_property):
            raise RuntimeError("connect %s -> %s failed" % (type(src).__name__, material_property))

    # --- small algebra helpers ---------------------------------------------

    def const(self, value):
        n = self.node(unreal.MaterialExpressionConstant)
        n.set_editor_property("r", float(value))
        return n

    def const3(self, r, g, b):
        n = self.node(unreal.MaterialExpressionConstant3Vector)
        n.set_editor_property("constant", unreal.LinearColor(r, g, b, 1.0))
        return n

    def scalar(self, name, default):
        n = self.node(unreal.MaterialExpressionScalarParameter)
        n.set_editor_property("parameter_name", name)
        n.set_editor_property("default_value", float(default))
        return n

    def vector(self, name, r, g, b):
        n = self.node(unreal.MaterialExpressionVectorParameter)
        n.set_editor_property("parameter_name", name)
        n.set_editor_property("default_value", unreal.LinearColor(r, g, b, 1.0))
        return n

    def binary(self, cls, a, a_out, b, b_out):
        n = self.node(cls)
        self.link(a, a_out, n, "A")
        self.link(b, b_out, n, "B")
        return n

    def mul(self, a, b, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionMultiply, a, a_out, b, b_out)

    def add(self, a, b, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionAdd, a, a_out, b, b_out)

    def sub(self, a, b, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionSubtract, a, a_out, b, b_out)

    def div(self, a, b, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionDivide, a, a_out, b, b_out)

    def saturate(self, a, a_out=""):
        n = self.node(unreal.MaterialExpressionSaturate)
        # "" targets the single input pin regardless of its display name -- the
        # same MaterialEditingLibrary convention create_ocean_material.py uses.
        self.link(a, a_out, n, "")
        return n

    def one_minus(self, a, a_out=""):
        n = self.node(unreal.MaterialExpressionOneMinus)
        self.link(a, a_out, n, "")
        return n

    def abs_(self, a, a_out=""):
        n = self.node(unreal.MaterialExpressionAbs)
        self.link(a, a_out, n, "")
        return n

    def maximum(self, a, b, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionMax, a, a_out, b, b_out)

    def append(self, a, a_out, b, b_out):
        n = self.node(unreal.MaterialExpressionAppendVector)
        self.link(a, a_out, n, "A")
        self.link(b, b_out, n, "B")
        return n

    def lerp(self, a, a_out, b, b_out, alpha, alpha_out=""):
        n = self.node(unreal.MaterialExpressionLinearInterpolate)
        self.link(a, a_out, n, "A")
        self.link(b, b_out, n, "B")
        self.link(alpha, alpha_out, n, "Alpha")
        return n

    def ramp(self, value, value_out, lo, hi):
        """saturate((value - lo) / (hi - lo)) with lo/hi as expressions."""
        num = self.sub(value, lo, value_out, "")
        den = self.sub(hi, lo)
        return self.saturate(self.div(num, den))

    def sample(self, texture_path, uv, uv_out, param_name, sampler_type):
        n = self.node(unreal.MaterialExpressionTextureSampleParameter2D)
        n.set_editor_property("parameter_name", param_name)
        n.set_editor_property("texture", load_texture(texture_path))
        n.set_editor_property("sampler_type", sampler_type)
        self.link(uv, uv_out, n, "UVs")
        return n


def build_terrain_base_color(
    b,
    vertex_color,
    detail_uv,
    detail_uv_out,
    *,
    rock_slope_strength,
    detail_fine_strength,
    detail_coarse_strength,
    bathy=None,
):
    """Build the biome/palette/detail graph.

    Returns (base_color_expr, snow_expr, base_color_out, wet_expr).

    `bathy` is the dict from bathy_field_graph.sample_bathy_field, or None to
    build the material without the wet-shore term at all. When it is None the
    fourth return value is None and the caller must not try to use it -- that is
    the "no field" path in the STRUCTURAL sense, distinct from the per-pixel
    validity gate inside the term, and it exists so a material can be authored
    before /Game/Voxel/T_VoxelBathyInfo does.

    `b` is a GraphBuilder. `vertex_color` is the shared VertexColor node.
    `detail_uv`/`detail_uv_out` supply the UV for T_VoxelDetail -- the two
    materials source it differently (see call sites), which is the only
    structural difference between them.

    The returned base colour still needs the AO multiply and DebugTint applied
    by the caller, because those differ (the clipmap's DebugTint is what the
    underground veil drives to near-black).
    """
    # --- material-id palette lookup ----------------------------------------
    #
    # VertexColor.R is the material id byte / 255. Landing on texel centre i of
    # a 16-wide texture needs u = (i + 0.5) / 16, i.e. R * (255/16) + 0.5/16.
    # The texture is TF_NEAREST + clamp + uncompressed + no mips (see
    # import_terrain_textures.py) so this lands exactly on one entry with no
    # bleed from its neighbours.
    # --- surface vs subsurface ---------------------------------------------
    #
    # VertexColor.R is a BINARY flag: 255 = this surface takes the biome colour,
    # 0 = it keeps a subsurface rock look. It is not a material id any more, and
    # there is no palette texture lookup, because thresholding a categorical id
    # through the vertex-colour path did not survive measurement -- an
    # exposure-proof probe read VertexColor.R * 255 back as ~6-8 where the CPU
    # wrote 4, and every id threshold evaluated to 0 across the whole frame.
    # VoxelClimateProbe.h records that measurement and what it costs (per-
    # material strata underground).
    #
    # 0 and 255 are fixed points of any monotonic per-channel transform, so this
    # flag is exact no matter what that path does. saturate() is belt-and-braces
    # against a transform that overshoots.
    tint_weight = b.saturate(b.mul(vertex_color, b.const(1.0), "R"))

    # One flat colour for everything below the surface. This is a REGRESSION
    # against the per-material palette this graph briefly had -- a cave wall is
    # now one rock tone rather than bedrock/rock/gravel/subsoil each having
    # their own -- but it is not a regression against main, where every
    # underground surface is the same flat beige as everything else.
    subsurface = b.vector("SubsurfaceColor", 0.20, 0.175, 0.145)

    # --- biome lookup -------------------------------------------------------
    #
    # U = precipitation (VertexColor.A), V = temperature (VertexColor.B), both
    # already remapped by VoxelClimateProbe to this world's measured p1..p99, so
    # the full 64x64 LUT is reachable. gen_terrain_textures.py paints the same
    # axis order.
    biome_uv = b.append(vertex_color, "A", vertex_color, "B")
    biome = b.sample(BIOME_LUT_TEXTURE, biome_uv, "", "BiomeLUT",
                     unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)

    # --- world position / slope --------------------------------------------
    world_pos = b.node(unreal.MaterialExpressionWorldPosition)
    world_z_uu = b.node(unreal.MaterialExpressionComponentMask)
    world_z_uu.set_editor_property("r", False)
    world_z_uu.set_editor_property("g", False)
    world_z_uu.set_editor_property("b", True)
    world_z_uu.set_editor_property("a", False)
    b.link(world_pos, "", world_z_uu, "")
    world_z_m = b.div(world_z_uu, b.const(100.0))  # UU -> metres

    normal = b.node(unreal.MaterialExpressionVertexNormalWS)
    normal_z = b.node(unreal.MaterialExpressionComponentMask)
    normal_z.set_editor_property("r", False)
    normal_z.set_editor_property("g", False)
    normal_z.set_editor_property("b", True)
    normal_z.set_editor_property("a", False)
    b.link(normal, "", normal_z, "")
    # 0 on a flat top face, 1 on a vertical wall.
    slope = b.saturate(b.one_minus(b.abs_(normal_z)))

    # --- surface modifiers --------------------------------------------------
    #
    # ROCK. On the clipmap the normal is a real terrain normal, so a steep face
    # genuinely is exposed rock and this runs at full strength. On voxels the
    # normal is axis-aligned: EVERY step riser on even a gentle grassy hill is a
    # vertical face, so running this at full strength would stripe the whole
    # world grass/rock. The voxel material therefore passes a low
    # rock_slope_strength, where the term reads as natural darkening of the
    # riser rather than as a change of material.
    rock_color = b.vector("RockColor", 0.36, 0.335, 0.30)
    rock_w = b.mul(
        b.ramp(slope, "", b.scalar("RockSlopeLow", 0.30), b.scalar("RockSlopeHigh", 0.80)),
        b.scalar("RockSlopeStrength", rock_slope_strength),
    )

    # BEACH. Flat ground within a few metres of sea level. This is the one place
    # real sand belongs; voxel-core currently labels the entire landmass
    # MAT_SAND, so sand cannot come from the material id.
    beach_color = b.vector("BeachColor", 0.72, 0.65, 0.50)
    near_sea = b.one_minus(b.ramp(b.abs_(world_z_m), "",
                                  b.const(0.0), b.scalar("BeachHeightMeters", 7.0)))
    beach_w = b.mul(near_sea, b.one_minus(slope))

    # SNOW. Driven by temperature FIRST (VertexColor.B, cold = low) with an
    # elevation term OR'd in for the highest ground. Temperature already
    # correlates with elevation in WorldClim, so the temperature term alone puts
    # snow on the uplands; the world Z term only bites above ~2700 m, and this
    # world's highest point is 2897 m, so it is a cap on the very tops.
    # SnowTempMax 0.16 in remapped units is about -4.1 degC, which is roughly
    # the coldest 8-10% of this world's land.
    snow_color = b.vector("SnowColor", 0.90, 0.925, 0.96)
    snow_from_temp = b.one_minus(
        b.ramp(vertex_color, "B", b.scalar("SnowTempMax", 0.16),
               b.add(b.scalar("SnowTempMax", 0.16), b.scalar("SnowTempFeather", 0.10))))
    snow_from_z = b.ramp(world_z_m, "",
                         b.scalar("SnowlineLowMeters", 2700.0),
                         b.scalar("SnowlineHighMeters", 2900.0))
    snow_w = b.saturate(b.maximum(snow_from_temp, snow_from_z))

    # --- compose the surface ------------------------------------------------
    surface = b.lerp(biome, "RGB", rock_color, "", rock_w)
    surface = b.lerp(surface, "", beach_color, "", beach_w)
    surface = b.lerp(surface, "", snow_color, "", snow_w)

    # Palette alpha decides how much of the surface biome takes over. It is 0
    # for subsurface strata (bedrock/rock/gravel/subsoil/mud/clay) and 255 for
    # surface materials, which is what lets ONE graph make a cave look like rock
    # and the hillside above it look like grassland.
    base = b.lerp(subsurface, "", surface, "", tint_weight, "")

    # --- detail -------------------------------------------------------------
    #
    # One texture sample, three octave bands in RGB. Multiplicative around 1.0
    # so it modulates whatever colour the biome path produced instead of
    # imposing a colour of its own.
    detail = b.sample(DETAIL_TEXTURE, detail_uv, detail_uv_out, "DetailTex",
                      unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    fine = b.mul(b.sub(detail, b.const(0.5), "R"), b.scalar("DetailFineStrength", detail_fine_strength))
    coarse = b.mul(b.sub(detail, b.const(0.5), "B"), b.scalar("DetailCoarseStrength", detail_coarse_strength))
    variation = b.add(b.add(b.const(1.0), fine), coarse)
    base = b.mul(base, variation)

    # --- DEBUG WATER MARKER override ----------------------------------------
    #
    # Decodes the sentinel VoxelQuadVertexFactory.ush writes for
    # vxc::MAT_WATERMARK: R=1, B=0, A=0. Applied LAST, after the detail multiply,
    # so the marker is one flat colour rather than magenta with fBm dragged
    # through it -- it is an instrument, and it has to read as "not terrain" at a
    # glance.
    #
    # WHY A SENTINEL AND NOT A PALETTE LOOKUP. This graph has no palette texture
    # sample at all; PALETTE_TEXTURE above is a dead constant. VoxelClimateProbe.h
    # records the measurement that removed it -- VertexColor.R * 255 arrived as
    # ~6-8 where the CPU wrote 4, so categorical id thresholds all read 0. The
    # POOLED path does the id compare in the shader on the quad's own bits, where
    # it is exact, and only then encodes this three-channel sentinel. That is the
    # difference between this and the palette lookup that had to be abandoned.
    #
    # The thresholds are loose because only the ENDPOINTS (0 and 1) are fixed
    # points of the per-channel transform; anything near them may have moved, and
    # a tight epsilon would reintroduce exactly the failure being avoided.
    #
    # COST, STATED: ground at p1 temperature AND p1 precipitation also reads
    # (0,0) and would go magenta. That is the coldest-and-driest corner of the
    # world, in a debug mode that is already not shippable.
    marker_color = b.vector("WaterMarkerColor", 0.90, 0.00, 0.90)
    is_marker = b.mul(
        b.mul(
            b.one_minus(b.ramp(vertex_color, "B", b.const(0.02), b.const(0.06))),
            b.one_minus(b.ramp(vertex_color, "A", b.const(0.02), b.const(0.06))),
        ),
        b.ramp(vertex_color, "R", b.const(0.94), b.const(0.98)),
    )
    base = b.lerp(base, "", marker_color, "", b.saturate(is_marker))

    # --- PHASE 3: WET SHORES, FROM THE BAKED SIGNED DISTANCE FIELD ----------
    #
    # THE LAND SIDE of the same field the water material reads. bathy_shore is
    # NEGATIVE on land and saturates at -100 m, so "within a metre or two of the
    # waterline" is a one-node ramp and needs no height comparison, no water
    # volume query and nothing per-basin.
    #
    # WHY THIS IS THE SINGLE MOST EFFECTIVE ITEM ON THE LIST. A water plane meeting
    # perfectly dry ground is what makes water read as pasted onto terrain rather
    # than sitting in it; every real shoreline has a band of ground that is wet,
    # and the eye reads that band as the join. Nothing else in this plan changes
    # the JOIN.
    #
    # DARKER AND GLOSSIER TOGETHER, which is Lagarde's rule and is not optional.
    # Wet ground is dark because a water film fills the surface microstructure and
    # traps light in it, and that same film is what makes it specular. Darkening
    # alone reads as a dirt texture -- a stain, not water -- and is the standard
    # way this effect goes wrong. The roughness half is applied by the CALLER,
    # which is why `wet` is returned rather than consumed here: the two terrain
    # materials assemble roughness differently and neither can be edited from
    # inside this function.
    #
    # `wet` is 0 wherever the field says nothing, so a run with no fine tier, a
    # world baked before bake_ver 27, and everything beyond the 480 m window all
    # produce exactly today's terrain.
    # VOXEL_SHORE_FX=0 is the LAND HALF of the shoreline-effects A/B arm; the
    # water half lives in create_water_voxel_material.py (SHORE_FX_ENV) and reads
    # the same variable, so one setting moves the whole shoreline treatment and
    # the two halves cannot end up armed differently. See that file for why this
    # is a generation-time arm rather than a runtime parameter -- the reasoning is
    # the one already written down at VOXEL_MATERIAL_DEBUG below.
    #
    # Off leaves `wet` as None, which is the SAME state a world with no fine tier
    # or a pre-bake_ver-27 bake produces, and both callers already guard on it
    # (create_voxel_material.py, create_clipmap_material.py). So the OFF arm is
    # not a new code path being exercised for the first time by a measurement --
    # it is the path that shipped before this feature existed.
    shore_fx = os.environ.get("VOXEL_SHORE_FX", "1").strip().lower() not in (
        "0", "off", "false", "no", "")
    # Logged from HERE rather than from either caller because this is the one
    # place that knows both halves of the answer: whether the arm is on, and
    # whether there was a baked field for it to act on at all. "Arm ON, bathy
    # absent" and "arm OFF" produce an identical material, and a capture pair
    # taken across that pair would show no difference and be read as "the effect
    # does nothing" -- which is why the second clause is in the line.
    unreal.log("TERRAIN SHORE FX ARM: %s (VOXEL_SHORE_FX=%r, baked bathy field %s)"
               % ("ON" if shore_fx else "OFF",
                  os.environ.get("VOXEL_SHORE_FX", "<unset>"),
                  "present" if bathy is not None else "ABSENT -- arm has nothing to act on"))
    wet = None
    if bathy is not None and shore_fx:
        wet_width = b.scalar("WetShoreWidthM", 2.5)
        # -shore, so distance INLAND is positive and the ramp reads forwards.
        inland = b.mul(bathy["shore_m"], b.const(-1.0))
        wet = b.mul(b.one_minus(b.ramp(inland, "", b.const(0.0), wet_width)), bathy["validity"])
        # ...and never on the water side of the line: there the terrain is the
        # lake BED, which is wet in a different sense and is already being graded
        # by the water volume above it. Double-darkening it would make shallows
        # read as a black rim.
        wet = b.mul(wet, b.saturate(inland))
        base = b.mul(base, b.lerp(b.const(1.0), "", b.scalar("WetShoreDarken", 0.55), "", wet))

    # --- generation-time debug bisect ---------------------------------------
    #
    # VOXEL_MATERIAL_DEBUG=<n> in the environment when the material is authored
    # routes an intermediate straight to BaseColor. Generation-time rather than a
    # runtime parameter deliberately: a runtime switch would mean a permanent
    # branch and a permanent extra sampler in the shipping shader, and there is
    # no per-chunk MID plumbing to drive one from the command line anyway. The
    # cost of this is one material regeneration (~15 s) per bisect step.
    #
    #   1 = biome LUT only        2 = palette RGB only     3 = palette ALPHA
    #   4 = vertex B (temperature) 5 = vertex A (precip)   6 = slope
    #   7 = detail texture RGB     8 = vertex R (material id) * 16
    debug = int(os.environ.get("VOXEL_MATERIAL_DEBUG", "0"))
    if debug:
        # UNLIT EMISSIVE, not BaseColor. Reading a probe off a lit surface is
        # useless here: UE's auto-exposure renormalizes overall brightness, so a
        # flat 1.0 surface and a flat 0.35 surface come back as the same mid-grey
        # (this cost several wasted round trips before it was spotted). Exposure
        # scales all three channels equally, so as UNLIT emissive the RATIOS
        # between R/G/B survive it -- which is why the multi-value probes below
        # pack three scalars into RGB rather than testing one at a time.
        b.material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
        probe = {
            1: (biome, "RGB"),
            2: (subsurface, ""),
            3: (tint_weight, ""),
            4: (vertex_color, "B"),
            5: (vertex_color, "A"),
            6: (slope, ""),
            7: (detail, "RGB"),
            8: (b.mul(vertex_color, b.const(1.0), "R"), ""),
            # All three surface modifiers at once: R = rock, G = beach, B = snow.
            # One run instead of three.
            9: (b.append(b.append(rock_w, "", beach_w, ""), "", snow_w, ""), ""),
            10: (b.div(world_z_m, b.const(1000.0)), ""),
            # R = tint weight, G = biome green channel, B = palette red channel.
            11: (b.append(b.append(tint_weight, "", biome, "G"), "", tint_weight, ""), ""),
            # EXPOSURE-PROOF readout of VertexColor.R. G is a known 0.5
            # reference, so VertexColor.R = 0.5 * (R/G) survives auto-exposure
            # (which scales all channels equally). B carries mat_index/64 as a
            # cross-check. This is the probe that finally pins down what the
            # shader actually receives in the material-id byte.
            13: (b.append(b.append(vertex_color, "R", b.const(0.5), ""), "",
                          tint_weight, ""), ""),
        }[debug]
        b.prop(probe[0], probe[1], unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        return probe[0], snow_w, probe[1], wet

    return base, snow_w, "", wet
