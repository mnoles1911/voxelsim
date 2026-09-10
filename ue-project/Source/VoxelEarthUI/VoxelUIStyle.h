#pragma once
// The port of assets/ui/UIStyles.gd: the oak button, the oak panel, the label
// variants, and the serif font they all use.
//
// DELIBERATELY NOT AN FSlateStyleSet. A style set buys name-keyed lookup, hot
// reload, and IMAGE_BRUSH's content-root resolution. Nothing here needs the
// first two, and the third is actively unwanted: it routes image loading
// through the Slate resource manager, which decides for itself when a
// multi-megapixel JPEG gets decoded. The menu backgrounds are 1920-wide JPEGs
// and this project's whole streaming design is "everything expensive is
// budgeted, never demand-driven" (doctrine 5), so FVoxelUIAssetLibrary decodes
// them explicitly, on a worker. That leaves this class holding only brushes it
// builds itself, which a plain singleton does perfectly well.

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "Fonts/CompositeFont.h"
#include "Fonts/SlateFontInfo.h"

class VOXELEARTHUI_API FVoxelUIStyle
{
public:
	static void Startup();
	static void Shutdown();
	static FVoxelUIStyle& Get();

	// Live widget count, for Shutdown's assertion.
	//
	// SButton STORES THE FButtonStyle* AND SImage STORES THE FSlateBrush*, as
	// bare pointers into this singleton. That is how Slate works and is fine
	// while the singleton outlives the widgets -- but nothing enforces that
	// ordering, and the failure if it inverts is a paint reading freed memory,
	// which is the kind of crash that gets blamed on the renderer for a week.
	// Front-end widgets register here on construction and unregister on
	// destruction, and Shutdown() says so loudly if any are left.
	static void RegisterWidget();
	static void UnregisterWidget();

	// The Macondo Swash Caps face at a pixel size, or the engine's default
	// face if the .ttf could not be found. IsProjectFontAvailable() says which
	// -- the front end logs it once and -VoxelUINoAssets forces the fallback,
	// so the degraded path is screenshot-testable rather than theoretical.
	FSlateFontInfo Serif(int32 SizePx) const;
	bool IsProjectFontAvailable() const { return bProjectFontLoaded; }

	// THE OTHER THREE FACES OF THE 2026-09-07 MOCKS, each with the same
	// "file present or engine fallback" story as Serif and its own flag.
	//   Mono       VT323           -- the CSS --mono; HUD numbers, key glyphs,
	//                                 tab hints, list headers.
	//   Hand       IM Fell English -- the CSS --hand; body copy on the
	//   HandItalic   (italic)         atmospheric screens: loading quips, the
	//                                 TIP footer, dialogue lines.
	// All three are OFL faces shipped as .ttf under Content/UI/Fonts, loaded
	// by path like Serif, so no .uasset and no editor are needed.
	FSlateFontInfo Mono(int32 SizePx) const;
	FSlateFontInfo Hand(int32 SizePx) const;
	FSlateFontInfo HandItalic(int32 SizePx) const;

	// THE FOURTH FACE, AND THE ONE THAT IS NOT IN THE REPOSITORY.
	//
	// `menus_shared.css` sets `--pixel:"Press Start 2P"` and the 2026-09-07
	// mocks spend it on the journal stamps, the journal page's kind line and a
	// handful of other small all-caps labels. It is an OFL face like the other
	// three, but no .ttf for it was ever committed:
	// docs/ui-mocks/2026-09-07/README.md records it as "not shipped", and a
	// directory listing of Content/UI/Fonts on 2026-09-07 confirmed the four
	// files there are Macondo, VT323 and the two IM Fell romans.
	//
	// SO THIS ACCESSOR SHIPS THE FALLBACK, NOT THE FONT. It looks for
	// PressStart2P-Regular.ttf beside the other three and uses it if the owner
	// drops it in -- no code change, no rebuild-with-an-asset, the same
	// load-by-path story every other face here has. Until then it returns Mono
	// (VT323), which is the closest thing in the shipped set: a fixed-pitch
	// bitmap-derived face, so the `--pixel` sites read as deliberate small caps
	// rather than as the Macondo swash they currently fall back to.
	//
	// IsPixelFontAvailable() is what a capture-comparison should read before
	// filing "the stamp font is wrong" a second time.
	FSlateFontInfo Pixel(int32 SizePx) const;
	bool IsPixelFontAvailable() const { return PixelFont.IsValid(); }

	// UIStyles.menu_button_styles() + apply_menu_button(), as one FButtonStyle:
	// normal PANEL_OAK_2 on black, hover PANEL_OAK_1 on GOLD, pressed
	// PANEL_OAK_2.darkened(0.15) on GOLD_DEEP, disabled
	// PANEL_OAK_2.darkened(0.3) on IRON_DEEP; 2px border, 10px content margin,
	// no corner radius.
	const FButtonStyle& MenuButton() const { return MenuButtonStyle; }
	// The title-screen item's SButton style: transparent in every state, so the
	// button contributes focus, hover and click and paints nothing -- the
	// cartouche chrome is drawn around it by SVoxelMenuButton. The overlay
	// family's Leather and PauseItem variants use it for the same reason.
	const FButtonStyle& CartoucheButton() const { return CartoucheButtonStyle; }

	// The SETTINGS audio sliders (.sl-track / .sl-fill / .sl-knob).
	//
	// THE BAR IS TRANSPARENT AND THE TRACK IS DRAWN BY THE CALLER. FSliderStyle
	// has one bar brush and the mock's track is three things -- a black rule, a
	// dark well and a warm fill whose width IS the value -- which no single
	// brush expresses. So SSlider here contributes the drag, the keyboard steps
	// and the focus, and paints only its knob, exactly as CartoucheButton lets
	// SButton contribute behaviour and no chrome.
	const FSliderStyle& Slider() const { return SliderStyle; }

	// Transparent in every state, for the same reason: the settings checkbox is
	// a well with an inner mark (.ck-box / .ck-box::after), which SCheckBox's
	// one-brush-per-state model cannot draw, so the widget supplies focus and
	// the toggle and the caller supplies the picture.
	const FCheckBoxStyle& OverlayCheckBox() const { return OverlayCheckBoxStyle; }

	// UIStyles.menu_body_panel(): PANEL_OAK_1 fill, 2px black border, 18px
	// content margin. The Godot version also carries a drop shadow
	// (rgba(0,0,0,0.6), size 8, offset (0,4)); Slate's FSlateBrush has no
	// shadow parameter, so SVoxelMainMenu draws it as a second, offset,
	// black-tinted box behind the panel -- see that file.
	const FSlateBrush* MenuBodyPanel() const { return &MenuBodyPanelBrush; }
	// The 1px black border of the panel above, as its own brush, for the
	// dividers the pause menu and settings screens use.
	const FSlateBrush* SolidWhite() const { return &WhiteBrush; }

	// Text colours, as the label variants in UIStyles.gd name them.
	//
	// DELIBERATELY NOT FTextBlockStyle ACCESSORS. STextBlock's TextStyle
	// argument stores a BARE POINTER to the style, so a `.TextStyle(&
	// Style.TitleText(84))` would hand the widget the address of a temporary
	// that dies at the end of the enclosing Construct -- a dangling read on
	// every subsequent paint. Handing out the font and the colour separately
	// makes that mistake unavailable: both are copied by value into the
	// widget's own Font and ColorAndOpacity attributes.
	static FSlateColor TitleColour();  // GOLD
	static FSlateColor BodyColour();   // INK
	static FSlateColor DimColour();    // INK_DIM
	static FSlateColor MutedColour();  // INK_MUTE

private:
	void Initialise();

	// FStandaloneCompositeFont over a filesystem path, so the front end needs
	// no .uasset. This is the explicit form of what FCoreStyle's TTF_FONT
	// macro does for the engine's own styles.
	TSharedPtr<FCompositeFont> SerifFont;
	bool bProjectFontLoaded = false;
	TSharedPtr<FCompositeFont> MonoFont;
	TSharedPtr<FCompositeFont> HandFont;
	TSharedPtr<FCompositeFont> HandItalicFont;
	// Null on every build to date -- see Pixel() above.
	TSharedPtr<FCompositeFont> PixelFont;

	FButtonStyle MenuButtonStyle;
	FButtonStyle CartoucheButtonStyle;
	FSliderStyle SliderStyle;
	FCheckBoxStyle OverlayCheckBoxStyle;
	FSlateBrush MenuBodyPanelBrush;
	FSlateBrush WhiteBrush;
};
