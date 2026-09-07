#include "SVoxelGameHud.h"

#include "SVoxelScreenChrome.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/Images/SImage.h"
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

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.f, L.HudCompassTop, 0.f, 0.f))
		[
			BuildCompass()
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(0.f, 0.f, 0.f, L.HudDockBottom))
		[
			BuildDock()
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
		]
	];

	// The HUD never takes the pointer: it is drawn over a world the player is
	// still playing in, and a hotbar cell that swallowed a click would swallow
	// a dig.
	SetVisibility(EVisibility::HitTestInvisible);
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
