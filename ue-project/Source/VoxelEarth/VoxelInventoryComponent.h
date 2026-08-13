#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Subsystems/WorldSubsystem.h"
#include "VoxelInventoryComponent.generated.h"

// ---------------------------------------------------------------------------
// PLAYER INVENTORY v0
// ---------------------------------------------------------------------------
//
// N slots of (item id, count), one selected slot, and the four verbs that any
// inventory has to get right: add, remove, read, select. What an item IS lives
// in VoxelItem.h -- this file only ever holds an FName and a number, which is
// the point (a slot must never carry a copy of the item's mass or stack size,
// or two players holding the same item can disagree about it).
//
// WHERE THIS COMPONENT LIVES: on the PLAYER CONTROLLER, not the pawn. Three
// reasons, in order of weight:
//   1. The controller already holds the state this is the grown-up version of
//      -- PaletteMaterialId and DigSizeVoxels (VoxelEarthPlayerController.h:134,
//      138) are a two-item proto-inventory in all but name.
//   2. The controller survives possession changes; this project already swaps
//      pawn behaviour between fly and walk mode (AVoxelEarthFlyPawn::SetWalkMode)
//      and a future respawn must not drop your inventory on the floor.
//   3. The controller is the actor UE lets a client send owned RPCs on
//      (VoxelEarthPlayerController.h:39-50 spells out exactly this), so when the
//      inventory becomes server-authoritative it is already in the right place.
//
// NOT REPLICATED IN v0, and that is a real gap rather than an oversight -- this
// project has a working dedicated-server path (docs/m3-plan.md, join sync, the
// ServerSubmit*Intent RPCs). A client's inventory here is purely local: two
// clients would each see their own seeded stack and neither would agree with the
// server. What v0 gets right is the SHAPE for fixing that: all mutation goes
// through TryAddItem/TryRemoveFromSlot, so making it authoritative means marking
// Slots replicated, making those two functions server-only, and adding a client
// RPC for the local prediction -- the same pattern the edit path already uses.
// No caller changes.
//
// SEEDING: slot 0 starts with a stack of 30 cm throwable voxel cubes, because
// that is what the owner wants to test today. Controlled by
// voxel.Inventory.Seed / voxel.Inventory.SeedCount, and it logs.

// One slot. Empty means Count == 0; ItemId is cleared to NAME_None at the same
// time so an empty slot can never be mistaken for a zero-count stack of
// something (that distinction is a classic source of "I have -1 rocks").
USTRUCT()
struct FVoxelInventorySlot
{
	GENERATED_BODY()

	UPROPERTY()
	FName ItemId;

	UPROPERTY()
	int32 Count = 0;

	bool IsEmpty() const { return Count <= 0 || ItemId.IsNone(); }
};

UCLASS(ClassGroup = (VoxelEarth), meta = (BlueprintSpawnableComponent))
class VOXELEARTH_API UVoxelInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVoxelInventoryComponent();

	virtual void BeginPlay() override;

	// --- The contract other code is written against -------------------------
	//
	// These six signatures are fixed: AVoxelThrownItem, the HUD and the player
	// controller are all being written against them in parallel. Anything added
	// later goes alongside; none of these change shape.

	// Add up to Count of ItemId, filling PARTIAL STACKS OF THE SAME ITEM FIRST
	// (in slot order) and only then taking empty slots. Returns how many were
	// actually taken, 0..Count.
	//
	// RETURNS A COUNT, NOT A BOOL, and callers must use it. A full inventory is
	// the normal case, not an error, and the caller is the only code that knows
	// what to do with the remainder -- a pickup should stay on the ground, a
	// crafting output should refuse to craft. Discarding the return value is how
	// items get deleted by a full backpack.
	//
	// An unknown ItemId adds nothing and logs (a save from a newer build, or a
	// typo in a console command). Count <= 0 adds nothing and logs.
	int32 TryAddItem(FName ItemId, int32 Count);

	// Remove exactly Count from SlotIndex. ALL OR NOTHING: if the slot holds
	// fewer, nothing is removed and this returns false.
	//
	// Partial removal was the other option and it is wrong for the caller this
	// exists to serve. "Consume one cube, then throw it" that half-succeeds
	// leaves you having paid for a throw that never happened -- or worse, throws
	// an item you did not have. All-or-nothing makes the check and the payment a
	// single decision.
	//
	// To know WHAT came out, call GetSlot() first: the thrower needs the item id
	// anyway to look up its definition, so the pair reads
	//   const FVoxelInventorySlot Slot = Inv->GetSlot(Inv->GetSelectedSlot());
	//   if (const FVoxelItemDef* Def = FVoxelItemRegistry::Find(Slot.ItemId))
	//       if (Def->CanThrow() && Inv->TryRemoveFromSlot(Inv->GetSelectedSlot(), 1))
	//           ... spawn ...
	bool TryRemoveFromSlot(int32 SlotIndex, int32 Count);

	// A copy of the slot. An out-of-range index returns an empty slot (Count 0,
	// ItemId NAME_None) rather than asserting -- a HUD drawing ten boxes over a
	// six-slot inventory should draw four empty boxes, not crash.
	FVoxelInventorySlot GetSlot(int32 SlotIndex) const;

	int32 NumSlots() const { return Slots.Num(); }

	int32 GetSelectedSlot() const { return SelectedSlot; }

	// CLAMPS rather than ignoring or wrapping, and logs when it had to. The
	// signature is void, so a rejected index would be a silent no-op, and the
	// symptom would be "key 7 does nothing" with no way to tell a bad index from
	// an unbound key.
	void SetSelectedSlot(int32 SlotIndex);

	// Hotbar step, wrapping at both ends. Mouse-wheel-shaped, though the wheel
	// on this project is already the movement speed dial
	// (VoxelEarthPlayerController.cpp:36-40) -- see docs/item-inventory-v0.md for
	// the binding options that are actually free.
	void SelectNextSlot();
	void SelectPrevSlot();

	// --- Convenience (not part of the fixed contract, safe to use) ----------

	// The selected slot's item, or NAME_None if it is empty.
	FName GetSelectedItemId() const;

	// Total of ItemId across all slots. For "do I have any" and for the HUD.
	int32 CountOf(FName ItemId) const;

	// Seeds slot 0 once (see the class comment). Idempotent -- safe to call from
	// BeginPlay and from EnsureForLocalPlayer, which is exactly what happens,
	// because a component registered after its owner has already begun play may
	// or may not get a BeginPlay call and this must not depend on that.
	void SeedDefaultsOnce();

	// Multi-line human-readable dump: every slot, the counters, and the seeding
	// state. Used by `voxel.Inventory.Dump`; also callable from anywhere that
	// wants to log inventory state next to its own failure.
	FString DescribeForLog() const;

	// --- Finding the local player's inventory -------------------------------
	//
	// Every consumer needs this and none of them should re-implement the search,
	// because the component may hang off the controller OR (if someone wires it
	// there) the pawn, and a consumer that only checks one will silently find
	// nothing.

	// The local player's inventory, or nullptr. Checks the first player
	// controller, then its pawn.
	static UVoxelInventoryComponent* FindForLocalPlayer(const UWorld* World);

	// Same, but creates and registers one on the local player controller if
	// there is none. Returns nullptr only when there is no local player
	// controller yet.
	//
	// This is what lets the inventory exist WITHOUT AN EDIT TO
	// AVoxelEarthPlayerController -- that file belongs to someone else and this
	// component would otherwise be unreachable and untestable until they got to
	// it. Once the controller creates one in its constructor, this finds that one
	// and creates nothing. Gated off with voxel.Inventory.AutoAttach 0.
	static UVoxelInventoryComponent* EnsureForLocalPlayer(UWorld* World);

private:
	UPROPERTY()
	TArray<FVoxelInventorySlot> Slots;

	int32 SelectedSlot = 0;

	bool bSeeded = false;
	int32 SeededCount = 0;

	// --- Counters: the ran-flag -------------------------------------------
	//
	// The standing rule on this project is that a stage must emit something that
	// distinguishes "ran and found nothing" from "never ran". For an inventory
	// the question is always the same one -- "I pressed throw and nothing
	// happened" -- and these separate the five different reasons, none of which
	// look any different on screen:
	//   * the component does not exist            -> Dump says so
	//   * it exists but is empty                  -> AddCalls 0, or AddedTotal 0
	//   * something tried to add an unknown item  -> AddRejectedUnknown > 0
	//   * it was full                             -> AddRejectedNoRoom > 0
	//   * the thrower asked and was refused       -> RemoveRejected > 0
	// A throw that never called in at all leaves every counter at zero, which is
	// the reading that says the problem is upstream of here.
	int64 AddCalls = 0;
	int64 AddedTotal = 0;
	int64 AddRejectedBadArgs = 0;
	int64 AddRejectedUnknown = 0;
	int64 AddRejectedNoRoom = 0;
	int64 RemoveCalls = 0;
	int64 RemovedTotal = 0;
	int64 RemoveRejected = 0;
	int64 SelectCalls = 0;
};

// Attaches the inventory to the local player controller without anyone having to
// edit that controller.
//
// WHY THIS EXISTS: the component has to live on an actor, this agent does not own
// AVoxelEarthPlayerController.cpp, and an inventory nobody can reach is an
// inventory nobody can test. A world subsystem can reach the controller from the
// outside, so it does.
//
// IT IS EXPLICITLY A SCAFFOLD. When the controller creates the component itself
// -- the right long-term home, one line in its constructor -- this subsystem
// finds it already there and does nothing, and the day the last of its retries
// is provably pointless it should be deleted. voxel.Inventory.AutoAttach 0
// disables it. It never runs on a dedicated server (no local player) and never
// in an editor-preview world.
UCLASS()
class VOXELEARTH_API UVoxelInventoryBootstrapSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

protected:
	// Game and PIE only -- matching every other gameplay subsystem in this module
	// (UVoxelAgentSubsystem, UVoxelSkySubsystem, ...). An editor world has no
	// player controller to attach to, and creating one there would be a component
	// nobody asked for on an actor nobody is playing.
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	// Retried on a timer rather than done once: the player controller does not
	// exist at world begin-play in every net mode/travel path, and a single
	// attempt that lost that race would fail in exactly the "it silently did
	// nothing" way this whole file is trying to avoid.
	void TryAttach();

	FTimerHandle AttachTimerHandle;
	int32 AttachAttempts = 0;
};
