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

bool NameIsSelfDriving(const FString& SwitchName)
{
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
	//
	// Tokenising the command line rather than testing ~200 known names keeps
	// this O(argv) and, more to the point, makes it work for switches that did
	// not exist when this was written.
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
			if (!Name.StartsWith(TEXT("Voxel"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (NameIsSelfDriving(Name))
			{
				Result.bEnabled = false;
				Result.Reason = FString::Printf(TEXT("self-driving switch -%s"), *Name);
				return Result;
			}
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
