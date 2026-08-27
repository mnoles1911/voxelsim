# Marcher cost per look direction

The requirement is a mechanism that cuts marcher ray cost in **every** direction.
This is the harness that decides whether one does.

## Run it

```powershell
# control vs a mechanism, alternated, all six poses
tools\voxel-march-direction-sweep.ps1 -Prefix DIR1 `
    -ArmBName pyr -ArmBCvars "voxel.March.HeightPyramid 1" `
    -ArmBProof "hpyr:.*consulted=[1-9]"

# see what it would do without taking the box
tools\voxel-march-direction-sweep.ps1 -Prefix DIR1 -DryRun ...

# reproduce the baseline only (control, three poses, ~8 min)
tools\voxel-march-direction-sweep.ps1 -Prefix BASE -Poses '-90,0','0,0','30,0'
```

```bash
bash tools/march-direction-summary.sh DIR1          # by sweep prefix
bash tools/march-direction-summary.sh ZZ-pitch0     # or by bare log name
```

13 legs at the defaults, about 34 minutes. `-Poses '-90,0','0,0','30,0'` is the
fast subset.

## The current baseline

Control arm, spawn `-61440,-61440`, parked (`-VoxelPerfFlight static`),
`view=1552x873`, sun frozen 12:00 03-20. Reproduced from `Saved/ZZ-pitch*.log`
by `march-direction-summary.sh`:

| pose | `VoxelMarch.March` | ratio to down | frame (context) |
|---|---|---|---|
| pitch -90, down | 1.108 ms | 1.00x | 3.630 ms |
| pitch 0, horizon | 4.448 ms | 4.01x | 7.303 ms |
| pitch +30, sky | 5.638 ms | 5.09x | 8.688 ms |

**Run-to-run noise floor: about 2% at the horizon.** `Saved/BK-*` and
`Saved/BL-*` are the same six legs half an hour apart on the same build; the
largest spread across a repeated pair was 1.9%. A claimed effect under 2% needs
`-Repeats`, not a louder assertion.

## How to read the output

**The number is `VoxelMarch.March` from ProfileGPU. Nothing else.**

- It is **not** `marchMs` from `voxel.March.Stats` — that is a running mean over
  the whole leg, and it read 6.348 on the same leg where ProfileGPU read 7.178.
  `March.Stats` is used here only for proof of traffic and for
  `frames`/`emitFrames`.
- It is **not** frame time. The frame column is context and is labelled `frame*`.
- Skipped-cell and avoided-iteration counters prove **engagement**, never a
  saving. Iterations and wall time move in opposite directions often enough to
  have been published three times.

**`view=` is read from the engine, never echoed from `-Width`.** `view=1552x873`
at a requested 2560x1440 is *correct*: a 60.6% screen percentage that TSR
upscales to the owner's 1440p. It is not a defect and the harness does not "fix"
it. What *is* an invalidator is the view differing between legs of one sweep.

**Ratio-to-down is the point of the table.** The thesis under test is that cost
tracks empty space crossed, so every direction is quoted against the direction
with none of it. A mechanism that helps sky and not the horizon shows up as
*that*, not as an average.

**The horizon block is a falsifier.** Three yaws 120 degrees apart give an
azimuthal spread. An A/B difference smaller than that spread has not been shown
at the horizon.

## Why six poses

`-90,0` down (the denominator; yaw is not swept because at -90 yaw only rolls
the image). `0,0` / `0,120` / `0,240` horizon — the frame is spent here, terrain
is not isotropic, and one azimuth is one sample. `30,0` sky. `90,0` straight up,
which no player holds: it is the **upper bound** of an empty-space mechanism's
own claim, because above the chunk grid there is no acceleration structure and
no Z bound at all. A mechanism that helps +30 and not +90 is doing something
other than skipping empty space, and that is worth knowing before it is
described to the owner as an empty-space skip.

## Timing legs and engagement legs are different legs

`voxel.March.HoleStats` is a **shader permutation of the timed kernel** — and it
is also the switch that makes the arm counters (`blkConsulted`, `blkSkipped`,
`blkCellsAvoided`) print at all. So the leg that can prove an arm engaged is not
the leg whose time can be quoted. Measured: `ZZ-pitch0` at HoleStats 0 read
4.448 ms; `BK-a0p0` / `BL-a0p0`, the same arm at HoleStats 1, read 4.548 and
4.463.

The sweep therefore owns that cvar and refuses an arm string that sets it:

- **timing legs** run `HoleStats 0`; these are the only ms quoted.
- **engagement legs** run `HoleStats 1`, one per armed arm at one pose, and
  exist only to print the arm's counters. Their ms is bracketed and labelled
  not-comparable.

An armed arm must supply `-Arm?Proof`, a regex matching a line its own counters
print only when it carried traffic. `armed=1` is a cvar reading itself back and
is not proof. The sweep refuses to start without one.

## Invalidators

Any one of these voids that leg. The sweep prints them per leg and exits 1; the
summary prints them under the row and exits 1.

| check | why |
|---|---|
| wall clock under 0.9x expected | a short log is not a fast run |
| no `VoxelPerfRun complete` | `FinishRun` never ran; the measured phase did not finish |
| `-ExecCmds` in the log differs from what was requested | catches comma/pipe/quoting loss at the only place it matters |
| pose pinned != pose requested | a direction sweep that looked elsewhere is worse than none |
| no `view=` line | render size unknown; no number may be quoted |
| `view=` differs from the sweep's other legs | different ray counts, not comparable |
| ProfileGPU never fired | no GPU number |
| capture fired before the pose was pinned | it photographed the preflight |
| capture fired in the linger window | it measured having stopped |
| no `VoxelMarch.March(` row | there is no number in this leg |
| `DOUBLE GRANT` non-zero | colliding claims were failed; the frame is missing geometry, which reads as a saving |
| `FINE TIER GATE LEAK` | absent tiles answer sea level, so real ground becomes "provably air" |
| `frames=0` | the pass never ran |
| `emitFrames` behind `frames` | the emit declined and RDG culled the march; the ms describes discarded work |
| proof-of-traffic regex matched nothing | armed and inert |
| this arm's cvars absent, or the other arm's present | "the arms differed in one thing" is a claim; this is its check |
| timing legs disagree on `voxel.March.HoleStats` | two different shaders were timed |
| manifest has no leg rows | an empty table is not a passing summary |

## Provenance

`Saved/<Prefix>-manifest.tsv` records the spawn, resolution, run shape, both
arms' cvars and proof patterns, and the mtimes of
`UnrealEditor-VoxelEarth.dll`, `UnrealEditor-VoxelEarthShaders.dll`,
`voxelcore.lib` and the shader build stamp. The summary prints all of it above
the table.

That block exists because of this pair: same pose, same spawn, same
`view=1552x873`, same HoleStats state, eight hours apart —
`BK-a0p0` 4.548 ms and `CENSUS-pitch0` 5.695 ms, +25%. Neither log records which
build it ran. **Do not compare across sweeps whose binary stamps differ.**

`Saved/.sweep-<Prefix>/` holds one generated wrapper per leg. Each is a
runnable, exact record of that leg; re-run one to reproduce it.

## Traps that have already cost results here

- Spawn `-84480,53760` is **fatal** — 38 km outside the baked fine tile set,
  every elevation query answered with sea level. Use `-61440,-61440`.
- `-ExecCmds` is split by UE on **comma** only. A `|` is swallowed into the
  preceding command's argument and everything after it is dropped silently.
- The ad-hoc legs passed `-VoxelPerfPitch=0,-VoxelExecCmds=ProfileGPU,-VoxelExecAfter=110`
  as one argv token and it only worked because `FParse::Value` substring-searches
  the whole command line. The sweep passes them as separate arguments.
- A speedup on a renderer is a claim about the image. The summary prints hole
  counters when a leg had HoleStats on, but a parked pose has had time to cover
  everything it can see — that column catches a gross change only. The owner's
  screenshot outranks it.
