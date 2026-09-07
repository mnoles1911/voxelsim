# Authoritative environment ownership after the visual pilot

The visual pilot deliberately leaves terrain gameplay authoritative. Enabling its
actor for gameplay without a projection bridge would leave the original generated
asset solid after the actor is carved or felled.

The first bridge should handle one stationary canonical 100 mm object. Publish an
immutable snapshot mapping full source provenance and stable ID to its current
voxel projection, object revision and projection revision. Worker sampling must
never borrow a mutable actor or UObject grid.

Separate terrain-owned sampling from aggregate gameplay queries. Core terrain
`GeneratedWorld::materialAt` and `makeBrick` must consume the same ownership
snapshot: mask the original winner for an active object, and use its current
projection only when that projection is terrain-owned after demotion. Aggregate
gameplay queries additionally sample immutable active-object projections and
return their owner IDs. Baking an active object's projection into terrain bricks
would duplicate the actor. Point and brick tests must agree within the same
sampling domain, not conflate these two domains.

Preserve terrain-overlay precedence and the original first non-air asset winner;
removing an owned source from the instance list incorrectly exposes a later
overlapping source. Rebuilding a brick after an adjacent terrain edit must also
respect that ownership snapshot, or original material will be baked back into it.
Return ownership identity with relevant hit samples so edits can be routed to the
object rather than incorrectly written into the terrain edit log.

World integration points include camera raycasts, dig preview and mining,
placement, sphere carving, `IsSolidAtVoxel`, `RaycastVoxelWorld`, edit samplers and
disconnected-island analysis. Agent navigation and pawn collision then inherit
the corrected solidity queries. The current OR between generated solidity and
prototype actor solidity cannot remove an owned source's original cells.

First acceptance: carve one cell from a stationary object, then edit an adjacent
terrain brick. The carved cell must remain air; point sampling, generated bricks,
raycasts and collision must agree. Earlier and later overlaps, base terrain and
explicit player edits need separate checks. Publish the new projection and
invalidate relevant fluid/navigation/terrain observers with the actor revision.
Felling, moving projections and multiplayer remain disabled until their revision
and lifetime transitions are integrated.

Restore must reconstruct provenance and ownership before revealing the actor.
The existing ordinary `PublishRestoredObject` path reveals an environment actor
immediately; using it unchanged for a promoted source would duplicate the original
generated asset. Stage the complete terrain/object transaction instead, refusing
missing or changed source content. Edited demotion likewise requires the current
projection or a tombstone before the actor disappears; regenerating the original
canonical bank would resurrect removed material. Require object and projection
revisions to agree.

This is remaining implementation work. The visual pilot, immutable render context,
prepared commit tokens and existing detached-object saves do not implement it.
