#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class AVoxelEarthPlayerController;
class SVerticalBox;

class SVoxelSurvivalPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelSurvivalPanel) {}
		SLATE_ARGUMENT(TWeakObjectPtr<AVoxelEarthPlayerController>, Controller)
		SLATE_EVENT(FSimpleDelegate, OnClose)
	SLATE_END_ARGS()
	void Construct(const FArguments& Args);
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnPreviewKeyDown(const FGeometry&, const FKeyEvent&) override;
	static TSharedRef<SWidget> MakeHotbar(TWeakObjectPtr<AVoxelEarthPlayerController> Controller);
private:
	void RefreshMethods();
	void SelectMethod(int32 Index);
	FText SelectedItem() const;
	FReply UseSelected();
	TWeakObjectPtr<AVoxelEarthPlayerController> Controller;
	FSimpleDelegate OnClose;
	TSharedPtr<SVerticalBox> Methods;
	TSharedPtr<SVerticalBox> Details;
	FString Search;
	int32 SelectedMethod = 0;
};
