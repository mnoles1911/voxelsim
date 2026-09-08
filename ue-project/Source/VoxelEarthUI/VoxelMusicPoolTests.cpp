// Headless tests for the music system's pure logic (VoxelMusicPools.h).
//
// WHAT IS AND IS NOT COVERED. Nothing here plays a sound: FVoxelUIMusic needs
// a UWorld, an audio device and 17-92 MB of decoded PCM, none of which exists
// on a -nullrhi automation run. What CAN be asserted is every decision
// docs/music-design.md actually argues about -- which pool ducks which, where
// the hour bands fall, which slot borrows from which, how long the authored
// silence is, and that a shuffle cannot repeat until it is exhausted -- and
// those are exactly the things a refactor breaks silently.
//
// EACH TEST CAN FAIL, and that is checkable by hand: change one boundary in
// VoxelMusicPools.cpp's kSlotBounds, one rung of the ladder in
// VoxelMusicResolvePool, or one number in VoxelMusicGapRange, and a named
// assertion below goes red. The assertions are written against the DOCUMENT's
// numbers rather than read back out of the table, so a table edit that was not
// also a design change is caught rather than agreed with.
//
// ENUMS ARE COMPARED BY NAME, not by value. FAutomationTestBase::TestEqual has
// no overload that can print an `enum class`, and an assertion that fails with
// "expected 3, got 4" is an assertion somebody has to go and decode.
//
// Run headlessly:
//   UnrealEditor-Cmd.exe VoxelEarth.uproject -unattended -nullrhi -nop4 \
//     -ExecCmds="Automation RunTests VoxelEarth.Music; Quit"

#include "VoxelMusicPools.h"

#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace VoxelMusicPoolTestsDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.
//
// EAutomationTestFlags is a strongly-typed enum class in 5.8, not an int
// bitmask, so this must keep the enum type all the way through -- the same note
// VoxelFrontEndTests.cpp and VoxelSkyTests.cpp carry.
constexpr EAutomationTestFlags kTestFlags = EAutomationTestFlags::EditorContext
                                          | EAutomationTestFlags::ClientContext
                                          | EAutomationTestFlags::EngineFilter;

FString SlotName(EVoxelMusicSlot Slot)
{
	return FString(VoxelMusicSlotName(Slot));
}

FString PoolName(EVoxelMusicPool Pool)
{
	return FString(VoxelMusicPoolName(Pool));
}

// A signals struct with nothing set: the player standing in the world with no
// boat, no cave, no town and no threat. Every priority test below is this plus
// one field, which is what makes each assertion about exactly one rule.
FVoxelMusicSignals InWorld()
{
	FVoxelMusicSignals S;
	S.bInWorld = true;
	return S;
}
} // namespace VoxelMusicPoolTestsDetail

// --- Section 4: the hour boundaries ------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMusicSlotTest, "VoxelEarth.Music.SlotFromHour",
                                 VoxelMusicPoolTestsDetail::kTestFlags)

bool FVoxelMusicSlotTest::RunTest(const FString& Parameters)
{
	using VoxelMusicPoolTestsDetail::SlotName;

	// docs/music-design.md section 4, read straight off the table:
	//   Dawn 05:00-07:00, Day 07:00-18:00, Dusk 18:00-20:00, Night 20:00-05:00.
	//
	// EACH BOUNDARY IS TESTED FROM BOTH SIDES, because an off-by-one in a
	// half-open interval is the whole failure mode here and a single-sided test
	// cannot see it.
	TestEqual(TEXT("04:59 is night"), SlotName(VoxelMusicSlotFromHour(4.99)), FString(TEXT("Night")));
	TestEqual(TEXT("05:00 is dawn"), SlotName(VoxelMusicSlotFromHour(5.0)), FString(TEXT("Dawn")));
	TestEqual(TEXT("06:59 is dawn"), SlotName(VoxelMusicSlotFromHour(6.99)), FString(TEXT("Dawn")));
	TestEqual(TEXT("07:00 is day"), SlotName(VoxelMusicSlotFromHour(7.0)), FString(TEXT("Day")));
	TestEqual(TEXT("17:59 is day"), SlotName(VoxelMusicSlotFromHour(17.99)), FString(TEXT("Day")));
	TestEqual(TEXT("18:00 is dusk"), SlotName(VoxelMusicSlotFromHour(18.0)), FString(TEXT("Dusk")));
	TestEqual(TEXT("19:59 is dusk"), SlotName(VoxelMusicSlotFromHour(19.99)), FString(TEXT("Dusk")));
	TestEqual(TEXT("20:00 is night"), SlotName(VoxelMusicSlotFromHour(20.0)), FString(TEXT("Night")));

	// THE NIGHT BAND WRAPS MIDNIGHT and is the one slot that appears twice in
	// the table. Midnight and midday are the two hours nobody would think to
	// check and the two a broken wrap gets wrong.
	TestEqual(TEXT("midnight is night"), SlotName(VoxelMusicSlotFromHour(0.0)), FString(TEXT("Night")));
	TestEqual(TEXT("midday is day"), SlotName(VoxelMusicSlotFromHour(12.0)), FString(TEXT("Day")));
	TestEqual(TEXT("23:59 is night"), SlotName(VoxelMusicSlotFromHour(23.99)), FString(TEXT("Night")));

	// A clock that has run past 24 h, and one that has gone negative. Both are
	// wrapped rather than clamped: the sky epoch accumulates, and -VoxelTimeOfDay
	// can be handed anything.
	TestEqual(TEXT("25:00 wraps to 01:00, which is night"),
	          SlotName(VoxelMusicSlotFromHour(25.0)), FString(TEXT("Night")));
	TestEqual(TEXT("32:00 wraps to 08:00, which is day"),
	          SlotName(VoxelMusicSlotFromHour(32.0)), FString(TEXT("Day")));
	TestEqual(TEXT("-2:00 wraps to 22:00, which is night"),
	          SlotName(VoxelMusicSlotFromHour(-2.0)), FString(TEXT("Night")));
	TestEqual(TEXT("-6:00 wraps to 18:00, which is dusk"),
	          SlotName(VoxelMusicSlotFromHour(-6.0)), FString(TEXT("Dusk")));

	// A sky subsystem that never ticked hands out garbage. Day is the widest
	// pool and the only harmless answer; the alternative is silence at noon.
	TestEqual(TEXT("a NaN clock falls back to day"),
	          SlotName(VoxelMusicSlotFromHour(std::numeric_limits<double>::quiet_NaN())),
	          FString(TEXT("Day")));
	TestEqual(TEXT("an infinite clock falls back to day"),
	          SlotName(VoxelMusicSlotFromHour(std::numeric_limits<double>::infinity())),
	          FString(TEXT("Day")));

	return true;
}

// --- Section 4: borrowing ----------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMusicBorrowTest, "VoxelEarth.Music.SlotBorrowing",
                                 VoxelMusicPoolTestsDetail::kTestFlags)

bool FVoxelMusicBorrowTest::RunTest(const FString& Parameters)
{
	using VoxelMusicPoolTestsDetail::SlotName;

	// "Dawn borrows Day, Dusk borrows Day then Night, Night borrows Dusk. Day
	// never borrows." Four sentences, four blocks, in the document's order.
	EVoxelMusicSlot Chain[kVoxelMusicSlotChainMax];

	int32 Num = VoxelMusicSlotChain(/*bRaining=*/false, /*LocalHours=*/12.0, Chain);
	TestEqual(TEXT("Day never borrows: its chain is one long"), Num, 1);
	TestEqual(TEXT("Day's chain is Day"), SlotName(Chain[0]), FString(TEXT("Day")));

	Num = VoxelMusicSlotChain(false, 6.0, Chain);
	TestEqual(TEXT("Dawn borrows exactly one slot"), Num, 2);
	TestEqual(TEXT("Dawn leads its own chain"), SlotName(Chain[0]), FString(TEXT("Dawn")));
	TestEqual(TEXT("Dawn borrows Day"), SlotName(Chain[1]), FString(TEXT("Day")));

	Num = VoxelMusicSlotChain(false, 19.0, Chain);
	TestEqual(TEXT("Dusk borrows two slots"), Num, 3);
	TestEqual(TEXT("Dusk leads its own chain"), SlotName(Chain[0]), FString(TEXT("Dusk")));
	TestEqual(TEXT("Dusk borrows Day first"), SlotName(Chain[1]), FString(TEXT("Day")));
	TestEqual(TEXT("Dusk borrows Night second"), SlotName(Chain[2]), FString(TEXT("Night")));

	Num = VoxelMusicSlotChain(false, 23.0, Chain);
	TestEqual(TEXT("Night borrows Dusk then Day as the floor"), Num, 3);
	TestEqual(TEXT("Night leads its own chain"), SlotName(Chain[0]), FString(TEXT("Night")));
	TestEqual(TEXT("Night borrows Dusk first"), SlotName(Chain[1]), FString(TEXT("Dusk")));
	TestEqual(TEXT("Night falls to Day only as the last link"), SlotName(Chain[2]), FString(TEXT("Day")));

	// SECTION 7'S RAIN LAYER: "played in place of Day/Night when raining". It
	// LEADS the chain rather than replacing it, so an empty Explore/Rain folder
	// -- which is what ships -- falls through to the clock rather than to
	// silence.
	Num = VoxelMusicSlotChain(/*bRaining=*/true, 12.0, Chain);
	TestEqual(TEXT("rain over Day is two long"), Num, 2);
	TestEqual(TEXT("rain leads the chain"), SlotName(Chain[0]), FString(TEXT("Rain")));
	TestEqual(TEXT("rain falls back to the hour's own slot"), SlotName(Chain[1]), FString(TEXT("Day")));

	Num = VoxelMusicSlotChain(true, 19.0, Chain);
	TestEqual(TEXT("rain at dusk keeps the whole dusk chain behind it"), Num, 4);
	TestEqual(TEXT("rain at dusk still leads with Rain"), SlotName(Chain[0]), FString(TEXT("Rain")));
	TestEqual(TEXT("rain at dusk keeps Dusk second"), SlotName(Chain[1]), FString(TEXT("Dusk")));

	// Nothing may overrun the caller's array. The longest chain the document
	// allows is Rain plus Dusk's three; the buffer is sized for six.
	TestTrue(TEXT("no chain overruns its buffer"), Num <= kVoxelMusicSlotChainMax);

	// The borrow threshold is the document's four, and the implementation reads
	// it from this one constant rather than from a literal.
	TestEqual(TEXT("a slot borrows below four cues"), kVoxelMusicSlotMinCues, 4);

	return true;
}

// --- Section 2: priority -----------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMusicPriorityTest, "VoxelEarth.Music.PoolPriority",
                                 VoxelMusicPoolTestsDetail::kTestFlags)

bool FVoxelMusicPriorityTest::RunTest(const FString& Parameters)
{
	using VoxelMusicPoolTestsDetail::InWorld;
	using VoxelMusicPoolTestsDetail::PoolName;

	// 1. THE BASE CASES. No world is the Menu pool ("title and loading
	// screens"); a world with nothing else going on is Explore ("everything
	// else: walking, building, boating on foot").
	TestEqual(TEXT("no world is the menu"),
	          PoolName(VoxelMusicResolvePool(FVoxelMusicSignals())), FString(TEXT("Menu")));
	TestEqual(TEXT("a plain world is explore"),
	          PoolName(VoxelMusicResolvePool(InWorld())), FString(TEXT("Explore")));

	// 2. EACH POOL BEATS EXPLORE ON ITS OWN. One field at a time, so a failure
	// names the rule that broke.
	{
		FVoxelMusicSignals S = InWorld();
		S.bAboard = true;
		TestEqual(TEXT("aboard a vessel is water"), PoolName(VoxelMusicResolvePool(S)),
		          FString(TEXT("Water")));
	}
	{
		FVoxelMusicSignals S = InWorld();
		S.bUnderground = true;
		TestEqual(TEXT("the veil is cave"), PoolName(VoxelMusicResolvePool(S)),
		          FString(TEXT("Cave")));
	}
	{
		FVoxelMusicSignals S = InWorld();
		S.bInTown = true;
		TestEqual(TEXT("a settlement is town"), PoolName(VoxelMusicResolvePool(S)),
		          FString(TEXT("Town")));
	}
	{
		FVoxelMusicSignals S = InWorld();
		S.bCombat = true;
		TestEqual(TEXT("a threat is combat"), PoolName(VoxelMusicResolvePool(S)),
		          FString(TEXT("Combat")));
	}
	{
		FVoxelMusicSignals S = InWorld();
		S.bStinger = true;
		TestEqual(TEXT("an event is a stinger"), PoolName(VoxelMusicResolvePool(S)),
		          FString(TEXT("Stingers")));
	}

	// 3. THE PRIORITY-3 TIE, WHICH IS THE ONE THE DOCUMENT SPELLS OUT: "Water
	// beats Cave beats Town (a boat in a flooded cavern is still a boat)".
	{
		FVoxelMusicSignals S = InWorld();
		S.bAboard = true;
		S.bUnderground = true;
		TestEqual(TEXT("a boat in a flooded cavern is still a boat"),
		          PoolName(VoxelMusicResolvePool(S)), FString(TEXT("Water")));
	}
	{
		FVoxelMusicSignals S = InWorld();
		S.bUnderground = true;
		S.bInTown = true;
		TestEqual(TEXT("a cellar under a town is a cave"),
		          PoolName(VoxelMusicResolvePool(S)), FString(TEXT("Cave")));
	}
	{
		FVoxelMusicSignals S = InWorld();
		S.bAboard = true;
		S.bInTown = true;
		TestEqual(TEXT("a boat at a town quay is a boat"),
		          PoolName(VoxelMusicResolvePool(S)), FString(TEXT("Water")));
	}

	// 4. HIGHER DUCKS LOWER, ALL THE WAY UP. Everything on at once resolves to
	// the top of the ladder; peel the top off and the next rung takes it.
	{
		FVoxelMusicSignals S = InWorld();
		S.bStinger = S.bCombat = S.bAboard = S.bUnderground = S.bInTown = true;
		TestEqual(TEXT("a stinger ducks everything"),
		          PoolName(VoxelMusicResolvePool(S)), FString(TEXT("Stingers")));
		S.bStinger = false;
		TestEqual(TEXT("combat ducks the three location pools"),
		          PoolName(VoxelMusicResolvePool(S)), FString(TEXT("Combat")));
		S.bCombat = false;
		TestEqual(TEXT("water is the most specific of the three"),
		          PoolName(VoxelMusicResolvePool(S)), FString(TEXT("Water")));
		S.bAboard = false;
		TestEqual(TEXT("cave is next"), PoolName(VoxelMusicResolvePool(S)), FString(TEXT("Cave")));
		S.bUnderground = false;
		TestEqual(TEXT("town is last of the three"),
		          PoolName(VoxelMusicResolvePool(S)), FString(TEXT("Town")));
		S.bInTown = false;
		TestEqual(TEXT("and then explore"),
		          PoolName(VoxelMusicResolvePool(S)), FString(TEXT("Explore")));
	}

	// 5. EXPLORE OUTRANKS MENU, which is priority 4 against 5 and only visible
	// when both could apply. A location signal with no world is a contradiction
	// the caller should never produce -- but if it does, the ladder must still
	// be the ladder rather than falling through to the menu.
	{
		FVoxelMusicSignals S; // bInWorld deliberately false
		S.bAboard = true;
		TestEqual(TEXT("a location signal outranks the menu even with no world"),
		          PoolName(VoxelMusicResolvePool(S)), FString(TEXT("Water")));
	}

	// 6. CINEMATIC IS NEVER RESOLVED TO. "Never in any cycling pool" -- there
	// is no combination of signals that produces it.
	{
		FVoxelMusicSignals S = InWorld();
		S.bStinger = S.bCombat = S.bAboard = S.bUnderground = S.bInTown = S.bRaining = true;
		TestTrue(TEXT("no signal combination reaches Cinematic"),
		         VoxelMusicResolvePool(S) != EVoxelMusicPool::Cinematic);
	}

	// 7. THE ENUM'S DECLARATION ORDER IS THE PRIORITY ORDER, which is what
	// FVoxelUIMusic::ChooseBank walks when it steps past an empty pool. If
	// somebody reorders the enum for tidiness the fall-through order changes
	// underneath it, and nothing else in the project would say so.
	TestTrue(TEXT("Stingers outranks Combat in declaration order"),
	         EVoxelMusicPool::Stingers < EVoxelMusicPool::Combat);
	TestTrue(TEXT("Combat outranks Water"), EVoxelMusicPool::Combat < EVoxelMusicPool::Water);
	TestTrue(TEXT("Water outranks Cave"), EVoxelMusicPool::Water < EVoxelMusicPool::Cave);
	TestTrue(TEXT("Cave outranks Town"), EVoxelMusicPool::Cave < EVoxelMusicPool::Town);
	TestTrue(TEXT("Town outranks Explore"), EVoxelMusicPool::Town < EVoxelMusicPool::Explore);
	TestTrue(TEXT("Explore outranks Menu"), EVoxelMusicPool::Explore < EVoxelMusicPool::Menu);

	return true;
}

// --- Section 3: authored silence ---------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMusicGapTest, "VoxelEarth.Music.Gaps",
                                 VoxelMusicPoolTestsDetail::kTestFlags)

bool FVoxelMusicGapTest::RunTest(const FString& Parameters)
{
	// "Explore, Cave, Town, Water: a randomised gap of 45-120 s between cues."
	// "Menu: a gap of 20-40 s." "Combat: no gap while the threat lasts."
	const EVoxelMusicPool Long[] = { EVoxelMusicPool::Explore, EVoxelMusicPool::Cave,
	                                 EVoxelMusicPool::Town, EVoxelMusicPool::Water };
	for (const EVoxelMusicPool Pool : Long)
	{
		float Min = 0.f, Max = 0.f;
		VoxelMusicGapRange(Pool, Min, Max);
		TestEqual(*FString::Printf(TEXT("the %s gap starts at 45 s"), VoxelMusicPoolName(Pool)),
		          Min, 45.f);
		TestEqual(*FString::Printf(TEXT("the %s gap ends at 120 s"), VoxelMusicPoolName(Pool)),
		          Max, 120.f);
	}

	float Min = 0.f, Max = 0.f;
	VoxelMusicGapRange(EVoxelMusicPool::Menu, Min, Max);
	TestEqual(TEXT("the menu gap starts at 20 s"), Min, 20.f);
	TestEqual(TEXT("the menu gap ends at 40 s"), Max, 40.f);

	VoxelMusicGapRange(EVoxelMusicPool::Combat, Min, Max);
	TestEqual(TEXT("combat has no gap"), Max, 0.f);
	VoxelMusicGapRange(EVoxelMusicPool::Stingers, Min, Max);
	TestEqual(TEXT("a stinger has no gap"), Max, 0.f);

	// EVERY DRAW LANDS IN ITS RANGE, and the range is actually SAMPLED rather
	// than pinned to one end -- a draw that always returned the minimum would
	// pass a bounds check and would be exactly the "silence is authored" defect
	// this section exists to prevent. Two hundred draws over a 75 s span cannot
	// all miss one half unless the draw is constant.
	FRandomStream Stream(20260908);
	bool bBelowMidpoint = false;
	bool bAboveMidpoint = false;
	for (int32 I = 0; I < 200; ++I)
	{
		const float Gap = VoxelMusicDrawGap(EVoxelMusicPool::Explore, Stream);
		TestTrue(TEXT("an explore gap is never under 45 s"), Gap >= 45.f);
		TestTrue(TEXT("an explore gap is never over 120 s"), Gap <= 120.f);
		bBelowMidpoint = bBelowMidpoint || Gap < 82.5f;
		bAboveMidpoint = bAboveMidpoint || Gap > 82.5f;
	}
	TestTrue(TEXT("the draw reaches the short half of the range"), bBelowMidpoint);
	TestTrue(TEXT("the draw reaches the long half of the range"), bAboveMidpoint);

	for (int32 I = 0; I < 50; ++I)
	{
		const float Gap = VoxelMusicDrawGap(EVoxelMusicPool::Menu, Stream);
		TestTrue(TEXT("a menu gap is never under 20 s"), Gap >= 20.f);
		TestTrue(TEXT("a menu gap is never over 40 s"), Gap <= 40.f);
	}
	TestEqual(TEXT("a combat draw is zero, not a small random number"),
	          VoxelMusicDrawGap(EVoxelMusicPool::Combat, Stream), 0.f);

	// Section 2's crossfade is "2-4 s, never a hard cut". The shipped constant
	// has to be inside that band or the sentence is not implemented.
	TestTrue(TEXT("the crossfade is at least 2 s"), kVoxelMusicCrossfadeSeconds >= 2.f);
	TestTrue(TEXT("the crossfade is at most 4 s"), kVoxelMusicCrossfadeSeconds <= 4.f);

	// Section 3's combat cooldown tail, "10-20 s ... before Explore resumes".
	TestEqual(TEXT("the cooldown tail starts at 10 s"), kVoxelMusicCombatCooldownMin, 10.f);
	TestEqual(TEXT("the cooldown tail ends at 20 s"), kVoxelMusicCombatCooldownMax, 20.f);

	return true;
}

// --- Section 2: no repeat until exhausted ------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMusicShuffleTest, "VoxelEarth.Music.Shuffle",
                                 VoxelMusicPoolTestsDetail::kTestFlags)

bool FVoxelMusicShuffleTest::RunTest(const FString& Parameters)
{
	constexpr int32 Count = 11; // the shipped Explore/Day bank

	// 1. ONE FULL CYCLE VISITS EVERY CUE EXACTLY ONCE. This is the whole
	// content of "shuffled, no repeat until exhausted", and a per-cue random
	// roll -- the obvious wrong implementation -- fails it within a few draws.
	{
		FRandomStream Stream(1);
		FVoxelMusicShuffle Shuffle;
		TArray<int32> Seen;
		for (int32 I = 0; I < Count; ++I)
		{
			const int32 Pick = Shuffle.Advance(Count, Stream);
			TestTrue(TEXT("every pick is in range"), Pick >= 0 && Pick < Count);
			TestFalse(TEXT("no cue repeats inside one cycle"), Seen.Contains(Pick));
			Seen.Add(Pick);
		}
		TestEqual(TEXT("one cycle is the whole bank"), Seen.Num(), Count);
	}

	// 2. THE SEAM. The last cue of one cycle must not be the first of the next
	// -- the one place a permutation on its own still repeats, and the one a
	// listener actually notices. Fifty cycles, because the seam is only wrong
	// about one time in Count and a single pass would usually miss it.
	{
		FRandomStream Stream(7);
		FVoxelMusicShuffle Shuffle;
		int32 Previous = INDEX_NONE;
		for (int32 Cycle = 0; Cycle < 50; ++Cycle)
		{
			for (int32 I = 0; I < Count; ++I)
			{
				const int32 Pick = Shuffle.Advance(Count, Stream);
				TestTrue(TEXT("no cue plays twice running, across the seam included"),
				         Pick != Previous);
				Previous = Pick;
			}
		}
	}

	// 3. IT IS A SHUFFLE, NOT A COUNTER. An implementation that simply walked
	// 0,1,2,... would pass both tests above. Over five cycles at least one
	// permutation must differ from the identity order.
	{
		FRandomStream Stream(99);
		FVoxelMusicShuffle Shuffle;
		bool bAnyOutOfOrder = false;
		for (int32 Cycle = 0; Cycle < 5; ++Cycle)
		{
			for (int32 I = 0; I < Count; ++I)
			{
				bAnyOutOfOrder = bAnyOutOfOrder || (Shuffle.Advance(Count, Stream) != I);
			}
		}
		TestTrue(TEXT("the order is shuffled, not sequential"), bAnyOutOfOrder);
	}

	// 4. A SEEDED RUN IS REPRODUCIBLE. The menu art and the music are drawn
	// from the same seeded stream so a capture pairs the same picture with the
	// same cue; that only holds if the draw is a pure function of the seed.
	{
		FRandomStream A(4242), B(4242);
		FVoxelMusicShuffle ShuffleA, ShuffleB;
		for (int32 I = 0; I < Count * 3; ++I)
		{
			TestEqual(TEXT("the same seed draws the same order"),
			          ShuffleA.Advance(Count, A), ShuffleB.Advance(Count, B));
		}
	}

	// 5. THE DEGENERATE BANKS. One cue (Explore/Night and Menu ship exactly
	// that) must keep playing rather than wedging, and an empty bank must
	// report nothing rather than index off the end.
	{
		FRandomStream Stream(3);
		FVoxelMusicShuffle Shuffle;
		TestEqual(TEXT("an empty bank has no pick"), Shuffle.Advance(0, Stream), int32(INDEX_NONE));
		for (int32 I = 0; I < 5; ++I)
		{
			TestEqual(TEXT("a one-cue bank keeps returning its one cue"),
			          Shuffle.Advance(1, Stream), 0);
		}
	}

	// 6. RETREAT IS "BACK", NOT "WRAP". It walks back inside the permutation the
	// listener actually heard and stops at its start; the previous cycle's order
	// was thrown away when it was re-drawn, so wrapping would walk into an order
	// nobody ever heard.
	{
		FRandomStream Stream(11);
		FVoxelMusicShuffle Shuffle;
		const int32 First = Shuffle.Advance(Count, Stream);
		const int32 Second = Shuffle.Advance(Count, Stream);
		TestEqual(TEXT("back from the second cue is the first"), Shuffle.Retreat(), First);
		TestEqual(TEXT("back from the first cue stays on the first"), Shuffle.Retreat(), First);
		TestEqual(TEXT("forward again is the second cue"), Shuffle.Advance(Count, Stream), Second);
	}

	// 7. THE BANK CHANGED UNDER US. A designer dropping a cue in mid-session
	// must not produce an out-of-range index.
	{
		FRandomStream Stream(5);
		FVoxelMusicShuffle Shuffle;
		Shuffle.Advance(4, Stream);
		const int32 Pick = Shuffle.Advance(9, Stream);
		TestTrue(TEXT("a grown bank re-draws rather than indexing off the end"),
		         Pick >= 0 && Pick < 9);
	}

	return true;
}

// --- Section 8: the cross-session memory and the folder table ----------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelMusicRecentsTest, "VoxelEarth.Music.RecentsAndBanks",
                                 VoxelMusicPoolTestsDetail::kTestFlags)

bool FVoxelMusicRecentsTest::RunTest(const FString& Parameters)
{
	using VoxelMusicPoolTestsDetail::PoolName;

	// "Remember the last five cues played per pool so a relaunch does not open
	// on the same one."
	TestEqual(TEXT("the memory is five cues deep"), kVoxelMusicRecentMemory, 5);

	const TArray<FString> Names = { TEXT("A"), TEXT("B"), TEXT("C"), TEXT("D") };
	const TArray<int32> Order = { 2, 0, 3, 1 }; // C, A, D, B

	// Nothing remembered: the permutation opens where it was drawn.
	TestEqual(TEXT("with no memory the first pick stands"),
	          VoxelMusicFirstNotRecent(Names, Order, TArray<FString>()), 0);

	// The cue the last session ended on is skipped, and the NEXT position in
	// the drawn order is used -- not a re-roll, which would throw the
	// permutation away.
	TestEqual(TEXT("a remembered opener is skipped"),
	          VoxelMusicFirstNotRecent(Names, Order, TArray<FString>({ TEXT("C") })), 1);
	TestEqual(TEXT("two remembered openers are both skipped"),
	          VoxelMusicFirstNotRecent(Names, Order, TArray<FString>({ TEXT("C"), TEXT("A") })), 2);

	// A remembered cue that is NOT at the front changes nothing.
	TestEqual(TEXT("a remembered cue later in the order is left alone"),
	          VoxelMusicFirstNotRecent(Names, Order, TArray<FString>({ TEXT("B") })), 0);

	// EVERY CUE REMEMBERED -- a four-cue bank after four plays. Opening on
	// something heard last session is better than refusing to open.
	TestEqual(TEXT("an exhausted memory still opens"),
	          VoxelMusicFirstNotRecent(Names, Order,
	                                   TArray<FString>({ TEXT("A"), TEXT("B"), TEXT("C"), TEXT("D") })),
	          0);

	// --- The folder table (section 8) ---------------------------------------
	//
	// Twelve folders, and every one addressable by (pool, slot). A missing row
	// means FVoxelUIMusic::ChooseBank silently never plays that pool, which is
	// exactly the class of failure that has no symptom.
	const TArrayView<const FVoxelMusicBank> Banks = VoxelMusicBanks();
	TestEqual(TEXT("there are twelve pool folders"), Banks.Num(), 12);
	for (const FVoxelMusicBank& Bank : Banks)
	{
		TestTrue(TEXT("every folder is addressable by pool and slot"),
		         VoxelMusicBankIndex(Bank.Pool, Bank.Slot) != INDEX_NONE);
	}
	// Explore is the only pool with slots; the other seven are one folder each.
	TestTrue(TEXT("Explore/Day exists"),
	         VoxelMusicBankIndex(EVoxelMusicPool::Explore, EVoxelMusicSlot::Day) != INDEX_NONE);
	TestTrue(TEXT("Explore/Rain exists"),
	         VoxelMusicBankIndex(EVoxelMusicPool::Explore, EVoxelMusicSlot::Rain) != INDEX_NONE);
	TestTrue(TEXT("Cave is slotless"),
	         VoxelMusicBankIndex(EVoxelMusicPool::Cave, EVoxelMusicSlot::None) != INDEX_NONE);
	TestEqual(TEXT("there is no Cave/Day"),
	          VoxelMusicBankIndex(EVoxelMusicPool::Cave, EVoxelMusicSlot::Day), int32(INDEX_NONE));

	// -VoxelMusicPool= parses the same eight names the selection log prints.
	EVoxelMusicPool Parsed = EVoxelMusicPool::Menu;
	TestTrue(TEXT("Explore parses"), VoxelMusicPoolFromName(TEXT("Explore"), Parsed));
	TestEqual(TEXT("Explore parses to Explore"), PoolName(Parsed), FString(TEXT("Explore")));
	TestTrue(TEXT("the parse is case-insensitive, unlike a switch name"),
	         VoxelMusicPoolFromName(TEXT("cave"), Parsed));
	TestEqual(TEXT("cave parses to Cave"), PoolName(Parsed), FString(TEXT("Cave")));
	TestFalse(TEXT("a name that is not a pool is refused"),
	          VoxelMusicPoolFromName(TEXT("Nonsense"), Parsed));
	TestFalse(TEXT("the enum's Count sentinel is not a pool name"),
	          VoxelMusicPoolFromName(TEXT("Count"), Parsed));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
