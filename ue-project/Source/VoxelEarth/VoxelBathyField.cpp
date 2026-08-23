#include "VoxelBathyField.h"

#include "VoxelSkySubsystem.h" // VoxelSky::kSkyCollectionPath

#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "PixelFormat.h"
#include "RenderUtils.h"
#include "UObject/ConstructorHelpers.h"

#include "VoxelDebug.h"           // LogVoxelWater
#include "VoxelFineTileStreamer.h" // pulls voxelcore/tilestore.h -- NOT UHT-parsed, so this is legal
#include "VoxelWorldSubsystem.h"

namespace
{
// The one asset this subsystem writes to. Authored by
// ue-project/Tools/create_bathy_info_texture.py, which is also where the
// size/format contract the guard below enforces is written down.
const TCHAR* kBathyTexturePath = TEXT("/Game/Voxel/T_VoxelBathyInfo.T_VoxelBathyInfo");

// The project's existing CPU->material channel. UVoxelSkySubsystem drives most
// of it every frame; we add three parameters and drive them only when the
// window moves. Authored (and DELETED AND RECREATED) by
// ue-project/Tools/create_sky_material.py -- if a name below is not in that
// script's SCALAR_PARAMS/VECTOR_PARAMS, the Set* call logs a warning and does
// nothing, which is the silent-no-op trap that script's comments describe.
// The collection's path is VoxelSky::kSkyCollectionPath (VoxelSkySubsystem.h)
// -- one definition for the whole module; per-file copies collided in a unity
// blob of the game target.

const TCHAR* kBathyParamOrigin = TEXT("BathyFieldOrigin");
const TCHAR* kBathyParamInvSize = TEXT("BathyFieldInvSize");
const TCHAR* kParamValid = TEXT("BathyFieldValid");

// Saturation of the baked shore plane, in metres (vxc::kBathyShoreClampMm).
// Reproduced here as a float because it is also the value we write into HOLES:
// a cell we could not read must read as "at least 100 m of dry land from any
// water", which draws nothing, rather than as 0 -- which is the waterline, and
// is where every shore effect is at FULL strength. A zero-filled hole would be
// a square of foam.
constexpr float kShoreClampM = 100.0f;
} // namespace

void UVoxelBathyFieldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Ordering, not a convenience: the streamer we read lives inside
	// UVoxelWorldSubsystem's Impl, and asking the collection for it here is what
	// makes "it exists by the time we Tick" a fact rather than a hope.
	Collection.InitializeDependency(UVoxelWorldSubsystem::StaticClass());

	InfoTexture_ = LoadObject<UTexture2D>(nullptr, kBathyTexturePath);
	if (!InfoTexture_)
	{
		UE_LOG(LogVoxelWater, Warning,
		       TEXT("BathyField: %s is missing, so no baked bathymetry will reach any material this run. ")
		       TEXT("Every lake falls back to the screen-space depth path, which is view-dependent and has no ")
		       TEXT("shoreline distance at all. Run ue-project/Tools/create_bathy_info_texture.py."),
		       kBathyTexturePath);
		PublishInvalid();
		return;
	}

	// THE GUARD. We write 8-byte half-float RGBA pixels straight into this
	// texture's RHI allocation, so every one of these has to be true before a
	// single byte moves. A mismatch here is not a cosmetic bug: writing
	// FFloat16Color into a 4-bytes-per-pixel texture walks off the end of the
	// mip. Refusing is always the right answer -- the material's fallback path
	// exists precisely so that "no field" is a supported state.
	const int32 SizeX = InfoTexture_->GetSizeX();
	const int32 SizeY = InfoTexture_->GetSizeY();
	const EPixelFormat Format = InfoTexture_->GetPixelFormat();
	const int32 NumMips = InfoTexture_->GetPlatformData() ? InfoTexture_->GetPlatformData()->Mips.Num() : 0;
	if (SizeX != kSize || SizeY != kSize || Format != PF_FloatRGBA || NumMips != 1)
	{
		UE_LOG(LogVoxelWater, Error,
		       TEXT("BathyField: %s is %dx%d %s with %d mip(s); this subsystem writes %dx%d PF_FloatRGBA with ")
		       TEXT("exactly 1 mip and will NOT write anything else. Baked bathymetry is disabled for this run ")
		       TEXT("and every water surface falls back to screen-space depth. Re-run ")
		       TEXT("ue-project/Tools/create_bathy_info_texture.py -- a mipped texture in particular is a silent ")
		       TEXT("failure otherwise, because distant water would sample a mip nothing ever writes."),
		       kBathyTexturePath, SizeX, SizeY, GetPixelFormatString(Format), NumMips, kSize, kSize);
		InfoTexture_ = nullptr;
		PublishInvalid();
		return;
	}

	DepthUnits_.SetNumUninitialized(kSize * kSize);
	ShoreUnits_.SetNumUninitialized(kSize * kSize);
	Pixels_.SetNumUninitialized(kSize * kSize);
	bArmed_ = true;

	// Published as invalid until the first window actually lands. The material
	// must never read a texture whose contents predate this run.
	PublishInvalid();
	UE_LOG(LogVoxelWater, Log,
	       TEXT("BathyField: armed. %dx%d texels at %.3f m -> a %.0f m window, refilled every %.0f m of camera ")
	       TEXT("travel."),
	       kSize, kSize, kTexelUU / 100.0, kSize * kTexelUU / 100.0,
	       kRecentreFraction * kSize * kTexelUU / 100.0);
}

void UVoxelBathyFieldSubsystem::Deinitialize()
{
	// A window published over a world that is going away is worse than none: the
	// texture asset outlives this subsystem (it is a /Game asset, not transient),
	// so without this the next world would start with the previous world's
	// pixels and a stale origin until its first refill.
	PublishInvalid();
	bArmed_ = false;
	bPublished_ = false;
	InfoTexture_ = nullptr;
	DepthUnits_.Empty();
	ShoreUnits_.Empty();
	Pixels_.Empty();
	Super::Deinitialize();
}

bool UVoxelBathyFieldSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Game and PIE only. An editor-preview or inactive world has no camera to
	// follow and no streamer to read, and publishing into the shared asset from
	// one would fight with the game world that is also publishing into it.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UVoxelBathyFieldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UVoxelBathyFieldSubsystem, STATGROUP_Tickables);
}

bool UVoxelBathyFieldSubsystem::GetCameraXY(double& OutX, double& OutY) const
{
	// The same anchor UVoxelWorldSubsystem::Tick streams against -- the first
	// local player's possessed pawn. Using the same one is the point: a window
	// centred somewhere the streamer is not keeping resident would be all holes.
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return false;
	}
	const FVector Loc = Pawn->GetActorLocation();
	OutX = Loc.X;
	OutY = Loc.Y;
	return true;
}

void UVoxelBathyFieldSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bArmed_)
	{
		return;
	}

	double CamX = 0.0, CamY = 0.0;
	if (!GetCameraXY(CamX, CamY))
	{
		return; // no pawn yet; nothing to centre on and nothing streamed either
	}

	// Camera position in FINE PIXELS, then the window's minimum corner, then the
	// snap. floor rather than truncate: pixel indices run negative and a
	// truncating divide mirrors them across the origin, which is the aliasing
	// vxc's floorDiv routing exists to avoid.
	const int64 CamPx = static_cast<int64>(FMath::FloorToDouble(CamX / kTexelUU));
	const int64 CamPy = static_cast<int64>(FMath::FloorToDouble(CamY / kTexelUU));
	auto SnapDown = [](int64 V) -> int64
	{
		const int64 Q = static_cast<int64>(kSnapTexels);
		const int64 R = ((V % Q) + Q) % Q; // floorMod: negatives snap DOWN, not toward zero
		return V - R;
	};
	const int64 WantPx = SnapDown(CamPx - kSize / 2);
	const int64 WantPy = SnapDown(CamPy - kSize / 2);

	if (bPublished_)
	{
		// Has the camera left the central band of the published window? Measured
		// in texels against the window we ACTUALLY published, not against the
		// window we would choose now -- otherwise the snap quantisation alone
		// would trigger a refill every time it rounded the other way.
		const double SlackTexels = kRecentreFraction * 0.5 * static_cast<double>(kSize);
		const double CentrePx = static_cast<double>(OriginPx_) + 0.5 * kSize;
		const double CentrePy = static_cast<double>(OriginPy_) + 0.5 * kSize;
		const double DriftPx = FMath::Abs(static_cast<double>(CamPx) - CentrePx);
		const double DriftPy = FMath::Abs(static_cast<double>(CamPy) - CentrePy);
		if (DriftPx <= SlackTexels && DriftPy <= SlackTexels)
		{
			return; // still well inside the published window
		}
		if (WantPx == OriginPx_ && WantPy == OriginPy_)
		{
			return; // drifted, but the snap puts the window back where it already is
		}
	}

	const double T0 = FPlatformTime::Seconds();
	LastHoleFraction_ = FillWindow(WantPx, WantPy);
	PublishWindow(WantPx, WantPy);
	LastFillMs_ = (FPlatformTime::Seconds() - T0) * 1000.0;
	++PublishedWindows_;

	// One line per refill, not per frame -- at 120 m of travel per refill this is
	// rare enough to be readable in a run log and is the only place the hole
	// fraction is visible. A hole fraction pinned at 1.0 with lakes on screen is
	// the diagnosis: either there is no fine tier in this run, or the world was
	// baked before bake_ver 27.
	UE_LOG(LogVoxelWater, Verbose,
	       TEXT("BathyField: window #%llu at px=(%lld,%lld) origin=(%.0f,%.0f)uu holes=%.1f%% fill=%.2fms"),
	       static_cast<unsigned long long>(PublishedWindows_), static_cast<long long>(OriginPx_),
	       static_cast<long long>(OriginPy_), static_cast<double>(OriginPx_) * kTexelUU,
	       static_cast<double>(OriginPy_) * kTexelUU, LastHoleFraction_ * 100.0, LastFillMs_);
}

double UVoxelBathyFieldSubsystem::FillWindow(int64 Px0, int64 Py0)
{
	const int32 Cells = kSize * kSize;

	// Read the two planes in ONE call over the whole window. sampleBathyRect
	// decodes each covered source block exactly once (four blocks per plane at
	// kSize 512) and copies the intersecting sub-rectangle out -- which is the
	// entire reason this is affordable on the game thread. A per-texel query
	// would decode the same 256x256 block a quarter of a million times.
	vxc::BathyRectStats Stats;
	Stats.cells = static_cast<uint64>(Cells);
	FVoxelFineTileStreamer* Streamer = nullptr;
	if (const UWorld* World = GetWorld())
	{
		if (const UVoxelWorldSubsystem* WorldSub = World->GetSubsystem<UVoxelWorldSubsystem>())
		{
			Streamer = WorldSub->GetFineTileStreamer();
		}
	}

	if (Streamer)
	{
		Stats = Streamer->ReadBathyRect(Px0, Py0, Px0 + kSize - 1, Py0 + kSize - 1, DepthUnits_.GetData(),
		                                ShoreUnits_.GetData(), kSize);
	}
	else
	{
		// No fine tier in this run at all -- the synthetic/coarse tile source
		// carries no bathymetry. Every cell is a hole, said once.
		for (int32 i = 0; i < Cells; ++i)
		{
			DepthUnits_[i] = vxc::kBathyMissing;
			ShoreUnits_[i] = vxc::kBathyMissing;
		}
		Stats.missingTiles = static_cast<uint64>(Cells);
		if (!bLoggedNoStreamer_)
		{
			bLoggedNoStreamer_ = true;
			UE_LOG(LogVoxelWater, Log,
			       TEXT("BathyField: this run has no fine-tile streamer (-VoxelFineTileDir= absent), so there ")
			       TEXT("is no baked bathymetry to publish. Water falls back to screen-space depth. Not an ")
			       TEXT("error -- but if you expected depth-graded lakes, this is why they are flat."));
		}
	}

	// Pack to the wire the material reads. The conversions live HERE and only
	// here: nothing downstream multiplies by 10 or 100, and the shader sees
	// metres in both channels.
	for (int32 i = 0; i < Cells; ++i)
	{
		const int16 D = DepthUnits_[i];
		const int16 S = ShoreUnits_[i];
		FFloat16Color& Out = Pixels_[i];
		if (D == vxc::kBathyMissing || S == vxc::kBathyMissing)
		{
			// A HOLE, and it must be shaded as dry land a long way from water --
			// see kShoreClampM above. Validity 0 is what tells the material to
			// use its fallback; the other two channels exist so that a BILINEAR
			// tap straddling the edge of a hole still degrades toward "nothing
			// here" rather than toward "waterline here".
			Out.R = FFloat16(0.0f);
			Out.G = FFloat16(-kShoreClampM);
			Out.B = FFloat16(0.0f);
			Out.A = FFloat16(0.0f);
			continue;
		}
		// vxc::bathyDepthIsDry, not `D <= 0`: a stored depth of exactly 0 is WET
		// at exactly the bed, and the extent's outermost ring quantises there.
		// Treating it as dry punches a one-pixel dry ring around every lake --
		// exactly at the shoreline, where it is most visible.
		const float DepthM = vxc::bathyDepthIsDry(D) ? 0.0f
		                                             : static_cast<float>(vxc::bathyDepthMm(D)) * 0.001f;
		const float ShoreM = static_cast<float>(vxc::bathyShoreMm(S)) * 0.001f;
		Out.R = FFloat16(DepthM);
		Out.G = FFloat16(ShoreM);
		Out.B = FFloat16(1.0f);
		Out.A = FFloat16(0.0f);
	}

	const uint64 Holes = Stats.cells - Stats.filled;
	return Stats.cells ? static_cast<double>(Holes) / static_cast<double>(Stats.cells) : 1.0;
}

void UVoxelBathyFieldSubsystem::PublishWindow(int64 Px0, int64 Py0)
{
	check(InfoTexture_);
	OriginPx_ = Px0;
	OriginPy_ = Py0;

	// ONE REGION, THE WHOLE MIP. The source buffer must outlive the render
	// command, so it is copied; the cleanup lambda frees both it and the region.
	// (Pixels_ itself must not be handed over -- the next refill overwrites it,
	// and the render thread may not have consumed the last one yet.)
	const int64 ByteCount = static_cast<int64>(kSize) * kSize * static_cast<int64>(sizeof(FFloat16Color));
	uint8* Copy = static_cast<uint8*>(FMemory::Malloc(static_cast<SIZE_T>(ByteCount)));
	FMemory::Memcpy(Copy, Pixels_.GetData(), static_cast<SIZE_T>(ByteCount));
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, kSize, kSize);
	InfoTexture_->UpdateTextureRegions(
		/*MipIndex=*/0, /*NumRegions=*/1, Region,
		/*SrcPitch=*/static_cast<uint32>(kSize * sizeof(FFloat16Color)),
		/*SrcBpp=*/static_cast<uint32>(sizeof(FFloat16Color)), Copy,
		[](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
		{
			FMemory::Free(SrcData);
			delete Regions;
		});

	// SAME TICK AS THE UPLOAD. See the header: the pixels and the origin they
	// are relative to must never be a frame apart, and the only way to guarantee
	// that is to enqueue the render command and write the collection in the same
	// game-thread call.
	if (UWorld* World = GetWorld())
	{
		if (UMaterialParameterCollection* Sky =
		        LoadObject<UMaterialParameterCollection>(nullptr, VoxelSky::kSkyCollectionPath))
		{
			const double OriginXUU = static_cast<double>(OriginPx_) * kTexelUU;
			const double OriginYUU = static_cast<double>(OriginPy_) * kTexelUU;
			UKismetMaterialLibrary::SetVectorParameterValue(
				World, Sky, kBathyParamOrigin,
				FLinearColor(static_cast<float>(OriginXUU), static_cast<float>(OriginYUU), 0.0f, 0.0f));
			UKismetMaterialLibrary::SetScalarParameterValue(
				World, Sky, kBathyParamInvSize, static_cast<float>(1.0 / (kSize * kTexelUU)));
			UKismetMaterialLibrary::SetScalarParameterValue(World, Sky, kParamValid, 1.0f);
		}
	}
	bPublished_ = true;
}

void UVoxelBathyFieldSubsystem::PublishInvalid()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (UMaterialParameterCollection* Sky =
	        LoadObject<UMaterialParameterCollection>(nullptr, VoxelSky::kSkyCollectionPath))
	{
		UKismetMaterialLibrary::SetScalarParameterValue(World, Sky, kParamValid, 0.0f);
	}
	bPublished_ = false;
}
