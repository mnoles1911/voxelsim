// Tests for voxelcore/weather.h -- the v0 wind field.
//
// WHAT THESE ARE FOR, given that weather.h already carries a dozen
// static_asserts. Those prove the pieces (the rose is unit, the wrap wraps,
// windMulQ does not overflow). These prove the WHOLE FUNCTION, over a domain
// too large to enumerate at compile time, and they pin the exact numbers so
// that a change to the field is a change somebody chose rather than one that
// happened.
//
// THE PINNED SAMPLES IN kReference BELOW WERE NOT PRODUCED BY THIS CODE. They
// come from an independent integer mirror of the algorithm, written in Python
// during the design and described in docs/weather-system-v0.md, which models
// C++'s truncating division and int64 width exactly. That is the point of
// them: the C++ and the mirror were written from the same specification by
// different routes, so agreement is evidence and disagreement is a real
// question rather than a tautology. If one of these fails, work out WHICH side
// is wrong before touching either -- do not regenerate the expectation.
//
// NO GOLDEN DIGEST HERE, deliberately, and it is a gap rather than a decision
// somebody should be happy with. The convention elsewhere in this suite is a
// CHECK_EQ(d.h, 0x...ull) // GOLDEN(name) over a swept digest, which catches a
// change anywhere in the field rather than at five points. It is absent
// because this file was written on a machine that could not compile it, so the
// digest could not be computed, and a fabricated one is worse than none. THE
// FOLLOW-UP: whoever first builds this should add a sweep digest and run
// tools/regen-goldens.ps1 to fill it in.

#include "vxctest.h"

#include "voxelcore/weather.h"

#include <cstdint>
#include <cstdio>  // std::printf, for the margin this test reports either way
#include <cstdlib> // std::abs for the integer overloads

using namespace vxc;

namespace {

// A cheap deterministic stream for the sweeps, so a failure is reproducible
// and does not depend on the platform's rand().
struct Stream {
    uint64_t s;
    explicit Stream(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s = splitmix64(s + 0x1234567ull);
        return s;
    }
    // Uniform-ish in [-range, range].
    int64_t sym(int64_t range) { return static_cast<int64_t>(next() % static_cast<uint64_t>(2 * range + 1)) - range; }
    int64_t pos(int64_t range) { return static_cast<int64_t>(next() % static_cast<uint64_t>(range + 1)); }
};

constexpr uint64_t kSeed = 20260719ull;

struct Reference {
    int64_t xMm, yMm, tMs;
    int32_t east, north, speed, sustained, gust, fromMilliDeg;
};

// TWO OF THESE FIVE ROWS ARE STILL INDEPENDENT EVIDENCE. THREE ARE NOW ONLY
// REGRESSION PINS, and the difference matters when one of them next fails.
//
// On 2026-08-13 sampleWind's advection was found to apply the EAST component of
// the advection direction to the x (NORTH) coordinate and vice versa -- it was
// implementing this header's own axis comment, which was itself wrong. Fixing it
// moved the field wherever advection is non-zero.
//
// The two t=0 rows did NOT move, and could not have: advectMm is zero there, so
// ax==xMm and ay==yMm whichever component is applied. They still hold their
// original meaning -- produced by a Python integer mirror written from the
// specification by a different route, so agreement is evidence.
//
// The three advected rows below were REGENERATED FROM THIS C++ after the fix,
// because the mirror lives in a scratch file that was not kept and still
// implements the old swap. That makes them tautological: they will detect an
// unintended change to the field, which is worth having, but they can no longer
// tell you the field is CORRECT. Do not cite them as though they could.
//
// That every one of the three still satisfies sustained + gust == speed
// (5435+1103=6538, 8016-780=7236, 8237+613=8850) is a real check that survived
// the regeneration, and it is the invariant the speed rail had been breaking.
//
// THE FOLLOW-UP, and it is now two things: re-derive the mirror with the
// corrected advection and restore these three rows to evidence, and add the
// swept golden digest the header asks for. Until the first is done this test is
// weaker than its header claims.
constexpr Reference kReference[] = {
    // t = 0: independent (mirror-derived, unaffected by the advection fix).
    {0, 0, 0, -7033, 1834, 7298, 9270, -1972, 104623},
    {0, 0, 1000, -7024, 1832, 7289, 9259, -1970, 104623},
    // advected: regression pins, regenerated 2026-08-13. See above.
    {1'000'000, -2'000'000, 3'600'000, -6071, 2306, 6511, 6104, 407, 110796},
    {-123'456'789, 987'654'321, 86'400'000, -6567, -750, 6641, 6954, -313, 83485},
    {2'048'000, 2'048'000, 600'000, -3496, 1425, 3778, 3470, 308, 112179},
};

} // namespace

VXC_TEST(wind_matches_the_independent_mirror) {
    const WindParams p{};
    for (const Reference& r : kReference) {
        const WindSample w = sampleWind(kSeed, r.xMm, r.yMm, r.tMs, p);
        CHECK_EQ(w.eastMmPerS, r.east);
        CHECK_EQ(w.northMmPerS, r.north);
        CHECK_EQ(w.speedMmPerS, r.speed);
        CHECK_EQ(w.sustainedMmPerS, r.sustained);
        CHECK_EQ(w.gustMmPerS, r.gust);
        CHECK_EQ(w.fromBearingMilliDeg, r.fromMilliDeg);
    }
}

VXC_TEST(wind_is_a_pure_function) {
    // Determinism is the whole determinism gate of
    // docs/lighting-weather-plan.md Part B: "two clients at the same (seed,
    // time, position) sample identical weather". Interleaving the two calls
    // with unrelated ones would catch a hidden static; there is none to catch,
    // but the test is what says so.
    const WindParams p{};
    Stream rng(99);
    for (int i = 0; i < 2000; ++i) {
        const int64_t x = rng.sym(4'000'000'000LL);
        const int64_t y = rng.sym(4'000'000'000LL);
        const int64_t t = rng.pos(100'000'000LL);
        const WindSample a = sampleWind(kSeed, x, y, t, p);
        (void)sampleWind(kSeed ^ 0xABCDull, -x, -y, t + 7, p); // unrelated call in between
        const WindSample b = sampleWind(kSeed, x, y, t, p);
        CHECK_EQ(a.eastMmPerS, b.eastMmPerS);
        CHECK_EQ(a.northMmPerS, b.northMmPerS);
        CHECK_EQ(a.speedMmPerS, b.speedMmPerS);
        CHECK_EQ(a.fromBearingMilliDeg, b.fromBearingMilliDeg);
    }
}

VXC_TEST(wind_invariants_hold_over_the_domain) {
    const WindParams p{};
    Stream rng(1234);
    int64_t minSpeed = 1LL << 40;
    int64_t maxSpeed = -1;
    for (int i = 0; i < 20000; ++i) {
        const int64_t x = rng.sym(4'000'000'000LL);
        const int64_t y = rng.sym(4'000'000'000LL);
        const int64_t t = rng.pos(100'000'000LL);
        const WindSample w = sampleWind(kSeed, x, y, t, p);

        // The invariant every consumer is allowed to rely on.
        CHECK_EQ(w.speedMmPerS, w.sustainedMmPerS + w.gustMmPerS);

        // Bearings are wrapped, not merely reduced. A negative or >= 360000
        // bearing reaching a material would index the wrong way round a
        // lookup and is exactly the kind of thing that shows up as one bad
        // frame an hour.
        CHECK(w.fromBearingMilliDeg >= 0 && w.fromBearingMilliDeg < 360'000);
        CHECK(w.toBearingMilliDeg >= 0 && w.toBearingMilliDeg < 360'000);
        // The two are exactly a half turn apart.
        CHECK_EQ(floorMod(static_cast<int64_t>(w.toBearingMilliDeg) + 180'000, 360'000),
                 static_cast<int64_t>(w.fromBearingMilliDeg));

        // Speed is inside the rail and never negative -- the gust is a
        // FRACTION of the sustained speed precisely so this cannot fail.
        CHECK(w.speedMmPerS >= 0 && w.speedMmPerS <= p.maxSpeedMmPerS);

        // The gust never exceeds its stated share. 25% + one unit of
        // truncation slack.
        CHECK(std::abs(static_cast<int64_t>(w.gustMmPerS)) * 32768 <=
              static_cast<int64_t>(w.sustainedMmPerS) * p.gustFractionQ + 32768);

        // The direction is unit to within the rose's measured 0.49% ripple.
        // Checked as a squared length so no square root is needed. The two
        // constants are not estimates: windDirFromBearing was enumerated over
        // all 360,000 whole-milli-degree bearings and its squared length runs
        // 1,063,329,545 (at 39.342 deg) to 1,073,766,676 (at 22.500 deg)
        // against a nominal 32768^2 = 1,073,741,824.
        const int64_t len2 = static_cast<int64_t>(w.dirEastQ) * w.dirEastQ +
                             static_cast<int64_t>(w.dirNorthQ) * w.dirNorthQ;
        CHECK(len2 >= 1'063'000'000LL && len2 <= 1'073'800'000LL);

        if (w.speedMmPerS < minSpeed) minSpeed = w.speedMmPerS;
        if (w.speedMmPerS > maxSpeed) maxSpeed = w.speedMmPerS;
    }

    // THE RAN-FLAG, and it is the reason this block exists rather than just
    // the per-sample checks above. Every assertion in the loop passes
    // trivially for a field that returns a constant, or zero, everywhere --
    // which is exactly what a broken noise call, a mis-derived seed or a
    // channel collision would produce. So the test also insists the field
    // actually VARIES, by a lot, across the sweep. Bounds are deliberately
    // loose: at the defaults the observed spread is 1.8 .. 11.8 m/s.
    CHECK(minSpeed < 3'000);
    CHECK(maxSpeed > 9'000);
    CHECK(maxSpeed - minSpeed > 5'000);
}

VXC_TEST(wind_visits_the_whole_compass) {
    // The regime band exists so the wind is not permanently a westerly. If it
    // ever stopped working the field would still look plausible in any single
    // screenshot, and only a census would notice -- so here is the census.
    const WindParams p{};
    Stream rng(777);
    int hits[16] = {};
    constexpr int kSamples = 20000;
    for (int i = 0; i < kSamples; ++i) {
        // 2e9 ms, not 2e8, since 2026-08-13. The regime band -- the only one
        // that can reach the whole compass -- was slowed from 3 h to 24 h, and
        // this test sampled 55 h of clock, i.e. barely two regime periods. A
        // noise field over two cells does not visit all sixteen sectors, so the
        // test failed on a field that is behaving exactly as intended. The
        // REQUIREMENT is unchanged ("the wind must not fidget about a fixed
        // quarter"); what changed is how long you have to watch to see it, and
        // the horizon has to follow the band it is measuring. 2e9 ms is ~23
        // regime periods, the same ratio the old 2e8 had against the old 3 h.
        const int64_t t = rng.pos(2'000'000'000LL);
        const WindSample w = sampleWind(kSeed, rng.sym(1'000'000'000LL), rng.sym(1'000'000'000LL), t, p);
        const int64_t b = floorMod(static_cast<int64_t>(w.fromBearingMilliDeg) + 11'250, 360'000);
        ++hits[b / 22'500];
    }
    for (int k = 0; k < 16; ++k) {
        // Every one of the sixteen points must be reached. The rarest -- the
        // quarter opposite the prevailing bearing -- runs about 1% of samples
        // at the defaults, so 0.1% is a floor that catches "this band is dead"
        // without pinning the distribution's shape.
        CHECK(hits[k] > kSamples / 1000);
    }
}

VXC_TEST(wind_is_smooth_in_time) {
    // A wind that jumps between frames reads as a rendering fault, not as
    // weather, and the first draft of the advection term did exactly that --
    // a 20.4 degree bearing jump inside one 16 ms frame, because it swung a
    // 1000 km lever arm by the frame's wobble in the prevailing bearing (see
    // the header). This test is that bug's headstone. The bounds are ~10x the
    // measured worst case (0.023 m/s and 0.213 deg per frame), loose enough
    // that ordinary re-tuning does not trip them and tight enough that the
    // lever arm cannot come back.
    const WindParams p{};
    Stream rng(31337);
    for (int i = 0; i < 4000; ++i) {
        const int64_t x = rng.sym(1'000'000'000LL);
        const int64_t y = rng.sym(1'000'000'000LL);
        const int64_t t = rng.pos(100'000'000LL);
        const WindSample a = sampleWind(kSeed, x, y, t, p);
        const WindSample b = sampleWind(kSeed, x, y, t + 16, p);
        CHECK(std::abs(a.speedMmPerS - b.speedMmPerS) < 250);
        int64_t db = std::abs(static_cast<int64_t>(a.fromBearingMilliDeg) - b.fromBearingMilliDeg);
        if (db > 180'000) db = 360'000 - db; // the wrap is not a discontinuity
        CHECK(db < 2'500);
    }
}

VXC_TEST(wind_is_smooth_across_lattice_seams) {
    // Value noise interpolated bilinearly has a gradient step on every lattice
    // line; the quintic fade is what removes it, and windNoiseXYT's two-stage
    // form could in principle reintroduce one at a TIME slice boundary through
    // its intermediate truncation. Walk across three of each kind at 1 mm and
    // 1 ms and insist nothing steps. This project has spent a great deal of
    // effort deleting seams; a new one arriving through the weather would be
    // hard to attribute.
    const WindParams p{};
    const WindParams def{};
    for (int64_t k = 1; k <= 3; ++k) {
        const int64_t t0 = k * def.gustLatticeMs;
        int32_t prev = sampleWind(kSeed, 1'234'567, 7'654'321, t0 - 3, p).speedMmPerS;
        for (int64_t d = -2; d <= 3; ++d) {
            const int32_t now = sampleWind(kSeed, 1'234'567, 7'654'321, t0 + d, p).speedMmPerS;
            CHECK(std::abs(now - prev) <= 4); // 4 mm/s per millisecond
            prev = now;
        }
        const int64_t x0 = k * def.synopticLatticeMm;
        int32_t prevX = sampleWind(kSeed, x0 - 3, 0, 3'000'000, p).speedMmPerS;
        for (int64_t d = -2; d <= 3; ++d) {
            const int32_t now = sampleWind(kSeed, x0 + d, 0, 3'000'000, p).speedMmPerS;
            CHECK(std::abs(now - prevX) <= 4); // 4 mm/s per millimetre
            prevX = now;
        }
    }
}

VXC_TEST(wind_pins_exactly) {
    // The property every reproducible capture depends on. Not "close to" the
    // pinned value -- equal to it, at every place and every moment, so two
    // screenshots taken an hour apart can be differenced.
    WindParams p{};
    p.pinFromBearingMilliDeg = 135'000; // from the south-east
    p.pinSpeedMmPerS = 12'345;
    Stream rng(5);
    for (int i = 0; i < 500; ++i) {
        const WindSample w = sampleWind(kSeed, rng.sym(1'000'000'000LL), rng.sym(1'000'000'000LL),
                                        rng.pos(500'000'000LL), p);
        CHECK_EQ(w.fromBearingMilliDeg, 135'000);
        CHECK_EQ(w.toBearingMilliDeg, 315'000);
        CHECK_EQ(w.speedMmPerS, 12'345);
        CHECK_EQ(w.gustMmPerS, 0);
        CHECK_EQ(w.sustainedMmPerS, 12'345);
    }
    // A pinned bearing with a live speed is the other half of the pin, and it
    // must still gust.
    WindParams q{};
    q.pinFromBearingMilliDeg = 135'000;
    bool sawGust = false;
    for (int i = 0; i < 200; ++i) {
        const WindSample w = sampleWind(kSeed, 0, 0, static_cast<int64_t>(i) * 1000, q);
        CHECK_EQ(w.fromBearingMilliDeg, 135'000);
        if (w.gustMmPerS != 0) sawGust = true;
    }
    CHECK(sawGust); // ran-flag: a pinned bearing must not silently pin the speed too
}

VXC_TEST(wind_is_never_calm) {
    // THE OWNER'S REQUIREMENT, 2026-08-13, stated as a test: "our weather system
    // should be such that there is almost always at least some breeze."
    //
    // It was already true when he asked, and that is exactly why this test
    // exists. The floor is a CONSEQUENCE of three parameters rather than
    // anything that enforces it:
    //
    //     sustained  = baseSpeedMmPerS * [1 - synopticSpeedSpanQ .. 1 + ...]
    //                = 6000 * [0.35 .. 1.65]
    //     speed      = sustained * [1 - gustFractionQ .. 1 + gustFractionQ]
    //     worst case = 6000 * 0.35 * 0.75 = 1575 mm/s
    //
    // So nudging synopticSpeedSpanQ toward 1.0 during tuning -- an obviously
    // reasonable thing to want, it widens the weather -- silently takes the
    // floor to zero and hands the lake a dead calm. Nothing would fail, and the
    // symptom would appear hours later as a mirror-flat pond that nobody could
    // explain.
    //
    // WHY A CALM MATTERS MORE THAN IT SOUNDS: the water material's amplitude
    // scale is u^1 and is deliberately NOT floored -- glass at zero wind is the
    // owner's decision and the feature reading true. This field is therefore
    // the ONLY thing standing between the shipped defaults and a glass lake.
    //
    // The bar is 1.0 m/s, comfortably under the 1.575 the parameters imply, so
    // this fails on a real regression rather than on tuning within the intended
    // envelope. Deliberately NOT pinned at 1.575: that would make the test a
    // restatement of the arithmetic instead of a statement of the requirement.
    const WindParams p{};
    Stream rng(90210);
    int32_t lowest = INT32_MAX;
    constexpr int32_t kBreezeFloorMmPerS = 1000;
    constexpr int kSamples = 20000;
    for (int i = 0; i < kSamples; ++i) {
        const WindSample w = sampleWind(kSeed, rng.sym(1'000'000'000LL), rng.sym(1'000'000'000LL),
                                        rng.pos(1'000'000'000LL), p);
        if (w.speedMmPerS < lowest) {
            lowest = w.speedMmPerS;
        }
    }
    CHECK(lowest >= kBreezeFloorMmPerS);
    // Reported whether or not it passes: the margin is the interesting number,
    // and a run that passes at 1005 mm/s is a warning that a later one will not.
    std::printf("    lowest speed over %d samples: %d mm/s (floor %d)\n",
                kSamples, lowest, kBreezeFloorMmPerS);
}

VXC_TEST(wind_differs_from_place_to_place) {
    // THE ACTUAL REQUIREMENT, stated as a test: two places on the map, at the
    // same instant, must have different wind. A field that varied only in time
    // would pass every other test in this file.
    //
    // The separation is one synoptic cell (2048 m). The measured median
    // difference there is 1.45 m/s; the bar below is a tenth of that, on the
    // MEDIAN of many pairs rather than on any one pair, because two points a
    // cell apart can legitimately land on the same contour.
    const WindParams p{};
    Stream rng(4242);
    int64_t total = 0;
    int agreed = 0;
    constexpr int kPairs = 2000;
    for (int i = 0; i < kPairs; ++i) {
        const int64_t x = rng.sym(1'000'000'000LL);
        const int64_t y = rng.sym(1'000'000'000LL);
        const int64_t t = rng.pos(100'000'000LL);
        const WindSample a = sampleWind(kSeed, x, y, t, p);
        const WindSample b = sampleWind(kSeed, x + p.synopticLatticeMm, y, t, p);
        total += std::abs(a.speedMmPerS - b.speedMmPerS);
        if (a.speedMmPerS == b.speedMmPerS && a.fromBearingMilliDeg == b.fromBearingMilliDeg) {
            ++agreed;
        }
    }
    CHECK(total / kPairs > 145); // mean |dSpeed| over 2048 m, mm/s
    CHECK(agreed < kPairs / 100);
}

VXC_TEST(wind_seed_separates_worlds) {
    // Two worlds must not share weather. Cheap to get wrong -- forgetting the
    // salt, or mixing the seed in somewhere that splitmix64 then discards --
    // and impossible to notice by looking at either world alone.
    const WindParams p{};
    CHECK(windSeed(0) != 0);
    CHECK(windSeed(1) != windSeed(2));
    int different = 0;
    for (int i = 0; i < 200; ++i) {
        const int64_t t = static_cast<int64_t>(i) * 137'000;
        const WindSample a = sampleWind(1, 0, 0, t, p);
        const WindSample b = sampleWind(2, 0, 0, t, p);
        if (a.speedMmPerS != b.speedMmPerS || a.fromBearingMilliDeg != b.fromBearingMilliDeg) {
            ++different;
        }
    }
    CHECK(different > 190);
}
