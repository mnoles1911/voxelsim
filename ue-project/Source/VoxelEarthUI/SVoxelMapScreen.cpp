#include "SVoxelMapScreen.h"

#include "SVoxelScreenChrome.h"
#include "SVoxelScreenShell.h"
#include "VoxelUIAssetLibrary.h"
#include "VoxelUIStrings.h"
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Misc/Paths.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace SVoxelMapScreenDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

// world-maps/ sits at the REPOSITORY root, two levels above the .uproject:
// ProjectDir() is ue-project/, so its parent is the repo. Resolved rather than
// hardcoded so a checkout anywhere works, and absent rather than fatal when the
// tree has been packaged and world-maps/ did not come with it.
FString WorldMapsDir()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("..") / TEXT("world-maps"));
}
} // namespace SVoxelMapScreenDetail

SVoxelMapScreen::~SVoxelMapScreen()
{
	FVoxelUIStyle::UnregisterWidget();
}

FString SVoxelMapScreen::RasterPathForSeed(uint64 Seed)
{
	// The directory naming is the offline tool's, not this port's: every
	// world-maps subdirectory is `seed<decimal seed>`. A seed with no directory
	// simply has no map, which RequestImageFile reports as null.
	return SVoxelMapScreenDetail::WorldMapsDir()
	     / FString::Printf(TEXT("seed%llu"), (unsigned long long)Seed)
	     / TEXT("01-heightmap-hillshade.png");
}

TArray<FVoxelScreenAction> SVoxelMapScreen::Actions(bool bHasRaster)
{
	TArray<FVoxelScreenAction> Out;
	// Said in the action bar rather than only in a comment. Without a raster, a
	// player looking at blank parchment deserves to know it is blank because
	// none exists for this seed rather than because the map failed; WITH one,
	// they deserve to know the sheet is a fixed overview and that their own
	// position is the readout beside it, not a mark on the paper.
	Out.Add(FVoxelScreenAction(FText::GetEmpty(),
	                           bHasRaster ? VoxelUIStrings::MapOverviewNote()
	                                      : VoxelUIStrings::MapNoRaster()));
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

	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			BuildSheet()
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
					BuildPlacesDrawer()
				]
			]
		]
	];
}

TSharedRef<SWidget> SVoxelMapScreen::BuildSheet()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SOverlay> Sheet =
		SNew(SOverlay)
		// .parchment-frame -- three gradient stops flattened to their ends.
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Mix(MapFrameTop, MapFrameBottom)))
		];

	if (Data.bHasTerrainRaster)
	{
		// THE BRUSH IS AN ATTRIBUTE, NOT A ONE-OFF LOOKUP. RequestImageFile
		// kicks the decode and returns null until it lands -- which is ALWAYS
		// null on the first call, because the decode runs on a worker. An
		// earlier draft read it once during Construct and drew nothing, so the
		// raster never appeared however long the screen stayed open. Asking
		// every paint is what the asset library's own contract expects ("the
		// caller draws nothing that frame and asks again"), and after the first
		// success it is a map lookup.
		const FString RasterPath = RasterPathForSeed(Data.Seed);
		Sheet->AddSlot()
		[
			SNew(SImage)
			.Image_Lambda([RasterPath]() -> const FSlateBrush*
			{
				return FVoxelUIAssetLibrary::Get().RequestImageFile(RasterPath);
			})
			// Multiplied down towards the parchment rather than drawn at full
			// strength: the hillshade is a grey relief render and the sheet is
			// meant to read as an inked map, not as a screenshot of a DEM.
			.ColorAndOpacity(Tint(MapSheetPaper, 0.85f))
		];
	}

	// .sheet-grain and the two foxing stains are dropped: Slate has neither a
	// repeating-linear-gradient nor a radial brush, and unlike the vignettes
	// elsewhere in this port they carry nothing a player reads.

	// THERE IS NO YOU-ARE-HERE PIN, and that is the deliberate answer rather
	// than the missing half of one.
	//
	// The first capture of this screen drew the mock's `.you` pin at the frame
	// centre, which is where the mock puts it -- but the mock's sheet is
	// PANNED so the player is genuinely at the centre, and this one is a fixed
	// overview of a 261 km square. A pin at the centre of it says "you are at
	// the middle of the world" to a player standing 61 km from the origin. That
	// is not a rough position, it is a wrong one, and it is the same failure
	// mode the seed check above exists to prevent.
	//
	// PLACING IT CORRECTLY NEEDS THE RASTER'S GEOTRANSFORM, which the PNG does
	// not carry. What is known, and is recorded here so the next attempt does
	// not have to re-derive it: the offline tool renders 17x17 coarse tiles
	// spanning indices -16..0 (Config/DefaultGame.ini:37), a tile is 15.36 km
	// (:115), so the extent is -245.76 km to +15.36 km on both axes and the
	// legend's "261 x 261 km" agrees to within a rounding. What is NOT known is
	// the image's row order or whether its X runs with world X -- and guessing
	// either wrong puts the pin somewhere else on the map with no way for a
	// player to tell. So the READOUT is the position this screen states, to
	// four decimal places, and the sheet is labelled an overview.
	Sheet->AddSlot().HAlign(HAlign_Right).VAlign(VAlign_Bottom).Padding(FMargin(L.MapCompassInset))
	[
		BuildCompass()
	];

	// The 2 px black border and the bronze ring, both at VoxelUITheme::RulePx.
	// The mock's ring is 1 px; ADR-0011 forbids a one-unit band, so the inner
	// inset is 4 rather than 3 and the ring is two units.
	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
		]
		+ SOverlay::Slot().Padding(FMargin(2.f))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(BronzeDeep))
		]
		+ SOverlay::Slot().Padding(FMargin(VoxelUITheme::RulePx * 2.f))
		[
			Sheet
		];
}

TSharedRef<SWidget> SVoxelMapScreen::BuildCompass()
{
	using namespace VoxelUITheme;
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// THE ROSE IS FOUR LETTERS ON A DISC, not the mock's engraved card.
	// "Voxelmark Map.html" draws it as thirty-odd SVG polygons with split
	// light/dark facets and a 5-degree graduated limb -- a genuinely beautiful
	// drawing that Slate has no polygon primitive to reproduce. Four cardinal
	// letters around a parchment disc keep the one thing the card is FOR, which
	// is telling the player which way north is, and the ornament is left to the
	// day this front end has a vector brush.
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

	return SNew(SBox)
		.WidthOverride(L.MapCompassSize)
		.HeightOverride(L.MapCompassSize)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(ParchmentInk, 0.55f))
			]
			+ SOverlay::Slot().Padding(FMargin(2.f))
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Parchment, 0.75f))
			]
			+ SOverlay::Slot()[Letter(Letters[0], HAlign_Center, VAlign_Top, true)]
			+ SOverlay::Slot()[Letter(Letters[1], HAlign_Right, VAlign_Center, false)]
			+ SOverlay::Slot()[Letter(Letters[2], HAlign_Center, VAlign_Bottom, false)]
			+ SOverlay::Slot()[Letter(Letters[3], HAlign_Left, VAlign_Center, false)]
		];
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
