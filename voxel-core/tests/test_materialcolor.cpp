// The palette table's CONTENT and the one evaluation that reads it.
//
// The static_asserts in materialpalette.h already cover what can be checked at
// compile time (one row per material, asset rows untinted, no patch strength
// without a wavelength). What is left is everything about the EVALUATION, and
// the reason it needs a test at all is that three consumers each wrote their
// own and all three were wrong in a way that still rendered: the detail-asset
// path dropped the patch term entirely and looked fine on one tuft.
//
// Every check here is written so that breaking the thing it guards makes it
// fail. Where a value is pinned exactly it is pinned to a number produced by
// this code and recorded, not to a number derived independently -- an exact
// pin is a tripwire against silent drift, not a proof of correctness, and the
// checks above and below it are what say the value is right.

#include "vxctest.h"

#include "voxelcore/materialcolor.h"

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace vxc;

namespace {

// The composition, end to end, as a consumer would run it. Linear Q16 in and
// out; no colour-space conversion, which is deliberately not voxel-core's job.
struct Composed {
    int32_t rgb[3];
};

Composed compose(MaterialId mat, FaceClass fc, int32_t vx, int32_t vy, int32_t vz,
                 int32_t cubeSizeMm, const int32_t* climate = nullptr, uint32_t salt = 0) {
    const MaterialAppearance& a = kMaterialPalette[mat];
    // sRGB bytes read straight into Q16 without a transfer curve. Fine HERE
    // because every test below compares one composition against another in the
    // same units; a real consumer must convert first (materialcolor.h says so
    // at length).
    Composed out{{a.face[fc].r * 257, a.face[fc].g * 257, a.face[fc].b * 257}};
    if (climate) blendBiomeQ16(out.rgb, climate, a.biomeTint);
    applyTintQ16(out.rgb, voxelTint(mat, vx, vy, vz, cubeSizeMm, salt));
    return out;
}

int64_t absDiff(int32_t a, int32_t b) {
    return a > b ? int64_t(a) - b : int64_t(b) - a;
}

} // namespace

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

VXC_TEST(palette_every_material_has_a_visible_appearance) {
    // MAT_AIR is the only row allowed to be black: it is never drawn. A black
    // row anywhere else is the failure mode ADR-0008's one-entry-per-material
    // rule exists to prevent, and the static_assert cannot see it because the
    // entry is PRESENT -- it is just empty.
    for (int m = 1; m < kMaterialCount; ++m) {
        const MaterialAppearance& a = kMaterialPalette[m];
        int sum = 0;
        for (int f = 0; f < kFaceClassCount; ++f) {
            sum += a.face[f].r + a.face[f].g + a.face[f].b;
        }
        CHECK_MSG(sum > 0, "material has an all-black appearance");
        if (sum == 0) std::fprintf(stderr, "    material id %d\n", m);
    }
}

VXC_TEST(palette_terrain_surfaces_are_climate_led_and_strata_are_not) {
    // The decision this colour system turns on, asserted rather than left to a
    // screenshot. Subsurface strata own their colour outright, which is what
    // makes a cave wall read as rock rather than as one flat tone; surface
    // materials hand most of it to the climate, which is what stops a hillside
    // being the same green in a tundra and a rainforest.
    const MaterialId strata[] = {MAT_BEDROCK, MAT_ROCK, MAT_GRAVEL, MAT_SUBSOIL,
                                 MAT_MUD,     MAT_CLAY};
    for (MaterialId m : strata) {
        CHECK_MSG(kMaterialPalette[m].biomeTint == 0,
                  "a subsurface stratum is climate-tinted: a cave wall would take "
                  "the colour of the grassland above it");
    }
    const MaterialId surfaces[] = {MAT_SAND,   MAT_TOPSOIL,       MAT_GRASS,
                                   MAT_PODZOL, MAT_SAVANNA_GRASS, MAT_JUNGLE_SOIL,
                                   MAT_PERMAFROST};
    for (MaterialId m : surfaces) {
        CHECK(kMaterialPalette[m].biomeTint >= 128);
    }
}

VXC_TEST(palette_materials_that_meet_are_distinguishable) {
    // ADR-0008's stated cost: "materials must be distinguishable by colour and
    // variation alone, which puts real pressure on the palette when two are
    // naturally similar (rock vs gravel, the six leaf types)". That pressure is
    // measurable, so measure it instead of hoping.
    //
    // The pairs are the ones that actually appear side by side -- a cave wall's
    // strata, and the leaf types, two of which can meet in one mixed wood. The
    // bar is a sum-of-channels distance on the TOP face, low enough that it
    // passes today and high enough that collapsing a pair fails it.
    struct Pair {
        MaterialId a, b;
        const char* why;
    };
    const Pair pairs[] = {
        {MAT_ROCK, MAT_GRAVEL, "a cave wall's two commonest neighbours"},
        {MAT_BEDROCK, MAT_ROCK, "the deep/shallow rock boundary"},
        {MAT_SUBSOIL, MAT_TOPSOIL, "the soil profile a dig cuts through"},
        {MAT_LEAF_BROADLEAF, MAT_LEAF_NEEDLE, "mixed woodland"},
        {MAT_LEAF_BROADLEAF, MAT_LEAF_JUNGLE, "adjacent biomes' canopies"},
        {MAT_SAND, MAT_GRAVEL, "the beach/shore boundary"},
        {MAT_BARK, MAT_BARK_PALE, "birch among oak"},
    };
    for (const Pair& p : pairs) {
        const Rgb& x = kMaterialPalette[p.a].face[kFaceTop];
        const Rgb& y = kMaterialPalette[p.b].face[kFaceTop];
        const int d = std::abs(int(x.r) - int(y.r)) + std::abs(int(x.g) - int(y.g)) +
                      std::abs(int(x.b) - int(y.b));
        CHECK_MSG(d >= 24, "two materials that meet in the world are the same colour");
        if (d < 24) std::fprintf(stderr, "    %s: distance %d\n", p.why, d);
    }
}

// ---------------------------------------------------------------------------
// The evaluation
// ---------------------------------------------------------------------------

VXC_TEST(colour_tint_is_keyed_to_the_voxel_not_the_face) {
    // ADR-0008 invariant 2. The tint takes no face argument at all, so this
    // reads as tautological -- and it is exactly the invariant that a future
    // "optimisation" folding the face into the hash would break, at which point
    // this is the test that says why not.
    const VoxelTint t = voxelTint(MAT_ROCK, 12, -7, 300, 100);
    for (int f = 0; f < kFaceClassCount; ++f) {
        int32_t withFace[3] = {kColorOne / 2, kColorOne / 2, kColorOne / 2};
        int32_t plain[3] = {kColorOne / 2, kColorOne / 2, kColorOne / 2};
        applyTintQ16(withFace, voxelTint(MAT_ROCK, 12, -7, 300, 100));
        applyTintQ16(plain, t);
        for (int c = 0; c < 3; ++c) CHECK(withFace[c] == plain[c]);
    }
}

VXC_TEST(colour_neighbouring_voxels_differ) {
    // The near-field half of invariant 3. A hash that correlated neighbours
    // would still produce variation over a large region and would still look
    // "varied" in a wide shot, while the cube-to-cube dither the art direction
    // is built on quietly disappeared.
    int differing = 0;
    for (int i = 0; i < 64; ++i) {
        const VoxelTint a = voxelTint(MAT_GRAVEL, i, 0, 0, 100);
        const VoxelTint b = voxelTint(MAT_GRAVEL, i + 1, 0, 0, 100);
        if (absDiff(a.lightQ16, b.lightQ16) > kColorOne / 200) ++differing;
    }
    CHECK(differing >= 60);
}

VXC_TEST(colour_patch_term_is_coherent_over_metres) {
    // The far-field half. The patch field must vary SLOWLY: adjacent voxels
    // nearly agree, voxels a wavelength apart do not. Measured as the mean
    // absolute step at 1 voxel against the mean at one wavelength.
    //
    // MAT_ROCK: patchScaleDm 20 = 2 m = 20 level-0 voxels.
    const int wave = kMaterialPalette[MAT_ROCK].patchScaleDm;
    int64_t nearSum = 0, farSum = 0;
    const int n = 200;
    for (int i = 0; i < n; ++i) {
        const int32_t p0 = voxelPatchNoise(int64_t(i) << 16, 0, 0, 0);
        const int32_t p1 = voxelPatchNoise(int64_t(i + 1) << 16, 0, 0, 0);
        const int32_t pw = voxelPatchNoise(int64_t(i + wave) << 16, 0, 0, 0);
        nearSum += absDiff(p0, p1);
        farSum += absDiff(p0, pw);
    }
    // Sampled in LATTICE units above, so one step is one whole wavelength and
    // "near" and "far" are both decorrelated -- that is the control. The real
    // check is below, in world units.
    CHECK(farSum > 0);

    int64_t voxNear = 0, voxFar = 0;
    for (int i = 0; i < n; ++i) {
        const auto at = [&](int v) {
            return voxelTint(MAT_ROCK, v, 0, 0, 100).lightQ16;
        };
        voxNear += absDiff(at(i), at(i + 1));
        voxFar += absDiff(at(i), at(i + wave));
    }
    // Both carry the same per-voxel jitter, which is decorrelated at every
    // distance; the patch term is what makes the far figure the larger one.
    CHECK_MSG(voxFar > voxNear, "the patch term is not coherent over metres -- "
                                "either it is switched off or its wavelength is "
                                "at the voxel scale");
}

VXC_TEST(colour_patch_wavelength_is_world_metric_not_per_cube) {
    // The defect this unit change fixes: read as VOXELS, one hillside's mottle
    // had a 2 m wavelength in the near streaming ring and a 64 m one two rings
    // out, and the two met at the ring boundary as a step in the very term that
    // exists to survive distance.
    //
    // MEASURED AS A CORRELATION, and the first version of this test was not --
    // it compared ONE fine sample against ONE coarse sample against a loose
    // bound, and the deliberate reversion to per-cube passed it by luck. Two
    // series over the same stretch of world, one at 10 cm cubes and one at 80 cm
    // cubes, must move together: they carry independent per-voxel jitter (drawn
    // from different lattice indices, so it cannot agree) over a SHARED patch
    // field (a function of world position, so it must).
    //
    // MAT_SNOW because it has the table's longest wavelength (28 dm) and its
    // lowest jitter, so the patch term dominates and the correlation is legible.
    const int n = 400;
    const auto correlation = [](const std::vector<double>& a,
                                const std::vector<double>& b) {
        double ma = 0, mb = 0;
        for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
        ma /= double(a.size());
        mb /= double(b.size());
        double num = 0, da = 0, db = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            const double x = a[i] - ma, y = b[i] - mb;
            num += x * y;
            da += x * x;
            db += y * y;
        }
        return (da > 0 && db > 0) ? num / std::sqrt(da * db) : 0.0;
    };

    std::vector<double> fine, coarse, offset;
    for (int k = 0; k < n; ++k) {
        // The same stretch of world both ways: cube k at 800 mm covers exactly
        // the ground cube 8k does at 100 mm.
        fine.push_back(double(voxelTint(MAT_SNOW, 8 * k, 0, 0, 100).lightQ16));
        coarse.push_back(double(voxelTint(MAT_SNOW, k, 0, 0, 800).lightQ16));
        // The CONTROL: the same coarse series read a long way off, which shares
        // no patch field with the fine one. If this correlates too, the test is
        // measuring nothing and its threshold is meaningless.
        offset.push_back(double(voxelTint(MAT_SNOW, k + 100000, 0, 0, 800).lightQ16));
    }

    const double shared = correlation(fine, coarse);
    const double control = correlation(fine, offset);
    CHECK_MSG(shared > 0.5,
              "the patch field changed scale with the cube size: a coarse "
              "streaming ring is sampling a different mottle from the fine ring "
              "it abuts, and the seam between them will show");
    CHECK_MSG(control < 0.25,
              "the control correlates, so this test is not measuring what it "
              "claims -- fix the test before trusting the check above");
    if (shared <= 0.5 || control >= 0.25) {
        std::fprintf(stderr, "    shared %.3f, control %.3f\n", shared, control);
    }
}

VXC_TEST(colour_patch_is_band_limited_at_coarse_lod) {
    // A wavelength finer than two rendered cubes is not a patch, it is a second
    // jitter -- and it aliases as the camera moves. The evaluator stretches it
    // instead of dropping it, so a coarse ring's mottle stays continuous with
    // the near ring's rather than vanishing at the seam.
    //
    // TESTED AS THE DECISION IT IS, against patchWavelengthMm directly. The
    // first version of this test inferred the limit from the statistics of the
    // tint and could not fail: its bound was set at two independent draws apart,
    // which is exactly what the UNLIMITED version produces. Removing the limit
    // passed it.
    for (int m = 1; m < kMaterialCount; ++m) {
        const MaterialAppearance& a = kMaterialPalette[m];
        if (a.patchScaleDm == 0) continue;
        const int64_t authored = int64_t(a.patchScaleDm) * 100;

        // At a level-0 voxel every wavelength in the table is well above the
        // floor, so the material gets exactly what it asked for.
        CHECK_EQ(patchWavelengthMm(a, 100), authored);

        // ...and at a cube big enough to undersample it, it is stretched to
        // exactly two cubes -- not dropped, not left to alias.
        const int32_t coarse = int32_t(authored); // one cube per authored wavelength
        CHECK_EQ(patchWavelengthMm(a, coarse), 2 * authored);

        // Monotonic in the cube size, all the way up the ring ladder. A limit
        // that went backwards anywhere would make one ring's mottle finer than
        // the ring inside it, which is the seam this exists to prevent.
        int64_t previous = 0;
        for (int level = 0; level <= 10; ++level) {
            const int64_t w = patchWavelengthMm(a, 100 << level);
            CHECK_MSG(w >= previous, "the patch wavelength shrinks as cubes grow");
            previous = w;
        }
    }
}

VXC_TEST(colour_amplitude_matches_the_authored_numbers) {
    // The table's units are 1/255ths of full range, and a consumer that halved
    // them (or, as the detail-asset path did, multiplied by an invented 0.35)
    // would still produce variation -- just not the variation a designer
    // approved. Sample widely and check the observed range against the authored
    // one.
    for (int m = 1; m < kMaterialCount; ++m) {
        const MaterialAppearance& a = kMaterialPalette[m];
        if (a.voxelJitter == 0 && a.patchStrength == 0) continue;
        int32_t lo = kColorOne, hi = -kColorOne;
        for (int i = 0; i < 400; ++i) {
            const int32_t l = voxelTint(MaterialId(m), i * 7, i * 13, i * 3, 100).lightQ16;
            if (l < lo) lo = l;
            if (l > hi) hi = l;
        }
        const int64_t authored =
            (int64_t(kColorOne) * (a.voxelJitter + a.patchStrength)) / 255;
        // Reaches most of the authored range over 400 samples...
        CHECK(hi - lo > authored);
        // ...and never exceeds it, which is what keeps a bright material from
        // being pushed past its own colour.
        CHECK(hi <= authored && -lo <= authored);
    }
}

VXC_TEST(colour_tint_has_no_dc_bias) {
    // A tint is supposed to move voxels either side of the material's authored
    // colour. If its mean drifts off zero the whole material darkens (or
    // brightens) by that much, and nothing about the picture says so -- it just
    // looks like the palette was authored wrong.
    //
    // THIS TEST FOUND A REAL DEFECT and is the reason it is written as a
    // measurement rather than an inspection. The signed-pair extraction was
    // transcribed from the shader's float line as `x * 131070 / 65535`, which
    // overflows uint32 for every half above 32768: the top of the range folded
    // back to the bottom, every tint in the world came out biased dark by half
    // its amplitude, and the result still looked like variation.
    for (int m = 1; m < kMaterialCount; ++m) {
        const MaterialAppearance& a = kMaterialPalette[m];
        if (a.voxelJitter == 0 && a.voxelHue == 0 && a.patchStrength == 0) continue;
        int64_t light = 0, hue = 0;
        int n = 0;
        // Wide enough to cover many patch wavelengths -- over a handful of
        // lattice cells the slow term legitimately has a mean of its own.
        for (int x = -150; x < 150; ++x) {
            for (int y = -150; y < 150; ++y) {
                const VoxelTint t = voxelTint(MaterialId(m), x, y, x - y, 100);
                light += t.lightQ16;
                hue += t.hueQ16;
                ++n;
            }
        }
        const int64_t authored =
            (int64_t(kColorOne) * (a.voxelJitter + a.patchStrength)) / 255;
        // A twentieth of the material's own amplitude. The overflow above put
        // the mean at half of it.
        CHECK_MSG(absDiff(int32_t(light / n), 0) * 20 <= authored,
                  "the lightness tint is biased: this material renders darker or "
                  "brighter than the colour authored for it");
        const int64_t hueAmp = (int64_t(kColorOne) * a.voxelHue) / 255;
        CHECK(absDiff(int32_t(hue / n), 0) * 20 <= hueAmp + 1);
    }
}

VXC_TEST(colour_zero_variation_materials_are_flat) {
    // MAT_WATERMARK is an instrument. If it ever picks up variation it stops
    // being unmistakable, which is the only property it has.
    for (int i = 0; i < 32; ++i) {
        const VoxelTint t = voxelTint(MAT_WATERMARK, i, i * 3, -i, 100);
        CHECK(t.lightQ16 == 0);
        CHECK(t.hueQ16 == 0);
    }
}

VXC_TEST(colour_salt_separates_lattices_that_share_coordinates) {
    // Every detail-asset grid starts at its own local origin, so without a
    // per-(species, seed) salt every tuft of grass in the world would carry the
    // identical dither -- a field of clones that reads as a repeating texture.
    int same = 0;
    for (int i = 0; i < 64; ++i) {
        if (voxelTint(MAT_LEAF_BROADLEAF, i, 0, 0, 50, 0).lightQ16 ==
            voxelTint(MAT_LEAF_BROADLEAF, i, 0, 0, 50, 7).lightQ16) {
            ++same;
        }
    }
    CHECK(same <= 2);
}

VXC_TEST(colour_biome_blend_respects_the_weight) {
    // Stage 2. Zero means the material keeps its colour outright; 255 means the
    // climate answer replaces it.
    const int32_t climate[3] = {kColorOne / 4, kColorOne, kColorOne / 8};

    int32_t rock[3] = {1000, 2000, 3000};
    blendBiomeQ16(rock, climate, kMaterialPalette[MAT_ROCK].biomeTint);
    CHECK(rock[0] == 1000 && rock[1] == 2000 && rock[2] == 3000);

    int32_t full[3] = {1000, 2000, 3000};
    blendBiomeQ16(full, climate, 255);
    for (int c = 0; c < 3; ++c) CHECK(full[c] == climate[c]);

    // ...and a surface material lands between the two, nearer the climate.
    int32_t grass[3] = {1000, 2000, 3000};
    const int32_t before[3] = {1000, 2000, 3000};
    blendBiomeQ16(grass, climate, kMaterialPalette[MAT_GRASS].biomeTint);
    for (int c = 0; c < 3; ++c) {
        CHECK(absDiff(grass[c], climate[c]) < absDiff(grass[c], before[c]));
    }
}

VXC_TEST(colour_variation_survives_the_biome_blend) {
    // WHY STAGE 3 COMES AFTER STAGE 2. Tinting before the blend would average
    // the variation away wherever the climate dominates -- which is every
    // outdoor surface in this world, since MAT_GRASS hands 215/255 of its
    // colour to the climate. Two neighbouring grass voxels under the SAME
    // climate must still differ.
    const int32_t climate[3] = {kColorOne / 3, kColorOne / 2, kColorOne / 5};
    int differing = 0;
    for (int i = 0; i < 64; ++i) {
        const Composed a = compose(MAT_GRASS, kFaceTop, i, 0, 0, 100, climate);
        const Composed b = compose(MAT_GRASS, kFaceTop, i + 1, 0, 0, 100, climate);
        if (absDiff(a.rgb[1], b.rgb[1]) > kColorOne / 500) ++differing;
    }
    CHECK_MSG(differing >= 60, "climate-led surfaces lost their per-voxel variation");
}

VXC_TEST(colour_is_deterministic_and_pinned) {
    // An exact pin. Its job is to fail loudly when the hash, the noise, the
    // fixed-point scaling or the composition order changes -- all four are
    // things a reasonable-looking edit can move without any other test
    // noticing, because every other check here is a property rather than a
    // value. Regenerate deliberately, and say in the commit message why the
    // world's dither is changing.
    struct Sample {
        MaterialId mat;
        int32_t x, y, z, cubeMm;
        int32_t light, hue;
    };
    const Sample expected[] = {
        {MAT_ROCK, 0, 0, 0, 100, -7056, -499},
        {MAT_ROCK, 1, 0, 0, 100, -6179, 1652},
        {MAT_GRASS, -40, 17, 902, 100, -9343, -3138},
        {MAT_LEAF_BROADLEAF, 5, 5, 5, 50, -8588, 6290},
        {MAT_SNOW, 1000, -1000, 64, 3200, 1858, 728},
        {MAT_BARK, 7, -3, 21, 100, -2791, 827},
    };
    for (const Sample& s : expected) {
        const VoxelTint t = voxelTint(s.mat, s.x, s.y, s.z, s.cubeMm);
        CHECK_EQ(t.lightQ16, s.light);
        CHECK_EQ(t.hueQ16, s.hue);
    }
}
