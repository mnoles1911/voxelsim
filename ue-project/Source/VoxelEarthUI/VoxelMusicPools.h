#pragma once
// The music system's PURE HALF: which pool owns the music, which time-of-day
// slot Explore draws from, how long the silence between cues is, and the
// no-repeat shuffle. Everything here is a function of its arguments -- no
// UWorld, no audio device, no file system -- which is what lets
// VoxelMusicPoolTests.cpp cover it on a machine with no display.
//
// docs/music-design.md is the spec. Sections 2 (pools and priority), 3
// (authored silence), 4 (time of day) and 6 (the folder layout) are
// implemented here; section 8 is the mechanics they add up to. Where a number
// below has a section reference beside it, that section is the authority and
// this file is the only copy.
//
// WHY A SEPARATE FILE FROM FVoxelUIMusic. That class owns a UObject, a
// critical section, an audio-render-thread callback and 30-92 MB of decoded
// PCM; none of that can be constructed in a headless automation test. The
// decisions the design document actually argues about -- priority, boundaries,
// gap ranges, borrowing -- are all pure, so they live where they can be
// asserted.

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

struct FRandomStream;

// --- Pools, in the priority order of docs/music-design.md section 2 ----------
//
// DECLARATION ORDER IS PRIORITY ORDER, highest first, and VoxelMusicResolvePool
// walks it in that order. Cinematic is last and outside the ladder entirely: it
// is "never in any cycling pool" and nothing resolves to it.
enum class EVoxelMusicPool : uint8
{
	Stingers = 0, // 1: an event fires. Plays over whatever is running.
	Combat,       // 2: a threat is active.
	Water,        // 3: aboard a vessel.       -- the three at priority 3 tie,
	Cave,         // 3: the underground veil.     and resolve by SPECIFICITY in
	Town,         // 3: inside a settlement.      this order (section 2).
	Explore,      // 4: everything else, filtered by time of day.
	Menu,         // 5: title and loading screens.
	Cinematic,    // -: scripted moments only; never cycled.
	Count
};

// The time-of-day slots Explore is filtered by (section 4), plus Rain, which
// section 7 describes as an optional layer "played in place of Day/Night when
// raining" rather than as a fifth hour band.
enum class EVoxelMusicSlot : uint8
{
	None = 0, // every pool except Explore
	Dawn,
	Day,
	Dusk,
	Night,
	Rain,
	Count
};

VOXELEARTHUI_API const TCHAR* VoxelMusicPoolName(EVoxelMusicPool Pool);
VOXELEARTHUI_API const TCHAR* VoxelMusicSlotName(EVoxelMusicSlot Slot);
// Empty when the name is not one of the above. Used by -VoxelMusicPool= only.
VOXELEARTHUI_API bool VoxelMusicPoolFromName(const FString& Name, EVoxelMusicPool& Out);

// --- One bank per FOLDER (section 8) ----------------------------------------
//
// "Subfolders, not filename prefixes ... The player builds one shuffled
// playlist per folder." A BANK is that folder and its playlist. A POOL is the
// priority-carrying group; only Explore has more than one bank, because only
// Explore is filtered by time of day.
struct FVoxelMusicBank
{
	EVoxelMusicPool Pool;
	EVoxelMusicSlot Slot;
	// Relative to Content/Audio/Music, forward-slashed.
	const TCHAR* Folder;
};

// The layout of Content/Audio/Music, in one table. Adding a pool folder is one
// row here plus one folder on disk.
VOXELEARTHUI_API TArrayView<const FVoxelMusicBank> VoxelMusicBanks();

// Index into VoxelMusicBanks(), or INDEX_NONE.
VOXELEARTHUI_API int32 VoxelMusicBankIndex(EVoxelMusicPool Pool, EVoxelMusicSlot Slot);

// --- The signals the pools are selected by ----------------------------------
//
// A PLAIN STRUCT SO THE RESOLUTION IS TESTABLE. Reading these off the running
// world is FVoxelUIMusic::ReadSignals; deciding what they mean is
// VoxelMusicResolvePool, and only one of those two can be asserted headlessly.
struct FVoxelMusicSignals
{
	// An event has fired a one-shot (defeat, discovery, boss reveal).
	// NOTHING SETS THIS TODAY: stingers are owner-deferred backlog
	// (docs/backlog.md 15c) and no event in this project fires one.
	bool bStinger = false;

	// A threat is active. STUBBED FALSE: this project has no combat, no
	// damage, no enemy actor and no threat signal of any kind, so the Combat
	// pool is wired to a predicate that cannot fire. See
	// VoxelUIMusicDetail::IsCombatThreatActive.
	bool bCombat = false;

	// The player's pawn is an AVoxelBoat. Real signal.
	bool bAboard = false;

	// AVoxelClipmapActor's underground veil latch. Real signal.
	bool bUnderground = false;

	// Inside a settlement's bounds. STUBBED FALSE: there are no settlements
	// and no bounds signal (section 2 says "when settlements exist";
	// docs/backlog.md 15c carries the deferral).
	bool bInTown = false;

	// The player has a world in front of them, rather than the title screen or
	// the loading curtain. THIS IS EXPLORE'S OWN CONDITION -- section 2 gives
	// Explore priority 4 and Menu priority 5, which is only meaningful if
	// Explore has a condition of its own rather than being "not the menu".
	bool bInWorld = false;

	// STUBBED FALSE: UVoxelWeatherSubsystem publishes wind and nothing else --
	// there is no precipitation state anywhere in this project.
	bool bRaining = false;

	// 0..24, the sky subsystem's LocalHours. Only read for Explore.
	double LocalHours = 12.0;
};

// Section 2's ladder, walked top down. The first signal that is set owns the
// music; the three priority-3 pools are ordered by specificity (Water beats
// Cave beats Town -- "a boat in a flooded cavern is still a boat").
VOXELEARTHUI_API EVoxelMusicPool VoxelMusicResolvePool(const FVoxelMusicSignals& Signals);

// --- Time of day (section 4) -------------------------------------------------
//
// THE BOUNDARY TABLE LIVES IN EXACTLY ONE PLACE, which is the .cpp beside
// VoxelMusicSlotFromHour, because section 4 says so in as many words: "The
// boundaries are a first draft; they are read from one table so they can move."
// Nothing else in this codebase may hard-code an hour band.
//
// Hours outside [0,24) are wrapped, not clamped -- a clock that has run past
// midnight is the ordinary case, not an error.
VOXELEARTHUI_API EVoxelMusicSlot VoxelMusicSlotFromHour(double LocalHours);

// A slot with fewer than this many cues borrows from its neighbours
// (section 4). Four is the document's number.
inline constexpr int32 kVoxelMusicSlotMinCues = 4;

// The ordered list of slots to draw Explore from, most specific first: the
// hour's own slot followed by its borrow chain (Dawn borrows Day; Dusk borrows
// Day then Night; Night borrows Dusk; Day never borrows). When it is raining,
// Rain leads and the hour's chain follows it, which is section 7's "played in
// place of Day/Night when raining" with a fallback rather than a cliff.
//
// Returns how many entries were written. Out must hold kVoxelMusicSlotChainMax.
inline constexpr int32 kVoxelMusicSlotChainMax = 6;
VOXELEARTHUI_API int32 VoxelMusicSlotChain(bool bRaining, double LocalHours,
                                           EVoxelMusicSlot Out[kVoxelMusicSlotChainMax]);

// --- Authored silence (section 3) -------------------------------------------
//
// "Never two cues touching." The gap is drawn FRESH PER CUE from the pool's
// range, and it resets on a pool change so entering a town does not wait out a
// 90 s silence.
VOXELEARTHUI_API void VoxelMusicGapRange(EVoxelMusicPool Pool, float& OutMinSeconds, float& OutMaxSeconds);
VOXELEARTHUI_API float VoxelMusicDrawGap(EVoxelMusicPool Pool, FRandomStream& Stream);

// Combat has no gap while the threat lasts; "the cooldown tail is the gap"
// (section 3), drawn when the music leaves Combat for anything else.
inline constexpr float kVoxelMusicCombatCooldownMin = 10.f;
inline constexpr float kVoxelMusicCombatCooldownMax = 20.f;

// Section 2: "A pool change is a crossfade of 2-4 s, never a hard cut."
// Three is the middle of the stated band.
inline constexpr float kVoxelMusicCrossfadeSeconds = 3.f;

// --- The shuffle (section 2: "no repeat until exhausted") --------------------
//
// A PERMUTATION, NOT A PER-CUE ROLL, for the reason FVoxelUIMusic's header
// already gives about the session playlist it replaces: a fresh random pick can
// play the same file twice running, which is the single thing players notice
// about a shuffle. What is new here is that the permutation is per BANK and is
// re-drawn when it runs out, rather than being drawn once per session.
struct VOXELEARTHUI_API FVoxelMusicShuffle
{
	// The permutation, as indices into the bank's file list.
	TArray<int32> Order;
	// Where in Order we are. INDEX_NONE before the first Advance.
	int32 Cursor = INDEX_NONE;

	// Draws a fresh permutation of [0, Count). PreviousPick, when not
	// INDEX_NONE, is kept off the front so the seam between two cycles cannot
	// repeat a cue -- the one place a permutation on its own still can.
	void Reshuffle(int32 Count, FRandomStream& Stream, int32 PreviousPick = INDEX_NONE);

	// The next index, re-drawing the permutation when this one is spent.
	// INDEX_NONE only when Count is 0.
	int32 Advance(int32 Count, FRandomStream& Stream);

	// One back, stopping at the start of the current permutation rather than
	// wrapping into a previous one that no longer exists.
	int32 Retreat();

	int32 Current() const;

	void Reset() { Order.Reset(); Cursor = INDEX_NONE; }
};

// --- Memory across sessions (section 8) --------------------------------------
//
// "Remember the last five cues played per pool so a relaunch does not open on
// the same one."
inline constexpr int32 kVoxelMusicRecentMemory = 5;

// Given a bank's file list and a freshly drawn Order, returns the position in
// Order whose file is NOT one of Recent -- or 0 when every candidate is
// recent (a bank of five or fewer cues after five plays), because opening on
// something is better than opening on nothing.
VOXELEARTHUI_API int32 VoxelMusicFirstNotRecent(const TArray<FString>& CueNames,
                                                const TArray<int32>& Order,
                                                const TArray<FString>& Recent);
