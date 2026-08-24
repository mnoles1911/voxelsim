#include "VoxelFrontEndPolicy.h"

#include "VoxelEarth.h" // LogVoxelEarth
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace VoxelFrontEndPolicyDetail
{
// NAMED, not anonymous, on purpose: tools/lint-unity-collisions.py exists
// because two anonymous namespaces in one unity blob ARE one scope. Everything
// file-local in this module's newer files is named for that reason.

// --- Rule 5's classifier ----------------------------------------------------
//
// A "self-driving" switch is one whose run drives ITSELF: it spawns, poses,
// waits, captures and usually quits, with no human present. Those runs must
// never stop at a menu.
//
// THIS IS A RULE, NOT A LIST, and that is a deliberate reversal of the obvious
// design. A hand-maintained list of ~200 switch names would be wrong the first
// week: somebody adds -VoxelFooTest, forgets the list, and their capture hangs
// for five minutes until the watchdog kills it. The rule below classifies by
// the naming convention this codebase already follows without being told to --
// every fixture is a *Test, every capture is a *Shot or *After, every
// measurement is a *Run/*Check/*Verify/*Survey/*Probe.
//
// THE ERROR BUDGET IS ASYMMETRIC, which is what makes a fuzzy rule the right
// tool. A false POSITIVE (suppressing the menu on a run that did not need it)
// costs a developer one extra `-VoxelFrontEnd=1`. A false NEGATIVE (a menu on
// a self-driving run) costs a hung capture and, worse, an archive diff that
// silently compares against nothing. So the rule errs toward suppression, and
// anything it cannot classify is forced into an explicit table by
// tools/lint-frontend-switch-coverage.py rather than being guessed at.
const TCHAR* const kSelfDrivingSubstrings[] = {
	TEXT("Test"),       // every fixture: VoxelGICaveTest, VoxelWaterWakeTest, ...
	TEXT("Shot"),       // every capture: VoxelOverlayShot, VoxelVistaShot, VoxelCavernShot
	TEXT("After"),      // every timed action: VoxelScreenshotAfter, VoxelSaveWorldAfter
	TEXT("Screenshot"), // VoxelScreenshotBurst and friends
	TEXT("Check"),      // VoxelGIColorCheck, VoxelWaterLoadCheck, VoxelWindingCheck
	TEXT("Verify"),     // VoxelVerify, VoxelCoarseGridVerify, VoxelL0GridVerify
	TEXT("Probe"),      // VoxelL0GridCacheProbe
	TEXT("Survey"),     // VoxelOceanSurvey
	TEXT("Ladder"),     // VoxelSkyLadder
	TEXT("Flight"),     // VoxelPerfFlight
	TEXT("Converge"),   // VoxelGIConverge
	TEXT("Histogram"),  // VoxelMatHistogram
	TEXT("Swarm"),      // VoxelSwarmTest/VoxelClientSwarm agent fixtures
	TEXT("Dump"),       // VoxelDumpDigestAfter, VoxelDumpAgentsAfter
};

// Switches whose names the convention above does not reach, but which still
// drive the run. Each needs a reason, and the lint fails on an entry that has
// none -- an unexplained entry here is how the table starts rotting.
struct FNamedException
{
	const TCHAR* Name;
	const TCHAR* Reason;
};
const FNamedException kSelfDrivingExtras[] = {
	{TEXT("VoxelPerfRun"), TEXT("perf harness: flies a fixed path and exits on its own watchdog")},
	{TEXT("VoxelPerfStaticAt"), TEXT("perf harness: poses the camera and measures, no input")},
	{TEXT("VoxelExecCmds"), TEXT("runs a console script at a timed offset; -VoxelExecAfter is its delay")},
	{TEXT("VoxelHudShotOnly"), TEXT("capture mode: 'Shot' is not the suffix here, it is mid-name")},
	{TEXT("VoxelCavern"), TEXT("cavern vista fixture; the bare form is the on-switch for VoxelCavernAt")},
	{TEXT("VoxelCavernAt"), TEXT("cavern vista fixture: explicit pose for the above")},
	{TEXT("VoxelOceanDig"), TEXT("ocean dig fixture: carves a trench and reports, unattended")},
	{TEXT("VoxelOceanDigAt"), TEXT("ocean dig fixture: explicit pose for the above")},
	{TEXT("VoxelWaterMarker"), TEXT("water marker fixture: places markers and captures")},
	{TEXT("VoxelWaterMarkerOnly"), TEXT("water marker fixture, marker-only arm")},
	{TEXT("VoxelWaterMarkerOcean"), TEXT("water marker fixture, ocean arm")},
	{TEXT("VoxelUndergroundView"), TEXT("underground capture fixture: poses the camera below ground")},
	{TEXT("VoxelMeasureEmpty"), TEXT("measurement run: reports empty-chunk stats and exits")},
	{TEXT("VoxelDetailResolve"), TEXT("detail-asset resolve fixture: dumps a placement report")},
	{TEXT("VoxelGpuPool"), TEXT("GPU geometry-pool soak fixture")},
	{TEXT("VoxelGIRelight"), TEXT("GI relight fixture: forces a relight then reports")},
};

// RULE 5 EXEMPTIONS: names the substring rule catches BY ACCIDENT.
//
// The substring rule reads intent off a naming convention, and mostly that
// works. But it matches anywhere in the name, not just the suffix, so a switch
// can inherit a fixture's vocabulary without being a fixture -- and the run
// then loses its menu for no reason the developer can see.
//
// THESE ARE NEUTRAL, NOT FORCED-ON. That distinction is the whole point, and it
// is why this is a separate table from kFrontEndCaptureSwitches above. Putting
// a name here says only "rule 5 must not read this as self-driving"; the run's
// OTHER switches still decide. If -VoxelReadyProbeLog were forced ON instead,
// then a genuine capture run that also passed it -- which is exactly when you
// want the probe's log -- would stop at a menu and hang, and a hung capture is
// the expensive half of this file's error budget.
//
// Each entry needs a reason, for the same reason kSelfDrivingExtras does.
//
// THIS TABLE IS THE RUNTIME HALF OF A DECISION THAT WAS ALREADY MADE.
// tools/frontend-switch-classification.txt records all thirteen of these as
// `= ACCIDENTAL`, with the substring that caught each one -- but nothing reads
// that file at runtime; it feeds the lint only. So recording the decision made
// the lint green and left the menu still disappearing. The names below are
// taken from that file rather than re-judged here, and
// VoxelEarth.FrontEnd.SwitchPolicy fails if the two ever drift apart.
const FNamedException kRule5Exemptions[] = {
	{TEXT("VoxelBucketedExitScanVerify"), TEXT("'Verify': correctness arm for the eviction index's bucketed exit scan; verifies DURING a normal run")},
	{TEXT("VoxelGpuMeshInFlight"), TEXT("'Flight', as a SUFFIX: a job in-flight cap, pure tuning -- the case that proves a suffix rename cannot fix this class")},
	{TEXT("VoxelGpuWorklistVerifyCT"), TEXT("'Verify': byte gate for the worklist ClassifyTotals stage; adds a compare pass and ends nothing")},
	{TEXT("VoxelGpuWorklistVerifyClaim"), TEXT("'Verify': byte gate for the worklist Claim stage")},
	{TEXT("VoxelGpuWorklistVerifyCols"), TEXT("'Verify': byte gate for the worklist Column stage")},
	{TEXT("VoxelGpuWorklistVerifyPack"), TEXT("'Verify': byte gate for the worklist Pack stage")},
	{TEXT("VoxelGpuWorklistVerifyStamp"), TEXT("'Verify': byte gate for the worklist AssetStamp stage")},
	{TEXT("VoxelGpuWorklistVerifyVox"), TEXT("'Verify': byte gate for the worklist Voxelize stage")},
	{TEXT("VoxelJobsInFlightPerCore"), TEXT("'Flight': the per-core job cap knob, pure tuning")},
	{TEXT("VoxelReadyProbeLog"), TEXT("'Probe': a log flag for the loading gate, and the gate only runs when the front end does -- see below")},
	{TEXT("VoxelVerifyBuriedSkip"), TEXT("'Verify': correctness arm for the buried-chunk skip; verifies during a normal run")},
	{TEXT("VoxelVerifySkyBand"), TEXT("'Verify': correctness arm for the sky band; verifies during a normal run")},
	{TEXT("VoxelVerifySolidSkip"), TEXT("'Verify': correctness arm for the solid-skip admission gate; verifies during a normal run")},
};

// WHY THE ONE ABOVE IS NOT MERELY COSMETIC. FVoxelWorldReadyProbe is the
// loading screen's gate; it exists only while the front end is up.
// -VoxelReadyProbeLog is the flag you add to find out why that gate is not
// opening. Caught by rule 5, it suppressed the front end, which removed the
// loading screen, which removed the probe -- so the diagnostic switch
// guaranteed its own subject never ran and printed nothing at all. It is the
// same self-defeating shape kFrontEndCaptureSwitches was added for; that list
// caught -VoxelMenuShot and -VoxelHourglassShot and missed this one only
// because the name does not contain "Shot".
//
// Found by desk-check, not by the lint: tools/lint-frontend-switch-coverage.py
// SKIPS any name the substring rule already matches, on the reasoning that the
// rule handles it -- so an accidental match is invisible to exactly the tool
// that exists to catch unclassified switches. Roughly a dozen more accidental
// matches exist outside this module (job-count caps matching "Flight",
// byte-verify toggles matching "Verify"); they belong to their owners and are
// being classified on the lint branch rather than guessed at here.

// THE FRONT END'S OWN CAPTURE SWITCHES, which the rule above would otherwise
// suppress the front end for -- -VoxelMenuShot and -VoxelHourglassShot both
// contain "Shot", and both exist precisely to photograph a menu. Passing
// -VoxelFrontEnd=1 alongside them works (rule 2 is checked first), and the
// documented command lines do, but a switch whose whole purpose is defeated
// when used on its own is a trap rather than a flag. So these force the front
// end ON, before rule 5 gets a chance to turn it off.
const TCHAR* const kFrontEndCaptureSwitches[] = {
	TEXT("VoxelMenuShot"),
	TEXT("VoxelMenuPanel"),
	TEXT("VoxelLoadingShot"),
	TEXT("VoxelLoadingShotAt"),
	TEXT("VoxelHourglassShot"),
	TEXT("VoxelMenuAutoStart"),
	TEXT("VoxelUINoAssets"),
};

bool NameIsFrontEndCapture(const FString& SwitchName)
{
	for (const TCHAR* Entry : kFrontEndCaptureSwitches)
	{
		if (SwitchName.Equals(Entry, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

bool NameIsSelfDriving(const FString& SwitchName)
{
	// Exemptions first: an accidental substring match must lose to the
	// recorded decision, not race it.
	for (const FNamedException& Entry : kRule5Exemptions)
	{
		if (SwitchName.Equals(Entry.Name, ESearchCase::CaseSensitive))
		{
			return false;
		}
	}
	for (const TCHAR* Needle : kSelfDrivingSubstrings)
	{
		if (SwitchName.Contains(Needle, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	for (const FNamedException& Entry : kSelfDrivingExtras)
	{
		if (SwitchName.Equals(Entry.Name, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

// Resolved once; see the header on why the answer is cached.
//
// A FUNCTION-LOCAL STATIC, not three namespace-scope globals. Two reasons, and
// the second is the one that bites: an FString at namespace scope has a
// non-trivial constructor and destructor, which drags this file into static
// initialisation order (FCommandLine is not necessarily up yet); and a
// function-local static gets thread-safe first-call initialisation from the
// language, which matters because the first caller here is a GameMode
// constructor and the second is a subsystem Initialize on a different frame.
struct FResolvedPolicy
{
	bool bEnabled = false;
	FString Reason;
};

FResolvedPolicy ComputePolicy()
{
	FResolvedPolicy Result;
	const TCHAR* CmdLine = FCommandLine::Get();

	// Rule 1: explicit off. -VoxelNoMenu is the short form; -VoxelFrontEnd=0
	// is the long one, so a script can pass 0/1 from a variable.
	int32 ExplicitValue = 0;
	const bool bHasExplicit = FParse::Value(CmdLine, TEXT("VoxelFrontEnd="), ExplicitValue);
	if (FParse::Param(CmdLine, TEXT("VoxelNoMenu")) || (bHasExplicit && ExplicitValue == 0))
	{
		Result.bEnabled = false;
		Result.Reason = TEXT("disabled explicitly (-VoxelNoMenu / -VoxelFrontEnd=0)");
		return Result;
	}

	// Rule 2: explicit ON, overriding rules 3-5. This is what the new capture
	// switches pass, and it is the ONLY way to photograph the menu at all --
	// every capture run is unattended, so without this override rule 4 would
	// suppress the very thing being captured.
	if (bHasExplicit && ExplicitValue != 0)
	{
		Result.bEnabled = true;
		Result.Reason = TEXT("enabled explicitly (-VoxelFrontEnd=1)");
		return Result;
	}

	// Rule 3: no viewport, no menu. A dedicated server never renders (see
	// UVoxelWorldSubsystem::OnWorldBeginPlay's NM_DedicatedServer early
	// return, which leaves ChunkOwner null for the same reason), and neither
	// does a -nullrhi commandlet run.
	if (!FApp::CanEverRender())
	{
		Result.bEnabled = false;
		Result.Reason = TEXT("this run cannot render (dedicated server / nullrhi)");
		return Result;
	}

	// Rules 3.5 and 5 both need the command line's -Voxel* names, so collect
	// them once. Tokenising rather than testing ~190 known names keeps this
	// O(argv) and, more to the point, makes it work for switches that did not
	// exist when this was written.
	TArray<FString> VoxelSwitchNames;
	{
		FString Token;
		const TCHAR* Cursor = CmdLine;
		while (FParse::Token(Cursor, Token, /*bUseEscape=*/false))
		{
			if (!Token.StartsWith(TEXT("-"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			FString Name = Token.RightChop(1);
			int32 EqualsIndex = INDEX_NONE;
			if (Name.FindChar(TEXT('='), EqualsIndex))
			{
				Name.LeftInline(EqualsIndex);
			}
			if (Name.StartsWith(TEXT("Voxel"), ESearchCase::CaseSensitive))
			{
				VoxelSwitchNames.Add(MoveTemp(Name));
			}
		}
	}

	// Rule 3.5: the front end's OWN capture switches force it on, overriding
	// rules 4 and 5.
	//
	// BOTH of those would otherwise defeat it. Rule 5 suppresses anything
	// containing "Shot", which -VoxelMenuShot and -VoxelHourglassShot both do;
	// and rule 4 suppresses every unattended run, which every capture run is.
	// A switch that turns off the thing it exists to photograph is a trap
	// rather than a flag, so this sits above both of them -- and below rule 3,
	// because a run that cannot render cannot capture either.
	for (const FString& Name : VoxelSwitchNames)
	{
		if (NameIsFrontEndCapture(Name))
		{
			Result.bEnabled = true;
			Result.Reason = FString::Printf(TEXT("front-end capture switch -%s"), *Name);
			return Result;
		}
	}

	// Rule 4: unattended. tools/voxel-capture.ps1 and every leg/burst script
	// built on it pass -unattended, so this single test covers the majority of
	// the verification fleet in one line.
	if (FApp::IsUnattended())
	{
		Result.bEnabled = false;
		Result.Reason = TEXT("unattended run (-unattended)");
		return Result;
	}

	// Rule 5: a self-driving switch is present. THIS IS THE RULE THAT EARNS
	// ITS KEEP, and the reason rule 4 alone is not enough:
	// tools/fluid-spike-measure.ps1 launches `-game -windowed` WITHOUT
	// -unattended and then drives itself with a fixture switch. Under rules
	// 1-4 only, that run would stop at a menu and measure nothing.
	for (const FString& Name : VoxelSwitchNames)
	{
		if (NameIsSelfDriving(Name))
		{
			Result.bEnabled = false;
			Result.Reason = FString::Printf(TEXT("self-driving switch -%s"), *Name);
			return Result;
		}
	}

	// Rule 6: an ordinary launch. This is the shipping path.
	Result.bEnabled = true;
	Result.Reason = TEXT("ordinary interactive launch");
	return Result;
}

const FResolvedPolicy& Policy()
{
	static const FResolvedPolicy Resolved = ComputePolicy();
	return Resolved;
}
} // namespace VoxelFrontEndPolicyDetail

namespace VoxelFrontEnd
{
bool IsEnabledThisRun()
{
	return VoxelFrontEndPolicyDetail::Policy().bEnabled;
}

const TCHAR* WhyThisAnswer()
{
	return *VoxelFrontEndPolicyDetail::Policy().Reason;
}

bool IsSelfDrivingSwitchName(const FString& SwitchName)
{
	return VoxelFrontEndPolicyDetail::NameIsSelfDriving(SwitchName);
}
} // namespace VoxelFrontEnd
