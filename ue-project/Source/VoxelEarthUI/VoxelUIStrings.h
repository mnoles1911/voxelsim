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

// The patch-notes callout, top-left of the 2026-09-07 title screen
// (.callout-news). Placeholder copy from the mock, same disclaimer as the tips:
// it describes a metalsmithing update this project does not have, and lives
// here so replacing it is a one-file edit.
VOXELEARTHUI_API FText CalloutTag();     // "PATCH 0.18 - THE BRONZE AGE"
VOXELEARTHUI_API FText CalloutTitle();   // "Metalsmithing Update"
VOXELEARTHUI_API FText CalloutCopy();

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
// SETTINGS stopped being a placeholder on 2026-09-04 and stopped being
// message-panel shaped on 2026-09-07; SettingsPanelBody ("Settings coming
// soon.") went with the shape. The title is still shared by the title screen's
// panel and the pause overlay's.
VOXELEARTHUI_API FText SettingsPanelTitle();
// The first real settings row (2026-09-04): the panel stopped being a
// placeholder the day the owner asked for a player-facing toggle. Label and
// description are plain player language; the cvar mapping lives in
// VoxelGraphicsUserSettings.cpp, never in a string.
VOXELEARTHUI_API FText SettingsFineDetailLabel();
VOXELEARTHUI_API FText SettingsFineDetailDesc();
VOXELEARTHUI_API FText SettingsToggleOn();
VOXELEARTHUI_API FText SettingsToggleOff();
VOXELEARTHUI_API FText SettingsFasterTerrainLabel();
VOXELEARTHUI_API FText SettingsFasterTerrainDesc();
// Rows three and four (2026-09-05), both about water. Written in what the
// PLAYER sees -- whether the swell has shape, how finely the sea is built --
// with no cvar, no metres, and no mention of how long a change takes to sweep
// the resident lake basins. That latency is real and it is under a second; a
// description that named it would read as a defect notice for something no
// player would otherwise notice. See VoxelWaterSheetActor.cpp's cvar note.
VOXELEARTHUI_API FText SettingsWaterWaveLabel();
VOXELEARTHUI_API FText SettingsWaterWaveDesc();
VOXELEARTHUI_API FText SettingsOceanDetailLabel();
VOXELEARTHUI_API FText SettingsOceanDetailDesc();

// --- Overlay family (2026-09-07 mocks) --------------------------------------
// Pause, Settings, Save and Load. Unlike the strings above these are NOT ported
// from the Godot build -- three of the four screens did not exist there -- so
// they are the mocks' own words, and where a mock says something this project
// cannot mean (the pause footer's "18th Summer of the Second Age") the string
// says the part that is true. See the note on PauseFooter.

// --- SETTINGS ---------------------------------------------------------------
VOXELEARTHUI_API FText SettingsSectionAudio();
VOXELEARTHUI_API FText SettingsSectionDisplay();
VOXELEARTHUI_API FText SettingsSectionGraphics();
VOXELEARTHUI_API FText SettingsMasterLabel();
VOXELEARTHUI_API FText SettingsMusicLabel();
VOXELEARTHUI_API FText SettingsFullscreenLabel();
VOXELEARTHUI_API FText SettingsFullscreenHint();
// The INTERFACE section, which the 2026-09-07 mock has no counterpart for:
// ADR-0011 decision 5, the player's manual multiplier on top of the engine's
// own resolution scale.
VOXELEARTHUI_API FText SettingsSectionInterface();
VOXELEARTHUI_API FText SettingsUIScaleLabel();
VOXELEARTHUI_API FText SettingsUIScaleHint();
// MENU SIZE -- the five in-game screens' own dial, separate from INTERFACE
// SIZE above: that one scales the whole application, this one scales the
// unified screen shell and nothing else. See VoxelScreenShellSettings.h.
VOXELEARTHUI_API FText SettingsMenuSizeLabel();
VOXELEARTHUI_API FText SettingsMenuSizeHint();
// "125%" -- the INTERFACE SIZE readout. Its own function because the slider
// readouts elsewhere on the panel are bare numbers and this one is not.
VOXELEARTHUI_API FText SettingsPercent(int32 Percent);
VOXELEARTHUI_API FText ButtonApply();
VOXELEARTHUI_API FText ButtonSaveAndLeave();
VOXELEARTHUI_API FText SettingsBackHint();   // the word after the ESC cap
VOXELEARTHUI_API FText KeyEscape();          // "ESC", inside a key cap
VOXELEARTHUI_API FText KeyEnter();           // "ENTER"

// --- PAUSE ------------------------------------------------------------------
VOXELEARTHUI_API FText PauseTitle();
VOXELEARTHUI_API FText ButtonResume();
VOXELEARTHUI_API FText ButtonSave();
VOXELEARTHUI_API FText ButtonExitToMenu();
// The mock's footer reads "Day 12, the 18th Summer of the Second Age".
//
// THE DAY IS REAL AND THE ERA IS NOT. UVoxelSkySubsystem keeps a genuine
// calendar -- EpochSeconds over voxel.Sky.DayLengthSeconds gives the day, and
// FVoxelSkyState::DayOfYear the seasonal position -- so "Day N" is a fact this
// project can state. There is no era or age anywhere in the codebase, so the
// footer says the day and stops.
//
// THE SEASON HALF OF THIS NOTE IS RETRACTED (2026-09-08). It used to argue that
// a season here would be "confidently wrong" because VoxelEarthHUD.cpp's
// boundaries (day-of-year 79/172/265/355) assume a 365-day year while
// voxel.Sky.DaysPerYear defaults to 48. DayOfYear is not a day COUNT: it is
// frac(Epoch / (DayLength x DaysPerYear)) x 365.2425, an astronomical index that
// spans 0..365 at every DaysPerYear, so those boundaries are right. The season
// is now printed by WorldStamp above. THIS FOOTER STILL DOES NOT USE IT -- not
// because it cannot, but because the pause screen is one the owner has judged
// and widening its footer is a visual change, not a correction.
VOXELEARTHUI_API FText PauseFooter(int32 DayNumber);

// --- SAVE DIALOG ------------------------------------------------------------
VOXELEARTHUI_API FText SaveDialogTitle();
VOXELEARTHUI_API FText SaveDialogPrompt();
VOXELEARTHUI_API FText SaveDialogCounter(int32 Used, int32 Max);
// The overwrite band, in three pieces because the colliding name is picked out
// in gold serif inside an otherwise italic line and one FText cannot carry two
// styles.
VOXELEARTHUI_API FText SaveOverwritePrefix();
VOXELEARTHUI_API FText SaveOverwriteSuffix();
VOXELEARTHUI_API FText SaveKeysConfirm();   // follows the ENTER cap
VOXELEARTHUI_API FText SaveKeysCancel();    // follows the ESC cap
VOXELEARTHUI_API FText ButtonConfirm();
// The default name offered for a new save: "<active save or Voxelmark> Day N".
VOXELEARTHUI_API FText DefaultSaveName(const FText& Context, int32 DayNumber);

// --- LOAD DIALOG ------------------------------------------------------------
VOXELEARTHUI_API FText LoadFilterAll();
VOXELEARTHUI_API FText LoadFilterManual();
VOXELEARTHUI_API FText LoadFilterAuto();
VOXELEARTHUI_API FText LoadSearchHint();
VOXELEARTHUI_API FText LoadCountTotal(int32 Total); // the "/ N" half; the visible count is drawn gold beside it
VOXELEARTHUI_API FText LoadTagLatest();
VOXELEARTHUI_API FText LoadTagAuto();
VOXELEARTHUI_API FText LoadNoMatch();
VOXELEARTHUI_API FText LoadAutosaveName();
// NO "DAY N" BADGE ON A SAVE ROW. The mock stamps one across each thumbnail,
// and the save format has no in-game day in it to stamp -- VoxelSave::FSaveInfo
// carries a wall-clock timestamp, a seed, a position and an edit count, and
// nothing about the world's calendar. Adding one is a meta.json key and a read
// in VoxelSaveLibrary, which is a gameplay-module change this UI port did not
// make; until then a badge would either be blank or invented.

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

// --- In-game screens (2026-09-07 wave 2) ------------------------------------
//
// CHROME ONLY. The labels below are the parts of the five screens that are the
// SCREEN rather than its contents -- tab names, column headings, button words,
// action-bar hints. The placeholder journal entries, codex pages, perks and
// factions are NOT here: they live in VoxelScreenData.cpp behind Seed*(), for
// the reason that file gives -- they are scaffolding to be deleted whole when
// the systems arrive, and mixing them into the permanent string table would
// make that deletion a hunt rather than a file.

// .menu-tab labels and the shortcut letter each draws in its .key span.
VOXELEARTHUI_API FText ScreenTabMap();
VOXELEARTHUI_API FText ScreenTabJournal();
VOXELEARTHUI_API FText ScreenTabInventory();
VOXELEARTHUI_API FText ScreenTabPlayer();
VOXELEARTHUI_API FText ScreenTabCodex();
VOXELEARTHUI_API FText ScreenKeyMap();
VOXELEARTHUI_API FText ScreenKeyJournal();
VOXELEARTHUI_API FText ScreenKeyInventory();
VOXELEARTHUI_API FText ScreenKeyPlayer();
VOXELEARTHUI_API FText ScreenKeyCodex();
VOXELEARTHUI_API FText ScreenActionClose();   // the shared "ESC  Exit" hint
VOXELEARTHUI_API FText ScreenActionPage();    // "Q/E  Switch screen"

// --- Inventory --------------------------------------------------------------
VOXELEARTHUI_API FText InvPack();
VOXELEARTHUI_API FText InvCrafting();
VOXELEARTHUI_API FText InvCharacter();
VOXELEARTHUI_API FText InvModeCraft();
VOXELEARTHUI_API FText InvModeCharacter();
VOXELEARTHUI_API FText InvCraftButton();
VOXELEARTHUI_API FText InvCraftHint();
VOXELEARTHUI_API FText InvNoRecipe();
// The seven .filter-tab chips. NOTHING IN THIS GAME HAS A CATEGORY that maps
// onto six of them -- FVoxelItemDef's four categories are Block, Tool,
// Throwable and Misc -- so Weapons, Armor, Food and Quest are always empty and
// are drawn anyway, because a filter row that changed length with the contents
// of the pack would move under the cursor.
VOXELEARTHUI_API const TArray<FText>& InvFilterNames();
// HEAD / NECK / CHEST / HANDS / MAIN / OFF / RING / FEET -- the INVENTORY
// mock's paperdoll.
VOXELEARTHUI_API const TArray<FText>& InvEquipSlotNames();
// HEAD / CHEST / HANDS / LEGS / MAIN / OFF / RING / FEET -- the PLAYER mock's.
//
// THE TWO MOCKS DISAGREE, and it is not a typo in either: the inventory screen
// has a NECK socket the player screen does not, and the player screen has LEGS
// where the inventory has HANDS in the fourth position. One shared list put
// the chest piece on the neck in the first player capture, which is what this
// second list exists to stop.
VOXELEARTHUI_API const TArray<FText>& PlayerEquipSlotNames();
VOXELEARTHUI_API FText InvSearchHint();
// The "x16" badge in the corner of a stacked inventory cell.
VOXELEARTHUI_API FText ItemStackCount(int32 Count);
VOXELEARTHUI_API FText InvWeight(float CarriedKg);
VOXELEARTHUI_API FText InvNoEquipment();

// --- Map --------------------------------------------------------------------
VOXELEARTHUI_API FText MapSavedPlaces();
VOXELEARTHUI_API FText MapNoPlaces();
VOXELEARTHUI_API FText MapLabelPosition();
VOXELEARTHUI_API FText MapLabelGeo();
VOXELEARTHUI_API FText MapLabelAltitude();
VOXELEARTHUI_API FText MapLabelChunk();
VOXELEARTHUI_API FText MapLabelHeading();
VOXELEARTHUI_API FText MapLabelSeed();
// The action-bar line shown when no raster loaded: the sheet is blank because
// none exists for this seed, not because the map failed.
VOXELEARTHUI_API FText MapNoRaster();
// Shown when the raster IS drawn. Was "a fixed world overview"; since
// 2026-09-08 the sheet pans and zooms and carries the player's own marker, so
// the line names the gesture that is least discoverable instead.
VOXELEARTHUI_API FText MapOverviewNote();

// --- Live map: action hints, context menu, name field (2026-09-08) ----------
// TWO CAPS, and the keyboard alternative rides in the label rather than taking
// a cap of its own -- the action bar is one row in a 1060-unit shell.
VOXELEARTHUI_API FText MapActionZoom();     // WHEEL -> "zoom  -  or +/-"
VOXELEARTHUI_API FText MapActionPan();      // RMB   -> "drag to pan  -  click to mark"
VOXELEARTHUI_API FText MapKeyWheel();       // the "WHEEL" cap
VOXELEARTHUI_API FText MapKeyRightDrag();   // the "RMB" cap
// The right-click menu over empty sheet, and over an existing mark.
VOXELEARTHUI_API FText MapCtxPlaceTitle();
VOXELEARTHUI_API FText MapCtxMarkHere();
VOXELEARTHUI_API FText MapCtxRename();
VOXELEARTHUI_API FText MapCtxRemove();
VOXELEARTHUI_API FText MapCtxCancel();
// The inline name field: its title, its hint, and the two keys that end it.
VOXELEARTHUI_API FText MapNameTitle();
VOXELEARTHUI_API FText MapNameHint();
VOXELEARTHUI_API FText MapNameKeys();
// What a mark is called when the player confirms an empty field.
VOXELEARTHUI_API FText MapMarkDefaultName();
// The zoom readout under the compass: "2.4 km across" / "780 m across".
VOXELEARTHUI_API FText MapScaleValue(double MetresAcross);
VOXELEARTHUI_API FText MapPositionValue(const FVector& World);
VOXELEARTHUI_API FText MapGeoValue(double LatitudeDeg, double LongitudeDeg);
VOXELEARTHUI_API FText MapChunkValue(const FIntVector& Chunk);
VOXELEARTHUI_API FText MapHeadingValue(float Degrees);
VOXELEARTHUI_API FText MapSeedValue(uint64 Seed);
// N, E, S, W in that order -- the four letters on the compass rose.
VOXELEARTHUI_API const TArray<FText>& MapCompassLetters();

// --- Journal ----------------------------------------------------------------
VOXELEARTHUI_API FText JournalSectionEntries();
VOXELEARTHUI_API FText JournalSectionGoals();
VOXELEARTHUI_API FText JournalTracked();
VOXELEARTHUI_API FText JournalUntracked();
VOXELEARTHUI_API FText JournalEntriesTab();
VOXELEARTHUI_API FText JournalNewEntry();
VOXELEARTHUI_API FText JournalWriteHere();
VOXELEARTHUI_API FText JournalEmpty();
VOXELEARTHUI_API FText JournalNothingTracked();
VOXELEARTHUI_API FText JournalNothingUntracked();
VOXELEARTHUI_API FText JournalSteps();
VOXELEARTHUI_API FText JournalTrack();
VOXELEARTHUI_API FText JournalUntrack();
VOXELEARTHUI_API FText JournalStepProgress(int32 Done, int32 Total, const FText& Place);
VOXELEARTHUI_API FText JournalKindEntry();
VOXELEARTHUI_API FText JournalKindTracked();
VOXELEARTHUI_API FText JournalKindUntracked();
VOXELEARTHUI_API FText JournalCardStamp(int32 Day, const FText& Season);

// --- Player -----------------------------------------------------------------
VOXELEARTHUI_API FText PlayerSubStats();
VOXELEARTHUI_API FText PlayerSubSkills();
VOXELEARTHUI_API FText PlayerSubPerks();
VOXELEARTHUI_API FText PlayerSubReputation();
VOXELEARTHUI_API FText PlayerMainLevel();
VOXELEARTHUI_API FText PlayerXp();
VOXELEARTHUI_API FText PlayerXpValue(int32 Current, int32 Next);
VOXELEARTHUI_API FText PlayerSkillPoints();
VOXELEARTHUI_API FText PlayerDisciplines();
VOXELEARTHUI_API FText PlayerColumnName();
VOXELEARTHUI_API FText PlayerColumnLevel();
VOXELEARTHUI_API FText PlayerColumnStatus();
VOXELEARTHUI_API FText PlayerColumnFaction();
VOXELEARTHUI_API FText PlayerColumnStanding();
VOXELEARTHUI_API FText PlayerColumnValue();
VOXELEARTHUI_API FText PlayerStatusOwned();
VOXELEARTHUI_API FText PlayerStatusLocked();
VOXELEARTHUI_API FText PlayerRankLine(int32 From, int32 To);
VOXELEARTHUI_API FText PlayerUnlockCost(int32 Points);
VOXELEARTHUI_API FText PlayerRankBadge(int32 Rank, int32 MaxRank);
VOXELEARTHUI_API FText PlayerDisciplineTally(int32 Unlocked, int32 Total);

// --- Codex ------------------------------------------------------------------
VOXELEARTHUI_API FText CodexRecipes();
VOXELEARTHUI_API FText CodexPlaces();
VOXELEARTHUI_API FText CodexIngredients();
VOXELEARTHUI_API FText CodexMadeAt(const FText& Station);
VOXELEARTHUI_API FText CodexYield(const FText& Category, int32 Count);
VOXELEARTHUI_API FText CodexYieldLabel();
VOXELEARTHUI_API FText CodexSearchHint();
VOXELEARTHUI_API FText CodexNoRecipe();
VOXELEARTHUI_API FText CodexLocked();
VOXELEARTHUI_API FText CodexIngredientLine(int32 Needed, const FText& Name);
VOXELEARTHUI_API FText CodexHeldLine(int32 Held);
VOXELEARTHUI_API FText CodexHeldUnknown();

// --- The world stamp --------------------------------------------------------
//
// "Day 11 · Summer, 1st Year of the Second Age" -- ONE composer, used by the
// journal (its three placeholder cards and the composer card's date line) and by
// the death screen. Both mocks print the identical string from the identical
// helper (`stampOf` in Voxelmark Journal.html:117 and the literal in Voxelmark
// Death Screen.html:63), and before 2026-09-08 the port had it hard-coded three
// times in VoxelScreenData.cpp and not at all on the death card.
//
// THE DAY, THE SEASON AND THE YEAR ARE ALL REAL. Day is EpochSeconds over
// voxel.Sky.DayLengthSeconds; year is the same over a whole game year; season is
// VoxelSky::SeasonIndexFromDayOfYear, which reads FVoxelSkyState::DayOfYear --
// an ASTRONOMICAL 0..365 index that is correct whatever voxel.Sky.DaysPerYear
// is. The note that used to sit on PauseFooter said a season here would be
// "confidently wrong" because SeasonName assumes a 365-day year; that was a
// misreading of DayOfYear and it is retracted. See the header of
// SeasonIndexFromDayOfYear for the arithmetic.
//
// "OF THE SECOND AGE" IS THE ONE PART THAT IS NOT. There is no era anywhere in
// this project, and it is here because both mocks say it and because the two
// screens that print it are the ones still showing seeded placeholder content
// (VoxelScreenData.cpp's Seed* functions). It is a single edit in WorldStamp's
// format string the day an era exists or the fiction is dropped -- which is why
// the era is inside the composer and not appended by each caller.
//
// THE PAUSE FOOTER DELIBERATELY DOES NOT USE THIS. It says "Day N" and stops;
// it is a shipped screen the owner has judged, not a placeholder, and its own
// note argues the case. See PauseFooter.

// 0 spring, 1 summer, 2 autumn, 3 winter -- VoxelSky::SeasonIndexFromDayOfYear's
// own numbering. Title case, because this is the register the stamps print in;
// the F1 overlay keeps its own lower-case table for mid-sentence use. Any index
// outside 0..3 gives an empty text rather than a wrong season.
VOXELEARTHUI_API FText SeasonLabel(int32 SeasonIndex);

// 1 -> "1st", 12 -> "12th", 18 -> "18th", 21 -> "21st". Its own function because
// the 11/12/13 exception is the part everybody's inline version gets wrong, and
// because it is the only piece of this that a headless test can pin.
// UNGROUPED: a 1000th year must not print as "1,000th".
VOXELEARTHUI_API FText Ordinal(int32 Number);

// EMPTY WHEN DayNumber <= 0, which is how UVoxelScreensUISubsystem::DayStamp
// reports "this session cannot name a day" -- both callers already omit the
// stamp rather than drawing a blank one. An empty Season drops the season half
// and keeps the rest, so a world with no sky subsystem still dates its journal.
VOXELEARTHUI_API FText WorldStamp(int32 DayNumber, const FText& Season, int32 YearNumber);

// --- Death screen -----------------------------------------------------------
VOXELEARTHUI_API FText DeathTitle();      // "YOU DIED"
VOXELEARTHUI_API FText DeathRespawn();
VOXELEARTHUI_API FText DeathQuit();
// The mock's generic pool. Its ten per-cause pools are NOT ported: this game
// has no damage system, so it can never name a cause, and ten pools that can
// only ever be unreachable would be ten lists nobody maintains. Adding them
// back is one array each, beside this one, the day something can kill a player.
VOXELEARTHUI_API const TArray<FText>& DeathQuips();

// --- Dialogue ---------------------------------------------------------------
VOXELEARTHUI_API FText DlgBandEasy();
VOXELEARTHUI_API FText DlgBandMedium();
VOXELEARTHUI_API FText DlgBandHard();
VOXELEARTHUI_API FText DlgActionSelect();

// --- HUD --------------------------------------------------------------------
// The interaction prompt the HUD mock shows. CAPTURE-ONLY: there is no
// interaction system and no examinable actor, so this is drawn under
// -VoxelDemoVitals and never in play. The mock's own line, verbatim.
VOXELEARTHUI_API FText HudDemoInteract();
VOXELEARTHUI_API FText HudDemoInteractKey();

// The music cluster's three tooltips, and the key each is also bound to.
//
// THE KEY IS IN THE TOOLTIP ON PURPOSE. The cluster is only clickable while a
// cursor is up, and the cursor is only up over a menu -- so the moment a player
// can read the tooltip is precisely the moment they most need to be told which
// key does the same thing while they are playing. See UVoxelScreensUISubsystem
// for the bindings these name; the two must be changed together.
VOXELEARTHUI_API FText HudMusicPrevTip();
VOXELEARTHUI_API FText HudMusicPlayTip();
VOXELEARTHUI_API FText HudMusicPauseTip();
VOXELEARTHUI_API FText HudMusicNextTip();

// The two INTERFACE rows that hide HUD furniture. Owner directive, 2026-09-08.
// Each hint says the thing a player would otherwise have to discover: hiding the
// transport does not stop the music, and the compass row takes only the compass.
VOXELEARTHUI_API FText SettingsHideMusicUILabel();
VOXELEARTHUI_API FText SettingsHideMusicUIHint();
VOXELEARTHUI_API FText SettingsHideCompassUILabel();
VOXELEARTHUI_API FText SettingsHideCompassUIHint();
} // namespace VoxelUIStrings
