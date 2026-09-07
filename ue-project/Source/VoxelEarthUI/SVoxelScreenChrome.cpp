#include "SVoxelScreenChrome.h"

#include "VoxelUIStrings.h" // ItemStackCount -- the cell's "x16" badge
#include "VoxelUIStyle.h"
#include "VoxelUITheme.h"

#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace VoxelScreenChromeDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.

constexpr float kBorderPx = 2.f;
// 2, NOT THE CSS'S 1. ADR-0011: no band may be one unit -- see
// VoxelUITheme::RulePx. Every stack below is built outside in, so a band's
// width is the difference between two consecutive insets; with kRingPx at 1 the
// oak card's edge, the iron well's edge and the item slot's rarity ring were
// each exactly one unit wide and smeared at any fractional scale. The chrome
// on each of those surfaces therefore grows by one unit per side.
constexpr float kRingPx = VoxelUITheme::RulePx;
const FColor kWellFill(0x0a, 0x08, 0x05);   // .pack-box / .side-box background
const FColor kSlotTop(0x1a, 0x14, 0x10);    // .slot linear-gradient stops
const FColor kSlotBottom(0x0a, 0x08, 0x05);

// --- The item glyphs --------------------------------------------------------
//
// THE MOCKS DRAW ITEMS AS CSS, NOT AS ART. Every `.it.*` class in
// menus_shared.css is a stack of linear-gradients and a clip-path -- there is
// no icon texture anywhere in the design project, and this game has no item
// icons either (FVoxelItemDef carries no image field of any kind).
//
// So the port cannot load an icon and will not invent one. What it does instead
// is keep the ONE property the CSS glyphs actually communicate at a 58 px cell:
// a two-band colour signature that distinguishes a sword from a log from a
// potion at a glance. Each entry below is the top and bottom stop of the mock's
// own gradient for that class, read out of the CSS, drawn as two stacked boxes
// inset inside the socket.
//
// This is deliberately a floor, not a ceiling: when real icons exist, ItemGlyph
// is the one function that changes and all six call sites follow.
struct FGlyphBands
{
	FColor Top;
	FColor Bottom;
};

FGlyphBands BandsFor(FName Glyph)
{
	static const TMap<FName, FGlyphBands> kTable = {
		// weapons and tools -- steel over a wooden haft
		{TEXT("sword"),      {FColor(0xd8, 0xd4, 0xc8), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("axe"),        {FColor(0xc8, 0xc0, 0xb0), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("pick"),       {FColor(0xb0, 0x7a, 0x3a), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("bow"),        {FColor(0x6b, 0x45, 0x20), FColor(0xd8, 0xd4, 0xc8)}},
		{TEXT("arrow"),      {FColor(0xd8, 0xd4, 0xc8), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("shield-i"),   {FColor(0x8a, 0x5a, 0x28), FColor(0x5a, 0x3a, 0x18)}},
		// armour
		{TEXT("helm"),       {FColor(0xc8, 0xc0, 0xb0), FColor(0x4a, 0x40, 0x38)}},
		{TEXT("armor"),      {FColor(0x8a, 0x83, 0x78), FColor(0x4a, 0x40, 0x38)}},
		{TEXT("boots"),      {FColor(0x6b, 0x45, 0x20), FColor(0x4a, 0x2f, 0x1a)}},
		{TEXT("ring"),       {FColor(0xd8, 0xa0, 0x50), FColor(0xb0, 0x3a, 0x6a)}},
		// consumables
		{TEXT("potion-hp"),  {FColor(0xd8, 0xd4, 0xc8), FColor(0xb8, 0x30, 0x2a)}},
		{TEXT("potion-mn"),  {FColor(0xd8, 0xd4, 0xc8), FColor(0x3a, 0x6f, 0xb8)}},
		{TEXT("meat"),       {FColor(0xc8, 0x88, 0x48), FColor(0x8a, 0x5a, 0x28)}},
		{TEXT("bread"),      {FColor(0xd8, 0xa8, 0x68), FColor(0xa8, 0x73, 0x20)}},
		// materials
		{TEXT("log"),        {FColor(0x8a, 0x5a, 0x28), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("plank"),      {FColor(0xa0, 0x6a, 0x30), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("stick"),      {FColor(0x8a, 0x5a, 0x28), FColor(0x4a, 0x2f, 0x1a)}},
		{TEXT("stone"),      {FColor(0x8a, 0x83, 0x78), FColor(0x2a, 0x24, 0x1f)}},
		{TEXT("cobble"),     {FColor(0x6e, 0x63, 0x58), FColor(0x3a, 0x34, 0x2d)}},
		{TEXT("iron"),       {FColor(0xc8, 0xc0, 0xb0), FColor(0x8a, 0x83, 0x78)}},
		{TEXT("goldb"),      {FColor(0xf0, 0xd8, 0x78), FColor(0xa8, 0x73, 0x20)}},
		{TEXT("gem"),        {FColor(0xe8, 0x4a, 0x3a), FColor(0x8a, 0x14, 0x10)}},
		{TEXT("coin"),       {FColor(0xf0, 0xc1, 0x4b), FColor(0xa8, 0x73, 0x20)}},
		{TEXT("clay"),       {FColor(0xa0, 0x6a, 0x50), FColor(0x6a, 0x40, 0x2a)}},
		{TEXT("glass"),      {FColor(0xc8, 0xe0, 0xe8), FColor(0x6a, 0x88, 0x98)}},
		{TEXT("wool"),       {FColor(0xe8, 0xe0, 0xd0), FColor(0xa8, 0xa0, 0x90)}},
		{TEXT("leather"),    {FColor(0xa0, 0x6a, 0x3a), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("string"),     {FColor(0xd8, 0xd0, 0xb0), FColor(0x8a, 0x83, 0x78)}},
		{TEXT("feather"),    {FColor(0xf0, 0xe0, 0xa8), FColor(0xa8, 0xa0, 0x90)}},
		// tools and misc
		{TEXT("torch"),      {FColor(0xf0, 0xc1, 0x4b), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("torchi"),     {FColor(0xf0, 0xc1, 0x4b), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("scroll"),     {FColor(0xe8, 0xd9, 0xb0), FColor(0xa8, 0x89, 0x5a)}},
		{TEXT("door"),       {FColor(0x8a, 0x5a, 0x28), FColor(0x4a, 0x2f, 0x1a)}},
		{TEXT("chest"),      {FColor(0xa0, 0x6a, 0x30), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("table"),      {FColor(0xa0, 0x6a, 0x30), FColor(0x8a, 0x5a, 0x28)}},
		{TEXT("furnace"),    {FColor(0x6e, 0x63, 0x58), FColor(0x2a, 0x24, 0x1f)}},
		{TEXT("anvil"),      {FColor(0x8a, 0x83, 0x78), FColor(0x3a, 0x34, 0x2d)}},
		{TEXT("ladder"),     {FColor(0x8a, 0x5a, 0x28), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("bucket"),     {FColor(0xc8, 0xc0, 0xb0), FColor(0x6e, 0x63, 0x58)}},

		// The attribute and skill glyphs (.g-* on the player screen, .sk-* in
		// the dialogue strip). Two families in the mocks, one here: where a
		// name appears in both -- speech, charisma, strength, speed, vitality
		// -- the CSS gives it the same gradient in each, so one entry serves.
		// Without these every attribute row would draw the same bronze
		// lozenge, and the six-row blocks would read as one texture.
		{TEXT("speech"),     {FColor(0xf0, 0xc1, 0x4b), FColor(0x6b, 0x45, 0x20)}},
		{TEXT("charisma"),   {FColor(0xf0, 0xc1, 0x4b), FColor(0xa8, 0x73, 0x20)}},
		{TEXT("speed"),      {FColor(0x6f, 0xbb, 0x47), FColor(0x2f, 0x7c, 0x1a)}},
		{TEXT("strength"),   {FColor(0xe8, 0x4a, 0x3a), FColor(0x5a, 0x14, 0x10)}},
		{TEXT("agility"),    {FColor(0x5f, 0xa8, 0x4a), FColor(0x2f, 0x7c, 0x1a)}},
		{TEXT("vitality"),   {FColor(0x5f, 0xa8, 0x4a), FColor(0x2f, 0x7c, 0x1a)}},
		{TEXT("noise"),      {FColor(0xa0, 0x4a, 0xc8), FColor(0x6a, 0x1a, 0x8a)}},
		{TEXT("vis"),        {FColor(0xa0, 0x4a, 0xc8), FColor(0x6a, 0x1a, 0x8a)}},
		{TEXT("consp"),      {FColor(0xa0, 0x4a, 0xc8), FColor(0x4a, 0x0c, 0x6a)}},
		{TEXT("coin"),       {FColor(0xf0, 0xc1, 0x4b), FColor(0xa8, 0x73, 0x20)}},
		{TEXT("rep"),        {FColor(0x4a, 0x86, 0xd8), FColor(0x17, 0x32, 0x58)}},
	};
	if (const FGlyphBands* Found = kTable.Find(Glyph))
	{
		return *Found;
	}
	// The misc lozenge: bronze over its own deep stop. What every unregistered
	// item and every block without a nicer match resolves to.
	return {VoxelUITheme::Bronze, VoxelUITheme::BronzeDeep};
}

FColor RarityColour(EVoxelItemRarity Rarity)
{
	using namespace VoxelUITheme;
	switch (Rarity)
	{
	case EVoxelItemRarity::Uncommon:  return RarityUncommon;
	case EVoxelItemRarity::Rare:      return RarityRare;
	case EVoxelItemRarity::Epic:      return RarityEpic;
	case EVoxelItemRarity::Legendary: return RarityLegendary;
	case EVoxelItemRarity::Common:    break;
	}
	// .slot with no rarity class keeps the plain iron ring.
	return Iron;
}
} // namespace VoxelScreenChromeDetail

namespace VoxelScreenChrome
{
using namespace VoxelUITheme;
using namespace VoxelScreenChromeDetail;

TSharedRef<SWidget> OakCard(TSharedRef<SWidget> Content, const FMargin& Padding)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
		]
		+ SOverlay::Slot().Padding(FMargin(kBorderPx))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(PanelOakEdge))
		]
		+ SOverlay::Slot().Padding(FMargin(kBorderPx + kRingPx))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Mix(PanelOak1, PanelOak2)))
		]
		+ SOverlay::Slot().Padding(Padding + FMargin(kBorderPx + kRingPx))
		[
			Content
		];
}

TSharedRef<SWidget> IronWell(TSharedRef<SWidget> Content, const FMargin& Padding)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
		]
		+ SOverlay::Slot().Padding(FMargin(kBorderPx))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(PanelIronEdge))
		]
		+ SOverlay::Slot().Padding(FMargin(kBorderPx + kRingPx))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(kWellFill))
		]
		+ SOverlay::Slot().Padding(Padding + FMargin(kBorderPx + kRingPx))
		[
			Content
		];
}

TSharedRef<SWidget> ParchmentPanel(TSharedRef<SWidget> Content)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
		]
		+ SOverlay::Slot().Padding(FMargin(kBorderPx))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(ParchmentEdge))
		]
		// ONE ring width, not two: this stack asked for `kRingPx * 2` back when
		// kRingPx was 1, i.e. it already wanted the 2-unit edge ADR-0011 now
		// gives every band. Leaving the doubling in would make it four.
		+ SOverlay::Slot().Padding(FMargin(kBorderPx + kRingPx))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Mix(Parchment, Parchment2)))
		]
		+ SOverlay::Slot().Padding(FMargin(kBorderPx + kRingPx))
		[
			Content
		];
}

TSharedRef<SWidget> ListHeader(const FText& Name, const TArray<FText>& Columns)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	TSharedRef<SHorizontalBox> Row =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Name)
			.Font(Style.Mono(L.ListHeaderSize))
			.ColorAndOpacity(FVoxelUIStyle::TitleColour())
		];
	for (const FText& Column : Columns)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(FMargin(24.f, 0.f, 0.f, 0.f))
		[
			SNew(STextBlock)
			.Text(Column)
			.Font(Style.Mono(L.ListHeaderSize))
			.ColorAndOpacity(FVoxelUIStyle::TitleColour())
		];
	}

	// The band is `rgba(60,40,20,.85)` over the parchment. Over() flattens it
	// rather than letting a translucent fill composite onto whatever happens to
	// be behind -- the measured bug its comment describes.
	static const FColor kBandTop(0x3c, 0x28, 0x14);
	static const FColor kBandBottom(0x28, 0x1c, 0x0e);
	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage)
			.Image(Style.SolidWhite())
			.ColorAndOpacity(Tint(Over(Mix(kBandTop, kBandBottom), 0.85f, Parchment)))
		]
		+ SOverlay::Slot().Padding(FMargin(L.ListHeaderPadX, L.ListHeaderPadY))
		[
			Row
		];
}

TSharedRef<SWidget> ListDivider(const FText& Text)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	FSlateFontInfo Font = Style.Serif(L.ListDividerSize);
	Font.LetterSpacing = L.ListDividerSpacing;
	return SNew(SBox)
		.Padding(FMargin(L.ListRowPadX, 8.f, L.ListRowPadX, 4.f))
		[
			SNew(STextBlock)
			.Text(Text)
			.Font(Font)
			.ColorAndOpacity(Tint(BronzeDeep))
		];
}

TSharedRef<SWidget> PanelHeading(const FText& Text, const FText& Meta)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();
	FSlateFontInfo Font = Style.Serif(L.PlayerBlockTitleSize);
	Font.LetterSpacing = L.PlayerBlockTitleSpacing;

	TSharedRef<SHorizontalBox> Row =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Text)
			.Font(Font)
			.ColorAndOpacity(FVoxelUIStyle::TitleColour())
			.ShadowOffset(FVector2D(1.f, 1.f))
			.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.9f))
		];
	if (!Meta.IsEmpty())
	{
		Row->AddSlot().FillWidth(1.f).HAlign(HAlign_Right).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Meta)
			.Font(Style.Mono(L.ListRowSize - 3))
			.ColorAndOpacity(FVoxelUIStyle::DimColour())
		];
	}
	return Row;
}

TSharedRef<SWidget> ItemGlyph(FName Glyph, float Size)
{
	const FGlyphBands Bands = BandsFor(Glyph);
	// A RAMP, NOT TWO FLAT BANDS. Every `.it.*` in the CSS is a
	// `linear-gradient(...)`; the port drew the two end stops as two hard blocks,
	// which is what the 2026-09-07 pass saw as "flat colour swatches". Four
	// bands is the smallest count that reads as a ramp rather than as a seam at
	// the 18-58 px sizes this is drawn at, and it is still four boxes per glyph.
	// The middle stop is the midpoint of the two the table carries -- the table
	// records the CSS's ends, not its interior.
	const FColor MidBand = Mix(Bands.Top, Bands.Bottom);
	return SNew(SBox)
		.WidthOverride(Size)
		.HeightOverride(Size)
		[
			VerticalRamp(Bands.Top, MidBand, Bands.Bottom, 4)
		];
}

TSharedRef<SWidget> ItemSlot(const FVoxelInventoryScreenItem& Item, float Size,
                             bool bHotbar, bool bSelected, const FText& KeyLabel)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// .slot's ring is the rarity colour when the item has one, the bronze deep
	// of .hotbar .slot when it is a hotbar cell, and plain iron otherwise. A
	// selected hotbar cell takes gold, which is the HUD mock's .slot.active.
	FColor Ring = bHotbar ? BronzeDeep : Iron;
	if (!Item.IsEmpty())
	{
		const FColor RarityRing = RarityColour(Item.Rarity);
		if (Item.Rarity != EVoxelItemRarity::Common)
		{
			Ring = RarityRing;
		}
	}
	if (bSelected)
	{
		Ring = Gold;
	}

	TSharedRef<SOverlay> Cell =
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
		]
		+ SOverlay::Slot().Padding(FMargin(kBorderPx))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Ring))
		]
		+ SOverlay::Slot().Padding(FMargin(kBorderPx + kRingPx))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(IronDeep))
		]
		+ SOverlay::Slot().Padding(FMargin(kBorderPx + kRingPx * 2.f))
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Mix(kSlotTop, kSlotBottom)))
		];

	if (!Item.IsEmpty())
	{
		// .it is `inset:6px` on a 56 px tile; scaled so the glyph keeps its
		// proportion at the 48, 58, 62, 64 and 66 px cells this is called at.
		// ROUNDED TO A WHOLE UNIT. Not load-bearing under ADR-0011 -- the scale
		// is continuous, so no authored value lands on a device pixel anyway --
		// but a fractional AUTHORED figure buys nothing, and 6.214 at the pack's
		// 58 px cell was an accident of the 6/56 ratio, not a design number.
		const float Inset = FMath::Max(4.f, FMath::RoundToFloat(Size * (6.f / 56.f)));
		Cell->AddSlot().Padding(FMargin(Inset))
		[
			ItemGlyph(Item.Glyph, Size - Inset * 2.f)
		];

		if (Item.Count > 1)
		{
			Cell->AddSlot().HAlign(HAlign_Right).VAlign(VAlign_Bottom).Padding(FMargin(4.f))
			[
				SNew(STextBlock)
				.Text(VoxelUIStrings::ItemStackCount(Item.Count))
				.Font(Style.Mono(L.InvSlotQtySize))
				.ColorAndOpacity(FVoxelUIStyle::BodyColour())
				.ShadowOffset(FVector2D(1.f, 1.f))
				.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
			];
		}

		// .dura -- the durability strip along the bottom of the cell, amber
		// under 40% and red under 20%. NOTHING IN THIS GAME HAS DURABILITY, so
		// this only ever draws for seeded rows; see FVoxelInventoryScreenItem.
		if (Item.Durability >= 0.f)
		{
			const FColor Fill = Item.Durability < 0.2f ? HpBright
			                  : Item.Durability < 0.4f ? RarityLegendary
			                                           : RarityUncommon;
			Cell->AddSlot().VAlign(VAlign_Bottom)
			.Padding(FMargin(kBorderPx + kRingPx * 2.f))
			[
				Track(L.InvDurabilityHeight, Tint(Fill), Item.Durability)
			];
		}
	}

	if (!KeyLabel.IsEmpty())
	{
		Cell->AddSlot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(FMargin(4.f, 2.f))
		[
			SNew(STextBlock)
			.Text(KeyLabel)
			.Font(Style.Mono(L.HudSlotNumSize))
			.ColorAndOpacity(bSelected ? FVoxelUIStyle::TitleColour() : FVoxelUIStyle::MutedColour())
			.ShadowOffset(FVector2D(1.f, 1.f))
			.ShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 1.f))
		];
	}

	return SNew(SBox).WidthOverride(Size).HeightOverride(Size)[Cell];
}

TSharedRef<SWidget> CardRule(float Alpha)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	return SNew(SBox)
		// ADR-0011: 2 units, not the CSS's 1 px. See VoxelUITheme::RulePx.
		.HeightOverride(RulePx)
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(PanelOakEdge, Alpha))
		];
}

TSharedRef<SWidget> VerticalRamp(const FColor& Top, const FColor& Mid, const FColor& Bottom, int32 Steps,
                                 float Alpha)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	const int32 BandCount = FMath::Max(2, Steps);

	TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);
	for (int32 I = 0; I < BandCount; ++I)
	{
		// The band's colour is sampled at its CENTRE, not its top edge: sampling
		// at the edge would put the Top stop on a band half a band tall and end
		// the ramp one band short of Bottom.
		const float T = (float(I) + 0.5f) / float(BandCount);
		// Two linear segments meeting at the middle stop, which is how a
		// three-stop CSS gradient interpolates.
		const FColor Lo = T < 0.5f ? Top : Mid;
		const FColor Hi = T < 0.5f ? Mid : Bottom;
		const float Local = T < 0.5f ? T * 2.f : (T - 0.5f) * 2.f;
		const FColor Band(
			uint8(FMath::RoundToInt(FMath::Lerp(float(Lo.R), float(Hi.R), Local))),
			uint8(FMath::RoundToInt(FMath::Lerp(float(Lo.G), float(Hi.G), Local))),
			uint8(FMath::RoundToInt(FMath::Lerp(float(Lo.B), float(Hi.B), Local))));

		Column->AddSlot().FillHeight(1.f)
		[
			SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(Band, Alpha))
		];
	}
	return Column;
}

TSharedRef<SWidget> Track(float Height, const FColor& FillTop, const FColor& FillMid, const FColor& FillBottom,
                          const TAttribute<float>& Fraction)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get(); // the black surround's brush
	return SNew(SBox)
		.HeightOverride(Height)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
			]
			// ADR-0011. On the HUD's 11-unit bar this takes the fill from 9
			// units to 7; flagged for the owner's capture with the tick rules.
			+ SOverlay::Slot().Padding(FMargin(RulePx))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(TAttribute<float>::CreateLambda([Fraction]()
				{
					return FMath::Clamp(Fraction.Get(0.f), 0.f, 1.f);
				}))
				[
					VerticalRamp(FillTop, FillMid, FillBottom)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(TAttribute<float>::CreateLambda([Fraction]()
				{
					return 1.f - FMath::Clamp(Fraction.Get(0.f), 0.f, 1.f);
				}))
				[
					SNullWidget::NullWidget
				]
			]
		];
}

TSharedRef<SWidget> Track(float Height, const FSlateColor& Fill, const TAttribute<float>& Fraction)
{
	const FVoxelUIStyle& Style = FVoxelUIStyle::Get();
	// A FILL SLOT, NOT A WIDTH OVERRIDE. The bar has to work at whatever width
	// its parent gives it -- the level bar spans a card, the reputation bar a
	// grid column, the HUD bars half of 694 px -- and only a two-slot
	// horizontal box expresses "this fraction of whatever I am".
	return SNew(SBox)
		.HeightOverride(Height)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Tint(FColor::Black))
			]
			// ADR-0011. On the HUD's 11-unit bar this takes the fill from 9
			// units to 7; flagged for the owner's capture with the tick rules.
			+ SOverlay::Slot().Padding(FMargin(RulePx))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(TAttribute<float>::CreateLambda([Fraction]()
				{
					return FMath::Clamp(Fraction.Get(0.f), 0.f, 1.f);
				}))
				[
					SNew(SImage).Image(Style.SolidWhite()).ColorAndOpacity(Fill)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(TAttribute<float>::CreateLambda([Fraction]()
				{
					return 1.f - FMath::Clamp(Fraction.Get(0.f), 0.f, 1.f);
				}))
				[
					SNullWidget::NullWidget
				]
			]
		];
}
} // namespace VoxelScreenChrome
