#include "VoxelItem.h"

#include "HAL/IConsoleManager.h"
#include "VoxelEarth.h"

// The .cpp side of the voxel-core doctrine: the HEADER stays voxel-core-free
// because UHT parses it, but this translation unit is free to include the real
// enum -- nine other files in this module already do (six .cpp plus three
// non-UHT headers). That is not a detail, it is the mechanism that makes block
// items derive from the material palette instead of restating it. See "THE
// IDENTITY QUESTION" below.
#include "voxelcore/core.h"

namespace
{
// ---------------------------------------------------------------------------
// THE IDENTITY QUESTION: FName item id vs uint8 vxc::MaterialId
// ---------------------------------------------------------------------------
//
// A placeable block already has an identity -- vxc::MaterialId, a uint8, the
// number that goes in the edit log, replicates to every client and sits in save
// files (voxelcore/core.h:318-324, "append-only -- never renumber an existing
// entry, it would invalidate every saved edit log"). An item has an FName. Two
// identity spaces for the same rock is exactly how these systems rot, so:
//
//   THE MATERIAL ID IS THE IDENTITY OF A BLOCK. THE ITEM ID IS THE IDENTITY OF
//   A THING YOU CAN HOLD. BLOCK ITEMS ARE DERIVED FROM THE MATERIAL PALETTE, IN
//   ONE TABLE, IN THIS FILE, AND NOWHERE ELSE.
//
// Concretely: kBlockTable below has one row per placeable material. Each row
// carries the vxc::MAT_* symbol (not a copied number -- the symbol, so a
// renumber that voxel-core would never do could not silently mis-key this
// either), the item id and the display name. Both identities are born on the
// same line and cannot drift, because there is no second line to drift from.
// Both directions of the mapping are then built from that one table:
// Find("block.rock") and FindBlockForMaterial(vxc::MAT_ROCK) are two indexes
// over the same rows.
//
// WHY NOT JUST KEY THE INVENTORY BY MATERIAL ID and skip FName entirely? Because
// the inventory has to hold things that are not blocks. A shovel has no
// material; the 30 cm throwable has a material only for its appearance. Keying
// slots by uint8 would force every non-block item into a fake material id, and
// then the edit-log's material space -- the one that is append-only forever
// because save files depend on it -- would be carrying entries that were never
// terrain. That is a far worse coupling than a hash lookup.
//
// WHY NOT KEY BLOCKS BY FName ALONE and let the material be a lookup detail?
// Because the placement path is uint8 end to end: AVoxelEarthPlayerController's
// PaletteMaterialId (VoxelEarthPlayerController.h:138), the
// ServerSubmitPlaceIntent RPC's MaterialId parameter (line 55), and
// UVoxelWorldSubsystem::TryPlace. Making the item id authoritative would put an
// FName -> uint8 lookup inside the replicated edit path, and an item id that
// failed to resolve there would be a placement that silently did nothing.
//
// So: uint8 downward (into the world, the edit log, the wire), FName sideways
// (across the inventory, the hotbar, the UI, save files that hold items rather
// than terrain), and exactly one table where the two meet.
//
// WHAT WOULD CHANGE THIS: a block whose identity is finer than its material --
// a "mossy rock" that places MAT_ROCK but is a different item, or a block that
// places two materials. Then FindBlockForMaterial stops being a function
// (several items, one material) and must return the DEFAULT block item for a
// material, with the palette naming which one. The table gains a column; no
// caller of Find() changes. That is the reason the reverse lookup is a named
// function rather than a public TMap.

struct FBlockRow
{
	vxc::MaterialId MaterialId;
	const TCHAR* ItemId;
	const TCHAR* DisplayName;
};

// ---------------------------------------------------------------------------
// THE BLOCK TABLE
// ---------------------------------------------------------------------------
//
// Every material the terrain amplifier can emit, minus air. Fourteen rows.
//
// The first three are the ones the creative palette cycles today
// (AVoxelEarthPlayerController::CyclePaletteMaterial, rock -> soil -> sand,
// VoxelEarthPlayerController.cpp:620-638) and their DISPLAY NAMES ARE COPIED
// VERBATIM from VoxelEarthHUD.cpp:22-31, including "Soil" for MAT_TOPSOIL. If
// you change one of those three strings you change what the HUD prints, which
// is the point -- there should be one name per material in this project, and
// after the HUD edit in docs/item-inventory-v0.md this is where it lives.
//
// The other eleven are not reachable in game yet (the palette cycles three).
// They are here anyway because they cost one line each and because the next
// question after "can I place rock" is "can I place grass", and the answer
// should be a palette change, not a new table.
//
// DELIBERATELY ABSENT:
//   MAT_AIR (0)          -- the absence of a block; a "hold some air" item is
//                           a category error and would make BlockMaterialId 0
//                           mean two things.
//   MAT_WATERMARK (15)   -- a debug instrument, not world content
//                           (voxelcore/core.h:343-354). Placeable by a human
//                           only by mistake.
//   MAT_BARK..MAT_LEAF_AUTUMN (16-25) -- asset materials. A "bark block" is a
//                           reasonable future item but it is a design decision
//                           about building materials, not a mechanical
//                           consequence of the material existing, and shipping
//                           it silently would pre-empt that decision.
//   MAT_SKIN_* (26-35)   -- creature skin colours. Never placeable.
constexpr FBlockRow kBlockTable[] = {
    // --- in the creative palette today ---
    {vxc::MAT_ROCK, TEXT("block.rock"), TEXT("Rock")},
    {vxc::MAT_SAND, TEXT("block.sand"), TEXT("Sand")},
    {vxc::MAT_TOPSOIL, TEXT("block.soil"), TEXT("Soil")},
    // --- the rest of the amplifier's stratigraphy and biome surfaces ---
    {vxc::MAT_BEDROCK, TEXT("block.bedrock"), TEXT("Bedrock")},
    {vxc::MAT_GRAVEL, TEXT("block.gravel"), TEXT("Gravel")},
    {vxc::MAT_SUBSOIL, TEXT("block.subsoil"), TEXT("Subsoil")},
    {vxc::MAT_SNOW, TEXT("block.snow"), TEXT("Snow")},
    {vxc::MAT_GRASS, TEXT("block.grass"), TEXT("Grass")},
    {vxc::MAT_JUNGLE_SOIL, TEXT("block.junglesoil"), TEXT("Jungle soil")},
    {vxc::MAT_SAVANNA_GRASS, TEXT("block.savannagrass"), TEXT("Savanna grass")},
    {vxc::MAT_PODZOL, TEXT("block.podzol"), TEXT("Podzol")},
    {vxc::MAT_PERMAFROST, TEXT("block.permafrost"), TEXT("Permafrost")},
    {vxc::MAT_MUD, TEXT("block.mud"), TEXT("Mud")},
    {vxc::MAT_CLAY, TEXT("block.clay"), TEXT("Clay")},
};

// One voxel is 10 cm (VoxelCoords.h:10, vxc::kVoxelSizeMm). A block item is one
// voxel, so 0.1 m on a side.
constexpr float kBlockEdgeMetres = 0.1f;

// 0.001 m^3 of rock at ~2000 kg/m^3. ONE MASS FOR ALL BLOCKS in v0 rather than
// a per-material density column: nothing reads MassKg yet, and a column of
// invented densities would look like measured data. Per-material density is one
// more column in kBlockTable the day something weighs a backpack.
constexpr float kBlockMassKg = 2.0f;

// 64 is the idiomatic voxel-game block stack and there is no reason here to be
// original. 64 blocks at 10 cm builds a 4 m wall one voxel thick -- enough that
// stack size is not what stops you building, which is the only property of the
// number that matters at this stage.
constexpr int32 kBlockMaxStack = 64;

FVoxelItemDef MakeBlock(const FBlockRow& Row)
{
	FVoxelItemDef Def;
	Def.ItemId = FName(Row.ItemId);
	Def.DisplayName = FString(Row.DisplayName);
	Def.Category = EVoxelItemCategory::Block;
	Def.MaxStack = kBlockMaxStack;
	Def.BlockMaterialId = static_cast<uint8>(Row.MaterialId);
	// See FVoxelItemDef::bCanThrow: false until a thrown block can deposit a
	// voxel where it lands.
	Def.bCanThrow = false;
	Def.CubeEdgeMetres = kBlockEdgeMetres;
	Def.MassKg = kBlockMassKg;
	Def.ThrowSpeedUUPerSec = 0.0f;
	return Def;
}

// ---------------------------------------------------------------------------
// THE THROWABLE -- the item the owner wants to test today
// ---------------------------------------------------------------------------
//
// A 30 cm cube: 3x3x3 of the game's 10 cm voxels, big enough to read clearly
// mid-flight at arm's length and small enough not to fill the screen.
//
// MASS IS A GAMEPLAY NUMBER, NOT A DENSITY CALCULATION, and this is the kind of
// thing that gets "fixed" by someone doing the arithmetic later, so: 0.3 m cubed
// is 0.027 m^3, which at rock's ~2000 kg/m^3 would be 54 kg. Nobody throws 54 kg
// twelve metres. 5 kg is what a thrown object that size should feel like. If the
// projectile physics ever wants true density, that is a different field
// (a DensityKgPerM3 on the block rows) and not a reinterpretation of this one.
//
// THROW SPEED 1200 UU/s IS CHOSEN TO REPRODUCE THE EXISTING CHARGE RANGE
// EXACTLY. AVoxelEarthPlayerController charges 600 -> 1600 UU/s over a 0.3 s ->
// 1.5 s hold (VoxelEarthPlayerController.h:145-148). 1200 * 0.5 = 600 and
// 1200 * 1.3333 = 1600, so a charge mechanic that lerps 0.5x -> 1.3333x of the
// item's own default speed leaves the explosive throwing precisely as it does
// today while making the range a property of the ITEM. Getting this number
// arbitrary would have meant either changing the feel of the existing throw or
// carrying two unrelated speed scales; docs/item-inventory-v0.md records the
// formula for whoever wires the charge.
constexpr float kThrowCubeEdgeMetres = 0.3f;
constexpr float kThrowCubeMassKg = 5.0f;
constexpr float kThrowCubeSpeedUUPerSec = 1200.0f;

// 16 not 64: a stack of throwables is ammunition, and the number should be
// something you can plausibly run out of during a test session. This is the
// count `voxel.Inventory.SeedCount` defaults to as well.
constexpr int32 kThrowCubeMaxStack = 16;

FVoxelItemDef MakeThrowCube30()
{
	FVoxelItemDef Def;
	Def.ItemId = FName(TEXT("throwable.voxelcube30"));
	Def.DisplayName = FString(TEXT("Voxel cube (30 cm)"));
	Def.Category = EVoxelItemCategory::Throwable;
	Def.MaxStack = kThrowCubeMaxStack;
	// Rock, for APPEARANCE only -- it is what the cube is drawn as. It does NOT
	// make this a block item and it does NOT put this item in the material ->
	// item reverse lookup; block.rock owns MAT_ROCK there. See
	// FVoxelItemDef::BlockMaterialId.
	Def.BlockMaterialId = static_cast<uint8>(vxc::MAT_ROCK);
	Def.bCanThrow = true;
	Def.CubeEdgeMetres = kThrowCubeEdgeMetres;
	Def.MassKg = kThrowCubeMassKg;
	Def.ThrowSpeedUUPerSec = kThrowCubeSpeedUUPerSec;
	return Def;
}

// ---------------------------------------------------------------------------
// NOT IN THE TABLE: the explosive, and no tools
// ---------------------------------------------------------------------------
//
// AVoxelExplosive is the game's existing throwable and it is NOT an item here.
// It is thrown by holding F, which spawns the actor directly
// (VoxelEarthPlayerController.cpp:683-691) and consumes nothing. Giving it a row
// now would put a line in every inventory dump for a thing the inventory does
// not actually gate -- and "the dump says I have three but F throws forever" is
// precisely the ambiguity this project's ran-flag rule exists to prevent. The
// row to add on the day F reads the inventory is written out in
// docs/item-inventory-v0.md, using the constants already in VoxelExplosive.h.
//
// Likewise no Tool items. Digging today is a controller verb with a size dial
// (DigSizeVoxels, VoxelEarthPlayerController.h:134), not a held object; a
// "Shovel" that appeared in the hotbar and changed nothing when selected would
// be a lie told by the HUD. EVoxelItemCategory::Tool exists so that the first
// real tool is a table row plus the field it needs (a dig size, or a reach),
// which is the whole test of whether this shape scales.

// ---------------------------------------------------------------------------
// REGISTRY STORAGE
// ---------------------------------------------------------------------------
//
// HARD-CODED C++ TABLE, and that is a v0 decision with a stated expiry.
//
// It is right for now because the table is fifteen rows, every row is derived
// from a C++ symbol (vxc::MAT_*) that a data asset could not reference safely,
// and a compiled table cannot be half-loaded, cannot be missing on a headless
// run, and shows up in a diff.
//
// WHAT WOULD FORCE IT TO BECOME A UDataTable / UDataAsset, any one of these:
//   1. A non-programmer needs to add items. That is the usual reason and it is
//      sufficient on its own.
//   2. Items need asset references -- a mesh, an icon, a sound. Those are
//      TObjectPtr/TSoftObjectPtr fields and hard-coding an asset path in C++ is
//      how you get a silent null at runtime instead of a cook-time error.
//   3. Items need to differ per game mode or be added by a mod/plugin.
// When it moves, FVoxelItemDef becomes an FTableRowBase row, the registry gains
// a load step and a "not loaded yet" state, and Find() keeps its exact
// signature -- which is why every consumer goes through Find() and nothing gets
// a reference to the table itself.
struct FRegistryData
{
	TArray<FVoxelItemDef> Items;
	TMap<FName, int32> IndexById;
	TMap<uint8, int32> BlockIndexByMaterial;
	int32 ErrorCount = 0;

	FRegistryData()
	{
		Items.Reserve(static_cast<int32>(UE_ARRAY_COUNT(kBlockTable)) + 1);
		for (const FBlockRow& Row : kBlockTable)
		{
			Items.Add(MakeBlock(Row));
		}
		Items.Add(MakeThrowCube30());

		// VALIDATION. Every check here is a bug that would otherwise present as
		// "the item does nothing", which is the worst failure shape this project
		// has and the reason for the ran-flag rule. A duplicate id, for instance,
		// means one of the two items is simply unreachable through Find() -- the
		// inventory would accept it, the dump would print it, and nothing would
		// ever resolve it back to a definition.
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			const FVoxelItemDef& Def = Items[Index];

			if (Def.ItemId.IsNone())
			{
				UE_LOG(LogVoxelEarth, Error, TEXT("VoxelItemRegistry: row %d has no ItemId; unreachable."), Index);
				++ErrorCount;
				continue;
			}

			if (const int32* Existing = IndexById.Find(Def.ItemId))
			{
				UE_LOG(LogVoxelEarth, Error,
				       TEXT("VoxelItemRegistry: duplicate ItemId '%s' (rows %d and %d). The second is unreachable."),
				       *Def.ItemId.ToString(), *Existing, Index);
				++ErrorCount;
				continue;
			}
			IndexById.Add(Def.ItemId, Index);

			if (Def.MaxStack < 1)
			{
				UE_LOG(LogVoxelEarth, Error, TEXT("VoxelItemRegistry: '%s' has MaxStack %d; nothing could ever be held."),
				       *Def.ItemId.ToString(), Def.MaxStack);
				++ErrorCount;
			}

			// Only BLOCK items claim a material. See FVoxelItemDef::BlockMaterialId
			// -- the throwable cube also carries MAT_ROCK and must not land here.
			if (Def.IsBlock())
			{
				if (Def.BlockMaterialId == 0)
				{
					UE_LOG(LogVoxelEarth, Error, TEXT("VoxelItemRegistry: block '%s' has no material; it would place air."),
					       *Def.ItemId.ToString());
					++ErrorCount;
				}
				else if (const int32* ExistingBlock = BlockIndexByMaterial.Find(Def.BlockMaterialId))
				{
					UE_LOG(LogVoxelEarth, Error,
					       TEXT("VoxelItemRegistry: material %u claimed by two block items ('%s' and '%s'). ")
					       TEXT("The reverse lookup keeps the first; see FindBlockForMaterial."),
					       static_cast<uint32>(Def.BlockMaterialId), *Items[*ExistingBlock].ItemId.ToString(),
					       *Def.ItemId.ToString());
					++ErrorCount;
				}
				else
				{
					BlockIndexByMaterial.Add(Def.BlockMaterialId, Index);
				}
			}

			if (Def.CanThrow() && (Def.CubeEdgeMetres <= 0.0f || Def.MassKg <= 0.0f || Def.ThrowSpeedUUPerSec <= 0.0f))
			{
				// A throwable with a zero here does not fail to compile and does
				// not crash: it spawns a zero-sized projectile at zero speed,
				// i.e. "my throw did nothing" with no other symptom.
				UE_LOG(LogVoxelEarth, Error,
				       TEXT("VoxelItemRegistry: throwable '%s' has edge=%.2f m mass=%.2f kg speed=%.0f UU/s; ")
				       TEXT("a zero in any of those throws an invisible, motionless object."),
				       *Def.ItemId.ToString(), Def.CubeEdgeMetres, Def.MassKg, Def.ThrowSpeedUUPerSec);
				++ErrorCount;
			}
		}

		int32 NumBlocks = 0, NumTools = 0, NumThrowables = 0, NumMisc = 0;
		for (const FVoxelItemDef& Def : Items)
		{
			switch (Def.Category)
			{
			case EVoxelItemCategory::Block: ++NumBlocks; break;
			case EVoxelItemCategory::Tool: ++NumTools; break;
			case EVoxelItemCategory::Throwable: ++NumThrowables; break;
			default: ++NumMisc; break;
			}
		}

		// The ran-flag for the whole file: this line proves the table was built
		// at all, and its counts are the difference between "the registry is
		// empty" and "the registry never ran".
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("VoxelItemRegistry: built %d item(s) -- %d block, %d tool, %d throwable, %d misc; %d error(s). ")
		       TEXT("`voxel.Item.List` prints them."),
		       Items.Num(), NumBlocks, NumTools, NumThrowables, NumMisc, ErrorCount);
	}
};

const FRegistryData& Registry()
{
	// Built on first use, thread-safe by C++11 magic statics, read-only after.
	// NOT a global object: a global would construct during static init, where
	// FName and UE_LOG are both unsafe.
	static const FRegistryData Data;
	return Data;
}

const TCHAR* CategoryName(EVoxelItemCategory Category)
{
	switch (Category)
	{
	case EVoxelItemCategory::Block: return TEXT("block");
	case EVoxelItemCategory::Tool: return TEXT("tool");
	case EVoxelItemCategory::Throwable: return TEXT("throwable");
	default: return TEXT("misc");
	}
}
} // namespace

const FVoxelItemDef* FVoxelItemRegistry::Find(FName ItemId)
{
	const FRegistryData& Data = Registry();
	const int32* Index = Data.IndexById.Find(ItemId);
	return Index ? &Data.Items[*Index] : nullptr;
}

const FVoxelItemDef* FVoxelItemRegistry::FindBlockForMaterial(uint8 MaterialId)
{
	// Material 0 is vxc::MAT_AIR and is also FVoxelItemDef's "not a block"
	// sentinel; both answers are "no item", so the early-out is honest.
	if (MaterialId == 0)
	{
		return nullptr;
	}
	const FRegistryData& Data = Registry();
	const int32* Index = Data.BlockIndexByMaterial.Find(MaterialId);
	return Index ? &Data.Items[*Index] : nullptr;
}

const TArray<FVoxelItemDef>& FVoxelItemRegistry::AllItems()
{
	return Registry().Items;
}

FString FVoxelItemRegistry::DisplayNameForMaterial(uint8 MaterialId)
{
	const FVoxelItemDef* Def = FindBlockForMaterial(MaterialId);
	// "?" rather than the number, matching VoxelEarthHUD.cpp:29's existing
	// default exactly -- this function is meant to be a drop-in for that switch,
	// and a HUD that suddenly printed "Place: 15" would be a visible change to
	// something nobody asked to change.
	return Def ? Def->DisplayName : FString(TEXT("?"));
}

void FVoxelItemRegistry::LogAll()
{
	const FRegistryData& Data = Registry();
	UE_LOG(LogVoxelEarth, Log, TEXT("VoxelItemRegistry: %d item(s), %d error(s) at build."), Data.Items.Num(),
	       Data.ErrorCount);
	for (const FVoxelItemDef& Def : Data.Items)
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("  %-24s %-10s \"%s\" stack=%d mat=%u throw=%s edge=%.2fm (%.0f UU) mass=%.1fkg speed=%.0f UU/s"),
		       *Def.ItemId.ToString(), CategoryName(Def.Category), *Def.DisplayName, Def.MaxStack,
		       static_cast<uint32>(Def.BlockMaterialId), Def.bCanThrow ? TEXT("yes") : TEXT("no"), Def.CubeEdgeMetres,
		       Def.CubeEdgeUU(), Def.MassKg, Def.ThrowSpeedUUPerSec);
	}
}

namespace VoxelItemIds
{
FName ThrowCube30()
{
	// Function-local static: constructed on first call, never during static
	// init. See the namespace comment in VoxelItem.h for why that matters.
	static const FName Id(TEXT("throwable.voxelcube30"));
	return Id;
}

FName Block(uint8 MaterialId)
{
	const FVoxelItemDef* Def = FVoxelItemRegistry::FindBlockForMaterial(MaterialId);
	return Def ? Def->ItemId : NAME_None;
}
} // namespace VoxelItemIds

namespace
{
// `voxel.Item.List` -- the definition-side half of the visibility rule. The
// inventory's own dump (`voxel.Inventory.Dump`, VoxelInventoryComponent.cpp)
// answers "what am I holding"; this answers "what could I hold, and with what
// numbers". Separating them matters when a throw does nothing: an item missing
// from THIS list is a registry problem, an item missing from THAT one is an
// inventory problem, and without both you cannot tell which.
FAutoConsoleCommand GVoxelItemListCmd(
    TEXT("voxel.Item.List"),
    TEXT("Print every item definition: id, category, display name, stack size, vxc material id, throwability and the ")
    TEXT("physical facts a thrown instance uses (cube edge, mass, default launch speed). Block rows are derived from ")
    TEXT("the voxel-core material palette -- see VoxelItem.cpp's identity comment."),
    FConsoleCommandDelegate::CreateStatic([]() { FVoxelItemRegistry::LogAll(); }));
} // namespace
