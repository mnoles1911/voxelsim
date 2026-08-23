#!/usr/bin/env python3
"""Execute the terrain material graph builders against a stub `unreal`.

WHAT THIS IS FOR
----------------
`create_voxel_material.py` is a `-run=pythonscript` commandlet. One editor per
box is a hard rule in this repo, so a graph change lands written but not run,
and "unrun" has until now meant *nothing has executed this code at all* -- not
the node creation, not the pin names, not even a misspelled method on
GraphBuilder. The last palette step shipped in that state for a fortnight.

This closes the cheapest part of that gap. It stands in a recording stub for
`unreal`, runs `build_terrain_base_color` down both of its paths, and fails on
anything that would have thrown on the box: a missing expression class, a method
GraphBuilder does not have, a wrong argument count, a pin name the editor would
reject.

WHAT IT IS NOT, and the distinction matters because overselling it is worse than
not having it. The stub accepts every connection, so it cannot tell a
well-formed graph from a CORRECT one -- it does not know that Multiply takes A
and B, that a float3 will not fit a scalar pin, or that the composition is in the
right order. Only the editor knows those. What a capture must show is written
down at the call site in create_voxel_material.py and is still the real check.

So: this says the code RUNS. It does not say the material is right.

Run:
    python3 tools/check-terrain-graph.py
"""
import sys
import tempfile
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "ue-project" / "Tools"


class Expression:
    """A recorded node. Every `set_editor_property` is kept so a failure can
    say which node it was rather than `<Expression object at 0x...>`."""

    def __init__(self, cls_name):
        self.cls_name = cls_name
        self.props = {}
        self.inputs = {}

    def set_editor_property(self, name, value):
        self.props[name] = value

    def get_editor_property(self, name):
        return self.props.get(name)

    def __repr__(self):
        label = self.props.get("parameter_name") or self.props.get("coordinate_index")
        return f"{self.cls_name}({label})" if label is not None else self.cls_name


class Material(Expression):
    def __init__(self):
        super().__init__("Material")


class MaterialEditingLibrary:
    """Records instead of building, and CHECKS THE ARGUMENTS IT CAN.

    Returning True unconditionally from connect would make this stub agree with
    anything, including a call that passed a node where a string belongs -- the
    exact class of typo it exists to catch. So the types are checked here even
    though the editor's own failure mode is a silent False.
    """

    created = []
    connections = []
    properties = []

    @classmethod
    def create_material_expression(cls, material, expr_cls, x=0, y=0):
        if not isinstance(expr_cls, type) or not issubclass(expr_cls, Expression):
            raise TypeError(f"not a material expression class: {expr_cls!r}")
        node = expr_cls()
        cls.created.append(node)
        return node

    @classmethod
    def connect_material_expressions(cls, src, src_out, dst, dst_in):
        for name, value in (("src", src), ("dst", dst)):
            if not isinstance(value, Expression):
                raise TypeError(
                    f"connect_material_expressions {name} is {value!r}, not a node "
                    "-- an argument order or an unassigned builder result")
        for name, value in (("src_out", src_out), ("dst_in", dst_in)):
            if not isinstance(value, str):
                raise TypeError(
                    f"connect_material_expressions {name} is {value!r}, not a pin "
                    "name -- almost always a missing \"\" in a builder call")
        cls.connections.append((src, src_out, dst, dst_in))
        return True

    @classmethod
    def connect_material_property(cls, src, src_out, prop):
        if not isinstance(src, Expression):
            raise TypeError(f"connect_material_property src is {src!r}, not a node")
        cls.properties.append((src, src_out, prop))
        return True


def _enum(*names):
    ns = types.SimpleNamespace()
    for n in names:
        setattr(ns, n, n)
    return ns


def install_stub():
    """A module named `unreal` with just the surface these builders touch."""
    u = types.ModuleType("unreal")

    # Every MaterialExpression* the graph builders name. Generated rather than
    # listed so a NEW node type fails with "the stub has no X", which names the
    # thing to add, instead of an AttributeError deep inside a builder.
    class _Missing:
        def __getattr__(self, name):
            if name.startswith("MaterialExpression"):
                cls = type(name, (Expression,), {"__init__": lambda s, n=name: Expression.__init__(s, n)})
                setattr(u, name, cls)
                return cls
            raise AttributeError(
                f"tools/check-terrain-graph.py's `unreal` stub has no {name}. "
                "Add it if the graph now needs it.")

    u.__getattr__ = _Missing().__getattr__
    u.MaterialEditingLibrary = MaterialEditingLibrary
    u.Material = Material
    u.LinearColor = lambda r=0, g=0, b=0, a=1: (r, g, b, a)
    u.MaterialSamplerType = _enum("SAMPLERTYPE_COLOR", "SAMPLERTYPE_LINEAR_COLOR",
                                  "SAMPLERTYPE_MASKS")
    u.MaterialProperty = _enum("MP_EMISSIVE_COLOR", "MP_BASE_COLOR", "MP_ROUGHNESS",
                               "MP_OPACITY_MASK", "MP_NORMAL")
    u.MaterialShadingModel = _enum("MSM_UNLIT", "MSM_DEFAULT_LIT")
    # load_object must return something truthy: load_texture() raises on None,
    # and that raise is the real behaviour on a box with no baked textures.
    u.load_object = lambda outer, path: Expression("Texture:" + str(path))
    u.log = lambda *a, **k: None
    u.log_warning = lambda *a, **k: None
    sys.modules["unreal"] = u
    return u



# ---------------------------------------------------------------------------
# THE EVALUATOR: what the graph COMPUTES, not just whether it builds
# ---------------------------------------------------------------------------
#
# Everything above answers "does this code run" -- nodes exist, methods resolve,
# arg counts match, the two surviving modifiers are reachable. None of it can
# tell a well-formed graph from a CORRECT one, and the header above says so.
#
# It does not have to stay that way. The stub already records the entire DAG:
# `connections` is (src, src_out, dst, dst_in) with the real pin names, and
# every set_editor_property is kept on the node. That is enough to EVALUATE the
# graph -- walk it post-order, one small function per expression type, with the
# leaves supplied by the caller -- and once it can be evaluated, ADR-0009's
# composition claims stop being things a screenshot has to adjudicate.
#
# WHAT THIS DOES AND DOES NOT REPLACE. It checks the COMPOSITION: the order of
# the stages, what the modifiers do to the material's colour, and what happens
# on a pixel the palette never reached. It does NOT check the tint arithmetic
# itself -- light and hue arrive here as inputs, exactly as they arrive at the
# real material through TexCoords[5]. tools/check-palette-parity.py owns that
# half, by compiling the shipped .ush and running it against vxc::voxelTint.
#
# The two together are what make the editor trip a confirmation. What is left
# for it is the genuinely visual: cave strata, a dry cliff reading as rock,
# snow on a mountain, and no mottle seam at a ring boundary.


def _vec(v):
    """Everything is a float list, so scalars and vectors compose like HLSL."""
    if isinstance(v, (int, float)):
        return [float(v)]
    return [float(x) for x in v]


def _broadcast(a, b):
    """HLSL promotes a scalar against a vector, and rejects mismatched vectors."""
    a, b = _vec(a), _vec(b)
    if len(a) == len(b):
        return a, b
    if len(a) == 1:
        return a * len(b), b
    if len(b) == 1:
        return a, b * len(a)
    raise ValueError(f"cannot combine a float{len(a)} with a float{len(b)}")


def _zip(f, a, b):
    a, b = _broadcast(a, b)
    return [f(x, y) for x, y in zip(a, b)]


def _pin(value, out):
    """Take an output pin off a value. '' is the whole thing."""
    v = _vec(value)
    if out in ("", None):
        return v
    if out == "RGB":
        return v[:3]
    lane = {"R": 0, "G": 1, "B": 2, "A": 3}.get(out)
    if lane is None:
        raise ValueError(f"unknown output pin {out!r}")
    if lane >= len(v):
        raise ValueError(f"pin {out} off a float{len(v)}")
    return [v[lane]]


class GraphEvaluator:
    """Numeric evaluation of a recorded graph.

    `leaves` supplies the values the editor would get from the mesh and the
    vertex factory: vertex colour, the three palette UVs, the world position and
    the vertex normal. `textures` supplies what each named texture sampler
    returns, since a sample is a leaf here too.
    """

    def __init__(self, connections, leaves, textures):
        self.leaves = leaves
        self.textures = textures
        self.incoming = {}
        for src, src_out, dst, dst_in in connections:
            # LAST WRITE WINS, matching the editor: connecting a pin twice
            # replaces the first connection rather than adding to it.
            self.incoming.setdefault(id(dst), {})[dst_in] = (src, src_out)
        self._memo = {}

    def _in(self, node, pin):
        edge = self.incoming.get(id(node), {}).get(pin)
        if edge is None:
            raise ValueError(f"{node.cls_name} has nothing connected to pin {pin!r}")
        src, src_out = edge
        return _pin(self.eval(src), src_out)

    def _has(self, node, pin):
        return pin in self.incoming.get(id(node), {})

    def eval(self, node):
        if id(node) in self._memo:
            return self._memo[id(node)]
        value = self._eval(node)
        self._memo[id(node)] = value
        return value

    def _eval(self, node):
        c, p = node.cls_name, node.props
        short = c.replace("MaterialExpression", "")

        # --- leaves the mesh and the vertex factory supply ------------------
        if short == "VertexColor":
            return _vec(self.leaves["vertex_color"])
        if short == "VertexNormalWS":
            return _vec(self.leaves["normal"])
        if short in ("WorldPosition", "CameraPositionWS"):
            return _vec(self.leaves["world_pos" if short == "WorldPosition"
                                    else "camera_pos"])
        if short == "TextureCoordinate":
            idx = int(p.get("coordinate_index", 0))
            if idx not in self.leaves["texcoords"]:
                raise ValueError(f"no leaf value supplied for UV{idx}")
            return _vec(self.leaves["texcoords"][idx])

        # --- authored constants ---------------------------------------------
        if short == "Constant":
            return [float(p["r"])]
        if short == "Constant3Vector":
            return _vec(p["constant"][:3])
        if short == "ScalarParameter":
            return [float(p["default_value"])]
        if short == "VectorParameter":
            return _vec(p["default_value"][:3])

        # --- a texture sample is a leaf too ----------------------------------
        if short.startswith("TextureSampleParameter"):
            name = p.get("parameter_name")
            if name not in self.textures:
                raise ValueError(f"no stub value for texture sampler {name!r}")
            fn = self.textures[name]
            return _vec(fn(self._in(node, "UVs")) if callable(fn) else fn)

        # --- DitherTemporalAA, which only ever feeds OpacityMask -------------
        if short == "MaterialFunctionCall":
            # Returned as a sentinel rather than a number: nothing downstream of
            # BaseColor may consume it, and a NaN would make that violation loud
            # instead of plausible.
            return [float("nan")]

        # --- arithmetic -------------------------------------------------------
        if short == "Add":
            return _zip(lambda x, y: x + y, self._in(node, "A"), self._in(node, "B"))
        if short == "Subtract":
            return _zip(lambda x, y: x - y, self._in(node, "A"), self._in(node, "B"))
        if short == "Multiply":
            return _zip(lambda x, y: x * y, self._in(node, "A"), self._in(node, "B"))
        if short == "Divide":
            return _zip(lambda x, y: x / y, self._in(node, "A"), self._in(node, "B"))
        if short == "Max":
            return _zip(max, self._in(node, "A"), self._in(node, "B"))
        if short == "Distance":
            a, b = _broadcast(self._in(node, "A"), self._in(node, "B"))
            return [sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5]
        if short == "Abs":
            return [abs(x) for x in self._in(node, "")]
        if short == "OneMinus":
            return [1.0 - x for x in self._in(node, "")]
        if short == "Saturate":
            return [min(1.0, max(0.0, x)) for x in self._in(node, "")]
        if short == "AppendVector":
            return self._in(node, "A") + self._in(node, "B")
        if short == "LinearInterpolate":
            a, bb = _broadcast(self._in(node, "A"), self._in(node, "B"))
            alpha = self._in(node, "Alpha")
            alpha = alpha * len(a) if len(alpha) == 1 else alpha
            return [x + (y - x) * t for x, y, t in zip(a, bb, alpha)]
        if short == "ComponentMask":
            v = self._in(node, "")
            lanes = [i for i, k in enumerate("rgba") if p.get(k)]
            return [v[i] for i in lanes]

        raise ValueError(f"the evaluator does not model {c}. Add it -- an "
                         "unmodelled node cannot be silently skipped, because a "
                         "skipped node is an unchecked one.")


# The composition ADR-0009 specifies, in three lines, so the expected value is
# written where it can be read next to the assertions that use it. This is the
# same arithmetic as vxc::applyTintQ16 and VoxelApplyVariation; that those two
# agree with EACH OTHER is check-palette-parity.py's job, and it compiles the
# shipped shader to say so rather than mirroring it. What is checked here is
# that the GRAPH performs it, on the right operand, in the right order.
def apply_variation(rgb, light, hue):
    c = [x * (1.0 + light) for x in rgb]
    c[0] *= 1.0 + hue
    c[2] *= 1.0 - hue
    return [max(x, 0.0) for x in c]


def close(a, b, tol=1e-6):
    a, b = _vec(a), _vec(b)
    return len(a) == len(b) and all(abs(x - y) <= tol for x, y in zip(a, b))


def check_composition(tmc, unreal_mod, failures):
    """Evaluate the palette path and hold it to ADR-0009's four claims."""

    # The base colour and the variation as the vertex factory delivers them:
    #   UV3 = base .rg      UV4 = (base .b, encoded biome weight)
    #   UV5 = (light, hue)
    # The weight lane reserves 0 for ABSENT and sends a real weight in the upper
    # half, so a biomeTint of 0 travels as 0.5.
    BASE = [0.31, 0.19, 0.11]
    LIGHT, HUE = 0.12, -0.05
    PRESENT_WEIGHT = 0.5           # biomeTint 0, present
    ABSENT_WEIGHT = 0.0            # the component path: no UV written at all

    def leaves(*, weight=PRESENT_WEIGHT, base=BASE, light=LIGHT, hue=HUE,
               normal_z=1.0, height_m=0.0):
        return {
            # R = 1: near-surface, so the modifiers are allowed to apply at all.
            # B and A are climate, and both are well clear of 0.06 so the water
            # marker's is_marker term evaluates to 0 and the override is inert.
            "vertex_color": [1.0, 1.0, 0.5, 0.5],
            "normal": [0.0, 0.0, normal_z],
            "world_pos": [0.0, 0.0, height_m * 100.0],   # metres -> UU
            "camera_pos": [0.0, 0.0, 0.0],
            "texcoords": {0: [0.0, 0.0],
                          3: [base[0], base[1]],
                          4: [base[2], weight],
                          5: [light, hue]},
        }

    # A detail sample of exactly 0.5 makes (sample - 0.5) * strength zero on
    # every channel, so `variation` is exactly 1.0 and the detail multiply is the
    # identity -- WITHOUT passing strengths of 0, which would test a
    # configuration the game does not ship.
    def textures(biome_rgb=(0.2, 0.4, 0.15)):
        return {"BiomeLUT": lambda uv: list(biome_rgb) + [1.0],
                "DetailTex": [0.5, 0.5, 0.5, 1.0]}

    def build(rock_strength=0.35):
        MaterialEditingLibrary.created.clear()
        MaterialEditingLibrary.connections.clear()
        b = tmc.GraphBuilder(Material())
        vc = b.node(unreal_mod.MaterialExpressionVertexColor)
        uv = b.node(unreal_mod.MaterialExpressionTextureCoordinate)
        base, _snow, base_out, _wet = tmc.build_terrain_base_color(
            b, vc, uv, "", rock_slope_strength=rock_strength,
            detail_fine_strength=0.05, detail_coarse_strength=0.04,
            bathy=None, palette=tmc.build_palette_inputs(b))
        return base, base_out, list(MaterialEditingLibrary.connections)

    def run(root, out, conns, lv, tx):
        return _pin(GraphEvaluator(conns, lv, tx).eval(root), out)

    root, out, conns = build()

    # --- 1. the palette path composes exactly what materialcolor.h says ------
    got = run(root, out, conns, leaves(), textures())
    want = apply_variation(BASE, LIGHT, HUE)
    if not close(got, want, 1e-6):
        failures.append(
            "the palette path does not compose the documented colour.\n"
            f"    base {BASE} light {LIGHT} hue {HUE}\n"
            f"    graph  {[round(x, 6) for x in got]}\n"
            f"    ADR-0009 {[round(x, 6) for x in want]}")
    else:
        print(f"composition: the palette path evaluates to "
              f"applyVariation(base, light, hue) to 1e-6")

    # --- 2. the variation SURVIVES the modifiers -----------------------------
    #
    # ADR-0009's load-bearing ordering claim, as a number rather than as a
    # screenshot of a hillside. With slope-rock fully applied the colour becomes
    # MAT_ROCK's -- but it must still be MODULATED by this voxel's own light and
    # hue, because the variation is stage 3 and the modifiers are stage 2. Move
    # the variation before the lerps and the output stops depending on it
    # entirely wherever a modifier bites, which is exactly the flat hillside
    # capture requirement 2 is looking for.
    vertical = dict(normal_z=0.0)         # a wall: slope = 1
    rr, ro, rc = build(rock_strength=1.0)
    a = run(rr, ro, rc, leaves(light=0.20, hue=0.08, **vertical), textures())
    bb = run(rr, ro, rc, leaves(light=-0.20, hue=-0.08, **vertical), textures())
    if close(a, bb, 1e-9):
        failures.append(
            "with slope-rock fully applied the output no longer depends on the "
            "per-voxel variation.\n"
            "    That is the variation being applied BEFORE the modifiers rather "
            "than after, so the lerps flatten it.\n"
            "    ADR-0009: 'place, then vary'. A cliff would render as one flat "
            "grey.")
    else:
        spread = max(abs(x - y) for x, y in zip(a, bb))
        print(f"ordering:    variation survives a fully-applied slope-rock "
              f"(spread {spread:.4f}); the modifiers do not flatten it")

    # --- 3. an unwritten palette falls through, and does NOT render black ----
    #
    # M_VoxelTerrain is also the material on the component path, where
    # FLocalVertexFactory supplies no fourth or fifth UV and they arrive as zero.
    # A raw weight of 0 legitimately means "this material owns its colour", so
    # the lane reserves 0 for ABSENT -- and if that encoding breaks, the world
    # renders from a base of (0,0,0). This is capture requirement 5, and it is
    # the one whose failure mode is a black world.
    absent = run(root, out, conns,
                 leaves(weight=ABSENT_WEIGHT, base=[0.0, 0.0, 0.0],
                        light=0.0, hue=0.0),
                 textures())
    if max(absent) <= 1e-6:
        failures.append(
            "with no palette written (UV3/4/5 all zero, the component path) the "
            "graph renders BLACK.\n"
            "    The ABSENT encoding is not being recovered, so `present` is not "
            "reaching the final lerp.\n"
            "    Every chunk drawn with voxel.Stream.GPU 0 would be black.")
    else:
        print(f"fallback:    an unwritten palette falls through to the "
              f"climate-only colour {[round(x, 4) for x in absent]}, not black")

    # --- 4. with biomeTint 0 the palette path ignores the climate ------------
    #
    # Every row is 0 today (ADR-0009 section 3a), so the near field must be
    # independent of the biome LUT. If a climate blend crept back in, the same
    # voxel would take two different colours in two different climates -- which
    # is precisely what that amendment removed.
    warm = run(root, out, conns, leaves(), textures(biome_rgb=(0.05, 0.6, 0.02)))
    cold = run(root, out, conns, leaves(), textures(biome_rgb=(0.7, 0.7, 0.9)))
    if not close(warm, cold, 1e-9):
        failures.append(
            "the palette path still depends on the biome LUT.\n"
            f"    two climates give {[round(x, 5) for x in warm]} and "
            f"{[round(x, 5) for x in cold]}\n"
            "    ADR-0009 section 3a: near-field colour is the material, and every "
            "biomeTint is 0.")
    else:
        print("climate:     with biomeTint 0 the palette path is independent of "
              "the biome LUT")

def main():
    # RECOMPILE FROM SOURCE, ALWAYS. CPython invalidates a cached .pyc on
    # (mtime, size), and mtime has one-second granularity -- so two edits to
    # terrain_material_common.py inside the same second that leave its LENGTH
    # unchanged reuse the first one's bytecode. Every edit that reorders or
    # swaps code has exactly that shape, which is to say every edit worth
    # checking: reordering the composition stages does not change the file
    # length, and this tool would then check the previous version and report on
    # a graph that no longer exists. Measured -- it silently did, twice, while
    # the breakage proofs for this file were being written.
    #
    # Pointing the cache at a fresh directory is non-destructive (nothing in the
    # repo is deleted) and makes the lookup always miss.
    sys.pycache_prefix = tempfile.mkdtemp(prefix="vxc-graphcheck-")

    install_stub()
    sys.path.insert(0, str(TOOLS))
    import terrain_material_common as tmc

    material = Material()
    failures = []

    for label, with_palette in (("voxel (palette)", True), ("clipmap (no palette)", False)):
        MaterialEditingLibrary.created.clear()
        MaterialEditingLibrary.connections.clear()
        b = tmc.GraphBuilder(material)
        vertex_color = b.node(sys.modules["unreal"].MaterialExpressionVertexColor)
        detail_uv = b.node(sys.modules["unreal"].MaterialExpressionTextureCoordinate)
        try:
            palette = tmc.build_palette_inputs(b) if with_palette else None
            base, snow, base_out, wet = tmc.build_terrain_base_color(
                b, vertex_color, detail_uv, "",
                rock_slope_strength=0.35,
                detail_fine_strength=0.05,
                detail_coarse_strength=0.04,
                bathy=None,
                palette=palette,
            )
        except Exception as exc:  # noqa: BLE001 -- the point is to report any of them
            failures.append(f"{label}: {type(exc).__name__}: {exc}")
            continue

        if base is None:
            failures.append(f"{label}: returned no base colour expression")
        if not isinstance(base_out, str):
            failures.append(f"{label}: base_color_out is {base_out!r}, not a pin name")
        if wet is not None:
            failures.append(f"{label}: bathy=None must leave the wet term absent")
        print(f"{label}: {len(MaterialEditingLibrary.created)} nodes, "
              f"{len(MaterialEditingLibrary.connections)} connections")

    # --- REACHABILITY: is each surviving modifier still in the LIVE graph? ---
    #
    # THE TRAP THIS CATCHES, which is the single most dangerous edit in
    # ADR-0009. The slope-rock and snowline terms used to be lerped into
    # `surface`, and `surface` was only reachable through the climate share. The
    # moment every biomeTint went to 0 that share became 0 too, so all three
    # modifiers were orphaned inside a branch nothing reads -- every cliff and
    # all the snow in the world silently deleted, no error, no failing test, and
    # nothing visible until someone regenerated the material on the editor box
    # and looked at a mountain.
    #
    # Node COUNTS cannot see it: an orphaned lerp is still a created node still
    # wired to its neighbours. What distinguishes live from dead is whether the
    # node can be REACHED from the expression the function returns, so that is
    # what is walked.
    def reachable_from(root):
        """Every node that feeds `root`, transitively."""
        incoming = {}
        for src, _so, dst, _di in MaterialEditingLibrary.connections:
            incoming.setdefault(id(dst), []).append(src)
        seen, stack = set(), [root]
        while stack:
            node = stack.pop()
            if id(node) in seen:
                continue
            seen.add(id(node))
            stack.extend(incoming.get(id(node), []))
        return seen

    def check_live(label, root, wanted):
        """Each named term must be a node that is REACHABLE from the result.

        Matched on the node's `desc`, not on its colour. Colour matching does
        not work here and the difference is instructive: the component-path
        fallback composes the SAME snow colour, so a snowline term deleted from
        the material path stayed 'reachable' through the fallback and the check
        passed a real deletion. A desc names the specific node whose presence is
        being asserted, and it labels that node for a human opening the graph
        too.
        """
        live = reachable_from(root)
        descs = {n.props.get("desc") for n in MaterialEditingLibrary.created
                 if id(n) in live}
        for name, desc in wanted:
            if desc not in descs:
                failures.append(
                    f"{label}: the {name} term is NOT reachable from the returned "
                    f"base colour (no live node marked '{desc}'). Either it was "
                    f"deleted, or it is orphaned in a dead branch -- and an orphan "
                    f"deletes it from the world with nothing erroring.")

    MaterialEditingLibrary.created.clear()
    MaterialEditingLibrary.connections.clear()
    b = tmc.GraphBuilder(material)
    vc = b.node(sys.modules["unreal"].MaterialExpressionVertexColor)
    uv = b.node(sys.modules["unreal"].MaterialExpressionTextureCoordinate)
    try:
        base, _snow, _out, _wet = tmc.build_terrain_base_color(
            b, vc, uv, "", rock_slope_strength=0.35, detail_fine_strength=0.05,
            detail_coarse_strength=0.04, bathy=None,
            palette=tmc.build_palette_inputs(b))
        check_live("voxel (palette)", base,
                   [("slope-rock", "ADR-0009 slope-rock (material path)"),
                    ("snowline", "ADR-0009 snowline (material path)")])
        print("reachability: slope-rock and snowline are live on the palette path")
    except Exception as exc:  # noqa: BLE001
        failures.append(f"reachability check: {type(exc).__name__}: {exc}")

    # The palette path must add nodes the no-palette path does not; equal counts
    # would mean the palette argument is being ignored, which is a change that
    # runs cleanly and does nothing -- this repo's signature failure.
    def node_count(with_palette):
        """Nodes created INSIDE build_terrain_base_color, and only those.

        The counter is cleared after build_palette_inputs has run, because those
        seven decode nodes exist whether or not the composition goes on to use
        them -- counting them made this comparison pass a deliberate `if True:`
        that took the climate-only branch with a fully-built palette in hand.
        """
        b = tmc.GraphBuilder(material)
        vc = b.node(sys.modules["unreal"].MaterialExpressionVertexColor)
        uv = b.node(sys.modules["unreal"].MaterialExpressionTextureCoordinate)
        palette = tmc.build_palette_inputs(b) if with_palette else None
        MaterialEditingLibrary.created.clear()
        tmc.build_terrain_base_color(
            b, vc, uv, "", rock_slope_strength=0.35, detail_fine_strength=0.05,
            detail_coarse_strength=0.04, bathy=None, palette=palette)
        return len(MaterialEditingLibrary.created)

    try:
        plain, withpal = node_count(False), node_count(True)
        if withpal <= plain:
            failures.append(
                f"the palette argument adds no nodes ({plain} without, {withpal} with) "
                "-- it is being accepted and ignored")
        else:
            print(f"palette adds {withpal - plain} nodes over the climate-only graph")
    except Exception as exc:  # noqa: BLE001
        failures.append(f"node-count comparison: {type(exc).__name__}: {exc}")

    try:
        check_composition(tmc, sys.modules["unreal"], failures)
    except Exception as exc:  # noqa: BLE001
        import traceback
        failures.append(f"composition check: {type(exc).__name__}: {exc}\n"
                        + "".join(traceback.format_tb(exc.__traceback__)[-2:]))

    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("terrain graph: both paths build, and the palette path composes what "
          "ADR-0009 says")
    print("  CHECKED HERE: the composition and its stage order, the slope-rock "
          "and snowline\n"
          "  modifiers being live rather than orphaned, the ABSENT fallback, and "
          "that a\n"
          "  biomeTint of 0 keeps the climate out of the near field.")
    print("  NOT CHECKED HERE: whether the .ush's own arithmetic matches "
          "vxc::voxelTint (that is\n"
          "  check-palette-parity.py), and anything a picture decides -- cave "
          "strata, a dry cliff\n"
          "  reading as rock, snow on a mountain, no mottle seam at a ring "
          "boundary. Those four\n"
          "  are what the editor is still for.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
