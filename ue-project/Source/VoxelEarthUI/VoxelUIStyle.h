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

	// UIStyles.menu_button_styles() + apply_menu_button(), as one FButtonStyle:
	// normal PANEL_OAK_2 on black, hover PANEL_OAK_1 on GOLD, pressed
	// PANEL_OAK_2.darkened(0.15) on GOLD_DEEP, disabled
	// PANEL_OAK_2.darkened(0.3) on IRON_DEEP; 2px border, 10px content margin,
	// no corner radius.
	const FButtonStyle& MenuButton() const { return MenuButtonStyle; }

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

	FButtonStyle MenuButtonStyle;
	FSlateBrush MenuBodyPanelBrush;
	FSlateBrush WhiteBrush;
};
