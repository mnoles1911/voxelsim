#pragma once
// Every word the front end puts on screen, in one file.
//
// PORTED VERBATIM from the Godot build (scripts/MainMenu.gd and
// scripts/TransitionManager.gd), including the Mira-Thal branding and the
// fantasy loading quips. That is a deliberate decision recorded in
// docs/front-end-plan.md, not an oversight: the brief was a 1:1 clone of the
// shipped screens, and the shipped screens say VOXELMARK.
//
// WHICH MEANS SOME OF THIS IS FICTION FROM A DIFFERENT GAME. The tips refer to
// Roland, Lethe's Draught, the Aelorin and a pause-menu save button, none of
// which exist in Voxel Earth; several describe systems this project does not
// have (NPCs, stamina, weather-dependent bowstrings). They are here because
// they are what the source screen shows. Re-authoring them for an
// Earth-realistic voxel sim is a named follow-up, and the reason every string
// lives in this one file is so that follow-up is a single-file edit rather
// than a hunt through widget code.

#include "CoreMinimal.h"
#include "Internationalization/Text.h"

namespace VoxelUIStrings
{
// --- Main menu (scripts/MainMenu.gd::_build_main_column) --------------------
VOXELEARTHUI_API FText Title();          // "VOXELMARK"
VOXELEARTHUI_API FText Subtitle();       // "Mira-Thal Trilogy · Game One"
VOXELEARTHUI_API FText VersionStamp();   // bottom-left dev build line

VOXELEARTHUI_API FText ButtonContinue();
VOXELEARTHUI_API FText ButtonNewGame();
VOXELEARTHUI_API FText ButtonLoadGame();
VOXELEARTHUI_API FText ButtonSettings();
VOXELEARTHUI_API FText ButtonHelp();
VOXELEARTHUI_API FText ButtonCredits();
VOXELEARTHUI_API FText ButtonQuit();
VOXELEARTHUI_API FText ButtonLoad();     // per-save row
VOXELEARTHUI_API FText ButtonDelete();   // per-save row
VOXELEARTHUI_API FText ButtonCancel();
VOXELEARTHUI_API FText ButtonBack();

// --- Panels -----------------------------------------------------------------
VOXELEARTHUI_API FText LoadPanelTitle();
VOXELEARTHUI_API FText LoadPanelEmpty();     // "No saves yet. Start a New Game to begin."
VOXELEARTHUI_API FText HelpPanelTitle();
VOXELEARTHUI_API FText HelpPanelBody();
VOXELEARTHUI_API FText CreditsPanelTitle();
VOXELEARTHUI_API FText CreditsPanelBody();
// SETTINGS is out of scope this pass (docs/front-end-plan.md R8), so the
// button opens a placeholder panel shaped exactly like HELP and CREDITS
// rather than being hidden -- the menu the player sees is the menu that
// shipped, and a missing button would be the more visible divergence.
VOXELEARTHUI_API FText SettingsPanelTitle();
VOXELEARTHUI_API FText SettingsPanelBody();

// --- Loading screen ---------------------------------------------------------
// "L O A D I N G" -- the spaces are literal. Godot has no letter-spacing
// property on Label either, so the source fakes tracking exactly this way and
// the port keeps the trick rather than inventing a different one.
VOXELEARTHUI_API FText LoadingTitle();
VOXELEARTHUI_API FText TipPrefix();   // "TIP"

// The 24 dark-humour lines (TransitionManager.gd:76-101) and the 13 gameplay
// tips (:57-71). Shuffled fresh on every show, which is why the caller gets
// the whole array rather than a "next" function -- the shuffle belongs to the
// screen's lifetime, not to the string table.
VOXELEARTHUI_API const TArray<FText>& LoadingQuips();
VOXELEARTHUI_API const TArray<FText>& GameplayTips();
} // namespace VoxelUIStrings
