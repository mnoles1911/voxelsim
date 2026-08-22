#include "SVoxelHourglass.h"

#include "VoxelEarthUI.h"
#include "VoxelUITheme.h"

#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

namespace SVoxelHourglassDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// --- Mock SVG space (LoadingHourglass.gd, verbatim) -------------------------
constexpr float kSvgW = 40.0f;
constexpr float kSvgH = 60.0f;
constexpr float kWaistX = 20.0f;
constexpr float kWaistY = 30.0f;

// The mock uses 38 grains; the Godot port cut it to 24 to reduce per-frame
// draw calls during chunk streaming. Kept at 24 -- the reason still applies,
// and more so here, since this front end is on screen precisely when the
// frame budget is worst.
constexpr int32 kMaxGrains = 24;
constexpr float kSpawnIntervalS = 0.045f;   // mock: 32 ms, raised to match the lower cap

// FIXED TIMESTEP, and this is not incidental. The mock's loop assumes 60 Hz
// and applies per-FRAME velocity magnitudes. At the 10 FPS a cold chunk
// cascade genuinely produces, a delta-scaled version moves each grain six
// times as far per step, which flings them straight past the mound and looks
// broken. Running several small sub-ticks instead keeps motion identical at
// any frame rate.
constexpr float kPhysicsTickS = 1.0f / 60.0f;
constexpr int32 kMaxSubticksPerFrame = 4;
constexpr float kGravity = 0.012f;
constexpr float kSpawnVyBase = 0.18f;
constexpr float kSpawnVyRand = 0.10f;
constexpr float kSpawnVxRand = 0.10f;
constexpr float kSpawnXJitter = 0.6f;

// The mock's second grain colour, alongside SandBright.
const FColor kGrainAlt = FColor(0xE8, 0xB8, 0x50);

// Glass, at the alphas the mock's CSS gives them.
const FLinearColor kGlassFill = FLinearColor(0.961f, 0.816f, 0.431f, 0.05f);   // rgba(245,208,110,0.05)

// A resource handle for a plain white texture, so MakeCustomVerts has
// something to sample. Slate has no "untextured triangle" primitive; every
// filled polygon in the engine's own UI is a white brush under a vertex tint.
FSlateResourceHandle WhiteHandle()
{
	static const FSlateBrush* White = FCoreStyle::Get().GetBrush("WhiteBrush");
	return FSlateApplication::Get().GetRenderer()->GetResourceHandle(*White);
}

// Fills a CONVEX polygon given in local widget space.
//
// Slate's substitute for Godot's draw_colored_polygon. A triangle fan from
// vertex 0 is correct only for convex input -- every caller here either passes
// a triangle, a convex pentagon, or a shape this file has explicitly split
// into convex halves (see the diamond in OnPaint).
void FillConvexPoly(FSlateWindowElementList& Out, int32 LayerId, const FGeometry& Geometry,
                    const TArray<FVector2f>& Points, const FLinearColor& Colour)
{
	if (Points.Num() < 3 || Colour.A <= 0.f)
	{
		return;
	}
	const FSlateRenderTransform& Transform = Geometry.ToPaintGeometry().GetAccumulatedRenderTransform();
	const FColor Packed = Colour.ToFColor(/*bSRGB=*/false);

	TArray<FSlateVertex> Vertices;
	Vertices.Reserve(Points.Num());
	for (const FVector2f& Point : Points)
	{
		// UV at the centre of the white texture; the colour comes from the
		// vertex tint.
		Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Point, FVector2f(0.5f, 0.5f),
		                                                                Packed));
	}
	TArray<SlateIndex> Indices;
	Indices.Reserve((Points.Num() - 2) * 3);
	for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
	{
		Indices.Add(0);
		Indices.Add(uint32(Index));
		Indices.Add(uint32(Index + 1));
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

// Godot's draw_rect(..., filled=false, width=1). Four thin boxes rather than
// MakeLines, so the outline lands on the same whole-pixel grid as the fills it
// surrounds and cannot half-cover them.
void StrokeRect(FSlateWindowElementList& Out, int32 LayerId, const FGeometry& Geometry, float X, float Y, float W,
                float H, const FLinearColor& Colour, float Thickness = 1.f)
{
	FillRect(Out, LayerId, Geometry, X, Y, W, Thickness, Colour);
	FillRect(Out, LayerId, Geometry, X, Y + H - Thickness, W, Thickness, Colour);
	FillRect(Out, LayerId, Geometry, X, Y, Thickness, H, Colour);
	FillRect(Out, LayerId, Geometry, X + W - Thickness, Y, Thickness, H, Colour);
}
} // namespace SVoxelHourglassDetail

void SVoxelHourglass::Construct(const FArguments& InArgs)
{
	ProgressAttribute = InArgs._Progress;
	Progress = ProgressAttribute.Get();
	// Deterministic under -unattended, so a -VoxelHourglassShot strip taken
	// twice produces comparable images. Without it every grain position would
	// differ between runs and the whole capture would be undiffable.
	Random = MakeVoxelUIRandomStream();
	SetCanTick(true);
}

void SVoxelHourglass::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	using namespace SVoxelHourglassDetail;
	Progress = FMath::Clamp(ProgressAttribute.Get(), 0.f, 1.f);

	PhysicsAccumulator += InDeltaTime;
	int32 Ticks = 0;
	while (PhysicsAccumulator >= kPhysicsTickS && Ticks < kMaxSubticksPerFrame)
	{
		PhysicsAccumulator -= kPhysicsTickS;
		PhysicsTick();
		++Ticks;
	}
	// A stall longer than the sub-tick cap drops its leftover rather than
	// trying to catch up forever on every subsequent frame.
	if (PhysicsAccumulator > kPhysicsTickS * kMaxSubticksPerFrame)
	{
		PhysicsAccumulator = 0.f;
	}
}

void SVoxelHourglass::PhysicsTick()
{
	using namespace SVoxelHourglassDetail;

	// Spawn cadence, bursty through the middle of the drain -- the mock's
	// `progress > 0.2 && progress < 0.8 && Math.random() < 0.5` double-spawn.
	if (Progress > 0.005f && Progress < 0.995f && Grains.Num() < kMaxGrains)
	{
		SpawnAccumulator += kPhysicsTickS;
		while (SpawnAccumulator >= kSpawnIntervalS)
		{
			SpawnAccumulator -= kSpawnIntervalS;
			SpawnGrain();
			if (Progress > 0.2f && Progress < 0.8f && Random.FRand() < 0.5f)
			{
				SpawnGrain();
			}
		}
	}

	// Backwards, so removals do not skew the indices.
	for (int32 Index = Grains.Num() - 1; Index >= 0; --Index)
	{
		FGrain& Grain = Grains[Index];
		const float NewVelY = Grain.VelY + kGravity;
		const float NewX = Grain.X + Grain.VelX;
		const float NewY = Grain.Y + NewVelY;
		if (NewY >= kSvgH || MoundContains(NewX, NewY))
		{
			Grains.RemoveAtSwap(Index);
			continue;
		}
		Grain.X = NewX;
		Grain.Y = NewY;
		Grain.VelY = NewVelY;
	}
}

void SVoxelHourglass::SpawnGrain()
{
	using namespace SVoxelHourglassDetail;
	FGrain Grain;
	Grain.X = kWaistX + (Random.FRand() - 0.5f) * kSpawnXJitter;
	Grain.Y = kWaistY;
	Grain.VelX = (Random.FRand() - 0.5f) * kSpawnVxRand;
	Grain.VelY = kSpawnVyBase + Random.FRand() * kSpawnVyRand;
	Grain.Size = Random.FRand() < 0.4f ? 0.9f : 0.7f;
	Grain.Colour = Random.FRand() < 0.5f ? VoxelUITheme::Tint(VoxelUITheme::SandBright)
	                                     : VoxelUITheme::Tint(kGrainAlt);
	Grains.Add(Grain);
}

bool SVoxelHourglass::MoundContains(float X, float Y) const
{
	using namespace SVoxelHourglassDetail;
	// The mound is a triangle with its apex at (kWaistX, ApexY) and its base on
	// the floor. Half-width at a given depth is linear in that depth.
	const float ApexY = kSvgH - Progress * (kSvgH - kWaistY);
	if (Y < ApexY)
	{
		return false;
	}
	if (Y > kSvgH)
	{
		return true;
	}
	const float Span = kSvgH - ApexY;
	const float T = Span > 0.0001f ? (Y - ApexY) / Span : 1.f;
	return FMath::Abs(X - kWaistX) <= T * (kSvgW * 0.5f);
}

int32 SVoxelHourglass::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
                               const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
                               int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	using namespace SVoxelHourglassDetail;
	using namespace VoxelUITheme;

	const FVector2D Size = AllottedGeometry.GetLocalSize();
	if (Size.X <= 0.0 || Size.Y <= 0.0)
	{
		return LayerId;
	}
	const float ScaleX = float(Size.X) / kSvgW;
	const float ScaleY = float(Size.Y) / kSvgH;
	// Mock space -> widget space.
	const auto P = [ScaleX, ScaleY](float MockX, float MockY) { return FVector2f(MockX * ScaleX, MockY * ScaleY); };

	// --- 1. Glass diamond ---------------------------------------------------
	//
	// The mock's polygon is "0,0 40,0 21,30 40,60 0,60 19,30" -- ASYMMETRIC,
	// with the right waist at x=21 and the left at x=19. The GDScript comment
	// calls that deliberate character and keeps it; so does this.
	//
	// It is also CONCAVE (the waist pinches inward), so a single triangle fan
	// would fill outside the shape. Split into its two convex halves instead,
	// which is exact rather than approximately right.
	{
		const TArray<FVector2f> TopHalf = {P(0.f, 0.f), P(kSvgW, 0.f), P(21.f, kWaistY), P(19.f, kWaistY)};
		const TArray<FVector2f> BottomHalf = {P(19.f, kWaistY), P(21.f, kWaistY), P(kSvgW, kSvgH), P(0.f, kSvgH)};
		FillConvexPoly(OutDrawElements, LayerId, AllottedGeometry, TopHalf, kGlassFill);
		FillConvexPoly(OutDrawElements, LayerId, AllottedGeometry, BottomHalf, kGlassFill);

		// The gold outline. bAntialias=false matches the GDScript's explicit
		// choice: Godot triangulates every antialiased segment, and at this
		// size the line sits on whole-pixel boundaries and reads clean without.
		TArray<FVector2f> Outline = {P(0.f, 0.f),      P(kSvgW, 0.f),  P(21.f, kWaistY), P(kSvgW, kSvgH),
		                             P(0.f, kSvgH),    P(19.f, kWaistY), P(0.f, 0.f)};
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(), Outline,
		                             ESlateDrawEffect::None, Tint(Brass1), /*bAntialias=*/false, 1.f);
	}

	// --- 2. Top sand --------------------------------------------------------
	// At p=0 the surface sits at y=0.5 spanning 0.5..39.5; at p=1 it collapses
	// onto the waist. Skipped near empty rather than drawn degenerate.
	if (Progress < 0.97f)
	{
		const float SurfaceY = 0.5f + Progress * 28.5f;
		const float HalfWidth = (1.f - Progress) * 19.5f;
		const TArray<FVector2f> TopSand = {P(kWaistX - HalfWidth, SurfaceY), P(kWaistX + HalfWidth, SurfaceY),
		                                   P(kWaistX, 29.5f)};
		FillConvexPoly(OutDrawElements, LayerId, AllottedGeometry, TopSand, Tint(SandMid));
	}

	// --- 3. Bottom mound ----------------------------------------------------
	if (Progress > 0.005f)
	{
		const float ApexY = kSvgH - Progress * (kSvgH - kWaistY);
		const float HalfWidth = Progress * (kSvgW * 0.5f);
		TArray<FVector2f> Mound;
		if (HalfWidth < 2.f)
		{
			// A tiny mound gets a clean triangle. The bevel below overshoots
			// the floor at this size and produces a self-intersecting polygon
			// -- which Godot's triangulator rejects outright, and which a
			// triangle fan would render as a visible artefact. Same guard,
			// same threshold.
			Mound = {P(kWaistX - HalfWidth, kSvgH), P(kWaistX + HalfWidth, kSvgH), P(kWaistX, ApexY)};
		}
		else
		{
			// The apex bevel: what makes the mound read as settled sand rather
			// than a wedge. Clamped so it stays above the floor.
			const float Bevel = HalfWidth * 0.06f;
			const float BevelY = FMath::Min(ApexY + 0.5f, kSvgH - 0.1f);
			Mound = {P(kWaistX - HalfWidth, kSvgH), P(kWaistX + HalfWidth, kSvgH), P(kWaistX + Bevel, BevelY),
			         P(kWaistX, ApexY), P(kWaistX - Bevel, BevelY)};
		}
		FillConvexPoly(OutDrawElements, LayerId, AllottedGeometry, Mound, Tint(SandDeep));
	}

	// --- 4. Falling grains --------------------------------------------------
	for (const FGrain& Grain : Grains)
	{
		FillRect(OutDrawElements, LayerId, AllottedGeometry, (Grain.X - Grain.Size * 0.5f) * ScaleX,
		         (Grain.Y - Grain.Size * 0.5f) * ScaleY, Grain.Size * ScaleX, Grain.Size * ScaleY, Grain.Colour);
	}

	// --- 5. Front pillars ---------------------------------------------------
	// Two mock-units wide, running y=2..58, drawn as three bands for a cheap
	// three-stop gradient. (The dimmed BACK pillars are deliberately absent --
	// see the header.)
	const auto DrawPillar = [&](float MockX)
	{
		const float WidthPx = 2.f * ScaleX;
		const float TopPx = 2.f * ScaleY;
		const float BottomPx = (kSvgH - 2.f) * ScaleY;
		const float SegmentPx = (BottomPx - TopPx) / 3.f;
		const float XPx = MockX * ScaleX;
		FillRect(OutDrawElements, LayerId, AllottedGeometry, XPx, TopPx, WidthPx, SegmentPx, Tint(Brass1));
		FillRect(OutDrawElements, LayerId, AllottedGeometry, XPx, TopPx + SegmentPx, WidthPx, SegmentPx, Tint(Brass3));
		FillRect(OutDrawElements, LayerId, AllottedGeometry, XPx, TopPx + SegmentPx * 2.f, WidthPx, SegmentPx,
		         Tint(Brass4));
		StrokeRect(OutDrawElements, LayerId, AllottedGeometry, XPx, TopPx, WidthPx, BottomPx - TopPx,
		           FLinearColor::Black);
	};
	DrawPillar(-4.f);
	DrawPillar(kSvgW + 2.f);

	// --- 6. Brass caps ------------------------------------------------------
	// Six mock-units tall, overhanging the diamond by six on each side. Drawn
	// last so they sit over the pillars. They extend OUTSIDE the widget's own
	// box, which is why nothing here sets ClipToBounds -- the Godot Control
	// does not clip either, and the overhang is the point.
	const auto DrawCap = [&](float MockY)
	{
		const float XPx = -6.f * ScaleX;
		const float YPx = MockY * ScaleY;
		const float WidthPx = (kSvgW + 12.f) * ScaleX;
		const float HeightPx = 6.f * ScaleY;
		const float BandPx = HeightPx * 0.5f;
		FillRect(OutDrawElements, LayerId, AllottedGeometry, XPx, YPx, WidthPx, BandPx, Tint(Brass1));
		FillRect(OutDrawElements, LayerId, AllottedGeometry, XPx, YPx + BandPx, WidthPx, BandPx, Tint(Brass3));
		StrokeRect(OutDrawElements, LayerId, AllottedGeometry, XPx, YPx, WidthPx, HeightPx, FLinearColor::Black);
	};
	DrawCap(-3.f);
	DrawCap(kSvgH - 3.f);

	return LayerId;
}
