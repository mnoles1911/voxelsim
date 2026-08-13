#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "VoxelEarthPlayerController.generated.h"

// Dig/place/explosives input (docs/m1-plan.md "Player experience decisions",
// Matt sign-off): LMB = dig, RMB = place, both instant on press and sized by
// DigSizeVoxels (mouse wheel / number keys 1-3); 'T' cycles the placement
// palette; 'F' held charges a thrown explosive, released throws it. The
// actual raycast + edit-log submission lives in UVoxelWorldSubsystem
// (TryDig/TryPlace/CarveSphere) -- this controller only supplies the camera
// ray, the selected size/material, and (for explosives) spawns
// AVoxelExplosive. AVoxelEarthHUD reads the size/material/charge state back
// out via the getters below.
UCLASS()
class VOXELEARTH_API AVoxelEarthPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AVoxelEarthPlayerController();

protected:
	virtual void SetupInputComponent() override;
	virtual void BeginPlay() override;

public:
	// --- HUD queries (AVoxelEarthHUD) --------------------------------------

	int32 GetDigSizeVoxels() const { return DigSizeVoxels; }

	// --- items and the hotbar ----------------------------------------------
	//
	// THE INVENTORY LIVES ON THE CONTROLLER, NOT THE PAWN, and that is the
	// choice everything else here follows from. A pawn can be destroyed and
	// respawned; what you are carrying should survive that. It also means a
	// thrown item looking for "the inventory to return to" hops pawn ->
	// controller, which AVoxelThrownItem already does.
	class UVoxelInventoryComponent* GetInventory() const { return Inventory; }

	// A NUMBER KEY MEANS "USE THE ITEM IN THIS SLOT", NOT "THROW".
	//
	// The owner asked for a number key that throws a cube. Binding a key
	// literally to "throw" would have been three lines and would have had to be
	// unpicked the moment the second kind of item existed -- and the brief was
	// explicit that this framework is meant to carry "tools, voxel blocks, etc".
	// So the key dispatches on the item's CATEGORY: a throwable is thrown, a
	// block will be placed, a tool will do its thing. Today only the throwable
	// branch exists, and it does exactly what was asked; the others log that
	// they are not implemented rather than silently doing nothing, because
	// "I pressed the key and nothing happened" is the one report that costs an
	// evening to diagnose.
	void UseHotbarSlot(int32 SlotIndex);

	// Raw vxc::MaterialId value (this header stays voxel-core-free by
	// doctrine, so the type is uint8, not vxc::MaterialId).
	uint8 GetPaletteMaterialId() const { return PaletteMaterialId; }

	bool IsChargingExplosive() const { return bChargingExplosive; }

	// 0 at charge start, 1 at/after MaxChargeSeconds; 0 when not charging.
	float GetExplosiveChargeAlpha() const;

	// --- M3 wave 1 (docs/m3-plan.md): client -> server edit intents --------
	//
	// These live on the PlayerController rather than the shared AVoxelEditRelay
	// actor because UE Server RPCs are only callable by the connection that
	// OWNS the target actor -- a PlayerController is always owned by its own
	// client connection, so this is the correct, minimal-plumbing place for
	// them (see VoxelEditRelay.h's class comment). Called by
	// UVoxelWorldSubsystem's NM_Client role-split path (TryDig/TryPlace/
	// CarveSphere) on the LOCAL player's own controller immediately after
	// applying the same edit as a local prediction; the server re-validates
	// and re-applies via the exact same UVoxelWorldSubsystem::TryDig/
	// TryPlace/CarveSphere path a local server edit uses.
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerSubmitDigIntent(const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerSubmitPlaceIntent(const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels, uint8 MaterialId,
	                              const FVector& PlayerActorLocation);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerSubmitCarveIntent(const FVector& CenterUU, float RadiusUU, float JitterUU);

	// Join sync (m3-plan.md "Join sync"): fired once from BeginPlay on an
	// NM_Client's own locally-controlled instance. Server replies with the
	// full edit log, chunked <= 48KB per Client RPC (see the .cpp), each
	// call routed through UVoxelWorldSubsystem::ReceiveJoinSyncChunk.
	UFUNCTION(Server, Reliable)
	void ServerRequestJoinSync();

	UFUNCTION(Client, Reliable)
	void ClientReceiveJoinSyncChunk(const TArray<uint8>& Bytes, bool bFinal);

private:
	// M3 wave 2 "Validation hardening" (docs/m3-plan.md): per-connection
	// token-bucket rate cap shared by every ServerSubmit*Intent handler
	// (voxel.Server.MaxIntentsPerSec, default 10/s). Continuous refill
	// (fractional tokens, not a fixed "reset every 1s" window) so a client
	// can't game a window boundary for a double-rate burst. Returns false
	// (logs a rejection reason -- IntentName is the calling RPC's name for
	// the log line) and consumes nothing when the bucket is empty; the
	// caller must then return without applying the edit. Server-side
	// (authority) only -- meaningless on a client's own locally-controlled
	// instance, which only ever SENDS these RPCs.
	bool TryConsumeIntentToken(const TCHAR* IntentName);
	double IntentTokens = 0.0;
	double LastIntentTokenRefillSeconds = -1.0;

	void OnDig();
	void OnPlace();

	// Dig/place cube size selection (m1-plan.md "Dig sizes" row): mouse
	// wheel cycles 1<->2<->4, number keys 1/2/3 select directly.
	void CycleDigSizeUp();
	void CycleDigSizeDown();
	// `1`: dump a bucket of water at the player's own position (design aid).
	// Amount is voxel.Water.BucketFill so it can be dialled from the console
	// mid-session without a rebuild -- the whole point is trying different
	// volumes on different terrain until the behaviour is legible.
	void PourWaterBucket();

	void SelectDigSize1();
	void SelectDigSize2();
	void SelectDigSize4();

	// Creative placement palette cycle (m1-plan.md "Place" row): rock -> soil
	// -> sand -> rock ...
	void CyclePaletteMaterial();

	// Explosive charge/throw (m1-plan.md "Explosives v1" row): hold F to
	// charge, release to throw -- see VoxelExplosive.h for the fuse/carve.
	void OnChargeStart();
	void OnChargeRelease();

	// docs/debug-tooling-plan.md P1 "CVars + F3": cycles voxel.Debug 0->1->2->0.
	void OnCycleDebugMode();

	// In-game debug overlay (usability task). F1 shows/hides it; the arrow
	// keys and Enter navigate it. All the state and the row table live on
	// AVoxelEarthHUD -- these handlers only forward, and they are no-ops when
	// the overlay is hidden so the arrow keys stay free for anything else that
	// wants them later. Arrow keys and Enter were chosen because every other
	// key this project binds is already spoken for (WASD/Space/Ctrl/Shift/Alt,
	// G, C, T, F, 1-3, the mouse buttons and wheel, F3).
	void OnToggleDebugOverlay();
	void OnOverlayUp();
	void OnOverlayDown();
	void OnOverlayLeft();
	void OnOverlayRight();
	void OnOverlayActivate();

	// Nullptr unless an AVoxelEarthHUD is the active HUD (it always is via
	// AVoxelEarthGameMode::HUDClass, but a -game run with a HUD override
	// should not crash).
	class AVoxelEarthHUD* GetVoxelHUD() const;

	// The five hotbar keys, one thunk each. UE's BindKey takes a no-argument
	// member function, so the slot index cannot be a parameter -- five thunks is
	// the whole cost of that, and it keeps the binding table readable.
	void OnUseHotbar1();
	void OnUseHotbar2();
	void OnUseHotbar3();
	void OnUseHotbar4();
	void OnUseHotbar5();

	// Created in the constructor rather than attached at BeginPlay by a
	// bootstrap subsystem. A scaffold subsystem exists
	// (UVoxelInventoryBootstrapSubsystem) only because the agent that wrote the
	// inventory could not edit this file; with the controller owning the
	// component properly, that scaffold is dead weight and its cvar
	// (voxel.Inventory.AutoAttach) now defaults off.
	UPROPERTY(VisibleAnywhere, Category = "Voxel Earth|Items")
	TObjectPtr<class UVoxelInventoryComponent> Inventory;

	int32 DigSizeVoxels = 1;

	// vxc::MAT_ROCK == 2 (voxelcore/core.h); kept as a numeric literal here
	// since this UHT-parsed header must stay voxel-core-free by doctrine.
	uint8 PaletteMaterialId = 2;

	bool bChargingExplosive = false;
	float ChargeStartTimeSeconds = 0.f;

	// m1-plan.md "Explosives v1" row: 0.3s->1.5s charge hold maps to
	// 600->1600 UU/s throw speed; thrown with a 30-degree upward arc.
	static constexpr float MinChargeSeconds = 0.3f;
	static constexpr float MaxChargeSeconds = 1.5f;
	static constexpr float MinThrowSpeedUU = 600.0f;
	static constexpr float MaxThrowSpeedUU = 1600.0f;
	static constexpr float ThrowUpwardArcDegrees = 30.0f;

	// M3 wave 1 gate verification (docs/m3-plan.md "two clients dig the same
	// hole"): -VoxelAutoDigAfter=<s> fires one TryDig at a fixed, seed-
	// derived world column (NOT pawn-relative -- every process computes the
	// identical spot from the shared seed, so multiple client processes
	// "dig the same hole" without any out-of-band coordination); intended
	// for -game clients in the dedicated-server verification run, harmless
	// if passed on a listen-server/standalone local player too.
	// -VoxelDumpDigestAfter=<s> logs this process's own perspective
	// (seed + World::editedDigest()) -- the dedicated server's equivalent
	// dump lives in AVoxelEarthGameMode::BeginPlay (GameMode only exists
	// server-side). Both read BeginPlay()'s command line once.
	// Usability task verification fixtures. The overlay and the walk/fly mode
	// line are HUD canvas drawing driven by key presses, and the existing
	// -VoxelScreenshotAfter chain captures with bShowUI=FALSE -- so neither
	// could ever appear in a verification shot. These three switches make both
	// capturable headlessly, which is the only way this stays verifiable:
	//
	//   -VoxelWalkModeAfter=<s>  enter walk mode at <s> (default is fly)
	//   -VoxelFlySpeedStep=<n>   set the fly speed table index (0-based)
	//   -VoxelOverlayShot=<s>    at <s>: open the overlay, screenshot WITH UI
	//                            ("VoxelOverlay*.png"), then quit 4 s later.
	//   -VoxelOverlayRow=<n>     which overlay row is selected in that shot.
	//
	// -VoxelOverlayShot=<s> without -VoxelOverlayRow captures the overlay with
	// its default (top) selection; pass -VoxelOverlayShot with the overlay
	// suppressed by simply not passing it at all -- there is no "close" form,
	// because the overlay is default-OFF and nothing else can open it.
	//   -VoxelOverlayOn          open the overlay and leave it open (no shot,
	//                            no quit) -- the overlay-on arm of the M1 perf
	//                            A/B, which -VoxelOverlayShot cannot serve.
	FTimerHandle WalkModeTimerHandle;
	FTimerHandle OverlayOnTimerHandle;
	FTimerHandle OverlayShotTimerHandle;
	FTimerHandle OverlayQuitTimerHandle;

	FTimerHandle AutoDigTimerHandle;
	FTimerHandle DumpDigestTimerHandle;
	// Self-quit a few seconds after DumpDigestTimerHandle fires (gate-run
	// convenience: nothing else naturally ends a headless -game client).
	FTimerHandle DigestQuitTimerHandle;
};
