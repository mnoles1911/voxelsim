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

protected:
	virtual void SetupInputComponent() override;
	virtual void BeginPlay() override;

public:
	// --- HUD queries (AVoxelEarthHUD) --------------------------------------

	int32 GetDigSizeVoxels() const { return DigSizeVoxels; }

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
	FTimerHandle AutoDigTimerHandle;
	FTimerHandle DumpDigestTimerHandle;
	// Self-quit a few seconds after DumpDigestTimerHandle fires (gate-run
	// convenience: nothing else naturally ends a headless -game client).
	FTimerHandle DigestQuitTimerHandle;
};
