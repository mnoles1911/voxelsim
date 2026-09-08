#pragma once
// The soundtrack: a shuffled playlist of the loose .wav files on disk, played
// from the menu, carried into gameplay, and driven by three buttons and three
// keys.
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
//
// --- THE PLAYLIST (2026-09-07) ----------------------------------------------
//
// ONE SHUFFLE PER SESSION, NOT A RE-ROLL PER TRACK. The library is listed and
// shuffled once, the first time anything asks for music, and the session then
// walks that order. A fresh random pick at every track end would let the same
// file come up twice running, which is the single thing players notice about a
// shuffle and complain about; walking a permutation cannot do it. The shuffle
// is drawn from the caller's FRandomStream, so a seeded run still pairs the
// same art with the same music -- which is what the capture legs depend on.
//
// WHERE EACH TRANSITION COMES FROM, because there are three and they are not
// interchangeable:
//   * NEXT/PREV are the PLAYER, on the game thread, from a HUD button or a key.
//   * "the track ended" is the AUDIO RENDER THREAD noticing the PCM ran out.
//     It may not touch a UObject, the playlist or the log, so all it does is
//     set one flag; the advance itself happens on the game thread in
//     TickGameThread. This is the reason FTSTicker is here at all.
//   * FadeOut/Stop are the FRONT END, at hand-off or teardown.
//
// PAUSE IS THE COMPONENT'S PAUSE, NOT A VOLUME OF ZERO. UAudioComponent::
// SetPaused stops the voice and leaves the queued buffer and PlayCursor exactly
// where they were, so RESUMED continues from the same bar. Muting instead would
// keep burning through the track while the player thought it was stopped, and
// they would come back to a different part of the song -- which is a bug
// report, not a feature.

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/CriticalSection.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include <atomic>

class UAudioComponent;
class USoundWaveProcedural;
class UWorld;
struct FRandomStream;

class FVoxelUIMusic
{
public:
	VOXELEARTHUI_API static FVoxelUIMusic& Get();

	// Builds the session's shuffled playlist (once) and starts its first track.
	// No-op if a track is already playing, if the folder holds nothing playable,
	// or if there is no world to spawn a component in. Safe to call more than
	// once -- which is what lets the menu, the loading screen and the in-game
	// HUD all call it without any of them owning the decision.
	VOXELEARTHUI_API void StartRandom(UWorld* World, FRandomStream& Stream);

	// Fades to silence over Seconds, then releases everything. This is the
	// hand-off path when the player has asked for NO music in game;
	// FVoxelMenuLayout::MusicFadeOut is the duration the front end passes, and
	// it has been 1.5s since before there was any audio to apply it to.
	VOXELEARTHUI_API void FadeOut(float Seconds);

	// Immediate stop and release. Used on teardown, where a fade has nothing
	// left to fade into. Keeps the playlist and the current index, so a later
	// PLAY resumes the session's order rather than re-shuffling it.
	VOXELEARTHUI_API void Stop();

	// Re-reads VoxelAudioUserSettings and pushes master x music at the live
	// component. Called when a track starts and whenever either slider moves;
	// a no-op while silent, because the next StartRandom applies it anyway.
	VOXELEARTHUI_API void ApplyVolume();

	VOXELEARTHUI_API bool IsPlaying() const;

	// --- Transport (the HUD cluster and its hotkeys) -------------------------

	// Forward one track, wrapping at the end of the playlist.
	VOXELEARTHUI_API void Next();

	// Back one track, wrapping at the start -- EXCEPT within the first few
	// seconds of a track, where it restarts the current one instead. That is
	// what every music player does and what a listener expects: "back" means
	// "the beginning of what I am hearing" until the beginning is far enough
	// behind to have stopped being the obvious answer.
	VOXELEARTHUI_API void Previous();

	// Pause and resume from the same cursor. Also the PLAY button when nothing
	// is playing at all -- it starts the playlist rather than doing nothing,
	// which is the only reading of a play button a player will accept.
	VOXELEARTHUI_API void TogglePause();
	VOXELEARTHUI_API bool IsPaused() const { return bPaused; }

	// What is currently playing, for the HUD label, the log and captures. Empty
	// when silent.
	VOXELEARTHUI_API const FString& NowPlaying() const { return TrackName; }
	// The older name for the same thing, kept so nothing has to change.
	VOXELEARTHUI_API const FString& CurrentTrackName() const { return TrackName; }

	// 1-based position and length, for the HUD and the "(i/n)" in the log.
	// Number is 0 when there is no current track.
	VOXELEARTHUI_API int32 TrackNumber() const { return CurrentIndex + 1; }
	VOXELEARTHUI_API int32 TrackCount() const { return Playlist.Num(); }

	// Roughly where in the current track the listener is, in seconds. The queued
	// cursor minus what the audio device has not consumed yet, so it is the
	// AUDIBLE position rather than the fed one -- the two differ by the whole
	// prebuffer, which is 2 s and would have made the "restart or go back"
	// decision wrong for the first two seconds of every track.
	VOXELEARTHUI_API double ElapsedSeconds() const;

private:
	FVoxelUIMusic() = default;

	// One decoded track, filled by LoadWav before anything is installed. A
	// local rather than the members, so a failed load cannot leave the live
	// track half-overwritten.
	struct FLoadedTrack
	{
		TArray<uint8> Pcm;
		FString Name;
		int32 SampleRate = 0;
		int32 NumChannels = 0;
	};

	// Lists Content/Audio/Music and shuffles it. Once per session; later calls
	// are no-ops, so a re-entry to the menu does not re-roll the order.
	void BuildPlaylist(FRandomStream& Stream);

	// Reads a 16-bit PCM WAV. False (with a log) on anything it will not play.
	bool LoadWav(const FString& Path, FLoadedTrack& Out) const;

	// Stops whatever is playing and starts Playlist[Index], walking FORWARD
	// past anything that will not load -- one bad file must not mean silence
	// when the other twenty-nine are fine. False only when nothing in the whole
	// playlist would play.
	bool PlayIndex(int32 Index);

	// Audio-render-thread callback. Queues the next slice of PcmData and, at
	// the end of the track, stops feeding and raises bEndPending. It never
	// advances the playlist itself -- see the header.
	void OnUnderflow(USoundWaveProcedural* InWave, int32 SamplesRequired);

	// FTSTicker, game thread. Consumes bEndPending once the already-queued tail
	// has drained, and advances. Armed once, on the first track, and never
	// removed: removing a ticker from inside its own callback is exactly what
	// the end-of-track advance would otherwise have to do.
	bool TickGameThread(float DeltaSeconds);
	void ArmTicker();

	void Release();

	TStrongObjectPtr<USoundWaveProcedural> Wave;
	TStrongObjectPtr<UAudioComponent> Component;
	// Where to spawn the next component. Weak, because the world can go away
	// under a paused singleton and a stale raw pointer here would be a crash at
	// the next NEXT.
	TWeakObjectPtr<UWorld> MusicWorld;

	// GUARDS EVERYTHING THE AUDIO RENDER THREAD TOUCHES: PcmData, PlayCursor,
	// bFeeding and ActiveWave. Held for a memcpy on that thread and for a
	// pointer swap on the game thread -- never across the file read, which is
	// done into an FLoadedTrack local first for exactly this reason.
	//
	// A LOCK ON THE AUDIO THREAD IS THE CHEAP HALF OF WHAT IS ALREADY THERE.
	// The callback already memcpys hundreds of KB through QueueAudio; an
	// uncontended critical section is nothing beside it, and what it buys is
	// that a track change can no longer race a callback that is mid-copy out of
	// the array it is about to replace. Before the playlist there was one track
	// change per session and the race was theoretical; there is now one per
	// button press.
	mutable FCriticalSection PcmGuard;

	// The whole track, decoded, in memory.
	//
	// THE ALTERNATIVE WAS STREAMING FROM DISK ON THE AUDIO THREAD, and this is
	// the deliberate trade. The underflow callback runs on the audio render
	// thread; a blocking file read there is a stall in the one place a stall is
	// audible. Holding one decoded track costs 30-92 MB for the current set,
	// which is real but bounded, and is now held for the whole session rather
	// than only until hand-off -- the price of music that keeps playing in game.
	TArray<uint8> PcmData;
	int32 PlayCursor = 0;
	// False once the track has been queued to its end, and while nothing is
	// installed. The audio thread's early-out.
	bool bFeeding = false;
	// Compared, never dereferenced: a callback from a wave we have already moved
	// on from must not queue the NEW track's bytes into the OLD wave. This is
	// also what replaces the old unbind-on-teardown -- see Release().
	const USoundWaveProcedural* ActiveWave = nullptr;

	// Set by the audio thread when PcmData runs out; consumed by the ticker.
	// Atomic rather than PcmGuard-guarded so the common case -- every frame of
	// every session, nothing to do -- is one relaxed load and no lock.
	std::atomic<bool> bEndPending{false};

	// The session's shuffled order, as full paths.
	TArray<FString> Playlist;
	int32 CurrentIndex = INDEX_NONE;
	bool bPlaylistBuilt = false;

	FTSTicker::FDelegateHandle TickerHandle;
	bool bTickerArmed = false;

	bool bPaused = false;

	FString TrackName;
	int32 SampleRate = 0;
	int32 NumChannels = 0;
};
