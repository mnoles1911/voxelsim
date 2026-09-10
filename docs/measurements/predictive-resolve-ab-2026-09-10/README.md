# Predictive asset-resolve prewarm A/B, full 256 m ring, 2026-09-10 (captures 30 OFF / 31 ON)

Same binary (retirement-pilot branch build, R0 diagnostic compiled in and ENABLED in both
arms), same cache `temperate-authored-lods-full-3`, same scene/spawn/settings as the
size-cull A/B. Only `-VoxelPredictiveAssetResolve` differs. Both receipts passed with
identical module hashes. (A first ON attempt was refused by the cache because an identity
source file had been edited between the arms; it is kept as
`walk-capture-31-...INVALID-identity-source-edited` and is not evidence.)

Engagement (ON, `predictive-windows.log.txt`, 40 windows): probes 22,469, launched 6,285,
landed 6,232, raced 37, nonresident 0, rejected 0, epochRejected 16. Admission resolves in the
last window: OFF calls=410 hits=0 coldMisses=410 (1,723 ms inline); ON calls=75 hits=69
coldMisses=6 (40 ms inline).

R0 entry profile (measurement window, `*.r0-entry-profile.json`):

| arm | entries | Entry total ms | Resolve total ms | Compute calls | Entry p95 ms | Entry max ms |
|-----|---------|----------------|------------------|---------------|--------------|--------------|
| OFF | 16      | 2109           | 2046             | 510           | 254          | 272          |
| ON  | 16      | 658            | 559              | 497           | 149          | 251          |

Frame metrics OFF -> ON (`flag-ab-comparison.json`):

| phase       | FrameTime median | FrameTime p95   | GameThread median | VoxelStream/TickMs p95 |
|-------------|------------------|-----------------|-------------------|------------------------|
| WalkForward | 42.5 -> 42.2     | 328.6 -> 119.8  | 38.4 -> 37.5      | 280.3 -> 93.6          |
| SprintGate  | 55.5 -> 48.7     | 448.3 -> 238.4  | 55.1 -> 48.0      | 281.1 -> 148.9         |
| JumpApex    | 43.5 -> 42.5     | 82.2 -> 60.9    | 44.2 -> 37.9      | 14.8 -> 17.4           |

Reading: prewarming the footprint resolve off the game thread removes most of the cold
Resolve cost the R0 profile named (-73% Resolve, -69% Entry) and cuts the movement-phase
hitch tails by 2-3x; medians move little because the GPU floor (~40 ms at this ring) is
unchanged. One matched pair at one site. The flag stays opt-in in this record; making it
the default for ecology worlds is the owner's call, with this as the first clean evidence
(the earlier 22/23 pair was confounded by the old LOD payload). A small residual remains
(Settle-phase TickMs median 0.07 -> 2.2 ms is the warm queue draining while parked).
