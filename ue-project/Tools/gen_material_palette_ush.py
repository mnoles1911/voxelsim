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

returns non-zero and names the first differing line when a committed file has
drifted from the header. That check needs no Unreal and no editor, which is the
point -- it has to be runnable by whoever is holding the box and by whoever is
not.

THE DATA IS GENERATED, THE EVALUATION IS MIRRORED. Everything above the EVAL
string below is the table. EVAL itself is hand-written HLSL, and it is a
transcription of `vxc::voxelTint` / `vxc::applyTintQ16`
(voxel-core/include/voxelcore/materialcolor.h), which is the definition. A
transcription can drift from its source in exactly the way this file exists to
prevent, so it is checked rather than trusted: tools/check-palette-parity.py
evaluates both on the same inputs and fails on a disagreement wider than
fixed-point rounding. It runs on Linux, needs no GPU, and CI runs it.

FOUR THINGS THIS FILE DECIDES, none of which the header does.

sRGB TO LINEAR. The header's values are sRGB, the numbers a designer reads off a
colour picker. Unreal shades in linear. Converting here rather than in the
shader keeps it exact and free, and matches what `gen_terrain_textures.py`
already does on its own way into a texture -- skipping that encode was a real
bug there that made every colour come back darker and more saturated than
authored.

BYTES TO FRACTIONS. `voxelJitter`, `voxelHue`, `patchStrength` and `biomeTint`
are 1/255ths of full range in the header because `MaterialAppearance` is all
`uint8_t` (no new float in voxel-core). The shader wants fractions, so they are
divided once here instead of on every pixel.

DECIMETRES TO MILLIMETRES. `patchScaleDm` is authored in decimetres because at
10 cm voxels that keeps the numbers small and readable. The shader's cube size
arrives in millimetres, so the conversion happens once, here.

WHERE THE ASSET MATERIALS START. `VOXEL_MATERIAL_FIRST_ASSET` is read from the
`--- ASSET MATERIALS ---` banner in core.h AND cross-checked against
`vxc::kFirstAssetMaterial`, rather than typed as 16. The renderer wiring uses it
to decide which voxels the palette is allowed to colour, and a hardcoded 16 here
would be a third copy of a boundary that already exists twice -- this repo's
documented failure mode.
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
    r"(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}",
    re.S,
)
ENUM = re.compile(r"^\s*(MAT_\w+)\s*=\s*(\d+)\s*,", re.M)
# The banner core.h puts above the block of materials that only ever arrive from
# a baked asset. Everything above it is something the terrain amplifier can
# emit; everything below it is asset-forge's.
ASSET_BANNER = re.compile(r"^\s*//\s*-+\s*ASSET MATERIALS\s*-+\s*$", re.M)
FIRST_ASSET_CONST = re.compile(
    r"kFirstAssetMaterial\s*=\s*(MAT_\w+)\s*;")
VOXEL_SIZE_MM = re.compile(r"kVoxelSizeMm\s*=\s*(\d+)\s*;")

# The number of trailing scalar columns ENTRY expects, named so the failure
# message when the struct grows says what to do rather than "no palette entries
# parsed", which is what it said the first time a column was added.
COLUMNS = ("voxelJitter", "voxelHue", "patchStrength", "patchScaleDm", "biomeTint")


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


def voxel_size_mm() -> int:
    """vxc::kVoxelSizeMm. The size of a level-0 cube, and the base of the ring
    scale the pack function encodes."""
    m = VOXEL_SIZE_MM.search(CORE.read_text(encoding="utf-8"))
    if not m:
        raise SystemExit(f"no kVoxelSizeMm in {CORE}")
    return int(m.group(1))


def first_asset_id():
    """The id of the first ASSET material, from core.h's own banner.

    Raises rather than guessing. A wrong answer here is the failure this whole
    file exists to prevent: too low and terrain voxels would start taking asset
    colours, too high and a bark voxel would silently keep the biome graph's
    answer -- and both of those still render as SOMETHING.

    Cross-checked against `vxc::kFirstAssetMaterial`, which is what voxel-core's
    own static_asserts use. Two readings of one boundary that must agree beats
    two boundaries.
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

    named = FIRST_ASSET_CONST.search(text)
    if not named:
        raise SystemExit(
            f"no kFirstAssetMaterial constant in {CORE}; voxel-core's palette "
            "asserts need it and this generator cross-checks the banner against it")
    if named.group(1) != after.group(1):
        raise SystemExit(
            f"the asset boundary disagrees with itself in {CORE.name}: the "
            f"banner is above {after.group(1)} but kFirstAssetMaterial is "
            f"{named.group(1)}. One of them moved without the other.")
    return int(after.group(2)), after.group(1)


def rows():
    """Every palette row, in file order, with its five scalar columns."""
    parsed = [(m.group(1), triple(m.group(2)), triple(m.group(3)), triple(m.group(4)),
               *(int(m.group(k)) for k in range(5, 10)))
              for m in ENTRY.finditer(HEADER.read_text(encoding="utf-8"))]
    if not parsed:
        raise SystemExit(
            f"no palette entries parsed from {HEADER}. ENTRY expects exactly "
            f"{len(COLUMNS)} scalar columns per row ({', '.join(COLUMNS)}); if "
            "MaterialAppearance gained or lost one, update ENTRY and every "
            "emitter below rather than letting this read as an empty table.")

    # The table is POSITIONAL: entry N must be material N. Nothing in C++
    # enforces that -- the static_assert counts entries and is blind to their
    # order -- so it is checked here against the enum. This is not hypothetical:
    # the table was once grouped by type, which put MAT_BARK_PALE (id 23) up
    # among the woods at index 19 and dressed every broadleaf tree in birch bark.
    names = enum_order()
    if len(parsed) != len(names):
        raise SystemExit(f"palette has {len(parsed)} entries, enum has {len(names)}")
    for i, (name, *_) in enumerate(parsed):
        if name != names[i]:
            raise SystemExit(
                f"palette is out of step with the enum at index {i}: "
                f"table has {name}, enum has {names[i]}")
    return parsed


def source() -> str:
    """The text `VoxelMaterialPalette.ush` should contain.

    Everything the generator decides happens here and nothing is written, so
    `--check` can redo the same work in memory and compare it against disk.
    """
    for needed in (HEADER, CORE):
        if not needed.exists():
            raise SystemExit(f"cannot read the engine palette: no {needed}")

    table = rows()
    first_asset, first_asset_name = first_asset_id()
    base_cube_mm = voxel_size_mm()

    out = [
        "// GENERATED by ue-project/Tools/gen_material_palette_ush.py -- do not edit.",
        "//",
        "// What each material looks like, from vxc::kMaterialPalette",
        "// (voxel-core/include/voxelcore/materialpalette.h), and how that is",
        "// evaluated, from vxc::voxelTint (voxelcore/materialcolor.h).",
        "//",
        "// ADR-0008 is the decision this implements and ADR-0009 extends it to",
        "// terrain. The invariants both rest on:",
        "//",
        "//   1. one entry per vxc::Material   -- the count is asserted at generation",
        "//   2. the tint is keyed to the VOXEL, not the face",
        "//   3. variation has two frequencies, and BOTH are required",
        "//   4. face class is for MATERIAL difference, not for shading",
        "//   5. the patch wavelength is WORLD metric, not per rendered cube",
        "//",
        "// Colours are LINEAR. The header authors them in sRGB.",
        "//",
        "// THE EVALUATION BELOW IS A TRANSCRIPTION, not a second definition.",
        "// voxelcore/materialcolor.h is the definition; this is the float mirror",
        "// of it that a shader can run. tools/check-palette-parity.py evaluates",
        "// both on the same inputs and fails when they disagree by more than",
        "// fixed-point rounding, so the mirror cannot drift unnoticed.",
        "",
        "#pragma once",
        "",
        f"#define VOXEL_MATERIAL_COUNT {len(table)}u",
        "",
        "// The first material that only ever arrives from a baked asset, read",
        f"// from core.h's own ASSET MATERIALS banner ({first_asset_name} is the",
        "// first one below it) and cross-checked against vxc::kFirstAssetMaterial.",
        "// Terrain cannot produce these.",
        f"#define VOXEL_MATERIAL_FIRST_ASSET {first_asset}u",
        "",
        "// vxc::kVoxelSizeMm -- the size of a LEVEL-0 cube. A coarse streaming",
        "// ring renders 2^level of them as one cube, which is what the pack",
        "// function encodes and what makes the patch wavelength world-metric.",
        f"#define VOXEL_PALETTE_BASE_CUBE_MM {float(base_cube_mm):.1f}",
        "",
        "// Face class, matching vxc::FaceClass.",
        "#define VOXEL_FACE_TOP    0u",
        "#define VOXEL_FACE_SIDE   1u",
        "#define VOXEL_FACE_BOTTOM 2u",
        "",
        "// Base colour, indexed [material * 3 + faceClass].",
        f"static const float3 VoxelPaletteFace[{len(table) * 3}] =",
        "{",
    ]
    for name, top, side, bottom, *_ in table:
        cols = ", ".join(
            "float3(%.6f, %.6f, %.6f)" % tuple(to_linear(c) for c in face)
            for face in (top, side, bottom))
        out.append(f"\t{cols},  // {name}")
    out += [
        "};",
        "",
        "// Variation, as fractions of full range: x = per-voxel lightness jitter,",
        "// y = per-voxel hue drift, z = patch strength, w = patch wavelength in",
        "// MILLIMETRES OF WORLD (0 disables the patch term). The header authors w",
        "// in decimetres; it is converted here so the shader can divide a",
        "// millimetre position by it directly.",
        f"static const float4 VoxelPaletteVariation[{len(table)}] =",
        "{",
    ]
    for name, _t, _s, _b, jitter, hue, patch, scale, _tint in table:
        out.append("\tfloat4(%.6f, %.6f, %.6f, %.1f),  // %s"
                   % (jitter / 255.0, hue / 255.0, patch / 255.0,
                      float(scale * 100), name))
    out += [
        "};",
        "",
        "// How much of each material's colour the CLIMATE owns: 0 = the material",
        "// keeps its own, 1 = the biome LUT's answer replaces it. This is what",
        "// makes one graph draw a cave wall as rock and the hillside above it as",
        "// grassland, without a per-id branch anywhere in the material.",
        f"static const float VoxelPaletteBiomeTint[{len(table)}] =",
        "{",
    ]
    for name, _t, _s, _b, _j, _h, _p, _sc, tint in table:
        out.append("\t%.6f,  // %s" % (tint / 255.0, name))
    out += ["};", ""]
    out.append(EVAL)
    return "\n".join(out) + "\n"


# The evaluation itself is hand-written and lives here so it travels with the
# table it reads. Everything above this line is data; everything below mirrors
# voxelcore/materialcolor.h term for term.
EVAL = r'''
// Integer hash of a voxel coordinate. Mirrors vxc::voxelColorHash.
//
// The odd constant is load-bearing: without it the mixer maps (0,0,0,0) to 0,
// and 0 is the one input the avalanche cannot spread, so the world origin drew
// the extreme minimum of both variation axes and dragged a whole patch
// wavelength around it dark.
uint VoxelPaletteHash(int3 v, uint salt)
{
	uint3 u = asuint(v);
	uint h = 0x9e3779b9u
	       + u.x * 0x8da6b343u + u.y * 0xd8163841u + u.z * 0xcb1ab31fu
	       + salt * 0x165667b1u;
	h ^= h >> 15;
	h *= 0x2c1b3c6du;
	h ^= h >> 12;
	return h;
}

// Two uncorrelated values in [-1, 1] from one hash. Two, and not two hashes,
// because lightness and hue must not be the same draw -- correlated, every dark
// voxel would also be the warm one and the two axes would read as one stronger
// jitter instead of as "uneven light" and "a mix of stuff".
float2 VoxelPaletteSigned(uint h)
{
	return float2((h & 0xffffu) * (2.0f / 65536.0f) - 1.0f,
	              (h >> 16)     * (2.0f / 65536.0f) - 1.0f);
}

// Value noise on a lattice, trilinear between hashed corners. This is the SLOW
// term, and ADR-0008 invariant 3 says it is not optional: per-voxel jitter alone
// averages back to its own mean once voxels fall below a pixel, so a varied
// hillside flattens to grey at exactly the range where the variation is doing
// the most work. This is what survives that averaging, because it is coherent
// over metres.
float VoxelPatchNoise(float3 p, uint salt)
{
	float3 i = floor(p);
	float3 f = p - i;
	f = f * f * (3.0f - 2.0f * f);          // smoothstep, so patches have no seams
	int3 b = (int3)i;

	float c[8];
	[unroll] for (int k = 0; k < 8; ++k)
	{
		int3 o = int3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
		c[k] = (VoxelPaletteHash(b + o, salt) & 0xffffu) * (1.0f / 65536.0f);
	}
	float x00 = lerp(c[0], c[1], f.x), x10 = lerp(c[2], c[3], f.x);
	float x01 = lerp(c[4], c[5], f.x), x11 = lerp(c[6], c[7], f.x);
	return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z) * 2.0f - 1.0f;
}

// THE VARIATION for one voxel: x = lightness, y = warm/cool tilt, both signed
// fractions. Mirrors vxc::voxelTint.
//
// `voxel` is the INTEGER voxel coordinate, not the face and not the hit point.
// Invariant 2: all six faces of one cube share a tint, because hashing per face
// gives a cube a different colour on its top than its side and it reads as six
// unrelated squares instead of one solid object -- the opposite of what a cubic
// world trades on. A greedy quad spans many voxels, so this has to be evaluated
// per pixel from the reconstructed voxel coordinate, never per vertex.
//
// `cubeSizeMm` is how big one of those cubes is IN THE WORLD, which is what
// makes the patch wavelength mean the same thing in every streaming ring
// (invariant 5). Read per cube instead, one hillside's mottle had a 2 m
// wavelength in the near ring and a 64 m one two rings out, and the two met at
// the ring boundary as a step in the very term that exists to survive distance.
float2 VoxelMaterialVariation(uint mat, int3 voxel, float cubeSizeMm, uint salt)
{
	const float4 var = VoxelPaletteVariation[min(mat, VOXEL_MATERIAL_COUNT - 1u)];
	const float2 r = VoxelPaletteSigned(VoxelPaletteHash(voxel, salt));

	float light = r.x * var.x;
	const float hue = r.y * var.y;

	if (var.z > 0.0f && var.w > 0.0f && cubeSizeMm > 0.0f)
	{
		// BAND LIMIT. Two rendered cubes is the finest wavelength a lattice of
		// those cubes can carry; below it the "patch" is one independent value
		// per cube -- a second jitter with none of the coherence the term exists
		// for, and one that aliases as the camera moves. Stretching rather than
		// dropping keeps a far ring's mottle continuous with the near ring's.
		const float waveMm = max(var.w, 2.0f * cubeSizeMm);
		// The cube CENTRE, so the field is anchored to the world rather than to
		// whichever corner happens to be the origin: without it a coarse ring
		// samples half a cube off the fine ring's answer along their seam.
		const float3 posMm = ((float3)voxel + 0.5f) * cubeSizeMm;
		light += VoxelPatchNoise(posMm / waveMm, salt) * var.z;
	}
	return float2(light, hue);
}

// Apply the variation to a linear colour. Mirrors vxc::applyTintQ16, including
// the clamp at zero and the deliberate absence of one at one -- a tint can
// legitimately push a bright material above 1.0 in linear, and clamping here
// would flatten the top of the range on exactly the materials (snow, pale bark,
// white plumage) that are near it already.
//
// The hue axis is the material's own warm/cool one -- push red up and blue down
// and back -- rather than a rotation in a colour space the rest of this renderer
// does not use.
float3 VoxelApplyVariation(float3 rgb, float2 v)
{
	float3 c = rgb * (1.0f + v.x);
	c.r *= 1.0f + v.y;
	c.b *= 1.0f - v.y;
	return max(c, 0.0f);
}

// The material's own colour for a face, with no variation and no climate.
float3 VoxelMaterialBase(uint mat, uint faceClass)
{
	return VoxelPaletteFace[min(mat, VOXEL_MATERIAL_COUNT - 1u) * 3u + min(faceClass, 2u)];
}

// The whole thing for a voxel whose colour the climate has no opinion about:
// every asset material, and every terrain stratum below the surface. This is
// the entry point a ray marcher calls.
//
// No lighting or ambient occlusion is applied here. Invariant 4: those are the
// renderer's business, and baking a top-is-brighter bias into the table would
// double-count them and go wrong the moment the sun moves.
float3 VoxelMaterialColor(uint mat, uint faceClass, int3 voxel, float cubeSizeMm)
{
	return VoxelApplyVariation(VoxelMaterialBase(mat, faceClass),
	                           VoxelMaterialVariation(mat, voxel, cubeSizeMm, 0u));
}

// ---------------------------------------------------------------------------
// THE ROUTE FROM A POOLED QUAD TO THE MATERIAL GRAPH
//
// VoxelQuadVertexFactory.ush carries the palette to M_VoxelTerrain through
// TexCoords[3]/[4]/[5] -- three channels of unvaried base colour, the two
// variation scalars, and the material's biome-tint weight. Both halves of the
// route live HERE, in the file DXC already compiles from
// tools/compile-shaders.ps1, rather than in the vertex factory, which cannot be
// compiled standalone because of its Unreal includes. That leaves the factory
// holding one call on each side and nothing else to get wrong, and it means
// VoxelMaterialPaletteTest.usf is a real compile check of the code that ships
// rather than of a copy of it.
//
// WHY THE BASE AND THE VARIATION TRAVEL SEPARATELY, when the shader could just
// multiply them together and send three floats. Because the CLIMATE goes between
// them. The composition is
//
//     albedo = lerp(materialBase, climate, biomeTint) * variation
//
// and the graph is where `climate` exists (it is a texture sample driven by two
// vertex-colour channels). Multiplying the variation in here would apply it to
// the material's half only, and since every outdoor surface hands 190-235/255 of
// its colour to the climate, the per-voxel dither would be almost entirely
// averaged away on exactly the surfaces the player spends the whole game looking
// at. This is the difference between a mottled hillside and a flat one.
//
// IT COSTS NO EXTRA INTERPOLANT. Unreal packs customised UVs two per float4, so
// TexCoords[4] already reserves the float4 that TexCoords[5] lives in: a
// material reading UV5 uses a slot that was allocated and unused.
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
//   xyz = the position, in RENDERED CUBES, of the point on the face stepped half
//         a cube back along the face normal -- so floor() of it names the SOLID
//         cube the face belongs to rather than the air cell in front of it.
//         A +Z face of cube k sits at (k+1)*size; minus half a cube it is
//         (k+0.5)*size, and floor(k+0.5) == k. The -Z face lands on the same k.
//   w   = mat | faceClass<<8 | ringLevel<<12, constant over the quad.
//
// THE RING LEVEL IS IN THE KEY because the pixel half needs the cube's WORLD
// size to evaluate a world-metric patch wavelength, and there is no interpolant
// left to spend on it. It is log2 of the quad's voxel size over a level-0 one,
// which is an integer in 0..10 for every ring this renderer has; at 4096 per
// level the key tops out around 41,519, well inside the 2^24 a float32 carries
// exactly.
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
                        float voxelSizeUU, float baseVoxelSizeUU,
                        uint mat, uint axis, bool positive)
{
	const float invVoxel = 1.0f / voxelSizeUU;
	const float3 insideVox = (localPositionUU - faceNormal * (0.5f * voxelSizeUU)) * invVoxel;
	const float3 originVox = round(chunkOriginUU * invVoxel);
	// round(), not floor(): the ratio is a power of two computed in float, and
	// a 7.9999 would drop a whole ring level and halve that ring's mottle.
	const uint level = (uint)max(0.0f, round(log2(max(voxelSizeUU / baseVoxelSizeUU, 1.0f))));
	const float key = float(mat
	                        + VoxelPaletteFaceClass(axis, positive) * 256u
	                        + level * 4096u);
	return float4(insideVox + originVox, key);
}

// The pixel half.
//
//   baseRGB   the material's own colour for this face, LINEAR, with no
//             variation, no climate, no lighting and no ambient occlusion
//   variation the two scalars VoxelApplyVariation consumes, to be applied AFTER
//             the graph has blended in the climate
//   biomeTint how much of this material's colour the climate owns, 0..1
//
// ROUNDING THE KEY IS NOT PARANOIA. It is constant across the quad, so the
// rasteriser's plane equation is flat and the value is exact in theory; in
// practice perspective-correct interpolation divides and multiplies by w, and
// the result can land an ULP either side of the integer. Truncating a 538 that
// arrived as 537.99999 would move a leaf voxel two face classes.
void VoxelPaletteUnpack(float4 packed, out float3 baseRGB, out float2 variation,
                        out float biomeTint)
{
	const uint key = (uint)(packed.w + 0.5f);
	const uint mat = min(key & 0xffu, VOXEL_MATERIAL_COUNT - 1u);
	const uint faceClass = (key >> 8u) & 0xfu;
	const float cubeSizeMm = VOXEL_PALETTE_BASE_CUBE_MM * float(1u << (key >> 12u));

	baseRGB = VoxelMaterialBase(mat, faceClass);
	variation = VoxelMaterialVariation(mat, (int3)floor(packed.xyz), cubeSizeMm, 0u);
	biomeTint = VoxelPaletteBiomeTint[mat];
}
'''


# The one row of terrain_palette.py's PALETTE list. Every field is generated
# now: the name comes from the enum and the four numbers from the header.
TERRAIN_ROW = re.compile(
    r'^\s*\("(\w+)",\s*[-\d.eE+]+,\s*[-\d.eE+]+,\s*[-\d.eE+]+,\s*(True|False)\),\s*$')


def terrain_source() -> str:
    """The text `ue-project/Tools/terrain_palette.py` should contain.

    Every row is rewritten -- the three RGB numbers AND the BIOME_TINT boolean.
    Everything else in that file (the docstring, the comments, the material
    names, `biome_tinted_runs`) is left byte for byte as authored.

    BIOME_TINT USED TO BE THE AUTHORED HALF OF THIS FILE, on the argument that
    "does the surface biome colour replace this material's own albedo" is UE-side
    policy with no counterpart in the engine. ADR-0009 moved it into the header
    as `biomeTint`, a WEIGHT rather than a boolean, because it is a fact about
    the material (rock is the same grey in every climate; grass is not) and
    because every consumer needs the same answer -- asset-forge's preview
    included. This column is now the boolean SHADOW of that weight, kept because
    T_VoxelPalette is a one-texel-per-id texture with nowhere to put a weight,
    and generated so the two cannot disagree.

    The colour taken is the TOP face. terrain_palette.py explains why and what
    it costs; the short version is that the table is indexed by material id with
    no face information, so a single value has to stand for three, and terrain
    is looked at from above.
    """
    if not TERRAIN.exists():
        raise SystemExit(f"cannot read the UE palette table: no {TERRAIN}")

    header_rows = {r[0]: r for r in rows()}

    out, seen = [], []
    for line in TERRAIN.read_text(encoding="utf-8").splitlines():
        row = TERRAIN_ROW.match(line)
        if not row:
            out.append(line)
            continue
        name = row.group(1)
        key = "MAT_" + name
        if key not in header_rows:
            raise SystemExit(
                f"{TERRAIN.name} has an entry for {name}, which is not in "
                f"{HEADER.name}. Material ids are append-only; if this one was "
                "renamed, rename it in core.h and here together.")
        seen.append(key)
        _n, top, _s, _b, _j, _h, _p, _sc, tint = header_rows[key]
        r, g, b = (to_linear(c) for c in top)
        # >= 128, i.e. "the climate owns more of this material than the material
        # does". The one-bit form of a weight has to round somewhere and the
        # midpoint is the only place that does not need an argument.
        out.append('    ("%s",%s%.6f, %.6f, %.6f, %s),'
                   % (name, " " * max(1, 18 - len(name)), r, g, b,
                      "True" if tint >= 128 else "False"))

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
            f"  missing rows (add them): {missing or 'none'}\n"
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
