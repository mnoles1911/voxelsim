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
