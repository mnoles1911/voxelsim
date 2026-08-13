// vxc_windprobe -- the v0 weather field (voxelcore/weather.h), printed.
//
// WHY IT EXISTS. The wind is a pure function of (seed, x, y, t) that nothing
// stores, so the only way to see it is to evaluate it. Inside the editor that
// means launching Unreal, waiting for terrain to stream and reading a HUD
// line, which costs minutes and cannot be diffed. This costs a second and its
// output is text. Every claim in docs/weather-system-v0.md that has a number
// in it came from here.
//
// Usage:
//   vxc_windprobe <seed> [--x <metres>] [--y <metres>]
//                        [--hours <h>] [--step <seconds>]
//                        [--pin-from <deg>] [--pin-speed <m/s>]
//                        [--quiet]
//
//   --x/--y      where the time series is taken. Default 0,0.
//   --hours      how much CLOCK to walk, in hours of the game clock (i.e. of
//                UVoxelSkySubsystem's EpochSeconds). Default 3.
//   --step       spacing of the time series, seconds. Default 300.
//   --pin-from   pin the bearing the wind comes FROM, degrees. Runs the pin
//                section instead of taking it on trust.
//   --pin-speed  pin the speed, m/s.
//   --quiet      totals and verdicts only, no per-row output.
//
// Exit codes follow the house convention: 2 = bad arguments, 1 = ran but will
// not report, 0 = reported.
//
// THE REFUSALS ARE THE POINT OF THE PROBE, not decoration on it. This project
// has a standing rule (docs/water-architecture.md:143) that every stage must
// write something that distinguishes "ran and found nothing" from "did not
// run", because three absent-stat zeros produced three false conclusions in
// one session. A wind field whose noise is dead -- wrong seed derivation,
// a channel that collides with itself, a lattice of zero -- returns a
// perfectly plausible constant 6.0 m/s from the west, forever, and every
// individual number it prints looks right. So this probe measures its own
// spread and refuses to print a verdict when there is none.
//
// WHAT IT FOUND (2026-08-12, first run of the v0 field, seed 20260719).
// Recorded here so the probe stays re-runnable rather than being a one-off:
//   * speed at a fixed point over 3 h of clock spans 1.8 .. 11.8 m/s, median
//     5.9, which is Beaufort 1 to 6 -- the intended range.
//   * two points 2048 m apart at one instant differ by a median 1.45 m/s, so
//     the place-to-place requirement is met with room to spare.
//   * two points 400 m apart differ by a median 0.65 m/s, which is the number
//     that justifies v0 publishing ONE camera sample to the water material
//     rather than a texture; see the design doc's sampling section.
//   * the field moves 0.023 m/s and 0.21 deg per 16 ms frame at worst, i.e.
//     smoothly.
//   * the same run BEFORE the advection fix (weather.h's lever-arm note)
//     jumped 20.4 deg in one frame. That is what this probe is for.

#include "voxelcore/weather.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace vxc;

namespace {

// Bench is outside the float ban (.github/workflows/ci.yml:77-104 greps
// voxel-core/include and /src only, and climateprobe.cpp:59-62 makes the same
// choice for the same reason): a diagnostic that reports percentiles wants
// real division. Nothing here feeds the field -- every number handed to
// sampleWind is an integer.
double mps(int32_t mmPerS) { return mmPerS / 1000.0; }
double deg(int32_t milliDeg) { return milliDeg / 1000.0; }

int64_t percentile(std::vector<int64_t>& v, int pct) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(static_cast<int64_t>(v.size() - 1) * pct / 100);
    return v[i];
}

void usage() {
    std::fprintf(stderr,
                 "usage: vxc_windprobe <seed> [--x <m>] [--y <m>] [--hours <h>] "
                 "[--step <s>] [--pin-from <deg>] [--pin-speed <m/s>] [--quiet]\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const uint64_t seed = std::strtoull(argv[1], nullptr, 10);

    int64_t xM = 0, yM = 0;
    int64_t hours = 3;
    int64_t stepS = 300;
    int64_t pinFromDeg = -1;
    int64_t pinSpeedMilliMps = -1;
    bool quiet = false;

    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--x") && i + 1 < argc) {
            xM = std::strtoll(argv[++i], nullptr, 10);
        } else if (!std::strcmp(a, "--y") && i + 1 < argc) {
            yM = std::strtoll(argv[++i], nullptr, 10);
        } else if (!std::strcmp(a, "--hours") && i + 1 < argc) {
            hours = std::strtoll(argv[++i], nullptr, 10);
        } else if (!std::strcmp(a, "--step") && i + 1 < argc) {
            stepS = std::strtoll(argv[++i], nullptr, 10);
        } else if (!std::strcmp(a, "--pin-from") && i + 1 < argc) {
            pinFromDeg = std::strtoll(argv[++i], nullptr, 10);
        } else if (!std::strcmp(a, "--pin-speed") && i + 1 < argc) {
            // Accepted in m/s, carried in milli-m/s so a fractional pin works
            // without this file inventing a second unit.
            pinSpeedMilliMps = static_cast<int64_t>(std::strtod(argv[++i], nullptr) * 1000.0);
        } else if (!std::strcmp(a, "--quiet")) {
            quiet = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a);
            usage();
            return 2;
        }
    }
    if (stepS <= 0 || hours <= 0) {
        std::fprintf(stderr, "--hours and --step must both be positive\n");
        return 2;
    }

    WindParams p{};
    const bool pinned = (pinFromDeg >= 0) || (pinSpeedMilliMps >= 0);
    if (pinFromDeg >= 0) {
        p.pinFromBearingMilliDeg =
            static_cast<int32_t>(floorMod(pinFromDeg * 1000, kWindTurnMilliDeg));
    }
    if (pinSpeedMilliMps >= 0) {
        p.pinSpeedMmPerS = static_cast<int32_t>(pinSpeedMilliMps);
    }

    const int64_t x0Mm = xM * 1000;
    const int64_t y0Mm = yM * 1000;

    // ------------------------------------------------------------------
    std::printf("=== CONFIG (every number the field used, so the readout is self-describing) ===\n");
    std::printf("  world seed          : %" PRIu64 "\n", seed);
    std::printf("  derived wind seed   : 0x%016" PRIX64 "  (windSeed = splitmix64(world ^ salt))\n",
                windSeed(seed));
    std::printf("  sample point        : x=%" PRId64 " m  y=%" PRId64 " m\n", xM, yM);
    std::printf("  clock walked        : %" PRId64 " h at %" PRId64 " s steps\n", hours, stepS);
    std::printf("  base bearing FROM   : %.1f deg (%s)   base speed %.2f m/s\n",
                deg(p.baseFromBearingMilliDeg), windCompass16(p.baseFromBearingMilliDeg),
                mps(p.baseSpeedMmPerS));
    std::printf("  regime              : %+.1f deg over %" PRId64 " s\n",
                deg(p.regimeTurnMilliDeg), p.regimeLatticeMs / 1000);
    std::printf("  prevail             : %+.1f deg over %" PRId64 " s\n",
                deg(p.prevailTurnMilliDeg), p.prevailLatticeMs / 1000);
    std::printf("  synoptic            : %+.1f deg, speed x%.2f, over %" PRId64 " m / %" PRId64 " s\n",
                deg(p.synopticTurnMilliDeg),
                static_cast<double>(p.synopticSpeedSpanQ) / static_cast<double>(kWindQ),
                p.synopticLatticeMm / 1000, p.synopticLatticeMs / 1000);
    std::printf("  gust                : %+.1f deg, %.0f%% of sustained, over %" PRId64
                " m / %" PRId64 " s\n",
                deg(p.gustTurnMilliDeg),
                100.0 * static_cast<double>(p.gustFractionQ) / static_cast<double>(kWindQ),
                p.gustLatticeMm / 1000, p.gustLatticeMs / 1000);
    std::printf("  advection           : x%.2f of base speed, along the FIXED base bearing\n",
                static_cast<double>(p.advectionGainQ) / static_cast<double>(kWindQ));
    std::printf("  pins                : bearing %s   speed %s\n",
                p.pinFromBearingMilliDeg >= 0 ? "PINNED" : "derived",
                p.pinSpeedMmPerS >= 0 ? "PINNED" : "derived");

    // ------------------------------------------------------------------
    // 1. TIME SERIES at one place. "Does it move plausibly where I stand."
    // ------------------------------------------------------------------
    const int64_t totalMs = hours * 3'600'000;
    const int64_t stepMs = stepS * 1000;
    std::vector<int64_t> speeds;
    std::vector<int64_t> gustAbs;
    int64_t rows = 0;

    if (!quiet) {
        std::printf("\n=== TIME SERIES at (%" PRId64 ", %" PRId64 ") m ===\n", xM, yM);
        std::printf("  %10s  %8s  %10s  %8s  %8s  %s\n", "clock s", "speed", "from", "sustain",
                    "gust", "quarter");
    }
    for (int64_t t = 0; t <= totalMs; t += stepMs) {
        const WindSample w = sampleWind(seed, x0Mm, y0Mm, t, p);
        speeds.push_back(w.speedMmPerS);
        gustAbs.push_back(w.gustMmPerS < 0 ? -w.gustMmPerS : w.gustMmPerS);
        ++rows;
        if (!quiet) {
            std::printf("  %10" PRId64 "  %6.2f m/s  %7.1f deg  %6.2f  %+6.2f  %s\n", t / 1000,
                        mps(w.speedMmPerS), deg(w.fromBearingMilliDeg), mps(w.sustainedMmPerS),
                        mps(w.gustMmPerS), windCompass16(w.fromBearingMilliDeg));
        }
    }

    // ------------------------------------------------------------------
    // 2. SPATIAL CENSUS at one instant. THE requirement, measured.
    // ------------------------------------------------------------------
    // A 17x17 grid at 2048 m spacing -- one synoptic cell per step, so the
    // grid spans 32.8 km, about eight times the drawn world. Taken at the
    // MIDDLE of the walked interval rather than at t=0, because t=0 is the one
    // moment where the advection offset is exactly zero and therefore the one
    // moment that is not representative.
    const int64_t tMid = totalMs / 2;
    constexpr int kGrid = 17;
    std::vector<int64_t> gridSpeeds;
    int sectorHits[16] = {};
    for (int gy = 0; gy < kGrid; ++gy) {
        for (int gx = 0; gx < kGrid; ++gx) {
            const int64_t sx = x0Mm + (gx - kGrid / 2) * p.synopticLatticeMm;
            const int64_t sy = y0Mm + (gy - kGrid / 2) * p.synopticLatticeMm;
            const WindSample w = sampleWind(seed, sx, sy, tMid, p);
            gridSpeeds.push_back(w.speedMmPerS);
            const int64_t b = floorMod(static_cast<int64_t>(w.fromBearingMilliDeg) + 11'250,
                                       kWindTurnMilliDeg);
            ++sectorHits[b / 22'500];
        }
    }

    // 3. NEIGHBOUR DIFFERENCES at three separations. This is the table the
    //    "one camera sample or a texture?" decision rests on: it says how
    //    wrong a single published value is for water N metres away.
    struct SepRow {
        const char* label;
        int64_t sepMm;
        std::vector<int64_t> diffs;
    };
    SepRow seps[] = {
        {"    26 m (near-field water reach)", 25'600, {}},
        {"   400 m (far lake sheet)", 400'000, {}},
        {"  2048 m (one synoptic cell)", 2'048'000, {}},
        {"  4096 m (the drawn world's edge)", 4'096'000, {}},
    };
    for (SepRow& s : seps) {
        for (int i = 0; i < 2000; ++i) {
            // Deterministic scatter, so two runs of the probe agree.
            const uint64_t h = splitmix64(static_cast<uint64_t>(i) ^ seed);
            const int64_t jx = static_cast<int64_t>(h % 2'000'000'000ull) - 1'000'000'000;
            const int64_t jy = static_cast<int64_t>(splitmix64(h) % 2'000'000'000ull) - 1'000'000'000;
            const int64_t jt = static_cast<int64_t>(splitmix64(h + 1) % static_cast<uint64_t>(totalMs + 1));
            const WindSample a = sampleWind(seed, jx, jy, jt, p);
            const WindSample b = sampleWind(seed, jx + s.sepMm, jy, jt, p);
            s.diffs.push_back(a.speedMmPerS > b.speedMmPerS ? a.speedMmPerS - b.speedMmPerS
                                                            : b.speedMmPerS - a.speedMmPerS);
        }
    }

    // 4. FRAME-TO-FRAME MOTION. The lever-arm regression guard, in probe form.
    int64_t worstFrameSpeed = 0;
    int64_t worstFrameBearing = 0;
    for (int64_t t = 0; t + 16 <= totalMs; t += 97) { // 97 ms: coprime with every lattice here
        const WindSample a = sampleWind(seed, x0Mm, y0Mm, t, p);
        const WindSample b = sampleWind(seed, x0Mm, y0Mm, t + 16, p);
        const int64_t ds = a.speedMmPerS > b.speedMmPerS ? a.speedMmPerS - b.speedMmPerS
                                                         : b.speedMmPerS - a.speedMmPerS;
        int64_t db = a.fromBearingMilliDeg > b.fromBearingMilliDeg
                         ? a.fromBearingMilliDeg - b.fromBearingMilliDeg
                         : b.fromBearingMilliDeg - a.fromBearingMilliDeg;
        if (db > 180'000) db = 360'000 - db;
        worstFrameSpeed = std::max(worstFrameSpeed, ds);
        worstFrameBearing = std::max(worstFrameBearing, db);
    }

    // ------------------------------------------------------------------
    // Report.
    // ------------------------------------------------------------------
    const int64_t sMin = percentile(speeds, 0);
    const int64_t sP50 = percentile(speeds, 50);
    const int64_t sMax = percentile(speeds, 100);
    const int64_t gMin = percentile(gridSpeeds, 0);
    const int64_t gP50 = percentile(gridSpeeds, 50);
    const int64_t gMax = percentile(gridSpeeds, 100);

    std::printf("\n=== IN TIME, at one place ===\n");
    std::printf("  samples             : %" PRId64 " over %" PRId64 " h of clock\n", rows, hours);
    std::printf("  speed               : min %.2f  p50 %.2f  max %.2f m/s   (spread %.2f)\n",
                mps(static_cast<int32_t>(sMin)), mps(static_cast<int32_t>(sP50)),
                mps(static_cast<int32_t>(sMax)), mps(static_cast<int32_t>(sMax - sMin)));
    std::printf("  |gust|              : p50 %.2f  max %.2f m/s\n",
                mps(static_cast<int32_t>(percentile(gustAbs, 50))),
                mps(static_cast<int32_t>(percentile(gustAbs, 100))));
    std::printf("  per 16 ms frame     : worst dSpeed %.4f m/s, worst dBearing %.4f deg\n",
                mps(static_cast<int32_t>(worstFrameSpeed)),
                deg(static_cast<int32_t>(worstFrameBearing)));

    std::printf("\n=== IN SPACE, at one instant (clock %" PRId64 " s, %dx%d grid at %" PRId64
                " m) ===\n",
                tMid / 1000, kGrid, kGrid, p.synopticLatticeMm / 1000);
    std::printf("  speed               : min %.2f  p50 %.2f  max %.2f m/s   (spread %.2f)\n",
                mps(static_cast<int32_t>(gMin)), mps(static_cast<int32_t>(gP50)),
                mps(static_cast<int32_t>(gMax)), mps(static_cast<int32_t>(gMax - gMin)));
    std::printf("  bearing occupancy   :");
    for (int k = 0; k < 16; ++k) {
        std::printf(" %s=%d", kWindCompass16[k], sectorHits[k]);
    }
    std::printf("\n");

    std::printf("\n=== HOW WRONG IS ONE PUBLISHED VALUE, BY DISTANCE ===\n");
    std::printf("  READ THIS AS: v0 publishes the wind AT THE CAMERA to every water surface on\n");
    std::printf("  screen. This is the error that decision accepts. Each row is |dSpeed| between\n");
    std::printf("  two points that far apart, over 2000 deterministic pairs.\n");
    std::printf("  %-36s %8s %8s %8s\n", "separation", "p50", "p95", "max");
    for (SepRow& s : seps) {
        std::printf("  %-36s %6.3f   %6.3f   %6.3f  m/s\n", s.label,
                    mps(static_cast<int32_t>(percentile(s.diffs, 50))),
                    mps(static_cast<int32_t>(percentile(s.diffs, 95))),
                    mps(static_cast<int32_t>(percentile(s.diffs, 100))));
    }

    if (pinned) {
        // A capture leg that pins the wind and does not check the pin took is
        // a capture leg that can silently compare two different winds. Same
        // rule as VoxelGpuVerify.cpp:2118-2126: report what was USED, never
        // what was asked for.
        std::printf("\n=== PIN CHECK ===\n");
        bool held = true;
        for (int i = 0; i < 512; ++i) {
            const uint64_t h = splitmix64(static_cast<uint64_t>(i) * 7919ull);
            const WindSample w = sampleWind(seed, static_cast<int64_t>(h % 1'000'000'000ull),
                                            static_cast<int64_t>(splitmix64(h) % 1'000'000'000ull),
                                            static_cast<int64_t>(splitmix64(h + 3) % 400'000'000ull), p);
            if (p.pinFromBearingMilliDeg >= 0 && w.fromBearingMilliDeg != p.pinFromBearingMilliDeg) {
                held = false;
            }
            if (p.pinSpeedMmPerS >= 0 && w.speedMmPerS != p.pinSpeedMmPerS) {
                held = false;
            }
        }
        std::printf("  512 samples over the whole domain: pin %s\n",
                    held ? "HELD at every one" : "*** BROKE ***");
        if (!held) {
            std::fprintf(stderr, "the pin did not hold -- a capture taken with it is not comparable\n");
            return 1;
        }
    }

    // ------------------------------------------------------------------
    // THE RAN-FLAG AND THE REFUSALS.
    // ------------------------------------------------------------------
    //
    // A wind field with dead noise prints a full page of entirely plausible
    // numbers. The only thing that distinguishes it is that nothing MOVES, so
    // that is what is tested, and the answer is stated as a line somebody can
    // grep rather than left as an exit code (tools/voxel-shore-fx-ab.ps1:184
    // and tools/voxel-underwater-captures.ps1:150-165 both record that exit 0
    // is not a ran-flag).
    std::printf("\n=== VERDICT ===\n");
    const int64_t timeSpread = sMax - sMin;
    const int64_t spaceSpread = gMax - gMin;
    std::printf("  vxc_windprobe: RAN. %" PRId64 " time samples, %d space samples, "
                "time spread %.3f m/s, space spread %.3f m/s\n",
                rows, kGrid * kGrid, mps(static_cast<int32_t>(timeSpread)),
                mps(static_cast<int32_t>(spaceSpread)));

    if (pinned) {
        std::printf("  (spreads are expected to be zero-ish under a pin; not judged)\n");
        return 0;
    }
    if (timeSpread == 0 && spaceSpread == 0) {
        std::fprintf(stderr,
                     "THE FIELD IS CONSTANT -- identical wind at every place and every moment "
                     "sampled. That is not a calm day, it is a dead noise source: check the seed "
                     "derivation (windSeed), the channel ids, and that no lattice is zero. "
                     "REFUSING TO REPORT, because a constant field and a field that never ran "
                     "print the same page.\n");
        return 1;
    }
    if (spaceSpread == 0) {
        std::fprintf(stderr,
                     "the field varies in TIME but is identical at all %d grid points -- the "
                     "synoptic band, which is the only one that depends on position, is not "
                     "contributing. That is the requirement this system exists to satisfy, so "
                     "REFUSING TO REPORT.\n",
                     kGrid * kGrid);
        return 1;
    }
    if (timeSpread == 0) {
        std::fprintf(stderr,
                     "the field varies in SPACE but not at all over %" PRId64 " h of clock -- the "
                     "regime, prevail and gust bands are not contributing. REFUSING TO REPORT.\n",
                     hours);
        return 1;
    }
    std::printf("  wind varies in both time and space. Field is live.\n");
    return 0;
}
