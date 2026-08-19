# Sun/moon visibility audit

Owner report: "the sun and moon assets are not always visible or dynamically moving when
the day or night is progressing. I have seen moonless nights on several play test occasions."

Verdict up front: **one real bug** (the moon teleports ~12 degrees at every calendar step,
every ~8 real minutes — confirmed in the owner's own playtest logs, including a moon that
set, rose again 37 seconds later, and set again), **one piece of correct astronomy**
(moonless nights are expected roughly every third-to-fourth night at this calendar's
compression — numbers below, with a design lever if the owner wants fewer), and **one
framing fact** about the sun (its disc is drawn at physical size, ~13 px — the exact size at
which the moon was once reported "there is no moon" and got a 9x enlargement the sun never
did; above ~31 degrees altitude it is out of a level camera's frame entirely).

All file:line references are to the working tree on branch `claude/f6-interior-rim-injection`,
2026-08-17. No code was changed; this document is the only write.

---

## (a) How the celestial system actually works

**One clock, one ephemeris, both bodies.** `UVoxelSkySubsystem::Tick` advances a single
epoch (`Impl->EpochSeconds += DeltaTime * TimeScale`, VoxelSkySubsystem.cpp:2503), converts
it to a Julian Day (`JulianDayFromGameClock`, :2522), and evaluates **both** the sun and the
moon from that same JD (`ComputeSun` :2523, `ComputeMoon` :2524). The moon is not a texture
on a schedule: it is a real circular-orbit ephemeris (VoxelEphemeris.cpp:352-442) — mean
longitude at the 27.32-day sidereal rate, 5.145-degree inclination, hour angle from local
sidereal time minus right ascension, refraction applied, **phase from the sun-moon
elongation** (signed `PhaseFraction`, 0=new 0.5=full) and **illuminated fraction from the
actual angle between the returned sun and moon direction vectors** (:426-439), so the drawn
terminator and the light's strength can never disagree with the geometry.

**The calendar, which is the crux.** `JulianDayFromGameClock` (VoxelEphemeris.cpp:305-321):

    DayFraction  = frac(epoch / DayLength)                     # diurnal clock
    RealDayOfYear = frac(epoch / (DayLength*DaysPerYear)) * 365.2425   # seasonal clock
    JD = JD(2000-01-01) + floor(RealDayOfYear) + DayFraction

Defaults: `DayLength = 3600 s` (VoxelSkySubsystem.cpp:228 — NOTE the help text still says
2400; the code default is 3600, confirmed by the round-22 playtest log), `DaysPerYear = 48`
(:243). So the DATE advances 365.2425/48 = **7.61 real days per game day**, delivered as
`floor()` steps of **+1 whole JD day every 473 seconds (7 min 53 s) of real play**, plus one
−1-day jump at each game midnight when DayFraction wraps. The header comment
(VoxelEphemeris.h:130-145) analyses this stepping **for the sun only**: the sun's hour angle
is a function of `frac(JD + 0.5)`, which an integer step leaves exactly unchanged — the
design's stated point — and only its declination hops (≤0.40°/step at equinox, documented as
intended seasonal compression). **The moon was never given that analysis, and it is not
immune** (see (b), H-B).

**Rendering.** Two directional lights: sun = atmosphere light 0 (never hidden, never zero —
:2642-2666, :712), moon = atmosphere light 1 (allowed to reach zero and leave the scene,
:713-718). UE's own phaseless moon disc is killed unconditionally at spawn
(`SetAtmosphereSunDiskColorScale(Black)`, :2187); **the only moon disc is the textured one
M_NightSky draws** on the 200 km star dome (VoxelSkyDomeActor.cpp), positioned by MPC
parameters (`MoonDirection`, `MoonAngularRadius` 2.34° drawn radius = 9x physical ≈ 90 px,
`MoonBrightness` 0.15, `MoonPhaseFraction`) written **every frame**, outside the 10 Hz
shadow-cadence gate, precisely so the disc glides (:2563-2574, :3134-3219). The sun's disc
is drawn by M_SkyAtmosphereDome via `SkyAtmosphereLightDiskLuminance(light 0)`
(create_sky_atmosphere_dome_material.py:305-330) at the light's own default angular size
(~0.535°); when the atmosphere dome is off or refused, the engine's atmosphere pass paints
the sky and the disc itself.

**Moonlight gates** (Tick :2744-2753): `MoonIntensity = cvar(0.16) x horizon gate (−1°..+3°)
x IlluminatedFraction x sun-suppression (fades to 0 as sun climbs −6°..0°)`. A new moon or a
set moon delivers exactly zero light **by design**, and the log states this on every horizon
crossing (:3259-3270). Since S2, starlight ambient (`StarAmbientGain`) keeps a moonless
night's ground at ~5/255 mean luma instead of black.

**What freezes the sky.** `voxel.Sky.TimeScale 0` (the capture pin: every harness script —
voxel-capture.ps1:92, voxel-run-leg.ps1, etc. — defaults to 0 deliberately, for
reproducibility) and `voxel.Sky.Enabled 0` (static pre-W4 pose). **The owner's playtests are
not pinned**: round-20/21/22 logs all resolve `TimeScale=1.000` and were launched with only
`-VoxelTimeOfDay=23:00`. No config override of any `voxel.Sky.*` cvar exists in
ue-project/Config.

---

## (b) Hypotheses, confirmed/refuted from code and logs

**H-A. "The moon/sun is stepped or static in gameplay" — REFUTED.**
Same epoch clock for both bodies (VoxelSkySubsystem.cpp:2522-2524); MPC disc position
written every frame (:2563-2574); light orientation stepped at 10 Hz = 0.015°/step,
invisible (:2597-2626). Playtest logs show TimeScale=1.000. TimeScale-0 pins are
capture-harness behaviour only, by design.

**H-B. "The moon jumps / moves wrongly" — CONFIRMED, and it is the bug.**
Every `floor(RealDayOfYear)` step (VoxelEphemeris.cpp:320) advances JD by +1.0 in one frame.
Per step, from the rates in VoxelEphemeris.cpp:256-267 and :363:

| quantity | per calendar step (every 7 min 53 s real) |
|---|---|
| sun hour angle | exactly 0 (design; frac(JD+0.5) invariant) |
| sun declination | ≤ 0.40° hop (documented intent, VoxelEphemeris.h:130-138) |
| star field (LST) | 0.99° hop |
| **moon hour angle** | **−12.19° hop** (GMST +360.986 − RA +13.176 ≡ −12.19 mod 360) |
| moon vs star field (RA) | 13.18° hop |
| moon declination | up to ~6° hop |
| **moon phase** | **+3.4% of a synodic cycle**; illuminated fraction steps up to ±10.6 points near quarters |

−12.19° is **2.6 drawn moon diameters (~300 px at 2200x1300/90°) teleporting in one frame,
every ~8 minutes**, while the stars barely move. Plus one +12.19° hop the other way at each
game midnight (DayFraction wrap −1 day).

**Owner-era proof, from his own sessions (both TimeScale=1):**
- `Saved/owner-playtest-round21.log`: 04:47:06 "moon has SET" (azimuth 293.6°, phase 0.421,
  94% lit) → 04:47:43 "moon IS UP: altitude +1.43°" (azimuth 284.7°, phase 0.455, 98% lit)
  → 04:48:11 "moon has SET" again. **The moon set, teleported back above the horizon 37
  seconds later, and set a second time**; the phase stepped by exactly +0.034 = one JD step.
- `Saved/owner-playtest-round20.log`: 04:27:54 alt +0.00° → 04:28:02 alt **−10.24°** — ten
  degrees in 8 seconds, against a continuous rate of ~0.6°; phase 0.613 → 0.647, again
  exactly one step.

This is precisely "not always visible or dynamically moving": near the horizon the moon
pops in and out of existence; high in the sky it lurches. It is a **design gap, not a
math error**: the decoupled-calendar design proved the sun immune to the floor steps and
never examined the moon, whose hour angle routes through sidereal time and RA and inherits
the full discontinuity.

**H-C. "Moonless nights" — CONFIRMED AS CORRECT ASTRONOMY, at a compressed rate.**
Elongation advances 12.19°/JD-day x 7.609 JD-days/game-day = 92.8°/game-day, so the
**synodic month is 29.53/7.609 = 3.88 game days = 3 h 53 min of real play** (at DayLength
3600). Consequences, all exactly as in real astronomy but 7.6x faster:
- The moon is <10% illuminated for 20.5% of each cycle ≈ **0.8 nights out of every 3.9**.
- Near new, the moon rises and sets with the sun (phase and schedule are locked, as in
  reality — net sky motion 267°/game-day means moonrise comes ~8.3 game hours later each
  night), so on those nights it is **also below the horizon for most of the dark hours**.
- Between quarters it is up for only about half the night.

Net: **roughly one night in three-to-four is functionally moonless, recurring every ~4
hours of continuous play** — "several play test occasions" is the expected observation
rate, not a defect. The subsystem even logs it: "A moon BELOW the horizon or at ~0%
illuminated is not a bug -- both draw nothing on purpose" (VoxelSkySubsystem.cpp:3263-3269).
Since S2, such nights are lit by starlight ambient rather than pitch black.

**H-D. "The moon asset fails to render at all" — REFUTED for the current tree.**
- MPC bound and driven: round-22 log shows "material params bound", "moon DISC RESOLVED
  2.340 deg ... about 90 pixels", "moon IS UP ... 86% illuminated" — the full path was live
  in the owner era.
- MPC-regen severance (the known hazard): **M_NightSky cannot be severed from MPC_VoxelSky**
  — `create_sky_material.py` authors both in the same run. The severable dependents
  (M_SkyAtmosphereDome, water, underwater, both terrain materials, ripple) are exactly what
  `tools/voxel-sky-chain-regen.ps1` discovers by scan and verifies with a pinned-pose photo;
  a severed dome material fails loudly (grey sky everywhere), not as a quietly missing moon.
- Missing MPC → Error log + the **opposite** symptom: a permanently-full moon frozen due
  east (:3079-3091). Missing dome mesh/material → Error naming the asset
  (VoxelSkyDomeActor.cpp:153-199). Dome radius vs clipmap → measured and Error'd at
  BeginPlay (:218-249); default clears by 2.16x.
- The one silent-by-design path: `voxel.Sky.DomeEnabled 0` removes the **only** moon disc
  (UE's own is killed unconditionally, VoxelSkySubsystem.cpp:2153-2187; the cvar's help says
  so). Default is 1 and nothing in Config or the playtest command lines sets it to 0.
- Real occluders that are correct behaviour: depth test (terrain occludes the moon), the
  ±1.7° horizon fade, and daylight — where note the moon disc is deliberately NOT faded by
  day (:3361-3377), so a daytime moon is real and a night moon can be genuinely absent.

**H-E. "Sun disc absent while daylight persists" — REFUTED as a decoupling, CONFIRMED as
framing/size.** The sun light is never hidden or zeroed (the file's central invariant,
:2642-2666), and the disc is always painted by whoever paints the sky (dome:
create_sky_atmosphere_dome_material.py:305-330, "Without this node the A/B ladder loses the
sun from every daytime rung"; atmosphere pass otherwise). There is no code window with
daylight and no disc. But: the disc renders at the light's default ~0.535° — **~13 px** at
2200x1300/90°, clipped white; the moon at that size was reported "there is no moon"
(kMoonDrawnAngularRadiusDeg's history, VoxelSkySubsystem.cpp:166-207) and was enlarged 9x;
the sun never was. A level camera's frame tops out at +30.6° altitude (61° vFOV), and the
equinox noon sun at 52 N sits at ~38° — **the noon sun is never in a level frame**. The sun
also inherits a minor H-B: its disc hops ≤0.4° (75% of its own width) per calendar step near
the equinoxes.

---

## (c) Diagnosis and fix plan

**BUG — moon discontinuity at calendar steps.** Fix: evaluate the moon's *slow* elements on
a continuous day count while keeping its *diurnal* term on the stepped JD, mirroring exactly
the split the design already performs for the sun.

- In `VoxelEphemeris`, expose the continuous day count (RealDayOfYear **without** the
  floor — the quantity JulianDayFromGameClock already computes at VoxelEphemeris.cpp:315).
- `ComputeMoon` takes both: mean longitude, argument of latitude, and the solar elements
  used for elongation/phase evaluate at `JD_slow` (continuous); `LocalSiderealTimeDeg` and
  the hour angle stay on `JD_diurnal` (stepped) — the same JD the star field's
  `StarRotation` uses (VoxelSkySubsystem.cpp:3332), so **the moon and the stars hop
  together** (0.99°/step, down from the moon's 12.19°) and their relative position becomes
  continuous. Phase then evolves smoothly (+26% of a cycle per game day — visible within a
  night, the same honest compression the sun's declination already shows).
- Average behaviour is unchanged: same 3.88-night synodic month, same rise schedule, same
  moonless-night frequency; full-moon-opposite-sun still holds. One discontinuity remains
  at the game-year wrap (present today too). Sun left as-is (its step is documented
  intent). Extend `FVoxelSkyMoonPhaseTest` (VoxelSkyTests.cpp:460) with: no moon-direction
  jump > ~1.1° across a floor step at game rates.

**ASTRONOMY — moonless nights.** Correct at 7.6x real frequency. Levers, owner's choice:
1. **Do nothing** — it is real lunar behaviour; starlight ambient already keeps those
   nights navigable, and the F1 HUD + log state the phase.
2. **Moonlight illumination floor** (recommended if he wants every moon-up night lit):
   in Tick :2751 use `max(IlluminatedFraction, floor)` for the *light only* — e.g. floor
   0.25 keeps a crescent night at quarter-moon brightness while the disc keeps its honest
   phase. One line + one cvar; new-moon nights where the moon is *down* stay properly dark.
3. **Slower phase clock** (rarer moonless nights): give the synodic cycle its own rate
   decoupled from the compressed year. Honest cost: either the terminator stops exactly
   matching the sun direction, or the whole lunar longitude must slow with it (changing the
   rise schedule). Bigger change; only worth it if the 4-hour cycle itself is the complaint.

**SUN (optional, owner screenshot call):** enlarge the drawn sun disc the way the moon was
(the dome's `SkyAtmosphereLightDiskLuminance` has an unconnected `DiskAngularDiameterOverride`
input; or set the light's source angle) — the same 0.26°→2.34° argument recorded for the
moon applies verbatim.

**Housekeeping:** `CVarSkyDayLengthSeconds` help text says "Default 2400 = a 40-minute day"
while the default is 3600 (VoxelSkySubsystem.cpp:227-240); the DaysPerYear help repeats the
2400 arithmetic (:244-249). Fix the strings when next touching the file.

---

## (d) Capture commands for the main session

All via `tools/voxel-capture.ps1` (defaults pin TimeScale 0 at 12:00 03-20; every departure
below is explicit). Reference pose is the owner-playtest / chain-regen column. After each
run, read `Saved\capture-<name>.log` — the subsystem prints "VoxelSky clock RESOLVED",
"moon IS UP/has SET ... % illuminated", and "moon DISC RESOLVED" lines that are half the
evidence.

**1. Prove the teleport (H-B): moving-clock burst strip across ≥2 calendar steps.**

    tools\voxel-capture.ps1 -Name moon-step-strip -SpawnAt '-65102,-51084' -SpawnAltM 120 `
        -TimeOfDay 23:00 -Date 03-20 -TimeScale 1 -SpawnYaw 219 -SpawnPitch 40 `
        -BurstCount 40 -BurstIntervalSec 25 -SettleSec 120

40 frames x 25 s ≈ 16.7 min ≥ 2 steps at the 7m53s cadence. Expected if the bug is real:
the ~90 px gibbous moon slides smoothly ~0.5°/frame, then **jumps ~12° (≈2.6 of its own
diameters) between two adjacent frames while the stars barely move**, with the terminator
visibly notching at the same frame. Then:

    findstr /C:"VoxelSky moon" D:\voxelsim\Saved\capture-moon-step-strip.log

Consecutive SET/IS-UP pairs seconds apart (as in round-21) are the same proof in text.

**2. Prove moonless nights are astronomy (H-C): phase ladder over successive reachable
dates** (they are 7.61 days apart; phase advances ~+26% of a cycle per rung):

    foreach ($d in '03-16','03-24','04-01','04-08','04-16') {
        tools\voxel-capture.ps1 -Name "moon-phase-$d" -SpawnAt '-65102,-51084' -SpawnAltM 120 `
            -TimeOfDay 23:00 -Date $d -TimeScale 0 -SpawnYaw 219 -SpawnPitch 40 -SettleSec 120
    }

Read each log's "moon IS UP/has SET ... % illuminated" line: expect the illumination to
march through the cycle, with at least one rung near-new (moon absent or a sliver — a
correct moonless night) and one near-full. If a rung's log says the moon is up and >50%
lit at that azimuth/altitude but the frame shows nothing at it, that rung falsifies H-D
instead. (Yaw/pitch frame the round-22 pose's moon; for rungs where the logged azimuth
differs by more than ~30°, re-shoot that rung with -SpawnYaw set to the logged azimuth and
-SpawnPitch to its altitude.)

**3. Prove the render path (H-D), pinned known-good pose + dome control arm:**

    tools\voxel-capture.ps1 -Name moon-disc-proof -SpawnAt '-65102,-51084' -SpawnAltM 120 `
        -TimeOfDay 23:00 -Date 03-20 -TimeScale 0 -SpawnYaw 219 -SpawnPitch 45 -SettleSec 120
    tools\voxel-capture.ps1 -Name moon-disc-control -SpawnAt '-65102,-51084' -SpawnAltM 120 `
        -TimeOfDay 23:00 -Date 03-20 -TimeScale 0 -SpawnYaw 219 -SpawnPitch 45 -SettleSec 120 `
        -Cvars 'voxel.Sky.DomeEnabled 0'

Proof arm: textured ~90 px gibbous disc at azimuth ~219°, altitude ~51° (round-22 logged
86% lit there). Control arm: disc gone, lighting unchanged — demonstrating the disc's only
draw path. If the PROOF arm has no disc while its log prints "IS UP ... 86%", the render
chain is broken: run `tools\voxel-sky-chain-regen.ps1` (full chain, it photographs its own
result) and re-shoot.

**4. Sun disc size/framing (H-E):**

    tools\voxel-capture.ps1 -Name sun-disc-low   -SpawnAt '-65102,-51084' -SpawnAltM 120 `
        -TimeOfDay 07:30 -Date 03-20 -TimeScale 0 -SpawnYaw 107 -SpawnPitch 12 -SettleSec 120
    tools\voxel-capture.ps1 -Name sun-noon-level -SpawnAt '-65102,-51084' -SpawnAltM 120 `
        -TimeOfDay 12:00 -Date 03-20 -TimeScale 0 -SpawnYaw 180 -SpawnPitch 0  -SettleSec 120
    tools\voxel-capture.ps1 -Name sun-noon-up    -SpawnAt '-65102,-51084' -SpawnAltM 120 `
        -TimeOfDay 12:00 -Date 03-20 -TimeScale 0 -SpawnYaw 180 -SpawnPitch 40 -SettleSec 120

Expected: low sun in frame as a ~13 px clipped disc with bloom; noon-level shows full
daylight with **no sun anywhere in frame** (it is at ~38° altitude, above the 30.6° frame
top — the owner's "sun not visible while day progresses", reproduced as geometry); noon-up
recovers it. These three are the owner's decision material for whether the sun disc gets
the moon's enlargement treatment.

Present the captures with conditions and let the owner judge; per project rule, no verdicts
from renders here.
