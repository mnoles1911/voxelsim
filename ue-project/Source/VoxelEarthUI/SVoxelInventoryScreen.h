#pragma once
// "Voxelmark Inventory.html": the 8x8 pack over its hotbar, the seven filter
// chips, and a side panel that toggles between the crafting grid and the
// character paperdoll.
//
// THE ONE SCREEN OF THE FIVE WITH A REAL BACKING SYSTEM.
// UVoxelInventoryComponent and FVoxelItemRegistry exist, are replicated, and
// already feed SVoxelSurvivalPanel's hotbar. So the pack and the hotbar here
// are live: what the player is carrying is what this draws.
//
// WHAT IS NOT LIVE, AND WHY IT IS DRAWN ANYWAY:
//   * RARITY -- FVoxelItemDef has no rarity field, so every real row is Common
//     and the four coloured rings only ever appear on the paperdoll's seeded
//     gear. The ring is in the shared slot chrome regardless, because a cell
//     that could not express rarity would need rewriting the day items gain it.
//   * DURABILITY -- nothing in this game wears out. The .dura strip is drawn
//     only for items whose Durability is non-negative, which is none of them.
//   * EQUIPMENT -- there is no equip concept at all. CHARACTER mode draws the
//     eight labelled sockets and says on the screen that they are chrome,
//     rather than implying the player has lost their gear.
//   * THE CATEGORY FILTERS -- FVoxelItemDef's four categories are Block, Tool,
//     Throwable and Misc, which map onto Materials and Tools and nothing else.
//     Weapons, Armor, Food and Quest are always empty and are still drawn, so
//     the chip row does not change length as the pack fills.
//   * CRAFTING -- there is no recipe system (docs/design/crafting-capabilities
//     -v1.json is a static capability table, not recipes). The 3x3 grid and its
//     output socket are drawn and inert, and the CRAFT button is disabled.
//
// THE MOCK'S DRAG AND DROP IS NOT PORTED. Dragging between the pack, the grid,
// the hotbar and the paperdoll needs a swap the inventory component does not
// expose -- TryAddItem and TryRemoveFromSlot are add and remove, not move -- and
// inventing a move out of remove-then-add can lose a stack when the destination
// is full. That is a gameplay change, not a UI one; see the report.
//
// THE CHARACTER TURNTABLE IS NOT PORTED. The mock builds a voxel figure out of
// fifteen absolutely-positioned divs and spins it with a CSS 3-D transform.
// Slate has no 3-D transform and no perspective, and a static front elevation
// of the same fifteen boxes would be a worse drawing than the empty plinth --
// so the render frame draws the plinth and the sockets, which are the parts
// that carry information.

#include "CoreMinimal.h"
#include "VoxelScreenData.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class VOXELEARTHUI_API SVoxelInventoryScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelInventoryScreen) {}
		SLATE_ARGUMENT(FVoxelInventoryScreenData, Data)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelInventoryScreen() override;

	// The hints this screen contributes to the shell's action bar.
	static TArray<struct FVoxelScreenAction> Actions();

private:
	TSharedRef<class SWidget> BuildPackColumn();
	TSharedRef<class SWidget> BuildSideColumn();
	TSharedRef<class SWidget> BuildCraftMode();
	TSharedRef<class SWidget> BuildCharacterMode();
	void RebuildPack();

	// Which filter chip is lit, and the live contents of the search box. Both
	// are view state, not game state, so they live here rather than in the data
	// struct -- the same split SVoxelLoadDialog makes for its filter chips.
	int32 ActiveFilter = 0;
	FText SearchText;
	bool bCharacterMode = false;

	FVoxelInventoryScreenData Data;
	TSharedPtr<class SUniformGridPanel> PackGrid;
	TSharedPtr<class SWidgetSwitcher> SideSwitcher;
	TSharedPtr<class SVoxelMenuButton> FirstChip;
};
