#include "VoxelLoadingCurtainThread.h"

#include "VoxelEarthUI.h"

#include "Engine/GameViewportClient.h"
#include "Engine/UserInterfaceSettings.h" // the UI scale the viewport path uses
#include "Engine/World.h"
#include "Slate/SceneViewport.h"              // FSceneViewport::GetSizeXY
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "MoviePlayer.h"
#include "MoviePlayerProxy.h"
#include "Widgets/SWidget.h"

// Named namespace, not anonymous: see tools/lint-unity-collisions.py.
namespace VoxelLoadingCurtainDetail
{
// The paint clock's state. A plain critical section rather than a lock-free
// ring: the only two writers are the game thread and the Slate loading thread,
// they never run at the same time by the bracketing invariant this file's
// header states, and an uncontended FCriticalSection is a few nanoseconds --
// paid once per PAINT, not per draw element.
FCriticalSection PaintLock;
double LastPaintSeconds = 0.0;
TArray<double> PendingIntervalsMs;
int64 PaintOverflows = 0;
int64 TotalPaints = 0;

// 4096 intervals is ~68 s of 60 Hz painting between two drains. A load that
// overflows this has gone more than a minute without the game thread reaching
// the front end's loading tick, which is a different and much worse bug than
// a missing sample -- the counter exists so that reading is available rather
// than inferred from a suspiciously small n.
constexpr int32 kMaxPendingIntervals = 4096;
} // namespace VoxelLoadingCurtainDetail

namespace VoxelLoadingCurtain
{
void NotePaint()
{
	using namespace VoxelLoadingCurtainDetail;
	const double Now = FPlatformTime::Seconds();
	FScopeLock Lock(&PaintLock);
	++TotalPaints;
	if (LastPaintSeconds > 0.0)
	{
		const double IntervalMs = (Now - LastPaintSeconds) * 1000.0;
		if (PendingIntervalsMs.Num() < kMaxPendingIntervals)
		{
			PendingIntervalsMs.Add(IntervalMs);
		}
		else
		{
			++PaintOverflows;
		}
	}
	LastPaintSeconds = Now;
}

void ResetPaintClock()
{
	using namespace VoxelLoadingCurtainDetail;
	FScopeLock Lock(&PaintLock);
	LastPaintSeconds = 0.0;
	PendingIntervalsMs.Reset();
	PaintOverflows = 0;
	TotalPaints = 0;
}

void DrainPaintIntervals(TFunctionRef<void(double)> Sink)
{
	using namespace VoxelLoadingCurtainDetail;
	TArray<double> Drained;
	{
		FScopeLock Lock(&PaintLock);
		Drained = MoveTemp(PendingIntervalsMs);
		PendingIntervalsMs.Reset();
	}
	// Outside the lock: Sink walks into VoxelFramePhase, which logs, and a
	// UE_LOG under a lock the Slate loading thread also takes is a deadlock
	// waiting for a bad day.
	for (const double IntervalMs : Drained)
	{
		Sink(IntervalMs);
	}
}

int64 PaintOverflowCount()
{
	using namespace VoxelLoadingCurtainDetail;
	FScopeLock Lock(&PaintLock);
	return PaintOverflows;
}

int64 PaintCount()
{
	using namespace VoxelLoadingCurtainDetail;
	FScopeLock Lock(&PaintLock);
	return TotalPaints;
}
} // namespace VoxelLoadingCurtain

// ---------------------------------------------------------------------------

FVoxelLoadingCurtainThread::~FVoxelLoadingCurtainThread()
{
	End();
}

FString FVoxelLoadingCurtainThread::UnavailableReason()
{
	// Asked in the engine's own terms, and in the order the engine asks them,
	// so the answer names the gate that actually fired rather than the first
	// one somebody thought of.
	if (!IsMoviePlayerEnabled())
	{
		// MoviePlayer.cpp:123: !GIsEditor && !dedicated server && !commandlet
		// && GUseThreadedRendering, minus -NoLoadingScreen. GIsEditor is the
		// one that bites here: in PIE there is no movie player, only
		// FNullGameMoviePlayer, and every call below would be an empty body.
		return GIsEditor
			? TEXT("PIE (IsMoviePlayerEnabled requires !GIsEditor, MoviePlayer.cpp:123) -- run -game for the threaded curtain")
			: TEXT("IsMoviePlayerEnabled() is false (dedicated server, commandlet, -NoLoadingScreen, or single-threaded rendering)");
	}
	if (GetMoviePlayer() == nullptr || !GetMoviePlayer()->IsInitialized())
	{
		// LaunchEngineLoop.cpp:3806-3826 initialises it during PreInit; a
		// process that got here without it has no main window to draw into.
		return TEXT("the movie player was never initialised (LaunchEngineLoop.cpp:3806)");
	}
	if (FPlatformMisc::NumberOfCores() <= 1)
	{
		// PlayMovie refuses on a single core (DefaultGameMoviePlayer.cpp:351).
		return TEXT("single core (PlayMovie refuses, DefaultGameMoviePlayer.cpp:351)");
	}
	return FString();
}

bool FVoxelLoadingCurtainThread::Begin(UWorld* InWorld, UGameViewportClient* InViewport,
                                       TSharedRef<SWidget> InCurtain, int32 InZOrder, bool bRequested)
{
	check(IsInGameThread());
	End();

	// ONE LINE IN EVERY RUN, WHICHEVER ARM IT TOOK -- the same rule the front
	// end's own "suppressed/active" pair follows. The SWITCH's own value is
	// printed once at init by UVoxelFrontEndSubsystem::Initialize
	// ("VoxelFrontEnd: loading screen thread=%d"); this line is the second
	// half, the one that says whether the process can honour it. Both are
	// needed: a run can ask for the threaded curtain and still not get it.
	const FString Why = bRequested ? UnavailableReason() : FString(TEXT("switched off (-VoxelLoadingScreenThread=0)"));
	UE_LOG(LogVoxelUI, Log, TEXT("LoadScreen: curtain thread requested=%d available=%s"),
	       bRequested ? 1 : 0, Why.IsEmpty() ? TEXT("yes") : *Why);
	if (!Why.IsEmpty())
	{
		return false;
	}
	if (InWorld == nullptr || InViewport == nullptr)
	{
		UE_LOG(LogVoxelUI, Warning, TEXT("LoadScreen: curtain thread has no world or viewport; staying on the game thread."));
		return false;
	}

	World = InWorld;
	Viewport = InViewport;
	Curtain = InCurtain;
	ZOrder = InZOrder;

	// Play-on-blocking is what registers the movie player as the proxy's server
	// (DefaultGameMoviePlayer.cpp:1076-1090). Without it BlockingStarted is a
	// call into a null pointer check and does nothing at all -- silently.
	GetMoviePlayer()->SetIsPlayOnBlockingEnabled(true);

	PreTickHandle = FWorldDelegates::OnWorldPreActorTick.AddRaw(this, &FVoxelLoadingCurtainThread::OnPreActorTick);
	PostTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FVoxelLoadingCurtainThread::OnPostActorTick);

	LastPreTickSeconds = 0.0;
	FramesSeen = 0;
	CoolDownRemaining = 0;
	ConsecutiveRefusals = 0;
	bDisabledByRefusals = false;
	BlocksArmed = 0;
	BlockRefusals = 0;
	CoveredSeconds = 0.0;
	LongestBlockMs = 0.0;
	LongestUncoveredFrameMs = 0.0;
	bActive = true;

	VoxelLoadingCurtain::ResetPaintClock();

	UE_LOG(LogVoxelUI, Log,
	       TEXT("LoadScreen: curtain thread ARMED (armMs=%.0f coolDownFrames=%d primeFrames=%d) -- ")
	       TEXT("the Slate loading thread paints the curtain across long world ticks."),
	       kArmThresholdMs, kCoolDownFrames, kPrimeFrames);
	return true;
}

void FVoxelLoadingCurtainThread::End()
{
	if (!bActive)
	{
		return;
	}
	check(IsInGameThread());

	DisarmBlock();

	if (PreTickHandle.IsValid())
	{
		FWorldDelegates::OnWorldPreActorTick.Remove(PreTickHandle);
		PreTickHandle.Reset();
	}
	if (PostTickHandle.IsValid())
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(PostTickHandle);
		PostTickHandle.Reset();
	}

	if (IGameMoviePlayer* Player = GetMoviePlayer())
	{
		// Also unregisters the proxy server and force-finishes any block.
		Player->SetIsPlayOnBlockingEnabled(false);
	}

	// THE ENGAGEMENT LINE. blocks=0 with a long load is the failure this whole
	// file can suffer silently, so the number is printed whether it is zero or
	// not, next to the longest frame that went UNCOVERED -- the two together
	// say both "did it fire" and "did it fire where it mattered".
	UE_LOG(LogVoxelUI, Log,
	       TEXT("LoadScreen: curtain thread blocks=%lld refusals=%lld coveredSec=%.2f ")
	       TEXT("longestBlockMs=%.1f longestUncoveredFrameMs=%.1f paints=%lld paintOverflow=%lld"),
	       (long long)BlocksArmed, (long long)BlockRefusals, CoveredSeconds,
	       LongestBlockMs, LongestUncoveredFrameMs,
	       (long long)VoxelLoadingCurtain::PaintCount(),
	       (long long)VoxelLoadingCurtain::PaintOverflowCount());

	bActive = false;
	World = nullptr;
	Viewport = nullptr;
	Curtain.Reset();
}

void FVoxelLoadingCurtainThread::OnPreActorTick(UWorld* TickWorld, ELevelTick TickType, float /*DeltaSeconds*/)
{
	if (!bActive || TickWorld != World || TickType != LEVELTICK_All)
	{
		return;
	}

	// Defensive: a world tick that never reached its post-tick broadcast (an
	// early return deep in LevelTick, a level transition) would leave the block
	// armed and the curtain out of the viewport. The engine's own
	// BlockingForceFinished at LaunchEngineLoop.cpp:6112 has already stopped
	// the movie by now; this puts our widget back.
	if (bBlockArmed)
	{
		DisarmBlock();
	}

	const double Now = FPlatformTime::Seconds();
	double LastFrameMs = 0.0;
	if (LastPreTickSeconds > 0.0)
	{
		LastFrameMs = (Now - LastPreTickSeconds) * 1000.0;
	}
	LastPreTickSeconds = Now;
	++FramesSeen;

	if (bDisabledByRefusals)
	{
		return;
	}

	const bool bPriming = FramesSeen <= kPrimeFrames;
	if (LastFrameMs >= kArmThresholdMs)
	{
		CoolDownRemaining = kCoolDownFrames;
	}

	if (bPriming || CoolDownRemaining > 0)
	{
		if (CoolDownRemaining > 0)
		{
			--CoolDownRemaining;
		}
		ArmBlock();
	}
	else if (LastFrameMs > LongestUncoveredFrameMs)
	{
		// The frame we did NOT cover. Read beside longestBlockMs: a large
		// number here is the hysteresis missing the first frame of a run, which
		// it does by construction and cannot avoid -- a frame's length is not
		// known until it is over.
		LongestUncoveredFrameMs = LastFrameMs;
	}
}

void FVoxelLoadingCurtainThread::OnPostActorTick(UWorld* TickWorld, ELevelTick TickType, float /*DeltaSeconds*/)
{
	if (!bActive || TickWorld != World || TickType != LEVELTICK_All)
	{
		return;
	}
	DisarmBlock();
}

void FVoxelLoadingCurtainThread::ArmBlock()
{
	if (bBlockArmed || !Curtain.IsValid() || Viewport == nullptr)
	{
		return;
	}

	// ONE WIDGET, MOVED -- never two instances. A second SVoxelLoadingScreen
	// would shuffle its own background and tip orders and the picture would
	// visibly change every time a block started. Taking the one instance out of
	// the viewport for the duration of the block also means it is only ever in
	// one widget tree, so its PersistentState and hit-test registration are
	// never written by two threads.
	Viewport->RemoveViewportWidgetContent(Curtain.ToSharedRef());

	// THE SAME UI SCALE THE VIEWPORT WOULD HAVE USED, TOLD RATHER THAN INHERITED.
	// The movie player's ViewportDPIScale defaults to 1.0 and NOTHING calls
	// SetViewportDPIScale on its own (DefaultGameMoviePlayer.cpp:121, :839),
	// while its widget renderer otherwise multiplies by the OS window's DPI
	// factor (DrawWindow's bIsDPIScaleEnabled branch). Neither of those is the
	// number SGameLayerManager applies to a viewport widget, which is
	// UUserInterfaceSettings::GetDPIScaleBasedOnSize -- so without this line
	// the curtain would change size every time a block armed, and the size it
	// changed to would depend on the Windows display scaling rather than on
	// the project's own UI scale rule. Reading the same settings object means
	// the two paths agree by construction, including under whatever
	// UIScaleRule/UIScaleCurve the project sets.
	{
		FIntPoint ViewportSize(1920, 1080);
		if (FSceneViewport* SceneViewport = Viewport->GetGameViewport())
		{
			ViewportSize = SceneViewport->GetSizeXY();
		}
		const float UIScale = GetDefault<UUserInterfaceSettings>()->GetDPIScaleBasedOnSize(ViewportSize);
		// Also turns the renderer's own window-DPI multiply off, which is the
		// documented meaning of this call ("we have our own scale", :844).
		GetMoviePlayer()->SetViewportDPIScale(UIScale);
	}

	// Rebuilt every block: WaitForMovieToFinish clears the attributes on the
	// way out (DefaultGameMoviePlayer.cpp:604), so a cached copy would be
	// consumed once and every later block would find LoadingScreenIsPrepared()
	// false and quietly do nothing.
	FLoadingScreenAttributes Attributes;
	Attributes.WidgetLoadingScreen = Curtain;
	// MUST STAY NEGATIVE. A non-negative MinimumLoadingScreenDisplayTime makes
	// WaitForMovieToFinish spin the GAME THREAD until it elapses
	// (DefaultGameMoviePlayer.cpp:449, :508) -- once per armed frame. The
	// project's minimum hold is LoadMinHoldSeconds in the front end's own
	// state machine and has nothing to do with this.
	Attributes.MinimumLoadingScreenDisplayTime = -1.0f;
	Attributes.bAutoCompleteWhenLoadingCompletes = false;
	Attributes.bMoviesAreSkippable = false;
	// False, or WaitForMovieToFinish waits for a StopMovie that never comes and
	// the block never ends.
	Attributes.bWaitForManualStop = false;
	Attributes.bAllowInEarlyStartup = false;
	// False deliberately: see fact 3 in the header. bAllowEngineTick would put
	// the world tick and the Slate tick back on one thread, which is the exact
	// arrangement this file exists to get out of.
	Attributes.bAllowEngineTick = false;
	// Widget-only. No MoviePaths, no registered streamer: nothing plays video.
	GetMoviePlayer()->SetupLoadingScreen(Attributes);

	FMoviePlayerProxy::BlockingStarted();

	// PROOF, NOT HOPE. IsMovieCurrentlyPlaying() is literally
	// `SyncMechanism != NULL` (DefaultGameMoviePlayer.cpp:638), i.e. "the Slate
	// loading thread exists and is running". If it does not, nothing above did
	// anything and the curtain must go straight back where it came from.
	if (!GetMoviePlayer()->IsMovieCurrentlyPlaying())
	{
		Viewport->AddViewportWidgetContent(Curtain.ToSharedRef(), ZOrder);
		++BlockRefusals;
		++ConsecutiveRefusals;
		if (ConsecutiveRefusals >= kMaxRefusals)
		{
			bDisabledByRefusals = true;
			UE_LOG(LogVoxelUI, Warning,
			       TEXT("LoadScreen: curtain thread refused to start %d times in a row; disabling it for this load. ")
			       TEXT("The curtain stays on the game thread and will freeze on long frames."),
			       ConsecutiveRefusals);
		}
		return;
	}

	ConsecutiveRefusals = 0;
	bBlockArmed = true;
	BlockStartedSeconds = FPlatformTime::Seconds();
	++BlocksArmed;
}

void FVoxelLoadingCurtainThread::DisarmBlock()
{
	if (!bBlockArmed)
	{
		return;
	}
	bBlockArmed = false;

	// Joins the Slate loading thread and hands the window back to the game
	// viewport (DefaultGameMoviePlayer.cpp:683 -> WaitForMovieToFinish).
	FMoviePlayerProxy::BlockingFinished();

	const double BlockMs = (FPlatformTime::Seconds() - BlockStartedSeconds) * 1000.0;
	CoveredSeconds += BlockMs / 1000.0;
	LongestBlockMs = FMath::Max(LongestBlockMs, BlockMs);

	if (Curtain.IsValid() && Viewport != nullptr)
	{
		Viewport->AddViewportWidgetContent(Curtain.ToSharedRef(), ZOrder);
	}
}
