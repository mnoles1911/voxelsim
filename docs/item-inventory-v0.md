# Items and player inventory, v0

Built 2026-08-13. Four new files, no existing file touched:

- `ue-project/Source/VoxelEarth/VoxelItem.h` / `.cpp` — what an item **is**.
- `ue-project/Source/VoxelEarth/VoxelInventoryComponent.h` / `.cpp` — what a
  player **has**.

Nothing in the game calls into either yet. The integration edits are written out
in full at the bottom of this document rather than applied, because the three
files they touch (`VoxelEarthPlayerController`, `VoxelEarthFlyPawn`,
`VoxelEarthHUD`) belong to their owner and a new `VoxelThrownItem.*` belongs to
another agent. **This code has not been compiled** — see "Not verified" at the
end for the specific things a build will settle.

The scope is deliberately one throwable and fourteen blocks. The thing that is
*not* small is the shape: the point of v0 is that tools, placeable blocks,
throwables and stacks all fit it without a rewrite.

## The one decision that mattered: item id vs material id

A placeable block already has an identity — `vxc::MaterialId`, a `uint8`, the
number that goes in the edit log, replicates to every client and sits in save
files. An item has an `FName`. Two identity spaces for the same rock is how this
kind of system rots, so:

> **The material id is the identity of a block. The item id is the identity of a
> thing you can hold. Block items are derived from the material palette, in one
> table, in `VoxelItem.cpp`, and nowhere else.**

Concretely: `kBlockTable` has one row per placeable material, and each row
carries the `vxc::MAT_*` **symbol** (not a copied number), the item id and the
display name. Both identities are born on the same line, so they cannot drift —
there is no second line to drift from. Both directions of the mapping,
`Find("block.rock")` and `FindBlockForMaterial(vxc::MAT_ROCK)`, are indexes over
those same rows.

Why not key the inventory by material id and drop `FName` entirely? Because the
inventory has to hold things that are not blocks. A shovel has no material. That
would force every non-block item to invent a fake material id, and the material
space is append-only *forever* because saved edit logs depend on it
(`voxelcore/core.h:322`). Polluting it with things that were never terrain is a
much worse coupling than a hash lookup.

Why not make the item id authoritative and treat the material as a lookup
detail? Because the placement path is `uint8` end to end — `PaletteMaterialId`
(`VoxelEarthPlayerController.h:138`), the `ServerSubmitPlaceIntent` RPC
(`:55`), `UVoxelWorldSubsystem::TryPlace`. An `FName`→`uint8` resolve inside the
replicated edit path turns a bad item id into a placement that silently does
nothing.

So: **`uint8` downward** (into the world, the edit log, the wire), **`FName`
sideways** (inventory, hotbar, UI, item save data), and exactly one table where
they meet.

What would change it: a block whose identity is finer than its material — a
"mossy rock" that places `MAT_ROCK` but is a different item, or a block that
places two materials. Then `FindBlockForMaterial` stops being a function and has
to return the *default* item for a material. The table gains a column; no caller
of `Find()` changes. That is why the reverse lookup is a named function and not a
public `TMap`.

### The corollary: one name per material

`VoxelEarthHUD.cpp:22-31` already maps material id → `"Rock"`/`"Soil"`/`"Sand"`.
The item registry would otherwise be a second table of the same facts. The three
display names in `kBlockTable` were **copied from that switch verbatim**
(including `"Soil"` for `MAT_TOPSOIL`, and `"?"` as the fallback), so the HUD edit
below is invisible on screen and there is one name per material afterwards.

## The API as built

```cpp
// VoxelItem.h
enum class EVoxelItemCategory : uint8 { Block, Tool, Throwable, Misc };

struct FVoxelItemDef
{
    FName    ItemId;                    // "block.rock", "throwable.voxelcube30"
    FString  DisplayName;               // "Rock"
    EVoxelItemCategory Category;
    int32    MaxStack;                  // 1 = never stacks
    uint8    BlockMaterialId;           // vxc::MaterialId, 0 = not made of one
    bool     bCanThrow;
    float    CubeEdgeMetres;            // 0.3 = a 30 cm cube
    float    MassKg;
    float    ThrowSpeedUUPerSec;        // uncharged launch speed
    bool  IsBlock() const; bool CanThrow() const; bool IsStackable() const;
    float CubeEdgeUU() const;           // metres * 100
};

struct FVoxelItemRegistry
{
    static const FVoxelItemDef*        Find(FName ItemId);                 // null if unknown
    static const FVoxelItemDef*        FindBlockForMaterial(uint8 MatId);  // BLOCK items only
    static const TArray<FVoxelItemDef>& AllItems();
    static FString                     DisplayNameForMaterial(uint8 MatId); // "?" if none
    static void                        LogAll();
};

namespace VoxelItemIds { FName ThrowCube30(); FName Block(uint8 MaterialId); }
```

```cpp
// VoxelInventoryComponent.h
USTRUCT() struct FVoxelInventorySlot { FName ItemId; int32 Count = 0; bool IsEmpty() const; };

class UVoxelInventoryComponent : public UActorComponent
{
    int32               TryAddItem(FName ItemId, int32 Count);      // returns count actually added
    bool                TryRemoveFromSlot(int32 SlotIndex, int32 Count); // all-or-nothing
    FVoxelInventorySlot GetSlot(int32 SlotIndex) const;             // out of range => empty
    int32               NumSlots() const;
    int32               GetSelectedSlot() const;
    void                SetSelectedSlot(int32 SlotIndex);           // clamps, logs if it had to
    void                SelectNextSlot();  void SelectPrevSlot();   // wrap
    // convenience
    FName               GetSelectedItemId() const;                  // NAME_None if empty
    int32               CountOf(FName ItemId) const;
    void                SeedDefaultsOnce();                         // idempotent
    FString             DescribeForLog() const;
    static UVoxelInventoryComponent* FindForLocalPlayer(const UWorld* World);
    static UVoxelInventoryComponent* EnsureForLocalPlayer(UWorld* World);
};
```

The six contract signatures are exactly as specified. Everything else is
additive.

Three shapes worth calling out, because each was a fork:

- **`TryAddItem` returns a count, not a bool.** A full inventory is normal, not
  an error, and only the caller knows what to do with the remainder (a pickup
  stays on the ground; a craft refuses). Ignoring the return value is how items
  get deleted by a full backpack.
- **`TryRemoveFromSlot` is all-or-nothing.** A partial "consume one cube, then
  throw it" leaves you having paid for a throw that never happened. To learn
  *what* came out, call `GetSlot()` first — the thrower needs the id anyway to
  look up the definition.
- **Adding fills partial stacks before empty slots.** A hotbar that fragments
  into three half-stacks of rock is the commonest complaint about naive
  inventories and costs one extra loop to avoid.

## Contents of the registry

Fifteen items: fourteen blocks, one throwable, no tools.

| item | why |
|---|---|
| `block.rock`, `block.sand`, `block.soil` | the three the palette cycles today |
| `block.bedrock/gravel/subsoil/snow/grass/junglesoil/savannagrass/podzol/permafrost/mud/clay` | the rest of the amplifier's materials; one line each, so expanding the palette is a palette change and not a new table |
| `throwable.voxelcube30` | 0.3 m cube, 5 kg, 1200 UU/s, stack 16 — what slot 0 seeds |

Deliberately absent: `MAT_AIR` (a "hold some air" item would make
`BlockMaterialId == 0` mean two things), `MAT_WATERMARK` (a debug instrument),
`MAT_BARK…MAT_LEAF_AUTUMN` (asset materials — a bark block is a reasonable future
item but it is a design decision, not a mechanical consequence of the material
existing), and `MAT_SKIN_*` (creature colours, never placeable).

Two numbers that will otherwise get "fixed" by someone doing arithmetic:

- **The cube's 5 kg is a gameplay number, not a density.** 0.3 m cubed is
  0.027 m³, which at rock's ~2000 kg/m³ is **54 kg**. Nobody throws 54 kg twelve
  metres. If projectile physics ever wants true density that is a new field on
  the block rows, not a reinterpretation of this one.
- **The cube's 1200 UU/s reproduces the existing charge range exactly.** The
  controller charges 600 → 1600 UU/s over a 0.3 s → 1.5 s hold
  (`VoxelEarthPlayerController.h:145-148`). 1200 × 0.5 = 600 and
  1200 × 1.3333 = 1600, so a charge that lerps **0.5× → 1.3333× of the item's own
  default speed** leaves the existing throw feeling identical while making the
  range a property of the item.

The registry is a hard-coded C++ table. That is right for now: fifteen rows,
every block row derived from a `vxc::MAT_*` symbol a data asset could not
reference safely, and a compiled table cannot be half-loaded or missing on a
headless run. **What would force it to become a `UDataTable`/`UDataAsset`:** a
non-programmer needing to add items (sufficient on its own); items needing asset
references (mesh, icon, sound — hard-coding those paths in C++ is how you get a
silent null instead of a cook-time error); or items differing per game mode or
arriving from a plugin. When it moves, `FVoxelItemDef` becomes an
`FTableRowBase` row and `Find()` keeps its signature, which is why nothing gets a
reference to the table itself.

## Console visibility

Five commands. The rule they serve is the standing one: a stage must emit a
ran-flag distinguishable from "found nothing", and "my throw did nothing" must be
answerable without a debugger.

| command | what it answers |
|---|---|
| `voxel.Item.List` | what *could* be held, with every number |
| `voxel.Inventory.Dump` | what *is* held, plus the counters |
| `voxel.Inventory.Give <ItemId> [Count]` | put items in (prefix optional: `rock` finds `block.rock`) |
| `voxel.Inventory.Select <Slot>` | change selection (the number keys are taken — see below) |
| `voxel.Inventory.Clear` | empty everything, to test the refusal path |

`Dump` separates the five reasons a throw can do nothing, which look identical on
screen:

- no component at all → says so explicitly, as a *different outcome* from empty;
- component exists but empty → slots print `--`;
- something added an unknown item → `rejected(unknown=N)`;
- inventory was full → `rejected(noroom=N)`, counted in **items**, not calls;
- the thrower asked and was refused → `removes: … N refused`.

All counters at zero means nothing ever called in, i.e. the problem is upstream
of the inventory. That reading is the whole reason the counters exist.

The registry logs one line when it builds (`built 15 item(s) — 14 block, …,
0 error(s)`), and the component logs one when it seeds
(`10 slot(s) …; seeded 16/16 x 'throwable.voxelcube30'`).

### cvars

| cvar | default | note |
|---|---|---|
| `voxel.Inventory.Slots` | 10 | read once at first init; clamped 1..64 |
| `voxel.Inventory.Seed` | 1 | 0 starts empty — the arm that proves an empty-handed throw is refused |
| `voxel.Inventory.SeedCount` | 16 | clamped by the item's own max stack |
| `voxel.Inventory.AutoAttach` | 1 | the scaffold below |

### The auto-attach scaffold, and when to delete it

`UVoxelInventoryBootstrapSubsystem` (a `UWorldSubsystem` in
`VoxelInventoryComponent.h/.cpp`) attaches the component to the local player
controller at world begin-play, retrying every 0.5 s for 10 s because the
controller does not exist at begin-play in every net mode.

It exists because the component has to live on an actor, this agent does not own
`VoxelEarthPlayerController.cpp`, and an inventory nobody can reach is an
inventory nobody can test. It is **scaffolding**: once the controller creates the
component itself (edit 2 below), the subsystem finds it already there and creates
nothing, and it should then be deleted. It never runs on a dedicated server (no
local player — it says so once) and never in an editor world.

## What v0 deliberately does not do

- **No replication.** This project has a working dedicated-server path
  (`docs/m3-plan.md`, join sync, the `ServerSubmit*Intent` RPCs); a client's
  inventory here is purely local and two clients would not agree. The fix is
  shaped for: all mutation already goes through `TryAddItem` /
  `TryRemoveFromSlot`, so it means marking `Slots` replicated, making those two
  server-only, and adding a client RPC for the local prediction — the pattern
  the edit path already uses. No caller changes.
- **No persistence.** Nothing writes the inventory to a save. `FName` ids were
  chosen so that when something does, an id from an older build still resolves.
- **No pickup, no drop, no crafting, no durability, no weight limit, no cooldown.**
- **No UI.** The HUD does not draw a hotbar; `Dump` is the only view.
- **No tools.** Digging is a controller verb with a size dial
  (`DigSizeVoxels`), not a held object. A "Shovel" that appeared in the hotbar
  and changed nothing when selected would be a lie told by the HUD. The
  `Tool` enumerator exists so the first real one is a table row plus the field it
  needs (a dig size, or a reach) — that is the actual test of whether this shape
  scales.
- **The explosive is not an item.** `AVoxelExplosive` is thrown by holding F,
  which spawns the actor directly and consumes nothing
  (`VoxelEarthPlayerController.cpp:683-691`). A row for it now would print in
  every dump for a thing the inventory does not gate, and "the dump says three
  but F throws forever" is exactly the ambiguity the ran-flag rule exists to
  prevent. Its row is written out in edit 5.
- **Blocks are `bCanThrow = false`.** Not because throwing a block is a bad idea
  — because a thrown block that fails to become a voxel where it lands looks
  broken. It is one column of the table once the thrown item can deposit.
- **No hotbar/backpack split.** The hotbar *is* the inventory. When a backpack
  arrives, the first N slots stay the hotbar and the wrap in
  `SelectNextSlot`/`SelectPrevSlot` bounds to N instead of `NumSlots()` — one
  line, which is why the wrap is written in one place.

## Integration edits NOT applied

Each is complete and self-contained. Edits 1–3 make the inventory real; 4 and 5
are the throw.

### 1. `VoxelEarthHUD.cpp` — delete the duplicate name table

Removes the second material→name mapping. Replace lines 16-31 (the anonymous
namespace's `PaletteMaterialName`) with a forward to the registry, and add
`#include "VoxelItem.h"` to the include block:

```cpp
// vxc::MaterialId -> display name now comes from the item registry
// (VoxelItem.h), which derives block items from the material palette. This
// used to be a local switch over 2/6/4; two tables of the same names is one
// table too many, and the registry's strings were copied from this one, so
// nothing on screen changes.
FString PaletteMaterialName(uint8 MaterialId)
{
    return FVoxelItemRegistry::DisplayNameForMaterial(MaterialId);
}
```

`VoxelEarthHUD.cpp:134` is unchanged. `"?"` is still the fallback.

Optional, once a hotbar exists — the selected item next to `Place:`:

```cpp
if (const UVoxelInventoryComponent* Inv = UVoxelInventoryComponent::FindForLocalPlayer(GetWorld()))
{
    const FVoxelInventorySlot Slot = Inv->GetSlot(Inv->GetSelectedSlot());
    const FVoxelItemDef* Def = FVoxelItemRegistry::Find(Slot.ItemId);
    const FString HeldText = Slot.IsEmpty()
        ? FString::Printf(TEXT("Slot %d: (empty)"), Inv->GetSelectedSlot())
        : FString::Printf(TEXT("Slot %d: %s x%d"), Inv->GetSelectedSlot(),
                          Def ? *Def->DisplayName : *Slot.ItemId.ToString(), Slot.Count);
    LineY += LineHeightPx;
    DrawText(HeldText, FLinearColor::White, MarginPx, LineY, nullptr, 1.f, false);
}
```

### 2. `VoxelEarthPlayerController.h/.cpp` — own the component

Header, private section:

```cpp
// The player's inventory (VoxelInventoryComponent.h). On the controller and
// not the pawn: this is the grown-up version of PaletteMaterialId and
// DigSizeVoxels below, it survives possession changes, and it is the actor a
// client may send owned RPCs on when this becomes server-authoritative.
UPROPERTY()
TObjectPtr<class UVoxelInventoryComponent> Inventory;
```

`.cpp`, in a constructor (the controller has none yet — add
`AVoxelEarthPlayerController();`):

```cpp
Inventory = CreateDefaultSubobject<UVoxelInventoryComponent>(TEXT("VoxelInventory"));
```

Once this lands, set `voxel.Inventory.AutoAttach 0` (or delete
`UVoxelInventoryBootstrapSubsystem`) — with the component present the subsystem
already does nothing, so this is tidying, not a fix.

### 3. Hotbar keys — what is actually free

Bound today across the pawn and controller: `W A S D`, `Space`, `LeftCtrl`,
`LeftShift`, `LeftAlt`, `C`, `G`, `Q`, `V`, `T`, `F`, `[`, `]`, `1` (water
bucket — a specific request, Matt 2026-07-29, `VoxelEarthPlayerController.cpp:41-47`,
**do not redefine**), `2/3/4` (dig sizes), `F1`, `F3`, arrows, `Enter`, both
mouse buttons and the wheel (the movement-speed dial).

Free and sensible: `Z`/`X` (prev/next, left hand, next to nothing), `R`,
`Tab`, `MiddleMouseButton`, `ThumbMouseButton`/`ThumbMouseButton2`, and the
number keys `5`–`0`.

Recommendation — `X`/`Z` for next/prev, leaving direct slot selection to the
console until a real hotbar UI exists:

```cpp
InputComponent->BindKey(EKeys::X, IE_Pressed, this, &AVoxelEarthPlayerController::OnNextHotbarSlot);
InputComponent->BindKey(EKeys::Z, IE_Pressed, this, &AVoxelEarthPlayerController::OnPrevHotbarSlot);
```

Do **not** take the mouse wheel: it was deliberately moved off dig size onto the
movement speed dial (`VoxelEarthPlayerController.cpp:36-40`), and taking it back
would undo a decision that was made on purpose.

### 4. The throw — the contract `VoxelThrownItem` is written against

`AVoxelThrownItem` should mirror `AVoxelExplosive`'s spawn/launch shape (spawn,
then an explicit `Launch(velocity)`, so the thrower controls the exact velocity)
and read its size and mass from the item definition instead of constants.
Voxel-space collision is not optional: terrain has no Chaos collision by design,
so a projectile that only asks `UProjectileMovementComponent` falls through the
world — that already happened once and was reported as "charges do nothing"
(`VoxelExplosive.h:72-82`). Step the DDA raycast along the segment travelled each
frame, as the explosive does.

The controller side, consuming from the selected slot:

```cpp
void AVoxelEarthPlayerController::OnThrowSelected()
{
    UWorld* World = GetWorld();
    UVoxelInventoryComponent* Inv = UVoxelInventoryComponent::FindForLocalPlayer(World);
    if (!World || !Inv)
    {
        UE_LOG(LogVoxelEarth, Warning, TEXT("Throw: no inventory on the local player."));
        return;
    }

    const int32 SlotIndex = Inv->GetSelectedSlot();
    const FVoxelInventorySlot Slot = Inv->GetSlot(SlotIndex);
    const FVoxelItemDef* Def = FVoxelItemRegistry::Find(Slot.ItemId);
    if (!Def || !Def->CanThrow())
    {
        // The refusal SAYS SO. An empty hand and a bug look identical otherwise.
        UE_LOG(LogVoxelEarth, Log, TEXT("Throw: slot %d holds %d x '%s' -- nothing throwable in hand."),
               SlotIndex, Slot.Count, *Slot.ItemId.ToString());
        return;
    }
    if (!Inv->TryRemoveFromSlot(SlotIndex, 1))
    {
        UE_LOG(LogVoxelEarth, Log, TEXT("Throw: slot %d refused (holds %d)."), SlotIndex, Slot.Count);
        return;
    }

    // Charge maps to 0.5x..1.3333x of the ITEM's own speed, which reproduces
    // the explosive's existing 600..1600 UU/s from its 1200 (see above).
    const float Alpha = FMath::Clamp((HeldSeconds - MinChargeSeconds) / (MaxChargeSeconds - MinChargeSeconds), 0.f, 1.f);
    const float SpeedUU = Def->ThrowSpeedUUPerSec * FMath::Lerp(0.5f, 4.f / 3.f, Alpha);
    // ... same 30-degree arc, same 80 UU muzzle offset as OnChargeRelease ...
    if (AVoxelThrownItem* Thrown = World->SpawnActor<AVoxelThrownItem>(...))
    {
        Thrown->InitFromItem(*Def);            // edge = Def->CubeEdgeUU(), mass = Def->MassKg
        Thrown->Launch(ThrowDirection * SpeedUU);
    }
}
```

If the throw is put on a key rather than replacing F, note that `F` is charge/
throw for the explosive and `R` is free.

### 5. The explosive's row, for the day F reads the inventory

Add to `VoxelItem.cpp` next to `MakeThrowCube30`, using the constants already in
`VoxelExplosive.h:64-67` — do not restate them, include the header and reference
them, or the crater tuning and the item drift apart:

```cpp
Def.ItemId = FName(TEXT("throwable.explosive"));
Def.DisplayName = FString(TEXT("Explosive charge"));
Def.Category = EVoxelItemCategory::Throwable;
Def.MaxStack = 8;
Def.BlockMaterialId = 0;             // not made of a world material
Def.bCanThrow = true;
Def.CubeEdgeMetres = 0.2f;           // matches the blast mesh
Def.MassKg = 1.0f;
Def.ThrowSpeedUUPerSec = 1200.0f;    // same 0.5x..1.3333x charge range
```

## Not verified — no compiler was run

Nothing here has been built. In likely-to-bite order:

1. **A component registered at runtime and `BeginPlay`.** `EnsureForLocalPlayer`
   does `NewObject` + `RegisterComponent` on the player controller.
   `RegisterComponentWithWorld` is supposed to call `BeginPlay` when the owner
   has already begun play, but this path must not depend on that in every net
   mode — so `SeedDefaultsOnce()` is idempotent and is called explicitly as well.
   If `BeginPlay` does fire, the second call returns immediately. Worst case if
   the assumption is wrong in the other direction: seeding happens once, from the
   explicit call, which is the intended behaviour anyway.
2. **UHT.** `USTRUCT`/`UENUM`/`UCLASS` in two new headers; `FVoxelItemDef` is a
   `USTRUCT` with `VOXELEARTH_API`, and `FVoxelItemRegistry` is a plain exported
   struct in the same header, which UHT should ignore.
3. **`UWorldSubsystem` overrides.** `OnWorldBeginPlay(UWorld&)` and
   `DoesSupportWorldType` were matched to `WorldSubsystem.h` in UE 5.8 at
   `D:\UE_5.8` and to `UVoxelAgentSubsystem`'s usage, not guessed.
4. **`voxelcore/core.h` in `VoxelItem.cpp`.** Nine other files in the module
   include it directly (six `.cpp` plus three non-UHT headers); the *UHT-parsed*
   headers are what must stay voxel-core-free, and both new headers are. `VoxelItem.cpp` does not include `VoxelCoords.h`, which
   states it is the one file allowed to include both (`VoxelCoords.h:20-22`) —
   the only conversion needed here is metres→centimetres, which needs neither.
5. **Log format specifiers.** `%-24s` with an `FString` in `UE_LOG`, and `%lld`
   with `static_cast<long long>` on the counters (the project's own convention).
6. **`TAutoConsoleVariable<bool>::GetValueOnAnyThread()`** is used from
   `SeedDefaultsOnce`, which runs on the game thread; `GetValueOnGameThread` is
   used in the subsystem. Both forms exist in this engine version and both are
   used elsewhere in the module.
