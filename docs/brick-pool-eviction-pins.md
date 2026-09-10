# Brick-pool eviction pressure pins

`AcquireEvictionPins` protects a bounded complete set of keys against unrelated allocator eviction. Keys may be absent at acquisition; later allocations inherit protection. Tickets are pool-scoped, reject overlap, and become invalid after release or pool reset. At most 16 tickets and 8,192 total keys are retained. Acquisition validation is all-or-nothing.

Existing same-key replacements, explicit removals and GPU shell cancellation remain legal while old jobs drain. Capture slot/AddSequence tokens after draining and revalidate immediately before publication. Pins are **not an immutable seal**, do not reserve arena capacity, and do not establish GPU completion. Same-key allocation still frees its previous allocation first and may fail to replace it under capacity pressure.

Protected prepared-batch publication requires its matching ticket. All pages in a ticketed batch must belong to that ticket. Existing unticketed publication remains available for unprotected pages when an index preflight contract is installed. Publication retains its no-eviction reservation phase and its refusal of GPU arena allocator mode. Failure retains pins; success also retains pins until the controller releases them. The controller must release on every abort and successful completion.

Eviction skips protected residents using bounded scanning, including when every resident is pinned. Unpin invalidates the eviction traversal lazily so skipped keys can become victims again. GPU shell descriptor allocation shares this path; queued GPU claims/frees and GPU word-arena capacity are separate concerns.

The production controller must still gate explicit world unload, parked eviction and new same-key publication after draining. If those gates cannot ensure exclusion, a separate sealed phase must validate tokens and block normal mutation paths before their first mutation. These APIs alone do not implement atomic terrain/object publication or a renderer frame boundary.

Regression: `Voxel.Objects.BrickEvictionPins`. Combined Unreal build passed in build-environment-startup-pins.log (19actions,257.87s after waiting for a separate build). All32 DX12 object tests passed with normal exit0 in environment-startup-pins-tests.log, including this regression and its two explicitly expected allocation/eviction diagnostics.
