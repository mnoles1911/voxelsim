#pragma once
// "Voxelmark Dialogue.html" and the .dlg-* layer of menus_shared.css: the
// speaker block bottom-left, the reply stack top-right, the skill strip
// bottom-right, and the companion aside.
//
// THE SPEC DOCUMENT DOES NOT EXIST. Both the CSS ("per
// design/CONVERSATION_SYSTEM.md") and the mock ("Roland is unvoiced per
// CONVERSATION_SYSTEM.md Roland Voicing Policy", "visible-but-greyed pattern per
// SpeechCheckBroker", "12 skills, 1-100, per SKILLS_AND_PROGRESSION.md") cite
// design documents that are not in this repository and never have been -- there
// is no design/ directory at the repo root and git log finds no deleted one.
// There is also no conversation code: Conversation returns zero hits across both
// modules, Dialogue one comment, Speaker none.
//
// So the ONLY spec for this screen is the mock's own markup plus the comments
// inside it, and the port takes both literally:
//   * Roland is text-only. His replies are rendered as written; nothing draws a
//     portrait or a name for him.
//   * Skill checks are VISIBLE BUT GREYED -- a failing option is drawn dim and
//     unselectable rather than hidden, so the player can see what a higher
//     score would have bought.
//   * The skill strip appears only when at least one option carries a check.
//   * The timer and the companion aside are optional and hidden by default.
//
// FVoxelDialogueData is the whole interface. When a conversation system arrives
// it fills that struct; this widget does not change.
//
// THE PORTRAITS ARE NOT DRAWN. The mock builds Captain Vossant and Orion out of
// forty-odd inline SVG paths each -- gradients, ellipses, a scar, stubble.
// Slate has no path primitive, and forty stacked boxes would be a worse
// drawing than an empty frame. The frame, its bronze ring stack and its
// proportions ARE drawn, so the layout is right the day a portrait texture
// exists to put in it.

#include "CoreMinimal.h"
#include "VoxelScreenData.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

DECLARE_DELEGATE_OneParam(FOnVoxelDialogueOptionChosen, int32 /*OptionIndex*/);

class VOXELEARTHUI_API SVoxelDialogueOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelDialogueOverlay) {}
		SLATE_ARGUMENT(FVoxelDialogueData, Data)
		SLATE_EVENT(FOnVoxelDialogueOptionChosen, OnChoose)
		SLATE_EVENT(FSimpleDelegate, OnClose)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelDialogueOverlay() override;

	void FocusDefaultWidget();

	virtual bool SupportsKeyboardFocus() const override { return true; }
	// 1..9 commit directly, the arrows move the selection, Enter takes it and
	// Escape leaves -- the mock's own bindings, plus Escape, which it has no
	// need for and an in-game overlay does.
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;

private:
	TSharedRef<class SWidget> BuildSpeaker() const;
	TSharedRef<class SWidget> BuildOptions();
	TSharedRef<class SWidget> BuildSkillStrip() const;
	TSharedRef<class SWidget> BuildCompanion() const;

	void MoveSelection(int32 Delta);
	void Commit(int32 Index);

	int32 Selected = 0;
	FVoxelDialogueData Data;
	TArray<TSharedPtr<class SVoxelMenuButton>> OptionButtons;

	FOnVoxelDialogueOptionChosen OnChoose;
	FSimpleDelegate OnClose;
};
