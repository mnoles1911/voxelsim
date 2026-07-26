#include "VoxelLightField.h"

#include "VoxelCoords.h"

#include "Async/ParallelFor.h"
#include "Misc/ScopeRWLock.h"

namespace VoxelLF
{
	const FVector3f DirTable[NumDirs] = {
		FVector3f(1, 0, 0), FVector3f(-1, 0, 0),
		FVector3f(0, 1, 0), FVector3f(0, -1, 0),
		FVector3f(0, 0, 1), FVector3f(0, 0, -1),
	};

	namespace
	{
		constexpr float kInvSqrt3 = 0.57735026f;
	}

	// 6 axes + 8 cube diagonals. The diagonals are what carry side light: for a
	// +Z slot they sit 54.7 degrees off the normal, so a floor lit only from
	// the side now gathers real energy instead of the exact zero the
	// axis-only basis gave it.
	const FVector3f TraceDirTable[NumTraceDirs] = {
		FVector3f(1, 0, 0), FVector3f(-1, 0, 0),
		FVector3f(0, 1, 0), FVector3f(0, -1, 0),
		FVector3f(0, 0, 1), FVector3f(0, 0, -1),
		FVector3f(kInvSqrt3, kInvSqrt3, kInvSqrt3),
		FVector3f(kInvSqrt3, kInvSqrt3, -kInvSqrt3),
		FVector3f(kInvSqrt3, -kInvSqrt3, kInvSqrt3),
		FVector3f(kInvSqrt3, -kInvSqrt3, -kInvSqrt3),
		FVector3f(-kInvSqrt3, kInvSqrt3, kInvSqrt3),
		FVector3f(-kInvSqrt3, kInvSqrt3, -kInvSqrt3),
		FVector3f(-kInvSqrt3, -kInvSqrt3, kInvSqrt3),
		FVector3f(-kInvSqrt3, -kInvSqrt3, -kInvSqrt3),
	};

	// Cosine weights, normalized per slot. Every slot gets its own axis at
	// cos 0 = 1 plus the four diagonals of its hemisphere at cos 54.7 =
	// 1/sqrt(3); the two perpendicular axes and the four opposite diagonals get
	// exactly 0, which is the correct cosine response at and below the horizon.
	// Row sum 1 + 4/sqrt(3) = 3.3094, so each row here is that normalized --
	// which is what makes an unoccluded cell solve to exactly 1.0.
	namespace
	{
		constexpr float kRowNorm = 1.f / (1.f + 4.f * kInvSqrt3); // 0.302169
		constexpr float kAxisW = kRowNorm;                        // 0.302169
		constexpr float kDiagW = kInvSqrt3 * kRowNorm;            // 0.174457
		constexpr float kZ = 0.f;
	}

	// Column order matches TraceDirTable: +X -X +Y -Y +Z -Z then the 8
	// diagonals in (+++ ++- +-+ +-- -++ -+- --+ ---) order.
	const float SlotWeight[NumDirs][NumTraceDirs] = {
		// slot +X : diagonals with +x -> indices 6,7,8,9
		{kAxisW, kZ, kZ, kZ, kZ, kZ, kDiagW, kDiagW, kDiagW, kDiagW, kZ, kZ, kZ, kZ},
		// slot -X : diagonals with -x -> indices 10,11,12,13
		{kZ, kAxisW, kZ, kZ, kZ, kZ, kZ, kZ, kZ, kZ, kDiagW, kDiagW, kDiagW, kDiagW},
		// slot +Y : diagonals with +y -> 6,7,10,11
		{kZ, kZ, kAxisW, kZ, kZ, kZ, kDiagW, kDiagW, kZ, kZ, kDiagW, kDiagW, kZ, kZ},
		// slot -Y : diagonals with -y -> 8,9,12,13
		{kZ, kZ, kZ, kAxisW, kZ, kZ, kZ, kZ, kDiagW, kDiagW, kZ, kZ, kDiagW, kDiagW},
		// slot +Z : diagonals with +z -> 6,8,10,12
		{kZ, kZ, kZ, kZ, kAxisW, kZ, kDiagW, kZ, kDiagW, kZ, kDiagW, kZ, kDiagW, kZ},
		// slot -Z : diagonals with -z -> 7,9,11,13
		{kZ, kZ, kZ, kZ, kZ, kAxisW, kZ, kDiagW, kZ, kDiagW, kZ, kDiagW, kZ, kDiagW},
	};
}

namespace
{
	// Floor division that is correct for negative coordinates. The world spans
	// +/-2e8 UU, so half of it is negative and C++ truncation-toward-zero would
	// put a seam of mis-keyed bricks right through the origin quadrant
	// boundaries.
	FORCEINLINE int64 FloorDiv(int64 A, int64 B)
	{
		const int64 Q = A / B;
		return (A % B != 0 && ((A < 0) != (B < 0))) ? Q - 1 : Q;
	}

	FORCEINLINE FIntVector FloorDivVec(const FIntVector& V, int32 B)
	{
		return FIntVector(int32(FloorDiv(V.X, B)), int32(FloorDiv(V.Y, B)), int32(FloorDiv(V.Z, B)));
	}

	// World UU -> integer cell/brick lattice. Done in double then floored;
	// |WorldUU| <= ~2e8 and doubles carry 53 bits, so this is exact well past
	// the 16 km streaming radius and on out to the full Earth-scale world.
	FORCEINLINE int32 FloorDivWorld(double WorldUU, int32 SizeUU)
	{
		return int32(FMath::FloorToDouble(WorldUU / double(SizeUU)));
	}
}

FIntVector FVoxelLightField::WorldToBrick(const FVector& WorldUU)
{
	return FIntVector(
		FloorDivWorld(WorldUU.X, VoxelLF::BrickEdgeUU),
		FloorDivWorld(WorldUU.Y, VoxelLF::BrickEdgeUU),
		FloorDivWorld(WorldUU.Z, VoxelLF::BrickEdgeUU));
}

const FVoxelLFBrick* FVoxelLightField::FindBrick(const FIntVector& Key) const
{
	const TUniquePtr<FVoxelLFBrick>* Found = Bricks.Find(Key);
	return Found ? Found->Get() : nullptr;
}

int32 FVoxelLightField::NumBricks() const
{
	FRWScopeLock ScopeLock(Lock, SLT_ReadOnly);
	return Bricks.Num();
}

bool FVoxelLightField::HasBrick(const FIntVector& Key) const
{
	FRWScopeLock ScopeLock(Lock, SLT_ReadOnly);
	return Bricks.Contains(Key);
}

SIZE_T FVoxelLightField::EstimatedBytes() const
{
	FRWScopeLock ScopeLock(Lock, SLT_ReadOnly);
	SIZE_T Total = Bricks.Num() * (sizeof(FVoxelLFBrick) + VoxelLF::BrickCells / 8);
	for (int32 I = 0; I < VoxelLF::NumCoarseLevels; ++I)
	{
		Total += Coarse[I].Num() * (sizeof(FIntVector) + sizeof(uint8) + 8);
	}
	return Total;
}

void FVoxelLightField::GetResidentKeys(TArray<FIntVector>& Out) const
{
	FRWScopeLock ScopeLock(Lock, SLT_ReadOnly);
	Out.Reset(Bricks.Num());
	for (const TPair<FIntVector, TUniquePtr<FVoxelLFBrick>>& Pair : Bricks)
	{
		Out.Add(Pair.Key);
	}
}

void FVoxelLightField::Reset()
{
	FRWScopeLock ScopeLock(Lock, SLT_Write);
	Bricks.Reset();
	for (int32 I = 0; I < VoxelLF::NumCoarseLevels; ++I)
	{
		Coarse[I].Reset();
	}
}

// --- voxelization ----------------------------------------------------------

uint32 FVoxelLightField::VoxelizeChunk(const FIntVector& BrickCoord, const FVector& ChunkOriginUU,
                                       const TArray<FVoxelChunkQuad>& Quads)
{
	FRWScopeLock ScopeLock(Lock, SLT_Write);

	TUniquePtr<FVoxelLFBrick>& Slot = Bricks.FindOrAdd(BrickCoord);
	if (!Slot)
	{
		Slot = MakeUnique<FVoxelLFBrick>();
	}
	FVoxelLFBrick& Brick = *Slot;

	// Clear-then-fill. One brick == one chunk, so this is always a complete
	// rewrite of that chunk's contribution -- never a partial overwrite, which
	// is exactly why edits (which arrive as a full chunk remesh) need no
	// differential bookkeeping at all.
	FMemory::Memzero(Brick.Opacity, sizeof(Brick.Opacity));
	Brick.SolvedCells.Init(false, VoxelLF::BrickCells);
	FMemory::Memzero(Brick.Vis, sizeof(Brick.Vis));
	FMemory::Memzero(Brick.AvgIrr, sizeof(Brick.AvgIrr));
	// Both halves of the Jacobi pair, or the re-voxelized brick would keep
	// bouncing light off geometry that no longer exists (visible as a dug
	// tunnel staying lit for a few passes after the rock came out).
	FMemory::Memzero(Brick.PrevAvgIrr, sizeof(Brick.PrevAvgIrr));
	Brick.bSolved = false;

	// The subsystem derives BrickCoord from the component location, so this
	// should be exactly zero; computed anyway so a chunk whose component was
	// placed off-lattice degrades into a clamped write rather than corrupting
	// a neighbour.
	const FVector BrickOriginUU(double(BrickCoord.X) * VoxelLF::BrickEdgeUU,
	                            double(BrickCoord.Y) * VoxelLF::BrickEdgeUU,
	                            double(BrickCoord.Z) * VoxelLF::BrickEdgeUU);
	const FVector OffsetUU = ChunkOriginUU - BrickOriginUU;

	const int32 VoxUU = VoxelCoords::VoxelSizeUU; // level-0 only feeds the field

	for (const FVoxelChunkQuad& Q : Quads)
	{
		const int32 Axis = Q.Axis;
		const int32 U = (Axis + 1) % 3;
		const int32 V = (Axis + 2) % 3;

		// The face plane sits at Slice (+1 for a positive face); the SOLID
		// voxel is always half a voxel back along the face normal, and -- for
		// a greedy mesh of this chunk's own voxels -- is always inside this
		// chunk, which is why every write below lands in this brick.
		const double FaceUU = double(Q.Slice + (Q.Positive ? 1 : 0)) * VoxUU;
		const double SolidAxisUU = OffsetUU[Axis] + FaceUU + (Q.Positive ? -0.5 : 0.5) * VoxUU;

		const int32 AxisCell = FMath::Clamp(int32(FMath::FloorToDouble(SolidAxisUU / VoxelLF::CellSizeUU)),
		                                    0, VoxelLF::BrickEdgeCells - 1);

		const double U0UU = OffsetUU[U] + double(Q.U0) * VoxUU;
		const double U1UU = OffsetUU[U] + double(Q.U0 + Q.W) * VoxUU;
		const double V0UU = OffsetUU[V] + double(Q.V0) * VoxUU;
		const double V1UU = OffsetUU[V] + double(Q.V0 + Q.H) * VoxUU;

		const int32 UStart = FMath::Clamp(int32(FMath::FloorToDouble(U0UU / VoxelLF::CellSizeUU)), 0, VoxelLF::BrickEdgeCells - 1);
		const int32 UEnd = FMath::Clamp(int32(FMath::FloorToDouble((U1UU - 0.001) / VoxelLF::CellSizeUU)), 0, VoxelLF::BrickEdgeCells - 1);
		const int32 VStart = FMath::Clamp(int32(FMath::FloorToDouble(V0UU / VoxelLF::CellSizeUU)), 0, VoxelLF::BrickEdgeCells - 1);
		const int32 VEnd = FMath::Clamp(int32(FMath::FloorToDouble((V1UU - 0.001) / VoxelLF::CellSizeUU)), 0, VoxelLF::BrickEdgeCells - 1);

		// A greedy quad covers at most 32x32 voxels == 8x8 cells, so this inner
		// loop is bounded at 64 iterations and is typically 1-4. Stepping in
		// CELL strides rather than voxel strides is what keeps voxelization off
		// the hitch budget: a per-voxel loop would be 1024x this.
		int32 Coord[3];
		Coord[Axis] = AxisCell;
		for (int32 Ui = UStart; Ui <= UEnd; ++Ui)
		{
			Coord[U] = Ui;
			for (int32 Vi = VStart; Vi <= VEnd; ++Vi)
			{
				Coord[V] = Vi;
				Brick.Opacity[VoxelLF::CellIndex(Coord[0], Coord[1], Coord[2])] = 255;
			}
		}
	}

	Brick.bHasGeometry = Quads.Num() > 0;
	BuildBrickMips(Brick);
	Brick.GeometrySerial = NextGeometrySerial++;
	return Brick.GeometrySerial;
}

void FVoxelLightField::BuildBrickMips(FVoxelLFBrick& Brick) const
{
	// MAX aggregation, deliberately -- see the leaking note in the header.
	FMemory::Memzero(Brick.Mip1, sizeof(Brick.Mip1));
	FMemory::Memzero(Brick.Mip2, sizeof(Brick.Mip2));
	Brick.Mip3 = 0;

	for (int32 Z = 0; Z < 8; ++Z)
	{
		for (int32 Y = 0; Y < 8; ++Y)
		{
			for (int32 X = 0; X < 8; ++X)
			{
				const uint8 A = Brick.Opacity[VoxelLF::CellIndex(X, Y, Z)];
				if (A == 0)
				{
					continue;
				}
				uint8& M1 = Brick.Mip1[(X >> 1) + 4 * ((Y >> 1) + 4 * (Z >> 1))];
				M1 = FMath::Max(M1, A);
				uint8& M2 = Brick.Mip2[(X >> 2) + 2 * ((Y >> 2) + 2 * (Z >> 2))];
				M2 = FMath::Max(M2, A);
				Brick.Mip3 = FMath::Max(Brick.Mip3, A);
			}
		}
	}
}

void FVoxelLightField::RebuildCoarse()
{
	FRWScopeLock ScopeLock(Lock, SLT_Write);
	for (int32 I = 0; I < VoxelLF::NumCoarseLevels; ++I)
	{
		Coarse[I].Reset();
	}
	for (const TPair<FIntVector, TUniquePtr<FVoxelLFBrick>>& Pair : Bricks)
	{
		const uint8 A = Pair.Value ? Pair.Value->Mip3 : 0;
		if (A == 0)
		{
			continue;
		}
		for (int32 I = 0; I < VoxelLF::NumCoarseLevels; ++I)
		{
			const FIntVector Key = FloorDivVec(Pair.Key, 2 << I); // 2, 4, 8 bricks -> 640/1280/2560 UU
			uint8& Slot = Coarse[I].FindOrAdd(Key, 0);
			Slot = FMath::Max(Slot, A);
		}
	}
}

int32 FVoxelLightField::EvictFarBricks(const FVector& CameraUU, double RadiusUU, int32 MaxEvictions,
                                       TArray<FIntVector>& OutEvicted)
{
	FRWScopeLock ScopeLock(Lock, SLT_Write);
	const double R2 = RadiusUU * RadiusUU;
	int32 Evicted = 0;
	for (auto It = Bricks.CreateIterator(); It && Evicted < MaxEvictions; ++It)
	{
		const FVector Center((double(It.Key().X) + 0.5) * VoxelLF::BrickEdgeUU,
		                     (double(It.Key().Y) + 0.5) * VoxelLF::BrickEdgeUU,
		                     (double(It.Key().Z) + 0.5) * VoxelLF::BrickEdgeUU);
		if (FVector::DistSquared(Center, CameraUU) > R2)
		{
			OutEvicted.Add(It.Key());
			It.RemoveCurrent();
			++Evicted;
		}
	}
	return Evicted;
}

// --- sampling --------------------------------------------------------------

float FVoxelLightField::SampleOpacity(const FVector& WorldUU, int32 Level) const
{
	if (Level < VoxelLF::NumBrickMips)
	{
		const FIntVector Key = WorldToBrick(WorldUU);
		const FVoxelLFBrick* Brick = FindBrick(Key);
		if (!Brick || Brick->Mip3 == 0)
		{
			return 0.f;
		}
		if (Level == 3)
		{
			return float(Brick->Mip3) * (1.f / 255.f);
		}

		const double LX = WorldUU.X - double(Key.X) * VoxelLF::BrickEdgeUU;
		const double LY = WorldUU.Y - double(Key.Y) * VoxelLF::BrickEdgeUU;
		const double LZ = WorldUU.Z - double(Key.Z) * VoxelLF::BrickEdgeUU;
		const int32 CS = VoxelLF::CellSizeUU << Level;
		const int32 Edge = VoxelLF::BrickEdgeCells >> Level;
		const int32 IX = FMath::Clamp(int32(LX / CS), 0, Edge - 1);
		const int32 IY = FMath::Clamp(int32(LY / CS), 0, Edge - 1);
		const int32 IZ = FMath::Clamp(int32(LZ / CS), 0, Edge - 1);

		uint8 A = 0;
		if (Level == 0) { A = Brick->Opacity[IX + 8 * (IY + 8 * IZ)]; }
		else if (Level == 1) { A = Brick->Mip1[IX + 4 * (IY + 4 * IZ)]; }
		else { A = Brick->Mip2[IX + 2 * (IY + 2 * IZ)]; }
		return float(A) * (1.f / 255.f);
	}

	const int32 CoarseIdx = Level - VoxelLF::NumBrickMips; // 0..2
	const int32 CS = VoxelLF::BrickEdgeUU << (CoarseIdx + 1); // 640 / 1280 / 2560
	const FIntVector Key(FloorDivWorld(WorldUU.X, CS), FloorDivWorld(WorldUU.Y, CS), FloorDivWorld(WorldUU.Z, CS));
	return float(Coarse[CoarseIdx].FindRef(Key)) * (1.f / 255.f);
}

// Reads the PUBLISHED (previous-pass) irradiance, never the in-flight one --
// this is the read side of the Jacobi iteration. See the ENERGY CONSERVATION
// note in the header: sampling the live AvgIrr here is what made the bounce
// count depend on scan order and thread scheduling, and made enclosed spaces
// brighten pass over pass.
float FVoxelLightField::SampleAvgIrr(const FVector& WorldUU) const
{
	const FIntVector Key = WorldToBrick(WorldUU);
	const FVoxelLFBrick* Brick = FindBrick(Key);
	if (!Brick)
	{
		return 0.f;
	}
	const int32 IX = FMath::Clamp(int32((WorldUU.X - double(Key.X) * VoxelLF::BrickEdgeUU) / VoxelLF::CellSizeUU), 0, 7);
	const int32 IY = FMath::Clamp(int32((WorldUU.Y - double(Key.Y) * VoxelLF::BrickEdgeUU) / VoxelLF::CellSizeUU), 0, 7);
	const int32 IZ = FMath::Clamp(int32((WorldUU.Z - double(Key.Z) * VoxelLF::BrickEdgeUU) / VoxelLF::CellSizeUU), 0, 7);
	const uint8* Src = bLegacyInPlaceGather ? Brick->AvgIrr : Brick->PrevAvgIrr;
	return float(Src[VoxelLF::CellIndex(IX, IY, IZ)]) * (1.f / 255.f);
}

bool FVoxelLightField::SampleIrradiance(const FVector& WorldUU, const FVector3f& Normal, float& OutIrradiance) const
{
	FRWScopeLock ScopeLock(Lock, SLT_ReadOnly);
	return SampleIrradianceUnlocked(WorldUU, Normal, OutIrradiance);
}

bool FVoxelLightField::SampleIrradianceUnlocked(const FVector& WorldUU, const FVector3f& Normal, float& OutIrradiance) const
{
	// Ambient-cube recombination weights. For this mesh every normal is
	// axis-aligned, so exactly one weight is non-zero -- the general form is
	// kept because it costs nothing and stops this from silently breaking if
	// non-axis-aligned geometry (vegetation cards, debris) ever samples here.
	float DirWeight[VoxelLF::NumDirs];
	float DirWeightSum = 0.f;
	for (int32 D = 0; D < VoxelLF::NumDirs; ++D)
	{
		DirWeight[D] = FMath::Max(0.f, FVector3f::DotProduct(Normal, VoxelLF::DirTable[D]));
		DirWeightSum += DirWeight[D];
	}
	if (DirWeightSum <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	const float InvDirWeightSum = 1.f / DirWeightSum;

	// Push out along the normal so the tap lands in an AIR cell in front of the
	// surface (solid cells are never solved). The offset is tried at
	// increasing distances because THIN geometry defeats a single fixed one:
	// a 20cm roof slab is half a light-field cell thick, so a half-cell offset
	// frequently stays inside the slab's own solid cell and the lookup fails
	// back to plain AO. Observed exactly that on the wall+roof fixture -- the
	// enclosed wall darkened correctly while the slab underside stayed fully
	// lit. First offset that finds solved data wins.
	static constexpr float kProbeOffsets[3] = {0.6f, 1.25f, 2.0f};
	for (float ProbeCells : kProbeOffsets)
	{
		if (SampleIrradianceAtProbe(WorldUU + FVector(Normal) * (VoxelLF::CellSizeUU * ProbeCells),
		                            DirWeight, InvDirWeightSum, OutIrradiance))
		{
			return true;
		}
	}
	return false;
}

bool FVoxelLightField::SampleIrradianceAtProbe(const FVector& P, const float (&DirWeight)[VoxelLF::NumDirs],
                                               float InvDirWeightSum, float& OutIrradiance) const
{
	// Trilinear over the 8 surrounding cell centres, skipping unsolved taps
	// and renormalizing by the surviving weight -- so a shading point at the
	// edge of solved data biases toward the data that exists instead of
	// fading to black.
	const double GX = P.X / VoxelLF::CellSizeUU - 0.5;
	const double GY = P.Y / VoxelLF::CellSizeUU - 0.5;
	const double GZ = P.Z / VoxelLF::CellSizeUU - 0.5;
	const int64 BX = int64(FMath::FloorToDouble(GX));
	const int64 BY = int64(FMath::FloorToDouble(GY));
	const int64 BZ = int64(FMath::FloorToDouble(GZ));
	const float FX = float(GX - double(BX));
	const float FY = float(GY - double(BY));
	const float FZ = float(GZ - double(BZ));

	float Accum = 0.f;
	float AccumWeight = 0.f;

	// One brick-map lookup is memoized across taps; the 8 taps almost always
	// share a brick, and a TMap probe per tap would otherwise dominate.
	FIntVector CachedKey(MAX_int32, MAX_int32, MAX_int32);
	const FVoxelLFBrick* CachedBrick = nullptr;

	for (int32 T = 0; T < 8; ++T)
	{
		const int32 OX = T & 1, OY = (T >> 1) & 1, OZ = (T >> 2) & 1;
		const float W = (OX ? FX : 1.f - FX) * (OY ? FY : 1.f - FY) * (OZ ? FZ : 1.f - FZ);
		if (W <= 0.f)
		{
			continue;
		}
		const int64 CX = BX + OX, CY = BY + OY, CZ = BZ + OZ;
		const FIntVector Key(int32(FloorDiv(CX, VoxelLF::BrickEdgeCells)),
		                     int32(FloorDiv(CY, VoxelLF::BrickEdgeCells)),
		                     int32(FloorDiv(CZ, VoxelLF::BrickEdgeCells)));
		if (Key != CachedKey)
		{
			CachedKey = Key;
			CachedBrick = FindBrick(Key);
		}
		if (!CachedBrick)
		{
			continue;
		}
		const int32 LX = int32(CX - int64(Key.X) * VoxelLF::BrickEdgeCells);
		const int32 LY = int32(CY - int64(Key.Y) * VoxelLF::BrickEdgeCells);
		const int32 LZ = int32(CZ - int64(Key.Z) * VoxelLF::BrickEdgeCells);
		const int32 Idx = VoxelLF::CellIndex(LX, LY, LZ);
		if (!CachedBrick->SolvedCells[Idx])
		{
			continue;
		}

		float Value = 0.f;
		for (int32 D = 0; D < VoxelLF::NumDirs; ++D)
		{
			if (DirWeight[D] > 0.f)
			{
				Value += DirWeight[D] * float(CachedBrick->Vis[Idx * VoxelLF::NumDirs + D]) * (1.f / 255.f);
			}
		}
		Value *= InvDirWeightSum;

		Accum += W * Value;
		AccumWeight += W;
	}

	if (AccumWeight <= 1.e-4f)
	{
		return false; // nothing solved here yet -- caller must keep its non-GI path
	}
	OutIrradiance = Accum / AccumWeight;
	return true;
}

// --- GPU volume encode -----------------------------------------------------

bool FVoxelLightField::EncodeBrickTexels(const FIntVector& Key, uint8* OutPos, uint8* OutNeg) const
{
	FRWScopeLock ScopeLock(Lock, SLT_ReadOnly);
	return EncodeBrickTexelsUnlocked(Key, OutPos, OutNeg);
}

bool FVoxelLightField::EncodeBrickTexelsUnlocked(const FIntVector& Key, uint8* OutPos, uint8* OutNeg) const
{
	check(OutPos && OutNeg);

	const FVoxelLFBrick* Brick = FindBrick(Key);
	// bSolved is checked as well as presence because VoxelizeChunk clears it,
	// so a brick that has been re-voxelized and not yet re-solved encodes as
	// "no data" rather than as its pre-edit lighting. Its Vis is already zeroed
	// there, so this is belt and braces -- but it is the branch that documents
	// the intent, and the eviction path relies on the same all-zero output.
	if (!Brick || !Brick->bSolved)
	{
		FMemory::Memzero(OutPos, BrickTexelBytes);
		FMemory::Memzero(OutNeg, BrickTexelBytes);
		return false;
	}

	for (int32 Idx = 0; Idx < VoxelLF::BrickCells; ++Idx)
	{
		uint8* P = OutPos + Idx * 4;
		uint8* N = OutNeg + Idx * 4;
		if (!Brick->SolvedCells[Idx])
		{
			// A = 0, and RGB = 0 BECAUSE they are premultiplied by it. Any other
			// RGB here would leak into a neighbouring texel's trilinear tap with
			// zero weight in the denominator -- i.e. energy from nowhere.
			P[0] = 0; P[1] = 0; P[2] = 0; P[3] = 0;
			N[0] = 0; N[1] = 0; N[2] = 0; N[3] = 0;
			continue;
		}

		const uint8* Vis = Brick->Vis + Idx * VoxelLF::NumDirs;
		// DirTable order: +X -X +Y -Y +Z -Z. The two volumes split it by SIGN, so
		// a face reads exactly one of them and takes exactly one component --
		// which is what makes Scheme A cost memory and not bandwidth.
		P[0] = Vis[0]; P[1] = Vis[2]; P[2] = Vis[4]; P[3] = 255;
		N[0] = Vis[1]; N[1] = Vis[3]; N[2] = Vis[5]; N[3] = 255;
	}
	return true;
}

void FVoxelLightField::SeedIrradiance(uint8 Value)
{
	FRWScopeLock ScopeLock(Lock, SLT_Write);
	for (TPair<FIntVector, TUniquePtr<FVoxelLFBrick>>& Pair : Bricks)
	{
		if (!Pair.Value)
		{
			continue;
		}
		FVoxelLFBrick& Brick = *Pair.Value;
		for (int32 C = 0; C < VoxelLF::BrickCells; ++C)
		{
			// Solved cells only: an unsolved cell is not part of the iteration
			// and seeding it would inject light into rock.
			if (Brick.SolvedCells[C])
			{
				Brick.AvgIrr[C] = Value;
				Brick.PrevAvgIrr[C] = Value;
			}
		}
	}
}

// --- solve -----------------------------------------------------------------

int32 FVoxelLightField::SolveBricks(const TArray<FIntVector>& Keys, const FVoxelGISolveParams& Params,
                                    FVoxelGIPassStats* OutStats)
{
	if (OutStats)
	{
		*OutStats = FVoxelGIPassStats();
	}
	if (Keys.Num() == 0)
	{
		return 0;
	}

	// Structural mutation is game-thread-only and this call is a BLOCKING
	// ParallelFor issued from the game thread, so no insert/evict can race it.
	// The read lock is belt-and-braces against a future async sampler.
	FRWScopeLock ScopeLock(Lock, SLT_ReadOnly);

	TArray<FVoxelLFBrick*> Targets;
	Targets.Reserve(Keys.Num());
	TArray<FIntVector> TargetKeys;
	TargetKeys.Reserve(Keys.Num());
	for (const FIntVector& Key : Keys)
	{
		if (TUniquePtr<FVoxelLFBrick>* Found = Bricks.Find(Key))
		{
			if (Found->IsValid())
			{
				Targets.Add(Found->Get());
				TargetKeys.Add(Key);
			}
		}
	}
	if (Targets.Num() == 0)
	{
		return 0;
	}

	TArray<int32> SolvedCounts;
	SolvedCounts.SetNumZeroed(Targets.Num());

	ParallelFor(Targets.Num(), [&](int32 I)
	{
		SolvedCounts[I] = SolveBrickInternal(TargetKeys[I], *Targets[I], Params);
	});

	// PUBLISH. Every brick in this pass swaps its freshly-written AvgIrr into
	// PrevAvgIrr only now, after all workers have finished -- which is what
	// makes the pass a Jacobi step (exactly one bounce, order-independent)
	// rather than the nondeterministic Gauss-Seidel it used to be.
	//
	// Deliberately AFTER the ParallelFor and not inside it: publishing per
	// brick as each worker finished would reintroduce exactly the race this
	// exists to remove. 512 bytes per brick, so at the default 10 bricks/frame
	// this is a 5 KB memcpy.
	int32 Total = 0;
	double IrrSum = 0.0;
	int32 IrrCount = 0;
	double MaxDelta = 0.0;
	for (int32 I = 0; I < Targets.Num(); ++I)
	{
		Total += SolvedCounts[I];
		FVoxelLFBrick& Brick = *Targets[I];
		for (int32 C = 0; C < VoxelLF::BrickCells; ++C)
		{
			if (!Brick.SolvedCells[C])
			{
				continue;
			}
			const int32 New = int32(Brick.AvgIrr[C]);
			MaxDelta = FMath::Max(MaxDelta, FMath::Abs(double(New - int32(Brick.PrevAvgIrr[C]))));
			IrrSum += double(New);
			++IrrCount;
		}
		FMemory::Memcpy(Brick.PrevAvgIrr, Brick.AvgIrr, sizeof(Brick.AvgIrr));
	}

	if (OutStats)
	{
		OutStats->CellsSolved = Total;
		OutStats->MeanIrr = IrrCount > 0 ? (IrrSum / double(IrrCount)) / 255.0 : 0.0;
		OutStats->MaxAbsDelta = MaxDelta / 255.0;
	}
	return Total;
}

int32 FVoxelLightField::SolveBrickInternal(const FIntVector& Key, FVoxelLFBrick& Brick, const FVoxelGISolveParams& Params)
{
	int32 NumSolved = 0;
	const FVector BrickOriginUU(double(Key.X) * VoxelLF::BrickEdgeUU,
	                            double(Key.Y) * VoxelLF::BrickEdgeUU,
	                            double(Key.Z) * VoxelLF::BrickEdgeUU);

	const float StartOffsetUU = VoxelLF::CellSizeUU * 0.75f;
	const float MinStepUU = float(VoxelLF::CellSizeUU);

	TBitArray<> NewSolved;
	NewSolved.Init(false, VoxelLF::BrickCells);

	for (int32 Z = 0; Z < 8; ++Z)
	for (int32 Y = 0; Y < 8; ++Y)
	for (int32 X = 0; X < 8; ++X)
	{
		const int32 Idx = VoxelLF::CellIndex(X, Y, Z);
		if (Brick.Opacity[Idx] != 0)
		{
			continue; // solid cell -- nothing shades from inside rock
		}

		const FVector P = BrickOriginUU + FVector((X + 0.5) * VoxelLF::CellSizeUU,
		                                          (Y + 0.5) * VoxelLF::CellSizeUU,
		                                          (Z + 0.5) * VoxelLF::CellSizeUU);

		// Only air cells within a 1-ring of a surface are worth solving: a
		// shading point sits ON a surface and its trilinear taps reach one
		// cell out, so the 26-neighbourhood is exactly the set that can ever
		// be read. Skipping the rest is the single biggest cost saving in the
		// solve -- an open-sky brick is typically ~10-20% solvable cells.
		bool bNearSurface = false;
		for (int32 DZ = -1; DZ <= 1 && !bNearSurface; ++DZ)
		for (int32 DY = -1; DY <= 1 && !bNearSurface; ++DY)
		for (int32 DX = -1; DX <= 1 && !bNearSurface; ++DX)
		{
			if (DX == 0 && DY == 0 && DZ == 0)
			{
				continue;
			}
			const FVector N = P + FVector(DX * VoxelLF::CellSizeUU, DY * VoxelLF::CellSizeUU, DZ * VoxelLF::CellSizeUU);
			bNearSurface = SampleOpacity(N, 0) > 0.f;
		}
		if (!bNearSurface)
		{
			continue;
		}

		// Trace the 14-cone basis once, then project it into the 6 ambient-cube
		// slots. Tracing per-slot instead would be 30 marches for the same
		// answer; the cones are shared because a diagonal cone contributes to
		// three slots at once.
		// TraceDirTable's first 6 entries ARE DirTable, in the same order, so
		// the legacy basis is just this loop stopped at 6 with an identity
		// projection below.
		const int32 NumTrace = Params.bLegacyConeBasis ? VoxelLF::NumDirs : VoxelLF::NumTraceDirs;
		float ConeIrr[VoxelLF::NumTraceDirs];
		for (int32 D = 0; D < NumTrace; ++D)
		{
			const FVector Dir(VoxelLF::TraceDirTable[D]);

			float Occ = 0.f;
			float Bounce = 0.f;
			float LastAirIrr = 0.f;
			float T = StartOffsetUU;

			while (T < Params.MaxConeDistanceUU && Occ < 0.98f)
			{
				const FVector S = P + Dir * double(T);
				const float Radius = T * Params.ConeTanHalfAperture;

				// Mip selection: cell size ~ cone radius. This is the whole
				// point of the pyramid -- a 30 m cone resolves against 2.5 m
				// cells, not 0.4 m ones, so the march is ~12 steps regardless
				// of distance.
				const int32 Level = FMath::Clamp(
					FMath::FloorToInt(FMath::Log2(FMath::Max(Radius, float(VoxelLF::CellSizeUU)) / float(VoxelLF::CellSizeUU))),
					0, VoxelLF::MaxLevel);
				// The step MUST be the sampled level's cell size, not the cone
				// radius. They diverge (radius > cell size between mip
				// boundaries) and the difference is not cosmetic: because this
				// is a SURFACE voxelization, the ground is a single-cell-thick
				// shell, and a step larger than the cell being sampled marches
				// straight through it. Symptom, seen on the wall+roof fixture:
				// the slab underside's downward cone jumped from 60 UU above
				// the ground to 160 UU below it, found no opacity either side,
				// and reported full sky visibility for a surface facing a floor
				// 3.2m away.
				const float LevelCellUU = float(VoxelLF::CellSizeUU << Level);

				const float A = SampleOpacity(S, Level);
				if (A > 0.f)
				{
					const float Contrib = (1.f - Occ) * A;
					// Progressive one bounce: the surface we just hit re-emits
					// whatever irradiance the last air cell in front of it was
					// carrying as of the previous solve. Converges over
					// successive re-solves rather than within one.
					Bounce += Contrib * Params.BounceAlbedo * LastAirIrr;
					Occ += Contrib;
				}
				else
				{
					LastAirIrr = SampleAvgIrr(S);
				}

				T += FMath::Max(LevelCellUU, MinStepUU);
			}

			// ENERGY: Vis and the Contrib_i that fed Bounce partition the cone
			// exactly (Vis + sum Contrib_i = 1), and Bounce scales every
			// Contrib_i by albedo < 1 -- so Irr <= Vis*Sky + albedo*(1-Vis) <= 1
			// for Sky <= 1 with no clamping needed. The Clamp is byte-encode
			// hygiene against a SkyIntensity someone pushed above 1, not the
			// thing keeping the iteration bounded.
			const float Vis = FMath::Max(0.f, 1.f - Occ);
			ConeIrr[D] = FMath::Clamp(Vis * FMath::Min(Params.SkyIntensity, 1.f) + Bounce, 0.f, 1.f);
		}

		// Project into the ambient cube. Each row of SlotWeight sums to 1, so
		// every slot is a convex combination of ConeIrr and stays in [0,1] --
		// the recombination cannot add energy either.
		float Sum = 0.f;
		for (int32 S = 0; S < VoxelLF::NumDirs; ++S)
		{
			float SlotIrr = 0.f;
			if (Params.bLegacyConeBasis)
			{
				SlotIrr = ConeIrr[S]; // the degenerate one-cone-per-slot basis
			}
			else
			{
				for (int32 D = 0; D < VoxelLF::NumTraceDirs; ++D)
				{
					SlotIrr += VoxelLF::SlotWeight[S][D] * ConeIrr[D];
				}
			}
			SlotIrr = FMath::Clamp(SlotIrr, 0.f, 1.f);
			Brick.Vis[Idx * VoxelLF::NumDirs + S] = uint8(FMath::RoundToInt(SlotIrr * 255.f));
			Sum += SlotIrr;
		}

		Brick.AvgIrr[Idx] = uint8(FMath::RoundToInt(FMath::Clamp(Sum / VoxelLF::NumDirs, 0.f, 1.f) * 255.f));
		NewSolved[Idx] = true;
		++NumSolved;
	}

	Brick.SolvedCells = MoveTemp(NewSolved);
	Brick.bSolved = true;
	return NumSolved;
}
