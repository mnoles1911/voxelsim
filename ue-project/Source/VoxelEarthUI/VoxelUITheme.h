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
	int32 SubtitleFontSize     = 18;
	int32 VersionFontSize      = 12;
	float VersionInsetLeft     = 16.f;
	float VersionInsetBottom   = 16.f;
	// The dark wash over the background art. MainMenu.gd: BG_NIGHT @ 0.55.
	float BackgroundTintAlpha  = 0.55f;

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

	// --- Loading screen -----------------------------------------------------
	// TransitionManager._build_loading_screen: centred VBox 600x360, sep 18.
	float LoadingColumnWidth   = 600.f;
	float LoadingColumnHeight  = 360.f;
	float LoadingSeparation    = 18.f;
	float HourglassWrapWidth   = 96.f;
	float HourglassWrapHeight  = 156.f;
	float HourglassWidth       = 96.f;
	float HourglassHeight      = 144.f;
	float HourglassOffsetY     = 6.f;
	int32 LoadingTitleSize     = 44;
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
