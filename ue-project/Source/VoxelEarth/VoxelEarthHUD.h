#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "VoxelEarthHUD.generated.h"

// m1-plan.md "Player experience decisions" (Matt sign-off), HUD row: plain
// DrawHUD canvas calls (no UMG) -- a crosshair dot plus bottom-left text
// showing the current dig/place cube size, palette material, and (while
// charging) a fill bar for the explosive throw charge. Reads its state from
// AVoxelEarthPlayerController (PlayerOwner); wired into
// AVoxelEarthGameMode::HUDClass.
UCLASS()
class VOXELEARTH_API AVoxelEarthHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

	// --- In-game debug overlay (usability task) -----------------------------
	//
	// A key-driven, navigable panel that flips the SAME voxel.Debug* / voxel.GI
	// cvars the console does -- deliberately not a parallel debug system (see
	// VoxelDebug.h). Default OFF and only ever shown by an explicit F1 press,
	// so it cannot appear in a headless verification screenshot.
	//
	// AVoxelEarthPlayerController owns the key bindings and forwards to these;
	// the state and the item table live here because everything they read is
	// already what DrawHUD reads.
	void ToggleDebugOverlay();
	bool IsDebugOverlayVisible() const { return bOverlayVisible; }

	// Up/Down: move the selection (Delta -1 / +1, wraps).
	void MoveOverlaySelection(int32 Delta);
	// Left/Right/Enter: change the selected row's value. Delta is -1 or +1;
	// boolean rows ignore the sign and just flip.
	void AdjustOverlaySelection(int32 Delta);

private:
	// --- Debug overlay ------------------------------------------------------

	// Rows the selection can land on. Kept as an enum (not an index into a
	// runtime array) so the switch in AdjustOverlaySelection is exhaustive and
	// adding a row is a compile error until it is both drawn and handled.
	enum class EOverlayRow : uint8
	{
		MovementMode = 0,
		FlySpeed,
		// Sits with the movement rows, not with the mode-2 layers below, because
		// voxel.Debug.PlayerBox is deliberately NOT gated on voxel.Debug >= 2
		// (see VoxelDebug.h) -- grouping it with them would imply a gate it does
		// not have.
		PlayerBox,
		DebugMode,
		ChunkStates,
		Bounds,
		Rings,
		GlobalIllumination,
		Wireframe,
		Count
	};

	void DrawDebugOverlay();

	// One "  Label .......... value" row, highlighted (and prefixed with '>')
	// when it is the selected one. Advances InOutY by OverlayLineHeightPx.
	void DrawOverlayRow(EOverlayRow Row, const FString& Label, const FString& Value, float PanelX, float& InOutY);

	// Non-selectable status line inside the overlay panel.
	void DrawOverlayInfo(const FString& Text, const FLinearColor& Color, float PanelX, float& InOutY);

	class AVoxelEarthFlyPawn* GetVoxelPawn() const;

	// Always-on, one line, bottom-right: "FLY 30 m/s" / "WALK". Suppressed
	// under VoxelDebug::IsUnattendedFixtureRun() so no headless verification
	// screenshot gains a pixel it did not have before this task.
	void DrawModeLine();

	bool bOverlayVisible = false;
	EOverlayRow OverlaySelection = EOverlayRow::MovementMode;

	// ShowFlag.Wireframe is write-only from here (there is no console read-back
	// for a show flag), so the overlay tracks what it last set. Starts false,
	// which is the engine default for a game viewport.
	bool bWireframeRequested = false;

	static constexpr float OverlayPanelWidthPx = 560.0f;
	static constexpr float OverlayMarginPx = 12.0f;
	static constexpr float OverlayLineHeightPx = 16.0f;
	// Column x-offset (from the panel's left edge) where a row's value starts.
	static constexpr float OverlayValueColumnPx = 300.0f;

	// Small filled square at the screen center.
	static constexpr float CrosshairHalfSizePx = 2.0f;

	// Bottom-left text block layout.
	static constexpr float MarginPx = 24.0f;
	static constexpr float LineHeightPx = 20.0f;

	// Charge bar (drawn above the text block while charging).
	static constexpr float ChargeBarWidthPx = 160.0f;
	static constexpr float ChargeBarHeightPx = 12.0f;

	// --- Perf HUD (docs/debug-tooling-plan.md P1, voxel.Debug mode >= 1) -----

	// Top-left multi-line panel, refreshed at 1Hz (matches the subsystem's
	// own 1Hz FVoxelPerfSnapshot refresh -- rebuilding the formatted string
	// more often than the data actually changes would be wasted work); drawn
	// every frame from the cached string.
	void DrawPerfHUD();

	static constexpr float PerfPanelMarginPx = 12.0f;
	static constexpr float PerfPanelLineHeightPx = 16.0f;
	static constexpr float PerfRefreshIntervalSeconds = 1.0f;

	FString CachedPerfHUDText;
	float PerfHUDLastRefreshWorldSeconds = -1000.f;

	// --- FPS counter -------------------------------------------------------
	//
	// SEPARATE FROM THE 1 Hz PERF PANEL ON PURPOSE. The panel republishes once
	// a second because its contents are 1 Hz aggregates; an FPS readout that
	// only moved once a second would be unusable for the thing an FPS counter
	// is actually for -- turning around, flying, and watching the number
	// respond. This refreshes four times a second.
	//
	// TIMED WITH FPlatformTime::Seconds(), NOT world delta time. World time is
	// dilated by slomo, stops when PIE is paused, and is clamped by the
	// smoothing settings -- so it would report a frame rate the editor is not
	// actually running at, which is the one thing this must never do.
	static constexpr float FpsRefreshIntervalSeconds = 0.25f;
	static constexpr int32 FpsHistorySize = 240; // ~4 s at 60 fps

	double FpsLastFrameSeconds = 0.0;     // 0 = not yet sampled
	double FpsWindowStartSeconds = 0.0;
	int32 FpsWindowFrames = 0;
	double FpsWindowWorstMs = 0.0;

	// Rolling frame times for the 1% low. A plain average hides exactly the
	// hitches this project spends its time chasing, so the panel carries both.
	TArray<double> FpsHistoryMs;
	int32 FpsHistoryNext = 0;

	FString CachedFpsText;
	// Kept as a number as well as in the string: the draw colour keys off it,
	// and re-parsing a formatted string to recover a value it was built from is
	// the kind of thing that survives until someone changes the format.
	double CachedFpsValue = 0.0;
};
