#pragma once
// Menu and loading-screen music: one track, picked at random, played from a
// loose .wav on disk with no import step.
//
// WHY THERE IS NO .uasset HERE. The same reason there is none for the menu
// art: this front end is built and photographed without ever opening the
// editor, so anything it needs at runtime has to be readable from a plain file
// by path. FVoxelUIAssetLibrary already decodes the background JPEGs that way.
// This is the audio half of the same decision, and it settles the open
// question recorded in Content/Audio/SFX/README.md -- for MUSIC, at least:
// runtime decode, no import, no asset registry.
//
// THE FILES ARE THE DESIGNER'S AND ARE NOT COMMITTED. They live in
// ue-project/Content/Audio/Music/ and are gitignored: the set on this machine
// is 30 tracks and 1.1 GB, the largest single file is 92 MB, and GitHub
// refuses anything over 100 MB while warning above 50. Adding a track is
// dropping a .wav in that folder -- no code change, no registration, exactly
// like dropping a .jpg in Content/UI/Backgrounds.
//
// FORMAT: 16-bit PCM WAV only. Every file in the shipped set is 2ch/48kHz/16
// -bit PCM, which matches USoundWaveProcedural's own default SampleByteSize of
// 2 exactly. Anything else -- a different bit depth, a compressed WAV, or a
// file that is not RIFF/WAVE at all (the folder currently contains one .mp4)
// -- is SKIPPED WITH A LOG rather than guessed at or half-played.
//
// AN EMPTY FOLDER IS A SUPPORTED STATE, not a failure. CI has no music, and
// neither will another machine until its designer puts some there. The menu
// runs silent and says so once.

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class UAudioComponent;
class USoundWaveProcedural;
class UWorld;
struct FRandomStream;

class FVoxelUIMusic
{
public:
	VOXELEARTHUI_API static FVoxelUIMusic& Get();

	// Picks one track at random and starts it. No-op if a track is already
	// playing, if the folder holds nothing playable, or if there is no world
	// to spawn a component in. Safe to call more than once.
	VOXELEARTHUI_API void StartRandom(UWorld* World, FRandomStream& Stream);

	// Fades to silence over Seconds, then releases everything. This is the
	// hand-off path; FVoxelMenuLayout::MusicFadeOut is the duration the front
	// end passes, and it has been 1.5s since before there was any audio to
	// apply it to.
	VOXELEARTHUI_API void FadeOut(float Seconds);

	// Immediate stop and release. Used on teardown, where a fade has nothing
	// left to fade into.
	VOXELEARTHUI_API void Stop();

	VOXELEARTHUI_API bool IsPlaying() const;

	// What is currently playing, for the log and for captures. Empty when
	// silent.
	VOXELEARTHUI_API const FString& CurrentTrackName() const { return TrackName; }

private:
	FVoxelUIMusic() = default;

	// Reads a 16-bit PCM WAV into PcmData and fills the format fields. False
	// (with a log) on anything it will not play.
	bool LoadWav(const FString& Path);

	// Audio-render-thread callback. Queues the next slice of PcmData, wrapping
	// at the end so the track loops seamlessly for as long as the menu is up.
	void OnUnderflow(USoundWaveProcedural* Wave, int32 SamplesRequired);

	void Release();

	TStrongObjectPtr<USoundWaveProcedural> Wave;
	TStrongObjectPtr<UAudioComponent> Component;

	// The whole track, decoded, in memory.
	//
	// THE ALTERNATIVE WAS STREAMING FROM DISK ON THE AUDIO THREAD, and this is
	// the deliberate trade. The underflow callback runs on the audio render
	// thread; a blocking file read there is a stall in the one place a stall is
	// audible. Holding one decoded track costs 30-92 MB for the current set,
	// which is real but bounded, is released at hand-off, and is paid while the
	// player is reading a menu rather than while the world is streaming.
	TArray<uint8> PcmData;
	int32 PlayCursor = 0;

	FString TrackName;
	int32 SampleRate = 0;
	int32 NumChannels = 0;
};
