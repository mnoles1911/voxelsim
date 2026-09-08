#pragma once
// The data every 2026-09-07 in-game screen draws, as one plain struct per
// screen, plus the placeholder each is seeded with until gameplay owns it.
//
// WHY THESE EXIST AT ALL. Of the eight screens in wave 2, exactly one has a
// backing system: INVENTORY reads UVoxelInventoryComponent and FVoxelItemDef,
// which are real, replicated and already used by the hotbar. The other seven
// draw things this project has never had -- there is no quest system, no
// journal, no codex, no skill tree, no perk or faction table, no health, no
// hunger, no death, and no conversation system (design/CONVERSATION_SYSTEM.md,
// which the dialogue CSS names as its spec, does not exist in this repository
// and never has). A grep for Quest, Journal, Codex, SkillTree, Respawn,
// TakeDamage and OnDeath across both modules on 2026-09-07 returned nothing.
//
// So the choice was between not building those screens at all and building them
// against an interface. This is that interface: each screen takes ONE struct,
// never a subsystem pointer, and the widget cannot tell a seeded placeholder
// from a real one. When a quest system arrives, the work is to fill
// FVoxelJournalData from it -- not to touch SVoxelJournalScreen.
//
// PLAIN STRUCTS, NOT USTRUCTS, and FText rather than FString for anything a
// player reads. Plain because nothing here is replicated, serialised or exposed
// to Blueprint yet, and a USTRUCT would imply all three; FText because these
// strings go straight into STextBlock and the existing widgets in this module
// take FText (VoxelUIStrings is the whole file of them). The one exception is
// FVoxelInventoryScreenItem::ItemId, which is an FName because that is what the
// real inventory keys on.
//
// EVERY Seed*() FUNCTION IS PLACEHOLDER COPY FROM THE MOCK, and carries the
// same disclaimer VoxelUIStrings.h opens with: it is fiction from a different
// game (Roland, the Second Age, the Aelorin, factions this project has no
// concept of). It lives behind a function whose name says "seed" so that
// deleting it is one edit and so that no widget hardcodes a word.

#include "CoreMinimal.h"
#include "Internationalization/Text.h"

// --- Shared vocabulary ------------------------------------------------------

// The five in-game screens the tab bar switches between, in the mock's tab
// order (which is NOT the order the brief lists them in -- the mock puts MAP
// first and the port follows the screen).
enum class EVoxelScreenTab : uint8
{
	Map,
	Journal,
	Inventory,
	Player,
	Codex,
};

// Rarity is the mock's, not the game's: FVoxelItemDef has no rarity field, so
// every real item resolves to Common and the four brighter rings are only ever
// seen on seeded rows. Kept because the ring is drawn by the shared slot
// chrome, and a slot that could not express rarity would have to be rewritten
// the day items gain it.
enum class EVoxelItemRarity : uint8
{
	Common,
	Uncommon,
	Rare,
	Epic,
	Legendary,
};

// --- Inventory --------------------------------------------------------------

// One pack or hotbar cell. Count 0 means the cell is empty; ItemId is cleared
// at the same time, on FVoxelInventorySlot's own rule.
struct FVoxelInventoryScreenItem
{
	FName ItemId;
	FText DisplayName;
	int32 Count = 0;
	// Which .it glyph the mock draws. The real registry has no icon field, so
	// this is derived from FVoxelItemDef::Category and BlockMaterialId by
	// VoxelScreenData::GlyphForItem -- a block reads as stone, a throwable as a
	// cube, and everything else falls back to the misc lozenge.
	FName Glyph;
	EVoxelItemRarity Rarity = EVoxelItemRarity::Common;
	// Negative means "this item has no durability", which is every real item:
	// nothing in FVoxelItemDef wears out. A non-negative value is a 0..1 bar.
	float Durability = -1.f;
	// The mock's category filter chips. No real item carries one; every real
	// row lands in Materials or Misc.
	FText Category;

	bool IsEmpty() const { return Count <= 0 || ItemId.IsNone(); }
};

struct FVoxelInventoryScreenData
{
	TArray<FVoxelInventoryScreenItem> Pack;
	TArray<FVoxelInventoryScreenItem> Hotbar;
	int32 SelectedHotbarSlot = 0;
	// The mock's "112 / 180" weight readout. FVoxelItemDef DOES carry MassKg,
	// so the numerator is real; the capacity is not -- this project has no
	// carry limit, so CapacityKg is 0 and the widget draws the mass alone.
	float CarriedKg = 0.f;
	float CapacityKg = 0.f;
	// CHARACTER mode's paperdoll. Empty until equipment exists; the widget
	// draws the eight labelled empty sockets rather than hiding the mode, so
	// the gap is visible rather than silently absent.
	TArray<FVoxelInventoryScreenItem> Equipment;
	bool bHasEquipmentSystem = false;
};

// --- Map --------------------------------------------------------------------

// One player-placed mark. The mock persists these to localStorage under
// `voxelmark.map.marks.v1` and the Codex "Places" list reads the same record.
//
// THESE NOW SURVIVE THE SESSION (2026-09-08, owner directive: "save the place
// for future reference on the map"). They are written through to
// Saved/VoxelWorlds/<seed>.vxmarks.json on every add, rename and remove -- see
// VoxelMapMarks.h for why that file and not the checkpoint system. The
// subsystem still owns the live array so the map and the Codex's PLACES list
// cannot disagree; the file is where that array comes from and goes to.
struct FVoxelMapMark
{
	FText Name;
	// One of the mock's eighteen ICONS keys. NOTHING SETS IT YET: the port
	// draws one geometry glyph for every mark (Slate has no vector brush and
	// eighteen hand-built polygons is a different piece of work), so this is
	// carried through the store unchanged and is what an icon picker would
	// write into.
	FName Icon;
	// World position in Unreal units, not sheet pixels: the mock's sheet
	// coordinates are meaningless outside its own 4200x2800 image.
	FVector2D WorldXY = FVector2D::ZeroVector;
	int32 DayPlaced = 0;
	// Unix seconds when the player made the mark, or 0 for a record written
	// before this field existed. WALL TIME, NOT GAME TIME: DayPlaced above is
	// the in-game day and answers "when in the story"; this answers "which of
	// my marks is the newest", which is the one a list has to sort by and which
	// an in-game clock that resets cannot.
	int64 CreatedUnixTime = 0;
};

// The two values the map's live player marker reads every frame.
//
// A SECOND, TINY STRUCT RATHER THAN RE-GATHERING FVoxelMapScreenData. The
// screen data is gathered once when the tab opens -- it queries the sky
// subsystem, the terrain height and the filesystem, none of which belong on a
// per-frame path. The marker needs two numbers, so two numbers are what the
// per-frame attribute carries. Same rule as everything else in this file: the
// widget takes plain data and never a subsystem pointer.
struct FVoxelMapPose
{
	// Unreal units. X is East, Y is North -- the same convention
	// VoxelUIStrings::MapPositionValue prints.
	FVector2D WorldXY = FVector2D::ZeroVector;
	// Yaw in degrees, 0 = +X = East, increasing towards +Y = North, which is
	// what FRotator::Yaw already is.
	float YawDeg = 0.f;
};

struct FVoxelMapScreenData
{
	// All real, and all already computed every frame by UVoxelSkySubsystem and
	// VoxelCoords -- see SVoxelMapScreen for the derivation.
	FVector PlayerWorld = FVector::ZeroVector;
	double LatitudeDeg = 0.0;
	double LongitudeDeg = 0.0;
	float HeadingDeg = 0.f;
	double SurfaceHeightUU = 0.0;
	int32 DayNumber = 0;
	// Voxel and chunk coordinates of the player, for the readout's second and
	// third lines.
	FIntVector VoxelCoord = FIntVector::ZeroValue;
	FIntVector ChunkKey = FIntVector::ZeroValue;
	uint64 Seed = 0;
	TArray<FVoxelMapMark> Marks;
	// True once the hillshade raster is on screen. False draws the flat
	// parchment with the readout and says so in the action bar, rather than
	// implying a map that is not there.
	bool bHasTerrainRaster = false;
	// Absolute path of the hillshade, resolved ONCE when the tab opens (see
	// SVoxelMapScreen::RasterPathForSeed -- it reads the run's coarse tile
	// directory, which is not something to do per paint). Empty when this run's
	// world has no map.
	FString RasterPath;
	// The world rectangle the hillshade covers, in Unreal units, and the
	// rectangle the view clamps itself to whether or not a raster exists. Both
	// come from SVoxelMapScreen::ResolveExtentUU.
	FVector2D ExtentMinUU = FVector2D::ZeroVector;
	FVector2D ExtentMaxUU = FVector2D::ZeroVector;
};

// --- Journal ----------------------------------------------------------------

struct FVoxelJournalEntry
{
	FText Title;
	FText Body;
	int32 Day = 0;
	FText Season;
	FText Stamp; // the fully formatted "Day 12 - Summer, 18th Year..." line
};

// One step of a goal. The mock's three states are done / now / pending, and
// "now" is the single step the goal is currently waiting on.
enum class EVoxelGoalStepState : uint8
{
	Pending,
	Current,
	Done,
};

struct FVoxelGoalStep
{
	FText Text;
	EVoxelGoalStepState State = EVoxelGoalStepState::Pending;
};

struct FVoxelGoal
{
	FText Name;
	FText Source;
	FText Place;
	FText Flavour;
	TArray<FVoxelGoalStep> Steps;
	bool bTracked = true;
};

struct FVoxelJournalData
{
	TArray<FVoxelJournalEntry> Entries;
	TArray<FVoxelGoal> Goals;
	// What a new entry would be stamped with. Real when there is a sky
	// subsystem; empty in a capture run, which hides the composer's stamp
	// rather than dating an entry to a day the session cannot name.
	FText TodayStamp;
};

// --- Player -----------------------------------------------------------------

struct FVoxelPlayerAttribute
{
	FText Name;
	int32 Value = 0;
	// The .attr.featured row -- one attribute per block reads gold.
	bool bFeatured = false;
	// Which .g-* glyph the mock draws beside it.
	FName Glyph;
};

struct FVoxelPlayerStatBlock
{
	FText Title;
	FText Meta; // the right-hand caption ("2 unspent points", "summer, day 12")
	TArray<FVoxelPlayerAttribute> Attributes;
};

struct FVoxelSkillNode
{
	FText Name;
	int32 Rank = 0;
	int32 MaxRank = 3;
	// Tier is the row the node sits on. The mock positions nodes at explicit
	// percentages inside an SVG; the port lays them out by tier instead -- see
	// SVoxelPlayerScreen for why the connector curves are not drawn.
	int32 Tier = 0;
	bool bUnlocked = false;
	bool bAvailable = false;
	FText Flavour;
	FText RankEffect;
	FText Requires;
	int32 Cost = 1;
};

struct FVoxelSkillDiscipline
{
	FText Name;
	int32 Unlocked = 0;
	int32 Total = 0;
	TArray<FVoxelSkillNode> Nodes;
};

struct FVoxelPerk
{
	FText Name;
	FText Group; // the .list-divider this perk sits under
	int32 Level = 0;
	bool bOwned = false;
	bool bLocked = false;
	FText Requirement;
	FText Description;
	FText Flavour;
};

struct FVoxelFactionStanding
{
	FText Name;
	// -100..+100, the mock's own range.
	int32 Standing = 0;
};

struct FVoxelPlayerData
{
	FText Name;
	FText Epithet; // "WAYWARD HEIR - ARMOUR 42"
	int32 Level = 0;
	int32 Xp = 0;
	int32 XpForNextLevel = 0;
	int32 SkillPoints = 0;
	TArray<FVoxelPlayerStatBlock> Blocks;
	TArray<FVoxelInventoryScreenItem> Equipment;
	TArray<FVoxelSkillDiscipline> Disciplines;
	TArray<FVoxelPerk> Perks;
	TArray<FVoxelFactionStanding> Factions;
};

// --- Codex ------------------------------------------------------------------

struct FVoxelRecipeIngredient
{
	FText Name;
	FName Glyph;
	int32 Needed = 0;
	// How many the player actually holds. Real: counted out of the live
	// inventory by name where a recipe ingredient matches a registry item, and
	// -1 where it does not (every seeded recipe, since none of their
	// ingredients exist as items yet).
	int32 Held = -1;
};

struct FVoxelRecipe
{
	FText Name;
	FText Category;
	FText Description;
	FText Station;
	FName Glyph;
	int32 Yield = 1;
	// Nine cells, row-major. An entry with an empty Glyph is a blank cell.
	TArray<FVoxelRecipeIngredient> Pattern;
	TArray<FVoxelRecipeIngredient> Ingredients;
};

// A codex page. The mock's three lore categories (People / Factions / Lore) all
// have the same shape, so they share one struct and differ only in which list
// they are filed under.
struct FVoxelCodexEntry
{
	FText Name;
	FText Subtitle;
	// The two-column fact table down the right of the page.
	TArray<TPair<FText, FText>> Facts;
	TArray<FText> Paragraphs;
	FText Quote;
	// The mock's three read states: locked entries show as "???" and have no
	// page at all.
	bool bLocked = false;
	bool bUnread = false;
};

struct FVoxelCodexCategory
{
	FText Name;
	FText Kind; // the .p-kind stamp: PERSON / FACTION / LORE
	TArray<FVoxelCodexEntry> Entries;
};

struct FVoxelCodexData
{
	TArray<FVoxelRecipe> Recipes;
	TArray<FVoxelCodexCategory> Categories;
	// PLACES is not seeded: it is the map's own mark list, rendered as a codex
	// category so the two cannot disagree about what the player has named.
};

// --- Dialogue ---------------------------------------------------------------

enum class EVoxelSkillCheckBand : uint8
{
	None,
	Easy,
	Medium,
	Hard,
};

struct FVoxelDialogueOption
{
	FText Text;
	// The 1..9 key that commits this option. The mock hard-codes one per row.
	FText Key;
	EVoxelSkillCheckBand Band = EVoxelSkillCheckBand::None;
	// Which .sk-* glyph precedes the row, and which player score the check
	// reads. Empty means an ordinary reply with no check.
	FName SkillGlyph;
	FName SkillId;
	int32 Threshold = 0;
	// VISIBLE BUT GREYED is the mock's stated pattern ("SpeechCheckBroker won't
	// let Roland succeed below threshold"), so a failing option is drawn dim
	// and unselectable rather than hidden.
	bool bAvailable = true;
};

struct FVoxelDialogueSkill
{
	FText Name;
	FName Glyph;
	int32 Value = 0;
};

struct FVoxelDialogueData
{
	FText SpeakerName;
	FText SpeakerRole;
	FText SpeakerLine;
	TArray<FVoxelDialogueOption> Options;
	// The top strip is drawn only when at least one option carries a check,
	// which is the mock's own rule.
	TArray<FVoxelDialogueSkill> Skills;
	// The companion observation, bottom-left. Empty name hides the block.
	FText CompanionName;
	FText CompanionLine;
	// Seconds remaining on a timed reply, or <= 0 for an untimed node.
	float TimerSeconds = 0.f;
	float TimerTotalSeconds = 0.f;
};

// --- HUD --------------------------------------------------------------------

// Everything SVoxelGameHud draws, refreshed from the world each frame.
//
// IT LIVES HERE RATHER THAN BESIDE ITS WIDGET so that
// UVoxelScreensUISubsystem.h -- which is UHT-parsed -- can name it without
// including a Slate header. VoxelPauseUISubsystem.h keeps the same discipline,
// forward-declaring SVoxelPauseMenu and including nothing from Slate.
struct FVoxelHudData
{
	float HeadingDeg = 0.f;
	TArray<FVoxelInventoryScreenItem> Hotbar;
	int32 SelectedSlot = 0;

	// NOTHING SETS THESE YET. There is no health, hunger, stamina or damage in
	// this project, and no interaction system -- see SVoxelGameHud's header for
	// why the bars are gated rather than drawn full.
	bool bHasVitals = false;
	float HealthFraction = 1.f;
	float WoundFraction = 0.f;
	float HungerFraction = 1.f;
	FText InteractPrompt;
	FText InteractKey;

	// Whether the player controller is currently showing a mouse cursor.
	//
	// THE ONE THING THE HUD CANNOT WORK OUT FOR ITSELF. Every other field here
	// is world state; this is input state, and the HUD is a Slate widget with no
	// route to a player controller. It exists because the music cluster is the
	// only part of the HUD that is ever allowed to take a click, and it may only
	// do so when there is a pointer to take it with -- a hit-testable button
	// under a captured mouse would eat a dig. See SVoxelGameHud::BuildMusic.
	bool bCursorVisible = false;
};

// --- Death ------------------------------------------------------------------

struct FVoxelDeathData
{
	// The cause-of-death quip. The mock picks one at random from a per-cause
	// pool, falling back to a generic pool when the cause is unknown; the pools
	// live in VoxelUIStrings with every other string.
	FText Quip;
	// "DAY 12 - SUMMER, 18TH YEAR OF THE SECOND AGE". Empty hides the stamp
	// rather than dating a death the session cannot date.
	FText Stamp;
};

// The calendar the placeholder journal is dated in. Passed in rather than read,
// so SeedJournal stays a pure function of its arguments and can be tested
// headlessly (VoxelFrontEndTests.cpp) with no world and no sky subsystem.
//
// Day 0 means "this session cannot name a day", exactly as
// UVoxelScreensUISubsystem::DayStamp uses it, and the fixture then falls back to
// the mock's own 12 / Summer / 18th year.
struct FVoxelSeedCalendar
{
	int32 Day = 0;
	int32 Year = 0;
	FText Season;
};
// Declared OUTSIDE the namespace: every caller (the subsystem, the tests)
// names it bare, and the 2026-09-08 build broke on exactly that.

namespace VoxelScreenData
{
// Which .it glyph an item draws with, derived from what the registry actually
// carries. See FVoxelInventoryScreenItem::Glyph.
VOXELEARTHUI_API FName GlyphForItem(FName ItemId);

// The placeholder content. Each is the mock's own data, verbatim, and each is
// what the screen shows until a real system replaces the call.
//
// SeedJournal TAKES THE LIVE CALENDAR because its newest entry used to be dated
// day 12 unconditionally -- the mock's number -- while the live world was on day
// 11, so the placeholder journal contained an entry from the future. The three
// cards keep the mock's SPACING (newest, newest-3, day 1); only the anchor
// moves. See the .cpp.
VOXELEARTHUI_API FVoxelJournalData SeedJournal(const FText& TodayStamp,
                                               const FVoxelSeedCalendar& Calendar);
VOXELEARTHUI_API FVoxelPlayerData SeedPlayer();
VOXELEARTHUI_API FVoxelCodexData SeedCodex();
VOXELEARTHUI_API FVoxelDialogueData SeedDialogue();
// Stamp is UVoxelScreensUISubsystem::DayStamp(), i.e.
// VoxelUIStrings::WorldStamp of the live day, season and year. It used to be
// "Day 11" and nothing more, which is why FVoxelDeathData::Stamp's own comment
// above described a string the port did not actually produce.
VOXELEARTHUI_API FVoxelDeathData SeedDeath(const FText& Stamp);
} // namespace VoxelScreenData
