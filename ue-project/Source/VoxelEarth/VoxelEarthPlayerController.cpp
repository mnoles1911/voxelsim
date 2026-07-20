#include "VoxelEarthPlayerController.h"

#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "VoxelDebug.h"
#include "VoxelEarth.h"
#include "VoxelExplosive.h"
#include "VoxelWorldSubsystem.h"

void AVoxelEarthPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	if (!InputComponent)
	{
		return;
	}

	// Legacy raw-key bindings (docs/m1-plan.md Stage 2 decisions table:
	// "legacy input bindings, no Enhanced Input assets").
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &AVoxelEarthPlayerController::OnDig);
	InputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &AVoxelEarthPlayerController::OnPlace);

	// Dig/place size selection (m1-plan.md "Dig sizes" row).
	InputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &AVoxelEarthPlayerController::CycleDigSizeUp);
	InputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &AVoxelEarthPlayerController::CycleDigSizeDown);
	InputComponent->BindKey(EKeys::One, IE_Pressed, this, &AVoxelEarthPlayerController::SelectDigSize1);
	InputComponent->BindKey(EKeys::Two, IE_Pressed, this, &AVoxelEarthPlayerController::SelectDigSize2);
	InputComponent->BindKey(EKeys::Three, IE_Pressed, this, &AVoxelEarthPlayerController::SelectDigSize4);

	// Creative placement palette cycle (m1-plan.md "Place" row).
	InputComponent->BindKey(EKeys::T, IE_Pressed, this, &AVoxelEarthPlayerController::CyclePaletteMaterial);

	// Explosive charge/throw (m1-plan.md "Explosives v1" row).
	InputComponent->BindKey(EKeys::F, IE_Pressed, this, &AVoxelEarthPlayerController::OnChargeStart);
	InputComponent->BindKey(EKeys::F, IE_Released, this, &AVoxelEarthPlayerController::OnChargeRelease);

	// docs/debug-tooling-plan.md P1 "CVars + F3": F3 cycles voxel.Debug
	// 0(off)->1(perf HUD)->2(HUD+visualizations)->0 in PIE/game.
	InputComponent->BindKey(EKeys::F3, IE_Pressed, this, &AVoxelEarthPlayerController::OnCycleDebugMode);
}

void AVoxelEarthPlayerController::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	if (!World || !IsLocalController())
	{
		// Only this process's OWN locally-controlled instance should drive
		// join-sync / verification switches -- a dedicated server also owns
		// one AVoxelEarthPlayerController PER CONNECTED CLIENT (remote-owned
		// proxies), and BeginPlay runs for those too; IsLocalController() is
		// false for all of them there, so this whole block correctly never
		// fires on the dedicated server's own process.
		return;
	}

	UVoxelWorldSubsystem* Subsystem = World->GetSubsystem<UVoxelWorldSubsystem>();
	if (!Subsystem)
	{
		return;
	}

	// M3 wave 1 (docs/m3-plan.md "Join sync"): a replica client requests the
	// full authoritative edit log on join, before accepting any live
	// MulticastAppliedEntries batches (buffered meanwhile -- see
	// UVoxelWorldSubsystem::BeginJoinSync/ReceiveJoinSyncChunk/ReceiveLiveEntries).
	if (World->GetNetMode() == NM_Client)
	{
		Subsystem->BeginJoinSync();
		ServerRequestJoinSync();
	}

	// M3 gate verification switches (docs/m3-plan.md "two clients dig the
	// same hole") -- see the header's doc comment on AutoDigTimerHandle/
	// DumpDigestTimerHandle for why both live here rather than GameMode.
	float AutoDigAfterSeconds = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelAutoDigAfter="), AutoDigAfterSeconds) && AutoDigAfterSeconds > 0.f)
	{
		UE_LOG(LogVoxelEarth, Log, TEXT("VoxelAutoDigAfter: will TryDig at the fixed (0,0) column in %.1fs"), AutoDigAfterSeconds);
		World->GetTimerManager().SetTimer(
		    AutoDigTimerHandle,
		    FTimerDelegate::CreateWeakLambda(this,
		                                      [this]()
		                                      {
			                                      UWorld* W = GetWorld();
			                                      UVoxelWorldSubsystem* Sub = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
			                                      if (!Sub)
			                                      {
				                                      return;
			                                      }
			                                      // Fixed, seed-derived world column (NOT pawn-relative): every
			                                      // process launched with the same -VoxelSeed computes the exact
			                                      // same spot, so independently-run client processes genuinely
			                                      // "dig the same hole" with zero coordination beyond the shared
			                                      // seed and this fixed switch.
			                                      // +500UU (5m) clearance: comfortably inside
			                                      // UVoxelWorldSubsystem::DigPlaceRangeMeters (8m) so the
			                                      // straight-down ray actually reaches the surface.
			                                      const double SurfaceUU = Sub->GetSurfaceHeightUU(0.0, 0.0);
			                                      const FVector FixedCameraLoc(0.0, 0.0, SurfaceUU + 500.0);
			                                      const FVector FixedCameraDir(0.0, 0.0, -1.0);
			                                      const bool bDug = Sub->TryDig(FixedCameraLoc, FixedCameraDir, 2);
			                                      UE_LOG(LogVoxelEarth, Log,
			                                             TEXT("VoxelAutoDigAfter: TryDig at fixed column (0,0), size=2 -> %s"),
			                                             bDug ? TEXT("applied") : TEXT("no-op (nothing hit in range)"));
		                                      }),
		    AutoDigAfterSeconds, false);
	}

	float DumpDigestAfterSeconds = 0.f;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelDumpDigestAfter="), DumpDigestAfterSeconds) && DumpDigestAfterSeconds > 0.f)
	{
		World->GetTimerManager().SetTimer(
		    DumpDigestTimerHandle,
		    FTimerDelegate::CreateWeakLambda(this,
		                                      [this]()
		                                      {
			                                      UWorld* W = GetWorld();
			                                      UVoxelWorldSubsystem* Sub = W ? W->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
			                                      if (!Sub)
			                                      {
				                                      return;
			                                      }
			                                      UE_LOG(LogVoxelEarth, Log,
			                                             TEXT("VoxelDigestDump: role=Client seed=%llu editedDigest=0x%016llX"),
			                                             (unsigned long long)Sub->GetSeed(), (unsigned long long)Sub->GetEditedDigest());

			                                      UWorld* QuitWorld = GetWorld();
			                                      if (QuitWorld)
			                                      {
				                                      QuitWorld->GetTimerManager().SetTimer(
				                                          DigestQuitTimerHandle,
				                                          FTimerDelegate::CreateLambda([]() { FPlatformMisc::RequestExit(/*bForce*/ false); }),
				                                          5.f, false);
			                                      }
		                                      }),
		    DumpDigestAfterSeconds, false);
	}
}

bool AVoxelEarthPlayerController::TryConsumeIntentToken(const TCHAR* IntentName)
{
	// M3 wave 2 "Validation hardening" (docs/m3-plan.md): continuous-refill
	// token bucket, capacity == voxel.Server.MaxIntentsPerSec. Starts full
	// (not empty) on first use so a client's very first post-join intent
	// isn't throttled by bucket-warm-up.
	const UWorld* World = GetWorld();
	const double NowSeconds = World ? World->GetTimeSeconds() : 0.0;
	const double MaxPerSec = FMath::Max(1, VoxelDebug::GetServerMaxIntentsPerSec());

	if (LastIntentTokenRefillSeconds < 0.0)
	{
		IntentTokens = MaxPerSec;
		LastIntentTokenRefillSeconds = NowSeconds;
	}
	const double ElapsedSeconds = FMath::Max(0.0, NowSeconds - LastIntentTokenRefillSeconds);
	LastIntentTokenRefillSeconds = NowSeconds;
	IntentTokens = FMath::Min(MaxPerSec, IntentTokens + ElapsedSeconds * MaxPerSec);

	if (IntentTokens < 1.0)
	{
		UE_LOG(LogVoxelEdit, Warning,
		       TEXT("%s rejected: rate cap exceeded (voxel.Server.MaxIntentsPerSec=%.0f) for %s"), IntentName, MaxPerSec,
		       *GetName());
		return false;
	}
	IntentTokens -= 1.0;
	return true;
}

bool AVoxelEarthPlayerController::ServerSubmitDigIntent_Validate(const FVector& CameraLoc, const FVector& CameraDir, int32 SizeVoxels)
{
	return SizeVoxels >= 1 && SizeVoxels <= 8; // loose wire-value sanity bound; the real cap is enforced (and logged) below
}

void AVoxelEarthPlayerController::ServerSubmitDigIntent_Implementation(const FVector& CameraLoc, const FVector& CameraDir,
                                                                        int32 SizeVoxels)
{
	if (!TryConsumeIntentToken(TEXT("ServerSubmitDigIntent")))
	{
		return;
	}
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	APawn* MyPawn = GetPawn();
	if (!Subsystem || !MyPawn)
	{
		return;
	}
	// Range validation (m3-plan.md "Authority flow": "server validates
	// (range from pawn ...)"): CastFromCamera already caps the ray itself to
	// DigPlaceRangeMeters, but a client could lie about CameraLoc -- reject
	// if the claimed camera origin is implausibly far from THIS connection's
	// own pawn (generous slack for the over-the-shoulder camera offset).
	constexpr double MaxCameraPawnOffsetUU = 500.0; // 5m
	if (FVector::Dist(CameraLoc, MyPawn->GetActorLocation()) > MaxCameraPawnOffsetUU)
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("ServerSubmitDigIntent rejected: camera origin too far from owning pawn."));
		return;
	}
	// M3 wave 2 "Validation hardening": size-cap enforcement (dig/place <=
	// MaxCubeSizeVoxels) -- rejected (logged), not silently clamped: a
	// client claiming an oversized cube gets nothing applied rather than a
	// smaller-than-requested edit it didn't ask for.
	if (SizeVoxels > UVoxelWorldSubsystem::MaxCubeSizeVoxels)
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("ServerSubmitDigIntent rejected: SizeVoxels=%d exceeds MaxCubeSizeVoxels=%d"), SizeVoxels,
		       UVoxelWorldSubsystem::MaxCubeSizeVoxels);
		return;
	}
	// Same edit path a local server-side dig uses -- re-validates range
	// (raycast cap) identically (UVoxelWorldSubsystem::TryDig, authority
	// branch: applies directly + broadcasts).
	Subsystem->TryDig(CameraLoc, CameraDir, SizeVoxels);
}

bool AVoxelEarthPlayerController::ServerSubmitPlaceIntent_Validate(const FVector& CameraLoc, const FVector& CameraDir,
                                                                     int32 SizeVoxels, uint8 MaterialId, const FVector& PlayerActorLocation)
{
	return SizeVoxels >= 1 && SizeVoxels <= 8;
}

void AVoxelEarthPlayerController::ServerSubmitPlaceIntent_Implementation(const FVector& CameraLoc, const FVector& CameraDir,
                                                                          int32 SizeVoxels, uint8 MaterialId,
                                                                          const FVector& PlayerActorLocation)
{
	if (!TryConsumeIntentToken(TEXT("ServerSubmitPlaceIntent")))
	{
		return;
	}
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	APawn* MyPawn = GetPawn();
	if (!Subsystem || !MyPawn)
	{
		return;
	}
	constexpr double MaxCameraPawnOffsetUU = 500.0;
	if (FVector::Dist(CameraLoc, MyPawn->GetActorLocation()) > MaxCameraPawnOffsetUU ||
	    FVector::Dist(PlayerActorLocation, MyPawn->GetActorLocation()) > MaxCameraPawnOffsetUU)
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("ServerSubmitPlaceIntent rejected: claimed camera/player location too far from owning pawn."));
		return;
	}
	if (SizeVoxels > UVoxelWorldSubsystem::MaxCubeSizeVoxels)
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("ServerSubmitPlaceIntent rejected: SizeVoxels=%d exceeds MaxCubeSizeVoxels=%d"), SizeVoxels,
		       UVoxelWorldSubsystem::MaxCubeSizeVoxels);
		return;
	}
	Subsystem->TryPlace(CameraLoc, CameraDir, SizeVoxels, MaterialId, PlayerActorLocation);
}

bool AVoxelEarthPlayerController::ServerSubmitCarveIntent_Validate(const FVector& CenterUU, float RadiusUU, float JitterUU)
{
	// Generous caps derived from AVoxelExplosive's tuning (VoxelExplosive.h:
	// BlastRadiusUU=300, BlastJitterUU=30) -- explosives are the only
	// current CarveSphere caller, thrown up to ~60m; a v1 sanity bound
	// (rejects garbage/negative/NaN-ish wire values), not the real game-rule
	// cap -- that (voxel.Server.MaxCarveRadiusUU, default 400) is enforced
	// (and logged) below.
	return RadiusUU > 0.f && RadiusUU <= 1000.f && FMath::Abs(JitterUU) <= 300.f;
}

void AVoxelEarthPlayerController::ServerSubmitCarveIntent_Implementation(const FVector& CenterUU, float RadiusUU, float JitterUU)
{
	if (!TryConsumeIntentToken(TEXT("ServerSubmitCarveIntent")))
	{
		return;
	}
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	APawn* MyPawn = GetPawn();
	if (!Subsystem || !MyPawn)
	{
		return;
	}
	constexpr double MaxThrowDistanceUU = 6000.0; // 60m -- generous over MaxThrowSpeedUU*FuseSeconds ballistic range
	if (FVector::Dist(CenterUU, MyPawn->GetActorLocation()) > MaxThrowDistanceUU)
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("ServerSubmitCarveIntent rejected: carve center too far from owning pawn."));
		return;
	}
	// M3 wave 2 "Validation hardening": carve radius <= voxel.Server.MaxCarveRadiusUU (default 400 UU).
	const float MaxRadiusUU = VoxelDebug::GetServerMaxCarveRadiusUU();
	if (RadiusUU > MaxRadiusUU)
	{
		UE_LOG(LogVoxelEdit, Warning, TEXT("ServerSubmitCarveIntent rejected: RadiusUU=%.1f exceeds voxel.Server.MaxCarveRadiusUU=%.1f"),
		       RadiusUU, MaxRadiusUU);
		return;
	}
	Subsystem->CarveSphere(CenterUU, (double)RadiusUU, (double)JitterUU);
}

void AVoxelEarthPlayerController::ServerRequestJoinSync_Implementation()
{
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Subsystem)
	{
		ClientReceiveJoinSyncChunk(TArray<uint8>(), true);
		return;
	}

	// M3 wave 2 "Join-sync compaction" (docs/m3-plan.md): send the COMPACTED
	// log instead of the raw full log -- fewer/smaller chunks for the exact
	// same replayed overlay state (see UVoxelWorldSubsystem::
	// SerializeCompactedLogEntries's doc comment). The server's own live log
	// (Impl->Voxels.log()) is never mutated by this.
	TArray<uint8> FullBytes;
	Subsystem->SerializeCompactedLogEntries(FullBytes);

	if (FullBytes.Num() == 0)
	{
		ClientReceiveJoinSyncChunk(FullBytes, true);
		UE_LOG(LogVoxelEdit, Log, TEXT("ServerRequestJoinSync: empty log, sent 0 bytes to %s"), *GetName());
		return;
	}

	// Chunked <= 48KB per RPC (m3-plan.md "Join sync"); reliable RPCs on one
	// actor channel are delivered in the order they were sent, so firing
	// these back-to-back is safe -- the client just concatenates until bFinal.
	constexpr int32 ChunkSizeBytes = 48 * 1024;
	int32 Offset = 0;
	while (Offset < FullBytes.Num())
	{
		const int32 ThisChunkSize = FMath::Min(ChunkSizeBytes, FullBytes.Num() - Offset);
		TArray<uint8> Chunk(FullBytes.GetData() + Offset, ThisChunkSize);
		Offset += ThisChunkSize;
		const bool bFinal = Offset >= FullBytes.Num();
		ClientReceiveJoinSyncChunk(Chunk, bFinal);
	}
	UE_LOG(LogVoxelEdit, Log, TEXT("ServerRequestJoinSync: sent %d bytes to %s"), FullBytes.Num(), *GetName());
}

void AVoxelEarthPlayerController::ClientReceiveJoinSyncChunk_Implementation(const TArray<uint8>& Bytes, bool bFinal)
{
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return;
	}
	Subsystem->ReceiveJoinSyncChunk(Bytes, bFinal);
}

void AVoxelEarthPlayerController::OnDig()
{
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return;
	}

	FVector CameraLocation;
	FRotator CameraRotation;
	GetPlayerViewPoint(CameraLocation, CameraRotation);

	Subsystem->TryDig(CameraLocation, CameraRotation.Vector(), DigSizeVoxels);
}

void AVoxelEarthPlayerController::OnPlace()
{
	UWorld* World = GetWorld();
	UVoxelWorldSubsystem* Subsystem = World ? World->GetSubsystem<UVoxelWorldSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return;
	}

	FVector CameraLocation;
	FRotator CameraRotation;
	GetPlayerViewPoint(CameraLocation, CameraRotation);

	const APawn* PlayerPawn = GetPawn();
	const FVector PlayerLocation = PlayerPawn ? PlayerPawn->GetActorLocation() : CameraLocation;

	Subsystem->TryPlace(CameraLocation, CameraRotation.Vector(), DigSizeVoxels, PaletteMaterialId, PlayerLocation);
}

void AVoxelEarthPlayerController::CycleDigSizeUp()
{
	// 1 -> 2 -> 4 -> 1 (m1-plan.md "Dig sizes" row).
	DigSizeVoxels = (DigSizeVoxels >= 4) ? 1 : DigSizeVoxels * 2;
}

void AVoxelEarthPlayerController::CycleDigSizeDown()
{
	DigSizeVoxels = (DigSizeVoxels <= 1) ? 4 : DigSizeVoxels / 2;
}

void AVoxelEarthPlayerController::SelectDigSize1() { DigSizeVoxels = 1; }
void AVoxelEarthPlayerController::SelectDigSize2() { DigSizeVoxels = 2; }
void AVoxelEarthPlayerController::SelectDigSize4() { DigSizeVoxels = 4; }

void AVoxelEarthPlayerController::CyclePaletteMaterial()
{
	// rock(2) -> soil/topsoil(6) -> sand(4) -> rock ... (m1-plan.md "Place"
	// row: "cycle material (rock/soil/sand)"). Numeric vxc::MaterialId
	// values (voxelcore/core.h) -- kept literal since this header/translation
	// unit intentionally avoids depending on the voxel-core enum type here.
	switch (PaletteMaterialId)
	{
	case 2: // MAT_ROCK -> MAT_TOPSOIL
		PaletteMaterialId = 6;
		break;
	case 6: // MAT_TOPSOIL -> MAT_SAND
		PaletteMaterialId = 4;
		break;
	default: // MAT_SAND (or anything unexpected) -> MAT_ROCK
		PaletteMaterialId = 2;
		break;
	}
}

void AVoxelEarthPlayerController::OnChargeStart()
{
	if (bChargingExplosive)
	{
		return;
	}
	bChargingExplosive = true;
	ChargeStartTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
}

void AVoxelEarthPlayerController::OnChargeRelease()
{
	if (!bChargingExplosive)
	{
		return;
	}
	bChargingExplosive = false;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 0.3s->1.5s hold maps to 600->1600 UU/s throw speed; a shorter tap still
	// throws at the minimum speed rather than being a no-op.
	const float HeldSeconds = World->GetTimeSeconds() - ChargeStartTimeSeconds;
	const float Alpha = FMath::Clamp((HeldSeconds - MinChargeSeconds) / (MaxChargeSeconds - MinChargeSeconds), 0.f, 1.f);
	const float ThrowSpeedUU = FMath::Lerp(MinThrowSpeedUU, MaxThrowSpeedUU, Alpha);

	FVector CameraLocation;
	FRotator CameraRotation;
	GetPlayerViewPoint(CameraLocation, CameraRotation);

	// 30-degree upward arc (m1-plan.md "Explosives v1" row).
	FRotator ThrowRotation = CameraRotation;
	ThrowRotation.Pitch = FMath::Clamp(CameraRotation.Pitch + ThrowUpwardArcDegrees, -89.f, 89.f);
	const FVector ThrowDirection = ThrowRotation.Vector();

	// Spawn a short distance in front of the camera so the explosive doesn't
	// immediately collide with the throwing pawn.
	const FVector ExplosiveSpawnLocation = CameraLocation + ThrowDirection * 80.0;

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.Instigator = GetPawn();
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	if (AVoxelExplosive* Explosive = World->SpawnActor<AVoxelExplosive>(AVoxelExplosive::StaticClass(), ExplosiveSpawnLocation, ThrowRotation, SpawnParams))
	{
		Explosive->Launch(ThrowDirection * ThrowSpeedUU);
	}
}

void AVoxelEarthPlayerController::OnCycleDebugMode()
{
	VoxelDebug::CycleDebugMode();
}

float AVoxelEarthPlayerController::GetExplosiveChargeAlpha() const
{
	if (!bChargingExplosive)
	{
		return 0.f;
	}
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.f;
	}
	const float HeldSeconds = World->GetTimeSeconds() - ChargeStartTimeSeconds;
	return FMath::Clamp(HeldSeconds / MaxChargeSeconds, 0.f, 1.f);
}
