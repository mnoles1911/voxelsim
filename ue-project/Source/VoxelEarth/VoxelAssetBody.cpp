#include "VoxelAssetBody.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/ConstructorHelpers.h"

#include "VoxelEarth.h" // LogVoxelEarth

#include "voxelcore/assetgrid.h"       // the read path -- AssetGrid::parse
#include "voxelcore/materialpalette.h" // the ONE array this file indexes by material

// ---------------------------------------------------------------------------
// CONSOLE VARIABLES
// ---------------------------------------------------------------------------
//
// All four are read at Build() time, not per frame: a vehicle's body is built
// once when it spawns, so a value that changes afterwards means nothing until
// the next spawn -- and that is exactly right for a spawn console command,
// which is how these are meant to be driven.

TAutoConsoleVariable<FString> CVarVoxelAssetBodyLibraryRoot(
	TEXT("voxel.AssetBody.LibraryRoot"), TEXT(""),
	TEXT("Directory asset-forge writes entity .vxa grids into. EMPTY means the default, which is ")
	TEXT("<ProjectDir>/../asset-forge/library -- the checkout layout, so an ordinary run needs ")
	TEXT("no argument at all. Set it to point a leg at a staging directory. The resolver tries ")
	TEXT("<root>/<name>/<name>-NNNN/tree.vxa (asset-forge's library layout), then ")
	TEXT("<root>/<name>/<name>-NNNN.vxa (export_banks.py's bank layout), then <root>/<name>.vxa, ")
	TEXT("and LOGS EVERY PATH IT TRIED when it finds none."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarVoxelAssetBodyMaxInstances(
	TEXT("voxel.AssetBody.MaxInstances"), 40000,
	TEXT("Ceiling on cube instances for ONE asset body. Above it the surface shell is strided down ")
	TEXT("uniformly -- never truncated, because a prefix renders the bottom slice of a hull and ")
	TEXT("reads as half the boat missing. A 4 m canoe at 25 mm is expected in the low thousands to ")
	TEXT("~15k, so hitting this is a sign the asset is not what was expected; the load line prints ")
	TEXT("both the shell size and the stride so the two are never confused."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelAssetBodyShellOnly(
	TEXT("voxel.AssetBody.ShellOnly"), true,
	TEXT("Draw only voxels with at least one exposed face. 0 draws the solid interior too, which is ")
	TEXT("visible in nothing and is here as the A arm for 'the hull looks hollow' -- if the shape ")
	TEXT("changes with this off, the shell filter is wrong; if it does not, the shape is the ")
	TEXT("asset's."),
	ECVF_Default);

TAutoConsoleVariable<bool> CVarVoxelAssetBodyCastShadow(
	TEXT("voxel.AssetBody.CastShadow"), true,
	TEXT("Whether asset bodies cast shadows. A few thousand cube instances in a shadow cascade is ")
	TEXT("not free and the terrain shadow pass is already this project's largest GPU item ")
	TEXT("(docs: terrain shadows cost 15.8 ms) -- this is the one-switch control arm for a boat ")
	TEXT("that turns out to cost more in the depth pass than in the base pass."),
	ECVF_Default);

namespace VoxelAssetBodyLocal
{
// NAMED, NOT ANONYMOUS, and prefixed. VoxelDetailAssetSubsystem.cpp has an
// internal-linkage `FPaletteLinear` doing the same decode, and this module is
// built as a unity blob: two internal-linkage definitions of one name in one
// blob is exactly the collision that stopped the module compiling on
// 2026-08-23 (VoxelRippleField.h records it). One distinct name each.
struct FAssetBodyPalette
{
	FLinearColor Flat[vxc::kMaterialCount];
	FAssetBodyPalette()
	{
		for (uint32 M = 0; M < uint32(vxc::kMaterialCount); ++M)
		{
			// The SIDE face class. A cube instance carries one colour for all six
			// faces, so a face-class choice has to be made; the side is the one a
			// hull, a wing and a spar are mostly seen from, and it is the class
			// most materials define identically to the other two anyway
			// (materialpalette.h: "Same value three times means the material looks
			// the same whichever way you hit it, which is true of most of them").
			const vxc::Rgb& C = vxc::kMaterialPalette[M].face[vxc::kFaceSide];
			// FLinearColor(FColor) is the exact sRGB decode -- the same conversion
			// the detail renderer uses, so a canoe and a fern agree about what
			// material 17 looks like.
			Flat[M] = FLinearColor(FColor(C.r, C.g, C.b));
		}
	}
};

const FAssetBodyPalette& Palette()
{
	static const FAssetBodyPalette P; // thread-safe magic-static init
	return P;
}

// Ceiling on the dense decode box. A vehicle is metres across at centimetre
// pitch; anything past this is not a vehicle and the allocation should be
// refused with a name rather than attempted.
constexpr int64 kMaxDenseCells = 8 * 1024 * 1024;

FString DefaultLibraryRoot()
{
	// <ProjectDir> is D:/voxelsim/ue-project/ ; asset-forge sits beside it at
	// D:/voxelsim/asset-forge/library. Collapsed so the log prints a path a
	// human can paste rather than one with ".." in the middle.
	FString Root = FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("asset-forge"), TEXT("library"));
	FPaths::CollapseRelativeDirectories(Root);
	FPaths::NormalizeDirectoryName(Root);
	return Root;
}
} // namespace VoxelAssetBodyLocal

UVoxelAssetBodyComponent::UVoxelAssetBodyComponent()
{
	// No tick. A static grid on a moving actor moves because its PARENT moves;
	// a tick here would be a second thing that could disagree about where the
	// boat is. Same discipline as UVoxelProxyBodyComponent.
	PrimaryComponentTick.bCanEverTick = false;
	SetMobility(EComponentMobility::Movable);

	// ConstructorHelpers::FObjectFinder is the supported way to load a default
	// asset from a UObject constructor, and /Engine/BasicShapes/Cube ships with
	// the engine -- so an asset body needs zero project content even before
	// asset-forge has produced anything.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	CubeMesh = CubeFinder.Succeeded() ? CubeFinder.Object : nullptr;
}

FString UVoxelAssetBodyComponent::ResolveVxaPath(const FString& Name, const FString& ExplicitPath,
                                                 TArray<FString>& OutTried)
{
	IFileManager& FM = IFileManager::Get();

	auto TryFile = [&](const FString& Path) -> bool
	{
		OutTried.Add(Path);
		return FM.FileExists(*Path);
	};

	// 1. The explicit path wins, and it is tried BOTH verbatim and relative to
	// the project, because "-VoxelBoatAsset=Content/foo.vxa" and an absolute
	// path are both things a leg will reasonably pass.
	if (!ExplicitPath.IsEmpty())
	{
		if (TryFile(ExplicitPath))
		{
			return ExplicitPath;
		}
		const FString Rel = FPaths::Combine(FPaths::ProjectDir(), ExplicitPath);
		if (TryFile(Rel))
		{
			return Rel;
		}
	}

	if (Name.IsEmpty())
	{
		return FString();
	}

	FString Root = CVarVoxelAssetBodyLibraryRoot.GetValueOnGameThread();
	if (Root.IsEmpty())
	{
		Root = VoxelAssetBodyLocal::DefaultLibraryRoot();
	}
	const FString SpeciesDir = FPaths::Combine(Root, Name);

	// 2. asset-forge's own library layout: <name>/<name>-NNNN/tree.vxa, one
	// directory per seed. SORTED filename order, which is the same rule
	// assetbank.h fixes for banks ("taken in SORTED FILENAME ORDER") -- so
	// "which seed did I get" has one answer across both readers rather than
	// whatever the filesystem felt like returning.
	{
		TArray<FString> SeedDirs;
		FM.FindFiles(SeedDirs, *(SpeciesDir / TEXT("*")), false, true);
		SeedDirs.Sort();
		for (const FString& Seed : SeedDirs)
		{
			const FString Candidate = FPaths::Combine(SpeciesDir, Seed, TEXT("tree.vxa"));
			if (TryFile(Candidate))
			{
				return Candidate;
			}
		}
	}

	// 3. export_banks.py's flattened bank layout: <name>/<name>-NNNN.vxa.
	{
		TArray<FString> Files;
		FM.FindFiles(Files, *(SpeciesDir / TEXT("*.vxa")), true, false);
		Files.Sort();
		for (const FString& File : Files)
		{
			const FString Candidate = FPaths::Combine(SpeciesDir, File);
			if (TryFile(Candidate))
			{
				return Candidate;
			}
		}
	}

	// 4. A single loose file.
	{
		const FString Candidate = FPaths::Combine(Root, Name + TEXT(".vxa"));
		if (TryFile(Candidate))
		{
			return Candidate;
		}
	}

	return FString();
}

void UVoxelAssetBodyComponent::ClearBatches()
{
	for (TObjectPtr<UInstancedStaticMeshComponent>& B : Batches)
	{
		if (B)
		{
			B->DestroyComponent();
		}
	}
	Batches.Reset();
	InstanceCount = 0;
	LocalBoundsUU = FBox(ForceInit);
}

void UVoxelAssetBodyComponent::EmitDense(const TArray<uint8>& Mat, int32 SX, int32 SY, int32 SZ,
                                         const FIntVector& OriginVoxels, double InPitchUU,
                                         const FLinearColor* FlatTint)
{
	PitchUU = InPitchUU;

	auto MatAt = [&](int32 X, int32 Y, int32 Z) -> uint8
	{
		if (X < 0 || Y < 0 || Z < 0 || X >= SX || Y >= SY || Z >= SZ)
		{
			return 0;
		}
		return Mat[int32((int64(X) * SY + Y) * SZ + Z)];
	};

	// --- surface shell ------------------------------------------------------
	//
	// AVoxelDebris::InitFromIsland's rule, and its reasoning transfers exactly:
	// a voxel whose six face neighbours are all solid cannot be seen from any
	// angle, so drawing it is pure cost. On a hull -- which is a shell already --
	// this removes almost nothing; on a solid-lofted wing it removes most of the
	// volume.
	const bool bShellOnly = CVarVoxelAssetBodyShellOnly.GetValueOnGameThread();
	TArray<FIntVector> Shell;
	TArray<uint8> ShellMat;
	int32 SolidCells = 0;
	for (int32 X = 0; X < SX; ++X)
	{
		for (int32 Y = 0; Y < SY; ++Y)
		{
			for (int32 Z = 0; Z < SZ; ++Z)
			{
				const uint8 M = MatAt(X, Y, Z);
				if (M == 0)
				{
					continue;
				}
				++SolidCells;
				if (bShellOnly && MatAt(X - 1, Y, Z) && MatAt(X + 1, Y, Z) && MatAt(X, Y - 1, Z)
				    && MatAt(X, Y + 1, Z) && MatAt(X, Y, Z - 1) && MatAt(X, Y, Z + 1))
				{
					continue;
				}
				Shell.Add(FIntVector(X, Y, Z));
				ShellMat.Add(M);
			}
		}
	}

	// Uniform stride, never a prefix. See the CVar's own text.
	const int32 Budget = FMath::Max(1, CVarVoxelAssetBodyMaxInstances.GetValueOnGameThread());
	const int32 Stride = (Shell.Num() > Budget) ? FMath::DivideAndRoundUp(Shell.Num(), Budget) : 1;

	// One ISM per material id actually present. See the header for why this is
	// not one ISM with per-instance colour.
	TMap<uint8, UInstancedStaticMeshComponent*> ByMaterial;
	const double HalfPitch = InPitchUU * 0.5;
	const double InstanceScale = InPitchUU / 100.0; // the engine cube is 100 UU

	for (int32 I = 0; I < Shell.Num(); I += Stride)
	{
		const FIntVector& C = Shell[I];
		const uint8 RawM = ShellMat[I];
		// CLAMP, do not refuse. See the header: this component indexes exactly
		// one material-keyed array and nothing it makes enters the world lattice.
		const uint8 M = uint8(FMath::Min<uint32>(RawM, uint32(vxc::kMaterialCount) - 1u));

		UInstancedStaticMeshComponent** Found = ByMaterial.Find(M);
		UInstancedStaticMeshComponent* Ism = Found ? *Found : nullptr;
		if (!Ism)
		{
			// Outered to the ACTOR, which is where a runtime component belongs, and
			// with a name made unique rather than fixed: Build() may run twice
			// (a rebuild, a re-spawned vehicle), and the previous batch's name is
			// still taken until GC collects it.
			UObject* Outer = GetOwner() ? static_cast<UObject*>(GetOwner()) : static_cast<UObject*>(this);
			const FName IsmName = MakeUniqueObjectName(
				Outer, UInstancedStaticMeshComponent::StaticClass(),
				FName(*FString::Printf(TEXT("AssetBodyISM_%u"), uint32(M))));
			Ism = NewObject<UInstancedStaticMeshComponent>(
				Outer, UInstancedStaticMeshComponent::StaticClass(), IsmName);
			Ism->SetupAttachment(this);
			Ism->RegisterComponent();
			Ism->SetMobility(EComponentMobility::Movable);
			if (CubeMesh)
			{
				Ism->SetStaticMesh(CubeMesh);
			}
			else
			{
				// InstanceCount still increments below, so without this line a
				// missing engine cube would read as a SUCCESSFUL load of an
				// invisible body -- thousands of instances, no mesh. The exact
				// silent-success class this file's header legislates against
				// (review finding #5).
				UE_LOG(LogVoxelEarth, Error,
				       TEXT("VoxelAssetBody '%s': /Engine/BasicShapes/Cube did not load -- "
				            "instances will be INVISIBLE despite a nonzero count."),
				       *GetName());
			}
			// Visual only. Terrain carries no Chaos collision in this project and
			// the vehicle's own physics body is the ONE body that may exist -- a
			// second collision source here would fight it (VoxelProxyBody's rule,
			// same words).
			Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Ism->SetCanEverAffectNavigation(false);
			Ism->SetCastShadow(CVarVoxelAssetBodyCastShadow.GetValueOnGameThread());

			const FLinearColor Tint = FlatTint ? *FlatTint : VoxelAssetBodyLocal::Palette().Flat[M];
			if (UMaterialInterface* BaseMat = Ism->GetMaterial(0))
			{
				if (UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMat, Ism))
				{
					// "Color" is VoxelProxyBody's parameter and carries its caveat
					// unchanged: a no-op if the resolved base material does not
					// expose one. The SHAPE is the thing being judged here (the
					// standing rule: judge shape, not colour), and the shape does
					// not depend on this line landing.
					MID->SetVectorParameterValue(TEXT("Color"), Tint);
					Ism->SetMaterial(0, MID);
				}
			}
			Batches.Add(Ism);
			ByMaterial.Add(M, Ism);
		}

		// Local placement. assetgrid.h fixes the convention: a voxel at local
		// (lx,ly,lz) sits at (origin + l) voxels from the asset's own anchor,
		// with the base at z = 0 and +z up. Half a pitch takes us from the
		// voxel's corner to its centre, which is where a cube instance goes.
		const FVector Rel((double(OriginVoxels.X + C.X) * InPitchUU) + HalfPitch,
		                  (double(OriginVoxels.Y + C.Y) * InPitchUU) + HalfPitch,
		                  (double(OriginVoxels.Z + C.Z) * InPitchUU) + HalfPitch);
		Ism->AddInstance(FTransform(FRotator::ZeroRotator, Rel, FVector(InstanceScale)));
		LocalBoundsUU += FBox(Rel - FVector(HalfPitch), Rel + FVector(HalfPitch));
		++InstanceCount;
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("VoxelAssetBody: %s pitch=%.1f mm solid=%d shell=%d instances=%d (stride %d) ")
	       TEXT("batches=%d local bounds X[%.1f..%.1f] Y[%.1f..%.1f] Z[%.1f..%.1f] UU%s"),
	       bPlaceholder ? TEXT("PLACEHOLDER") : *FPaths::GetCleanFilename(ResolvedPath), InPitchUU * 10.0,
	       SolidCells, Shell.Num(), InstanceCount, Stride, Batches.Num(), LocalBoundsUU.Min.X,
	       LocalBoundsUU.Max.X, LocalBoundsUU.Min.Y, LocalBoundsUU.Max.Y, LocalBoundsUU.Min.Z,
	       LocalBoundsUU.Max.Z, bShellOnly ? TEXT("") : TEXT(" (ShellOnly 0)"));

	// A ZERO IS A DIAGNOSIS, NOT A QUIET DAY. "The boat is invisible" splits
	// three ways -- no file, a file that decoded to air, and a file that decoded
	// and was strided to nothing -- and only this line separates them.
	if (InstanceCount == 0)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelAssetBody: ZERO instances from a %dx%dx%d box (%d solid cells). Nothing ")
		       TEXT("will render. A grid that parsed but holds no solid voxels is an asset-forge ")
		       TEXT("problem, not a renderer one -- the silent no-op trap on record."),
		       SX, SY, SZ, SolidCells);
	}
}

bool UVoxelAssetBodyComponent::Build()
{
	ClearBatches();
	ResolvedPath.Reset();
	bPlaceholder = true;

	TArray<FString> Tried;
	const FString Path = ResolveVxaPath(AssetName, AssetFilePath, Tried);

	if (!Path.IsEmpty())
	{
		TArray<uint8> Blob;
		if (!FFileHelper::LoadFileToArray(Blob, *Path))
		{
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("VoxelAssetBody '%s': %s exists but could not be read. Falling back to a ")
			       TEXT("placeholder."),
			       *AssetName, *Path);
		}
		else
		{
			vxc::AssetGrid Grid;
			const vxc::AssetParseError Err = Grid.parse(Blob.GetData(), size_t(Blob.Num()));
			if (Err != vxc::AssetParseError::kOk)
			{
				// REFUSED BY NAME AND REASON, assetgrid.h's own rule: "the only
				// thing worse than refusing an asset is refusing to say which
				// asset and why".
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("VoxelAssetBody '%s': %s REFUSED: %s. Falling back to a placeholder."),
				       *AssetName, *Path, UTF8_TO_TCHAR(vxc::assetParseErrorText(Err)));
			}
			else if (Grid.hasParts())
			{
				// The unrigged exclusion, stated rather than assumed. Only animals
				// carry rig parts; this component has no rig and would draw one in
				// its bind pose, which renders as SOMETHING -- the worst available
				// outcome (assetgrid.h's phrase, about a different failure with the
				// same shape).
				UE_LOG(LogVoxelEarth, Warning,
				       TEXT("VoxelAssetBody '%s': %s carries %d rig joints -- it is a RIGGED asset ")
				       TEXT("and this component draws only unrigged grids (no animation, D1 scope). ")
				       TEXT("Falling back to a placeholder."),
				       *AssetName, *Path, int32(Grid.joints().size()));
			}
			else
			{
				const int32 SX = Grid.sizeX(), SY = Grid.sizeY(), SZ = Grid.sizeZ();
				const int64 Cells = int64(SX) * int64(SY) * int64(SZ);
				if (Cells <= 0 || Cells > VoxelAssetBodyLocal::kMaxDenseCells)
				{
					UE_LOG(LogVoxelEarth, Warning,
					       TEXT("VoxelAssetBody '%s': %s is %dx%dx%d = %lld cells, past this ")
					       TEXT("component's %lld ceiling. That is not a vehicle. Falling back."),
					       *AssetName, *Path, SX, SY, SZ, (long long)Cells,
					       (long long)VoxelAssetBodyLocal::kMaxDenseCells);
				}
				else
				{
					// Dense decode, z fastest -- columnRuns' own delivery order, and
					// O(runs) per column rather than O(z*runs) through at().
					TArray<uint8> Mat;
					Mat.SetNumZeroed(int32(Cells));
					uint32 Beyond = 0;
					for (int32 X = 0; X < SX; ++X)
					{
						for (int32 Y = 0; Y < SY; ++Y)
						{
							const int64 Base = (int64(X) * SY + Y) * SZ;
							Grid.columnRuns(X, Y,
							                [&](int32 Z0, int32 Len, vxc::MaterialId M)
							                {
								                if (M == vxc::MAT_AIR)
								                {
									                return;
								                }
								                if (uint32(M) >= uint32(vxc::kMaterialCount))
								                {
									                Beyond += uint32(Len);
								                }
								                for (int32 Z = Z0; Z < Z0 + Len; ++Z)
								                {
									                Mat[int32(Base + Z)] = uint8(M);
								                }
							                });
						}
					}

					if (Beyond > 0)
					{
						// LOUD, AND WITH A NUMBER. assetgrid.h says every asset baked
						// so far carries ids past kMaterialCount and that the fix is
						// an engine enum append with three known tails. Until that
						// lands the vehicle is drawn in clamped colours and this line
						// is the record that it was.
						UE_LOG(LogVoxelEarth, Warning,
						       TEXT("VoxelAssetBody '%s': %u voxels carry a material id at or past ")
						       TEXT("vxc::kMaterialCount (%d) -- max id %d. Their COLOUR is clamped ")
						       TEXT("to the last palette entry; their SHAPE is exact. This is a ")
						       TEXT("presentation-only consumer (one palette lookup, nothing enters ")
						       TEXT("the world lattice), so it clamps where the bank loader refuses. ")
						       TEXT("The real fix is the engine material append."),
						       *AssetName, Beyond, int32(vxc::kMaterialCount),
						       int32(Grid.maxMaterialId()));
					}

					ResolvedPath = Path;
					bPlaceholder = false;
					// THE GRID'S OWN PITCH, never the world's. ADR-0010, and
					// assetgrid.h's "read it before placing anything".
					const double GridPitchUU = double(Grid.voxelSizeMm()) * 0.1;
					EmitDense(Mat, SX, SY, SZ,
					          FIntVector(Grid.originX(), Grid.originY(), Grid.originZ()), GridPitchUU,
					          nullptr);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("VoxelAssetBody '%s': LOADED %s (%dx%dx%d at %u mm, origin %d,%d,%d)."),
					       *AssetName, *Path, SX, SY, SZ, Grid.voxelSizeMm(), Grid.originX(),
					       Grid.originY(), Grid.originZ());
					return true;
				}
			}
		}
	}
	else
	{
		// NAME EVERY PATH. asset-forge writes on its own schedule into a
		// gitignored tree, so "not there yet" is the expected first state and the
		// only useful thing to print is where it was looked for.
		FString TriedList;
		for (const FString& T : Tried)
		{
			TriedList += TEXT("\n    ") + T;
		}
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("VoxelAssetBody '%s': NO .vxa FOUND. Rendering a placeholder of the same gross ")
		       TEXT("size at %d mm so the vehicle is still flyable/floatable and photographable. ")
		       TEXT("Set voxel.AssetBody.LibraryRoot, or the actor's AssetFilePath, to point at the ")
		       TEXT("real file. Tried:%s"),
		       *AssetName, FallbackPitchMm, TriedList.IsEmpty() ? TEXT(" (nothing -- no name and no path set)")
		                                                        : *TriedList);
	}

	// --- the placeholder ------------------------------------------------------
	//
	// Built from the same cubes at the same pitch and emitted through the same
	// EmitDense, so the shell filter, the stride budget, the batching and the
	// bounds are the ones the real asset will get. A fallback measured
	// differently from the asset would make every buoyancy constant tuned
	// against it wrong twice.
	{
		const int32 Pitch = FMath::Max(1, FallbackPitchMm);
		const double PitchUUFallback = double(Pitch) * 0.1;
		const double CellM = double(Pitch) * 0.001;
		const int32 NX = FMath::Clamp(FMath::RoundToInt(FallbackSizeM.X / CellM), 2, 4096);
		const int32 NY = FMath::Clamp(FMath::RoundToInt(FallbackSizeM.Y / CellM), 2, 4096);
		const int32 NZ = FMath::Clamp(FMath::RoundToInt(FallbackSizeM.Z / CellM), 2, 4096);
		const int64 Cells = int64(NX) * int64(NY) * int64(NZ);
		if (Cells <= 0 || Cells > VoxelAssetBodyLocal::kMaxDenseCells)
		{
			UE_LOG(LogVoxelEarth, Error,
			       TEXT("VoxelAssetBody '%s': placeholder of %dx%dx%d is past the %lld-cell ceiling; ")
			       TEXT("nothing will render. Reduce FallbackSizeM or coarsen FallbackPitchMm."),
			       *AssetName, NX, NY, NZ, (long long)VoxelAssetBodyLocal::kMaxDenseCells);
			return false;
		}

		TArray<uint8> Mat;
		Mat.SetNumZeroed(int32(Cells));
		auto Set = [&](int32 X, int32 Y, int32 Z)
		{
			if (X < 0 || Y < 0 || Z < 0 || X >= NX || Y >= NY || Z >= NZ)
			{
				return;
			}
			Mat[int32((int64(X) * NY + Y) * NZ + Z)] = 1;
		};

		switch (FallbackShape)
		{
		case EVoxelAssetBodyFallback::Hull:
		{
			// An open-topped box whose beam tapers to a point over the outer
			// fifth at each end. Not a canoe; recognisably canoe-SHAPED, which is
			// the whole job of a placeholder -- nobody must be able to mistake
			// this for the asset in a screenshot.
			const double HalfY = 0.5 * double(NY - 1);
			for (int32 X = 0; X < NX; ++X)
			{
				const double T = double(X) / double(FMath::Max(1, NX - 1));
				const double Taper = FMath::Min(1.0, FMath::Min(T, 1.0 - T) / 0.2);
				const int32 HW = FMath::Max(0, FMath::RoundToInt(HalfY * Taper));
				const int32 Y0 = FMath::RoundToInt(HalfY) - HW;
				const int32 Y1 = FMath::RoundToInt(HalfY) + HW;
				for (int32 Y = Y0; Y <= Y1; ++Y)
				{
					Set(X, Y, 0); // keel plate
					if (Y == Y0 || Y == Y1 || X == 0 || X == NX - 1)
					{
						for (int32 Z = 0; Z < NZ; ++Z)
						{
							Set(X, Y, Z); // gunwale walls, top left open
						}
					}
				}
			}
			break;
		}
		case EVoxelAssetBodyFallback::Wing:
		{
			// A flat plate two voxels thick spanning Y, a spar along X through
			// the centre, and a seat block under it. A glider from any angle.
			const int32 CY = NY / 2;
			const int32 PlateZ = NZ - 1;
			for (int32 X = 0; X < NX; ++X)
			{
				for (int32 Y = 0; Y < NY; ++Y)
				{
					Set(X, Y, PlateZ);
					Set(X, Y, FMath::Max(0, PlateZ - 1));
				}
			}
			for (int32 X = 0; X < NX; ++X)
			{
				for (int32 Z = 0; Z < NZ; ++Z)
				{
					Set(X, CY, Z);
				}
			}
			for (int32 X = NX / 3; X < (2 * NX) / 3; ++X)
			{
				for (int32 Y = CY - 1; Y <= CY + 1; ++Y)
				{
					Set(X, Y, 0);
				}
			}
			break;
		}
		case EVoxelAssetBodyFallback::Box:
		default:
			for (int32 X = 0; X < NX; ++X)
			{
				for (int32 Y = 0; Y < NY; ++Y)
				{
					for (int32 Z = 0; Z < NZ; ++Z)
					{
						if (X == 0 || Y == 0 || Z == 0 || X == NX - 1 || Y == NY - 1 || Z == NZ - 1)
						{
							Set(X, Y, Z);
						}
					}
				}
			}
			break;
		}

		// Centred in XY with the base at local Z = 0, which is asset-forge's own
		// convention ("+z up, base at (0,0,0)") -- so a vehicle's probe geometry
		// reads the same numbers from a placeholder and from the asset.
		const FIntVector Origin(-NX / 2, -NY / 2, 0);
		EmitDense(Mat, NX, NY, NZ, Origin, PitchUUFallback, &PlaceholderTint);
	}
	return false;
}
