#include "VoxelInventoryComponent.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "TimerManager.h"
#include "VoxelEarth.h"
#include "VoxelEofDirtyLedger.h" // EndOfFrameUpdates attribution -- global reg= roll-up
#include "VoxelItem.h"

namespace
{
// Ten slots, all of them hotbar.
//
// v0 HAS NO BACKPACK: the hotbar is the inventory. A separate carried-but-not-
// selectable store is a real feature (it needs a UI, a transfer verb, and a rule
// for what a pickup does when the hotbar is full) and none of that is needed to
// throw a cube. When it arrives, the first N slots stay the hotbar and
// SelectNextSlot/SelectPrevSlot wrap within N instead of within Slots.Num() --
// that is the only line that changes, which is why the wrap is written against
// NumSlots() in one place.
//
// Ten because that is how many number keys there are, even though this project
// cannot use most of them: `1` is the water bucket (a specific request, Matt
// 2026-07-29, VoxelEarthPlayerController.cpp:41-47) and 2/3/4 are the dig sizes.
// docs/item-inventory-v0.md lists what is actually free.
constexpr int32 kDefaultSlotCount = 10;

// Sanity bound on the cvar. 64 is far past anything a hotbar-only inventory can
// display; the bound exists so a fat-fingered console value cannot allocate
// something absurd or make the dump unreadable.
constexpr int32 kMaxSlotCount = 64;

TAutoConsoleVariable<int32> CVarVoxelInventorySlots(
	TEXT("voxel.Inventory.Slots"),
	kDefaultSlotCount,
	TEXT("How many inventory slots the player's inventory has. Read ONCE, when the component first initialises -- ")
	TEXT("changing it mid-session does nothing until the next map load. Clamped to 1..64."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelInventorySeed(
	TEXT("voxel.Inventory.Seed"),
	true,
	TEXT("Seed slot 0 with a stack of throwable 30 cm voxel cubes at start. 0 starts empty, which is the arm of the ")
	TEXT("A/B that proves a throw with nothing in hand is refused rather than silently ignored."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelInventorySeedCount(
	TEXT("voxel.Inventory.SeedCount"),
	16,
	TEXT("How many throwable cubes the seeded slot 0 holds. Clamped to the item's own max stack (16), so asking for ")
	TEXT("more gets you 16 and says so in the log rather than quietly overfilling a stack."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelInventoryAutoAttach(
	TEXT("voxel.Inventory.AutoAttach"),
	false,
	TEXT("Attach an inventory component to the local player controller automatically at world begin-play. This was ")
	TEXT("SCAFFOLDING for the window in which the inventory existed but AVoxelEarthPlayerController did not create ")
	TEXT("one. It does now, in its constructor, so this DEFAULTS OFF as of 2026-08-13 and the scaffold is dead code ")
	TEXT("kept only until someone confirms nothing else relies on it. Turning it back on is harmless -- it finds the ")
	TEXT("controller's component and creates nothing -- but a retry loop that can never succeed at anything is worth ")
	TEXT("deleting rather than leaving armed."),
	ECVF_Default);

// 0.5 s apart, 20 tries = 10 s. Long enough to cover a slow level load and a
// seamless travel; short enough that the give-up warning lands while the log is
// still about startup.
constexpr float kAttachRetrySeconds = 0.5f;
constexpr int32 kMaxAttachAttempts = 20;
} // namespace

UVoxelInventoryComponent::UVoxelInventoryComponent()
{
	// No tick. An inventory changes only when something calls into it; a tick
	// here would be pure cost and an invitation to put polling in it.
	PrimaryComponentTick.bCanEverTick = false;

	// Sized again from the cvar in SeedDefaultsOnce (the cvar cannot be trusted
	// to be at its final value while the CDO is being constructed during module
	// load). This default means the array is never zero-length even if that path
	// somehow does not run.
	Slots.SetNum(kDefaultSlotCount);
}

void UVoxelInventoryComponent::BeginPlay()
{
	Super::BeginPlay();
	SeedDefaultsOnce();
}

void UVoxelInventoryComponent::SeedDefaultsOnce()
{
	if (bSeeded)
	{
		return;
	}
	bSeeded = true;

	// --- Slot count --------------------------------------------------------
	const int32 DesiredSlots = FMath::Clamp(CVarVoxelInventorySlots.GetValueOnAnyThread(), 1, kMaxSlotCount);
	if (DesiredSlots != Slots.Num())
	{
		// Shrinking would silently delete anything already held. Nothing can be
		// held this early in practice, but "in practice" is how items vanish, so
		// it is checked and reported.
		for (int32 Index = DesiredSlots; Index < Slots.Num(); ++Index)
		{
			if (!Slots[Index].IsEmpty())
			{
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("Inventory: resizing %d -> %d slots discards %d x '%s' from slot %d."), Slots.Num(),
				       DesiredSlots, Slots[Index].Count, *Slots[Index].ItemId.ToString(), Index);
			}
		}
		Slots.SetNum(DesiredSlots);
	}
	SelectedSlot = FMath::Clamp(SelectedSlot, 0, Slots.Num() - 1);

	// --- Seed --------------------------------------------------------------
	if (!CVarVoxelInventorySeed.GetValueOnAnyThread())
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("Inventory: %d slot(s), NOT seeded (voxel.Inventory.Seed 0)."), Slots.Num());
		return;
	}

	const FName SeedItem = VoxelItemIds::ThrowCube30();
	const int32 Requested = FMath::Max(0, CVarVoxelInventorySeedCount.GetValueOnAnyThread());
	SeededCount = TryAddItem(SeedItem, Requested);

	// The ran-flag for seeding. "Inventory: 10 slots, slot 0 = 16 x
	// throwable.voxelcube30" in the log is the whole answer to "does the player
	// start with anything"; without it, an empty hotbar and a hotbar that never
	// initialised look identical.
	if (SeededCount > 0)
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("Inventory: %d slot(s) on %s; seeded %d/%d x '%s' (voxel.Inventory.SeedCount). ")
		       TEXT("`voxel.Inventory.Dump` prints the rest."),
		       Slots.Num(), *GetNameSafe(GetOwner()), SeededCount, Requested, *SeedItem.ToString());
	}
	else
	{
		// Reachable two ways -- SeedCount 0, or the registry not knowing the item
		// -- and TryAddItem has already logged which.
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("Inventory: %d slot(s) on %s; seeding added NOTHING (asked for %d x '%s'). ")
		       TEXT("`voxel.Item.List` shows whether that item exists at all."),
		       Slots.Num(), *GetNameSafe(GetOwner()), Requested, *SeedItem.ToString());
	}
}

int32 UVoxelInventoryComponent::TryAddItem(FName ItemId, int32 Count)
{
	++AddCalls;

	if (ItemId.IsNone() || Count <= 0)
	{
		++AddRejectedBadArgs;
		UE_LOG(LogVoxelEarth, Verbose, TEXT("Inventory.TryAddItem('%s', %d): nothing to do (empty id or non-positive count)."),
		       *ItemId.ToString(), Count);
		return 0;
	}

	const FVoxelItemDef* Def = FVoxelItemRegistry::Find(ItemId);
	if (!Def)
	{
		// NOT an assert. An unknown id is what a save file from a newer build, a
		// renamed item or a mistyped console argument looks like, and refusing it
		// loudly is correct behaviour rather than a crash.
		++AddRejectedUnknown;
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("Inventory.TryAddItem: no item definition for '%s' -- added 0 of %d. `voxel.Item.List` shows every id."),
		       *ItemId.ToString(), Count);
		return 0;
	}

	const int32 MaxStack = FMath::Max(1, Def->MaxStack);
	int32 Remaining = Count;

	// PASS 1: top up partial stacks of the same item, in slot order.
	//
	// Before empty slots, deliberately. Picking up two rocks when you already
	// hold a part-stack of rocks should make that stack three, not open a second
	// rock slot -- a hotbar that fragments into several half-stacks of the same
	// thing is the single most common complaint about naive inventories, and it
	// costs one extra loop to avoid.
	for (int32 Index = 0; Index < Slots.Num() && Remaining > 0; ++Index)
	{
		FVoxelInventorySlot& Slot = Slots[Index];
		if (Slot.Count <= 0 || Slot.ItemId != ItemId)
		{
			continue;
		}
		const int32 Room = MaxStack - Slot.Count;
		if (Room <= 0)
		{
			continue;
		}
		const int32 Take = FMath::Min(Room, Remaining);
		Slot.Count += Take;
		Remaining -= Take;
	}

	// PASS 2: empty slots, in slot order, one full stack at a time.
	for (int32 Index = 0; Index < Slots.Num() && Remaining > 0; ++Index)
	{
		FVoxelInventorySlot& Slot = Slots[Index];
		if (!Slot.IsEmpty())
		{
			continue;
		}
		const int32 Take = FMath::Min(MaxStack, Remaining);
		Slot.ItemId = ItemId;
		Slot.Count = Take;
		Remaining -= Take;
	}

	const int32 Added = Count - Remaining;
	AddedTotal += Added;
	if (Remaining > 0)
	{
		// Counted, and counted in ITEMS not calls: "the inventory was full 3
		// times" is much less useful than "37 items were dropped on the floor".
		AddRejectedNoRoom += Remaining;
		UE_LOG(LogVoxelEarth, Verbose, TEXT("Inventory.TryAddItem('%s', %d): full -- added %d, %d did not fit."),
		       *ItemId.ToString(), Count, Added, Remaining);
	}
	return Added;
}

bool UVoxelInventoryComponent::TryRemoveFromSlot(int32 SlotIndex, int32 Count)
{
	++RemoveCalls;

	if (!Slots.IsValidIndex(SlotIndex) || Count <= 0)
	{
		++RemoveRejected;
		UE_LOG(LogVoxelEarth, Verbose, TEXT("Inventory.TryRemoveFromSlot(%d, %d): bad slot index or count (have %d slots)."),
		       SlotIndex, Count, Slots.Num());
		return false;
	}

	FVoxelInventorySlot& Slot = Slots[SlotIndex];
	if (Slot.IsEmpty() || Slot.Count < Count)
	{
		// All-or-nothing -- see the header. This is the branch a throw with an
		// empty hand takes, so it is the one that must be visible in the counters.
		++RemoveRejected;
		UE_LOG(LogVoxelEarth, Verbose, TEXT("Inventory.TryRemoveFromSlot(%d, %d): slot holds %d x '%s'; removed nothing."),
		       SlotIndex, Count, Slot.Count, *Slot.ItemId.ToString());
		return false;
	}

	Slot.Count -= Count;
	RemovedTotal += Count;
	if (Slot.Count <= 0)
	{
		// Clear the id as well, so "empty" has exactly one representation.
		Slot.Count = 0;
		Slot.ItemId = NAME_None;
	}
	return true;
}

FVoxelInventorySlot UVoxelInventoryComponent::GetSlot(int32 SlotIndex) const
{
	if (!Slots.IsValidIndex(SlotIndex))
	{
		return FVoxelInventorySlot();
	}
	return Slots[SlotIndex];
}

void UVoxelInventoryComponent::SetSelectedSlot(int32 SlotIndex)
{
	++SelectCalls;
	if (Slots.Num() <= 0)
	{
		return;
	}
	const int32 Clamped = FMath::Clamp(SlotIndex, 0, Slots.Num() - 1);
	if (Clamped != SlotIndex)
	{
		UE_LOG(LogVoxelEarth, Verbose, TEXT("Inventory.SetSelectedSlot(%d): out of range, clamped to %d of %d slots."),
		       SlotIndex, Clamped, Slots.Num());
	}
	SelectedSlot = Clamped;
}

void UVoxelInventoryComponent::SelectNextSlot()
{
	if (Slots.Num() > 0)
	{
		++SelectCalls;
		SelectedSlot = (SelectedSlot + 1) % Slots.Num();
	}
}

void UVoxelInventoryComponent::SelectPrevSlot()
{
	if (Slots.Num() > 0)
	{
		++SelectCalls;
		SelectedSlot = (SelectedSlot + Slots.Num() - 1) % Slots.Num();
	}
}

FName UVoxelInventoryComponent::GetSelectedItemId() const
{
	const FVoxelInventorySlot Slot = GetSlot(SelectedSlot);
	return Slot.IsEmpty() ? NAME_None : Slot.ItemId;
}

int32 UVoxelInventoryComponent::CountOf(FName ItemId) const
{
	if (ItemId.IsNone())
	{
		return 0;
	}
	int32 Total = 0;
	for (const FVoxelInventorySlot& Slot : Slots)
	{
		if (Slot.ItemId == ItemId)
		{
			Total += Slot.Count;
		}
	}
	return Total;
}

FString UVoxelInventoryComponent::DescribeForLog() const
{
	FString Text;

	Text += FString::Printf(TEXT("Inventory on %s: %d slot(s), selected %d, seeded=%s (%d at start)"),
	                        *GetNameSafe(GetOwner()), Slots.Num(), SelectedSlot, bSeeded ? TEXT("yes") : TEXT("no"),
	                        SeededCount);
	Text += LINE_TERMINATOR;

	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		const FVoxelInventorySlot& Slot = Slots[Index];
		const TCHAR* Marker = (Index == SelectedSlot) ? TEXT(">") : TEXT(" ");
		if (Slot.IsEmpty())
		{
			Text += FString::Printf(TEXT("  %s [%d] --"), Marker, Index);
		}
		else
		{
			// The definition is looked up rather than cached, so a slot holding
			// an id the registry does not know prints as such instead of printing
			// a blank name -- that is a real state (save from a newer build) and
			// it should be legible.
			const FVoxelItemDef* Def = FVoxelItemRegistry::Find(Slot.ItemId);
			Text += FString::Printf(TEXT("  %s [%d] %d x %s (%s)"), Marker, Index, Slot.Count, *Slot.ItemId.ToString(),
			                        Def ? *Def->DisplayName : TEXT("UNKNOWN ITEM -- not in the registry"));
		}
		Text += LINE_TERMINATOR;
	}

	// The counters. See the header: this block is how "my throw did nothing"
	// gets an answer without a debugger.
	Text += FString::Printf(TEXT("  adds: %lld call(s), %lld added, rejected(badargs=%lld unknown=%lld noroom=%lld)"),
	                        static_cast<long long>(AddCalls), static_cast<long long>(AddedTotal),
	                        static_cast<long long>(AddRejectedBadArgs), static_cast<long long>(AddRejectedUnknown),
	                        static_cast<long long>(AddRejectedNoRoom));
	Text += LINE_TERMINATOR;
	Text += FString::Printf(TEXT("  removes: %lld call(s), %lld removed, %lld refused; selects: %lld"),
	                        static_cast<long long>(RemoveCalls), static_cast<long long>(RemovedTotal),
	                        static_cast<long long>(RemoveRejected), static_cast<long long>(SelectCalls));

	return Text;
}

UVoxelInventoryComponent* UVoxelInventoryComponent::FindForLocalPlayer(const UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}

	// GetFirstPlayerController is the local one in every configuration this
	// project runs interactively (standalone, PIE, listen server). On a dedicated
	// server it is a remote-owned proxy or null, and the IsLocalController check
	// below is what keeps this from attaching an inventory to somebody else's
	// connection.
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return nullptr;
	}

	if (UVoxelInventoryComponent* OnController = PC->FindComponentByClass<UVoxelInventoryComponent>())
	{
		return OnController;
	}

	// The pawn is checked too. The controller is the right owner (see the header)
	// but if someone puts it on AVoxelEarthFlyPawn instead, every consumer that
	// goes through this function keeps working rather than silently finding
	// nothing.
	if (APawn* Pawn = PC->GetPawn())
	{
		if (UVoxelInventoryComponent* OnPawn = Pawn->FindComponentByClass<UVoxelInventoryComponent>())
		{
			return OnPawn;
		}
	}

	return nullptr;
}

UVoxelInventoryComponent* UVoxelInventoryComponent::EnsureForLocalPlayer(UWorld* World)
{
	if (UVoxelInventoryComponent* Existing = FindForLocalPlayer(World))
	{
		return Existing;
	}
	if (!World)
	{
		return nullptr;
	}

	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->IsLocalController())
	{
		return nullptr;
	}

	// No explicit object name: a fixed name would collide with a stale outer-
	// scoped object if this ever ran twice for the same controller, and the
	// failure mode of that is a fatal name clash rather than anything useful.
	UVoxelInventoryComponent* Component = NewObject<UVoxelInventoryComponent>(PC);
	if (!Component)
	{
		return nullptr;
	}
	Component->RegisterComponent();
	// No source column owns this: it is one non-terrain component registration,
	// counted into the GLOBAL reg= roll-up so a leg with reg= above the sum of
	// the source columns shows the gap instead of hiding it.
	VoxelEofLedger::CountRegister();

	// Seeding is called explicitly rather than relying on BeginPlay: a component
	// registered after its owner has already begun play is supposed to get one,
	// but this is a runtime-registration path that must not depend on that being
	// true in every net mode. SeedDefaultsOnce is idempotent for exactly this
	// reason -- if BeginPlay does fire, the second call returns immediately.
	Component->SeedDefaultsOnce();

	UE_LOG(LogVoxelEarth, Log, TEXT("Inventory: created and registered on %s (auto-attach scaffold)."), *GetNameSafe(PC));
	return Component;
}

// ---------------------------------------------------------------------------
// BOOTSTRAP SUBSYSTEM
// ---------------------------------------------------------------------------

bool UVoxelInventoryBootstrapSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UVoxelInventoryBootstrapSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (InWorld.GetNetMode() == NM_DedicatedServer)
	{
		// A dedicated server has no local player controller, so there is nothing
		// to attach to and no inventory to be missing. Said once, so a server log
		// with no inventory lines is explained rather than just quiet.
		UE_LOG(LogVoxelEarth, Log, TEXT("Inventory: dedicated server -- no local player, no inventory attached."));
		return;
	}

	if (!CVarVoxelInventoryAutoAttach.GetValueOnGameThread())
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("Inventory: auto-attach disabled (voxel.Inventory.AutoAttach 0). `voxel.Inventory.Dump` will report ")
		       TEXT("no inventory unless something else creates one."));
		return;
	}

	AttachAttempts = 0;
	InWorld.GetTimerManager().SetTimer(AttachTimerHandle, FTimerDelegate::CreateUObject(this, &UVoxelInventoryBootstrapSubsystem::TryAttach),
	                                   kAttachRetrySeconds, /*bLoop=*/true, /*FirstDelay=*/0.0f);
}

void UVoxelInventoryBootstrapSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AttachTimerHandle);
	}
	Super::Deinitialize();
}

void UVoxelInventoryBootstrapSubsystem::TryAttach()
{
	UWorld* World = GetWorld();
	++AttachAttempts;

	if (!World)
	{
		return;
	}

	if (UVoxelInventoryComponent* Component = UVoxelInventoryComponent::EnsureForLocalPlayer(World))
	{
		World->GetTimerManager().ClearTimer(AttachTimerHandle);
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("Inventory: ready on %s after %d attempt(s) -- %d slot(s), slot %d selected, holding %d item(s) there."),
		       *GetNameSafe(Component->GetOwner()), AttachAttempts, Component->NumSlots(), Component->GetSelectedSlot(),
		       Component->GetSlot(Component->GetSelectedSlot()).Count);
		return;
	}

	if (AttachAttempts >= kMaxAttachAttempts)
	{
		World->GetTimerManager().ClearTimer(AttachTimerHandle);
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("Inventory: gave up attaching after %d attempt(s) over %.0f s -- no local player controller in this ")
		       TEXT("world. Anything that asks for the inventory will get nothing, and it will say so."),
		       AttachAttempts, AttachAttempts * kAttachRetrySeconds);
	}
}

// ---------------------------------------------------------------------------
// CONSOLE
// ---------------------------------------------------------------------------
//
// The rule these exist for: every stage emits a ran-flag distinguishable from
// "found nothing". For the inventory, the question in the owner's words is "my
// throw did nothing" -- and there are five different reasons for that (listed
// on the counters in the header), none of which look any different on screen.
// Dump separates all five. Give and Select let the same session set up each of
// them deliberately, which is what makes the reading trustworthy: it is only
// worth believing the "inventory empty" line if you have also seen it say
// something else.

namespace
{
void LogInventoryLines(const FString& Text)
{
	// Split so each line gets its own log entry -- a single multi-line UE_LOG
	// prints one timestamped header and then unprefixed lines, which is
	// noticeably harder to grep out of a 200 MB headless log.
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
	for (const FString& Line : Lines)
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("%s"), *Line);
	}
}

// An item id argument that a human typed. "rock" and "block.rock" both work,
// because the prefix is a convention and making somebody type it to test a throw
// is friction for no benefit.
FName ResolveItemIdArg(const FString& Arg)
{
	const FName Direct(*Arg);
	if (FVoxelItemRegistry::Find(Direct))
	{
		return Direct;
	}
	static const TCHAR* Prefixes[] = {TEXT("block."), TEXT("tool."), TEXT("throwable."), TEXT("misc.")};
	for (const TCHAR* Prefix : Prefixes)
	{
		const FName Prefixed(*(FString(Prefix) + Arg));
		if (FVoxelItemRegistry::Find(Prefixed))
		{
			return Prefixed;
		}
	}
	return Direct; // Let TryAddItem report the unknown id, with the id as typed.
}

FAutoConsoleCommandWithWorld GVoxelInventoryDumpCmd(
	TEXT("voxel.Inventory.Dump"),
	TEXT("Print the local player's inventory: every slot, which one is selected, and the add/remove counters. ")
	TEXT("Read the counters when a throw or a pickup 'does nothing' -- all-zero means nothing ever called in (the ")
	TEXT("problem is upstream), while rejected(unknown/noroom) or refused removes name the reason. Reports the ")
	TEXT("absence of an inventory as a distinct outcome from an empty one."),
	FConsoleCommandWithWorldDelegate::CreateStatic(
		[](UWorld* World)
		{
			// Deliberately Find, not Ensure: this command answers "what is
			// there", and a command that created what it was asked to inspect
			// could never report the one state that matters most.
			const UVoxelInventoryComponent* Inventory = UVoxelInventoryComponent::FindForLocalPlayer(World);
			if (!Inventory)
			{
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("Inventory.Dump: NO INVENTORY COMPONENT on the local player (auto-attach is %s). ")
				       TEXT("This is not an empty inventory -- there is nothing to be empty. ")
				       TEXT("`voxel.Inventory.Give throwable.voxelcube30 1` creates one."),
				       CVarVoxelInventoryAutoAttach.GetValueOnGameThread() ? TEXT("on") : TEXT("OFF"));
				return;
			}
			LogInventoryLines(Inventory->DescribeForLog());
		}));

FAutoConsoleCommandWithWorldAndArgs GVoxelInventoryGiveCmd(
	TEXT("voxel.Inventory.Give"),
	TEXT("voxel.Inventory.Give <ItemId> [Count=1] -- put items in the local player's inventory, creating it if needed. ")
	TEXT("The category prefix is optional ('rock' finds 'block.rock'). Prints how many were actually taken, which is ")
	TEXT("less than asked when the inventory is full. `voxel.Item.List` shows every id."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogVoxelEarth, Warning, TEXT("Inventory.Give: usage is voxel.Inventory.Give <ItemId> [Count]"));
				return;
			}
			UVoxelInventoryComponent* Inventory = UVoxelInventoryComponent::EnsureForLocalPlayer(World);
			if (!Inventory)
			{
				UE_LOG(LogVoxelEarth, Warning, TEXT("Inventory.Give: no local player controller to hold an inventory."));
				return;
			}
			const FName ItemId = ResolveItemIdArg(Args[0]);
			const int32 Count = (Args.Num() > 1) ? FMath::Max(1, FCString::Atoi(*Args[1])) : 1;
			const int32 Added = Inventory->TryAddItem(ItemId, Count);
			UE_LOG(LogVoxelEarth, Log, TEXT("Inventory.Give: added %d/%d x '%s'; now holding %d."), Added, Count,
			       *ItemId.ToString(), Inventory->CountOf(ItemId));
		}));

FAutoConsoleCommandWithWorldAndArgs GVoxelInventorySelectCmd(
	TEXT("voxel.Inventory.Select"),
	TEXT("voxel.Inventory.Select <SlotIndex> -- set the selected hotbar slot (0-based, clamped). Exists because the ")
	TEXT("number keys are already spoken for on this project (1 = water bucket, 2/3/4 = dig sizes), so until a hotbar ")
	TEXT("binding is chosen this is the only way to change selection."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			UVoxelInventoryComponent* Inventory = UVoxelInventoryComponent::EnsureForLocalPlayer(World);
			if (!Inventory)
			{
				UE_LOG(LogVoxelEarth, Warning, TEXT("Inventory.Select: no local player controller to hold an inventory."));
				return;
			}
			if (Args.Num() < 1)
			{
				UE_LOG(LogVoxelEarth, Warning, TEXT("Inventory.Select: usage is voxel.Inventory.Select <SlotIndex>"));
				return;
			}
			Inventory->SetSelectedSlot(FCString::Atoi(*Args[0]));
			const FVoxelInventorySlot Slot = Inventory->GetSlot(Inventory->GetSelectedSlot());
			UE_LOG(LogVoxelEarth, Log, TEXT("Inventory.Select: slot %d of %d -- %d x '%s'."), Inventory->GetSelectedSlot(),
			       Inventory->NumSlots(), Slot.Count, *Slot.ItemId.ToString());
		}));

FAutoConsoleCommandWithWorld GVoxelInventoryClearCmd(
	TEXT("voxel.Inventory.Clear"),
	TEXT("Empty every slot. The other arm of the throw test: with nothing in hand the throw must refuse and SAY so ")
	TEXT("(RemoveRejected climbs in voxel.Inventory.Dump), not fail silently. Counters are NOT reset -- they are the ")
	TEXT("record of what happened this session."),
	FConsoleCommandWithWorldDelegate::CreateStatic(
		[](UWorld* World)
		{
			UVoxelInventoryComponent* Inventory = UVoxelInventoryComponent::FindForLocalPlayer(World);
			if (!Inventory)
			{
				UE_LOG(LogVoxelEarth, Warning, TEXT("Inventory.Clear: no inventory on the local player."));
				return;
			}
			int32 Cleared = 0;
			for (int32 Index = 0; Index < Inventory->NumSlots(); ++Index)
			{
				const FVoxelInventorySlot Slot = Inventory->GetSlot(Index);
				if (!Slot.IsEmpty() && Inventory->TryRemoveFromSlot(Index, Slot.Count))
				{
					Cleared += Slot.Count;
				}
			}
			UE_LOG(LogVoxelEarth, Log, TEXT("Inventory.Clear: removed %d item(s) across %d slot(s)."), Cleared,
			       Inventory->NumSlots());
		}));
} // namespace
