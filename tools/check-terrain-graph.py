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


def main():
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

    if failures:
        for f in failures:
            print("FAIL: " + f, file=sys.stderr)
        return 1
    print("terrain graph: both paths build against the stub")
    print("  NOTE: this says the code RUNS, not that the material is correct. "
          "The editor is still the only thing that can say that.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
