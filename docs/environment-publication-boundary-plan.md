# First complete visual publication boundary

The private CPU/GPU preparation diagnostic does not publish ownership. The next
pilot must combine prepared pool/index, registry and actor tokens without a
fallible step after the first visible mutation.

Use an explicitly enabled, single-world CPU-arena diagnostic first. Keep ordinary
production disabled. Refuse GPU-armed allocation, component/quad-owned terrain
pages, multiplayer and authoritative interaction until their integration exists.
Do not switch allocator mode in a running world.

The initial renderer boundary may deliberately block:

1. Prepare hidden actor resources and complete terrain packs asynchronously.
   Retain the complete page freeze and pressure pins, including absent pages.
2. Reserve the full pool/index batch and stable object registration. Record
   explicit absence; an absent page has no fabricated replacement payload.
3. Drain prior render commands before final validation. A flush can pump work;
   guard against reentrant publication and revalidate all participants afterward.
4. In one non-yielding game-thread section, install the prebuilt ownership context,
   commit the prepared terrain/index batch and logical actor/registry tokens.
5. Reveal the prepared actor and submit pending deferred render updates for every
   registered actor component, including root and all LOD sections. Flush those
   commands before another view family may be enqueued. Keep pins/freeze through
   that boundary.

This relies on one game-thread view submission path and RHI submission order.
`FlushRenderingCommands` alone does not submit deferred UObject render updates.
The hidden actor's earlier resource fence also does not cover its later reveal.
The exact implementation still needs rendered-frame evidence across color, depth
and shadows; this plan is not that evidence. Measure the blocking interval and
replace it with a verified renderer-owned asynchronous boundary before describing
the handoff as low latency or enabling it by default.

The visual pilot must retain a reversible ownership lifetime, prevent the ordinary
hidden-candidate timeout from destroying the published actor, and keep future
generation on the new immutable context. It must not enter saves or replication as
a supported interactive object while terrain collision/edit queries still return
the original source. Those authoritative routes and persisted provenance remain
separate required work, not properties granted by successful rendering.
