#pragma once
// The front end's design tokens: the palette from the Godot build's
// assets/ui/Colors.gd, and every layout number the menu and loading screen
// use.
//
// ONE CONVERSION SITE, ON PURPOSE. Every colour below is an FColor holding the
// exact sRGB hex the source uses, and VoxelUITheme::Tint() is the ONLY place
// that turns one into the FLinearColor Slate wants. That matters more than it
// looks: FLinearColor(FColor) applies the sRGB->linear transfer function, and
// whether Slate then re-encodes it back to the same hex on screen is a
// property of the renderer, not something to assume. Funnelling every colour
// through one function makes that an experiment (voxel.UI.SRGBTint) instead of
// a rewrite. Until that experiment has been run, nothing in this port should
// be described as pixel-exact.
//
// LAYOUT NUMBERS ARE CONFIG-OVERRIDABLE for a specific reason. This project
// has no editor available to the people most likely to want to nudge the menu,
// and a Slate widget tree is no more designer-editable than a C++-built UMG
// one would be (only a real Widget Blueprint is, and authoring one needs the
// editor). So the next best thing is that every number lives in one struct
// that reads Config/DefaultVoxelUI.ini at startup -- a designer moves the
// title up four pixels with a text editor and no compiler.

#include "CoreMinimal.h"

namespace VoxelUITheme
{
// --- Palette (assets/ui/Colors.gd, autoload `Colors`) -----------------------
// Identical to the CSS :root block in assets/ui/css/menus_shared.css, which is
// what the HTML mocks in assets/ui/html/ are drawn against.

// Backgrounds
inline const FColor BgNight        = FColor(0x0d, 0x0a, 0x07);
inline const FColor BgStone        = FColor(0x1a, 0x14, 0x10);
// The main menu's own backing rect is NOT BgNight -- MainMenu.tscn hardcodes
// Color(0.04, 0.04, 0.06) = #0a0a0f, a cooler near-black than the palette's.
// Kept distinct rather than "corrected" to the palette value, because it is
// what the shipped screen actually renders when no background art loads.
inline const FColor MenuBackdrop   = FColor(0x0a, 0x0a, 0x0f);

// Oak panels
inline const FColor PanelOak1      = FColor(0x4a, 0x2f, 0x1a);
inline const FColor PanelOak2      = FColor(0x2e, 0x1b, 0x0d);
inline const FColor PanelOakEdge   = FColor(0x6b, 0x44, 0x22);

// Iron panels
inline const FColor PanelIron      = FColor(0x2a, 0x24, 0x1f);
inline const FColor PanelIronEdge  = FColor(0x4a, 0x40, 0x38);

// Parchment
inline const FColor Parchment      = FColor(0xe8, 0xd9, 0xb0);
inline const FColor Parchment2     = FColor(0xd4, 0xc0, 0x8c);
inline const FColor ParchmentEdge  = FColor(0xa8, 0x89, 0x5a);
inline const FColor ParchmentInk   = FColor(0x3a, 0x2a, 0x14);

// Metals
inline const FColor Bronze         = FColor(0xb0, 0x7a, 0x3a);
inline const FColor BronzeDeep     = FColor(0x6b, 0x45, 0x20);
inline const FColor Gold           = FColor(0xf0, 0xc1, 0x4b);
inline const FColor GoldDeep       = FColor(0xa8, 0x73, 0x20);
inline const FColor Iron           = FColor(0x6e, 0x63, 0x58);
inline const FColor IronDeep       = FColor(0x3a, 0x34, 0x2d);

// Vitals
inline const FColor Hp             = FColor(0xb8, 0x30, 0x2a);
inline const FColor HpDeep         = FColor(0x5a, 0x14, 0x10);
inline const FColor HpBright       = FColor(0xe8, 0x4a, 0x3a); // the DELETE button's text
inline const FColor Stam           = FColor(0xc8, 0xa0, 0x4a);
inline const FColor Hunger         = FColor(0x8a, 0x5a, 0x28);
inline const FColor Mana           = FColor(0x3a, 0x6f, 0xb8);

// Rarity
inline const FColor RarityCommon    = FColor(0x8a, 0x83, 0x78);
inline const FColor RarityUncommon  = FColor(0x5f, 0xa8, 0x4a);
inline const FColor RarityRare      = FColor(0x4a, 0x86, 0xd8);
inline const FColor RarityEpic      = FColor(0xa0, 0x4a, 0xc8);
inline const FColor RarityLegendary = FColor(0xf0, 0xa0, 0x2a);

// Text
inline const FColor Ink            = FColor(0xf3, 0xe6, 0xc4);
inline const FColor InkDim         = FColor(0xb4, 0xa0, 0x7a);
inline const FColor InkMute        = FColor(0x7a, 0x6a, 0x4e);

// --- Title-screen / overlay layer (menus_shared.css, 2026-09-07 revision) ----
// The Voxelmark mocks grew a second family of tokens for the atmospheric
// screens -- main menu, pause overlay, loading, death, the save/load and
// settings dialogs -- distinct from the in-game tab-bar menus above. Same
// source of truth (the CSS :root block), same one-conversion-site rule.
inline const FColor InkBright      = FColor(0xf0, 0xe3, 0xbf); // hero text, brighter than Ink
// --gold-glow / --gold-soft / --gold-rule are Gold at 0.6 / 0.18 / 0.7 alpha;
// spelled as constants so the three cannot be re-typed at three call sites.
inline constexpr float GoldGlowAlpha = 0.60f;
inline constexpr float GoldSoftAlpha = 0.18f;
inline constexpr float GoldRuleAlpha = 0.70f;
// The hover/active cartouche's fill tops out at 0.22 at its right edge; Slate
// has no gradient brush, so the port paints ONE translucent fill at the mean
// of the gradient's visible span (0.18..0.22) rather than faking a ramp with
// a stack of boxes.
inline constexpr float CartoucheFillAlpha = 0.20f;
inline const FColor WarmPrimary    = FColor(0xE8, 0x87, 0x3A);
inline const FColor WarmSecondary  = FColor(0x8B, 0x3A, 0x1A);
inline const FColor Leather1       = FColor(0x3a, 0x24, 0x12);
inline const FColor Leather2       = FColor(0x2a, 0x18, 0x0a);
inline const FColor LeatherEdge    = FColor(0x5a, 0x38, 0x18);
inline const FColor InkFaded       = FColor(0x5a, 0x3a, 0x1c);
// The patch-notes callout's tag colour (#d4a64a): between Gold and Bronze,
// named separately in the CSS and therefore here.
inline const FColor CalloutTag     = FColor(0xd4, 0xa6, 0x4a);
// Active title-menu item text (#fff5d0), warmer than white.
inline const FColor CartoucheText  = FColor(0xff, 0xf5, 0xd0);

// --- Overlay family (pause / settings / save / load, 2026-09-07 mocks) -------
// The four overlay mocks share one chrome vocabulary, and these are the colours
// it needs beyond the tokens above. Each names the CSS declaration it comes
// from, because none of them is in the :root block -- they are literals inside
// the four mocks' own <style> blocks, and a literal with no name is how two
// screens start disagreeing about the same colour.
//
// The panel body is `linear-gradient(180deg,rgba(28,18,10,.96),rgba(14,8,4,.98))`.
// Slate has no gradient brush; Mix() below collapses a two-stop gradient to its
// byte-space mean, so the two stops are named and the flattening happens at the
// call site where it can be seen.
inline const FColor OverlayPanelTop    = FColor(0x1c, 0x12, 0x0a); // rgba(28,18,10)
inline const FColor OverlayPanelBottom = FColor(0x0e, 0x08, 0x04); // rgba(14,8,4)
inline constexpr float OverlayPanelAlpha = 0.965f;                 // .96 -> .98
// .ld-row / .sv-context fill, and the .ld-row:hover it lifts to.
inline const FColor OverlayRowTop      = FColor(0x3a, 0x24, 0x12); // rgba(58,36,18)
inline const FColor OverlayRowBottom   = FColor(0x1c, 0x12, 0x0a); // rgba(28,18,10)
inline constexpr float OverlayRowAlpha = 0.55f;
inline const FColor OverlayRowHoverTop    = FColor(0x4e, 0x2e, 0x16); // rgba(78,46,22)
inline const FColor OverlayRowHoverBottom = FColor(0x28, 0x18, 0x0c); // rgba(40,24,12)
inline constexpr float OverlayRowHoverAlpha = 0.60f;
// The dark inset of a slider track / checkbox well.
inline const FColor WellTop            = FColor(0x1a, 0x0e, 0x04);
inline const FColor WellBottom         = FColor(0x0a, 0x05, 0x02);
// Text on a warm-primary plate (.se-act.primary, .ld-filter.on, .ld-tag.latest).
inline const FColor OnWarm             = FColor(0xff, 0xf8, 0xe0);
// .pa-btn.danger at rest and on hover; .ld-btn.del at rest.
inline const FColor DangerRest         = FColor(0xe6, 0xaa, 0x96); // rgba(230,170,150,.7)
inline const FColor DangerHover        = FColor(0xe8, 0xa0, 0x90);
inline const FColor DeleteRest         = FColor(0xe6, 0x96, 0x82); // rgba(230,150,130,.8)
// .sv-warn: an italic line on a rust wash behind a warm-secondary bar, with the
// colliding name itself picked out in gold.
inline const FColor WarnText           = FColor(0xe8, 0xb0, 0x90);
inline const FColor WarnWash           = FColor(0x8c, 0x3c, 0x1e); // rgba(140,60,30)
inline constexpr float WarnWashAlpha   = 0.20f;
inline const FColor WarnName           = FColor(0xf5, 0xd0, 0x90);
// .mn-btn.depth -- the mining cycler's "carves into terrain" arm reads green,
// which is the one non-warm accent in the whole overlay family.
inline const FColor AnchorGreen        = FColor(0xb6, 0xe8, 0xa0);
// The four .ld-thumb classes, as gradient pairs plus their glyph colour.
inline const FColor ThumbCaveTop       = FColor(0x0a, 0x05, 0x02);
inline const FColor ThumbCaveBottom    = FColor(0x28, 0x18, 0x0c);
inline const FColor ThumbCaveInk       = FColor(0xa0, 0x70, 0x50);
inline const FColor ThumbFieldTop      = FColor(0x1a, 0x20, 0x08);
inline const FColor ThumbFieldBottom   = FColor(0x3a, 0x38, 0x12);
inline const FColor ThumbFieldInk      = FColor(0xa8, 0x90, 0x50);
inline const FColor ThumbDeepTop       = FColor(0x02, 0x08, 0x12);
inline const FColor ThumbDeepBottom    = FColor(0x0a, 0x18, 0x28);
inline const FColor ThumbDeepInk       = FColor(0x50, 0x80, 0xa0);
inline const FColor ThumbHolyTop       = FColor(0x18, 0x0c, 0x04);
inline const FColor ThumbHolyBottom    = FColor(0x3a, 0x20, 0x08);

// --- Loading-screen extras --------------------------------------------------
// Not in Colors.gd; hardcoded in LoadingHourglass.gd and TransitionManager.gd,
// straight from the mock's :root block.
inline const FColor SandBright     = FColor(0xF5, 0xD0, 0x6E); // the progress bar fill
inline const FColor SandMid        = FColor(0xD9, 0xA8, 0x4A);
inline const FColor SandDeep       = FColor(0xA8, 0x73, 0x20);
inline const FColor Brass1         = FColor(0xd8, 0xa0, 0x50);
inline const FColor Brass2         = FColor(0xa8, 0x73, 0x20);
inline const FColor Brass3         = FColor(0x6b, 0x45, 0x20);
inline const FColor Brass4         = FColor(0x3a, 0x24, 0x10);
inline const FColor TopSurface     = FColor(0xFF, 0xE9, 0xA8); // sand surface highlight
// The TIP footer's gold prefix. TransitionManager._format_tip uses #f0c14b,
// which IS Gold above -- named separately only because the source names it
// separately, and a later divergence should not be a silent one. Called
// TipGold rather than TipPrefix so it cannot be confused with
// VoxelUIStrings::TipPrefix(), which is the WORD "TIP".
inline const FColor TipGold        = FColor(0xf0, 0xc1, 0x4b);

// The one sRGB->linear conversion site. Alpha is applied on top of the
// colour's own (every token above is opaque), so Tint(Gold, 0.55f) is the
// idiom for the Godot build's `Color(GOLD, 0.55)`.
VOXELEARTHUI_API FLinearColor Tint(const FColor& Colour, float Alpha = 1.0f);

// Godot's Color.darkened(k), ported literally.
//
// THIS IS NOT FLinearColor * (1-k). Godot's implementation multiplies the
// stored sRGB components, and its Colors are stored non-linearly, so
// PANEL_OAK_2.darkened(0.15) is a scaling of the BYTES -- #2e1b0d becomes
// #27170b, not the rather different colour a linear-space scale would give.
// The pressed and disabled button states are both defined this way, so getting
// it wrong is visible on every menu.
VOXELEARTHUI_API FColor Darkened(const FColor& Colour, float Amount);

// A two-stop CSS linear-gradient, flattened to one colour.
//
// SLATE HAS NO GRADIENT BRUSH, and the overlay mocks are built almost entirely
// out of two-stop vertical gradients -- every panel, plate, row, well and knob.
// Faking a ramp with a stack of boxes would multiply the widget count of every
// screen here for a difference of a few values of luminance across 18 pixels;
// the port paints the MEAN instead, in the same byte space Darkened works in
// and for the same reason (these are sRGB hex values, not linear ones). Naming
// both stops at the call site keeps the CSS readable from the C++.
VOXELEARTHUI_API FColor Mix(const FColor& A, const FColor& B);

// Src at Alpha composited over Dst, returning an OPAQUE colour.
//
// THIS EXISTS BECAUSE OF A MEASURED BUG. The overlay rows are `rgba(58,36,18,
// .55)` over the panel, and the port drew them the obvious way: a translucent
// fill inset by 1 px inside a full-bleed rect in the border colour. But a
// full-bleed border is BEHIND the fill as well as around it, so the row
// composited over --leather-edge instead of over the panel. The 2026-09-07 load
// capture measured the result at #584129 against an intended #22150b -- roughly
// twice as bright, and the whole dialog read as a different material.
//
// Flattening the blend HERE, at authoring time, is the fix that keeps the
// stacked-border idiom (which every panel, plate and well in this front end
// uses) and cannot be got wrong per call site. Byte space, like Mix and
// Darkened, because these are sRGB hex values.
VOXELEARTHUI_API FColor Over(const FColor& Src, float Alpha, const FColor& Dst);
} // namespace VoxelUITheme

// --- Layout -----------------------------------------------------------------
//
// Every number the front end positions anything with. Defaults are the values
// the Godot build actually uses (MainMenu.gd's _build_* functions and
// TransitionManager._build_loading_screen), NOT the HTML mock's -- the two
// disagree, and the shipped screen is the thing being cloned. Where the mock
// differs the comment says so.
struct VOXELEARTHUI_API FVoxelMenuLayout
{
	// --- Main menu ----------------------------------------------------------
	// MainMenu.gd: _main_panel offsets (-260,-400)-(260,400).
	float MainPanelHalfWidth   = 260.f;
	float MainPanelHalfHeight  = 400.f;
	float MainColumnSeparation = 14.f;
	float TitleToButtonsSpacer = 36.f;
	float QuitSpacer           = 80.f;
	float ButtonMinHeight      = 56.f;
	int32 ButtonFontSize       = 24;
	// 84, not the mock's 108, and with no letter-spacing (the mock asks for
	// 10px; Slate has no tracking and the Godot build applies none either).
	int32 TitleFontSize        = 84;
	// THE TITLE GETS ITS OWN WIDTH, WIDER THAN THE PANEL. Without this the
	// title is laid out inside MainPanelHalfWidth*2 = 520 and Slate's text
	// layout cuts it at that boundary: the first capture rendered "VOXELMARK"
	// as "OXELMAR", losing the V and the K, clipped symmetrically. 520 is the
	// Godot _main_panel width and it governs the BUTTON column; the title was
	// never meant to inherit it.
	//
	// 720 IS MEASURED, NOT GUESSED, and the measurement is the interesting
	// part. The gold title pixels in the first capture spanned centre-347 to
	// centre+346 of a 2560-wide shot -- symmetric, and 347 = 260 x 1.335, which
	// identified both the clipper and the layout scale. Backing the visible
	// substring out against the face's advance widths puts the full string at
	// 659 local units. 720 is that plus headroom.
	//
	// DO NOT RE-DERIVE THIS FROM THE FONT FILE. MacondoSwashCaps at nominal
	// 84 px measures 483 px advance and 518 px ink for "VOXELMARK", which fits
	// 520 comfortably -- and it does not. Slate lays the string out about 25%
	// wider than the raw face metrics predict, so an offline measurement
	// clears this as fine. Only a rendered capture is trustworthy here; if the
	// title string or TitleFontSize changes, re-shoot -Shot Menu and re-measure
	// the span rather than recomputing it. See backlog 0.0l.
	float TitleBoxWidth        = 720.f;
	int32 SubtitleFontSize     = 18;
	// 16 and (36, 24): the 2026-09-07 mock's .version-stamp.bl. Was 12/(16,16).
	int32 VersionFontSize      = 16;
	float VersionInsetLeft     = 36.f;
	float VersionInsetBottom   = 24.f;
	// The dark wash over the background art. 0.50 is the mock's .scene-tint
	// (was MainMenu.gd's BG_NIGHT @ 0.55); the vignette that used to be folded
	// into it is now its own layer, see SceneVignetteAlpha.
	float BackgroundTintAlpha  = 0.50f;
	// .scene-vignette: transparent to 0.85 black at the frame edge. Slate has
	// no radial-gradient brush and this pass does NOT fake one; the field is
	// reserved so the day a vignette texture is added it is a config value and
	// not a magic number. Until then the flat tint above is the whole wash.
	float SceneVignetteAlpha   = 0.85f;

	// --- Title screen (Voxelmark Main Menu.html, 2026-09-07) ----------------
	// THE MOCK IS AUTHORED AT 1920x1080 and shown at zoom 1.333 for 1440p; every
	// number below is the 1080p figure, which is the base the rest of this
	// struct already uses. Slate's DPI scale does the 1.333.
	//
	// The menu moved: the logo sits top-RIGHT, the list of items hangs under it
	// right-aligned, and a patch-notes callout takes the top-left. The centred
	// oak column above is what the previous mock had; its numbers stay because
	// the sub-panels still use them.
	float LogoTop              = 80.f;
	// 100 in the mock. Macondo's swash K overhangs its advance by 0.42 em (73
	// px at 1440p), which Slate right-aligns without: the 2026-09-07 capture
	// measured 34 px of ink margin against the mock's ~60. +20 restores it.
	float LogoRight            = 120.f;
	int32 LogoFontSize         = 132;
	// letter-spacing in the mock is px; FSlateFontInfo::LetterSpacing is
	// 1/1000 em. 6 px at 132 px = 45. (The earlier note that Slate "has no
	// tracking" predates UE5's LetterSpacing and is retired by this field.)
	int32 LogoLetterSpacing    = 45;
	float TitleMenuTop         = 400.f;
	float TitleMenuRight       = 160.f;
	float TitleMenuWidth       = 640.f;
	int32 TitleMenuItemSize    = 29;
	int32 TitleMenuActiveSize  = 33;   // hover/active grows the face
	int32 TitleMenuLetterSpacing = 172; // 5 px at 29 px
	float TitleMenuGap         = 10.f;
	float TitleMenuItemPadX    = 32.f;
	float TitleMenuItemPadY    = 10.f;
	float TitleMenuItemMinWidth = 380.f;
	float TitleMenuQuitGap     = 52.f;  // .title-menu__item.quit margin-top
	// .callout-news
	float CalloutTop           = 80.f;
	float CalloutLeft          = 60.f;
	float CalloutMaxWidth      = 520.f;
	float CalloutPadX          = 14.f;
	float CalloutGap           = 16.f;
	float CalloutGlyphSize     = 64.f;
	// The ornament inside the glyph box: a gold square turned 45 degrees,
	// standing in for the mock's U+2726 star (no shipped face carries it).
	float CalloutOrnamentSize  = 22.f;
	int32 CalloutTagSize       = 16;
	int32 CalloutTitleSize     = 22;
	int32 CalloutCopySize      = 16;

	// --- Sub-panels (load / help / credits) ---------------------------------
	// All three share geometry: offsets (-360,-280)-(360,280).
	float SubPanelHalfWidth    = 360.f;
	float SubPanelHalfHeight   = 280.f;
	float SubPanelPadding      = 16.f;
	float SubPanelSeparation   = 10.f;
	int32 SubPanelTitleSize    = 32;
	int32 SubPanelBodySize     = 18;
	float SaveRowMinHeight     = 60.f;
	float SaveRowSeparation    = 6.f;
	int32 SaveRowFontSize      = 16;
	float SaveRowButtonWidth   = 96.f;
	float SaveRowButtonHeight  = 44.f;
	int32 SaveRowButtonFont    = 16;
	float DialogButtonWidth    = 160.f;
	float DialogButtonHeight   = 44.f;

	// --- Overlay family (2026-09-07 mocks) ----------------------------------
	// Pause, Settings, Save and Load share one chrome: a dark leather panel over
	// a dimmed world. Every number below is the 1080p figure from the mock whose
	// CSS class the comment names.
	//
	// THE PAUSE PANEL IS PORTED UNSCALED, which is the one deliberate divergence
	// in this block. "Voxelmark Pause Menu.html" puts `scale(0.7)` on .pa-panel
	// on top of the shared 1.333 document zoom, which would land a 266 px panel
	// next to a 600 px settings panel and a 780 px load dialog from the same
	// family. The scale reads as the author shrinking one card to sit beside two
	// other states in the same file, not as a design intent, so the authored
	// 380 px is what is built.
	float OverlayBorderPx      = 2.f;  // border:2px solid #000
	float OverlayEdgeRingPx    = 1.f;  // inset 0 0 0 1px --leather-edge
	float OverlayInnerRingPx   = 3.f;  // inset 0 0 0 3px --leather-1
	float OverlayShadowSize    = 24.f; // 0 8px 24px rgba(0,0,0,.8)
	float OverlayShadowOffsetY = 8.f;
	// The paused world behind the panel. The mock blurs it and drops it to 35%
	// brightness; Slate has no blur brush over the scene, so the port draws a
	// flat black wash at the brightness the blur was there to support.
	float OverlayDimAlpha      = 0.60f;
	// .vignette: inset 0 0 200px rgba(0,0,0,.65). Slate has no radial gradient;
	// reserved on the same terms as SceneVignetteAlpha above.
	float OverlayVignetteAlpha = 0.65f;

	// .pa-panel / .pa-title / .pa-btn / .pa-foot
	float PausePanelWidth      = 380.f;
	float PausePanelPadX       = 28.f;
	float PausePanelPadY       = 32.f;
	float PausePanelGap        = 6.f;
	int32 PauseTitleSize       = 34;
	int32 PauseTitleLetterSpacing = 176; // 6 px at 34 px
	float PauseRuleTopGap      = 10.f;   // .pa-title margin-bottom 6 + .pa-rule margin-top 4
	float PauseRuleBottomGap   = 14.f;
	int32 PauseItemSize        = 22;
	int32 PauseItemLetterSpacing = 136;  // 3 px at 22 px
	float PauseItemPadY        = 10.f;
	float PauseItemChevronGap  = 14.f;
	int32 PauseChevronSize     = 24;
	int32 PauseFootSize        = 13;
	float PauseFootTopGap      = 14.f;

	// .se-panel and its rows
	float SettingsPanelWidth   = 600.f;
	// The mock is `max-height:calc(100vh - 24px)` on a panel whose content
	// happens to fit; this port keeps the four GRAPHICS rows the earlier
	// settings panel already had ON TOP of the mock's audio and display
	// sections, so it does not fit and the body scrolls. A FIXED height rather
	// than a max, because a dialog that changes size as rows are added is a
	// dialog whose footer moves.
	// 660: measured against the built panel (2026-09-07 settings capture), which
	// at 780 left a third of itself empty under the last row.
	float SettingsPanelHeight  = 660.f;
	float SettingsPanelPadX    = 32.f;
	float SettingsPanelPadTop  = 18.f;
	float SettingsPanelPadBottom = 14.f;
	float SettingsPanelGap     = 10.f;
	int32 SettingsTitleSize    = 24;
	int32 SettingsTitleLetterSpacing = 167; // 4 px at 24 px
	int32 SettingsSectionSize  = 13;
	int32 SettingsSectionLetterSpacing = 154; // 2 px at 13 px
	float SettingsLabelColumn  = 130.f; // grid-template-columns 130px 1fr 50px
	float SettingsRowGap       = 14.f;
	int32 SettingsRowLabelSize = 18;
	float SliderTrackHeight    = 18.f;
	float SliderKnobWidth      = 14.f;
	float SliderKnobHeight     = 24.f;
	float SliderValueWidth     = 50.f;
	int32 SliderValueSize      = 14;
	float CheckboxSize         = 22.f;
	float CheckboxMarkSize     = 14.f;
	int32 SettingsCyclerSize   = 15;
	int32 SettingsBadgeSize    = 11;
	int32 SettingsHelpSize     = 12;
	int32 SettingsActionSize   = 15;
	int32 SettingsActionLetterSpacing = 200; // 3 px at 15 px
	float SettingsActionPadX   = 24.f;
	float SettingsActionPadY   = 9.f;
	int32 SettingsHintSize     = 13;
	// THE HINT WRAPS AT AN EXPLICIT WIDTH, NOT WITH AutoWrapText, and this is a
	// measurement rather than a preference. Inside the panel's SScrollBox an
	// auto-wrapping block wrapped at a width WIDER than the panel and was
	// clipped at its border -- two settings captures on 2026-09-07 showed the
	// graphics descriptions losing their last word, while the short FULLSCREEN
	// hint on the same pass wrapped after three. An auto-wrap resolves against
	// the allotted geometry, which a scroll box measures differently from what
	// it later paints; a number does not have that problem.
	//
	// 460 = 600 panel - 64 side padding - 10 border and rings - 16 scrollbar -
	// 36 row indent, less a few pixels of headroom. Re-derive it if
	// SettingsPanelWidth or the indent moves.
	float SettingsHintWrapWidth = 460.f;

	// .sv-panel (Voxelmark Save Dialog.html)
	float SaveDialogWidth      = 560.f;
	float SaveDialogPadX       = 36.f;
	float SaveDialogPadTop     = 28.f;
	float SaveDialogPadBottom  = 22.f;
	float SaveDialogGap        = 14.f;
	int32 SaveDialogTitleSize  = 28;
	int32 SaveDialogTitleLetterSpacing = 214; // 6 px at 28 px
	float SaveContextIconSize  = 38.f;
	int32 SaveContextNameSize  = 17;
	int32 SavePromptSize       = 13;
	int32 SavePromptLetterSpacing = 231; // 3 px at 13 px
	int32 SaveInputSize        = 22;
	float SaveInputPadX        = 14.f;
	float SaveInputPadY        = 12.f;
	int32 SaveCounterSize      = 12;
	// The right half of `.sv-input`'s `padding:12px 90px 12px 14px` -- the gutter
	// the counter sits in, kept clear so a long name never runs under it.
	float SaveCounterGutter    = 90.f;
	// The `<kbd>` caps in the dialog footers: 11 px serif, 2 px tracking, on an
	// 8x2 plate.
	int32 KeyCapFontSize       = 11;
	int32 KeyCapLetterSpacing  = 182;
	float KeyCapPadX           = 8.f;
	float KeyCapPadY           = 2.f;
	// maxlength="40", and the counter turns warm past 32.
	int32 SaveNameMaxLength    = 40;
	int32 SaveNameWarnLength   = 32;
	int32 SaveWarnSize         = 13;
	float SaveWarnBarWidth     = 3.f; // border-left:3px solid --warm-secondary

	// .ld-panel (Voxelmark Load Dialog.html)
	float LoadDialogWidth      = 780.f;
	float LoadDialogHeight     = 640.f;
	float LoadDialogPadX       = 28.f;
	float LoadDialogPadTop     = 24.f;
	float LoadDialogPadBottom  = 20.f;
	float LoadDialogGap        = 12.f;
	int32 LoadDialogTitleSize  = 26;
	int32 LoadDialogTitleLetterSpacing = 154; // 4 px at 26 px
	int32 LoadFilterSize       = 11;
	int32 LoadFilterLetterSpacing = 182; // 2 px at 11 px
	float LoadFilterPadX       = 12.f;
	float LoadFilterPadY       = 6.f;
	int32 LoadSearchSize       = 14;
	int32 LoadCountSize        = 12;
	float LoadRowGap           = 6.f;
	float LoadRowPadX          = 14.f;
	float LoadRowPadY          = 10.f;
	float LoadThumbSize        = 64.f;
	int32 LoadThumbGlyphSize   = 28;
	int32 LoadRowNameSize      = 18;
	// .ld-name is `white-space:nowrap; overflow:hidden; text-overflow:ellipsis`
	// inside a `min-width:0` flex row. Slate has the ellipsis (an overflow
	// policy) but no min-width-0 shrink rule, so the name is clamped instead:
	// short names hug their text and the tags sit beside them, a 40-character
	// one (the save dialog's cap) stops here and ellipsises rather than pushing
	// LOAD and DELETE off the row.
	//
	// 300 of the ~470 px the info column gets at LoadDialogWidth, leaving room
	// for both tags.
	float LoadRowNameMaxWidth  = 300.f;
	int32 LoadTagSize          = 9;
	int32 LoadTagLetterSpacing = 222; // 2 px at 9 px
	int32 LoadRowMetaSize      = 13;
	int32 LoadRowButtonSize    = 12;
	float LoadRowButtonPadX    = 14.f;
	float LoadRowButtonPadY    = 8.f;
	int32 LoadEmptyGlyphSize   = 54;
	int32 LoadEmptySize        = 16;
	int32 LoadKeyHintSize      = 12;

	// --- Loading screen -----------------------------------------------------
	// TransitionManager._build_loading_screen: centred VBox 600x360, sep 18.
	float LoadingColumnWidth   = 600.f;
	float LoadingColumnHeight  = 360.f;
	float LoadingSeparation    = 18.f;
	// 2026-09-07 mock (Voxelmark Loading Screen.html): the hourglass is "~1/4
	// prior size" -- a 46x69 glass in a 72x79 stage, and the stack sits at
	// 33% of the height rather than centred. Was 96x144 in 96x156.
	float HourglassWrapWidth   = 72.f;
	float HourglassWrapHeight  = 79.f;
	float HourglassWidth       = 46.f;
	float HourglassHeight      = 69.f;
	float HourglassOffsetY     = 6.f;
	// The mock centres the stack at 33% of the height (top:33% +
	// translate(-50%)); with a ~200 px stack at 1080p that puts its TOP near
	// 24%, which is what this fraction is: the share of the height above the
	// stack's top edge.
	float LoadingStackTopFrac  = 0.24f;
	// 40 with 10 px tracking (= 250/1000 em); was 44 with literal spaces in
	// the string. The spaced string stays for the fallback face.
	int32 LoadingTitleSize     = 40;
	int32 LoadingTitleLetterSpacing = 250;
	int32 LoadingQuipSize      = 20;
	float LoadingQuipMinHeight = 56.f;
	float LoadingBarWidth      = 520.f;
	float LoadingBarHeight     = 8.f;
	int32 LoadingPctSize       = 18;
	int32 LoadingTipSize       = 14;
	float LoadingTipInsetX     = 60.f;
	float LoadingTipInsetBottom = 16.f;
	float LoadingTintAlpha     = 0.62f;
	int32 FpsFontSize          = 14;

	// --- Timings (seconds) --------------------------------------------------
	float FadeDuration         = 0.4f;
	float BackgroundRotate     = 20.0f;
	float BackgroundFade       = 1.0f;
	float QuipRotate           = 2.5f;
	float QuipFade             = 0.4f;
	float TipRotate            = 8.0f;
	float MusicFadeOut         = 1.5f;
	// TransitionManager._process clamps its animation delta so that at 10 FPS
	// -- which a cold chunk cascade genuinely produces -- the hourglass bob and
	// the quip crossfade step forward smoothly instead of jumping.
	float MaxAnimationDelta    = 0.05f;
	// The hourglass's vertical bob: +/-2 px on a 5.2 s cycle.
	float HourglassBobPeriod   = 5.2f;
	float HourglassBobPixels   = 2.0f;
	// The FPS readout re-rasterises at most this often. Rebuilding an
	// STextBlock's layout every frame while the machine is already struggling
	// is exactly the wrong time to pay for it.
	float FpsRefreshInterval   = 0.25f;

	// Loaded once from Config/DefaultVoxelUI.ini, section [VoxelUI.Layout],
	// key names identical to the member names above. Absent keys keep the
	// defaults, so an empty or missing ini is the normal case.
	static const FVoxelMenuLayout& Get();
};
