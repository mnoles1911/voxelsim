# Rock generation memory — 2026-09-07

The four heavy Asset Forge specs can be tested at their authored **100 mm**
pitch. The previous CI override, `--res 20`, means 200 mm and is correctly
rejected by production pitch validation. No pitch exception, spec resizing,
generator feature omission, or connectivity-check exemption was introduced.

## Change

`asset-forge/forge/rock.py` releases scratch arrays after their final use:
per-lump distance fields, previous noise octaves, original mass/relief before
erosion, facet coordinates/projections, arch component labels, and erosion
normals/per-iteration scratch. Private distance scratch uses the same sqrt and
subtraction operations in place. Durability retains its independently allocated
clipped array instead of copying it again unnecessarily.

The algorithms, random draw sequence, numerical operation order, specimen
parameters and output pitch remain unchanged. This is allocation-lifetime work;
the generator still uses dense grids and its existing memory tripwire is not a
hard RAM budget.

## Completed measurement

Windows, Python 3.12.14, NumPy 2.3.5, SciPy 1.18.1. The full current
`hero-arch-colossal`, seed 1, spec hash `6b5947ad059c8ec8`, was generated at
100 mm and then passed the same health and single-piece checks used by
`buildcheck.py`:

| Measurement | Result |
| --- | --- |
| Full build plus health/single-piece checks | 635.93 seconds |
| Peak resident working set | 9.116 GiB |
| Peak private commit | 9.838 GiB |
| Final occupied voxels | 50,284,322 |
| Final cropped grid | 914 × 321 × 433 |
| Health problems / extra pieces | 0 / 0 |

Memory values include the interpreter and come from Windows
`GetProcessMemoryInfo` high-water counters, queried during and after the run.
They are not merely the largest 15-second sample. Output SHA-256:
`ecb637353f868eff0f00462cd0aa888e75b592c10e559697b8c0f397866048a8`.

The other heavy specs passed ordinary `buildcheck.py` at authored 100 mm:
balanced rock 678,574 voxels / 24.4 s; sea stack 3,026,802 voxels / 98.1 s;
sequoia 4,347,012 voxels / 44.1 s. Together they took 168 s. These local
timings overlap other verification work and are not isolated speed benchmarks.
The quick Forge selftest also passed, including validation of all 840 specs.

## Before/after evidence and limits

Five original, unmodified-generator seed-1 specimens were built before editing
and compared with the final generator. Grid dimensions, material bytes and
health results match exactly. These are SHA-256 hashes of dense material bytes:

| Spec | Grid | Unchanged SHA-256 |
| --- | --- | --- |
| river-cobble | 9 × 8 × 6 | `962b462ce1d2b90c58e07b3c925e291477aaa8cdd3a44b58068fd6df81d6b8a6` |
| desert-arch | 65 × 24 × 44 | `c9527bb2cc39cb2c1d9ed052fba4e541598e132b7c1afad47e1f0329f9d518f9` |
| hero-natural-arch | 410 × 142 × 264 | `f2ae32ea90fa5979ff89878c002cc402cfe4bf018646a9bc1e86c0ba414e9149` |
| hero-balanced-rock | 108 × 99 × 133 | `bd3932ed43fad6be695a0e4b6d52d28d55396340ef9b2c6104de041f7e8d9eea` |
| karren-pavement | 25 × 16 × 5 | `7df99b1fcff1ef76f958afa40fd675ef99781fa4f2635c6172994a6a045cc315` |

A **partial** arch run after the first cleanup, before facet/arch-label lifetime
fixes, reached 11.754 GiB resident / 12.482 GiB private commit. It was stopped
after 165 seconds to measure the final revision. This is not a completed
unmodified-generator baseline; no full-arch before/after byte-equivalence or
end-to-end speedup was measured.

Historical 24 GiB claims describe a different seed/configuration, including an
older rubble-margin grid of 1,738 × 470 × 1,294. Current production builds omit
that rubble ring. Executing the current size-fit probes, then stopping before
full-resolution allocation, gives first 100 mm grids of 318.6 M, 290.0 M and
231.9 M cells for arch seeds 1, 2 and 6. These are derived allocation bounds for
the first full-resolution attempt, not measured peaks or guarantees about later
size-fit attempts. Only seed 1 completed the full measured arch build here.

The public repository's Ubuntu runner has 16 GB RAM according to the
[GitHub runner specification](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
The measured peak supports using its ordinary authored-pitch heavy build gate;
CI records `/usr/bin/time -v` for the actual Linux result. A smaller private runner
would not have the same margin. Future larger specs and finer pitches need new
measurements; this result does not establish a universal dense-generation budget.

Local evidence under `D:/voxelsim/Saved/`: `rock-arch-100mm-memory-final.log`,
`rock-arch-100mm-memory-first-pass.log`, `rock-memory-before.jsonl`,
`rock-memory-final-equality.log`, `rock-other-heavy-100mm.log`,
`forge-rock-memory-selftest.log`, and `environment-heavy-bounds.jsonl`.
