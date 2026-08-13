#pragma once

#include "CoreMinimal.h"
#include "VoxelItem.generated.h"

// ---------------------------------------------------------------------------
// ITEM DEFINITIONS -- what a thing IS, as opposed to how many of it you have.
// ---------------------------------------------------------------------------
//
// The split this file exists to make: a DEFINITION says "a 30 cm voxel cube is
// 0.3 m on a side, weighs 5 kg, stacks to 16 and can be thrown at 1200 UU/s".
// An INVENTORY SLOT (VoxelInventoryComponent.h) says "you are holding nine of
// them". Definitions are shared, immutable and exist exactly once per item
// kind; slots are per-player and change every time you pick something up. Every
// item system that ends up needing a rewrite got that boundary wrong first --
// usually by copying the physical facts into the slot, after which two players
// holding the same item can disagree about how heavy it is.
//
// v0 SCOPE. Fourteen block items derived from the voxel-core material palette,
// one throwable, no tools. The registry is a hard-coded table built once in
// VoxelItem.cpp. That is deliberate and it is called out again above the table,
// including what would force it to become a data asset.
//
// THIS HEADER IS UHT-PARSED, SO IT STAYS voxel-core-FREE, by the same doctrine
// that keeps VoxelEarthPlayerController.h:30-32 and VoxelEarthHUD.cpp:18-21
// free of it. That is why FVoxelItemDef::BlockMaterialId is a bare uint8 rather
// than a vxc::MaterialId. The .cpp DOES include voxelcore/core.h and writes the
// table in terms of vxc::MAT_ROCK, vxc::MAT_SAND and so on -- see the long
// comment on identity below, because that is the whole point.

// What role an item plays. The four cases the game actually distinguishes when
// you press a button while holding one:
//
//   Block     -- goes into the world as voxels (RMB place). Carries a vxc
//                MaterialId; that material is its identity (see below).
//   Tool      -- used, not consumed. Nothing is a Tool in v0; the enumerator
//                exists so the first one is a table row and not a refactor.
//   Throwable -- leaves your hand as an actor and does something where it
//                lands. The 30 cm cube, and (once F is inventory-driven)
//                AVoxelExplosive.
//   Misc      -- everything else: materials you carry, quest objects, junk.
//
// NOT A BITFIELD, deliberately. A block you can also throw is a Block with
// bCanThrow set, not a Block|Throwable. Category answers "what is this for",
// which is a single answer and drives the HUD label and the sort order;
// capabilities answer "what can I do with it", which is several answers and
// each gets its own field. Making the category a mask is the fork in the road
// where item systems become unreadable, because every switch then has to decide
// which bit wins.
UENUM()
enum class EVoxelItemCategory : uint8
{
	Block UMETA(DisplayName = "Block"),
	Tool UMETA(DisplayName = "Tool"),
	Throwable UMETA(DisplayName = "Throwable"),
	Misc UMETA(DisplayName = "Misc"),
};

// One item kind. Value type, copied freely, never edited after registry build.
//
// A USTRUCT rather than a plain struct even though v0 never puts one in a
// UPROPERTY: it is what lets this become an FTableRowBase-derived data-table row
// later without touching a single consumer's code. That cost nothing today.
USTRUCT()
struct VOXELEARTH_API FVoxelItemDef
{
	GENERATED_BODY()

	// Stable identity. Dotted lowercase, category first: "block.rock",
	// "throwable.voxelcube30". The prefix is a naming convention only -- nothing
	// parses it -- but it makes a console dump readable at a glance and it makes
	// `voxel.Inventory.Give block.` autocomplete-shaped for a human.
	//
	// FName, not an enum or an int: item ids must survive being written into a
	// save file and read back by a build that has more items than the one that
	// wrote it. An enum renumbers the moment somebody inserts a value in the
	// middle, which is the same hazard voxel-core's material enum guards against
	// with its "append-only, never renumber" rule (voxelcore/core.h:322-323).
	// FName costs a hash lookup and is immune.
	UPROPERTY()
	FName ItemId;

	// What the player is shown. FString rather than FText because every other
	// string this project puts on the HUD is an FString and there is no
	// localisation pipeline to feed; FText here would be a lone ceremony. If
	// localisation ever arrives this becomes FText and the HUD keeps compiling
	// because it goes through *DisplayName either way.
	UPROPERTY()
	FString DisplayName;

	UPROPERTY()
	EVoxelItemCategory Category = EVoxelItemCategory::Misc;

	// Slot capacity. 1 means "never stacks" (tools).
	UPROPERTY()
	int32 MaxStack = 1;

	// vxc::MaterialId of the voxel this item places, or 0 (vxc::MAT_AIR) for
	// "this item is not made of a world material".
	//
	// A NON-ZERO VALUE HERE DOES NOT MAKE SOMETHING A BLOCK. The throwable cube
	// also carries MAT_ROCK, because a thrown cube has to be drawn in some
	// material and rock is what it looks like. Only Category == Block means
	// "placing this puts that material in the world", and only Block items
	// appear in the material -> item reverse lookup
	// (FVoxelItemRegistry::FindBlockForMaterial). Blurring those two would make
	// the reverse lookup ambiguous the first time two items share a material,
	// which is already true in v0 -- so it is settled here rather than later.
	UPROPERTY()
	uint8 BlockMaterialId = 0;

	// Can this leave your hand? A separate flag from Category (see the enum
	// comment). Blocks are all false in v0 -- not because throwing a block is a
	// bad idea, but because a thrown block that fails to become a voxel where it
	// lands is a feature that looks broken. Flipping these to true is one column
	// of the table once VoxelThrownItem knows how to deposit on impact.
	UPROPERTY()
	bool bCanThrow = false;

	// --- Physical facts a thrown instance needs -----------------------------
	//
	// Populated for Blocks and Throwables; zero for anything with no physical
	// body yet. AVoxelThrownItem reads these instead of hard-coding a size the
	// way AVoxelExplosive currently hard-codes its blast (VoxelExplosive.h:64-67)
	// -- those constants are fine where they are because there is exactly one
	// explosive, and stop being fine the moment there are two throwables with
	// different sizes, which is now.

	// Edge length of the item's cube, in METRES. 0.3 = a 3x3x3 block of 10 cm
	// voxels. Metres because that is the unit the owner and the design docs
	// speak ("a 30 cm cube"), and because a number in metres cannot be silently
	// confused with the UU (centimetre) values the rest of the runtime passes
	// around. CubeEdgeUU() below is the one conversion, so no caller writes
	// `* 100` itself.
	UPROPERTY()
	float CubeEdgeMetres = 0.0f;

	// Kilograms. Not used by v0's flight (see ThrowSpeedUUPerSec) -- it is here
	// because a projectile that ignores mass is a placeholder, and the value
	// should already be in the row when the physics starts reading it.
	UPROPERTY()
	float MassKg = 0.0f;

	// Default launch speed in UU/s (UE units per second = cm/s), matching the
	// units AVoxelEarthPlayerController::OnChargeRelease already throws in
	// (VoxelEarthPlayerController.cpp:664-668). This is the UNCHARGED speed; the
	// charge mechanic scales it. See the throwable's table row for how the
	// existing 600..1600 UU/s charge range is reproduced from a single 1200.
	//
	// Speed in UU/s while size is in metres is a genuine unit split and it is
	// intentional: sizes are talked about in metres by people, speeds are
	// consumed in UU/s by UProjectileMovementComponent, and converting at the
	// boundary is cheaper than either a metre-per-second value that every call
	// site must remember to scale or a 30.0f "size" nobody can read as 30 cm.
	UPROPERTY()
	float ThrowSpeedUUPerSec = 0.0f;

	bool IsValid() const { return !ItemId.IsNone(); }
	bool IsBlock() const { return Category == EVoxelItemCategory::Block; }
	bool CanThrow() const { return bCanThrow; }
	bool IsStackable() const { return MaxStack > 1; }

	// Metres -> UE units. 1 UU = 1 cm (VoxelCoords.h:10), so this is a plain
	// unit conversion and needs nothing from the voxel grid.
	float CubeEdgeUU() const { return CubeEdgeMetres * 100.0f; }
};

// Lookup. All static, no instance, no UObject: the table is immutable process
// state, built once on first access.
//
// THREAD SAFETY: the table is built inside a function-local static, so the build
// itself is thread-safe (C++11 magic statics) and everything after it is
// read-only. Nothing here needs the game thread, which matters because a thrown
// item's flight code may want a definition off the game thread later.
struct VOXELEARTH_API FVoxelItemRegistry
{
	// nullptr if no such item. Callers MUST handle null -- an unknown id is the
	// normal outcome of a save file from a newer build, not a programmer error,
	// and UVoxelInventoryComponent::TryAddItem counts it rather than asserting.
	static const FVoxelItemDef* Find(FName ItemId);

	// Reverse lookup: which BLOCK item places this vxc::MaterialId. nullptr for
	// air, for materials with no block item (the water marker, the asset and
	// creature materials -- see the table), and for any material only referenced
	// by a non-block item.
	static const FVoxelItemDef* FindBlockForMaterial(uint8 MaterialId);

	// The whole table, registry order (blocks by ascending material id, then
	// everything else). Stable for the life of the process.
	static const TArray<FVoxelItemDef>& AllItems();

	// The display name for a placement-palette material, or "?" for one with no
	// block item.
	//
	// THIS EXISTS TO DELETE A DUPLICATE. VoxelEarthHUD.cpp:22-31 currently
	// carries its own switch over material ids returning "Rock"/"Soil"/"Sand",
	// and this registry would otherwise become a second table of the same facts
	// -- two lists that agree today and disagree the first time somebody adds a
	// material to one of them. The names in the table below were copied from
	// that switch verbatim (including "Soil" for MAT_TOPSOIL) so the swap is
	// invisible on screen. See docs/item-inventory-v0.md for the exact three-line
	// edit; it is not applied here because this agent does not own that file.
	static FString DisplayNameForMaterial(uint8 MaterialId);

	// Logs the whole table (also reachable as `voxel.Item.List`). The count line
	// is the ran-flag: "0 items" and "no such command" are different failures and
	// this is how you tell them apart without a debugger.
	static void LogAll();
};

// Well-known ids, as FUNCTIONS rather than constants.
//
// A namespace-scope `const FName X(TEXT("..."))` is constructed during static
// initialisation, which on some link orders runs BEFORE UE's name table exists.
// The failure is a crash or a corrupted name at startup, dependent on link
// order, i.e. it works until it doesn't. A function-local static inside an
// out-of-line function is constructed on first call, which is always after
// engine init. The cost is a function call and a guard variable.
namespace VoxelItemIds
{
	// The 30 cm throwable cube -- the thing slot 0 is seeded with.
	VOXELEARTH_API FName ThrowCube30();

	// The block item for a vxc::MaterialId, or NAME_None if that material has no
	// block item. Equivalent to FindBlockForMaterial()->ItemId with the null
	// check done for you.
	VOXELEARTH_API FName Block(uint8 MaterialId);
}
