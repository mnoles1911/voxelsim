#include "VoxelMusicPools.h"

#include "Math/RandomStream.h"

namespace VoxelMusicPoolsDetail
{
// Named, not anonymous: tools/lint-unity-collisions.py.

// ---- THE FOLDER LAYOUT (docs/music-design.md section 8) ---------------------
//
// One row per folder under Content/Audio/Music. The five Explore rows are the
// time-of-day slots; every other pool is a single folder because nothing else
// is filtered by the clock.
const FVoxelMusicBank kBanks[] = {
	{ EVoxelMusicPool::Explore,   EVoxelMusicSlot::Day,   TEXT("Explore/Day")   },
	{ EVoxelMusicPool::Explore,   EVoxelMusicSlot::Night, TEXT("Explore/Night") },
	{ EVoxelMusicPool::Explore,   EVoxelMusicSlot::Dawn,  TEXT("Explore/Dawn")  },
	{ EVoxelMusicPool::Explore,   EVoxelMusicSlot::Dusk,  TEXT("Explore/Dusk")  },
	{ EVoxelMusicPool::Explore,   EVoxelMusicSlot::Rain,  TEXT("Explore/Rain")  },
	{ EVoxelMusicPool::Cave,      EVoxelMusicSlot::None,  TEXT("Cave")          },
	{ EVoxelMusicPool::Town,      EVoxelMusicSlot::None,  TEXT("Town")          },
	{ EVoxelMusicPool::Water,     EVoxelMusicSlot::None,  TEXT("Water")         },
	{ EVoxelMusicPool::Combat,    EVoxelMusicSlot::None,  TEXT("Combat")        },
	{ EVoxelMusicPool::Menu,      EVoxelMusicSlot::None,  TEXT("Menu")          },
	{ EVoxelMusicPool::Stingers,  EVoxelMusicSlot::None,  TEXT("Stingers")      },
	{ EVoxelMusicPool::Cinematic, EVoxelMusicSlot::None,  TEXT("Cinematic")     },
};

// ---- THE BOUNDARY TABLE (docs/music-design.md section 4) --------------------
//
// THIS IS THE ONLY COPY OF THESE FOUR NUMBERS IN THE PROJECT. Section 4 asks
// for exactly that: "The boundaries are a first draft; they are read from one
// table so they can move."
//
// Read as: from StartHour (inclusive) until the next row's StartHour, the slot
// is Slot. The table must start at 0.0, be sorted, and cover the whole day; the
// first and last rows are both Night because the night band wraps midnight,
// and splitting it here is what lets the lookup stay a plain scan with no
// modular special case.
struct FSlotBound
{
	double StartHour;
	EVoxelMusicSlot Slot;
};
constexpr FSlotBound kSlotBounds[] = {
	{  0.0, EVoxelMusicSlot::Night }, // 20:00-05:00, second half
	{  5.0, EVoxelMusicSlot::Dawn  }, // 05:00-07:00
	{  7.0, EVoxelMusicSlot::Day   }, // 07:00-18:00
	{ 18.0, EVoxelMusicSlot::Dusk  }, // 18:00-20:00
	{ 20.0, EVoxelMusicSlot::Night }, // 20:00-05:00, first half
};

// ---- THE BORROW ORDER (docs/music-design.md section 4) ----------------------
//
// "A slot with fewer than four cues borrows from its neighbours in this order:
// Dawn borrows Day, Dusk borrows Day then Night, Night borrows Dusk. Day never
// borrows."
//
// Static lists rather than a walk over neighbours, so Dusk->Night->Dusk cannot
// become a loop, and so the order is readable against the sentence it came
// from.
int32 BorrowChain(EVoxelMusicSlot Slot, EVoxelMusicSlot Out[kVoxelMusicSlotChainMax])
{
	switch (Slot)
	{
	case EVoxelMusicSlot::Day:
		Out[0] = EVoxelMusicSlot::Day;
		return 1; // Day never borrows: it is the widest pool.
	case EVoxelMusicSlot::Dawn:
		Out[0] = EVoxelMusicSlot::Dawn;
		Out[1] = EVoxelMusicSlot::Day;
		return 2;
	case EVoxelMusicSlot::Dusk:
		Out[0] = EVoxelMusicSlot::Dusk;
		Out[1] = EVoxelMusicSlot::Day;
		Out[2] = EVoxelMusicSlot::Night;
		return 3;
	case EVoxelMusicSlot::Night:
		// Day LAST, as a floor (coordinator, 2026-09-08): the selection walk stops
		// as soon as the running count reaches four, so Day is only reached while
		// Night + Dusk hold fewer than four cues between them -- which is the
		// shipped library today (one Night cue, no Dusk). Without this floor the
		// whole night is one cue on repeat. Once the six Night cues in
		// docs/music-generation-prompts-2026-09-08.md exist, this link is never
		// consulted; it needs no removal.
		Out[0] = EVoxelMusicSlot::Night;
		Out[1] = EVoxelMusicSlot::Dusk;
		Out[2] = EVoxelMusicSlot::Day;
		return 3;
	default:
		Out[0] = Slot;
		return 1;
	}
}
} // namespace VoxelMusicPoolsDetail

const TCHAR* VoxelMusicPoolName(EVoxelMusicPool Pool)
{
	switch (Pool)
	{
	case EVoxelMusicPool::Stingers:  return TEXT("Stingers");
	case EVoxelMusicPool::Combat:    return TEXT("Combat");
	case EVoxelMusicPool::Water:     return TEXT("Water");
	case EVoxelMusicPool::Cave:      return TEXT("Cave");
	case EVoxelMusicPool::Town:      return TEXT("Town");
	case EVoxelMusicPool::Explore:   return TEXT("Explore");
	case EVoxelMusicPool::Menu:      return TEXT("Menu");
	case EVoxelMusicPool::Cinematic: return TEXT("Cinematic");
	default:                         return TEXT("none");
	}
}

const TCHAR* VoxelMusicSlotName(EVoxelMusicSlot Slot)
{
	switch (Slot)
	{
	case EVoxelMusicSlot::Dawn:  return TEXT("Dawn");
	case EVoxelMusicSlot::Day:   return TEXT("Day");
	case EVoxelMusicSlot::Dusk:  return TEXT("Dusk");
	case EVoxelMusicSlot::Night: return TEXT("Night");
	case EVoxelMusicSlot::Rain:  return TEXT("Rain");
	default:                     return TEXT("-");
	}
}

bool VoxelMusicPoolFromName(const FString& Name, EVoxelMusicPool& Out)
{
	for (uint8 I = 0; I < static_cast<uint8>(EVoxelMusicPool::Count); ++I)
	{
		const EVoxelMusicPool Pool = static_cast<EVoxelMusicPool>(I);
		if (Name.Equals(VoxelMusicPoolName(Pool), ESearchCase::IgnoreCase))
		{
			Out = Pool;
			return true;
		}
	}
	return false;
}

TArrayView<const FVoxelMusicBank> VoxelMusicBanks()
{
	return MakeArrayView(VoxelMusicPoolsDetail::kBanks, UE_ARRAY_COUNT(VoxelMusicPoolsDetail::kBanks));
}

int32 VoxelMusicBankIndex(EVoxelMusicPool Pool, EVoxelMusicSlot Slot)
{
	const TArrayView<const FVoxelMusicBank> Banks = VoxelMusicBanks();
	for (int32 I = 0; I < Banks.Num(); ++I)
	{
		if (Banks[I].Pool == Pool && Banks[I].Slot == Slot)
		{
			return I;
		}
	}
	return INDEX_NONE;
}

EVoxelMusicPool VoxelMusicResolvePool(const FVoxelMusicSignals& Signals)
{
	// SECTION 2'S TABLE, TOP DOWN, AND THE ORDER IS THE WHOLE CONTENT OF THIS
	// FUNCTION. Higher priority ducks lower; the three at priority 3 are
	// ordered by specificity, which the document states as "Water beats Cave
	// beats Town (a boat in a flooded cavern is still a boat)".
	if (Signals.bStinger)
	{
		return EVoxelMusicPool::Stingers;
	}
	if (Signals.bCombat)
	{
		return EVoxelMusicPool::Combat;
	}
	if (Signals.bAboard)
	{
		return EVoxelMusicPool::Water;
	}
	if (Signals.bUnderground)
	{
		return EVoxelMusicPool::Cave;
	}
	if (Signals.bInTown)
	{
		return EVoxelMusicPool::Town;
	}
	// EXPLORE HAS ITS OWN CONDITION rather than being "whatever is left". The
	// document puts Explore at 4 and Menu at 5, which only means something if
	// Explore can fail to apply -- and it fails exactly when there is no world
	// in front of the player. Written the other way round (Menu tested first)
	// the two priorities would be inverted, and a future higher-priority pool
	// added between them would land in the wrong place.
	if (Signals.bInWorld)
	{
		return EVoxelMusicPool::Explore;
	}
	return EVoxelMusicPool::Menu;
}

EVoxelMusicSlot VoxelMusicSlotFromHour(double LocalHours)
{
	// Wrapped, not clamped: a clock past midnight is the ordinary case. A NaN
	// hour (a sky subsystem that never ticked) falls through to Day, which is
	// the widest pool and the only harmless answer.
	if (!FMath::IsFinite(LocalHours))
	{
		return EVoxelMusicSlot::Day;
	}
	double Hour = FMath::Fmod(LocalHours, 24.0);
	if (Hour < 0.0)
	{
		Hour += 24.0;
	}

	EVoxelMusicSlot Slot = VoxelMusicPoolsDetail::kSlotBounds[0].Slot;
	for (const VoxelMusicPoolsDetail::FSlotBound& Bound : VoxelMusicPoolsDetail::kSlotBounds)
	{
		if (Hour >= Bound.StartHour)
		{
			Slot = Bound.Slot;
		}
	}
	return Slot;
}

int32 VoxelMusicSlotChain(bool bRaining, double LocalHours, EVoxelMusicSlot Out[kVoxelMusicSlotChainMax])
{
	int32 Num = 0;
	if (bRaining)
	{
		// RAIN LEADS, IT DOES NOT REPLACE. Section 7 calls the rain cues "an
		// optional layer, played in place of Day/Night when raining", and the
		// folder is empty today -- so a hard replacement would mean silence
		// every time it rained. Leading the chain gives it precedence when it
		// has cues and costs nothing when it does not.
		Out[Num++] = EVoxelMusicSlot::Rain;
	}
	EVoxelMusicSlot Chain[kVoxelMusicSlotChainMax];
	const int32 ChainNum = VoxelMusicPoolsDetail::BorrowChain(VoxelMusicSlotFromHour(LocalHours), Chain);
	for (int32 I = 0; I < ChainNum && Num < kVoxelMusicSlotChainMax; ++I)
	{
		Out[Num++] = Chain[I];
	}
	return Num;
}

void VoxelMusicGapRange(EVoxelMusicPool Pool, float& OutMinSeconds, float& OutMaxSeconds)
{
	// SECTION 3, VERBATIM. "Explore, Cave, Town, Water: a randomised gap of
	// 45-120 s between cues, drawn fresh each time. Never two cues touching."
	// "Menu: a gap of 20-40 s." "Combat: no gap while the threat lasts."
	switch (Pool)
	{
	case EVoxelMusicPool::Explore:
	case EVoxelMusicPool::Cave:
	case EVoxelMusicPool::Town:
	case EVoxelMusicPool::Water:
		OutMinSeconds = 45.f;
		OutMaxSeconds = 120.f;
		return;
	case EVoxelMusicPool::Menu:
		OutMinSeconds = 20.f;
		OutMaxSeconds = 40.f;
		return;
	default:
		// Combat (no gap), Stingers (fired, never cycled) and Cinematic
		// (never cycled at all).
		OutMinSeconds = 0.f;
		OutMaxSeconds = 0.f;
		return;
	}
}

float VoxelMusicDrawGap(EVoxelMusicPool Pool, FRandomStream& Stream)
{
	float Min = 0.f, Max = 0.f;
	VoxelMusicGapRange(Pool, Min, Max);
	return Max > Min ? Stream.FRandRange(Min, Max) : Min;
}

void FVoxelMusicShuffle::Reshuffle(int32 Count, FRandomStream& Stream, int32 PreviousPick)
{
	Order.Reset(Count);
	for (int32 I = 0; I < Count; ++I)
	{
		Order.Add(I);
	}
	// Fisher-Yates from the caller's stream, the same way the old session
	// playlist was drawn.
	for (int32 I = Order.Num() - 1; I > 0; --I)
	{
		const int32 J = Stream.RandRange(0, I);
		if (J != I)
		{
			Order.Swap(I, J);
		}
	}
	// THE SEAM IS THE ONE PLACE A PERMUTATION STILL REPEATS: the last cue of
	// one cycle can be the first of the next. With two or more cues that is
	// always avoidable, so avoid it.
	if (Order.Num() > 1 && PreviousPick != INDEX_NONE && Order[0] == PreviousPick)
	{
		Order.Swap(0, 1);
	}
	Cursor = INDEX_NONE;
}

int32 FVoxelMusicShuffle::Advance(int32 Count, FRandomStream& Stream)
{
	if (Count <= 0)
	{
		Reset();
		return INDEX_NONE;
	}
	// A bank whose folder gained or lost files since the last draw. Rebuild
	// rather than index off the end.
	if (Order.Num() != Count)
	{
		Reshuffle(Count, Stream);
	}
	if (Cursor + 1 >= Order.Num())
	{
		Reshuffle(Count, Stream, /*PreviousPick=*/Current());
	}
	++Cursor;
	return Order[Cursor];
}

int32 FVoxelMusicShuffle::Retreat()
{
	if (Order.Num() == 0)
	{
		return INDEX_NONE;
	}
	// STOPS AT THE START OF THIS PERMUTATION rather than wrapping. The previous
	// cycle's order was thrown away when it was re-drawn, so wrapping would
	// walk into an order the listener never heard -- which is not what "back"
	// means to anybody.
	Cursor = FMath::Max(0, Cursor - 1);
	return Order[Cursor];
}

int32 FVoxelMusicShuffle::Current() const
{
	return Order.IsValidIndex(Cursor) ? Order[Cursor] : INDEX_NONE;
}

int32 VoxelMusicFirstNotRecent(const TArray<FString>& CueNames, const TArray<int32>& Order,
                               const TArray<FString>& Recent)
{
	for (int32 Position = 0; Position < Order.Num(); ++Position)
	{
		const int32 Index = Order[Position];
		if (!CueNames.IsValidIndex(Index))
		{
			continue;
		}
		if (!Recent.Contains(CueNames[Index]))
		{
			return Position;
		}
	}
	// EVERY CANDIDATE IS RECENT. A bank of five or fewer cues reaches this
	// after five plays, and refusing to open would be worse than opening on a
	// cue heard last session.
	return 0;
}
