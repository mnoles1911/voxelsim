#include "SVoxelGameHud.h"

#include "SVoxelScreenChrome.h"
#include "VoxelGraphicsUserSettings.h"
#include "VoxelUIMusic.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelGameHudDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// The tape is two full turns plus a closing N, so the strip never runs out of
// segments at either end of a rotation -- exactly the mock's `for (pass=0;
// pass<2; pass++)` plus its `tape.push({deg:720,name:'N'})`.
constexpr int32 kSegmentsPerTurn = 12;   // one every 30 degrees
constexpr int32 kTapeSegments = kSegmentsPerTurn * 2 + 1;

// Which cardinal letter, if any, sits at this bearing.
FText CardinalAt(int32 Degrees)
{
	const TArray<FText>& Letters = VoxelUIStrings::MapCompassLetters();
	switch (((Degrees % 360) + 360) % 360)
	{
	case 0:   return Letters[0];
	case 90:  return Letters[1];
	case 180: return Letters[2];
	case 270: return Letters[3];
	default:  break;
	}
	return FText::GetEmpty();
}

// --- The music icons --------------------------------------------------------
//
// A TRIANGLE OUT OF A SQUARE AND A CLIP, and it is worth saying why this rather
// than the obvious alternative. Slate has no polygon brush and this port has no
// icon art (ADR-0011: "there is no icon texture in Content/UI at all"), so a
// triangle is either a custom OnPaint override, a staircase of boxes, or this.
// A staircase is a ladder of horizontal bars whose step height is fixed by the
// number of bars -- which is a pixel-locked primitive by another name, and
// ADR-0011 forbids exactly that. This is exact geometry at every scale:
//
//   VoxelOverlayChrome::Diamond already establishes that a 45-degree
//   FSlateRenderTransform on a box gives a diamond. A diamond is two triangles
//   that share a vertical edge down its middle, so CLIPPING one to its right
//   half leaves a right-pointing triangle with a flat left edge, and to its left
//   half a left-pointing one. The clip is EWidgetClipping::ClipToBounds, which
//   this same file already uses for the compass tape, and the half-width offset
//   is a negative left padding on a box-panel slot -- also the compass tape's
//   idiom, and honoured by Slate's layout rather than by a second transform.
//
// The square's side is Size/sqrt(2) so that its DIAGONAL is Size: the finished
// triangle is Size tall and Size/2 wide, which is the proportion a play glyph
// has in every media player ever shipped.
TSharedRef<SWidget> Triangle(float Size, bool bPointRight, const TAttribute<FSlateColor>& Colour)
{
	const float Square = Size * 0.70710678f; // side whose diagonal is Size

	TSharedRef<SWidget> Diamond =
		SNew(SBox)
		.WidthOverride(Size)
		.HeightOverride(Size)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(Square)
			.HeightOverride(Square)
			.RenderTransform(FSlateRenderTransform(FQuat2D(FMath::DegreesToRadians(45.f))))
			.RenderTransformPivot(FVector2D(0.5f, 0.5f))
			[
				SNew(SImage).Image(FVoxelUIStyle::Get().SolidWhite()).ColorAndOpacity(Colour)
			]
		];

	return SNew(SBox)
		.WidthOverride(Size * 0.5f)
		.HeightOverride(Size)
		.Clipping(EWidgetClipping::ClipToBounds)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			// Right-pointing shows the diamond's RIGHT half, so the diamond is
			// pulled half its width to the left and the clip keeps what is left.
			.Padding(FMargin(bPointRight ? -Size * 0.5f : 0.f, 0.f, 0.f, 0.f))
			[
				Diamond
			]
		];
}

// Two of the above with a gap between them: the skip glyphs. A PAIR rather than
// the usual triangle-plus-bar, because a bar at this size would be two units
// wide and read as a rendering artefact rather than as part of the icon.
//
// SIZED TO FIT THE SAME CELL AS THE PLAY TRIANGLE. Each half is Size-2 tall and
// therefore (Size-2)/2 wide, so the pair plus its 2-unit gap is exactly Size
// across -- otherwise the skip glyphs would overhang the plate's inner ring by a
// unit on each side, which is the sort of thing that only shows up in a capture.
constexpr float kSkipGapPx = 2.f;

TSharedRef<SWidget> DoubleTriangle(float Size, bool bPointRight, const TAttribute<FSlateColor>& Colour)
{
	const float Half = FMath::Max(2.f, Size - kSkipGapPx);
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			Triangle(Half, bPointRight, Colour)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		.Padding(FMargin(kSkipGapPx, 0.f, 0.f, 0.f))
		[
			Triangle(Half, bPointRight, Colour)
		];
}

// The pause glyph: two vertical bars. Plain boxes, and each is well over
// ADR-0011's two-unit floor at every size this is drawn at.
TSharedRef<SWidget> PauseBars(float Size, const TAttribute<FSlateColor>& Colour)
{
	const float Bar = FMath::Max(VoxelUITheme::RulePx, Size * 0.3f);
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	for (int32 I = 0; I < 2; ++I)
	{
		Row->AddSlot().AutoWidth().Padding(FMargin(I == 0 ? 0.f : Size * 0.2f, 0.f, 0.f, 0.f))
		[
			SNew(SBox)
			.WidthOverride(Bar)
			.HeightOverride(Size)
			[
				SNew(SImage).Image(FVoxelUIStyle::Get().SolidWhite()).ColorAndOpacity(Colour)
			]
		];
	}
	return Row;
}
} // namespace SVoxelGameHudDetail

SVoxelGameHud::~SVoxelGameHud()
{
	FVoxelUIStyle::UnregisterWidget();
}

void SVoxelGameHud::Construct(const FArguments& InArgs)
{
	FVoxelUIStyle::RegisterWidget();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	Data = InArgs._Data;

	// COORDINATOR DECISION, 2026-09-07: the BODY is what an overlay collapses,
	// not the whole HUD, so the music transport stays on screen and clickable
	// while a screen is open. Everything the mocks show belongs here; the
	// transport is deliberately outside it.
	BodyRoot =
		SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, L.HudCompassTop, 0.f, 0.f))
		[
			// HitTestInvisible STATED HERE, not inherited. The root used to
			// carry it for the whole tree, which also made it impossible for one
			// child to opt back in; the root is now SelfHitTestInvisible (see
			// the bottom of this function) and each body element says for itself
			// what it always effectively was.
			//
			// NOW A BOUND ATTRIBUTE rather than a constant, for the HIDE COMPASS
			// UI row (owner directive, 2026-09-08). Collapsed, not hidden: the
			// compass is the only thing in this slot, so there is nothing for it
			// to reserve space for, and Collapsed is what stops it being laid out
			// at all.
			SNew(SBox)
			.Visibility_Lambda([]()
			{
				return VoxelGraphicsUserSettings::GetHideCompassUI() ? EVisibility::Collapsed
				                                                     : EVisibility::HitTestInvisible;
			})
			[
				BuildCompass()
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(0.f, 0.f, 0.f, L.HudDockBottom))
		[
			SNew(SBox)
			.Visibility(EVisibility::HitTestInvisible)
			[
				BuildDock()
			]
		]
		// The interaction prompt, centre-right of the screen, hidden until
		// something sets it. See the header.
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SHorizontalBox)
			.Visibility_Lambda([this]()
			{
				return Cached.InteractPrompt.IsEmpty() ? EVisibility::Collapsed
				                                           : EVisibility::HitTestInvisible;
			})
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			.Padding(FMargin(0.f, 0.f, 7.f, 0.f))
			[
				SNew(SBox)
				.WidthOverride(L.HudInteractKeySize)
				.HeightOverride(L.HudInteractKeySize)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage).Image(FVoxelUIStyle::Get().SolidWhite())
						.ColorAndOpacity(VoxelUITheme::Tint(VoxelUITheme::Bronze))
					]
					// 2, NOT 1.5. ADR-0011 forbids any band under two units, and
					// two is what the compass frame in this same file uses for
					// the identical bronze band.
					+ SOverlay::Slot().Padding(FMargin(2.f))
					[
						SNew(SImage).Image(FVoxelUIStyle::Get().SolidWhite())
						.ColorAndOpacity(VoxelUITheme::Tint(FColor(0x14, 0x0e, 0x08), 0.85f))
					]
					+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this]() { return Cached.InteractKey; })
						.Font(FVoxelUIStyle::Get().Mono(L.HudSlotNumSize))
						.ColorAndOpacity(FVoxelUIStyle::TitleColour())
					]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() { return Cached.InteractPrompt; })
				.Font(FVoxelUIStyle::Get().HandItalic(L.HudInteractSize))
				.ColorAndOpacity(VoxelUITheme::Tint(VoxelUITheme::InkBright))
				.ShadowOffset(FVector2D(0.f, 1.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.85f))
			]
		];

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			BodyRoot.ToSharedRef()
		]
		// The music transport, top right. ANCHORED ON THE OUTER OVERLAY rather
		// than inside the body, and that placement is the whole point: an
		// SOverlay hands every slot the full geometry independently, so
		// collapsing the body cannot move this one. The cluster sits at exactly
		// the same top-right inset whether a screen is open or not.
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, L.HudMusicTop, L.HudMusicRight, 0.f))
		[
			BuildMusic()
		]
	];

	SetBodyVisible(true);

	// THE HUD STILL NEVER TAKES THE POINTER -- it is drawn over a world the
	// player is still playing in, and a hotbar cell that swallowed a click would
	// swallow a dig. SelfHitTestInvisible rather than HitTestInvisible says
	// exactly that and nothing more: this widget is not hittable, and its
	// children answer for themselves. Every element that was unhittable under
	// the old blanket flag now carries HitTestInvisible of its own (see the
	// slots above), so the ONLY behavioural difference is the music cluster,
	// which is hittable while the cursor is up and HitTestInvisible otherwise.
	SetVisibility(EVisibility::SelfHitTestInvisible);
}

void SVoxelGameHud::Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime)
{
	SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
	// The one read of the attribute per frame. See the header for why every
	// lambda below takes the cached copy instead of reading it again.
	Cached = Data.Get();
}

FMargin SVoxelGameHud::GetTapePadding() const
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	// CENTRE ON THE MIDDLE TURN, not on the first. The tape is two full
	// revolutions of twelve segments; centring the needle on segment 0 leaves
	// the entire left half of the frame empty, because there is no tape before
	// north -- which is exactly what the first HUD capture showed. Offsetting
	// by one further turn puts a complete revolution on each side of the
	// needle, so the strip is full at every bearing. That is the whole reason
	// the mock builds two passes rather than one.
	// The half-segment term centres the LABEL on the needle rather than the
	// segment's left edge. Without it the second capture put N thirty pixels
	// right of the gold rule at heading zero -- readable, and wrong by exactly
	// half a segment at every bearing.
	const float TurnWidth = (360.f / 30.f) * L.HudCompassSegWidth;
	const float Offset = L.HudCompassWidth * 0.5f
	                   - L.HudCompassSegWidth * 0.5f
	                   - TurnWidth
	                   - (Cached.HeadingDeg / 30.f) * L.HudCompassSegWidth;
	// ROUNDED TO A WHOLE UNIT, and what it buys is worth stating precisely. The
	// heading term is continuous, so the tape's left edge otherwise lands on an
	// arbitrary fraction and every tick and cardinal letter is resampled on a
	// different subpixel phase each frame. ADR-0011 makes the display scale
	// continuous too, so this does NOT buy pixel alignment -- it buys a STABLE
	// phase, which is what stops the letters shimmering as the player turns.
	// The tape advances one unit per half a degree at the shipped 60-unit
	// segment, below what a turning player can see.
	return FMargin(FMath::RoundToFloat(Offset), 0.f, 0.f, 0.f);
}

TSharedRef<SWidget> SVoxelGameHud::BuildBarTicks() const
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// `.bar::after` -- a repeating-linear-gradient that rules the bar into ten
	// 10% cells. Nine 1 px lines rather than a gradient, which is the same
	// substitution the rest of this port makes; drawn OVER both the fill and
	// the wound band, exactly as the CSS's z-index:3 puts it.
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	for (int32 I = 0; I < L.HudBarSegments; ++I)
	{
		Row->AddSlot().FillWidth(1.f)[SNullWidget::NullWidget];
		if (I < L.HudBarSegments - 1)
		{
			Row->AddSlot().AutoWidth()
			[
				// ADR-0011 promotes these ticks with everything else. THE ONE
				// PROMOTION MOST LIKELY TO LOOK WRONG: nine 2-unit rules on an
				// 11-unit bar is a lot of black where the CSS asks for 1 px.
				// Flagged for the owner's capture rather than exempted, because a
				// 1-unit tick smears at 1.333 exactly like every other rule.
				SNew(SBox).WidthOverride(RulePx)
				[
					SNew(SImage).Image(Style.SolidWhite())
					.ColorAndOpacity(Tint(FColor::Black, 0.55f))
				]
			];
		}
	}
	return SNew(SBox).Visibility(EVisibility::HitTestInvisible)[Row];
}

TSharedRef<SWidget> SVoxelGameHud::BuildCompass()
{
	using namespace VoxelUITheme;
	using namespace SVoxelGameHudDetail;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SHorizontalBox> Tape = SNew(SHorizontalBox);
	for (int32 I = 0; I < kTapeSegments; ++I)
	{
		const int32 Bearing = (I * 30) % 360;
		const FText Cardinal = CardinalAt(Bearing);
		const bool bCardinal = !Cardinal.IsEmpty();
		Tape->AddSlot().AutoWidth()
		[
			SNew(SBox)
			.WidthOverride(L.HudCompassSegWidth)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(bCardinal ? Cardinal : FText::AsNumber(Bearing))
				.Font(Style.Serif(bCardinal ? L.HudCompassCardSize : L.HudCompassSegSize))
				.ColorAndOpacity(bCardinal ? FVoxelUIStyle::TitleColour() : FVoxelUIStyle::DimColour())
				.ShadowOffset(FVector2D(1.f, 1.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
			]
		];
	}

	return SNew(SBox)
		.WidthOverride(L.HudCompassWidth)
		.HeightOverride(L.HudCompassHeight)
		[
			SNew(SOverlay)
			// The frame's ring stack: black border, bronze-deep, black, bronze.
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor(0x1a, 0x0d, 0x05)))
			]
			+ SOverlay::Slot().Padding(FMargin(2.f))
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Bronze))
			]
			// 4, NOT 3: the bronze band between this inset and the 2 above it
			// was one unit wide (ADR-0011). The tape's clip inset below moves
			// with it or the strip paints over the frame.
			+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx * 2.f))
			[
				SNew(SImage).Image(Style.SolidWhite())
				.ColorAndOpacity(Tint(Over(Mix(FColor(0x28, 0x1c, 0x10), FColor(0x14, 0x0e, 0x08)),
				                          0.85f, FColor::Black)))
			]
			// The sliding tape, clipped to the frame.
			+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx * 2.f))
			[
				SNew(SBox)
				.Clipping(EWidgetClipping::ClipToBounds)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(TAttribute<FMargin>::CreateSP(this, &SVoxelGameHud::GetTapePadding))
					[
						Tape
					]
				]
			]
			// .compass-needle -- a 2 px gold rule down the centre.
			+ SOverlay::Slot().HAlign(HAlign_Center)
			[
				SNew(SBox).WidthOverride(2.f)
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Gold))
				]
			]
		];
}

TSharedRef<SWidget> SVoxelGameHud::BuildDock()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SVerticalBox> Dock = SNew(SVerticalBox);

	// --- the vitals bars, gated on bHasVitals ---------------------------------
	// See the header for why nothing sets that flag yet.
	Dock->AddSlot().AutoHeight().Padding(FMargin(0.f, 0.f, 0.f, L.HudDockGap))
	[
		SNew(SBox)
		.WidthOverride(L.HudBarsWidth)
		.Visibility_Lambda([this]()
		{
			return Cached.bHasVitals ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SOverlay)
				// .bar.hp .wound -- the un-healable band, drawn BEHIND the fill
				// and from the same left edge, so the health bar's remaining
				// capacity reads as shortened rather than as merely low.
				+ SOverlay::Slot()
				[
					// THREE STOPS, NOT ONE. See VoxelUITheme::HpTop: every
					// `.bar .fill` and `.wound` in the HUD mock is a 180deg
					// ramp, and the flat overload was painting the midpoint of
					// the two ends it knew. The wound band has only two stops
					// in the CSS, so its middle is stated as their midpoint
					// rather than invented.
					VoxelScreenChrome::Track(L.HudBarHeight, HudWoundTop, Mix(HudWoundTop, HudWound),
					                         HudWound,
					                         TAttribute<float>::CreateLambda([this]()
					                         {
						                         return Cached.WoundFraction;
					                         }))
				]
				+ SOverlay::Slot()
				[
					VoxelScreenChrome::Track(L.HudBarHeight, HpTop, Hp, HpDeep,
					                         TAttribute<float>::CreateLambda([this]()
					                         {
						                         return Cached.HealthFraction;
					                         }))
				]
				+ SOverlay::Slot()[BuildBarTicks()]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(FMargin(L.HudBarGap, 0.f, 0.f, 0.f))
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					// HudHungerFill IS THE MIDDLE STOP, and it is blue on
					// purpose: the HUD mock carries its own :root and overrides
					// --stam to #6fb8d8. See the note at VoxelUITheme.h's
					// HudHungerFill before "correcting" this to gold.
					VoxelScreenChrome::Track(L.HudBarHeight, HudHungerTop, HudHungerFill, HudHungerDeep,
					                         TAttribute<float>::CreateLambda([this]()
					                         {
						                         return Cached.HungerFraction;
					                         }))
				]
				+ SOverlay::Slot()[BuildBarTicks()]
			]
		]
	];

	// --- the hotbar ----------------------------------------------------------
	// TEN CELLS ALWAYS, whatever the inventory holds. GetSlot's own comment
	// makes that the contract ("a HUD drawing ten boxes over a six-slot
	// inventory should draw four empty boxes, not crash"), and a dock whose
	// width changed with the inventory would move under the player's eye.
	TSharedRef<SHorizontalBox> Hotbar = SNew(SHorizontalBox);
	for (int32 I = 0; I < L.HudSlotCount; ++I)
	{
		Hotbar->AddSlot().AutoWidth().Padding(FMargin(L.HudSlotGap * 0.5f, 0.f))
		[
			// REBUILT EVERY PAINT would be wrong here: this is the one widget
			// that lives while the game runs. So each cell is a box whose
			// CONTENT is swapped by an attribute -- the slot chrome is built
			// once and only the item inside it is re-read.
			SNew(SBox)
			.WidthOverride(L.HudSlotSize)
			.HeightOverride(L.HudSlotSize)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor(0x1a, 0x0d, 0x05)))
				]
				+ SOverlay::Slot().Padding(FMargin(2.f))
				[
					SNew(SImage).Image(Style.SolidWhite())
					.ColorAndOpacity_Lambda([this, I]()
					{
						return Tint(Cached.SelectedSlot == I ? Gold : Bronze);
					})
				]
				+ SOverlay::Slot().Padding(FMargin(4.f))
				[
					SNew(SImage).Image(Style.SolidWhite())
					.ColorAndOpacity(Tint(Mix(FColor(0x1a, 0x14, 0x10), FColor(0x0a, 0x08, 0x05))))
				]
				+ SOverlay::Slot().Padding(FMargin(L.HudSlotGlyphInset))
				[
					SNew(SImage)
					.Image(Style.SolidWhite())
					.ColorAndOpacity_Lambda([this, I]()
					{
												if (!Cached.Hotbar.IsValidIndex(I) || Cached.Hotbar[I].IsEmpty())
						{
							return Tint(FColor::Black, 0.f);
						}
						// One flat band rather than VoxelScreenChrome's two:
						// the glyph helper builds a subtree, and this cell has
						// to change its item without rebuilding.
						return Tint(Bronze);
					})
				]
				+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(FMargin(5.f))
				[
					SNew(STextBlock)
					.Text(FText::AsNumber((I + 1) % 10))
					.Font(Style.Mono(L.HudSlotNumSize))
					.ColorAndOpacity_Lambda([this, I]()
					{
						return Cached.SelectedSlot == I ? FVoxelUIStyle::TitleColour()
						                                    : FVoxelUIStyle::MutedColour();
					})
					.ShadowOffset(FVector2D(1.f, 1.f))
					.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
				]
				+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Bottom).Padding(FMargin(5.f))
				[
					SNew(STextBlock)
					.Text_Lambda([this, I]()
					{
												if (!Cached.Hotbar.IsValidIndex(I) || Cached.Hotbar[I].Count <= 1)
						{
							return FText::GetEmpty();
						}
						return FText::AsNumber(Cached.Hotbar[I].Count);
					})
					.Font(Style.Mono(L.HudSlotQtySize))
					.ColorAndOpacity(FVoxelUIStyle::BodyColour())
					.ShadowOffset(FVector2D(1.f, 1.f))
					.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
				]
			]
		];
	}
	Dock->AddSlot().AutoHeight()[Hotbar];

	return Dock;
}

bool SVoxelGameHud::IsMusicButtonLit(int32 Which) const
{
	if (Which < 0 || Which >= kMusicButtonCount || !MusicButtons[Which].IsValid())
	{
		return false;
	}
	// NOT LIT WHILE THE CURSOR IS AWAY, and this is not cosmetic. The plate is
	// HitTestInvisible under a captured mouse, so Slate stops updating its hover
	// state -- but the LAST hover it saw would otherwise still be latched, and
	// the player would go back to looking around with one gold button stuck on.
	if (!Cached.bCursorVisible)
	{
		return false;
	}
	const TSharedPtr<SButton>& Button = MusicButtons[Which];
	// HOVER AND PRESSED ONLY -- NO KEYBOARD FOCUS TERM, deliberately, and it is
	// the same 2026-09-08 bug as the IsFocusable(false) above. SVoxelMenuButton's
	// IsLit counts focus because those buttons are driven by a stick and arrow
	// keys and have to show where you are; these three cannot be focused at all
	// now, so a focus term here would be a condition that can never come out
	// true -- dead code pretending to be a state.
	return Button->IsHovered() || Button->IsPressed();
}

TSharedRef<SWidget> SVoxelGameHud::BuildMusicButton(int32 Which, TSharedRef<SWidget> Icon,
                                                    const TAttribute<FText>& Tooltip,
                                                    FSimpleDelegate OnPressed)
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	return SNew(SBox)
		.WidthOverride(L.HudMusicButtonSize)
		.HeightOverride(L.HudMusicButtonSize)
		[
			// THE BUTTON PAINTS NOTHING. CartoucheButton is transparent in every
			// state, so SButton contributes the click, the hover and the focus
			// and the chrome below is drawn by this port -- the same division
			// SVoxelMenuButton's Cartouche and Leather variants use, for the
			// reason FVoxelUIStyle gives at that accessor.
			SAssignNew(MusicButtons[Which], SButton)
			.ButtonStyle(&Style.CartoucheButton())
			.ContentPadding(FMargin(0.f))
			// NOT FOCUSABLE, AND THIS IS A SHIPPED BUG (2026-09-08). A click
			// under hold-Tab left the button holding Slate keyboard focus, and
			// SButton activates a focused button on SPACEBAR by default -- which
			// is the player's ascend key (AVoxelEarthFlyPawn binds EKeys::SpaceBar
			// for jump/ascend, AVoxelGlider for its own). So every press of Space
			// after clicking the transport re-fired the last button clicked: the
			// owner's log shows a burst of "VoxelUIMusic: PREV ->" a second apart
			// at 04:09:47-04:10:06 walking the playlist 27 -> 22 while he was
			// flying, having touched nothing but Space.
			//
			// THE FIX IS TO REFUSE THE FOCUS, NOT TO REBIND SPACE. Space belongs
			// to the pawn; a HUD ornament that quietly claims a movement key is
			// the defect, and any button here that took focus would reintroduce
			// it. The transport is reachable by pointer and by its own hotkeys,
			// so it has no use for focus at all.
			.IsFocusable(false)
			.ToolTipText(Tooltip)
			.OnClicked_Lambda([OnPressed]()
			{
				OnPressed.ExecuteIfBound();
				return FReply::Handled();
			})
			[
				// The hotbar cell's ring stack at half size: black border,
				// bronze (gold when lit) ring, dark fill, glyph.
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor(0x1a, 0x0d, 0x05)))
				]
				+ SOverlay::Slot().Padding(FMargin(RulePx))
				[
					SNew(SImage).Image(Style.SolidWhite())
					.ColorAndOpacity_Lambda([this, Which]()
					{
						return Tint(IsMusicButtonLit(Which) ? Gold : Bronze);
					})
				]
				+ SOverlay::Slot().Padding(FMargin(RulePx * 2.f))
				[
					SNew(SImage).Image(Style.SolidWhite())
					.ColorAndOpacity(Tint(Mix(FColor(0x1a, 0x14, 0x10), FColor(0x0a, 0x08, 0x05))))
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.Padding(FMargin(L.HudMusicGlyphInset))
				[
					Icon
				]
			]
		];
}

TSharedRef<SWidget> SVoxelGameHud::BuildMusic()
{
	using namespace VoxelUITheme;
	using namespace SVoxelGameHudDetail;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	const float Glyph = FMath::Max(4.f, L.HudMusicButtonSize - L.HudMusicGlyphInset * 2.f);

	// One colour for the whole icon, tracking the plate it sits on. A lambda per
	// button rather than one shared attribute, because three buttons light
	// separately and a shared one would light all three together.
	auto IconColour = [this](int32 Which)
	{
		return TAttribute<FSlateColor>::CreateLambda([this, Which]() -> FSlateColor
		{
			return FSlateColor(Tint(IsMusicButtonLit(Which) ? Gold : Ink));
		});
	};

	// PLAY WHEN PAUSED, PAUSE WHEN PLAYING -- the button shows what pressing it
	// will DO, which is the convention every transport in the world follows and
	// the opposite of showing the current state.
	TSharedRef<SWidget> PlayPauseIcon =
		SNew(SOverlay)
		+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
		[
			SNew(SBox)
			.Visibility_Lambda([]()
			{
				return FVoxelUIMusic::Get().IsPaused() ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
			})
			[
				Triangle(Glyph, /*bPointRight=*/true, IconColour(kMusicPlay))
			]
		]
		+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
		[
			SNew(SBox)
			.Visibility_Lambda([]()
			{
				return FVoxelUIMusic::Get().IsPaused() ? EVisibility::Collapsed : EVisibility::HitTestInvisible;
			})
			[
				PauseBars(Glyph, IconColour(kMusicPlay))
			]
		];

	TSharedRef<SHorizontalBox> Row =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			BuildMusicButton(kMusicPrev,
			                 DoubleTriangle(Glyph, /*bPointRight=*/false, IconColour(kMusicPrev)),
			                 VoxelUIStrings::HudMusicPrevTip(),
			                 FSimpleDelegate::CreateLambda([]() { FVoxelUIMusic::Get().Previous(); }))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(L.HudMusicButtonGap, 0.f, 0.f, 0.f))
		[
			BuildMusicButton(kMusicPlay, PlayPauseIcon,
			                 TAttribute<FText>::CreateLambda([]()
			                 {
				                 return FVoxelUIMusic::Get().IsPaused() ? VoxelUIStrings::HudMusicPlayTip()
				                                                        : VoxelUIStrings::HudMusicPauseTip();
			                 }),
			                 FSimpleDelegate::CreateLambda([]() { FVoxelUIMusic::Get().TogglePause(); }))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(L.HudMusicButtonGap, 0.f, 0.f, 0.f))
		[
			BuildMusicButton(kMusicNext,
			                 DoubleTriangle(Glyph, /*bPointRight=*/true, IconColour(kMusicNext)),
			                 VoxelUIStrings::HudMusicNextTip(),
			                 FSimpleDelegate::CreateLambda([]() { FVoxelUIMusic::Get().Next(); }))
		];

	// OWNER, LIVE, 2026-09-08, after seeing the transport in game: "for the music
	// player in the top right, move the song/title name to the left of the back,
	// pause, and next buttons." So the cluster is ONE right-aligned row -- label,
	// gap, then the three plates -- rather than a row with a caption under it.
	return SNew(SHorizontalBox)
		// THE ONLY HIT-TESTABLE THING IN THIS WIDGET, and only while there is a
		// pointer to test with. Under a captured mouse this falls back to
		// HitTestInvisible -- the cluster is still drawn, still shows what is
		// playing, and cannot swallow a dig.
		.Visibility_Lambda([this]()
		{
			// THREE GATES, AND THEY ANSWER DIFFERENT QUESTIONS.
			//
			// HIDE MUSIC UI is the PLAYER, and it wins over both of the others
			// (owner directive, 2026-09-08): they have said they do not want to
			// see this, and neither an overlay nor a cursor is entitled to
			// overrule that. It hides the plates AND the label -- everything in
			// this slot -- and it does NOT stop the music: the , . / keys still
			// work, which is what the row's hint promises.
			if (VoxelGraphicsUserSettings::GetHideMusicUI())
			{
				return EVisibility::Collapsed;
			}
			// bMusicClusterAllowed is composition -- the death screen and the
			// dialogue overlay are full-bleed and take the transport off the
			// screen entirely.
			// bCursorVisible is input -- with the mouse captured the cluster is
			// still drawn and still shows what is playing, it just cannot be
			// clicked.
			if (!bMusicClusterAllowed)
			{
				return EVisibility::Collapsed;
			}
			return Cached.bCursorVisible ? EVisibility::Visible : EVisibility::HitTestInvisible;
		})
		// The now-playing line, FIRST in the row so it reads left-to-right into
		// the controls it describes. VAlign_Center puts its baseline on the
		// middle of the plates rather than on their top edge, and the right
		// padding is the gap the owner's "to the left of" implies.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		.Padding(FMargin(0.f, 0.f, L.HudMusicLabelGap, 0.f))
		[
			// NEVER hit-testable, whatever the plates beside it are doing: it is
			// a label, and a label that ate a click would be a bug with no
			// visible cause.
			SNew(SBox)
			.MaxDesiredWidth(L.HudMusicLabelWidth)
			// RIGHT-ALIGNED, and it matters more here than it did underneath.
			// The box shrink-wraps up to MaxDesiredWidth and then clips, so a
			// long filename loses its START; keeping the text right-aligned is
			// what puts the surviving end of the name against the plates rather
			// than leaving a gap between them.
			.HAlign(HAlign_Right)
			.Clipping(EWidgetClipping::ClipToBounds)
			.Visibility(EVisibility::HitTestInvisible)
			[
				SNew(STextBlock)
				.Text_Lambda([]()
				{
					// The filename, which is the only display name a loose .wav
					// in a gitignored folder has. Empty while silent, so an empty
					// library draws no orphan label beside the buttons.
					const FString& Name = FVoxelUIMusic::Get().NowPlaying();
					return Name.IsEmpty() ? FText::GetEmpty() : FText::FromString(Name);
				})
				.Font(Style.Serif(L.HudMusicLabelSize))
				.Justification(ETextJustify::Right)
				.ColorAndOpacity_Lambda([]()
				{
					// Dimmed further while paused, so "there is no music" and
					// "the music is stopped" are not the same picture.
					return Tint(FVoxelUIMusic::Get().IsPaused() ? InkMute : InkDim);
				})
				.ShadowOffset(FVector2D(1.f, 1.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			Row
		];
}

void SVoxelGameHud::SetBodyVisible(bool bVisible)
{
	// COORDINATOR DECISION, 2026-09-07: an overlay collapses the HUD BODY, not
	// the HUD. SelfHitTestInvisible rather than HitTestInvisible when shown,
	// because the body's three elements each carry HitTestInvisible themselves
	// (see Construct) and a blanket flag here would put back exactly the
	// all-or-nothing that made a clickable child impossible.
	if (BodyRoot.IsValid())
	{
		BodyRoot->SetVisibility(bVisible ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed);
	}
}

void SVoxelGameHud::SetMusicClusterVisible(bool bVisible)
{
	// A FLAG READ BY THE EXISTING ATTRIBUTE, NOT SetVisibility ON THE WIDGET.
	// The cluster's visibility is a bound lambda (it tracks the cursor every
	// frame); calling SetVisibility on it would replace that binding with a
	// fixed value and the cursor gate would never work again. This is the
	// smallest change that leaves the binding intact.
	bMusicClusterAllowed = bVisible;
}
