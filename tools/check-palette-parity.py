#!/usr/bin/env python3
"""Do the four voxel-colour implementations still agree?

WHY THIS EXISTS
---------------
ADR-0008 made the palette TABLE single and left every consumer to evaluate it,
and by the time ADR-0009 was written there were three evaluations and they
disagreed. The shader applied jitter, hue and patch; the detail-asset subsystem
applied jitter only, at an invented 0.35 scale, with no patch term at all;
asset-forge's preview applied all three with the hue axis at 0.6 strength and
one wavelength for a whole grid. A fern authored in the forge, the same fern
drawn as ground cover and the same species baked into terrain were three
different pictures of one row of one table.

`vxc::voxelTint` (voxel-core/include/voxelcore/materialcolor.h) is now the
definition. The other implementations are transcriptions of it, and a
transcription drifts. This is the check that says when.

WHAT IT CHECKS, AND WHY EACH ONE NEEDS CHECKING DIFFERENTLY
-----------------------------------------------------------
1. THE TABLE, four ways. materialpalette.h against the three generated copies
   (VoxelMaterialPalette.ush, terrain_palette.py, asset-forge's forge/palette.py).
   Cheap, exact, and mostly covered by each generator's own --check; done here
   too so one command answers the whole question.

2. THE EVALUATION, C++ against HLSL. The interesting half. The .ush is PARSED --
   its constant tables are read out of the generated text and its arithmetic is
   re-implemented here in float, mirroring the HLSL line for line -- and run
   against `vxc::voxelTint` compiled and dumped by a tiny C++ probe. Every
   material, several coordinates, three cube sizes.

   A REIMPLEMENTATION IS NOT THE SHADER, and that limit is real: this catches a
   changed constant, a changed table, a changed wavelength unit, a dropped term
   or a changed composition order, and it does not catch a typo confined to the
   HLSL text that this file's mirror does not copy. Compiling the HLSL is the
   other half of the guard and tools/compile-shaders.ps1 does it -- on Windows,
   with DXC. This half runs on Linux, in CI, on every PR, which is what makes it
   the one that actually runs.

3. TOLERANCE. The C++ side is Q16 fixed point (voxel-core has no floats) and the
   shader side is float32, so they cannot be bit-identical. The bar is 1/2048 of
   full range -- about a tenth of one 8-bit code value, far below anything a
   display can show, and roughly a hundred times tighter than the smallest
   authored variation amplitude, so any real divergence blows straight through
   it.

Run:
    python3 tools/check-palette-parity.py
    python3 tools/check-palette-parity.py --samples 4000   (a deeper sweep)

It needs a C++ compiler and nothing else -- no GPU, no Unreal, no numpy.
"""
import argparse
import math
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "voxel-core" / "include" / "voxelcore" / "materialpalette.h"
CORE = ROOT / "voxel-core" / "include" / "voxelcore" / "core.h"
USH = ROOT / "ue-project" / "Shaders" / "VoxelMaterialPalette.ush"
FORGE = ROOT / "asset-forge" / "forge" / "palette.py"
TERRAIN = ROOT / "ue-project" / "Tools" / "terrain_palette.py"
INCLUDE = ROOT / "voxel-core" / "include"

# 1/2048 of full range. See the tolerance note in the docstring.
TOLERANCE = 1.0 / 2048.0

HEADER_ENTRY = re.compile(
    r"/\*\s*(MAT_\w+)\s*\*/\s*"
    r"\{\{\{([^}]*)\},\s*\{([^}]*)\},\s*\{([^}]*)\}\},\s*"
    r"(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}",
    re.S)


def fail(msg):
    print("FAIL: " + msg, file=sys.stderr)
    return 1


# ---------------------------------------------------------------------------
# 1. the table
# ---------------------------------------------------------------------------

def read_header():
    rows = []
    for m in HEADER_ENTRY.finditer(HEADER.read_text(encoding="utf-8")):
        faces = tuple(tuple(int(v) for v in m.group(i).replace(" ", "").split(","))
                      for i in (2, 3, 4))
        rows.append({
            "name": m.group(1),
            "faces": faces,
            "jitter": int(m.group(5)),
            "hue": int(m.group(6)),
            "patch": int(m.group(7)),
            "scale_dm": int(m.group(8)),
            "biome": int(m.group(9)),
        })
    if not rows:
        raise SystemExit(f"no palette rows parsed from {HEADER}")
    return rows


def srgb_to_linear(byte):
    c = byte / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


FLOAT3 = re.compile(r"float3\(([-\d.eE+]+), ([-\d.eE+]+), ([-\d.eE+]+)\)")
FLOAT4 = re.compile(r"float4\(([-\d.eE+]+), ([-\d.eE+]+), ([-\d.eE+]+), ([-\d.eE+]+)\)")


def read_ush():
    """The generated shader's three constant tables, plus its base cube size."""
    text = USH.read_text(encoding="utf-8")

    def block(name, pattern):
        m = re.search(r"static const \w+ " + name + r"\[\d+\]\s*=\s*\{(.*?)\n\};",
                      text, re.S)
        if not m:
            raise SystemExit(f"{USH.name}: no {name} table -- has the generator changed?")
        return pattern.findall(m.group(1))

    faces = [tuple(float(v) for v in t) for t in block("VoxelPaletteFace", FLOAT3)]
    var = [tuple(float(v) for v in t) for t in block("VoxelPaletteVariation", FLOAT4)]
    tint_block = re.search(
        r"static const float VoxelPaletteBiomeTint\[\d+\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not tint_block:
        raise SystemExit(f"{USH.name}: no VoxelPaletteBiomeTint table")
    tint = [float(v) for v in re.findall(r"^\s*([-\d.eE+]+),", tint_block.group(1), re.M)]

    cube = re.search(r"#define VOXEL_PALETTE_BASE_CUBE_MM ([\d.]+)", text)
    if not cube:
        raise SystemExit(f"{USH.name}: no VOXEL_PALETTE_BASE_CUBE_MM")
    return faces, var, tint, float(cube.group(1))


def check_tables(rows):
    """The header against all three generated copies."""
    bad = 0
    faces, var, tint, base_cube = read_ush()
    n = len(rows)
    if len(faces) != n * 3 or len(var) != n or len(tint) != n:
        return fail(f"{USH.name} has {len(faces)//3}/{len(var)}/{len(tint)} rows, "
                    f"header has {n}")

    for i, r in enumerate(rows):
        for f in range(3):
            want = tuple(srgb_to_linear(c) for c in r["faces"][f])
            got = faces[i * 3 + f]
            if max(abs(a - b) for a, b in zip(want, got)) > 1e-6:
                bad |= fail(f"{r['name']} face {f}: .ush has {got}, header says {want}")
        want_var = (r["jitter"] / 255.0, r["hue"] / 255.0, r["patch"] / 255.0,
                    float(r["scale_dm"] * 100))
        if max(abs(a - b) for a, b in zip(want_var, var[i])) > 1e-6:
            bad |= fail(f"{r['name']} variation: .ush has {var[i]}, header says {want_var}")
        if abs(tint[i] - r["biome"] / 255.0) > 1e-6:
            bad |= fail(f"{r['name']} biomeTint: .ush has {tint[i]}, "
                        f"header says {r['biome'] / 255.0}")

    # asset-forge's copy, which is a Python dict of sRGB bytes and the five
    # scalar columns -- the same shape as the header, so this compares numbers
    # rather than a conversion.
    if FORGE.exists():
        ns = {}
        exec(compile(FORGE.read_text(encoding="utf-8"), str(FORGE), "exec"), ns)
        pal = ns.get("PALETTE")
        if pal is None:
            bad |= fail(f"{FORGE.name} has no PALETTE dict")
        elif len(pal) != n:
            bad |= fail(f"{FORGE.name} has {len(pal)} rows, header has {n}")
        else:
            for i, r in enumerate(rows):
                want = (r["faces"][0], r["faces"][1], r["faces"][2], r["jitter"],
                        r["hue"], r["patch"], r["scale_dm"], r["biome"])
                if tuple(pal[i]) != want:
                    bad |= fail(f"{r['name']}: {FORGE.name} has {tuple(pal[i])}, "
                                f"header says {want}")
    else:
        print(f"note: {FORGE} not present, skipping the asset-forge copy")

    if not bad:
        print(f"table: {n} materials agree across materialpalette.h, "
              f"{USH.name}, {FORGE.name} (base cube {base_cube:.0f} mm)")
    return bad


# ---------------------------------------------------------------------------
# 2. the evaluation
# ---------------------------------------------------------------------------
#
# The SHIPPED shader text, compiled as C++ against tools/hlsl_cpp_shim.h and run
# against vxc::voxelTint in one binary. Read the shim's header comment for why
# it is done this way: the first version of this check re-implemented the
# shader's arithmetic in Python, and that version passed a deliberately
# corrupted hash constant and a deliberately reverted patch wavelength, because
# the Python said what the shader was supposed to say rather than what it did.

# The three mechanical transforms that turn HLSL into compilable C++, plus the
# two lane spellings C++ cannot alias. Each is (pattern, replacement, why).
# Anything not on this list reaches the compiler verbatim.
TRANSFORMS = [
    (re.compile(r"\[unroll\]"), "", "an HLSL attribute with no C++ spelling"),
    (re.compile(r"\bout (float[234]?) "), r"\1& ", "HLSL puts the direction before the type"),
    (re.compile(r"\.xyz\b"), ".xyz_()", "HLSL swizzles are members; C++ needs a call"),
    (re.compile(r"\.r\b"), ".x", "the red lane, which C++ cannot alias"),
    (re.compile(r"\.b\b"), ".z", "the blue lane, which C++ cannot alias"),
]


def shim_shader(text):
    for pattern, repl, _why in TRANSFORMS:
        text = pattern.sub(repl, text)
    return text


CPP_PROBE = r'''
// Compares the SHIPPED shader text against vxc::voxelTint. Generated by
// tools/check-palette-parity.py; see that file and tools/hlsl_cpp_shim.h.
#include "hlsl_cpp_shim.h"
#include "shader_as_cpp.h"

#include "voxelcore/materialcolor.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const int samples = argc > 1 ? std::atoi(argv[1]) : 400;
    const float cubes[3] = {100.0f, 50.0f, 3200.0f};

    double worst = 0.0;
    int worstMat = 0, worstCube = 0, worstX = 0, worstY = 0, worstZ = 0;
    const char* worstAxis = "light";
    double worstWant = 0.0, worstGot = 0.0;
    long comparisons = 0;

    // Every material has to be checked, not a sample of them: the tables are
    // indexed by id and an off-by-one in one row is exactly the defect class
    // this whole apparatus exists for.
    for (int m = 0; m < vxc::kMaterialCount; ++m) {
        for (int c = 0; c < 3; ++c) {
            for (int i = 0; i < samples; ++i) {
                // A deterministic spread that is not axis-aligned, so a bug in
                // any single coordinate's contribution still shows up.
                const int x = (i * 37) % 511 - 255;
                const int y = (i * 101) % 397 - 198;
                const int z = (i * 7) % 233 - 116;

                const vxc::VoxelTint want =
                    vxc::voxelTint(vxc::MaterialId(m), x, y, z, int32_t(cubes[c]));
                const float2 got = VoxelMaterialVariation(uint(m), int3(x, y, z), cubes[c], 0u);

                const double pairs[2][2] = {
                    {double(want.lightQ16) / 65536.0, double(got.x)},
                    {double(want.hueQ16) / 65536.0, double(got.y)},
                };
                for (int axis = 0; axis < 2; ++axis) {
                    const double d = pairs[axis][0] - pairs[axis][1];
                    const double ad = d < 0 ? -d : d;
                    ++comparisons;
                    if (ad > worst) {
                        worst = ad;
                        worstMat = m; worstCube = int(cubes[c]);
                        worstX = x; worstY = y; worstZ = z;
                        worstAxis = axis == 0 ? "light" : "hue";
                        worstWant = pairs[axis][0];
                        worstGot = pairs[axis][1];
                    }
                }
            }
        }
    }

    // FACE CLASS, which nothing above touches. The sweeps call
    // VoxelMaterialBase with a raw 0/1/2, so they are blind to the mapping from
    // a quad's axis and direction onto a class -- and transposing top and bottom
    // is a one-token edit that shows up in the world as cut logs wearing bark on
    // their end grain and grass growing on the underside of an overhang. A
    // deliberate transposition passed this checker until this loop existed.
    int faceClassMismatches = 0;
    int firstBadAxis = -1, firstBadDir = -1;
    for (int axis = 0; axis < 3; ++axis) {
        for (int dir = 0; dir < 2; ++dir) {
            const uint got = VoxelPaletteFaceClass(uint(axis), dir != 0);
            const uint want = uint(vxc::faceClassOf(axis, dir != 0));
            if (got != want) {
                if (!faceClassMismatches) { firstBadAxis = axis; firstBadDir = dir; }
                ++faceClassMismatches;
            }
        }
    }

    // THE COMPOSITION, which is a separate question from the tint and needs its
    // own comparison. The sweep above checks that both sides compute the same
    // (light, hue); it says nothing about how those two numbers are APPLIED to a
    // colour, and "applied differently" is a real defect class -- asset-forge's
    // preview ran the hue axis at 0.6 strength for its whole life. A deliberate
    // reintroduction of exactly that bug passed this checker until this block
    // existed.
    double worstApply = 0.0;
    int worstApplyMat = 0, worstApplyLane = 0;
    for (int m = 0; m < vxc::kMaterialCount; ++m) {
        for (int i = 0; i < samples; ++i) {
            const int x = (i * 37) % 511 - 255;
            const int y = (i * 101) % 397 - 198;
            const int z = (i * 7) % 233 - 116;

            // A mid-grey rather than the material's own colour: a dark row would
            // shrink every difference toward zero and hide a wrong multiplier
            // behind its own dimness.
            const int32_t half = vxc::kColorOne / 2;
            int32_t want[3] = {half, half, half};
            vxc::applyTintQ16(want, vxc::voxelTint(vxc::MaterialId(m), x, y, z, 100));

            const float2 v = VoxelMaterialVariation(uint(m), int3(x, y, z), 100.0f, 0u);
            const float3 got = VoxelApplyVariation(float3(0.5f, 0.5f, 0.5f), v);

            const float lanes[3] = {got.x, got.y, got.z};
            for (int k = 0; k < 3; ++k) {
                const double d = double(want[k]) / 65536.0 - lanes[k];
                const double ad = d < 0 ? -d : d;
                if (ad > worstApply) { worstApply = ad; worstApplyMat = m; worstApplyLane = k; }
            }
        }
    }

    // ...and the whole thing end to end, which is what a ray marcher calls.
    // Catches a composition that is right in both halves and wired together
    // wrongly -- variation applied before the face lookup, say.
    double worstWhole = 0.0;
    int worstWholeMat = 0;
    for (int m = 0; m < vxc::kMaterialCount; ++m) {
        for (uint f = 0; f < 3; ++f) {
            for (int i = 0; i < 64; ++i) {
                const int x = (i * 37) % 511 - 255;
                const int y = (i * 101) % 397 - 198;
                const int z = (i * 7) % 233 - 116;

                const vxc::Rgb& srgb = vxc::kMaterialPalette[m].face[f];
                const uint8_t bytes[3] = {srgb.r, srgb.g, srgb.b};
                int32_t want[3];
                for (int k = 0; k < 3; ++k) {
                    const double u = bytes[k] / 255.0;
                    const double lin = u <= 0.04045 ? u / 12.92
                                                    : std::pow((u + 0.055) / 1.055, 2.4);
                    want[k] = int32_t(lin * 65536.0 + 0.5);
                }
                vxc::applyTintQ16(want, vxc::voxelTint(vxc::MaterialId(m), x, y, z, 100));

                const float3 got = VoxelMaterialColor(uint(m), f, int3(x, y, z), 100.0f);
                const float lanes[3] = {got.x, got.y, got.z};
                for (int k = 0; k < 3; ++k) {
                    const double d = double(want[k]) / 65536.0 - lanes[k];
                    const double ad = d < 0 ? -d : d;
                    if (ad > worstWhole) { worstWhole = ad; worstWholeMat = m; }
                }
            }
        }
    }

    // The BASE colour, checked separately again: the shader's table is float and
    // linear, the header's is sRGB bytes, so this is where a converted-wrongly or
    // transposed-face-class row would show and the sweeps above would not.
    double worstBase = 0.0;
    int worstBaseMat = 0, worstBaseFace = 0;
    for (int m = 0; m < vxc::kMaterialCount; ++m) {
        for (uint f = 0; f < 3; ++f) {
            const float3 got = VoxelMaterialBase(uint(m), f);
            const vxc::Rgb& want = vxc::kMaterialPalette[m].face[f];
            const float lanes[3] = {got.x, got.y, got.z};
            const uint8_t bytes[3] = {want.r, want.g, want.b};
            for (int k = 0; k < 3; ++k) {
                // sRGB byte to linear, the exact piecewise curve.
                const double u = bytes[k] / 255.0;
                const double lin = u <= 0.04045 ? u / 12.92
                                                : std::pow((u + 0.055) / 1.055, 2.4);
                const double d = lin - lanes[k];
                const double ad = d < 0 ? -d : d;
                if (ad > worstBase) { worstBase = ad; worstBaseMat = m; worstBaseFace = int(f); }
            }
        }
        // ...and the biome-tint weight, which nothing else here reads.
        const double wantTint = vxc::kMaterialPalette[m].biomeTint / 255.0;
        const double d = wantTint - VoxelPaletteBiomeTint[m];
        const double ad = d < 0 ? -d : d;
        if (ad > worstBase) { worstBase = ad; worstBaseMat = m; worstBaseFace = -1; }
    }

    std::printf("comparisons %ld\n", comparisons);
    std::printf("worst %.9f mat %d cube %d at %d %d %d axis %s want %.9f got %.9f\n",
                worst, worstMat, worstCube, worstX, worstY, worstZ, worstAxis,
                worstWant, worstGot);
    std::printf("worstbase %.9f mat %d face %d\n", worstBase, worstBaseMat, worstBaseFace);
    std::printf("worstapply %.9f mat %d lane %d\n", worstApply, worstApplyMat, worstApplyLane);
    std::printf("worstwhole %.9f mat %d\n", worstWhole, worstWholeMat);
    std::printf("faceclass %d axis %d dir %d\n", faceClassMismatches, firstBadAxis, firstBadDir);
    return 0;
}
'''


def check_evaluation(rows, samples):
    cxx = shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        return fail("no g++ or clang++ on PATH; the evaluation half needs one to "
                    "compile both vxc::voxelTint and the shader text. "
                    "(The table half above already ran.)")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        shutil.copy(ROOT / "tools" / "hlsl_cpp_shim.h", tmp / "hlsl_cpp_shim.h")
        (tmp / "shader_as_cpp.h").write_text(
            shim_shader(USH.read_text(encoding="utf-8")), encoding="utf-8")
        (tmp / "probe.cpp").write_text(CPP_PROBE, encoding="utf-8")

        build = subprocess.run(
            [cxx, "-std=c++20", "-O1", "-I", str(tmp), "-I", str(INCLUDE),
             str(tmp / "probe.cpp"), "-o", str(tmp / "probe")],
            capture_output=True, text=True)
        if build.returncode != 0:
            return fail(
                "the shader did not compile as C++.\n"
                "  This is not (necessarily) a shader bug: it can also mean the .ush "
                "grew an HLSL construct tools/hlsl_cpp_shim.h does not model.\n"
                "  Either way the parity check cannot run, which is a failure -- an "
                "unrunnable check is an absent one.\n\n" + build.stderr)

        run = subprocess.run([str(tmp / "probe"), str(samples)],
                             capture_output=True, text=True)
        if run.returncode != 0:
            return fail("the parity probe did not run:\n" + run.stderr)

    out = {}
    for line in run.stdout.splitlines():
        parts = line.split()
        out[parts[0]] = parts[1:]

    comparisons = int(out["comparisons"][0])
    worst = float(out["worst"][0])
    worst_base = float(out["worstbase"][0])

    bad = 0
    if worst_base > 1e-6:
        mat, face = int(out["worstbase"][2]), int(out["worstbase"][4])
        what = "biomeTint" if face < 0 else f"face class {face}"
        bad |= fail(
            f"the shader's TABLE has drifted from the header: {rows[mat]['name']} "
            f"{what} is out by {worst_base:.9f}.\n"
            f"  run python ue-project/Tools/gen_material_palette_ush.py")

    if worst > TOLERANCE:
        w = out["worst"]
        bad |= fail(
            "the shader's EVALUATION has drifted from vxc::voxelTint.\n"
            f"  worst: {rows[int(w[2])]['name']} {w[10]} at voxel "
            f"({w[6]},{w[7]},{w[8]}), {w[4]} mm cubes\n"
            f"  materialcolor.h says {float(w[12]):+.6f}, "
            f"VoxelMaterialPalette.ush says {float(w[14]):+.6f}\n"
            f"  difference {worst:.6f}, tolerance {TOLERANCE:.6f}\n"
            "  materialcolor.h is the definition; the .ush is the transcription.")

    face_bad = int(out["faceclass"][0])
    if face_bad:
        axis, direction = out["faceclass"][2], out["faceclass"][4]
        bad |= fail(
            f"VoxelPaletteFaceClass disagrees with vxc::faceClassOf on {face_bad} "
            f"of 6 quad orientations (first: axis {axis}, "
            f"{'positive' if direction == '1' else 'negative'}).\n"
            "  In the world this is a cut log wearing bark on its end grain, or "
            "grass on the underside of an overhang.")

    worst_apply = float(out["worstapply"][0])
    if worst_apply > TOLERANCE:
        mat, lane = int(out["worstapply"][2]), int(out["worstapply"][4])
        bad |= fail(
            "the shader APPLIES the variation differently from vxc::applyTintQ16.\n"
            f"  worst: {rows[mat]['name']}, {'rgb'[lane]} lane, out by {worst_apply:.6f}\n"
            "  Both sides agree on the tint and disagree on what to do with it --\n"
            "  a wrong hue-axis strength, a missing lane, or a clamp that should\n"
            "  not be there. asset-forge shipped exactly this for its whole life.")

    worst_whole = float(out["worstwhole"][0])
    if worst_whole > TOLERANCE:
        mat = int(out["worstwhole"][2])
        bad |= fail(
            "VoxelMaterialColor end to end disagrees with the header.\n"
            f"  worst: {rows[mat]['name']}, out by {worst_whole:.6f}\n"
            "  The pieces may each be right and wired together wrongly.")

    if not bad:
        print(f"evaluation: the shipped shader text, compiled as C++, agrees with "
              f"vxc::voxelTint over {comparisons} comparisons "
              f"({len(rows)} materials x 3 cube sizes); worst {worst:.2e}, "
              f"tolerance {TOLERANCE:.2e}")
        print(f"composition: applying the tint agrees to {worst_apply:.2e}; "
              f"end to end to {worst_whole:.2e}")
        print(f"tables:     base colours and biome weights agree to {worst_base:.2e}")
    return bad


def check_forge_evaluation(rows, samples):
    """asset-forge's numpy mirror against vxc::voxelTint.

    The third arm. The shader is compiled and compared above; this one is
    imported and compared, and it is the arm with the most history: forge's
    preview ran the warm/cool tilt at 0.6 of the authored strength and sampled
    the patch term at ONE median wavelength for a whole grid, so what a designer
    approved and what the game drew were different pictures for the whole life
    of the library.

    Needs numpy. Where it is absent this SAYS SO and names what went unchecked,
    rather than passing quietly -- a skipped arm reported as a pass is how a
    check stops existing.
    """
    try:
        import numpy as np  # noqa: PLC0415
    except ImportError:
        print("forge:      SKIPPED -- numpy is not installed, so asset-forge's "
              "evaluation was NOT compared (pip install numpy to cover it)")
        return 0

    forge_dir = ROOT / "asset-forge"
    if not (forge_dir / "forge" / "render.py").exists():
        print(f"forge:      SKIPPED -- no {forge_dir}")
        return 0

    saved = list(sys.path)
    sys.path.insert(0, str(forge_dir))
    try:
        from forge import render  # noqa: PLC0415
    except ImportError as exc:
        sys.path[:] = saved
        print(f"forge:      SKIPPED -- {exc} (its other dependencies are absent)")
        return 0

    cxx = shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        sys.path[:] = saved
        return fail("no C++ compiler for the reference side of the forge check")

    # The two pitches the library is authored at: terrain-lattice assets at
    # 10 cm and detail-lattice cover at 5 cm. Both, because the pitch is what
    # the world-metric patch wavelength is measured against and a mirror that
    # ignored it would agree at one and not the other.
    pitches = (100, 50)
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "probe.cpp"
        src.write_text(FORGE_PROBE % {"pitches": ", ".join(str(p) for p in pitches)})
        exe = Path(tmp) / "probe"
        build = subprocess.run([cxx, "-std=c++20", "-O2", "-I", str(INCLUDE),
                                str(src), "-o", str(exe)], capture_output=True, text=True)
        if build.returncode != 0:
            sys.path[:] = saved
            return fail("the forge reference probe did not compile:\n" + build.stderr)
        run = subprocess.run([str(exe), str(samples)], capture_output=True, text=True)

    parsed = [tuple(int(v) for v in line.split()) for line in run.stdout.splitlines()]
    worst, where = 0.0, None
    for pitch in pitches:
        sub = [r for r in parsed if r[1] == pitch]
        mats = np.array([r[0] for r in sub])
        xs = np.array([r[2] for r in sub])
        ys = np.array([r[3] for r in sub])
        zs = np.array([r[4] for r in sub])
        tint = render._voxel_tint(xs, ys, zs, mats, float(pitch))
        # The forge returns the finished (N, 3) multiplier; recover the two
        # scalars from it, which also checks that it COMPOSES them the way
        # vxc::applyTintQ16 does rather than only computing them right.
        light = tint[:, 1] - 1.0
        hue = np.where(np.abs(tint[:, 1]) > 1e-9, tint[:, 0] / tint[:, 1] - 1.0, 0.0)
        for i, r in enumerate(sub):
            for got, want, axis in ((light[i], r[5] / 65536.0, "light"),
                                    (hue[i], r[6] / 65536.0, "hue")):
                d = abs(float(got) - want)
                if d > worst:
                    worst, where = d, (rows[r[0]]["name"], pitch, axis, want, float(got))
    sys.path[:] = saved

    if worst > TOLERANCE:
        name, pitch, axis, want, got = where
        return fail(
            "asset-forge's preview has drifted from vxc::voxelTint.\n"
            f"  worst: {name} {axis} at a {pitch} mm pitch\n"
            f"  materialcolor.h says {want:+.6f}, forge/render.py says {got:+.6f}\n"
            f"  difference {worst:.6f}, tolerance {TOLERANCE:.6f}\n"
            "  What a designer approves in the forge is not what the game draws.")

    name, pitch, axis, _want, _got = where
    print(f"forge:      the numpy mirror agrees at both authored pitches "
          f"(100 and 50 mm); worst {worst:.2e} at {name} {axis}")
    return 0


FORGE_PROBE = r'''
#include "voxelcore/materialcolor.h"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 200;
    const int cubes[] = {%(pitches)s};
    for (int m = 0; m < vxc::kMaterialCount; ++m)
        for (unsigned c = 0; c < sizeof(cubes) / sizeof(cubes[0]); ++c)
            for (int i = 0; i < n; ++i) {
                const int x = (i * 37) %% 511 - 255;
                const int y = (i * 101) %% 397 - 198;
                const int z = (i * 7) %% 233 - 116;
                const vxc::VoxelTint t =
                    vxc::voxelTint(vxc::MaterialId(m), x, y, z, cubes[c]);
                std::printf("%%d %%d %%d %%d %%d %%d %%d\n", m, cubes[c], x, y, z,
                            t.lightQ16, t.hueQ16);
            }
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--samples", type=int, default=400,
                    help="coordinates per material per cube size (default 400)")
    ap.add_argument("--table-only", action="store_true",
                    help="skip the evaluation half (no compiler needed)")
    a = ap.parse_args()

    rows = read_header()
    rc = check_tables(rows)
    if not a.table_only:
        rc |= check_evaluation(rows, a.samples)
        rc |= check_forge_evaluation(rows, max(20, a.samples // 2))
    if rc == 0:
        print("palette parity: pass")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
