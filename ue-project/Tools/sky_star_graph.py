"""Shared star-dome graph: MPC_VoxelSky's identity, the checked-binding
GraphBuilder, and the ONE equirectangular celestial star-map lookup.

WHY THIS FILE EXISTS RATHER THAN A SECOND COPY OF THE DERIVATION
---------------------------------------------------------------
Two materials now sample T_SkyStarmap at the same sidereal rotation:

  * M_NightSky            (Tools/create_sky_material.py) -- the VISIBLE stars,
                          additive, main view only, depth-tested.
  * M_SkyAtmosphereDome   (Tools/create_sky_atmosphere_dome_material.py) -- the
                          IsSky dome, whose reflection-pass branch feeds the
                          SkyLight's real-time capture so the Milky Way becomes
                          real ambient light.

If those two ever disagree about the horizon->equatorial rotation, the U
handedness or the seam fix, the ambient light arrives from a sky that is NOT the
sky on screen -- and nothing in a frame would say so, because both would still
be sharp, still rotate at the right rate, and still have the pole at the right
altitude. That is precisely the failure VoxelClimateProbe.h documents at length:
four independent copies of one derivation drifting apart until the whole world
classified as desert. One graph, one definition, one answer -- the same argument
terrain_material_common.py makes for sharing one biome graph between
M_VoxelTerrain and M_VoxelClipmap.

So the derivation below is the ONLY copy. create_sky_material.py's module
docstring keeps the rest of its reasoning (blend mode, moon disc, terminator)
and points here for the star maths.


================================================================================
HORIZON -> EQUATORIAL
================================================================================

The star map is in celestial coordinates, so this is the step that makes the sky
ROTATE correctly instead of being painted on the inside of the dome. With
phi = ObserverLatitude and d = (dN, dE, dU):

    Ex = dU*cos(phi) - dN*sin(phi)      == cos(dec) * cos(H)
    Ey = -dE                           == cos(dec) * sin(H)
    Ez = dN*cos(phi) + dU*sin(phi)      == sin(dec)

That is the standard alt/az -> hour-angle/declination rotation written
rectangularly (a single rotation by 90-phi about the east axis, which is what
puts the celestial pole at altitude phi where it belongs). Two checks that pin
the signs:

    * north celestial pole: it must sit at altitude phi, azimuth 0, i.e.
      d = (cos phi, 0, sin phi). Then Ez = cos^2 + sin^2 = 1 (dec = +90) and
      Ex = sin(phi)cos(phi) - cos(phi)sin(phi) = 0. Correct.
    * observer on the equator (phi = 0) looking at the zenith, d = (0,0,1):
      Ex = 1, Ez = 0, so dec = 0 and H = 0 -- the celestial equator crossing the
      meridian. Correct.

    dec = asin(clamp(Ez, -1, 1))
    H   = atan2(Ey, Ex)                 hour angle, increasing westward
    RA  = LST - H

VIEW DIRECTION. d = normalize(-CameraVectorWS). CameraVectorWS points from the
shaded pixel TOWARD the camera, so its negation is the direction of gaze. In
this project's world frame that is d = (north, east, up) directly --
VoxelEphemeris.h:44-48 fixes X=north, Y=east, Z=up and defines every ephemeris
direction as (cos(Alt)cos(Az), cos(Alt)sin(Az), sin(Alt)) in exactly that frame.
So SunDirection and MoonDirection need no transform at all on the way in.


================================================================================
EQUIRECT UV
================================================================================

    v = 0.5 - dec/pi              V=0 is the top row = dec +90 = north pole
    u = StarRotation + StarUDirection * (-H / 2pi)

RA/2pi appears only as (LST - H)/2pi, and LST/2pi is a constant per frame -- so
it is folded wholesale into StarRotation. That is what makes StarRotation "a
rotation the game supplies": one scalar in turns carrying local sidereal time
plus the map's RA origin offset, with no second knob to keep in sync.


================================================================================
THE SEAM
================================================================================

atan2's branch cut (Ey = 0, Ex < 0) makes u jump by exactly 1. The FETCH is
unharmed -- import_sky_textures.py sets TA_WRAP in U precisely because right
ascension wraps -- but the hardware's mip selection is not: ddx(u) at the seam
column is ~1.0 instead of ~1e-4, the sampler picks the lowest mip, and the
result is a permanent blurred meridian across the sky that shimmers as the view
turns. The fix is to sample with EXPLICIT derivatives (TMVM_Derivative) and
wrap-correct the U component of each:

    dudx' = dudx - round(dudx)          a +/-1 jump maps back to ~0

which is exact everywhere except within a pixel or two of the celestial poles,
where du/dx legitimately exceeds 0.5 because all meridians converge there. Near
the poles the correction can pick a slightly wrong mip; the visible consequence
is a small soft spot exactly at the pole, which is the benign end of equirect's
inherent pole singularity and is the reason mips were enabled on this texture in
the first place (import_sky_textures.py:22-26).
"""

import os
import sys

import unreal

# Shared GraphBuilder, same directory. A -run=pythonscript commandlet does not
# put the script's own directory on sys.path, so add it explicitly (same as
# create_voxel_material.py:56-58).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from terrain_material_common import GraphBuilder, load_texture  # noqa: E402

PACKAGE_PATH = "/Game/Voxel"
COLLECTION_NAME = "MPC_VoxelSky"
COLLECTION_PATH = PACKAGE_PATH + "/" + COLLECTION_NAME

STARMAP_TEXTURE = "/Game/Voxel/T_SkyStarmap.T_SkyStarmap"

PI = 3.14159265358979
TWO_PI = 2.0 * PI
DEG2RAD = PI / 180.0


def load_collection():
    """Load MPC_VoxelSky, or raise naming the script that authors it.

    unreal.load_object rather than EditorAssetLibrary.load_asset, for the reason
    terrain_material_common.load_texture states: a -run=pythonscript commandlet
    does not wait for the asset registry's background scan.

    THE ORDERING THIS ENFORCES. create_sky_material.py DELETES and recreates the
    collection every run (create_sky_material.py:499-506). Any script that binds
    to the collection must therefore run AFTER it, and must re-check that the
    parameters it needs are present -- which is what
    SkyGraphBuilder.collection_param does on every single binding.
    """
    collection = unreal.load_object(None, COLLECTION_PATH + "." + COLLECTION_NAME)
    if collection is None:
        raise RuntimeError(
            "failed to load %s -- run Tools/create_sky_material.py FIRST. It is the "
            "single author of this collection (it deletes and recreates the asset), so "
            "every other sky material script runs after it, never before."
            % COLLECTION_PATH)
    return collection


class SkyGraphBuilder(GraphBuilder):
    """GraphBuilder plus the nodes the sky graphs need.

    link() is overridden only to widen the error message: when a connect fails
    the usual cause is a pin NAME, and the fastest fix is seeing the list of
    names the node actually has. The failure is still a raise, never a warn --
    create_clipmap_material.py:3-5.
    """

    def __init__(self, material, collection):
        GraphBuilder.__init__(self, material)
        self.collection = collection
        self._mpc_names = None  # lazily read back from the MPC, see collection_param

    def link(self, src, src_out, dst, dst_in):
        if not self.mel.connect_material_expressions(src, src_out, dst, dst_in):
            try:
                names = [str(n) for n in self.mel.get_material_expression_input_names(dst)]
            except Exception:  # noqa: BLE001 -- diagnostics only, the raise below stands
                names = ["<could not enumerate>"]
            raise RuntimeError(
                "connect %s.%s -> %s.%s failed; %s inputs are %s"
                % (type(src).__name__, src_out or "<default>",
                   type(dst).__name__, dst_in or "<default>",
                   type(dst).__name__, names))

    # --- parameters ---------------------------------------------------------

    def mpc_names(self):
        """Every parameter name on the collection, as READ BACK from the asset."""
        if self._mpc_names is None:
            self._mpc_names = set()
            for prop in ("scalar_parameters", "vector_parameters"):
                for p in self.collection.get_editor_property(prop):
                    self._mpc_names.add(str(p.get_editor_property("parameter_name")))
        return self._mpc_names

    def collection_param(self, name):
        """A CollectionParameter bound to MPC_VoxelSky, with the binding CHECKED.

        Order is load-bearing: Collection must be set before ParameterName,
        because PostEditChangeProperty resolves ParameterId from
        Collection->GetParameterId(ParameterName) every time either changes
        (MaterialExpressions.cpp:17163-17177) and a null Collection blanks it.
        """
        # The binding is checked by NAME MEMBERSHIP, not by reading ParameterId
        # back. ParameterId is not exposed to Python on UE 5.8 ("Failed to find
        # property 'parameter_id'", measured 2026-07-29) and neither is
        # FCollectionParameterBase::Id, so the guid route is closed.
        #
        # Membership is the stronger check anyway, and it is complete. The only
        # way a CollectionParameter fails to resolve is a name that is not on the
        # MPC -- and comparing against the collection as READ BACK (not against
        # any script's own literals) also catches the case where the MPC write
        # silently dropped a parameter, or where create_sky_material.py was
        # re-run from an older revision that predates a parameter this graph
        # needs.
        #
        # This matters because an unresolved collection parameter does NOT fail
        # to compile: UMaterialExpressionCollectionParameter::Compile emits a
        # CONSTANT instead (MaterialExpressions.cpp:17179-17193). A typo here
        # would ship a black sky, or frozen stars, with no error anywhere.
        if name not in self.mpc_names():
            raise RuntimeError(
                "CollectionParameter %r is not on %s. Present: %s. An unresolved "
                "collection parameter compiles to a CONSTANT rather than failing, "
                "so this must raise here. If the name looks right, re-run "
                "Tools/create_sky_material.py -- it is the sole author of this "
                "collection and it recreates the asset from scratch."
                % (name, COLLECTION_PATH, sorted(self.mpc_names())))

        n = self.node(unreal.MaterialExpressionCollectionParameter)
        n.set_editor_property("collection", self.collection)
        n.set_editor_property("parameter_name", name)
        # Order is load-bearing (see docstring); verify it took.
        if str(n.get_editor_property("parameter_name")) != name:
            raise RuntimeError("CollectionParameter name did not round-trip for %r" % name)
        if n.get_editor_property("collection") is None:
            raise RuntimeError("CollectionParameter %r lost its Collection" % name)
        return n

    # --- small nodes --------------------------------------------------------

    def mask(self, src, src_out, r=False, g=False, b=False, a=False):
        n = self.node(unreal.MaterialExpressionComponentMask)
        n.set_editor_property("r", r)
        n.set_editor_property("g", g)
        n.set_editor_property("b", b)
        n.set_editor_property("a", a)
        self.link(src, src_out, n, "")
        return n

    def xyz(self, src, src_out=""):
        return self.mask(src, src_out, r=True, g=True, b=True)

    def unary(self, cls, a, a_out=""):
        n = self.node(cls)
        self.link(a, a_out, n, "")
        return n

    # RADIANS IN, AND THE Period PROPERTY IS WHAT MAKES THAT TRUE.
    #
    # UMaterialExpressionSine/Cosine DO NOT COMPUTE sin(x). They default to
    # Period = 1.0 (MaterialExpressionSine.h:22, 5.8) and compile to
    #
    #     Sine:   Period > 0 ? sin(Input * 2pi/Period) : sin(Input)
    #     Cosine: cos(Input * (Period > 0 ? 2pi/Period : 0))
    #                            -- MaterialExpressions.cpp:5729, :5747
    #
    # so a node left at its default turns a RADIAN input into sin(2pi*x): every
    # angle in this file silently multiplied by 6.283. That is not a rounding
    # error, it is a different angle, and NOTHING REPORTS IT -- the graph
    # compiles, the sky renders, and the numbers are wrong. It is the same class
    # of silent-wrong-value defect as the unresolved CollectionParameter above
    # (which compiles to a constant), and it gets the same treatment: set the
    # property explicitly, then read it back and raise.
    #
    # WHAT IT COST BEFORE IT WAS FOUND, so nobody "simplifies" this away:
    #   * build_star_uv's sin/cos of ObserverLatitude. At the shipped 52 N,
    #     sin(2pi*0.9076) = -0.548 and cos(2pi*0.9076) = +0.837, which is the
    #     honest horizon frame for latitude -33.2 -- the star field was built for
    #     the WRONG HEMISPHERE, and so was the starlight the SkyLight captured
    #     from M_SkyAtmosphereDome.
    #   * create_sky_material.build_moon's sin(MoonAngularRadius). sin_radius
    #     came out 6.283x too large, so the disc mask reached 1.63 deg instead of
    #     0.26 -- a moon 6.3x too wide and ~40x too large in area.
    #
    # 2*pi, NOT 0. Period = 0 is the documented "no period" path for SINE only.
    # Cosine's compile multiplies by a literal 0 in that branch, i.e. cos(0) = 1,
    # a CONSTANT -- so the one value that is safe for both nodes is 2*pi, which
    # makes the scale factor 2pi/2pi = 1 and leaves the input in radians.
    _PERIOD_RADIANS = TWO_PI

    def _trig(self, cls, a, a_out):
        n = self.node(cls)
        n.set_editor_property("period", self._PERIOD_RADIANS)
        got = float(n.get_editor_property("period"))
        if abs(got - self._PERIOD_RADIANS) > 1.0e-4:
            raise RuntimeError(
                "%s.Period did not round-trip: wrote %.9f, read back %.9f. Left at the "
                "engine default of 1.0 this node computes sin/cos(2pi*x) instead of "
                "sin/cos(x) (MaterialExpressions.cpp:5729/:5747) and every angle in this "
                "graph is silently multiplied by 6.283 with no error anywhere."
                % (cls.__name__, self._PERIOD_RADIANS, got))
        self.link(a, a_out, n, "")
        return n

    def sine(self, a, a_out=""):
        return self._trig(unreal.MaterialExpressionSine, a, a_out)

    def cosine(self, a, a_out=""):
        return self._trig(unreal.MaterialExpressionCosine, a, a_out)

    def arcsine(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionArcsine, a, a_out)

    def sqrt(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionSquareRoot, a, a_out)

    def normalize(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionNormalize, a, a_out)

    def round_(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionRound, a, a_out)

    def ddx(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionDDX, a, a_out)

    def ddy(self, a, a_out=""):
        return self.unary(unreal.MaterialExpressionDDY, a, a_out)

    def neg(self, a, a_out=""):
        return self.mul(a, self.const(-1.0), a_out, "")

    def dot(self, a, b, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionDotProduct, a, a_out, b, b_out)

    def cross(self, a, b, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionCrossProduct, a, a_out, b, b_out)

    def dist(self, a, b, a_out="", b_out=""):
        return self.binary(unreal.MaterialExpressionDistance, a, a_out, b, b_out)

    def atan2(self, y, x, y_out="", x_out=""):
        n = self.node(unreal.MaterialExpressionArctangent2)
        self.link(y, y_out, n, "Y")
        self.link(x, x_out, n, "X")
        return n

    def clamp(self, value, lo, hi, value_out=""):
        n = self.node(unreal.MaterialExpressionClamp)
        # Clamp's first input is UNNAMED -- GetInputName reports it as 'None',
        # so the pin list is ['None', 'Min', 'Max'] and "Input" does not match.
        # Measured 2026-07-29 by the checked-connect assertion in link(), which
        # is exactly what that assertion exists for: a silently-failed connect
        # here would have left the clamp reading 0 and the star field blank.
        self.link(value, value_out, n, "")
        self.link(lo, "", n, "Min")
        self.link(hi, "", n, "Max")
        return n

    def smoothstep(self, lo, hi, value, value_out=""):
        n = self.node(unreal.MaterialExpressionSmoothStep)
        self.link(lo, "", n, "Min")
        self.link(hi, "", n, "Max")
        self.link(value, value_out, n, "Value")
        return n

    def sample_derivative(self, texture_path, param_name, sampler_type,
                          uv, ddx_uv, ddy_uv):
        """TextureSampleParameter2D with EXPLICIT derivatives.

        MipValueMode MUST be set before the DDX/DDY pins are connected: the pin
        NAMES only exist in Derivative mode (UMaterialExpressionTextureSample::
        GetInputName, MaterialExpressions.cpp:2743-2749). Set it afterwards and
        the connects fail -- loudly, via link(), but pointlessly.
        """
        n = self.node(unreal.MaterialExpressionTextureSampleParameter2D)
        n.set_editor_property("parameter_name", param_name)
        n.set_editor_property("texture", load_texture(texture_path))
        n.set_editor_property("sampler_type", sampler_type)
        n.set_editor_property("mip_value_mode", unreal.TextureMipValueMode.TMVM_DERIVATIVE)
        self.link(uv, "", n, "UVs")
        self.link(ddx_uv, "", n, "DDX(UVs)")
        self.link(ddy_uv, "", n, "DDY(UVs)")
        return n

    # sample() (hardware mip selection) is inherited from GraphBuilder unchanged
    # -- it is what the moon disc uses.


# ===========================================================================
# THE SHARED SUBGRAPH
# ===========================================================================


def build_view_direction(b):
    """(view, view_z) -- the pixel's gaze direction in (north, east, up).

    Returns the normalized negation of CameraVectorWS. See the module docstring
    under "HORIZON -> EQUATORIAL" for why no frame transform is needed.

    THIS IS ALSO CORRECT IN THE SKYLIGHT'S CAPTURE VIEW, which matters for
    M_SkyAtmosphereDome: FScene::AllocateAndCaptureFrameSkyEnvMap builds six
    real cube-face view matrices from the SkyLight's CapturePosition
    (ReflectionEnvironmentRealTimeCapture.cpp:540-567), so -CameraVector is the
    per-texel ray direction there exactly as it is on screen. The star map
    therefore lands in the capture at the same sidereal rotation it is drawn at.
    """
    camera_vector = b.node(unreal.MaterialExpressionCameraVectorWS)
    view = b.normalize(b.neg(camera_vector))
    view_z = b.mask(view, "", b=True)
    return view, view_z


def build_horizon_fade(b, view_z):
    """smoothstep(-StarHorizonFade, +StarHorizonFade, view.z).

    Stars and moon vanish THROUGH the horizon line rather than showing through
    gaps under the terrain, and it doubles as a crude stand-in for horizon
    extinction on a rising moon.

    In the SkyLight capture it earns its place a second way: without it the
    capture would integrate a full star field over the LOWER hemisphere too --
    starlight arriving from below the ground -- which SH-projects into a term
    that brightens DOWN-facing surfaces out of nowhere. The atmosphere's own
    raymarch already returns the dark planet ground for those directions, so
    fading the star branch there keeps the two consistent.
    """
    fade_half_width = b.collection_param("StarHorizonFade")
    return b.smoothstep(b.neg(fade_half_width), fade_half_width, view_z)


def build_star_uv(b, view, view_z):
    """(uv, ddx_fixed, ddy_fixed) for the equirect star map, seam-corrected.

    The whole derivation is in the module docstring; this is the only place it
    is expressed as a graph.
    """
    # --- observer latitude, degrees -> radians ------------------------------
    lat_rad = b.mul(b.collection_param("ObserverLatitude"), b.const(DEG2RAD))
    sin_lat = b.sine(lat_rad)
    cos_lat = b.cosine(lat_rad)

    d_n = b.mask(view, "", r=True)
    d_e = b.mask(view, "", g=True)
    d_u = view_z

    # Horizon -> equatorial, rectangular. See the module docstring for the two
    # checks that pin these signs (celestial pole at altitude phi; equator
    # zenith on the meridian).
    e_x = b.sub(b.mul(d_u, cos_lat), b.mul(d_n, sin_lat))     # cos(dec)cos(H)
    e_y = b.neg(d_e)                                          # cos(dec)sin(H)
    e_z = b.add(b.mul(d_n, cos_lat), b.mul(d_u, sin_lat))     # sin(dec)

    # asin's input is a dot of unit vectors, so it is in [-1,1] mathematically
    # but not necessarily after float rounding. Clamp before the arcsine.
    dec = b.arcsine(b.clamp(e_z, b.const(-1.0), b.const(1.0)))
    v = b.sub(b.const(0.5), b.mul(dec, b.const(1.0 / PI)))

    hour_angle = b.atan2(e_y, e_x)
    ra_turns = b.mul(hour_angle, b.const(-1.0 / TWO_PI))
    u = b.add(b.collection_param("StarRotation"),
              b.mul(b.collection_param("StarUDirection"), ra_turns))

    uv = b.append(u, "", v, "")

    # --- seam-safe derivatives ---------------------------------------------
    #
    # u jumps by exactly 1 across atan2's branch cut. TA_WRAP makes the FETCH
    # correct there; it does not make the hardware's mip selection correct, and
    # a one-texel column at the lowest mip is a permanent blurred meridian.
    # Subtracting round() folds a +/-1 jump back to ~0 and leaves every other
    # derivative untouched. See "THE SEAM" in the module docstring.
    ddx_uv = b.ddx(uv)
    ddy_uv = b.ddy(uv)
    du_dx = b.mask(ddx_uv, "", r=True)
    dv_dx = b.mask(ddx_uv, "", g=True)
    du_dy = b.mask(ddy_uv, "", r=True)
    dv_dy = b.mask(ddy_uv, "", g=True)
    ddx_fixed = b.append(b.sub(du_dx, b.round_(du_dx)), "", dv_dx, "")
    ddy_fixed = b.append(b.sub(du_dy, b.round_(du_dy)), "", dv_dy, "")

    return uv, ddx_fixed, ddy_fixed


def sample_starmap(b, uv, ddx_fixed, ddy_fixed, param_name="StarmapTex"):
    """The T_SkyStarmap sample node. Read its RGB output, not its default."""
    # SAMPLERTYPE_LINEAR_COLOR, not COLOR: T_SkyStarmap is TC_HDR with sRGB OFF
    # (import_sky_textures.py:82-84), and GetSamplerTypeForTexture maps
    # (non-sRGB, non-special compression) to LinearColor
    # (MaterialExpressionUtils.cpp:45-47). A mismatch is a compile error, not a
    # silent wrong colour -- but it is a compile error that would only show up
    # in the editor, so get it right here.
    return b.sample_derivative(
        STARMAP_TEXTURE, param_name,
        unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR,
        uv, ddx_fixed, ddy_fixed)
