#include "VoxelScreenData.h"

#include "VoxelUIStrings.h"

#include "VoxelItem.h"

#define LOCTEXT_NAMESPACE "VoxelEarthUI"

// THE SEED CONTENT LIVES HERE, NOT IN VoxelUIStrings.
//
// VoxelUIStrings.h says it is "every word the front end puts on screen", and
// this file breaks that rule on purpose. The difference is lifetime: the strings
// in that file are the front end's permanent vocabulary -- button words, column
// headings, the loading tips -- and they will be re-authored in place. Every
// word below is SCAFFOLDING. It is the mock's own fiction, standing in for a
// quest system, a codex, a skill tree, a perk table and a faction table that do
// not exist, and the day any of those ships the right change is to delete the
// corresponding Seed function whole and never look at its text again.
//
// Mixing the two would make that deletion a hunt through a 400-line string
// table for which entries were placeholders. Keeping it here means the seeds
// are exactly the things inside functions called Seed*, in the file whose header
// explains why they exist.
//
// Same disclaimer as VoxelUIStrings' own: this is fiction from a different game.
// Roland, the Second Age, the Ashfallen, Bregga and the factions below are the
// design project's invention, ported verbatim so the screens can be reviewed
// against the mocks rather than against blank panels.

namespace VoxelScreenData
{
FName GlyphForItem(FName ItemId)
{
	// DERIVED, NOT STORED. FVoxelItemDef has no icon field of any kind, so the
	// glyph is inferred from the identity the registry does carry. The block
	// list is the fourteen materials in VoxelItem.cpp's table; anything the
	// registry does not know resolves to the misc lozenge in
	// VoxelScreenChrome::BandsFor.
	static const TMap<FName, FName> kByItem = {
		{TEXT("block.rock"),          TEXT("stone")},
		{TEXT("block.bedrock"),       TEXT("stone")},
		{TEXT("block.gravel"),        TEXT("cobble")},
		{TEXT("block.sand"),          TEXT("clay")},
		{TEXT("block.clay"),          TEXT("clay")},
		{TEXT("block.mud"),           TEXT("clay")},
		{TEXT("block.soil"),          TEXT("leather")},
		{TEXT("block.subsoil"),       TEXT("leather")},
		{TEXT("block.junglesoil"),    TEXT("leather")},
		{TEXT("block.podzol"),        TEXT("leather")},
		{TEXT("block.permafrost"),    TEXT("glass")},
		{TEXT("block.snow"),          TEXT("wool")},
		{TEXT("block.grass"),         TEXT("log")},
		{TEXT("block.savannagrass"),  TEXT("log")},
		{TEXT("throwable.voxelcube30"), TEXT("stone")},
	};
	if (const FName* Found = kByItem.Find(ItemId))
	{
		return *Found;
	}
	// Fall back on the category, so an item added to the registry without a
	// line above still draws as something rather than as the default lozenge.
	if (const FVoxelItemDef* Def = FVoxelItemRegistry::Find(ItemId))
	{
		switch (Def->Category)
		{
		case EVoxelItemCategory::Block:     return TEXT("stone");
		case EVoxelItemCategory::Tool:      return TEXT("pick");
		case EVoxelItemCategory::Throwable: return TEXT("stone");
		case EVoxelItemCategory::Misc:      break;
		}
	}
	return NAME_None;
}

FVoxelJournalData SeedJournal(const FText& TodayStamp)
{
	FVoxelJournalData Data;
	Data.TodayStamp = TodayStamp;

	auto Entry = [](const FText& Title, const FText& Body, int32 Day, const FText& Season, const FText& Stamp)
	{
		FVoxelJournalEntry E;
		E.Title = Title; E.Body = Body; E.Day = Day; E.Season = Season; E.Stamp = Stamp;
		return E;
	};
	const FText Summer = LOCTEXT("SeedSummer", "Summer");
	Data.Entries.Add(Entry(
		LOCTEXT("SeedE3T", "The wolves came back"),
		LOCTEXT("SeedE3B", "Three of them at the treeline before dusk. They did not come closer than the fire, but they did not leave either.\n\nI have started a stone wall on the north side. Two courses laid. It will not stop anything yet but it makes me feel better to have put something between us and the dark."),
		12, Summer, LOCTEXT("SeedE3S", "Day 12 · Summer, 18th Year of the Second Age")));
	Data.Entries.Add(Entry(
		LOCTEXT("SeedE2T", "Copper in the cliff face"),
		LOCTEXT("SeedE2B", "Found a green streak in the rock above the creek. Chipped out eleven lumps before the pick gave out.\n\nIf I can find tin I can make bronze, and then I can stop breaking a tool every second day."),
		9, Summer, LOCTEXT("SeedE2S", "Day 9 · Summer, 18th Year of the Second Age")));
	Data.Entries.Add(Entry(
		LOCTEXT("SeedE1T", "First night"),
		LOCTEXT("SeedE1B", "No shelter, no fire, nothing but what I could carry. Slept in a hole I dug into the hillside with my hands.\n\nIt rained. Of course it rained."),
		1, Summer, LOCTEXT("SeedE1S", "Day 1 · Summer, 18th Year of the Second Age")));

	auto Step = [](const FText& Text, EVoxelGoalStepState State)
	{
		FVoxelGoalStep S; S.Text = Text; S.State = State; return S;
	};
	const auto Done = EVoxelGoalStepState::Done;
	const auto Now = EVoxelGoalStepState::Current;
	const auto Todo = EVoxelGoalStepState::Pending;

	FVoxelGoal G1;
	G1.Name = LOCTEXT("SeedG1N", "Survive the first winter");
	G1.Source = LOCTEXT("SeedG1S", "World");
	G1.Place = LOCTEXT("SeedG1P", "Anywhere");
	G1.Flavour = LOCTEXT("SeedG1F", "The cold does not negotiate. Food stores, a sealed roof and a fuel pile are the whole of the argument.");
	G1.Steps = {Step(LOCTEXT("SeedG1a", "Build a shelter with a door"), Done),
	            Step(LOCTEXT("SeedG1b", "Stockpile 40 units of food"), Now),
	            Step(LOCTEXT("SeedG1c", "Cut 200 logs for fuel"), Todo),
	            Step(LOCTEXT("SeedG1d", "Survive to the first thaw"), Todo)};
	Data.Goals.Add(G1);

	FVoxelGoal G2;
	G2.Name = LOCTEXT("SeedG2N", "Smelt your first bronze");
	G2.Source = LOCTEXT("SeedG2S", "Metalsmithing");
	G2.Place = LOCTEXT("SeedG2P", "Any bloomery");
	G2.Flavour = LOCTEXT("SeedG2F", "Copper is soft and tin is scarce. Together they hold an edge long enough to be worth the walk.");
	G2.Steps = {Step(LOCTEXT("SeedG2a", "Mine 12 copper ore"), Done),
	            Step(LOCTEXT("SeedG2b", "Find a tin vein"), Now),
	            Step(LOCTEXT("SeedG2c", "Build a bloomery"), Todo),
	            Step(LOCTEXT("SeedG2d", "Pour a bronze ingot"), Todo)};
	Data.Goals.Add(G2);

	FVoxelGoal G3;
	G3.Name = LOCTEXT("SeedG3N", "Clear the wolf den");
	G3.Source = LOCTEXT("SeedG3S", "Threat");
	G3.Place = LOCTEXT("SeedG3P", "North ridge");
	G3.Flavour = LOCTEXT("SeedG3F", "The pack has a hole in the ridge they return to at dawn. Whatever is in there, they are feeding it.");
	G3.Steps = {Step(LOCTEXT("SeedG3a", "Track the pack to the ridge"), Done),
	            Step(LOCTEXT("SeedG3b", "Destroy the den"), Now)};
	Data.Goals.Add(G3);

	FVoxelGoal G4;
	G4.Name = LOCTEXT("SeedG4N", "Raise a stone wall around camp");
	G4.Source = LOCTEXT("SeedG4S", "Building");
	G4.Place = LOCTEXT("SeedG4P", "Camp");
	G4.Flavour = LOCTEXT("SeedG4F", "Wood burns and rots. Stone does neither, and there is a great deal of it under the hill.");
	G4.Steps = {Step(LOCTEXT("SeedG4a", "Quarry 120 stone"), Now),
	            Step(LOCTEXT("SeedG4b", "Enclose the camp perimeter"), Todo),
	            Step(LOCTEXT("SeedG4c", "Fit a gate"), Todo)};
	Data.Goals.Add(G4);

	FVoxelGoal G5;
	G5.Name = LOCTEXT("SeedG5N", "Map the cave system");
	G5.Source = LOCTEXT("SeedG5S", "Exploration");
	G5.Place = LOCTEXT("SeedG5P", "Hollow under the creek");
	G5.Flavour = LOCTEXT("SeedG5F", "The draught coming out of the creek mouth is cold and steady, which means it goes a long way back.");
	G5.bTracked = false;
	G5.Steps = {Step(LOCTEXT("SeedG5a", "Enter the cave mouth"), Done),
	            Step(LOCTEXT("SeedG5b", "Reach the second chamber"), Todo),
	            Step(LOCTEXT("SeedG5c", "Find where the draught comes from"), Todo)};
	Data.Goals.Add(G5);

	FVoxelGoal G6;
	G6.Name = LOCTEXT("SeedG6N", "Tame a pack animal");
	G6.Source = LOCTEXT("SeedG6S", "Husbandry");
	G6.Place = LOCTEXT("SeedG6P", "Open moor");
	G6.Flavour = LOCTEXT("SeedG6F", "Carrying ore up a hill on your back is a decision, not a fate.");
	G6.bTracked = false;
	G6.Steps = {Step(LOCTEXT("SeedG6a", "Craft a rope halter"), Todo),
	            Step(LOCTEXT("SeedG6b", "Feed a wild ox three times"), Todo),
	            Step(LOCTEXT("SeedG6c", "Lead it back to camp"), Todo)};
	Data.Goals.Add(G6);

	return Data;
}

FVoxelPlayerData SeedPlayer()
{
	FVoxelPlayerData Data;
	Data.Name = LOCTEXT("SeedPlName", "ROLAND");
	Data.Epithet = LOCTEXT("SeedPlEpithet", "WAYWARD HEIR · ARMOUR 42");
	Data.Level = 6;
	Data.Xp = 2140;
	Data.XpForNextLevel = 5080;
	Data.SkillPoints = 3;

	auto Attr = [](const FText& Name, int32 Value, FName Glyph, bool bFeatured = false)
	{
		FVoxelPlayerAttribute A;
		A.Name = Name; A.Value = Value; A.Glyph = Glyph; A.bFeatured = bFeatured;
		return A;
	};

	FVoxelPlayerStatBlock Attributes;
	Attributes.Title = LOCTEXT("SeedPlAttrs", "ATTRIBUTES");
	Attributes.Meta = LOCTEXT("SeedPlAttrsMeta", "2 unspent points");
	Attributes.Attributes = {
		Attr(LOCTEXT("SeedAtSpeech", "Speech"), 6, TEXT("speech"), true),
		Attr(LOCTEXT("SeedAtCharisma", "Charisma"), 2, TEXT("charisma")),
		Attr(LOCTEXT("SeedAtSpeed", "Speed"), 22, TEXT("speed")),
		Attr(LOCTEXT("SeedAtStrength", "Strength"), 5, TEXT("strength")),
		Attr(LOCTEXT("SeedAtAgility", "Agility"), 6, TEXT("agility")),
		Attr(LOCTEXT("SeedAtVitality", "Vitality"), 6, TEXT("vitality")),
	};
	Data.Blocks.Add(Attributes);

	FVoxelPlayerStatBlock Stealth;
	Stealth.Title = LOCTEXT("SeedPlStealth", "STEALTH & PRESENCE");
	Stealth.Attributes = {
		Attr(LOCTEXT("SeedAtConsp", "Conspicuous"), 55, TEXT("consp")),
		Attr(LOCTEXT("SeedAtNoise", "Noise"), 25, TEXT("noise")),
		Attr(LOCTEXT("SeedAtVis", "Visibility"), 61, TEXT("vis")),
	};
	Data.Blocks.Add(Stealth);

	// THE COMBAT MATRIX, FLATTENED INTO SIX ROWS. The mock draws it as a
	// four-column table -- a blank corner, then SLASH / STAB / BLUNT headers
	// over an Armour row and a Weapon row. That is a different widget from the
	// three-across attribute grid every other block uses, and the information
	// survives the flattening intact: naming each cell "Armour · slash" says
	// the same thing the header row said. Building a second grid shape for six
	// numbers was not worth it; if the matrix earns its own widget later, the
	// data is already separated from the drawing.
	FVoxelPlayerStatBlock Combat;
	Combat.Title = LOCTEXT("SeedPlCombat", "COMBAT");
	Combat.Attributes = {
		Attr(LOCTEXT("SeedAtArmSlash", "Armour · slash"), 21, TEXT("consp")),
		Attr(LOCTEXT("SeedAtArmStab", "Armour · stab"), 34, TEXT("consp")),
		Attr(LOCTEXT("SeedAtArmBlunt", "Armour · blunt"), 35, TEXT("consp")),
		Attr(LOCTEXT("SeedAtWpnSlash", "Weapon · slash"), 28, TEXT("strength")),
		Attr(LOCTEXT("SeedAtWpnStab", "Weapon · stab"), 30, TEXT("strength")),
		Attr(LOCTEXT("SeedAtWpnBlunt", "Weapon · blunt"), 12, TEXT("strength")),
	};
	Data.Blocks.Add(Combat);

	FVoxelPlayerStatBlock Vitals;
	Vitals.Title = LOCTEXT("SeedPlVitals", "VITALS & EXPOSURE");
	Vitals.Meta = LOCTEXT("SeedPlVitalsMeta", "summer, day 12");
	Vitals.Attributes = {
		Attr(LOCTEXT("SeedAtHealth", "Health"), 69, TEXT("strength")),
		Attr(LOCTEXT("SeedAtEnergy", "Energy"), 47, TEXT("charisma")),
		Attr(LOCTEXT("SeedAtFood", "Food"), 26, TEXT("vitality")),
		Attr(LOCTEXT("SeedAtWarmth", "Warmth"), 74, TEXT("vitality")),
		Attr(LOCTEXT("SeedAtRest", "Rest"), 52, TEXT("agility")),
		Attr(LOCTEXT("SeedAtCarry", "Carry"), 112, TEXT("speed")),
	};
	Data.Blocks.Add(Vitals);

	auto Equip = [](const FText& Name, FName Glyph, EVoxelItemRarity Rarity)
	{
		FVoxelInventoryScreenItem I;
		I.DisplayName = Name; I.Glyph = Glyph; I.Rarity = Rarity;
		I.ItemId = TEXT("seed.equipment"); I.Count = 1;
		return I;
	};
	Data.Equipment = {
		Equip(LOCTEXT("SeedEqHelm", "Iron Helm"), TEXT("helm"), EVoxelItemRarity::Uncommon),
		Equip(LOCTEXT("SeedEqArmor", "Chain Hauberk"), TEXT("armor"), EVoxelItemRarity::Rare),
		Equip(LOCTEXT("SeedEqGloves", "Leather Gloves"), TEXT("leather"), EVoxelItemRarity::Common),
		Equip(LOCTEXT("SeedEqLegs", "Padded Hose"), TEXT("wool"), EVoxelItemRarity::Common),
		Equip(LOCTEXT("SeedEqSword", "Iron Longsword"), TEXT("sword"), EVoxelItemRarity::Common),
		Equip(LOCTEXT("SeedEqShield", "Oaken Shield"), TEXT("shield-i"), EVoxelItemRarity::Uncommon),
		Equip(LOCTEXT("SeedEqRing", "Ring of Vigor"), TEXT("ring"), EVoxelItemRarity::Rare),
		Equip(LOCTEXT("SeedEqBoots", "Leather Boots"), TEXT("boots"), EVoxelItemRarity::Common),
	};

	auto Node = [](const FText& Name, int32 Rank, int32 Tier, bool bUnlocked, bool bAvailable,
	               const FText& Flavour, const FText& Effect, const FText& Requires)
	{
		FVoxelSkillNode N;
		N.Name = Name; N.Rank = Rank; N.MaxRank = 3; N.Tier = Tier;
		N.bUnlocked = bUnlocked; N.bAvailable = bAvailable;
		N.Flavour = Flavour; N.RankEffect = Effect; N.Requires = Requires;
		return N;
	};

	FVoxelSkillDiscipline Blade;
	Blade.Name = LOCTEXT("SeedDiscBlade", "Blade");
	Blade.Unlocked = 3; Blade.Total = 8;
	Blade.Nodes = {
		Node(LOCTEXT("SeedSkSword", "Swordplay"), 3, 0, true, false,
		     LOCTEXT("SeedSkSwordF", "The first thing anyone learns, and the last thing anyone masters."),
		     LOCTEXT("SeedSkSwordE", "Swing damage +6"), FText::GetEmpty()),
		Node(LOCTEXT("SeedSkRiposte", "Riposte"), 2, 1, true, false,
		     LOCTEXT("SeedSkRiposteF", "A blow turned aside is half a blow returned."),
		     LOCTEXT("SeedSkRiposteE", "Counter window +0.08s"), LOCTEXT("SeedSkRiposteR", "Swordplay III")),
		Node(LOCTEXT("SeedSkParry", "Parry"), 0, 2, false, true,
		     LOCTEXT("SeedSkParryF", "A blade in the right hand turns aside a blade in the wrong one — and Roland's grip has lately grown right-handed."),
		     LOCTEXT("SeedSkParryE", "Parry window +0.10s\nStagger on parry +8"),
		     LOCTEXT("SeedSkParryR", "Riposte II")),
		Node(LOCTEXT("SeedSkExecute", "Execute"), 0, 2, false, true,
		     LOCTEXT("SeedSkExecuteF", "Some fights end before the second exchange."),
		     LOCTEXT("SeedSkExecuteE", "Finisher damage +25%"), LOCTEXT("SeedSkExecuteR", "Riposte II")),
		Node(LOCTEXT("SeedSkUnyield", "Unyielding"), 0, 3, false, false,
		     LOCTEXT("SeedSkUnyieldF", "Standing still is a tactic if you can afford it."),
		     LOCTEXT("SeedSkUnyieldE", "Stagger resistance +20"), LOCTEXT("SeedSkUnyieldR", "Parry II")),
		Node(LOCTEXT("SeedSkWhirl", "Whirlwind"), 0, 3, false, false,
		     LOCTEXT("SeedSkWhirlF", "For when there is more than one of them."),
		     LOCTEXT("SeedSkWhirlE", "Sweep arc +40°"), LOCTEXT("SeedSkWhirlR", "Execute II")),
		Node(LOCTEXT("SeedSkDuelist", "Duelist"), 0, 3, false, false,
		     LOCTEXT("SeedSkDuelistF", "For when there is exactly one of them."),
		     LOCTEXT("SeedSkDuelistE", "One-on-one damage +12%"), LOCTEXT("SeedSkDuelistR", "Parry III")),
		Node(LOCTEXT("SeedSkSunder", "Sunderstrike"), 0, 3, false, false,
		     LOCTEXT("SeedSkSunderF", "Armour is only a promise about the next blow."),
		     LOCTEXT("SeedSkSunderE", "Ignores 30 armour"), LOCTEXT("SeedSkSunderR", "Execute III")),
	};
	Data.Disciplines.Add(Blade);

	auto Discipline = [](const FText& Name, int32 Unlocked, int32 Total)
	{
		FVoxelSkillDiscipline D;
		D.Name = Name; D.Unlocked = Unlocked; D.Total = Total;
		return D;
	};
	Data.Disciplines.Add(Discipline(LOCTEXT("SeedDiscBulwark", "Bulwark"), 1, 6));
	Data.Disciplines.Add(Discipline(LOCTEXT("SeedDiscHunt", "Hunt"), 2, 7));
	Data.Disciplines.Add(Discipline(LOCTEXT("SeedDiscWay", "Wayfaring"), 4, 9));
	Data.Disciplines.Add(Discipline(LOCTEXT("SeedDiscStone", "Stonecraft"), 2, 8));
	Data.Disciplines.Add(Discipline(LOCTEXT("SeedDiscPyro", "Pyromancy"), 0, 6));

	auto Perk = [](const FText& Name, const FText& Group, int32 Level, bool bOwned, bool bLocked,
	               const FText& Req, const FText& Desc, const FText& Flavour)
	{
		FVoxelPerk P;
		P.Name = Name; P.Group = Group; P.Level = Level; P.bOwned = bOwned; P.bLocked = bLocked;
		P.Requirement = Req; P.Description = Desc; P.Flavour = Flavour;
		return P;
	};
	const FText MainLevel = LOCTEXT("SeedPerkGroupMain", "Main Level");
	const FText BladeGroup = LOCTEXT("SeedPerkGroupBlade", "Blade");
	Data.Perks = {
		Perk(LOCTEXT("SeedPk1", "Charming Man"), MainLevel, 1, false, false,
		     LOCTEXT("SeedPk1R", "MAIN LEVEL · MIN. LV 1"),
		     LOCTEXT("SeedPk1D", "People remember a pleasant word longer than they remember a threat."),
		     LOCTEXT("SeedPk1F", "Speech checks against strangers are one band easier.")),
		Perk(LOCTEXT("SeedPk2", "Driven by Vengeance"), MainLevel, 3, false, false,
		     LOCTEXT("SeedPk2R", "MAIN LEVEL · MIN. LV 3"),
		     LOCTEXT("SeedPk2D", "There is a kind of strength that comes only from having lost something."),
		     LOCTEXT("SeedPk2F", "Damage rises as health falls.")),
		Perk(LOCTEXT("SeedPk3", "Martin's Heritage"), MainLevel, 4, false, false,
		     LOCTEXT("SeedPk3R", "MAIN LEVEL · MIN. LV 4"),
		     LOCTEXT("SeedPk3D", "A smith's son knows what a good weld looks like without being told."),
		     LOCTEXT("SeedPk3F", "Repairs cost 20% less.")),
		Perk(LOCTEXT("SeedPk4", "Memorable"), MainLevel, 5, true, false,
		     LOCTEXT("SeedPk4R", "MAIN LEVEL · MIN. LV 5"),
		     LOCTEXT("SeedPk4D", "Whether they liked you or not, they will know you again."),
		     LOCTEXT("SeedPk4F", "Reputation changes are 25% larger, in both directions.")),
		Perk(LOCTEXT("SeedPk5", "Undaunted Cavalier"), MainLevel, 6, true, false,
		     LOCTEXT("SeedPk5R", "MAIN LEVEL · MIN. LV 6"),
		     LOCTEXT("SeedPk5D", "Looking like an intrepid hero of legends is the first step to performing heroic deeds. And that's the first step to entering into legend."),
		     LOCTEXT("SeedPk5F", "If your Charisma is higher than 20, your armour will be considered 15 higher.")),
		Perk(LOCTEXT("SeedPk6", "Burgher"), MainLevel, 8, false, true,
		     LOCTEXT("SeedPk6R", "MAIN LEVEL · MIN. LV 8"),
		     LOCTEXT("SeedPk6D", "A man with a house is a man with an opinion worth hearing."),
		     LOCTEXT("SeedPk6F", "Traders open at better prices.")),
		Perk(LOCTEXT("SeedPk7", "Local Hero"), MainLevel, 10, false, true,
		     LOCTEXT("SeedPk7R", "MAIN LEVEL · MIN. LV 10"),
		     LOCTEXT("SeedPk7D", "The valley has decided about you, and it decided well."),
		     LOCTEXT("SeedPk7F", "Reputation never falls below neutral in your home region.")),
		Perk(LOCTEXT("SeedPk8", "Quick Hands"), BladeGroup, 2, true, false,
		     LOCTEXT("SeedPk8R", "BLADE · MIN. LV 2"),
		     LOCTEXT("SeedPk8D", "The second cut matters more than the first."),
		     LOCTEXT("SeedPk8F", "Attack recovery shortened by 10%.")),
		Perk(LOCTEXT("SeedPk9", "Blood of Siegfried"), BladeGroup, 12, false, true,
		     LOCTEXT("SeedPk9R", "BLADE · MIN. LV 12"),
		     LOCTEXT("SeedPk9D", "There are older stories than yours, and you are starting to appear in them."),
		     LOCTEXT("SeedPk9F", "Killing blows restore a tenth of your health.")),
	};

	auto Faction = [](const FText& Name, int32 Standing)
	{
		FVoxelFactionStanding F; F.Name = Name; F.Standing = Standing; return F;
	};
	Data.Factions = {
		Faction(LOCTEXT("SeedFc1", "Aldenholt Guard"), 68),
		Faction(LOCTEXT("SeedFc2", "The Wayfarers"), 36),
		Faction(LOCTEXT("SeedFc3", "House Mira-Thal"), 24),
		Faction(LOCTEXT("SeedFc4", "Brothers' End Miners"), -16),
		Faction(LOCTEXT("SeedFc5", "The Drun Cult"), -84),
		Faction(LOCTEXT("SeedFc6", "Hollowford Merchants"), 12),
		Faction(LOCTEXT("SeedFc7", "Wardens of the Spires"), 0),
	};

	return Data;
}

FVoxelCodexData SeedCodex()
{
	FVoxelCodexData Data;

	// The recipe table, and its ingredient names, are the mock's. NONE of these
	// ingredients is an item in FVoxelItemRegistry -- there is no plank, stick,
	// ingot or hide in this game -- so every "in pack" count resolves to
	// unknown. See FVoxelRecipeIngredient::Held.
	auto Ing = [](const FText& Name, FName Glyph, int32 Needed)
	{
		FVoxelRecipeIngredient I;
		I.Name = Name; I.Glyph = Glyph; I.Needed = Needed;
		return I;
	};
	const FText Plank = LOCTEXT("SeedIngPlank", "Oak Plank");
	const FText Log = LOCTEXT("SeedIngLog", "Oak Log");
	const FText Stick = LOCTEXT("SeedIngStick", "Stick");
	const FText Iron = LOCTEXT("SeedIngIron", "Iron Ingot");
	const FText Cobble = LOCTEXT("SeedIngCobble", "Cobblestone");
	const FText Leather = LOCTEXT("SeedIngLeather", "Cured Hide");
	const FText Coal = LOCTEXT("SeedIngCoal", "Charcoal");
	const FText String = LOCTEXT("SeedIngString", "Flax Twine");
	const FText Feather = LOCTEXT("SeedIngFeather", "Feather");

	const FText Building = LOCTEXT("SeedCatBuilding", "Building");
	const FText Tools = LOCTEXT("SeedCatTools", "Tools");
	const FText Weapons = LOCTEXT("SeedCatWeapons", "Weapons");
	const FText Armor = LOCTEXT("SeedCatArmor", "Armor");
	const FText Grid = LOCTEXT("SeedStationGrid", "Crafting grid");
	const FText Anvil = LOCTEXT("SeedStationAnvil", "Anvil");

	auto Recipe = [&](const FText& Name, const FText& Category, FName Glyph, int32 Yield,
	                  const FText& Station, const FText& Desc,
	                  TArray<FVoxelRecipeIngredient> Ingredients)
	{
		FVoxelRecipe R;
		R.Name = Name; R.Category = Category; R.Glyph = Glyph; R.Yield = Yield;
		R.Station = Station; R.Description = Desc; R.Ingredients = MoveTemp(Ingredients);
		return R;
	};

	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc1", "Oak Plank"), Building, TEXT("plank"), 4, Grid,
	                        LOCTEXT("SeedRc1D", "Split a log four ways. The base of nearly every wooden recipe."),
	                        {Ing(Log, TEXT("log"), 1)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc2", "Stick"), Building, TEXT("stick"), 4, Grid,
	                        LOCTEXT("SeedRc2D", "Two planks, quartered lengthwise."),
	                        {Ing(Plank, TEXT("plank"), 2)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc3", "Wooden Door"), Building, TEXT("door"), 3, Grid,
	                        LOCTEXT("SeedRc3D", "A plank door on iron pins. Opens with a shove; bars from the inside."),
	                        {Ing(Plank, TEXT("plank"), 6)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc4", "Storage Chest"), Building, TEXT("chest"), 1, Grid,
	                        LOCTEXT("SeedRc4D", "Twenty-seven slots. Contents survive a cave-in."),
	                        {Ing(Plank, TEXT("plank"), 8)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc5", "Stone Furnace"), Building, TEXT("furnace"), 1, Grid,
	                        LOCTEXT("SeedRc5D", "Smelts ore and fires clay. Burns charcoal or coal."),
	                        {Ing(Cobble, TEXT("cobble"), 8)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc6", "Iron Anvil"), Building, TEXT("anvil"), 1, Grid,
	                        LOCTEXT("SeedRc6D", "Required for every armour recipe and for weapon repair."),
	                        {Ing(Iron, TEXT("iron"), 7)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc7", "Iron Pickaxe"), Tools, TEXT("pick"), 1, Grid,
	                        LOCTEXT("SeedRc7D", "Cuts stone at three voxels a swing. Tier gate for iron and deeper ores."),
	                        {Ing(Iron, TEXT("iron"), 3), Ing(Stick, TEXT("stick"), 2)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc8", "Iron Axe"), Tools, TEXT("axe"), 1, Grid,
	                        LOCTEXT("SeedRc8D", "Fells trees whole. Doubles as a poor man's war axe."),
	                        {Ing(Iron, TEXT("iron"), 3), Ing(Stick, TEXT("stick"), 2)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc9", "Long Torch"), Tools, TEXT("torchi"), 4, Grid,
	                        LOCTEXT("SeedRc9D", "Burns nine minutes. Placed torches keep spawns off a corridor."),
	                        {Ing(Coal, TEXT("stone"), 1), Ing(Stick, TEXT("stick"), 1)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc10", "Iron Longsword"), Weapons, TEXT("sword"), 1, Anvil,
	                        LOCTEXT("SeedRc10D", "Two ingots to the blade, one to the tang. Roland's standing sidearm."),
	                        {Ing(Iron, TEXT("iron"), 2), Ing(Stick, TEXT("stick"), 1)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc11", "Hunter's Bow"), Weapons, TEXT("bow"), 1, Grid,
	                        LOCTEXT("SeedRc11D", "Yew stave, flax string. Draw weight scales with Strength."),
	                        {Ing(Stick, TEXT("stick"), 3), Ing(String, TEXT("string"), 3)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc12", "Steel Arrow"), Weapons, TEXT("arrow"), 4, Grid,
	                        LOCTEXT("SeedRc12D", "Iron head, oak shaft, goose fletching."),
	                        {Ing(Iron, TEXT("iron"), 1), Ing(Stick, TEXT("stick"), 1),
	                         Ing(Feather, TEXT("feather"), 1)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc13", "Iron Helm"), Armor, TEXT("helm"), 1, Anvil,
	                        LOCTEXT("SeedRc13D", "Five ingots. Halves head-shot damage; muffles hearing."),
	                        {Ing(Iron, TEXT("iron"), 5)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc14", "Chain Hauberk"), Armor, TEXT("armor"), 1, Anvil,
	                        LOCTEXT("SeedRc14D", "Eight ingots drawn to wire. The heaviest thing Roland can run in."),
	                        {Ing(Iron, TEXT("iron"), 8)}));
	Data.Recipes.Add(Recipe(LOCTEXT("SeedRc15", "Leather Boots"), Armor, TEXT("boots"), 1, Grid,
	                        LOCTEXT("SeedRc15D", "Quiet on stone. Negates the first two voxels of fall damage."),
	                        {Ing(Leather, TEXT("leather"), 4)}));

	auto Entry = [](const FText& Name, const FText& Sub, TArray<TPair<FText, FText>> Facts,
	                TArray<FText> Paragraphs, const FText& Quote = FText::GetEmpty(),
	                bool bUnread = false)
	{
		FVoxelCodexEntry E;
		E.Name = Name; E.Subtitle = Sub; E.Facts = MoveTemp(Facts);
		E.Paragraphs = MoveTemp(Paragraphs); E.Quote = Quote; E.bUnread = bUnread;
		return E;
	};
	auto Locked = [](const FText& Name)
	{
		FVoxelCodexEntry E; E.Name = Name; E.bLocked = true; return E;
	};

	FVoxelCodexCategory People;
	People.Name = LOCTEXT("SeedCdPeople", "People");
	People.Kind = LOCTEXT("SeedCdKindPerson", "PERSON");
	People.Entries = {
		Entry(LOCTEXT("SeedPp1", "Maren Kolt"), LOCTEXT("SeedPp1S", "— a smith, met on the coast road"),
		      {{LOCTEXT("SeedFactTrade", "Trade"), LOCTEXT("SeedPp1F1", "Metalsmithing")},
		       {LOCTEXT("SeedFactSeen", "Last seen"), LOCTEXT("SeedPp1F2", "The coast road")},
		       {LOCTEXT("SeedFactDisp", "Disposition"), LOCTEXT("SeedPp1F3", "Wary")},
		       {LOCTEXT("SeedFactTeaches", "Teaches"), LOCTEXT("SeedPp1F4", "Bronze casting")}},
		      {LOCTEXT("SeedPp1B1", "She travels with a hand cart and a bellows strapped to it, and she will not say where she is going."),
		       LOCTEXT("SeedPp1B2", "For the price of six copper lumps she showed me how to bank a bloomery so it holds heat overnight. She would not take food. She said food makes people follow you.")},
		      LOCTEXT("SeedPp1Q", "\"Stone breaks. Copper bends. Bronze does neither, if you get the mix right — and you will not, the first four times.\"")),
		Entry(LOCTEXT("SeedPp2", "The Woodcutter"), LOCTEXT("SeedPp2S", "— name unknown, north of the ridge"),
		      {{LOCTEXT("SeedFactTrade", "Trade"), LOCTEXT("SeedPp2F1", "Unclear")},
		       {LOCTEXT("SeedFactSeen", "Last seen"), LOCTEXT("SeedPp2F2", "North ridge")},
		       {LOCTEXT("SeedFactDisp", "Disposition"), LOCTEXT("SeedPp2F3", "Silent")},
		       {LOCTEXT("SeedFactTrades", "Trades"), LOCTEXT("SeedPp2F4", "Rope for meat")}},
		      {LOCTEXT("SeedPp2B1", "I have seen him three times and he has never spoken. He fells trees far from any camp and leaves the logs where they land."),
		       LOCTEXT("SeedPp2B2", "He left a coil of rope on my wall the morning after the wolves came. I left him a haunch of venison. It was gone by dusk.")}),
		Entry(LOCTEXT("SeedPp3", "Old Bregga"), LOCTEXT("SeedPp3S", "— keeps goats on the moor"),
		      {{LOCTEXT("SeedFactTrade", "Trade"), LOCTEXT("SeedPp3F1", "Husbandry")},
		       {LOCTEXT("SeedFactSeen", "Last seen"), LOCTEXT("SeedPp3F2", "Open moor")},
		       {LOCTEXT("SeedFactDisp", "Disposition"), LOCTEXT("SeedPp3F3", "Friendly")},
		       {LOCTEXT("SeedFactTeaches", "Teaches"), LOCTEXT("SeedPp3F4", "Taming")}},
		      {LOCTEXT("SeedPp3B1", "Bregga has survived eleven winters out here, which makes her the foremost authority on everything."),
		       LOCTEXT("SeedPp3B2", "Her advice, given freely and at length: build the roof before the walls, never sleep facing the door, and an ox is worth more than a sword.")}),
		Entry(LOCTEXT("SeedPp4", "The Man in the Cave"), LOCTEXT("SeedPp4S", "— found below the creek"),
		      {{LOCTEXT("SeedFactTrade", "Trade"), LOCTEXT("SeedPp4F1", "Unknown")},
		       {LOCTEXT("SeedFactSeen", "Last seen"), LOCTEXT("SeedPp4F2", "Second chamber")},
		       {LOCTEXT("SeedFactDisp", "Disposition"), LOCTEXT("SeedPp4F3", "Unknown")},
		       {LOCTEXT("SeedFactNotable", "Notable"), LOCTEXT("SeedPp4F4", "Left a map")}},
		      {LOCTEXT("SeedPp4B1", "A firepit long cold, a bedroll rotted to threads, and charcoal marks on the wall counting something to two hundred and forty."),
		       LOCTEXT("SeedPp4B2", "No body. Whoever kept that count either walked out or went deeper.")},
		      FText::GetEmpty(), /*bUnread=*/true),
		Locked(LOCTEXT("SeedPpL1", "??? — the voice on the ridge")),
		Locked(LOCTEXT("SeedPpL2", "??? — the trader")),
		Locked(LOCTEXT("SeedPpL3", "??? — the other survivor")),
	};
	Data.Categories.Add(People);

	FVoxelCodexCategory Factions;
	Factions.Name = LOCTEXT("SeedCdFactions", "Factions");
	Factions.Kind = LOCTEXT("SeedCdKindFaction", "FACTION");
	Factions.Entries = {
		Entry(LOCTEXT("SeedFa1", "The Ashfallen"), LOCTEXT("SeedFa1S", "— they come with the cold"),
		      {{LOCTEXT("SeedFactTerr", "Territory"), LOCTEXT("SeedFa1F1", "The high passes")},
		       {LOCTEXT("SeedFactStr", "Strength"), LOCTEXT("SeedFa1F2", "Unknown")},
		       {LOCTEXT("SeedFactHost", "Hostility"), LOCTEXT("SeedFa1F3", "Total")},
		       {LOCTEXT("SeedFactActive", "Active"), LOCTEXT("SeedFa1F4", "Winter")}},
		      {LOCTEXT("SeedFa1B1", "They do not raid for food. They raid for tools, and they burn what they cannot carry."),
		       LOCTEXT("SeedFa1B2", "Every account agrees on one detail: they travel at night and they never come twice by the same road.")},
		      LOCTEXT("SeedFa1Q", "\"They took the anvil and left the grain. Think about what that means.\" — Bregga")),
		Entry(LOCTEXT("SeedFa2", "The Hollow Pack"), LOCTEXT("SeedFa2S", "— wolves, but organised"),
		      {{LOCTEXT("SeedFactTerr", "Territory"), LOCTEXT("SeedFa2F1", "North ridge")},
		       {LOCTEXT("SeedFactStr", "Strength"), LOCTEXT("SeedFa2F2", "9-12")},
		       {LOCTEXT("SeedFactHost", "Hostility"), LOCTEXT("SeedFa2F3", "Opportunistic")},
		       {LOCTEXT("SeedFactActive", "Active"), LOCTEXT("SeedFa2F4", "Dusk to dawn")}},
		      {LOCTEXT("SeedFa2B1", "A pack this size should have split by now. It has not, and something in the den is the reason."),
		       LOCTEXT("SeedFa2B2", "They test a camp for three nights before they commit. Fire buys the first two nights. The third night you need a wall.")}),
		Locked(LOCTEXT("SeedFaL1", "??? — the ones who left the markers")),
		Locked(LOCTEXT("SeedFaL2", "??? — the deep")),
	};
	Data.Categories.Add(Factions);

	FVoxelCodexCategory Lore;
	Lore.Name = LOCTEXT("SeedCdLore", "Lore");
	Lore.Kind = LOCTEXT("SeedCdKindLore", "LORE");
	Lore.Entries = {
		Entry(LOCTEXT("SeedLo1", "The Second Age"), LOCTEXT("SeedLo1S", "— by the reckoning of the stone markers"),
		      {{LOCTEXT("SeedFactYear", "Current year"), LOCTEXT("SeedLo1F1", "18th")},
		       {LOCTEXT("SeedFactSeason", "Season length"), LOCTEXT("SeedLo1F2", "20 days")},
		       {LOCTEXT("SeedFactReck", "Reckoning"), LOCTEXT("SeedLo1F3", "Stone markers")},
		       {LOCTEXT("SeedFactBegan", "Age began"), LOCTEXT("SeedLo1F4", "Unknown")}},
		      {LOCTEXT("SeedLo1B1", "Every valley has a marker stone with a count scratched into it, and every count agrees: this is the eighteenth year of the second age."),
		       LOCTEXT("SeedLo1B2", "Nobody living knows what ended the first one. The markers do not say, and the people who cut them are not here to ask.")}),
		Entry(LOCTEXT("SeedLo2", "On bronze"), LOCTEXT("SeedLo2S", "— copper and tin, in the right measure"),
		      {{LOCTEXT("SeedFactCopper", "Copper"), LOCTEXT("SeedLo2F1", "Surface veins")},
		       {LOCTEXT("SeedFactTin", "Tin"), LOCTEXT("SeedLo2F2", "Scarce, deep")},
		       {LOCTEXT("SeedFactRatio", "Ratio"), LOCTEXT("SeedLo2F3", "9 to 1")},
		       {LOCTEXT("SeedFactYield", "Yield"), LOCTEXT("SeedLo2F4", "4× stone life")}},
		      {LOCTEXT("SeedLo2B1", "Copper alone is nearly useless — it holds no edge and folds under load. Tin alone is worse."),
		       LOCTEXT("SeedLo2B2", "Together, in roughly nine parts to one, they make a metal that will mine stone all day and still take an edge in the evening. This is the whole reason to dig.")}),
		Entry(LOCTEXT("SeedLo3", "The long winter"), LOCTEXT("SeedLo3S", "— sixty days of it, at least"),
		      {{LOCTEXT("SeedFactDur", "Duration"), LOCTEXT("SeedLo3F1", "60+ days")},
		       {LOCTEXT("SeedFactGround", "Ground"), LOCTEXT("SeedLo3F2", "Frozen day 8")},
		       {LOCTEXT("SeedFactWater", "Water"), LOCTEXT("SeedLo3F3", "Iced day 12")},
		       {LOCTEXT("SeedFactSurv", "Survival"), LOCTEXT("SeedLo3F4", "Fuel + stores")}},
		      {LOCTEXT("SeedLo3B1", "The cold arrives before the snow. The ground freezes first, so anything you meant to dig, dig it now."),
		       LOCTEXT("SeedLo3B2", "Forty units of food and two hundred logs is the figure Bregga gives, and she has been right eleven times.")}),
	};
	Data.Categories.Add(Lore);

	return Data;
}

FVoxelDialogueData SeedDialogue()
{
	// The Captain Vossant fine-or-cell node, verbatim from the mock. Every
	// number in it -- the skill scores, the thresholds, the 120-crown fine --
	// is the mock's, because SKILLS_AND_PROGRESSION.md and
	// CONVERSATION_SYSTEM.md, which the mock cites for all of them, are not in
	// this repository.
	FVoxelDialogueData Data;
	Data.SpeakerName = LOCTEXT("SeedDlgName", "CAPTAIN VOSSANT");
	Data.SpeakerRole = LOCTEXT("SeedDlgRole", "— Golden Lance, Solgrade");
	Data.SpeakerLine = LOCTEXT("SeedDlgLine", "\"The fine is a hundred and twenty crowns. The cell is for ten days. Either way you'll leave this hall a smaller man than you came in. Choose.\"");

	auto Skill = [](const FText& Name, FName Glyph, int32 Value)
	{
		FVoxelDialogueSkill S; S.Name = Name; S.Glyph = Glyph; S.Value = Value; return S;
	};
	Data.Skills = {
		Skill(LOCTEXT("SeedDlgSpeech", "SPEECH"), TEXT("speech"), 32),
		Skill(LOCTEXT("SeedDlgCharisma", "CHARISMA"), TEXT("charisma"), 17),
		Skill(LOCTEXT("SeedDlgStrength", "STRENGTH"), TEXT("strength"), 19),
		Skill(LOCTEXT("SeedDlgRep", "REPUTATION"), TEXT("rep"), 8),
		Skill(LOCTEXT("SeedDlgCoin", "COIN"), TEXT("coin"), 142),
	};

	auto Option = [](const FText& Text, const TCHAR* Key, EVoxelSkillCheckBand Band,
	                 FName Glyph, FName SkillId, int32 Threshold, bool bAvailable)
	{
		FVoxelDialogueOption O;
		O.Text = Text; O.Key = FText::FromString(Key); O.Band = Band;
		O.SkillGlyph = Glyph; O.SkillId = SkillId; O.Threshold = Threshold;
		O.bAvailable = bAvailable;
		return O;
	};
	// Availability is resolved against the scores above exactly as the mock's
	// isAvailable() does: coin >= cost, or the named score >= threshold.
	Data.Options = {
		Option(LOCTEXT("SeedDlgO1", "I'll pay the fine. (120)"), TEXT("1"),
		       EVoxelSkillCheckBand::None, TEXT("coin"), TEXT("coin"), 120, true),
		Option(LOCTEXT("SeedDlgO2", "I accept my punishment."), TEXT("2"),
		       EVoxelSkillCheckBand::None, NAME_None, NAME_None, 0, true),
		Option(LOCTEXT("SeedDlgO3", "We can come to an agreement…"), TEXT("3"),
		       EVoxelSkillCheckBand::Easy, TEXT("speech"), TEXT("speech"), 18, true),
		Option(LOCTEXT("SeedDlgO4", "Do you know who I am?"), TEXT("4"),
		       EVoxelSkillCheckBand::Easy, TEXT("charisma"), TEXT("charisma"), 14, true),
		Option(LOCTEXT("SeedDlgO5", "Let me go, or else!"), TEXT("5"),
		       EVoxelSkillCheckBand::Medium, TEXT("strength"), TEXT("strength"), 22, false),
		Option(LOCTEXT("SeedDlgO6", "The law is on my side."), TEXT("6"),
		       EVoxelSkillCheckBand::Medium, TEXT("rep"), TEXT("rep"), 25, false),
		Option(LOCTEXT("SeedDlgO7", "I just had a bit to drink…"), TEXT("7"),
		       EVoxelSkillCheckBand::Medium, TEXT("speech"), TEXT("speech"), 45, false),
	};

	return Data;
}

FVoxelDeathData SeedDeath(const FText& Stamp)
{
	FVoxelDeathData Data;
	Data.Stamp = Stamp;
	const TArray<FText>& Quips = VoxelUIStrings::DeathQuips();
	Data.Quip = Quips.Num() > 0 ? Quips[FMath::RandHelper(Quips.Num())] : FText::GetEmpty();
	return Data;
}
} // namespace VoxelScreenData

#undef LOCTEXT_NAMESPACE
