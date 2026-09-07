#pragma once
// The SETTINGS overlay: the port of "Voxelmark Settings.html" (2026-09-07).
//
// ONE WIDGET, TWO HOSTS. The title screen shows it in its panel switcher and
// the pause overlay shows it over the paused world; nothing about the panel
// differs between them, so nothing about it knows which one it is in. Leaving
// is a delegate the host binds -- back to the main column, or back to the pause
// list.
//
// WHAT THE MOCK ASKS FOR AND WHAT THIS BUILDS.
//
//   AUDIO -- MASTER and MUSIC. The mock's third slider, SFX, is NOT built:
//   there is no sound effect in this project to bind it to. See
//   VoxelAudioUserSettings.h, which is where that measurement is recorded.
//
//   DISPLAY -- FULLSCREEN, on UGameUserSettings, which is the existing store.
//
//   GRAPHICS -- the four VoxelGraphicsUserSettings rows the previous settings
//   panel already had. The mock does not show them because it was drawn from
//   the Godot build's settings screen, which has none; they are player-facing
//   settings that shipped on 2026-09-04 and 2026-09-05 with owner verdicts
//   behind them, so they are ADDED to the mock's sections rather than replaced
//   by them. They are drawn as the mock's checkbox rows so the panel reads as
//   one screen.
//
//   GAMEPLAY -- the mock's mining-anchor cycler is NOT built. The anchor is a
//   hardcoded branch in VoxelWorldSubsystem::ComputeCubeMinCorner with no cvar
//   and no setting behind it; a cycler would be a control that changes nothing.
//
// APPLY AND SAVE & LEAVE ARE NOT THE SAME BUTTON, and the split is not
// decoration. Every row here except FULLSCREEN applies and persists the moment
// it is touched -- a volume slider that needed confirming would be unusable,
// because the only way to judge it is to hear it. FULLSCREEN cannot work that
// way: re-creating the swap chain on each click of a checkbox is violent and,
// mid-drag of a slider, disorienting. So the checkbox records a PENDING mode
// and APPLY commits it; SAVE & LEAVE is APPLY followed by leaving.

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SVoxelMenuButton;

class VOXELEARTHUI_API SVoxelSettingsPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelSettingsPanel) {}
		// SAVE & LEAVE, and Escape. The host decides where "back" is.
		SLATE_EVENT(FSimpleDelegate, OnLeave)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	// Holds bare pointers into FVoxelUIStyle -- see that singleton's Shutdown
	// assertion.
	virtual ~SVoxelSettingsPanel() override;

	// The first control a keyboard or gamepad lands on when the panel opens.
	void FocusDefaultWidget();

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;

private:
	// One AUDIO row: label, track, warm fill, knob, and the 0-100 readout.
	// Getter and setter are plain function pointers into
	// VoxelAudioUserSettings, for the reason SVoxelMainMenu's row table gives.
	TSharedRef<class SWidget> BuildSliderRow(const FText& Label, float (*Get)(), void (*Set)(float));
	// One checkbox row (.ck-row): the well, the mark, a label and an italic
	// hint. Used by DISPLAY and by every GRAPHICS row. The toggle is a
	// TFunction rather than an FOnCheckStateChanged so this header does not have
	// to pull SCheckBox.h in behind it -- the DISPLAY row captures `this` and
	// the graphics rows capture a function pointer, and neither is a delegate.
	TSharedRef<class SWidget> BuildCheckRow(const FText& Label, const FText& Hint, TAttribute<bool> Checked,
	                                        TFunction<void(bool)> OnToggled);

	// Commits the pending window mode. Returns quietly when nothing is pending,
	// which is the ordinary case.
	void ApplyPending();

	// The window mode the FULLSCREEN checkbox is showing. Seeded from
	// UGameUserSettings at Construct and written back by ApplyPending.
	bool bPendingFullscreen = false;

	TSharedPtr<SVoxelMenuButton> ApplyButton;
	FSimpleDelegate OnLeave;
};
