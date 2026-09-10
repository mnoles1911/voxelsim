#pragma once
// "Voxelmark Player.html": four sub-pages -- STATS, SKILLS, PERKS and
// REPUTATION -- behind the shared sub-tab row.
//
// NONE OF IT HAS A BACKING SYSTEM. There is no level, no XP, no attribute, no
// skill tree, no perk table and no faction standing anywhere in this project;
// the case-sensitive greps are recorded in VoxelScreenData.h. Everything here
// draws FVoxelPlayerData, seeded from the mock.
//
// THE SUB-TABS ARE CHIPS, NOT SHIELDS. The mock's .sub-tab is a pentagonal
// shield (`clip-path: polygon(0 0, 100% 0, 100% 60%, 50% 100%, 0 60%)`) with a
// pixel-art glyph inside it and the label underneath. Slate has no clip-path
// and no polygon brush, and the shield carries no information the label does
// not -- so the four sub-tabs are the same Chip variant the filter rows use,
// and the shield waits for the day this front end has a vector brush. That is
// the same call the map's compass rose records, for the same reason.
//
// THE SKILL TREE IS A TIER LADDER, NOT A GRAPH. The mock lays its eight nodes
// out at explicit percentages inside a 600x480 SVG and draws seven bezier
// connectors between them, two of them glowing to show the unlocked path.
// Slate can draw neither the curves nor the hexagonal node -- and a node grid
// with no connectors would be a graph that has lost the only thing a graph is
// for, which is showing what leads to what. So the nodes are grouped into TIER
// ROWS in dependency order, and each node's own detail panel names its
// requirement in words ("Requires: Riposte II"). The information survives; the
// picture does not. Drawing the real tree needs a line primitive this module
// does not have.
//
// THE CHARACTER TURNTABLE IS NOT DRAWN, on SVoxelInventoryScreen's reasoning:
// fifteen absolutely-positioned divs spun by a CSS 3-D transform, and Slate has
// no perspective.

#include "CoreMinimal.h"
#include "VoxelScreenData.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

enum class EVoxelPlayerPage : uint8
{
	Stats,
	Skills,
	Perks,
	Reputation,
};

class VOXELEARTHUI_API SVoxelPlayerScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelPlayerScreen) {}
		SLATE_ARGUMENT(FVoxelPlayerData, Data)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelPlayerScreen() override;

	static TArray<struct FVoxelScreenAction> Actions();

private:
	TSharedRef<class SWidget> BuildSubTabs();
	TSharedRef<class SWidget> BuildStatsPage();
	TSharedRef<class SWidget> BuildSkillsPage();
	TSharedRef<class SWidget> BuildPerksPage();
	TSharedRef<class SWidget> BuildReputationPage();
	void RebuildSkillDetail();
	void RebuildPerkDetail();

	EVoxelPlayerPage Page = EVoxelPlayerPage::Stats;
	int32 SelectedDiscipline = 0;
	int32 SelectedNode = 2;   // the mock opens on PARRY, its one available node
	int32 SelectedPerk = 4;   // ... and on Undaunted Cavalier

	FVoxelPlayerData Data;
	TSharedPtr<class SWidgetSwitcher> PageSwitcher;
	TSharedPtr<class SVerticalBox> SkillNodeBox;
	TSharedPtr<class SVerticalBox> SkillDetailBox;
	TSharedPtr<class SVerticalBox> PerkDetailBox;
};
