#include "VoxelUIStrings.h"

#define LOCTEXT_NAMESPACE "VoxelEarthUI"

namespace VoxelUIStrings
{
FText Title() { return LOCTEXT("MenuTitle", "VOXELMARK"); }
FText Subtitle() { return LOCTEXT("MenuSubtitle", "Mira-Thal Trilogy · Game One"); }
FText VersionStamp() { return LOCTEXT("MenuVersion", "Milestone 5-3D — dev build"); }

FText ButtonContinue() { return LOCTEXT("BtnContinue", "CONTINUE"); }
FText ButtonNewGame() { return LOCTEXT("BtnNewGame", "NEW GAME"); }
FText ButtonLoadGame() { return LOCTEXT("BtnLoadGame", "LOAD GAME"); }
FText ButtonSettings() { return LOCTEXT("BtnSettings", "SETTINGS"); }
FText ButtonHelp() { return LOCTEXT("BtnHelp", "HELP"); }
FText ButtonCredits() { return LOCTEXT("BtnCredits", "CREDITS"); }
FText ButtonQuit() { return LOCTEXT("BtnQuit", "QUIT"); }
FText ButtonLoad() { return LOCTEXT("BtnLoad", "LOAD"); }
FText ButtonDelete() { return LOCTEXT("BtnDelete", "DELETE"); }
FText ButtonCancel() { return LOCTEXT("BtnCancel", "CANCEL"); }
FText ButtonBack() { return LOCTEXT("BtnBack", "BACK"); }

FText LoadPanelTitle() { return LOCTEXT("LoadPanelTitle", "LOAD GAME"); }
FText LoadPanelEmpty() { return LOCTEXT("LoadPanelEmpty", "No saves yet. Start a New Game to begin."); }
FText HelpPanelTitle() { return LOCTEXT("HelpPanelTitle", "HELP"); }
FText HelpPanelBody() { return LOCTEXT("HelpPanelBody", "Help content coming soon."); }
FText CreditsPanelTitle() { return LOCTEXT("CreditsPanelTitle", "CREDITS"); }
// THE CREDITS SCREEN IS A SHIPPING OBLIGATION, NOT A COURTESY, and that is why
// it stopped being a placeholder. The owner cleared the project's own assets
// for commercial release on 2026-08-25 (see the credits files under
// Content/), but clearance is a permission and the NASA/ESA notices below are
// a CONDITION of use -- they have to appear in any build that renders those
// textures, and until now there was nowhere in the game for them to appear.
//
// TWO THINGS TO GET RIGHT IF THIS IS EDITED.
//
// 1. The MOON lines always apply: T_MoonColor and T_MoonDisplacement are
//    fetched from NASA SVS and imported in every build. The STAR MAP line
//    applies only to builds that import the NASA EXR -- the default star map
//    is procedural and contains no NASA data (see
//    Content/Voxel/TextureSource/SKY_ASSET_CREDITS.md, and note the date on
//    that decision: 2026-08-09). It is included here unconditionally because
//    over-crediting is harmless and under-crediting is a licence breach, and
//    because a static string cannot know which texture a given build imported.
//    If that ever needs to be exact, the sky subsystem knows which star map it
//    loaded and this could be built at runtime instead.
//
// 2. The Unreal attribution wording should be checked against Epic's CURRENT
//    requirement before release rather than trusted from here. It is included
//    because omitting it entirely is the worse error.
FText CreditsPanelBody()
{
	return LOCTEXT("CreditsPanelBody",
		"VOXELMARK\n"
		"Mira-Thal Trilogy - Game One\n"
		"\n\n"
		"THIRD-PARTY NOTICES\n"
		"\n"
		"Moon surface and elevation\n"
		"NASA's Scientific Visualization Studio.\n"
		"\n"
		"Star map\n"
		"NASA/Goddard Space Flight Center Scientific\n"
		"Visualization Studio. Gaia DR2: ESA/Gaia/DPAC.\n"
		"\n"
		"Macondo Swash Caps\n"
		"Open Font Licence 1.1.\n"
		"\n"
		"Unreal Engine\n"
		"Unreal Engine, copyright Epic Games, Inc.\n"
		"All rights reserved.\n"
		"\n\n"
		"Music, sound and menu art are original to this\n"
		"project.\n");
}
FText SettingsPanelTitle() { return LOCTEXT("SettingsPanelTitle", "SETTINGS"); }
FText SettingsPanelBody() { return LOCTEXT("SettingsPanelBody", "Settings coming soon."); }
FText SettingsFineDetailLabel() { return LOCTEXT("SettingsFineDetailLabel", "Fine Detail Smoothing"); }
FText SettingsFineDetailDesc()
{
	return LOCTEXT("SettingsFineDetailDesc",
	               "Extra anti-aliasing for very fine silhouettes. Off runs faster; most "
	               "scenes look identical either way.");
}
FText SettingsToggleOn() { return LOCTEXT("SettingsToggleOn", "ON"); }
FText SettingsFasterTerrainLabel() { return LOCTEXT("SettingsFasterTerrainLabel", "Faster Terrain Drawing"); }
FText SettingsFasterTerrainDesc()
{
	return LOCTEXT("SettingsFasterTerrainDesc",
	               "Speeds up terrain rendering by reusing the previous frame's view. "
	               "No known visual difference; turn off to compare.");
}
FText SettingsToggleOff() { return LOCTEXT("SettingsToggleOff", "OFF"); }

FText LoadingTitle() { return LOCTEXT("LoadingTitle", "L O A D I N G"); }
FText TipPrefix() { return LOCTEXT("TipPrefix", "TIP"); }

const TArray<FText>& LoadingQuips()
{
	// TransitionManager.gd:76-101, in source order. The shuffle happens per
	// show, in SVoxelLoadingScreen::OnShown.
	static const TArray<FText> Quips = {
		LOCTEXT("Quip01", "Pillaging villages..."),
		LOCTEXT("Quip02", "Organizing goblin bands..."),
		LOCTEXT("Quip03", "Conjuring sorcerer spells..."),
		LOCTEXT("Quip04", "Inviting pirates to the royal feast..."),
		LOCTEXT("Quip05", "Sharpening dwarven axes..."),
		LOCTEXT("Quip06", "Lighting the ash-throne's braziers..."),
		LOCTEXT("Quip07", "Forging cursed blades..."),
		LOCTEXT("Quip08", "Plucking arrows from corpses..."),
		LOCTEXT("Quip09", "Counting the king's gold (twice)..."),
		LOCTEXT("Quip10", "Polishing the executioner's block..."),
		LOCTEXT("Quip11", "Whispering rumours in tavern corners..."),
		LOCTEXT("Quip12", "Teaching wolves to read maps..."),
		LOCTEXT("Quip13", "Reminding the Aelorin who they were..."),
		LOCTEXT("Quip14", "Bargaining with the dwindling dead..."),
		LOCTEXT("Quip15", "Stoking the volcano under Drûn-Khazad..."),
		LOCTEXT("Quip16", "Rehearsing Roland's funeral oration..."),
		LOCTEXT("Quip17", "Apologizing to the goats..."),
		LOCTEXT("Quip18", "Bribing the night watch..."),
		LOCTEXT("Quip19", "Translating goblin curses..."),
		LOCTEXT("Quip20", "Salting the fields after harvest..."),
		LOCTEXT("Quip21", "Drafting unfair trade agreements..."),
		LOCTEXT("Quip22", "Misremembering the prophecy..."),
		LOCTEXT("Quip23", "Pouring mead for the long-dead..."),
		LOCTEXT("Quip24", "Stealing songs from minstrels..."),
	};
	return Quips;
}

const TArray<FText>& GameplayTips()
{
	// TransitionManager.gd:57-71, in source order. See the header on why these
	// describe a different game.
	static const TArray<FText> Tips = {
		LOCTEXT("Tip01", "Press [E] to talk to NPCs. Most have things to do."),
		LOCTEXT("Tip02", "Edits to the world persist. The pit you dug last week is still there."),
		LOCTEXT("Tip03", "Hold attack longer for a heavier swing — at the cost of stamina."),
		LOCTEXT("Tip04", "Lock-on with [RMB]. Useful when one-vs-many."),
		LOCTEXT("Tip05", "Settlements are protected. The world won't yield inside their walls."),
		LOCTEXT("Tip06", "Water flows. If you carve under a pond, expect a small flood."),
		LOCTEXT("Tip07", "Save anywhere from the pause menu. Rest at a fire to autosave."),
		LOCTEXT("Tip08", "Lethe's Draught lets you re-spec — once. Spend it carefully."),
		LOCTEXT("Tip09", "Rain dampens fire. Wet bowstrings misfire. Dress for the weather."),
		LOCTEXT("Tip10", "The compass points north. The sun rises east. The map is hand-drawn."),
		LOCTEXT("Tip11", "You can throw most things. Sometimes that solves the problem."),
		LOCTEXT("Tip12", "Roland flinches when low. The HUD rarely lies, but his body never does."),
		LOCTEXT("Tip13", "Press Q / E to cycle quick slots. Shovels won't break stone."),
	};
	return Tips;
}
} // namespace VoxelUIStrings

#undef LOCTEXT_NAMESPACE
