"""The HULL WATER EXCLUSION mask, as one importable builder (owner directive,
2026-09-05 boat session: "the water should be masked and not filling the boat
just because the canoe bottom is below the lake water body surface").

WHAT THIS IS. A 0/1 keep-mask both water surfaces multiply into their opacity
mask, culling water pixels that lie inside a hull's exclusion volume -- so the
lake stops drawing through the bottom of a floating canoe. Standard stencil
mechanism: the boat renders an invisible interior volume into CUSTOM DEPTH +
CUSTOM STENCIL, and the water discards where that volume sits in front of the
water pixel within a bounded band.


=============================================================================
THE CONTRACT -- PINNED, BECAUSE THE BOAT-SIDE WRITER IS BUILT AGAINST IT
=============================================================================

STENCIL SEMANTICS: BIT 0 (the LSB) of CustomDepthStencilValue means "water
exclusion volume". Writers set CustomDepthStencilValue = 1. The material
tests the BIT (fmod(stencil, 2) >= 1), not equality, so future stencil users
can claim bits 1..7 without colliding -- this file is the registry of bit 0
and any future bit gets its line here. No other custom-stencil semantics
exist in the project today (verified 2026-09-05: the only custom-depth
plumbing is the chunk components' pass-through bRenderCustomDepth flags).

WHAT THE BOAT-SIDE WRITER MUST DO (the C++ half, built in parallel):

  1. r.CustomDepth=3 (enabled WITH stencil) in DefaultEngine.ini -- it is NOT
     currently set, and without it CustomStencil samples 0 and this whole
     term is inert (which is also the safe failure direction).
  2. AVoxelBoat (and any future hull) gains an exclusion mesh component:
       * a CLOSED, OUTWARD-FACING volume enclosing the water-free interior --
         the cockpit "bathtub" for a fitted look, a simple box for v1. The
         material tests against the volume's NEAR SHELL (front faces), so
         the shape only needs to be right where water could visibly clip.
       * SetRenderCustomDepth(true), SetCustomDepthStencilValue(1),
         SetRenderInMainPass(false) (never visible), no collision, attached
         to the hull so it rides every wave the hull rides.
       * vertical extent: from safely above the highest wave crest that can
         cross the gunwale (waterline + ~0.5 m covers the shipped field's
         crests) down to the inner hull bottom.
  3. Nothing else. No MPC parameter, no per-frame push: the mask reads the
     stencil/depth buffers directly, so with NO writers in the world the
     term multiplies by 1.0 everywhere and the water is PIXEL-IDENTICAL to
     today -- the off arm is the absence of writers, plus the
     HullMaskEnabled scalar below for an explicit material-side A/B.

THE MATERIAL TEST (this file):

    cull = (stencil bit 0 set)
         AND 0 < PixelDepth - CustomDepth < HullMaskMaxDepthUU

i.e. the water pixel lies BEHIND the exclusion volume's near shell, within a
bounded band (default 300 UU = 3 m, canoe-scaled; a parameter so a future
tall hull can widen it per instance). keep = 1 - cull * HullMaskEnabled.

WHY THE BAND EXISTS, AND THE ONE HONEST ARTEFACT. The custom-depth buffer
contains ONLY custom-depth writers -- the ordinary scene does not occlude
them -- so from outside the boat the volume's near shell is still "visible"
to this test through the hull. Unbounded, that would punch a hole in any
water behind the boat that overlaps it on screen. The band bounds the damage
to MaxDepth behind the shell: water within the band but outside the hull is
exactly the water the OPAQUE HULL ITSELF occludes on screen (the volume is
inside the hull), so the artefact hides behind the boat that causes it; water
beyond the band survives untouched. Water IN FRONT of the volume fails the
0 < test and always survives.

WHY OPACITY-MASK DISCARD AND NOT DEPTH TRICKERY: both waters are
BLEND_MASKED Single Layer Water since B4 -- a masked discard is the native
mechanism, it composes with the existing shore-clip mask by one multiply
(0.5 threshold, both terms are hard 0/1 where they act), and a discarded
pixel also skips the SLW volume pass, so the boat's bilge does not pick up
an underwater tint from water that is not drawn.

ENGINE-SUPPORT CAVEAT, STATED RATHER THAN ASSUMED: SceneTexture:CustomDepth /
CustomStencil in a MASKED surface material is documented-supported in UE5
(custom depth is its own prepass, order-independent -- the same argument this
project's scene-DEPTH reads already rely on), but this project has never
compiled one inside an SLW material. The regen chain's "Failed to compile
Material" grep is the verifier; if this engine build refuses the node, the
fallback is an analytic box test driven from a per-boat MID (documented here
so the next person does not re-derive it; NOT built).
"""

import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

DEFAULTS = {
    # The exclusion band depth behind the volume's near shell, UU. 300 = 3 m
    # covers a canoe or small boat interior at any camera angle; widen per
    # instance for a tall hull.
    "HullMaskMaxDepthUU": 300.0,
    # Material-side kill switch for A/Bs; the real off arm is "no writers".
    "HullMaskEnabled": 1.0,
}

# One Custom node: three comparisons and a select read as the mathematics far
# better than six hand-wired expression nodes, and the boundary stays checked
# (every input wired by name, raise-on-failure) -- the standing custom-node
# argument.
HULL_MASK_CODE = """
// keep-mask: 0 where the water pixel is inside a hull exclusion volume.
// Stencil bit 0 = "water exclusion" (the contract at the top of
// water_hull_mask_graph.py). Behind = distance behind the volume's near
// shell along the view ray; the band bound keeps the no-scene-occlusion
// artefact hidden behind the hull that causes it.
float bit0 = fmod(Stencil, 2.0);
float behind = PixelDepthUU - CustomDepthUU;
float cull = (bit0 >= 1.0 && behind > 0.0 && behind < MaxDepthUU) ? 1.0 : 0.0;
return 1.0 - cull * Enabled;
"""


def build_hull_mask(b, defaults=None):
    """The keep-mask. Returns a dict of expressions.

    Arguments:
      b         a builder exposing `.material` and `.mel` (the water_caustics
                contract -- SkyGraphBuilder qualifies; no collection needed,
                this term binds nothing on the MPC).
      defaults  optional dict merged over DEFAULTS.

    Returns keys:
      keep      float 0/1 -- MULTIPLY into MP_OPACITY_MASK (1 everywhere when
                no exclusion writer exists: the inert default).
      cull_node the Custom node, for read-back checks.
    """
    d = dict(DEFAULTS)
    if defaults:
        d.update(defaults)
    mel = b.mel
    material = b.material

    def node(cls, x, y):
        return mel.create_material_expression(material, cls, x, y)

    def link(src, src_out, dst, dst_in, what):
        if not mel.connect_material_expressions(src, src_out, dst, dst_in):
            raise RuntimeError(
                "connect %s (%s.%s -> %s.%s) failed"
                % (what, type(src).__name__, src_out or "<default>",
                   type(dst).__name__, dst_in or "<default>"))

    # SceneTexture reads. Output pin "Color" BY NAME, r-masked -- the same
    # three-outputs argument create_underwater_material.py makes for its
    # scene read: the default pin is Color only by convention.
    stencil_tex = node(unreal.MaterialExpressionSceneTexture, -1300, 3200)
    stencil_tex.set_editor_property(
        "scene_texture_id", unreal.SceneTextureId.PPI_CUSTOM_STENCIL)
    stencil_r = node(unreal.MaterialExpressionComponentMask, -1150, 3200)
    stencil_r.set_editor_property("r", True)
    stencil_r.set_editor_property("g", False)
    stencil_r.set_editor_property("b", False)
    stencil_r.set_editor_property("a", False)
    link(stencil_tex, "Color", stencil_r, "", "CustomStencil.Color -> r mask")

    depth_tex = node(unreal.MaterialExpressionSceneTexture, -1300, 3280)
    depth_tex.set_editor_property(
        "scene_texture_id", unreal.SceneTextureId.PPI_CUSTOM_DEPTH)
    depth_r = node(unreal.MaterialExpressionComponentMask, -1150, 3280)
    depth_r.set_editor_property("r", True)
    depth_r.set_editor_property("g", False)
    depth_r.set_editor_property("b", False)
    depth_r.set_editor_property("a", False)
    link(depth_tex, "Color", depth_r, "", "CustomDepth.Color -> r mask")

    pixel_depth = node(unreal.MaterialExpressionPixelDepth, -1300, 3360)

    max_depth = node(unreal.MaterialExpressionScalarParameter, -1300, 3420)
    max_depth.set_editor_property("parameter_name", "HullMaskMaxDepthUU")
    max_depth.set_editor_property("default_value", float(d["HullMaskMaxDepthUU"]))
    enabled = node(unreal.MaterialExpressionScalarParameter, -1300, 3480)
    enabled.set_editor_property("parameter_name", "HullMaskEnabled")
    enabled.set_editor_property("default_value", float(d["HullMaskEnabled"]))

    cull = node(unreal.MaterialExpressionCustom, -1000, 3300)
    cull.set_editor_property("description", "HullWaterMask")
    cull.set_editor_property("code", HULL_MASK_CODE)
    cull.set_editor_property("output_type",
                             unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    ins = []
    for nm in ("Stencil", "CustomDepthUU", "PixelDepthUU", "MaxDepthUU", "Enabled"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", nm)
        ins.append(ci)
    cull.set_editor_property("inputs", ins)
    link(stencil_r, "", cull, "Stencil", "stencil -> HullWaterMask.Stencil")
    link(depth_r, "", cull, "CustomDepthUU", "custom depth -> HullWaterMask.CustomDepthUU")
    link(pixel_depth, "", cull, "PixelDepthUU", "pixel depth -> HullWaterMask.PixelDepthUU")
    link(max_depth, "", cull, "MaxDepthUU", "HullMaskMaxDepthUU -> HullWaterMask.MaxDepthUU")
    link(enabled, "", cull, "Enabled", "HullMaskEnabled -> HullWaterMask.Enabled")

    return {
        "keep": cull,
        "cull_node": cull,
    }
