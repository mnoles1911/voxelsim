"""Generate `Shaders/VoxelMaterialPalette.ush` from the engine's palette header.

ADR-0008 makes `vxc::kMaterialPalette` the one definition of what a material
looks like, for every consumer. The renderer was not one of them: `kMaterialPalette`
appeared nowhere outside its own header, so what a designer approved in
asset-forge and what the game drew were two different answers. This closes that
for the GPU quad path.

It is generated rather than transcribed for a reason this repo has already paid
for. Material IDs were declared in two places, drifted, and because MaterialId
is a byte the result rendered as SOMETHING rather than failing -- nine of
thirteen materials were out of range before anyone noticed. Colour is the
version nobody catches, because a wrong colour still looks like a colour.

Run after editing the header:

    python ue-project/Tools/gen_material_palette_ush.py

Forgetting to is the failure this cannot prevent, so it is caught separately:

    python ue-project/Tools/gen_material_palette_ush.py --check

returns non-zero and names the first differing line when the committed .ush has
drifted from the header. That check needs no Unreal and no editor, which is the
point -- it has to be runnable by whoever is holding the box and by whoever is
not.

TWO THINGS THIS FILE DECIDES, both of which the header deliberately does not.

sRGB TO LINEAR. The header's values are sRGB, the numbers a designer reads off a
colour picker. Unreal shades in linear. Converting here rather than in the
shader keeps it exact and free, and matches what `gen_terrain_textures.py`
already does on its own way into a texture -- skipping that encode was a real
bug there that made every colour come back darker and more saturated than
authored.

BYTES TO FRACTIONS. `voxelJitter`, `voxelHue` and `patchStrength` are 1/255ths
of full range in the header because `MaterialAppearance` is all `uint8_t` (no
new float in voxel-core). The shader wants fractions, so they are divided once
here instead of on every pixel.

WHERE THE ASSET MATERIALS START. `VOXEL_MATERIAL_FIRST_ASSET` is read from the
`--- ASSET MATERIALS ---` banner in core.h rather than typed as 16, because the
renderer wiring (VoxelQuadVertexFactory.ush) uses it to decide which voxels the
palette is allowed to colour -- everything below it is terrain, whose colour is
climate-driven and stays the biome graph's business. A hardcoded 16 here would
be a third copy of a boundary that already exists twice (the enum and the
banner), which is this repo's documented failure mode.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "voxel-core" / "include" / "voxelcore" / "materialpalette.h"
CORE = HEADER.parent / "core.h"
OUT = ROOT / "ue-project" / "Shaders" / "VoxelMaterialPalette.ush"
TERRAIN = ROOT / "ue-project" / "Tools" / "terrain_palette.py"

ENTRY = re.compile(
    r"/\*\s*(MAT_\w+)\s*\*/\s*"
    r"\{\{\{([^}]*)\},\s*\{([^}]*)\},\s*\{([^}]*)\}\},\s*"
    r"(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}",
    re.S,
)
ENUM = re.compile(r"^\s*(MAT_\w+)\s*=\s*(\d+)\s*,", re.M)
# The banner core.h puts above the block of materials that only ever arrive from
# a baked asset. Everything above it is something the terrain amplifier can
# emit; everything below it is asset-forge's.
ASSET_BANNER = re.compile(r"^\s*//\s*-+\s*ASSET MATERIALS\s*-+\s*$", re.M)


def triple(text):
    return tuple(int(v) for v in text.replace(" ", "").split(","))


def to_linear(byte: int) -> float:
    """sRGB byte to linear float, the exact piecewise curve rather than 2.2."""
    c = byte / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def enum_order():
    """The material names in id order, read from the enum itself."""
    pairs = [(int(m.group(2)), m.group(1))
             for m in ENUM.finditer(CORE.read_text(encoding="utf-8"))]
    pairs.sort()
    return [name for _, name in pairs]


def first_asset_id():
    """The id of the first ASSET material, from core.h's own banner.

    Raises rather than guessing. A wrong answer here is the failure this whole
    file exists to prevent: too low and terrain voxels would start taking asset
    colours, too high and a bark voxel would silently keep the biome graph's
    answer -- and both of those still render as SOMETHING.
    """
    text = CORE.read_text(encoding="utf-8")
    banner = ASSET_BANNER.search(text)
    if not banner:
        raise SystemExit(
            f"no '--- ASSET MATERIALS ---' banner in {CORE}; the renderer needs "
            "to know where the asset materials start and will not guess")
    after = ENUM.search(text, banner.end())
    if not after:
        raise SystemExit(f"no MAT_ enumerator after the asset banner in {CORE}")
    return int(after.group(2)), after.group(1)


def source() -> str:
    """The text `VoxelMaterialPalette.ush` should contain.

    Everything the generator decides happens here and nothing is written, so
    `--check` can redo the same work in memory and compare it against disk.
    """
    for needed in (HEADER, CORE):
        if not needed.exists():
            raise SystemExit(f"cannot read the engine palette: no {needed}")

    rows = []
    for m in ENTRY.finditer(HEADER.read_text(encoding="utf-8")):
        rows.append((m.group(1), triple(m.group(2)), triple(m.group(3)),
                     triple(m.group(4)), *(int(m.group(k)) for k in range(5, 9))))
    if not rows:
        raise SystemExit(f"no palette entries parsed from {HEADER}")

    # The table is POSITIONAL: entry N must be material N. Nothing in C++
    # enforces that -- the static_assert counts entries and is blind to their
    # order -- so it is checked here against the enum. This is not hypothetical:
    # the table was once grouped by type, which put MAT_BARK_PALE (id 23) up
    # among the woods at index 19 and dressed every broadleaf tree in birch bark.
    names = enum_order()
    first_asset, first_asset_name = first_asset_id()
    if len(rows) != len(names):
        raise SystemExit(f"palette has {len(rows)} entries, enum has {len(names)}")
    for i, (name, *_) in enumerate(rows):
        if name != names[i]:
            raise SystemExit(
                f"palette is out of step with the enum at index {i}: "
                f"table has {name}, enum has {names[i]}")

    out = [
        "// GENERATED by ue-project/Tools/gen_material_palette_ush.py -- do not edit.",
        "//",
        "// What each material looks like, from vxc::kMaterialPalette",
        "// (voxel-core/include/voxelcore/materialpalette.h). ADR-0008 is the",
        "// decision this implements; its four invariants are load-bearing and the",
        "// evaluation below is written to keep them:",
        "//",
        "//   1. one entry per vxc::Material   -- the count is asserted at generation",
        "//   2. the tint is keyed to the VOXEL, not the face",
        "//   3. variation has two frequencies, and BOTH are required",
        "//   4. face class is for MATERIAL difference, not for shading",
        "//",
        "// Colours are LINEAR. The header authors them in sRGB.",
        "//",
        "// The last section is the route into the pooled renderer: what",
        "// VoxelQuadVertexFactory.ush packs per vertex and unpacks per pixel on",
        "// its way to M_VoxelTerrain's TexCoords[3]/[4].",
        "",
        "#pragma once",
        "",
        f"#define VOXEL_MATERIAL_COUNT {len(rows)}u",
        "",
        "// The first material that only ever arrives from a baked asset, read",
        f"// from core.h's own ASSET MATERIALS banner ({first_asset_name} is the",
        "// first one below it). Terrain cannot produce these, so this is the",
        "// line the renderer uses to decide whether the palette or the biome",
        "// graph owns a voxel's colour.",
        f"#define VOXEL_MATERIAL_FIRST_ASSET {first_asset}u",
        "",
        "// Face class, matching vxc::FaceClass.",
        "#define VOXEL_FACE_TOP    0u",
        "#define VOXEL_FACE_SIDE   1u",
        "#define VOXEL_FACE_BOTTOM 2u",
        "",
        "// Base colour, indexed [material * 3 + faceClass].",
        f"static const float3 VoxelPaletteFace[{len(rows) * 3}] =",
        "{",
    ]
    for name, top, side, bottom, *_ in rows:
        cols = ", ".join(
            "float3(%.6f, %.6f, %.6f)" % tuple(to_linear(c) for c in face)
            for face in (top, side, bottom))
        out.append(f"\t{cols},  // {name}")
    out += [
        "};",
        "",
        "// Variation, as fractions of full range: x = per-voxel lightness jitter,",
        "// y = per-voxel hue drift, z = patch strength, w = patch wavelength in",
        "// VOXELS (0 disables the patch term).",
        f"static const float4 VoxelPaletteVariation[{len(rows)}] =",
        "{",
    ]
    for name, _t, _s, _b, jitter, hue, patch, scale in rows:
        out.append("\tfloat4(%.6f, %.6f, %.6f, %.1f),  // %s"
                   % (jitter / 255.0, hue / 255.0, patch / 255.0, float(scale), name))
    out += ["};", ""]
    out.append(EVAL)
    return "\n".join(out) + "\n"


# The evaluation itself is hand-written and lives here so it travels with the
# table it reads. Everything above this line is data; everything below is the
# ADR's "In-game implementation" block, expressed once.
EVAL = r'''
// Integer hash of a voxel coordinate. Cheap, and good enough that neighbouring
// voxels are uncorrelated -- which is the only property the dither needs.
uint VoxelPaletteHash(int3 v)
{
	uint3 u = asuint(v);
	uint h = u.x * 0x8da6b343u + u.y * 0xd8163841u + u.z * 0xcb1ab31fu;
	h ^= h >> 15;
	h *= 0x2c1b3c6du;
	h ^= h >> 12;
	return h;
}

// Two uncorrelated values in [-1, 1] from one hash.
float2 VoxelPaletteSigned(uint h)
{
	return float2((h & 0xffffu) * (2.0f / 65535.0f) - 1.0f,
	              (h >> 16)     * (2.0f / 65535.0f) - 1.0f);
}

// Value noise on the voxel lattice, trilinear between hashed corners. This is
// the SLOW term, and ADR-0008 invariant 3 says it is not optional: per-voxel
// jitter alone averages back to its own mean once voxels fall below a pixel, so
// a varied hillside flattens to grey at exactly the range where the variation
// is doing the most work. This is what survives that averaging, because it is
// coherent over metres.
float VoxelPatchNoise(float3 p)
{
	float3 i = floor(p);
	float3 f = p - i;
	f = f * f * (3.0f - 2.0f * f);          // smoothstep, so patches have no seams
	int3 b = (int3)i;

	float c[8];
	[unroll] for (int k = 0; k < 8; ++k)
	{
		int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
		c[k] = (VoxelPaletteHash(b + o) & 0xffffu) * (1.0f / 65535.0f);
	}
	float x00 = lerp(c[0], c[1], f.x), x10 = lerp(c[2], c[3], f.x);
	float x01 = lerp(c[4], c[5], f.x), x11 = lerp(c[6], c[7], f.x);
	return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z) * 2.0f - 1.0f;
}

// The colour of one voxel face, before any lighting.
//
// `voxel` is the INTEGER voxel coordinate, not the face and not the hit point.
// Invariant 2: all six faces of one cube share a tint, because hashing per face
// gives a cube a different colour on its top than its side and it reads as six
// unrelated squares instead of one solid object -- the opposite of what a cubic
// world trades on. A greedy quad spans many voxels, so this has to be evaluated
// per pixel from the reconstructed voxel coordinate, never per vertex.
//
// No lighting or ambient occlusion is applied here. Invariant 4: those are the
// renderer's business, and baking a top-is-brighter bias into the table would
// double-count them and go wrong the moment the sun moves.
float3 VoxelMaterialColor(uint mat, uint faceClass, int3 voxel)
{
	mat = min(mat, VOXEL_MATERIAL_COUNT - 1u);
	const float3 base = VoxelPaletteFace[mat * 3u + min(faceClass, 2u)];
	const float4 var  = VoxelPaletteVariation[mat];

	const float2 r = VoxelPaletteSigned(VoxelPaletteHash(voxel));

	// Lightness and hue are kept apart because they read differently: lightness
	// variation looks like uneven light, hue variation looks like a mix of
	// stuff. The hue axis is the material's own warm/cool one -- push red up
	// and blue down and back -- rather than a rotation in some colour space the
	// rest of this renderer does not use.
	const float lightness = r.x * var.x;
	const float hue       = r.y * var.y;

	float patch = 0.0f;
	if (var.w > 0.0f)
	{
		patch = VoxelPatchNoise((float3)voxel / var.w) * var.z;
	}

	float3 c = base * (1.0f + lightness + patch);
	c.r *= 1.0f + hue;
	c.b *= 1.0f - hue;
	return max(c, 0.0f);
}

// ---------------------------------------------------------------------------
// THE ROUTE FROM A POOLED QUAD TO THE MATERIAL GRAPH
//
// VoxelQuadVertexFactory.ush carries the palette to M_VoxelTerrain through
// TexCoords[3]/[4] -- three channels of colour and one binary "is this an asset
// voxel". Both halves of that live HERE, in the file DXC already compiles from
// tools/compile-shaders.ps1, rather than in the vertex factory, which cannot be
// compiled standalone because of its Unreal includes. That leaves the factory
// holding one call on each side and nothing else to get wrong, and it means
// VoxelMaterialPaletteTest.usf is a real compile check of the code that ships
// rather than of a copy of it.
//
// WHY TWO FUNCTIONS AND NOT ONE. The pack half runs per VERTEX (it needs the
// quad's decoded bits, which only the vertex shader has) and the unpack half
// runs per PIXEL (invariant 2: a greedy quad spans many voxels, so a per-vertex
// tint would colour whole quads). What crosses between them is one float4.

// Face class from the quad's own axis/direction bits. Mirrors vxc::faceClassOf
// term for term: only a Z face can be a top or a bottom.
uint VoxelPaletteFaceClass(uint axis, bool positive)
{
	return (axis != 2u) ? VOXEL_FACE_SIDE
	                    : (positive ? VOXEL_FACE_TOP : VOXEL_FACE_BOTTOM);
}

// Everything the pixel shader needs, in one interpolant.
//
//   xyz = the position, in VOXELS, of the point on the face stepped half a
//         voxel back along the face normal -- so floor() of it names the SOLID
//         voxel the face belongs to rather than the air cell in front of it.
//         A +Z face of voxel k sits at (k+1)*size; minus half a voxel it is
//         (k+0.5)*size, and floor(k+0.5) == k. The -Z face lands on the same k.
//   w   = mat | faceClass<<8, which is constant over the quad.
//
// THE ARITHMETIC IS DELIBERATELY DONE CHUNK-LOCALLY AND THEN OFFSET. Chunk-local
// positions are exact small multiples of the voxel size, so `insideVox` is
// exactly an integer plus a half and the lattice this floors onto lines up with
// the voxel grid instead of straddling it -- a half-voxel misalignment would put
// TWO tints across ONE voxel face, which is the artefact this is avoiding. The
// chunk origin is added as a rounded integer voxel count for the same reason.
// It is also why nothing here is computed from a world position: pool space
// carries this world's 84 km offset only in the component's double-precision
// transform, and a float32 world coordinate would be quantised coarser than a
// voxel long before it got here.
//
// `voxelSizeUU` is the quad's own voxel size, i.e. already multiplied by the
// chunk's level scale, so a coarse ring gets one tint per RENDERED cube rather
// than one per 32.
float4 VoxelPalettePack(float3 localPositionUU, float3 chunkOriginUU, float3 faceNormal,
                        float voxelSizeUU, uint mat, uint axis, bool positive)
{
	const float invVoxel = 1.0f / voxelSizeUU;
	const float3 insideVox = (localPositionUU - faceNormal * (0.5f * voxelSizeUU)) * invVoxel;
	const float3 originVox = round(chunkOriginUU * invVoxel);
	const float key = float(mat + VoxelPaletteFaceClass(axis, positive) * 256u);
	return float4(insideVox + originVox, key);
}

// The pixel half. `rgb` is the palette colour, linear, with no lighting and no
// ambient occlusion applied (invariant 4 -- those are the renderer's, and the
// material graph multiplies them in after the lerp). `isAsset` is 1 only for
// materials terrain cannot produce.
//
// ROUNDING THE KEY IS NOT PARANOIA. It is constant across the quad, so the
// rasteriser's plane equation is flat and the value is exact in theory; in
// practice perspective-correct interpolation divides and multiplies by w, and
// the result can land an ULP either side of the integer. Truncating a 538 that
// arrived as 537.99999 would move a leaf voxel two face classes.
void VoxelPaletteUnpack(float4 packed, out float3 rgb, out float isAsset)
{
	const uint key = (uint)(packed.w + 0.5f);
	const uint mat = key & 0xffu;
	const uint faceClass = key >> 8u;
	rgb = VoxelMaterialColor(mat, faceClass, (int3)floor(packed.xyz));
	// >= and not a range test: MAT_WATERMARK sits just below this line and has
	// its own sentinel path in the factory, and everything above it is an asset
	// material by construction (core.h's banner is what generated this number).
	isAsset = (mat >= VOXEL_MATERIAL_FIRST_ASSET) ? 1.0f : 0.0f;
}
'''


# The one row of terrain_palette.py's PALETTE list, and the two things about it
# that are NOT this generator's to decide: the name (which is the enum's) and the
# trailing True/False (which is UE-side biome policy, authored there).
TERRAIN_ROW = re.compile(
    r'^\s*\("(\w+)",\s*[-\d.eE+]+,\s*[-\d.eE+]+,\s*[-\d.eE+]+,\s*(True|False)\),\s*$')


def terrain_source() -> str:
    """The text `ue-project/Tools/terrain_palette.py` should contain.

    Only the three RGB numbers on each row are rewritten. Everything else in
    that file -- the docstring, the comments, the material names, the BIOME_TINT
    column, `biome_tinted_runs` -- is left byte for byte as authored, because
    BIOME_TINT is a UE-side policy call ("does the surface biome colour replace
    this material's own albedo") and has no counterpart in the engine header.
    That split is the whole point: colour is generated, policy is written by a
    person.

    The colour taken is the TOP face. terrain_palette.py explains why and what
    it costs; the short version is that the table is indexed by material id with
    no face information, so a single value has to stand for three, and terrain
    is looked at from above.
    """
    if not TERRAIN.exists():
        raise SystemExit(f"cannot read the UE palette table: no {TERRAIN}")

    rows = {}
    for m in ENTRY.finditer(HEADER.read_text(encoding="utf-8")):
        # group(2) is the TOP face; group(3)/(4) are side and bottom.
        rows[m.group(1)] = triple(m.group(2))

    out, seen = [], []
    for line in TERRAIN.read_text(encoding="utf-8").splitlines():
        row = TERRAIN_ROW.match(line)
        if not row:
            out.append(line)
            continue
        name, tint = row.group(1), row.group(2)
        key = "MAT_" + name
        if key not in rows:
            raise SystemExit(
                f"{TERRAIN.name} has an entry for {name}, which is not in "
                f"{HEADER.name}. Material ids are append-only; if this one was "
                "renamed, rename it in core.h and here together.")
        seen.append(key)
        r, g, b = (to_linear(c) for c in rows[key])
        out.append('    ("%s",%s%.6f, %.6f, %.6f, %s),'
                   % (name, " " * max(1, 18 - len(name)), r, g, b, tint))

    # One entry per vxc::Material, in id order -- the same shape the C++ table's
    # static_assert enforces, for the same reason. A material added to core.h
    # without a row here would otherwise be a hole in a table indexed by id, and
    # a hole in a table indexed by id renders as SOMETHING.
    names = enum_order()
    if seen != names:
        missing = [n for n in names if n not in seen]
        extra = [n for n in seen if n not in names]
        raise SystemExit(
            f"{TERRAIN.name} is out of step with the enum.\n"
            f"  missing rows (add them, with a BIOME_TINT decision): {missing or 'none'}\n"
            f"  rows with no enum entry: {extra or 'none'}\n"
            f"  order matches: {seen == sorted(seen, key=names.index) if not extra else 'n/a'}")
    return "\n".join(out) + "\n"


def report_drift(path: Path, want: str) -> int:
    """0 if `path` already holds `want`, else 1 and the first differing line."""
    if not path.exists():
        print(f"{path} does not exist; run this without --check", file=sys.stderr)
        return 1
    # read_text normalises line endings, so this compares content and does not
    # trip over a checkout that landed CRLF.
    got = path.read_text(encoding="utf-8")
    if got == want:
        return 0
    gl, wl = got.splitlines(), want.splitlines()
    for i in range(max(len(gl), len(wl))):
        g = gl[i] if i < len(gl) else "<end of file>"
        w = wl[i] if i < len(wl) else "<end of file>"
        if g != w:
            print(f"{path.name} line {i + 1} differs\n  header says: {w}\n"
                  f"  file has:    {g}\n  run python {Path(__file__).name}",
                  file=sys.stderr)
            break
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="fail if either generated file has drifted from the header")
    a = ap.parse_args()

    targets = [(OUT, source()), (TERRAIN, terrain_source())]
    if not a.check:
        for path, want in targets:
            path.write_text(want, encoding="utf-8")
            print(f"wrote {path}")
        return 0

    rc = 0
    for path, want in targets:
        rc |= report_drift(path, want)
    if rc == 0:
        print("palette shader and terrain_palette.py match the engine header: pass")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
