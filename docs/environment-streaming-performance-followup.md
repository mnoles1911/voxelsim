# Environment streaming performance follow-up

The September 7 ownership diagnostics check correctness. Real cached-site logs still contain substantial cold-streaming frame hitches with ordinary terrain rendering unchanged.

Evidence: `D:/voxelsim/Saved/environment-held-cpu-probe.log`, 08:41:17 UTC; local build passed 19 actions in 85.61 seconds.

- A 5.03-second window recorded 5,149.2 ms in World dispatch across 38 ticks. Overlapping diagnostic brackets must not be added as independent wall time.
- GPU request assembly totaled 3,363.1 ms over 2,048 calls. Asset resolution, span tables and instance marshaling accounted for 3,252.2 ms; header 3.6 ms, raster 0.5 ms, pool 0.9 ms, manager submission 105.8 ms.
- Manager tick promotion totaled 1,776.0 ms, of which its broad dispatch/enqueue bracket accounted for 1,773.0 ms. Polling was 1.9 ms and brick flush 2.0 ms. The tightly measured enqueue handoffs in the separate job-cost counters were near zero, so this does not establish render-thread queue backpressure.
- A nearby window copied approximately 0.8–1 MiB of asset tables per submitted chunk. JobLean was off. The batch log reported assets blocking 997 of 1,024 classic jobs from stacking, with classic work estimated at 15,360 passes.

These measurements localize this asset-rich site's next investigation to repeated asset request preparation and classic per-job GPU graph construction. They do not support blaming the small held CPU preparation (18 packs, 58,640 bytes, 1.058 seconds across frames), readback polling, or a global pool flush.

Before changing defaults, split asset assembly into cached resolution, span-cache lookup/build and per-request array copying; retain the original canonical instance order and first-winner suppression. Compare equal-site/equal-seed runs and per-frame tails, not only generation throughput. Existing JobLean and worklist optimizations need evidence on this asset-rich path; older empty/asset-free benchmarks in the August handovers do not establish it. A shared immutable bank-table/GPU upload cache is a possible later architecture change, but is not implemented or validated by this note. Do not claim the ordinary frame hitch issue fully resolved.

The next instrumentation emits `Voxel gpu asset preparation (window)` alongside
the existing submission split. It measures resolution, span-cache lookup/build,
table append/rebase, and residual accounting/instance work, plus span hits/misses
and copied bytes. Private rehearsal requests are excluded. Cache-hit reporting
uses the existing lookup, not a second lookup. The clocks add measurement cost;
these counters do not change caching, instance order, or ownership behavior.

Use `python tools/analyze-environment-asset-preparation.py <log>` to summarize
instrumented receipts. Missing/incomplete receipts or inconsistent brackets
refuse analysis. Window totals are not per-frame latency percentiles. This source
increment passed the Unreal build and the same-site no-buffer visual probe.

Baseline `environment-private-gpu-no-buffer-probe.log` completed normally:
25windows, resolution10304.022ms, span lookup/build79.840ms, table copy/rebase
4401.514ms, residual166.803ms;1,226,793span hits/211misses and24,774,632,440
bytes copied. Largest measured asset-work window1126.807ms. This confirms that
copying and resolution deserve attention; cache lookup/build is a small share.
It does not establish frame latency percentiles or a production speedup.

Next candidate: omit instances whose full vertical bounds cannot touch the
request's sampled cells before constructing/copying span tables. Preserve the
relative order of retained instances, first-winner suppression, coarse sample
coordinates and apron. Require boundary/negative-coordinate tests and same-site
comparison before accepting the change.

The existing resolve cache is startup opt-in, not enabled in the baseline above.
`verify-environment-handoff.ps1 -AssetResolveCacheOnly` supplies
`-VoxelAsyncAssetResolve -VoxelAsyncAssetResolveWarm=0` and verifies no warm
workers launched. Use a fresh identical process for each comparison. This is
diagnostic only; defaults remain unchanged. Existing cache keys lack explicit
provider/catalog/residency epochs, pruning is not a strict retained-memory cap,
and the asynchronous worker path has separate lifetime/admission risks. A
stationary cache-only speedup would not resolve those production requirements.

## Completed conservative culling and cache-only comparison

All three runs used the same seed/site and visual pilot, with normal exit and
reviewed green first-after/steady captures. They have different streaming request
mixes; these are measured preparation counters, not controlled FPS benchmarks.

| Run | Requests | Asset us/request | Resolve us/request | Copy us/request | Copied bytes/request |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline private-gpu-no-buffer | 47607 | 314.075 | 216.439 | 92.455 | 520399 |
| Culling visual | 51092 | 319.377 | 293.656 | 23.797 | 148589 |
| Culling cache-only | 43959 | 78.887 | 40.131 | 36.259 | 155410 |

Logs are D:/voxelsim/Saved/environment-private-gpu-no-buffer-probe.log,
environment-culling-visual-probe.log and environment-culling-cache-only-probe.log.
Culling alone cut bytes/request71.45% and copy time74.26%, but total asset
preparation slightly worsened as resolution grew. Cache-only is promising for
resolution cost, with warm launched=0 verified throughout; it remains diagnostic.
Culling25windows: resolve15003.464ms, lookup31.726ms, copy1215.856ms,
other66.547ms, copied7591707460bytes, zCulled1508271/submitted250839.
Cache-only26windows: resolve1764.102ms, lookup34.009ms, copy1593.928ms,
other75.734ms, copied6831657560bytes, zCulled875219/submitted176353.

Validation:14-action Unreal build,49DX12 tests, real20/20 held-GPU page parity
against unculled CPU output, and both visual probes. No production handoff,
continuous-frame shadow/depth or ordinary frame-tail acceptance is implied.
