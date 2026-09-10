#include "VoxelMenuScalability.h"

#include "VoxelEarth.h"           // LogVoxelEarth
#include "VoxelFrontEndPolicy.h"  // IsEnabledThisRun -- the leak guard
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace VoxelMenuScalabilityDetail
{
// NAMED, not anonymous, on purpose: tools/lint-unity-collisions.py exists
// because two anonymous namespaces in one unity blob ARE one scope.

// ============================================================================
// THE LIST, AND WHY EACH LINE IS ON IT
// ============================================================================
//
// The rule for membership: the cvar must make the MENU FRAME cheaper without
// being able to change what the menu LOOKS like. The menu is a full-screen 2D
// Slate image composited after the scene render over a scene the player cannot
// see, so anything that only affects the 3D scene qualifies by construction.
// Anything that could touch the Slate composite, the backbuffer format, or the
// window itself does not, and is not here.
//
// The second rule: it has to plausibly be a real cost on THIS menu. That
// disqualified more candidates than it admitted -- see the EXCLUDED block
// below, which is the more useful half of this comment.
struct FDrop
{
	const TCHAR* Name;
	const TCHAR* MenuValue;
};

const FDrop kDrops[] = {
	// 1. THE BIG ONE. Everything before the temporal upscale -- base pass,
	//    GBuffer, deferred lighting, SSR (r.ReflectionMethod=2), single-layer
	//    water on the ocean plane, translucency, AO -- is per-PIXEL at the
	//    scene resolution, and the menu world is NOT empty: the game mode
	//    spawns AVoxelOceanActor and AVoxelClipmapActor at BeginPlay
	//    (VoxelEarthGameMode.cpp:519/545), both of which draw. 65 -> 25 is
	//    6.8x fewer shaded pixels.
	//
	//    25 IS THE ENGINE'S FLOOR, NOT A ROUND NUMBER:
	//    ISceneViewFamilyScreenPercentage::kMinTSRResolutionFraction = 0.25f
	//    (SceneView.h:2280). Ask for less and TSR clamps, which would make the
	//    "after" value in the log below a lie.
	{TEXT("r.ScreenPercentage"), TEXT("25")},

	// 2. TSR IS THE PASS ENTRY 1 DOES NOT SHRINK. It runs at OUTPUT
	//    resolution, so the menu pays for it in full whatever the scene
	//    resolution is, and its history buffers are sized as a multiple of
	//    that output. 200 -> 100 is 4x fewer history texels through the
	//    reproject/accumulate/output chain.
	//
	//    THIS IS LIVE ON THIS BOX AND IS NOT AN INI VALUE. The cvar's own
	//    default is 100 (TemporalSuperResolution.cpp:54); it reads 200 because
	//    Saved/Config/WindowsEditor/GameUserSettings.ini carries
	//    sg.AntiAliasingQuality=3, and [AntiAliasingQuality@3] in the engine's
	//    BaseScalability.ini sets it. A machine with a different saved preset
	//    will find it already at 100, this entry will change nothing, and the
	//    APPLIED line will say so rather than claiming a saving it did not make.
	{TEXT("r.TSR.History.ScreenPercentage"), TEXT("100")},

	// 3. BLOOM RUNS AFTER THE UPSCALE, at output resolution, so entry 1 does
	//    not shrink it either: a downsample/upsample pyramid over a scene
	//    nobody sees. It is ON here (sg.PostProcessQuality=0 leaves
	//    r.BloomQuality=4 with r.Bloom.ScreenPercentage=25 -- that group zeroes
	//    motion blur, DOF, lens flare and SSAO but NOT bloom), and on a default
	//    profile it is quality 5 at full bloom resolution, i.e. more.
	{TEXT("r.BloomQuality"), TEXT("0")},
};

// int32, not the raw UE_ARRAY_COUNT: the macro's type is size_t, and handing a
// 64-bit value to a %d in a UE_LOG varargs list is undefined formatting.
constexpr int32 kNumDrops = static_cast<int32>(UE_ARRAY_COUNT(kDrops));

// ============================================================================
// EXCLUDED, AND WHY -- the candidates this list is accused of missing
// ============================================================================
//
// * r.Lumen.* / r.LumenScene.* -- LUMEN DOES NOT RUN IN THIS PROJECT. The
//   plan that commissioned this file cites "DefaultEngine.ini:717-730 arms
//   Lumen fully (r.Lumen.DiffuseIndirect.Allow=1, r.Lumen.FinalGatherMethod=1,
//   r.LumenScene.SurfaceCache.AtlasSize=4096)". Those three lines exist, but
//   in the ENGINE's Config/BaseScalability.ini under [GlobalIlluminationQuality@3]
//   (which this box selects via sg.GlobalIlluminationQuality=3), not in this
//   project's ini -- ue-project/Config/DefaultEngine.ini contains the string
//   "Lumen" only inside comments, and a repo-wide grep for those cvar names
//   finds nothing outside the engine. They are armed and inert: that group
//   does NOT set r.DynamicGlobalIlluminationMethod, whose default is 0 = None
//   (IndirectLightRendering.cpp:93) and which nothing in this project or in
//   BaseScalability/BaseEngine/BaseDeviceProfiles ever sets. With the method
//   at None, ShouldRenderLumen* is false and no Lumen pass is built, so
//   turning r.Lumen.DiffuseIndirect.Allow off here would change zero frames
//   while adding a line to the "changed" list that implies a saving. That is
//   exactly the fake-engagement failure this file's log format exists to
//   prevent, so it is left out.
//
// * r.VolumetricFog, r.SkyAtmosphere.*, r.SkyLight.RealTimeReflectionCapture,
//   r.ShadowQuality / r.Shadow.CSM.MaxCascades -- NONE OF THAT EXISTS DURING
//   THE MENU. UVoxelSkySubsystem::OnWorldBeginPlay DEFERS the entire sky rig
//   while VoxelFrontEnd::IsWorldHeldForMenu (VoxelSkySubsystem.cpp:2783), so
//   there is no SkyAtmosphere component, no ExponentialHeightFog, no SkyLight
//   and no ADirectionalLight in the world until NEW GAME. No fog froxel grid
//   is built, no atmosphere LUT is integrated and no shadow-depth pass is
//   created, because there is no light to create one for. Setting any of them
//   on the menu is a guaranteed no-op.
//
//   They become real during LOADING, which this drop also covers -- and that
//   is deliberately left on the table. Shadows in particular are where this
//   project has bled most (see the two long blocks in DefaultEngine.ini about
//   a stale sg.ShadowQuality=0 preset costing seven false root causes), and a
//   loading-phase-only change cannot be gated by the menu PNG hash that gates
//   everything here. It needs its own image gate at the reveal.
//
// * r.MotionBlurQuality / r.DepthOfFieldQuality / r.AmbientOcclusionLevels --
//   already 0 on this box ([PostProcessQuality@0], sg.PostProcessQuality=0),
//   and near-free anyway on a static camera with nothing in front of it.
//   Adding them would pad the candidate count with entries that can never
//   move.
//
// * r.ReflectionMethod / r.SSR.Quality -- SSR is real here (the ocean plane is
//   in the menu world and r.ReflectionMethod=2 is an owner-settled decision),
//   but it runs at SCENE resolution, so entry 1 already cuts it 6.8x. A second
//   knob for the same pass buys little and touches a settled value.
//
// * r.AntiAliasingMethod 0 (skip TSR entirely) -- the largest remaining
//   output-resolution item, and the obvious next lever. NOT taken yet because
//   switching the AA method mid-session changes shader permutations, and a
//   permutation that is not in the pipeline cache compiles ON DEMAND -- a
//   stall, at the exact moment (the reveal) that Phase 4 is trying to make
//   smooth. It wants its own measurement, not a free ride on this one.
//
// * t.MaxFPS -- capping the menu to 60 would halve the WORK the machine does
//   at the title screen, which is arguably the owner's actual complaint. It is
//   left out because it does not make a frame cheaper, it makes fewer frames:
//   it would RAISE the seg=MENU frame time the box owner is about to A/B this
//   change with, and poison the instrument. It is a separate, owner-facing
//   decision.
//
// * ShowFlags.Rendering = false on the game viewport -- would remove the whole
//   ~11 ms rather than shrinking it, and is the real end state for a menu with
//   an opaque curtain. It is not a cvar, it cannot be A/B'd from a command
//   line on one binary, and it risks a black frame if any part of the front
//   end is ever less than opaque. Named here so the next reader does not have
//   to rediscover it.

// The saved half. One entry per cvar Apply() actually changed, holding the
// string it read BEFORE the change -- strings, not GetInt/GetFloat, because
// this table mixes float and int cvars and a restore has to be EXACT. Both
// strings come out of the same cvar's own formatter, so "changed?" is a
// reliable string compare and a float never comes back as 65 when it was
// 65.000000.
struct FSavedValue
{
	const TCHAR* Name;
	FString Before;
	FString After;
};

// PROCESS-WIDE, NOT PER-SUBSYSTEM, because console variables are. One front
// end per process is the shipping shape; if two ever existed (PIE plus a game
// preview world), the first Restore releases the drop for both, which is the
// safe direction -- full quality is never wrong to render, only expensive.
//
// A FUNCTION-LOCAL STATIC, not two namespace-scope globals, for the reason
// VoxelFrontEndPolicy.cpp records beside its own: a TArray<FString> at
// namespace scope has a non-trivial constructor and destructor and drags this
// file into static initialisation and teardown order, and the first caller
// here runs from a subsystem's OnWorldBeginPlay.
struct FState
{
	TArray<FSavedValue> Saved;
	bool bApplied = false;
};

FState& State()
{
	static FState Instance;
	return Instance;
}

VoxelMenuScalability::EMode ComputeMode()
{
	int32 Value = 1;
	FParse::Value(FCommandLine::Get(), TEXT("VoxelMenuScalability="), Value);
	switch (FMath::Clamp(Value, 0, 2))
	{
	case 0:
		return VoxelMenuScalability::EMode::Off;
	case 2:
		return VoxelMenuScalability::EMode::DryRun;
	default:
		return VoxelMenuScalability::EMode::On;
	}
}
} // namespace VoxelMenuScalabilityDetail

namespace VoxelMenuScalability
{
EMode ModeThisRun()
{
	static const EMode Resolved = VoxelMenuScalabilityDetail::ComputeMode();
	return Resolved;
}

void Apply()
{
	using namespace VoxelMenuScalabilityDetail;

	if (State().bApplied)
	{
		// Idempotent by design: EXIT TO MENU reopens the map, so EnterMenu can
		// run twice in one process. A second save would record the DROPPED
		// values as the ones to restore, and the drop would then leak into the
		// next session's gameplay -- silently, and only on the second visit.
		return;
	}

	// THE LEAK GUARD, and it is the whole reason no capture or perf leg can
	// measure a dropped frame. IsEnabledThisRun() is already false for every
	// unattended run, every self-driving switch and every run that cannot
	// render (VoxelFrontEndPolicy.cpp rules 3-5). This is an Error rather than
	// a quiet return because reaching it means a caller outside the front end
	// started calling this, which is the beginning of exactly the leak the
	// design forbids.
	if (!VoxelFrontEnd::IsEnabledThisRun())
	{
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("MenuScalability: REFUSED -- the front end is not enabled this run (%s). "
		            "Nothing was changed. A menu-only scalability drop must never touch a run "
		            "with no menu."),
		       VoxelFrontEnd::WhyThisAnswer());
		return;
	}

	const EMode Mode = ModeThisRun();
	if (Mode == EMode::Off)
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("MenuScalability: OFF (-VoxelMenuScalability=0). The menu renders at the "
		            "shipped configuration; this is the control arm."));
		return;
	}

	const bool bDryRun = (Mode == EMode::DryRun);

	FString Changed;
	FString Unchanged;
	int32 NumChanged = 0;
	int32 NumUnchanged = 0;
	int32 NumMissing = 0;

	for (const FDrop& Drop : kDrops)
	{
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Drop.Name);
		if (Var == nullptr)
		{
			// A cvar can be absent on a build that compiled its feature out, or
			// renamed by an engine upgrade. Named, counted, and skipped -- never
			// fatal, and never silent: a shrinking list is how a drop stops
			// working without anybody noticing.
			++NumMissing;
			UE_LOG(LogVoxelEarth, Warning,
			       TEXT("MenuScalability: cvar %s not found on this build; skipped."), Drop.Name);
			continue;
		}

		const FString Before = Var->GetString();

		if (bDryRun)
		{
			// The dry-run arm reports what it WOULD do by comparing strings,
			// which cannot tell "already at the target" from "would be refused
			// by priority". It does not have to: the point of mode 2 is to
			// prove the LIST, and mode 1's own APPLIED line reports what really
			// moved.
			if (Before.Equals(Drop.MenuValue))
			{
				++NumUnchanged;
				Unchanged += FString::Printf(TEXT(" | %s already %s"), Drop.Name, *Before);
			}
			else
			{
				++NumChanged;
				Changed += FString::Printf(TEXT(" | %s %s -> %s"), Drop.Name, *Before, Drop.MenuValue);
			}
			continue;
		}

		// ECVF_SetByCode (priority 8) is REQUIRED, not stylistic:
		// r.ScreenPercentage is pinned at SetBySystemSettingsIni (4) by
		// DefaultEngine.ini's [SystemSettings] and r.TSR.History.ScreenPercentage
		// at SetByScalability (1); anything lower is refused silently by
		// FConsoleVariableBase::CanChange and this whole arm would read as a
		// null result. Same priority UVoxelFrontEndSubsystem::
		// CapStreamingForTheatre uses, for the same reason.
		Var->Set(Drop.MenuValue, ECVF_SetByCode);
		const FString After = Var->GetString();

		if (Before.Equals(After))
		{
			// Either it was already at the menu value, or the set was refused.
			// Either way nothing moved and it must not be counted as engagement.
			++NumUnchanged;
			Unchanged += FString::Printf(TEXT(" | %s stayed %s"), Drop.Name, *After);
			continue;
		}

		++NumChanged;
		Changed += FString::Printf(TEXT(" | %s %s -> %s"), Drop.Name, *Before, *After);
		State().Saved.Add(FSavedValue{Drop.Name, Before, After});
	}

	if (bDryRun)
	{
		UE_LOG(LogVoxelEarth, Log,
		       TEXT("MenuScalability: DRY RUN (-VoxelMenuScalability=2), nothing was set. "
		            "wouldChange=%d unchanged=%d missing=%d of %d candidates%s%s"),
		       NumChanged, NumUnchanged, NumMissing, kNumDrops, *Changed, *Unchanged);
		return;
	}

	if (NumChanged == 0)
	{
		// AN ARM THAT SET NOTHING IS A FAILURE, NOT A QUIET SUCCESS. This is
		// the line a gate greps for the absence of; without it the run would
		// look identical to a working one and the A/B would report "no
		// difference" as though the change had been measured.
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("MenuScalability: APPLIED NOTHING -- changed=0 unchanged=%d missing=%d of %d "
		            "candidates. The menu is rendering at full cost and this arm did NOT engage.%s"),
		       NumUnchanged, NumMissing, kNumDrops, *Unchanged);
		return;
	}

	State().bApplied = true;
	UE_LOG(LogVoxelEarth, Log,
	       TEXT("MenuScalability: APPLIED changed=%d unchanged=%d missing=%d of %d candidates%s%s"),
	       NumChanged, NumUnchanged, NumMissing, kNumDrops, *Changed, *Unchanged);
}

void Restore()
{
	using namespace VoxelMenuScalabilityDetail;

	if (!State().bApplied)
	{
		// Silent, because this has two callers by design -- the reveal (the
		// normal path) and TeardownMenu (the backstop for a quit or a world
		// teardown that never reaches one) -- exactly like
		// RestoreStreamingBudget. Nothing applied means nothing to say.
		return;
	}
	State().bApplied = false;

	FString Line;
	int32 NumRestored = 0;
	int32 NumMismatched = 0;

	for (const FSavedValue& Saved : State().Saved)
	{
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Saved.Name);
		if (Var == nullptr)
		{
			// It was found at Apply() and is gone now: impossible short of an
			// engine teardown mid-session. Say so rather than skipping.
			++NumMismatched;
			Line += FString::Printf(TEXT(" | %s VANISHED (found at apply, absent now)"), Saved.Name);
			continue;
		}

		// UNSET FIRST, SET SECOND, AND THE ORDER IS THE POINT. Restoring by
		// Set(Before, SetByCode) alone would leave the cvar PINNED at priority
		// 8 for the rest of the process, which is a real leak into gameplay:
		// the player's settings panel writes cvars at ECVF_SetByGameSetting (2)
		// and the scalability groups at SetByScalability (1), and both would
		// then be refused for anything this file has touched. Unset removes
		// this file's SetByCode entry and lets the next-highest priority value
		// -- the one Apply() read -- become current again.
		//
		// Unset is a no-op when the engine is built without cvar history
		// (ConsoleManager.cpp:1191 returns immediately on a null
		// PriorityHistory), which is why the value is CHECKED afterwards
		// rather than assumed, and the pin is used as the fallback.
		Var->Unset(ECVF_SetByCode);
		const TCHAR* Route = TEXT("released");
		if (!Var->GetString().Equals(Saved.Before))
		{
			Var->Set(*Saved.Before, ECVF_SetByCode);
			Route = TEXT("pinned");
		}

		const FString Now = Var->GetString();
		if (!Now.Equals(Saved.Before))
		{
			++NumMismatched;
			Line += FString::Printf(TEXT(" | %s DID NOT RESTORE: wanted %s, reads %s"),
			                        Saved.Name, *Saved.Before, *Now);
			continue;
		}
		++NumRestored;
		Line += FString::Printf(TEXT(" | %s %s -> %s (%s)"), Saved.Name, *Saved.After, *Now, Route);
	}

	State().Saved.Reset();

	if (NumMismatched > 0)
	{
		// A cvar that did not come back is a live gameplay defect -- the player
		// would keep playing at the menu's quality -- so it is an Error, and it
		// names which one.
		UE_LOG(LogVoxelEarth, Error,
		       TEXT("MenuScalability: RESTORED restored=%d mismatched=%d%s"),
		       NumRestored, NumMismatched, *Line);
		return;
	}

	UE_LOG(LogVoxelEarth, Log,
	       TEXT("MenuScalability: RESTORED restored=%d mismatched=0%s"), NumRestored, *Line);
}

bool IsApplied()
{
	return VoxelMenuScalabilityDetail::State().bApplied;
}
} // namespace VoxelMenuScalability
