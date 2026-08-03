// vxc_caveprobe -- the underground system's first instrument
// (docs/underground-system-plan.md W1).
//
// WHY THIS EXISTS. Until now the cave system had NO measurement at all. The
// only way to look at it was to launch the editor, teleport a pawn underground
// and take a screenshot -- which shows one 3 m-wide room at a time, needs the
// box's one editor slot, and cannot answer any of the questions the redesign
// is actually about ("is the network still connected?", "did the direction
// lock flatten?", "did entrances get rarer?", "is there anywhere for a mob to
// stand?"). Every claim in the plan's diagnosis section was read out of the
// source rather than measured, and every claim in its work list has to be
// verifiable after a constant changes. That is what this tool is for.
//
// It renders and counts the SHIPPING carve predicates -- Amplifier::materialAt
// via caves.h's caveCarveAt and caverns.h's cavernCarveAt -- so it can never
// be measuring a different underground from the one that gets built.
//
// ---------------------------------------------------------------------------
// HOW A CARVED VOXEL IS ATTRIBUTED TO A FAMILY (and why this is exact)
// ---------------------------------------------------------------------------
// caveColumnFor reduces tunnels AND crevices into one flat `segs[]` array with
// no provenance tag, so a carved voxel carries no family label. The obvious
// fix -- re-deriving the geometry here -- would be a second implementation of
// worldgen, which is exactly the drift-prone thing this codebase refuses to
// do. So the attribution is done by DIFFERENCING the shipping predicate over
// controlled variants of the lattice block instead:
//
//   L      the real CaveLattice for the column's cell        -> tunnels+crevices+shaft
//   Lns    L with shaftNodeSlot = -1                          -> tunnels+crevices
//   Lt     Lns with every edge's crevHash forced gate-CLOSED  -> tunnels only
//
// caveColumnFromLattice is a pure function of the block, and the tunnel
// emission does not read crevHash at all, so cT's segs are bit-identical to
// the tunnel subset of cFull's. Hence:
//
//   tunnel  = caveCarveAt(cT , ...)
//   crevice = caveCarveAt(cTC, ...) && !tunnel
//   shaft   = the shaft branch of caveCarveAt, evaluated directly
//   cavern  = cavernCarveAt(col.cavern, ...)
//
// and the attribution is CHECKED against ground truth every voxel:
// caveCarveAt(cFull) must equal (shaft || tunnel || crevice), and cFull must
// equal the ColumnSample's own memoised `cave`. Both mismatch counters are
// printed; a non-zero one means this tool is lying and says so, rather than
// quietly reporting a plausible number.
//
// ---------------------------------------------------------------------------
// WHAT IT MEASURES
// ---------------------------------------------------------------------------
//  * per-family volume: carved voxels as a fraction of the solid subsurface
//    band (the plan's global budget line; status.md M4 records 1.18%)
//  * per-family column occupancy: is there anything under your feet at all
//  * ENTRANCES, the plan's headline: perforation (surface columns open to the
//    sky), split by family, plus sideways DAYLIGHTING MOUTHS -- carved voxels
//    face-adjacent to open air on a slope, which is the only entrance kind
//    relief gets for free today. Reported per km^2 so a "one entrance per
//    205 m square" claim is checkable, and separately for FLAT vs STEEP
//    ground, because the plan's flat-terrain decision lives or dies on it.
//  * FLOOR AREA and HEADROOM per family (plan 5.6): where can a mob stand?
//    A floor is a solid voxel with `--headroom` metres of continuous air above
//    it whose first air voxel is cave air. This is the future spawn system's
//    surface budget, measured instead of guessed.
//  * DEPTH HISTOGRAM in 5 m bands -- the readout for 5.7's storeys, and the
//    thing that shows today's underground stops at ~37 m with half the rock
//    column empty.
//  * DIRECTION HISTOGRAM over lattice edge headings, with a "cardinal lock"
//    index: the fraction of tunnel axis length within 15 degrees of due E or
//    due N. Tell #2 in the plan's diagnosis, and W2's acceptance number.
//  * CONNECTIVITY: flood-fill component count and largest-component share of
//    a decimated sub-box, the same statistic test_caves.cpp uses, so any
//    entrance or passage rework can be shown not to have shattered the graph.
//  * W4 CHAMBER SHAPE (v26), as an A/B against a CONTROL rather than as a
//    number: plan anisotropy, room-to-room stack drift, enclosed rock islands
//    (which is what a pillar is, measured topologically), half-turn defect and
//    within-room floor step, each reduced twice on the same columns -- once
//    from the shipping predicate and once with cave_families.h's
//    `cavernSiteWithoutChamberShape`, i.e. v25's coaxial round pillar-free
//    flat-floored chamber out of the v26 world. The control is the half that
//    matters: a one-arm asymmetry number is satisfied by roof, bedrock and
//    region clipping, all of which were already true at v25, which is the same
//    contamination that made W3's first mouth metric wrong.
//  * PER-CAVERN-SITE CHAMBER GEOMETRY, and a SEARCHED vxc_gpu fixture origin
//    for each -- because a third of sites hash to no pillars at all and both a
//    capture and a GPU fixture placed on one of those would be testing the
//    pillar term by not containing it.
//
// ---------------------------------------------------------------------------
// IMAGES (PNG, written directly -- no venv, no PIL, no editor)
// ---------------------------------------------------------------------------
// PNG rather than the repo's usual headerless-raw-plus-python-script idiom on
// purpose: the owner judges pictures, tools/imgdiff.py already A/Bs PNG pairs,
// and an instrument that needs a python environment to be looked at will not
// be looked at. The encoder below is stored-deflate (no compression library).
//
//   <prefix>-plan-class.png  plan view, colour = family of the SHALLOWEST cave
//                            void under each column, over a hillshade-free
//                            grey elevation base. This is the picture that
//                            shows the lattice: backbone rows/columns run as
//                            unbroken straight corridors, and shafts sit on
//                            their crossings.
//   <prefix>-plan-depth.png  plan view, colour ramp = depth below surface of
//                            that shallowest void (blue shallow -> red deep).
//   <prefix>-plan-thick.png  plan view, brightness = total carved thickness in
//                            the column (the "how much void is under here"
//                            view; caverns light up, crevices barely register).
//   <prefix>-sect-x.png      vertical cross-section along +x through the
//   <prefix>-sect-y.png      region centre (and along +y), full voxel z
//                            resolution, coloured by family against the real
//                            stratigraphy. Storeys, roof cover and the bedrock
//                            floor are all directly readable.
//
// Usage:
//   vxc_caveprobe <tiledir|--synthetic> <seed> [options]
//     --origin XM YM     region centre in world METRES (default 0,0)
//     --span M           region edge in metres (default 409.6 -- 16 lattice
//                        cells, 2 cavern cells: big enough to contain several
//                        entrances and at least one cavern site)
//     --px N             image edge in pixels (default 512)
//     --max-depth M      deepest metres below surface to scan (default 45;
//                        raise to ~180 when 5.7's storeys land)
//     --headroom M       walkable headroom for the floor-area stat (default 1.8)
//     --steep MM         slope threshold in mm of relief per metre that counts
//                        a column as STEEP for the entrance split (default 300)
//     --scale N          tile raster scale: 1 = the 30 m COARSE tier (default),
//                        16 = the FINE tier, which is what the editor renders
//                        and therefore the only tier a capture site may be
//                        chosen on -- the amplifier branches on the pitch
//     --coarse-dir D     s1 tile dir supplying CLIMATE behind a --scale 16 run
//                        (the fine tier carries elevation and flow, no climate)
//     --entrance-off     THE CONTROL PANEL: reduce the SAME world with the v25
//                        entrance cavity switched off, leaving only the v24
//                        bore. For A/B pairs that have to show the cavity is
//                        the thing that changed (cave_families.h explains why
//                        this is done by differencing rather than by building
//                        the v24 tree)
//     --cavern-ab-off    skip the W4 chamber-shape A/B. It is ON by default and
//                        should stay on: it is what turns "plan-view symmetry
//                        is broken" from an assertion into a measurement, and
//                        it costs only the columns a cavern actually reaches
//                        (~0.5% of them). See the W4 report block for what the
//                        control arm is and why one arm alone proves nothing.
//     --connect N        connectivity flood-fill sample box edge (default 192
//                        samples at 0.4 m = 76.8 m); 0 disables it
//     --out PREFIX       output path prefix (default ./caveprobe)
//     --no-images        stats only
//     --section-only     images only, skip the region census
//
// Compare vxc_riverprobe / vxc_terrainprobe / vxc_climateprobe: same shape of
// tool, same reason -- a hand-derived number cannot be re-run after a constant
// changes.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "voxelcore/amplifier.h"
#include "voxelcore/cave_families.h"
#include "voxelcore/caverns.h"
#include "voxelcore/caves.h"
#include "voxelcore/connectivity.h"
#include "voxelcore/tilestore.h"

using namespace vxc;

namespace {

// --- families ---------------------------------------------------------------
// The attribution itself lives in voxelcore/cave_families.h so that this tool
// and test_caves.cpp cannot drift apart about what a "crevice voxel" is; see
// that header for why differencing the shipping predicate is the only honest
// way to label a carved voxel. Everything below is presentation.
const char* kFamName[kCaveFamilyCount] = {"tunnel", "crevice", "shaft", "cavern"};

struct Rgb {
    uint8_t r, g, b;
};
// Deliberately far apart in hue AND in luminance, so the plan views survive
// being looked at on a bad monitor or printed to a grey PDF.
const Rgb kFamColour[kCaveFamilyCount] = {
    {90, 150, 255},  // tunnel  -- blue
    {80, 230, 210},  // crevice -- cyan
    {255, 80, 60},   // shaft   -- red
    {255, 175, 40},  // cavern  -- amber
};

// --- PNG (stored deflate; no zlib) ------------------------------------------

uint32_t crc32Of(const uint8_t* p, size_t n, uint32_t crc) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

void pushBe32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void pushChunk(std::vector<uint8_t>& out, const char type[5], const std::vector<uint8_t>& data) {
    pushBe32(out, static_cast<uint32_t>(data.size()));
    const size_t crcStart = out.size();
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(type[i]));
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t crc = crc32Of(out.data() + crcStart, out.size() - crcStart, 0);
    pushBe32(out, crc);
}

// RGB8, no interlace, one stored-deflate stream. Uncompressed PNGs of a
// 512x512 plan view are ~800 KB, which is not worth a compression dependency.
bool writePng(const std::string& path, const std::vector<uint8_t>& rgb, int w, int h) {
    if (rgb.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 3u) return false;

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(h) * (1u + 3u * static_cast<size_t>(w)));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0); // filter type 0 (None)
        const uint8_t* row = rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3u;
        raw.insert(raw.end(), row, row + static_cast<size_t>(w) * 3u);
    }

    // zlib stream, stored blocks.
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t off = 0;
    for (;;) {
        const size_t n = std::min<size_t>(65535u, raw.size() - off);
        const bool last = (off + n >= raw.size());
        z.push_back(last ? 1u : 0u);
        z.push_back(static_cast<uint8_t>(n & 0xFFu));
        z.push_back(static_cast<uint8_t>((n >> 8) & 0xFFu));
        z.push_back(static_cast<uint8_t>((~n) & 0xFFu));
        z.push_back(static_cast<uint8_t>(((~n) >> 8) & 0xFFu));
        z.insert(z.end(), raw.begin() + static_cast<ptrdiff_t>(off),
                 raw.begin() + static_cast<ptrdiff_t>(off + n));
        off += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    pushBe32(z, (b << 16) | a);

    std::vector<uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    pushBe32(ihdr, static_cast<uint32_t>(w));
    pushBe32(ihdr, static_cast<uint32_t>(h));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(2); // colour type 2 = truecolour RGB
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    pushChunk(out, "IHDR", ihdr);
    pushChunk(out, "IDAT", z);
    pushChunk(out, "IEND", {});

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t wrote = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return wrote == out.size();
}

// --- tile loading (same as vxc_riverprobe) -----------------------------------

std::optional<std::vector<uint8_t>> probeReadFileBytes(const std::filesystem::path& p) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec) return std::nullopt;
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return std::nullopt;
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) return std::nullopt;
    return buf;
}

// --- small helpers ----------------------------------------------------------

Rgb rampBlueRed(double t) {
    t = std::min(1.0, std::max(0.0, t));
    // blue -> cyan -> yellow -> red, luminance rising then falling: readable
    // both in colour and in greyscale.
    const double r = std::min(1.0, std::max(0.0, 1.6 * t - 0.4));
    const double g = std::min(1.0, std::max(0.0, 1.0 - 2.2 * std::abs(t - 0.45)));
    const double b = std::min(1.0, std::max(0.0, 1.0 - 1.9 * t));
    return Rgb{static_cast<uint8_t>(255.0 * r), static_cast<uint8_t>(255.0 * g),
               static_cast<uint8_t>(255.0 * b)};
}

void putPx(std::vector<uint8_t>& img, int w, int x, int y, Rgb c) {
    const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3u;
    img[o] = c.r;
    img[o + 1] = c.g;
    img[o + 2] = c.b;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: vxc_caveprobe <tiledir|--synthetic> <seed> [--origin XM YM]\n"
                     "       [--span M] [--px N] [--max-depth M] [--headroom M] [--steep MM]\n"
                     "       [--connect N] [--scale N] [--coarse-dir D] [--entrance-off] [--out PREFIX]\n"
                     "       [--no-images]\n"
                     "       [--section-only]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t seed = std::strtoull(argv[2], nullptr, 10);

    double originXM = 0, originYM = 0, spanM = 409.6, headroomM = 1.8, mouthReachM = 6.0;
    // 512 samples at 0.4 m = a 204.8 m box. Deliberately >= the ~205 m mean
    // spacing of OPEN sinkhole nodes (102.4 m candidates, 1-in-4 gate): a
    // smaller box routinely contains no entrance at all and then reports "0%
    // reachable from the sky", which is a property of the box and not of the
    // world. It did exactly that twice during this tool's bring-up, at 76.8 m
    // and again at 128 m -- so the box is sized to the feature, and the count
    // of open shaft nodes actually inside it is printed next to the result.
    int64_t maxDepthM = 45, steepMmPerM = 300, connectN = 512;
    int px = 512, tileScale = 1;
    bool entranceCavityOff = false;
    bool cavernAbOff = false;
    std::string coarseDir;
    bool images = true, sectionOnly = false, connectAtSet = false;
    double connectAtXM = 0, connectAtYM = 0;
    std::string outPrefix = "caveprobe";
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--origin" && i + 2 < argc) {
            originXM = std::strtod(argv[i + 1], nullptr);
            originYM = std::strtod(argv[i + 2], nullptr);
            i += 2;
        } else if (a == "--span" && i + 1 < argc) {
            spanM = std::strtod(argv[++i], nullptr);
        } else if (a == "--scale" && i + 1 < argc) {
            tileScale = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        } else if (a == "--coarse-dir" && i + 1 < argc) {
            coarseDir = argv[++i];
        } else if (a == "--px" && i + 1 < argc) {
            px = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        } else if (a == "--max-depth" && i + 1 < argc) {
            maxDepthM = std::strtoll(argv[++i], nullptr, 10);
        } else if (a == "--headroom" && i + 1 < argc) {
            headroomM = std::strtod(argv[++i], nullptr);
        } else if (a == "--steep" && i + 1 < argc) {
            steepMmPerM = std::strtoll(argv[++i], nullptr, 10);
        } else if (a == "--mouth-reach" && i + 1 < argc) {
            mouthReachM = std::strtod(argv[++i], nullptr);
        } else if (a == "--connect" && i + 1 < argc) {
            connectN = std::strtoll(argv[++i], nullptr, 10);
        } else if (a == "--connect-at" && i + 2 < argc) {
            connectAtXM = std::strtod(argv[i + 1], nullptr);
            connectAtYM = std::strtod(argv[i + 2], nullptr);
            connectAtSet = true;
            i += 2;
        } else if (a == "--out" && i + 1 < argc) {
            outPrefix = argv[++i];
        } else if (a == "--entrance-off") {
            entranceCavityOff = true;
        } else if (a == "--cavern-ab-off") {
            cavernAbOff = true;
        } else if (a == "--no-images") {
            images = false;
        } else if (a == "--section-only") {
            sectionOnly = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }
    if (px < 16 || px > 4096) {
        std::fprintf(stderr, "--px must be in [16, 4096]\n");
        return 2;
    }

    // --- world ---------------------------------------------------------------
    // TIER, AND IT IS NOT COSMETIC -- this tool measured the wrong world until
    // W3's capture pass. The amplifier BRANCHES on the raster pitch
    // (vxc::isFineTier): a different detail octave table, a different band
    // split, a different channel-to-lattice mapping. Caves live in DEPTH SPACE
    // under that surface, so a change of tier moves every cave in the world --
    // docs/underground-system-plan.md section 2.5 says exactly this about v23.
    // And the editor renders the FINE tier by default (ue-project/Config/
    // DefaultGame.ini DefaultFineTileDir). So every entrance site this tool has
    // ever named for a capture was named on a surface the game does not draw.
    //
    // The two tiers are also two different FILE FORMATS and two different
    // samplers -- v1 flat tiles through TileGridSampler at s1, v2 block-coded
    // tiles through FineTileSampler at s16 -- which is why pointing --scale 16
    // at the s16 directory rejected all 17 tiles rather than loading them: the
    // v1 parser was reading a v2 header. Both paths are wired here now, and the
    // fine path takes the coarse sampler as its CLIMATE source exactly as the
    // runtime does (the fine tier carries elevation and flow, never climate).
    SyntheticTileSampler synth(seed);
    TileGridSampler grid(seed, 1);
    FineTileSampler fine(seed, &grid);
    ITileSampler* tiles = &synth;
    const bool wantFine = tileScale != 1;
    if (dir != "--synthetic") {
        int loaded = 0, rejected = 0;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".vxtl") continue;
            std::optional<std::vector<uint8_t>> bytes = probeReadFileBytes(e.path());
            if (!bytes) { ++rejected; continue; }
            if (wantFine) {
                FineError err = FineError::kNone;
                if (!fine.loadTile(*bytes, &err)) {
                    std::fprintf(stderr, "  fine tile %s rejected (FineError %d)\n",
                                 e.path().filename().string().c_str(), static_cast<int>(err));
                    ++rejected;
                    continue;
                }
            } else {
                std::optional<TileData> parsed = TileData::parse(bytes->data(), bytes->size());
                if (!parsed) { ++rejected; continue; }
                if (!grid.loadTile(std::move(*parsed))) { ++rejected; continue; }
            }
            ++loaded;
        }
        // The fine tier carries no climate, so a fine run ALSO wants the coarse
        // set behind it -- without it every column gets the bland default and
        // the biome, topsoil and surface material are all wrong. Not fatal
        // (caves read none of them), but it is stated rather than silent.
        int coarseLoaded = 0;
        if (wantFine && !coarseDir.empty()) {
            for (auto& e : std::filesystem::directory_iterator(coarseDir, ec)) {
                if (e.path().extension() != ".vxtl") continue;
                std::optional<std::vector<uint8_t>> bytes = probeReadFileBytes(e.path());
                if (!bytes) continue;
                std::optional<TileData> parsed = TileData::parse(bytes->data(), bytes->size());
                if (!parsed) continue;
                if (grid.loadTile(std::move(*parsed))) ++coarseLoaded;
            }
        }
        std::printf("tiles loaded=%d rejected=%d tier=%s pixelSizeMm=%d coarse-climate tiles=%d%s\n",
                    loaded, rejected, wantFine ? "FINE (s16)" : "coarse (s1)",
                    wantFine ? fine.pixelSizeMm() : grid.pixelSizeMm(), coarseLoaded,
                    (wantFine && coarseLoaded == 0)
                        ? "   <-- NO CLIMATE: biome/topsoil/surface material are defaults"
                        : "");
        if (loaded == 0) {
            std::fprintf(stderr, "no tiles loaded from %s\n", dir.c_str());
            return 1;
        }
        tiles = wantFine ? static_cast<ITileSampler*>(&fine) : static_cast<ITileSampler*>(&grid);
    }
    Amplifier amp(seed, *tiles);

    // Region, in voxels. The sample stride is what one image pixel covers; the
    // census is over the SAME samples the images are drawn from, so a picture
    // and a number can never disagree about the region.
    const int64_t cx = static_cast<int64_t>(originXM * 1000.0 / kVoxelSizeMm);
    const int64_t cy = static_cast<int64_t>(originYM * 1000.0 / kVoxelSizeMm);
    const int64_t spanVx = static_cast<int64_t>(spanM * 1000.0 / kVoxelSizeMm);
    const int64_t stride = std::max<int64_t>(1, spanVx / px);
    const int64_t x0 = cx - spanVx / 2, y0 = cy - spanVx / 2;
    const int64_t connectAtVx = static_cast<int64_t>(connectAtXM * 1000.0 / kVoxelSizeMm);
    const int64_t connectAtVy = static_cast<int64_t>(connectAtYM * 1000.0 / kVoxelSizeMm);
    const int64_t depthVox = maxDepthM * 1000 / kVoxelSizeMm;
    const int64_t headVox = std::max<int64_t>(1, static_cast<int64_t>(headroomM * 1000.0) / kVoxelSizeMm);
    const int64_t mouthReachSamples = std::max<int64_t>(
        1, static_cast<int64_t>(mouthReachM * 1000.0) / (stride * kVoxelSizeMm));
    // Each sample stands for a stride x stride block of voxel columns.
    const double sampleAreaM2 = static_cast<double>(stride * stride * kVoxelSizeMm * kVoxelSizeMm) / 1e6;
    const double regionKm2 = (static_cast<double>(px) * static_cast<double>(px) * sampleAreaM2) / 1e6;

    std::printf("\nvxc_caveprobe  seed=%llu  kWorldGenVersion=%u%s\n", (unsigned long long)seed,
                kWorldGenVersion,
                entranceCavityOff ? "  [--entrance-off: the v24 bore, cavity suppressed]" : "");
    std::printf("region centre=(%.1f, %.1f) m  span=%.1f m  %dx%d samples  stride=%.1f m "
                "(%.4f km^2)\n",
                originXM, originYM, spanM, px, px,
                static_cast<double>(stride * kVoxelSizeMm) / 1000.0, regionKm2);
    std::printf("scan band: surface .. %lld m down (%lld voxels)   headroom=%.1f m   "
                "steep>=%lld mm/m\n",
                (long long)maxDepthM, (long long)depthVox, headroomM, (long long)steepMmPerM);

    // --- region census + plan views -----------------------------------------
    std::vector<uint8_t> imgClass, imgDepth, imgThick;
    if (images) {
        imgClass.assign(static_cast<size_t>(px) * px * 3u, 0);
        imgDepth.assign(static_cast<size_t>(px) * px * 3u, 0);
        imgThick.assign(static_cast<size_t>(px) * px * 3u, 0);
    }

    // counters
    int64_t colsSampled = 0, colsSteep = 0;
    int64_t solidBandVox = 0, carvedVox = 0;
    int64_t famVox[kCaveFamilyCount] = {0, 0, 0, 0};
    int64_t famCols[kCaveFamilyCount] = {0, 0, 0, 0};
    int64_t colsWithAny = 0;
    // entrances
    int64_t perfCols[kCaveFamilyCount] = {0, 0, 0, 0}; // surface voxel of the column is carved
    int64_t perfColsAny = 0, perfColsFlat = 0, perfColsSteep = 0;
    int64_t mouthCols = 0, mouthColsFlat = 0, mouthColsSteep = 0; // sideways daylighting
    // ENTRANCE SHAPE (v25, plan W3). The three counters above are terrain-side
    // proxies -- "is there a void near a free face" -- and they were the only
    // entrance geometry this tool could see while an entrance was a bore. The
    // entrance is now a cavity that reports its own shape per column, so ask it
    // directly instead: how much ground lies over one, and how much of that is
    // ROOFED (a doline's overhanging lip, a mouth's ceiling) rather than open to
    // the sky. That split is a region-wide statistic and it is honest; what it
    // cannot do is separate a mouth from a doline -- see the note at the print.
    int64_t entCols = 0, entOpenCols = 0, entRoofedCols = 0;
    int64_t entRoofedFlat = 0, entRoofedSteep = 0;
    // floors
    int64_t floorCols[kCaveFamilyCount] = {0, 0, 0, 0};
    int64_t floorSpots[kCaveFamilyCount] = {0, 0, 0, 0};
    int64_t floorColsAny = 0;
    int64_t headroomSumMm[kCaveFamilyCount] = {0, 0, 0, 0};
    // depth histogram, 5 m bands
    const int kDepthBands = static_cast<int>((maxDepthM + 4) / 5) + 1;
    std::vector<int64_t> depthHist(static_cast<size_t>(kDepthBands) * kCaveFamilyCount, 0);
    // W4 (v26) PLAN-VIEW SYMMETRY, MEASURED AS A DIFFERENCE.
    //
    // The plan's Verify line for W4 is "plan-view symmetry visibly broken", and
    // a single-arm asymmetry number cannot carry that: a v25 cavern footprint
    // already reads asymmetric wherever the roof clamp truncates it against a
    // slope, the bedrock clamp cuts its bottom off, or this region clips it at
    // the edge. Reporting one arm would be the W3 mouth-metric mistake again --
    // a statistic satisfied by geometry other than the one under test.
    //
    // So both arms are reduced here, on the same columns, same seed, same
    // terrain, same shipping predicate, differing only by cave_families.h's
    // `cavernSiteWithoutChamberShape` (v25's coaxial, round, pillar-free,
    // flat-floored chamber out of the v26 world). The CONTROL is what proves
    // each statistic can report "symmetric"; the difference is the finding.
    std::vector<uint8_t> cavMaskReal(static_cast<size_t>(px) * px, 0);
    std::vector<uint8_t> cavMaskPlain(static_cast<size_t>(px) * px, 0);
    // The column's carving segments, kept so the breakdown statistic can
    // compare floors PER ROOM. Two adjacent columns are in the same room
    // exactly when they carry a segment with the same zCenterMm (room centres
    // are distinct and at most one site reaches any column), so no site lookup
    // is needed. Asked per room because a column's LOWEST floor is dominated
    // by the step where one room's footprint ends and the next room's deeper
    // floor takes over -- chain geometry, not rubble.
    std::vector<int32_t> cavSegZ(static_cast<size_t>(px) * px * kMaxCavernSegs, 0);
    std::vector<int32_t> cavSegFloor(static_cast<size_t>(px) * px * kMaxCavernSegs, 0);
    std::vector<uint8_t> cavSegN(static_cast<size_t>(px) * px, 0);
    std::vector<int32_t> cavSegZP(static_cast<size_t>(px) * px * kMaxCavernSegs, 0);
    std::vector<int32_t> cavSegFloorP(static_cast<size_t>(px) * px * kMaxCavernSegs, 0);
    std::vector<uint8_t> cavSegNP(static_cast<size_t>(px) * px, 0);
    int64_t cavPlainCols = 0, cavRealCols = 0, pillarCols = 0;

    // self-checks
    int64_t attrMismatch = 0, memoMismatch = 0;
    int64_t minRoofMm = 1ll << 40, maxCarveDepthMm = 0;

    std::vector<uint32_t> fam(static_cast<size_t>(depthVox) + 1, 0);
    std::vector<uint8_t> airCol(static_cast<size_t>(depthVox) + 1, 0);

    if (!sectionOnly) {
        for (int iy = 0; iy < px; ++iy) {
            for (int ix = 0; ix < px; ++ix) {
                const int64_t vx = x0 + static_cast<int64_t>(ix) * stride;
                const int64_t vy = y0 + static_cast<int64_t>(iy) * stride;
                const ColumnSample col = amp.column(vx, vy);
                ++colsSampled;

                // Local relief, from the four stride-neighbours: mm of surface
                // change per metre. This is the same "is this a hillside"
                // question the plan's entrance portfolio splits on.
                const int32_t sE = amp.surfaceMm(vx + stride, vy);
                const int32_t sW = amp.surfaceMm(vx - stride, vy);
                const int32_t sN = amp.surfaceMm(vx, vy + stride);
                const int32_t sS = amp.surfaceMm(vx, vy - stride);
                // mm of rise per metre of run. Kept in mm throughout: the run
                // is 2*stride voxels = 1.6 m at the default stride, and doing
                // this as an integer metre count rounded it to 1 m and
                // overstated every slope by 60%.
                const int64_t runMm = 2 * stride * kVoxelSizeMm;
                const int64_t slopeMmPerM =
                    std::max<int64_t>(std::abs(static_cast<int64_t>(sE) - sW),
                                      std::abs(static_cast<int64_t>(sN) - sS)) *
                    1000 / runMm;
                const bool steep = slopeMmPerM >= steepMmPerM;
                if (steep) ++colsSteep;

                const CaveColumnVariants cv = caveColumnVariantsFor(seed, vx, vy, col.surfaceMm, amp.surfaceAtFn(), entranceCavityOff);
                if (cv.full.count != col.cave.count ||
                    cv.full.shaftMarginSq != col.cave.shaftMarginSq)
                    ++memoMismatch;

                const int64_t topVz = floorDiv(col.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm);
                uint32_t colMask = 0;
                int32_t topVoidIdx = -1;
                int64_t thickVox = 0;

                for (int64_t k = 0; k <= depthVox; ++k) {
                    const int64_t vz = topVz - k;
                    const bool solid = Amplifier::stratigraphyAt(col, vz) != MAT_AIR;
                    if (!solid) {
                        fam[static_cast<size_t>(k)] = 0;
                        airCol[static_cast<size_t>(k)] = 1;
                        continue;
                    }
                    ++solidBandVox;
                    bool truth = false;
                    const uint32_t m = caveFamilyMaskAt(cv, col.cavern, col.surfaceMm,
                                                    col.bedrockDepthMm, vz, truth);
                    if (caveFamilyMaskIsCavePass(m) != truth) ++attrMismatch;

                    fam[static_cast<size_t>(k)] = m;
                    airCol[static_cast<size_t>(k)] = m ? 1 : 0;
                    if (!m) continue;

                    ++carvedVox;
                    ++thickVox;
                    colMask |= m;
                    if (topVoidIdx < 0) topVoidIdx = static_cast<int32_t>(k);
                    const int64_t depthMm =
                        static_cast<int64_t>(col.surfaceMm) - (vz * kVoxelSizeMm + kVoxelSizeMm / 2);
                    if (depthMm > maxCarveDepthMm) maxCarveDepthMm = depthMm;
                    // Roof cover, excluding the shaft (the one designed breach).
                    if (!(m & (1u << CAVE_FAM_SHAFT)) && depthMm < minRoofMm) minRoofMm = depthMm;
                    const int band = std::min(kDepthBands - 1,
                                              static_cast<int>(depthMm / 5000));
                    for (uint32_t f = 0; f < kCaveFamilyCount; ++f)
                        if (m & (1u << f)) {
                            ++famVox[f];
                            ++depthHist[static_cast<size_t>(band) * kCaveFamilyCount + f];
                        }
                }

                if (colMask) ++colsWithAny;
                for (uint32_t f = 0; f < kCaveFamilyCount; ++f)
                    if (colMask & (1u << f)) ++famCols[f];

                // --- W4 A/B: this column's cavern plan cell, both arms ------
                //
                // A column is "in the plan mask" if any ROOM CENTRE voxel
                // carves there. That is exact rather than a sample: marginSq>0
                // already means dz==0 satisfies the ellipsoid test and a room
                // centre is always above its own floor, so this asks the
                // shipping cavernCarveAt instead of re-deriving anything. It
                // is also what makes a pillar visible -- a pillar column emits
                // no segments at all.
                if (!cavernAbOff) {
                    const size_t cell = static_cast<size_t>(iy) * px + ix;
                    const int64_t si = floorDiv(vx * kVoxelSizeMm, kCavernCoarseMm);
                    const int64_t sj = floorDiv(vy * kVoxelSizeMm, kCavernCoarseMm);
                    const CavernCandidates cands = cavernCandidatesFor(seed, si, sj);
                    const CavernColumn plain = col.surfaceMm < kCaveMinSurfaceMm
                                                   ? CavernColumn{}
                                                   : cavernColumnWithoutChamberShape(
                                                         seed, cands, vx, vy, amp.surfaceAtFn());
                    for (int32_t arm = 0; arm < 2; ++arm) {
                        const CavernColumn& cc = arm ? plain : col.cavern;
                        std::vector<uint8_t>& mk = arm ? cavMaskPlain : cavMaskReal;
                        std::vector<int32_t>& sz = arm ? cavSegZP : cavSegZ;
                        std::vector<int32_t>& sf = arm ? cavSegFloorP : cavSegFloor;
                        std::vector<uint8_t>& sn = arm ? cavSegNP : cavSegN;
                        for (int32_t sg = 0; sg < cc.count; ++sg) {
                            const int64_t cvz =
                                floorDiv(static_cast<int64_t>(cc.segs[sg].zCenterMm) -
                                             kVoxelSizeMm / 2,
                                         static_cast<int64_t>(kVoxelSizeMm));
                            if (!cavernCarveAt(cc, col.surfaceMm, col.bedrockDepthMm, cvz))
                                continue;
                            mk[cell] = 1;
                            const size_t si2 = cell * kMaxCavernSegs + sn[cell];
                            sz[si2] = cc.segs[sg].zCenterMm;
                            sf[si2] = cc.segs[sg].zFloorMm;
                            ++sn[cell];
                        }
                    }
                    if (cavMaskReal[cell]) ++cavRealCols;
                    if (cavMaskPlain[cell]) ++cavPlainCols;
                    // A PILLAR COLUMN, measured rather than asked of the pillar
                    // field: the control arm has chamber here and the shipping
                    // arm has solid rock. (It also counts columns the offsets
                    // moved the room away from, which is why this is reported
                    // as a bound and the topological hole count below is the
                    // statistic that isolates pillars.)
                    if (cavMaskPlain[cell] && !cavMaskReal[cell]) ++pillarCols;
                }

                // Entrance shape, straight off the reduction.
                if (col.cave.shaftMarginSq > 0) {
                    ++entCols;
                    if (col.cave.shaftDepthMinMm > 0) {
                        ++entRoofedCols;
                        if (steep) ++entRoofedSteep; else ++entRoofedFlat;
                    } else {
                        ++entOpenCols;
                    }
                }

                // Perforation: the column's own top solid voxel is carved, i.e.
                // you would fall in walking over it. This is the entrance-rate
                // guarantee, measured.
                if (fam[0]) {
                    ++perfColsAny;
                    if (steep) ++perfColsSteep; else ++perfColsFlat;
                    for (uint32_t f = 0; f < kCaveFamilyCount; ++f)
                        if (fam[0] & (1u << f)) ++perfCols[f];
                }

                // Sideways daylighting mouth. caves.h:64-68 claims tunnels
                // "daylight sideways on steep slopes for free", and that is the
                // only entrance kind relief gets for nothing -- so W3's
                // mountainside-mouth budget has to be measured against it.
                //
                // Stated as a REACH, not as an adjacency, and the reason is
                // geometric: a tunnel voxel is >= 6 m below its own column's
                // surface, so for the immediate neighbour 0.8 m away to be
                // below it the ground would have to fall 6 m in 0.8 m. Testing
                // adjacency therefore finds essentially nothing and would have
                // reported "no mountainside mouths exist", which is an artifact
                // of the test, not of the world. So: a void voxel counts as
                // daylighting if ANY column within `mouthReach` metres has its
                // surface at or below that voxel. It is a proxy for "this void
                // is within digging-free reach of a free face", labelled as one.
                int32_t minNearSurfMm = INT32_MAX;
                for (int64_t dy = -mouthReachSamples; dy <= mouthReachSamples && colMask; ++dy)
                    for (int64_t dx = -mouthReachSamples; dx <= mouthReachSamples; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        minNearSurfMm = std::min(
                            minNearSurfMm, amp.surfaceMm(vx + dx * stride, vy + dy * stride));
                    }
                bool mouth = false;
                for (int64_t k = 0; k <= depthVox && !mouth; ++k) {
                    if (!fam[static_cast<size_t>(k)]) continue;
                    const int64_t zMm = (topVz - k) * kVoxelSizeMm + kVoxelSizeMm / 2;
                    if (minNearSurfMm <= zMm) mouth = true;
                }
                if (mouth) {
                    ++mouthCols;
                    if (steep) ++mouthColsSteep; else ++mouthColsFlat;
                }

                // Floors: solid voxel with `headVox` of continuous air above it,
                // where the first air voxel is cave air. Attributed to that
                // voxel's family -- this is the spawn-surface budget of 5.6.
                uint32_t colFloorMask = 0;
                for (int64_t k = 1; k <= depthVox; ++k) {
                    if (airCol[static_cast<size_t>(k)]) continue;   // not solid
                    if (!fam[static_cast<size_t>(k - 1)]) continue; // air above is not cave air
                    bool clear = true;
                    int64_t hv = 0;
                    for (; hv < headVox; ++hv) {
                        const int64_t kk = k - 1 - hv;
                        if (kk < 0) break; // ran out of the scan band -> open to the sky
                        if (!airCol[static_cast<size_t>(kk)]) { clear = false; break; }
                    }
                    if (!clear) continue;
                    const int32_t f = caveDominantFamily(fam[static_cast<size_t>(k - 1)]);
                    if (f < 0) continue;
                    ++floorSpots[f];
                    headroomSumMm[f] += hv * kVoxelSizeMm;
                    colFloorMask |= 1u << f;
                }
                if (colFloorMask) ++floorColsAny;
                for (uint32_t f = 0; f < kCaveFamilyCount; ++f)
                    if (colFloorMask & (1u << f)) ++floorCols[f];

                if (images) {
                    // Elevation base: a dark grey so cave colour reads on top.
                    const double e = std::min(1.0, std::max(0.0, (col.surfaceMm) / 2000000.0));
                    const uint8_t g = static_cast<uint8_t>(20 + 55 * e);
                    const Rgb base{g, g, static_cast<uint8_t>(g + 6)};
                    putPx(imgClass, px, ix, iy, base);
                    putPx(imgDepth, px, ix, iy, base);
                    putPx(imgThick, px, ix, iy, base);
                    if (topVoidIdx >= 0) {
                        const int32_t f = caveDominantFamily(fam[static_cast<size_t>(topVoidIdx)]);
                        if (f >= 0) putPx(imgClass, px, ix, iy, kFamColour[f]);
                        putPx(imgDepth, px, ix, iy,
                              rampBlueRed(static_cast<double>(topVoidIdx) /
                                          static_cast<double>(depthVox)));
                        const double t = std::min(1.0, static_cast<double>(thickVox) / 120.0);
                        const uint8_t v = static_cast<uint8_t>(40 + 215 * t);
                        putPx(imgThick, px, ix, iy, Rgb{v, v, static_cast<uint8_t>(v / 2 + 40)});
                    }
                }
            }
        }
    }

    // --- cross-sections -----------------------------------------------------
    // Full voxel z resolution, sampled in xy at the same stride as the plan
    // views, through the region centre. The point of these is that roof cover,
    // storey structure and the bedrock floor are directly readable -- none of
    // which a plan view can show and none of which a screenshot can measure.
    if (images) {
        for (int axis = 0; axis < 2; ++axis) {
            int64_t zTop = INT64_MIN, zBot = INT64_MAX;
            for (int i = 0; i < px; ++i) {
                const int64_t vx = (axis == 0) ? x0 + static_cast<int64_t>(i) * stride : cx;
                const int64_t vy = (axis == 0) ? cy : y0 + static_cast<int64_t>(i) * stride;
                const int32_t s = amp.surfaceMm(vx, vy);
                const int64_t t = floorDiv(s - kVoxelSizeMm / 2, kVoxelSizeMm);
                zTop = std::max(zTop, t + 20);
                zBot = std::min(zBot, t - depthVox - 20);
            }
            const int64_t h = zTop - zBot + 1;
            if (h <= 0 || h > 8192) continue;
            std::vector<uint8_t> sect(static_cast<size_t>(px) * static_cast<size_t>(h) * 3u, 0);
            for (int i = 0; i < px; ++i) {
                const int64_t vx = (axis == 0) ? x0 + static_cast<int64_t>(i) * stride : cx;
                const int64_t vy = (axis == 0) ? cy : y0 + static_cast<int64_t>(i) * stride;
                const ColumnSample col = amp.column(vx, vy);
                const CaveColumnVariants cv = caveColumnVariantsFor(seed, vx, vy, col.surfaceMm, amp.surfaceAtFn(), entranceCavityOff);
                for (int64_t r = 0; r < h; ++r) {
                    const int64_t vz = zTop - r; // row 0 = top of image
                    const MaterialId strat = Amplifier::stratigraphyAt(col, vz);
                    Rgb c{12, 14, 20}; // sky
                    if (strat != MAT_AIR) {
                        bool truth = false;
                        const uint32_t m = caveFamilyMaskAt(cv, col.cavern, col.surfaceMm,
                                                        col.bedrockDepthMm, vz, truth);
                        const int32_t f = caveDominantFamily(m);
                        if (f >= 0) {
                            c = kFamColour[f];
                        } else if (strat == MAT_BEDROCK) {
                            c = Rgb{48, 40, 36};
                        } else if (strat == MAT_ROCK) {
                            c = Rgb{96, 92, 88};
                        } else {
                            c = Rgb{132, 116, 92}; // soils
                        }
                    }
                    putPx(sect, px, i, static_cast<int>(r), c);
                }
            }
            const std::string p = outPrefix + (axis == 0 ? "-sect-x.png" : "-sect-y.png");
            if (!writePng(p, sect, px, static_cast<int>(h)))
                std::fprintf(stderr, "failed to write %s\n", p.c_str());
            else
                std::printf("wrote %s (%dx%lld, 1 voxel/row = 0.1 m)\n", p.c_str(), px,
                            (long long)h);
        }
        if (!sectionOnly) {
            struct {
                const char* suffix;
                std::vector<uint8_t>* img;
            } outs[3] = {{"-plan-class.png", &imgClass},
                         {"-plan-depth.png", &imgDepth},
                         {"-plan-thick.png", &imgThick}};
            for (auto& o : outs) {
                const std::string p = outPrefix + o.suffix;
                if (!writePng(p, *o.img, px, px))
                    std::fprintf(stderr, "failed to write %s\n", p.c_str());
                else
                    std::printf("wrote %s (%dx%d)\n", p.c_str(), px, px);
            }
        }
    }

    if (sectionOnly) return 0;

    // --- direction histogram over lattice edges ------------------------------
    // Tell #2 in the plan's diagnosis: every edge joins a node to its +x or +y
    // neighbour, so after jitter the headings cluster hard around due E and due
    // N. Measured as tunnel AXIS LENGTH per 15-degree bin over [0, 180), and
    // summarised as the fraction within 15 degrees of a cardinal -- which is
    // the single number W2 has to move.
    const int kDirBins = 12; // 15 degrees each
    std::vector<double> dirLenM(static_cast<size_t>(kDirBins), 0.0);
    double totalEdgeLenM = 0.0, cardinalLenM = 0.0;
    // Entrance SITES, counted from the lattice rather than from perforated
    // columns: "roughly one entrance per 205 m square" (caves.h:148-152) is a
    // claim about gated-open shaft nodes, and that is the number W3's
    // replacement portfolio has to re-provide. Cavern sites for the same
    // reason -- W4 must not quietly change how many rooms exist.
    int64_t shaftCandidates = 0, shaftOpen = 0, cavernCandidates = 0, cavernOpen = 0;
    {
        const int64_t li0 = floorDiv(x0 * kVoxelSizeMm, kCaveLatticeMm);
        const int64_t lj0 = floorDiv(y0 * kVoxelSizeMm, kCaveLatticeMm);
        const int64_t li1 = floorDiv((x0 + spanVx) * kVoxelSizeMm, kCaveLatticeMm);
        const int64_t lj1 = floorDiv((y0 + spanVx) * kVoxelSizeMm, kCaveLatticeMm);
        // Sites are counted by their JITTERED POSITION falling inside the
        // census region, not by their lattice index. A node lands anywhere in
        // its own 25.6 m cell, so index-based counting reports entrances that
        // are not in the region and misses ones that are -- on the savanna tile
        // it claimed two entrances in a region whose surface the probe found to
        // be unperforated, because both nodes had jittered out of it. The
        // lattice range below is widened by one cell so no in-region node is
        // missed.
        const int64_t rxMm0 = x0 * kVoxelSizeMm, rxMm1 = (x0 + spanVx) * kVoxelSizeMm;
        const int64_t ryMm0 = y0 * kVoxelSizeMm, ryMm1 = (y0 + spanVx) * kVoxelSizeMm;
        auto inRegion = [&](const CaveNode& nd) {
            return nd.xMm >= rxMm0 && nd.xMm < rxMm1 && nd.yMm >= ryMm0 && nd.yMm < ryMm1;
        };
        for (int64_t j = lj0 - 1; j <= lj1 + 1; ++j)
            for (int64_t i = li0 - 1; i <= li1 + 1; ++i) {
                if ((i & kCaveShaftNodeMask) == 0 && (j & kCaveShaftNodeMask) == 0 &&
                    inRegion(caveNode(seed, i, j))) {
                    ++shaftCandidates;
                    if (((hash2(seed, i, j, CH_CAVE_SHAFT) >> 48) & kCaveShaftGateMask) == 0)
                        ++shaftOpen;
                }
                // (i & 7), not (i % 8): two's-complement AND is the floored
                // modulo for negative indices too -- the same reason caves.h
                // uses a power-of-two mask for its backbone selector.
                if ((i & (kCavernCoarseLatticeRatio - 1)) == 0 &&
                    (j & (kCavernCoarseLatticeRatio - 1)) == 0 && inRegion(caveNode(seed, i, j))) {
                    ++cavernCandidates;
                    // Site gate AND the child-0 depth safety window, which is
                    // what actually decides whether a room exists.
                    if (cavernSiteGateOpen(seed, i, j) &&
                        cavernDepthIsSafe(caveNode(seed, i, j).depthMm))
                        ++cavernOpen;
                }
            }
        for (int64_t j = lj0; j <= lj1; ++j)
            for (int64_t i = li0; i <= li1; ++i)
                for (int32_t d = 0; d < 2; ++d) {
                    if (!caveEdgeExists(seed, i, j, d)) continue;
                    const CaveNode a = caveNode(seed, i, j);
                    const CaveNode b = (d == 0) ? caveNode(seed, i + 1, j) : caveNode(seed, i, j + 1);
                    // Walk the axis the way the carve does: the SUB-SEGMENTS of
                    // the waypointed polyline, not the node-to-node chord.
                    // Measuring the chord is what this histogram did first, and
                    // it is blind by construction to the very change waypointing
                    // makes -- it reported the identical 54.14% before and after
                    // W2, because a waypoint moves the axis without moving
                    // either endpoint.
                    const CaveWaypoint w = caveWaypoint(seed, i, j, d, a, b);
                    const int64_t pxs[3] = {a.xMm, w.xMm, b.xMm};
                    const int64_t pys[3] = {a.yMm, w.yMm, b.yMm};
                    for (int32_t sub = 0; sub < kCaveEdgeSubSegs; ++sub) {
                        const double dx = static_cast<double>(pxs[sub + 1] - pxs[sub]);
                        const double dy = static_cast<double>(pys[sub + 1] - pys[sub]);
                        const double len = std::sqrt(dx * dx + dy * dy) / 1000.0;
                        if (len <= 0.0) continue;
                        double deg = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
                        if (deg < 0) deg += 180.0;
                        if (deg >= 180.0) deg -= 180.0;
                        dirLenM[static_cast<size_t>(
                            std::min(kDirBins - 1, static_cast<int>(deg / 15.0)))] += len;
                        totalEdgeLenM += len;
                        // within 15 degrees of due E (0/180) or due N (90)
                        const double dE = std::min(deg, 180.0 - deg);
                        const double dN = std::abs(deg - 90.0);
                        if (std::min(dE, dN) <= 15.0) cardinalLenM += len;
                    }
                }
    }

    // --- connectivity --------------------------------------------------------
    int32_t compCount = 0;
    double largestShare = 0.0, skyShare = 0.0;
    size_t compTotal = 0, compLargest = 0, skyReachable = 0;
    int64_t shaftsInConnectBox = 0, portalSeeds = 0;
    bool ranConnectivity = false;
    if (connectN > 0) {
        // Same decimation as test_caves.cpp: every 4th voxel (0.4 m), which
        // still leaves >= 6 samples across the narrowest 2.4 m tube, so
        // 6-connected paths survive.
        const int64_t st = 4;
        const int64_t n = connectN;
        const int64_t bx0 = (connectAtSet ? connectAtVx : cx) - n * st / 2;
        const int64_t by0 = (connectAtSet ? connectAtVy : cy) - n * st / 2;
        // How many open sinkhole nodes are actually inside this box? Without
        // this the reachability figure below cannot be read: 0% means "sealed"
        // only if there was an entrance in the box to begin with.
        //
        // Counted by the node's JITTERED POSITION, not by its lattice index. A
        // node is jittered anywhere inside its own 25.6 m cell, so an index
        // whose cell overlaps the box routinely places its node outside it --
        // which is exactly how this counter first reported "1 entrance in the
        // box, 0% reachable" for a box that in fact contained no entrance at
        // all, and made a correct result look like a bug.
        const int64_t bxMm0 = bx0 * kVoxelSizeMm, bxMm1 = (bx0 + n * st) * kVoxelSizeMm;
        const int64_t byMm0 = by0 * kVoxelSizeMm, byMm1 = (by0 + n * st) * kVoxelSizeMm;
        for (int64_t j = floorDiv(byMm0, kCaveLatticeMm) - 1;
             j <= floorDiv(byMm1, kCaveLatticeMm) + 1; ++j)
            for (int64_t i = floorDiv(bxMm0, kCaveLatticeMm) - 1;
                 i <= floorDiv(bxMm1, kCaveLatticeMm) + 1; ++i) {
                if ((i & kCaveShaftNodeMask) != 0 || (j & kCaveShaftNodeMask) != 0) continue;
                if (((hash2(seed, i, j, CH_CAVE_SHAFT) >> 48) & kCaveShaftGateMask) != 0) continue;
                const CaveNode nd = caveNode(seed, i, j);
                if (nd.xMm < bxMm0 || nd.xMm >= bxMm1 || nd.yMm < byMm0 || nd.yMm >= byMm1)
                    continue;
                ++shaftsInConnectBox;
            }
        std::vector<ColumnSample> cols(static_cast<size_t>(n) * static_cast<size_t>(n));
        int64_t sMin = INT64_MAX, sMax = INT64_MIN;
        for (int64_t b = 0; b < n; ++b)
            for (int64_t a = 0; a < n; ++a) {
                const ColumnSample c = amp.column(bx0 + a * st, by0 + b * st);
                cols[static_cast<size_t>(a + n * b)] = c;
                sMin = std::min<int64_t>(sMin, c.surfaceMm);
                sMax = std::max<int64_t>(sMax, c.surfaceMm);
            }
        const int64_t z0 = floorDiv(sMin - maxDepthM * 1000, kVoxelSizeMm);
        // Deliberate headroom above the highest surface in the box, so the top
        // slab of the analysis volume is unambiguously open sky.
        const int64_t z1 = floorDiv(sMax, kVoxelSizeMm) + 8 * st;
        const int64_t nz = (z1 - z0) / st + 1;
        const size_t cells = static_cast<size_t>(n) * static_cast<size_t>(n) *
                             static_cast<size_t>(std::max<int64_t>(0, nz));
        if (nz > 0 && cells <= 400ull * 1000ull * 1000ull) {
            ranConnectivity = true;
            // One byte per sample: 0 solid, 1 cave air, 2 open (above-ground)
            // air. Materialising it once is what makes the two analyses below
            // affordable -- and in particular is why the sky reachability is
            // NOT done with findComponents: the sky component of a box with
            // real relief is tens of millions of samples, and findComponents
            // stores every member's coordinates.
            enum : uint8_t { CELL_SOLID = 0, CELL_CAVE = 1, CELL_OPEN = 2 };
            std::vector<uint8_t> gridCells(cells, CELL_SOLID);
            auto idx = [&](int64_t a, int64_t b, int64_t c) {
                return static_cast<size_t>((c * n + b) * n + a);
            };
            for (int64_t c = 0; c < nz; ++c)
                for (int64_t b = 0; b < n; ++b)
                    for (int64_t a = 0; a < n; ++a) {
                        const ColumnSample& col = cols[static_cast<size_t>(a + n * b)];
                        const int64_t vz = z0 + c * st;
                        if (Amplifier::materialAt(col, vz) != MAT_AIR) continue;
                        gridCells[idx(a, b, c)] =
                            Amplifier::stratigraphyAt(col, vz) == MAT_AIR ? CELL_OPEN : CELL_CAVE;
                    }

            const ConnectivityResult r = findComponents(
                [&](int64_t a, int64_t b, int64_t c) { return gridCells[idx(a, b, c)] == CELL_CAVE; },
                VoxelCoord{0, 0, 0}, VoxelCoord{n - 1, n - 1, nz - 1});
            compCount = r.componentCount;
            for (const Component& c : r.components) {
                compTotal += c.size();
                compLargest = std::max(compLargest, c.size());
            }
            if (compTotal)
                largestShare = static_cast<double>(compLargest) / static_cast<double>(compTotal);

            // THE ENTRANCE GUARANTEE, measured end to end rather than proxied.
            // "Findable on a walk without digging" is not a count of holes; it
            // is whether cave air and the outside are the same connected
            // volume. So: BFS over CAVE cells only, seeded from every cave cell
            // that touches open air (a shaft mouth, a hillside daylighting, or
            // anything a redesign invents), and report the share of cave volume
            // it reaches. Seeding from the boundary keeps the walk inside the
            // ~45k cave cells instead of the millions of sky cells.
            std::vector<uint8_t> seen(cells, 0);
            std::vector<int64_t> stack;
            const int64_t dA[6] = {1, -1, 0, 0, 0, 0};
            const int64_t dB[6] = {0, 0, 1, -1, 0, 0};
            const int64_t dC[6] = {0, 0, 0, 0, 1, -1};
            for (int64_t c = 0; c < nz; ++c)
                for (int64_t b = 0; b < n; ++b)
                    for (int64_t a = 0; a < n; ++a) {
                        if (gridCells[idx(a, b, c)] != CELL_CAVE) continue;
                        bool portal = false;
                        for (int k = 0; k < 6 && !portal; ++k) {
                            const int64_t aa = a + dA[k], bb = b + dB[k], cc = c + dC[k];
                            if (aa < 0 || aa >= n || bb < 0 || bb >= n || cc < 0 || cc >= nz)
                                continue;
                            if (gridCells[idx(aa, bb, cc)] == CELL_OPEN) portal = true;
                        }
                        if (!portal) continue;
                        ++portalSeeds;
                        const size_t s = idx(a, b, c);
                        if (seen[s]) continue;
                        seen[s] = 1;
                        stack.push_back(static_cast<int64_t>(s));
                    }
            while (!stack.empty()) {
                const int64_t s = stack.back();
                stack.pop_back();
                ++skyReachable;
                const int64_t a = s % n, b = (s / n) % n, c = s / (n * n);
                for (int k = 0; k < 6; ++k) {
                    const int64_t aa = a + dA[k], bb = b + dB[k], cc = c + dC[k];
                    if (aa < 0 || aa >= n || bb < 0 || bb >= n || cc < 0 || cc >= nz) continue;
                    const size_t t = idx(aa, bb, cc);
                    if (seen[t] || gridCells[t] != CELL_CAVE) continue;
                    seen[t] = 1;
                    stack.push_back(static_cast<int64_t>(t));
                }
            }
            if (compTotal)
                skyShare = static_cast<double>(skyReachable) / static_cast<double>(compTotal);
        } else if (nz > 0) {
            std::printf("\n[connectivity skipped: %zu samples exceeds the 400M budget -- lower "
                        "--connect or --max-depth]\n",
                        cells);
        }
    }

    // --- report --------------------------------------------------------------
    const double colsD = static_cast<double>(std::max<int64_t>(1, colsSampled));
    const double bandD = static_cast<double>(std::max<int64_t>(1, solidBandVox));

    std::printf("\n--- self-check (a wrong instrument must say so) ---\n");
    std::printf("family attribution vs caveCarveAt : %lld mismatched voxels %s\n",
                (long long)attrMismatch, attrMismatch ? "  <-- ATTRIBUTION BROKEN" : "(exact)");
    std::printf("lattice recompute vs ColumnSample : %lld mismatched columns %s\n",
                (long long)memoMismatch, memoMismatch ? "  <-- MEMO DIVERGENCE" : "(exact)");

    std::printf("\n--- volume and occupancy ---\n");
    std::printf("solid voxels in band = %lld   carved = %lld (%.3f%% of the band)\n",
                (long long)solidBandVox, (long long)carvedVox, 100.0 * carvedVox / bandD);
    std::printf("columns with any cave beneath: %lld / %lld (%.2f%%)\n", (long long)colsWithAny,
                (long long)colsSampled, 100.0 * colsWithAny / colsD);
    std::printf("deepest carve = %.1f m below surface; thinnest non-shaft roof = %.1f m "
                "(clamp %.1f m)\n",
                maxCarveDepthMm / 1000.0,
                minRoofMm == (1ll << 40) ? 0.0 : minRoofMm / 1000.0, kCaveRoofMinMm / 1000.0);
    std::printf("%-9s %14s %10s %14s %8s\n", "family", "voxels", "%band", "columns", "%cols");
    for (uint32_t f = 0; f < kCaveFamilyCount; ++f)
        std::printf("%-9s %14lld %9.4f%% %14lld %7.3f%%\n", kFamName[f], (long long)famVox[f],
                    100.0 * famVox[f] / bandD, (long long)famCols[f], 100.0 * famCols[f] / colsD);

    std::printf("\n--- entrances (the plan's headline: is the network findable on a walk?) ---\n");
    std::printf("ground: %lld flat / %lld steep columns (steep >= %lld mm/m)\n",
                (long long)(colsSampled - colsSteep), (long long)colsSteep,
                (long long)steepMmPerM);
    // Perforation is reported three ways on purpose. The percentage is what
    // test_caves.cpp already gates on; the per-km^2 count is what "one entrance
    // per 205 m square" means in walkable terms (~24/km^2); the flat/steep
    // split is the number the plan's flat-ground decision has to be judged on.
    std::printf("perforated surface columns (you would fall in): %lld (%.5f%% of the surface, "
                "%.1f sample-columns per km^2)\n",
                (long long)perfColsAny, 100.0 * perfColsAny / colsD,
                static_cast<double>(perfColsAny) / std::max(1e-9, regionKm2));
    std::printf("  on flat ground : %lld (%.1f per km^2 of flat ground)\n", (long long)perfColsFlat,
                static_cast<double>(perfColsFlat) /
                    std::max(1e-9, (colsSampled - colsSteep) * sampleAreaM2 / 1e6));
    std::printf("  on steep ground: %lld (%.1f per km^2 of steep ground)\n",
                (long long)perfColsSteep,
                static_cast<double>(perfColsSteep) / std::max(1e-9, colsSteep * sampleAreaM2 / 1e6));
    for (uint32_t f = 0; f < kCaveFamilyCount; ++f)
        if (perfCols[f])
            std::printf("  by family: %-8s %lld\n", kFamName[f], (long long)perfCols[f]);
    std::printf("sideways daylighting mouths (columns): %lld total, %lld flat, %lld steep\n",
                (long long)mouthCols, (long long)mouthColsFlat, (long long)mouthColsSteep);
    std::printf("entrance cavity footprint: %lld columns (%.4f%% of the surface, "
                "%.0f m^2/km^2)\n",
                (long long)entCols, 100.0 * entCols / colsD,
                entCols * sampleAreaM2 / std::max(1e-9, regionKm2));
    std::printf("  open to the sky : %lld (%.1f%% of the footprint) -- the doline mouth\n",
                (long long)entOpenCols, 100.0 * entOpenCols / std::max<int64_t>(1, entCols));
    std::printf("  roofed          : %lld (%.1f%%), %lld flat / %lld steep -- overhanging "
                "lip and hillside ceiling\n",
                (long long)entRoofedCols, 100.0 * entRoofedCols / std::max<int64_t>(1, entCols),
                (long long)entRoofedFlat, (long long)entRoofedSteep);
    // WHAT THIS SPLIT DOES *NOT* TELL YOU, recorded because the first
    // version of this tool got it wrong and the wrong number was reported
    // onward. "Roof thinner than the 6 m clamp" looks like a
    // horizontal-mouth statistic and is not one: near the cavity rim the
    // lens roof pinches to nothing, so the cover there is just the drawn
    // floor depth, [5, 9) m, which is under 6 m for more than half of all
    // sites ON DEAD FLAT GROUND. It measures the lens, not the hill. The
    // open/roofed split above is real; the mouth-versus-doline question is
    // about the SHAPE of the open set, needs the per-site footprint radius,
    // and is answered per site in the site list below.
    std::printf("  (mouth vs doline is a per-SITE question -- see the site list below; a\n"
                "   roof-thickness threshold cannot answer it, see the comment here)\n");

    // --- W4 (v26): plan-view chamber symmetry, v26 vs the v25-shape control --
    if (!cavernAbOff && (cavRealCols > 0 || cavPlainCols > 0)) {
        struct ArmStats {
            int64_t area = 0, comps = 0, holes = 0;
            double anisoMax = 1.0, anisoAreaWeighted = 1.0;
            int64_t driftMm = 0, d180PerMille = 0, floorStepMm = 0;
        };
        const double cellM = stride * kVoxelSizeMm / 1000.0;

        auto analyse = [&](const std::vector<uint8_t>& mask, const std::vector<int32_t>& segZ,
                           const std::vector<int32_t>& segF, const std::vector<uint8_t>& segN) {
            ArmStats a;
            // (1) COMPONENTS + ANISOTROPY. Second moments of each 8-connected
            // component; the eigenvalue ratio of a 2x2 symmetric matrix in
            // closed form. A disc reads ~1 however rough its edge is, which is
            // what makes this the statistic for "round in plan" rather than a
            // boundary-noise statistic.
            std::vector<int32_t> comp(mask.size(), -1);
            std::vector<int32_t> stack;
            double anisoWeighted = 0.0;
            int64_t weightedArea = 0;
            for (int cy = 0; cy < px; ++cy)
                for (int cx = 0; cx < px; ++cx) {
                    const size_t s0 = static_cast<size_t>(cy) * px + cx;
                    if (!mask[s0] || comp[s0] >= 0) continue;
                    const int32_t id = static_cast<int32_t>(a.comps++);
                    stack.clear();
                    stack.push_back(static_cast<int32_t>(s0));
                    comp[s0] = id;
                    std::vector<int32_t> cells;
                    while (!stack.empty()) {
                        const int32_t t = stack.back();
                        stack.pop_back();
                        cells.push_back(t);
                        const int tx = t % px, ty = t / px;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const int nx = tx + dx, ny = ty + dy;
                                if (nx < 0 || ny < 0 || nx >= px || ny >= px) continue;
                                const size_t ns = static_cast<size_t>(ny) * px + nx;
                                if (!mask[ns] || comp[ns] >= 0) continue;
                                comp[ns] = id;
                                stack.push_back(static_cast<int32_t>(ns));
                            }
                    }
                    a.area += static_cast<int64_t>(cells.size());
                    if (cells.size() < 64) continue; // slivers carry no shape
                    double sx = 0, sy = 0;
                    for (int32_t t : cells) {
                        sx += t % px;
                        sy += t / px;
                    }
                    sx /= double(cells.size());
                    sy /= double(cells.size());
                    double mxx = 0, myy = 0, mxy = 0;
                    for (int32_t t : cells) {
                        const double ex = (t % px) - sx, ey = (t / px) - sy;
                        mxx += ex * ex;
                        myy += ey * ey;
                        mxy += ex * ey;
                    }
                    mxx /= double(cells.size());
                    myy /= double(cells.size());
                    mxy /= double(cells.size());
                    const double tr = mxx + myy;
                    const double disc = std::sqrt((mxx - myy) * (mxx - myy) + 4 * mxy * mxy);
                    const double lo = tr - disc;
                    const double ratio = lo > 1e-9 ? (tr + disc) / lo : 64.0;
                    if (ratio > a.anisoMax) a.anisoMax = ratio;
                    anisoWeighted += ratio * double(cells.size());
                    weightedArea += static_cast<int64_t>(cells.size());
                    // (2) HALF-TURN DEFECT for this component, exact on the
                    // grid: the centroid is carried doubled so p = 2c - i lands
                    // on a cell and no resampling can manufacture a defect.
                    const int64_t cx2 = static_cast<int64_t>(std::llround(2 * sx));
                    const int64_t cy2 = static_cast<int64_t>(std::llround(2 * sy));
                    int64_t unmatched = 0;
                    for (int32_t t : cells) {
                        const int64_t rx = cx2 - (t % px), ry = cy2 - (t / px);
                        if (rx < 0 || ry < 0 || rx >= px || ry >= px) {
                            ++unmatched;
                            continue;
                        }
                        if (!mask[static_cast<size_t>(ry) * px + rx]) ++unmatched;
                    }
                    a.d180PerMille += unmatched * 1000 / static_cast<int64_t>(cells.size());
                }
            if (weightedArea > 0) a.anisoAreaWeighted = anisoWeighted / double(weightedArea);
            if (a.comps > 0) a.d180PerMille /= a.comps;

            // (3) HOLES == PILLARS. 4-connected runs of non-mask cells that
            // never reach the grid border: rock completely enclosed by chamber
            // in plan. A union of ellipses cannot produce one.
            {
                std::vector<uint8_t> seen(mask.size(), 0);
                for (int cy = 0; cy < px; ++cy)
                    for (int cx = 0; cx < px; ++cx) {
                        const size_t s0 = static_cast<size_t>(cy) * px + cx;
                        if (mask[s0] || seen[s0]) continue;
                        bool border = false;
                        stack.clear();
                        stack.push_back(static_cast<int32_t>(s0));
                        seen[s0] = 1;
                        while (!stack.empty()) {
                            const int32_t t = stack.back();
                            stack.pop_back();
                            const int tx = t % px, ty = t / px;
                            if (tx == 0 || ty == 0 || tx == px - 1 || ty == px - 1) border = true;
                            const int nb[4] = {tx > 0 ? t - 1 : -1, tx < px - 1 ? t + 1 : -1,
                                               ty > 0 ? t - px : -1, ty < px - 1 ? t + px : -1};
                            for (int k = 0; k < 4; ++k) {
                                if (nb[k] < 0 || mask[size_t(nb[k])] || seen[size_t(nb[k])])
                                    continue;
                                seen[size_t(nb[k])] = 1;
                                stack.push_back(nb[k]);
                            }
                        }
                        if (!border) ++a.holes;
                    }
            }

            // (4) STACK DRIFT. Largest plan distance between the centroids of
            // two DIFFERENT room slices (a room is one distinct zCenterMm).
            // A coaxial chain puts every slice on one axis, so the control
            // arm's value here is the statistic's own noise floor -- the
            // wall-roughness wobble, which v25 already had.
            {
                std::vector<int32_t> zs;
                std::vector<double> sxv, syv;
                std::vector<int64_t> nv;
                for (size_t c2 = 0; c2 < mask.size(); ++c2)
                    for (int32_t k = 0; k < segN[c2]; ++k) {
                        const int32_t z = segZ[c2 * kMaxCavernSegs + k];
                        size_t idx = zs.size();
                        for (size_t q = 0; q < zs.size(); ++q)
                            if (zs[q] == z) {
                                idx = q;
                                break;
                            }
                        if (idx == zs.size()) {
                            zs.push_back(z);
                            sxv.push_back(0);
                            syv.push_back(0);
                            nv.push_back(0);
                        }
                        sxv[idx] += double(c2 % px);
                        syv[idx] += double(c2 / px);
                        ++nv[idx];
                    }
                for (size_t q = 0; q < zs.size(); ++q)
                    for (size_t r = q + 1; r < zs.size(); ++r) {
                        if (nv[q] < 64 || nv[r] < 64) continue;
                        const double dx = sxv[q] / double(nv[q]) - sxv[r] / double(nv[r]);
                        const double dy = syv[q] / double(nv[q]) - syv[r] / double(nv[r]);
                        // Rooms of DIFFERENT sites are not comparable, and two
                        // sites are at least a coarse cell apart, so anything
                        // beyond one site's own reach is skipped.
                        const double dMm = std::sqrt(dx * dx + dy * dy) * cellM * 1000.0;
                        if (dMm > double(kCavernMaxReachMm)) continue;
                        if (int64_t(dMm) > a.driftMm) a.driftMm = int64_t(dMm);
                    }
            }

            // (5) FLOOR STEP == BREAKDOWN. Mean |floor difference| between
            // 4-adjacent columns that share a ROOM (same zCenterMm). Asked per
            // room because a column's lowest floor is dominated by the step
            // where one room ends and the next room's deeper floor takes over,
            // which is chain geometry rather than rubble. A machined floor
            // gives exactly 0.
            {
                int64_t sum = 0, pairs = 0;
                for (int cy = 0; cy < px; ++cy)
                    for (int cx = 0; cx + 1 < px; ++cx) {
                        const size_t A = static_cast<size_t>(cy) * px + cx, B = A + 1;
                        for (int32_t ka = 0; ka < segN[A]; ++ka)
                            for (int32_t kb = 0; kb < segN[B]; ++kb) {
                                if (segZ[A * kMaxCavernSegs + ka] != segZ[B * kMaxCavernSegs + kb])
                                    continue;
                                sum += std::llabs(int64_t(segF[A * kMaxCavernSegs + ka]) -
                                                  segF[B * kMaxCavernSegs + kb]);
                                ++pairs;
                            }
                    }
                a.floorStepMm = pairs ? sum / pairs : 0;
            }
            return a;
        };

        const ArmStats R = analyse(cavMaskReal, cavSegZ, cavSegFloor, cavSegN);
        const ArmStats P = analyse(cavMaskPlain, cavSegZP, cavSegFloorP, cavSegNP);
        std::printf("\n--- W4 chamber shape: plan-view symmetry, v26 vs the v25-shape control "
                    "---\n");
        std::printf("(both arms are the SAME sites, terrain, seed and shipping predicate; the\n"
                    " control differs only by cave_families.h cavernSiteWithoutChamberShape.\n"
                    " The control is what proves each statistic can report SYMMETRIC at all --\n"
                    " a one-arm asymmetry number is satisfied by roof, bedrock and region\n"
                    " clipping, all of which were already true at v25.)\n");
        std::printf("%-34s %15s %15s\n", "statistic", "v26 (shipping)", "v25-shape ctl");
        std::printf("%-34s %15lld %15lld\n", "cavern plan columns", (long long)cavRealCols,
                    (long long)cavPlainCols);
        std::printf("%-34s %15lld %15lld\n", "plan components", (long long)R.comps,
                    (long long)P.comps);
        std::printf("%-34s %13.2f:1 %13.2f:1\n", "plan anisotropy (area-weighted)",
                    R.anisoAreaWeighted, P.anisoAreaWeighted);
        std::printf("%-34s %13.2f:1 %13.2f:1\n", "plan anisotropy (worst room)", R.anisoMax,
                    P.anisoMax);
        std::printf("%-34s %13.1f m %13.1f m\n", "room-to-room stack drift", R.driftMm / 1000.0,
                    P.driftMm / 1000.0);
        std::printf("%-34s %15lld %15lld\n", "enclosed rock islands (PILLARS)", (long long)R.holes,
                    (long long)P.holes);
        std::printf("%-34s %10lld/1000 %10lld/1000\n", "half-turn defect",
                    (long long)R.d180PerMille, (long long)P.d180PerMille);
        std::printf("%-34s %12lld mm %12lld mm\n", "floor step within a room (RUBBLE)",
                    (long long)R.floorStepMm, (long long)P.floorStepMm);
        std::printf("columns the shipping arm turned to rock that the control carved: %lld\n"
                    "  (an UPPER BOUND on pillar area, not a pillar count -- the offsets also\n"
                    "   move a room off a column. The enclosed-island row isolates pillars.)\n",
                    (long long)pillarCols);
    }

    std::printf("\n--- floor area and headroom (5.6: where can a mob stand?) ---\n");
    std::printf("%-9s %14s %14s %12s %12s\n", "family", "floor spots", "m^2/km^2", "columns",
                "mean head m");
    for (uint32_t f = 0; f < kCaveFamilyCount; ++f) {
        const double m2 = floorSpots[f] * sampleAreaM2;
        std::printf("%-9s %14lld %14.0f %12lld %12.2f\n", kFamName[f], (long long)floorSpots[f],
                    m2 / std::max(1e-9, regionKm2), (long long)floorCols[f],
                    floorSpots[f] ? headroomSumMm[f] / 1000.0 / static_cast<double>(floorSpots[f])
                                  : 0.0);
    }
    std::printf("columns with any standable floor: %lld (%.3f%%)\n", (long long)floorColsAny,
                100.0 * floorColsAny / colsD);

    std::printf("\n--- depth distribution (5.7: how much of the rock column is used?) ---\n");
    std::printf("%-12s %12s %12s %12s %12s\n", "band", kFamName[0], kFamName[1], kFamName[2],
                kFamName[3]);
    for (int b = 0; b < kDepthBands; ++b) {
        int64_t rowTotal = 0;
        for (uint32_t f = 0; f < kCaveFamilyCount; ++f)
            rowTotal += depthHist[static_cast<size_t>(b) * kCaveFamilyCount + f];
        if (!rowTotal) continue;
        char label[32];
        std::snprintf(label, sizeof(label), "%d-%d m", b * 5, b * 5 + 5);
        std::printf("%-12s %12lld %12lld %12lld %12lld\n", label,
                    (long long)depthHist[static_cast<size_t>(b) * kCaveFamilyCount + 0],
                    (long long)depthHist[static_cast<size_t>(b) * kCaveFamilyCount + 1],
                    (long long)depthHist[static_cast<size_t>(b) * kCaveFamilyCount + 2],
                    (long long)depthHist[static_cast<size_t>(b) * kCaveFamilyCount + 3]);
    }

    std::printf("\n--- passage direction (tell #2: the lattice's cardinal lock) ---\n");
    for (int b = 0; b < kDirBins; ++b) {
        const double pct = 100.0 * dirLenM[static_cast<size_t>(b)] / std::max(1e-9, totalEdgeLenM);
        char bar[64];
        const int n = std::min(50, static_cast<int>(pct * 2.0));
        for (int k = 0; k < n; ++k) bar[k] = '#';
        bar[n] = 0;
        std::printf("%3d-%3d deg  %6.2f%%  %s\n", b * 15, b * 15 + 15, pct, bar);
    }
    std::printf("SEGMENT lock: %.2f%% of tunnel axis length lies within 15 deg of due E or due N "
                "(isotropic would be 33.3%%)\n",
                100.0 * cardinalLenM / std::max(1e-9, totalEdgeLenM));

    // ROUTING lock -- a different and stronger statement than the segment
    // histogram above, and the one the plan's tell #2 is actually about
    // ("backbone rows are unbroken west-east corridors every 102.4 m").
    // Measured as the net displacement heading of a run of 8 consecutive
    // backbone edges: both endpoints are lattice nodes 8 cells apart on one
    // axis, so jitter can only ever perturb the net heading by the jitter
    // bound over 204.8 m of run. This is worth reporting separately because
    // NOTHING that keeps edges anchored on their nodes can move it -- see the
    // note in the summary below.
    {
        const int64_t chain = 8;
        const int64_t li0 = floorDiv(x0 * kVoxelSizeMm, kCaveLatticeMm);
        const int64_t lj0 = floorDiv(y0 * kVoxelSizeMm, kCaveLatticeMm);
        const int64_t li1 = floorDiv((x0 + spanVx) * kVoxelSizeMm, kCaveLatticeMm);
        const int64_t lj1 = floorDiv((y0 + spanVx) * kVoxelSizeMm, kCaveLatticeMm);
        double worstDeg = 0.0, sumDeg = 0.0;
        int64_t chains = 0;
        for (int64_t j = lj0; j <= lj1; ++j)
            for (int64_t i = li0; i <= li1; ++i)
                for (int32_t d = 0; d < 2; ++d) {
                    // Backbone runs only: +x along a backbone row, +y along a
                    // backbone column. These are the guaranteed-unbroken ones.
                    if (d == 0 && (j & kCaveBackboneMask) != 0) continue;
                    if (d == 1 && (i & kCaveBackboneMask) != 0) continue;
                    const CaveNode a = caveNode(seed, i, j);
                    const CaveNode b = (d == 0) ? caveNode(seed, i + chain, j)
                                                : caveNode(seed, i, j + chain);
                    const double dx = static_cast<double>(b.xMm - a.xMm);
                    const double dy = static_cast<double>(b.yMm - a.yMm);
                    double deg = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
                    if (deg < 0) deg += 180.0;
                    const double off =
                        std::min(std::min(deg, 180.0 - deg), std::abs(deg - 90.0));
                    worstDeg = std::max(worstDeg, off);
                    sumDeg += off;
                    ++chains;
                }
        if (chains)
            std::printf("ROUTING lock: over %lld backbone runs of %lld cells (%.0f m), net "
                        "heading departs from due E/N by %.2f deg on average, %.2f deg worst "
                        "-- the network runs grid-cardinal at range whatever the segments do\n",
                        (long long)chains, (long long)chain,
                        static_cast<double>(chain * kCaveLatticeMm) / 1000.0, sumDeg / chains,
                        worstDeg);
    }
    std::printf("total tunnel axis length in region: %.0f m (%.0f m per km^2)\n", totalEdgeLenM,
                totalEdgeLenM / std::max(1e-9, regionKm2));

    std::printf("\n--- generator sites in region (what W3/W4 must re-provide) ---\n");
    std::printf("shaft nodes : %lld open of %lld candidates (%.0f%%) = %.1f entrances/km^2, "
                "one per %.0f m square\n",
                (long long)shaftOpen, (long long)shaftCandidates,
                100.0 * shaftOpen / std::max<int64_t>(1, shaftCandidates),
                shaftOpen / std::max(1e-9, regionKm2),
                shaftOpen ? std::sqrt(1e6 * regionKm2 / static_cast<double>(shaftOpen)) : 0.0);
    std::printf("cavern sites: %lld open of %lld candidates (%.0f%%) = %.1f sites/km^2\n",
                (long long)cavernOpen, (long long)cavernCandidates,
                100.0 * cavernOpen / std::max<int64_t>(1, cavernCandidates),
                cavernOpen / std::max(1e-9, regionKm2));

    // The site list, in world metres. This is what points a capture run at a
    // real entrance instead of hunting for one: -VoxelGICaveTest searches
    // outward from spawn and reports where it landed, which is fine for one
    // shot and useless for an A/B pair that has to be the SAME hole before and
    // after. Feed these straight to -VoxelSpawnAt.
    std::printf("\n--- entrance and cavern sites, world metres (feed to -VoxelSpawnAt) ---\n");
    {
        const int64_t li0 = floorDiv(x0 * kVoxelSizeMm, kCaveLatticeMm);
        const int64_t lj0 = floorDiv(y0 * kVoxelSizeMm, kCaveLatticeMm);
        const int64_t li1 = floorDiv((x0 + spanVx) * kVoxelSizeMm, kCaveLatticeMm);
        const int64_t lj1 = floorDiv((y0 + spanVx) * kVoxelSizeMm, kCaveLatticeMm);
        int listed = 0;
        for (int64_t j = lj0 - 1; j <= lj1 + 1 && listed < 24; ++j)
            for (int64_t i = li0 - 1; i <= li1 + 1 && listed < 24; ++i) {
                const CaveNode probeNode = caveNode(seed, i, j);
                if (probeNode.xMm < x0 * kVoxelSizeMm ||
                    probeNode.xMm >= (x0 + spanVx) * kVoxelSizeMm ||
                    probeNode.yMm < y0 * kVoxelSizeMm ||
                    probeNode.yMm >= (y0 + spanVx) * kVoxelSizeMm)
                    continue; // jittered out of the census region
                const bool isShaft =
                    (i & kCaveShaftNodeMask) == 0 && (j & kCaveShaftNodeMask) == 0 &&
                    ((hash2(seed, i, j, CH_CAVE_SHAFT) >> 48) & kCaveShaftGateMask) == 0;
                const bool isCavern = (i & (kCavernCoarseLatticeRatio - 1)) == 0 &&
                                      (j & (kCavernCoarseLatticeRatio - 1)) == 0 &&
                                      cavernSiteGateOpen(seed, i, j) &&
                                      cavernDepthIsSafe(caveNode(seed, i, j).depthMm);
                if (!isShaft && !isCavern) continue;
                const CaveNode nd = caveNode(seed, i, j);
                const int64_t nvx = floorDiv(nd.xMm, static_cast<int64_t>(kVoxelSizeMm));
                const int64_t nvy = floorDiv(nd.yMm, static_cast<int64_t>(kVoxelSizeMm));
                const ColumnSample nc = amp.column(nvx, nvy);
                // Does the shaft actually break the surface here? The gate is
                // necessary, not sufficient -- kCaveMinSurfaceMm excludes low
                // ground outright -- and that distinction is the whole reason
                // this line exists rather than a bare site count.
                const bool breaks =
                    isShaft && nc.cave.shaftMarginSq > 0 &&
                    Amplifier::materialAt(
                        nc, floorDiv(nc.surfaceMm - kVoxelSizeMm / 2, kVoxelSizeMm)) == MAT_AIR;
                std::printf("  %-9s lattice(%lld,%lld) at (%.1f, %.1f) m  surface %.1f m  "
                            "node depth %.1f m%s\n",
                            isShaft ? (isCavern ? "shaft+cav" : "shaft") : "cavern", (long long)i,
                            (long long)j, nd.xMm / 1000.0, nd.yMm / 1000.0, nc.surfaceMm / 1000.0,
                            nd.depthMm / 1000.0,
                            isShaft ? (breaks ? "   [breaks surface]" : "   [GATED OPEN BUT SEALED]")
                                    : "");
                // CHAMBER SHAPE PER SITE (v26, plan W4) -- so a capture or a
                // vxc_gpu fixture is aimed at a chamber whose shape has been
                // READ rather than assumed. Three of nine vista sites picked
                // by eye were wrong once already; the same discipline the
                // entrance shape lines above exist for applies here, and it
                // matters more, because a third of sites have no pillars at
                // all and a fixture placed on one of those would test the
                // pillar term by not containing it.
                if (isCavern) {
                    const CavernSite cs = cavernSiteFor(seed, i, j, nd, amp.surfaceAtFn());
                    if (cs.valid) {
                        std::printf("             chamber: anchor z %.1f m; pillars r=%.1f m%s; "
                                    "rubble amp %.2f m%s\n",
                                    cs.anchorZMm / 1000.0, cs.pillarRadiusMm / 1000.0,
                                    cs.pillarRadiusMm == 0 ? " [THIS SITE HAS NONE]" : "",
                                    cs.breakdownAmpMm / 1000.0,
                                    cs.breakdownAmpMm == 0 ? " [flat floors]" : "");
                        for (int32_t c = 0; c < kCavernChildCount; ++c) {
                            const CavernChild& ch = cs.children[c];
                            const double offX = (ch.xMm - cs.anchorXMm) / 1000.0;
                            const double offY = (ch.yMm - cs.anchorYMm) / 1000.0;
                            std::printf("               room %d: axis %+6.1f,%+6.1f m from "
                                        "anchor  z %.1f m  long r %.1f m  short r %.1f m  "
                                        "elong %.2f:1  floor %.1f m\n",
                                        c, offX, offY, ch.zMm / 1000.0, ch.rxyMm / 1000.0,
                                        ch.rxyMm * 1024.0 / ch.elongQ10 / 1000.0,
                                        ch.elongQ10 / 1024.0, ch.zFloorMm / 1000.0);
                        }
                        // A vxc_gpu FIXTURE ORIGIN, chosen by SEARCH rather
                        // than by eye -- the discipline gpu_harness.cpp's own
                        // fixture comments demand, and the one the cave-band
                        // fixture skipped when it asserted in a comment that a
                        // gated-open sinkhole was inside it and never checked.
                        //
                        // What makes a cavern fixture good is not "sits on the
                        // chamber": a block entirely inside one room compares
                        // only the interior and would pass with the wall test,
                        // the pillar term and the elongation all wrong. So the
                        // score is the distance from a HALF-AND-HALF split of
                        // the block's columns, which forces both sides of the
                        // chamber boundary into the compared volume, with a
                        // hard requirement that the block also contain at
                        // least one PILLAR column (a column the chamber would
                        // otherwise carve and the pillar field turns to rock).
                        const int64_t kFixN = 96; // 9.6 m, the dispatch width
                        int64_t bestVx = 0, bestVy = 0, bestScore = -1, bestCav = 0, bestPil = 0;
                        const int64_t aVx = floorDiv(cs.anchorXMm, (int64_t)kVoxelSizeMm);
                        const int64_t aVy = floorDiv(cs.anchorYMm, (int64_t)kVoxelSizeMm);
                        const int64_t sI = floorDiv(cs.anchorXMm, kCavernCoarseMm);
                        const int64_t sJ = floorDiv(cs.anchorYMm, kCavernCoarseMm);
                        const CavernCandidates fcands = cavernCandidatesFor(seed, sI, sJ);
                        for (int64_t oy = -400; oy <= 400; oy += 40)
                            for (int64_t ox = -400; ox <= 400; ox += 40) {
                                int64_t cav = 0, pil = 0;
                                for (int64_t qy = 0; qy < kFixN; qy += 6)
                                    for (int64_t qx = 0; qx < kFixN; qx += 6) {
                                        const int64_t cvx = aVx + ox + qx;
                                        const int64_t cvy = aVy + oy + qy;
                                        const ColumnSample q = amp.column(cvx, cvy);
                                        bool anyReal = false;
                                        for (int32_t sg = 0; sg < q.cavern.count; ++sg) {
                                            const int64_t qz = floorDiv(
                                                (int64_t)q.cavern.segs[sg].zCenterMm -
                                                    kVoxelSizeMm / 2,
                                                (int64_t)kVoxelSizeMm);
                                            if (cavernCarveAt(q.cavern, q.surfaceMm,
                                                              q.bedrockDepthMm, qz))
                                                anyReal = true;
                                        }
                                        if (anyReal) { ++cav; continue; }
                                        const CavernColumn pc = cavernColumnWithoutChamberShape(
                                            seed, fcands, cvx, cvy, amp.surfaceAtFn());
                                        for (int32_t sg = 0; sg < pc.count; ++sg) {
                                            const int64_t qz = floorDiv(
                                                (int64_t)pc.segs[sg].zCenterMm - kVoxelSizeMm / 2,
                                                (int64_t)kVoxelSizeMm);
                                            if (cavernCarveAt(pc, q.surfaceMm, q.bedrockDepthMm,
                                                              qz)) {
                                                ++pil;
                                                break;
                                            }
                                        }
                                    }
                                const int64_t total = (kFixN / 6) * (kFixN / 6);
                                if (cav == 0 || pil == 0) continue;
                                const int64_t score = total - std::llabs(2 * cav - total);
                                if (score > bestScore) {
                                    bestScore = score;
                                    bestVx = aVx + ox;
                                    bestVy = aVy + oy;
                                    bestCav = cav;
                                    bestPil = pil;
                                }
                            }
                        if (bestScore >= 0)
                            std::printf("               vxc_gpu fixture: origin voxel (%lld, "
                                        "%lld), %lldx%lld columns -- %lld%% of a decimated "
                                        "sample is chamber and %lld sample(s) are pillar rock\n",
                                        (long long)bestVx, (long long)bestVy, (long long)kFixN,
                                        (long long)kFixN,
                                        (long long)(100 * bestCav / ((kFixN / 6) * (kFixN / 6))),
                                        (long long)bestPil);
                        else
                            std::printf("               vxc_gpu fixture: none found near this "
                                        "site (no block has both chamber and pillar rock)\n");
                    }
                }
                // ENTRANCE SHAPE PER SITE (v25) -- so a capture is aimed at
                // a site whose type has been VERIFIED rather than assumed.
                // Three of nine vista sites picked by eye were wrong once
                // already, and a mislabelled capture is worse than none
                // because it becomes evidence in the owner's judgement.
                //
                // THE DISCRIMINATOR, AND THE ONE THAT DID NOT WORK. The
                // first version of this classified a "horizontal mouth" as
                // a column whose entrance roof is thinner than the 6 m roof
                // clamp. That metric is CONTAMINATED and it briefly had me
                // reporting 55% of every footprint as mouth: near the rim
                // the lens roof pinches to nothing, so the cover there is
                // just the cavity FLOOR DEPTH -- drawn from [5, 9) m -- and
                // dips under a 6 m clamp on dead flat ground for more than
                // half of all sites. It measures the lens, not the hill.
                //
                // What actually separates a hillside mouth from a doline is
                // WHICH SURFACE CLIPPED THE ROOF. The cavity is open to the
                // sky wherever the ground clipped it and roofed wherever
                // its own lens did, so the shape of the OPEN set is the
                // answer: on flat ground it is a compact disc around the
                // axis, strictly interior to the footprint; on falling
                // ground it is pushed off-axis and reaches the footprint
                // RIM, because that is where the hill has cut the roof away
                // entirely. So: centroid offset of the open set, and how
                // much of it sits in the outer fifth of the footprint.
                if (isShaft) {
                    int64_t fpCols = 0, openCols = 0, roofCols = 0;
                    int64_t openXMm = 0, openYMm = 0;
                    int64_t fpMaxRSqMm = 0, openMaxRSqMm = 0;
                    int32_t openMinSurfMm = INT32_MAX, openMaxSurfMm = INT32_MIN;
                    // Two passes: the footprint radius has to be known
                    // before "outer fifth" means anything.
                    for (int64_t dy = -160; dy <= 160; dy += 2)
                        for (int64_t dx = -160; dx <= 160; dx += 2) {
                            const ColumnSample fc = amp.column(nvx + dx, nvy + dy);
                            if (fc.cave.shaftMarginSq <= 0) continue;
                            const int64_t rSq = (dx * dx + dy * dy) * kVoxelSizeMm * kVoxelSizeMm;
                            if (rSq > fpMaxRSqMm) fpMaxRSqMm = rSq;
                            ++fpCols;
                            if (fc.cave.shaftDepthMinMm > 0) { ++roofCols; continue; }
                            ++openCols;
                            openXMm += dx * kVoxelSizeMm;
                            openYMm += dy * kVoxelSizeMm;
                            if (rSq > openMaxRSqMm) openMaxRSqMm = rSq;
                            if (fc.surfaceMm < openMinSurfMm) openMinSurfMm = fc.surfaceMm;
                            if (fc.surfaceMm > openMaxSurfMm) openMaxSurfMm = fc.surfaceMm;
                        }
                    const double fpRM = std::sqrt(double(fpMaxRSqMm)) / 1000.0;
                    const double openRM = std::sqrt(double(openMaxRSqMm)) / 1000.0;
                    int64_t rimOpen = 0;
                    if (fpMaxRSqMm > 0)
                        for (int64_t dy = -160; dy <= 160; dy += 2)
                            for (int64_t dx = -160; dx <= 160; dx += 2) {
                                const ColumnSample fc = amp.column(nvx + dx, nvy + dy);
                                if (fc.cave.shaftMarginSq <= 0) continue;
                                if (fc.cave.shaftDepthMinMm > 0) continue;
                                const int64_t rSq =
                                    (dx * dx + dy * dy) * kVoxelSizeMm * kVoxelSizeMm;
                                if (rSq * 25 >= fpMaxRSqMm * 16) ++rimOpen; // r >= 0.8 R
                            }
                    const double cxM =
                        openCols ? double(openXMm) / double(openCols) / 1000.0 : 0.0;
                    const double cyM =
                        openCols ? double(openYMm) / double(openCols) / 1000.0 : 0.0;
                    const double offM = std::sqrt(cxM * cxM + cyM * cyM);
                    const bool mouth = rimOpen > 0;
                    std::printf("             shape: %s -- footprint r=%.1f m (%lld cols), "
                                "open %lld / roofed %lld; open set centroid (%+.1f, %+.1f) m "
                                "off-axis %.1f m, reaches r=%.1f m, %lld col(s) in the outer "
                                "fifth; ground across the opening %.1f..%.1f m\n",
                                mouth ? "MOUTH (opens at the rim)" : "doline (opens on axis)",
                                fpRM, (long long)fpCols, (long long)openCols,
                                (long long)roofCols, cxM, cyM, offM, openRM,
                                (long long)rimOpen,
                                openMinSurfMm == INT32_MAX ? 0.0 : openMinSurfMm / 1000.0,
                                openMaxSurfMm == INT32_MIN ? 0.0 : openMaxSurfMm / 1000.0);
                    // The capture path offers ONE yaw (+X: VoxelEarthGameMode
                    // fixes it so two shots of a column differ only in the
                    // quantity being varied), so a mouth is shootable head-on
                    // only when its opening is displaced toward -X.
                    if (mouth && cxM < -0.5)
                        std::printf("             SHOOTABLE at yaw 0: -VoxelSpawnAt=%.1f,%.1f (METRES) "
                                    "looking +X from %.1f m west\n",
                                    nd.xMm / 1000.0 + 3.0 * cxM,
                                    nd.yMm / 1000.0 + 3.0 * cyM, -3.0 * cxM);
                }
                ++listed;
            }
        if (!listed) std::printf("  (none in this region)\n");
    }

    if (ranConnectivity) {
        std::printf("\n--- connectivity (%lldx%lld sample box at 0.4 m) ---\n", (long long)connectN,
                    (long long)connectN);
        std::printf("cave-only flood fill: components=%d  cave samples=%zu  largest=%zu (%.2f%%)\n",
                    compCount, compTotal, compLargest, 100.0 * largestShare);
        std::printf("open sinkhole nodes inside the box: %lld;  portal samples (cave air touching "
                    "open air): %lld\n",
                    (long long)shaftsInConnectBox, (long long)portalSeeds);
        std::printf("REACHABLE FROM OPEN SKY WITHOUT DIGGING: %zu of %zu cave samples (%.2f%%)%s\n",
                    skyReachable, compTotal, 100.0 * skyShare,
                    (skyReachable == 0 && shaftsInConnectBox == 0)
                        ? "   <-- no entrance in the box; this is the box, not the world"
                        : "");
    }

    // A digest over the census so two runs can be compared in one line.
    Digest d;
    d.u64(static_cast<uint64_t>(carvedVox));
    d.u64(static_cast<uint64_t>(solidBandVox));
    for (uint32_t f = 0; f < kCaveFamilyCount; ++f) {
        d.u64(static_cast<uint64_t>(famVox[f]));
        d.u64(static_cast<uint64_t>(famCols[f]));
        d.u64(static_cast<uint64_t>(floorSpots[f]));
    }
    d.u64(static_cast<uint64_t>(perfColsAny));
    d.u64(static_cast<uint64_t>(mouthCols));
    d.u64(static_cast<uint64_t>(entCols));
    d.u64(static_cast<uint64_t>(entRoofedCols));
    std::printf("\ncaveprobe census digest=%016llx  (kWorldGenVersion=%u)\n",
                (unsigned long long)d.h, kWorldGenVersion);
    if (attrMismatch || memoMismatch) return 1;
    return 0;
}
