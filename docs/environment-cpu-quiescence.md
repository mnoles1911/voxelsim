# CPU dependency lifetime during shutdown

The previous 60-second shutdown deadline discarded unfinished task handles and continued destroying their borrowed World state. Leaking the dedicated thread pool did not retain the World, result queues or externally owned water sampler.

CPU launch families now use one owner-thread admission gate. Shutdown closes admission and retains all task/future handles until their bodies finish. A long wait reports outstanding counts every 60 seconds; it never authorizes dependency destruction. Completed drains report task/pool counts and elapsed time. The dedicated pool is destroyed only after its futures complete.

Water-marker installation and removal use the same drain before changing the amplifier's borrowed pointer or related settings. The previous admission state is restored afterward, so shutdown cannot reopen dispatch. Current callers are startup and teardown. This does not invalidate old meshes, caches or already queued results for arbitrary live configuration changes.

The audited CPU bodies sample and enqueue results without waiting for game-thread publication. The drain holds no provider/cache/water mutex and pumps no game-thread callbacks. A future worker that needs game-thread progress cannot use this blocking teardown contract. A permanently stalled provider or scheduler will retain the world and block shutdown; freeing its storage would be unsafe. A bounded shutdown ultimately requires independently owned generation contexts or a separate abnormal process-exit policy.

Focused core tests use a deterministic clock to exercise repeated diagnostic deadlines while handles and an external marker remain retained. A native test uses real UE Tasks and a dedicated pool through the same production drain helper, verifying both borrowed reads finish before marker destruction. It does not instantiate the full water subsystem. The two focused core cases passed (0.13 seconds), using integer millisecond timing. The final integer-timing build passed all56nativeDX12 tests with normal exit0. The actual three-world/two-OpenLevel harness also passed, requiring completed CPU quiescence before every GPU teardown.

Asynchronous authority capture still needs separate ownership for retained inputs and completed results. Task completion alone does not release those leases. Owner-held references must be dropped before waiting on their lifetime; this change does not activate production authority capture.
