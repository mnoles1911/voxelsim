#include "SVoxelMapScreen.h"

#include "SVoxelMenuButton.h"
#include "SVoxelScreenChrome.h"
#include "SVoxelScreenShell.h"
#include "VoxelEarthUI.h" // LogVoxelUI
#include "VoxelUIAssetLibrary.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/CursorReply.h"
#include "InputCoreTypes.h" // EKeys, for the +/- zoom keys
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleDefaults.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelMapScreenDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// --- The world the offline hillshade covers ---------------------------------
//
// EVERY NUMBER HERE IS A FACT ABOUT THE TERRAIN, NOT A DESIGN TOKEN, which is
// why none of them is in VoxelUITheme.h. ADR-0011 governs authored pixel
// dimensions; a tile's width in metres is not one.
//
// A COARSE TILE IS 512 px AT 30 m/px = 15.36 km. Both halves are stated in
// Config/DefaultGame.ini's comments and the fine tier's 8192 px at 1.875 m/px
// is the same 15.36 km, which is the cross-check that the number is the tile
// footprint rather than one tier's convention.
constexpr double kUUPerMetre = 100.0;
constexpr double kTileMetres = 15360.0;
constexpr double kTileUU = kTileMetres * kUUPerMetre;

// Used only when the coarse tile directory cannot be read. The live world is
// tx,ty in -16..0 (DefaultGame.ini's own comment, and 289 .vxtl filenames on
// disk agree), so the extent is -245.76 km to +15.36 km on both axes and the
// legend's "261 x 261 km" agrees to within a rounding.
constexpr int32 kFallbackTileMin = -16;
constexpr int32 kFallbackTileMax = 0;

// --- The hillshade PNG's own geometry ---------------------------------------
//
// The file is a MATPLOTLIB FIGURE, not a bare raster, so the map is a
// sub-rectangle of it. heightmap.py renders a 13.0 x (13.0*H/W + 0.9) inch
// figure at 150 dpi with the axes at fig.add_axes([0.0, 0.06, 1.0, 0.94]) and
// a caption drawn under it, so the bottom of the file is white paper and
// matplotlib's equal-aspect fit leaves a thin white band above and below the
// square map inside that axes box.
//
// THESE FOUR NUMBERS ARE MEASURED, NOT DERIVED. They are the fractions of the
// file that the map itself occupies, found by scanning the PNG for rows and
// columns that are not essentially pure white. Deriving them from matplotlib's
// layout rules would be a second implementation of matplotlib, and the whole
// class of bug this screen has already shipped once is a join computed instead
// of checked.
constexpr float kFigMapLeft   = 0.0f;
constexpr float kFigMapRight  = 1.0f;
constexpr float kFigMapTop    = 0.002398f; // row 5 of 2085
constexpr float kFigMapBottom = 0.937650f; // row 1954 of 2085 (exclusive)

// ROW 0 OF THE IMAGE IS THE MINIMUM WORLD Y, i.e. the SOUTH edge, so the
// picture is upside down for a north-up map and the sheet flips it.
//
// heightmap.py writes grid[(ty - y0)*n ...] and calls imshow(origin="upper"),
// so array row 0 -- the minimum ty -- lands at the TOP of the figure; and the
// tile pipeline maps world Y to the row index with no negation anywhere
// (tile_codec's "row-major (y outer)", tiles.h's "pixel (0,0) covers world mm
// range [0,pixelSizeMm)", amplifier.cpp's `s.py = floorDiv(yMm, pxMm)`).
// Minimum ty is therefore minimum world Y.
//
// A CONSTANT RATHER THAN A HARD-CODED FLIP because it is the one fact in this
// file that a change to the offline tool would silently invert, and because
// getting it wrong mirrors the terrain about the equator with nothing on
// screen to say so. If the relief ever reads mirrored, this is the line.
constexpr bool kRasterRowZeroIsMinY = true;

// --- The view ---------------------------------------------------------------
// The owner's default, verbatim: "10km x 10km square". Fitted to the frame's
// SHORTER side, so the 10 km is guaranteed visible on both axes rather than
// only on the one that happens to be long.
constexpr double kDefaultViewMetres = 10000.0;
// "sane bounds (~500 m at least)". Note what 500 m actually buys: the
// hillshade is 120 m per source pixel, so a 500 m view is four source pixels
// across and is a colour wash, not relief. The bound is the owner's and is
// honoured; the picture's own resolution is the reason to stop well before it,
// and the scale readout under the compass is what tells a player where they
// are on that scale.
constexpr double kMinViewMetres = 500.0;

// world-maps/ sits at the REPOSITORY root, two levels above the .uproject:
// ProjectDir() is ue-project/, so its parent is the repo. Resolved rather than
// hardcoded so a checkout anywhere works, and absent rather than fatal when the
// tree has been packaged and world-maps/ did not come with it.
FString WorldMapsDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("..") / TEXT("world-maps"));
}

// The coarse tile directory this run is actually using, resolved EXACTLY as
// UVoxelWorldSubsystem::Initialize resolves it (VoxelWorldSubsystem.cpp:
// -VoxelTileDir= wins, else DefaultTileDir under the subsystem's ini section,
// else empty for the synthetic sampler). Duplicated rather than called because
// the subsystem does not expose it and this module may not add an accessor to
// a UHT-parsed header it does not own; if the two ever disagree, the map draws
// the wrong world and that is the reason to give the subsystem a getter.
FString CoarseTileDir()
{
	FString TileDir;
	if (!FParse::Value(FCommandLine::Get(), TEXT("VoxelTileDir="), TileDir) && GConfig)
	{
		GConfig->GetString(TEXT("/Script/VoxelEarth.VoxelWorldSubsystem"), TEXT("DefaultTileDir"), TileDir,
		                   GGameIni);
	}
	if (!TileDir.IsEmpty() && FPaths::IsRelative(TileDir))
	{
		TileDir = FPaths::Combine(FPaths::ProjectContentDir(), TileDir);
		FPaths::CollapseRelativeDirectories(TileDir);
	}
	return TileDir;
}

// The provider's short hex, pulled out of the tile directory's path. The cache
// layout is <root>/<provider-id>/<seed-hex>/s<scale>, the provider id is
// `terrain-diffusion-unlabeled-<16 hex>`, and world-maps names its directories
// `<16 hex>-seed<decimal seed>`. Empty when nothing in the path looks like one,
// which is the honest answer for a synthetic world.
FString ProviderHexFromTileDir(const FString& TileDir)
{
	FString Rest = TileDir;
	Rest.ReplaceInline(TEXT("\\"), TEXT("/"));
	TArray<FString> Parts;
	Rest.ParseIntoArray(Parts, TEXT("/"), /*InCullEmpty=*/true);
	for (const FString& Part : Parts)
	{
		int32 Dash = INDEX_NONE;
		if (!Part.StartsWith(TEXT("terrain-")) || !Part.FindLastChar(TEXT('-'), Dash))
		{
			continue;
		}
		const FString Hex = Part.Mid(Dash + 1);
		// Sixteen hex digits is what every provider id in this project ends
		// with. Checked rather than assumed so a directory called
		// "terrain-scratch-copy" cannot become a provider.
		if (Hex.Len() != 16)
		{
			continue;
		}
		bool bAllHex = true;
		for (const TCHAR Ch : Hex)
		{
			bAllHex &= FChar::IsHexDigit(Ch);
		}
		if (bAllHex)
		{
			return Hex;
		}
	}
	return FString();
}

// The tx/ty range of the .vxtl tiles in a directory. THIS IS THE SAME SOURCE
// heightmap.py READ: it globs *.vxtl, takes sorted({tx}) and sorted({ty}), and
// lays the grid out from their minima. Reading the same directory the same way
// is what makes the transform a measurement of the picture's own input rather
// than a guess about it.
bool TileIndexRange(const FString& TileDir, int32& OutMinX, int32& OutMaxX, int32& OutMinY, int32& OutMaxY)
{
	if (TileDir.IsEmpty())
	{
		return false;
	}
	TArray<FString> Names;
	IFileManager::Get().FindFiles(Names, *(TileDir / TEXT("*.vxtl")), /*Files=*/true, /*Directories=*/false);
	bool bAny = false;
	for (const FString& Name : Names)
	{
		FString TxText;
		FString TyText;
		if (!FPaths::GetBaseFilename(Name).Split(TEXT("_"), &TxText, &TyText))
		{
			continue;
		}
		if (!TxText.IsNumeric() || !TyText.IsNumeric())
		{
			continue;
		}
		const int32 Tx = FCString::Atoi(*TxText);
		const int32 Ty = FCString::Atoi(*TyText);
		if (!bAny)
		{
			OutMinX = OutMaxX = Tx;
			OutMinY = OutMaxY = Ty;
			bAny = true;
			continue;
		}
		OutMinX = FMath::Min(OutMinX, Tx);
		OutMaxX = FMath::Max(OutMaxX, Tx);
		OutMinY = FMath::Min(OutMinY, Ty);
		OutMaxY = FMath::Max(OutMaxY, Ty);
	}
	return bAny;
}

// --- Slate geometry helpers -------------------------------------------------
//
// The same three primitives SVoxelHourglass built for the loading screen, and
// for the same reason: Slate has no untextured-polygon primitive, so every
// filled shape in this front end is a white brush under a vertex tint. They
// are duplicated rather than shared because SVoxelHourglass's are private to
// its own mock space and lifting them into a fourth "chrome" file for two
// callers is a file nobody would know to look in.

FSlateResourceHandle WhiteHandle()
{
	static const FSlateBrush* White = FCoreStyle::Get().GetBrush("WhiteBrush");
	return FSlateApplication::Get().GetRenderer()->GetResourceHandle(*White, FVector2f::ZeroVector, 1.0f);
}

// Fills a CONVEX polygon given in local widget space. A triangle fan from
// vertex 0, which is correct only for convex input -- the player arrow below is
// concave and is split into two convex halves for exactly this reason.
void FillConvexPoly(FSlateWindowElementList& Out, int32 LayerId, const FGeometry& Geometry,
                    const TArray<FVector2f>& Points, const FLinearColor& Colour)
{
	if (Points.Num() < 3 || Colour.A <= 0.f)
	{
		return;
	}
	// BY VALUE, VIA A NAMED TEMPORARY. GetAccumulatedRenderTransform() returns
	// a reference into the FPaintGeometry; binding it to a temporary does not
	// extend that temporary's life through a member call, and every vertex
	// below would then be transformed by freed memory. Compiles clean, draws
	// garbage. SVoxelHourglass paid for this one already.
	const FPaintGeometry PaintGeometry = Geometry.ToPaintGeometry();
	const FSlateRenderTransform& Transform = PaintGeometry.GetAccumulatedRenderTransform();
	const FColor Packed = Colour.ToFColor(/*bSRGB=*/false);

	TArray<FSlateVertex> Vertices;
	Vertices.Reserve(Points.Num());
	for (const FVector2f& Point : Points)
	{
		Vertices.Add(
			FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Point, FVector2f(0.5f, 0.5f), Packed));
	}
	TArray<SlateIndex> Indices;
	Indices.Reserve((Points.Num() - 2) * 3);
	for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
	{
		Indices.Add(SlateIndex(0));
		Indices.Add(SlateIndex(Index));
		Indices.Add(SlateIndex(Index + 1));
	}
	FSlateDrawElement::MakeCustomVerts(Out, LayerId, WhiteHandle(), Vertices, Indices, nullptr, 0, 0);
}

void FillRect(FSlateWindowElementList& Out, int32 LayerId, const FGeometry& Geometry, float X, float Y, float W,
              float H, const FLinearColor& Colour)
{
	if (W <= 0.f || H <= 0.f || Colour.A <= 0.f)
	{
		return;
	}
	static const FSlateBrush* White = FCoreStyle::Get().GetBrush("WhiteBrush");
	FSlateDrawElement::MakeBox(Out, LayerId,
	                           Geometry.ToPaintGeometry(FVector2f(W, H), FSlateLayoutTransform(FVector2f(X, Y))),
	                           White, ESlateDrawEffect::None, Colour);
}

// A quad with EXPLICIT UVs, which is the whole reason the raster is not drawn
// with MakeBox.
//
// MakeBox draws a brush across a rectangle with UVs 0..1 and no way to say
// otherwise that survives a version bump: FSlateBrush::SetUVRegion exists, but
// the batcher's handling of an INVERTED region (which is what a vertical flip
// is) differs between engine versions and at least one of them tests the
// region's signed area and silently ignores it. Custom verts have no such
// ambiguity -- the four UVs are written down here and the shader samples
// exactly them -- and they get the crop out of the caption strip for free.
void DrawTexturedQuad(FSlateWindowElementList& Out, int32 LayerId, const FGeometry& Geometry,
                      const FSlateBrush& Brush, const FVector2f& TopLeft, const FVector2f& BottomRight,
                      const FVector2f& UvTopLeft, const FVector2f& UvBottomRight, const FLinearColor& Tint)
{
	const FPaintGeometry PaintGeometry = Geometry.ToPaintGeometry();
	const FSlateRenderTransform& Transform = PaintGeometry.GetAccumulatedRenderTransform();
	const FColor Packed = Tint.ToFColor(/*bSRGB=*/false);
	const FSlateResourceHandle Handle =
		FSlateApplication::Get().GetRenderer()->GetResourceHandle(Brush, FVector2f::ZeroVector, 1.0f);
	if (!Handle.IsValid())
	{
		return;
	}

	const FVector2f P[4] = {TopLeft, FVector2f(BottomRight.X, TopLeft.Y), BottomRight,
	                        FVector2f(TopLeft.X, BottomRight.Y)};
	const FVector2f U[4] = {UvTopLeft, FVector2f(UvBottomRight.X, UvTopLeft.Y), UvBottomRight,
	                        FVector2f(UvTopLeft.X, UvBottomRight.Y)};

	TArray<FSlateVertex> Vertices;
	Vertices.Reserve(4);
	for (int32 I = 0; I < 4; ++I)
	{
		Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, P[I], U[I], Packed));
	}
	const TArray<SlateIndex> Indices = {SlateIndex(0), SlateIndex(1), SlateIndex(2),
	                                    SlateIndex(0), SlateIndex(2), SlateIndex(3)};
	FSlateDrawElement::MakeCustomVerts(Out, LayerId, Handle, Vertices, Indices, nullptr, 0, 0);
}

// The player arrow, as two convex halves of one concave dart, in a canonical
// frame pointing along +X and scaled by Size. Yaw is degrees, 0 = +X = East,
// increasing towards +Y = North.
//
// THE SCREEN ROTATION IS NOT THE WORLD ROTATION. Local Y runs DOWN and world Y
// runs north, so a yaw of theta points at (cos theta, -sin theta) on the sheet.
// Getting that sign wrong mirrors the heading about the east-west line, which
// on a compass reads as a plausible bearing rather than as a bug.
void ArrowHalves(const FVector2D& Centre, float Size, float YawDeg, TArray<FVector2f>& OutLeft,
                 TArray<FVector2f>& OutRight)
{
	const float Rad = FMath::DegreesToRadians(YawDeg);
	const float C = FMath::Cos(Rad);
	const float S = FMath::Sin(Rad);
	const auto P = [&](float X, float Y)
	{
		return FVector2f(float(Centre.X) + (X * C + Y * S) * Size, float(Centre.Y) + (-X * S + Y * C) * Size);
	};
	// tip, back-left, notch, back-right -- the notch is what makes it read as
	// an arrowhead rather than a triangle at 18 px.
	const FVector2f Tip = P(0.62f, 0.f);
	const FVector2f Left = P(-0.42f, -0.38f);
	const FVector2f Notch = P(-0.18f, 0.f);
	const FVector2f Right = P(-0.42f, 0.38f);
	OutLeft = {Tip, Left, Notch};
	OutRight = {Tip, Notch, Right};
}

TArray<FVector2f> DiamondPoly(const FVector2D& Centre, float HalfSize)
{
	return {FVector2f(float(Centre.X), float(Centre.Y) - HalfSize),
	        FVector2f(float(Centre.X) + HalfSize, float(Centre.Y)),
	        FVector2f(float(Centre.X), float(Centre.Y) + HalfSize),
	        FVector2f(float(Centre.X) - HalfSize, float(Centre.Y))};
}
} // namespace SVoxelMapScreenDetail

// ===========================================================================
// Raster and extent resolution
// ===========================================================================

FString SVoxelMapScreen::RasterPathForSeed(uint64 Seed)
{
	using namespace SVoxelMapScreenDetail;

	// PROVIDER FIRST, AND THERE IS NO FALL-BACK TO THE BARE SEED DIRECTORY.
	// See the header: two directories under world-maps/ carry seed 20260719 and
	// they are different planets. Picking the wrong one puts the player marker
	// on relief that is not the relief they are standing on, which is worse
	// than a blank sheet and is invisible without a marker to disagree with it.
	const FString Hex = ProviderHexFromTileDir(CoarseTileDir());
	if (Hex.IsEmpty())
	{
		return FString();
	}
	const FString Path = WorldMapsDir()
	                   / FString::Printf(TEXT("%s-seed%llu"), *Hex, (unsigned long long)Seed)
	                   / TEXT("01-heightmap-hillshade.png");
	return IFileManager::Get().FileExists(*Path) ? Path : FString();
}

bool SVoxelMapScreen::ResolveExtentUU(FVector2D& OutMinUU, FVector2D& OutMaxUU)
{
	using namespace SVoxelMapScreenDetail;

	int32 MinX = kFallbackTileMin;
	int32 MaxX = kFallbackTileMax;
	int32 MinY = kFallbackTileMin;
	int32 MaxY = kFallbackTileMax;
	const bool bMeasured = TileIndexRange(CoarseTileDir(), MinX, MaxX, MinY, MaxY);

	// Tile index i covers [i * 15.36 km, (i+1) * 15.36 km), so the extent runs
	// from the minimum index's near edge to the maximum index's FAR edge --
	// which is why the maxima below are +1. Dropping that +1 loses one whole
	// tile off two sides and shifts every position on the sheet by half of it.
	OutMinUU = FVector2D(double(MinX) * kTileUU, double(MinY) * kTileUU);
	OutMaxUU = FVector2D(double(MaxX + 1) * kTileUU, double(MaxY + 1) * kTileUU);
	return bMeasured;
}

// ===========================================================================
// SVoxelMapSheet
// ===========================================================================

void SVoxelMapSheet::Construct(const FArguments& InArgs)
{
	Data = InArgs._Data;
	LivePose = InArgs._LivePose;
	OnMarksChanged = InArgs._OnMarksChanged;
	Pose.WorldXY = FVector2D(Data.PlayerWorld.X, Data.PlayerWorld.Y);
	Pose.YawDeg = Data.HeadingDeg;
	ExtentMinUU = Data.ExtentMinUU;
	ExtentMaxUU = Data.ExtentMaxUU;

	// THE RASTER IS DRAWN AT WHATEVER SIZE THE ZOOM ASKS FOR, which at the
	// 500 m floor is a quad roughly a million pixels across. Everything outside
	// the frame has to be cut off by the widget rather than by luck.
	SetClipping(EWidgetClipping::ClipToBounds);
	RebuildOverlay();
}

FVector2D SVoxelMapSheet::WorldToLocal(const FVector2D& WorldUU, const FVector2D& LocalSize) const
{
	// LOCAL Y RUNS DOWN AND WORLD Y RUNS NORTH, hence the minus. This function
	// and its inverse below are the only two places in the screen that know
	// that, which is the point of having them.
	return FVector2D(LocalSize.X * 0.5 + (WorldUU.X - ViewCentreUU.X) / UUPerPixel,
	                 LocalSize.Y * 0.5 - (WorldUU.Y - ViewCentreUU.Y) / UUPerPixel);
}

FVector2D SVoxelMapSheet::LocalToWorld(const FVector2D& LocalPx, const FVector2D& LocalSize) const
{
	return FVector2D(ViewCentreUU.X + (LocalPx.X - LocalSize.X * 0.5) * UUPerPixel,
	                 ViewCentreUU.Y - (LocalPx.Y - LocalSize.Y * 0.5) * UUPerPixel);
}

double SVoxelMapSheet::MinUUPerPixel(const FVector2D& LocalSize) const
{
	using namespace SVoxelMapScreenDetail;
	const double ShortSide = FMath::Max(1.0, FMath::Min(LocalSize.X, LocalSize.Y));
	return kMinViewMetres * kUUPerMetre / ShortSide;
}

double SVoxelMapSheet::MaxUUPerPixel(const FVector2D& LocalSize) const
{
	const double ShortSide = FMath::Max(1.0, FMath::Min(LocalSize.X, LocalSize.Y));
	// "Whole world at most": the largest of the two spans, so that zooming all
	// the way out shows every part of the extent on both axes rather than
	// cropping the long one.
	const double Span = FMath::Max(ExtentMaxUU.X - ExtentMinUU.X, ExtentMaxUU.Y - ExtentMinUU.Y);
	// A degenerate extent (no tiles, both corners equal) would give a zero
	// ceiling and clamp every zoom to nothing. Never let the ceiling fall
	// below the floor.
	return FMath::Max(Span / ShortSide, MinUUPerPixel(LocalSize));
}

void SVoxelMapSheet::ResetViewToPlayer(const FVector2D& LocalSize)
{
	using namespace SVoxelMapScreenDetail;
	// THE OWNER'S DEFAULT: centred on the player, 10 km across, and the 10 km
	// is fitted to the SHORTER side so it is visible on both axes.
	const double ShortSide = FMath::Max(1.0, FMath::Min(LocalSize.X, LocalSize.Y));
	UUPerPixel = FMath::Clamp(kDefaultViewMetres * kUUPerMetre / ShortSide, MinUUPerPixel(LocalSize),
	                          MaxUUPerPixel(LocalSize));
	ViewCentreUU = Pose.WorldXY;
	ClampView(LocalSize);
}

void SVoxelMapSheet::ClampView(const FVector2D& LocalSize)
{
	UUPerPixel = FMath::Clamp(UUPerPixel, MinUUPerPixel(LocalSize), MaxUUPerPixel(LocalSize));

	const double HalfViewX = LocalSize.X * 0.5 * UUPerPixel;
	const double HalfViewY = LocalSize.Y * 0.5 * UUPerPixel;
	const auto ClampAxis = [](double Centre, double Min, double Max, double HalfView)
	{
		// WHEN THE VIEW IS WIDER THAN THE WORLD, CENTRE IT. Clamping into an
		// empty interval is what makes a fully zoomed-out map snap to a corner
		// and refuse to move, which reads as a broken pan rather than as a
		// limit.
		if (Max - Min <= HalfView * 2.0)
		{
			return (Min + Max) * 0.5;
		}
		return FMath::Clamp(Centre, Min + HalfView, Max - HalfView);
	};
	ViewCentreUU.X = ClampAxis(ViewCentreUU.X, ExtentMinUU.X, ExtentMaxUU.X, HalfViewX);
	ViewCentreUU.Y = ClampAxis(ViewCentreUU.Y, ExtentMinUU.Y, ExtentMaxUU.Y, HalfViewY);
}

void SVoxelMapSheet::ZoomAbout(int32 Notches, const FVector2D& AnchorLocal, const FVector2D& LocalSize)
{
	if (Notches == 0)
	{
		return;
	}
	// THE POINT UNDER THE ANCHOR MUST NOT MOVE. Read it before the scale
	// changes, then choose the centre that puts it back where it was.
	const FVector2D AnchorWorld = LocalToWorld(AnchorLocal, LocalSize);
	const double Step = FMath::Pow(double(FVoxelMenuLayout::Get().MapZoomStep), double(-Notches));
	UUPerPixel = FMath::Clamp(UUPerPixel * Step, MinUUPerPixel(LocalSize), MaxUUPerPixel(LocalSize));
	ViewCentreUU.X = AnchorWorld.X - (AnchorLocal.X - LocalSize.X * 0.5) * UUPerPixel;
	ViewCentreUU.Y = AnchorWorld.Y + (AnchorLocal.Y - LocalSize.Y * 0.5) * UUPerPixel;
	ClampView(LocalSize);
}

double SVoxelMapSheet::GetVisibleMetresAcross() const
{
	using namespace SVoxelMapScreenDetail;
	const double ShortSide = FMath::Max(1.0, FMath::Min(LastLocalSize.X, LastLocalSize.Y));
	return UUPerPixel * ShortSide / kUUPerMetre;
}

void SVoxelMapSheet::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
	if (LocalSize.X <= 0.0 || LocalSize.Y <= 0.0)
	{
		return;
	}
	LastLocalSize = LocalSize;

	// EVERY FRAME, FROM THE ATTRIBUTE. This is the whole of "live overlay": the
	// pose is re-read here and the marker is drawn from it in OnPaint, which is
	// const and may not evaluate a delegate the caller expects once per frame.
	const FVoxelMapPose NewPose = LivePose.IsSet() ? LivePose.Get() : Pose;
	const bool bPoseMoved = !NewPose.WorldXY.Equals(Pose.WorldXY, 0.5)
	                     || !FMath::IsNearlyEqual(NewPose.YawDeg, Pose.YawDeg, 0.1f);
	Pose = NewPose;

	if (!bViewInitialised)
	{
		// FIRST TICK WITH A REAL FRAME, NOT Construct. The default view is
		// "10 km across the shorter side", and during Construct the widget has
		// no geometry at all -- ComputeDesiredSize has not run and the fill
		// slot has not been arranged, so both sides are zero and the shorter
		// one is meaningless.
		bViewInitialised = true;
		ResetViewToPlayer(LocalSize);
		Invalidate(EInvalidateWidgetReason::Paint);
		return;
	}
	if (bPoseMoved)
	{
		// The marker moved. Under global invalidation a widget that only
		// changes what it PAINTS is not repainted unless it says so; without
		// invalidation this is a cheap no-op. Belt and braces, because a
		// marker that updates only when something else on screen happens to
		// change is the failure that reads as "the map is frozen".
		Invalidate(EInvalidateWidgetReason::Paint);
	}
}

int32 SVoxelMapSheet::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
                              const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
                              int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	using namespace SVoxelMapScreenDetail;
	using namespace VoxelUITheme;

	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
	if (LocalSize.X <= 0.0 || LocalSize.Y <= 0.0 || !bViewInitialised)
	{
		return SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId,
		                                InWidgetStyle, bParentEnabled);
	}

	// --- 1. The parchment ---------------------------------------------------
	// .parchment-frame, three gradient stops flattened to their ends. Drawn
	// under the raster rather than instead of it, so the sheet still reads as
	// paper where the raster has not decoded yet or does not reach.
	FillRect(OutDrawElements, LayerId, AllottedGeometry, 0.f, 0.f, float(LocalSize.X), float(LocalSize.Y),
	         Tint(Mix(MapFrameTop, MapFrameBottom)));

	// --- 2. The hillshade ---------------------------------------------------
	if (!Data.RasterPath.IsEmpty())
	{
		// THE BRUSH IS ASKED FOR EVERY PAINT, NOT ONCE. RequestImageFile kicks
		// the decode on a worker and returns null until it lands -- which is
		// ALWAYS null on the first call. An earlier draft of this screen read
		// it once during Construct and drew nothing, so the raster never
		// appeared however long the screen stayed open.
		if (const FSlateBrush* Raster = FVoxelUIAssetLibrary::Get().RequestImageFile(Data.RasterPath))
		{
			const FVector2D TopLeftWorld(ExtentMinUU.X, ExtentMaxUU.Y);
			const FVector2D BottomRightWorld(ExtentMaxUU.X, ExtentMinUU.Y);
			const FVector2D TL = WorldToLocal(TopLeftWorld, LocalSize);
			const FVector2D BR = WorldToLocal(BottomRightWorld, LocalSize);

			// The crop out of the matplotlib figure, and the vertical flip
			// that makes north up. Both are one constant each at the top of
			// this file.
			const float VTop = kRasterRowZeroIsMinY ? kFigMapBottom : kFigMapTop;
			const float VBottom = kRasterRowZeroIsMinY ? kFigMapTop : kFigMapBottom;
			DrawTexturedQuad(OutDrawElements, LayerId + 1, AllottedGeometry, *Raster,
			                 FVector2f(float(TL.X), float(TL.Y)), FVector2f(float(BR.X), float(BR.Y)),
			                 FVector2f(kFigMapLeft, VTop), FVector2f(kFigMapRight, VBottom),
			                 // Multiplied down towards the parchment rather
			                 // than drawn at full strength: the hillshade is a
			                 // relief render and the sheet is meant to read as
			                 // an inked map, not as a screenshot of a DEM.
			                 Tint(MapSheetPaper, 0.85f));
		}
	}

	// --- 3. The player's saved marks ---------------------------------------
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
	const FSlateFontInfo CaptionFont = Style.HandItalic(L.MapMarkCaptionSize);
	const float MarkHalf = L.MapMarkIconSize * 0.5f;
	for (const FVoxelMapMark& Mark : Data.Marks)
	{
		const FVector2D P = WorldToLocal(Mark.WorldXY, LocalSize);
		// CULLED BY THE MARK'S OWN BOX, not by the frame alone: a label is
		// wider than its glyph and a mark just off the left edge should still
		// draw the tail of its name.
		if (P.X < -LocalSize.X || P.X > LocalSize.X * 2.0 || P.Y < -LocalSize.Y || P.Y > LocalSize.Y * 2.0)
		{
			continue;
		}
		// A dark diamond under an ink one, so the glyph reads on pale
		// parchment AND on dark ocean relief. Two units of rim, per ADR-0011.
		FillConvexPoly(OutDrawElements, LayerId + 2, AllottedGeometry,
		               DiamondPoly(P, MarkHalf + VoxelUITheme::RulePx), Tint(MapYouEdge, 0.85f));
		FillConvexPoly(OutDrawElements, LayerId + 3, AllottedGeometry, DiamondPoly(P, MarkHalf),
		               Tint(MapMarkInk));

		const FString Label = Mark.Name.ToString();
		if (!Label.IsEmpty())
		{
			const FVector2D Size = Measure->Measure(Label, CaptionFont);
			const FVector2f At(float(P.X - Size.X * 0.5),
			                   float(P.Y + MarkHalf + L.MapMarkLabelGap));
			FSlateDrawElement::MakeText(
				OutDrawElements, LayerId + 4,
				AllottedGeometry.ToPaintGeometry(FVector2f(float(Size.X), float(Size.Y)),
				                                 FSlateLayoutTransform(At)),
				Label, CaptionFont, ESlateDrawEffect::None, Tint(MapMarkCaption));
		}
	}

	// --- 4. The player -------------------------------------------------------
	// LAST, SO IT IS NEVER UNDER A MARK. A marker you cannot find because
	// something is drawn over it is the same as no marker.
	{
		const FVector2D P = WorldToLocal(Pose.WorldXY, LocalSize);
		// DELIBERATELY NOT CLAMPED TO THE FRAME EDGE. The two honest options
		// were an edge chevron or letting it leave, and letting it leave is the
		// one that cannot lie: an arrow pinned to the edge states a position
		// the player is not at, on a screen whose entire purpose is stating
		// where they are, and this screen has already shipped one pin that did
		// exactly that. What replaces it is cheap and unambiguous -- the
		// default view is player-centred every time the map opens, so the way
		// back is to close and reopen, and the POSITION readout beside the
		// sheet never leaves.
		FillConvexPoly(OutDrawElements, LayerId + 5, AllottedGeometry, DiamondPoly(P, L.MapYouRing * 0.5f),
		               Tint(Gold, 0.45f));
		TArray<FVector2f> Left;
		TArray<FVector2f> Right;
		ArrowHalves(P, L.MapYouSize + VoxelUITheme::RulePx * 2.f, Pose.YawDeg, Left, Right);
		FillConvexPoly(OutDrawElements, LayerId + 6, AllottedGeometry, Left, Tint(MapYouEdge));
		FillConvexPoly(OutDrawElements, LayerId + 6, AllottedGeometry, Right, Tint(MapYouEdge));
		ArrowHalves(P, L.MapYouSize, Pose.YawDeg, Left, Right);
		FillConvexPoly(OutDrawElements, LayerId + 7, AllottedGeometry, Left, Tint(HpBright));
		FillConvexPoly(OutDrawElements, LayerId + 7, AllottedGeometry, Right, Tint(HpBright));
	}

	// --- 5. The chrome that does NOT move with the view ---------------------
	return SCompoundWidget::OnPaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId + 8,
	                                InWidgetStyle, bParentEnabled);
}

// --- Input ------------------------------------------------------------------

FReply SVoxelMapSheet::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	const FVector2D Local = Geometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());

	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		bRightDown = true;
		bPanning = false;
		PressLocal = Local;
		PressCentreUU = ViewCentreUU;
		ClosePopup();
		// CAPTURE, because a pan that leaves the frame must keep panning and
		// must still get its button-up. Without the capture, dragging off the
		// sheet strands bRightDown true and the next right-click anywhere
		// becomes a pan from a stale anchor.
		//
		// SetUserFocus is what makes the +/- keys reachable at all: the shell
		// focuses its active TAB BUTTON when a screen opens, and the sheet is
		// not on that focus path. Taking focus on a press rather than
		// proactively keeps the shipped focus ring where it was until the
		// player actually touches the map, and Escape and Q/E still reach the
		// shell by bubbling up out of this widget.
		return FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this), EFocusCause::Mouse);
	}

	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		// LEFT-DRAG DOES NOT PAN, by directive. It is left free for whatever
		// wants it next; taking focus is all this does with it, and the reply
		// is Handled so a click on the sheet does not fall through to the shell
		// and move focus back to a tab.
		ClosePopup();
		return FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::Mouse);
	}
	return FReply::Unhandled();
}

FReply SVoxelMapSheet::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	if (!bRightDown)
	{
		return FReply::Unhandled();
	}
	const FVector2D LocalSize = Geometry.GetLocalSize();
	const FVector2D Local = Geometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	const FVector2D Delta = Local - PressLocal;

	if (!bPanning && Delta.Size() >= double(FVoxelMenuLayout::Get().MapDragThresholdPx))
	{
		// ONCE A PAN, ALWAYS A PAN until the button comes up. A gesture that
		// wandered and came back is still not a click, and re-testing the
		// threshold every move would open a context menu at the end of one.
		bPanning = true;
	}
	if (!bPanning)
	{
		return FReply::Handled();
	}

	// FROM THE PRESS, NOT FROM THE LAST MOVE. Accumulating per-move deltas
	// drifts against the clamp: every frame the clamp refuses part of a move,
	// that part is lost, and the map slides away from the cursor.
	ViewCentreUU = FVector2D(PressCentreUU.X - Delta.X * UUPerPixel, PressCentreUU.Y + Delta.Y * UUPerPixel);
	ClampView(LocalSize);
	Invalidate(EInvalidateWidgetReason::Paint);
	return FReply::Handled();
}

FReply SVoxelMapSheet::OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::RightMouseButton || !bRightDown)
	{
		return FReply::Unhandled();
	}
	const FVector2D LocalSize = Geometry.GetLocalSize();
	const FVector2D Local = Geometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	const bool bWasPan = bPanning;
	bRightDown = false;
	bPanning = false;

	FReply Reply = FReply::Handled().ReleaseMouseCapture();
	if (!bWasPan)
	{
		// A PRESS-RELEASE THAT NEVER CROSSED THE THRESHOLD IS A CLICK, and a
		// right-click is the menu. Opening it on the UP rather than the DOWN is
		// what makes the two gestures share a button without either guessing.
		const int32 Index = MarkAtLocal(Local, LocalSize);
		OpenPopup(Index != INDEX_NONE ? EPopup::Mark : EPopup::Place, Local, LocalSize);
	}
	return Reply;
}

FReply SVoxelMapSheet::OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	// CTRL BELONGS TO THE SHELL. SVoxelScreenShell::OnMouseWheel implements the
	// player's menu-size dial on Ctrl+wheel, and a wheel event bubbles from
	// here up to it -- so this must decline the modified form rather than eat
	// it. A bare wheel is the map's.
	if (MouseEvent.IsControlDown())
	{
		return FReply::Unhandled();
	}
	const float Delta = MouseEvent.GetWheelDelta();
	if (FMath::IsNearlyZero(Delta))
	{
		return FReply::Unhandled();
	}
	// The sign only. Stepping by the raw delta gives a different zoom rate per
	// mouse, which is the same reason the shell's dial reads only the sign.
	ZoomAbout(Delta > 0.f ? 1 : -1, Geometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()),
	          Geometry.GetLocalSize());
	ClosePopup();
	Invalidate(EInvalidateWidgetReason::Paint);
	return FReply::Handled();
}

FReply SVoxelMapSheet::OnKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
{
	const FKey Key = KeyEvent.GetKey();
	const FVector2D LocalSize = Geometry.GetLocalSize();
	// ABOUT THE FRAME CENTRE, not the cursor: a keyboard zoom has no cursor to
	// be about, and a player using the keys is looking at the middle of the
	// sheet.
	const FVector2D Centre = LocalSize * 0.5;

	// BOTH ROWS OF EACH KEY. Equals/Add and Hyphen/Subtract are different FKeys
	// and a player on a numeric keypad presses the second of each pair.
	if (Key == EKeys::Add || Key == EKeys::Equals)
	{
		ZoomAbout(+1, Centre, LocalSize);
		ClosePopup();
		Invalidate(EInvalidateWidgetReason::Paint);
		return FReply::Handled();
	}
	if (Key == EKeys::Subtract || Key == EKeys::Hyphen || Key == EKeys::Underscore)
	{
		ZoomAbout(-1, Centre, LocalSize);
		ClosePopup();
		Invalidate(EInvalidateWidgetReason::Paint);
		return FReply::Handled();
	}
	if (Key == EKeys::Escape && Popup != EPopup::None)
	{
		// EATEN ONLY WHEN THERE IS A POPUP TO CLOSE. Escape otherwise has to
		// bubble to the shell and close the screen, which is what a player
		// pressing it expects; a map that swallowed Escape would need two
		// presses and look like it had ignored the first.
		ClosePopup();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FCursorReply SVoxelMapSheet::OnCursorQuery(const FGeometry& Geometry, const FPointerEvent& CursorEvent) const
{
	return bPanning ? FCursorReply::Cursor(EMouseCursor::GrabHandClosed) : FCursorReply::Unhandled();
}

// --- Marks ------------------------------------------------------------------

int32 SVoxelMapSheet::MarkAtLocal(const FVector2D& LocalPx, const FVector2D& LocalSize) const
{
	const double Radius = double(FVoxelMenuLayout::Get().MapMarkHitRadius);
	int32 Best = INDEX_NONE;
	double BestDistance = Radius;
	for (int32 Index = 0; Index < Data.Marks.Num(); ++Index)
	{
		const double Distance = FVector2D::Distance(WorldToLocal(Data.Marks[Index].WorldXY, LocalSize), LocalPx);
		// STRICTLY NEARER, so two marks on top of each other resolve to one of
		// them deterministically rather than to whichever the loop saw last.
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = Index;
		}
	}
	return Best;
}

void SVoxelMapSheet::CommitMarks()
{
	// UP TO THE SUBSYSTEM, WHICH OWNS THE LIST AND KNOWS THE SEED. The widget
	// keeps a copy so it can draw without a round trip, but the subsystem's
	// array is what the Codex's PLACES list reads and what gets written to
	// disk, so it is the one that must not fall behind.
	OnMarksChanged.ExecuteIfBound(Data.Marks);
}

// --- The popup ---------------------------------------------------------------

void SVoxelMapSheet::OpenPopup(EPopup Mode, const FVector2D& LocalPx, const FVector2D& LocalSize)
{
	Popup = Mode;
	PopupLocal = LocalPx;
	PopupMarkIndex = MarkAtLocal(LocalPx, LocalSize);
	PopupWorldUU = (Mode == EPopup::Mark && Data.Marks.IsValidIndex(PopupMarkIndex))
	                 ? Data.Marks[PopupMarkIndex].WorldXY
	                 : LocalToWorld(LocalPx, LocalSize);
	RebuildOverlay();
}

void SVoxelMapSheet::ClosePopup()
{
	if (Popup == EPopup::None)
	{
		return;
	}
	Popup = EPopup::None;
	PopupMarkIndex = INDEX_NONE;
	bNamingRename = false;
	NameField.Reset();
	RebuildOverlay();
}

void SVoxelMapSheet::BeginName(bool bRename)
{
	bNamingRename = bRename;
	PendingName = (bRename && Data.Marks.IsValidIndex(PopupMarkIndex)) ? Data.Marks[PopupMarkIndex].Name
	                                                                   : FText::GetEmpty();
	Popup = EPopup::Name;
	RebuildOverlay();
	if (NameField.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(NameField, EFocusCause::SetDirectly);
	}
}

void SVoxelMapSheet::ConfirmName()
{
	FString Trimmed = PendingName.ToString().TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		// AN EMPTY FIELD STILL MAKES A MARK. The player right-clicked a place
		// and chose to mark it; refusing because they did not type a word
		// throws away the thing they asked for over the thing they did not.
		Trimmed = VoxelUIStrings::MapMarkDefaultName().ToString();
	}
	if (bNamingRename && Data.Marks.IsValidIndex(PopupMarkIndex))
	{
		Data.Marks[PopupMarkIndex].Name = FText::FromString(Trimmed);
	}
	else
	{
		FVoxelMapMark Mark;
		Mark.Name = FText::FromString(Trimmed);
		Mark.WorldXY = PopupWorldUU;
		Mark.DayPlaced = Data.DayNumber;
		Mark.CreatedUnixTime = FDateTime::UtcNow().ToUnixTimestamp();
		Data.Marks.Add(MoveTemp(Mark));
	}
	CommitMarks();
	ClosePopup();
}

TSharedRef<SWidget> SVoxelMapSheet::BuildPopupRow(const FText& Label, bool bDanger, TFunction<void()> OnChosen)
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	return SNew(SVoxelMenuButton)
		.Text(Label)
		// .mk-btn IS the overlay family's leather plate -- same gradient, same
		// edge, same gold-on-hover -- so it is the same variant rather than a
		// sixth one.
		.Variant(EVoxelMenuButtonVariant::Leather)
		.Danger(bDanger)
		.FontSize(L.MapCtxItemSize)
		.MinHeight(L.MapCtxRowHeight)
		.ContentPadding(FMargin(L.MapCtxItemPadX, 0.f))
		.OnClicked_Lambda([Chosen = MoveTemp(OnChosen)]()
		{
			Chosen();
			return FReply::Handled();
		});
}

TSharedRef<SWidget> SVoxelMapSheet::BuildPopup()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	FSlateFontInfo TitleFont = Style.Serif(L.MapCtxTitleSize);
	TitleFont.LetterSpacing = L.MapCtxTitleSpacing;
	Column->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, L.MapCtxGap))
	[
		SNew(STextBlock)
		.Text(Popup == EPopup::Name ? VoxelUIStrings::MapNameTitle() : VoxelUIStrings::MapCtxPlaceTitle())
		.Font(TitleFont)
		.ColorAndOpacity(Tint(Gold))
	];

	if (Popup == EPopup::Name)
	{
		// The .mk-input plate: black, a bronze inset ring and the well fill,
		// with the field inside it. Same construction as the save dialog's name
		// field, which is the other place in this front end a player types.
		Column->AddSlot().AutoHeight()
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
			]
			+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx))
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(BronzeDeep))
			]
			+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx * 2.f))
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Mix(WellTop, WellBottom)))
			]
			+ SOverlay::Slot().Padding(FMargin(L.MapCtxInputPadX, L.MapCtxInputPadY)).VAlign(VAlign_Center)
			[
				SAssignNew(NameField, SEditableText)
				.Text(PendingName)
				.HintText(VoxelUIStrings::MapNameHint())
				.Font(Style.Mono(L.MapCtxInputSize))
				.ColorAndOpacity(FSlateColor(Tint(Ink)))
				.SelectAllTextWhenFocused(true)
				// ESCAPE MUST REACH THIS WIDGET. RevertTextOnEscape would have
				// the field consume the key to undo an edit, and the player
				// would press Escape twice to dismiss a menu that looked like
				// it had ignored them -- the exact reasoning SVoxelSaveDialog
				// records for the same setting.
				.RevertTextOnEscape(false)
				.ClearKeyboardFocusOnCommit(false)
				.OnTextChanged_Lambda([this](const FText& New) { PendingName = New; })
				.OnTextCommitted_Lambda([this](const FText& New, ETextCommit::Type Commit)
				{
					PendingName = New;
					if (Commit == ETextCommit::OnEnter)
					{
						ConfirmName();
					}
					else if (Commit == ETextCommit::OnCleared)
					{
						// SEditableText reports Escape as OnCleared. Anything
						// else -- losing focus to a click elsewhere -- leaves
						// the field open rather than committing behind the
						// player's back.
						ClosePopup();
					}
				})
			]
		];
		Column->AddSlot().AutoHeight().Padding(FMargin(0.f, L.MapCtxGap, 0.f, 0.f))
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::MapNameKeys())
			.Font(Style.Mono(L.MapCtxLabelSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
		];
	}
	else if (Popup == EPopup::Mark)
	{
		Column->AddSlot().AutoHeight()
		[
			BuildPopupRow(VoxelUIStrings::MapCtxRename(), false, [this]() { BeginName(/*bRename=*/true); })
		];
		Column->AddSlot().AutoHeight().Padding(FMargin(0.f, L.MapCtxGap * 0.5f, 0.f, 0.f))
		[
			BuildPopupRow(VoxelUIStrings::MapCtxRemove(), true, [this]()
			{
				if (Data.Marks.IsValidIndex(PopupMarkIndex))
				{
					Data.Marks.RemoveAt(PopupMarkIndex);
					CommitMarks();
				}
				ClosePopup();
			})
		];
	}
	else
	{
		Column->AddSlot().AutoHeight()
		[
			BuildPopupRow(VoxelUIStrings::MapCtxMarkHere(), false, [this]() { BeginName(/*bRename=*/false); })
		];
	}

	if (Popup != EPopup::Name)
	{
		Column->AddSlot().AutoHeight().Padding(FMargin(0.f, L.MapCtxGap * 0.5f, 0.f, 0.f))
		[
			BuildPopupRow(VoxelUIStrings::MapCtxCancel(), false, [this]() { ClosePopup(); })
		];
	}

	// The .mk-dialog plate: 2 px black, the oak-edge ring, the panel fill.
	//
	// WRAPPED IN A CLICK-EATING BORDER, and this is not cosmetic. The plate is
	// built from SImage layers, SImage is hit-testable and returns Unhandled,
	// and an unhandled mouse-down bubbles straight up to the sheet -- whose
	// OnMouseButtonDown closes the popup. Without this, clicking the padding
	// beside the name field would throw away what the player had typed. SBorder
	// is the one stock container that exposes a pointer handler, so it is what
	// says "this rectangle belongs to the menu".
	return SNew(SBorder)
		.BorderImage(FStyleDefaults::GetNoBrush())
		.Padding(FMargin(0.f))
		.OnMouseButtonDown_Lambda([](const FGeometry&, const FPointerEvent&) { return FReply::Handled(); })
		[
			SNew(SBox)
			.WidthOverride(L.MapCtxWidth)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
				]
				+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx))
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(PanelOakEdge))
				]
				+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx * 2.f))
				[
					SNew(SImage).Image(Style.SolidWhite())
					.ColorAndOpacity(Tint(Mix(OverlayPanelTop, OverlayPanelBottom), 0.97f))
				]
				+ SOverlay::Slot().Padding(FMargin(L.MapCtxPad))
				[
					Column
				]
			]
		];
}

TSharedRef<SWidget> SVoxelMapSheet::BuildCompass()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// THE ROSE IS FOUR LETTERS ON A DISC, not the mock's engraved card.
	// "Voxelmark Map.html" draws it as thirty-odd SVG polygons with split
	// light/dark facets and a 5-degree graduated limb -- a genuinely beautiful
	// drawing that Slate has no polygon primitive to reproduce at that
	// fidelity. Four cardinal letters around a parchment disc keep the one
	// thing the card is FOR, which is telling the player which way north is,
	// and it is now load-bearing rather than decorative: the sheet is drawn
	// north-up and the raster is flipped to make it so.
	const TArray<FText>& Letters = VoxelUIStrings::MapCompassLetters();
	auto Letter = [&](const FText& Text, EHorizontalAlignment H, EVerticalAlignment V, bool bNorth)
	{
		return SNew(SBox)
			.HAlign(H)
			.VAlign(V)
			.Padding(FMargin(4.f))
			[
				SNew(STextBlock)
				.Text(Text)
				.Font(Style.Serif(bNorth ? 16 : 12))
				.ColorAndOpacity(Tint(ParchmentInk))
			];
	};

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(L.MapCompassSize)
			.HeightOverride(L.MapCompassSize)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(ParchmentInk, 0.55f))
				]
				+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx))
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Parchment, 0.75f))
				]
				+ SOverlay::Slot()[Letter(Letters[0], HAlign_Center, VAlign_Top, true)]
				+ SOverlay::Slot()[Letter(Letters[1], HAlign_Right, VAlign_Center, false)]
				+ SOverlay::Slot()[Letter(Letters[2], HAlign_Center, VAlign_Bottom, false)]
				+ SOverlay::Slot()[Letter(Letters[3], HAlign_Left, VAlign_Center, false)]
			]
		]
		// THE SCALE READOUT IS THE ZOOM'S ONLY FEEDBACK. Without it, a wheel
		// notch on a smooth hillshade is indistinguishable from nothing
		// happening, and a player has no way to know how far out they are.
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(FMargin(0.f, 4.f, 0.f, 0.f))
		[
			SNew(STextBlock)
			.Text_Lambda([this]() { return VoxelUIStrings::MapScaleValue(GetVisibleMetresAcross()); })
			.Font(Style.Mono(L.MapCtxLabelSize))
			.ColorAndOpacity(Tint(ParchmentInk))
		];
}

void SVoxelMapSheet::RebuildOverlay()
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SOverlay> Chrome =
		SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(L.MapCompassInset))
		[
			BuildCompass()
		];

	if (Popup != EPopup::None)
	{
		// POSITIONED BY PADDING ON A TOP-LEFT-ALIGNED SLOT, which is the
		// stable way to put a widget at an arbitrary point in Slate without
		// SCanvas -- whose slot API has changed shape across engine versions
		// and which nothing else in this module uses.
		//
		// FLIPPED AWAY FROM THE EDGES. The sheet clips to its bounds, so a menu
		// opened near the right or bottom would be cut in half; anchoring it on
		// the other side of the click point is what every context menu does and
		// costs two comparisons. The height is estimated from the row count
		// rather than measured because a widget's desired size is not known
		// until after it is arranged, and being a few pixels out only moves the
		// flip point.
		const float Rows = (Popup == EPopup::Mark) ? 3.f : 2.f;
		const float MenuHeight = L.MapCtxPad * 2.f + float(L.MapCtxTitleSize) + L.MapCtxGap
		                       + Rows * (L.MapCtxRowHeight + L.MapCtxGap * 0.5f);
		const double X = (PopupLocal.X + L.MapCtxWidth + L.MapCtxOffset > LastLocalSize.X)
		                   ? PopupLocal.X - L.MapCtxWidth - L.MapCtxOffset
		                   : PopupLocal.X + L.MapCtxOffset;
		const double Y = (PopupLocal.Y + MenuHeight + L.MapCtxOffset > LastLocalSize.Y)
		                   ? PopupLocal.Y - MenuHeight - L.MapCtxOffset
		                   : PopupLocal.Y + L.MapCtxOffset;
		Chrome->AddSlot()
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Top)
			.Padding(FMargin(float(FMath::Max(0.0, X)), float(FMath::Max(0.0, Y)), 0.f, 0.f))
			[
				BuildPopup()
			];
	}

	ChildSlot[Chrome];
}

// ===========================================================================
// SVoxelMapScreen
// ===========================================================================

SVoxelMapScreen::~SVoxelMapScreen()
{
	FVoxelUIStyle::UnregisterWidget();
}

TArray<FVoxelScreenAction> SVoxelMapScreen::Actions(bool bHasRaster)
{
	TArray<FVoxelScreenAction> Out;
	// Said in the action bar rather than only in a comment. Without a raster, a
	// player looking at blank parchment deserves to know it is blank because
	// none exists for this world rather than because the map failed; WITH one,
	// the line names the gesture nothing else on screen hints at.
	Out.Add(FVoxelScreenAction(FText::GetEmpty(),
	                           bHasRaster ? VoxelUIStrings::MapOverviewNote()
	                                      : VoxelUIStrings::MapNoRaster()));
	// TWO CAPS, NOT FOUR. The action bar is one row inside a 1060-unit shell and
	// already carried three groups; the keyboard alternative rides in the label
	// rather than taking a cap of its own.
	Out.Add(FVoxelScreenAction(VoxelUIStrings::MapKeyWheel(), VoxelUIStrings::MapActionZoom()));
	Out.Add(FVoxelScreenAction(VoxelUIStrings::MapKeyRightDrag(), VoxelUIStrings::MapActionPan()));
	Out.Add(FVoxelScreenAction(FText::FromString(TEXT("Q/E")), VoxelUIStrings::ScreenActionPage()));
	Out.Add(FVoxelScreenAction(FText::FromString(TEXT("ESC")), VoxelUIStrings::ScreenActionClose(),
	                           /*bSpacerBefore=*/true));
	return Out;
}

void SVoxelMapScreen::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	Data = InArgs._Data;
	OnMarksChanged = InArgs._OnMarksChanged;

	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			SNew(SOverlay)
			// The 2 px black border and the bronze ring, both at
			// VoxelUITheme::RulePx. The mock's ring is 1 px; ADR-0011 forbids a
			// one-unit band, so the inner inset is 4 rather than 3 and the ring
			// is two units.
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(FVoxelUIStyle::Get().SolidWhite())
				.ColorAndOpacity(VoxelUITheme::Tint(FColor::Black))
			]
			+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx))
			[
				SNew(SImage).Image(FVoxelUIStyle::Get().SolidWhite())
				.ColorAndOpacity(VoxelUITheme::Tint(VoxelUITheme::BronzeDeep))
			]
			+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx * 2.f))
			[
				SAssignNew(Sheet, SVoxelMapSheet)
				.Data(Data)
				.LivePose(InArgs._LivePose)
				// INTERCEPTED, not forwarded. The screen updates its own copy
				// and refills the drawer, then passes the list on to whoever
				// owns it -- so the sheet, the drawer and the subsystem cannot
				// disagree about what the player has named.
				.OnMarksChanged(FOnVoxelMapMarksChanged::CreateSP(
					this, &SVoxelMapScreen::HandleSheetMarks))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(L.JournalColumnGap, 0.f, 0.f, 0.f))
		[
			SNew(SBox)
			.WidthOverride(L.MapDrawerWidth)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[BuildReadout()]
				+ SVerticalBox::Slot().FillHeight(1.f).Padding(FMargin(0.f, 10.f, 0.f, 0.f))
				[
					SAssignNew(DrawerHost, SBox)[BuildPlacesDrawer()]
				]
			]
		]
	];
}

void SVoxelMapScreen::HandleSheetMarks(const TArray<FVoxelMapMark>& NewMarks)
{
	Data.Marks = NewMarks;
	RefreshDrawer();
	OnMarksChanged.ExecuteIfBound(NewMarks);
}

void SVoxelMapScreen::RefreshDrawer()
{
	if (DrawerHost.IsValid())
	{
		DrawerHost->SetContent(BuildPlacesDrawer());
	}
}

TSharedRef<SWidget> SVoxelMapScreen::BuildReadout()
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	auto Row = [&](const FText& Label, const FText& Value)
	{
		Column->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, 8.f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(Style.Serif(L.MapReadoutLabelSize))
				.ColorAndOpacity(FVoxelUIStyle::MutedColour())
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(Value)
				.Font(Style.Mono(L.MapReadoutSize))
				.ColorAndOpacity(FVoxelUIStyle::BodyColour())
			]
		];
	};

	Row(VoxelUIStrings::MapLabelPosition(), VoxelUIStrings::MapPositionValue(Data.PlayerWorld));
	Row(VoxelUIStrings::MapLabelGeo(), VoxelUIStrings::MapGeoValue(Data.LatitudeDeg, Data.LongitudeDeg));
	Row(VoxelUIStrings::MapLabelHeading(), VoxelUIStrings::MapHeadingValue(Data.HeadingDeg));
	Row(VoxelUIStrings::MapLabelChunk(), VoxelUIStrings::MapChunkValue(Data.ChunkKey));
	Row(VoxelUIStrings::MapLabelAltitude(),
	    FText::AsNumber(FMath::RoundToInt(Data.SurfaceHeightUU / 100.0)));
	Row(VoxelUIStrings::MapLabelSeed(), VoxelUIStrings::MapSeedValue(Data.Seed));

	return VoxelScreenChrome::OakCard(Column, FMargin(L.PlayerCardPadX, L.PlayerCardPadY));
}

TSharedRef<SWidget> SVoxelMapScreen::BuildPlacesDrawer()
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
	if (Data.Marks.Num() == 0)
	{
		List->AddSlot().AutoHeight().Padding(FMargin(12.f, 16.f))
		[
			SNew(STextBlock)
			.Text(VoxelUIStrings::MapNoPlaces())
			.Font(Style.HandItalic(L.MapDrawerRowSize))
			.ColorAndOpacity(FVoxelUIStyle::MutedColour())
			.Justification(ETextJustify::Center)
			.AutoWrapText(true)
		];
	}
	else
	{
		for (const FVoxelMapMark& Mark : Data.Marks)
		{
			const double DistanceM =
				FVector2D::Distance(Mark.WorldXY, FVector2D(Data.PlayerWorld.X, Data.PlayerWorld.Y)) / 100.0;
			List->AddSlot().AutoHeight().Padding(FMargin(10.f, 6.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Mark.Name)
					.Font(Style.Mono(L.MapDrawerRowSize))
					.ColorAndOpacity(FVoxelUIStyle::BodyColour())
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::AsNumber(FMath::RoundToInt(DistanceM)))
					.Font(Style.Mono(L.MapDrawerRowSize - 3))
					.ColorAndOpacity(FVoxelUIStyle::MutedColour())
				]
			];
		}
	}

	return VoxelScreenChrome::OakCard(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			VoxelScreenChrome::PanelHeading(VoxelUIStrings::MapSavedPlaces(),
			                                FText::AsNumber(Data.Marks.Num()))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.f, 6.f))
		[
			VoxelScreenChrome::CardRule()
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SScrollBox) + SScrollBox::Slot()[List]
		],
		FMargin(L.PlayerCardPadX, L.PlayerCardPadY));
}
