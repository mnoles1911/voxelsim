#include "VoxelUIMusic.h"

#include "VoxelEarthUI.h" // LogVoxelUI
#include "VoxelAudioUserSettings.h"
#include "VoxelFrontEndSwitches.h"

#include "VoxelBoat.h"
#include "VoxelClipmapActor.h"
#include "VoxelSkySubsystem.h"

#include "AudioDefines.h" // INDEFINITELY_LOOPING_DURATION
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h" // TActorIterator, for the clipmap's veil latch
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Math/RandomStream.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Sound/SoundWaveProcedural.h"

namespace VoxelUIMusicDetail
{
// Named, not anonymous: tools/lint-unity-collisions.py.

const TCHAR* const kMusicSubdir = TEXT("Audio/Music");

// Queued ahead of the audio thread's demand so a slow frame cannot starve it.
// 2 seconds at 48kHz stereo 16-bit is ~384 KB, which is nothing against a
// cue already resident in full.
constexpr double kPrebufferSeconds = 2.0;

// How far into a cue PREV stops meaning "start this one again" and starts
// meaning "the one before". Five seconds is the figure every music player has
// converged on; below it the listener has barely heard the cue and cannot
// have meant it, above it they are asking to leave.
constexpr double kPrevRestartSeconds = 5.0;

// How often the world is asked which pool should be playing. The answer cannot
// change faster than a player can walk into a cave or step onto a boat, and
// the poll costs a subsystem lookup, a pawn cast and a weak-pointer deref.
constexpr double kSignalPollSeconds = 0.5;

// Slack on top of the crossfade before the outgoing component is destroyed, so
// a frame hitch during the fade cannot cut its last few milliseconds off.
constexpr double kCrossfadeReapSlackSeconds = 0.5;

// ---- THE COMBAT THREAT SIGNAL DOES NOT EXIST ------------------------------
//
// STUBBED, DELIBERATELY, AND SAID OUT LOUD RATHER THAN LEFT AS A `false`.
// docs/music-design.md section 2 gives Combat priority 2, "a threat is
// active". This project has no combat: no health, no damage, no hostile actor,
// no aggro state, no threat delegate -- the same absence
// -VoxelDemoVitals records for the HUD's health bar. There is nothing to read.
//
// So the Combat pool is wired, its folder is populated (two cues), its priority
// is implemented and tested, and this predicate returns false, which means it
// never plays. When a threat signal arrives this is the ONE line that changes.
bool IsCombatThreatActive(const UWorld* /*World*/)
{
	return false;
}

// SAME SHAPE, SAME REASON: section 2 says Town owns the music "inside a
// settlement's bounds (when settlements exist)". They do not exist, there is no
// bounds volume and no settlement actor, and docs/backlog.md 15c carries the
// deferral of the signal. The seven Town cues are on disk and unreachable until
// this returns something.
bool IsInsideSettlement(const UWorld* /*World*/)
{
	return false;
}

// AND AGAIN: UVoxelWeatherSubsystem publishes WIND and nothing else -- there is
// no precipitation state anywhere in this project -- so the Explore/Rain slot
// (empty on disk today) can never be selected.
bool IsRaining(const UWorld* /*World*/)
{
	return false;
}

// RIFF/WAVE parsing, by hand and deliberately so. The engine's importers all
// live behind editor-only modules; this front end has to work in a -game build
// with no editor at all, which is the whole reason the art is decoded by path
// too. A 16-bit PCM WAV header is a dozen fields and the parse below refuses
// everything it does not fully understand rather than guessing.
struct FWavFormat
{
	int32 SampleRate = 0;
	int32 NumChannels = 0;
	int32 BitsPerSample = 0;
	int64 DataOffset = 0;
	int64 DataSize = 0;
};

bool ParseWavHeader(const TArray<uint8>& Bytes, FWavFormat& Out, FString& OutWhy)
{
	// Smallest conceivable valid file: RIFF(12) + fmt (24) + data(8).
	if (Bytes.Num() < 44)
	{
		OutWhy = TEXT("shorter than a WAV header");
		return false;
	}
	if (FMemory::Memcmp(Bytes.GetData(), "RIFF", 4) != 0 ||
	    FMemory::Memcmp(Bytes.GetData() + 8, "WAVE", 4) != 0)
	{
		OutWhy = TEXT("not RIFF/WAVE");
		return false;
	}

	bool bHaveFmt = false;
	int64 Cursor = 12;
	while (Cursor + 8 <= Bytes.Num())
	{
		const uint8* Chunk = Bytes.GetData() + Cursor;
		uint32 ChunkSize = 0;
		FMemory::Memcpy(&ChunkSize, Chunk + 4, 4);

		if (FMemory::Memcmp(Chunk, "fmt ", 4) == 0 && ChunkSize >= 16 && Cursor + 8 + 16 <= Bytes.Num())
		{
			uint16 AudioFormat = 0, Channels = 0, Bits = 0;
			uint32 Rate = 0;
			FMemory::Memcpy(&AudioFormat, Chunk + 8, 2);
			FMemory::Memcpy(&Channels, Chunk + 10, 2);
			FMemory::Memcpy(&Rate, Chunk + 12, 4);
			FMemory::Memcpy(&Bits, Chunk + 22, 2);

			// Format 1 is uncompressed PCM. Everything else -- IEEE float,
			// ADPCM, extensible -- is refused by name rather than played as
			// noise, which is what reinterpreting the bytes would produce.
			if (AudioFormat != 1)
			{
				OutWhy = FString::Printf(TEXT("audioFormat %d, only 1 (PCM) is supported"), AudioFormat);
				return false;
			}
			if (Bits != 16)
			{
				// USoundWaveProcedural's SampleByteSize defaults to 2 and this
				// path relies on that; 24- and 32-bit would need conversion.
				OutWhy = FString::Printf(TEXT("%d-bit, only 16-bit is supported"), Bits);
				return false;
			}
			if (Channels != 1 && Channels != 2)
			{
				OutWhy = FString::Printf(TEXT("%d channels, only mono and stereo are supported"), Channels);
				return false;
			}
			Out.SampleRate = static_cast<int32>(Rate);
			Out.NumChannels = static_cast<int32>(Channels);
			Out.BitsPerSample = static_cast<int32>(Bits);
			bHaveFmt = true;
		}
		else if (FMemory::Memcmp(Chunk, "data", 4) == 0)
		{
			Out.DataOffset = Cursor + 8;
			// A truncated or lying size header is common in generated audio;
			// clamp rather than read off the end of the buffer.
			Out.DataSize = FMath::Min<int64>(ChunkSize, Bytes.Num() - Out.DataOffset);
			if (!bHaveFmt)
			{
				OutWhy = TEXT("data chunk before fmt chunk");
				return false;
			}
			if (Out.DataSize <= 0)
			{
				OutWhy = TEXT("empty data chunk");
				return false;
			}
			return true;
		}

		// Chunks are word-aligned: an odd size is followed by a pad byte.
		Cursor += 8 + ChunkSize + (ChunkSize & 1);
	}

	OutWhy = bHaveFmt ? TEXT("no data chunk") : TEXT("no fmt chunk");
	return false;
}
} // namespace VoxelUIMusicDetail

FVoxelUIMusic& FVoxelUIMusic::Get()
{
	static FVoxelUIMusic Instance;
	return Instance;
}

bool FVoxelUIMusic::LoadWav(const FString& Path, FLoadedTrack& Out) const
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelUIMusic: could not read %s."), *Path);
		return false;
	}

	VoxelUIMusicDetail::FWavFormat Format;
	FString Why;
	if (!VoxelUIMusicDetail::ParseWavHeader(Bytes, Format, Why))
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelUIMusic: skipping %s -- %s."),
		       *FPaths::GetCleanFilename(Path), *Why);
		return false;
	}

	Out.Pcm.Reset(static_cast<int32>(Format.DataSize));
	Out.Pcm.Append(Bytes.GetData() + Format.DataOffset, static_cast<int32>(Format.DataSize));
	Out.SampleRate = Format.SampleRate;
	Out.NumChannels = Format.NumChannels;
	Out.Name = FPaths::GetBaseFilename(Path);

	const double Seconds = static_cast<double>(Format.DataSize) / (Format.SampleRate * Format.NumChannels * 2);
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: loaded '%s' -- %.1fs, %dch %dHz 16-bit, %.1f MB."),
	       *Out.Name, Seconds, Out.NumChannels, Out.SampleRate, Out.Pcm.Num() / (1024.0 * 1024.0));
	return true;
}

void FVoxelUIMusic::BuildBanks(FRandomStream& Stream)
{
	if (bBanksBuilt)
	{
		// ONCE PER SESSION. See the header: a permutation cannot repeat, and
		// re-drawing it on a second visit to the menu would throw that away.
		return;
	}
	bBanksBuilt = true;

	// EVERY DRAW IN THIS CLASS COMES FROM HERE, seeded from the caller's stream
	// -- the same one the menu backgrounds were shuffled from -- so a seeded
	// run still pairs the same art with the same music, and the gaps are
	// reproducible too.
	PoolStream.Initialize(Stream.GetCurrentSeed());

	const FString Root = FPaths::ProjectContentDir() / VoxelUIMusicDetail::kMusicSubdir;
	const TArrayView<const FVoxelMusicBank> BankDescs = VoxelMusicBanks();

	Banks.Reset();
	Banks.SetNum(BankDescs.Num());

	int32 TotalCues = 0;
	FString Summary;
	for (int32 I = 0; I < BankDescs.Num(); ++I)
	{
		const FString Dir = Root / BankDescs[I].Folder;
		TArray<FString> Files;
		// RECURSIVE, PER FOLDER (docs/music-design.md section 8: "the glob
		// becomes recursive-by-folder rather than a flat *.wav"). Recursive
		// rather than one flat listing so a designer may group a pool's cues
		// further -- Town/Taverns/, say -- without a code change. Explore
		// itself is NOT a bank, which is what keeps its five slot folders from
		// collapsing back into one list.
		IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.wav"),
		                                       /*Files=*/true, /*Directories=*/false,
		                                       /*bClearFileNames=*/true);

		// Sorted before it is shuffled, and that is not decoration.
		// IFileManager's enumeration order is the filesystem's, which is not
		// guaranteed stable between machines or after a defragment -- so a
		// seeded run would pair a different cue with the same background art on
		// a different box. Sorting first makes the permutation a pure function
		// of the seed.
		Files.Sort();

		FBankState& Bank = Banks[I];
		Bank.Paths = MoveTemp(Files);
		Bank.Names.Reset(Bank.Paths.Num());
		for (const FString& Path : Bank.Paths)
		{
			Bank.Names.Add(FPaths::GetBaseFilename(Path));
		}
		TotalCues += Bank.Paths.Num();

		if (!Summary.IsEmpty())
		{
			Summary += TEXT(", ");
		}
		Summary += FString::Printf(TEXT("%s=%d"), BankDescs[I].Folder, Bank.Paths.Num());
	}

	if (TotalCues == 0)
	{
		// NOT A FAILURE. CI has no music and neither does a fresh checkout --
		// the cues are the designer's and are not committed. Said once, at
		// Log level, so a silent game is explained rather than mysterious.
		UE_LOG(LogVoxelUI, Log,
		       TEXT("VoxelUIMusic: no .wav files under %s -- the game runs silent. "
		            "Drop 16-bit PCM WAVs into the pool folders (Explore/Day, Cave, Town, ...) "
		            "to add music; no code change is needed."),
		       *Root);
		return;
	}

	// THE ENGAGEMENT WITNESS FOR THE WHOLE LAYOUT. A pool that is empty because
	// its folder is misspelled and a pool that is empty because the designer has
	// not written those cues yet are indistinguishable from the music itself, so
	// this prints the count of every bank once, at boot.
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: pools built -- %d cue(s): %s."), TotalCues, *Summary);
}

FVoxelMusicSignals FVoxelUIMusic::ReadSignals() const
{
	FVoxelMusicSignals Signals;
	Signals.bInWorld = (Context == EVoxelMusicContext::InWorld);

	UWorld* World = MusicWorld.Get();
	if (!Signals.bInWorld || World == nullptr)
	{
		// THE MENU AND THE LOADING CURTAIN ASK NOTHING OF THE WORLD. Section 2
		// gives both to the Menu pool, and reading a half-built world's sky
		// clock during the load would be reading a number that is about to
		// change anyway.
		return Signals;
	}

	if (const UVoxelSkySubsystem* Sky = World->GetSubsystem<UVoxelSkySubsystem>())
	{
		// THE SAME CLOCK THE JOURNAL READS (UVoxelScreensUISubsystem's day
		// count is EpochSeconds / DayLength off this state). LocalHours is
		// "the UTC hour the sun is at", 0..24, which is precisely what
		// section 4's boundary table is expressed in.
		Signals.LocalHours = Sky->GetSkyState().LocalHours;
	}

	if (const APlayerController* PC = World->GetFirstPlayerController())
	{
		// ABOARD IS "THE PAWN IS THE BOAT", which is exactly what
		// AVoxelBoat::Enter makes true (it possesses the boat and parks the
		// previous pawn) and what AVoxelBoat::ExitToStoredPawn undoes. There is
		// no separate aboard flag to drift out of sync with it.
		Signals.bAboard = Cast<AVoxelBoat>(PC->GetPawn()) != nullptr;
	}

	if (!Clipmap.IsValid())
	{
		for (TActorIterator<AVoxelClipmapActor> It(World); It; ++It)
		{
			Clipmap = *It;
			break;
		}
	}
	if (const AVoxelClipmapActor* Actor = Clipmap.Get())
	{
		// "The underground veil is engaged (we already have this signal)",
		// section 2. Read, never re-derived -- see the accessor's comment in
		// VoxelClipmapActor.h for why there is exactly one such predicate.
		Signals.bUnderground = Actor->IsUndergroundVeilActive();
	}

	Signals.bCombat = VoxelUIMusicDetail::IsCombatThreatActive(World);
	Signals.bInTown = VoxelUIMusicDetail::IsInsideSettlement(World);
	Signals.bRaining = VoxelUIMusicDetail::IsRaining(World);
	// bStinger stays false: stingers are fired by an event, never polled, and
	// nothing in this project fires one (docs/backlog.md 15c).
	return Signals;
}

int32 FVoxelUIMusic::ChooseBank(const FVoxelMusicSignals& Signals, EVoxelMusicPool& OutPool,
                                EVoxelMusicSlot& OutSlot) const
{
	OutPool = VoxelMusicResolvePool(Signals);
	OutSlot = EVoxelMusicSlot::None;

	// -VoxelMusicPool=<name> pins the ladder's answer. Validated here rather
	// than at the parse so the warning can name the legal spellings once.
	const FString& Pinned = VoxelFrontEndSwitches::Get().MusicPool;
	if (!Pinned.IsEmpty())
	{
		EVoxelMusicPool Forced = EVoxelMusicPool::Explore;
		if (VoxelMusicPoolFromName(Pinned, Forced))
		{
			OutPool = Forced;
		}
		else
		{
			static bool bWarned = false;
			if (!bWarned)
			{
				bWarned = true;
				UE_LOG(LogVoxelUI, Warning,
				       TEXT("-VoxelMusicPool=%s is not a pool. Use one of: Explore, Cave, Town, Water, "
				            "Combat, Menu, Stingers, Cinematic. Falling back to the world's signals."),
				       *Pinned);
			}
		}
	}

	// WALK DOWN THE LADDER PAST ANY EMPTY POOL. The design ships with an empty
	// Explore/Dawn, Explore/Dusk and Explore/Rain, and a fresh checkout has no
	// cues at all; a pool that owns the music but has nothing to play would be
	// silence with no explanation, which is the one outcome the priority table
	// is not asking for. The order below is the enum's declaration order, which
	// IS the priority order (VoxelMusicPools.h).
	for (uint8 Step = static_cast<uint8>(OutPool); Step < static_cast<uint8>(EVoxelMusicPool::Count); ++Step)
	{
		const EVoxelMusicPool Pool = static_cast<EVoxelMusicPool>(Step);
		if (Pool == EVoxelMusicPool::Cinematic)
		{
			// "Never in any cycling pool" (section 2, row 7). It is reachable
			// only by -VoxelMusicPool=Cinematic, which is a deliberate pin.
			if (Step != static_cast<uint8>(OutPool))
			{
				continue;
			}
		}

		if (Pool != EVoxelMusicPool::Explore)
		{
			const int32 Index = VoxelMusicBankIndex(Pool, EVoxelMusicSlot::None);
			if (Banks.IsValidIndex(Index) && Banks[Index].Paths.Num() > 0)
			{
				OutPool = Pool;
				OutSlot = EVoxelMusicSlot::None;
				return Index;
			}
			continue;
		}

		// EXPLORE IS THE ONE POOL WITH SLOTS, and this is section 4's borrow
		// rule: "A slot with fewer than four cues borrows from its neighbours
		// in this order ... Day never borrows."
		//
		// WALK THE CHAIN UNTIL IT IS THICK ENOUGH, AND PLAY FROM THE LINK THAT
		// MADE IT SO. One bank is one shuffled playlist, so "borrowing" has to
		// resolve to a single folder rather than to a union -- and the folder
		// that satisfied the four-cue rule is the honest answer, because it is
		// the one that made the pool wide enough to be worth filtering by hour
		// at all. At the shipped library: Day (11) satisfies it on its own and
		// borrows nothing; Dawn and Dusk are empty and go straight to Day;
		// Night has one cue and Dusk has none, so the chain never reaches four
		// and the fallback below keeps Night's own cue rather than reaching
		// past its borrow list into Day, which section 4 does not permit.
		//
		// The slot the cue actually came from is what gets logged, so a
		// borrowed selection is visible rather than silently mislabelled.
		EVoxelMusicSlot Chain[kVoxelMusicSlotChainMax];
		const int32 ChainNum = VoxelMusicSlotChain(Signals.bRaining, Signals.LocalHours, Chain);
		int32 Accumulated = 0;
		int32 Chosen = INDEX_NONE;
		EVoxelMusicSlot ChosenSlot = EVoxelMusicSlot::None;
		int32 Widest = INDEX_NONE;
		EVoxelMusicSlot WidestSlot = EVoxelMusicSlot::None;
		int32 WidestCount = 0;
		for (int32 I = 0; I < ChainNum; ++I)
		{
			const int32 Index = VoxelMusicBankIndex(EVoxelMusicPool::Explore, Chain[I]);
			if (!Banks.IsValidIndex(Index) || Banks[Index].Paths.Num() == 0)
			{
				continue;
			}
			const int32 Num = Banks[Index].Paths.Num();
			if (Num > WidestCount)
			{
				// The fallback for a chain that never reaches four: the widest
				// link in it. Strictly greater, so an earlier link wins a tie
				// and the choice stays the chain's own order.
				WidestCount = Num;
				Widest = Index;
				WidestSlot = Chain[I];
			}
			Accumulated += Num;
			if (Accumulated >= kVoxelMusicSlotMinCues)
			{
				Chosen = Index;
				ChosenSlot = Chain[I];
				break;
			}
		}
		if (Chosen == INDEX_NONE)
		{
			Chosen = Widest;
			ChosenSlot = WidestSlot;
		}
		if (Chosen != INDEX_NONE)
		{
			OutPool = EVoxelMusicPool::Explore;
			OutSlot = ChosenSlot;
			return Chosen;
		}
	}

	return INDEX_NONE;
}

bool FVoxelUIMusic::InstallTrack(const FString& Path, bool bCrossfade)
{
	UWorld* World = MusicWorld.Get();
	if (World == nullptr)
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelUIMusic: no world to play in; music stays silent."));
		return false;
	}

	FLoadedTrack Track;
	if (!LoadWav(Path, Track))
	{
		return false;
	}

	// The old voice goes here, AFTER the read rather than before it -- which is
	// the one place this differs from the pre-pool version. A crossfade needs
	// the outgoing voice still alive when the incoming one starts, and
	// BeginCrossfadeOut is what makes that cost one queue rather than a second
	// decoded cue. Without a crossfade this is the old Release(), and the gap
	// of silence is the one the player is about to hear anyway.
	if (bCrossfade && Component.IsValid())
	{
		BeginCrossfadeOut();
	}
	Release();

	USoundWaveProcedural* NewWave = NewObject<USoundWaveProcedural>();
	NewWave->SetSampleRate(Track.SampleRate);
	NewWave->NumChannels = Track.NumChannels;
	// Indefinite: the FILE's length is not the voice's length. The underflow
	// handler decides when the PCM has run out and the game thread decides
	// what happens next, so the wave must never terminate itself.
	NewWave->Duration = INDEFINITELY_LOOPING_DURATION;
	NewWave->SoundGroup = SOUNDGROUP_Music;
	NewWave->bLooping = false; // there is no looping any more: cues advance
	NewWave->OnSoundWaveProceduralUnderflow =
		FOnSoundWaveProceduralUnderflow::CreateRaw(this, &FVoxelUIMusic::OnUnderflow);

	SampleRate = Track.SampleRate;
	NumChannels = Track.NumChannels;
	TrackName = Track.Name;

	{
		// The install and the prime, together, under the one lock: after
		// this block the audio thread may call in at any moment, and it
		// must never see the new wave with the old PCM behind it.
		FScopeLock Lock(&PcmGuard);
		PcmData = MoveTemp(Track.Pcm);
		PlayCursor = 0;
		bFeeding = true;
		ActiveWave = NewWave;

		const int32 PrebufferBytes = FMath::Min<int32>(
			PcmData.Num(),
			static_cast<int32>(VoxelUIMusicDetail::kPrebufferSeconds * SampleRate * NumChannels * 2));
		NewWave->QueueAudio(PcmData.GetData(), PrebufferBytes);
		PlayCursor = PrebufferBytes;
		if (PlayCursor >= PcmData.Num())
		{
			// A cue shorter than the prebuffer is entirely queued here and
			// the underflow handler will never run for it. Raising the end
			// signal now is what stops such a file from wedging the pool.
			bFeeding = false;
			bEndPending.store(true, std::memory_order_relaxed);
		}
	}
	Wave.Reset(NewWave);

	UAudioComponent* NewComponent = UGameplayStatics::CreateSound2D(
		World, NewWave, /*VolumeMultiplier=*/1.f, /*PitchMultiplier=*/1.f, /*StartTime=*/0.f,
		/*ConcurrencySettings=*/nullptr, /*bPersistAcrossLevelTransition=*/false, /*bAutoDestroy=*/false);
	if (NewComponent == nullptr)
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelUIMusic: could not create an audio component for '%s'."),
		       *TrackName);
		Release();
		return false;
	}
	Component.Reset(NewComponent);
	// The component is created at 1.0 above and brought to the player's
	// chosen level HERE rather than by passing the volume to CreateSound2D,
	// so that there is exactly one place -- ApplyVolume -- that knows how
	// the two audio settings combine. Before Play(), so a cue never sounds
	// for one buffer at full volume on a machine set to 20%.
	ApplyVolume();
	if (bCrossfade)
	{
		// FadeIn drives the component's FADE multiplier from 0 to 1; the
		// player's volume is the separate VolumeMultiplier ApplyVolume just
		// set, so the two do not fight.
		NewComponent->FadeIn(kVoxelMusicCrossfadeSeconds, /*FadeVolumeLevel=*/1.f, /*StartTime=*/0.f);
	}
	else
	{
		NewComponent->Play();
	}
	bPaused = false;
	bInGap = false;
	GapRemaining = 0.0;
	ArmTicker();
	return true;
}

bool FVoxelUIMusic::PlayFromBank(int32 BankIndex, bool bAdvance, bool bCrossfade)
{
	if (!Banks.IsValidIndex(BankIndex) || Banks[BankIndex].Paths.Num() == 0)
	{
		return false;
	}
	FBankState& Bank = Banks[BankIndex];
	const FVoxelMusicBank& Desc = VoxelMusicBanks()[BankIndex];
	const int32 Count = Bank.Paths.Num();

	// FIRST CUE OF A BANK THIS SESSION: draw the permutation and keep the last
	// five cues the player heard from this POOL off its front
	// (docs/music-design.md section 8, "a relaunch does not open on the same
	// one"). Done here rather than in BuildBanks so that a bank never pays for
	// an ini read it will not use.
	if (Bank.Shuffle.Order.Num() != Count)
	{
		Bank.Shuffle.Reshuffle(Count, PoolStream);
		const TArray<FString> Recent = VoxelAudioUserSettings::GetRecentMusicCues(VoxelMusicPoolName(Desc.Pool));
		const int32 Position = VoxelMusicFirstNotRecent(Bank.Names, Bank.Shuffle.Order, Recent);
		if (Position > 0)
		{
			Bank.Shuffle.Order.Swap(0, Position);
		}
	}

	// Walk forward past anything that will not load: one bad file must not mean
	// silence when the rest of the bank is fine.
	for (int32 Step = 0; Step < Count; ++Step)
	{
		// bAdvance=false means "play what the cursor is already on", which is
		// what PREV (after its Retreat) and the PLAY button want. A retry after
		// a failed load always steps, and so does the very first cue of a bank,
		// where there is no cursor yet.
		const bool bStep = bAdvance || Step > 0 || Bank.Shuffle.Current() == INDEX_NONE;
		const int32 File = bStep ? Bank.Shuffle.Advance(Count, PoolStream) : Bank.Shuffle.Current();
		if (!Bank.Paths.IsValidIndex(File))
		{
			continue;
		}
		if (!InstallTrack(Bank.Paths[File], bCrossfade))
		{
			continue;
		}

		ActiveBank = BankIndex;
		ActivePool = Desc.Pool;
		ActiveSlot = Desc.Slot;

		// THE GAP IS DRAWN WITH THE CUE, NOT WHEN THE CUE ENDS (section 3:
		// "drawn fresh each time"; section 8: "Gaps ... drawn per cue"). That
		// is also what makes the selection log line below able to state it.
		const float GapOverride = VoxelFrontEndSwitches::Get().MusicGapSeconds;
		PendingGapSeconds = GapOverride >= 0.f ? GapOverride
		                                       : VoxelMusicDrawGap(ActivePool, PoolStream);

		VoxelAudioUserSettings::PushRecentMusicCue(VoxelMusicPoolName(ActivePool), TrackName);

		// THE ONE LINE THAT PROVES THE MECHANISM ENGAGED. Format is fixed by
		// the 2026-09-08 brief; a leg greps it and can count selections, read
		// the slot the clock resolved to, and see the authored silence.
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: pool=%s slot=%s cue='%s' gap=%.1f"),
		       VoxelMusicPoolName(ActivePool), VoxelMusicSlotName(ActiveSlot), *TrackName,
		       PendingGapSeconds);
		return true;
	}

	UE_LOG(LogVoxelUI, Warning,
	       TEXT("VoxelUIMusic: pool=%s slot=%s has %d file(s) and none playable."),
	       VoxelMusicPoolName(Desc.Pool), VoxelMusicSlotName(Desc.Slot), Count);
	return false;
}

void FVoxelUIMusic::StartRandom(UWorld* World, FRandomStream& Stream)
{
	if (World == nullptr)
	{
		return;
	}
	// Recorded even when a cue is already playing: the menu's world dies at a
	// map reopen and the next NEXT needs somewhere to spawn a component.
	MusicWorld = World;
	Clipmap.Reset();

	BuildBanks(Stream);

	if (IsEngaged())
	{
		// ADOPT, DO NOT RESTART. The contract ADR-0009 recorded is that the cue
		// the menu started is the same one the loading screen and now the game
		// keep playing, and this no-op is what makes that true without a state
		// flag anywhere else. IsEngaged rather than IsPlaying because an
		// authored gap is also "the music is running"; restarting through one
		// would cut every silence short at the hand-off.
		return;
	}

	EVoxelMusicPool Pool = EVoxelMusicPool::Menu;
	EVoxelMusicSlot Slot = EVoxelMusicSlot::None;
	const int32 Bank = ChooseBank(ReadSignals(), Pool, Slot);
	if (Bank == INDEX_NONE)
	{
		return; // BuildBanks has already said so, once
	}
	PlayFromBank(Bank, /*bAdvance=*/true, /*bCrossfade=*/false);
}

void FVoxelUIMusic::SetContext(EVoxelMusicContext InContext)
{
	if (Context == InContext)
	{
		return;
	}
	Context = InContext;
	// The pool change itself is left to the next poll rather than done here, so
	// that there is exactly one place a pool change happens and one place it is
	// logged. The poll is at 2 Hz, so the hand-off does not wait on it.
	SignalPollRemaining = 0.0;
}

void FVoxelUIMusic::Next()
{
	if (ActiveBank == INDEX_NONE || !Banks.IsValidIndex(ActiveBank))
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: NEXT ignored -- there is no pool playing."));
		return;
	}
	// CANCELS A GAP. A transport button that appears to do nothing for the next
	// ninety seconds is a broken button from where the player is sitting.
	bInGap = false;
	GapRemaining = 0.0;
	PlayFromBank(ActiveBank, /*bAdvance=*/true, /*bCrossfade=*/false);
}

void FVoxelUIMusic::Previous()
{
	if (ActiveBank == INDEX_NONE || !Banks.IsValidIndex(ActiveBank))
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: PREV ignored -- there is no pool playing."));
		return;
	}
	FBankState& Bank = Banks[ActiveBank];
	const double Position = ElapsedSeconds();
	const bool bRestart = IsPlaying() && Position > VoxelUIMusicDetail::kPrevRestartSeconds;

	bInGap = false;
	GapRemaining = 0.0;

	if (!bRestart)
	{
		Bank.Shuffle.Retreat();
	}
	// bAdvance=false: play whatever the cursor is on now, which after the
	// Retreat above is the previous cue and without it is the current one.
	if (PlayFromBank(ActiveBank, /*bAdvance=*/false, /*bCrossfade=*/false) && bRestart)
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: PREV restarted '%s', %.1fs in."), *TrackName, Position);
	}
}

void FVoxelUIMusic::TogglePause()
{
	if (!Component.IsValid())
	{
		if (bInGap)
		{
			// PAUSE FREEZES THE AUTHORED SILENCE. A player who paused during a
			// ninety-second gap must not come back to find the next cue already
			// half over -- that is the same complaint the component pause
			// exists to prevent, one level up.
			bPaused = !bPaused;
			UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: %s during the gap (%.1fs of silence left)."),
			       bPaused ? TEXT("PAUSED") : TEXT("RESUMED"), GapRemaining);
			return;
		}

		// A PLAY BUTTON WITH NOTHING LOADED STARTS THE MUSIC. The alternative --
		// a button that does nothing after the player stopped the cue, or
		// after a run that never had a menu -- is the same as a broken button
		// from where they are sitting.
		EVoxelMusicPool Pool = EVoxelMusicPool::Menu;
		EVoxelMusicSlot Slot = EVoxelMusicSlot::None;
		const int32 Bank = ActiveBank != INDEX_NONE ? ActiveBank : ChooseBank(ReadSignals(), Pool, Slot);
		if (Bank == INDEX_NONE)
		{
			UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: PLAY ignored -- there is nothing on disk to play."));
			return;
		}
		bPaused = false;
		PlayFromBank(Bank, /*bAdvance=*/false, /*bCrossfade=*/false);
		return;
	}

	bPaused = !bPaused;
	// THE VOICE, NOT THE VOLUME. See the header: a muted cue keeps running and
	// the player comes back to a different bar of it.
	Component->SetPaused(bPaused);
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: %s at %.1fs ('%s')."),
	       bPaused ? TEXT("PAUSED") : TEXT("RESUMED"), ElapsedSeconds(), *TrackName);
}

int32 FVoxelUIMusic::TrackNumber() const
{
	if (!Banks.IsValidIndex(ActiveBank))
	{
		return 0;
	}
	return Banks[ActiveBank].Shuffle.Cursor + 1;
}

int32 FVoxelUIMusic::TrackCount() const
{
	return Banks.IsValidIndex(ActiveBank) ? Banks[ActiveBank].Paths.Num() : 0;
}

double FVoxelUIMusic::ElapsedSeconds() const
{
	if (SampleRate <= 0 || NumChannels <= 0)
	{
		return 0.0;
	}
	int32 Cursor = 0;
	{
		FScopeLock Lock(&PcmGuard);
		Cursor = PlayCursor;
	}
	// The queued cursor runs a whole prebuffer ahead of what is coming out of
	// the speakers. Subtracting what the device has not consumed yet is what
	// makes this the LISTENER's position, which is the only one PREV and the
	// PAUSED log line are about.
	const int32 Queued = Wave.IsValid() ? Wave->GetAvailableAudioByteCount() : 0;
	const double Bytes = static_cast<double>(FMath::Max(0, Cursor - Queued));
	return Bytes / static_cast<double>(SampleRate * NumChannels * 2);
}

void FVoxelUIMusic::ApplyVolume()
{
	if (!Component.IsValid())
	{
		return; // silent, and the next StartRandom will apply it
	}
	Component->SetVolumeMultiplier(VoxelAudioUserSettings::GetEffectiveMusicVolume());
}

bool FVoxelUIMusic::PoolChangesAllowed() const
{
	if (Context != EVoxelMusicContext::InWorld)
	{
		// The menu and the loading curtain always run their pool. Neither the
		// unattended rule nor the MusicInGame setting is about them --
		// EnsureMusicInGame's own comment is explicit that "this does NOT
		// change the menu's music".
		return true;
	}
	// UNATTENDED RUNS STAY SILENT IN GAME, and this is the second of the two
	// guards rather than the only one: UVoxelScreensUISubsystem::
	// EnsureMusicInGame already refuses to START a cue on a leg, for the
	// measurement reason it records. This one refuses to CHANGE POOLS, so a
	// leg that inherited a cue from an attended menu cannot start reading pool
	// folders off disk inside the window the leg is measuring.
	if (FApp::IsUnattended())
	{
		return false;
	}
	// THE PLAYER TURNED IT OFF. Without this, the hand-off's fade-and-stop
	// would be undone by the very next poll: Menu -> Explore is a pool change,
	// and a pool change starts a cue. The setting is read at 2 Hz rather than
	// cached because it is a GConfig map lookup and because a player who
	// unticks the row expects the next poll to honour it -- although the cue
	// they are already hearing keeps playing, which is what SetMusicInGame's
	// comment promises.
	return VoxelAudioUserSettings::GetMusicInGame();
}

void FVoxelUIMusic::BeginCrossfadeOut()
{
	if (!Component.IsValid() || !Wave.IsValid())
	{
		return;
	}
	// A SECOND POOL CHANGE INSIDE ONE CROSSFADE. Rare (the poll is 2 Hz and
	// the fade is 3 s, so it needs the player to step on and off a boat inside
	// three seconds) but not impossible, and two outgoing voices would need two
	// slots. The older one is cut; it is already most of the way down.
	FinishCrossfade();

	{
		FScopeLock Lock(&PcmGuard);
		// HAND IT ENOUGH BYTES TO FINISH ON ITS OWN. After this it is fed
		// nothing more: ActiveWave stops naming it, so OnUnderflow returns
		// immediately for every later callback from it, exactly as it already
		// does for a wave we have moved on from.
		const int32 BytesPerSecond = FMath::Max(1, SampleRate * NumChannels * 2);
		const int32 Held = Wave->GetAvailableAudioByteCount();
		int32 Need = static_cast<int32>(kVoxelMusicCrossfadeSeconds * BytesPerSecond) - Held;
		while (Need > 0 && PlayCursor < PcmData.Num())
		{
			const int32 Chunk = FMath::Min(Need, PcmData.Num() - PlayCursor);
			Wave->QueueAudio(PcmData.GetData() + PlayCursor, Chunk);
			PlayCursor += Chunk;
			Need -= Chunk;
		}
		bFeeding = false;
		ActiveWave = nullptr;
	}
	// The end signal belongs to the cue that is leaving; the incoming one has
	// not started yet and must not inherit it.
	bEndPending.store(false, std::memory_order_relaxed);

	Component->FadeOut(kVoxelMusicCrossfadeSeconds, 0.f);
	FadingComponent = MoveTemp(Component);
	FadingWave = MoveTemp(Wave);
	FadingRemaining = kVoxelMusicCrossfadeSeconds + VoxelUIMusicDetail::kCrossfadeReapSlackSeconds;
	Component.Reset();
	Wave.Reset();
}

void FVoxelUIMusic::FinishCrossfade()
{
	if (FadingComponent.IsValid())
	{
		FadingComponent->Stop();
		FadingComponent->DestroyComponent();
	}
	FadingComponent.Reset();
	FadingWave.Reset();
	FadingRemaining = 0.0;
}

void FVoxelUIMusic::OnUnderflow(USoundWaveProcedural* InWave, int32 SamplesRequired)
{
	// AUDIO RENDER THREAD. Everything it touches is under PcmGuard, and the only
	// UObject call it makes is QueueAudio on the wave that called it -- which is
	// what the whole-cue-in-memory trade bought. It does NOT advance the pool,
	// log, or touch TrackName: it raises one flag and the game thread does the
	// rest (see TickGameThread).
	if (InWave == nullptr)
	{
		return;
	}

	FScopeLock Lock(&PcmGuard);
	if (!bFeeding || InWave != ActiveWave || PcmData.Num() == 0)
	{
		// Either we are between cues, or this is a callback from a wave we have
		// already moved on from -- including the outgoing half of a crossfade,
		// which is deliberately left to drain what it was handed.
		return;
	}

	int32 BytesWanted = FMath::Max(SamplesRequired * 2, 4096);
	BytesWanted = FMath::Min(BytesWanted, PcmData.Num());

	while (BytesWanted > 0 && PlayCursor < PcmData.Num())
	{
		const int32 Chunk = FMath::Min(BytesWanted, PcmData.Num() - PlayCursor);
		InWave->QueueAudio(PcmData.GetData() + PlayCursor, Chunk);
		PlayCursor += Chunk;
		BytesWanted -= Chunk;
	}

	if (PlayCursor >= PcmData.Num())
	{
		// THE END OF THE CUE IS A SIGNAL: stop feeding, and let the game thread
		// open the authored gap once the tail that is already queued has been
		// heard.
		bFeeding = false;
		bEndPending.store(true, std::memory_order_relaxed);
	}
}

bool FVoxelUIMusic::TickGameThread(float DeltaSeconds)
{
	// GAME THREAD, EVERY FRAME. The common case is one relaxed load, one double
	// subtract and a branch -- this project is frame-time bound and a per-frame
	// no-op is still a per-frame call (see UVoxelFrontEndSubsystem::IsTickable).
	const double Delta = static_cast<double>(DeltaSeconds);

	// 1. Retire a finished crossfade.
	if (FadingComponent.IsValid())
	{
		FadingRemaining -= Delta;
		if (FadingRemaining <= 0.0)
		{
			FinishCrossfade();
		}
	}

	// 2. Has the world changed which pool owns the music?
	//
	// UNATTENDED RUNS NEVER REACH THIS with a world, and the guard is here as
	// well as at UVoxelScreensUISubsystem::EnsureMusicInGame because a leg that
	// somehow started a cue must not then start reading pool folders off disk
	// in the middle of the window a streaming leg is measuring.
	if (Banks.Num() > 0 && PoolChangesAllowed())
	{
		SignalPollRemaining -= Delta;
		if (SignalPollRemaining <= 0.0)
		{
			SignalPollRemaining = VoxelUIMusicDetail::kSignalPollSeconds;
			EVoxelMusicPool Pool = EVoxelMusicPool::Menu;
			EVoxelMusicSlot Slot = EVoxelMusicSlot::None;
			const int32 Bank = ChooseBank(ReadSignals(), Pool, Slot);
			// A SLOT CHANGE IS NOT A POOL CHANGE. Walking from 17:59 to 18:01
			// must not interrupt the Explore cue that is playing; the new slot
			// is picked up by the next selection, which is what the borrow
			// chain and the gap are for. Only a POOL change ducks.
			if (Bank != INDEX_NONE && Pool != ActivePool && ActiveBank != INDEX_NONE)
			{
				const EVoxelMusicPool Leaving = ActivePool;
				UE_LOG(LogVoxelUI, Log,
				       TEXT("VoxelUIMusic: pool change %s -> %s (crossfade %.1fs)"),
				       VoxelMusicPoolName(Leaving), VoxelMusicPoolName(Pool),
				       kVoxelMusicCrossfadeSeconds);

				if (Leaving == EVoxelMusicPool::Combat)
				{
					// SECTION 3: Combat "ends with a 10-20 s cooldown tail
					// before Explore resumes". The tail IS the gap, so the
					// combat cue fades and the new pool waits it out rather
					// than cutting straight in.
					BeginCrossfadeOut();
					Release();
					ActiveBank = Bank;
					ActivePool = Pool;
					ActiveSlot = Slot;
					GapRemaining = PoolStream.FRandRange(kVoxelMusicCombatCooldownMin,
					                                     kVoxelMusicCombatCooldownMax);
					bInGap = true;
					UE_LOG(LogVoxelUI, Log,
					       TEXT("VoxelUIMusic: pool=%s slot=%s cue='(cooldown)' gap=%.1f"),
					       VoxelMusicPoolName(Pool), VoxelMusicSlotName(Slot), GapRemaining);
				}
				else
				{
					// THE GAP RESETS ON A POOL CHANGE (section 3, last bullet:
					// "so entering a town does not wait out a 90 s silence").
					// PlayFromBank clears it.
					PlayFromBank(Bank, /*bAdvance=*/true, /*bCrossfade=*/true);
				}
			}
			else if (Bank != INDEX_NONE && ActiveBank == INDEX_NONE && !bInGap)
			{
				// Nothing has ever played -- the pool machinery is what starts
				// it, which is the path a -VoxelNoMenu run takes.
				PlayFromBank(Bank, /*bAdvance=*/true, /*bCrossfade=*/false);
			}
		}
	}

	// 3. Run down an authored gap.
	if (bInGap)
	{
		if (!bPaused)
		{
			GapRemaining -= Delta;
			if (GapRemaining <= 0.0)
			{
				bInGap = false;
				GapRemaining = 0.0;
				if (ActiveBank != INDEX_NONE)
				{
					PlayFromBank(ActiveBank, /*bAdvance=*/true, /*bCrossfade=*/false);
				}
			}
		}
		return true;
	}

	// 4. Has the current cue finished?
	if (!bEndPending.load(std::memory_order_relaxed))
	{
		return true;
	}
	// A PAUSED CUE NEVER ADVANCES. Pausing inside the last two seconds of a cue
	// would otherwise leave the end signal raised and the queue frozen, and the
	// moment the device did drain it the pool would move on under a player who
	// had just stopped it.
	if (bPaused)
	{
		return true;
	}
	// The PCM has run out but up to a prebuffer of it is still queued in the
	// device. Advancing now would clip the last two seconds off every cue in
	// the library -- and section 3 is explicit that "a cue's own authored tail
	// ... counts toward the gap", so that tail has to be heard.
	if (Wave.IsValid() && Wave->GetAvailableAudioByteCount() > 0)
	{
		return true;
	}
	bEndPending.store(false, std::memory_order_relaxed);

	// THE CUE ENDS INTO SILENCE, NOT INTO THE NEXT CUE. This is the single
	// biggest behavioural change of 2026-09-08 and the whole of section 3:
	// "Never two cues touching."
	const FString Ended = TrackName;
	Release();
	// Kept so the HUD label names the cue through the silence rather than going
	// blank, which reads as a fault.
	TrackName = Ended;

	if (ActiveBank == INDEX_NONE || PendingGapSeconds <= 0.f)
	{
		// Combat, Stingers and Cinematic have no gap; so does an A/B run with
		// -VoxelMusicGap=0.
		if (ActiveBank != INDEX_NONE)
		{
			PlayFromBank(ActiveBank, /*bAdvance=*/true, /*bCrossfade=*/false);
		}
		return true;
	}

	GapRemaining = PendingGapSeconds;
	bInGap = true;
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: '%s' ended; %.1fs of authored silence."),
	       *Ended, GapRemaining);
	return true;
}

void FVoxelUIMusic::ArmTicker()
{
	if (bTickerArmed)
	{
		return;
	}
	// ARMED ONCE AND NEVER REMOVED, on purpose. The end-of-cue advance runs
	// inside this delegate and calls Release(); if Release() removed the ticker,
	// it would be removing the delegate that is currently executing. One
	// permanently registered callback that early-outs on an atomic load is the
	// cheaper and far safer shape.
	bTickerArmed = true;
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FVoxelUIMusic::TickGameThread));
}

void FVoxelUIMusic::FadeOut(float Seconds)
{
	// A FADE ALSO ENDS A GAP. Otherwise the hand-off that asked for silence
	// would be followed, forty seconds later, by a cue starting under a world
	// the player asked to be quiet.
	bInGap = false;
	GapRemaining = 0.0;
	if (!Component.IsValid())
	{
		return;
	}
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: fading '%s' out over %.2fs."), *TrackName, Seconds);
	Component->FadeOut(Seconds, 0.f);
	// The component is left alive for the fade to run. Release happens on
	// Stop(), which the front end calls once the curtain is down -- fading and
	// then immediately destroying the component would cut the fade off at its
	// first frame, which is worse than no fade at all because it sounds like a
	// bug rather than a choice.
}

void FVoxelUIMusic::Stop()
{
	FinishCrossfade();
	bInGap = false;
	GapRemaining = 0.0;
	Release();
}

void FVoxelUIMusic::Release()
{
	// THE DELEGATE IS DELIBERATELY NOT UNBOUND. Unbinding closes a hazard that
	// does not exist (the bound object is an immortal singleton, so it can never
	// dangle) and opens one that does: USoundWaveProcedural::GeneratePCMData
	// checks IsBound() and then Executes, both on the audio render thread and
	// neither synchronised, so an Unbind from the game thread can free the
	// delegate's storage between those two lines.
	//
	// What makes leaving it bound safe is ActiveWave: a callback from a wave we
	// have moved on from takes PcmGuard, sees InWave != ActiveWave and returns
	// without touching anything. The callbacks stop of their own accord as soon
	// as the audio device drops the stopped voice. The crossfade's outgoing
	// voice relies on exactly this.
	if (Component.IsValid())
	{
		Component->Stop();
		Component->DestroyComponent();
	}
	Component.Reset();
	Wave.Reset();

	{
		FScopeLock Lock(&PcmGuard);
		bFeeding = false;
		ActiveWave = nullptr;
		PcmData.Empty();
		PlayCursor = 0;
	}
	bEndPending.store(false, std::memory_order_relaxed);
	bPaused = false;
	TrackName.Reset();
	// SampleRate, NumChannels, the banks and their permutations are deliberately
	// kept: a Stop() followed by a PLAY has to resume the session's order rather
	// than re-shuffle it, and the cursor is the only record of where that order
	// was.
}

bool FVoxelUIMusic::IsPlaying() const
{
	return Component.IsValid();
}
