#pragma once
// The chrome the four 2026-09-07 overlay mocks share: the leather panel frame,
// the gold title, the hairline rules, the section captions, the key caps, and
// the ornament that stands in for the glyphs no shipped face carries.
//
// BUILDER FUNCTIONS, NOT A WIDGET CLASS. Nothing here holds state, takes focus
// or needs a lifetime -- each function returns a freshly built subtree that its
// caller owns. A class would add a registration to FVoxelUIStyle's live-widget
// count for every decorative rule on screen and buy nothing.
//
// WHY IT IS SHARED AT ALL. Pause, Settings, Save and Load are four separate
// mocks with one chrome vocabulary, described four times in four <style>
// blocks. Copying that into four .cpp files is how the save dialog ends up with
// a 2 px rule and the load dialog with a 1 px one -- a difference nobody would
// ever notice reviewing either file alone.

#include "CoreMinimal.h"
#include "Layout/Margin.h"
#include "Styling/SlateColor.h"

class SWidget;

namespace VoxelOverlayChrome
{
// The panel box every overlay dialog sits in: a hard drop shadow, a 2 px black
// border, the 1 px --leather-edge and 2 px --leather-1 inset rings, and the
// panel fill, with Content inset by Padding inside all of it.
//
// Height <= 0 means "as tall as the content", which is what .pa-panel,
// .se-panel and .sv-panel are; .ld-panel is the one fixed-height dialog.
VOXELEARTHUI_API TSharedRef<SWidget> Panel(TSharedRef<SWidget> Content, float Width, float Height,
                                           const FMargin& Padding);

// .pa-title / .se-title / .sv-title / .ld-title: centred, gold, tracked, with
// the mocks' `text-shadow:2px 2px 0 #000`.
VOXELEARTHUI_API TSharedRef<SWidget> Title(const FText& Text, int32 FontSize, int32 LetterSpacing);

// A 1 px --leather-edge rule (.pa-rule / .se-rule / .ld-rule, and the
// `border-top` above every dialog footer).
VOXELEARTHUI_API TSharedRef<SWidget> Rule(float Alpha = 1.f);

// .se-section: a small tracked --warm-primary caption followed by a rule that
// fades out to the right. Slate has no gradient brush, so the fade is the rule
// at a single reduced alpha -- the caption is what carries the meaning and a
// two-tone ramp across 400 px is not worth a stack of boxes.
VOXELEARTHUI_API TSharedRef<SWidget> SectionHeader(const FText& Text);

// The ornament that replaces the mocks' U+2726 (.sv-ctx-icon) and U+2205
// (.ld-empty .glyph). A cmap dump of the four shipped faces on 2026-09-07 found
// NEITHER codepoint in any of them, and what Slate draws for a missing glyph is
// its last-resort tile -- which is exactly what the previous capture of the
// title screen's callout showed before that star became a diamond. This is that
// same diamond, so the two ornaments in this front end are one ornament.
VOXELEARTHUI_API TSharedRef<SWidget> Diamond(float Size, const FSlateColor& Colour);

// One `<kbd>` cap: a tracked gold word on a --leather-2 plate. The dialogs'
// keyboard hints are built from these plus their own italic connective text,
// rather than from a single formatted string, because the cap has chrome.
VOXELEARTHUI_API TSharedRef<SWidget> KeyCap(const FText& Key);

// The wash the overlays sit on: flat black at FVoxelMenuLayout::OverlayDimAlpha.
//
// THE BLUR AND THE VIGNETTE ARE NOT FAKED. The mocks blur the paused world by
// 8 px and add `box-shadow: inset 0 0 200px` on top; Slate has neither a blur
// over the scene nor a radial-gradient brush, and the existing
// SceneVignetteAlpha field records the same decision for the title screen. The
// flat tint is set at the brightness the blur was there to support.
VOXELEARTHUI_API TSharedRef<SWidget> Scrim();
} // namespace VoxelOverlayChrome
