#pragma once
// "Voxelmark Death Screen.html": YOU DIED, a cause-of-death quip, the day stamp
// and two buttons.
//
// NOTHING CAN KILL THE PLAYER YET. There is no health, no damage, no death
// event and no respawn entry point anywhere in this project -- TakeDamage,
// ApplyDamage, IsDead, OnDeath and HandleDeath return zero hits across both
// modules, and every "Respawn" hit is prose in a comment or
// AVoxelThrownItem's item-respawn timer. AVoxelEarthGameMode::BeginPlayerSession
// calls RestartPlayer, and its own comment says that is the FIRST spawn and
// warns against building a second path through it.
//
// So this screen is reachable only from -VoxelDeathShot, and the subsystem says
// so when it opens one. It is built now rather than later because it is the
// screen a death system would otherwise have to grow a UI for at the same time
// as it grew the death, and because it costs one widget.
//
// THE TEN PER-CAUSE QUIP POOLS ARE NOT PORTED. The mock keys its quip off a
// CAUSE constant with pools for bears, wolves, starvation, cold, falling,
// drowning, the dark, mobs, fire, mining and goblins. A cause this game can
// never report would select a pool that could never be reached; the generic
// pool is ported and the rest wait for something that can name a cause. See
// VoxelUIStrings::DeathQuips.
//
// THE WORLD IS NOT DESATURATED. The mock greys the scene out over 2.6 s and
// inks it down under a radial wash. Slate can neither desaturate what is behind
// it nor draw a radial gradient, so this lays one flat black wash at
// DeathDimAlpha -- the same substitution, and the same reasoning, as
// VoxelOverlayChrome::Scrim's.

#include "CoreMinimal.h"
#include "VoxelScreenData.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class VOXELEARTHUI_API SVoxelDeathScreen : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SVoxelDeathScreen) {}
		SLATE_ARGUMENT(FVoxelDeathData, Data)
		SLATE_EVENT(FSimpleDelegate, OnRespawn)
		SLATE_EVENT(FSimpleDelegate, OnQuit)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SVoxelDeathScreen() override;

	void FocusDefaultWidget();

	virtual bool SupportsKeyboardFocus() const override { return true; }
	// Enter respawns and Escape quits, which is the mock's own binding.
	virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent) override;

private:
	TSharedPtr<class SVoxelMenuButton> RespawnButton;
	FSimpleDelegate OnRespawn;
	FSimpleDelegate OnQuit;
};
