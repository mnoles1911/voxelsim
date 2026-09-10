#pragma once
// The soundtrack: a POOL of shuffled playlists over the loose .wav files on
// disk, chosen by what the player is doing and by the sky clock, separated by
// authored silence, played from the menu, carried into gameplay, and driven by
// three buttons and three keys.
//
// docs/music-design.md IS THE SPEC and this file implements sections 2, 3, 4
// and 8 of it. The pure half -- priority, hour boundaries, borrowing, gap
// ranges, the no-repeat shuffle -- lives in VoxelMusicPools.h so it can be
// asserted headlessly (VoxelMusicPoolTests.cpp). What is left here is the part
// that needs a UWorld, an audio device and a file system.
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
// ue-project/Content/Audio/Music/<Pool>/ and are gitignored: the set on this
// machine is 29 tracks and ~1.0 GB, the largest single file is 92 MB, and
// GitHub refuses anything over 100 MB while warning above 50. Adding a track is
// dropping a .wav in the right POOL FOLDER -- no code change, no registration,
// exactly like dropping a .jpg in Content/UI/Backgrounds. Content/Audio/Music/
// MUSIC_CREDITS.md lists what is there now and which folder each cue is in.
//
// FORMAT: 16-bit PCM WAV only. Every file in the shipped set is 2ch/48kHz/16
// -bit PCM, which matches USoundWaveProcedural's own default SampleByteSize of
// 2 exactly. Anything else -- a different bit depth, a compressed WAV, or a
// file that is not RIFF/WAVE at all (the library still contains one .mp4, for
// which no ffmpeg was available on the box that did this port) -- is SKIPPED
// WITH A LOG rather than guessed at or half-played.
//
// AN EMPTY FOLDER IS A SUPPORTED STATE, not a failure, and that now applies per
// pool: Explore/Dawn, Explore/Dusk and Explore/Rain are empty today. A pool
// with no cues is skipped and the next one down the priority ladder plays. CI
// has no music at all, and neither will another machine until its designer puts
// some there; the game runs silent and says so once.
//
// --- WHAT REPLACED THE ONE FLAT SHUFFLE (2026-09-08) -------------------------
//
// Until today the whole library was one shuffled list: the main title, a boss
// battle, a game-over sting and fourteen exploration cues, back to back, in the
// menu and in the world alike. The design document's one-line specification is
// the opposite of that, so:
//
//   * ONE SHUFFLED PLAYLIST PER FOLDER, not one per session. Explore/Day,
//     Explore/Night, Cave, Town, Water, Combat, Menu, Stingers, Cinematic --
//     see VoxelMusicBanks(). The permutation is re-drawn when it is exhausted
//     (no repeat until then), and the seam between two permutations is checked
//     so a cue cannot land twice running across it.
//   * WHICH FOLDER IS A FUNCTION OF THE WORLD, re-evaluated at 2 Hz:
//     VoxelMusicResolvePool over signals read in ReadSignals.
//   * SILENCE IS AUTHORED. A gap of 45-120 s (Explore/Cave/Town/Water) or
//     20-40 s (Menu) is drawn PER CUE, at the moment the cue is chosen, and
//     runs after it. It is reset by a pool change.
//   * A POOL CHANGE IS A CROSSFADE. See BeginCrossfadeOut for how one is
//     performed without ever holding two decoded tracks.
//
// WHERE EACH TRANSITION COMES FROM, because there are now four and they are not
// interchangeable:
//   * NEXT/PREV are the PLAYER, on the game thread, from a HUD button or a key.
//     They act on the CURRENT POOL and they cancel a gap in progress.
//   * "the track ended" is the AUDIO RENDER THREAD noticing the PCM ran out.
//     It may not touch a UObject, the playlist or the log, so all it does is
//     set one flag; the advance itself happens on the game thread in
//     TickGameThread, and now opens the gap rather than starting a cue.
//   * "the gap ran out" and "the pool changed" are TickGameThread as well.
//   * FadeOut/Stop are the FRONT END, at hand-off or teardown.
//
// PAUSE IS THE COMPONENT'S PAUSE, NOT A VOLUME OF ZERO. UAudioComponent::
// SetPaused stops the voice and leaves the queued buffer and PlayCursor exactly
// where they were, so RESUMED continues from the same bar. Muting instead would
// keep burning through the track while the player thought it was stopped, and
// they would come back to a different part of the song -- which is a bug
// report, not a feature. PAUSE ALSO FREEZES A GAP, for the same reason: a
// player who paused during the silence must not find the next cue already
// running when they come back.

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/CriticalSection.h"
#include "Math/RandomStream.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "VoxelMusicPools.h"

#include <atomic>

class AVoxelClipmapActor;
class UAudioComponent;
class USoundWaveProcedural;
class UWorld;

// WHERE THE PLAYER IS, as far as the music is concerned. Set explicitly by the
// front end rather than derived from "is there a pawn", because the two are not
// the same thing at three different moments of the hand-off and a derived
// answer would flicker through Explore for a frame on the way into the world.
enum class EVoxelMusicContext : uint8
{
	Menu,    // the title screen
	Loading, // the curtain. Section 8: the Menu pool covers both.
	InWorld  // the player has the world
};

class FVoxelUIMusic
{
public:
	VOXELEARTHUI_API static FVoxelUIMusic& Get();

	// Builds every pool's playlist (once), resolves the pool for the current
	// context and starts its first cue. No-op if music is already engaged (a
	// cue playing OR a gap running), if nothing playable is on disk, or if
	// there is no world to spawn a component in. Safe to call more than once --
	// which is what lets the menu, the loading screen and the in-game HUD all
	// call it without any of them owning the decision.
	VOXELEARTHUI_API void StartRandom(UWorld* World, FRandomStream& Stream);

	// Menu / Loading / InWorld. The Menu pool covers the first two
	// (docs/music-design.md section 2, row 5); Explore's own condition is the
	// third. Calling this with the value it already has is free.
	VOXELEARTHUI_API void SetContext(EVoxelMusicContext InContext);
	VOXELEARTHUI_API EVoxelMusicContext GetContext() const { return Context; }

	// Fades to silence over Seconds, then releases everything. This is the
	// hand-off path when the player has asked for NO music in game;
	// FVoxelMenuLayout::MusicFadeOut is the duration the front end passes, and
	// it has been 1.5s since before there was any audio to apply it to.
	VOXELEARTHUI_API void FadeOut(float Seconds);

	// Immediate stop and release. Used on teardown, where a fade has nothing
	// left to fade into. Keeps the pools and their permutations, so a later
	// PLAY resumes where the session was rather than re-shuffling everything.
	VOXELEARTHUI_API void Stop();

	// Re-reads VoxelAudioUserSettings and pushes master x music at the live
	// component. Called when a cue starts and whenever either slider moves;
	// a no-op while silent, because the next StartRandom applies it anyway.
	VOXELEARTHUI_API void ApplyVolume();

	VOXELEARTHUI_API bool IsPlaying() const;

	// --- Transport (the HUD cluster and its hotkeys) -------------------------
	//
	// ALL THREE ACT ON THE CURRENT POOL (section 8). They never change which
	// pool is playing -- that is the world's decision, not the player's -- and
	// NEXT/PREV cancel a gap in progress, because a transport button that
	// appears to do nothing for the next ninety seconds is a broken button.

	// Forward one cue in the current pool's permutation, re-drawing it when it
	// is exhausted.
	VOXELEARTHUI_API void Next();

	// Back one cue -- EXCEPT within the first few seconds of a cue, where it
	// restarts the current one instead. That is what every music player does
	// and what a listener expects: "back" means "the beginning of what I am
	// hearing" until the beginning is far enough behind to have stopped being
	// the obvious answer.
	VOXELEARTHUI_API void Previous();

	// Pause and resume from the same cursor, and freeze/thaw a gap. Also the
	// PLAY button when nothing is playing at all -- it starts the pool rather
	// than doing nothing, which is the only reading of a play button a player
	// will accept.
	VOXELEARTHUI_API void TogglePause();
	VOXELEARTHUI_API bool IsPaused() const { return bPaused; }

	// What is currently playing, for the HUD label, the log and captures. Held
	// across a gap -- the label naming the cue that just finished is a better
	// answer during ninety seconds of authored silence than an empty label,
	// which reads as "the music broke". Empty only when nothing has played.
	VOXELEARTHUI_API const FString& NowPlaying() const { return TrackName; }
	// The older name for the same thing, kept so nothing has to change.
	VOXELEARTHUI_API const FString& CurrentTrackName() const { return TrackName; }

	// Which pool and slot own the music right now, for the log and the HUD.
	VOXELEARTHUI_API EVoxelMusicPool CurrentPool() const { return ActivePool; }
	VOXELEARTHUI_API EVoxelMusicSlot CurrentSlot() const { return ActiveSlot; }

	// 1-based position in the CURRENT POOL's permutation, and its length.
	// Number is 0 when there is no current cue.
	VOXELEARTHUI_API int32 TrackNumber() const;
	VOXELEARTHUI_API int32 TrackCount() const;

	// Roughly where in the current cue the listener is, in seconds. The queued
	// cursor minus what the audio device has not consumed yet, so it is the
	// AUDIBLE position rather than the fed one -- the two differ by the whole
	// prebuffer, which is 2 s and would have made the "restart or go back"
	// decision wrong for the first two seconds of every cue.
	VOXELEARTHUI_API double ElapsedSeconds() const;

	// Seconds of authored silence still to run, 0 when a cue is playing. For
	// the log and for tests of the transport's gap cancellation.
	VOXELEARTHUI_API double GapRemainingSeconds() const { return bInGap ? GapRemaining : 0.0; }

private:
	FVoxelUIMusic() = default;

	// One decoded cue, filled by LoadWav before anything is installed. A
	// local rather than the members, so a failed load cannot leave the live
	// cue half-overwritten.
	struct FLoadedTrack
	{
		TArray<uint8> Pcm;
		FString Name;
		int32 SampleRate = 0;
		int32 NumChannels = 0;
	};

	// One folder's worth of cues and the permutation being walked through them.
	// Parallel to VoxelMusicBanks(); Paths are full paths, sorted before the
	// shuffle so a seeded run is reproducible across machines.
	struct FBankState
	{
		TArray<FString> Paths;
		TArray<FString> Names; // GetBaseFilename of each path, for the recents
		FVoxelMusicShuffle Shuffle;
	};

	// Lists every pool folder and seeds the shared stream. Once per session;
	// later calls are no-ops, so a re-entry to the menu does not re-roll
	// anything.
	void BuildBanks(FRandomStream& Stream);

	// Reads a 16-bit PCM WAV. False (with a log) on anything it will not play.
	bool LoadWav(const FString& Path, FLoadedTrack& Out) const;

	// The world signals section 2's priority ladder is resolved over. Reads the
	// sky clock, the pawn and the clipmap's veil latch; the Combat, Town, Rain
	// and Stinger signals are stubbed (see the struct's own comments).
	FVoxelMusicSignals ReadSignals() const;

	// Which bank should be playing, given the signals. Walks the priority
	// ladder down past any pool whose folders are empty, and for Explore walks
	// the slot's borrow chain until it has kVoxelMusicSlotMinCues to draw from.
	// INDEX_NONE when the whole library is empty.
	int32 ChooseBank(const FVoxelMusicSignals& Signals, EVoxelMusicPool& OutPool,
	                 EVoxelMusicSlot& OutSlot) const;

	// Advances Bank's permutation and plays what it lands on, walking FORWARD
	// past anything that will not load -- one bad file must not mean silence
	// when the other ten are fine. Draws and records the cue's gap, remembers
	// it in the user settings, and writes the one selection log line. False
	// only when nothing in the bank would play.
	bool PlayFromBank(int32 BankIndex, bool bAdvance, bool bCrossfade);

	// Installs one decoded cue on a fresh component. The bottom half of
	// PlayFromBank, split out because Previous() needs to restart the current
	// file without touching the permutation.
	bool InstallTrack(const FString& Path, bool bCrossfade);

	// May the 2 Hz poll change pools right now? False on an unattended leg and
	// false when the player has turned music off in game -- without which the
	// hand-off's fade-and-stop would be undone by the next poll.
	bool PoolChangesAllowed() const;

	// Hands the outgoing voice enough already-decoded bytes to finish a
	// crossfade on its own, then lets go of it.
	//
	// THIS IS HOW A CROSSFADE HAPPENS WITHOUT TWO DECODED TRACKS. The header's
	// original memory argument still stands -- holding two cues at once is up
	// to 184 MB for this set -- so the outgoing wave is not kept fed. Instead
	// it is QUEUED with kVoxelMusicCrossfadeSeconds of audio in one go
	// (~576 KB), un-registered as the active wave so the underflow callback
	// ignores it, told to fade, and destroyed by the ticker when the fade is
	// done. The incoming cue starts immediately with FadeIn over the same
	// duration. Both voices are live at once; only one decoded buffer is.
	void BeginCrossfadeOut();
	void FinishCrossfade();

	// Audio-render-thread callback. Queues the next slice of PcmData and, at
	// the end of the cue, stops feeding and raises bEndPending. It never
	// advances anything itself -- see the header.
	void OnUnderflow(USoundWaveProcedural* InWave, int32 SamplesRequired);

	// FTSTicker, game thread. Four jobs, in this order: retire a finished
	// crossfade, poll the world for a pool change, run down an authored gap,
	// and consume bEndPending once the already-queued tail has drained. Armed
	// once and never removed: removing a ticker from inside its own callback is
	// exactly what the end-of-cue advance would otherwise have to do.
	bool TickGameThread(float DeltaSeconds);
	void ArmTicker();

	// True while a cue is playing OR a gap is running. The adopt-don't-restart
	// predicate: StartRandom must be a no-op during authored silence too, or
	// every call site that "just makes sure music is on" would cut the silence
	// short.
	bool IsEngaged() const { return Component.IsValid() || bInGap; }

	void Release();

	TStrongObjectPtr<USoundWaveProcedural> Wave;
	TStrongObjectPtr<UAudioComponent> Component;

	// The outgoing half of a crossfade: a component fading out on bytes it
	// already holds, with no PCM behind it. Destroyed by the ticker.
	TStrongObjectPtr<USoundWaveProcedural> FadingWave;
	TStrongObjectPtr<UAudioComponent> FadingComponent;
	double FadingRemaining = 0.0;

	// Where to spawn the next component. Weak, because the world can go away
	// under a paused singleton and a stale raw pointer here would be a crash at
	// the next NEXT.
	TWeakObjectPtr<UWorld> MusicWorld;

	// The clipmap actor, found by iteration once and cached. Weak for the same
	// reason MusicWorld is.
	mutable TWeakObjectPtr<AVoxelClipmapActor> Clipmap;

	// GUARDS EVERYTHING THE AUDIO RENDER THREAD TOUCHES: PcmData, PlayCursor,
	// bFeeding and ActiveWave. Held for a memcpy on that thread and for a
	// pointer swap on the game thread -- never across the file read, which is
	// done into an FLoadedTrack local first for exactly this reason.
	//
	// A LOCK ON THE AUDIO THREAD IS THE CHEAP HALF OF WHAT IS ALREADY THERE.
	// The callback already memcpys hundreds of KB through QueueAudio; an
	// uncontended critical section is nothing beside it, and what it buys is
	// that a cue change can no longer race a callback that is mid-copy out of
	// the array it is about to replace.
	mutable FCriticalSection PcmGuard;

	// The whole cue, decoded, in memory.
	//
	// THE ALTERNATIVE WAS STREAMING FROM DISK ON THE AUDIO THREAD, and this is
	// the deliberate trade. The underflow callback runs on the audio render
	// thread; a blocking file read there is a stall in the one place a stall is
	// audible. Holding one decoded cue costs 17-92 MB for the current set,
	// which is real but bounded, and is held for as long as the cue plays --
	// the price of music that keeps playing in game. An authored gap is the
	// one time this project holds NO decoded audio at all.
	TArray<uint8> PcmData;
	int32 PlayCursor = 0;
	// False once the cue has been queued to its end, and while nothing is
	// installed. The audio thread's early-out.
	bool bFeeding = false;
	// Compared, never dereferenced: a callback from a wave we have already moved
	// on from must not queue the NEW cue's bytes into the OLD wave. This is
	// also what replaces the old unbind-on-teardown -- see Release() -- and it
	// is what makes the crossfade above safe.
	const USoundWaveProcedural* ActiveWave = nullptr;

	// Set by the audio thread when PcmData runs out; consumed by the ticker.
	// Atomic rather than PcmGuard-guarded so the common case -- every frame of
	// every session, nothing to do -- is one relaxed load and no lock.
	std::atomic<bool> bEndPending{false};

	// One per VoxelMusicBanks() entry, in the same order.
	TArray<FBankState> Banks;
	bool bBanksBuilt = false;
	// Every shuffle and every gap draw comes from here, seeded once from the
	// caller's stream, so a seeded run is reproducible end to end.
	FRandomStream PoolStream;

	EVoxelMusicContext Context = EVoxelMusicContext::Menu;
	EVoxelMusicPool ActivePool = EVoxelMusicPool::Menu;
	EVoxelMusicSlot ActiveSlot = EVoxelMusicSlot::None;
	int32 ActiveBank = INDEX_NONE;

	// Drawn when the cue is CHOSEN (section 3: "drawn fresh each time") and
	// logged with it; started when that cue ends.
	float PendingGapSeconds = 0.f;
	double GapRemaining = 0.0;
	bool bInGap = false;

	// Signals are polled on a timer rather than every frame: the answer cannot
	// change faster than the player can walk into a cave, and the poll touches
	// a subsystem, a pawn cast and a weak pointer.
	double SignalPollRemaining = 0.0;

	FTSTicker::FDelegateHandle TickerHandle;
	bool bTickerArmed = false;

	bool bPaused = false;

	FString TrackName;
	int32 SampleRate = 0;
	int32 NumChannels = 0;
};
