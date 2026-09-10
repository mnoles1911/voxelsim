# Private GPU publication backend prerequisite

Source applied after checkpoint9abebb2. Combined Unreal build and all54DX12 object tests passed; real-site GPU allocator visual test is pending.

The extension transfers already verified private GPU ranges into Resident in a single host commit. It reserves the index delivery packet, host map/list capacities and exact absent-page membership before visible mutation. GPU claim allocation does not occur at commit. Existing GPU-side free kernels retire old GPU ranges before the prepared index delta reaches the renderer. Production gameplay activation remains absent. The isolated visual pilot now has an unbuilt GPU allocator branch.

Private-phase validity uses allocator epoch, exact shared buffer identity, pin identity and exact old target slot/sequence (or absence). The private descriptor allocations are not in Resident and cannot be selected by eviction; the descriptor arena cannot grant them twice. Unrelated ordinary add/remove/flush cannot change them. Explicit target replacement/removal is caught by slot/sequence; reset invalidates epoch; pin release invalidates serial. Therefore broad IndexMutationSequence is not needed during private claims. Final prepared index delivery retains its strict mutation/sink-generation check, including no-op flushes, across the short prepare-to-commit interval.

Bounds: same64-page/8MiB/30s private limit; final complete footprint <=1024 keys, conservative32MiB host preparation bound. UE host OOM remains fatal allocator behavior; this is not a general recoverable-memory-allocation promise.

The focused test exercises actual private claim/payload proof, no-op flushes during pending work, host budget/overlap refusal, final packet invalidation, cancellation/retry, first whole-batch publication, and replacement of old GPU residents. The first test retains the explicit mock sink for host bookkeeping. The additional RealIndexCredits test uses a local FVoxelMarchChunkIndex and its actual production PrepareIndexDelivery/ordered packet implementation: zero budget and competing credits refuse before publication, cancellation returns credits, the RT-installed index remains unchanged before commit, and two cells plus absence install together at one generation. Its bounded RT gate proves credits persist until actual packet execution. It exercises replacement of old GPU residents as well. No global index is modified.

Remaining acceptance: device-loss behavior beyond engine fatal handling; same-boundary World ownership-context + actor reveal integration; real frame/depth evidence. Final commit is game-thread non-yielding but does not itself drain previous views or establish render-frame atomicity. Caller must retain full World freeze and provide the established renderer boundary. No save/gameplay/multiplayer claim.

The real-index test observes installed render-thread ordered state used by rendering; it is not a pixel capture or direct GPU index-buffer readback. Tests were not executed during drafting; later combined54-test suite passed with complete queue/normal exit0.

Callback validation is guarded against recursive prepare/validate; lifetime is checked before accessing the pool after a callback. A consumed token cannot cancel transferred ranges, including from its delivery callback. These guards do not authorize arbitrary reentrant pool destruction/mutation inside index delivery: the production delivery remains a non-yielding contract. Mutable host map/list capacity is reserved, but temporary flush arrays and engine bookkeeping can still allocate host memory.

## Isolated World visual integration (source only)

`verify-environment-handoff.ps1 -Mode VisualPilot -GpuAllocatorVisualPilot`
selects the normal GPU allocator for a fresh process and requires an explicit
allocator/private-proof publication receipt. CPU-arena pilot remains available.
The actor remains visual-only, isolated from gameplay, save and network registries.

After before-capture and private actor readiness, the GPU branch begins bounded
claims, polls across ticks, prepares exact absent/index evidence, then enters the
existing twice-validated rendering drain and actor/context publication boundary.
Its128MiB aggregate admission reserves the full additional8MiB private plus32MiB
host bounds while retaining source CPU/GPU packs. Every refusal cancels private
claims before releasing World pins; committed ranges remain resident-owned.
This integration compiled and its token/absence tests passed; the live visual pilot is not yet accepted. The GPU payload
is the canonical CPU pack uploaded into GPU allocator ranges; independently held
GPU generation is still compared against that pack before the visual boundary.
Continuous-frame shadow/depth and nonblocking performance remain unproven.

## Live default-GPU pilot checkpoint

The first live run refused pending ordinary claims without exposure. The diagnostic
now pauses new DispatchJobs producers for at most15seconds while manager ticks,
result/GT mesh drains and pool flushing continue. It requires those producers and
ordinary pool inputs empty before private allocation, and rechecks before final
index preparation. Completion/cancellation destroys the pause state. A removal-only
GPU flush bug was also fixed: PendingIndexRemovals now prevents the no-work early
return; the real reset test asserts index removal and drained inputs.

`environment-default-gpu-drain-visual-probe.log` reached GPU publication with
27complete pages (16allocated/11absent),8.908s yielding producer drain and5.646ms
blocking boundary. First-after and10.013s steady images in capture
7AC1CA5E4C1B06A9C8FC439DFAB86343 were inspected green without fallback/glow.
Harness completed normally with all four captures and no allocator/claim proof errors. This explicitly is not acceptable
production latency or continuous-frame depth/shadow acceptance.
