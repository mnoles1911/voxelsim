#pragma once
// What each material LOOKS like. One definition, for every consumer.
//
// The renderer is a GPU ray marcher over a single voxel volume that holds both
// terrain and baked assets, so a bark voxel and a cliff voxel are shaded by the
// same code from the same table. There is no second appearance path to keep in
// step -- which is deliberate, and worth protecting: material IDs were declared
// in two places for a while and drifted, and because MaterialId is a uint8_t
// the result rendered as SOMETHING rather than failing. Colour would drift the
// same way and be even harder to notice.
//
// THE LOOK THIS IS BUILT FOR: every voxel face is one flat colour. Not a
// texture per face -- at 5-10 cm a face is a few pixels and a pattern inside it
// reads as noise -- and not world-space procedural detail, which puts structure
// at a finer scale than a voxel and fights the cubic read. One colour per
// voxel, varied between voxels.
//
// WHY THE VARIATION HAS TWO SCALES. Per-voxel jitter alone looks right up close
// and disappears at range: once voxels are smaller than a pixel the jitter
// averages back to its own mean, so a varied hillside turns into flat grey
// exactly when you most want it to read. A slow patch term survives that
// averaging because it is coherent over metres. Near-field dither and
// far-field mottle are different jobs and need different frequencies.
//
// WHY THE TINT IS KEYED TO THE VOXEL AND NOT THE FACE. All six faces of one
// cube share a tint. Hashing per face would give a cube a different colour on
// its top than its side, which reads as six unrelated squares rather than one
// solid object -- the exact opposite of what a cubic world is trading on.
//
// WHAT THIS FILE IS AND IS NOT. It is the TABLE: five numbers and three colours
// per material, and nothing that reads them. The EVALUATION -- the hash, the
// two noise frequencies, the order the terms compose in -- is
// voxelcore/materialcolor.h, and it is separate because it is the half that
// drifted. Three consumers each grew their own version of it: the shader
// applied jitter, hue and patch; the detail-asset subsystem applied jitter only
// at an invented 0.35 scale; asset-forge's preview applied all three with the
// hue axis at 0.6 strength and one wavelength for a whole grid. All three read
// the same table and drew different pictures from it, which is exactly the
// failure the table was made single to prevent -- just moved one level down.
//
// THE THIRD SCALE. Two frequencies of variation belong to the material
// (per-voxel jitter, per-patch mottle). The third belongs to the PLACE: a
// biome's climate colour, which crosses a whole landscape. `biomeTint` below is
// how much of each material's colour that answer owns, and it is what stops the
// world being either "one flat rock tone underground" or "the same green
// hillside in the tundra and the rainforest".

#include "voxelcore/core.h"

namespace vxc {

// Which of a voxel's faces was hit. A ray marcher knows this for free -- it is
// the axis the DDA crossed to enter the cell.
//
// This exists for materials that genuinely DIFFER by face, not for shading.
// Grass is green on top and soil underneath; a cut trunk shows end grain on
// the top face and bark on the sides. Light direction and ambient occlusion
// are the renderer's business and are applied on top of this; baking a
// top-is-brighter bias in here would double-count them and go wrong the moment
// the sun moves.
enum FaceClass : uint8_t { kFaceTop = 0, kFaceSide = 1, kFaceBottom = 2, kFaceClassCount = 3 };

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
};

struct MaterialAppearance {
    // Base colour per face class, sRGB. Same value three times means the
    // material looks the same whichever way you hit it, which is true of most
    // of them.
    Rgb face[kFaceClassCount];

    // Per-voxel lightness jitter, in 1/255ths of full range. This is the
    // near-field dither: each voxel picks its own offset from a hash of its
    // position. High for anything granular (gravel, foliage), near zero for
    // anything that should read as one continuous surface (snow, water mark).
    uint8_t voxelJitter = 0;

    // Per-voxel hue drift along the material's own warm/cool axis, same units.
    // Kept separate from lightness because they read differently: lightness
    // variation looks like uneven light, hue variation looks like a mix of
    // stuff. Grass wants hue (yellow through blue-green); stone mostly does
    // not.
    uint8_t voxelHue = 0;

    // The slow term. Strength in 1/255ths of full range.
    uint8_t patchStrength = 0;

    // ...and its wavelength, in DECIMETRES OF WORLD, not in voxels.
    //
    // THE UNIT IS METRIC BECAUSE THE VOXEL IS NOT A FIXED SIZE. A level-0 voxel
    // is 10 cm, so dm and voxels are the same number there and every value in
    // this table carries over from when this field was `patchScaleVox` -- but a
    // coarse streaming ring renders 32 of them as one cube, and a detail-lattice
    // grid renders tufts at 5 cm. Read as voxels, one hillside's mottle would
    // have a 2 m wavelength in the near ring and a 64 m one two rings out, and
    // the two would meet at the ring boundary as a visible step in the very term
    // that exists to survive distance. Read as world, the pattern is the same
    // size everywhere and rings agree across their seam.
    //
    // The evaluator band-limits it: below two rendered cubes per wavelength the
    // effective wavelength is stretched to two, because a patch finer than the
    // cubes sampling it is not a patch, it is noise (materialcolor.h).
    uint8_t patchScaleDm = 0;

    // HOW MUCH OF THIS MATERIAL'S COLOUR THE CLIMATE OWNS, in 1/255ths.
    // 0 = the material's own colour, always, everywhere. 255 = the biome LUT's
    // answer for this column's temperature and precipitation, and this table's
    // RGB is only what it falls back to.
    //
    // WHY THIS IS APPEARANCE AND NOT RENDERER POLICY. It lived as a boolean
    // `BIOME_TINT` column in ue-project/Tools/terrain_palette.py, on the
    // argument that "who decides this voxel's colour" is UE-side policy rather
    // than a fact about the material. It is a fact about the material. Bedrock
    // is the same grey in a rainforest and in a tundra because it is rock;
    // grassland is a different green in each because grass is a plant that
    // responds to climate. That difference is what the material IS, and every
    // consumer needs the same answer for the same reason they need the same
    // colour -- asset-forge's preview included, which today shows terrain
    // materials at a saturation the game never draws them at.
    //
    // WHY IT IS A WEIGHT AND NOT A BOOLEAN. A boolean forces the choice between
    // "one flat rock tone underground" (what shipped, and what makes a cave wall
    // read as untextured cardboard) and "the hillside is the colour of its
    // material, not its climate". A weight is the third option, and it is the one
    // the colour system is built on: the climate answer and the material answer
    // are blended per material, so a cave keeps bedrock/rock/gravel/subsoil
    // apart while a hillside still reads as its biome.
    //
    // WHY THE SURFACE VALUES ARE AS HIGH AS THEY ARE (190-235 rather than, say,
    // 128). Not taste -- the classifier. On real diffusion tiles voxel-core
    // labels very nearly every outdoor surface voxel MAT_SAND (its precipitation
    // thresholds are calibrated for SyntheticTileSampler, not for WorldClim's
    // 0-12000 mm/yr quantisation; VoxelClimateProbe.h records the measurement).
    // Until that is fixed, material id carries almost no information outdoors and
    // climate carries all of it, so a low weight here would not make the world
    // more varied -- it would paint the whole landmass sand. These numbers say
    // "climate still owns the surface, but the material is allowed to show
    // through", which is the largest step that is safe to take before the
    // classifier is measured again. When it is fixed, this column is where the
    // world gets its material identity back, and lowering it is a data change
    // rather than a code one.
    //
    // ASSET MATERIALS ARE ALL 0, and that is a stronger statement than it looks.
    // A biome tint answers "what colour is the GROUND here" -- it is a property
    // of a place. Bark is attached to a place; a fish is not, and would change
    // colour as it swam across a climate boundary. A tinted MAT_PLUME_CYAN would
    // also destroy the most stylised entry in the table, which exists precisely
    // because a kingfisher is not the colour of anything around it.
    uint8_t biomeTint = 0;
};

// Indexed by Material. Every entry is written out; a material without an
// appearance is a build failure, not a black voxel someone notices in a
// screenshot three weeks later.
inline constexpr MaterialAppearance kMaterialPalette[kMaterialCount] = {
    // ---- terrain ---------------------------------------------------------
    /* MAT_AIR */ {{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, 0, 0, 0, 0, 0},
    /* MAT_BEDROCK */ {{{68, 68, 74}, {68, 68, 74}, {62, 62, 68}}, 22, 6, 30, 24, 0},
    /* MAT_ROCK */ {{{124, 120, 113}, {118, 114, 108}, {110, 107, 102}}, 34, 10, 44, 20, 0},
    /* MAT_GRAVEL */ {{{141, 134, 123}, {136, 129, 119}, {128, 122, 113}}, 52, 14, 38, 12, 0},
    /* MAT_SAND */ {{{216, 200, 154}, {210, 194, 149}, {198, 183, 141}}, 30, 12, 34, 16, 235},
    /* MAT_SUBSOIL */ {{{120, 96, 72}, {115, 92, 69}, {106, 85, 64}}, 34, 14, 40, 18, 0},
    /* MAT_TOPSOIL */ {{{92, 70, 50}, {88, 67, 48}, {80, 61, 44}}, 38, 16, 46, 16, 210},
    /* MAT_SNOW */ {{{243, 246, 251}, {238, 241, 247}, {228, 232, 240}}, 10, 4, 16, 28, 0},
    // Grass: the one where the face split earns its keep. Green on top, a
    // shorter fringe on the sides, soil underneath.
    /* MAT_GRASS */ {{{98, 142, 58}, {86, 116, 55}, {88, 67, 48}}, 46, 34, 54, 14, 215},
    /* MAT_JUNGLE_SOIL */ {{{84, 64, 44}, {80, 61, 42}, {72, 55, 38}}, 36, 16, 44, 16, 215},
    /* MAT_SAVANNA_GRASS */ {{{170, 158, 90}, {154, 142, 82}, {104, 84, 54}}, 44, 30, 52, 14, 215},
    /* MAT_PODZOL */ {{{78, 64, 54}, {74, 61, 51}, {66, 54, 46}}, 34, 14, 42, 18, 215},
    /* MAT_PERMAFROST */ {{{170, 180, 188}, {164, 174, 182}, {152, 162, 172}}, 26, 10, 32, 22, 190},
    /* MAT_MUD */ {{{74, 62, 50}, {71, 59, 48}, {64, 53, 43}}, 28, 12, 38, 18, 0},
    /* MAT_CLAY */ {{{152, 118, 94}, {146, 113, 90}, {136, 105, 84}}, 24, 12, 34, 20, 0},
    // Debug instrument. Deliberately flat and deliberately hideous: it is
    // meant to be unmistakable, and any variation would make it look like
    // content.
    /* MAT_WATERMARK */ {{{255, 0, 255}, {255, 0, 255}, {255, 0, 255}}, 0, 0, 0, 0, 0},

    // ---- assets: wood ----------------------------------------------------
    // A trunk's top and bottom faces are where it was cut, so they show
    // heartwood while the sides show bark. Costs nothing and is the sort of
    // thing that makes a felled log read as a felled log.
    /* MAT_BARK */ {{{150, 112, 74}, {86, 65, 47}, {150, 112, 74}}, 30, 14, 40, 10, 0},
    /* MAT_HEARTWOOD */ {{{158, 118, 78}, {152, 113, 75}, {150, 112, 74}}, 22, 10, 28, 8, 0},
    /* MAT_DEADWOOD */ {{{138, 126, 106}, {132, 120, 101}, {124, 113, 95}}, 34, 12, 40, 10, 0},

    // ---- assets: foliage -------------------------------------------------
    // THIS TABLE IS POSITIONAL: entry N is material N. Grouping it by type
    // reads better and is wrong -- MAT_BARK_PALE is id 23 and belongs down
    // among the leaves, not up with the other woods. Putting it there shifted
    // every id from 19 up by one and dressed every broadleaf tree in birch
    // bark. The static_assert below counts entries and cannot see it; the
    // generator checks the comment names against the enum, and that is what
    // catches it. Keep the comments accurate -- they are load-bearing.
    // High jitter on purpose. Foliage is the one class where flat colour is
    // most at risk of reading as a solid blob, and per-voxel variation is what
    // breaks a canopy into leaves. The clumps also carry real air voxels from
    // the thinning pass, so the marcher sees daylight through them.
    /* MAT_LEAF_BROADLEAF */ {{{78, 120, 54}, {72, 111, 50}, {62, 96, 44}}, 58, 40, 60, 10, 0},
    /* MAT_LEAF_NEEDLE */ {{{54, 88, 64}, {50, 82, 59}, {43, 71, 51}}, 54, 32, 56, 10, 0},
    /* MAT_LEAF_JUNGLE */ {{{58, 112, 50}, {53, 103, 46}, {45, 88, 39}}, 60, 42, 62, 12, 0},
    /* MAT_LEAF_DRY */ {{{142, 134, 82}, {134, 126, 77}, {118, 111, 68}}, 56, 36, 58, 10, 0},
    /* MAT_BARK_PALE */ {{{176, 168, 152}, {198, 194, 181}, {176, 168, 152}}, 26, 10, 44, 12, 0},
    /* MAT_LEAF_BLOSSOM */ {{{234, 190, 200}, {228, 182, 193}, {214, 168, 180}}, 40, 30, 46, 10, 0},
    /* MAT_LEAF_AUTUMN */ {{{192, 118, 50}, {184, 112, 47}, {168, 101, 42}}, 62, 48, 60, 10, 0},

    // ---- creatures: skin -------------------------------------------------
    // THE QUIET END OF EVERY RANGE, and that is the whole point of these being
    // their own class rather than reused foliage and soil colours. Terrain and
    // foliage want high per-voxel jitter and a strong slow patch term because
    // they are granular surfaces made of many small things. An animal is ONE
    // smooth creature: at foliage's jitter of 58/255 a fish's flank reads as
    // television static, and at MAT_PERMAFROST's patch scale of 22 voxels a
    // 26-voxel fish gets one slow mottle across its entire side.
    //
    // Some jitter, though, not none. MAT_SNOW is the only entry in this table
    // with jitter under 20 and it is deliberately flat; a belly drawn at that
    // setting reads as a printed decal stuck on the animal rather than as part
    // of it. 12-16 is enough to break the flatness and not enough to grain it.
    //
    // MAT_SKIN_SILVER IS THE ONE ENTRY WHOSE SIDES ARE BRIGHTER THAN ITS TOP,
    // and it is deliberate. Every other material in this table darkens
    // downward, because that is what a diffuse surface under a sky does. A
    // silver fish is not diffuse -- it is a mirror, and a vertical mirror on
    // its flank reflects the bright water beside it while its top face
    // reflects the dark bottom. Flipping it back would be "correcting" the one
    // thing that makes a shoal of herring read as silver rather than as grey.
    /* MAT_SKIN_DARK */ {{{46, 48, 56}, {44, 46, 54}, {40, 42, 50}}, 12, 4, 10, 8, 0},
    /* MAT_SKIN_PALE */ {{{232, 226, 212}, {226, 220, 206}, {216, 210, 198}}, 12, 5, 10, 8, 0},
    /* MAT_SKIN_SILVER */ {{{176, 186, 196}, {186, 196, 206}, {196, 204, 212}}, 16, 6, 12, 6, 0},
    /* MAT_SKIN_OLIVE */ {{{86, 96, 54}, {82, 92, 52}, {76, 85, 48}}, 14, 8, 12, 8, 0},
    /* MAT_SKIN_BROWN */ {{{110, 82, 52}, {105, 78, 50}, {97, 72, 46}}, 16, 8, 14, 8, 0},
    /* MAT_SKIN_ORANGE */ {{{226, 118, 34}, {218, 113, 32}, {204, 105, 30}}, 14, 8, 10, 8, 0},
    /* MAT_SKIN_YELLOW */ {{{232, 194, 54}, {224, 187, 52}, {210, 175, 48}}, 14, 8, 10, 8, 0},
    /* MAT_SKIN_RED */ {{{170, 46, 40}, {164, 44, 38}, {152, 41, 35}}, 14, 8, 12, 8, 0},
    /* MAT_SKIN_BLUE */ {{{46, 96, 168}, {44, 92, 162}, {40, 85, 150}}, 14, 6, 12, 8, 0},
    /* MAT_SKIN_GREEN */ {{{58, 148, 92}, {56, 142, 88}, {51, 131, 81}}, 14, 8, 12, 8, 0},

    // ---- creatures: plumage ----------------------------------------------
    // The same quiet end of every range the skins above sit at, and for the
    // same reason: a bird is one smooth creature, not a granular surface.
    // These eleven are what the skin set cannot do -- four neutrals, six
    // saturated hues and one keratin. See asset-forge/docs/bird-colour-
    // proposal.md.
    //
    // STILL POSITIONAL. Entry N is material N, and the static_assert below
    // counts entries and is blind to their order. Appending eleven rows in
    // any grouping other than id order would do here exactly what it did when
    // MAT_BARK_PALE was written up with the woods: shift every id above it and
    // dress the whole library in the wrong colour. The comment names are what
    // the generators check against the enum, so they are load-bearing.
    //
    // TWO ENTRIES BREAK THE DARKENS-DOWNWARD CONVENTION, both deliberately.
    //
    // MAT_PLUME_IRIDESCENT has BRIGHTER SIDES THAN ITS TOP and the highest hue
    // tilt in this block (14), for the same physical reason MAT_SKIN_SILVER
    // does. Structural colour is not a pigment: it is thin-film interference,
    // and its hue depends on the angle between the viewer, the feather and the
    // light -- Simpson & McGraw 2018 found a displaying male's position
    // relative to the sun was the strongest single predictor of what colour he
    // appeared. A flat dark green renders a starling as a dark blob. Letting
    // the sides run brighter and warmer than the top is the cheapest
    // approximation available of a colour that changes as you walk past it,
    // and it costs nothing at runtime because these four numbers already exist
    // on every material.
    //
    // MAT_PLUME_WHITE has the LOWEST JITTER IN THE WHOLE TABLE (10) and the
    // lowest patch strength (8), under even the skins. A white bird is white
    // everywhere, and per-voxel noise on it reads as dirt rather than as
    // texture.
    /* MAT_PLUME_WHITE */ {{{246, 246, 242}, {240, 240, 236}, {230, 230, 226}}, 10, 4, 8, 8, 0},
    /* MAT_PLUME_GREY */ {{{150, 156, 164}, {145, 151, 159}, {134, 140, 147}}, 14, 6, 12, 8, 0},
    /* MAT_PLUME_SLATE */ {{{78, 92, 112}, {75, 88, 107}, {69, 81, 99}}, 14, 6, 12, 8, 0},
    /* MAT_PLUME_BUFF */ {{{208, 176, 118}, {201, 170, 114}, {186, 158, 106}}, 16, 8, 14, 8, 0},
    /* MAT_PLUME_RUFOUS */ {{{180, 84, 36}, {174, 81, 35}, {161, 75, 32}}, 16, 8, 12, 8, 0},
    /* MAT_PLUME_CRIMSON */ {{{208, 40, 56}, {201, 39, 54}, {186, 36, 50}}, 14, 6, 10, 8, 0},
    /* MAT_PLUME_LIME */ {{{152, 202, 70}, {147, 195, 68}, {136, 181, 63}}, 14, 8, 10, 8, 0},
    /* MAT_PLUME_CYAN */ {{{52, 184, 206}, {50, 178, 199}, {46, 165, 185}}, 14, 6, 10, 8, 0},
    /* MAT_PLUME_LILAC */ {{{166, 132, 208}, {160, 128, 201}, {149, 118, 186}}, 14, 8, 12, 8, 0},
    /* MAT_PLUME_IRIDESCENT */ {{{28, 78, 70}, {34, 88, 80}, {24, 68, 62}}, 16, 14, 16, 6, 0},
    /* MAT_BEAK_HORN */ {{{96, 88, 76}, {93, 85, 74}, {86, 79, 68}}, 12, 6, 10, 6, 0},
};

static_assert(sizeof(kMaterialPalette) / sizeof(kMaterialPalette[0]) == kMaterialCount,
              "kMaterialPalette must have one entry per vxc::Material");

// The count assert above is blind to CONTENT, and content is where this table
// has actually gone wrong. These three say what the rows have to mean.
//
// Each is a defect that has either happened here or is one careless append
// away: a new plumage colour pasted from a terrain row and left carrying its
// biomeTint (birds would change colour in mid-air); a debug instrument given
// variation and stopping being unmistakable; a patch strength authored with no
// wavelength to spend it on, which is not subtle -- it is silently zero, the
// far-field half of ADR-0008 invariant 3 gone with nothing to notice.
constexpr bool paletteAssetRowsAreUntinted() {
    for (int m = kFirstAssetMaterial; m < kMaterialCount; ++m) {
        if (kMaterialPalette[m].biomeTint != 0) return false;
    }
    return true;
}
static_assert(paletteAssetRowsAreUntinted(),
              "an ASSET material has a non-zero biomeTint: a tree, fish or bird "
              "would take the colour of the ground it is standing on and change "
              "as it crossed a climate boundary");

static_assert(kMaterialPalette[MAT_WATERMARK].voxelJitter == 0 &&
                  kMaterialPalette[MAT_WATERMARK].voxelHue == 0 &&
                  kMaterialPalette[MAT_WATERMARK].patchStrength == 0 &&
                  kMaterialPalette[MAT_WATERMARK].biomeTint == 0,
              "MAT_WATERMARK is an instrument, not content: any variation makes "
              "it read as something the world produced");

constexpr bool palettePatchTermsAreSpendable() {
    for (int m = 0; m < kMaterialCount; ++m) {
        if (kMaterialPalette[m].patchStrength != 0 && kMaterialPalette[m].patchScaleDm == 0) {
            return false;
        }
    }
    return true;
}
static_assert(palettePatchTermsAreSpendable(),
              "a material has patchStrength with patchScaleDm 0: the far-field "
              "half of the variation is switched off and the strength reads as "
              "though it were doing something");

// Face class from the axis a ray crossed and which way its normal points.
// `axis` is 0=x, 1=y, 2=z, matching the mesher's quad encoding.
constexpr FaceClass faceClassOf(int axis, bool positive) {
    if (axis != 2) return kFaceSide;
    return positive ? kFaceTop : kFaceBottom;
}

} // namespace vxc
