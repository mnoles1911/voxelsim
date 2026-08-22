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
FText CreditsPanelBody() { return LOCTEXT("CreditsPanelBody", "Credits coming soon."); }
FText SettingsPanelTitle() { return LOCTEXT("SettingsPanelTitle", "SETTINGS"); }
FText SettingsPanelBody() { return LOCTEXT("SettingsPanelBody", "Settings coming soon."); }

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
