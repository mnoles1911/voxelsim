#include "VoxelUIMusic.h"

#include "VoxelEarthUI.h" // LogVoxelUI

#include "AudioDefines.h" // INDEFINITELY_LOOPING_DURATION
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "Sound/SoundWaveProcedural.h"

namespace VoxelUIMusicDetail
{
// Named, not anonymous: tools/lint-unity-collisions.py.

const TCHAR* const kMusicSubdir = TEXT("Audio/Music");

// Queued ahead of the audio thread's demand so a slow frame cannot starve it.
// 2 seconds at 48kHz stereo 16-bit is ~384 KB, which is nothing against a
// track already resident in full.
constexpr double kPrebufferSeconds = 2.0;

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

bool FVoxelUIMusic::LoadWav(const FString& Path)
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

	PcmData.Reset(static_cast<int32>(Format.DataSize));
	PcmData.Append(Bytes.GetData() + Format.DataOffset, static_cast<int32>(Format.DataSize));
	SampleRate = Format.SampleRate;
	NumChannels = Format.NumChannels;
	PlayCursor = 0;
	TrackName = FPaths::GetBaseFilename(Path);

	const double Seconds = static_cast<double>(Format.DataSize) / (Format.SampleRate * Format.NumChannels * 2);
	UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: loaded '%s' -- %.1fs, %dch %dHz 16-bit, %.1f MB."),
	       *TrackName, Seconds, NumChannels, SampleRate, PcmData.Num() / (1024.0 * 1024.0));
	return true;
}

void FVoxelUIMusic::StartRandom(UWorld* World, FRandomStream& Stream)
{
	if (IsPlaying() || World == nullptr)
	{
		return;
	}

	const FString Dir = FPaths::ProjectContentDir() / VoxelUIMusicDetail::kMusicSubdir;
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.wav")), /*Files=*/true, /*Directories=*/false);

	if (Files.Num() == 0)
	{
		// NOT A FAILURE. CI has no music and neither does a fresh checkout --
		// the tracks are the designer's and are not committed. Said once, at
		// Log level, so a silent menu is explained rather than mysterious.
		UE_LOG(LogVoxelUI, Log,
		       TEXT("VoxelUIMusic: no .wav files in %s -- the menu runs silent. "
		            "Drop 16-bit PCM WAVs there to add music; no code change is needed."),
		       *Dir);
		return;
	}

	// Shuffled the same way the backgrounds are, from the same stream, so a
	// seeded run gets a reproducible pairing of art and music.
	const int32 Pick = Stream.RandRange(0, Files.Num() - 1);
	if (!LoadWav(Dir / Files[Pick]))
	{
		// One bad file must not mean silence when 29 others are fine. Walk the
		// rest in order from the pick; the folder currently contains an .mp4
		// among the WAVs, which is exactly this case.
		bool bLoaded = false;
		for (int32 i = 1; i < Files.Num() && !bLoaded; ++i)
		{
			bLoaded = LoadWav(Dir / Files[(Pick + i) % Files.Num()]);
		}
		if (!bLoaded)
		{
			UE_LOG(LogVoxelUI, Warning,
			       TEXT("VoxelUIMusic: %d file(s) present and none playable -- the menu runs silent."),
			       Files.Num());
			return;
		}
	}

	USoundWaveProcedural* NewWave = NewObject<USoundWaveProcedural>();
	NewWave->SetSampleRate(SampleRate);
	NewWave->NumChannels = NumChannels;
	// Indefinite: the underflow handler wraps, so the track outlasts any menu.
	// The front end decides when it ends, not the file's length.
	NewWave->Duration = INDEFINITELY_LOOPING_DURATION;
	NewWave->SoundGroup = SOUNDGROUP_Music;
	NewWave->bLooping = false; // looping is ours, by wrapping the cursor
	NewWave->OnSoundWaveProceduralUnderflow =
		FOnSoundWaveProceduralUnderflow::CreateRaw(this, &FVoxelUIMusic::OnUnderflow);
	Wave.Reset(NewWave);

	// Prime it before the component starts, so the first callback is not
	// already an underrun.
	const int32 PrebufferBytes =
		FMath::Min<int32>(PcmData.Num(),
		                  static_cast<int32>(VoxelUIMusicDetail::kPrebufferSeconds * SampleRate * NumChannels * 2));
	NewWave->QueueAudio(PcmData.GetData(), PrebufferBytes);
	PlayCursor = PrebufferBytes % FMath::Max(PcmData.Num(), 1);

	UAudioComponent* NewComponent = UGameplayStatics::CreateSound2D(
		World, NewWave, /*VolumeMultiplier=*/1.f, /*PitchMultiplier=*/1.f, /*StartTime=*/0.f,
		/*ConcurrencySettings=*/nullptr, /*bPersistAcrossLevelTransition=*/false, /*bAutoDestroy=*/false);
	if (NewComponent == nullptr)
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("VoxelUIMusic: could not create an audio component for '%s'."), *TrackName);
		Release();
		return;
	}
	Component.Reset(NewComponent);
	NewComponent->Play();

	UE_LOG(LogVoxelUI, Log, TEXT("VoxelUIMusic: playing '%s' (%d track(s) available)."), *TrackName, Files.Num());
}

void FVoxelUIMusic::OnUnderflow(USoundWaveProcedural* InWave, int32 SamplesRequired)
{
	// AUDIO RENDER THREAD. Everything it touches is either read-only for the
	// life of the track (PcmData, SampleRate, NumChannels) or touched only
	// here (PlayCursor). No file I/O, no allocation, no UObject calls beyond
	// QueueAudio -- which is what the whole-track-in-memory trade bought.
	if (InWave == nullptr || PcmData.Num() == 0)
	{
		return;
	}

	int32 BytesWanted = FMath::Max(SamplesRequired * 2, 4096);
	BytesWanted = FMath::Min(BytesWanted, PcmData.Num());

	while (BytesWanted > 0)
	{
		const int32 Chunk = FMath::Min(BytesWanted, PcmData.Num() - PlayCursor);
		InWave->QueueAudio(PcmData.GetData() + PlayCursor, Chunk);
		PlayCursor += Chunk;
		BytesWanted -= Chunk;
		if (PlayCursor >= PcmData.Num())
		{
			PlayCursor = 0; // wrap: the track loops for as long as the menu is up
		}
	}
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
	if (Component.IsValid())
	{
		Component->Stop();
	}
	Release();
}

void FVoxelUIMusic::Release()
{
	if (Wave.IsValid())
	{
		// Unbind before the delegate's owner can go away underneath the audio
		// thread. CreateRaw has no lifetime tracking, which is the price of
		// binding to a non-UObject singleton.
		Wave->OnSoundWaveProceduralUnderflow.Unbind();
	}
	if (Component.IsValid())
	{
		Component->DestroyComponent();
	}
	Component.Reset();
	Wave.Reset();
	PcmData.Empty();
	PlayCursor = 0;
	TrackName.Reset();
}

bool FVoxelUIMusic::IsPlaying() const
{
	return Component.IsValid();
}
