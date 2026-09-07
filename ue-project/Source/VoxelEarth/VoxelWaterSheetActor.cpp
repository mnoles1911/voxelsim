#include "VoxelWaterSheetActor.h"

#include "Materials/MaterialInstanceDynamic.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "ProceduralMeshComponent.h"
#include "VoxelEarth.h"
#include "VoxelEofDirtyLedger.h" // EndOfFrameUpdates attribution -- the adoption-drain hypothesis
#include "VoxelWaterSubsystem.h"

#include "HAL/IConsoleManager.h"

// ---------------------------------------------------------------------------
// CONSOLE VARIABLES
// ---------------------------------------------------------------------------
//
// B1's tessellation radius, AND THE REASON IT IS A CVAR AND NOT STILL A MEMBER.
// It was parsed once out of -VoxelWaveTessM at BeginPlay, which was the right
// shape while the only thing that ever set it was a leg command line. It is now
// also the player-facing "Water Wave Detail" row (VoxelGraphicsUserSettings),
// and a setting a player flips in the menu has to be readable AFTER BeginPlay
// or the toggle is decoration.
//
// READ AT REBUILD TIME, which is what makes a mid-session change take effect at
// all: the sheet rebuilds one basin per tick on a round robin, so a flip is
// visible on the basins nearest the camera within a few frames and on the whole
// resident set within a rotation. That latency is a property of the rebuild
// schedule, not of this variable, and it is deliberately NOT mentioned in the
// player-facing description -- a player does not need to be told that water
// updates over the next second, and the row would read as a defect if they were.
//
// -VoxelWaveTessM= still works and now SETS this at BeginPlay; see there.
TAutoConsoleVariable<float> CVarVoxelWaterWaveTessRadiusM(
	TEXT("voxel.Water.WaveTessRadiusM"), 80.0f,
	TEXT("Radius in METRES of the wave-tessellation disc on lake sheets: inside it the greedy ")
	TEXT("rectangles are subdivided to one fine pixel so the material's World Position Offset has ")
	TEXT("vertices to push; outside it the sheet stays flat rectangles and the waves are ")
	TEXT("normal-only. 0 is the honest OFF control -- no tessellated vertex is emitted at all and ")
	TEXT("the sheet is byte-identical to every capture taken before B1. CLAMPED at read time to ")
	TEXT("the finest LOD band (AVoxelWaterSheetActor::FineBandRadiusM, 96 m by default): past that ")
	TEXT("the sheet is not meshing at one fine pixel, so cells emitted out there would not be the ")
	TEXT("finest band's. Read on every rebuild, so a change lands as the round robin revisits ")
	TEXT("basins rather than at the next launch."),
	ECVF_Default);

namespace
{
// Rectangles come out of vxc::lakeSheetRects as tile-local pixel spans and
// arrive here as world-UU boxes; this turns one into four vertices and two
// triangles at the datum.
//
// SEPARATE VERTICES PER RECTANGLE, not a shared vertex pool. Adjacent
// rectangles abut exactly and are coplanar, so a shared-vertex weld buys
// nothing visually and costs a hash per vertex over a set that is already only
// a few hundred quads per lake; and welding across rectangles is precisely
// where a T-junction crack would come from if the decomposition ever changed.
void AppendRectQuad(const FBox2D& RectUU, double ZUU, const FVector2D& UVOriginUU,
                    TArray<FVector>& Verts, TArray<int32>& Tris, TArray<FVector>& Normals,
                    TArray<FVector2D>& UVs, TArray<FColor>& Colors, TArray<FProcMeshTangent>& Tangents)
{
	const int32 Base = Verts.Num();
	const double X0 = RectUU.Min.X, Y0 = RectUU.Min.Y, X1 = RectUU.Max.X, Y1 = RectUU.Max.Y;
	Verts.Add(FVector(X0, Y0, ZUU));
	Verts.Add(FVector(X1, Y0, ZUU));
	Verts.Add(FVector(X1, Y1, ZUU));
	Verts.Add(FVector(X0, Y1, ZUU));

	// UVs in METRES, world-planar, anchored at the actor-relative origin. The
	// material's ripple reads TexCoord0 as metres (create_water_voxel_material.py:
	// "uv is in METRES"), which the pooled voxel vertex factory supplies wrapped
	// to a 32 m period. This does NOT wrap, on purpose: a rectangle here can be a
	// kilometre long and a wrap inside a quad would smear the whole period across
	// it. The consequence is a ripple PHASE offset against the near-field water,
	// which is invisible -- the voxel path's own 32 m wrap already makes its phase
	// discontinuous every 32 m (freq*32 is not a multiple of 2pi for any of the
	// four channels), so there was never a continuous phase to match.
	for (int32 i = Base; i < Verts.Num(); ++i)
	{
		UVs.Add(FVector2D((Verts[i].X - UVOriginUU.X) / 100.0, (Verts[i].Y - UVOriginUU.Y) / 100.0));
		Normals.Add(FVector::UpVector);
		Tangents.Add(FProcMeshTangent(1.f, 0.f, 0.f));
		// See the header for this convention -- it is M_WaterVoxel's, not a new
		// one. R=255 full fill, G=255 no AO, B=255 top boundary, A=0 no foam.
		Colors.Add(FColor(255, 255, 255, 0));
	}

	Tris.Add(Base + 0);
	Tris.Add(Base + 1);
	Tris.Add(Base + 2);
	Tris.Add(Base + 0);
	Tris.Add(Base + 2);
	Tris.Add(Base + 3);
}

// ---- B1: THE SAME RECTANGLE, AS A GRID OF CELLS ---------------------------
//
// THE DEFECT THIS EXISTS FOR, and it is a geometry defect wearing a shading
// costume. A greedy rectangle out of vxc::lakeSheetRects can be a KILOMETRE
// long, and it arrives here as FOUR vertices. World Position Offset is a
// per-VERTEX displacement, so a wave field the material computes perfectly
// moves those four corners and interpolates a plane between them: the crests
// are authored, evaluated, and invisible. Every wind-wave and ripple term in
// M_WaterVoxel has been shipping into a surface with no vertices to push since
// the sheet existed. This function is the vertices.
//
// SAME CONVENTION, DELIBERATELY BY REUSE. It calls AppendRectQuad per cell
// rather than restating the vertex layout, so the colour (255,255,255,0), the
// world-planar metre UVs anchored at the basin bbox corner, the up normal, the
// (1,0,0) tangent and the NO-WELDING rule are the same by construction and
// cannot drift. Four verts per cell is therefore the cost, and it is the right
// cost here: welding a shared grid would save ~3x the vertices and reintroduce
// exactly the T-junction class the no-weld rule exists to refuse -- against a
// neighbouring rectangle that is NOT tessellated, which is every rectangle at
// the disc boundary.
//
// NO STITCHING AGAINST THE GREEDY RECTANGLES AROUND IT, AND THAT IS A
// DEPENDENCY, NOT AN OVERSIGHT. A tessellated cell edge meets a greedy
// rectangle's long edge at a T-junction, and a T-junction cracks the moment
// the two sides are displaced by different amounts. It does not crack here
// because Tools/create_water_voxel_material.py fades WPO to IDENTICALLY ZERO
// between WaveWpoFadeStartM (55 m) and WaveWpoFadeEndM (72 m), and the
// tessellation disc's radius (voxel.Water.WaveTessRadiusM, 80 m) is strictly outside
// fade end -- so displacement is zero on BOTH sides of every seam this
// function creates. IF THAT FADE IS EVER REMOVED, RAISED PAST THE TESS RADIUS,
// OR THE TESS RADIUS IS DROPPED BELOW IT, THE SHEET CRACKS AT THE DISC EDGE.
// The two numbers are checked against each other in BeginPlay's log line, which
// is the only place a capture can be told which regime it was taken in.
//
// Cells are sized by COUNT, not by stepping a cursor: ceil(span / MaxCellUU)
// cells of span/N each, so the last cell lands exactly on the rectangle's own
// edge instead of accumulating a sliver, and no cell is ever larger than
// MaxCellUU. Returns the number of vertices appended -- the diagnostic the
// header's GetTessellatedVertCount reports.
int32 AppendRectQuadTessellated(const FBox2D& RectUU, double ZUU, const FVector2D& UVOriginUU, double MaxCellUU,
                                TArray<FVector>& Verts, TArray<int32>& Tris, TArray<FVector>& Normals,
                                TArray<FVector2D>& UVs, TArray<FColor>& Colors,
                                TArray<FProcMeshTangent>& Tangents)
{
	const double SpanX = RectUU.Max.X - RectUU.Min.X;
	const double SpanY = RectUU.Max.Y - RectUU.Min.Y;
	if (SpanX <= 0.0 || SpanY <= 0.0 || MaxCellUU <= 0.0)
	{
		return 0;
	}

	const int64 NX = FMath::Max<int64>(1, int64(FMath::CeilToDouble(SpanX / MaxCellUU)));
	const int64 NY = FMath::Max<int64>(1, int64(FMath::CeilToDouble(SpanY / MaxCellUU)));

	// A CEILING WITH A FALLBACK, not a check(). The caller only ever passes the
	// intersection of a rectangle with the tessellation square, whose side is
	// 2 * the tess radius (160 m at the default) -- 86 x 86 cells, ~30 k verts,
	// which is the plan's stated worst case. This bound therefore cannot be hit
	// by the shipped path; it exists so that a future caller passing a
	// kilometre-long rectangle degrades to the flat greedy quad (a wave that
	// does not move) instead of allocating a million vertices on the game
	// thread (a hitch that does).
	constexpr int64 kMaxCellsPerRect = 16384;
	if (NX * NY > kMaxCellsPerRect)
	{
		const int32 Before = Verts.Num();
		AppendRectQuad(RectUU, ZUU, UVOriginUU, Verts, Tris, Normals, UVs, Colors, Tangents);
		return Verts.Num() - Before;
	}

	const int32 Before = Verts.Num();
	for (int64 j = 0; j < NY; ++j)
	{
		// The last row/column takes the rectangle's own edge, so the cover is
		// exact rather than exact-to-rounding: a sliver here is a gap in the
		// water at the waterline, which is the one place the sheet is looked at
		// from a metre away.
		const double Y0 = RectUU.Min.Y + (SpanY * double(j)) / double(NY);
		const double Y1 = (j + 1 == NY) ? RectUU.Max.Y : RectUU.Min.Y + (SpanY * double(j + 1)) / double(NY);
		for (int64 i = 0; i < NX; ++i)
		{
			const double X0 = RectUU.Min.X + (SpanX * double(i)) / double(NX);
			const double X1 = (i + 1 == NX) ? RectUU.Max.X : RectUU.Min.X + (SpanX * double(i + 1)) / double(NX);
			AppendRectQuad(FBox2D(FVector2D(X0, Y0), FVector2D(X1, Y1)), ZUU, UVOriginUU, Verts, Tris, Normals,
			               UVs, Colors, Tangents);
		}
	}
	return Verts.Num() - Before;
}
} // namespace

AVoxelWaterSheetActor::AVoxelWaterSheetActor()
{
	PrimaryActorTick.bCanEverTick = true;

	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("LakeSheets"));
	SetRootComponent(Mesh);
	Mesh->SetMobility(EComponentMobility::Movable);
	// Cosmetic, exactly like the clipmap and the ocean plane: a sheet must never
	// be something a player can stand on out at 5 km. Being IN the water is the
	// datum's job (IsUnderwaterAtWorld, §5.3), not this mesh's.
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->bUseAsyncCooking = false;
	// Sheets are flat and horizontal; a shadow from one is a black rectangle on
	// its own lakebed, and the near-field water casts none either.
	Mesh->SetCastShadow(false);

	// Same load-in-constructor pattern AVoxelOceanActor and AVoxelClipmapActor
	// use, so no section ever renders with the engine default for a frame.
	WaterMaterial = Cast<UMaterialInterface>(
		StaticLoadObject(UMaterialInterface::StaticClass(), nullptr, TEXT("/Game/Voxel/M_WaterVoxel.M_WaterVoxel")));
}

UProceduralMeshComponent* AVoxelWaterSheetActor::GetOrCreateSheetComp(FSheet& Sheet)
{
	if (Sheet.Comp != nullptr)
	{
		return Sheet.Comp;
	}
	// Named for the log, not for lookup: nothing addresses these by name. The
	// serial suffix is load-bearing -- see CompNameSerial's comment in the
	// header for the displace-on-name-collision cost it avoids.
	UProceduralMeshComponent* C = NewObject<UProceduralMeshComponent>(
		this, *FString::Printf(TEXT("LakeBasin_%d_%d_%d_g%u"), Sheet.TileX, Sheet.TileY, Sheet.BasinId,
		                       ++CompNameSerial));
	C->SetupAttachment(Mesh);
	C->SetMobility(EComponentMobility::Movable);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->bUseAsyncCooking = false;
	C->SetCastShadow(false);
	C->RegisterComponent();
	// Global reg column only -- the LakeCreate source column counts the mesh
	// section that ALWAYS follows in the same frame (RebuildSheet's only caller
	// of this function), and counting both would report one component dirtied
	// as two units of EndOfFrameUpdates work.
	VoxelEofLedger::CountRegister();
	Sheet.Comp = C;
	return C;
}

void AVoxelWaterSheetActor::BeginPlay()
{
	Super::BeginPlay();

	if (!WaterMaterial)
	{
		UE_LOG(LogVoxelEarth, Warning,
		       TEXT("M_WaterVoxel not found at /Game/Voxel/M_WaterVoxel -- lake sheets using the engine default "
		            "material. The frame is NOT comparable to a near-field water capture."));
	}

	// THE SHORE CLIP IS OPT-IN AND THE SHEET IS THE ONE OPTER-IN. The material
	// asset ships WaterShoreClipEnabled=0 because bathy validity is not a wet
	// test: the shore plane is LAKE-only, so every river ribbon, poured pool and
	// near-field column outside a baked basin reads shore_m=-100 with validity 1
	// and would be clipped to nothing (2026-09-04 review finding #1 -- the fix
	// that deleted the water it was fixing). Those consumers stay on the bare
	// asset. The SHEET over-covers its basin by up to a mask cell, and that
	// over-cover at grazing angles IS the black band (RGB 26,30,36 tracing the
	// mask boundary; docs/lake-sheet-black-band-2026-08-29.md), so the sheet
	// alone turns the clip on -- through the same MID the diagnostic switches
	// use, created here unconditionally so they compose. `-VoxelWaterShoreClip=0`
	// is the control arm (MID with clip 0 == the asset default). Logged in both
	// positions: this fix sat authored-but-inert once already, and a silent arm
	// cannot be told apart from a mis-spelled one.
	if (WaterMaterial)
	{
		int32 ShoreClip = 1;
		FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterShoreClip="), ShoreClip);
		if (!SheetMaterialOverride)
		{
			SheetMaterialOverride = UMaterialInstanceDynamic::Create(WaterMaterial, this);
		}
		if (SheetMaterialOverride)
		{
			SheetMaterialOverride->SetScalarParameterValue(
				TEXT("WaterShoreClipEnabled"), ShoreClip != 0 ? 1.0f : 0.0f);
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: shore-SDF clip %s on the sheet MID (asset default is OFF; "
			            "ribbons and near-field water deliberately keep it off)."),
			       ShoreClip != 0 ? TEXT("ARMED") : TEXT("DISARMED by -VoxelWaterShoreClip=0"));
		}
	}

	// -VoxelWaterDepthAuthority: see SheetMaterialOverride's declaration for the
	// defect this exists for and for why the engine's absorption half is the
	// unbounded one. Since the shore clip above landed, the ONE sheet MID
	// always exists and these switches write onto it rather than deciding
	// whether it exists; an unpassed switch leaves its parameter at the
	// material's own default, so the control arm is VALUE-identical to the
	// bare asset rather than instance-free -- judge arms by the logged values.
	//
	// LOGGED IN BOTH POSITIONS. A silent switch cannot be told apart from a
	// mis-spelled one when the image comes back unchanged.
	// -VoxelWaterFoamGain=<float>: overrides BathyFoamGain for SHEETS ONLY.
	//
	// THE DIAGNOSTIC THIS EXISTS FOR. The black band around every lake traces the
	// SHORELINE, which is exactly where shore foam peaks -- and on a Single Layer
	// Water material `Opacity` is not transparency, it is "the fraction of the
	// pixel covered by the OPAQUE MATERIAL SITTING ON the water"
	// (BasePassPixelShader.usf:1140). This material wires
	// `Opacity = saturate(foam)` and `BaseColor = lerp(black, foamTint, foam)`,
	// so the foam term controls BOTH coverage and colour, and create_water_voxel_
	// material.py records what coverage-without-colour looks like: "a black
	// BaseColor with Specular 0.5 and Roughness 0.08, i.e. a dark mirror, plus
	// the emissive sky reflection". That is the band, measured at RGB (26,30,36),
	// blue-dominant.
	//
	// Gain 0 removes the shore foam AND the coverage it drives together -- the
	// generator's own comment insists they are one mechanism, not two, so a null
	// result here exonerates the foam path outright rather than leaving it half
	// suspected.
	// -VoxelWaterMatScalar=Name:Value[,Name:Value...] -- set ANY scalar parameter
	// on the sheet material, sheets only.
	//
	// A GENERIC PROBE, added after two named switches each cost a rebuild to
	// answer one question about a graph with dozens of parameters. The lake band
	// has now survived BathyDepthAuthority=1.0 and BathyFoamGain=0, and the next
	// candidates are all scalars on the same material -- so the bottleneck was
	// the build/capture cycle, not the thinking.
	//
	// DIAGNOSTIC, NOT A SHIPPING KNOB. It cannot validate a parameter name: a
	// typo sets nothing and looks exactly like a null result, which is the trap
	// this session hit four separate times. So it LOGS EVERY ASSIGNMENT -- read
	// the log, not the command line.
	{
		FString ScalarSpec;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterMatScalar="), ScalarSpec)
		    && !ScalarSpec.IsEmpty() && WaterMaterial)
		{
			if (!SheetMaterialOverride)
			{
				SheetMaterialOverride = UMaterialInstanceDynamic::Create(WaterMaterial, this);
			}
			TArray<FString> Pairs;
			ScalarSpec.ParseIntoArray(Pairs, TEXT(","), true);
			for (const FString& Pair : Pairs)
			{
				FString Name, Value;
				if (Pair.Split(TEXT(":"), &Name, &Value) && SheetMaterialOverride)
				{
					const float V = FCString::Atof(*Value);
					SheetMaterialOverride->SetScalarParameterValue(FName(*Name), V);
					UE_LOG(LogVoxelEarth, Log,
					       TEXT("Lake sheets: material scalar '%s' set to %.4f (diagnostic override)."),
					       *Name, V);
				}
			}
		}
	}

	float FoamGain = -1.0f;
	const bool bFoamOverride =
		FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterFoamGain="), FoamGain) && WaterMaterial;

	float Authority = -1.0f;
	if (bFoamOverride)
	{
		if (!SheetMaterialOverride)
		{
			SheetMaterialOverride = UMaterialInstanceDynamic::Create(WaterMaterial, this);
		}
		if (SheetMaterialOverride)
		{
			SheetMaterialOverride->SetScalarParameterValue(TEXT("BathyFoamGain"), FoamGain);
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: BathyFoamGain OVERRIDDEN to %.3f (material default 0.55). This "
			            "moves BOTH the shore foam and the water COVERAGE it drives -- on SLW, Opacity "
			            "is opaque-material coverage, not alpha."),
			       FoamGain);
		}
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterDepthAuthority="), Authority) && WaterMaterial)
	{
		Authority = FMath::Clamp(Authority, 0.0f, 1.0f);
		if (!SheetMaterialOverride)
		{
			SheetMaterialOverride = UMaterialInstanceDynamic::Create(WaterMaterial, this);
		}
		if (SheetMaterialOverride)
		{
			SheetMaterialOverride->SetScalarParameterValue(TEXT("BathyDepthAuthority"), Authority);
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: BathyDepthAuthority OVERRIDDEN to %.3f for sheets only (material default "
			            "is 0.85). At 1.0 the engine's along-view-ray absorption term is switched off wherever "
			            "the bake answered, which is what removes the near-black band on the over-covered cells "
			            "outside the basin. Where the bake did NOT answer (validity 0) the engine term is "
			            "unchanged."),
			       Authority);
		}
	}
	else
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("Lake sheets: BathyDepthAuthority left at the material's own value "
		            "(-VoxelWaterDepthAuthority not passed). This is the CONTROL arm."));
	}

	int32 Flag = 1;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheets="), Flag))
	{
		bEnabled = (Flag != 0);
	}
	// -VoxelWaterMarkerOnly=1 wins over the switch above: the marker view exists
	// so ONE renderer describes where water is. Sheets draw the same baked lakes
	// in blue at ~15 m rectangles, which is a second, coarser answer overlaid on
	// the one being judged. See VoxelWaterSubsystem.cpp's note at the near-field
	// call site for the report that prompted this.
	if (FParse::Param(FCommandLine::Get(), TEXT("VoxelWaterMarkerOnly"))
	    || (FParse::Value(FCommandLine::Get(), TEXT("VoxelWaterMarkerOnly="), Flag) && Flag != 0))
	{
		bEnabled = false;
	}

	// SCAN RADIUS: 10 km, and this is a MEMORY decision, not a taste one.
	//
	// The obvious default was AVoxelClipmapActor::OuterHalfExtentUU() -- put
	// water on all the ground the far terrain draws. At the shipped cascade that
	// is 65.5 km, which is 81 fine tiles, and a resident fine tile holds its
	// whole compressed .vxtl in memory (FineTileSampler keeps `bytes_`; the
	// lazily-decoded blocks are on top of that). The exemplar set runs 30-51 MB
	// a tile, so that default would have asked for ~3 GB of tile bytes to draw
	// water the player cannot resolve anyway.
	//
	// 10 km is the range the plan's own verification names ("capture at 2-10
	// km"), and its square touches at most 3x3 tiles. What it costs is stated
	// after each gather rather than assumed, and -VoxelLakeSheetRangeM moves it.
	// The honest limitation: a basin further out than this draws no sheet, so a
	// 30 km vista still has dry bowls in it. That is a bounded, logged absence
	// instead of an unbounded load.
	ScanRadiusUU = 1000000.0; // 10 km
	double RangeM = 0.0;
	if (FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheetRangeM="), RangeM) && RangeM > 1.0)
	{
		ScanRadiusUU = RangeM * 100.0;
	}
	FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheetCells="), TargetCellsPerSide);
	TargetCellsPerSide = FMath::Clamp(TargetCellsPerSide, 8, 2048);
	FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheetFineM="), FineBandRadiusM);
	FineBandRadiusM = FMath::Clamp(FineBandRadiusM, 0.0, 4000.0);
	FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheetSnapPx="), LodSnapPx);
	LodSnapPx = FMath::Clamp(LodSnapPx, 1, 4096);
	FParse::Value(FCommandLine::Get(), TEXT("VoxelLakeSheetBands="), MaxBands);
	MaxBands = FMath::Clamp(MaxBands, 1, int32(UVoxelWaterSubsystem::FLakeSheetLod::kMaxBands));

	// -VoxelWaveTessM=<metres>: B1's tessellation radius, and 0 is the OFF
	// control. IT NO LONGER OWNS THE VALUE -- it SETS voxel.Water.WaveTessRadiusM
	// and the cvar is read at every rebuild. The flag is kept, spelled and
	// logged exactly as before, because live leg commands and docs pass it; what
	// changed is only who the authority is.
	//
	// SET BY GAME SETTING, NOT BY COMMANDLINE, and the choice is not cosmetic.
	// SetByCommandline outranks SetByGameSetting, so a leg that passed the flag
	// would leave the "Water Wave Detail" row SILENTLY INERT for the rest of
	// that session -- clicks that persist a value and change nothing. At equal
	// priority the last writer wins, and this runs after
	// UVoxelFrontEndSubsystem::Initialize's ApplyAll(), so the flag still beats
	// the persisted setting at boot exactly as a leg expects.
	{
		double TessM = 0.0;
		if (FParse::Value(FCommandLine::Get(), TEXT("VoxelWaveTessM="), TessM))
		{
			CVarVoxelWaterWaveTessRadiusM->Set(float(FMath::Max(0.0, TessM)), ECVF_SetByGameSetting);
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: -VoxelWaveTessM=%.0f applied to voxel.Water.WaveTessRadiusM (the flag "
			            "sets the cvar; the cvar is what every rebuild reads)."),
			       FMath::Max(0.0, TessM));
		}
		// CLAMPED OUT LOUD. Past FineBandRadiusM the sheet is no longer meshing
		// at one fine pixel, so cells emitted out there would not be the finest
		// band's -- and a switch that silently did half of what it was asked is
		// the trap this file has already been bitten by four times (see
		// -VoxelWaterMatScalar's note above). The clamp itself now lives in
		// WaveTessRadiusMNow() so it also covers a value set from the console or
		// the settings panel; this says so once, for the value present at boot.
		const double RequestedM = double(CVarVoxelWaterWaveTessRadiusM.GetValueOnGameThread());
		if (RequestedM > FineBandRadiusM)
		{
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("Lake sheets: voxel.Water.WaveTessRadiusM=%.0f is beyond the finest band (%.0f m) and "
			            "is CLAMPED to it on every read. Tessellating past the fine band would emit cells at a "
			            "decimation the extent mask does not express there; raise -VoxelLakeSheetFineM first if "
			            "you want more."),
			       RequestedM, FineBandRadiusM);
		}
	}

	const bool bOnePath = !UVoxelWaterSubsystem::ShouldMeshImplicitLakes();
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Lake sheets: %s, scan radius %.0f m, %d cells/basin side, up to %d LOD band(s), finest %.0f m at "
	            "1.875 m/cell (re-centred every %d fine px = %.0f m). Lake basins: %s."),
	       bEnabled ? TEXT("enabled") : TEXT("DISABLED (-VoxelLakeSheets=0)"), ScanRadiusUU / 100.0,
	       TargetCellsPerSide, MaxBands, FineBandRadiusM, LodSnapPx, double(LodSnapPx) * 1.875,
	       bOnePath ? TEXT("ONE rendering path -- the sheet owns them at every range, no near-field hole")
	                : TEXT("TWO rendering paths (voxel.Water.MeshImplicitLakes=1) -- a hole is cut for the "
	                       "near-field disc"));

	// THE WAVE LINE, SEPARATE FROM THE LINE ABOVE because it is the one a wave
	// capture has to be read against. It names the tessellation radius, the
	// cell size, and -- explicitly -- the material fade window the no-stitching
	// argument depends on, so a capture with cracks at the disc edge can be
	// diagnosed from the log instead of from the material graph.
	const double TessNowM = WaveTessRadiusMNow();
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("Lake sheets: wave tessellation %s -- radius %.0f m, cells <= 1.875 m (one fine pixel), "
	            "finest band only. NO stitching against the greedy rectangles outside the disc: that seam "
	            "is safe ONLY because M_WaterVoxel fades WPO to zero by ~72 m, i.e. INSIDE this radius. "
	            "voxel.Water.WaveTessRadiusM=0 (or -VoxelWaveTessM=0) is the OFF control, and the \"Water "
	            "Wave Detail\" settings row moves the same cvar mid-session."),
	       TessNowM > 0.0 ? TEXT("ENABLED") : TEXT("DISABLED (radius 0)"), TessNowM);
}

double AVoxelWaterSheetActor::WaveTessRadiusMNow() const
{
	// GetValueOnGameThread, and every caller is on the game thread: the sheet's
	// rebuild, its LOD arithmetic and its census line all run in Tick. If a
	// render-thread caller ever appears it must NOT reach for the render-thread
	// accessor here -- it must be handed the value the rebuild used, or the
	// geometry and the value describing it come from different frames.
	const double RequestedM = FMath::Max(0.0, double(CVarVoxelWaterWaveTessRadiusM.GetValueOnGameThread()));
	return FMath::Min(RequestedM, FineBandRadiusM);
}

namespace
{
// A camera further than this from its own pawn is not a camera POSITION, it is
// a camera CACHE that has not caught up with where the pawn was placed. The
// largest legitimate separation anything in this project produces is the
// third-person boom -- 2.5 m back and 0.4 m to the active shoulder
// (VoxelMovementTuning::ThirdPersonBoomBackUU / ThirdPersonBoomRightUU, applied
// per frame in AVoxelEarthFlyPawn::UpdateThirdPersonCamera, no spring arm). 100
// m is two orders of magnitude past that, so this cannot separate a real first-
// or third-person camera from its pawn; it can only catch a cache still holding
// a position the pawn has left. Deliberately loose, because what it falls back
// to is not a refusal: it is the PAWN's location, which at a 10 km gather
// radius with 1 km of hysteresis answers every question this actor asks
// identically.
constexpr double kMaxCameraPawnSeparationUU = 10000.0; // 100 m
} // namespace

bool AVoxelWaterSheetActor::GetCameraLocationUU(FVector& Out) const
{
	// WHY THIS IS NO LONGER THE FALLBACK CHAIN AVoxelClipmapActor::
	// GetCameraLocationUU, AVoxelRiverRibbonActor and AVoxelOceanActor::
	// UpdateFollowPlane still use ("camera manager first, pawn second").
	//
	// MEASURED 2026-08-23, the owner's PIE session. APlayerCameraManager is
	// spawned with the PlayerController, well before AVoxelEarthGameMode::
	// RestartPlayer picks a spawn column and puts a pawn in it, and until its
	// first UpdateCamera its cached POV is the DEFAULT: (0,0,0). The old chain
	// took that default for a camera position, so this actor's FIRST basin
	// gather ran at the world origin and logged
	//     "Lake sheets: scanning 4 fine tile(s) within 10000 m of (0, 0)"
	// -- tiles (0,0), (0,-1), (-1,0) and (-1,-1), the four that meet at the
	// origin, about 61 km from where the player actually spawned and none of
	// them ever baked (only 15 fine tiles exist, all in the -3..-15 band).
	// 10,814 elevation reads went into them over the first ~2.5 s, until the
	// pawn appeared and the gather re-ran correctly at (-6144000, -6144000).
	// The leak counter never moved again all session: ONE gather, at a fake
	// position, and the session was filed as a streaming regression on it.
	//
	// WHY THAT IS NOT JUST A NOISY COUNTER. A fine-tier query into a
	// non-resident tile does NOT fall back to the coarse tier. It returns SEA
	// LEVEL -- FVoxelFineTileStreamer::ReportGateLeak_Locked ends
	// `return Sampler_.elevationMm(px,py)` -- i.e. ground no other client
	// computes. And under -unattended the FIRST such query is UE_LOG(Fatal)
	// (SetLeakIsFatal, driven from FApp::IsUnattended), so on the headless path
	// this is not a warning, it KILLS THE RUN. tools/voxel-capture.ps1 launches
	// -unattended with a fine tile dir and does NOT pass
	// -VoxelFineTileGateFatal=0; tools/bv12-river-captures.ps1:43 and
	// tools/bv12-shoot-one.ps1:37 do, which is this bug being worked around
	// rather than diagnosed.
	//
	// THE CONDITION IS "IS THERE A POSSESSED PAWN", NOT "IS THE CAMERA AWAY
	// FROM THE ORIGIN". A player standing legitimately at (0,0) is a position,
	// not a default, and a test that cannot tell those apart would refuse that
	// player lakes forever. RestartPlayer places the pawn on a ground-derived
	// column before it possesses, so a pawn EXISTING is the honest evidence
	// that a real position exists -- it is the event actually being waited for,
	// not a proxy for it.
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const APlayerController* PC = World->GetFirstPlayerController();
	if (!PC)
	{
		return false;
	}
	const APawn* Pawn = PC->GetPawn();
	if (!Pawn)
	{
		// No pawn: whatever the camera manager reports is a default, not a place.
		return false;
	}
	const FVector PawnUU = Pawn->GetActorLocation();
	if (const APlayerCameraManager* Cam = PC->PlayerCameraManager)
	{
		// The pawn existing closes the "before RestartPlayer" window but NOT the
		// one-frame window after it: the camera manager fills its cache during
		// the PlayerController's own tick, and nothing orders this actor after
		// that. One frame at (0,0) is still one fatal gate leak on the
		// unattended path, so the cache is believed only when it is actually ON
		// the pawn. Every SetViewTarget in this project targets PC->GetPawn()
		// (VoxelSkyLadderFixture.cpp:392, VoxelSweBreachFixture.cpp:477,
		// VoxelWorldSubsystem.cpp:20948), so "on the pawn" is the whole truth
		// about where this camera is allowed to be.
		const FVector CamUU = Cam->GetCameraLocation();
		if (FVector::DistSquared(CamUU, PawnUU) < FMath::Square(kMaxCameraPawnSeparationUU))
		{
			Out = CamUU;
			return true;
		}
	}
	// Not a failure and not a guess: the pawn is a PLACED actor. This is also
	// the answer on the frame a pose fixture teleports the pawn kilometres
	// (SetActorLocation with TeleportPhysics) and the camera cache still holds
	// the pose before it.
	Out = PawnUU;
	return true;
}

namespace
{
// A fine pixel, in UU. 1.875 m; the fine tier's pixel, see tiles.h.
constexpr double kFinePixelUU = 187.5;
} // namespace

int32 AVoxelWaterSheetActor::StepForBasin(double SpanUU) const
{
	// SpanUU is the basin's longer bbox side. A fine pixel is 1.875 m; the step
	// is whatever makes the longer side about TargetCellsPerSide cells, floored
	// at 1 so a small pond meshes at full lattice resolution and never coarser
	// than the ground it sits in.
	//
	// THIS IS NOW THE COARSEST BAND'S STEP, not the whole basin's. It is
	// unchanged on purpose: the far field keeps exactly the decimation it had, so
	// the band ladder is a strict addition near the camera rather than a retune
	// of everything. See BuildLodForBasin.
	const double Cells = SpanUU / kFinePixelUU;
	return FMath::Max(1, FMath::CeilToInt(Cells / double(TargetCellsPerSide)));
}

FIntPoint AVoxelWaterSheetActor::LodKeyForCamera(const FVector& CamUU) const
{
	const double CellUU = double(FMath::Max(LodSnapPx, 1)) * kFinePixelUU;
	return FIntPoint(int32(FMath::FloorToDouble(CamUU.X / CellUU)),
	                 int32(FMath::FloorToDouble(CamUU.Y / CellUU)));
}

FVector2D AVoxelWaterSheetActor::SnappedCamXY(const FVector& CamUU) const
{
	// THE BANDS ARE CENTRED ON THE CELL, NOT ON THE CAMERA, and that is what
	// makes a basin's geometry a pure function of its LodKey. Centre them on the
	// raw camera instead and two things go wrong at once: the mesh a basin was
	// last built with depends on where inside the cell the camera happened to be,
	// and the uniform/banded verdict becomes a bare distance threshold that
	// flaps -- rebuilding a basin every frame while the camera jitters across it.
	const double CellUU = double(FMath::Max(LodSnapPx, 1)) * kFinePixelUU;
	const FIntPoint Key = LodKeyForCamera(CamUU);
	return FVector2D((double(Key.X) + 0.5) * CellUU, (double(Key.Y) + 0.5) * CellUU);
}

bool AVoxelWaterSheetActor::TessBoxForCamera(const FVector& CamUU, FBox2D& OutBoxUU) const
{
	OutBoxUU = FBox2D(ForceInit);
	const double TessRadiusM = WaveTessRadiusMNow();
	if (TessRadiusM <= 0.0)
	{
		return false; // radius 0 -- the OFF control ("Water Wave Detail" off, or -VoxelWaveTessM=0)
	}
	// THE SNAPPED CELL CENTRE, exactly the one the LOD bands are built around
	// (SnappedCamXY). Sharing the centre is what makes the tessellated geometry
	// a pure function of LodKey: the box moves only when the key does, so the
	// existing "rebuild when the key changes" trigger is necessary and
	// sufficient for the tessellation too and no second trigger is needed.
	const FVector2D Snap = SnappedCamXY(CamUU);
	const double RadiusUU = TessRadiusM * 100.0;
	OutBoxUU = FBox2D(FVector2D(Snap.X - RadiusUU, Snap.Y - RadiusUU),
	                  FVector2D(Snap.X + RadiusUU, Snap.Y + RadiusUU));
	return true;
}

bool AVoxelWaterSheetActor::IsBandedAtCamera(const FSheet& Sheet, const FVector& CamUU) const
{
	if (Sheet.StepPx <= 1)
	{
		return false; // a pond already meshes at full lattice resolution everywhere
	}
	// The outermost BOUNDED band. How many rungs a ladder has depends on Base, so
	// which radius is the last bounded one does too -- ask BuildLadder, which is
	// the one place the ladder is constructed, rather than restating it here.
	UVoxelWaterSubsystem::FLakeSheetLod Lod;
	BuildLadder(Sheet, Lod);
	if (Lod.NumBands <= 1)
	{
		return false;
	}
	const double OuterBandedUU = double(Lod.RadiusPx[Lod.NumBands - 2]) * kFinePixelUU;
	const FVector2D Snap = SnappedCamXY(CamUU);
	const double DX = FMath::Max(FMath::Max(Sheet.MinXUU - Snap.X, Snap.X - Sheet.MaxXUU), 0.0);
	const double DY = FMath::Max(FMath::Max(Sheet.MinYUU - Snap.Y, Snap.Y - Sheet.MaxYUU), 0.0);
	return FMath::Max(DX, DY) < OuterBandedUU;
}

void AVoxelWaterSheetActor::BuildLadder(const FSheet& Sheet,
                                        UVoxelWaterSubsystem::FLakeSheetLod& OutLod) const
{
	const int32 Base = Sheet.StepPx;

	// THE LADDER IS {1, 2, 4, Base}, keeping only the rungs finer than Base.
	//
	// Base is NOT rounded to a power of two, and the far field therefore keeps
	// exactly the decimation it already had: the exemplar 1564 m basin picks 7,
	// and 7 is what its outer sheet still meshes at. voxel-core aligns the band
	// edges to the LCM of the rungs (28 px here) rather than demanding they
	// divide each other, which is what makes an arbitrary Base affordable -- see
	// lakeSheetRectsBanded. Rounding Base up instead would have coarsened the
	// far field by up to 2x to buy nothing.
	const int32 BandCap = FMath::Clamp(MaxBands, 1, int32(UVoxelWaterSubsystem::FLakeSheetLod::kMaxBands));
	int32 Steps[UVoxelWaterSubsystem::FLakeSheetLod::kMaxBands];
	int32 N = 0;
	for (const int32 Candidate : {1, 2, 4})
	{
		if (Candidate < Base && N < BandCap - 1)
		{
			Steps[N++] = Candidate;
		}
	}
	Steps[N++] = Base; // always the outermost, always unbounded

	OutLod = UVoxelWaterSubsystem::FLakeSheetLod();
	OutLod.NumBands = N;

	const int32 FineRadiusPx = FMath::Max(1, FMath::CeilToInt((FineBandRadiusM * 100.0) / kFinePixelUU));
	for (int32 k = 0; k < N; ++k)
	{
		OutLod.StepPx[k] = Steps[k];
		// Doubling radii against doubling-ish steps: a rectangle's SCREEN size is
		// then roughly constant across the bands, which is the only sense in which
		// a decimation ladder can be even.
		OutLod.RadiusPx[k] = FineRadiusPx << k;
	}
}

void AVoxelWaterSheetActor::BuildLodForBasin(const FSheet& Sheet, const FVector& CamUU,
                                             UVoxelWaterSubsystem::FLakeSheetLod& OutLod,
                                             bool& bOutUniform) const
{
	BuildLadder(Sheet, OutLod);
	// The CELL centre, not the camera -- see SnappedCamXY. The mesh a basin
	// carries is then determined entirely by its LodKey, which is what makes
	// "rebuild when the key changes" both necessary and sufficient.
	const FVector2D Snap = SnappedCamXY(CamUU);
	OutLod.CamXUU = Snap.X;
	OutLod.CamYUU = Snap.Y;

	// UNIFORM means "every block of this basin lands in the last, unbounded
	// band", so its geometry does not depend on the camera at all and it can
	// leave the rebuild rotation for good.
	bOutUniform = !IsBandedAtCamera(Sheet, CamUU);
}

bool AVoxelWaterSheetActor::HoleForDatum(double SurfaceZUU, FBox2D& OutHoleUU) const
{
	const UWorld* World = GetWorld();
	const UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return false;
	}
	// ONE RENDERING PATH FOR LAKE BASINS (voxel.Water.MeshImplicitLakes = 0).
	//
	// The hole exists only because two renderers drew the same lake water and an
	// overlap of two coplanar surfaces z-fights. With the near-field voxel disc
	// retired for lakes there is no second renderer to hand over to -- and the
	// hole EDGE is itself the seam the owner is looking at: measured at +36.4/255
	// across adjacent pixels at matched depth and view angle. So no near disc, no
	// hole, no edge.
	//
	// THE ONE THING THAT STILL EARNS A HOLE is water the CA has taken over.
	// Digging into a lake mobilizes its bricks, and mobilized water IS still
	// meshed near-field (it always was -- the implicit sweep never drew it). That
	// water is real, moving, and must not have a flat sheet drawn on top of it.
	// So the hole is now keyed on the CA actually owning water at this datum
	// inside the disc, which is false for every untouched lake in the world and
	// costs one empty-set test to say so.
	if (!UVoxelWaterSubsystem::ShouldMeshImplicitLakes() && !Water->HasMobilizedWaterInImplicitDisc(SurfaceZUU))
	{
		return false;
	}

	FBox2D DiscXY(ForceInit);
	double MinZ = 0.0, MaxZ = 0.0;
	if (!Water->GetImplicitWaterDiscUU(DiscXY, MinZ, MaxZ))
	{
		return false;
	}
	// THE Z TEST IS WHAT MAKES THE HOLE HONEST. RefreshImplicitWater's disc is
	// bounded in z by kImplicitRadiusBricksZ, so a camera 30 m above this lake is
	// meshing NO water at its datum -- cutting a hole for it would punch a 52 m
	// square of missing water out of a lake seen from a ridge, which is the very
	// defect this actor exists to fix, reintroduced at close range.
	if (SurfaceZUU < MinZ || SurfaceZUU >= MaxZ)
	{
		return false;
	}
	OutHoleUU = DiscXY;
	return true;
}

namespace
{
// HOW FAR A SHEET MAY BE MOVED BY A TRANSFORM BEFORE IT HAS TO BE RE-MESHED.
// 50 UU = 0.5 m, the plan's B3 number.
//
// WHY A TRANSFORM IS NOT ENOUGH FOREVER, since a flat sheet translated in z is
// geometrically exact at any offset. Two things about a sheet are functions of
// its DATUM and not of its height: the near-field hole (HoleForDatum tests the
// implicit disc's z span against the surface -- move the surface far enough and
// the basin owes a hole it does not have, or carries one it no longer owes),
// and, from Phase C4, the EXTENT itself (a falling tide uncovers the shallows,
// and a translated sheet would draw water over dry ground rather than
// retreating from it). Half a metre is well inside both tolerances at the
// 1.875 m raster the extent is quantised to.
constexpr double kTideRebuildDriftUU = 50.0;
} // namespace

double AVoxelWaterSheetActor::CurrentSurfaceZUUForBasin(const FSheet& Sheet,
                                                        const UVoxelWaterSubsystem* Water) const
{
	if (Water == nullptr)
	{
		return Sheet.SurfaceZUU;
	}
	// PHASE C REPLACED THE BODY, NOT THE CALL SITES (exactly as the header
	// promised): the honest answer is now a PER-BASIN query through the one
	// datum seam -- the basin ledger wrapped by vxc::TidalDatumSource -- so
	// the sheet rides whatever the near field fills to, by construction. An
	// oracle-qualified rock pool rides the tide while connected and holds
	// full at its sill when cut off; an inland lake (and everything else the
	// oracle refuses, which with the tide dark is everything) answers its
	// ledger datum, which is exactly the number the gather wrote into
	// Sheet.SurfaceZUU -- so the drift below is 0.0 there, not a rounding
	// error there. Phase B's "any basin standing AT the sea datum rides"
	// epsilon test is gone WITH ITS FALSE POSITIVES: a coastal lagoon parked
	// a few centimetres off the sea no longer moves unless the oracle proves
	// the sea actually reaches it.
	double NowZUU = 0.0;
	if (Water->GetBasinDatumNowZUU(Sheet.TileX, Sheet.TileY, Sheet.BasinId, NowZUU))
	{
		return NowZUU;
	}
	// Tile/basin not resolvable this tick: keep the gathered height rather
	// than inventing one -- the same "could not resolve is not dry" doctrine
	// as the extent masks.
	return Sheet.SurfaceZUU;
}

void AVoxelWaterSheetActor::ApplyTideNudge(const UVoxelWaterSubsystem* Water)
{
	// THE EARLY OUT IS THE FEATURE. The subsystem's datum is quantised (25 mm)
	// and rate-limited (2 s), so on the order of 99.9% of ticks it has not
	// moved -- and on those ticks this function must not touch a single
	// component. Walking ~500 basins to write an unchanged SetRelativeLocation
	// would dirty ~500 render transforms every frame, which is the same shape
	// of cost as the 9 ms proxy recreate this actor was restructured to remove.
	//
	// EXACT ==, ON PURPOSE. The value being compared is a QUANTISED datum, so
	// it either stepped or it did not; a tolerance here would be a second,
	// disagreeing quantum bolted onto the first one.
	const double TideUU =
		Water ? (Water->SeaSurfaceZNowUU() - UVoxelWaterSubsystem::SeaLevelZUU()) : 0.0;
	if (bTideOffsetApplied && TideUU == AppliedTideOffsetUU)
	{
		return;
	}
	AppliedTideOffsetUU = TideUU;
	bTideOffsetApplied = true;

	int32 Nudged = 0;
	int32 Flagged = 0;
	for (FSheet& S : Sheets)
	{
		if (!S.bBuilt || S.Comp == nullptr)
		{
			continue; // nothing drawn yet; it will be built at the current datum
		}
		const double Drift = CurrentSurfaceZUUForBasin(S, Water) - S.BuiltSurfaceZUU;
		// NUDGE FIRST, FLAG SECOND. The rebuild is queued into the existing
		// one-basin-per-tick rotation, so a tide step that drifts N basins takes
		// N ticks to re-mesh them; the transform keeps every one of them at the
		// right height for the whole of that drain instead of leaving them at
		// the old waterline until their turn comes.
		S.Comp->SetRelativeLocation(FVector(0.0, 0.0, Drift));
		++Nudged;
		if (FMath::Abs(Drift) > kTideRebuildDriftUU)
		{
			// bBuilt = false is the ONE enqueue mechanism this actor has (see
			// the round-robin's trigger). Deliberately not a second queue: a
			// basin that is also LOD-stale must rebuild once, not twice.
			S.bBuilt = false;
			++Flagged;
		}
	}
	TideNudgedSheets += Nudged;
	TideRebuilds += Flagged;

	if (Nudged > 0 || Flagged > 0)
	{
		// ONE LINE PER DATUM STEP, which is at most one per MinStepIntervalS
		// (2 s). This is the sheet half of the tide's engagement proof: the
		// subsystem can log datum steps all day and the water still not move if
		// this never fires.
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("Lake sheets: tide datum step -- offset %.1f UU, %d sheet(s) nudged, %d queued for rebuild "
		            "(drift > %.0f UU). Cumulative: nudged=%d rebuilds=%d."),
		       TideUU, Nudged, Flagged, kTideRebuildDriftUU, TideNudgedSheets, TideRebuilds);
	}
}

bool AVoxelWaterSheetActor::RebuildSheet(FSheet& Sheet, const FVector& CamUU)
{
	UWorld* World = GetWorld();
	UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water || !Mesh)
	{
		return false;
	}

	UVoxelWaterSubsystem::FLakeSheetBasin Basin;
	Basin.TileX = Sheet.TileX;
	Basin.TileY = Sheet.TileY;
	Basin.BasinId = Sheet.BasinId;
	// PHASE C4 LANDED AND THIS LINE DID NOT HAVE TO CHANGE, which is the
	// seam doing its job: the subsystem's rect builder routes an oracle-tidal
	// basin's extent through extentMaskAtDatum at the CURRENT seam datum
	// internally (ResolveExtentMask), keyed by tile/basin id -- the SurfaceZUU
	// on this struct is identification-and-height for the gather compare, not
	// the extent's input. The geometry below still meshes at SurfaceNowZUU,
	// the same seam number, so a tidal pool's outline and height move together.
	Basin.SurfaceZUU = Sheet.SurfaceZUU;

	// ---- B3: THE SURFACE THIS BUILD IS FOR ---------------------------------
	//
	// Everything below meshes at SurfaceNowZUU, and BuiltSurfaceZUU records it
	// so the per-tick nudge measures its drift from what was actually built
	// rather than from the baked datum. On a run with no tide the two are the
	// same number and this is a rename.
	const double SurfaceNowZUU = CurrentSurfaceZUUForBasin(Sheet, Water);
	Sheet.BuiltSurfaceZUU = SurfaceNowZUU;

	UVoxelWaterSubsystem::FLakeSheetLod Lod;
	bool bUniform = true;
	BuildLodForBasin(Sheet, CamUU, Lod, bUniform);
	Sheet.bUniformCoarse = bUniform;
	Sheet.LodKey = LodKeyForCamera(CamUU);

	TArray<FBox2D> Rects;
	bool bResolved = false;
	Water->BuildLakeSheetRectsBanded(Basin, Lod, Rects, bResolved);
	if (!bResolved)
	{
		return false;
	}

	FBox2D Hole(ForceInit);
	// The CURRENT surface: the implicit disc is bounded in z, so whether this
	// basin owes a hole is a question about where its water is NOW, not about
	// where the bake put it.
	Sheet.bHadHole = HoleForDatum(SurfaceNowZUU, Hole);
	Sheet.HoleUU = Hole;

	TArray<FVector> Verts;
	TArray<int32> Tris;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Colors;
	TArray<FProcMeshTangent> Tangents;
	Verts.Reserve(Rects.Num() * 4);
	Tris.Reserve(Rects.Num() * 6);

	// UV origin: this basin's own bbox corner. Absolute world XY would be ~180 km
	// here, i.e. 1.8e5 metres in a float TexCoord, which quantises a 0.7 m ripple
	// wavelength to nothing. Anchoring per basin keeps the UV in the low
	// thousands with no wrap seam inside any quad.
	const FVector2D UVOrigin(Sheet.MinXUU, Sheet.MinYUU);

	// ---- B1: WHERE THE FINE CELLS GO, AND WHY ONLY THERE --------------------
	//
	// The tessellation square (TessBoxForCamera), clipped to the FINEST BAND.
	// Two conditions, and both have to hold or the cells emitted are not the
	// finest band's:
	//
	//   * StepPx[0] == 1 -- the first band actually meshes at ONE FINE PIXEL. A
	//     basin far enough out that even its first band is decimated has no
	//     finest band to tessellate, and subdividing its coarse rectangles
	//     would draw water at a resolution the extent mask never answered at.
	//   * inside RadiusPx[0] -- band 0's own bound. vxc::lakeSheetRectsBanded
	//     PARTITIONS space between the bands (they must not overlap or the
	//     sheet z-fights, and must not gap or the lake has a hole), so a
	//     rectangle meeting this clipped box IS a band-0 rectangle and nothing
	//     else can be. WaveTessRadiusMNow() clamps to FineBandRadiusM, so the
	//     clip is normally a no-op -- it is here so the invariant is ENFORCED
	//     rather than argued from two constants that could drift. That matters
	//     more now than it did: the radius is a cvar the settings panel can move
	//     mid-session, so "the two constants" are no longer both constants.
	FBox2D TessBox(ForceInit);
	bool bTess = TessBoxForCamera(CamUU, TessBox) && Lod.StepPx[0] == 1;
	if (bTess && Lod.NumBands > 1)
	{
		const double R0 = double(Lod.RadiusPx[0]) * kFinePixelUU;
		TessBox.Min.X = FMath::Max(TessBox.Min.X, Lod.CamXUU - R0);
		TessBox.Min.Y = FMath::Max(TessBox.Min.Y, Lod.CamYUU - R0);
		TessBox.Max.X = FMath::Min(TessBox.Max.X, Lod.CamXUU + R0);
		TessBox.Max.Y = FMath::Min(TessBox.Max.Y, Lod.CamYUU + R0);
		bTess = TessBox.Min.X < TessBox.Max.X && TessBox.Min.Y < TessBox.Max.Y;
	}

	if (bTess)
	{
		// RESERVED FOR THE WORST CASE THIS BASIN CAN REACH: the whole
		// tessellation square, at four verts per cell. Without it a lake
		// underfoot grows a ~30 k-element array from a few hundred, which is a
		// dozen reallocations and copies of a megabyte on the game thread, once
		// per camera cell. The estimate is an upper bound, so it never
		// under-reserves and never reallocates.
		const double CellsX = (TessBox.Max.X - TessBox.Min.X) / kFinePixelUU;
		const double CellsY = (TessBox.Max.Y - TessBox.Min.Y) / kFinePixelUU;
		const int32 WorstVerts = int32(FMath::Min(4.0 * (CellsX + 1.0) * (CellsY + 1.0), 200000.0));
		Verts.Reserve(Verts.Max() + WorstVerts);
		Tris.Reserve(Tris.Max() + (WorstVerts * 3) / 2);
		Normals.Reserve(Verts.Max());
		UVs.Reserve(Verts.Max());
		Colors.Reserve(Verts.Max());
		Tangents.Reserve(Verts.Max());
	}

	int32 Emitted = 0;
	int32 TessVerts = 0;

	// The four-vertex quad the sheet has always emitted.
	auto Greedy = [&](double X0, double Y0, double X1, double Y1)
	{
		if (X1 <= X0 || Y1 <= Y0)
		{
			return;
		}
		AppendRectQuad(FBox2D(FVector2D(X0, Y0), FVector2D(X1, Y1)), SurfaceNowZUU, UVOrigin, Verts, Tris,
		               Normals, UVs, Colors, Tangents);
		++Emitted;
	};

	// One rectangle, split against the tessellation square: the part inside
	// becomes fine cells, the four parts outside stay greedy. The SAME four-way
	// decomposition the near-field hole cut below uses -- deliberately the same
	// shape, because that one has already been read for correctness once.
	auto EmitPiece = [&](double X0, double Y0, double X1, double Y1)
	{
		if (X1 <= X0 || Y1 <= Y0)
		{
			return;
		}
		if (!bTess || TessBox.Max.X <= X0 || TessBox.Min.X >= X1 || TessBox.Max.Y <= Y0 || TessBox.Min.Y >= Y1)
		{
			Greedy(X0, Y0, X1, Y1);
			return;
		}
		const double IX0 = FMath::Max(X0, TessBox.Min.X);
		const double IX1 = FMath::Min(X1, TessBox.Max.X);
		const double IY0 = FMath::Max(Y0, TessBox.Min.Y);
		const double IY1 = FMath::Min(Y1, TessBox.Max.Y);
		Greedy(X0, Y0, X1, IY0);   // below the square
		Greedy(X0, IY1, X1, Y1);   // above it
		Greedy(X0, IY0, IX0, IY1); // left of it
		Greedy(IX1, IY0, X1, IY1); // right of it
		TessVerts += AppendRectQuadTessellated(FBox2D(FVector2D(IX0, IY0), FVector2D(IX1, IY1)), SurfaceNowZUU,
		                                       UVOrigin, kFinePixelUU, Verts, Tris, Normals, UVs, Colors,
		                                       Tangents);
		// ONE rectangle in the census, not one per cell. TotalRects measures the
		// DECOMPOSITION -- what the extent mask produced -- and burying it under
		// ~7,000 cells would destroy the only number that says whether the
		// greedy mesher is behaving. The cells have their own counter
		// (GetTessellatedVertCount) for exactly that reason.
		++Emitted;
	};

	for (const FBox2D& R : Rects)
	{
		if (!Sheet.bHadHole)
		{
			EmitPiece(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y);
			continue;
		}
		// Rectangle minus rectangle, in UU. The pixel-space helper in lakes.h is
		// the tested one; this is the same four cases on doubles, which is what
		// world units are -- converting the hole into pixel space and back would
		// round the cut to a 1.875 m lattice and open a sub-pixel seam against
		// the voxel water it is supposed to meet exactly.
		if (Hole.Max.X <= R.Min.X || Hole.Min.X >= R.Max.X || Hole.Max.Y <= R.Min.Y || Hole.Min.Y >= R.Max.Y)
		{
			EmitPiece(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y);
			continue;
		}
		const double MY0 = FMath::Max(R.Min.Y, Hole.Min.Y);
		const double MY1 = FMath::Min(R.Max.Y, Hole.Max.Y);
		EmitPiece(R.Min.X, R.Min.Y, R.Max.X, FMath::Min(Hole.Min.Y, R.Max.Y)); // below
		EmitPiece(R.Min.X, FMath::Max(Hole.Max.Y, R.Min.Y), R.Max.X, R.Max.Y); // above
		EmitPiece(R.Min.X, MY0, FMath::Min(Hole.Min.X, R.Max.X), MY1);         // left
		EmitPiece(FMath::Max(Hole.Max.X, R.Min.X), MY0, R.Max.X, MY1);         // right
	}

	// One COMPONENT per basin (see FSheet::Comp for the 9 ms measurement that
	// forced the split). CreateMeshSection rather than UpdateMeshSection because
	// a hole re-cut changes the TOPOLOGY (a rectangle becomes up to four), which
	// Update cannot express -- unlike the clipmap, whose vertex layout never
	// changes and which is right to prefer Update.
	// Always section 0 OF THIS BASIN'S OWN COMPONENT -- see FSheet::Comp. The
	// proxy recreate this triggers now converts one basin's vertices, not every
	// resident basin's.
	if (Verts.Num() == 0)
	{
		if (Sheet.Comp != nullptr)
		{
			Sheet.Comp->ClearAllMeshSections();
			VoxelEofLedger::Count(VoxelEofLedger::ESource::LakeCreate);
		}
		// No component yet = nothing ever drew = nothing to clear. Do NOT create
		// one here: an empty PMC still registers a primitive, and a basin that
		// decimates to zero rectangles at range would pay it for nothing.
	}
	else
	{
		UProceduralMeshComponent* C = GetOrCreateSheetComp(Sheet);
		C->CreateMeshSection(0, Verts, Tris, Normals, UVs, Colors, Tangents,
		                     /*bCreateCollision*/ false);
		C->SetMaterial(0, SheetMaterialOverride ? static_cast<UMaterialInterface*>(SheetMaterialOverride)
		                                        : WaterMaterial.Get());
		// One proxy recreate on ONE basin's component (the 2026-08-28 split; it
		// used to be one recreate of a 495-section proxy).
		VoxelEofLedger::Count(VoxelEofLedger::ESource::LakeCreate);
	}
	// The mesh now stands AT BuiltSurfaceZUU in world space, so whatever tide
	// nudge this component was carrying has been absorbed into its vertices and
	// the transform goes back to identity. Tested before writing: an unchanged
	// SetRelativeLocation still dirties the render transform, and this runs on
	// every rebuild, tide or not.
	if (Sheet.Comp != nullptr && !Sheet.Comp->GetRelativeLocation().IsNearlyZero())
	{
		Sheet.Comp->SetRelativeLocation(FVector::ZeroVector);
	}

	TotalRects += Emitted - Sheet.RectCount;
	Sheet.RectCount = Emitted;
	TotalTessVerts += TessVerts - Sheet.TessVerts;
	Sheet.TessVerts = TessVerts;
	// The radius this mesh now stands at. Recorded from the same accessor the
	// build above went through, so the record cannot describe a different radius
	// from the geometry -- which is the whole point of recording it.
	Sheet.TessRadiusM = WaveTessRadiusMNow();
	Sheet.bBuilt = true;
	return true;
}

void AVoxelWaterSheetActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bEnabled || !Mesh)
	{
		return;
	}

	// Destroy parked leftovers a few per tick. 32 x ~37 us (the measured
	// per-component DestroyComponent cost on the LSFIX leg) is ~1.2 ms worst
	// case -- against the 17.7 ms the old destroy-all-in-one-tick hitch cost.
	// Drained BEFORE any other work so a teleport's 495 leftovers cannot starve
	// behind a busy rebuild rotation.
	{
		int32 Destroyed = 0;
		while (PendingDestroy.Num() > 0 && Destroyed < 32)
		{
			UProceduralMeshComponent* C = PendingDestroy.Pop(EAllowShrinking::No);
			if (C != nullptr)
			{
				C->DestroyComponent();
				// COUNTED AT THE DRAIN, not where PendingDestroy is appended:
				// the append is bookkeeping, this is the frame that pays.
				VoxelEofLedger::Count(VoxelEofLedger::ESource::LakeDestroy);
				VoxelEofLedger::CountUnregister();
			}
			++Destroyed;
		}
	}
	UWorld* World = GetWorld();
	UVoxelWaterSubsystem* Water = World ? World->GetSubsystem<UVoxelWaterSubsystem>() : nullptr;
	if (!Water)
	{
		return;
	}

	// ---- Anchor: is there a real position to gather around yet? -------------
	//
	// GetCameraLocationUU now REFUSES to answer until a pawn exists, because
	// the answer it used to give before that was (0,0,0) and it cost 10,814
	// fine-tier gate leaks into four unbaked origin tiles -- each answered at
	// SEA LEVEL, and fatal on the first one under -unattended. See that
	// function's note.
	//
	// AND THE WAIT IS COUNTED, not swallowed. A skip that logs nothing is the
	// same failure in the other direction, and this project has shipped that
	// shape before (the weathering pass that removed 20 voxels of 90,000 for
	// months). "No lake sheets drew" and "no lake sheets were ever asked for"
	// have to be different lines in a log, so the deferral is announced once,
	// warned about if it drags, and reported by the first gather that follows.
	FVector CamUU;
	if (!GetCameraLocationUU(CamUU))
	{
		const double NowSec = FPlatformTime::Seconds();
		if (DeferredGatherTicks == 0)
		{
			FirstDeferSeconds = NowSec;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: no possessed pawn yet -- DEFERRING the first basin gather. Gathering now would "
			            "scan the four fine tiles meeting at the world origin, which are unbaked; every elevation "
			            "read into them is answered at sea level, and the first one is FATAL under -unattended."));
		}
		++DeferredGatherTicks;
		// A menu session is a legitimate forever-wait: VoxelFrontEnd::
		// IsEnabledThisRun makes AVoxelEarthGameMode set
		// bStartPlayersAsSpectators, so no pawn is spawned at StartPlay and one
		// only appears on NEW GAME/CONTINUE. A -game capture is not: ten
		// seconds with no pawn there means the run is stuck on something else,
		// and absent lakes are a symptom of that rather than a lake bug. Say it
		// once; do not repeat it every tick on the menu.
		if (!bWarnedLongDefer && NowSec - FirstDeferSeconds > 10.0)
		{
			bWarnedLongDefer = true;
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("Lake sheets: still no possessed pawn after %d tick(s) / %.1f s. NO LAKE SHEETS WILL DRAW "
			            "until one exists. Expected while the front-end menu is up (VoxelFrontEnd::IsEnabledThisRun); "
			            "on a -game capture it means the pawn never spawned, which is not a water problem."),
			       DeferredGatherTicks, NowSec - FirstDeferSeconds);
		}
		return;
	}

	// ---- B3: the tide, as a transform ---------------------------------------
	//
	// Runs on every tick that has a camera, INCLUDING the gather ticks below
	// that return early -- a tile gather can span several ticks and the water
	// must not sit at the old waterline for the whole of one. It costs a
	// subsystem call and a double compare on the ticks where the datum has not
	// stepped, which is all but a handful of them.
	ApplyTideNudge(Water);

	// ---- Gather: which basins are in range at all ---------------------------
	//
	// Not every tick, and not all at once. A gather LOADS FINE TILES -- tens of
	// MB each, read and parsed on the game thread -- and a 10 km square is up to
	// nine of them. Taken in one call that is a multi-second stall at spawn, on
	// the same thread the streamer and the water CA are using. So the tile LIST
	// is computed when the camera has moved far enough to change the answer, and
	// exactly one tile is read per tick after that.
	const FVector2D CamXY(CamUU.X, CamUU.Y);
	if (!bGathered || FVector2D::Distance(CamXY, LastGatherXY) > RegatherDistanceUU)
	{
		const bool bFirstGather = !bGathered;
		int32 Tx0 = 0, Ty0 = 0, Tx1 = 0, Ty1 = 0;
		UVoxelWaterSubsystem::FineTileForWorldUU(CamUU.X - ScanRadiusUU, CamUU.Y - ScanRadiusUU, Tx0, Ty0);
		UVoxelWaterSubsystem::FineTileForWorldUU(CamUU.X + ScanRadiusUU, CamUU.Y + ScanRadiusUU, Tx1, Ty1);
		PendingTiles.Reset();
		for (int32 Ty = Ty0; Ty <= Ty1; ++Ty)
		{
			for (int32 Tx = Tx0; Tx <= Tx1; ++Tx)
			{
				PendingTiles.Add(FIntPoint(Tx, Ty));
			}
		}
		// PARK, DON'T DESTROY -- see AdoptableSheets in the header. Destroying
		// everything here cost a measured 17.7 ms single-tick hitch and deleted
		// every lake for the ~5 s the rebuild drain took; parking lets the
		// re-gather adopt unchanged basins (~98% on the measured legs) and keeps
		// them DRAWING throughout. Whatever the new gather does not reclaim is
		// flushed to PendingDestroy when the last tile lands, and destroyed a few
		// per tick -- an out-of-range lake therefore lingers a few extra ticks,
		// which is the deliberate price of never blinking the in-range ones.
		for (FSheet& Old : Sheets)
		{
			AdoptableSheets.Add(FIntVector(Old.TileX, Old.TileY, Old.BasinId), Old);
		}
		Sheets.Reset();
		GatherCenterXY = CamXY;
		LastGatherXY = CamXY;
		bGathered = true;
		UnresolvedBasins = 0;
		TotalRects = 0;
		TotalTessVerts = 0;
		RoundRobinCursor = 0;
		bLoggedFirstBuild = false;
		UE_LOG(LogVoxelEarth, Log, TEXT("Lake sheets: scanning %d fine tile(s) within %.0f m of (%.0f, %.0f)"),
		       PendingTiles.Num(), ScanRadiusUU / 100.0, CamUU.X, CamUU.Y);
		if (bFirstGather)
		{
			// ALWAYS PRINTED, INCLUDING "0 deferred tick(s)". The fix is that
			// the first gather runs at a position something was actually placed
			// at; the proof is this line naming that position and what it
			// waited for. Printing it only when the wait was non-zero would
			// make its ABSENCE ambiguous -- which is precisely how a gather at
			// (0,0) went unnoticed until a leak counter forced the question.
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: FIRST gather ran at (%.0f, %.0f) after %d deferred tick(s), %.2f s waiting for "
			            "a possessed pawn. A first gather at (0, 0) would be the origin-gather bug, not a spawn on "
			            "the origin -- see GetCameraLocationUU."),
			       CamUU.X, CamUU.Y, DeferredGatherTicks,
			       DeferredGatherTicks > 0 ? FPlatformTime::Seconds() - FirstDeferSeconds : 0.0);
		}
	}

	if (PendingTiles.Num() > 0)
	{
		const FIntPoint T = PendingTiles.Pop(EAllowShrinking::No);
		TArray<UVoxelWaterSubsystem::FLakeSheetBasin> Found;
		const double StartSec = FPlatformTime::Seconds();
		Water->GatherLakeSheetBasinsInTile(T.X, T.Y, GatherCenterXY.X, GatherCenterXY.Y, ScanRadiusUU, Found);
		for (const UVoxelWaterSubsystem::FLakeSheetBasin& B : Found)
		{
			FSheet S;
			S.TileX = B.TileX;
			S.TileY = B.TileY;
			S.BasinId = B.BasinId;
			S.SurfaceZUU = B.SurfaceZUU;
			S.MinXUU = B.MinXUU;
			S.MinYUU = B.MinYUU;
			S.MaxXUU = B.MaxXUU;
			S.MaxYUU = B.MaxYUU;
			S.StepPx = StepForBasin(FMath::Max(B.MaxXUU - B.MinXUU, B.MaxYUU - B.MinYUU));
			// ADOPT the previous gather's component when the basin is unchanged --
			// same datum, same extent, same decimation -- so it never rebuilds and
			// never stops drawing. Geometry equality is exact-compare on purpose:
			// these fields are copied from the basin table, not derived, so an
			// unchanged basin reproduces them bit-for-bit, and a tolerance would
			// only invent a class of nearly-adopted basins with stale meshes.
			// An entry that exists but fails the test is a CHANGED basin: its old
			// component is queued for destruction and it rebuilds fresh.
			if (FSheet* Old = AdoptableSheets.Find(FIntVector(S.TileX, S.TileY, S.BasinId)))
			{
				const bool bSame = Old->bBuilt && !Old->bUnresolved && Old->Comp != nullptr
					&& Old->SurfaceZUU == S.SurfaceZUU && Old->StepPx == S.StepPx
					&& Old->MinXUU == S.MinXUU && Old->MinYUU == S.MinYUU
					&& Old->MaxXUU == S.MaxXUU && Old->MaxYUU == S.MaxYUU;
				if (bSame)
				{
					S.Comp = Old->Comp;
					S.bBuilt = true;
					S.LodKey = Old->LodKey;
					S.bUniformCoarse = Old->bUniformCoarse;
					S.bHadHole = Old->bHadHole;
					S.HoleUU = Old->HoleUU;
					S.RectCount = Old->RectCount;
					TotalRects += Old->RectCount;
					// The adopted mesh is the OLD one, so it still stands at the
					// datum it was built at and still carries whatever tide
					// nudge that component was given. Carrying both across is
					// what makes adoption invisible to B1 and B3: drop
					// BuiltSurfaceZUU and the next nudge measures drift from
					// the baked datum and double-applies the tide; drop
					// TessVerts and the actor-wide vertex count silently decays
					// toward zero over a session of re-gathers.
					S.BuiltSurfaceZUU = Old->BuiltSurfaceZUU;
					S.TessVerts = Old->TessVerts;
					// AND THE RADIUS IT WAS BUILT AT, for the same reason as
					// TessVerts one line up: an adopted mesh that reported "never
					// built at any radius" would be flagged tess-stale on the
					// next tick and every adopted basin would rebuild -- turning
					// the adoption path, whose whole purpose is to dirty nothing,
					// into a full re-mesh after every gather.
					S.TessRadiusM = Old->TessRadiusM;
					TotalTessVerts += Old->TessVerts;
					// CONTROL TERM: adoption dirties NOTHING -- no register, no
					// section, no mark. Counted at the ++ site rather than from
					// the gather-complete log line's `Adopted`, which recomputes
					// the same population and is skipped entirely when the gather
					// reclaims every parked basin. The two must agree when both
					// print. See VoxelEofDirtyLedger.h's reading note.
					VoxelEofLedger::Count(VoxelEofLedger::ESource::LakeAdopt);
				}
				else if (Old->Comp != nullptr)
				{
					PendingDestroy.Add(Old->Comp);
				}
				AdoptableSheets.Remove(FIntVector(S.TileX, S.TileY, S.BasinId));
			}
			Sheets.Add(S);
		}
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("Lake sheets: tile (%d,%d) contributed %d basin(s) in %.1f ms; %d tile(s) left, %d basin(s) so far"),
		       T.X, T.Y, Found.Num(), (FPlatformTime::Seconds() - StartSec) * 1000.0, PendingTiles.Num(),
		       Sheets.Num());
		if (PendingTiles.Num() == 0 && AdoptableSheets.Num() > 0)
		{
			// The gather is complete, so anything still parked is a basin the new
			// scan does not contain -- out of range, or gone from the table. Flush
			// to the amortised destroy queue; see the park site for why they were
			// kept drawing until now.
			for (TPair<FIntVector, FSheet>& Left : AdoptableSheets)
			{
				if (Left.Value.Comp != nullptr)
				{
					PendingDestroy.Add(Left.Value.Comp);
				}
			}
			int32 Adopted = 0;
			for (const FSheet& Sh : Sheets)
			{
				if (Sh.bBuilt)
				{
					++Adopted; // built at gather-complete = adopted (nothing has rebuilt yet)
				}
			}
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: gather complete -- %d of %d basin(s) adopted intact, %d leftover ")
			       TEXT("component(s) queued for amortised destroy."),
			       Adopted, Sheets.Num(), AdoptableSheets.Num());
			AdoptableSheets.Empty();
		}
		return; // one unit of work per tick, tile reads included
	}

	// ---- Build/refresh: at most one basin per tick --------------------------
	//
	// A basin is rebuilt when it has no mesh yet, when its LOD bands need
	// re-centring, or when the near-field hole it was cut with no longer matches
	// the disc the water subsystem is meshing now. The last case is normally ZERO
	// basins: with lake basins on one rendering path a hole is only owed where
	// the CA has actually taken water over.
	const FIntPoint LodKey = LodKeyForCamera(CamUU);
	// B1: the tessellation square for THIS camera cell, hoisted out of the walk.
	// It is a pure function of LodKey (TessBoxForCamera), which is what lets the
	// staleness test below be a box overlap rather than a rebuild.
	FBox2D TessBoxNow(ForceInit);
	const bool bTessNow = TessBoxForCamera(CamUU, TessBoxNow);
	// ONE READ OF THE RADIUS FOR THE WHOLE TICK, used by the staleness test
	// below and by the census line at the bottom. Reading it twice would let a
	// console write between them gate on one value and print another.
	const double TessRadiusNowM = WaveTessRadiusMNow();
	const int32 Num = Sheets.Num();
	for (int32 Step = 0; Step < Num; ++Step)
	{
		const int32 Index = (RoundRobinCursor + Step) % Num;
		FSheet& S = Sheets[Index];

		// THE LOD TRIGGER, and it has to have BOTH terms.
		//
		// The second alone is the obvious version and it is wrong in a way that
		// only shows up when you walk towards a lake: a basin built while it was
		// far away is flagged uniform, and a trigger that reads only the flag
		// never rebuilds it, so approaching a lake leaves it meshed at its coarse
		// far-field step forever. The first term is the state FLIP -- uniform
		// becoming banded as you approach, and banded becoming uniform as you
		// leave -- evaluated against the live camera every tick.
		//
		// Both are functions of the SNAPPED camera cell, so neither can flap: the
		// verdict changes only when LodKey does.
		const bool bBandedNow = IsBandedAtCamera(S, CamUU);
		const bool bLodStale = (bBandedNow == S.bUniformCoarse) || (bBandedNow && S.LodKey != LodKey);

		// ---- B1's OWN staleness term, and it is not covered by the two above --
		//
		// The tessellation square is a function of the camera CELL, so a basin
		// that meets it -- or that still carries cells from a cell it no longer
		// meets -- is stale the moment LodKey changes. That is NOT the same
		// question the LOD terms ask, and the difference is a real defect: a
		// pond (StepPx 1) is never "banded", so bBandedNow is false and
		// bUniformCoarse is true forever, both LOD terms stay false, and the
		// pond leaves the rebuild rotation permanently. Without this term it
		// would keep whatever cells it was built with the first time it came
		// into range: flat greedy quads on the pond you are standing beside,
		// and fine cells on one you have walked half a kilometre away from.
		//
		// Gated on LodKey having actually moved, so a settled camera does no box
		// tests at all, and on "touches OR already tessellated", so a basin far
		// from the disc with nothing to lose is never woken.
		//
		// SECOND GATE, ADDED WITH THE PLAYER-FACING TOGGLE: the radius itself
		// can now change without the camera moving. Every other term here is a
		// function of the camera CELL, so a player who flips "Water Wave Detail"
		// standing still would have changed a cvar, persisted a setting, logged
		// an apply -- and seen nothing until they walked 30 m. The comparison is
		// against the radius the MESH was built at, not against a flag someone
		// has to remember to set, so it is true exactly while the geometry
		// disagrees with the setting and false the moment it stops.
		//
		// It settles: a basin the rebuild decides needs nothing has its record
		// updated in the skip path below, so a toggle costs one sweep of box
		// tests and not one per tick forever.
		bool bTessStale = false;
		if (S.LodKey != LodKey || S.TessRadiusM != TessRadiusNowM)
		{
			const bool bTouches = bTessNow && !(TessBoxNow.Max.X <= S.MinXUU || TessBoxNow.Min.X >= S.MaxXUU
			                                    || TessBoxNow.Max.Y <= S.MinYUU || TessBoxNow.Min.Y >= S.MaxYUU);
			bTessStale = bTouches || S.TessVerts > 0;
		}

		FBox2D WantHole(ForceInit);
		// AT THE CURRENT SURFACE, not the baked one, because that is what
		// RebuildSheet will build against (its HoleForDatum call takes
		// SurfaceNowZUU). Ask this question at a different datum from the one
		// the build answers it at and the two never agree: the trigger fires
		// every tick and the rebuild never satisfies it, or it never fires and a
		// basin keeps a hole for water that has moved out from under it.
		const bool bWantHole = HoleForDatum(CurrentSurfaceZUUForBasin(S, Water), WantHole);
		const bool bHoleChanged = (bWantHole != S.bHadHole) || (bWantHole && WantHole != S.HoleUU);
		// A hole only matters where it actually meets this basin -- and BOTH the
		// old and the new one have to be tested. Testing only the new one is a
		// real bug and it was written that way first: when the camera walks out
		// of a lake the new hole is absent (an empty box at the origin), which
		// touches nothing, so the basin would keep the 52 m square cut out of it
		// forever and the lake would be left with a permanent hole in the middle.
		auto Touches = [&S](const FBox2D& H)
		{
			return !(H.Max.X <= S.MinXUU || H.Min.X >= S.MaxXUU || H.Max.Y <= S.MinYUU || H.Min.Y >= S.MaxYUU);
		};
		const bool bHoleTouches = (bWantHole && Touches(WantHole)) || (S.bHadHole && Touches(S.HoleUU));
		if (S.bUnresolved || (S.bBuilt && !bLodStale && !bTessStale && !(bHoleChanged && bHoleTouches)))
		{
			// EVALUATED AT THIS RADIUS AND FOUND TO NEED NOTHING, which is a
			// real answer and has to be recorded as one -- a basin outside the
			// disc with no tessellated vertices is already exactly what the new
			// radius asks for. Without this the radius test above would stay
			// true for those basins for the rest of the session and re-run its
			// box tests every tick.
			S.TessRadiusM = TessRadiusNowM;
			continue;
		}

		if (!RebuildSheet(S, CamUU))
		{
			++UnresolvedBasins;
			// Take it out of the rotation: an unresolvable basin retried every
			// tick is a disk stat per tick forever. It is COUNTED, which is what
			// keeps "the lake is missing" answerable from the log.
			S.bUnresolved = true;
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("Lake sheets: basin %d of tile (%d,%d) would not resolve -- its water will be ABSENT, "
			            "which looks exactly like a dry basin. %d unresolved so far."),
			       S.BasinId, S.TileX, S.TileY, UnresolvedBasins);
		}
		RoundRobinCursor = (Index + 1) % Num;
		break;
	}

	// ---- B1's engagement proof ----------------------------------------------
	//
	// ONE LINE PER CAMERA CELL (LodSnapPx, 30 m of travel), not one per rebuild:
	// the rebuild rotation touches one basin a tick and would otherwise write a
	// line a tick. The number that matters is the ACTOR-WIDE vertex count,
	// because that is the one that can be zero while every other sheet counter
	// looks healthy -- which is exactly what "the waves are computed and
	// invisible" looks like from a log.
	// Read ONCE for both the gate and the line: the radius is a cvar now, and a
	// line that gated on one read and printed another could claim a disc size
	// the census was not taken at.
	const double TessLogRadiusM = WaveTessRadiusMNow();
	if (TessLogRadiusM > 0.0 && LodKey != LastTessLogKey)
	{
		int32 TessBasins = 0;
		for (const FSheet& S : Sheets)
		{
			if (S.TessVerts > 0)
			{
				++TessBasins;
			}
		}
		if (TessBasins > 0 || TotalTessVerts > 0)
		{
			LastTessLogKey = LodKey;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: wave tessellation -- %d of %d basin(s) tessellated, %d vertex(es) inside "
			            "the %.0f m disc at cell (%d,%d)."),
			       TessBasins, Num, TotalTessVerts, TessLogRadiusM, LodKey.X, LodKey.Y);
		}
	}

	if (!bLoggedFirstBuild && Num > 0)
	{
		int32 Settled = 0;
		for (const FSheet& S : Sheets)
		{
			// A basin with zero rectangles is settled too: an extent that
			// decimates to nothing at this range is a real answer, and waiting
			// for it to become non-zero would never log at all.
			if (S.bBuilt || S.bUnresolved)
			{
				++Settled;
			}
		}
		if (Settled == Num)
		{
			bLoggedFirstBuild = true;
			UE_LOG(LogVoxelEarth, Log,
			       TEXT("Lake sheets: DRAINED build -- %d basin(s), %d rectangle(s), %d tessellated vertex(es), "
			            "%d unresolved. The far field now has water; a capture taken before this line has not. "
			            "Zero tessellated vertices with the camera over a lake means WAVES CANNOT BE DRAWN, "
			            "whatever the material graph is doing."),
			       Num, TotalRects, TotalTessVerts, UnresolvedBasins);
		}
	}
}
