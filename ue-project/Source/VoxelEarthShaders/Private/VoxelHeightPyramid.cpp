// VoxelHeightPyramid.cpp -- storage, max-reduction and upload for the marcher's
// terrain-height upper bound. The rules this file must not break are stated in
// VoxelHeightPyramid.h; the two that are easiest to break by accident are
// repeated at the sites that could break them.

#include "VoxelHeightPyramid.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

#include <limits>

float VoxelHeightPyramidPositiveInfinity()
{
	// FROM THE BIT PATTERN, NOT FROM 1.0f/0.0f AND NOT FROM A LITERAL.
	// The shader side writes asfloat(0x7F800000) and the two have to be the same
	// number for the air test to be false on both sides of the wire. A division
	// would be at the mercy of a fast-math flag on either compiler.
	static_assert(std::numeric_limits<float>::is_iec559,
	              "the +INF contract with the shader assumes IEEE-754 floats");
	const uint32 Bits = 0x7F800000u;
	float Out;
	FMemory::Memcpy(&Out, &Bits, sizeof(float));
	return Out;
}

namespace
{
	// Floored division. The lattice is signed and the leg spawn is at negative
	// world coordinates, so C++ truncation-toward-zero is wrong here in exactly
	// the half of the world the project measures in.
	FORCEINLINE int64 FloorDivInt(int64 A, int64 B)
	{
		const int64 Q = A / B;
		const int64 R = A % B;
		return (R != 0 && ((R < 0) != (B < 0))) ? Q - 1 : Q;
	}

	FORCEINLINE int32 AlignDownSigned(int32 V, int32 Align)
	{
		return int32(FloorDivInt(int64(V), int64(Align)) * int64(Align));
	}
}

FVoxelHeightPyramid::FVoxelHeightPyramid() = default;
FVoxelHeightPyramid::~FVoxelHeightPyramid() = default;

FVoxelHeightPyramid::FGeometry FVoxelHeightPyramid::MakeGeometryFor(double WorldXUU, double WorldYUU,
                                                                    int32 LeafChunkLevel)
{
	FGeometry G;
	// 0..6: level 6 is the last ring level the chunk record's four-bit level
	// field can carry (VoxelCoords::kNumLevels), and a leaf coarser than that
	// has no meaning as a chunk footprint.
	G.LeafChunkLevel = FMath::Clamp(LeafChunkLevel, 0, 6);
	// 32 level-0 voxels per chunk edge = 2^5, times 2^Level.
	G.LeafVoxelShift = 5 + G.LeafChunkLevel;

	const double LeafEdgeUU = 320.0 * double(int64(1) << G.LeafChunkLevel);

	// Dim is the largest power of two that keeps the field inside kFieldSpanUU,
	// clamped so the array stays sane at both ends of the leaf-level range.
	int32 Dim = 1;
	while (double(Dim) * 2.0 * LeafEdgeUU <= kFieldSpanUU && Dim < 2048)
	{
		Dim *= 2;
	}
	Dim = FMath::Clamp(Dim, 64, 2048);
	G.Dim = Dim;

	// Enough mips that the coarsest cell is at least 8x8 leaves -- a coarsest
	// level of one cell would make the top of the walk a single global ceiling,
	// which is the mechanism this design exists to beat.
	int32 LogDim = 0;
	while ((1 << LogDim) < Dim) { ++LogDim; }
	G.NumMips = FMath::Clamp(LogDim - 2, 1, int32(kMaxMips));

	// THE ANCHOR IS ALIGNED TO THE COARSEST MIP. An unaligned origin makes a
	// mip-m cell's 2x2 block depend on where the camera happened to be, so the
	// reduce would cover a different set of children than the shader's
	// cell >> m addresses -- and a parent whose bound does not cover one of its
	// children is proven air over real ground.
	const int32 Align = 1 << (G.NumMips - 1);

	const int32 CamCellX = int32(FloorDivInt(int64(FMath::FloorToDouble(WorldXUU)),
	                                         int64(LeafEdgeUU)));
	const int32 CamCellY = int32(FloorDivInt(int64(FMath::FloorToDouble(WorldYUU)),
	                                         int64(LeafEdgeUU)));
	G.MinCellX = AlignDownSigned(CamCellX - Dim / 2, Align);
	G.MinCellY = AlignDownSigned(CamCellY - Dim / 2, Align);

	int32 Offset = 0;
	for (int32 m = 0; m < G.NumMips; ++m)
	{
		G.MipOffset[m] = Offset;
		const int32 D = Dim >> m;
		Offset += D * D;
	}
	G.TotalFloats = Offset;
	return G;
}

bool FVoxelHeightPyramid::ShouldReanchor(double WorldXUU, double WorldYUU, int32 LeafChunkLevel) const
{
	FScopeLock Guard(&Lock);
	if (!Geom.IsValid() || Geom.LeafChunkLevel != FMath::Clamp(LeafChunkLevel, 0, 6))
	{
		return true;
	}
	const double LeafEdgeUU = Geom.LeafEdgeUU();
	const int32 CamCellX = int32(FloorDivInt(int64(FMath::FloorToDouble(WorldXUU)), int64(LeafEdgeUU)));
	const int32 CamCellY = int32(FloorDivInt(int64(FMath::FloorToDouble(WorldYUU)), int64(LeafEdgeUU)));
	const int32 CentreX = Geom.MinCellX + Geom.Dim / 2;
	const int32 CentreY = Geom.MinCellY + Geom.Dim / 2;

	// Dim/8, NOT Dim/4, AND THE MARGIN IS THE REASON. The march reach is
	// 8,396 m across, i.e. 4,198 m of radius. At the shipped geometry the field
	// half-span is 6,553 m, so the camera may drift 6,553 - 4,198 = 2,355 m
	// before any part of the reach leaves the field. Dim/8 is 1,638 m, which
	// keeps the whole reach inside with room to spare; Dim/4 would not, and the
	// symptom of getting it wrong is not a crash -- it is +INF over the far
	// field, i.e. the arm quietly doing less than the leg reports.
	const int32 Slack = Geom.Dim / 8;
	return FMath::Abs(CamCellX - CentreX) > Slack || FMath::Abs(CamCellY - CentreY) > Slack;
}

int32 FVoxelHeightPyramid::Reanchor(const FGeometry& NewGeom, TArray<FIntPoint>& OutTileOrder)
{
	OutTileOrder.Reset();
	if (!NewGeom.IsValid())
	{
		return 0;
	}

	{
		FScopeLock Guard(&Lock);
		Geom = NewGeom;
		Cells.SetNumUninitialized(NewGeom.TotalFloats);
		// CLEARED TO +INF, NEVER TO ZERO. This is the single most likely way
		// this feature ships broken: a zero-cleared height field reads as
		// "terrain tops out at sea level", and at the leg spawn that is a claim
		// that 2.8 km of mountain is empty air. Memset cannot express this, so
		// it is a loop, and this is the only place the array is cleared.
		const float Inf = VoxelHeightPyramidPositiveInfinity();
		for (int32 i = 0; i < Cells.Num(); ++i)
		{
			Cells[i] = Inf;
		}
		Census = FCensus();
		Census.LeafCells = NewGeom.Dim * NewGeom.Dim;
		++Version;
	}

	const int32 TilesPerSide = FMath::Max(1, NewGeom.Dim / int32(kTileLeafCells));
	const double Centre = double(TilesPerSide - 1) * 0.5;
	OutTileOrder.Reserve(TilesPerSide * TilesPerSide);
	for (int32 ty = 0; ty < TilesPerSide; ++ty)
	{
		for (int32 tx = 0; tx < TilesPerSide; ++tx)
		{
			OutTileOrder.Emplace(tx, ty);
		}
	}
	// NEAREST THE CENTRE FIRST. The field is filled on a millisecond budget, so
	// for the first seconds after a re-anchor only part of it is real. Filling
	// outward from the camera means the part that is real is the part the ray
	// crosses first, which is where a skip is worth most -- and it makes the
	// engagement counter climb immediately instead of staying at zero until the
	// far corners land, which is the shape that gets misread as an inert arm.
	OutTileOrder.Sort([Centre](const FIntPoint& A, const FIntPoint& B)
	{
		const double Ax = double(A.X) - Centre, Ay = double(A.Y) - Centre;
		const double Bx = double(B.X) - Centre, By = double(B.Y) - Centre;
		return (Ax * Ax + Ay * Ay) < (Bx * Bx + By * By);
	});
	return OutTileOrder.Num();
}

int32 FVoxelHeightPyramid::LeafIndexUnlocked(int32 CellX, int32 CellY) const
{
	const int32 Lx = CellX - Geom.MinCellX;
	const int32 Ly = CellY - Geom.MinCellY;
	// THE RANGE TEST IS THE INDEX. There is no wrap here on purpose: a toroidal
	// read that escapes the valid rectangle aliases onto ground 13 km away, and
	// a wrong height is proven air over real terrain rather than a blurred
	// texel. Out of field returns -1 and every caller treats that as +INF.
	if (Lx < 0 || Ly < 0 || Lx >= Geom.Dim || Ly >= Geom.Dim)
	{
		return -1;
	}
	return Geom.MipOffset[0] + Ly * Geom.Dim + Lx;
}

void FVoxelHeightPyramid::WriteLeaf(int32 CellX, int32 CellY, float ValueUU)
{
	FScopeLock Guard(&Lock);
	if (!Geom.IsValid()) { return; }
	const int32 Index = LeafIndexUnlocked(CellX, CellY);
	if (Index >= 0)
	{
		Cells[Index] = ValueUU;
	}
}

void FVoxelHeightPyramid::WriteLeafBlock(int32 CellX0, int32 CellY0, int32 W, int32 H,
                                         const float* Values, int32 DeclinedCount,
                                         int32 NotResidentCount)
{
	FScopeLock Guard(&Lock);
	if (!Geom.IsValid() || Values == nullptr) { return; }
	for (int32 j = 0; j < H; ++j)
	{
		for (int32 i = 0; i < W; ++i)
		{
			const int32 Index = LeafIndexUnlocked(CellX0 + i, CellY0 + j);
			if (Index >= 0)
			{
				Cells[Index] = Values[j * W + i];
			}
		}
	}
	// COUNTED AS CELLS OFFERED, not as cells that landed in range. The builder
	// only ever offers tiles inside the field, so the two are equal there -- and
	// if they ever stop being equal, a Filled count that outran the field is the
	// symptom that says so, which is more useful than one that quietly agreed.
	Census.Filled += W * H;
	Census.Declined += DeclinedCount;
	Census.NotResident += NotResidentCount;
}

void FVoxelHeightPyramid::MaxLeafEdited(int32 CellX, int32 CellY, float ValueUU)
{
	FScopeLock Guard(&Lock);
	if (!Geom.IsValid()) { return; }
	const int32 Index = LeafIndexUnlocked(CellX, CellY);
	if (Index >= 0 && !(ValueUU <= Cells[Index]))
	{
		// Written as !(<=) rather than (>) so that a NaN -- which no path here
		// should produce, and which every path here would rather refuse than
		// trust -- raises the cell instead of silently leaving it.
		Cells[Index] = ValueUU;
	}
}

void FVoxelHeightPyramid::RebuildMips()
{
	FScopeLock Guard(&Lock);
	if (!Geom.IsValid()) { return; }

	const float Inf = VoxelHeightPyramidPositiveInfinity();
	const int32 LeafCount = Geom.Dim * Geom.Dim;
	int32 InfCount = 0;
	float MinF = Inf;
	float MaxF = -Inf;
	{
		const int32 Base = Geom.MipOffset[0];
		for (int32 i = 0; i < LeafCount; ++i)
		{
			const float V = Cells[Base + i];
			if (!(V < Inf)) { ++InfCount; continue; }
			MinF = FMath::Min(MinF, V);
			MaxF = FMath::Max(MaxF, V);
		}
	}
	Census.Infinite = InfCount;
	Census.MinUU = (InfCount == LeafCount) ? 0.0f : MinF;
	Census.MaxUU = (InfCount == LeafCount) ? 0.0f : MaxF;

	// FULL REBUILD FROM MIP 0, NOT AN INCREMENTAL MAX. An incremental reduce
	// that raises ancestors as leaves land is correct only while leaves only
	// ever rise; a refill after a fine tile arrives legitimately LOWERS a leaf,
	// and an ancestor that kept the old higher value would merely be loose --
	// but an ancestor that missed a child is BELOW its subtree, which is proven
	// air over real ground. Recomputing every parent from its children makes
	// that unrepresentable, and it costs a few hundred microseconds.
	for (int32 m = 1; m < Geom.NumMips; ++m)
	{
		const int32 D = Geom.Dim >> m;
		const int32 PD = Geom.Dim >> (m - 1);
		const int32 Dst = Geom.MipOffset[m];
		const int32 Src = Geom.MipOffset[m - 1];
		for (int32 y = 0; y < D; ++y)
		{
			for (int32 x = 0; x < D; ++x)
			{
				const int32 X0 = x * 2, Y0 = y * 2;
				// max(x, INF) == INF, so one absent child poisons the parent for
				// free and in the correct direction. No sentinel handling.
				const float A = Cells[Src + (Y0 + 0) * PD + (X0 + 0)];
				const float B = Cells[Src + (Y0 + 0) * PD + (X0 + 1)];
				const float C = Cells[Src + (Y0 + 1) * PD + (X0 + 0)];
				const float E = Cells[Src + (Y0 + 1) * PD + (X0 + 1)];
				Cells[Dst + y * D + x] = FMath::Max(FMath::Max(A, B), FMath::Max(C, E));
			}
		}
	}
}

void FVoxelHeightPyramid::BumpVersion()
{
	FScopeLock Guard(&Lock);
	++Version;
}

FVoxelHeightPyramid::FGeometry FVoxelHeightPyramid::GetGeometry() const
{
	FScopeLock Guard(&Lock);
	return Geom;
}

uint32 FVoxelHeightPyramid::GetVersion() const
{
	FScopeLock Guard(&Lock);
	return Version;
}

FVoxelHeightPyramid::FCensus FVoxelHeightPyramid::GetCensus() const
{
	FScopeLock Guard(&Lock);
	return Census;
}

void FVoxelHeightPyramid::NoteLeafOutcome(bool bDeclined, bool bNotResident)
{
	FScopeLock Guard(&Lock);
	++Census.Filled;
	if (bDeclined) { ++Census.Declined; }
	if (bNotResident) { ++Census.NotResident; }
}

void FVoxelHeightPyramid::NoteEdited()
{
	FScopeLock Guard(&Lock);
	++Census.Edited;
}

FVoxelHeightPyramid::FBinding FVoxelHeightPyramid::BindForRender(FRDGBuilder& GraphBuilder)
{
	FBinding Out;
	FScopeLock Guard(&Lock);
	if (!Geom.IsValid() || Cells.Num() != Geom.TotalFloats)
	{
		// NOTHING SAFE TO BIND. The caller must leave the arm off rather than
		// bind a null SRV: an unbound typed buffer reads zeros, and zero at this
		// datum is sea level, which is the exact failure this whole feature is
		// written to avoid.
		return Out;
	}

	Out.Geom = Geom;

	// BOTH QUESTIONS, the way the chunk index learned to ask them:
	// QueueBufferExtraction writes the pooled pointer at graph EXECUTE, so an
	// upload queued into a graph that is later abandoned leaves it null while
	// the version stamp says the GPU is current. Testing the pointer as well as
	// the version makes that self-heal on the next bind.
	if (Pooled.IsValid() && PooledVersion == Version)
	{
		Out.Buffer = GraphBuilder.RegisterExternalBuffer(Pooled, TEXT("VoxelMarch.HeightPyramid"));
		Out.bValid = true;
		return Out;
	}

	Out.Buffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), Geom.TotalFloats),
		TEXT("VoxelMarch.HeightPyramid"));
	GraphBuilder.QueueBufferUpload(Out.Buffer, Cells.GetData(), Cells.Num() * sizeof(float),
	                               ERDGInitialDataFlags::None);
	GraphBuilder.QueueBufferExtraction(Out.Buffer, &Pooled);
	PooledVersion = Version;
	Out.bValid = true;
	return Out;
}

FVoxelHeightPyramid& GetGlobalVoxelHeightPyramid()
{
	static FVoxelHeightPyramid Pyramid;
	return Pyramid;
}
