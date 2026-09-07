#pragma once
// The chrome the five in-game screens share BELOW the tab bar: the oak card,
// the iron well, the parchment list, the item slot, and the small captions that
// label them.
//
// A SECOND CHROME FILE, NOT AN ADDITION TO SVoxelOverlayChrome, because these
// are a different material vocabulary answering to a different mock family. The
// overlay dialogs are dark leather plates floating over a dimmed world; the
// in-game screens are oak and iron and parchment inside a bordered frame. They
// share the palette and nothing else, and one file called "chrome" holding both
// would be a file nobody can change safely.
//
// BUILDER FUNCTIONS, on SVoxelOverlayChrome's own reasoning: nothing here holds
// state or takes focus, and a class per decoration would add a registration to
// FVoxelUIStyle's live-widget count for every rule and stud on screen.
//
// THE ONE EXCEPTION IS ItemSlot, which takes an item by value and returns a
// finished cell. It is a function rather than a widget for the same reason, but
// it is worth saying why it is HERE rather than in the inventory screen: the
// pack, the hotbar, the crafting grid, the equipment paperdoll, the codex's
// recipe grid and the HUD's own hotbar are six places that draw the same 58 px
// cell, in four different files. They were one cell in the mock's CSS (.slot)
// and they are one function here.

#include "CoreMinimal.h"
#include "Layout/Margin.h"
#include "Styling/SlateColor.h"
#include "VoxelScreenData.h"

class SWidget;

namespace VoxelScreenChrome
{
// An oak card: 2 px black border, a 1 px --panel-oak-edge ring and the oak
// gradient fill, with Content inset by Padding. .char-card / .stat-block.
VOXELEARTHUI_API TSharedRef<SWidget> OakCard(TSharedRef<SWidget> Content, const FMargin& Padding);

// The dark well the slots sit in: #0a0805 inside a 2 px black border and a 1 px
// --panel-iron-edge ring. .pack-box / .side-box / .sk-tree-wrap.
VOXELEARTHUI_API TSharedRef<SWidget> IronWell(TSharedRef<SWidget> Content, const FMargin& Padding);

// .list-parchment: the parchment sheet the perk, faction and codex lists are
// written on. The CSS's two `radial-gradient(ellipse ...)` foxing stains are
// dropped rather than approximated -- Slate has no radial brush, and unlike the
// vignettes elsewhere in this port they carry no information at all.
VOXELEARTHUI_API TSharedRef<SWidget> ParchmentPanel(TSharedRef<SWidget> Content);

// .list-header: a tracked gold caption on a dark band across the top of a
// parchment panel. Columns are laid out right-aligned after the name.
VOXELEARTHUI_API TSharedRef<SWidget> ListHeader(const FText& Name, const TArray<FText>& Columns);

// .list-divider: an italic bronze rule-and-caption between groups of rows.
VOXELEARTHUI_API TSharedRef<SWidget> ListDivider(const FText& Text);

// .panel-head .name / .sub-title / .stat-block h3 -- a tracked gold caption.
// Meta is the .meta span some of them carry on the right; empty omits it.
VOXELEARTHUI_API TSharedRef<SWidget> PanelHeading(const FText& Text, const FText& Meta = FText::GetEmpty());

// One inventory cell. Empty items draw the bare socket, which is what a pack
// with fewer than 64 things in it is mostly made of.
//
// bHotbar picks the bronze ring the mock gives .hotbar .slot; KeyLabel is the
// 1..0 digit in its top-left corner, empty for a pack cell.
VOXELEARTHUI_API TSharedRef<SWidget> ItemSlot(const FVoxelInventoryScreenItem& Item, float Size,
                                              bool bHotbar = false, bool bSelected = false,
                                              const FText& KeyLabel = FText::GetEmpty());

// The .it glyph alone, without the socket around it -- for the list rows and
// ingredient lines that show an item at text size.
VOXELEARTHUI_API TSharedRef<SWidget> ItemGlyph(FName Glyph, float Size);

// A horizontal 1 px rule in --panel-oak-edge, for the hairlines inside a card.
VOXELEARTHUI_API TSharedRef<SWidget> CardRule(float Alpha = 1.f);

// A labelled progress track: the level bar, the reputation bar and the HUD's
// vitals are the same two boxes at three sizes. Fraction is 0..1.
VOXELEARTHUI_API TSharedRef<SWidget> Track(float Height, const FSlateColor& Fill,
                                           const TAttribute<float>& Fraction);
} // namespace VoxelScreenChrome
