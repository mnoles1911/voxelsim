#pragma once
// HOW a material's colour is evaluated for one voxel. The table is
// voxelcore/materialpalette.h; this is the arithmetic that reads it.
//
// ---------------------------------------------------------------------------
// WHY THIS FILE EXISTS
// ---------------------------------------------------------------------------
//
// ADR-0008 made the TABLE single and left the EVALUATION to each consumer, and
// three consumers promptly grew three different ones:
//
//   * VoxelMaterialPalette.ush              jitter + hue + patch
//   * VoxelDetailAssetSubsystem.cpp         jitter only, at an invented 0.35
//                                           scale; no hue, no patch at all
//   * asset-forge forge/render.py           all three, but the hue axis at 0.6
//                                           strength and ONE patch wavelength
//                                           for a whole grid
//
// So a fern authored in the forge, the same fern drawn as detail cover, and the
// same species baked into terrain were three different pictures of one row of
// one table. The detail path is the worst of the three and it is not a rounding
// difference: dropping the patch term deletes the half of ADR-0008 invariant 3
// that survives distance, which is the half that stops a hillside of cover
// flattening to one grey exactly where the variation was doing the most work.
//
// This header is the fourth implementation and the last one. It is the
// definition; the shader mirrors it (generated commentary in
// VoxelMaterialPalette.ush names this file), and both are checked against each
// other by tools/check-palette-parity.py, which needs no GPU and no editor.
//
// ---------------------------------------------------------------------------
// THE COMPOSITION, IN ORDER. Four stages, and the order is the definition.
// ---------------------------------------------------------------------------
//
//   1. BASE       kMaterialPalette[mat].face[faceClass]        what it is
//   2. PLACE      lerp(base, climate, biomeTint/255)           where it is
//   3. VARIATION  x (1 + light), then r x (1+hue), b x (1-hue) which one it is
//   4. LIGHT      x ambient occlusion x sun x sky              the renderer's
//
// Stage 3 comes AFTER stage 2 deliberately. The jitter is a property of the
// voxel, not of the palette entry: a grass voxel in a tundra and a grass voxel
// in a rainforest are different colours (stage 2) and both are mottled cube to
// cube (stage 3). Applying the variation before the biome blend would average
// it away wherever the climate dominates, which is every outdoor surface in
// this world.
//
// Stage 4 is NOT here and must not be, per ADR-0008 invariant 4: bake a
// top-is-brighter bias into the table or the tint and it double-counts the
// renderer's own terms and goes wrong the moment the sun moves.
//
// ---------------------------------------------------------------------------
// COLOUR SPACE: APPLY THE TINT IN LINEAR, ALWAYS
// ---------------------------------------------------------------------------
//
// Everything here is a MULTIPLIER, deliberately: voxel-core has no floats
// (doctrine 2.3, enforced by CI) and therefore cannot hold the sRGB transfer
// curve, so it does not own colour space at all. It hands out a scale factor
// and the consumer applies it to its own linear albedo.
//
// A tint is a reflectance scale, and a reflectance scale is meaningful in
// LINEAR light and meaningless in sRGB. Applying `x 1.15` to an sRGB byte
// brightens a dark voxel by a completely different physical amount than a light
// one, so a hillside dithered in sRGB comes out with its contrast pulled toward
// the mid-tones. Convert to linear FIRST -- exactly, with the piecewise curve,
// not a 2.2 gamma -- then apply. The shader is generated with the palette
// already converted; ue-project's CPU path uses FLinearColor(FColor), which is
// the exact decode; asset-forge does the same in gen_palette.py.
//
// ---------------------------------------------------------------------------
// FIXED POINT, AND WHY IT IS NOT JUST DOCTRINE COMPLIANCE
// ---------------------------------------------------------------------------
//
// Q16 (65536 == 1.0) throughout. Doctrine forces integers here, but the result
// is worth having on its own: the tint is bit-identical on every compiler and
// every platform, which is what lets a test pin exact numbers rather than
// asserting a tolerance and hoping. The shader's float version cannot be
// bit-identical to it, so the parity checker allows a tolerance -- but the
// tolerance is measured against something exact rather than against another
// approximation.

#include "voxelcore/materialpalette.h"

namespace vxc {

// 1.0 in the fixed-point format everything below speaks.
inline constexpr int32_t kColorOne = 1 << 16;

// ---------------------------------------------------------------------------
// The colour hash. THIS IS NOT A WORLDGEN HASH and must not become one.
// ---------------------------------------------------------------------------
//
// hash.h's hash2/hash3 are the world's procedural randomness: 64-bit splitmix,
// domain-separated by a registered HashChannel, and world-breaking to change
// (docs/determinism.md). Colour is presentation. It changes nothing about the
// solid set, no edit log depends on it, and a client that dithered a voxel
// differently would still agree with the server about where the voxel is.
//
// So it gets its own 32-bit mixer, for two concrete reasons. It has to run per
// pixel in a shader, where a 64-bit splitmix chain costs real instructions and
// is not available at all on some targets. And a colour that consumed a
// HashChannel id would put a presentation change inside the registry whose
// whole job is to keep worldgen subsystems from correlating -- an id spent on
// something that cannot correlate with anything.
//
// The multipliers are the ones already shipping in VoxelMaterialPalette.ush.
// Keeping them is not inertia: changing them would redither every voxel in the
// world for no gain.
//
// THE ODD CONSTANT IS NOT DECORATION. Without it the mixer maps (0, 0, 0, 0) to
// 0, and 0 is the one input the avalanche cannot spread: every shift-xor and
// multiply leaves it 0. So the voxel at the world origin drew the extreme
// minimum of both the lightness and the hue range -- and, worse, so did the
// patch lattice's corner there, dragging a wavelength of world around the origin
// darker than it should be. One voxel is a curiosity; a 2 m stain at (0, 0, 0)
// is the kind of thing that gets found in a screenshot and diagnosed as a
// lighting bug.
constexpr uint32_t voxelColorHash(int32_t x, int32_t y, int32_t z, uint32_t salt) {
    uint32_t h = 0x9e3779b9u +
                 static_cast<uint32_t>(x) * 0x8da6b343u +
                 static_cast<uint32_t>(y) * 0xd8163841u +
                 static_cast<uint32_t>(z) * 0xcb1ab31fu +
                 salt * 0x165667b1u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h;
}

// The low and high halves of one hash as two uncorrelated Q16 values in
// [-1, 1]. Two, from one hash, because lightness and hue must not be the same
// draw: correlated, every dark voxel would also be the warm one and the two
// axes would read as a single stronger jitter rather than as "uneven light" and
// "a mix of stuff".
struct SignedPairQ16 {
    int32_t a = 0, b = 0;
};

//
// DOUBLE AND RECENTRE, rather than the `x * 131070 / 65535` the shader's float
// line transcribes to. Written that way it is a 32-bit overflow for every half
// above 32768: 65535 * 131070 is 8.6e9 and wraps, so the top half of the range
// folded back to the bottom and every tint in the world came out biased dark by
// half its amplitude. It looked like variation, which is exactly why it needs a
// test (colour_tint_has_no_dc_bias) and not just a read.
//
// The result differs from the shader's float form by at most 2/65536 of full
// range -- one part in 32768 of a tint that is itself a few per cent -- which is
// inside the parity checker's tolerance.
constexpr SignedPairQ16 voxelColorSignedPair(uint32_t h) {
    const int32_t lo = static_cast<int32_t>(h & 0xffffu);
    const int32_t hi = static_cast<int32_t>(h >> 16);
    return {lo * 2 - kColorOne, hi * 2 - kColorOne};
}

// ---------------------------------------------------------------------------
// The slow term: trilinear value noise on a lattice of the material's own
// wavelength, in [-1, 1] Q16.
// ---------------------------------------------------------------------------
//
// Smoothstepped rather than linear between corners, because a linear lerp
// leaves a gradient discontinuity on every lattice plane and at these
// wavelengths that reads as a grid of straight creases across a hillside --
// the same artefact hash.h's quintic fade exists to remove from the terrain
// octaves, for the same reason.
//
// Coordinates arrive in Q16 lattice units (position / wavelength).
constexpr int32_t voxelPatchNoise(int64_t xQ16, int64_t yQ16, int64_t zQ16, uint32_t salt) {
    // Arithmetic shift floors for negatives, which is what a lattice index
    // needs; plain division would truncate toward zero and put a seam through
    // the world origin.
    const int64_t ix = xQ16 >> 16, iy = yQ16 >> 16, iz = zQ16 >> 16;
    const int64_t fx = xQ16 - (ix << 16), fy = yQ16 - (iy << 16), fz = zQ16 - (iz << 16);

    auto smooth = [](int64_t f) -> int64_t {
        const int64_t f2 = (f * f) >> 16;
        return (f2 * (3 * static_cast<int64_t>(kColorOne) - 2 * f)) >> 16;
    };
    const int64_t sx = smooth(fx), sy = smooth(fy), sz = smooth(fz);

    auto corner = [&](int64_t dx, int64_t dy, int64_t dz) -> int64_t {
        const uint32_t h = voxelColorHash(static_cast<int32_t>(ix + dx),
                                          static_cast<int32_t>(iy + dy),
                                          static_cast<int32_t>(iz + dz), salt);
        return static_cast<int64_t>(h & 0xffffu); // Q16 in [0, 1)
    };
    auto mix = [](int64_t a, int64_t b, int64_t t) -> int64_t {
        return a + (((b - a) * t) >> 16);
    };

    const int64_t x00 = mix(corner(0, 0, 0), corner(1, 0, 0), sx);
    const int64_t x10 = mix(corner(0, 1, 0), corner(1, 1, 0), sx);
    const int64_t x01 = mix(corner(0, 0, 1), corner(1, 0, 1), sx);
    const int64_t x11 = mix(corner(0, 1, 1), corner(1, 1, 1), sx);
    const int64_t v = mix(mix(x00, x10, sy), mix(x01, x11, sy), sz);
    return static_cast<int32_t>(v * 2 - kColorOne);
}

// The patch wavelength this material actually gets, in millimetres, once the
// size of the cube sampling it is taken into account.
//
// BAND LIMIT. The authored wavelength is what the material wants; two rendered
// cubes is the finest a lattice of those cubes can carry. Below that the "patch"
// is one independent value per cube -- a second jitter with none of the
// coherence the term exists for, and one that aliases as the camera moves.
// Stretching rather than dropping is what keeps a far ring's mottle continuous
// with the near ring's instead of making it vanish at the seam.
//
// A NAMED FUNCTION RATHER THAN THREE LINES INSIDE voxelTint, because the limit
// is a decision and a decision should be testable as itself. Inlined, the only
// way to check it was to infer it from the statistics of the tint -- and the
// test written that way could not fail: its bound was two independent draws
// apart, which is what the UNLIMITED version produces.
constexpr int64_t patchWavelengthMm(const MaterialAppearance& a, int32_t cubeSizeMm) {
    const int64_t wanted = static_cast<int64_t>(a.patchScaleDm) * 100;
    const int64_t floorMm = 2 * static_cast<int64_t>(cubeSizeMm);
    return wanted > floorMm ? wanted : floorMm;
}

// ---------------------------------------------------------------------------
// The variation, as two multipliers. Stage 3 of the composition.
// ---------------------------------------------------------------------------
struct VoxelTint {
    // Multiply all three linear channels by (1 + lightQ16).
    int32_t lightQ16 = 0;
    // Then red by (1 + hueQ16) and blue by (1 - hueQ16). A warm/cool tilt along
    // the material's own axis rather than a rotation in a colour space nothing
    // else in this renderer uses -- it is one multiply per channel, it never
    // leaves the gamut, and at these amplitudes it is indistinguishable from a
    // proper rotation.
    int32_t hueQ16 = 0;
};

// The tint for one voxel.
//
// `vx, vy, vz` are the INTEGER index of the voxel in the lattice being drawn --
// not the face, not the hit point (ADR-0008 invariant 2: all six faces of a cube
// share one tint, or a cube reads as six unrelated squares).
//
// `cubeSizeMm` is the size of one of those lattice cells IN THE WORLD: 100 for a
// level-0 terrain voxel, 3200 for a cube in the level-5 streaming ring, 50 for a
// 5 cm detail-lattice tuft. It is what makes the patch wavelength mean the same
// thing everywhere -- see the note on patchScaleDm in materialpalette.h.
//
// `salt` separates lattices that share coordinates and must not share a
// pattern: every detail-asset grid starts at its own local origin, so without a
// per-(species, seed) salt every tuft in the world would carry the identical
// dither.
constexpr VoxelTint voxelTint(MaterialId mat, int32_t vx, int32_t vy, int32_t vz,
                              int32_t cubeSizeMm, uint32_t salt = 0) {
    const MaterialAppearance& a =
        kMaterialPalette[mat < kMaterialCount ? mat : MaterialId(kMaterialCount - 1)];

    const SignedPairQ16 r = voxelColorSignedPair(voxelColorHash(vx, vy, vz, salt));

    // Both amplitudes are authored in 1/255ths of full range.
    int64_t light = (static_cast<int64_t>(r.a) * a.voxelJitter) / 255;
    const int64_t hue = (static_cast<int64_t>(r.b) * a.voxelHue) / 255;

    if (a.patchStrength != 0 && a.patchScaleDm != 0 && cubeSizeMm > 0) {
        const int64_t waveMm = patchWavelengthMm(a, cubeSizeMm);

        // The cube CENTRE, so the pattern is anchored to the world and not to
        // whichever corner of a cube happens to be the origin. Without it a
        // coarse ring samples the field half a cube off the fine ring's answer
        // and the two disagree along their seam.
        const int64_t halfMm = cubeSizeMm / 2;
        auto axis = [&](int32_t v) -> int64_t {
            return ((static_cast<int64_t>(v) * cubeSizeMm + halfMm) << 16) / waveMm;
        };
        const int32_t patch = voxelPatchNoise(axis(vx), axis(vy), axis(vz), salt);
        light += (static_cast<int64_t>(patch) * a.patchStrength) / 255;
    }

    return {static_cast<int32_t>(light), static_cast<int32_t>(hue)};
}

// ---------------------------------------------------------------------------
// Applying the stages to a colour. Q16 linear in, Q16 linear out.
// ---------------------------------------------------------------------------
//
// These exist so the ORDER lives in one place too. Three consumers agreeing on
// the table and the tint and then composing them differently is the same defect
// one level further down, and it is not hypothetical: the difference between
// tinting before and after the biome blend is the difference between a mottled
// hillside and a flat one.

// Stage 2. `climate` is the biome LUT's answer for this column, linear Q16.
constexpr void blendBiomeQ16(int32_t rgbQ16[3], const int32_t climateQ16[3], uint8_t biomeTint) {
    if (biomeTint == 0) return;
    for (int c = 0; c < 3; ++c) {
        const int64_t d = static_cast<int64_t>(climateQ16[c]) - rgbQ16[c];
        rgbQ16[c] = static_cast<int32_t>(rgbQ16[c] + (d * biomeTint) / 255);
    }
}

// Stage 3.
//
// Clamped at zero and NOT at one. A tint can legitimately push a bright
// material above 1.0 in linear, and the renderer's exposure is what decides
// whether that matters; clamping here would flatten the top of the jitter's
// range on exactly the materials (snow, pale bark, white plumage) whose whole
// problem is that they are near the top of it already.
constexpr void applyTintQ16(int32_t rgbQ16[3], VoxelTint tint) {
    auto scale = [](int32_t c, int64_t kQ16) -> int32_t {
        const int64_t v = (static_cast<int64_t>(c) * kQ16) >> 16;
        return static_cast<int32_t>(v > 0 ? v : 0);
    };
    const int64_t light = static_cast<int64_t>(kColorOne) + tint.lightQ16;
    for (int c = 0; c < 3; ++c) rgbQ16[c] = scale(rgbQ16[c], light);
    rgbQ16[0] = scale(rgbQ16[0], static_cast<int64_t>(kColorOne) + tint.hueQ16);
    rgbQ16[2] = scale(rgbQ16[2], static_cast<int64_t>(kColorOne) - tint.hueQ16);
}

} // namespace vxc
