#include "SVoxelSurvivalPanel.h"
#include "VoxelEarthPlayerController.h"
#include "VoxelInventoryComponent.h"
#include "VoxelItem.h"
#include "VoxelUITheme.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "InputCoreTypes.h"

namespace VoxelSurvivalPanelPrivate
{
struct FMethod
{
	const TCHAR* Id;
	const TCHAR* Title;
	const TCHAR* Family;
	const TCHAR* Requires;
	const TCHAR* Unlocks;
	const TCHAR* Note;
};
static const FMethod Handbook[] = {
#include "VoxelCraftingHandbook.inl"
};

TSharedRef<STextBlock> Label(const FString& Value, int32 Size = 14, bool bAccent = false)
{
	return SNew(STextBlock).Text(FText::FromString(Value)).AutoWrapText(true)
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", Size))
		.ColorAndOpacity(VoxelUITheme::Tint(bAccent ? VoxelUITheme::Gold : VoxelUITheme::Ink));
}

UVoxelInventoryComponent* Inventory(TWeakObjectPtr<AVoxelEarthPlayerController> PC)
{
	return PC.IsValid() ? PC->GetInventory() : nullptr;
}

FText SlotText(TWeakObjectPtr<AVoxelEarthPlayerController> PC, int32 Index, bool bCompact)
{
	const auto* Inv = Inventory(PC);
	const auto Slot = Inv ? Inv->GetSlot(Index) : FVoxelInventorySlot{};
	const auto* Def = FVoxelItemRegistry::Find(Slot.ItemId);
	const FString Name = Slot.IsEmpty() ? TEXT("Empty") : Def ? Def->DisplayName : Slot.ItemId.ToString();
	const FString Hint = bCompact && Index < 5 ? FString::Printf(TEXT("[%d]"), Index + 5) : FString::Printf(TEXT("%02d"), Index + 1);
	return FText::FromString(FString::Printf(TEXT("%s  %s\n%s"), *Hint, *Name,
		Slot.IsEmpty() ? TEXT("--") : *FString::Printf(TEXT("x%d"), Slot.Count)));
}

TSharedRef<SWidget> Slot(TWeakObjectPtr<AVoxelEarthPlayerController> PC, int32 Index, bool bCompact)
{
	return SNew(SBorder).Padding(2).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor_Lambda([PC, Index] {
			const auto* Inv = Inventory(PC);
			return VoxelUITheme::Tint(Inv && Inv->GetSelectedSlot() == Index ? VoxelUITheme::Gold : VoxelUITheme::PanelIronEdge);
		})
		[SNew(SButton).ContentPadding(FMargin(7, 8))
			.ToolTipText(FText::FromString(TEXT("Select carried slot. Selecting does not consume or use an item.")))
			.OnClicked_Lambda([PC, Index] { if (auto* Inv = Inventory(PC)) Inv->SetSelectedSlot(Index); return FReply::Handled(); })
			[SNew(STextBlock).Text_Lambda([PC, Index, bCompact] { return SlotText(PC, Index, bCompact); })
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", bCompact ? 11 : 13)).AutoWrapText(true)
				.ColorAndOpacity(VoxelUITheme::Tint(VoxelUITheme::Ink))]];
}
}

void SVoxelSurvivalPanel::Construct(const FArguments& Args)
{
	using namespace VoxelSurvivalPanelPrivate;
	Controller = Args._Controller;
	OnClose = Args._OnClose;
	const auto* Inv = Inventory(Controller);
	TSharedRef<SUniformGridPanel> Slots = SNew(SUniformGridPanel).SlotPadding(3);
	for (int32 I = 0; I < (Inv ? Inv->NumSlots() : 0); ++I)
		Slots->AddSlot(I % 2, I / 2)[Slot(Controller, I, false)];
	ChildSlot
	[SNew(SBorder).Padding(24).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FLinearColor(0, 0, 0, 0.65f))
		[SNew(SScaleBox).Stretch(EStretch::ScaleToFit).StretchDirection(EStretchDirection::DownOnly)
			[SNew(SBox).WidthOverride(1120).HeightOverride(710)
				[SNew(SBorder).Padding(20).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor(VoxelUITheme::Tint(VoxelUITheme::BgStone))
					[SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
						[SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1)[Label(TEXT("PACK & FIELDCRAFT"), 26, true)]
							+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).OnClicked_Lambda([this] { OnClose.ExecuteIfBound(); return FReply::Handled(); })[Label(TEXT("Close  [Esc]"))]]]
						+ SVerticalBox::Slot().FillHeight(1)
						[SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(0.30f).Padding(0, 0, 18, 0)
							[SNew(SVerticalBox)
								+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[Label(TEXT("CARRIED ITEMS"), 17, true)]
								+ SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox) + SScrollBox::Slot()[Slots]]
								+ SVerticalBox::Slot().AutoHeight().Padding(0, 12)[SNew(STextBlock).Text(this, &SVoxelSurvivalPanel::SelectedItem).AutoWrapText(true).ColorAndOpacity(VoxelUITheme::Tint(VoxelUITheme::Ink))]
								+ SVerticalBox::Slot().AutoHeight()[SNew(SButton)
									.IsEnabled_Lambda([this] { const auto* I = Inventory(Controller); const auto* D = I ? FVoxelItemRegistry::Find(I->GetSelectedItemId()) : nullptr; return D && D->CanThrow() && !I->GetSlot(I->GetSelectedSlot()).IsEmpty(); })
									.OnClicked(this, &SVoxelSurvivalPanel::UseSelected)[Label(TEXT("Throw selected & return"))]]]
							+ SHorizontalBox::Slot().FillWidth(0.31f).Padding(0, 0, 18, 0)
							[SNew(SVerticalBox)
								+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[Label(TEXT("CRAFTING HANDBOOK"), 17, true)]
								+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[SNew(SSearchBox).HintText(FText::FromString(TEXT("Search tools, materials, processes...")))
									.OnTextChanged_Lambda([this](const FText& Text) { Search = Text.ToString(); RefreshMethods(); })]
								+ SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(Methods, SVerticalBox)]]]
							+ SHorizontalBox::Slot().FillWidth(0.39f)[SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(Details, SVerticalBox)]]]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 14, 0, 0)
						[Label(TEXT("Handbook preview: crafting processes and tool unlocks are planned. Inventory is live. The world continues while this panel is open."), 12)]
					]]]]];
	RefreshMethods();
	SelectMethod(0);
}

FReply SVoxelSurvivalPanel::OnPreviewKeyDown(const FGeometry&, const FKeyEvent& Event)
{
	// I must remain typeable in the search box. Escape closes from any child.
	if (Event.GetKey() == EKeys::Escape) { OnClose.ExecuteIfBound(); return FReply::Handled(); }
	return FReply::Unhandled();
}

void SVoxelSurvivalPanel::RefreshMethods()
{
	using namespace VoxelSurvivalPanelPrivate;
	Methods->ClearChildren();
	int32 Found = 0;
	for (int32 I = 0; I < UE_ARRAY_COUNT(Handbook); ++I)
	{
		const auto& M = Handbook[I];
		if (!Search.IsEmpty() && !(FString(M.Title) + M.Family + M.Requires + M.Unlocks).Contains(Search)) continue;
		++Found;
		Methods->AddSlot().AutoHeight().Padding(0, 0, 0, 5)
		[SNew(SButton).ContentPadding(10)
			.ButtonColorAndOpacity_Lambda([this, I] { return VoxelUITheme::Tint(SelectedMethod == I ? VoxelUITheme::Bronze : VoxelUITheme::PanelIron); })
			.OnClicked_Lambda([this, I] { SelectMethod(I); return FReply::Handled(); })
			[SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[Label(M.Title, 14)]
				+ SVerticalBox::Slot().AutoHeight()[Label(FString(M.Family) + TEXT(" / Planned"), 11, true)]]];
	}
	if (!Found) Methods->AddSlot().AutoHeight()[Label(TEXT("No matching methods. Try a material or tool name."))];
}

void SVoxelSurvivalPanel::SelectMethod(int32 Index)
{
	using namespace VoxelSurvivalPanelPrivate;
	SelectedMethod = Index;
	const auto& M = Handbook[Index];
	Details->ClearChildren();
	auto Add = [this](const FString& Text, int32 Size, bool bAccent) {
		Details->AddSlot().AutoHeight().Padding(0, 0, 0, 12)[VoxelSurvivalPanelPrivate::Label(Text, Size, bAccent)];
	};
	Add(M.Title, 22, true);
	Add(TEXT("PLANNED PROCESS  /  ") + FString(M.Family), 12, true);
	Add(TEXT("Materials, tools & workshop requirements"), 15, true);
	Add(M.Requires, 14, false);
	Add(TEXT("Opens a route toward"), 15, true);
	Add(M.Unlocks, 14, false);
	if (FCString::Strlen(M.Note)) Add(M.Note, 13, false);
	Add(TEXT("These are capability prerequisites, not a balanced ingredient recipe. Quantities and owned-tool checks will appear when the crafting system is connected."), 12, false);
}

FText SVoxelSurvivalPanel::SelectedItem() const
{
	using namespace VoxelSurvivalPanelPrivate;
	const auto* Inv = Inventory(Controller);
	if (!Inv) return FText::FromString(TEXT("Inventory unavailable."));
	const auto S = Inv->GetSlot(Inv->GetSelectedSlot());
	if (S.IsEmpty()) return FText::FromString(TEXT("Select a carried item to inspect it."));
	const auto* D = FVoxelItemRegistry::Find(S.ItemId);
	return FText::FromString(FString::Printf(TEXT("%s\nCarrying %d total / stack limit %d\n%s"), D ? *D->DisplayName : *S.ItemId.ToString(),
		Inv->CountOf(S.ItemId), D ? D->MaxStack : 0,
		D && D->CanThrow() ? TEXT("Throwable: returns to the game and throws one item.") : TEXT("Item use is not connected for this type yet.")));
}

FReply SVoxelSurvivalPanel::UseSelected()
{
	const auto PC = Controller;
	const auto* Inv = VoxelSurvivalPanelPrivate::Inventory(PC);
	if (Inv) { const int32 SlotIndex = Inv->GetSelectedSlot(); OnClose.ExecuteIfBound(); if (PC.IsValid()) PC->UseHotbarSlot(SlotIndex); }
	return FReply::Handled();
}

TSharedRef<SWidget> SVoxelSurvivalPanel::MakeHotbar(TWeakObjectPtr<AVoxelEarthPlayerController> PC)
{
	using namespace VoxelSurvivalPanelPrivate;
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	const auto* Inv = Inventory(PC);
	for (int32 I = 0; I < FMath::Min(Inv ? Inv->NumSlots() : 0, 5); ++I)
		Row->AddSlot().FillWidth(1).Padding(2)[Slot(PC, I, true)];
	return SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(FMargin(0, 0, 0, 16))
		[SNew(SBox).WidthOverride(660)[SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Justification(ETextJustify::Center)
				.Text_Lambda([PC] { return FText::FromString(PC.IsValid() ? FString::Printf(TEXT("[I] Pack & fieldcraft   |   Dig cube: %d mm per side   |   Place %s\nDig size: [2] 100 mm   [3] 200 mm   [4] 300 mm   |   Left-click to dig"), PC->GetDigSizeVoxels() * 100, *FVoxelItemRegistry::DisplayNameForMaterial(PC->GetPaletteMaterialId())) : TEXT("")); })
				.ColorAndOpacity(VoxelUITheme::Tint(VoxelUITheme::Ink)).ShadowOffset(FVector2D(1, 1))]
			+ SVerticalBox::Slot().AutoHeight()[Row]]];
}
