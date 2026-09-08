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

// --- In-game screen family (2026-09-07 wave 2) ------------------------------
// Inventory / Map / Journal / Player / Codex share the .menu-shell tab bar and
// draw almost entirely from the tokens above. These are the ones their own
// <style> blocks introduce as literals, named here for the same reason the
// overlay family's are: a literal with no name is how two screens start
// disagreeing about the same colour.

// The map's parchment. .parchment-frame is a three-stop gradient and .sheet-paper
// a different three-stop gradient over it; Mix() takes two stops, so the port
// names the frame's end stops and uses the sheet's middle stop as the flat body.
inline const FColor MapFrameTop    = FColor(0xe8, 0xd3, 0xa0);
inline const FColor MapFrameBottom = FColor(0xc8, 0xa8, 0x68);
inline const FColor MapSheetPaper  = FColor(0xdd, 0xc4, 0x8c); // .sheet-paper 45% stop
// Marks are drawn in iron-gall ink on the sheet, not in the UI palette's golds.
inline const FColor MapMarkInk     = FColor(0x7a, 0x2a, 0x12);
inline const FColor MapMarkCaption = FColor(0x5a, 0x2a, 0x12);
// The live you-are-here glyph (2026-09-08, owner directive). `.you::before` is
// `background:var(--hp-bright)` inside `border:2px solid #1a0a05` under a
// `0 0 0 3px rgba(240,193,75,0.5)` gold halo. HpBright and Gold are already
// named above; the near-black rim is the one literal the mock introduces here.
inline const FColor MapYouEdge     = FColor(0x1a, 0x0a, 0x05);

// The journal page's ink accents, all on parchment rather than on leather.
inline const FColor PageKind       = FColor(0xa0, 0x4a, 0x14); // .p-kind, .p-steps li.now
inline const FColor PageDropCap    = FColor(0x5a, 0x2a, 0x14); // .p-body .first, .p-sect

// .perks-detail is the one blood-red panel in the whole front end: a dark
// crimson plate with a lighter crimson ring, so an owned perk's description
// reads as a different KIND of object from the oak stat blocks beside it.
inline const FColor PerkPanelTop    = FColor(0x5a, 0x14, 0x10);
inline const FColor PerkPanelBottom = FColor(0x2a, 0x08, 0x08);
inline const FColor PerkPanelEdge   = FColor(0x8a, 0x30, 0x30);

// .dlg-option__badge.easy. MEDIUM is Gold and HARD is HpBright, both already
// named above; only the green has no existing token.
inline const FColor DlgCheckEasy    = FColor(0x9e, 0xe0, 0x7a);

// --- Death screen -----------------------------------------------------------
//
// THE MOCK REFERENCES TWO CSS VARIABLES THAT DO NOT EXIST. "Voxelmark Death
// Screen.html" paints .died and .rule with var(--blood) and var(--blood-bright),
// and neither is declared in menus_shared.css's :root block nor in the mock's
// own <style> -- in a browser both resolve to nothing and the heading renders
// in the inherited parchment. So the port cannot copy a value and has to choose
// one, which is recorded here rather than buried at the call site.
//
// DeathBlood is measured from the only blood-red the mock does state: the
// heading's own `text-shadow: 0 0 40px rgba(142,31,20,0.75)`. The bright stop
// is HpBright, the palette's existing brightest red, so the death screen and
// the DELETE button cannot drift apart.
inline const FColor DeathBlood     = FColor(0x8e, 0x1f, 0x14);

// --- HUD (2026-09-07 "Voxelmark HUD v2") ------------------------------------
//
// THE HUD MOCK OVERRIDES THE SHARED PALETTE. It carries its own :root block and
// sets --stam to #6fb8d8, a cold blue, where menus_shared.css sets it to the
// gold #c8a04a -- and then draws the HUNGER bar with .bar.stam. Taking the
// shared token would paint that bar gold and lose the only colour contrast the
// bottom dock has, so the port follows the screen it is cloning and names the
// override instead of silently resolving the conflict.
inline const FColor HudHungerFill  = FColor(0x6f, 0xb8, 0xd8);
inline const FColor HudHungerDeep  = FColor(0x2a, 0x5a, 0x78);
// .bar.hp .wound -- the un-healable band at the left of the health bar. Same
// value as HpDeep; named because the mock names it (--hp-wound) and because a
// later divergence should not be a silent one.
inline const FColor HudWound       = FColor(0x5a, 0x14, 0x10);

// THE TOP STOP OF EACH BAR, which is the half of every `.bar .fill` gradient
// the port had no name for and therefore could not draw. Each fill in the HUD
// mock is a three-stop 180deg ramp; the port was painting the MIDPOINT of the
// two ends it did know, which is why both bars read as one flat colour. The
// bright stops below are the CSS's literal 0% values -- they are not derived
// from the darker tokens, so do not "simplify" them into a Lerp.
//
//   .bar.hp   .fill  #d44a3a 0% -> --hp     50%  -> --hp-deep   100%
//   .bar.stam .fill  #bfe6f5 0% -> --stam   55%  -> --stam-deep 100%
//   .bar.hp   .wound #6a1a14 0%               -> --hp-wound  100%
inline const FColor HpTop          = FColor(0xd4, 0x4a, 0x3a);
inline const FColor HudHungerTop   = FColor(0xbf, 0xe6, 0xf5);
inline const FColor HudWoundTop    = FColor(0x6a, 0x1a, 0x14);

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

// --- The minimum width of any line this front end draws ---------------------
//
// ADR-0011 (scale-tolerant UI), decision 3: **never a 1 px border**. The
// interface is authored at 1080p and the engine scales it continuously off the
// shortest side, so on the owner's 1440p screen every authored unit is
// multiplied by 1.333. A 1-unit rule therefore lands on 1.333 device pixels --
// it cannot sit on a pixel boundary and is resampled into a smear at every
// scale that is not a whole number. Two units survive any factor: the worst it
// can do is spread 2.67 device pixels over three, and the line is still a line.
//
// EVERY hairline rule, divider, tick and border BAND in this module is this
// number. It is a constant rather than an FVoxelMenuLayout field on purpose:
// it is a rendering constraint, not a design choice, and nothing should be able
// to tune it back down to 1 from an ini.
//
// `menus_shared.css` already uses 2 px in places, so where the mock states a
// heavier weight nearby that weight is what the port follows. Where the mock
// says 1 px and this says 2, the port is deliberately heavier than the mock and
// that is a visible change the owner judges on a capture.
inline constexpr float RulePx = 2.f;

// --- Line height ------------------------------------------------------------
//
// A CSS `line-height:1.45` AND AN STextBlock `LineHeightPercentage(1.45f)` ARE
// NOT THE SAME NUMBER, and passing the mock's straight through is how the title
// screen's callout ended up a third looser than the mock it was copied from.
//
//   CSS   line-height: N   =>  N x the FONT SIZE.
//   Slate LineHeightPercentage(N)
//         =>  N x the FACE'S OWN line height. FTextLayout.cpp:451 is literally
//             `LineSize.Y = UnscaleLineHeight * LineHeightPercentage`, and
//             UnscaleLineHeight comes from the font's ascent/descent/lineGap.
//
// IM Fell English -- the --hand face every wrapped body block in this front end
// uses -- has a natural line height of 1.269 em (hhea ascender 1638, descender
// -961, lineGap 0, unitsPerEm 2048; read out of both IMFeENrm28P.ttf and
// IMFeENit28P.ttf on 2026-09-07). So the mock's 1.45 was arriving as 1.84 em.
//
// Call sites keep writing the CSS number and convert here, so the mock and the
// port can still be compared line for line. The other two faces are recorded
// beside it for the day a wrapped block uses one.
inline constexpr float HandFaceLineHeightEm  = 1.269f; // IM Fell English, roman and italic
inline constexpr float SerifFaceLineHeightEm = 1.180f; // Macondo Swash Caps
inline constexpr float MonoFaceLineHeightEm  = 1.000f; // VT323 (USE_TYPO_METRICS set)

// NOTE: these are the metrics of the SHIPPED faces. Under -VoxelUINoAssets, or
// on a checkout missing Content/UI/Fonts, the accessors fall back to the engine
// face and the conversion is off by that face's metrics instead. That is the
// documented degraded path, not a bug to guard here.
inline constexpr float HandLineHeight(float CssLineHeight)
{
	return CssLineHeight / HandFaceLineHeightEm;
}
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
	// DEAD SINCE THE 2026-09-07 TITLE SCREEN, TitleFontSize AND TitleBoxWidth
	// BOTH. Nothing in the module reads either any more -- the title screen's
	// wordmark is `.title-logo__name` and comes from LogoFontSize below; a grep
	// of every .cpp on 2026-09-07 found the only remaining references are their
	// own ini registrations in VoxelUITheme.cpp.
	//
	// This matters because ADR-0011 lists "TitleFontSize = 84 against the
	// mock's 108" as a constant to re-derive. It cannot be re-derived and does
	// not need to be: it draws nothing. Left at its values rather than deleted
	// because the centred oak column they belong to still exists behind the
	// sub-panels, and re-deriving 84 to 108 without also re-measuring the 720
	// (which was measured from a rendered capture, not from the face) would
	// clip the string the day something draws it again.
	//
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
	float VersionInsetLeft     = 20.f;
	float VersionInsetBottom   = 14.f;
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
	// CORNER INSETS TIGHTENED BY OWNER DIRECTION, 2026-09-07 night, live on his 1440p
	// screen: "there is currently too much buffer and padding between the edges of the
	// 1440p 2K monitor screen and the UI elements" -- the patch-notes callout, the
	// Voxelmark title, the menu items and the version stamp all move toward their
	// corners. These are AUTHORED 1080p figures per ADR-0011 (the engine scale is
	// untouched); the mock's 80/100/160/80/60/36/24 were roughly halved. Design
	// choice, judged live; iterate here, never in the scale curve.
	float LogoTop              = 40.f;
	// 100, WHICH IS WHAT `.stage .title-logo{right:100px}` SAYS.
	//
	// This was 120: +20 over the mock, added to "restore the ink margin" after
	// a 2026-09-07 capture measured 34 device px of margin against an expected
	// ~60. ADR-0011 retires that compensation. The 34 was device pixels at a
	// 1.333 scale (25 units), the ~60 it was compared against was an authored
	// figure, and the two were never in the same space -- so the deficit it
	// corrected for may not exist. The overhang itself is real (Macondo's swash
	// K runs about 0.42 em past its advance, and Slate right-aligns on the
	// advance) but a browser right-aligns on the box too, so the mock has the
	// same overhang and the port should not be compensating for it at all.
	//
	// If a 1.0-era capture shows the K crowding the frame, the fix is a right
	// padding on the logo's own text block, not a bigger inset on everything.
	// SECOND LIVE PASS, 2026-09-08 (owner): "the voxelmark title needs to move further to
	// the left because the end of the K letter at the end is currently clipping off
	// screen" -- Macondo's swash K overhangs its advance by ~0.42 em, 47 px at 112 px, so
	// a 48 inset put the ink past the edge; 96 keeps the INK about 49 from the edge, in
	// line with the accepted spacing. And "patch notes can be moved up and to the left
	// more": CalloutTop 40 -> 24, CalloutLeft 32 -> 16.
	float LogoRight            = 96.f;
	// SIZES REDUCED BY OWNER DIRECTION, 2026-09-07 night, live at 1440p, after the corner
	// insets were accepted ("spacing looks good"): "the Voxelmark title text and the patch
	// notes ... are both too large ... overall smaller by 25% for the patch note highlights
	// in the top left and 15% for the game title in top right." Title 132 -> 112. Callout
	// 520/14/16/64/22 and faces 16/22/16 -> 390/10/12/48/16 and 12/16/12, i.e. every
	// dimension x0.75 (rounded down where it lands on a half, since he asked for tighter
	// and compact, not merely scaled). Authored 1080p figures per ADR-0011.
	int32 LogoFontSize         = 112;
	// letter-spacing in the mock is px; FSlateFontInfo::LetterSpacing is
	// 1/1000 em. 6 px at 132 px = 45. (The earlier note that Slate "has no
	// tracking" predates UE5's LetterSpacing and is retired by this field.)
	int32 LogoLetterSpacing    = 45;
	float TitleMenuTop         = 400.f;
	float TitleMenuRight       = 64.f;
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
	float CalloutTop           = 24.f;
	float CalloutLeft          = 16.f;
	float CalloutMaxWidth      = 390.f;
	float CalloutPadX          = 10.f;
	float CalloutGap           = 12.f;
	float CalloutGlyphSize     = 48.f;
	// The ornament inside the glyph box: a gold square turned 45 degrees,
	// standing in for the mock's U+2726 star (no shipped face carries it).
	float CalloutOrnamentSize  = 16.f;
	int32 CalloutTagSize       = 12;
	int32 CalloutTitleSize     = 16;
	int32 CalloutCopySize      = 12;

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
	// 2 / 2 / 2, NOT THE CSS'S 2 / 1 / 3. VoxelOverlayChrome::Panel stacks these
	// outside in, so the widths of the three visible bands are Border, then
	// EdgeRing - Border, then InnerRing - EdgeRing. The mock's 2/1/3 gives bands
	// of 2, 1 and 2 -- and the 1 is a hairline ADR-0011 forbids. Widening the
	// edge ring to 2 and the inner ring to 4 gives 2, 2, 2: the panel's chrome
	// grows by one unit in total and no band is thinner than VoxelUITheme::RulePx.
	float OverlayBorderPx      = 2.f;  // border:2px solid #000
	float OverlayEdgeRingPx    = 2.f;  // inset 0 0 0 1px --leather-edge, widened
	float OverlayInnerRingPx   = 4.f;  // inset 0 0 0 3px --leather-1, widened
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
	// THERE IS NO FIXED PANEL HEIGHT ANY MORE, AND THAT IS THE FIX FOR THE FOLD.
	//
	// This was `SettingsPanelHeight = 660`, a constant tuned against one
	// capture. On the 2026-09-07 settings capture at 2560x1440 it cut OCEAN
	// MESH DETAIL -- one of only two player-facing water rows -- off below a
	// scrollbar, because four graphics rows with two-line hints do not fit in
	// 660 units and nothing in the code knew that. A dialog whose height is a
	// constant hides a row the moment a row is added, silently, and the row it
	// hides is always the newest one.
	//
	// The panel is now as tall as its content (VoxelOverlayChrome::Panel's
	// documented `Height <= 0` contract) and clamped only by what the viewport
	// can hold -- which is the mock's own rule, `max-height:calc(100vh - 24px)`.
	// The body still sits in an SScrollBox, so a viewport too short for the
	// content scrolls instead of clipping; at 1080p and 1440p it never engages.
	float SettingsPanelViewportMargin = 24.f; // .se-panel calc(100vh - 24px)
	// The floor the viewport clamp will not go below, so a freak-small window
	// yields a scrolling panel rather than a panel with no body at all.
	float SettingsPanelMinHeight = 240.f;
	// AND WHEN IT DOES SCROLL, IT SAYS SO. Slate's default bar is 8 units of
	// near-panel-coloured hairline; on the 2026-09-07 capture it was drawn, and
	// the row below it was still read as absent rather than as scrolled off.
	// A control the player cannot see is the same as no control.
	float SettingsScrollBarThickness = 12.f;
	float SettingsScrollBarPadding   = 6.f;
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
	// 10, NOT 14. The well's two 1-unit bands became two 2-unit bands under
	// ADR-0011, which takes the well's interior from 18 units to 14 -- exactly
	// the old mark, so a checked box would have read as a solid block with no
	// well left around it. 10 restores the two-unit gap the mock's
	// `.ck-box::after` sits in.
	float CheckboxMarkSize     = 10.f;
	int32 SettingsCyclerSize   = 15;
	int32 SettingsBadgeSize    = 11;
	int32 SettingsHelpSize     = 12;
	int32 SettingsActionSize   = 15;
	int32 SettingsActionLetterSpacing = 200; // 3 px at 15 px
	float SettingsActionPadX   = 24.f;
	float SettingsActionPadY   = 9.f;
	int32 SettingsHintSize     = 13;
	// `.grp-head` letter-spacing: 3 px at the sub-tab label size.
	int32 CodexGroupLetterSpacing = 231;
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

	// --- In-game screen shell (2026-09-07 wave 2) ---------------------------
	// .menu-shell.compact and the tab bar above it, shared by all five in-game
	// screens. The mock sizes the shell
	// `min(1060px, calc((100vw - 24px) / 1.33333))`, which is the author saying
	// "1060 unless the window is too small"; Slate's DPI scale already divides
	// by the same 1.333, so the port uses the 1060x760 figure and lets the
	// containing SBox clamp it.
	float ScreenShellWidth     = 1060.f;
	float ScreenShellHeight    = 760.f;
	float ScreenShellPadX      = 18.f; // padding:14px 18px 18px
	float ScreenShellPadTop    = 14.f;
	float ScreenShellPadBottom = 18.f;
	// THE INVENTORY SHELL HAS NO SIZE HERE, deliberately. Its mock overrides the
	// compact shell with `width:max-content`, and the port honours that by
	// setting no override at all -- see SVoxelScreenShell::Construct for the
	// arithmetic that rules out giving it a second fixed size. A knob nothing
	// reads is worse than no knob, so there is none.

	// .menu-tabs / .menu-tab
	float TabBarPadLeft        = 8.f;
	int32 TabFontSize          = 15;
	int32 TabLetterSpacing     = 133;  // 2 px at 15 px
	float TabPadX              = 22.f; // padding:10px 22px 9px
	float TabPadTop            = 10.f;
	float TabPadBottom         = 9.f;
	float TabActivePadBottom   = 11.f; // .menu-tab.active grows 2 px downward
	float TabUnderlinePx       = 3.f;  // inset 0 -3px 0 var(--gold)
	int32 TabKeySize           = 15;   // .menu-tab .key
	float TabKeyGap            = 8.f;
	float TabBarRulePx         = 2.f;  // border-bottom:2px solid #000

	// .menu-body -- an oak panel with a black/oak/black/oak ring stack and a
	// bronze stud inset 8 px at each corner.
	float ScreenBodyPad        = 18.f;
	float ScreenBodyStudInset  = 8.f;
	float ScreenBodyStudSize   = 4.f;  // radial-gradient ... 2px radius

	// .action-bar
	float ActionBarTopGap      = 10.f; // padding:10px 4px 0
	float ActionBarGap         = 18.f;
	int32 ActionBarFontSize    = 14;
	int32 ActionKeyFontSize    = 15;
	float ActionKeyMinWidth    = 18.f;
	float ActionKeyHeight      = 18.f;
	float ActionKeyPadX        = 5.f;
	float ActionKeyGap         = 6.f;

	// .sub-tabs / .sub-tab (PLAYER's four sub-pages, CODEX's categories)
	float SubTabGap            = 6.f;
	float SubTabRowBottomGap   = 10.f;
	float SubTabRowPadBottom   = 6.f;
	float SubTabShieldWidth    = 36.f;
	float SubTabShieldHeight   = 42.f;
	int32 SubTabLabelSize      = 11;
	int32 SubTabLetterSpacing  = 182; // 2 px at 11 px
	float SubTabMinWidth       = 64.f;

	// .list-parchment / .list-header / .list-row / .list-divider
	float ListHeaderPadX       = 16.f;
	float ListHeaderPadY       = 8.f;
	int32 ListHeaderSize       = 15;
	float ListRowPadX          = 16.f;
	float ListRowPadY          = 7.f;
	int32 ListRowSize          = 17;
	float ListRowIconSize      = 20.f;
	int32 ListDividerSize      = 14;
	int32 ListDividerSpacing   = 214; // 3 px at 14 px

	// --- Inventory (.pack / .hotbar / .filter-tab / .side) -------------------
	float InvSlotSize          = 58.f;
	float InvSlotGap           = 2.f;
	int32 InvPackCols          = 8;
	int32 InvPackRows          = 8;
	int32 InvHotbarSlots       = 8;
	float InvPackBoxPad        = 6.f;
	float InvHotbarRuleGap     = 5.f;
	int32 InvSlotQtySize       = 14;   // .slot .qty
	float InvDurabilityHeight  = 3.f;  // .dura
	int32 InvFilterSize        = 11;
	int32 InvFilterSpacing     = 182;  // 2 px at 11 px
	float InvFilterPadX        = 10.f;
	float InvFilterPadY        = 5.f;
	int32 InvPanelNameSize     = 15;
	int32 InvPanelNameSpacing  = 267;  // 4 px at 15 px
	float InvSidePanelWidth    = 430.f;
	float InvSideBoxPad        = 16.f;
	float InvColumnGap         = 22.f; // .fit-inner gap
	float InvCraftSlotSize     = 66.f;
	float InvCraftSlotGap      = 3.f;
	float InvEquipSlotSize     = 58.f;
	float InvEquipColumnGap    = 14.f;
	float InvRenderFrameHeight = 300.f;
	int32 InvStatsSize         = 17;
	int32 InvWeightSize        = 17;
	// `#invSearch{max-width:280px}`. A REAL WIDTH, not a max on a stretched
	// slot: see the note at the call site for why the max never bound.
	float InvSearchWidth       = 280.f;
	// `.weight i` -- the 11 px swatch in front of the readout.
	float InvWeightSwatchSize  = 11.f;

	// --- Map ----------------------------------------------------------------
	float MapCompassSize       = 72.f;
	float MapCompassInset      = 26.f;
	// SUPERSEDED 2026-09-08: this used to read "the port draws the hillshade
	// letterboxed at a fixed scale, see SVoxelMapScreen for why panning is not
	// what this screen can honestly offer yet." It pans and zooms now; the
	// tokens for that are in the block below.
	float MapMarkIconSize      = 26.f;
	int32 MapMarkCaptionSize   = 19;
	float MapDrawerWidth       = 250.f;
	int32 MapDrawerRowSize     = 18;
	int32 MapReadoutSize       = 18;
	int32 MapReadoutLabelSize  = 13;

	// --- Live map: player marker, marks, pan/zoom, context menu -------------
	// 2026-09-08, owner directive: the sheet pans, zooms, carries a live player
	// marker and takes named marks. The paragraph above is now historical --
	// what the port draws IS a pannable map, and the mock's interaction is
	// ported rather than declined.
	//
	// EVERY FIGURE HERE IS AN AUTHORED 1080p PIXEL, ADR-0011. The world-space
	// constants the transform needs (the raster's extent, the default 10 km
	// view, the zoom bounds) are NOT design tokens and are not here -- they are
	// facts about the terrain and live in SVoxelMapScreen.cpp beside the
	// arithmetic that uses them.

	// `.you` is 16x16 with a 3 px halo ring outside it. The port draws an
	// ARROW rather than a dot, because the mock's dot cannot show heading and
	// the readout beside it already states the bearing in degrees.
	float MapYouSize           = 18.f;
	float MapYouRing           = 26.f;  // the outer gold halo's diameter
	// The mark glyph is MapMarkIconSize above. This is how close the cursor has
	// to come to one to hit it -- generous, because a 26 px diamond on a
	// zoomed-out sheet is a small target and the cost of a miss is a context
	// menu offering the wrong thing.
	float MapMarkHitRadius     = 18.f;
	// The label sits under the glyph (`.mark{transform:translate(-50%,-100%)}`
	// puts the glyph's point ON the position and the caption below it).
	float MapMarkLabelGap      = 2.f;

	// A PRESS-MOVE-RELEASE HAS TO BE DISTINGUISHABLE FROM A CLICK, and 4 px is
	// the figure Slate itself uses for its drag-detection default. Below it a
	// right-press is a context menu; at or above it, it is a pan and no menu
	// opens on release.
	float MapDragThresholdPx   = 4.f;
	// One wheel notch. A ratio, not a length -- 1.25 gives ten notches from the
	// 10 km default out to the whole world and about nine in to the 500 m
	// floor, which is a hand's worth of scrolling in either direction.
	float MapZoomStep          = 1.25f;

	// `.mk-dialog` -- the right-click panel. 236 px wide, 12 px padding, 8 px
	// between rows.
	float MapCtxWidth          = 236.f;
	float MapCtxPad            = 12.f;
	float MapCtxGap            = 8.f;
	float MapCtxRowHeight      = 26.f;
	int32 MapCtxTitleSize      = 13;   // .mk-title
	int32 MapCtxTitleSpacing   = 231;  // letter-spacing:3px at 13 px
	int32 MapCtxItemSize       = 16;
	int32 MapCtxLabelSize      = 15;   // .mk-lab
	int32 MapCtxInputSize      = 19;   // .mk-input
	float MapCtxInputPadX      = 8.f;  // padding:5px 8px
	float MapCtxInputPadY      = 5.f;
	float MapCtxItemPadX       = 8.f;
	// How far the menu is nudged off the click point so the cursor is not
	// sitting on its first row the instant it opens.
	float MapCtxOffset         = 4.f;
	// The longest name the field will take. Shorter than the marks store's own
	// 64-character cap on purpose: the store's cap is a corruption guard, this
	// is what fits under a glyph on the sheet.
	int32 MapMarkNameMaxLength = 28;

	// --- Journal ------------------------------------------------------------
	float JournalListWidth     = 300.f; // grid-template-columns:300px 1fr
	float JournalColumnGap     = 18.f;
	float JournalCardPadX      = 12.f;
	float JournalCardPadY      = 9.f;
	float JournalCardGap       = 4.f;
	int32 JournalStampSize     = 12;   // .card .stamp (--pixel 8px; see note)
	int32 JournalCardHeadSize  = 15;
	int32 JournalCardSnipSize  = 15;
	float JournalPagePadX      = 28.f;
	float JournalPagePadY      = 24.f;
	int32 JournalPageKindSize  = 13;
	int32 JournalPageTitleSize = 30;
	int32 JournalPageStampSize = 16;
	int32 JournalPageBodySize  = 18;
	int32 JournalDropCapSize   = 48;
	int32 JournalSectionSize   = 13;
	int32 JournalStepSize      = 17;
	float JournalStepMarkSize  = 10.f;

	// --- Player -------------------------------------------------------------
	float PlayerColumnGap      = 14.f;
	float PlayerCardPadX       = 12.f;
	float PlayerCardPadY       = 10.f;
	float PlayerEquipSlotSize  = 62.f;
	float PlayerEquipGap       = 6.f;
	int32 PlayerNameSize       = 20;
	int32 PlayerNameSpacing    = 150; // 3 px at 20 px
	int32 PlayerHeadSize       = 12;
	int32 PlayerBlockTitleSize = 13;
	int32 PlayerBlockTitleSpacing = 231; // 3 px at 13 px
	int32 PlayerAttrNameSize   = 13;
	int32 PlayerAttrValueSize  = 12;
	float PlayerAttrGlyphSize  = 19.f;
	float PlayerLevelTrackHeight = 6.f;
	int32 PlayerSkillNodeSize  = 64;
	int32 PlayerRepNameSize    = 18;
	float PlayerRepBarHeight   = 14.f;
	float PlayerRepNameColumn  = 240.f;
	float PlayerRepValueColumn = 80.f;

	// --- Codex --------------------------------------------------------------
	float CodexCategoryWidth   = 200.f;
	float CodexEntryListWidth  = 260.f;
	float CodexColumnGap       = 14.f;
	int32 CodexCategorySize    = 14;
	int32 CodexEntrySize       = 17;
	int32 CodexTitleSize       = 26;
	int32 CodexBodySize        = 17;
	float CodexRecipeSlotSize  = 48.f;
	float CodexRecipeSlotGap   = 3.f;

	// --- Death screen (.died / .rule / .quip / .btn) -------------------------
	int32 DeathTitleSize       = 104;
	int32 DeathTitleSpacing    = 135;  // 14 px at 104 px
	float DeathRuleWidth       = 520.f;
	float DeathRuleHeight      = 2.f;
	float DeathRuleTopGap      = 22.f;
	float DeathRuleBottomGap   = 18.f;
	int32 DeathQuipSize        = 26;
	float DeathQuipMaxWidth    = 720.f;
	int32 DeathStampSize       = 12;
	int32 DeathStampSpacing    = 417; // 5 px at 12 px
	float DeathStampTopGap     = 26.f;
	// .actions sits `bottom:120px` in a 1080-tall stage; expressed as a gap
	// below the message stack, which is what a Slate vertical box can honour at
	// any window size.
	float DeathActionsTopGap   = 120.f;
	float DeathButtonGap       = 18.f;
	float DeathButtonPadX      = 40.f;
	float DeathButtonPadY      = 13.f;
	int32 DeathButtonSize      = 17;
	int32 DeathButtonSpacing   = 235; // 4 px at 17 px
	// The mock fades the world to greyscale at 0.28 brightness under a radial
	// ink wash. Slate can neither desaturate the scene nor draw a radial
	// gradient, so the port lays one flat black wash at the brightness the two
	// together were reaching for -- the same decision, and the same reasoning,
	// as OverlayDimAlpha above.
	float DeathDimAlpha        = 0.82f;

	// --- Dialogue (.dlg-* in menus_shared.css) ------------------------------
	float DlgStagePadX         = 64.f;
	float DlgStagePadY         = 48.f;
	float DlgPortraitWidth     = 160.f;
	float DlgPortraitHeight    = 200.f;
	float DlgSpeakerGap        = 18.f;
	float DlgSpeakerMaxWidth   = 780.f;
	int32 DlgSpeakerNameSize   = 22;
	int32 DlgSpeakerNameSpacing = 182; // 4 px at 22 px
	int32 DlgSpeakerRoleSize   = 16;
	int32 DlgSpeakerLineSize   = 26;
	float DlgSpeakerLineMaxWidth = 620.f;
	float DlgOptionGap         = 6.f;
	float DlgOptionPadX        = 14.f;
	float DlgOptionPadY        = 8.f;
	int32 DlgOptionSize        = 22;
	float DlgOptionKeySize     = 22.f;
	int32 DlgOptionKeyFontSize = 16;
	float DlgOptionDiamondSize = 6.f;
	int32 DlgBadgeSize         = 15;
	float DlgSkillPadX         = 14.f;
	float DlgSkillPadY         = 8.f;
	float DlgSkillIconSize     = 26.f;
	int32 DlgSkillValueSize    = 20;
	int32 DlgSkillNameSize     = 9;
	int32 DlgSkillNameSpacing  = 222; // 2 px at 9 px
	float DlgCompanionWidth    = 360.f;
	// The overlay dims the world less than the pause menu does, because the
	// player is meant to still read the scene behind the conversation -- but
	// NOT as little as the mock's own .scene-tint alone.
	//
	// 0.35 -> 0.55, MEASURED. The mock stacks rgba(0,0,0,0.25) on a .dlg-stage
	// radial wash that reaches 0.6 at the edges, and the port draws one flat
	// layer for both. Setting that layer to the tint alone was reading the
	// stack's first term only: the 2026-09-07 dialogue capture put white
	// parchment reply text over sunlit snow at near-full brightness, and the
	// right-hand column was barely legible. 0.55 is the wash's own mid-range,
	// which is what the two layers together were reaching for.
	float DlgDimAlpha          = 0.55f;

	// --- HUD ("Voxelmark HUD v2") -------------------------------------------
	float HudCompassWidth      = 560.f;
	float HudCompassHeight     = 24.f;
	float HudCompassTop        = 12.f;
	float HudCompassSegWidth   = 60.f;  // one 30-degree segment
	int32 HudCompassSegSize    = 12;
	int32 HudCompassCardSize   = 14;    // N/E/S/W read larger than the numbers
	float HudDockBottom        = 12.f;
	float HudDockGap           = 5.f;
	float HudBarsWidth         = 694.f; // --hotbar-w: 10 slots + 9 gaps
	float HudBarHeight         = 11.f;
	float HudBarGap            = 10.f;
	int32 HudBarSegments       = 10;    // the ::after 10% tick overlay
	float HudSlotSize          = 64.f;
	float HudSlotGap           = 6.f;
	int32 HudSlotCount         = 10;
	int32 HudSlotNumSize       = 13;
	int32 HudSlotQtySize       = 14;
	float HudSlotGlyphInset    = 15.f;
	int32 HudInteractSize      = 14;
	float HudInteractKeySize   = 21.f;

	// --- HUD music cluster (owner directive, 2026-09-07) --------------------
	// AUTHORED 1080p, like everything else in this struct (ADR-0011). The HUD
	// mock has no top-right element to copy, so the numbers come from the two
	// corner conventions it does establish: the compass sits 12 px from the top
	// and the dock 12 px from the bottom, so the cluster takes the same 12 px
	// inset; and the button plate is the hotbar slot's chrome at half its size.
	float HudMusicTop          = 12.f;   // == HudCompassTop
	float HudMusicRight        = 12.f;
	// 30, inside the brief's 28-32 band. Half the 64 px hotbar slot minus a
	// rounding, so the two plates read as the same object at two sizes.
	float HudMusicButtonSize   = 30.f;
	float HudMusicButtonGap    = 6.f;    // == HudSlotGap
	// The glyph's inset inside the plate, on HudSlotGlyphInset's pattern
	// (15 of 64 there, 7 of 30 here -- the same fraction to within a unit).
	float HudMusicGlyphInset   = 7.f;
	// HORIZONTAL since 2026-09-08: the owner moved the now-playing name to the
	// LEFT of the three plates, so this is the gap between the end of the name
	// and the first plate rather than the gap under the row. Same quantity, same
	// name, one axis over -- a second token would have been two names for one
	// distance.
	float HudMusicLabelGap     = 4.f;
	// The now-playing line. Narrow enough that a long filename elides rather
	// than reaching back across the screen into the compass.
	float HudMusicLabelWidth   = 220.f;
	int32 HudMusicLabelSize    = 12;     // == HudCompassSegSize, the HUD's small serif

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
