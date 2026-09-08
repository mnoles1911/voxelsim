#include "VoxelUIMusic.h"

#include "VoxelEarthUI.h" // LogVoxelUI
#include "VoxelAudioUserSettings.h"

#include "AudioDefines.h" // INDEFINITELY_LOOPING_DURATION
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Math/RandomStream.h"
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
// track already resident in full.
constexpr double kPrebufferSeconds = 2.0;

// How far into a track PREV stops meaning "start this one again" and starts
// meaning "the one before". Five seconds is the figure every music player has
// converged on; below it the listener has barely heard the track and cannot
// have meant it, above it they are asking to leave.
constexpr double kPrevRestartSeconds = 5.0;

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

void FVoxelUIMusic::BuildPlaylist(FRandomStream& Stream)
{
	if (bPlaylistBuilt)
	{
		// ONCE PER SESSION. See the header: the whole reason for a permutation
		// rather than a per-track roll is that a permutation cannot repeat, and
		// re-rolling it on a second visit to the menu would throw that away.
		return;
	}
	bPlaylistBuilt = true;

	const FString Dir = FPaths::ProjectContentDir() / VoxelUIMusicDetail::kMusicSubdir;
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.wav")), /*Files=*/true, /*Directories=*/false);

	if (Files.Num() == 0)
	{
		// NOT A FAILURE. CI has no music and neither does a fresh checkout --
		// the tracks are the designer's and are not committed. Said once, at
		// Log level, so a silent game is explained rather than mysterious.
		UE_LOG(LogVoxelUI, Log,
		       TEXT("VoxelUIMusic: no .wav files in %s -- the game runs silent. "
		            "Drop 16-bit PCM WAVs there to add music; no code change is needed."),
		       *Dir);
		return;
	}

	// Sorted before it is shuffled, and that is not decoration. IFileManager's
	// enumeration order is the filesystem's, which is not guaranteed stable
	// between machines or after a defragment -- so a seeded run would pair a
	// different track with the same background art on a different box. Sorting
	// first makes the permutation a pure function of the seed.
	Files.Sort();

	Playlist.Reset(Files.Num());
	for (const FString& File : Files)
	{
		Playlist.Add(Dir / File);
	}

	// Fisher-Yates, from the caller's stream -- the same one the backgrounds
	// were shuffled from, so a seeded run gets a reproducible pairing of art
	// and music.
	for (int32 I = Playlist.Num() - 1; I > 0; --I)
	{
		const int32 J = Stream.RandRange(0, I);
		if (J != I)
		{
			Playlist.Swap(I, J);
		}
	}

	UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: playlist built -- %d track(s), shuffled."), Playlist.Num());
}

bool FVoxelUIMusic::PlayIndex(int32 Index)
{
	if (Playlist.Num() == 0)
	{
		return false;
	}
	UWorld* World = MusicWorld.Get();
	if (World == nullptr)
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelUIMusic: no world to play in; music stays silent."));
		return false;
	}

	// The old voice goes FIRST, before the new track is read off disk. The
	// other order would hold two whole decoded tracks at once -- up to 184 MB
	// for this set -- to save a gap of silence the player is about to hear
	// anyway, on a project whose stated constraint is memory and frame time.
	Release();

	const int32 Count = Playlist.Num();
	for (int32 Step = 0; Step < Count; ++Step)
	{
		const int32 Try = ((Index + Step) % Count + Count) % Count;
		FLoadedTrack Track;
		if (!LoadWav(Playlist[Try], Track))
		{
			// One bad file must not mean silence when the rest are fine. The
			// folder currently contains an .mp4 among the WAVs, which is
			// exactly this case.
			continue;
		}

		USoundWaveProcedural* NewWave = NewObject<USoundWaveProcedural>();
		NewWave->SetSampleRate(Track.SampleRate);
		NewWave->NumChannels = Track.NumChannels;
		// Indefinite: the FILE's length is not the voice's length. The underflow
		// handler decides when the PCM has run out and the game thread decides
		// what happens next, so the wave must never terminate itself.
		NewWave->Duration = INDEFINITELY_LOOPING_DURATION;
		NewWave->SoundGroup = SOUNDGROUP_Music;
		NewWave->bLooping = false; // there is no looping any more: tracks advance
		NewWave->OnSoundWaveProceduralUnderflow =
			FOnSoundWaveProceduralUnderflow::CreateRaw(this, &FVoxelUIMusic::OnUnderflow);

		SampleRate = Track.SampleRate;
		NumChannels = Track.NumChannels;
		TrackName = Track.Name;
		CurrentIndex = Try;

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
				// A track shorter than the prebuffer is entirely queued here and
				// the underflow handler will never run for it. Raising the end
				// signal now is what stops such a file from wedging the playlist.
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
		// the two audio settings combine. Before Play(), so a track never sounds
		// for one buffer at full volume on a machine set to 20%.
		ApplyVolume();
		NewComponent->Play();
		bPaused = false;
		ArmTicker();
		return true;
	}

	UE_LOG(LogVoxelUI, Warning,
	       TEXT("VoxelUIMusic: %d file(s) present and none playable -- the game runs silent."), Count);
	return false;
}

void FVoxelUIMusic::StartRandom(UWorld* World, FRandomStream& Stream)
{
	if (World == nullptr)
	{
		return;
	}
	// Recorded even when a track is already playing: the menu's world dies at a
	// map reopen and the next NEXT needs somewhere to spawn a component.
	MusicWorld = World;

	BuildPlaylist(Stream);

	if (IsPlaying())
	{
		// ADOPT, DO NOT RESTART. The contract ADR-0009 recorded is that the
		// track the menu started is the same one the loading screen and now the
		// game keep playing, and this no-op is what makes that true without a
		// state flag anywhere else.
		return;
	}
	if (Playlist.Num() == 0)
	{
		return; // BuildPlaylist has already said so, once
	}

	if (PlayIndex(0))
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: playing '%s' (%d/%d)."),
		       *TrackName, TrackNumber(), TrackCount());
	}
}

void FVoxelUIMusic::Next()
{
	if (Playlist.Num() == 0)
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: NEXT ignored -- there is no playlist."));
		return;
	}
	// INDEX_NONE + 1 is 0, so "next" from a session that has played nothing is
	// the first track rather than a wrap to the last.
	const int32 Target = (CurrentIndex + 1) % Playlist.Num();
	if (PlayIndex(Target))
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: NEXT -> '%s' (%d/%d)."),
		       *TrackName, TrackNumber(), TrackCount());
	}
}

void FVoxelUIMusic::Previous()
{
	const int32 Count = Playlist.Num();
	if (Count == 0)
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: PREV ignored -- there is no playlist."));
		return;
	}

	const double Position = ElapsedSeconds();
	const bool bRestart = IsPlaying() && CurrentIndex >= 0
	                   && Position > VoxelUIMusicDetail::kPrevRestartSeconds;
	const int32 Base = CurrentIndex >= 0 ? CurrentIndex : 0;
	const int32 Target = bRestart ? Base : (CurrentIndex >= 0 ? (Base - 1 + Count) % Count : 0);

	if (PlayIndex(Target))
	{
		if (bRestart)
		{
			UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: PREV -> '%s' (%d/%d) restarted, %.1fs in."),
			       *TrackName, TrackNumber(), TrackCount(), Position);
		}
		else
		{
			UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: PREV -> '%s' (%d/%d)."),
			       *TrackName, TrackNumber(), TrackCount());
		}
	}
}

void FVoxelUIMusic::TogglePause()
{
	if (!Component.IsValid())
	{
		// A PLAY BUTTON WITH NOTHING LOADED STARTS THE MUSIC. The alternative --
		// a button that does nothing after the player stopped the track, or
		// after a run that never had a menu -- is the same as a broken button
		// from where they are sitting.
		if (Playlist.Num() == 0)
		{
			UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: PLAY ignored -- there is no playlist."));
			return;
		}
		const int32 Target = CurrentIndex >= 0 ? CurrentIndex : 0;
		if (PlayIndex(Target))
		{
			UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: RESUMED -- '%s' (%d/%d) from the start."),
			       *TrackName, TrackNumber(), TrackCount());
		}
		return;
	}

	bPaused = !bPaused;
	// THE VOICE, NOT THE VOLUME. See the header: a muted track keeps running and
	// the player comes back to a different bar of it.
	Component->SetPaused(bPaused);
	if (bPaused)
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: PAUSED at %.1fs ('%s')."), ElapsedSeconds(), *TrackName);
	}
	else
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: RESUMED at %.1fs ('%s')."), ElapsedSeconds(), *TrackName);
	}
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

void FVoxelUIMusic::OnUnderflow(USoundWaveProcedural* InWave, int32 SamplesRequired)
{
	// AUDIO RENDER THREAD. Everything it touches is under PcmGuard, and the only
	// UObject call it makes is QueueAudio on the wave that called it -- which is
	// what the whole-track-in-memory trade bought. It does NOT advance the
	// playlist, log, or touch TrackName: it raises one flag and the game thread
	// does the rest (see TickGameThread).
	if (InWave == nullptr)
	{
		return;
	}

	FScopeLock Lock(&PcmGuard);
	if (!bFeeding || InWave != ActiveWave || PcmData.Num() == 0)
	{
		// Either we are between tracks, or this is a callback from a wave we
		// have already moved on from. Queueing here would put the new track's
		// bytes into the old voice.
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
		// THE END OF THE TRACK USED TO BE A WRAP. It is now a signal: stop
		// feeding, and let the game thread pick the next track once the tail
		// that is already queued has been heard.
		bFeeding = false;
		bEndPending.store(true, std::memory_order_relaxed);
	}
}

bool FVoxelUIMusic::TickGameThread(float /*DeltaSeconds*/)
{
	// GAME THREAD, EVERY FRAME, AND ALMOST ALWAYS ONE RELAXED LOAD. The flag is
	// atomic rather than guarded so that the overwhelmingly common case costs no
	// lock at all -- this project is frame-time bound and a per-frame no-op is
	// still a per-frame call (see UVoxelFrontEndSubsystem::IsTickable).
	if (!bEndPending.load(std::memory_order_relaxed))
	{
		return true;
	}
	// A PAUSED TRACK NEVER ADVANCES. Pausing inside the last two seconds of a
	// track would otherwise leave the end signal raised and the queue frozen,
	// and the moment the device did drain it the playlist would move on under a
	// player who had just stopped it.
	if (bPaused)
	{
		return true;
	}
	// The PCM has run out but up to a prebuffer of it is still queued in the
	// device. Advancing now would clip the last two seconds off every track in
	// the library, which is exactly the kind of defect that gets heard and never
	// diagnosed.
	if (Wave.IsValid() && Wave->GetAvailableAudioByteCount() > 0)
	{
		return true;
	}
	bEndPending.store(false, std::memory_order_relaxed);

	if (Playlist.Num() == 0)
	{
		return true;
	}
	const int32 Target = (CurrentIndex + 1) % Playlist.Num();
	if (PlayIndex(Target))
	{
		UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: track ended -> '%s' (%d/%d)."),
		       *TrackName, TrackNumber(), TrackCount());
	}
	return true;
}

void FVoxelUIMusic::ArmTicker()
{
	if (bTickerArmed)
	{
		return;
	}
	// ARMED ONCE AND NEVER REMOVED, on purpose. The end-of-track advance runs
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
	Release();
}

void FVoxelUIMusic::Release()
{
	// THE DELEGATE IS DELIBERATELY NOT UNBOUND, and this is a change from the
	// single-track version, which unbound here. Unbinding closes a hazard that
	// does not exist (the bound object is an immortal singleton, so it can never
	// dangle) and opens one that does: USoundWaveProcedural::GeneratePCMData
	// checks IsBound() and then Executes, both on the audio render thread and
	// neither synchronised, so an Unbind from the game thread can free the
	// delegate's storage between those two lines. That window was survivable
	// while there was exactly one track change per session; with a NEXT button
	// there is one per press.
	//
	// What makes leaving it bound safe is ActiveWave: a callback from a wave we
	// have moved on from takes PcmGuard, sees InWave != ActiveWave and returns
	// without touching anything. The callbacks stop of their own accord as soon
	// as the audio device drops the stopped voice.
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
	// SampleRate, NumChannels, Playlist and CurrentIndex are deliberately kept:
	// a Stop() followed by a PLAY has to resume the session's order rather than
	// re-shuffle it, and the index is the only record of where that order was.
}

bool FVoxelUIMusic::IsPlaying() const
{
	return Component.IsValid();
}
