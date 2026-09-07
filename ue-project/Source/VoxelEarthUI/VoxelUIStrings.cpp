#include "VoxelUIStrings.h"

#define LOCTEXT_NAMESPACE "VoxelEarthUI"

namespace VoxelUIStrings
{
FText Title() { return LOCTEXT("MenuTitle", "VOXELMARK"); }
FText Subtitle() { return LOCTEXT("MenuSubtitle", "Mira-Thal Trilogy · Game One"); }
FText VersionStamp() { return LOCTEXT("MenuVersion", "Milestone 5-3D — dev build"); }

FText CalloutTag() { return LOCTEXT("CalloutTag", "PATCH 0.18 — THE BRONZE AGE"); }
FText CalloutTitle() { return LOCTEXT("CalloutTitle", "Metalsmithing Update"); }
FText CalloutCopy()
{
	return LOCTEXT("CalloutCopy",
	               "Copper and tin now spawn in surface veins. Smelt them together for bronze: a full tool tier that "
	               "mines faster and lasts four times as long as stone. New crafting stations — bloomery, crucible "
	               "and anvil. Worn tools can be reforged instead of thrown away.");
}

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

// --- Overlay family (2026-09-07 mocks) --------------------------------------
FText SettingsSectionAudio() { return LOCTEXT("SetSecAudio", "AUDIO"); }
FText SettingsSectionDisplay() { return LOCTEXT("SetSecDisplay", "DISPLAY"); }
FText SettingsSectionGraphics() { return LOCTEXT("SetSecGraphics", "GRAPHICS"); }
FText SettingsMasterLabel() { return LOCTEXT("SetMaster", "MASTER"); }
FText SettingsMusicLabel() { return LOCTEXT("SetMusic", "MUSIC"); }
FText SettingsFullscreenLabel() { return LOCTEXT("SetFullscreen", "FULLSCREEN"); }
// The one row on this panel that does NOT take effect as you touch it, said out
// loud rather than left for the player to discover by not noticing.
FText SettingsFullscreenHint() { return LOCTEXT("SetFullscreenHint", "Takes effect on APPLY."); }
FText ButtonApply() { return LOCTEXT("BtnApply", "APPLY"); }
FText ButtonSaveAndLeave() { return LOCTEXT("BtnSaveLeave", "SAVE & LEAVE"); }
FText SettingsBackHint() { return LOCTEXT("SetBackHint", "back"); }
FText KeyEscape() { return LOCTEXT("KeyEsc", "ESC"); }
FText KeyEnter() { return LOCTEXT("KeyEnter", "ENTER"); }

FText PauseTitle() { return LOCTEXT("PauseTitle", "— PAUSED —"); }
FText ButtonResume() { return LOCTEXT("BtnResume", "RESUME"); }
FText ButtonSave() { return LOCTEXT("BtnSave", "SAVE"); }
FText ButtonExitToMenu() { return LOCTEXT("BtnExitToMenu", "EXIT TO MENU"); }
FText PauseFooter(int32 DayNumber)
{
	return FText::Format(LOCTEXT("PauseFooter", "Day {0}"), FText::AsNumber(DayNumber));
}

FText SaveDialogTitle() { return LOCTEXT("SaveDialogTitle", "— SAVE GAME —"); }
FText SaveDialogPrompt() { return LOCTEXT("SaveDialogPrompt", "NAME THIS SAVE"); }
FText SaveDialogCounter(int32 Used, int32 Max)
{
	// AsNumber would group at a thousand; these are two- and three-digit counts
	// in a monospace readout and a separator would be noise.
	return FText::FromString(FString::Printf(TEXT("%d / %d"), Used, Max));
}
FText SaveOverwritePrefix() { return LOCTEXT("SaveOverwritePrefix", "A save named"); }
FText SaveOverwriteSuffix() { return LOCTEXT("SaveOverwriteSuffix", "already exists. Confirm to overwrite it."); }
FText SaveKeysConfirm() { return LOCTEXT("SaveKeysConfirm", "to confirm"); }
FText SaveKeysCancel() { return LOCTEXT("SaveKeysCancel", "to cancel"); }
FText ButtonConfirm() { return LOCTEXT("BtnConfirm", "CONFIRM"); }
FText DefaultSaveName(const FText& Context, int32 DayNumber)
{
	return FText::Format(LOCTEXT("DefaultSaveName", "{0} Day {1}"), Context, FText::AsNumber(DayNumber));
}

FText LoadFilterAll() { return LOCTEXT("LoadFilterAll", "ALL"); }
FText LoadFilterManual() { return LOCTEXT("LoadFilterManual", "MANUAL"); }
FText LoadFilterAuto() { return LOCTEXT("LoadFilterAuto", "AUTO"); }
FText LoadSearchHint() { return LOCTEXT("LoadSearchHint", "Search…"); }
FText LoadCountTotal(int32 Total)
{
	return FText::Format(LOCTEXT("LoadCountTotal", "/ {0}"), FText::AsNumber(Total));
}
FText LoadTagLatest() { return LOCTEXT("LoadTagLatest", "LATEST"); }
FText LoadTagAuto() { return LOCTEXT("LoadTagAuto", "AUTO"); }
FText LoadNoMatch() { return LOCTEXT("LoadNoMatch", "No saves match."); }
FText LoadAutosaveName() { return LOCTEXT("LoadAutosaveName", "Autosave"); }
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
FText SettingsWaterWaveLabel() { return LOCTEXT("SettingsWaterWaveLabel", "Water Wave Detail"); }
FText SettingsWaterWaveDesc()
{
	return LOCTEXT("SettingsWaterWaveDesc",
	               "Lets waves on nearby lakes and rivers rise and fall as real shapes. Off "
	               "keeps those surfaces flat and only shades the waves onto them.");
}
FText SettingsOceanDetailLabel() { return LOCTEXT("SettingsOceanDetailLabel", "Ocean Mesh Detail"); }
FText SettingsOceanDetailDesc()
{
	return LOCTEXT("SettingsOceanDetailDesc",
	               "How finely the open sea is built around you. Off uses a coarser surface "
	               "close to the camera; the horizon reaches just as far either way.");
}

// "LOADING", tracked by FSlateFontInfo::LetterSpacing (FVoxelMenuLayout::
// LoadingTitleLetterSpacing) rather than by literal spaces: UE5 has real
// tracking, which the Godot-era note in the header predates.
FText LoadingTitle() { return LOCTEXT("LoadingTitle", "LOADING"); }
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
// --- In-game screens (2026-09-07 wave 2) ------------------------------------

FText ScreenTabMap()       { return LOCTEXT("ScrTabMap", "MAP"); }
FText ScreenTabJournal()   { return LOCTEXT("ScrTabJournal", "JOURNAL"); }
FText ScreenTabInventory() { return LOCTEXT("ScrTabInventory", "INVENTORY"); }
FText ScreenTabPlayer()    { return LOCTEXT("ScrTabPlayer", "PLAYER"); }
FText ScreenTabCodex()     { return LOCTEXT("ScrTabCodex", "CODEX"); }
// THE MOCK DRAWS NO KEY LETTERS. `.menu-tab .key` is styled in menus_shared.css
// and every one of the five files leaves the span out, so the shortcut set is
// chosen by this port rather than copied -- see SVoxelScreenShell::TabKey.
FText ScreenKeyMap()       { return LOCTEXT("ScrKeyMap", "M"); }
FText ScreenKeyJournal()   { return LOCTEXT("ScrKeyJournal", "J"); }
FText ScreenKeyInventory() { return LOCTEXT("ScrKeyInventory", "I"); }
FText ScreenKeyPlayer()    { return LOCTEXT("ScrKeyPlayer", "P"); }
FText ScreenKeyCodex()     { return LOCTEXT("ScrKeyCodex", "K"); }
FText ScreenActionClose()  { return LOCTEXT("ScrActClose", "Exit"); }
FText ScreenActionPage()   { return LOCTEXT("ScrActPage", "Switch screen"); }

FText InvPack()          { return LOCTEXT("InvPack", "PACK"); }
FText InvCrafting()      { return LOCTEXT("InvCrafting", "CRAFTING"); }
FText InvCharacter()     { return LOCTEXT("InvCharacter", "CHARACTER"); }
FText InvModeCraft()     { return LOCTEXT("InvModeCraft", "CRAFT"); }
FText InvModeCharacter() { return LOCTEXT("InvModeChar", "CHARACTER"); }
FText InvCraftButton()   { return LOCTEXT("InvCraftBtn", "CRAFT"); }
FText InvCraftHint()     { return LOCTEXT("InvCraftHint", "Drag from the pack into the grid."); }
FText InvNoRecipe()      { return LOCTEXT("InvNoRecipe", "NO MATCH"); }
FText InvNoEquipment()
{
	// Said on the screen rather than only in a comment: the paperdoll is drawn
	// with eight empty sockets and no way to fill them, and a player looking at
	// it deserves to be told that rather than left to conclude they have lost
	// their gear.
	return LOCTEXT("InvNoEquip", "No equipment system yet — these sockets are chrome.");
}

const TArray<FText>& InvFilterNames()
{
	static const TArray<FText> Names = {
		LOCTEXT("InvFilterAll", "All"),
		LOCTEXT("InvFilterWeapons", "Weapons"),
		LOCTEXT("InvFilterArmor", "Armor"),
		LOCTEXT("InvFilterTools", "Tools"),
		LOCTEXT("InvFilterFood", "Food"),
		LOCTEXT("InvFilterMaterials", "Materials"),
		LOCTEXT("InvFilterQuest", "Quest"),
	};
	return Names;
}

const TArray<FText>& InvEquipSlotNames()
{
	static const TArray<FText> Names = {
		LOCTEXT("InvEqHead", "HEAD"),   LOCTEXT("InvEqNeck", "NECK"),
		LOCTEXT("InvEqChest", "CHEST"), LOCTEXT("InvEqHands", "HANDS"),
		LOCTEXT("InvEqMain", "MAIN"),   LOCTEXT("InvEqOff", "OFF"),
		LOCTEXT("InvEqRing", "RING"),   LOCTEXT("InvEqFeet", "FEET"),
	};
	return Names;
}

const TArray<FText>& PlayerEquipSlotNames()
{
	static const TArray<FText> Names = {
		LOCTEXT("PlEqHead", "HEAD"),   LOCTEXT("PlEqChest", "CHEST"),
		LOCTEXT("PlEqHands", "HANDS"), LOCTEXT("PlEqLegs", "LEGS"),
		LOCTEXT("PlEqMain", "MAIN"),   LOCTEXT("PlEqOff", "OFF"),
		LOCTEXT("PlEqRing", "RING"),   LOCTEXT("PlEqFeet", "FEET"),
	};
	return Names;
}

FText InvSearchHint() { return LOCTEXT("InvSearch", "Search the pack"); }

FText InvWeight(float CarriedKg)
{
	// THE MOCK'S "112 / 180" LOSES ITS DENOMINATOR. Mass is real -- every
	// FVoxelItemDef carries MassKg -- but this project has no carry limit, so
	// there is no 180 to print and inventing one would be inventing a system.
	return FText::Format(LOCTEXT("InvWeight", "{0} kg"),
	                     FText::AsNumber(FMath::RoundToInt(CarriedKg)));
}

FText MapSavedPlaces()   { return LOCTEXT("MapSaved", "SAVED PLACES"); }
FText MapNoPlaces()      { return LOCTEXT("MapNoPlaces", "No places saved yet."); }
FText MapLabelPosition() { return LOCTEXT("MapLabPos", "POSITION"); }
FText MapLabelGeo()      { return LOCTEXT("MapLabGeo", "LATITUDE / LONGITUDE"); }
FText MapLabelAltitude() { return LOCTEXT("MapLabAlt", "SURFACE"); }
FText MapLabelChunk()    { return LOCTEXT("MapLabChunk", "CHUNK"); }
FText MapLabelHeading()  { return LOCTEXT("MapLabHeading", "HEADING"); }
FText MapLabelSeed()     { return LOCTEXT("MapLabSeed", "SEED"); }
FText MapNoRaster()
{
	return LOCTEXT("MapNoRaster", "No terrain raster — position readout only");
}

FText MapOverviewNote()
{
	return LOCTEXT("MapOverview", "World overview — your position is on the right");
}

FText MapPositionValue(const FVector& World)
{
	// Metres, not Unreal units: a player reading a position wants a number they
	// can pace out, and VoxelCoords::VoxelSizeUU makes one voxel 10 uu = 0.1 m.
	//
	// THREE LINES, NOT ONE. The first capture ran the three axes together and
	// the readout card clipped the third off at its right edge -- 61 km from
	// the origin is a six-digit metre count and three of them do not fit a
	// 250 px column at any font this screen uses.
	return FText::Format(LOCTEXT("MapPosVal", "E {0}\nN {1}\nUp {2}"),
	                     FText::AsNumber(FMath::RoundToInt(World.X / 100.0)),
	                     FText::AsNumber(FMath::RoundToInt(World.Y / 100.0)),
	                     FText::AsNumber(FMath::RoundToInt(World.Z / 100.0)));
}

FText MapSeedValue(uint64 Seed)
{
	// NO GROUPING SEPARATORS. FText::AsNumber renders 20260719 as "20,260,719",
	// which reads as a quantity; a seed is an identifier and has to be
	// copy-pasteable back into -VoxelSeed=.
	return FText::FromString(FString::Printf(TEXT("%llu"), (unsigned long long)Seed));
}

FText MapGeoValue(double LatitudeDeg, double LongitudeDeg)
{
	FNumberFormattingOptions Opts;
	Opts.MinimumFractionalDigits = 4;
	Opts.MaximumFractionalDigits = 4;
	return FText::Format(LOCTEXT("MapGeoVal", "{0}° {1}   {2}° {3}"),
	                     FText::AsNumber(FMath::Abs(LatitudeDeg), &Opts),
	                     LatitudeDeg >= 0.0 ? LOCTEXT("MapN", "N") : LOCTEXT("MapS", "S"),
	                     FText::AsNumber(FMath::Abs(LongitudeDeg), &Opts),
	                     LongitudeDeg >= 0.0 ? LOCTEXT("MapE", "E") : LOCTEXT("MapW", "W"));
}

FText MapChunkValue(const FIntVector& Chunk)
{
	return FText::Format(LOCTEXT("MapChunkVal", "{0}, {1}, {2}"),
	                     FText::AsNumber(Chunk.X), FText::AsNumber(Chunk.Y), FText::AsNumber(Chunk.Z));
}

FText MapHeadingValue(float Degrees)
{
	static const TCHAR* const kPoints[] = {TEXT("N"), TEXT("NE"), TEXT("E"), TEXT("SE"),
	                                       TEXT("S"), TEXT("SW"), TEXT("W"), TEXT("NW")};
	const float Wrapped = FMath::Fmod(Degrees + 360.f, 360.f);
	const int32 Index = FMath::RoundToInt(Wrapped / 45.f) % 8;
	return FText::Format(LOCTEXT("MapHeadingVal", "{0}°  {1}"),
	                     FText::AsNumber(FMath::RoundToInt(Wrapped)),
	                     FText::FromString(kPoints[Index]));
}

const TArray<FText>& MapCompassLetters()
{
	static const TArray<FText> Letters = {
		LOCTEXT("MapCompassN", "N"), LOCTEXT("MapCompassE", "E"),
		LOCTEXT("MapCompassS", "S"), LOCTEXT("MapCompassW", "W"),
	};
	return Letters;
}

FText JournalSectionEntries()  { return LOCTEXT("JrSecEntries", "PLAYER'S ENTRIES"); }
FText JournalSectionGoals()    { return LOCTEXT("JrSecGoals", "GOALS"); }
FText JournalTracked()         { return LOCTEXT("JrTracked", "Tracked"); }
FText JournalUntracked()       { return LOCTEXT("JrUntracked", "Untracked"); }
FText JournalEntriesTab()      { return LOCTEXT("JrEntriesTab", "Entries"); }
FText JournalNewEntry()        { return LOCTEXT("JrNewEntry", "NEW ENTRY"); }
FText JournalWriteHere()       { return LOCTEXT("JrWrite", "Write in the journal"); }
FText JournalEmpty()           { return LOCTEXT("JrEmpty", "Nothing written yet."); }
FText JournalNothingTracked()  { return LOCTEXT("JrNoTracked", "Nothing tracked."); }
FText JournalNothingUntracked(){ return LOCTEXT("JrNoUntracked", "Nothing hidden."); }
FText JournalSteps()           { return LOCTEXT("JrSteps", "Steps"); }
FText JournalTrack()           { return LOCTEXT("JrTrack", "TRACK"); }
FText JournalUntrack()         { return LOCTEXT("JrUntrack", "UNTRACK"); }
FText JournalKindEntry()       { return LOCTEXT("JrKindEntry", "PLAYER'S ENTRIES"); }
FText JournalKindTracked()     { return LOCTEXT("JrKindTracked", "TRACKED GOAL"); }
FText JournalKindUntracked()   { return LOCTEXT("JrKindUntracked", "UNTRACKED GOAL"); }

FText JournalStepProgress(int32 Done, int32 Total, const FText& Place)
{
	return FText::Format(LOCTEXT("JrStepProgress", "{0} of {1} steps · {2}"),
	                     FText::AsNumber(Done), FText::AsNumber(Total), Place);
}

FText JournalCardStamp(int32 Day, const FText& Season)
{
	return FText::Format(LOCTEXT("JrCardStamp", "DAY {0} · {1}"),
	                     FText::AsNumber(Day), Season);
}

FText PlayerSubStats()      { return LOCTEXT("PlSubStats", "STATS"); }
FText PlayerSubSkills()     { return LOCTEXT("PlSubSkills", "SKILLS"); }
FText PlayerSubPerks()      { return LOCTEXT("PlSubPerks", "PERKS"); }
FText PlayerSubReputation() { return LOCTEXT("PlSubRep", "REPUTATION"); }
FText PlayerMainLevel()     { return LOCTEXT("PlMainLevel", "MAIN LEVEL"); }
FText PlayerXp()            { return LOCTEXT("PlXp", "XP"); }
FText PlayerSkillPoints()   { return LOCTEXT("PlSkillPts", "SKILL POINTS"); }
FText PlayerDisciplines()   { return LOCTEXT("PlDisciplines", "Disciplines"); }
FText PlayerColumnName()    { return LOCTEXT("PlColName", "NAME"); }
FText PlayerColumnLevel()   { return LOCTEXT("PlColLevel", "LEVEL"); }
FText PlayerColumnStatus()  { return LOCTEXT("PlColStatus", "STATUS"); }
FText PlayerColumnFaction() { return LOCTEXT("PlColFaction", "FACTION"); }
FText PlayerColumnStanding(){ return LOCTEXT("PlColStanding", "STANDING"); }
FText PlayerColumnValue()   { return LOCTEXT("PlColValue", "VALUE"); }
FText PlayerStatusOwned()   { return LOCTEXT("PlOwned", "Owned"); }
FText PlayerStatusLocked()  { return LOCTEXT("PlLocked", "Locked"); }

FText PlayerXpValue(int32 Current, int32 Next)
{
	return FText::Format(LOCTEXT("PlXpVal", "{0} / {1}"),
	                     FText::AsNumber(Current), FText::AsNumber(Next));
}

FText PlayerRankLine(int32 From, int32 To)
{
	return FText::Format(LOCTEXT("PlRankLine", "Rank {0} → Rank {1}"),
	                     FText::AsNumber(From), FText::AsNumber(To));
}

FText PlayerUnlockCost(int32 Points)
{
	return FText::Format(LOCTEXT("PlUnlock", "UNLOCK · {0} PT"), FText::AsNumber(Points));
}

FText PlayerRankBadge(int32 Rank, int32 MaxRank)
{
	return FText::Format(LOCTEXT("PlRankBadge", "{0}/{1}"),
	                     FText::AsNumber(Rank), FText::AsNumber(MaxRank));
}

FText PlayerDisciplineTally(int32 Unlocked, int32 Total)
{
	return FText::Format(LOCTEXT("PlDiscTally", "{0}/{1}"),
	                     FText::AsNumber(Unlocked), FText::AsNumber(Total));
}

FText CodexRecipes()     { return LOCTEXT("CdRecipes", "Recipes"); }
FText CodexPlaces()      { return LOCTEXT("CdPlaces", "Places"); }
FText CodexIngredients() { return LOCTEXT("CdIngredients", "INGREDIENTS"); }
FText CodexYieldLabel()  { return LOCTEXT("CdYieldLab", "YIELD"); }
// SHORT ENOUGH FOR THE COLUMN IT SITS IN. The mock's "Search recipes or
// ingredients…" was clipped to "Search recipes or ingredi" by the 260 px entry
// list in the first codex capture; the search still matches ingredient names.
FText CodexSearchHint()  { return LOCTEXT("CdSearch", "Search recipes"); }
FText CodexNoRecipe()    { return LOCTEXT("CdNoRecipe", "No recipe matches that."); }
FText CodexLocked()      { return LOCTEXT("CdLocked", "Not yet discovered."); }

FText CodexMadeAt(const FText& Station)
{
	return FText::Format(LOCTEXT("CdMadeAt", "Made at {0}"), Station);
}

FText CodexYield(const FText& Category, int32 Count)
{
	return FText::Format(LOCTEXT("CdYield", "{0} · yields {1}"), Category, FText::AsNumber(Count));
}

FText CodexIngredientLine(int32 Needed, const FText& Name)
{
	return FText::Format(LOCTEXT("CdIngLine", "{0} × {1}"), FText::AsNumber(Needed), Name);
}

FText CodexHeldLine(int32 Held)
{
	return FText::Format(LOCTEXT("CdHeld", "{0} in pack"), FText::AsNumber(Held));
}

FText CodexHeldUnknown()
{
	// A recipe ingredient that names nothing in FVoxelItemRegistry cannot be
	// counted, and printing "0 in pack" would be a claim rather than a gap.
	return LOCTEXT("CdHeldUnknown", "not an item yet");
}

FText DeathTitle()   { return LOCTEXT("DeathTitle", "YOU DIED"); }
FText DeathRespawn() { return LOCTEXT("DeathRespawn", "RESPAWN"); }
FText DeathQuit()    { return LOCTEXT("DeathQuit", "QUIT"); }

const TArray<FText>& DeathQuips()
{
	static const TArray<FText> Quips = {
		LOCTEXT("DQ01", "Try not to starve next time."),
		LOCTEXT("DQ02", "The world does not keep notes on how hard you tried."),
		LOCTEXT("DQ03", "Somewhere out there, your tools are still in a chest."),
		LOCTEXT("DQ04", "You lasted longer than most. Not much longer."),
		LOCTEXT("DQ05", "Consider a wall. Walls have worked before."),
		LOCTEXT("DQ06", "Nothing out here was ever on your side."),
		LOCTEXT("DQ07", "Eat something. Sleep somewhere. Build a door."),
		LOCTEXT("DQ08", "Your camp is exactly where you left it. So is everything else."),
		LOCTEXT("DQ09", "Being brave and being prepared are different projects."),
		LOCTEXT("DQ10", "The night was always going to win one of these."),
	};
	return Quips;
}

FText DlgBandEasy()      { return LOCTEXT("DlgEasy", "EASY"); }
FText DlgBandMedium()    { return LOCTEXT("DlgMedium", "MEDIUM"); }
FText DlgBandHard()      { return LOCTEXT("DlgHard", "HARD"); }
FText DlgActionSelect()  { return LOCTEXT("DlgSelect", "Choose a reply"); }

FText HudDemoInteract()    { return LOCTEXT("HudInteract", "Examine the cairn"); }
FText HudDemoInteractKey() { return LOCTEXT("HudInteractKey", "E"); }
} // namespace VoxelUIStrings

#undef LOCTEXT_NAMESPACE
