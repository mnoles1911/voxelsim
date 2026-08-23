// The Whittaker grid, classified: what surface material each (precipitation,
// temperature) cell of T_VoxelBiomeLUT stands for.
//
//   vxc_biomelut [size]        -> "u v biome mat" per cell, row-major
//
// WHY THIS EXISTS
// ---------------
// ADR-0009 makes near-field colour purely material-led, and the 50 km clipmap
// keeps the biome LUT because a heightmap vertex has no material id to look one
// up with. That leaves two answers to "what colour is a grassland" -- the
// palette, and the LUT's six hand-picked corner anchors -- and
// terrain_material_common.py's own header records what happens then: the near
// field and the vista diverged once already, "the vista was pale green, the
// ground was beige", which is why the graph is shared code.
//
// So the LUT stops being authored and becomes a projection of the palette: each
// cell is classified by the ENGINE's own classifyBiome, mapped through the
// ENGINE's own biomeSurfaceMaterial, and painted with that material's colour.
// The vista is then the palette smoothed across climate space, and a retune
// reaches both paths from one table.
//
// WHY A C++ PROBE AND NOT A PYTHON PORT. classifyBiome is worldgen: header-only,
// integer-only, and mirrored bit-for-bit in worldgen.ush under the CPU/GPU
// contract. A Python transcription of it would be a third copy of the biome
// gates, which is the failure mode this codebase has paid for twice (material
// ids, then colour). gen_terrain_textures.py shells out to this instead, and
// fails loudly with a build command if it is missing.
//
// WHAT A 2D LUT CANNOT REPRESENT, stated because it changes what the vista can
// show. classifyBiome reads THREE climate channels -- temperature,
// precipitation and precipitation variability, the last of which splits SAVANNA
// off GRASSLAND. The LUT has two axes, because that is what the material graph
// samples it with. The third is therefore collapsed to one value, and the cell
// prints which, so nobody has to guess later.

#include <cstdio>
#include <cstdlib>

#include "voxelcore/biome.h"
#include "voxelcore/climate.h"

using namespace vxc;

int main(int argc, char** argv) {
    const int size = argc > 1 ? std::atoi(argv[1]) : 64;
    if (size < 2) {
        std::fprintf(stderr, "size must be > 1\n");
        return 2;
    }

    // The axis remap gen_terrain_textures.py paints over: this world's measured
    // p1..p99, so every texel is reachable by real data. Duplicated there and in
    // VoxelClimateProbe.h, which static_asserts them against climate.h -- the
    // comment in each says to change all three together.
    const int32_t tempLo = 100, tempHi = 189;
    const int32_t precipLo = 14, precipHi = 32;

    // MEASURED, AND IT IS NOT WHAT THIS TEXTURE LOOKS LIKE IT IS.
    //
    // Inside this window the U axis cannot change the answer. classifyBiome's
    // precipitation gates sit at 9 (arid), 10 (semi) and 34 (moderate) on the
    // same u8 scale, so every column from 14 to 32 falls in ONE class -- the
    // `precipU8 < kBiomePrecipModU8` branch -- and the biome is then decided by
    // temperature and seasonality alone. Classifying the grid confirms it:
    // 0 of 64 rows change biome across the whole precipitation axis, while
    // 64 of 64 columns change across temperature.
    //
    // So the vista's 64x64 lookup is, today, a 64-entry TEMPERATURE ramp
    // stretched sideways. That is a fact about the axis window versus the
    // gates, not about any tile data -- it follows from the constants alone.
    //
    // NOT "FIXED" HERE, deliberately. The window is 636..1483 mm/yr, which is a
    // reasonable p1..p99 for real WorldClim land, and it is static_asserted in
    // VoxelClimateProbe.h against climate.h and duplicated in
    // gen_terrain_textures.py -- moving it changes what the whole vista shows
    // and wants the real climate distribution to decide, not this file. What is
    // recorded here is that the next person should not read a horizontally
    // banded LUT as a bug in this probe: it is the correct picture of a window
    // that spans one precipitation class.
    //
    // (Measuring the repo's baked tile cache is NOT how to settle it. Its
    // precipitation runs 101..145 u8, about 4700-6800 mm/yr everywhere, because
    // those tiles are synthetic and their climate is not WorldClim's -- the
    // caveat vxc_matcensus prints for exactly this reason.)

    // JUST ABOVE THE BEACH BAND, so the climate branch is what decides.
    // classifyBiome runs morphology gates FIRST -- sea level, then treeline --
    // and at a high elevation every cold cell would come back TUNDRA_ALPINE and
    // the LUT would be a picture of the treeline rather than of climate. At
    // 100 m the treeline gate still fires where the treeline is genuinely below
    // 100 m, which is a real fact about a very cold place and belongs in the
    // table.
    const int32_t surfaceMm = 100'000;
    const int64_t slopeMmPerM = 0; // flat: the cliff gate is submarine-only at v27

    // The third climate axis, collapsed. 120 is the midpoint of this world's
    // measured p1..p99 for precip variability (88..151); the SAVANNA gate sits
    // at 89, so the warm-dry corner of this table reads savanna. That is a real
    // simplification and not a neutral one -- a vista cell shows the savanna
    // colour wherever temperature and precipitation allow it, whether or not
    // that column's own variability would have.
    const int32_t precipVarU8 = 120;

    std::printf("# vxc_biomelut %d %d\n", size, size);
    std::printf("# temp_u8 %d..%d precip_u8 %d..%d surface_mm %d precip_var_u8 %d\n",
                tempLo, tempHi, precipLo, precipHi, surfaceMm, precipVarU8);
    std::printf("# u v biome mat\n");
    for (int v = 0; v < size; ++v) {
        for (int u = 0; u < size; ++u) {
            const int32_t tempU8 = tempLo + (tempHi - tempLo) * v / (size - 1);
            const int32_t precipU8 = precipLo + (precipHi - precipLo) * u / (size - 1);
            const BiomeId biome =
                classifyBiome(tempU8, precipU8, precipVarU8, surfaceMm, slopeMmPerM);
            const MaterialId mat = biomeSurfaceMaterial(biome, surfaceMm);
            std::printf("%d %d %d %d\n", u, v, int(biome), int(mat));
        }
    }
    return 0;
}
