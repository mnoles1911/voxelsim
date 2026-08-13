# Thrown voxel cubes, v0

**Effect first:** the player throws a 30 cm cube. It arcs, and then one of two
things happens. If it lands in water it makes a ring that spreads, runs into the
shore and fades. If it lands on ground it sits there as a pickup, and five
minutes later it takes itself back into the thrower's inventory rather than
lying on the map forever.

The **splash is today's deliverable**. Everything else exists so that the splash
has something to be attached to.

Files: `ue-project/Source/VoxelEarth/VoxelThrownItem.h` and `.cpp`. Nothing else
in the tree was edited — the two integration edits this needs are written out in
§8 and **not applied**.

---

## 1. What it is, and what it is built on

`AVoxelThrownItem` is a sibling of `AVoxelExplosive` (`VoxelExplosive.h:34`), not
a parallel invention. Same body (`UStaticMeshComponent` +
`UProjectileMovementComponent`), same `Launch()` entry point, and the same answer
to the problem that dominates every moving object in this project:

> **UE collision does not know about voxel terrain.** There is no Chaos body for
> the ground anywhere by design, so a projectile left to
> `UProjectileMovementComponent` falls through the planet. The fix, already
> proven in `VoxelExplosive.cpp:139-171` and `VoxelDebris.cpp:152-170`, is to run
> the voxel DDA (`UVoxelWorldSubsystem::RaycastVoxelWorld`,
> `VoxelWorldSubsystem.h:366`) along **the segment actually travelled this
> frame** and rest in the last empty voxel before the first solid one. Testing
> the endpoint instead lets a fast throw tunnel through a thin roof between two
> frames — that is how the original "charges do nothing" bug presented.

Three deliberate differences from the explosive:

| | `AVoxelExplosive` | `AVoxelThrownItem` |
|---|---|---|
| shape | 15 UU sphere | 30 UU cube = 3×3×3 voxels at 10 cm |
| rest position | centre at the last empty voxel's centre | **underside** on the hit voxel's top face — parking a 30 UU cube's centre there buries its bottom 10 UU in the ground |
| any hit settles it | yes | only a hit **below** settles it; a wall or ceiling strips the into-surface velocity and it keeps falling, instead of sticking to a cliff face in mid-air |

Drawn as one scaled engine cube, not 27 instances: the outer surface of a solid
3×3×3 block *is* a 30 cm cube, so 27 instances would draw the same silhouette at
27× the cost. That changes if the cube ever fragments.

It is **not replicated** and touches no edit log — like `AVoxelDebris` it is
presentation plus a local affordance. See §7 for what multiplayer needs.

---

## 2. The water crossing, and the trap in the obvious version

The obvious hook is one `IsUnderwaterAtWorld` per frame on the actor's centre,
firing when it flips false→true. It is what the ripple field's own poller does
(`VoxelRippleField.cpp:1050`) and what the patch note in
`docs/water-interactive-ripples.md` §8.5 writes out. For a grenade arcing into a
deep lake it is correct. Here it is wrong in two ways, and **both fail silently**
— no splash, no log, no counter:

1. **It is a point test at discrete times.** A cube thrown at the top of this
   project's speed range (1600 UU/s = 16 m/s,
   `VoxelEarthPlayerController.h:148`) covers **26.7 cm in a 60 fps frame and
   53.3 cm at 30 fps** — 2.7 and 5.3 voxels. Water thinner than the step is
   stepped clean over: a 30 cm puddle, the shallow lip of a lake shelf, a river
   the CA has filled two voxels deep. Both endpoints are honestly dry and the
   object went straight through the wet part.
2. **It asks about the wrong point of the object.** The cube is 30 cm; its centre
   is 15 cm above its underside. A cube skimming the surface wets its bottom face
   and never its centre.

### What it does instead

Walk the segment the cube's **bottom-face centre** travelled this frame, one
sample per 10 cm, and fire on the first dry→wet sample. That is the same "test
the segment, not the endpoint" rule the terrain sweep already lives by, applied
to a surface instead of a solid.

- **Why 10 cm.** One voxel, and also exactly one texel of the ripple field
  (`VoxelRippleField.h:177`, `kTexelUU = 10.0`). Sampling finer would resolve
  detail the consumer throws away.
- **Why the bottom face.** We are deciding "did it touch", i.e. *first contact*.
  `VoxelCharacterMovement.cpp:630` makes the opposite choice — the character's
  **top** — for the opposite reason: it is deciding "am I swimming", i.e. *fully
  in*.
- **No sub-sample refinement.** Bisecting the 10 cm interval costs four more
  water queries and buys accuracy finer than one texel of a field that then
  discards the caller's Z entirely (`VoxelRippleField.h:222-224`).
- **Cost is bounded and stated.** `IsUnderwaterAtWorld` is a CA cell read plus,
  on a miss, one implicit worldgen column, and `VoxelWaterSubsystem.cpp:6999`
  says in as many words *"do not call this one per cell"*. So samples are capped
  at **16 per item per frame**: typical flight is 3, and the cap only binds above
  1.6 m of travel in one frame — a 96 m/s object at 60 fps, 6× anything this game
  can throw. Past that the step widens beyond a voxel and thin water can be
  missed again; that is logged once rather than pretended away. A settled cube
  costs nothing, because `Tick` returns before reaching the water code.
- **One splash per entry.** The flag is edge-triggered, so *being* submerged is
  not an event; only *becoming* submerged is. A 0.5 s cooldown backs that up in
  case a future buoyancy pass makes the bottom face oscillate across the surface.
  An object re-triggering every frame is the standard way this effect becomes a
  mess, and it is cheaper to make it impossible now than to diagnose it later.
- **An item thrown while already underwater does not splash.** The edge state is
  primed in `Launch()`, not on the first tick. The ripple field needed the
  identical guard and calls it `bSeen` (`VoxelRippleField.h:318-321`) — it was
  written there because debris promoted from a lake bed splashed on spawn.

### One thing this hook does better than the poller

The auto-watcher cannot see an object's velocity, so it infers one from
`(LastPos.Z - P.Z) / DeltaTime` (`VoxelRippleField.cpp:1062`) — the *average*
vertical speed over the frame containing the crossing, frame-rate dependent by
construction and written up as a known defect in
`docs/water-interactive-ripples.md` §12.4. We are inside the object and can ask
the projectile what its velocity **is**. Residual error: the projectile has
already integrated gravity for this frame, so the reading is the end-of-tick
speed — 16 cm/s high at 60 fps, 33 cm/s at 30. Against a ramp that saturates at
6 m/s that is ≤3%, and zero in the common case because a real throw is already
saturated.

### No double splash

`UVoxelRippleFieldSubsystem::AutoWatch` polls exactly two classes by `IsA` —
`AVoxelExplosive` and `AVoxelDebris` (`VoxelRippleField.cpp:1083`). This class is
invisible to it, so there is no double count. **If anyone adds
`AVoxelThrownItem` to that list, delete the hook in `VoxelThrownItem.cpp` in the
same commit.** Having both would splash twice, and the poller's estimate is
strictly worse.

---

## 3. How big the splash is, and why

Both numbers come from the cube and its impact speed. Neither is a constant
picked to look right.

**Impact fraction** `f = clamp(v_down / 6 m/s, 0.25, 1)`. The shape is copied
from the ripple field's own ramp (`VoxelRippleField.cpp:141,144`) so that a
thrown cube and the poller's estimate of the same cube agree; those constants are
file-local there and cannot be shared, so they are duplicated **with a citation**
rather than silently. Vertical speed, not total: the splash comes from the
velocity component normal to the surface, and lake water here is a flat sheet, so
normal means vertical. A cube skimmed flat across a lake genuinely should barely
ring.

**Radius.**

```
R = max(0.30, 0.169 × (1 + 2f))     →  0.30 m at a gentle drop … 0.51 m at ≥6 m/s
```

0.169 m is the equivalent-circle radius of the cube's 0.3 × 0.3 m face
(`sqrt(0.09/π)`) — the hole it makes in the surface at the instant of contact,
and the geometric floor. A faster entry opens a wider crown than the body itself,
hence the ramp. **The `1 + 2f` is chosen, not derived**, and only its endpoints
were reasoned about: at the bottom the ring is the cube's own footprint; at the
top it is 0.51 m, which lands next to the **0.5 m the ripple field already uses
for a whole player entering the water** (`voxel.Water.Ripple.PlayerRadiusM`). A
hard-thrown 30 cm cube making about the ring a person does is the right
relationship; the straight line between is a convenience.

The 0.30 m floor is not ours — it is `kMinDisturbanceRadiusM`
(`VoxelRippleField.h:205`), below which a raised-cosine bump on a 10 cm grid is
mostly grid and what radiates from it is checkerboard noise. **Note what that
means:** a 30 cm cube sits *at* the field's resolution floor, so for a gentle
drop the grid decides the radius and the geometry never gets a vote.

**Strength (ring height).**

```
H0 = cube volume / smallest representable ring area = 0.027 m³ / (π × 0.30²) = 0.0955 m
S  = H0 × f × voxel.Throwable.SplashScale            →  2.4 cm … 9.6 cm
```

The reassuring part: `H0` came from the cube's own geometry and landed **within
6% of the 0.09 m the ripple field independently chose for a player entering water
at full speed** (`voxel.Water.Ripple.PlayerStrengthM`). Two unrelated derivations
agreeing to 6% is the best evidence available that the scale is right, since
nothing here can be measured without a screenshot.

This is **not volume-conserving** and does not pretend to be. Strength is an
amplitude handed to a linear wave equation, not a volume of water; `H0` is a
scale the cube's size sets and `f` is the fraction of it the impact delivers.
For context on whether 9.6 cm is large: the ambient wind wave on this water is
8.6 cm crest-to-mean, so a full-speed throw makes a ring slightly bigger than the
sea state it lands in.

**In practice the ramp is almost always saturated.** A 16 m/s throw on a 30°
arc comes down at 8 m/s; even a flat 6 m/s throw across a 10 m gap arrives at
~5.9 m/s. The sub-6 m/s band exists mainly for `voxel.Throwable.DropAt` with a
low speed, which is how to see the small end.

Only one knob is exposed: `voxel.Throwable.SplashScale`, a level. The *shape* is
fixed in code, for the reason `VoxelRippleField.cpp:138-141` gives about its own
ramp — two knobs for one effect is how a tuning session stops converging.

---

## 4. Settling

`Settle()` stops the projectile, puts the cube's underside on the hit voxel's top
face with its XY snapped to the voxel lattice (which is the right look here, not
a rounding error), drops the tick rate to **4 Hz**, and starts the return timer.

A safety net copied in spirit from `AVoxelDebris::MaxFallSeconds`: if the DDA
never finds ground within 30 s — thrown into unstreamed space — it settles in
place and logs why. Otherwise a cube falling forever never starts its timer and
is lost silently, which is the exact failure this class is supposed to not have.

An item that sinks and lands on a **lake bed** settles there normally and is a
pickup underwater. That is v0 behaviour, not a decision anyone defended.

---

## 5. The five minutes, and the one-line switch

The owner's words were *"should respawn after 5 minutes if not picked back up by
player and put in inventory"*. Read here as **return to inventory**:

- `voxel.Throwable.ReturnSeconds` (default **300**) — read **at settle time**, so
  an item's fate is decided when it lands. `≤ 0` means never (a debugging state).
- On expiry: `TryAddItem(ItemId, Count)`, then **destroy the actor either way**.
- If the inventory takes none, the item is **logged as LOST and counted**, not
  silently deleted. `voxel.Throwable.Stat`'s `LOST` is the number that should
  mean something is wrong.

**The alternative reading — plain despawn — is one line:**

```
voxel.Throwable.ReturnToInventory 0
```

The actor is destroyed and the whole count goes to `lost` with a log line saying
the loss was intended. Nothing else changes.

### Pickup is different from timeout on purpose

Walking within `voxel.Throwable.PickupRadiusM` (default 1.5 m) of a settled item
picks it up, on the 4 Hz tick, after a 1 s arming delay (without which a cube
dropped at your own feet is back in your inventory before you see it land).

If the inventory takes **none**, the pickup **leaves the item on the ground**.
That is the opposite of what the timeout does, deliberately: the timeout has been
asked to remove the actor from the world and a pickup has not. Losing the
player's property because they walked past it would be a bug. A partial accept
keeps the remainder in the world.

### Wiring the inventory — already done, and why there are still two paths

The item system landed while this was being written. **`VoxelInventoryComponent.h:89`
is `int32 TryAddItem(FName ItemId, int32 Count)` returning how many were
accepted — exactly what this class was written against**, and that header's own
comment (`:71-75`) names `AVoxelThrownItem` as one of the callers its signatures
are fixed for. So the direct path compiles today and **nothing needs to be wired
for the return to work**.

Two paths remain, in priority order, both meeting in `GiveBackToThrower()`:

1. **A sink** — no compile dependency at all, and the escape hatch if the API
   ever moves. One line anywhere at startup:

   ```cpp
   AVoxelThrownItem::SetInventorySink(
       [](AActor* Thrower, FName ItemId, int32 Count) -> int32
       {
           // The component lives on the CONTROLLER (VoxelInventoryComponent.h:205-218),
           // and the thrower is the pawn, so the hop is required.
           UVoxelInventoryComponent* Inv = Thrower ? Thrower->FindComponentByClass<UVoxelInventoryComponent>() : nullptr;
           if (!Inv)
           {
               if (const APawn* P = Cast<APawn>(Thrower))
               {
                   if (AController* C = P->GetController()) { Inv = C->FindComponentByClass<UVoxelInventoryComponent>(); }
               }
           }
           return Inv ? Inv->TryAddItem(ItemId, Count) : 0;
       });
   ```

2. **The direct call**, compiled when `VoxelInventoryComponent.h` exists next to
   `VoxelThrownItem.cpp` (`__has_include`). It does the same three-place lookup
   itself — thrower, its controller, a controller's pawn — first hit wins.

**Where the component actually lives matters.** `UVoxelInventoryBootstrapSubsystem`
attaches it to the local **player controller**, not the pawn. The thrower passed
to `Launch()` should still be the **pawn**, because that is what the proximity
pickup measures distance to; the pawn → controller hop is what makes both work.

With neither path finding anything, every timeout is a logged loss. That is
visible, not silent — which is the whole point of counting it.

---

## 6. Seeing it, and photographing it

```
voxel.Throwable.ThrowHere [SpeedUU=1200] [PitchDeg=0] [Count=1]
voxel.Throwable.DropAt <XUU> <YUU> <ZUU> [DownSpeedUU=0] [Count=1]
voxel.Throwable.Stat
```

`ThrowHere` throws along the crosshair with no keypress and no inventory —
convenient, **not reproducible** (it depends on where the camera is standing).
`DropAt` is the deterministic one: an exact position and an exact downward speed,
so the crossing point, the impact fraction, the radius and the strength are
decided by the arguments and nothing else. Drop it 2–3 m above a known lake
surface **and within 25.6 m of the camera**.

Both throw the **real** registered id `throwable.voxelcube30`
(`VoxelItem.cpp:193`) rather than a placeholder, so the whole chain — throw,
splash, settle, five minutes, inventory — is exercised rather than half of it.
One consequence worth knowing: `ThrowHere` does **not** debit anything (the
controller owns that, §8.1), so a cube thrown from the console and left to time
out is a free item. That is a debug command creating an item from nothing, which
is fine for a debug command and would not be fine anywhere else. `DropAt` has no
thrower at all, so it always ends as a logged loss instead.

Neither command freezes the ripple field, and neither can: the splash happens
some frames after the spawn, and by then a capture has to be waiting for it. For
a pixel-identical A/B of the *field*, `voxel.Water.Ripple.Drop <X> <Y> r s Steps`
+ `voxel.Water.Ripple.Freeze 1` is still the right tool
(`docs/water-interactive-ripples.md` §10.3). These commands prove that **this
actor** fires the hook with the right numbers; their instruments are the splash
log line and `voxel.Water.Ripple.Stat`.

A working sequence:

```
voxel.Water.Ripple.Stat                 # note injected / dropped before
voxel.Throwable.DropAt <X> <Y> <Z+300> 400
voxel.Throwable.Stat                    # splashed should have gone up by 1
voxel.Water.Ripple.Stat                 # injected should have gone up by 1
```

`splashed` up and `injected` up = the hook works end to end. `splashed` up and
`droppedOutside` up = it works and you are standing too far away. `splashed` flat
= the crossing never fired, and the problem is in `VoxelThrownItem.cpp`.

### The splash log line

One line at the moment of entry, with everything a "the splash did nothing"
diagnosis needs, because the alternative is guessing from a screenshot:

```
VoxelThrownItem SPLASH item='throwable.voxelcube30' at (X, Y, Z) UU: down 8.12 m/s -> f=1.00,
r=0.51 m, s=0.096 m. Water surface Z=... UU (basin datum). Camera 41.3 m away
(window half-width 25.6 m) -- OUTSIDE THE RIPPLE WINDOW, ... Not a bug: get closer.
```

The surface height in that line comes from
`UVoxelWaterSubsystem::SubmergedDepthUUAtWorld`, **never from a trace** — see
§7.4 below. A surface reported as *"has NO datum"* is itself informative: it
means CA-only water (`VoxelWaterSubsystem.h:382-387`), which is also precisely
the water the baked shore mask is most likely to call dry.

---

## 7. Four things that make a correct implementation look broken

Each of these deletes a *correct* splash. None of them is a bug in this class,
and all four have to be ruled out before touching it.

1. **The ripple field is a 51.2 m window that follows the camera**
   (`VoxelRippleField.h:176-178`). A splash more than 25.6 m away is discarded
   and counted as `outside`. **This is not a corner case for a thrown item** — a
   16 m/s throw on a 30° arc travels well over 25 m, so the most likely reason a
   perfectly correct splash is invisible is that the player threw it too far.
   The splash log line says so explicitly, in those words, rather than leaving it
   to be inferred from a counter afterwards.
2. **Ripple gain is held at 0 until the first simulated frame exists.** An
   undriven collection must fail to the old water, so `RippleFieldGain` rises
   above zero only after the subsystem has stepped once. A splash injected in the
   first frames of a level — or in a run with no pawn — is real, queued, and
   drawn at zero. `voxel.Water.Ripple.Stat` showing `steps=0` is the whole
   diagnosis.
3. **The baked bathymetry masks ripples to where water actually is.** The
   simulation is multiplied by the baked shoreline distance every step, so a
   splash on a spot the bake calls dry is deleted at the first step, silently and
   correctly. This bites hardest on water that exists only in the CA — a
   player-poured pool, a flooded dig, a freshly breached channel — which
   `IsUnderwaterAtWorld` reports as water (so our hook fires, correctly) and the
   bake has never heard of. `voxel.Water.Ripple.MaskEnable 0` is the diagnostic;
   if the ring appears with the mask off, the bake calls that spot dry.
4. **Lake water is a flat sheet at a per-basin datum, so the surface height at a
   point is not the terrain height there.** A downward trace answers the wrong
   question and would put the splash at the lake *bed*. Nothing in this class
   traces for water: entry comes from `IsUnderwaterAtWorld`
   (`VoxelWaterSubsystem.h:347`) and the reported surface from
   `SubmergedDepthUUAtWorld` (`:388`), which resolves through the basin ledger.
   The ripple field discards the caller's Z anyway (`VoxelRippleField.h:222-224`)
   — the XY is what matters — but the log line would lie, and a lying instrument
   is worse than none.

A fifth, from `docs/water-interactive-ripples.md` §10.2 and from the project's
standing rule: if the water looks like grey plastic, grep the generation log for
`Failed to compile Material` **before** diagnosing anything visual.
`create_sky_material.py` recreates `MPC_VoxelSky` and unbinds every dependent,
and this project has lost a night to that once already.

---

## 8. The integration edits, written out and NOT applied

Both files belong to the owner. Neither was touched.

### 8.1 `VoxelEarthPlayerController.*` — spawn and launch

Mirrors `OnChargeRelease` (`VoxelEarthPlayerController.cpp:650-692`) exactly;
`SpawnAndThrow` is the one call, so the key path and the console path stay the
same code.

```cpp
// VoxelEarthPlayerController.cpp -- add to the includes
#include "VoxelThrownItem.h"

// SetupInputComponent(), next to the F-key charge binding (line ~57):
InputComponent->BindKey(EKeys::G, IE_Pressed, this, &AVoxelEarthPlayerController::OnThrowItem);

// The handler. Same 30-degree arc and 80 UU muzzle offset as the explosive throw;
// no charge, because an item throw is a single press.
void AVoxelEarthPlayerController::OnThrowItem()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    FVector CameraLocation;
    FRotator CameraRotation;
    GetPlayerViewPoint(CameraLocation, CameraRotation);

    FRotator ThrowRotation = CameraRotation;
    ThrowRotation.Pitch = FMath::Clamp(CameraRotation.Pitch + ThrowUpwardArcDegrees, -89.f, 89.f);
    const FVector Dir = ThrowRotation.Vector();

    // Pay for the throw BEFORE it happens. This is the pattern the inventory's
    // own header writes out (VoxelInventoryComponent.h:100-105): read the slot,
    // look up the definition, check it can be thrown, and remove exactly one --
    // all-or-nothing, so the check and the payment are a single decision. A cube
    // that is in the world AND in the inventory is duplication, which is why
    // SpawnAndThrow deliberately does not touch the inventory itself: only the
    // thrower knows what was selected.
    UVoxelInventoryComponent* Inv = FindComponentByClass<UVoxelInventoryComponent>();
    if (!Inv)
    {
        return;
    }
    const int32 Slot = Inv->GetSelectedSlot();
    const FVoxelInventorySlot Selected = Inv->GetSlot(Slot);
    const FVoxelItemDef* Def = FVoxelItemRegistry::Find(Selected.ItemId);
    if (!Def || !Def->CanThrow() || !Inv->TryRemoveFromSlot(Slot, 1))
    {
        return;
    }

    // The item's own throw speed, not this class's explosive-charge constants
    // (throwable.voxelcube30 sets 1200 UU/s, VoxelItem.cpp:183).
    AVoxelThrownItem::SpawnAndThrow(World, CameraLocation + Dir * 80.0,
                                    Dir * Def->ThrowSpeedUUPerSec, GetPawn(),
                                    Selected.ItemId, 1);
}
```

Nothing else needs wiring: the return path compiles against the real
`TryAddItem` today (§5).

### 8.3 One number that is currently agreed rather than owned

`FVoxelItemDef::CubeEdgeMetres` is 0.3 m for `throwable.voxelcube30`
(`VoxelItem.cpp:181`), and `AVoxelThrownItem::EdgeUU` is 3 voxels × 10 cm = the
same 0.3 m, arrived at independently. The splash radius and strength are now
derived from `EdgeUU` rather than written as literals, so the arithmetic follows
whichever edge length it is given — but **the actor still sets its own size and
does not read the definition.** The day a second thrown size exists, `Launch()`
should take the edge from `FVoxelItemRegistry::Find(ItemId)->CubeEdgeMetres` and
scale the mesh and `HalfEdgeUU` from it. Until then, two numbers agreeing is fine
and one of them being wrong would be silent, so it is written down here.

### 8.2 `VoxelEarthHUD.*` — the stat line, optional

`AVoxelThrownItem::GetStatLine(World)` returns one preformatted string:

```
Throwables: 2 in flight, 1 settled | thrown 7, splashed 3, picked up 1, returned 0, LOST 0
```

```cpp
// VoxelEarthHUD.cpp, in the voxel.Debug >= 1 panel
DrawText(AVoxelThrownItem::GetStatLine(GetWorld()), FLinearColor::White, MarginPx, LineY,
         nullptr, 1.f, false);
```

The two gauges are counted by walking the actor list rather than kept as running
totals: a gauge maintained by increment/decrement drifts the first time an actor
dies by a route this class does not own (level teardown, PIE stop), and a
drifting gauge is worse than no gauge. The cumulative counters are per editor
process, not per PIE session.

---

## 9. What v0 is not, and what each thing should become

- **Pickup is a distance check against the thrower only**, on a 4 Hz tick. It
  should become: a sphere overlap that any player can trigger, an interact prompt
  rather than walk-over (walk-over pickup and a five-minute return timer will
  fight each other the moment items matter), and a server-authoritative add.
- **Multiplayer.** Nothing here replicates. The throw, the pickup and the
  inventory credit all have to become server-authoritative before this is
  anything but single-player scaffolding — the same authority split
  `AVoxelEarthPlayerController`'s dig/place intents already use.
- **No bounce.** With no terrain collision there is nothing to bounce off; the
  DDA branch that handles a wall hit is where a real bounce belongs — reflect the
  velocity about the hit voxel's face normal and damp it.
- **No tumble.** `bRotationFollowsVelocity` is off and the cube lands square. A
  spin during flight and a settle-to-square would read better.
- **Water entry is a single velocity multiply** (keeps 35% of speed), not drag.
  There is no buoyancy anywhere in this project, so a drag model would be the
  only physics in the water and would look stranger than none. Without the
  multiply the cube is on the lake bed within two frames of the splash and the
  player never connects the ring to the thing that made it.
- **One splash, no exit splash, no wake.** An object leaving the water, or
  dragging across it, makes nothing.
- **The cube is a placeholder mesh** with a best-effort tint, exactly as the
  explosive is.

---

## 10. What could not be verified

No compiler and no editor were run — builds are the owner's. Written carefully
against the surrounding code, but **unproven**:

- **None of the C++ has been compiled.** Highest-risk points, in order:
  1. The `__has_include` inventory block, which **is now live**: the item system
     landed mid-task, so `VoxelInventoryComponent.h` exists and the direct
     `TryAddItem` call is what builds. The signature was verified by reading
     `VoxelInventoryComponent.h:89`, not guessed — but this is the one place two
     agents' code meets, and it meets for the first time in your build.
  2. UHT parsing of the header — `static constexpr double` members in a `UCLASS`
     body (`AVoxelExplosive` and `AVoxelDebris` both do this, so it is proven in
     principle) and the `TWeakObjectPtr<AActor>` without a `UPROPERTY` (the
     ripple field does the same).
  3. `Projectile->Velocity |` dot product and `FVector` double/float mixing in
     the strength arithmetic.
- **The splash has never been seen.** No screenshot exists. The radius/strength
  mapping is arithmetic plus two coincidences with numbers the ripple field chose
  independently; whether 0.51 m and 9.6 cm *look* like a thrown crate hitting a
  lake is a question only a capture can answer, and per project doctrine the
  owner judges that, not me.
- **The `bFloorHit` test** (`HitVoxelCenter.Z < PrevVoxelCenter.Z - 1.0`) assumes
  the DDA steps one axis per cell, so that the hit and last-empty voxels differ
  on exactly one axis. That reading of `voxelcore/raycast.h` is from
  `RaycastVoxelWorld`'s documented contract (`VoxelWorldSubsystem.h:354-366`),
  not from running it.
- **`SubmergedDepthUUAtWorld` is called once per splash from `Tick`.** It
  `check(IsInGameThread())` and does lazy fine-tile decode; `AVoxelOceanActor`
  already calls it per tick, so once per splash is within its stated envelope —
  but the cost was not measured.
- **The 16-sample cap has never been hit** in anything that ran, because nothing
  ran. The arithmetic says it binds above 1.6 m of travel per frame.
