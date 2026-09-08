// Headless tests for the front end's pure logic.
//
// WHAT IS AND IS NOT COVERED. Almost all of a menu is pixels, and pixels are
// verified by the capture switches (docs/front-end-plan.md). What is left is a
// handful of small functions with invariants that are easy to state, easy to
// break, and INVISIBLE in a screenshot -- a progress bar that quietly goes
// backwards for two seconds looks fine in a still frame, and a slugify rule
// that maps an odd name onto an empty string writes into the saves root itself.
// Those are what this file covers.
//
// 2026-09-08 adds two of the same shape: the world stamp's ordinal, and the
// journal fixture's dates. A placeholder journal card dated one day into the
// FUTURE is the purest example of the class -- it is a correct-looking screen
// and there is no frame in which it looks wrong.
//
// It is also the only part of this work that a machine with no display can run,
// which matters given the whole front end was written somewhere with no engine
// at all.
//
// Run headlessly:
//   UnrealEditor-Cmd.exe VoxelEarth.uproject -unattended -nullrhi -nop4 \
//     -ExecCmds="Automation RunTests VoxelEarth.FrontEnd; Quit"

#include "VoxelScreenData.h"
#include "VoxelUIStrings.h"
#include "VoxelUITheme.h"
#include "VoxelWorldReadyProbe.h"
#include "VoxelSaveLibrary.h"
#include "VoxelFrontEndPolicy.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VoxelFrontEndTestsDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.
//
// EAutomationTestFlags is a strongly-typed enum class in 5.8, not an int
// bitmask, so this must keep the enum type all the way through -- the same note
// VoxelSkyTests.cpp carries.
constexpr EAutomationTestFlags kTestFlags = EAutomationTestFlags::EditorContext
                                          | EAutomationTestFlags::ClientContext
                                          | EAutomationTestFlags::EngineFilter;
} // namespace VoxelFrontEndTestsDetail

// --- The progress model ------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFrontEndProgressTest, "VoxelEarth.FrontEnd.LoadProgress",
                                 VoxelFrontEndTestsDetail::kTestFlags)

bool FVoxelFrontEndProgressTest::RunTest(const FString& Parameters)
{
	// The theatre model (owner directive, 2026-09-05): the bar plays an
	// artificial 30-60 s roll out on a smoothstep, and the world's readiness
	// is one bit that only gates the ending. The invariants below are the
	// reveal contract, restated as assertions -- each is easy to break in a
	// refactor and invisible in a still frame.

	// 1. THE EASING IS SMOOTHSTEP. Fixed points at 0, 1/2 and 1, and a slow
	// start (below linear at t=0.25) -- the property the curve was chosen for,
	// along with its zero slope at both ends, which is what makes the ~97%
	// hold read as a landing rather than a stop.
	TestEqual(TEXT("starts at zero"), ComputeTheatreProgress(0.f, false, 0.f), 0.f, 0.0001f);
	TestEqual(TEXT("midpoint of the ease"), ComputeTheatreProgress(0.5f, true, 0.f), 0.5f, 0.001f);
	TestTrue(TEXT("eases in below linear"), ComputeTheatreProgress(0.25f, true, 0.f) < 0.25f);

	// 2. 100% IS THE GATE'S PRIVILEGE. A finished theatre with the world ready
	// completes exactly; with the world NOT ready it holds at the shelf --
	// "100% while the world is still landing" stays the one lie this model
	// refuses to tell, same as its predecessor.
	TestEqual(TEXT("gate open completes"), ComputeTheatreProgress(1.f, true, 0.f), 1.f, 0.0001f);
	const float Held = ComputeTheatreProgress(1.f, false, 0.f);
	TestEqual(TEXT("gate closed holds at the shelf"), Held, kVoxelTheatreHoldProgress, 0.0001f);

	// 3. THE SHELF KEEPS THE HOURGLASS ALIVE. SVoxelHourglass stops spawning
	// grains at 0.995; the hold must sit strictly under it or a slow world
	// freezes the sand again -- the exact defect the old model had.
	TestTrue(TEXT("hold is under the grain emitter's cut-off"), Held < 0.995f);

	// 4. MONOTONE, including across the cap. A bar going backwards reads worse
	// than one standing still, and the elapsed fraction can only grow -- so
	// the previous value must dominate a smaller recomputation.
	const float High = ComputeTheatreProgress(0.9f, false, 0.f);
	TestEqual(TEXT("progress never decreases"), ComputeTheatreProgress(0.5f, false, High), High, 0.0001f);

	// 5. THE GATE OPENING AFTER A HOLD RESUMES FORWARD ONLY: the shelf lifts
	// to completion, never dips first.
	const float Resumed = ComputeTheatreProgress(1.f, true, Held);
	TestEqual(TEXT("completes after the hold"), Resumed, 1.f, 0.0001f);
	TestTrue(TEXT("no dip when the cap lifts"), Resumed >= Held);

	// 6. Out-of-range fractions are clamped rather than propagated. A caller
	// dividing by a zero duration should produce a full-but-bounded bar, not a
	// NaN that paints garbage.
	TestTrue(TEXT("negative fraction clamps"), ComputeTheatreProgress(-1.f, false, 0.f) >= 0.f);
	TestTrue(TEXT("over-unity fraction clamps"),
	         ComputeTheatreProgress(5.f, false, 0.f) <= kVoxelTheatreHoldProgress + 0.0001f);

	return true;
}

// --- Godot's Color.darkened(), which is a byte operation ---------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFrontEndDarkenTest, "VoxelEarth.FrontEnd.Darkened",
                                 VoxelFrontEndTestsDetail::kTestFlags)

bool FVoxelFrontEndDarkenTest::RunTest(const FString& Parameters)
{
	// THE POINT OF THIS TEST is that the obvious implementation is wrong.
	// Godot's Color.darkened(k) scales the STORED components, which for a
	// colour built from a hex literal are sRGB bytes -- so PANEL_OAK_2
	// darkened by 0.15 is #27170b. Doing the same scale in linear space gives a
	// visibly different colour, and both the pressed and disabled button states
	// are defined this way, so getting it wrong shows on every menu.
	const FColor Pressed = VoxelUITheme::Darkened(VoxelUITheme::PanelOak2, 0.15f);
	TestEqual(TEXT("pressed R"), int32(Pressed.R), int32(FMath::RoundToInt(0x2e * 0.85f)));
	TestEqual(TEXT("pressed G"), int32(Pressed.G), int32(FMath::RoundToInt(0x1b * 0.85f)));
	TestEqual(TEXT("pressed B"), int32(Pressed.B), int32(FMath::RoundToInt(0x0d * 0.85f)));

	// Alpha is carried through untouched -- Godot's does the same, and a
	// darkened button that also went translucent would be a surprise.
	TestEqual(TEXT("alpha preserved"), int32(Pressed.A), int32(VoxelUITheme::PanelOak2.A));

	// The ends behave.
	TestEqual(TEXT("zero darkening is identity"),
	          VoxelUITheme::Darkened(VoxelUITheme::Gold, 0.f).ToPackedARGB(), VoxelUITheme::Gold.ToPackedARGB());
	const FColor Black = VoxelUITheme::Darkened(VoxelUITheme::Gold, 1.0f);
	TestEqual(TEXT("full darkening is black"), int32(Black.R) + int32(Black.G) + int32(Black.B), 0);

	return true;
}

// --- Save-name slugification -------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFrontEndSlugifyTest, "VoxelEarth.FrontEnd.Slugify",
                                 VoxelFrontEndTestsDetail::kTestFlags)

bool FVoxelFrontEndSlugifyTest::RunTest(const FString& Parameters)
{
	// The example from GameState.gd's own comment.
	TestEqual(TEXT("the documented case"), VoxelSave::Slugify(TEXT("My Save 1!")), TEXT("my_save_1"));

	// Runs collapse and the ends are trimmed, so a name cannot produce a slug
	// with leading or doubled separators.
	TestEqual(TEXT("runs collapse"), VoxelSave::Slugify(TEXT("  a   b  ")), TEXT("a_b"));
	TestEqual(TEXT("punctuation collapses"), VoxelSave::Slugify(TEXT("a---b")), TEXT("a_b"));

	// THE CASE THAT MATTERS MOST. An all-punctuation name would otherwise slug
	// to the empty string, and a save directory named "" is the saves ROOT --
	// which is a directory full of other people's saves.
	TestEqual(TEXT("empty becomes untitled"), VoxelSave::Slugify(TEXT("")), TEXT("untitled"));
	TestEqual(TEXT("all punctuation becomes untitled"), VoxelSave::Slugify(TEXT("!!!")), TEXT("untitled"));
	TestEqual(TEXT("whitespace becomes untitled"), VoxelSave::Slugify(TEXT("   ")), TEXT("untitled"));

	// Case folds, digits survive.
	TestEqual(TEXT("case folds"), VoxelSave::Slugify(TEXT("ABC")), TEXT("abc"));
	TestEqual(TEXT("digits survive"), VoxelSave::Slugify(TEXT("Day 12")), TEXT("day_12"));

	// A slug is idempotent: re-slugging one must not change it, or a save
	// written and then re-read under its own slug would drift to a new
	// directory each time.
	const FString Once = VoxelSave::Slugify(TEXT("Copper Isles: Day 1"));
	TestEqual(TEXT("idempotent"), VoxelSave::Slugify(Once), Once);

	return true;
}

// --- Rule 5's switch classifier ----------------------------------------------
//
// WHY THIS TEST EXISTS. Rule 5 decides whether a run gets a main menu by
// looking for 14 substrings ANYWHERE in a -Voxel* switch name. That is a
// deliberate fuzzy rule with an asymmetric error budget (see
// VoxelFrontEndPolicy.cpp), and the fuzziness is the right call -- but it had
// no test, and the tool meant to police it cannot see this class of mistake:
// lint-frontend-switch-coverage.py SKIPS every name the substring rule already
// matches, so a name matched BY ACCIDENT is invisible to it by construction.
//
// Of 366 -Voxel* names in the tree, 81 match rule 5. Most deserve to. The ones
// that do not are ordinary knobs and instruments that merely inherited a
// fixture's vocabulary, and each one silently costs its user the menu.
//
// This test pins the two directions that matter. It is cheap, it is headless,
// and it fails loudly if somebody deletes an exemption -- which is the property
// the exemption table needs in order to be worth having.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFrontEndSwitchPolicyTest, "VoxelEarth.FrontEnd.SwitchPolicy",
                                 VoxelFrontEndTestsDetail::kTestFlags)

bool FVoxelFrontEndSwitchPolicyTest::RunTest(const FString& Parameters)
{
	using VoxelFrontEnd::IsSelfDrivingSwitchName;

	// Direction 1: the rule still does its job. These are real fixtures, and a
	// menu in front of any of them is a hung capture -- the expensive failure.
	TestTrue(TEXT("a *Test fixture is self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelGICaveTest")));
	TestTrue(TEXT("a *Shot capture is self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelVistaShot")));
	TestTrue(TEXT("a timed *After is self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelScreenshotAfter")));
	TestTrue(TEXT("a named extra is self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelPerfRun")));
	TestTrue(TEXT("mid-name Shot still counts"), IsSelfDrivingSwitchName(TEXT("VoxelHudShotOnly")));

	// A switch that did not exist when the rule was written must still classify
	// from its name alone. This is the property that makes a rule better than a
	// list, so it is worth pinning rather than assuming.
	TestTrue(TEXT("an unknown *Test classifies from the convention alone"),
	         IsSelfDrivingSwitchName(TEXT("VoxelSomethingNobodyHasWrittenYetTest")));

	// Direction 2: the accidental match. -VoxelReadyProbeLog contains "Probe"
	// but is a log flag for FVoxelWorldReadyProbe -- the loading screen's gate,
	// which exists ONLY while the front end is up. Classified self-driving, it
	// suppressed the front end, which removed the loading screen, which removed
	// the probe: the diagnostic switch guaranteed its own subject never ran.
	//
	// Delete the exemption in VoxelFrontEndPolicy.cpp and this line fails --
	// which is the point of writing it down.
	TestFalse(TEXT("-VoxelReadyProbeLog is a log flag, not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelReadyProbeLog")));

	// The other twelve accidents, spot-checked across all three substrings that
	// produced them. Every one of these is recorded ACCIDENTAL in
	// tools/frontend-switch-classification.txt; this is the half that makes the
	// recording true of the running game rather than only of the lint.
	//
	// -VoxelGpuMeshInFlight is the case worth keeping forever: it already ENDS
	// in the substring that caught it, so the advice the lint used to print --
	// "rename it to end in Test/Shot/After and the rule handles it" -- cannot
	// fix this class of mistake, and a suffix convention cannot encode intent.
	TestFalse(TEXT("a job in-flight cap is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelGpuMeshInFlight")));
	TestFalse(TEXT("a per-core job cap is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelJobsInFlightPerCore")));

	// The correctness arms. These verify DURING an ordinary run and never end
	// one, so they are exactly the switches a person arms interactively -- which
	// is the only situation where rule 5 is reached at all, since rule 4
	// (unattended) short-circuits every capture leg before rule 5 runs.
	TestFalse(TEXT("the solid-skip correctness arm is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelVerifySolidSkip")));
	TestFalse(TEXT("the buried-skip correctness arm is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelVerifyBuriedSkip")));
	TestFalse(TEXT("the sky-band correctness arm is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelVerifySkyBand")));
	TestFalse(TEXT("a worklist byte gate is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelGpuWorklistVerifyCT")));
	TestFalse(TEXT("the eviction-index exit-scan arm is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelBucketedExitScanVerify")));

	// An exemption must not leak to a NEIGHBOURING name. The table matches whole
	// names, not substrings, and a genuine fixture that merely shares a prefix
	// with an exempted switch must still be caught.
	TestTrue(TEXT("an exemption does not spread to a longer name"),
	         IsSelfDrivingSwitchName(TEXT("VoxelVerifySolidSkipTest")));

	// Ordinary configuration must never be caught. These are the plain cases
	// the rule gets right, and they are cheap insurance against a future
	// substring being added that is too greedy to be safe.
	TestFalse(TEXT("a seed is not self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelSeed")));
	TestFalse(TEXT("a spawn pose is not self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelSpawnAt")));
	TestFalse(TEXT("the front-end switch itself is not self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelFrontEnd")));

	// The match is case-sensitive by construction; a lowercase spelling is a
	// different switch and must not inherit the classification.
	TestFalse(TEXT("classification is case-sensitive"), IsSelfDrivingSwitchName(TEXT("Voxelgicavetest")));

	return true;
}

// --- The world stamp and the journal's dates ---------------------------------
//
// WHY THIS IS HERE AND THE REST OF THE JOURNAL IS NOT. Everything else about
// that screen is pixels, and pixels are the owner's to judge on a capture. What
// is testable is the pair of pure functions underneath it: the ordinal, whose
// 11/12/13 exception is the thing every inline version gets wrong, and
// SeedJournal's date anchoring, which is INVISIBLE when it breaks -- a
// placeholder card dated one day into the future looks exactly like a
// placeholder card dated correctly, and it shipped that way.
//
// Both take their calendar as arguments, so neither needs a world, a sky
// subsystem or a viewport.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFrontEndWorldStampTest, "VoxelEarth.FrontEnd.WorldStamp",
                                 VoxelFrontEndTestsDetail::kTestFlags)

bool FVoxelFrontEndWorldStampTest::RunTest(const FString& Parameters)
{
	// 1. THE ORDINAL. The first three take their own suffix; 11, 12 and 13 do
	// not, despite ending in 1, 2 and 3; and the exception repeats every century,
	// which is the half a last-digit-only implementation gets wrong.
	TestEqual(TEXT("1st"), VoxelUIStrings::Ordinal(1).ToString(), FString(TEXT("1st")));
	TestEqual(TEXT("2nd"), VoxelUIStrings::Ordinal(2).ToString(), FString(TEXT("2nd")));
	TestEqual(TEXT("3rd"), VoxelUIStrings::Ordinal(3).ToString(), FString(TEXT("3rd")));
	TestEqual(TEXT("4th"), VoxelUIStrings::Ordinal(4).ToString(), FString(TEXT("4th")));
	TestEqual(TEXT("11th, not 11st"), VoxelUIStrings::Ordinal(11).ToString(), FString(TEXT("11th")));
	TestEqual(TEXT("12th, not 12nd"), VoxelUIStrings::Ordinal(12).ToString(), FString(TEXT("12th")));
	TestEqual(TEXT("13th, not 13rd"), VoxelUIStrings::Ordinal(13).ToString(), FString(TEXT("13th")));
	TestEqual(TEXT("21st"), VoxelUIStrings::Ordinal(21).ToString(), FString(TEXT("21st")));
	TestEqual(TEXT("the mock's own year"), VoxelUIStrings::Ordinal(18).ToString(), FString(TEXT("18th")));
	TestEqual(TEXT("111th, the repeat of the exception"),
	          VoxelUIStrings::Ordinal(111).ToString(), FString(TEXT("111th")));
	// UNGROUPED. FText::AsNumber's default would make this "1,000th".
	TestEqual(TEXT("a four-digit year does not group"),
	          VoxelUIStrings::Ordinal(1000).ToString(), FString(TEXT("1000th")));

	// 2. THE FOUR SEASON LABELS, in VoxelSky::SeasonIndexFromDayOfYear's own
	// numbering. An index outside 0..3 is EMPTY, not a guess -- WorldStamp reads
	// that emptiness as "there was no sky to ask" and drops the season half.
	TestEqual(TEXT("index 0 is spring"), VoxelUIStrings::SeasonLabel(0).ToString(), FString(TEXT("Spring")));
	TestEqual(TEXT("index 1 is summer"), VoxelUIStrings::SeasonLabel(1).ToString(), FString(TEXT("Summer")));
	TestEqual(TEXT("index 2 is autumn"), VoxelUIStrings::SeasonLabel(2).ToString(), FString(TEXT("Autumn")));
	TestEqual(TEXT("index 3 is winter"), VoxelUIStrings::SeasonLabel(3).ToString(), FString(TEXT("Winter")));
	TestTrue(TEXT("INDEX_NONE names no season"), VoxelUIStrings::SeasonLabel(INDEX_NONE).IsEmpty());

	// 3. THE STAMP ITSELF. The mock's own line, reproduced exactly, is the
	// clearest statement of what this composes -- both Voxelmark Journal.html and
	// Voxelmark Death Screen.html print it for day 12 of the 18th year.
	TestEqual(TEXT("the mock's stamp, reproduced"),
	          VoxelUIStrings::WorldStamp(12, VoxelUIStrings::SeasonLabel(1), 18).ToString(),
	          FString(TEXT("Day 12 · Summer, 18th Year of the Second Age")));

	// 4. THE TWO WAYS IT DEGRADES, both of which a capture leg with no sky
	// subsystem actually hits. Day 0 means "this session cannot name a day" and
	// every caller omits the stamp entirely rather than drawing a blank one; a
	// missing season or year falls back to the day alone rather than printing
	// "Day 11 · , 0th Year of the Second Age".
	TestTrue(TEXT("day zero produces no stamp at all"),
	         VoxelUIStrings::WorldStamp(0, VoxelUIStrings::SeasonLabel(1), 18).IsEmpty());
	TestEqual(TEXT("no season falls back to the day"),
	          VoxelUIStrings::WorldStamp(11, FText::GetEmpty(), 18).ToString(),
	          FString(TEXT("Day 11")));
	TestEqual(TEXT("no year falls back to the day"),
	          VoxelUIStrings::WorldStamp(11, VoxelUIStrings::SeasonLabel(1), 0).ToString(),
	          FString(TEXT("Day 11")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFrontEndJournalSeedTest, "VoxelEarth.FrontEnd.JournalSeed",
                                 VoxelFrontEndTestsDetail::kTestFlags)

bool FVoxelFrontEndJournalSeedTest::RunTest(const FString& Parameters)
{
	using VoxelScreenData::SeedJournal;

	auto Calendar = [](int32 Day, int32 Year, int32 SeasonIndex)
	{
		FVoxelSeedCalendar C;
		C.Day = Day;
		C.Year = Year;
		C.Season = VoxelUIStrings::SeasonLabel(SeasonIndex);
		return C;
	};

	// THE DEFECT THIS FILE WAS EXTENDED FOR. The owner's session was on day 11
	// and the newest placeholder card was stamped day 12: an entry from tomorrow,
	// in a journal a player cannot tell from a real one. The invariant is not
	// "the newest card says 11" but "no card is dated after today", which is what
	// stays true when somebody changes the fixture's shape.
	{
		const FVoxelJournalData Data = SeedJournal(FText::GetEmpty(), Calendar(11, 1, 1));
		TestEqual(TEXT("three placeholder cards"), Data.Entries.Num(), 3);
		for (const FVoxelJournalEntry& E : Data.Entries)
		{
			TestTrue(TEXT("no entry is dated after the live day"), E.Day <= 11);
			TestTrue(TEXT("no entry is dated before day one"), E.Day >= 1);
		}
		TestEqual(TEXT("the newest card IS today"), Data.Entries[0].Day, 11);
		TestEqual(TEXT("and it is stamped with today's calendar"),
		          Data.Entries[0].Stamp.ToString(),
		          FString(TEXT("Day 11 · Summer, 1st Year of the Second Age")));
	}

	// A RE-ANCHORING, NOT A REWRITE. On a world that really is on day 12 the
	// three cards are the mock's own 12 / 9 / 1, which is what makes it safe to
	// compare a capture taken after this change against one taken before it.
	{
		const FVoxelJournalData Data = SeedJournal(FText::GetEmpty(), Calendar(12, 18, 1));
		TestEqual(TEXT("newest is the mock's 12"), Data.Entries[0].Day, 12);
		TestEqual(TEXT("middle is the mock's 9"), Data.Entries[1].Day, 9);
		TestEqual(TEXT("oldest is the mock's 1"), Data.Entries[2].Day, 1);
		TestEqual(TEXT("and the mock's own stamp comes back"),
		          Data.Entries[0].Stamp.ToString(),
		          FString(TEXT("Day 12 · Summer, 18th Year of the Second Age")));
	}

	// A WORLD YOUNGER THAN THE FIXTURE'S SPACING. Day 2 collapses the middle card
	// onto the first; a duplicate date is deliberate and an entry dated after
	// today is not. Day 1 is the tightest case there is.
	{
		const FVoxelJournalData Data = SeedJournal(FText::GetEmpty(), Calendar(2, 1, 3));
		TestEqual(TEXT("newest follows the live day down"), Data.Entries[0].Day, 2);
		TestTrue(TEXT("the middle card cannot go below day one"), Data.Entries[1].Day >= 1);
		TestTrue(TEXT("nothing is dated after day two"),
		         Data.Entries[0].Day <= 2 && Data.Entries[1].Day <= 2 && Data.Entries[2].Day <= 2);
	}
	{
		const FVoxelJournalData Data = SeedJournal(FText::GetEmpty(), Calendar(1, 1, 0));
		for (const FVoxelJournalEntry& E : Data.Entries)
		{
			TestTrue(TEXT("on day one every card is day one"), E.Day == 1);
		}
	}

	// NO CALENDAR AT ALL -- an unattended capture leg with no sky subsystem. The
	// fixture falls back to the mock's 12 / Summer / 18 rather than dating
	// everything day zero, because a -VoxelScreenShot of the journal has to
	// photograph a journal with dates in it.
	{
		const FVoxelJournalData Data = SeedJournal(FText::GetEmpty(), FVoxelSeedCalendar());
		TestEqual(TEXT("falls back to the mock's newest day"), Data.Entries[0].Day, 12);
		TestEqual(TEXT("and to the mock's whole stamp"),
		          Data.Entries[0].Stamp.ToString(),
		          FString(TEXT("Day 12 · Summer, 18th Year of the Second Age")));
	}

	// DETERMINISTIC. Same calendar in, same journal out -- the property that lets
	// a capture of this screen be compared with an earlier one at all.
	{
		const FVoxelJournalData A = SeedJournal(FText::GetEmpty(), Calendar(7, 3, 2));
		const FVoxelJournalData B = SeedJournal(FText::GetEmpty(), Calendar(7, 3, 2));
		TestEqual(TEXT("same card count"), A.Entries.Num(), B.Entries.Num());
		for (int32 I = 0; I < A.Entries.Num(); ++I)
		{
			TestEqual(TEXT("same day"), A.Entries[I].Day, B.Entries[I].Day);
			TestEqual(TEXT("same stamp"), A.Entries[I].Stamp.ToString(), B.Entries[I].Stamp.ToString());
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
