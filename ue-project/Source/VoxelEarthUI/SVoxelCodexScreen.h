#pragma once
// "Voxelmark Codex.html": the recipe book and the lore pages, behind one
// category column.
//
// TWO SHAPES BEHIND ONE COLUMN. RECIPES is a searchable directory with a 3x3
// grid detail; PEOPLE, FACTIONS and LORE are an entry list with a parchment
// page; PLACES is neither -- it is the map's own mark list, rendered here so
// the two screens cannot disagree about what the player has named. The mock
// switches the middle and right columns wholesale between these, and so does
// this.
//
// NO RECIPE SYSTEM EXISTS. docs/design/crafting-capabilities-v1.json is a
// static capability table -- what a tool tier unlocks -- not a recipe list, and
// SVoxelSurvivalPanel already renders it as a read-only handbook. The recipes
// here are the mock's, seeded, and their ingredients name things that are not
// items in FVoxelItemRegistry (there is no plank, stick, ingot or hide in this
// game). So the "in pack" column resolves to unknown rather than to zero: see
// FVoxelRecipeIngredient::Held, and CodexHeldUnknown for why printing 0 would
// be a claim rather than a gap.
//
// THE 3x3 PATTERN IS NOT DRAWN. The mock lays each recipe out as a nine-cell
// grid showing where each ingredient goes. That is a real and useful picture --
// and it is a picture of a crafting system with positional recipes, which this
// project does not have and may never choose to have. Drawing it would be the
// UI asserting a design decision. The INGREDIENT LIST is drawn instead, which
// is the part of the recipe that survives whatever shape crafting eventually
// takes, and the yield socket beside it.

#include "CoreMinimal.h"
#include "VoxelScreenData.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class VOXELEARTHUI_API SVoxelCodexScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelCodexScreen) {}
		SLATE_ARGUMENT(FVoxelCodexData, Data)
		// The map's marks, rendered as the PLACES category.
		SLATE_ARGUMENT(TArray<FVoxelMapMark>, Places)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelCodexScreen() override;

	static TArray<struct FVoxelScreenAction> Actions();

private:
	TSharedRef<class SWidget> BuildCategoryColumn();
	void RebuildEntryList();
	void RebuildPage();

	// -1 is RECIPES, -2 is PLACES, and 0..n index Data.Categories. A signed
	// index rather than a separate enum plus an index, because every consumer
	// asks the same question ("which list am I showing") and two fields would
	// let them disagree.
	static constexpr int32 kRecipes = -1;
	static constexpr int32 kPlaces = -2;
	int32 Category = kRecipes;
	int32 SelectedEntry = 0;
	FText SearchText;

	FVoxelCodexData Data;
	TArray<FVoxelMapMark> Places;
	// Recipes surviving the search, as indices into Data.Recipes.
	TArray<int32> VisibleRecipes;

	TSharedPtr<class SVerticalBox> EntryListBox;
	TSharedPtr<class SVerticalBox> PageBox;
};
